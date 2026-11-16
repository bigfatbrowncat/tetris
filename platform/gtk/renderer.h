// ============================================================================
//  Linux renderer — the platform part of tetris::frontend::Renderer.
//  ---------------------------------------------------------------------------
//  Offscreen path: single-threaded bgfx with the Vulkan backend renders the
//  scene into an app-created texture that is exactly the window's
//  device-pixel size (recreated on resize). Each frame the texture's memory
//  is exported as a dma-buf (bgfx::getTextureDmaBuf) and handed to the UI
//  layer (uiWindowSetFrameImage), which imports it into the GTK GL area's
//  native GLES2 context and blits it 1:1 — GPU to GPU, only the texture
//  crosses the Vulkan/GL boundary.
//
//  Threading: created and driven from the UI thread only. Because
//  bgfx::renderFrame() is latched before bgfx::init (from that same
//  thread), bgfx runs single-threaded: bgfx::frame() performs the GPU submit
//  inline on the caller. The UI layer's blit is the present, so there is no
//  extra present sync (platformSyncPresent stays the base default: a no-op).
//
//  The scene construction and the draw flow are the base class
//  (frontend/renderer.{h,cpp}); this file implements only the platform hooks
//  and the frontend's createRenderer() factory for this platform.
// ============================================================================
#pragma once

#include "frontend/renderer.h"

namespace tetris::platform::gtk {

class Renderer : public tetris::frontend::Renderer {
private:
    bool platformInit(const UiWindow* win, bgfx::Init& init) override;
    void platformShutdown() override;
    bool platformResize(uint32_t w, uint32_t h) override;
    void platformPrepareView() override;
    void platformPresentFrame() override;
    void platformRequestScreenShot(const char* path) override;

    // The offscreen scene texture + its frame buffer (created on the first
    // render/resize, destroyed in platformShutdown).
    bool m_offscreen = false;
    bgfx::TextureHandle m_sceneRT = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_sceneFB = BGFX_INVALID_HANDLE;
};

}  // namespace tetris::platform::gtk
