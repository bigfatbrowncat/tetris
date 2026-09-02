// ============================================================================
//  Tetris — entry point (platform-neutral wiring).
//  ---------------------------------------------------------------------------
//  Everything OS-specific lives in the UI module (macos/window.mm on macOS,
//  gtk/window.cpp on Linux): window creation, event pumping, key mapping.
//  Game logic is the backend (libtetrisback); the bgfx game loop is the
//  frontend (libtetrisfront). This file only wires UI events into the
//  frontend.
//
//  Threading:
//    - This (main/UI) thread pumps UI events only (never bgfx::renderFrame)
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
#include "ui/window.h"

#include <cstdio>
#include <thread>

using namespace tetris;

// Map the UI layer's logical keys to backend input events.
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
    uiInit();
    const UiWindow* window = uiCreateWindow(640, 640, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    TetrisFrontend frontend;
    std::thread gameThread(&TetrisFrontend::run, &frontend, window);

    // Pump UI events until the frontend game loop has stopped. The actual
    // rendering is driven by bgfx's own internal render thread — we must NOT
    // call bgfx::renderFrame() here (see the notes above).
    while (!frontend.loopDone()) {
        uiPumpEvents(0.016);
        int k;
        while ((k = uiPopKey()) != KEY_NONE)
            frontend.pushKey(toBackendKey(k));
        uint32_t pw, ph;
        uiWindowSize(&pw, &ph);
        frontend.setWindowSize(pw, ph);
        if (uiShouldQuit()) frontend.requestStop();
    }

    frontend.uiShutdownRequested();
    gameThread.join();
    uiShutdown();
    return 0;
}
