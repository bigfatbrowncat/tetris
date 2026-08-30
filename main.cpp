// ============================================================================
//  Tetris — a color terminal Tetris for Linux and macOS
//  ---------------------------------------------------------------------------
//  Pure C++17 + POSIX (termios/select). No external libraries.
//
//  Build:   c++ -std=c++17 -O2 -o tetris main.cpp
//  Run:     ./tetris
//
//  Controls:
//    Arrow keys / A D : move left / right
//    Arrow down / S   : soft drop
//    Arrow up / W / X : rotate clockwise
//    Z                : rotate counter-clockwise
//    Space            : hard drop
//    P                : pause
//    R                : restart
//    Q / Esc          : quit
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
// 1. Constants
// ----------------------------------------------------------------------------
static const int W = 10;   // board width  (cells)
static const int H = 20;   // board height (cells)
static const int EMPTY = -1;

// Piece types (index used everywhere): I O T S Z J L
enum Piece { I = 0, O = 1, T = 2, S = 3, Z = 4, J = 5, L = 6, NUM_PIECES = 7 };

// 256-color palette: bright color per piece (index = piece type)
static const int PIECE_COLOR[NUM_PIECES] = { 51, 220, 135, 46, 196, 21, 208 };
// Dimmer palette for the ghost (landing preview)
static const int DIM_COLOR[NUM_PIECES]   = { 24, 180, 96, 24, 88, 18, 130 };

// Shape of each piece for each of the 4 rotations.
// CELLS[pi][rot][cell] = {dx, dy}  (relative to the piece origin)
static const int CELLS[NUM_PIECES][4][4][2] = {
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

// ----------------------------------------------------------------------------
// 2. Terminal I/O  (raw mode + non-blocking key reading + ANSI color)
// ----------------------------------------------------------------------------
enum class Key { None, Left, Right, Down, Up, RotCW, RotCCW, HardDrop, Pause, Restart, Quit };

struct Terminal {
    termios saved{};
    bool raw = false;

    void enable() {
        tcgetattr(STDIN_FILENO, &saved);
        termios t = saved;
        t.c_lflag &= ~(ICANON | ECHO);   // no line buffering, no echo
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        raw = true;
        // Hide cursor and clear the screen once.
        std::cout << "\033[?25l\033[2J\033[H" << std::flush;
    }

    void disable() {
        if (raw) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::cout << "\033[?25h" << std::flush;  // show cursor again
        raw = false;
    }

    ~Terminal() { disable(); }

    // Wait up to `timeoutMs` for a character; return false on timeout.
    static bool waitChar(int timeoutMs, char& out) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return false;
        return ::read(STDIN_FILENO, &out, 1) == 1;
    }

    // Read one logical key (decoding arrow-key escape sequences).
    Key readKey(int timeoutMs) {
        char c;
        if (!waitChar(timeoutMs, c)) return Key::None;

        if (c == 27) {  // ESC: could be bare Esc or the start of an arrow key
            char c2;
            if (waitChar(50, c2) && c2 == '[') {
                char c3;
                if (waitChar(50, c3)) {
                    switch (c3) {
                        case 'D': return Key::Left;
                        case 'C': return Key::Right;
                        case 'A': return Key::Up;
                        case 'B': return Key::Down;
                        default:  break;
                    }
                }
            }
            return Key::Quit;  // bare ESC
        }

        switch (c) {
            case 'a': case 'A': return Key::Left;
            case 'd': case 'D': return Key::Right;
            case 's': case 'S': return Key::Down;
            case 'w': case 'W': return Key::Up;
            case 'x': case 'X': return Key::RotCW;
            case 'z': case 'Z': return Key::RotCCW;
            case ' ':           return Key::HardDrop;
            case 'p': case 'P': return Key::Pause;
            case 'r': case 'R': return Key::Restart;
            case 'q': case 'Q': return Key::Quit;
            default:            return Key::None;
        }
    }
};

// Build an ANSI-colored single character (256-color foreground/background).
static std::string cellStr(char ch, int fg, int bg) {
    if (fg < 0 && bg < 0) return std::string(1, ch);
    std::string s = "\033[";
    if (fg >= 0) s += "38;5;" + std::to_string(fg);
    if (fg >= 0 && bg >= 0) s += ";";
    if (bg >= 0) s += "48;5;" + std::to_string(bg);
    s += "m" + std::string(1, ch) + "\033[0m";
    return s;
}

// ----------------------------------------------------------------------------
// 3. Game state and logic
// ----------------------------------------------------------------------------
struct Game {
    std::vector<std::vector<int>> grid;     // grid[y][x] = EMPTY or piece type
    int curType = 0, curRot = 0, curX = 0, curY = 0;
    std::deque<int> queue;                  // upcoming pieces (bag)
    std::vector<int> bag;
    std::mt19937 rng;
    long score = 0, highScore = 0;
    int level = 0, lines = 0;
    bool gameOver = false, paused = false;
    bool wantGravityReset = false;

    Game() : grid(H, std::vector<int>(W, EMPTY)), rng(std::random_device{}()) {
        loadHigh();
        reset();
    }

    // ---- scoring persistence -------------------------------------------------
    void loadHigh() {
        std::ifstream f("tetris_high.txt");
        if (f) f >> highScore;
    }
    void saveHigh() {
        std::ofstream f("tetris_high.txt");
        if (f) f << highScore;
    }
    void bumpHigh() { if (score > highScore) highScore = score; }

    // ---- setup ---------------------------------------------------------------
    void reset() {
        grid.assign(H, std::vector<int>(W, EMPTY));
        bag.clear();
        queue.clear();
        refillQueue();
        score = 0; level = 0; lines = 0;
        gameOver = false; paused = false;
        wantGravityReset = false;
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
        curX = (curType == I) ? 3 : 4;      // center the spawn horizontally
        if (collides(curX, curY, curType, curRot)) gameOver = true;
    }

    // ---- collision -----------------------------------------------------------
    bool collides(int px, int py, int type, int rot) const {
        for (int i = 0; i < 4; i++) {
            int bx = px + CELLS[type][rot][i][0];
            int by = py + CELLS[type][rot][i][1];
            if (bx < 0 || bx >= W || by >= H) return true;
            if (by >= 0 && grid[by][bx] != EMPTY) return true;
        }
        return false;
    }

    // ---- movement / rotation -------------------------------------------------
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

    // ---- locking / line clears ----------------------------------------------
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

    // ---- gravity -------------------------------------------------------------
    int gravityInterval() const {  // ms per row, speeds up with level
        static const int table[] = {800,720,630,550,470,380,300,220,130,100,
                                    80, 70, 60, 50, 40, 30, 20, 15, 10, 10};
        return table[std::min(level, 19)];
    }

    void gravityStep() {
        if (gameOver || paused) return;
        if (!move(0, 1)) lockPiece();
    }

    // ---- input ---------------------------------------------------------------
    void handleGameKey(Key k) {
        switch (k) {
            case Key::Left:      move(-1, 0); break;
            case Key::Right:     move(1, 0);  break;
            case Key::Down:
                if (move(0, 1)) { score += 1; bumpHigh(); wantGravityReset = true; }
                break;
            case Key::Up:
            case Key::RotCW:     rotate(1);  break;
            case Key::RotCCW:    rotate(-1); break;
            case Key::HardDrop:  hardDrop(); wantGravityReset = true; break;
            default: break;
        }
    }

    // ---- rendering -----------------------------------------------------------
    // Screen layout (0-indexed columns/rows).
    static constexpr int PANEL_W   = 14;
    static constexpr int BOARD_L   = PANEL_W + 2;        // left border column
    static constexpr int SCREEN_W  = BOARD_L + W + 2;    // +1 left border + W + 1 right
    static constexpr int TITLE_ROW = 0;
    static constexpr int BOARD_TOP = 2;                  // top border row
    static constexpr int CELL_TOP  = 3;                  // first board cell row
    static constexpr int BOARD_BOT = CELL_TOP + H;       // bottom border row
    static constexpr int CTRL_ROW  = BOARD_BOT + 1;
    static constexpr int SCREEN_H  = CTRL_ROW + 1;

    std::vector<std::string> scr;
    std::vector<std::vector<int>> fg, bg;

    void setCell(int r, int c, char ch, int f = -1, int b = -1) {
        if (r < 0 || r >= SCREEN_H || c < 0 || c >= SCREEN_W) return;
        scr[r][c] = ch; fg[r][c] = f; bg[r][c] = b;
    }
    void setText(int r, int c, const std::string& s, int f = -1) {
        for (size_t i = 0; i < s.size(); i++) setCell(r, (int)c + (int)i, s[i], f);
    }

    void render() {
        scr.assign(SCREEN_H, std::string(SCREEN_W, ' '));
        fg.assign(SCREEN_H, std::vector<int>(SCREEN_W, -1));
        bg.assign(SCREEN_H, std::vector<int>(SCREEN_W, -1));

        // Title
        std::string title = "T E T R I S";
        setText(TITLE_ROW, (SCREEN_W - (int)title.size()) / 2, title, 208);

        // Board border
        int bc = 250;
        for (int c = 0; c <= W + 1; c++) {
            char e = (c == 0 || c == W + 1) ? '+' : '-';
            setCell(BOARD_TOP, BOARD_L + c, e, bc);
            setCell(BOARD_BOT, BOARD_L + c, e, bc);
        }
        for (int y = 0; y < H; y++) {
            int r = CELL_TOP + y;
            setCell(r, BOARD_L, '|', bc);
            setCell(r, BOARD_L + W + 1, '|', bc);
            for (int x = 0; x < W; x++) {
                int v = grid[y][x];
                int col = (v == EMPTY) ? 235 : PIECE_COLOR[v];
                setCell(r, BOARD_L + 1 + x, ' ', -1, col);
            }
        }

        // Ghost + current piece
        if (!gameOver) {
            int gy = ghostY();
            for (int i = 0; i < 4; i++) {
                int gx = curX + CELLS[curType][curRot][i][0];
                int gyy = gy + CELLS[curType][curRot][i][1];
                if (gyy >= 0 && gyy < H && gx >= 0 && gx < W && grid[gyy][gx] == EMPTY)
                    setCell(CELL_TOP + gyy, BOARD_L + 1 + gx, ' ', -1, DIM_COLOR[curType]);
            }
            for (int i = 0; i < 4; i++) {
                int px = curX + CELLS[curType][curRot][i][0];
                int py = curY + CELLS[curType][curRot][i][1];
                if (py >= 0 && py < H && px >= 0 && px < W)
                    setCell(CELL_TOP + py, BOARD_L + 1 + px, ' ', -1, PIECE_COLOR[curType]);
            }
        }

        // Info panel
        int lc = 249, vc = 231;
        setText(CELL_TOP, 1, "SCORE", lc);
        setText(CELL_TOP + 1, 1, std::to_string(score), vc);
        setText(CELL_TOP + 3, 1, "LEVEL", lc);
        setText(CELL_TOP + 4, 1, std::to_string(level), vc);
        setText(CELL_TOP + 5, 1, "LINES", lc);
        setText(CELL_TOP + 6, 1, std::to_string(lines), vc);
        setText(CELL_TOP + 8, 1, "NEXT", lc);
        drawNext(CELL_TOP + 9);
        setText(CELL_TOP + 14, 1, "HIGH", lc);
        setText(CELL_TOP + 15, 1, std::to_string(highScore), vc);

        // Controls
        setText(CTRL_ROW, 1, "arrows/wasd move  up/x rot  z ccw  space drop  p pause  q quit", 240);

        // Overlays
        if (gameOver) drawOverlay("GAME OVER", "r restart  -  q quit");
        else if (paused) drawOverlay("PAUSED", "press p to resume");

        emit();
    }

    void drawNext(int topRow) {
        refillQueue();
        int t = queue.front();
        for (int dy = 0; dy < 4; dy++)
            for (int dx = 0; dx < 4; dx++)
                setCell(topRow + dy, 3 + dx, ' ', -1, 235);
        for (int i = 0; i < 4; i++)
            setCell(topRow + CELLS[t][0][i][1], 3 + CELLS[t][0][i][0], ' ', -1, PIECE_COLOR[t]);
    }

    void drawOverlay(const std::string& headline, const std::string& sub) {
        int r1 = CELL_TOP + H / 2 - 2, r2 = CELL_TOP + H / 2 + 2;
        int c1 = BOARD_L + 2, c2 = BOARD_L + W - 1;
        for (int r = r1; r <= r2; r++)
            for (int c = c1; c <= c2; c++)
                setCell(r, c, ' ', -1, 233);
        setText((r1 + r2) / 2 - 1, (c1 + c2) / 2 - (int)headline.size() / 2, headline, 196);
        setText((r1 + r2) / 2 + 1, (c1 + c2) / 2 - (int)sub.size() / 2, sub, 249);
    }

    void emit() {
        std::string out;
        out.reserve(SCREEN_H * SCREEN_W * 24);
        for (int r = 0; r < SCREEN_H; r++) {
            out += "\033[" + std::to_string(r + 1) + ";1H";
            for (int c = 0; c < SCREEN_W; c++)
                out += cellStr(scr[r][c], fg[r][c], bg[r][c]);
        }
        std::cout << out << std::flush;
    }

    // ---- main loop -----------------------------------------------------------
    void run(Terminal& term) {
        using clock = std::chrono::steady_clock;
        auto nextGravity = clock::now() + std::chrono::milliseconds(gravityInterval());
        bool running = true;

        render();
        while (running) {
            auto now = clock::now();
            long waitMs = (paused || gameOver)
                ? 100  // poll for input while paused / on game over
                : std::max(0LL, std::chrono::duration_cast<std::chrono::milliseconds>(nextGravity - now).count());
            Key k = term.readKey((int)waitMs);

            if (k == Key::Quit) { running = false; break; }
            if (k == Key::Restart) { reset(); nextGravity = clock::now() + std::chrono::milliseconds(gravityInterval()); }
            else if (k == Key::Pause && !gameOver) {
                paused = !paused;
                nextGravity = clock::now() + std::chrono::milliseconds(gravityInterval());
            }
            else if (!gameOver && !paused) {
                handleGameKey(k);
                if (wantGravityReset) {
                    nextGravity = clock::now() + std::chrono::milliseconds(gravityInterval());
                    wantGravityReset = false;
                }
            }

            // Apply gravity for every interval that has elapsed.
            while (!gameOver && !paused && clock::now() >= nextGravity) {
                gravityStep();
                nextGravity += std::chrono::milliseconds(gravityInterval());
            }

            render();
        }
        saveHigh();
    }
};

// ----------------------------------------------------------------------------
// 4. Entry point
// ----------------------------------------------------------------------------
int main() {
    Terminal term;
    term.enable();

    Game game;
    game.run(term);

    term.disable();
    std::cout << "\nThanks for playing! High score: " << game.highScore << "\n";
    return 0;
}
