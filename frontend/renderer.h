// ============================================================================
//  Renderer — bgfx rendering of a TetrisBackend::Snapshot.
//  (Metal on macOS; headless Vulkan on Linux; Direct3D 11 on Windows.)
//  ---------------------------------------------------------------------------
//  Single dynamic vertex buffer, one draw call per frame. All drawing is
//  derived from the Snapshot — this class knows nothing about game rules.
//
//  On Linux the renderer runs single-threaded and offscreen: bgfx (Vulkan
//  backend) renders the scene into an app-created texture that is exactly
//  the window's device-pixel size (recreated on resize). Each frame the
//  texture's memory is exported as a dma-buf (bgfx::getTextureDmaBuf) and
//  handed to the UI layer (uiWindowSetFrameImage), which imports it into
//  the GTK GL area's native GLES2 context and blits it 1:1 — GPU to GPU,
//  only the texture crosses the Vulkan/GL boundary.
//
//  Threading (Linux): created and driven from the UI thread only. Because
//  bgfx::renderFrame() is latched before bgfx::init() (from that same
//  thread), bgfx runs single-threaded: bgfx::frame() performs the GPU submit
//  inline on the caller.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

#include "tetris_backend.h"
#include "ui/window.h"

namespace tetris {

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initialize bgfx on the given native window data (handle + platform
    // type; offscreen flag on the Linux and Windows composition paths).
    bool init(const UiWindow* win);

    // Destroy resources and shut bgfx down.
    void shutdown();

    // Build the scene for this frame and submit it (does not call bgfx::frame).
    // Returns false when nothing was submitted (the frame's texture is then
    // whatever the previous frame left in it).
    bool render(const TetrisBackend::Snapshot& s, uint32_t pixelW, uint32_t pixelH);

    // End the frame (bgfx::frame; on Linux the rendered frame is then
    // exported as a dma-buf and handed to the UI layer).
    void endFrame();

    // Block until the frame submitted by the preceding endFrame() has been
    // presented to the native surface. Issues the extra bgfx::frame() calls
    // that bgfx's pipeline needs before that present is guaranteed.
    // No-op on the Linux offscreen path (the UI layer's blit is the present).
    void syncPresent();

    // Ask bgfx to write a screenshot (delivered via the CallbackI::screenShot).
    void requestScreenShot(const char* path);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace tetris
