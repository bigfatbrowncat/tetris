// ============================================================================
//  Tetris — entry point (platform-neutral wiring).
//  ---------------------------------------------------------------------------
//  Everything OS-specific lives in the UI module (macos/window.mm on macOS,
//  gtk/window.cpp on Linux): window creation, event pumping, key mapping.
//  Game logic is the backend (libtetrisback); the bgfx game loop is the
//  frontend (libtetrisfront). This file only wires UI events into the
//  frontend.
//
//  Two frontend modes:
//    - Threaded (macOS): a dedicated game thread owns bgfx (init/frame/
//      shutdown); the UI thread pumps events and must never call
//      bgfx::renderFrame(). The renderer presents to the window directly.
//    - Callback (Linux, both X11 and Wayland): the UI layer is a GtkGLArea
//      (the glsync design). The game runs on the UI thread inside the GL
//      area's render callback — once per displayed frame, paced by the
//      compositor's frame clock. bgfx (the OpenGL/EGL backend, single-
//      threaded) renders the scene into a shared GL texture; the UI layer
//      presents it as a full-frame quad. No CPU copy anywhere.
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

static void onFrameSync(uint32_t w, uint32_t h, void* user) {
    TetrisFrontend* fe = static_cast<TetrisFrontend*>(user);
    fe->repaintSynchronous(w, h);
}

// The GL area's render callback driver (Linux): one game iteration with the
// new pixel size; the scene lands in the shared texture, which the UI layer
// presents as a full-frame quad right after this returns.
static void onFrameDriver(uint32_t w, uint32_t h, double dt, void* user) {
    TetrisFrontend* fe = static_cast<TetrisFrontend*>(user);
    fe->frame(w, h, dt);
}

int main() {
    uiInit();
    // TETRIS_WINDOW_SIZE="w h" (test hook, like TETRIS_SMOKE_TEST): the outer
    // window size in device pixels; the default is 640x640.
    uint32_t winW = 640, winH = 640;
    if (const char* e = ::getenv("TETRIS_WINDOW_SIZE")) {
        if (sscanf(e, "%u %u", &winW, &winH) == 2 && winW >= 32 && winH >= 32) {
            fprintf(stderr, "[tetris] window size override: %ux%u\n", winW, winH);
        } else {
            fprintf(stderr, "[tetris] bad TETRIS_WINDOW_SIZE '%s' (want 'w h'); using 640x640\n", e);
        }
    }
    const UiWindow* window = uiCreateWindow(winW, winH, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    TetrisFrontend frontend;
    uiSetFrameSyncCallback(&onFrameSync, &frontend);

    if (window->offscreen) {
        // Linux GL area path: the game runs on this (UI) thread, driven from
        // the GL area's render callback. bgfx is single-threaded and must be
        // initialized on this thread before the first callback renders.
        if (!frontend.init(window)) {
            fprintf(stderr, "[tetris] renderer init failed\n");
            uiShutdown();
            return 1;
        }
        uiSetFrameDriver(&onFrameDriver, &frontend);
        while (!frontend.loopDone()) {
            uiPumpEvents(0.016);
            int k;
            while ((k = uiPopKey()) != KEY_NONE)
                frontend.pushKey(toBackendKey(k));
            if (uiShouldQuit()) frontend.requestStop();
        }
        frontend.shutdownNow();
    } else {
        // macOS: a dedicated game thread owns bgfx; the UI thread pumps
        // events and resizes the canvas synchronously (canvas and window land
        // in the same frame).
        std::thread gameThread(&TetrisFrontend::run, &frontend, window);
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
    }

    uiShutdown();
    return 0;
}
