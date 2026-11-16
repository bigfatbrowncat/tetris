// macOS renderer implementation (the platform hooks for Metal) + the
// tetris::frontend::createRenderer() definition.
#include "renderer.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <memory>

namespace tetris::platform::cocoa {

bool Renderer::platformInit(const UiWindow* win, bgfx::Init& init) {
    init.type = bgfx::RendererType::Count;   // auto-select -> Metal
    init.fallback = true;
    init.platformData.nwh = win->nwh;
    init.platformData.ndt = win->ndt;
    init.platformData.type = (bgfx::NativeWindowHandleType::Enum)win->nwhType;
    init.resolution.width = 640;
    init.resolution.height = 640;
    init.resolution.reset = BGFX_RESET_VSYNC;
    return true;
}

bool Renderer::platformResize(uint32_t w, uint32_t h) {
    // The Metal back buffer is only resized via bgfx::reset(); without it
    // the drawable stays at the init size (640x640) for the whole run,
    // while the projection below is computed for the real window size —
    // Core Animation then stretches the small buffer to fill the window
    // and the scene appears zoomed in / cropped. Resize the drawable
    // whenever the window size changes (the game thread is the bgfx API
    // thread, so this is a plain once-per-frame API call).
    bgfx::reset(w, h, BGFX_RESET_VSYNC);
    return true;
}

void Renderer::platformPrepareView() {
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101018ff, 1.0f, 0);
}

void Renderer::platformSyncPresent() {
    // Issue the extra bgfx::frame() calls that bgfx's pipeline needs before
    // the present is guaranteed.
    bgfx::frame();
    bgfx::frame();
}

void Renderer::platformRequestScreenShot(const char* path) {
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path);
}

}  // namespace tetris::platform::cocoa

namespace tetris::frontend {
// The platform module's factory: constructs this platform's renderer.
std::unique_ptr<Renderer> createRenderer() {
    return std::make_unique<platform::cocoa::Renderer>();
}
}  // namespace tetris::frontend
