// Renderer implementation (bgfx): the common, platform-agnostic part — the
// scene construction, the vertex assembly, and the per-frame draw flow.
// The platform-specific parts (bgfx init for the platform's renderer type,
// the resize path, the frame handover, the present sync, the screenshot
// capture) live in the platform modules:
//   platform/cocoa/renderer.cpp   Metal
//   platform/gtk/renderer.cpp     headless Vulkan (offscreen, dma-buf export)
//   platform/win32/renderer.cpp   Direct3D 11 (composition)
#include "renderer.h"

#include "font.h"
#include "shaders/vs_quad.h"
#include "shaders/fs_quad.h"

#include <bgfx/bgfx.h>

#include <bimg/bimg.h>
#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/filepath.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace tetris::frontend {

namespace {

constexpr int W = TetrisBackend::kCols;
constexpr int H = TetrisBackend::kRows;
constexpr int EMPTY = -1;

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
const RGB kWhite      {1.00f, 1.00f, 1.00f};
const RGB kValue      {0.98f, 0.80f, 0.40f};  // gold (the artwork's value style)
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

// World layout. y grows up; the window center maps to world (13,13) with
// min(pw,ph)/26 px per world unit (computeOrtho).
const float WORLD_X0 = 0.0f, WORLD_Y0 = 0.0f, WORLD_X1 = 26.0f, WORLD_Y1 = 26.0f;

// field.png (the UI artwork) in image pixels (top-left origin, y down).
// The board aligns with the "glass" — the dark-brick background — and the
// HUD values sit in the artwork's right-side panels, so all rects are
// measured in the image (full-res pixel analysis).
constexpr uint32_t FIELD_W = 3300, FIELD_H = 3000;
const ImageRect kGlass     {1052,  282,  2305,  2634};  // dark-brick background
const ImageRect kScoreSlot {2545,  775,  3205,  975};   // SCORE value area
const ImageRect kLevelSlot {2560,  1240, 3190,  1400};  // LEVEL value area
const ImageRect kLinesSlot {2575,  1670, 3180,  1780};  // LINES value area
const ImageRect kNextSlot  {2590,  2140, 3155,  2495};  // NEXT preview area

// The seven brick textures (res/brick-<color>.png), indexed by piece id
// (the same color mapping as kColor): 0 I, 1 O, 2 T, 3 S, 4 Z, 5 J, 6 L.
const char* const kBrickFiles[7] = {
    "brick-teal.png",     // I
    "brick-yellow.png",   // O
    "brick-violet.png",   // T
    "brick-green.png",    // S
    "brick-red.png",      // Z
    "brick-blue.png",     // J
    "brick-orange.png",   // L
};

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
        // Screenshot: bgfx hands us 4-byte pixels (RGBA or BGRA depending on
        // backend); writeBmp writes a 24-bit BMP (B G R order).
        bool isBgra = _format == bgfx::TextureFormat::BGRA8;
        writeBmp(_filePath, _w, _h, _pitch, (const uint8_t*)_data, _yflip, isBgra);
    }
};

ShotCallback g_shotImpl;

}  // namespace

// Screenshot: bgfx hands us 4-byte pixels (RGBA or BGRA depending on
// backend); we write a 24-bit BMP (B G R order).
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

Renderer::Renderer() {}
Renderer::~Renderer() {}

bool Renderer::init(const UiWindow* win) {
    bgfx::Init init;
    init.callback = &g_shotImpl;
    if (!platformInit(win, init)) return false;
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
    // The field background quad (two triangles, like addQuad).
    fieldVbuf = bgfx::createDynamicVertexBuffer(6, layout, BGFX_BUFFER_ALLOW_RESIZE);
    loadFieldImage();
    loadBricks();
    ok = true;
    return true;
}

void Renderer::shutdown() {
    if (!ok) return;
    platformShutdown();
    if (bgfx::isValid(vbuf)) bgfx::destroy(vbuf);
    if (bgfx::isValid(fieldVbuf)) bgfx::destroy(fieldVbuf);
    if (bgfx::isValid(fieldTex)) bgfx::destroy(fieldTex);
    if (bgfx::isValid(brickVbuf)) bgfx::destroy(brickVbuf);
    if (bgfx::isValid(brickTex)) bgfx::destroy(brickTex);
    if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
    if (bgfx::isValid(uTex)) bgfx::destroy(uTex);
    if (bgfx::isValid(program)) bgfx::destroy(program);
    bgfx::shutdown();
    ok = false;
}

bool Renderer::render(const TetrisBackend::Snapshot& s, uint32_t pw, uint32_t ph) {
    if (!ok || pw == 0 || ph == 0) return false;
    if (pw != lastW || ph != lastH) {
        if (!platformResize(pw, ph)) return false;
        lastW = pw;
        lastH = ph;
    }
    const Layout L = computeLayout(pw, ph);
    m_verts.clear();
    m_brickVerts.clear();
    // Per-side inflation for the solid/brick quads (world units). The scene
    // spans min(pw,ph) pixels over 26 world units (computeOrtho), so
    // kSolidGrowPx of screen space is kSolidGrowPx/scale world units.
    const float scale = (float)(pw < ph ? pw : ph) / 26.0f;
    const float grow = scale > 0.0f ? kSolidGrowPx / scale : 0.0f;
    buildScene(L, s, grow);

    float view[16], proj[16];
    mtxIdentity(view);
    computeOrtho(proj, pw, ph);
    bgfx::setViewTransform(0, view, proj);
    bgfx::setViewRect(0, 0, 0, (uint16_t)pw, (uint16_t)ph);
    platformPrepareView();
    // Blending composites the text glyphs (straight alpha from the atlas)
    // and the brick gems (alpha from the texture) over the scene; opaque
    // geometry (a=1) is unaffected.
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);

    bool submitted = false;
    // 1) Field background first (it is below everything).
    if (bgfx::isValid(fieldTex)) {
        std::vector<Vert> fv;
        drawField(fv, pw, ph);
        const bgfx::Memory* fmem = bgfx::copy(fv.data(), (uint32_t)(fv.size() * sizeof(Vert)));
        bgfx::update(fieldVbuf, 0, fmem);
        bgfx::setTexture(0, uTex, fieldTex);
        bgfx::setVertexBuffer(0, fieldVbuf, 0, (uint32_t)fv.size());
        bgfx::submit(0, program);
        submitted = true;
    }
    // 2) Brick gems: locked cells, ghost, current piece, NEXT preview.
    if (m_bricksOk && !m_brickVerts.empty()) {
        const bgfx::Memory* bmem =
            bgfx::copy(m_brickVerts.data(), (uint32_t)(m_brickVerts.size() * sizeof(Vert)));
        bgfx::update(brickVbuf, 0, bmem);
        bgfx::setTexture(0, uTex, brickTex);
        bgfx::setVertexBuffer(0, brickVbuf, 0, (uint32_t)m_brickVerts.size());
        bgfx::submit(0, program);
        submitted = true;
    }
    // 3) Text/HUD (font atlas). When the brick images are missing the brick
    //    cells land here too (solid quads from the atlas's white strip).
    if (!m_verts.empty()) {
        const bgfx::Memory* mem =
            bgfx::copy(m_verts.data(), (uint32_t)(m_verts.size() * sizeof(Vert)));
        bgfx::update(vbuf, 0, mem);
        bgfx::setTexture(0, uTex, atlas);
        // Draw exactly the vertices we uploaded this frame. Without the
        // count, setVertexBuffer draws the whole dynamic buffer (8192
        // verts), so any stale tail left by a smaller/larger previous
        // upload (or garbage latched during a fast resize) is re-rendered
        // over the scene every frame — the static cyan/blue bands seen
        // after resizing.
        bgfx::setVertexBuffer(0, vbuf, 0, (uint32_t)m_verts.size());
        bgfx::submit(0, program);
        submitted = true;
    }
    return submitted;
}

void Renderer::endFrame() {
    if (!ok) return;
    bgfx::frame();
    platformPresentFrame();
}

void Renderer::syncPresent() {
    if (!ok) return;
    platformSyncPresent();
}

void Renderer::requestScreenShot(const char* path) {
    if (!ok) return;
    platformRequestScreenShot(path);
}

// y0 = bottom, y1 = top (world y grows up). v grows down in the texture.
// grow (world units, per side) inflates the quad so adjacent solid quads
// overlap by 2*grow in screen space — see kSolidGrowPx. Only pass it for
// solid quads (the uniform white strip makes the stretch invisible).
void Renderer::addQuad(std::vector<Vert>& v, float x0, float y0, float x1, float y1,
                       const UVRect& uv, float r, float g, float b, float grow) {
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

void Renderer::drawText(std::vector<Vert>& v, float x, float y, const std::string& s,
                        const RGB& c, float cell) {
    for (char ch : s) {
        char cc = ch;
        if (cc >= 'a' && cc <= 'z') cc = (char)(cc - 'a' + 'A');
        if (cc < 32 || cc > 126) cc = 32;
        addQuad(v, x, y, x + cell, y + cell, glyphUV(cc), c.r, c.g, c.b);
        x += cell;
    }
}

void Renderer::drawBrickCell(float x0, float y0, float x1, float y1, int t,
                             const RGB& c, float grow) {
    if (m_bricksOk) {
        addQuad(m_brickVerts, x0, y0, x1, y1, brickUV(t), c.r, c.g, c.b, grow);
    } else {
        // No brick images: solid-color quad from the font atlas's white strip.
        addQuad(m_verts, x0, y0, x1, y1, solidUV(), c.r, c.g, c.b, grow);
    }
}

void Renderer::drawNext(const TetrisBackend::Cell cells[4], float bx, float by,
                        float cell, float grow) {
    for (int i = 0; i < 4; i++) {
        int dx = cells[i].x, dy = cells[i].y;
        float x0 = bx + dx * cell, x1 = bx + (dx + 1) * cell;
        float yBot = by + (3 - dy) * cell, yTop = by + (4 - dy) * cell;
        RGB c = m_bricksOk ? kWhite : kColor[cells[i].color];
        drawBrickCell(x0, yBot, x1, yTop, cells[i].color, c, grow);
    }
}

void Renderer::drawValue(std::vector<Vert>& v, const RectF& slot, int value, int maxDigits) {
    const std::string text = std::to_string(value);
    const float sw = slot.x1 - slot.x0, sh = slot.y1 - slot.y0;
    // Cell: half the panel's height, but never wider than the panel for the
    // worst-case digit count (so a long score shrinks instead of overflowing).
    const float cell = std::min(sh * 0.5f, sw * 0.9f / (float)maxDigits);
    const float w = (float)text.size() * cell;
    drawText(v, (slot.x0 + slot.x1) * 0.5f - w * 0.5f,
             (slot.y0 + slot.y1) * 0.5f - cell * 0.5f, text, kValue, cell);
}

void Renderer::drawOverlay(std::vector<Vert>& v, const Layout& L, const std::string& head,
                           const std::string& sub, float grow) {
    addQuad(v, L.board.x0, L.board.y0, L.board.x1, L.board.y1,
            solidUV(), kOverlayDim.r, kOverlayDim.g, kOverlayDim.b, grow);
    const float cx = (L.board.x0 + L.board.x1) * 0.5f;
    const float cy = (L.board.y0 + L.board.y1) * 0.5f;
    const float cw = (L.board.x1 - L.board.x0) / (float)W;
    const float ch = (L.board.y1 - L.board.y0) / (float)H;
    drawText(v, cx - (float)head.size() * 0.55f * cw, cy + ch, head, kOverlayHead, 1.1f * cw);
    drawText(v, cx - (float)sub.size() * 0.5f * cw, cy - ch, sub, kOverlaySub, 0.55f * cw);
}

void Renderer::buildScene(const Layout& L, const TetrisBackend::Snapshot& s, float grow) {
    // Per-axis cell size: the glass is not exactly 1:2 (its aspect ~0.53 vs
    // the board's 0.5), so the gems are slightly wider than tall — accepted
    // in favor of the grid filling the glass exactly.
    const float cw = (L.board.x1 - L.board.x0) / (float)W;
    const float ch = (L.board.y1 - L.board.y0) / (float)H;
    // The world rect of board cell (x, y); grid y=0 is the top row.
    auto cellRect = [&](int x, int y) {
        return RectF{L.board.x0 + x * cw, L.board.y0 + (H - 1 - y) * ch,
                     L.board.x0 + (x + 1) * cw, L.board.y0 + (H - y) * ch};
    };

    // Locked cells as brick gems; empty cells stay transparent so the
    // glass bricks of the artwork show through. Inflated (kSolidGrowPx) so
    // adjacent gems never open a rasterization crack.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int val = s.grid[y][x];
            if (val == EMPTY) continue;
            RectF r = cellRect(x, y);
            RGB c = m_bricksOk ? kWhite : kColor[val];
            drawBrickCell(r.x0, r.y0, r.x1, r.y1, val, c, grow);
        }
    }

    // Ghost (darkened gem) first, current piece (full-color gem) over it.
    if (s.pieceColor >= 0) {
        for (int i = 0; i < 4; i++) {
            int gx = s.ghost[i].x, gy = s.ghost[i].y;
            if (gy >= 0 && gy < H && gx >= 0 && gx < W && s.grid[gy][gx] == EMPTY) {
                RectF r = cellRect(gx, gy);
                RGB c = kColorDim[s.ghost[i].color];
                drawBrickCell(r.x0, r.y0, r.x1, r.y1, s.ghost[i].color, c, grow);
            }
        }
        for (int i = 0; i < 4; i++) {
            int px = s.piece[i].x, py = s.piece[i].y;
            if (py >= 0 && py < H && px >= 0 && px < W) {
                RectF r = cellRect(px, py);
                RGB c = m_bricksOk ? kWhite : kColor[s.piece[i].color];
                drawBrickCell(r.x0, r.y0, r.x1, r.y1, s.piece[i].color, c, grow);
            }
        }
    }

    // HUD values: gold text centered in the artwork's right-side panels
    // (the labels are part of the artwork itself).
    drawValue(m_verts, L.score, s.score, 9);
    drawValue(m_verts, L.level, s.level, 3);
    drawValue(m_verts, L.lines, s.lines, 4);

    // NEXT preview: a 4x4 box of gems centered in the NEXT panel.
    const float nextW = L.next.x1 - L.next.x0, nextH = L.next.y1 - L.next.y0;
    const float ncell = std::min(nextW, nextH) * 0.8f / 4.0f;
    drawNext(s.next, (L.next.x0 + L.next.x1) * 0.5f - 2.0f * ncell,
             (L.next.y0 + L.next.y1) * 0.5f - 2.0f * ncell, ncell, grow);

    // Overlays (dim the board rect only).
    if (s.gameOver) drawOverlay(m_verts, L, "GAME OVER", "r restart  -  q quit", grow);
    else if (s.paused) drawOverlay(m_verts, L, "PAUSED", "press p to resume", grow);
}

UVRect Renderer::brickUV(int t) const {
    const int col = t % 4, row = t / 4;  // 4x2 grid, row-major, index = piece id
    return { (float)col / 4.0f, (float)row / 2.0f,
             (float)(col + 1) / 4.0f, (float)(row + 1) / 2.0f };
}

Layout Renderer::computeLayout(uint32_t pw, uint32_t ph) const {
    Layout L;
    const float pxPerUnit = (float)(pw < ph ? pw : ph) / 26.0f;
    const float cx = (WORLD_X0 + WORLD_X1) * 0.5f, cy = (WORLD_Y0 + WORLD_Y1) * 0.5f;
    auto wx = [&](float sx) { return cx + (sx - (float)pw * 0.5f) / pxPerUnit; };
    auto wy = [&](float sy) { return cy - (sy - (float)ph * 0.5f) / pxPerUnit; };

    if (fieldW == 0) {
        // No artwork: the old fixed layout (10x20 board at (12,3); value
        // slots where the left info panel used to be).
        L.board = {12.0f, 3.0f, 22.0f, 23.0f};
        L.score = {2.0f, 20.2f, 12.0f, 21.8f};
        L.level = {2.0f, 16.6f, 12.0f, 18.2f};
        L.lines = {2.0f, 13.2f, 12.0f, 14.8f};
        L.next = {3.0f, 6.0f, 7.0f, 10.0f};
        return L;
    }

    // The artwork's screen rect (contain-fit + centered, as drawField).
    const float imgAspect = (float)fieldW / (float)fieldH;
    const float winAspect = (float)pw / (float)ph;
    float dw, dh;
    if (winAspect > imgAspect) { dh = (float)ph; dw = (float)ph * imgAspect; }
    else { dw = (float)pw; dh = (float)pw / imgAspect; }
    const float sx0 = ((float)pw - dw) * 0.5f;
    const float sy0 = ((float)ph - dh) * 0.5f;
    // An image rect (top-left origin, y down) to a world rect (y up).
    auto mapRect = [&](const ImageRect& r) {
        const float x0 = wx(sx0 + (float)r.x0 * (dw / (float)FIELD_W));
        const float x1 = wx(sx0 + (float)r.x1 * (dw / (float)FIELD_W));
        const float yTop = wy(sy0 + (float)r.y0 * (dh / (float)FIELD_H));
        const float yBot = wy(sy0 + (float)r.y1 * (dh / (float)FIELD_H));
        return RectF{x0, yBot, x1, yTop};
    };
    L.board = mapRect(kGlass);
    L.score = mapRect(kScoreSlot);
    L.level = mapRect(kLevelSlot);
    L.lines = mapRect(kLinesSlot);
    L.next = mapRect(kNextSlot);
    return L;
}

void Renderer::loadBricks() {
    // Decode the seven brick textures and pack them into one 4x2 atlas
    // (index = piece id, row-major). All images must be the same size.
    bx::FilePath exe(bx::Dir::Executable);
    const bx::StringView dir = exe.getPath();
    const std::string exeDir = std::string(dir.getPtr(), (size_t)dir.getLength());

    std::vector<std::vector<uint8_t>> px(7);
    uint32_t bw = 0, bh = 0;
    for (int i = 0; i < 7; i++) {
        const std::string candidates[] = {
            exeDir + std::string("res/") + kBrickFiles[i],
            std::string("res/") + kBrickFiles[i],
        };
        bool got = false;
        for (const std::string& path : candidates) {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) continue;
            std::vector<uint8_t> data;
            uint8_t chunk[8192];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
                data.insert(data.end(), chunk, chunk + n);
            fclose(f);
            if (data.empty()) continue;

            bx::DefaultAllocator alloc;
            bx::Error err;
            bimg::ImageContainer* img =
                bimg::imageParse(&alloc, data.data(), (uint32_t)data.size(),
                                 bimg::TextureFormat::RGBA8, &err);
            if (!img) {
                fprintf(stderr, "[tetris] cannot decode %s: %s\n", path.c_str(),
                        err.getMessage().getCPtr());
                continue;
            }
            if (bw == 0) { bw = img->m_width; bh = img->m_height; }
            if (img->m_width != bw || img->m_height != bh ||
                img->m_size != (uint32_t)bw * bh * 4) {
                fprintf(stderr, "[tetris] brick %s is %ux%u, expected %ux%u; "
                                "using solid colors for pieces\n",
                        kBrickFiles[i], img->m_width, img->m_height, bw, bh);
                bimg::imageFree(img);
                return;
            }
            const uint8_t* d = (const uint8_t*)img->m_data;
            px[i].assign(d, d + img->m_size);
            bimg::imageFree(img);
            got = true;
            break;
        }
        if (!got) {
            fprintf(stderr, "[tetris] brick %s not found (looked next to the executable "
                            "and in the working directory); using solid colors for pieces\n",
                    kBrickFiles[i]);
            return;
        }
    }

    // Pack the 4x2 atlas (RGBA8, top row first; v=0 is the top).
    const uint32_t aw = bw * 4, ah = bh * 2;
    std::vector<uint8_t> atlasData((size_t)aw * ah * 4, 0);
    for (int i = 0; i < 7; i++) {
        const int col = i % 4, row = i / 4;
        for (uint32_t y = 0; y < bh; y++) {
            uint8_t* dst =
                atlasData.data() + (((size_t)(row * bh + y) * aw + col * bw) * 4);
            const uint8_t* src = px[i].data() + (size_t)y * bw * 4;
            memcpy(dst, src, (size_t)bw * 4);
        }
    }
    const bgfx::Memory* mem = bgfx::copy(atlasData.data(), (uint32_t)atlasData.size());
    // No flags = linear min/mag filtering (the bgfx default).
    brickTex = bgfx::createTexture2D((uint16_t)aw, (uint16_t)ah, false, 1,
                                     bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, mem);
    if (!bgfx::isValid(brickTex)) {
        fprintf(stderr, "[tetris] brick atlas creation failed; using solid colors for pieces\n");
        return;
    }
    brickW = bw;
    brickH = bh;
    brickVbuf = bgfx::createDynamicVertexBuffer(8192, layout, BGFX_BUFFER_ALLOW_RESIZE);
    m_bricksOk = true;
    fprintf(stderr, "[tetris] brick atlas %ux%u (%ux%u cells, 7 bricks)\n", aw, ah, bw, bh);
}

void Renderer::computeOrtho(float* proj, uint32_t pw, uint32_t ph) const {
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

void Renderer::loadFieldImage() {
    // The res folder is copied next to the executable; look for the image
    // there, then in the working directory (for a source-tree run).
    bx::FilePath exe(bx::Dir::Executable);
    const bx::StringView dir = exe.getPath();
    const std::string exeDir = std::string(dir.getPtr(), (size_t)dir.getLength());
    const std::string candidates[] = {
        exeDir + "res/field.png",
        "res/field.png",
    };
    for (const std::string& path : candidates) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> data;
        uint8_t chunk[8192];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            data.insert(data.end(), chunk, chunk + n);
        fclose(f);
        if (data.empty()) continue;

        bx::DefaultAllocator alloc;
        bx::Error err;
        bimg::ImageContainer* img =
            bimg::imageParse(&alloc, data.data(), (uint32_t)data.size(),
                             bimg::TextureFormat::RGBA8, &err);
        if (!img) {
            fprintf(stderr, "[tetris] cannot decode %s: %s\n", path.c_str(),
                    err.getMessage().getCPtr());
            continue;
        }
        const bgfx::Memory* mem = bgfx::copy(img->m_data, img->m_size);
        // No flags = linear min/mag filtering (the bgfx default), which is
        // what we want: the image is scaled down to window size.
        const uint64_t sflags = BGFX_TEXTURE_NONE;
        fieldTex = bgfx::createTexture2D((uint16_t)img->m_width, (uint16_t)img->m_height,
                                         false, 1, bgfx::TextureFormat::RGBA8, sflags, mem);
        if (bgfx::isValid(fieldTex)) {
            fieldW = img->m_width;
            fieldH = img->m_height;
            fprintf(stderr, "[tetris] field image %ux%u from %s\n",
                    fieldW, fieldH, path.c_str());
            bimg::imageFree(img);
            return;
        }
        fprintf(stderr, "[tetris] field texture creation failed for %s\n", path.c_str());
        bimg::imageFree(img);
    }
    fprintf(stderr, "[tetris] field image not found (looked next to the executable "
                    "and in the working directory); rendering without it\n");
}

void Renderer::drawField(std::vector<Vert>& v, uint32_t pw, uint32_t ph) {
    // Contain-fit: the image keeps its aspect and touches left+right or
    // top+bottom, centered in the window. The ortho projection maps the 26x26
    // world (center (13,13)) to the window with min(pw,ph)/26 px per world
    // unit, so the image's screen-space half-extents become world
    // half-extents by dividing by that scale.
    const float imgAspect = (float)fieldW / (float)fieldH;
    const float winAspect = (float)pw / (float)ph;
    float dw, dh;  // image size in window pixels
    if (winAspect > imgAspect) { dh = (float)ph; dw = (float)ph * imgAspect; }
    else { dw = (float)pw; dh = (float)pw / imgAspect; }
    const float pxPerUnit = (float)(pw < ph ? pw : ph) / 26.0f;
    const float hw = dw * 0.5f / pxPerUnit, hh = dh * 0.5f / pxPerUnit;
    const float cx = (WORLD_X0 + WORLD_X1) * 0.5f;
    const float cy = (WORLD_Y0 + WORLD_Y1) * 0.5f;
    const UVRect full{0.0f, 0.0f, 1.0f, 1.0f};
    addQuad(v, cx - hw, cy - hh, cx + hw, cy + hh, full, 1.0f, 1.0f, 1.0f);
}

}  // namespace tetris::frontend
