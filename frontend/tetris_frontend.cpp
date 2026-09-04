// TetrisFrontend implementation: game loop over TetrisBackend + Renderer.
#include "tetris_frontend.h"

#include "renderer.h"

#include <bgfx/bgfx.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace tetris {

TetrisFrontend::TetrisFrontend() : m_renderer(new Renderer()) {}
TetrisFrontend::~TetrisFrontend() {}

void TetrisFrontend::pushKey(TetrisBackend::Key k) {
    std::lock_guard<std::mutex> lock(m_keysMutex);
    m_keys.push_back(k);
}

void TetrisFrontend::setWindowSize(uint32_t w, uint32_t h) {
    const uint32_t oldW = m_pxW.load();
    const uint32_t oldH = m_pxH.load();
    m_pxW.store(w);
    m_pxH.store(h);
    // Wake the game thread so it can run bgfx::reset() this frame, keeping the
    // canvas in sync with the window resize (no one-frame gap / stray line).
    if (w != oldW || h != oldH) {
        m_resizePending.store(true);
        m_frameCv.notify_all();
    }
}

void TetrisFrontend::repaintSynchronous(uint32_t w, uint32_t h) {
    // No-op if the buffer is already at this size (the UI loop calls this every
    // frame; only an actual resize does work).
    if (w == m_lastRenderedW.load() && h == m_lastRenderedH.load()) return;
    m_pxW.store(w);
    m_pxH.store(h);
    // Wake the game thread so it renders this size right now, then block the
    // caller (the UI thread, mid-resize) until that frame has been submitted.
    // This guarantees the GL buffer resize lands before the toolkit commits the
    // new window size, so both are presented in the same frame.
    // m_syncResize freezes the game (no gravity) for this repaint frame.
    m_syncResize.store(true);
    m_resizePending.store(true);
    m_frameCv.notify_all();
    std::unique_lock<std::mutex> lock(m_repaintMutex);
    m_repaintCv.wait(lock, [this, w, h] {
        return m_lastRenderedW.load() == w && m_lastRenderedH.load() == h;
    });
}

void TetrisFrontend::requestStop() { m_stop.store(true); }
bool TetrisFrontend::done() const { return m_done.load(); }
bool TetrisFrontend::loopDone() const { return m_loopDone.load(); }
void TetrisFrontend::uiShutdownRequested() {
    {
        std::lock_guard<std::mutex> lock(m_shutdownMutex);
        m_uiShutdown = true;
    }
    m_shutdownCv.notify_one();
}

void TetrisFrontend::run(const UiWindow* win) {
    if (!m_renderer->init(win)) {
        m_loopDone.store(true);
        m_done.store(true);
        return;
    }

    int smokeFrames = 0;
    if (const char* e = ::getenv("TETRIS_SMOKE_TEST")) smokeFrames = atoi(e);
    if (smokeFrames < 0) smokeFrames = 0;
    if (smokeFrames) fprintf(stderr, "[tetris] smoke test: %d frames\n", smokeFrames);

    int shotFrame = 0;
    if (const char* e = ::getenv("TETRIS_SCREENSHOT")) shotFrame = atoi(e);
    if (shotFrame <= 0 && smokeFrames > 4) shotFrame = smokeFrames - 3;  // default: near the end
    const char* shotPath = "tetris_shot.bmp";
    if (const char* e = ::getenv("TETRIS_SCREENSHOT_PATH")) shotPath = e;
    if (shotFrame > 0) fprintf(stderr, "[tetris] screenshot at frame %d -> %s\n", shotFrame, shotPath);

    using clock = std::chrono::steady_clock;
    auto now = clock::now();

    int frameNo = 0;
    // Stops on requestStop() (UI close button) or the backend's Quit key.
    while (!m_stop.load() && !m_backend.quitRequested()) {
        // Input: drain the key queue pushed by the UI thread.
        {
            std::lock_guard<std::mutex> lock(m_keysMutex);
            while (!m_keys.empty()) {
                m_backend.handleKey(m_keys.front());
                m_keys.pop_front();
            }
        }

        // Advance game logic by the real elapsed time (drives gravity). During a
        // synchronous resize repaint the game is frozen (the canvas is resizing).
        auto t = clock::now();
        double dt = std::chrono::duration<double>(t - now).count();
        now = t;
        const bool syncResize = m_syncResize.exchange(false);
        if (!syncResize) m_backend.update(dt);

        // Render.
        const uint32_t rw = m_pxW.load();
        const uint32_t rh = m_pxH.load();
        m_renderer->render(m_backend.state(), rw, rh);
        m_renderer->endFrame();
        // On a synchronous resize, wait until this frame is actually presented
        // (not merely submitted) before waking repaintSynchronous(): the UI
        // thread commits the new window size immediately after, so the content
        // buffer must already be up on the native surface (no white strip).
        if (syncResize) m_renderer->syncPresent();
        // Record the size that was just presented so repaintSynchronous() can
        // wait for it (guarantees the buffer resize is committed).
        m_lastRenderedW.store(rw);
        m_lastRenderedH.store(rh);
        m_repaintCv.notify_all();
        ++frameNo;

        if (shotFrame > 0 && frameNo == shotFrame)
            m_renderer->requestScreenShot(shotPath);

        if (smokeFrames > 0 && --smokeFrames == 0) m_stop.store(true);

        // Pace the loop to ~60 fps, but wake immediately on a resize so the
        // canvas keeps up with the window (see m_resizePending / m_frameCv).
        {
            std::unique_lock<std::mutex> lock(m_frameMutex);
            m_frameCv.wait_for(lock, std::chrono::milliseconds(16),
                               [this] { return m_resizePending.load(); });
            m_resizePending.store(false);
        }
    }

    m_backend.saveHighScore();
    m_loopDone.store(true);
    {
        std::unique_lock<std::mutex> lock(m_shutdownMutex);
        m_shutdownCv.wait(lock, [this] { return m_uiShutdown; });
    }
    // bgfx::shutdown() signals and joins its internal render thread; after
    // this returns no bgfx thread is left running.
    m_renderer->shutdown();
    m_done.store(true);
}

}  // namespace tetris
