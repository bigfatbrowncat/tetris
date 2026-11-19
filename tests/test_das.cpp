// Standalone verification of the game's own key repeater (DAS) in
// TetrisBackend. The backend is pure C++ (no UI/OS), so the test links
// tetrisback directly (CMake target `test_das`):
//   cmake --build <build-dir> --target test_das
//   ./<build-dir>/test_das          # exits non-zero if any check fails
//
// DT (the movement repeat interval) is a function of the level: 110 ms at
// level 0, 51.2/1.05 ≈ 48.76 ms at level 10 and above, linear in between.
// RotateCW repeats at 75% of the movement frequency (every DT/0.75). These
// tests run at level 0, so DT = 110 ms.
//
// Expected timeline for a held repeatable key:
//   t=0     press      -> immediate move
//   t=DT    (110)      -> skipped (no move)
//   t=2*DT  (220)      -> move
//   t=3*DT  (330)      -> move
//   ...       one move per DT while held.
//
// Multiple held keys: the most recently pressed one moves the piece;
// releasing it hands the movement back to the previously pressed one
// (whose timer was frozen while it was not the most recent).

#include "tetris_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using tetris::TetrisBackend;
using K = TetrisBackend::Key;

static int g_fail = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_fail;                                             \
        }                                                         \
    } while (0)

// Anchor of the active piece: the minimum x of its cells (board coords).
static int anchorX(const TetrisBackend::Snapshot& s) {
    int m = 100;
    for (int i = 0; i < 4; i++) m = std::min(m, s.piece[i].x);
    return m;
}

struct Sim {
    TetrisBackend be;
    double t = 0.0;  // ms

    // Advance the game by one frame of the given duration (ms).
    void step(double dtMs) {
        be.update(dtMs / 1000.0);
        t += dtMs;
    }
    // Advance to exactly tms using 16 ms frames.
    void runTo(double tms) {
        while (t < tms - 1e-9) step(std::min(16.0, tms - t));
    }
    int x() const { return anchorX(be.state()); }
    long score() const { return be.state().score; }
};

// A: basic DAS timeline (press -> immediate -> 1 DT skip -> per DT).
static void testBasicDas(Sim& s) {
    const int a0 = s.x();
    s.be.keyDown(K::Left);                    // t=0
    CHECK(s.x() == a0 - 1, "A1: immediate move on press");
    s.runTo(110);                             // t=DT
    CHECK(s.x() == a0 - 1, "A2: no move within the skipped DT");
    s.runTo(220);                             // t=2*DT
    CHECK(s.x() == a0 - 2, "A3: first repeat at 2*DT");
    s.runTo(330);                             // t=3*DT
    CHECK(s.x() == a0 - 3, "A4: repeats at every DT (2 more moves)");
    s.be.keyUp(K::Left);                      // t=330
    s.runTo(550);
    CHECK(s.x() == a0 - 3, "A5: no moves after release");
}

// B: most recently pressed wins; the older key resumes on release.
static void testMostRecentWins(Sim& s) {
    const int a0 = s.x();
    s.be.keyDown(K::Left);                    // t=0:  x = a0-1, left acc = -110
    CHECK(s.x() == a0 - 1, "B1: left immediate");
    s.runTo(100);                             // left acc = -110+100 = -10 (frozen after)
    s.be.keyDown(K::Right);                   // t=100: immediate, x = a0
    CHECK(s.x() == a0, "B2: right press moves right immediately (wins)");
    s.runTo(320);                             // right's first repeat (100 + 2*DT)
    CHECK(s.x() == a0 + 1, "B3: right repeats at its own 2*DT");
    s.runTo(400);
    s.be.keyUp(K::Right);                     // left is active again, acc = -10
    s.runTo(500);
    CHECK(s.x() == a0 + 1, "B4: left does not move before its 110 ms are up");
    s.runTo(520);                             // -10 + (520-400) = 110 -> move
    CHECK(s.x() == a0, "B5: left resumes (no immediate jump, then per DT)");
    s.runTo(630);
    CHECK(s.x() == a0 - 1, "B6: left keeps repeating per DT");
}

// C: soft drop keeps scoring +1 per successful step (immediate + repeats).
static void testSoftDropScore(Sim& s) {
    const long s0 = s.score();
    s.be.keyDown(K::Down);                    // t=0: +1
    CHECK(s.score() == s0 + 1, "C1: immediate soft drop scores");
    s.runTo(220);                             // first repeat at 2*DT
    CHECK(s.score() == s0 + 2, "C2: the repeat scores");
    s.runTo(440);                             // repeats at 3*DT and 4*DT
    CHECK(s.score() == s0 + 4, "C3: ...and keep scoring per DT");
    s.be.keyUp(K::Down);
}

// D: aliasing (arrow + WASD) — refcount per logical key.
static void testAlias(Sim& s) {
    const int a0 = s.x();
    s.be.keyDown(K::Left);                    // "arrow left"
    CHECK(s.x() == a0 - 1, "D1: first alias moves");
    s.be.keyDown(K::Left);                    // "a" while the arrow is held
    CHECK(s.x() == a0 - 1, "D2: alias press does not re-trigger the move");
    s.be.keyUp(K::Left);                      // release "a": still held
    s.runTo(220);
    CHECK(s.x() == a0 - 2, "D3: repeat continues while one alias is held");
    s.be.keyUp(K::Left);                      // release the arrow: now free
    s.runTo(440);
    CHECK(s.x() == a0 - 2, "D4: no moves after the last alias is released");
}

// E: non-repeatable keys act once on the press (they are not in the
// repeat set, so a hold cannot fire them again — and the platform no
// longer forwards system repeats anyway).
static void testSingleShot(Sim& s) {
    const long sc0 = s.score();
    s.be.keyDown(K::HardDrop);                // locks the piece
    CHECK(s.score() > sc0, "E1: hard drop locks the piece and scores");
    const long sc1 = s.score();
    s.be.keyDown(K::HardDrop);                // a second press
    CHECK(s.score() > sc1, "E2: the second press drops the new piece once");
}

// F: the repeater timer is frozen while paused.
static void testPauseFreeze(Sim& s) {
    const int a0 = s.x();
    s.be.keyDown(K::Left);                    // x = a0-1, acc = -110
    CHECK(s.x() == a0 - 1, "F1: immediate move");
    s.runTo(80);
    s.be.keyDown(K::Pause);                   // t=80: acc = -30 (frozen)
    s.runTo(2000);
    CHECK(s.x() == a0 - 1, "F2: no repeats while paused");
    s.be.keyDown(K::Pause);                   // t=2000: resume, acc = -30
    s.runTo(2080);                            // -30 + 80 = 50: still in the pause
    CHECK(s.x() == a0 - 1, "F3: still counting the frozen remainder");
    s.runTo(2140);                            // -30 + 140 = 110 = 1 DT: move
    CHECK(s.x() == a0 - 2, "F4: left repeats again after resume");
}

// G: the DT formula (TetrisBackend::dtIntervalMs): 110 ms at level 0,
// 51.2/1.05 ≈ 48.76 ms at level 10 and above (5% faster than the
// previous level-10 value), linearly interpolated between, clamped above
// level 10.
static void testDtCurve() {
    const double dt10 = 51.2 / 1.05;
    CHECK(std::fabs(TetrisBackend::dtIntervalMs(0) - 110.0) < 1e-9,
          "G1: DT(0) == 110 ms");
    CHECK(std::fabs(TetrisBackend::dtIntervalMs(10) - dt10) < 1e-9,
          "G2: DT(10) == 51.2/1.05 (5% faster than before)");
    CHECK(std::fabs(TetrisBackend::dtIntervalMs(15) - dt10) < 1e-9,
          "G3: DT clamped at level 10");
    for (int l = 0; l <= 10; l++) {  // linear between the two anchors
        const double expected = 110.0 + (dt10 - 110.0) * l / 10.0;
        CHECK(std::fabs(TetrisBackend::dtIntervalMs(l) - expected) < 1e-9,
              "G4: DT(l) is linear for 0 <= l <= 10");
    }
}

// H: the per-key repeat interval (TetrisBackend::repeatIntervalMs):
// movement keys (Left, Right, Down) repeat every DT; RotateCW at 75% of
// the movement frequency, i.e. every DT/0.75 (a longer interval).
static void testRotationInterval() {
    for (int l : {0, 3, 7, 10, 15}) {
        const double dt = TetrisBackend::dtIntervalMs(l);
        CHECK(std::fabs(TetrisBackend::repeatIntervalMs(K::RotateCW, l)
                        - dt / 0.75) < 1e-9,
              "H1: RotateCW interval == DT/0.75 (75% of the movement freq)");
        CHECK(std::fabs(TetrisBackend::repeatIntervalMs(K::Left, l) - dt) < 1e-9,
              "H2: Left interval == DT");
        CHECK(std::fabs(TetrisBackend::repeatIntervalMs(K::Right, l) - dt) < 1e-9,
              "H3: Right interval == DT");
        CHECK(std::fabs(TetrisBackend::repeatIntervalMs(K::Down, l) - dt) < 1e-9,
              "H4: Down interval == DT");
    }
}

int main() {
    Sim sA; testBasicDas(sA);
    Sim sB; testMostRecentWins(sB);
    Sim sC; testSoftDropScore(sC);
    Sim sD; testAlias(sD);
    Sim sE; testSingleShot(sE);
    Sim sF; testPauseFreeze(sF);
    testDtCurve();
    testRotationInterval();
    if (g_fail == 0) std::printf("ALL DAS TESTS PASSED\n");
    else std::printf("%d CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
