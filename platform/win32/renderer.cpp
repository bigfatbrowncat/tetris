// Windows renderer implementation (the platform hooks for the Direct3D 11
// composition path) + the tetris::frontend::createRenderer() definition.
#include "renderer.h"

#include "ui/window.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace tetris::platform::win32 {

bool Renderer::platformInit(const UiWindow* win, bgfx::Init& init) {
    // Windows composition path: the UI layer owns the D3D11 device, the
    // composition swap chain and a full-window scene texture (created in
    // uiCreateWindow). bgfx adopts the device (platformData.context — it
    // AddRefs it) and renders into the scene texture (platformData.
    // backBuffer = its RTV — bgfx borrows the pointer and never releases
    // it); the UI layer blits the scene texture to the swap chain's back
    // buffer and presents it through DirectComposition.
    if (win->offscreen == 0 || win->context == nullptr || win->backBuffer == nullptr) {
        fprintf(stderr, "[tetris] Windows windowed (non-composition) mode is not supported\n");
        return false;
    }
    m_d3dDevice = win->context;
    m_nwh = win->nwh;
    m_nwhType = win->nwhType;

    // Single-threaded bgfx: latching bgfx::renderFrame() from this thread
    // (the UI thread that will drive bgfx::frame() from uiPumpEvents)
    // keeps bgfx from spawning its own render thread, so bgfx::frame()
    // performs the GPU submit inline on the caller — every D3D11 call
    // (bgfx's render + the UI layer's ResizeBuffers/Present) runs on one
    // thread (the POC's single-threaded discipline).
    bgfx::renderFrame();

    init.type = bgfx::RendererType::Direct3D11;
    init.fallback = false;
    init.platformData.context = win->context;
    init.platformData.backBuffer = win->backBuffer;
    init.platformData.backBufferDS = nullptr;  // bgfx creates/owns its depth texture
    init.platformData.nwh = win->nwh;          // HWND (unused in this mode; for debugging)
    init.platformData.ndt = nullptr;
    init.platformData.type = (bgfx::NativeWindowHandleType::Enum)win->nwhType;
    uint32_t w = 0, h = 0;
    uiWindowSize(&w, &h);
    init.resolution.width = w != 0 ? w : 640;
    init.resolution.height = h != 0 ? h : 640;
    init.resolution.reset = 0;  // flip() never presents in this mode; vsync is the UI layer's Present
    return true;
}

bool Renderer::platformResize(uint32_t w, uint32_t h) {
    // The default frame buffer is the UI layer's full-window scene
    // texture (the RTV passed as platformData.backBuffer); the UI layer
    // resizes the swap chain and (re)creates the scene texture whenever
    // the window size changes. Re-point bgfx at the (re)created RTV:
    // setPlatformData() marks the platform data dirty so the next reset
    // re-reads the backBuffer, and the reset moves bgfx's depth texture
    // to the new size. (Runs on the UI thread — single-threaded bgfx.)
    void* rtv = uiWindowResize(w, h);
    if (rtv == nullptr) return false;
    bgfx::PlatformData pd;
    pd.context = m_d3dDevice;   // unchanged (setPlatformData requires it)
    pd.backBuffer = rtv;        // the (new) back-buffer RTV
    pd.backBufferDS = nullptr;  // bgfx owns its depth texture
    pd.nwh = m_nwh;
    pd.ndt = nullptr;
    pd.type = (bgfx::NativeWindowHandleType::Enum)m_nwhType;
    bgfx::setPlatformData(pd);
    bgfx::reset(w, h, 0);
    return true;
}

void Renderer::platformPrepareView() {
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101018ff, 1.0f, 0);
}

void Renderer::platformSyncPresent() {
    // The UI layer's resize present (DO_NOT_SEQUENCE) is the sync.
}

void Renderer::platformRequestScreenShot(const char* path) {
    // bgfx::requestScreenShot cannot work in external-back-buffer mode
    // (its D3D11 capture path expects its own swap chain); read the
    // full-window scene texture back through the UI layer instead — the
    // frame bgfx just rendered (the blit copies it 1:1 to the back
    // buffer, so it is exactly the frame about to be presented). The
    // texture is R8G8B8A8 — RGBA bytes, not BGRA.
    if (lastW > 0 && lastH > 0) {
        std::vector<uint8_t> px((size_t)lastW * lastH * 4);
        const int n = uiWindowReadBackbuffer(lastW, lastH, px.data(), (uint32_t)px.size());
        if (n > 0)
            frontend::writeBmp(path, lastW, lastH, (uint32_t)(lastW * 4), px.data(),
                               /*yflip=*/false, /*isBgra=*/false);
    }
}

}  // namespace tetris::platform::win32

namespace tetris::frontend {
// The platform module's factory: constructs this platform's renderer.
std::unique_ptr<Renderer> createRenderer() {
    return std::make_unique<platform::win32::Renderer>();
}
}  // namespace tetris::frontend
