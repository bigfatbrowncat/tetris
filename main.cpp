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

// Wayland resizes the content asynchronously (see main()): the content is a
// subsurface that the compositor keeps showing at its last committed size, and
// the window background matches the renderer's clear color, so the root can
// commit the new size immediately (no resize lag) while the content catches up
// within a frame with no visible strip. Other backends block until the frame
// is presented, keeping the canvas in the same frame as the window resize.
static bool s_asyncResize = false;

static void onFrameSync(uint32_t w, uint32_t h, void* user) {
    TetrisFrontend* fe = static_cast<TetrisFrontend*>(user);
    if (s_asyncResize) fe->setWindowSize(w, h);
    else fe->repaintSynchronous(w, h);
}

int main() {
    uiInit();
    const UiWindow* window = uiCreateWindow(640, 640, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    s_asyncResize = (window->nwhType == UI_NWH_WAYLAND);

    TetrisFrontend frontend;
    uiSetFrameSyncCallback(&onFrameSync, &frontend);
    std::thread gameThread(&TetrisFrontend::run, &frontend, window);

    // Pump UI events until the frontend game loop has stopped. The actual
    // rendering is driven by bgfx's own internal render thread — we must NOT
    // call bgfx::renderFrame() here (see the notes above).
    //
    // Per-frame ordering:
    //   1. uiPumpEvents  -> process events, update the allocation. On Wayland
    //                        the "layout" hook hands the new size to the game
    //                        thread (non-blocking).
    //   2. set/repaint   -> push the new size to the game thread.
    //                        Wayland: setWindowSize() (async — the root commits
    //                        the new size immediately; the subsurface catches up
    //                        within a frame and the gap shows the matching window
    //                        background). X11/macOS: repaintSynchronous() (blocks
    //                        until the frame is presented, so the canvas and the
    //                        window land in the same frame).
    //   3. uiCommitFrame -> commit the root surface (Wayland).
    while (!frontend.loopDone()) {
        uiPumpEvents(0.016);
        int k;
        while ((k = uiPopKey()) != KEY_NONE)
            frontend.pushKey(toBackendKey(k));
        uint32_t pw, ph;
        uiWindowSize(&pw, &ph);
        if (s_asyncResize) frontend.setWindowSize(pw, ph);
        else frontend.repaintSynchronous(pw, ph);
        uiCommitFrame();
        if (uiShouldQuit()) frontend.requestStop();
    }

    frontend.uiShutdownRequested();
    gameThread.join();
    uiShutdown();
    return 0;
}
