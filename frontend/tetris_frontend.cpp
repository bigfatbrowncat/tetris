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
    m_pxW.store(w);
    m_pxH.store(h);
}

void TetrisFrontend::requestStop() { m_stop.store(true); }
bool TetrisFrontend::done() const { return m_done.load(); }

void TetrisFrontend::run(const void* nwh) {
    if (!m_renderer->init(nwh)) {
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

        // Advance game logic by the real elapsed time (drives gravity).
        auto t = clock::now();
        double dt = std::chrono::duration<double>(t - now).count();
        now = t;
        m_backend.update(dt);

        // Render.
        m_renderer->render(m_backend.state(), m_pxW.load(), m_pxH.load());
        m_renderer->endFrame();
        ++frameNo;

        if (shotFrame > 0 && frameNo == shotFrame)
            m_renderer->requestScreenShot(shotPath);

        if (smokeFrames > 0 && --smokeFrames == 0) m_stop.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    m_backend.saveHighScore();
    // bgfx::shutdown() signals and joins its internal render thread; after
    // this returns no bgfx thread is left running.
    m_renderer->shutdown();
    m_done.store(true);
    fprintf(stderr, "[tetris] done. high score: %ld\n", m_backend.state().highScore);
}

}  // namespace tetris
