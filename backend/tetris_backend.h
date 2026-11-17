// ============================================================================
//  TetrisBackend — game logic API (the frontend/backend interface).
//  ---------------------------------------------------------------------------
//  Pure game state and rules: board, pieces, scoring, gravity, high score,
//  and the game's own key repeater (the system's key auto-repeat is not
//  used: the UI layer reports key down/up, and the game decides when a
//  held key repeats).
//  No bgfx, no windowing, no OS-specific code. The frontend drives it:
//  keyDown/keyUp + update(dt) each frame, and reads state() to render.
// ============================================================================
#pragma once

#include <cstddef>

namespace tetris {

class TetrisBackend {
public:
    static constexpr int kCols = 10;  // board width  (cells)
    static constexpr int kRows = 20;  // board height (cells)
    static constexpr int kNumPieces = 7;

    // Logical input events (the UI layer maps raw keys to these).
    enum class Key : int {
        None = 0,
        Left, Right, Down,
        RotateCW, RotateCCW, HardDrop,
        Pause, Restart, Quit,
    };

    // A board cell: x grows right, y grows down from the top row.
    // color = piece id 0..6.
    struct Cell {
        int x, y;
        int color;
    };

    // Immutable per-frame view of the game state.
    struct Snapshot {
        int grid[kRows][kCols];  // -1 empty, else piece id
        Cell piece[4];           // active piece (board coordinates)
        Cell ghost[4];           // landing position (board coordinates)
        Cell next[4];            // next piece, rotation 0 (local 4x4 coordinates)
        int  pieceColor = -1;    // 0..6, or -1 when the game is over
        int  nextColor = 0;
        long score = 0, highScore = 0;
        int  level = 0, lines = 0;
        bool paused = false, gameOver = false;
    };

    // Loads "tetris_high.txt" (if present) and starts a fresh game.
    TetrisBackend();
    ~TetrisBackend();
    TetrisBackend(const TetrisBackend&) = delete;
    TetrisBackend& operator=(const TetrisBackend&) = delete;

    // Key state changes from the UI layer.
    //
    // Repeatable keys (Left, Right, Down, RotateCW): a press moves the
    // piece once immediately; then, unless the key is released, it repeats
    // every DT = gravityInterval()/5 (one fifth of the falling tick — it
    // speeds up with the level) after a pause of one DT. When several
    // repeatable keys are held at once, the most recently pressed one
    // moves the piece; releasing it hands the movement back to the
    // previously pressed one.
    //
    // The other keys (RotateCCW, HardDrop, Pause, Restart, Quit) act once,
    // on the press.
    void keyDown(Key k);
    void keyUp(Key k);

    // Advance the game by dt seconds (drives piece gravity).
    void update(double dtSeconds);

    // Current state (copied — safe to keep between frames).
    Snapshot state() const;

    bool quitRequested() const;

    // Persist the high score to "tetris_high.txt".
    void saveHighScore() const;

private:
    class Impl;
    Impl* m_impl;
};

}  // namespace tetris
