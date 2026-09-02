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
    void setWindowSize(uint32_t pixelW, uint32_t pixelH);
    void requestStop();
    bool done() const;

private:
    TetrisBackend m_backend;
    std::unique_ptr<Renderer> m_renderer;

    std::mutex m_keysMutex;
    std::deque<TetrisBackend::Key> m_keys;
    std::atomic<uint32_t> m_pxW{640}, m_pxH{640};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_done{false};
};

}  // namespace tetris
