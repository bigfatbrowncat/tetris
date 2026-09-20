// ============================================================================
//  Renderer — bgfx rendering of a TetrisBackend::Snapshot.
//  (Metal on macOS; the OpenGL/EGL backend on Linux.)
//  ---------------------------------------------------------------------------
//  Single dynamic vertex buffer, one draw call per frame. All drawing is
//  derived from the Snapshot — this class knows nothing about game rules.
//
//  On Linux the renderer runs single-threaded and offscreen: it renders the
//  scene into a GL texture that the UI layer owns (UiWindow.sceneTex) and
//  presents as a full-frame quad in a GtkGLArea — GPU to GPU, no CPU copy.
//  bgfx adopts the UI layer's shared EGL context (Init.platformData.context)
//  and the scene texture is swapped in with bgfx::overrideInternal(), so
//  bgfx renders straight into it and never deletes it.
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
    // type, or the shared EGL texture pair on the Linux GL area path).
    bool init(const UiWindow* win);

    // Destroy resources and shut bgfx down.
    void shutdown();

    // Build the scene for this frame and submit it (does not call bgfx::frame).
    // Returns false when nothing was submitted (the frame's texture is then
    // whatever the previous frame left in it).
    bool render(const TetrisBackend::Snapshot& s, uint32_t pixelW, uint32_t pixelH);

    // End the frame (bgfx::frame; on Linux the scene texture write is
    // flushed with glFinish() before returning).
    void endFrame();

    // Block until the frame submitted by the preceding endFrame() has been
    // presented to the native surface. Issues the extra bgfx::frame() calls
    // that bgfx's pipeline needs before that present is guaranteed.
    // No-op on the Linux GL area path (the UI layer's blit is the present).
    void syncPresent();

    // Ask bgfx to write a screenshot (delivered via the CallbackI::screenShot).
    void requestScreenShot(const char* path);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace tetris
