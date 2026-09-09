// ============================================================================
//  Renderer — bgfx rendering of a TetrisBackend::Snapshot.
//  (Metal on macOS, Vulkan on Linux — bgfx auto-selects.)
//  ---------------------------------------------------------------------------
//  Single dynamic vertex buffer, one draw call per frame. All drawing is
//  derived from the Snapshot — this class knows nothing about game rules.
//
//  Threading: created/used on one (API) thread only. The bgfx::frame() call
//  (endFrame) is made from the same thread that called init(); because
//  bgfx::renderFrame() is never invoked before bgfx::init(), bgfx spawns its
//  own internal render thread that performs the actual GPU submit.
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

    // Initialize bgfx on the given native window data (handle + platform type).
    bool init(const UiWindow* win);

    // Destroy resources and shut bgfx down (joins its internal render thread).
    void shutdown();

    // Build the scene for this frame and submit it (does not call bgfx::frame).
    void render(const TetrisBackend::Snapshot& s, uint32_t pixelW, uint32_t pixelH);

    // End the frame (bgfx::frame).
    void endFrame();

    // Block until the frame submitted by the preceding endFrame() has been
    // presented to the native surface. Issues the extra bgfx::frame() calls
    // that bgfx's multithreaded pipeline needs before that present is guaranteed.
    // No-op when running offscreen (there is no native surface to present to).
    void syncPresent();

    // Ask bgfx to write a screenshot (delivered via the CallbackI::screenShot).
    void requestScreenShot(const char* path);

    // Offscreen mode only: after endFrame() has returned, the just-rendered
    // frame has been read back. Return its raw BGRA8 pixels (w*h*4 bytes, top
    // row first) and its size. Returns nullptr when not running offscreen.
    // The pointer stays valid until the next render().
    const uint8_t* framePixels(uint32_t* w, uint32_t* h) const;

    bool offscreen() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace tetris
