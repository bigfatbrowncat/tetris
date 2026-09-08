// ============================================================================
//  TetrisFrontend — the bgfx-based frontend.
//  ---------------------------------------------------------------------------
//  Owns the game loop: drives a TetrisBackend and a Renderer on a dedicated
//  (API) thread. The UI thread (ui/ + macos/ or gtk/) pushes logical keys and
//  window size and polls done(); the two communicate through lock-free-ish
//  atomics and a small mutex-guarded key queue.
//
//  Threading (bgfx contract):
//    - The thread that calls run() owns bgfx: init, per-frame
//      render()+endFrame() (bgfx::frame), shutdown.
//    - bgfx::renderFrame() must NEVER be called from the UI thread: because
//      it is not invoked before bgfx::init(), bgfx spawns its own internal
//      render thread that performs the GPU submit.
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

    // Run the game loop on the calling thread.
    // win: native window data from the UI layer (handle + bgfx platform type).
    void run(const UiWindow* win);

    // --- thread-safe UI-side API --------------------------------------------
    void pushKey(TetrisBackend::Key k);
    // Update the target pixel size (non-blocking). If it changed, the game
    // thread is woken so it resets bgfx + renders the new size on its next
    // iteration. This is the resize path on Wayland, where the content is a
    // subsurface that commits independently of the root (see main.cpp).
    void setWindowSize(uint32_t pixelW, uint32_t pixelH);
    // Force the game thread to reset bgfx to (pixelW, pixelH) and submit one
    // frame, blocking the caller until that frame has actually been presented.
    // Used by the backends where the canvas must land in the same frame as the
    // window resize (X11, macOS): the UI layer calls this DURING a window
    // resize so the GL canvas resizes in the same frame as the window — no
    // stray line while the old, smaller buffer is still on screen.
    void repaintSynchronous(uint32_t pixelW, uint32_t pixelH);
    void requestStop();
    bool done() const;
    bool loopDone() const;
    void uiShutdownRequested();

private:
    TetrisBackend m_backend;
    std::unique_ptr<Renderer> m_renderer;

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
};

}  // namespace tetris
