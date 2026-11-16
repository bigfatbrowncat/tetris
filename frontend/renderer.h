// ============================================================================
//  Renderer — the common, platform-agnostic part of the bgfx renderer.
//  ---------------------------------------------------------------------------
//  Single dynamic vertex buffer, one draw call per frame. All drawing is
//  derived from the TetrisBackend::Snapshot — this class knows nothing about
//  game rules.
//
//  This header (and frontend/renderer.cpp) owns everything that is identical
//  on every platform: the scene construction (board, pieces, HUD), the
//  vertex layout, the shader program, the font atlas, the per-frame draw
//  flow, and the screenshot file format. It only calls bgfx (the
//  cross-platform API) and the neutral ui/window.h C API — no platform
//  conditionals, no OS APIs.
//
//  The platform-specific parts — bgfx init for the platform's renderer type,
//  the resize path, the frame handover, the present sync, the screenshot
//  capture — are implemented by one subclass per platform, in files that
//  mirror this one:
//
//    platform/cocoa/renderer.{h,cpp}   tetris::platform::cocoa::Renderer
//        Metal; bgfx presents to the window (threaded mode).
//    platform/gtk/renderer.{h,cpp}     tetris::platform::gtk::Renderer
//        Headless Vulkan; the scene renders into an offscreen texture that
//        is exported as a dma-buf and blitted 1:1 by the GTK GL area.
//    platform/win32/renderer.{h,cpp}   tetris::platform::win32::Renderer
//        Direct3D 11; the scene renders into the UI layer's scene texture
//        (DirectComposition presentation).
//
//  Relationship: the platform renderers inherit this class and implement its
//  protected platform hooks; the base class fixes the frame flow
//  (init -> resize -> scene build -> submit -> frame -> handover).
//  tetris::frontend::createRenderer() — declared here, defined by the
//  platform module for the build platform — constructs the concrete
//  renderer.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>

#include "font.h"
#include "tetris_backend.h"
#include "ui/window.h"

namespace tetris::frontend {

// A linear color (the scene palette and the text colors).
struct RGB { float r, g, b; };
// One vertex: position (world), UV (atlas), color.
struct Vert { float x, y, u, v, r, g, b; };

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initialize bgfx (through the platform init hook) on the given native
    // window data (handle + platform type; offscreen flag on the Linux and
    // Windows composition paths).
    bool init(const UiWindow* win);

    // Destroy resources and shut bgfx down.
    void shutdown();

    // Build the scene for this frame and submit it (does not call bgfx::frame).
    // Returns false when nothing was submitted (the frame's texture is then
    // whatever the previous frame left in it).
    bool render(const TetrisBackend::Snapshot& s, uint32_t pixelW, uint32_t pixelH);

    // End the frame (bgfx::frame; then the platform's frame handover — on
    // Linux the rendered frame is exported as a dma-buf and handed to the
    // UI layer).
    void endFrame();

    // Block until the frame submitted by the preceding endFrame() has been
    // presented to the native surface.
    void syncPresent();

    // Ask bgfx to write a screenshot (via the platform's capture path).
    void requestScreenShot(const char* path);

protected:
    // ------------------------------------------------------------------------
    //  Platform hooks, implemented by tetris::platform::<os>::Renderer.
    //  The base class fixes the frame flow; the hooks fill in the platform's
    //  behavior at the marked points of it.
    // ------------------------------------------------------------------------
    // Configure the bgfx::Init (renderer type, fallback, platformData,
    // resolution; the single-threaded bgfx::renderFrame() latch on the
    // platforms that drive bgfx from the UI thread) and create the
    // platform's GPU resources. Return false when the renderer cannot start
    // (bgfx::init is then not called).
    virtual bool platformInit(const UiWindow* win, bgfx::Init& init) = 0;
    // Destroy the platform's resources (runs before the common resource
    // destruction + bgfx::shutdown).
    virtual void platformShutdown() {}
    // The platform's resize path, before the first draw at the new size
    // (Metal's bgfx::reset; Windows's scene RTV re-point; the Linux scene
    // texture recreation). Return false when the frame cannot be rendered.
    virtual bool platformResize(uint32_t w, uint32_t h) = 0;
    // Per-frame view setup (the render target + the clear state).
    virtual void platformPrepareView() = 0;
    // After bgfx::frame(): hand the rendered frame to the platform's
    // presentation path (the dma-buf export on Linux; a no-op when bgfx
    // presents to the surface directly).
    virtual void platformPresentFrame() {}
    // Block until the frame submitted by the preceding endFrame() has been
    // presented to the native surface (a no-op when the UI layer's present
    // is the sync — the Linux blit, the Windows DO_NOT_SEQUENCE present).
    virtual void platformSyncPresent() {}
    // Screenshot capture: the platform's path (bgfx::requestScreenShot, a
    // scene-texture readback, or the UI layer's readback).
    virtual void platformRequestScreenShot(const char* path) = 0;

    // The size of the frame buffer as it currently is (the last successful
    // resize); the hooks use it (e.g. for readbacks).
    uint32_t lastW = 0, lastH = 0;

private:
    // --- scene construction (all from the Snapshot; defined in renderer.cpp)
    void addQuad(std::vector<Vert>& v, float x0, float y0, float x1, float y1,
                 const UVRect& uv, float r, float g, float b, float grow = 0.0f);
    void drawText(std::vector<Vert>& v, float x, float y, const std::string& s,
                  const RGB& c, float cell = 1.0f);
    void drawNext(std::vector<Vert>& v, const TetrisBackend::Cell cells[4], float bx, float by,
                  float grow = 0.0f);
    void drawOverlay(std::vector<Vert>& v, const std::string& head, const std::string& sub,
                     float grow = 0.0f);
    void buildScene(std::vector<Vert>& v, const TetrisBackend::Snapshot& s, float grow);
    void computeOrtho(float* proj, uint32_t pw, uint32_t ph) const;

    // --- state ---------------------------------------------------------------
    bool ok = false;
    const bgfx::Caps* caps = nullptr;

    // --- GPU resources (created in init, destroyed in shutdown) ---------------
    bgfx::DynamicVertexBufferHandle vbuf = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uTex = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;
    std::vector<Vert> m_verts;
};

// Write a 24-bit BMP from 4-byte pixels (RGBA or BGRA, depending on the
// backend); the screenshot file format, shared by the bgfx screenshot
// callback and the platform capture paths.
void writeBmp(const char* path, uint32_t w, uint32_t h, uint32_t pitch,
              const uint8_t* data, bool yflip, bool isBgra);

// Implemented by exactly one platform module (platform/<os>/renderer.cpp);
// it constructs the concrete tetris::platform::<os>::Renderer for the build
// platform.
std::unique_ptr<Renderer> createRenderer();

}  // namespace tetris::frontend
