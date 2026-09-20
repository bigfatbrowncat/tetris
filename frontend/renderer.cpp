// Renderer implementation (bgfx; Metal on macOS, OpenGL/EGL on Linux).
#include "renderer.h"

#include "font.h"
#include "shaders/vs_quad.h"
#include "shaders/fs_quad.h"

#include <bgfx/bgfx.h>
#include <bx/platform.h>

#if !BX_PLATFORM_OSX
// Epoxy first: it defines __khrplatform_h_ itself, which suppresses the
// duplicate khronos enum in the real KHR/khrplatform.h pulled in by EGL.
#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

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

#if !BX_PLATFORM_OSX
    // Linux GL area path: single-threaded bgfx on the UI thread, rendering the
    // scene into the UI layer's shared scene texture (sceneTex) through the
    // UI layer's pbuffer context (bgfx adopts it via Init.platformData.context).
    bool m_glarea = false;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglPbuffer = EGL_NO_SURFACE;
    EGLSurface m_eglAreaSurf = EGL_NO_SURFACE;
    GLuint m_sceneTex = 0;
    bgfx::TextureHandle sceneRT = BGFX_INVALID_HANDLE;   // bgfx handle over sceneTex
    bgfx::FrameBufferHandle sceneFB = BGFX_INVALID_HANDLE;
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
#else
        m_glarea = (win->offscreen != 0);
        if (!m_glarea) {
            fprintf(stderr, "[tetris] Linux windowed (non-GL-area) mode is not supported\n");
            return false;
        }
        // eglAreaSurface may legitimately be NULL: on Wayland the GL area's
        // context is surfaceless (GDK swaps the window surface itself in
        // end_frame), and the UI layer re-binds it surfaceless before the
        // blit.
        if (win->eglDisplay == nullptr || win->eglContext == nullptr ||
            win->sceneTex == 0) {
            fprintf(stderr, "[tetris] GL area mode: the UI layer did not provide the EGL resources\n");
            return false;
        }
        m_eglDisplay  = (EGLDisplay)win->eglDisplay;
        m_eglContext  = (EGLContext)win->eglContext;
        // NULL = surfaceless (EGL_NO_SURFACE): legal for a
        // EGL_KHR_surfaceless_context, and what bgfx adopts as its (unused)
        // surface.
        m_eglPbuffer  = (EGLSurface)win->eglPbuffer;
        m_eglAreaSurf = (EGLSurface)win->eglAreaSurface;
        m_sceneTex    = win->sceneTex;

        // Single-threaded bgfx: latching bgfx::renderFrame() from this thread
        // (the UI thread that will drive bgfx::frame() inside the GL area's
        // render callback) keeps bgfx from spawning its own render thread, so
        // bgfx::frame() performs the GPU submit inline on the caller.
        bgfx::renderFrame();

        // bgfx adopts the UI layer's shared pbuffer context; it must be
        // current at bgfx::init time.
        if (!eglMakeCurrent(m_eglDisplay, m_eglPbuffer, m_eglPbuffer, m_eglContext)) {
            fprintf(stderr, "[tetris] cannot make the shared EGL context current (error 0x%x)\n", eglGetError());
            return false;
        }

        init.type = bgfx::RendererType::OpenGL;   // the EGL backend
        init.fallback = false;
        init.platformData.nwh = nullptr;
        init.platformData.ndt = nullptr;
        init.platformData.context = win->eglContext;
        // No backbuffer is ever used: the scene renders into sceneTex and the
        // pbuffer is never presented (bgfx's flip() stays a no-op).
        init.resolution.width = 1;
        init.resolution.height = 1;
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
#if !BX_PLATFORM_OSX
        if (m_glarea) {
            // The scene texture storage is sceneTex (shared; bgfx never
            // deletes it); only the bgfx handles go away here.
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
#if !BX_PLATFORM_OSX
        if (m_glarea) {
            // Make the shared context current: the resize path below does raw
            // GL work (overrideInternal) and bgfx's submit expects it too.
            eglMakeCurrent(m_eglDisplay, m_eglPbuffer, m_eglPbuffer, m_eglContext);

            if (pw != lastW || ph != lastH) {
                // The UI layer re-created sceneTex at the new size before this
                // call. Recreate the bgfx RT/FBO over it: the RT is created
                // with the external texture id, so bgfx adopts sceneTex's
                // storage (BGFX_SAMPLER_INTERNAL_SHARED — it never deletes it)
                // and renders directly into the shared texture.
                if (bgfx::isValid(sceneFB)) { bgfx::destroy(sceneFB); sceneFB = BGFX_INVALID_HANDLE; }
                if (bgfx::isValid(sceneRT)) { bgfx::destroy(sceneRT); sceneRT = BGFX_INVALID_HANDLE; }
                sceneRT = bgfx::createTexture2D(pw, ph, false, 1,
                                                bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT,
                                                nullptr, (uint64_t)m_sceneTex);
                sceneFB = bgfx::createFrameBuffer(1, &sceneRT);
                lastW = pw;
                lastH = ph;
            }
        }
#endif
        verts.clear();
        buildScene(verts, s);
        if (verts.empty()) return false;

        const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(Vert)));
        bgfx::update(vbuf, 0, mem);

        float view[16], proj[16];
        mtxIdentity(view);
        computeOrtho(proj, pw, ph);
        bgfx::setViewTransform(0, view, proj);
        bgfx::setViewRect(0, 0, 0, (uint16_t)pw, (uint16_t)ph);
#if !BX_PLATFORM_OSX
        if (m_glarea) {
            // Render the scene into the shared texture. No depth attachment,
            // so clear color only (the scene is 2D and never uses depth).
            bgfx::setViewFrameBuffer(0, sceneFB);
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x101018ff, 1.0f, 0);
        } else
#endif
        {
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101018ff, 1.0f, 0);
        }

        bgfx::setTexture(0, uTex, atlas);
        bgfx::setVertexBuffer(0, vbuf);
        // Blending composites the text glyphs (straight alpha from the atlas)
        // over the scene; opaque geometry (a=1) is unaffected.
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(0, program);
        return true;
    }

    void endFrame() {
        if (!ok) return;
        bgfx::frame();
#if !BX_PLATFORM_OSX
        if (m_glarea) {
            // bgfx::frame() (single-threaded) just issued the scene render
            // into sceneTex on the shared context's stream. Flush it —
            // glFinish() flushes the FBO — so the UI thread's blit, a
            // different context and command stream in the same share group,
            // never samples an in-flight write.
            eglMakeCurrent(m_eglDisplay, m_eglPbuffer, m_eglPbuffer, m_eglContext);
            glFinish();
        }
#endif
    }

    void syncPresent() {
        if (!ok) return;
#if !BX_PLATFORM_OSX
        if (m_glarea) return;  // the UI layer's blit is the present
#endif
        bgfx::frame();
        bgfx::frame();
    }

    void requestScreenShot(const char* path) {
        fprintf(stderr, "[dbg] requestScreenShot(%s) ok=%d glarea=%d\n", path, (int)ok, (int)m_glarea);
        if (!ok) return;
#if !BX_PLATFORM_OSX
        if (m_glarea) {
            // bgfx::requestScreenShot only works on *window* frame buffers;
            // sceneFB is a texture FBO. Read the shared scene texture back
            // directly instead (C2 is current and flushed by endFrame()).
            eglMakeCurrent(m_eglDisplay, m_eglPbuffer, m_eglPbuffer, m_eglContext);
            glBindTexture(GL_TEXTURE_2D, m_sceneTex);
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            glBindTexture(GL_TEXTURE_2D, 0);
            fprintf(stderr, "[dbg] requestScreenShot: %dx%d err=0x%x glerr=0x%x\n",
                    w, h, eglGetError(), glGetError());
            if (w > 0 && h > 0) {
                // glReadPixels reads the read framebuffer, not a texture:
                // attach sceneTex to a temporary FBO.
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, m_sceneTex, 0);
                std::vector<uint8_t> px((size_t)w * (size_t)h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                // glReadPixels row 0 is the texture bottom; BMP row 0 is the
                // bottom too — no flip.
                writeBmp(path, (uint32_t)w, (uint32_t)h, (uint32_t)(w * 4),
                         px.data(), false, false);
            }
            return;
        }
#endif
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path);
    }

    // y0 = bottom, y1 = top (world y grows up). v grows down in the texture.
    void addQuad(std::vector<Vert>& v, float x0, float y0, float x1, float y1,
                 const UVRect& uv, float r, float g, float b) {
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

    void drawNext(std::vector<Vert>& v, const TetrisBackend::Cell cells[4], float bx, float by) {
        addQuad(v, bx, by, bx + 4, by + 4, solidUV(), kEmptyCell.r, kEmptyCell.g, kEmptyCell.b);
        for (int i = 0; i < 4; i++) {
            int dx = cells[i].x, dy = cells[i].y;
            int t = cells[i].color;
            float x0 = bx + dx, x1 = bx + dx + 1;
            float yBot = by + (3 - dy), yTop = by + (4 - dy);
            RGB c = kColor[t];
            addQuad(v, x0, yBot, x1, yTop, solidUV(), c.r, c.g, c.b);
        }
    }

    void drawOverlay(std::vector<Vert>& v, const std::string& head, const std::string& sub) {
        addQuad(v, BOARD_X0, BOARD_Y0, BOARD_X0 + W, BOARD_Y0 + H,
                solidUV(), kOverlayDim.r, kOverlayDim.g, kOverlayDim.b);
        float cx = BOARD_X0 + W / 2.0f, cy = BOARD_Y0 + H / 2.0f;
        drawText(v, cx - head.size() * 0.6f, cy + 1.0f, head, kOverlayHead, 1.2f);
        drawText(v, cx - sub.size() * 0.4f, cy - 1.0f, sub, kOverlaySub, 0.8f);
    }

    void buildScene(std::vector<Vert>& v, const TetrisBackend::Snapshot& s) {
        // Board border ring (drawn behind the cells)
        addQuad(v, BOARD_X0 - 0.3f, BOARD_Y0 - 0.3f, BOARD_X0 + W + 0.3f, BOARD_Y0 + H + 0.3f,
                solidUV(), kBorder.r, kBorder.g, kBorder.b);

        // Board cells (grid y=0 is the top row)
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int val = s.grid[y][x];
                float x0 = BOARD_X0 + x, x1 = BOARD_X0 + x + 1;
                float yTop = BOARD_Y0 + (H - y), yBot = BOARD_Y0 + (H - 1 - y);
                RGB c = (val == EMPTY) ? kEmptyCell : kColor[val];
                addQuad(v, x0, yBot, x1, yTop, solidUV(), c.r, c.g, c.b);
            }
        }

        // Ghost + current piece
        if (s.pieceColor >= 0) {
            for (int i = 0; i < 4; i++) {
                int gx = s.ghost[i].x, gyy = s.ghost[i].y;
                if (gyy >= 0 && gyy < H && gx >= 0 && gx < W && s.grid[gyy][gx] == EMPTY) {
                    RGB c = kColorDim[s.ghost[i].color];
                    addQuad(v, BOARD_X0 + gx, BOARD_Y0 + (H - 1 - gyy),
                            BOARD_X0 + gx + 1, BOARD_Y0 + (H - gyy),
                            solidUV(), c.r, c.g, c.b);
                }
            }
            for (int i = 0; i < 4; i++) {
                int px = s.piece[i].x, py = s.piece[i].y;
                if (py >= 0 && py < H && px >= 0 && px < W) {
                    RGB c = kColor[s.piece[i].color];
                    addQuad(v, BOARD_X0 + px, BOARD_Y0 + (H - 1 - py),
                            BOARD_X0 + px + 1, BOARD_Y0 + (H - py),
                            solidUV(), c.r, c.g, c.b);
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
        drawNext(v, s.next, PANEL_X + 1, 6.5f);
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
        if (s.gameOver) drawOverlay(v, "GAME OVER", "r restart  -  q quit");
        else if (s.paused) drawOverlay(v, "PAUSED", "press p to resume");
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
