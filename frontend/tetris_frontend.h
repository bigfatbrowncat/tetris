// ============================================================================
//  TetrisFrontend — the bgfx-based frontend.
//  ---------------------------------------------------------------------------
//  Owns the game loop: drives a TetrisBackend and a Renderer. Two modes:
//
//  Threaded (macOS): run() runs the loop on a dedicated (API) thread; the UI
//  thread pushes logical keys and window size and polls done(). bgfx spawns
//  its own internal render thread (bgfx::renderFrame is never called before
//  bgfx::init).
//
//  Callback (Linux GTK GL area): the game runs on the UI thread, driven from
//  the GL area's render callback (paced by the compositor's frame clock).
//  init() prepares bgfx single-threaded (bgfx::renderFrame is latched before
//  bgfx::init, on the UI thread), frame() runs one update+render iteration
//  inside the callback, and shutdownNow() tears everything down. The two
//  communicate through atomics and a small mutex-guarded key queue.
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "tetris_backend.h"
#include "ui/window.h"

namespace tetris {

class Renderer;

class TetrisFrontend {
public:
    TetrisFrontend();
    ~TetrisFrontend();
    TetrisFrontend(const TetrisFrontend&) = delete;
    TetrisFrontend& operator=(const TetrisFrontend&) = delete;

    // --- threaded mode (macOS) ---------------------------------------------
    // Run the game loop on the calling thread.
    // win: native window data from the UI layer (handle + bgfx platform type).
    void run(const UiWindow* win);

    // --- callback mode (Linux GTK GL area) ---------------------------------
    // Initialize the renderer on the calling (UI) thread. Must be called
    // before frame(); bgfx is then driven single-threaded from that thread.
    bool init(const UiWindow* win);
    // One game iteration: update + render + endFrame. Called from the GL
    // area's render callback (the UI thread) with the new pixel size.
    void frame(uint32_t pixelW, uint32_t pixelH, double dt);
    // Tear down on the UI thread (after the last frame()).
    void shutdownNow();

    // --- thread-safe UI-side API --------------------------------------------
    void pushKey(TetrisBackend::Key k);
    // Update the target pixel size (non-blocking, threaded mode). If it
    // changed, the game thread is woken so it resets bgfx + renders the new
    // size on its next iteration.
    void setWindowSize(uint32_t pixelW, uint32_t pixelH);
    // Force the game thread to reset bgfx to (pixelW, pixelH) and submit one
    // frame, blocking the caller until that frame has actually been presented
    // (threaded mode: the canvas must land in the same frame as the window
    // resize).
    void repaintSynchronous(uint32_t pixelW, uint32_t pixelH);
    void requestStop();
    bool done() const;
    bool loopDone() const;
    void uiShutdownRequested();

private:
    // Shared by both modes: renderer init + env hooks (smoke test /
    // screenshot frame). Returns false when the renderer cannot start.
    bool start(const UiWindow* win);
    // One game iteration (input, update, render, endFrame, hooks).
    void doFrame(uint32_t pixelW, uint32_t pixelH, double dt);

    TetrisBackend m_backend;
    std::unique_ptr<Renderer> m_renderer;
    bool m_initDone = false;

    std::mutex m_keysMutex;
    std::deque<TetrisBackend::Key> m_keys;
    std::atomic<uint32_t> m_pxW{640}, m_pxH{640};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_loopDone{false};
    std::mutex m_shutdownMutex;
    std::condition_variable m_shutdownCv;
    bool m_uiShutdown{false};
    // Wakes the game thread immediately on a resize so bgfx::reset() runs in
    // the same frame as the window resize (avoids a visible gap while the old,
    // smaller buffer is still up). The game thread waits on this with a 16 ms
    // timeout, so normal frame pacing is unchanged.
    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    // Persistent "a resize is pending" flag used as the CV predicate. A plain
    // notify can be lost if it lands between the game thread's render and its
    // wait; the flag guarantees the new size is picked up on the very next frame.
    std::atomic<bool> m_resizePending{false};
    // Size last submitted by the game thread. repaintSynchronous() waits until
    // this matches the requested size, guaranteeing the buffer resize has been
    // committed before the caller (the UI render pass) proceeds.
    std::atomic<uint32_t> m_lastRenderedW{0}, m_lastRenderedH{0};
    std::mutex m_repaintMutex;
    std::condition_variable m_repaintCv;
    // Set by repaintSynchronous(); consumed by the game loop, which skips the
    // gravity update for that frame (freezes the game while the canvas resizes).
    std::atomic<bool> m_syncResize{false};

    // Frame hooks (TETRIS_SMOKE_TEST / TETRIS_SCREENSHOT), set in start().
    int m_smokeFrames = 0;
    int m_shotFrame = 0;
    const char* m_shotPath = "tetris_shot.bmp";
    int m_frameNo = 0;
};

}  // namespace tetris
