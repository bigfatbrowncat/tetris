// TetrisBackend implementation: game state and rules (no UI, no OS).
#include "tetris_backend.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <random>
#include <vector>

namespace tetris {

namespace {

constexpr int W = TetrisBackend::kCols;
constexpr int H = TetrisBackend::kRows;
constexpr int EMPTY = -1;

// ms per row, speeds up with level (level >= 19: the minimum).
constexpr int GRAVITY_INTERVAL_MS[20] = {
    800, 720, 630, 550, 470, 380, 300, 220, 130, 100,
    80, 70, 60, 50, 40, 30, 20, 15, 10, 10};

constexpr int gravityIntervalFor(int lvl) {
    return GRAVITY_INTERVAL_MS[std::min(lvl, 19)];
}

// The key repeater interval (DT) anchors, in ms: 110 at level 0 (the old
// DT at level 3), 51.2/1.05 ≈ 48.76 at level 10 and above (5% faster than
// the previous curve's level-10 value). Linear in between (see
// TetrisBackend::dtIntervalMs).
//
// RotateCW repeats at ROTATION_FREQ (75%) of the movement frequency — i.e.,
// once every DT/ROTATION_FREQ (see TetrisBackend::repeatIntervalMs).
constexpr double DT_LEVEL_0_MS  = gravityIntervalFor(3) / 5.0;  // 110 ms
constexpr double DT_LEVEL_10_MS = 51.2 / 1.05;                  // 48.76 ms
constexpr double ROTATION_FREQ  = 0.75;

enum Piece { I = 0, O = 1, T = 2, S = 3, Z = 4, J = 5, L = 6, NUM_PIECES = 7 };

// CELLS[pi][rot][cell] = {dx, dy} (relative to the piece origin, y grows down)
const int CELLS[NUM_PIECES][4][4][2] = {
    // I
    { { {0,1},{1,1},{2,1},{3,1} },
      { {2,0},{2,1},{2,2},{2,3} },
      { {0,2},{1,2},{2,2},{3,2} },
      { {1,0},{1,1},{1,2},{1,3} } },
    // O
    { { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} } },
    // T
    { { {1,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{2,1},{1,2} },
      { {0,1},{1,1},{2,1},{1,2} },
      { {1,0},{0,1},{1,1},{1,2} } },
    // S
    { { {1,0},{2,0},{0,1},{1,1} },
      { {1,0},{1,1},{2,1},{2,2} },
      { {1,1},{2,1},{0,2},{1,2} },
      { {0,0},{0,1},{1,1},{1,2} } },
    // Z
    { { {0,0},{1,0},{1,1},{2,1} },
      { {2,0},{1,1},{2,1},{1,2} },
      { {0,1},{1,1},{1,2},{2,2} },
      { {1,0},{0,1},{1,1},{0,2} } },
    // J
    { { {0,0},{0,1},{1,1},{2,1} },
      { {1,0},{2,0},{1,1},{1,2} },
      { {0,1},{1,1},{2,1},{2,2} },
      { {1,0},{1,1},{0,2},{1,2} } },
    // L
    { { {2,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{1,2},{2,2} },
      { {0,1},{1,1},{2,1},{0,2} },
      { {0,0},{1,0},{1,1},{1,2} } },
};

}  // namespace

class TetrisBackend::Impl {
public:
    Impl() : grid(H, std::vector<int>(W, EMPTY)), rng(std::random_device{}()) {
        loadHigh();
        reset();
    }

    // --- the game's own key repeater (the system's auto-repeat is not
    //     used) -----------------------------------------------------------
    // The repeatable keys: Left, Right, Down, RotateCW.
    enum { REP_LEFT = 0, REP_RIGHT, REP_DOWN, REP_CW, REP_NUM };
    struct Repeat {
        int refs = 0;        // physical keys mapping to this logical key
                             // (arrow + the WASD alias may be held at once)
        double accMs = 0.0;  // DT accumulator (ms); -DT right after a press
                             // (the one-DT pause before the first repeat)
    };
    Repeat m_rep[REP_NUM];
    // Held repeatable keys, oldest -> newest; the back (the most recently
    // pressed) is the active one. A key appears at most once.
    std::deque<TetrisBackend::Key> m_held;

    static int repIndex(TetrisBackend::Key k) {
        switch (k) {
            case TetrisBackend::Key::Left:     return REP_LEFT;
            case TetrisBackend::Key::Right:    return REP_RIGHT;
            case TetrisBackend::Key::Down:     return REP_DOWN;
            case TetrisBackend::Key::RotateCW: return REP_CW;
            default:                           return -1;
        }
    }

    // One input action (a press or a repeat step).
    void performAction(TetrisBackend::Key k) {
        switch (k) {
            case TetrisBackend::Key::Left:  move(-1, 0); break;
            case TetrisBackend::Key::Right: move(1, 0);  break;
            case TetrisBackend::Key::Down:
                if (move(0, 1)) { score += 1; bumpHigh(); wantGravityReset = true; }
                break;
            case TetrisBackend::Key::RotateCW:  rotate(1);  break;
            case TetrisBackend::Key::RotateCCW: rotate(-1); break;
            case TetrisBackend::Key::HardDrop:  hardDrop(); wantGravityReset = true; break;
            default: break;
        }
        if (wantGravityReset) {
            gravityAccMs = 0.0;
            wantGravityReset = false;
        }
    }

    void keyDown(TetrisBackend::Key k) {
        switch (k) {
            case TetrisBackend::Key::Quit:    quit = true; return;
            case TetrisBackend::Key::Restart: reset(); return;
            case TetrisBackend::Key::Pause:
                if (!gameOver) {
                    paused = !paused;
                    gravityAccMs = 0.0;
                }
                return;
            default: break;
        }
        if (gameOver) return;
        const int i = repIndex(k);
        if (i < 0) {  // RotateCCW / HardDrop: act once on the press
            if (paused) return;
            performAction(k);
            return;
        }
        Repeat& r = m_rep[i];
        if (r.refs > 0) {  // an alias of a key that is already held
                            // (arrow + WASD): count it, act on the release
                            // of the last one
            r.refs++;
            return;
        }
        r.refs = 1;
        m_held.push_back(k);
        if (!paused) {
            performAction(k);                              // the press moves once, immediately
            r.accMs = -TetrisBackend::repeatIntervalMs(k, level); // one interval of pause before the first repeat
        }
    }

    void keyUp(TetrisBackend::Key k) {
        const int i = repIndex(k);
        if (i < 0) return;
        Repeat& r = m_rep[i];
        if (r.refs <= 0) return;
        if (--r.refs > 0) return;  // another alias of the key is still held
        m_held.erase(std::remove(m_held.begin(), m_held.end(), k), m_held.end());
    }

    void update(double dtSeconds) {
        if (gameOver || paused) return;
        // A sub-millisecond epsilon so floating-point accumulation of dt
        // cannot push a step a frame late (the timers below compare
        // accumulated doubles against exact ms values).
        constexpr double EPS_MS = 1e-3;
        gravityAccMs += dtSeconds * 1000.0;
        while (!gameOver && !paused) {
            const double interval = gravityInterval();
            if (gravityAccMs < interval - EPS_MS) break;
            gravityAccMs -= interval;
            gravityStep();
        }
        // The held key pressed most recently moves the piece: one step per
        // repeat interval (see TetrisBackend::repeatIntervalMs) while it is
        // held. Only the active (last pressed) key's timer runs; the others
        // are frozen until they are the most recent again.
        if (!m_held.empty()) {
            const TetrisBackend::Key active = m_held.back();
            Repeat& r = m_rep[repIndex(active)];
            r.accMs += dtSeconds * 1000.0;
            const double repMs = TetrisBackend::repeatIntervalMs(active, level);
            while (r.accMs >= repMs - EPS_MS) {
                r.accMs -= repMs;
                performAction(active);
            }
        }
    }

    TetrisBackend::Snapshot state() const {
        TetrisBackend::Snapshot s;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                s.grid[y][x] = grid[y][x];
        s.pieceColor = gameOver ? -1 : curType;
        int gy = gameOver ? curY : ghostY();
        for (int i = 0; i < 4; i++) {
            const int* c = CELLS[curType][curRot][i];
            s.piece[i] = {curX + c[0], curY + c[1], curType};
            s.ghost[i] = {curX + c[0], gy + c[1], curType};
        }
        s.nextColor = nextType();
        for (int i = 0; i < 4; i++) {
            const int* c = CELLS[s.nextColor][0][i];
            s.next[i] = {c[0], c[1], s.nextColor};
        }
        s.score = score;
        s.highScore = highScore;
        s.level = level;
        s.lines = lines;
        s.paused = paused;
        s.gameOver = gameOver;
        return s;
    }

    bool quitRequested() const { return quit; }

    void saveHigh() const {
        std::ofstream f("tetris_high.txt");
        if (f) f << highScore;
    }

private:
    std::vector<std::vector<int>> grid;  // grid[y][x] = EMPTY or piece type
    int curType = 0, curRot = 0, curX = 0, curY = 0;
    std::deque<int> queue;               // upcoming pieces (bag)
    std::vector<int> bag;
    std::mt19937 rng;
    long score = 0, highScore = 0;
    int level = 0, lines = 0;
    bool gameOver = false, paused = false;
    bool quit = false;
    bool wantGravityReset = false;
    double gravityAccMs = 0.0;

    void loadHigh() { std::ifstream f("tetris_high.txt"); if (f) f >> highScore; }
    void bumpHigh() { if (score > highScore) highScore = score; }

    void reset() {
        grid.assign(H, std::vector<int>(W, EMPTY));
        bag.clear(); queue.clear(); refillQueue();
        score = 0; level = 0; lines = 0;
        gameOver = false; paused = false; wantGravityReset = false;
        gravityAccMs = 0.0;
        for (auto& r : m_rep) { r.refs = 0; r.accMs = 0.0; }
        m_held.clear();
        spawn();
    }

    void refillQueue() {
        while ((int)queue.size() < 5) {
            if (bag.empty()) {
                bag = {0, 1, 2, 3, 4, 5, 6};
                std::shuffle(bag.begin(), bag.end(), rng);
            }
            queue.push_back(bag.back());
            bag.pop_back();
        }
    }

    void spawn() {
        refillQueue();
        curType = queue.front(); queue.pop_front();
        curRot = 0;
        curY = 0;
        curX = (curType == I) ? 3 : 4;
        if (collides(curX, curY, curType, curRot)) gameOver = true;
    }

    bool collides(int px, int py, int type, int rot) const {
        for (int i = 0; i < 4; i++) {
            int bx = px + CELLS[type][rot][i][0];
            int by = py + CELLS[type][rot][i][1];
            if (bx < 0 || bx >= W || by >= H) return true;
            if (by >= 0 && grid[by][bx] != EMPTY) return true;
        }
        return false;
    }

    bool move(int dx, int dy) {
        if (collides(curX + dx, curY + dy, curType, curRot)) return false;
        curX += dx; curY += dy;
        return true;
    }

    bool rotate(int dir) {
        int nr = (curRot + dir + 4) % 4;
        static const int kicks[] = {0, -1, 1, -2, 2};
        for (int k : kicks) {
            if (!collides(curX + k, curY, curType, nr)) {
                curX += k; curRot = nr;
                return true;
            }
        }
        return false;
    }

    int ghostY() const {
        int gy = curY;
        while (!collides(curX, gy + 1, curType, curRot)) gy++;
        return gy;
    }

    void hardDrop() {
        int dist = 0;
        while (move(0, 1)) dist++;
        score += dist * 2;
        bumpHigh();
        lockPiece();
    }

    void lockPiece() {
        bool topOut = false;
        for (int i = 0; i < 4; i++) {
            int bx = curX + CELLS[curType][curRot][i][0];
            int by = curY + CELLS[curType][curRot][i][1];
            if (by < 0) { topOut = true; continue; }
            if (by >= 0 && by < H && bx >= 0 && bx < W) grid[by][bx] = curType;
        }
        if (topOut) { gameOver = true; bumpHigh(); return; }

        int cleared = clearLines();
        if (cleared > 0) {
            static const int pts[5] = {0, 100, 300, 500, 800};
            score += pts[cleared] * (level + 1);
            lines += cleared;
            level = lines / 10;
            bumpHigh();
        }
        spawn();
    }

    int clearLines() {
        std::vector<std::vector<int>> kept;
        int cleared = 0;
        for (int y = 0; y < H; y++) {
            bool full = true;
            for (int x = 0; x < W; x++) if (grid[y][x] == EMPTY) { full = false; break; }
            if (full) cleared++;
            else kept.push_back(grid[y]);
        }
        while ((int)kept.size() < H) kept.insert(kept.begin(), std::vector<int>(W, EMPTY));
        grid = kept;
        return cleared;
    }

    int gravityInterval() const {  // ms per row, speeds up with level
        return gravityIntervalFor(level);
    }

    void gravityStep() {
        if (gameOver || paused) return;
        if (!move(0, 1)) lockPiece();
    }

    int nextType() const { return queue.empty() ? 0 : queue.front(); }
};

TetrisBackend::TetrisBackend() : m_impl(new Impl()) {}
TetrisBackend::~TetrisBackend() { delete m_impl; }

void TetrisBackend::keyDown(Key k) { m_impl->keyDown(k); }
void TetrisBackend::keyUp(Key k) { m_impl->keyUp(k); }

double TetrisBackend::dtIntervalMs(int level) {
    // Linear between the two anchors (DT_LEVEL_0_MS / DT_LEVEL_10_MS);
    // clamped above level 10.
    return DT_LEVEL_0_MS + (DT_LEVEL_10_MS - DT_LEVEL_0_MS)
           * std::clamp(level, 0, 10) / 10.0;
}

double TetrisBackend::repeatIntervalMs(Key k, int level) {
    // Rotation repeats at ROTATION_FREQ (75%) of the movement frequency.
    const double dt = dtIntervalMs(level);
    return k == Key::RotateCW ? dt / ROTATION_FREQ : dt;
}

void TetrisBackend::update(double dtSeconds) { m_impl->update(dtSeconds); }
TetrisBackend::Snapshot TetrisBackend::state() const { return m_impl->state(); }
bool TetrisBackend::quitRequested() const { return m_impl->quitRequested(); }
void TetrisBackend::saveHighScore() const { m_impl->saveHigh(); }

}  // namespace tetris
