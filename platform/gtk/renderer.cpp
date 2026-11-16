// Linux renderer implementation (the platform hooks for the offscreen
// Vulkan path) + the tetris::frontend::createRenderer() definition.
#include "renderer.h"

#include "ui/window.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace tetris::platform::gtk {

bool Renderer::platformInit(const UiWindow* win, bgfx::Init& init) {
    m_offscreen = (win->offscreen != 0);
    if (!m_offscreen) {
        fprintf(stderr, "[tetris] Linux windowed (non-offscreen) mode is not supported\n");
        return false;
    }

    // Single-threaded bgfx: latching bgfx::renderFrame() from this thread
    // (the UI thread that will drive bgfx::frame() inside the GL area's
    // render callback) keeps bgfx from spawning its own render thread, so
    // bgfx::frame() performs the GPU submit inline on the caller.
    bgfx::renderFrame();

    // Vulkan backend, headless: no native window handle, no backbuffer
    // (all platform data stays NULL, resolution 0x0 — the requirements
    // for bgfx's headless mode). The scene renders into an app-created
    // texture (made on first render/resize); each frame its memory is
    // exported as a dma-buf and handed to the UI layer, which blits it
    // 1:1 into the GL area.
    init.type = bgfx::RendererType::Vulkan;
    init.fallback = false;
    init.platformData.nwh = nullptr;
    init.platformData.ndt = nullptr;
    init.resolution.width = 0;
    init.resolution.height = 0;
    init.resolution.reset = 0;
    return true;
}

void Renderer::platformShutdown() {
    if (m_offscreen) {
        if (bgfx::isValid(m_sceneFB)) { bgfx::destroy(m_sceneFB); m_sceneFB = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_sceneRT)) { bgfx::destroy(m_sceneRT); m_sceneRT = BGFX_INVALID_HANDLE; }
    }
}

bool Renderer::platformResize(uint32_t w, uint32_t h) {
    // The scene buffer is exactly the window's device-pixel size and
    // is recreated on every size change (bgfx defers the actual GPU
    // destruction of the old image until its referencing frames are
    // done). BGFX_TEXTURE_DMA_BUF_EXPORT makes the image memory
    // exportable as a dma-buf (bgfx::getTextureDmaBuf, consumed in
    // endFrame); BGFX_TEXTURE_READ_BACK lets requestScreenShot read
    // it back.
    if (bgfx::isValid(m_sceneFB)) { bgfx::destroy(m_sceneFB); m_sceneFB = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(m_sceneRT)) { bgfx::destroy(m_sceneRT); m_sceneRT = BGFX_INVALID_HANDLE; }
    const uint64_t tflags = BGFX_TEXTURE_RT
        | BGFX_TEXTURE_DMA_BUF_EXPORT
        | BGFX_TEXTURE_READ_BACK;
    m_sceneRT = bgfx::createTexture2D((uint16_t)w, (uint16_t)h, false, 1,
                                      bgfx::TextureFormat::RGBA8, tflags, nullptr);
    if (!bgfx::isValid(m_sceneRT)) {
        fprintf(stderr, "[tetris] scene texture creation failed for %ux%u\n", w, h);
        return false;
    }
    m_sceneFB = bgfx::createFrameBuffer(1, &m_sceneRT);
    return true;
}

void Renderer::platformPrepareView() {
    // Render the scene into the app-owned scene texture. No depth
    // attachment, so clear color only (the scene is 2D and never
    // uses depth). The clear color matches the UI layer's exposed-
    // strip clear (GTK CSS #101018), so a fresh resize strip blends
    // in with the scene background.
    bgfx::setViewFrameBuffer(0, m_sceneFB);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x101018ff, 1.0f, 0);
}

void Renderer::platformPresentFrame() {
    if (m_offscreen) {
        // Export the frame just rendered as a dma-buf and hand
        // fd/modifier/pitch to the UI layer. bgfx::frame() (single-
        // threaded) only submitted the scene render; getTextureDmaBuf
        // waits for the GPU (vkDeviceWaitIdle) before exporting, so the
        // fd always refers to a completed frame. The UI layer imports it
        // into the GL area's GLES2 context on the same thread, right
        // before its blit (GPU to GPU — only the texture crosses the
        // Vulkan/GL boundary).
        int32_t fd = -1;
        uint64_t modifier = 0;
        uint32_t pitch = 0;
        if (bgfx::isValid(m_sceneRT)
        &&  bgfx::getTextureDmaBuf(m_sceneRT, &fd, &modifier, &pitch)
        &&  fd >= 0) {
            uiWindowSetFrameImage(fd, modifier, pitch);
        }
        // On export failure the UI layer keeps blitting the previous
        // frame's image (the slot is simply not updated).
    }
}

void Renderer::platformRequestScreenShot(const char* path) {
    // bgfx::requestScreenShot only works on *window* frame buffers;
    // m_sceneFB is a texture FBO. Read the scene texture back with
    // bgfx::read instead (synchronous: readTexture kicks the command
    // queue with a fence wait). The readback is a straight row copy —
    // row 0 is the image top (Vulkan NDC +Y), which is also the BMP
    // convention, so no flip.
    if (bgfx::isValid(m_sceneRT) && lastW > 0 && lastH > 0) {
        bgfx::TextureRegion region;
        region.init(m_sceneRT);
        std::vector<uint8_t> px((size_t)lastW * lastH * 4);
        bgfx::read(region, px.data());
        bgfx::frame();
        frontend::writeBmp(path, lastW, lastH, (uint32_t)(lastW * 4), px.data(),
                           /*yflip=*/false, /*isBgra=*/false);
    }
}

}  // namespace tetris::platform::gtk

namespace tetris::frontend {
// The platform module's factory: constructs this platform's renderer.
std::unique_ptr<Renderer> createRenderer() {
    return std::make_unique<platform::gtk::Renderer>();
}
}  // namespace tetris::frontend
