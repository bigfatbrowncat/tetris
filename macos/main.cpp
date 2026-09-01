// ============================================================================
//  Tetris — macOS entry point (Cocoa UI module).
//  ---------------------------------------------------------------------------
//  Everything OS-specific lives here (and in window.mm): window creation,
//  event pumping, key mapping. Game logic is the backend (libtetrisback);
//  the bgfx game loop is the frontend (libtetrisfront). This file only wires
//  Cocoa events into the frontend.
//
//  Threading:
//    - This (main/Cocoa) thread pumps Cocoa events only (never bgfx::renderFrame)
//    - The frontend thread: bgfx::init + game loop (bgfx::frame) + bgfx::shutdown
//    - bgfx owns its own internal render thread that drives bgfx::renderFrame
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

#include "tetris_frontend.h"
#include "window.h"

#include <cstdio>
#include <thread>

using namespace tetris;

// Map the Cocoa layer's logical keys to backend input events.
static TetrisBackend::Key toBackendKey(int k) {
    switch (k) {
        case KEY_LEFT:    return TetrisBackend::Key::Left;
        case KEY_RIGHT:   return TetrisBackend::Key::Right;
        case KEY_DOWN:    return TetrisBackend::Key::Down;
        case KEY_CW:      return TetrisBackend::Key::RotateCW;
        case KEY_CCW:     return TetrisBackend::Key::RotateCCW;
        case KEY_DROP:    return TetrisBackend::Key::HardDrop;
        case KEY_PAUSE:   return TetrisBackend::Key::Pause;
        case KEY_RESTART: return TetrisBackend::Key::Restart;
        case KEY_QUIT:    return TetrisBackend::Key::Quit;
        default:          return TetrisBackend::Key::None;
    }
}

int main() {
    cocoaInit();
    void* window = cocoaCreateWindow(640, 640, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    TetrisFrontend frontend;
    std::thread gameThread(&TetrisFrontend::run, &frontend, window);

    // Pump Cocoa events until the frontend thread has torn down bgfx.
    // The actual rendering is driven by bgfx's own internal render thread —
    // we must NOT call bgfx::renderFrame() here (see the notes above).
    while (!frontend.done()) {
        cocoaPumpEvents(0.016);
        int k;
        while ((k = cocoaPopKey()) != KEY_NONE)
            frontend.pushKey(toBackendKey(k));
        uint32_t pw, ph;
        cocoaWindowSize(&pw, &ph);
        frontend.setWindowSize(pw, ph);
        if (cocoaShouldQuit()) frontend.requestStop();
    }

    gameThread.join();
    cocoaShutdown();
    return 0;
}
