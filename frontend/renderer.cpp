// Renderer implementation (bgfx; Metal on macOS, Vulkan (offscreen) on
// Linux, Direct3D 11 on Windows).
#include "renderer.h"

#include "font.h"
#include "shaders/vs_quad.h"
#include "shaders/fs_quad.h"

#include <bgfx/bgfx.h>
#include <bx/platform.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace tetris {

namespace {

constexpr int W = TetrisBackend::kCols;
constexpr int H = TetrisBackend::kRows;
constexpr int EMPTY = -1;

struct RGB { float r, g, b; };
const RGB kColor[7] = {
    {0.20f, 0.90f, 1.00f},  // I cyan
    {1.00f, 0.95f, 0.20f},  // O yellow
    {0.85f, 0.30f, 1.00f},  // T purple
    {0.30f, 1.00f, 0.40f},  // S green
    {1.00f, 0.35f, 0.35f},  // Z red
    {0.30f, 0.40f, 1.00f},  // J blue
    {1.00f, 0.60f, 0.25f},  // L orange
};
const RGB kColorDim[7] = {
    {0.10f, 0.45f, 0.50f},
    {0.50f, 0.48f, 0.10f},
    {0.42f, 0.15f, 0.50f},
    {0.15f, 0.50f, 0.20f},
    {0.50f, 0.17f, 0.17f},
    {0.15f, 0.20f, 0.50f},
    {0.50f, 0.30f, 0.12f},
};
const RGB kEmptyCell  {0.10f, 0.10f, 0.13f};
const RGB kBorder     {0.70f, 0.72f, 0.78f};
const RGB kLabel      {0.55f, 0.60f, 0.70f};
const RGB kValue      {0.92f, 0.94f, 1.00f};
const RGB kTitle      {1.00f, 0.85f, 0.30f};
const RGB kControls   {0.45f, 0.48f, 0.56f};
const RGB kOverlayDim {0.05f, 0.05f, 0.07f};
const RGB kOverlayHead{1.00f, 0.30f, 0.30f};
const RGB kOverlaySub {0.75f, 0.78f, 0.85f};

// Per-side screen-space inflation of solid (opaque, white-strip-textured)
// quads, in pixels: adjacent solid quads overlap by 2*kSolidGrowPx. Closes
// the 1px rasterization crack that opens when a shared edge lands exactly on
// a pixel row/column CENTER: there the rasterizer's edge function is pure
// float noise (signal ~1e-4 px^2 vs rounding ~0.01 px^2), so that pixel row
// is only partly covered and whatever is behind (the light border ring) shows
// through as a "white" horizontal/vertical line — at certain window sizes
// only (e.g. the mid-board row when the client height is odd; every other
// row when the px/unit scale is x.5). At a pixel BOUNDARY the tiling is exact
// and no crack opens. The overlap puts the seam pixel ~kSolidGrowPx strictly
// inside both quads, far beyond the noise; text quads are NOT inflated (the
// atlas alpha must stay aligned to the glyph).
constexpr float kSolidGrowPx = 0.75f;

// World layout (1 world unit = 1 board cell). y grows up.
const float WORLD_X0 = 0.0f, WORLD_Y0 = 0.0f, WORLD_X1 = 26.0f, WORLD_Y1 = 26.0f;
const float BOARD_X0 = 12.0f, BOARD_Y0 = 3.0f;   // board left / bottom
const float PANEL_X  = 2.0f;

// Column-major 4x4 helpers (matching bx::mtxOrtho / bx::mtxIdentity).
void mtxIdentity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mtxOrtho(float* m, float l, float r, float b, float t,
              float n, float f, float off, bool homogeneousNdc) {
    const float aa = 2.0f / (r - l);
    const float bb = 2.0f / (t - b);
    const float cc = (homogeneousNdc ? 2.0f : 1.0f) / (f - n);
    const float dd = (l + r) / (l - r);
    const float ee = (t + b) / (b - t);
    const float ff = homogeneousNdc ? (n + f) / (n - f) : n / (n - f);
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = aa;   m[5] = bb;   m[10] = cc;
    m[12] = dd + off; m[13] = ee; m[14] = ff;
    m[15] = 1.0f;
}

// Screenshot: bgfx hands us 4-byte pixels (RGBA or BGRA depending on backend);
// we write a 24-bit BMP (B G R order).
void writeBmp(const char* path, uint32_t w, uint32_t h, uint32_t pitch,
              const uint8_t* data, bool yflip, bool isBgra) {
    uint32_t rowBytes = w * 3;
    uint32_t pad = (4 - (rowBytes % 4)) % 4;
    uint32_t stride = rowBytes + pad;
    uint32_t dataSize = stride * h;
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[tetris] cannot open %s for writing\n", path); return; }

    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    // 14-byte file header
    fputc('B', f); fputc('M', f);
    w32(54 + dataSize);   // file size
    w16(0); w16(0);       // reserved
    w32(54);              // offset to pixel data
    // 40-byte DIB header (BITMAPINFOHEADER)
    w32(40);              // header size
    int32_t iw = (int32_t)w; w32((uint32_t)iw);
    int32_t ih = (int32_t)h; w32((uint32_t)ih);  // positive = bottom-up
    w16(1);               // planes
    w16(24);              // bits per pixel
    w32(0);               // compression (BI_RGB)
    w32(dataSize);        // image size
    w32(2835); w32(2835); // ~72 dpi
    w32(0); w32(0);       // palette sizes

    std::vector<uint8_t> row(stride, 0);
    for (uint32_t bmpRow = 0; bmpRow < h; bmpRow++) {
        // bottom-up BMP: row 0 is the image bottom
        uint32_t srcRow = yflip ? bmpRow : (h - 1 - bmpRow);
        const uint8_t* src = data + (size_t)srcRow * pitch;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src + (size_t)x * 4;
            if (isBgra) {  // p = B G R A
                row[x * 3 + 0] = p[0];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = p[2];
            } else {  // p = R G B A
                row[x * 3 + 0] = p[2];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = p[0];
            }
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    fprintf(stderr, "[tetris] saved screenshot %ux%u -> %s\n", w, h, path);
}

struct ShotCallback : bgfx::CallbackI {
    void fatal(const char*, uint16_t, bgfx::Fatal::Enum, const char* _str) override {
        fprintf(stderr, "[bgfx] fatal: %s\n", _str ? _str : "");
    }
    void traceVargs(const char*, uint16_t, const char* _fmt, va_list _args) override {
        vfprintf(stderr, _fmt, _args); fputc('\n', stderr);
    }
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
    void screenShot(const char* _filePath, uint32_t _w, uint32_t _h, uint32_t _pitch,
                    bgfx::TextureFormat::Enum _format, const void* _data, uint32_t, bool _yflip) override {
        bool isBgra = _format == bgfx::TextureFormat::BGRA8;
        writeBmp(_filePath, _w, _h, _pitch, (const uint8_t*)_data, _yflip, isBgra);
    }
};

ShotCallback g_shotImpl;

struct Vert { float x, y, u, v, r, g, b; };

}  // namespace

class Renderer::Impl {
public:
    bgfx::DynamicVertexBufferHandle vbuf = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uTex = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;
    std::vector<Vert> verts;
    uint32_t lastW = 0, lastH = 0;
    uint32_t m_resetFlags = BGFX_RESET_VSYNC;
    bool ok = false;
    const bgfx::Caps* caps = nullptr;

#if !BX_PLATFORM_OSX && !BX_PLATFORM_WINDOWS
    // Linux offscreen path: single-threaded bgfx with the Vulkan backend
    // renders the scene into an app-owned texture that is exactly the
    // window's device-pixel size. Each frame the texture's memory is
    // exported as a dma-buf (bgfx::getTextureDmaBuf) and handed to the UI
    // layer (uiWindowSetFrameImage); the UI layer imports it into the GTK
    // GL area's native GLES2 context and blits it 1:1 (GPU to GPU).
    bool m_offscreen = false;
    bgfx::TextureHandle sceneRT = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle sceneFB = BGFX_INVALID_HANDLE;
#elif BX_PLATFORM_WINDOWS
    // Windows composition path: the UI layer's ID3D11Device* (adopted via
    // Init.platformData.context) and the HWND — kept to re-issue
    // bgfx::setPlatformData with the *same* context/ndt when the back-buffer
    // RTV is re-created on a resize (setPlatformData requires them unchanged
    // and re-reads only the backBuffer).
    void*    m_d3dDevice = nullptr;
    void*    m_nwh = nullptr;
    uint32_t m_nwhType = UI_NWH_DEFAULT;
#endif

    bool init(const UiWindow* win) {
        bgfx::Init init;
        init.callback = &g_shotImpl;
#if BX_PLATFORM_OSX
        init.type = bgfx::RendererType::Count;   // auto-select -> Metal
        init.fallback = true;
        init.platformData.nwh = win->nwh;
        init.platformData.ndt = win->ndt;
        init.platformData.type = (bgfx::NativeWindowHandleType::Enum)win->nwhType;
        init.resolution.width = 640;
        init.resolution.height = 640;
        init.resolution.reset = m_resetFlags;
#elif BX_PLATFORM_WINDOWS
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
#else
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
#endif
        if (!bgfx::init(init)) {
            fprintf(stderr, "[tetris] bgfx init failed\n");
            return false;
        }
        caps = bgfx::getCaps();

        layout.begin();
        layout.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float);
        layout.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
        layout.add(bgfx::Attrib::Color0, 3, bgfx::AttribType::Float);
        layout.end();

        const bgfx::Memory* vsMem = bgfx::makeRef(vs_quad, sizeof(vs_quad));
        const bgfx::Memory* fsMem = bgfx::makeRef(fs_quad, sizeof(fs_quad));
        bgfx::ShaderHandle vs = bgfx::createShader(vsMem);
        bgfx::ShaderHandle fs = bgfx::createShader(fsMem);
        program = bgfx::createProgram(vs, fs, true);

        std::vector<uint8_t> atlasData((size_t)ATLAS_W * ATLAS_H * 4);
        buildAtlas(atlasData.data(), ATLAS_W, ATLAS_H);
        const bgfx::Memory* aMem = bgfx::copy(atlasData.data(), (uint32_t)atlasData.size());
        uint64_t sflags = BGFX_TEXTURE_NONE
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
        atlas = bgfx::createTexture2D(ATLAS_W, ATLAS_H, false, 1, bgfx::TextureFormat::RGBA8, sflags, aMem);
        uTex = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);

        vbuf = bgfx::createDynamicVertexBuffer(8192, layout, BGFX_BUFFER_ALLOW_RESIZE);
        ok = true;
        return true;
    }

    void shutdown() {
        if (!ok) return;
#if !BX_PLATFORM_OSX && !BX_PLATFORM_WINDOWS
        if (m_offscreen) {
            if (bgfx::isValid(sceneFB)) { bgfx::destroy(sceneFB); sceneFB = BGFX_INVALID_HANDLE; }
            if (bgfx::isValid(sceneRT)) { bgfx::destroy(sceneRT); sceneRT = BGFX_INVALID_HANDLE; }
        }
#endif
        if (bgfx::isValid(vbuf)) bgfx::destroy(vbuf);
        if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
        if (bgfx::isValid(uTex)) bgfx::destroy(uTex);
        if (bgfx::isValid(program)) bgfx::destroy(program);
        bgfx::shutdown();
        ok = false;
    }

    bool render(const TetrisBackend::Snapshot& s, uint32_t pw, uint32_t ph) {
        if (!ok || pw == 0 || ph == 0) return false;
#if BX_PLATFORM_OSX
        // The Metal back buffer is only resized via bgfx::reset(); without it
        // the drawable stays at the init size (640x640) for the whole run,
        // while the projection below is computed for the real window size —
        // Core Animation then stretches the small buffer to fill the window
        // and the scene appears zoomed in / cropped. Resize the drawable
        // whenever the window size changes (the game thread is the bgfx API
        // thread, so this is a plain once-per-frame API call).
        if (pw != lastW || ph != lastH) {
            bgfx::reset(pw, ph, m_resetFlags);
            lastW = pw;
            lastH = ph;
        }
#elif BX_PLATFORM_WINDOWS
        // The default frame buffer is the UI layer's full-window scene
        // texture (the RTV passed as platformData.backBuffer); the UI layer
        // resizes the swap chain and (re)creates the scene texture whenever
        // the window size changes. Re-point bgfx at the (re)created RTV:
        // setPlatformData() marks the platform data dirty so the next reset
        // re-reads the backBuffer, and the reset moves bgfx's depth texture
        // to the new size. (Runs on the UI thread — single-threaded bgfx.)
        if (pw != lastW || ph != lastH) {
            void* rtv = uiWindowResize(pw, ph);
            if (rtv == nullptr) return false;
            bgfx::PlatformData pd;
            pd.context = m_d3dDevice;   // unchanged (setPlatformData requires it)
            pd.backBuffer = rtv;        // the (new) back-buffer RTV
            pd.backBufferDS = nullptr;  // bgfx owns its depth texture
            pd.nwh = m_nwh;
            pd.ndt = nullptr;
            pd.type = (bgfx::NativeWindowHandleType::Enum)m_nwhType;
            bgfx::setPlatformData(pd);
            bgfx::reset(pw, ph, 0);
            lastW = pw;
            lastH = ph;
        }
#else
        if (m_offscreen && (pw != lastW || ph != lastH)) {
            // The scene buffer is exactly the window's device-pixel size and
            // is recreated on every size change (bgfx defers the actual GPU
            // destruction of the old image until its referencing frames are
            // done). BGFX_TEXTURE_DMA_BUF_EXPORT makes the image memory
            // exportable as a dma-buf (bgfx::getTextureDmaBuf, consumed in
            // endFrame); BGFX_TEXTURE_READ_BACK lets requestScreenShot read
            // it back.
            if (bgfx::isValid(sceneFB)) { bgfx::destroy(sceneFB); sceneFB = BGFX_INVALID_HANDLE; }
            if (bgfx::isValid(sceneRT)) { bgfx::destroy(sceneRT); sceneRT = BGFX_INVALID_HANDLE; }
            const uint64_t tflags = BGFX_TEXTURE_RT
                | BGFX_TEXTURE_DMA_BUF_EXPORT
                | BGFX_TEXTURE_READ_BACK;
            sceneRT = bgfx::createTexture2D((uint16_t)pw, (uint16_t)ph, false, 1,
                                            bgfx::TextureFormat::RGBA8, tflags, nullptr);
            if (!bgfx::isValid(sceneRT)) {
                fprintf(stderr, "[tetris] scene texture creation failed for %ux%u\n", pw, ph);
                return false;
            }
            sceneFB = bgfx::createFrameBuffer(1, &sceneRT);
            lastW = pw;
            lastH = ph;
        }
#endif
        verts.clear();
        // Per-side inflation for the solid quads (world units). The scene
        // spans min(pw,ph) pixels over 26 world units (computeOrtho), so
        // kSolidGrowPx of screen space is kSolidGrowPx/scale world units.
        const float scale = (float)(pw < ph ? pw : ph) / 26.0f;
        const float grow = scale > 0.0f ? kSolidGrowPx / scale : 0.0f;
        buildScene(verts, s, grow);
        if (verts.empty()) return false;

        const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(Vert)));
        bgfx::update(vbuf, 0, mem);

        float view[16], proj[16];
        mtxIdentity(view);
        computeOrtho(proj, pw, ph);
        bgfx::setViewTransform(0, view, proj);
        bgfx::setViewRect(0, 0, 0, (uint16_t)pw, (uint16_t)ph);
#if !BX_PLATFORM_OSX && !BX_PLATFORM_WINDOWS
        if (m_offscreen) {
            // Render the scene into the app-owned scene texture. No depth
            // attachment, so clear color only (the scene is 2D and never
            // uses depth). The clear color matches the UI layer's exposed-
            // strip clear (GTK CSS #101018), so a fresh resize strip blends
            // in with the scene background.
            bgfx::setViewFrameBuffer(0, sceneFB);
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x101018ff, 1.0f, 0);
        } else
#endif
        {
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101018ff, 1.0f, 0);
        }

        bgfx::setTexture(0, uTex, atlas);
        // Draw exactly the vertices we uploaded this frame. Without the count,
        // setVertexBuffer draws the whole dynamic buffer (8192 verts), so any
        // stale tail left by a smaller/larger previous upload (or garbage
        // latched during a fast resize) is re-rendered over the scene every
        // frame — the static cyan/blue bands seen after resizing.
        bgfx::setVertexBuffer(0, vbuf, 0, (uint32_t)verts.size());
        // Blending composites the text glyphs (straight alpha from the atlas)
        // over the scene; opaque geometry (a=1) is unaffected.
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(0, program);
        return true;
    }

    void endFrame() {
        if (!ok) return;
        bgfx::frame();
#if !BX_PLATFORM_OSX && !BX_PLATFORM_WINDOWS
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
            if (bgfx::isValid(sceneRT)
            &&  bgfx::getTextureDmaBuf(sceneRT, &fd, &modifier, &pitch)
            &&  fd >= 0) {
                uiWindowSetFrameImage(fd, modifier, pitch);
            }
            // On export failure the UI layer keeps blitting the previous
            // frame's image (the slot is simply not updated).
        }
#endif
    }

    void syncPresent() {
        if (!ok) return;
#if BX_PLATFORM_WINDOWS
        return;  // the UI layer's resize present (DO_NOT_SEQUENCE) is the sync
#elif !BX_PLATFORM_OSX
        if (m_offscreen) return;  // the UI layer's blit is the present
#endif
        bgfx::frame();
        bgfx::frame();
    }

    void requestScreenShot(const char* path) {
        if (!ok) return;
#if BX_PLATFORM_WINDOWS
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
                writeBmp(path, lastW, lastH, (uint32_t)(lastW * 4), px.data(),
                         /*yflip=*/false, /*isBgra=*/false);
        }
        return;
#elif !BX_PLATFORM_OSX
        if (m_offscreen) {
            // bgfx::requestScreenShot only works on *window* frame buffers;
            // sceneFB is a texture FBO. Read the scene texture back with
            // bgfx::read instead (synchronous: readTexture kicks the command
            // queue with a fence wait). The readback is a straight row copy —
            // row 0 is the image top (Vulkan NDC +Y), which is also the BMP
            // convention, so no flip.
            if (bgfx::isValid(sceneRT) && lastW > 0 && lastH > 0) {
                bgfx::TextureRegion region;
                region.init(sceneRT);
                std::vector<uint8_t> px((size_t)lastW * lastH * 4);
                bgfx::read(region, px.data());
                bgfx::frame();
                writeBmp(path, lastW, lastH, (uint32_t)(lastW * 4), px.data(),
                         /*yflip=*/false, /*isBgra=*/false);
            }
            return;
        }
#endif
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path);
    }

    // y0 = bottom, y1 = top (world y grows up). v grows down in the texture.
    // grow (world units, per side) inflates the quad so adjacent solid quads
    // overlap by 2*grow in screen space — see kSolidGrowPx. Only pass it for
    // solid quads (the uniform white strip makes the stretch invisible).
    void addQuad(std::vector<Vert>& v, float x0, float y0, float x1, float y1,
                 const UVRect& uv, float r, float g, float b, float grow = 0.0f) {
        x0 -= grow;
        y0 -= grow;
        x1 += grow;
        y1 += grow;
        v.push_back({x0, y0, uv.u0, uv.v1, r, g, b});  // BL
        v.push_back({x1, y0, uv.u1, uv.v1, r, g, b});  // BR
        v.push_back({x0, y1, uv.u0, uv.v0, r, g, b});  // TL
        v.push_back({x1, y0, uv.u1, uv.v1, r, g, b});  // BR
        v.push_back({x1, y1, uv.u1, uv.v0, r, g, b});  // TR
        v.push_back({x0, y1, uv.u0, uv.v0, r, g, b});  // TL
    }

    void drawText(std::vector<Vert>& v, float x, float y, const std::string& s,
                  const RGB& c, float cell = 1.0f) {
        for (char ch : s) {
            char cc = ch;
            if (cc >= 'a' && cc <= 'z') cc = (char)(cc - 'a' + 'A');
            if (cc < 32 || cc > 126) cc = 32;
            addQuad(v, x, y, x + cell, y + cell, glyphUV(cc), c.r, c.g, c.b);
            x += cell;
        }
    }

    void drawNext(std::vector<Vert>& v, const TetrisBackend::Cell cells[4], float bx, float by,
                  float grow = 0.0f) {
        addQuad(v, bx, by, bx + 4, by + 4, solidUV(),
                kEmptyCell.r, kEmptyCell.g, kEmptyCell.b, grow);
        for (int i = 0; i < 4; i++) {
            int dx = cells[i].x, dy = cells[i].y;
            int t = cells[i].color;
            float x0 = bx + dx, x1 = bx + dx + 1;
            float yBot = by + (3 - dy), yTop = by + (4 - dy);
            RGB c = kColor[t];
            addQuad(v, x0, yBot, x1, yTop, solidUV(), c.r, c.g, c.b, grow);
        }
    }

    void drawOverlay(std::vector<Vert>& v, const std::string& head, const std::string& sub,
                     float grow = 0.0f) {
        addQuad(v, BOARD_X0, BOARD_Y0, BOARD_X0 + W, BOARD_Y0 + H,
                solidUV(), kOverlayDim.r, kOverlayDim.g, kOverlayDim.b, grow);
        float cx = BOARD_X0 + W / 2.0f, cy = BOARD_Y0 + H / 2.0f;
        drawText(v, cx - head.size() * 0.6f, cy + 1.0f, head, kOverlayHead, 1.2f);
        drawText(v, cx - sub.size() * 0.4f, cy - 1.0f, sub, kOverlaySub, 0.8f);
    }

    void buildScene(std::vector<Vert>& v, const TetrisBackend::Snapshot& s, float grow) {
        // Board border ring (drawn behind the cells); inflated like the cells
        // so the inflated outer cells stay fully inside the ring.
        addQuad(v, BOARD_X0 - 0.3f, BOARD_Y0 - 0.3f, BOARD_X0 + W + 0.3f, BOARD_Y0 + H + 0.3f,
                solidUV(), kBorder.r, kBorder.g, kBorder.b, grow);

        // Board cells (grid y=0 is the top row); inflated so adjacent cells
        // overlap (kSolidGrowPx) and the shared-edge pixel is never left to
        // the rasterizer's float-noise edge case.
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int val = s.grid[y][x];
                float x0 = BOARD_X0 + x, x1 = BOARD_X0 + x + 1;
                float yTop = BOARD_Y0 + (H - y), yBot = BOARD_Y0 + (H - 1 - y);
                RGB c = (val == EMPTY) ? kEmptyCell : kColor[val];
                addQuad(v, x0, yBot, x1, yTop, solidUV(), c.r, c.g, c.b, grow);
            }
        }

        // Ghost + current piece (solid quads: inflated like the cells)
        if (s.pieceColor >= 0) {
            for (int i = 0; i < 4; i++) {
                int gx = s.ghost[i].x, gyy = s.ghost[i].y;
                if (gyy >= 0 && gyy < H && gx >= 0 && gx < W && s.grid[gyy][gx] == EMPTY) {
                    RGB c = kColorDim[s.ghost[i].color];
                    addQuad(v, BOARD_X0 + gx, BOARD_Y0 + (H - 1 - gyy),
                            BOARD_X0 + gx + 1, BOARD_Y0 + (H - gyy),
                            solidUV(), c.r, c.g, c.b, grow);
                }
            }
            for (int i = 0; i < 4; i++) {
                int px = s.piece[i].x, py = s.piece[i].y;
                if (py >= 0 && py < H && px >= 0 && px < W) {
                    RGB c = kColor[s.piece[i].color];
                    addQuad(v, BOARD_X0 + px, BOARD_Y0 + (H - 1 - py),
                            BOARD_X0 + px + 1, BOARD_Y0 + (H - py),
                            solidUV(), c.r, c.g, c.b, grow);
                }
            }
        }

        // Info panel
        drawText(v, PANEL_X, 22.0f, "SCORE", kLabel);
        drawText(v, PANEL_X, 20.5f, std::to_string(s.score), kValue);
        drawText(v, PANEL_X, 18.5f, "LEVEL", kLabel);
        drawText(v, PANEL_X, 17.0f, std::to_string(s.level), kValue);
        drawText(v, PANEL_X, 15.0f, "LINES", kLabel);
        drawText(v, PANEL_X, 13.5f, std::to_string(s.lines), kValue);
        drawText(v, PANEL_X, 11.5f, "NEXT", kLabel);
        drawNext(v, s.next, PANEL_X + 1, 6.5f, grow);
        drawText(v, PANEL_X, 4.5f, "HIGH", kLabel);
        drawText(v, PANEL_X, 3.0f, std::to_string(s.highScore), kValue);

        // Title
        const std::string title = "TETRIS";
        drawText(v, BOARD_X0 + W / 2.0f - title.size() * 0.7f, 24.0f, title, kTitle, 1.4f);

        // Controls
        const std::string c1 = "arrows/wasd move   up/x rot   z ccw";
        const std::string c2 = "space drop   p pause   r restart   q quit";
        drawText(v, 7.0f, 1.6f, c1, kControls, 0.55f);
        drawText(v, 7.0f, 0.9f, c2, kControls, 0.55f);

        // Overlays
        if (s.gameOver) drawOverlay(v, "GAME OVER", "r restart  -  q quit", grow);
        else if (s.paused) drawOverlay(v, "PAUSED", "press p to resume", grow);
    }

    void computeOrtho(float* proj, uint32_t pw, uint32_t ph) const {
        float worldW = WORLD_X1 - WORLD_X0, worldH = WORLD_Y1 - WORLD_Y0;
        float winAspect = (float)pw / (float)ph;
        float worldAspect = worldW / worldH;
        float cx = (WORLD_X0 + WORLD_X1) * 0.5f;
        float cy = (WORLD_Y0 + WORLD_Y1) * 0.5f;
        float vw, vh;
        if (winAspect > worldAspect) { vh = worldH; vw = worldH * winAspect; }
        else { vw = worldW; vh = worldW / winAspect; }
        float left = cx - vw * 0.5f, right = cx + vw * 0.5f;
        float bottom = cy - vh * 0.5f, top = cy + vh * 0.5f;
        mtxOrtho(proj, left, right, bottom, top, -1.0f, 1.0f, 0.0f, caps->homogeneousDepth);
    }
};

Renderer::Renderer() : m_impl(new Impl()) {}
Renderer::~Renderer() {}
bool Renderer::init(const UiWindow* win) { return m_impl->init(win); }
void Renderer::shutdown() { m_impl->shutdown(); }
bool Renderer::render(const TetrisBackend::Snapshot& s, uint32_t w, uint32_t h) { return m_impl->render(s, w, h); }
void Renderer::endFrame() { m_impl->endFrame(); }
void Renderer::syncPresent() { m_impl->syncPresent(); }
void Renderer::requestScreenShot(const char* path) { m_impl->requestScreenShot(path); }

}  // namespace tetris
