// ============================================================================
//  Renderer — the common, platform-agnostic part of the bgfx renderer.
//  ---------------------------------------------------------------------------
//  A few dynamic vertex buffers, one draw call per texture per frame
//  (field artwork, brick gems, font-atlas text/HUD). All drawing is
//  derived from the TetrisBackend::Snapshot — this class knows nothing
//  about game rules.
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

// A rectangle in field.png pixel coordinates (top-left origin, y down).
struct ImageRect { int x0, y0, x1, y1; };
// A rectangle in world coordinates (y up).
struct RectF { float x0, y0, x1, y1; };

// The per-frame layout in world space: the board rect (aligned with the
// "glass" of field.png, the dark-brick background) and the HUD value slots
// (the artwork's right-side SCORE / LEVEL / LINES panels and the NEXT
// preview area). Recomputed every frame from the window size, because the
// artwork is contain-fit + centered in the window.
struct Layout {
    RectF board;
    RectF score;
    RectF level;
    RectF lines;
    RectF next;
};

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
    // Append one tetromino cell as a brick gem: the brick-atlas texture for
    // color index t (or a solid-color quad from the font atlas when the brick
    // images are missing), tinted by c, into the brick (or text) vertex
    // buffer. x0,y0 = cell bottom-left, x1,y1 = top-right (world, y up).
    void drawBrickCell(float x0, float y0, float x1, float y1, int t,
                       const RGB& c, float grow = 0.0f);
    // Append the NEXT preview: the 4x4 box with bottom-left (bx,by) and cell
    // size `cell`, built from brick gems.
    void drawNext(const TetrisBackend::Cell cells[4], float bx, float by,
                  float cell, float grow = 0.0f);
    // Append a numeric value in gold, centered in its panel slot.
    void drawValue(std::vector<Vert>& v, const RectF& slot, int value, int maxDigits);
    void drawOverlay(std::vector<Vert>& v, const Layout& L, const std::string& head,
                     const std::string& sub, float grow = 0.0f);
    // Build the scene: brick cells (locked grid, ghost, piece, NEXT) go into
    // m_brickVerts; HUD text and overlays into m_verts.
    void buildScene(const Layout& L, const TetrisBackend::Snapshot& s, float grow);
    void computeOrtho(float* proj, uint32_t pw, uint32_t ph) const;
    // The per-frame layout (board + HUD slots) in world space. The board is
    // the "glass" of field.png mapped through the artwork's contain-fit
    // screen rect into world; without the artwork, the old fixed layout.
    Layout computeLayout(uint32_t pw, uint32_t ph) const;
    // Load res/field.png (next to the executable) into a GPU texture.
    // A failure is non-fatal: the game renders without the background.
    void loadFieldImage();
    // Load the seven res/brick-<color>.png textures into one 4x2 GPU atlas.
    // A failure is non-fatal: pieces render as solid-color quads instead.
    void loadBricks();
    // The UV cell of piece color t in the brick atlas (4x2 grid, row-major).
    UVRect brickUV(int t) const;
    // Append the field background quad (contain-fit to the window, centered)
    // in world space. The window center maps to world (13,13) with
    // min(pw,ph)/26 px per world unit (computeOrtho), so the screen-space
    // image rectangle converts to world by dividing by that scale.
    void drawField(std::vector<Vert>& v, uint32_t pw, uint32_t ph);

    // --- state ---------------------------------------------------------------
    bool ok = false;
    const bgfx::Caps* caps = nullptr;

    // --- GPU resources (created in init, destroyed in shutdown) ---------------
    bgfx::DynamicVertexBufferHandle vbuf = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uTex = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;
    // Frame vertex assembly: text/HUD (font atlas) and brick gems (brick
    // atlas); the two submits use different textures, hence two buffers.
    std::vector<Vert> m_verts;
    std::vector<Vert> m_brickVerts;
    // Field background (res/field.png), drawn under the scene; BGFX_INVALID_
    // HANDLE when the image could not be loaded.
    bgfx::TextureHandle fieldTex = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle fieldVbuf = BGFX_INVALID_HANDLE;
    uint32_t fieldW = 0, fieldH = 0;
    // Brick atlas (the seven res/brick-<color>.png gems in a 4x2 grid);
    // BGFX_INVALID_HANDLE when the images could not be loaded (the pieces
    // then render as solid-color quads from the font atlas).
    bgfx::TextureHandle brickTex = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle brickVbuf = BGFX_INVALID_HANDLE;
    uint32_t brickW = 0, brickH = 0;  // one brick's size in pixels
    bool m_bricksOk = false;
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
