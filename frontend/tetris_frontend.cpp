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
        // m_loopDone: the game loop ended (quit) while we were waiting — a
        // resize can race the shutdown; bail instead of blocking forever.
        return (m_lastRenderedW.load() == w && m_lastRenderedH.load() == h) || m_loopDone.load();
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

bool TetrisFrontend::start(const UiWindow* win) {
    if (m_initDone) return true;
    if (!m_renderer->init(win)) return false;
    m_initDone = true;

    if (const char* e = ::getenv("TETRIS_SMOKE_TEST")) m_smokeFrames = atoi(e);
    if (m_smokeFrames < 0) m_smokeFrames = 0;
    if (m_smokeFrames) fprintf(stderr, "[tetris] smoke test: %d frames\n", m_smokeFrames);

    if (const char* e = ::getenv("TETRIS_SCREENSHOT")) m_shotFrame = atoi(e);
    if (m_shotFrame <= 0 && m_smokeFrames > 4) m_shotFrame = m_smokeFrames - 3;  // default: near the end
    m_shotPath = "tetris_shot.bmp";
    if (const char* e = ::getenv("TETRIS_SCREENSHOT_PATH")) m_shotPath = e;
    if (m_shotFrame > 0) fprintf(stderr, "[tetris] screenshot at frame %d -> %s\n", m_shotFrame, m_shotPath);

    return true;
}

void TetrisFrontend::doFrame(uint32_t pixelW, uint32_t pixelH, double dt) {
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
    const bool syncResize = m_syncResize.exchange(false);
    if (!syncResize) m_backend.update(dt);

    // Render.
    m_renderer->render(m_backend.state(), pixelW, pixelH);
    m_renderer->endFrame();
    // On a synchronous resize, wait until this frame is actually presented
    // (not merely submitted) before waking repaintSynchronous(): the UI
    // thread commits the new window size immediately after, so the content
    // buffer must already be up on the native surface (no white strip).
    if (syncResize) m_renderer->syncPresent();
    // Record the size that was just presented so repaintSynchronous() can
    // wait for it (guarantees the buffer resize is committed).
    m_lastRenderedW.store(pixelW);
    m_lastRenderedH.store(pixelH);
    m_repaintCv.notify_all();
    ++m_frameNo;

    if (m_frameNo <= m_shotFrame + 1 && m_shotFrame > 0)
        fprintf(stderr, "[dbg] frame %d shot %d\n", m_frameNo, m_shotFrame);
    if (m_shotFrame > 0 && m_frameNo == m_shotFrame)
        m_renderer->requestScreenShot(m_shotPath);

    if (m_smokeFrames > 0 && --m_smokeFrames == 0) m_stop.store(true);
}

void TetrisFrontend::run(const UiWindow* win) {
    if (!start(win)) {
        m_loopDone.store(true);
        m_done.store(true);
        return;
    }

    using clock = std::chrono::steady_clock;
    auto now = clock::now();

    // Stops on requestStop() (UI close button) or the backend's Quit key.
    while (!m_stop.load() && !m_backend.quitRequested()) {
        auto t = clock::now();
        const double dt = std::chrono::duration<double>(t - now).count();
        now = t;

        doFrame(m_pxW.load(), m_pxH.load(), dt);

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
    // Wake any UI thread parked in repaintSynchronous() (a resize racing the
    // shutdown); its predicate now sees m_loopDone.
    m_repaintCv.notify_all();
    {
        std::unique_lock<std::mutex> lock(m_shutdownMutex);
        m_shutdownCv.wait(lock, [this] { return m_uiShutdown; });
    }
    // bgfx::shutdown() signals and joins its internal render thread; after
    // this returns no bgfx thread is left running.
    m_renderer->shutdown();
    m_done.store(true);
}

// --- callback mode (Linux GTK GL area) --------------------------------------

bool TetrisFrontend::init(const UiWindow* win) {
    return start(win);
}

void TetrisFrontend::frame(uint32_t pixelW, uint32_t pixelH, double dt) {
    if (!m_initDone) return;
    if (m_stop.load() || m_backend.quitRequested()) {
        m_loopDone.store(true);
        return;
    }
    doFrame(pixelW, pixelH, dt);
    if (m_stop.load() || m_backend.quitRequested()) {
        m_loopDone.store(true);
    }
}

void TetrisFrontend::shutdownNow() {
    if (!m_initDone) {
        m_loopDone.store(true);
        m_done.store(true);
        return;
    }
    m_backend.saveHighScore();
    m_renderer->shutdown();
    m_initDone = false;
    m_loopDone.store(true);
    m_done.store(true);
}

}  // namespace tetris
