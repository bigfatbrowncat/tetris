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

// Called from the UI layer's surface "layout" signal (frame clock LAYOUT phase)
// on Wayland, BEFORE GTK commits the root surface (PAINT phase). Resize +
// present the content synchronously so it lands in the same frame as the window
// resize (no white strip). No-op if the size is unchanged.
static void onFrameSync(uint32_t w, uint32_t h, void* user) {
    static_cast<TetrisFrontend*>(user)->repaintSynchronous(w, h);
}

int main() {
    uiInit();
    const UiWindow* window = uiCreateWindow(640, 640, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    TetrisFrontend frontend;
    uiSetFrameSyncCallback(&onFrameSync, &frontend);
    std::thread gameThread(&TetrisFrontend::run, &frontend, window);

    // Pump UI events until the frontend game loop has stopped. The actual
    // rendering is driven by bgfx's own internal render thread — we must NOT
    // call bgfx::renderFrame() here (see the notes above).
    //
    // Per-frame ordering (matters on Wayland, where the GL canvas is a
    // subsurface that must not lag the committed window size):
    //   1. uiPumpEvents  -> process events, update the allocation (NO commit)
    //   2. repaintSync   -> resize the GL canvas to the new size, blocking until
    //                        that frame is submitted (no-op if the size is unchanged)
    //   3. uiCommitFrame -> commit the root surface (present the new window size)
    // This keeps the canvas and the window in lockstep (no stray line on resize).
    while (!frontend.loopDone()) {
        uiPumpEvents(0.016);
        int k;
        while ((k = uiPopKey()) != KEY_NONE)
            frontend.pushKey(toBackendKey(k));
        uint32_t pw, ph;
        uiWindowSize(&pw, &ph);
        frontend.repaintSynchronous(pw, ph);
        uiCommitFrame();
        if (uiShouldQuit()) frontend.requestStop();
    }

    frontend.uiShutdownRequested();
    gameThread.join();
    uiShutdown();
    return 0;
}
