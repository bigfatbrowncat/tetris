// ============================================================================
//  Tetris — bgfx (Metal) + Cocoa window, macOS
//  ---------------------------------------------------------------------------
//  Rendering via bgfx (Metal backend). Native Cocoa window for input/resize.
//
//  Build: see Makefile
//  Run:   ./tetris
//
//  Threading:
//    - Main (Cocoa) thread: pumps Cocoa events only (never bgfx::renderFrame)
//    - Game thread: bgfx::init + game loop (bgfx::frame) + bgfx::shutdown
//    - bgfx owns its own internal render thread that drives bgfx::renderFrame
//
//  Controls:
//    Arrow keys / A D : move left / right
//    Arrow down / S   : soft drop
//    Arrow up / W / X : rotate clockwise
//    Z                : rotate counter-clockwise
//    Space            : hard drop
//    P                : pause
//    R                : restart
//    Q / Esc          : quit
// ============================================================================

#include "window.h"
#include "font.h"
#include "shaders/vs_quad.h"
#include "shaders/fs_quad.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace tetris;

// ----------------------------------------------------------------------------
// Constants and piece data
// ----------------------------------------------------------------------------
static const int W = 10;   // board width  (cells)
static const int H = 20;   // board height (cells)
static const int EMPTY = -1;

enum Piece { I = 0, O = 1, T = 2, S = 3, Z = 4, J = 5, L = 6, NUM_PIECES = 7 };

// CELLS[pi][rot][cell] = {dx, dy} (relative to the piece origin, y grows down)
static const int CELLS[NUM_PIECES][4][4][2] = {
    // I
    { { {0,1},{1,1},{2,1},{3,1} },
      { {2,0},{2,1},{2,2},{2,3} },
      { {0,2},{1,2},{2,2},{3,2} },
      { {1,0},{1,1},{1,2},{1,3} } },
    // O
    { { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} },
      { {0,0},{1,0},{0,1},{1,1} } },
    // T
    { { {1,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{2,1},{1,2} },
      { {0,1},{1,1},{2,1},{1,2} },
      { {1,0},{0,1},{1,1},{1,2} } },
    // S
    { { {1,0},{2,0},{0,1},{1,1} },
      { {1,0},{1,1},{2,1},{2,2} },
      { {1,1},{2,1},{0,2},{1,2} },
      { {0,0},{0,1},{1,1},{1,2} } },
    // Z
    { { {0,0},{1,0},{1,1},{2,1} },
      { {2,0},{1,1},{2,1},{1,2} },
      { {0,1},{1,1},{1,2},{2,2} },
      { {1,0},{0,1},{1,1},{0,2} } },
    // J
    { { {0,0},{0,1},{1,1},{2,1} },
      { {1,0},{2,0},{1,1},{1,2} },
      { {0,1},{1,1},{2,1},{2,2} },
      { {1,0},{1,1},{0,2},{1,2} } },
    // L
    { { {2,0},{0,1},{1,1},{2,1} },
      { {1,0},{1,1},{1,2},{2,2} },
      { {0,1},{1,1},{2,1},{0,2} },
      { {0,0},{1,0},{1,1},{1,2} } },
};

struct RGB { float r, g, b; };
static const RGB kColor[NUM_PIECES] = {
    {0.20f, 0.90f, 1.00f},  // I cyan
    {1.00f, 0.95f, 0.20f},  // O yellow
    {0.85f, 0.30f, 1.00f},  // T purple
    {0.30f, 1.00f, 0.40f},  // S green
    {1.00f, 0.35f, 0.35f},  // Z red
    {0.30f, 0.40f, 1.00f},  // J blue
    {1.00f, 0.60f, 0.25f},  // L orange
};
static const RGB kColorDim[NUM_PIECES] = {
    {0.10f, 0.45f, 0.50f},
    {0.50f, 0.48f, 0.10f},
    {0.42f, 0.15f, 0.50f},
    {0.15f, 0.50f, 0.20f},
    {0.50f, 0.17f, 0.17f},
    {0.15f, 0.20f, 0.50f},
    {0.50f, 0.30f, 0.12f},
};
static const RGB kEmptyCell  {0.10f, 0.10f, 0.13f};
static const RGB kBorder     {0.70f, 0.72f, 0.78f};
static const RGB kLabel      {0.55f, 0.60f, 0.70f};
static const RGB kValue      {0.92f, 0.94f, 1.00f};
static const RGB kTitle      {1.00f, 0.85f, 0.30f};
static const RGB kControls   {0.45f, 0.48f, 0.56f};
static const RGB kOverlayDim {0.05f, 0.05f, 0.07f};
static const RGB kOverlayHead{1.00f, 0.30f, 0.30f};
static const RGB kOverlaySub {0.75f, 0.78f, 0.85f};

// World layout (1 world unit = 1 board cell). y grows up.
static const float WORLD_X0 = 0.0f, WORLD_Y0 = 0.0f, WORLD_X1 = 26.0f, WORLD_Y1 = 26.0f;
static const float BOARD_X0 = 12.0f, BOARD_Y0 = 3.0f;   // board left / bottom
static const float PANEL_X  = 2.0f;

// ----------------------------------------------------------------------------
// Matrix helpers (column-major 4x4, matching bx::mtxOrtho / bx::mtxIdentity)
// ----------------------------------------------------------------------------
static void mtxIdentity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mtxOrtho(float* m, float l, float r, float b, float t,
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

// ----------------------------------------------------------------------------
// Game state and logic (ported from the terminal version)
// ----------------------------------------------------------------------------
struct Game {
    std::vector<std::vector<int>> grid;   // grid[y][x] = EMPTY or piece type
    int curType = 0, curRot = 0, curX = 0, curY = 0;
    std::deque<int> queue;                // upcoming pieces (bag)
    std::vector<int> bag;
    std::mt19937 rng;
    long score = 0, highScore = 0;
    int level = 0, lines = 0;
    bool gameOver = false, paused = false;
    bool wantGravityReset = false;

    Game() : grid(H, std::vector<int>(W, EMPTY)), rng(std::random_device{}()) {
        loadHigh();
        reset();
    }

    void loadHigh() { std::ifstream f("tetris_high.txt"); if (f) f >> highScore; }
    void saveHigh() { std::ofstream f("tetris_high.txt"); if (f) f << highScore; }
    void bumpHigh() { if (score > highScore) highScore = score; }

    void reset() {
        grid.assign(H, std::vector<int>(W, EMPTY));
        bag.clear(); queue.clear(); refillQueue();
        score = 0; level = 0; lines = 0;
        gameOver = false; paused = false; wantGravityReset = false;
        spawn();
    }

    void refillQueue() {
        while ((int)queue.size() < 5) {
            if (bag.empty()) {
                bag = {0, 1, 2, 3, 4, 5, 6};
                std::shuffle(bag.begin(), bag.end(), rng);
            }
            queue.push_back(bag.back());
            bag.pop_back();
        }
    }

    void spawn() {
        refillQueue();
        curType = queue.front(); queue.pop_front();
        curRot = 0;
        curY = 0;
        curX = (curType == I) ? 3 : 4;
        if (collides(curX, curY, curType, curRot)) gameOver = true;
    }

    bool collides(int px, int py, int type, int rot) const {
        for (int i = 0; i < 4; i++) {
            int bx = px + CELLS[type][rot][i][0];
            int by = py + CELLS[type][rot][i][1];
            if (bx < 0 || bx >= W || by >= H) return true;
            if (by >= 0 && grid[by][bx] != EMPTY) return true;
        }
        return false;
    }

    bool move(int dx, int dy) {
        if (collides(curX + dx, curY + dy, curType, curRot)) return false;
        curX += dx; curY += dy;
        return true;
    }

    bool rotate(int dir) {
        int nr = (curRot + dir + 4) % 4;
        static const int kicks[] = {0, -1, 1, -2, 2};
        for (int k : kicks) {
            if (!collides(curX + k, curY, curType, nr)) {
                curX += k; curRot = nr;
                return true;
            }
        }
        return false;
    }

    int ghostY() const {
        int gy = curY;
        while (!collides(curX, gy + 1, curType, curRot)) gy++;
        return gy;
    }

    void hardDrop() {
        int dist = 0;
        while (move(0, 1)) dist++;
        score += dist * 2;
        bumpHigh();
        lockPiece();
    }

    void lockPiece() {
        bool topOut = false;
        for (int i = 0; i < 4; i++) {
            int bx = curX + CELLS[curType][curRot][i][0];
            int by = curY + CELLS[curType][curRot][i][1];
            if (by < 0) { topOut = true; continue; }
            if (by >= 0 && by < H && bx >= 0 && bx < W) grid[by][bx] = curType;
        }
        if (topOut) { gameOver = true; bumpHigh(); return; }

        int cleared = clearLines();
        if (cleared > 0) {
            static const int pts[5] = {0, 100, 300, 500, 800};
            score += pts[cleared] * (level + 1);
            lines += cleared;
            level = lines / 10;
            bumpHigh();
        }
        spawn();
    }

    int clearLines() {
        std::vector<std::vector<int>> kept;
        int cleared = 0;
        for (int y = 0; y < H; y++) {
            bool full = true;
            for (int x = 0; x < W; x++) if (grid[y][x] == EMPTY) { full = false; break; }
            if (full) cleared++;
            else kept.push_back(grid[y]);
        }
        while ((int)kept.size() < H) kept.insert(kept.begin(), std::vector<int>(W, EMPTY));
        grid = kept;
        return cleared;
    }

    int gravityInterval() const {  // ms per row, speeds up with level
        static const int table[] = {800,720,630,550,470,380,300,220,130,100,
                                    80, 70, 60, 50, 40, 30, 20, 15, 10, 10};
        return table[std::min(level, 19)];
    }

    void gravityStep() {
        if (gameOver || paused) return;
        if (!move(0, 1)) lockPiece();
    }

    void handleGameKey(int k) {
        switch (k) {
            case KEY_LEFT:  move(-1, 0); break;
            case KEY_RIGHT: move(1, 0);  break;
            case KEY_DOWN:
                if (move(0, 1)) { score += 1; bumpHigh(); wantGravityReset = true; }
                break;
            case KEY_CW:    rotate(1);  break;
            case KEY_CCW:   rotate(-1); break;
            case KEY_DROP:  hardDrop(); wantGravityReset = true; break;
            default: break;
        }
    }

    int nextType() const { return queue.empty() ? 0 : queue.front(); }
};

// ----------------------------------------------------------------------------
// Screenshot: bgfx hands us 4-byte BGRA pixels; we write a 24-bit BMP.
// ----------------------------------------------------------------------------
static void writeBmp(const char* path, uint32_t w, uint32_t h, uint32_t pitch,
                     const uint8_t* bgra, bool yflip) {
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
        const uint8_t* src = bgra + (size_t)srcRow * pitch;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src + (size_t)x * 4;  // B G R A
            row[x * 3 + 0] = p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[2];
        }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    fprintf(stderr, "[tetris] saved screenshot %ux%u -> %s\n", w, h, path);
}

struct ShotCallback : bgfx::CallbackI {
    bool shotWritten = false;
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
                    bgfx::TextureFormat::Enum, const void* _data, uint32_t, bool _yflip) override {
        writeBmp(_filePath, _w, _h, _pitch, (const uint8_t*)_data, _yflip);
        shotWritten = true;
    }
};

static ShotCallback g_shotImpl;

// ----------------------------------------------------------------------------
// Renderer (bgfx, single dynamic vertex buffer, one draw call)
// ----------------------------------------------------------------------------
struct Vert { float x, y, u, v, r, g, b; };

struct Renderer {
    bgfx::DynamicVertexBufferHandle vbuf = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uTex = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;
    std::vector<Vert> verts;
    uint32_t lastW = 0, lastH = 0;
    bool ok = false;
    const bgfx::Caps* caps = nullptr;

    void init(const void* nwh) {
        bgfx::Init init;
        init.type = bgfx::RendererType::Count;   // auto-select (Metal on macOS)
        init.fallback = true;
        init.platformData.nwh = (void*)nwh;
        init.platformData.ndt = nullptr;
        init.platformData.type = bgfx::NativeWindowHandleType::Default;
        init.resolution.width = 640;
        init.resolution.height = 640;
        init.resolution.reset = BGFX_RESET_VSYNC;
        init.callback = &g_shotImpl;
        if (!bgfx::init(init)) {
            fprintf(stderr, "[tetris] bgfx init failed\n");
            return;
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
    }

    void shutdown() {
        if (!ok) return;
        if (bgfx::isValid(vbuf)) bgfx::destroy(vbuf);
        if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
        if (bgfx::isValid(uTex)) bgfx::destroy(uTex);
        if (bgfx::isValid(program)) bgfx::destroy(program);
        bgfx::shutdown();
        ok = false;
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

    void drawNext(std::vector<Vert>& v, int t, float bx, float by) {
        addQuad(v, bx, by, bx + 4, by + 4, solidUV(), kEmptyCell.r, kEmptyCell.g, kEmptyCell.b);
        for (int i = 0; i < 4; i++) {
            int dx = CELLS[t][0][i][0], dy = CELLS[t][0][i][1];
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

    void buildScene(std::vector<Vert>& v, const Game& game) {
        // Board border ring (drawn behind the cells)
        addQuad(v, BOARD_X0 - 0.3f, BOARD_Y0 - 0.3f, BOARD_X0 + W + 0.3f, BOARD_Y0 + H + 0.3f,
                solidUV(), kBorder.r, kBorder.g, kBorder.b);

        // Board cells (grid y=0 is the top row)
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int val = game.grid[y][x];
                float x0 = BOARD_X0 + x, x1 = BOARD_X0 + x + 1;
                float yTop = BOARD_Y0 + (H - y), yBot = BOARD_Y0 + (H - 1 - y);
                RGB c = (val == EMPTY) ? kEmptyCell : kColor[val];
                addQuad(v, x0, yBot, x1, yTop, solidUV(), c.r, c.g, c.b);
            }
        }

        // Ghost + current piece
        if (!game.gameOver) {
            int gy = game.ghostY();
            for (int i = 0; i < 4; i++) {
                int gx = game.curX + CELLS[game.curType][game.curRot][i][0];
                int gyy = gy + CELLS[game.curType][game.curRot][i][1];
                if (gyy >= 0 && gyy < H && gx >= 0 && gx < W && game.grid[gyy][gx] == EMPTY) {
                    RGB c = kColorDim[game.curType];
                    addQuad(v, BOARD_X0 + gx, BOARD_Y0 + (H - 1 - gyy),
                            BOARD_X0 + gx + 1, BOARD_Y0 + (H - gyy),
                            solidUV(), c.r, c.g, c.b);
                }
            }
            for (int i = 0; i < 4; i++) {
                int px = game.curX + CELLS[game.curType][game.curRot][i][0];
                int py = game.curY + CELLS[game.curType][game.curRot][i][1];
                if (py >= 0 && py < H && px >= 0 && px < W) {
                    RGB c = kColor[game.curType];
                    addQuad(v, BOARD_X0 + px, BOARD_Y0 + (H - 1 - py),
                            BOARD_X0 + px + 1, BOARD_Y0 + (H - py),
                            solidUV(), c.r, c.g, c.b);
                }
            }
        }

        // Info panel
        drawText(v, PANEL_X, 22.0f, "SCORE", kLabel);
        drawText(v, PANEL_X, 20.5f, std::to_string(game.score), kValue);
        drawText(v, PANEL_X, 18.5f, "LEVEL", kLabel);
        drawText(v, PANEL_X, 17.0f, std::to_string(game.level), kValue);
        drawText(v, PANEL_X, 15.0f, "LINES", kLabel);
        drawText(v, PANEL_X, 13.5f, std::to_string(game.lines), kValue);
        drawText(v, PANEL_X, 11.5f, "NEXT", kLabel);
        drawNext(v, game.nextType(), PANEL_X + 1, 6.5f);
        drawText(v, PANEL_X, 4.5f, "HIGH", kLabel);
        drawText(v, PANEL_X, 3.0f, std::to_string(game.highScore), kValue);

        // Title
        const std::string title = "TETRIS";
        drawText(v, BOARD_X0 + W / 2.0f - title.size() * 0.7f, 24.0f, title, kTitle, 1.4f);

        // Controls
        const std::string c1 = "arrows/wasd move   up/x rot   z ccw";
        const std::string c2 = "space drop   p pause   r restart   q quit";
        drawText(v, 7.0f, 1.6f, c1, kControls, 0.55f);
        drawText(v, 7.0f, 0.9f, c2, kControls, 0.55f);

        // Overlays
        if (game.gameOver) drawOverlay(v, "GAME OVER", "r restart  -  q quit");
        else if (game.paused) drawOverlay(v, "PAUSED", "press p to resume");
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

    void frame(const Game& game, uint32_t pw, uint32_t ph) {
        if (!ok || pw == 0 || ph == 0) return;
        if (pw != lastW || ph != lastH) {
            bgfx::reset(pw, ph, BGFX_RESET_VSYNC);
            lastW = pw; lastH = ph;
        }
        verts.clear();
        buildScene(verts, game);
        if (verts.empty()) return;

        const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(Vert)));
        bgfx::update(vbuf, 0, mem);

        float view[16], proj[16];
        mtxIdentity(view);
        computeOrtho(proj, pw, ph);
        bgfx::setViewTransform(0, view, proj);
        bgfx::setViewRect(0, 0, 0, (uint16_t)pw, (uint16_t)ph);
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101018ff, 1.0f, 0);

        bgfx::setTexture(0, uTex, atlas);
        bgfx::setVertexBuffer(0, vbuf);
        bgfx::setState(BGFX_STATE_WRITE_RGB);
        bgfx::submit(0, program);
    }
};

// ----------------------------------------------------------------------------
// Threading coordination:
//   main (Cocoa) thread: pumps Cocoa events only. It must NOT call
//     bgfx::renderFrame(): because bgfx::renderFrame() is not invoked before
//     bgfx::init(), bgfx creates and owns its own internal render thread
//     ("bgfx - renderer backend thread") that calls bgfx::renderFrame() in a
//     loop. Driving Context::renderFrame()/submit() from a second thread races
//     that internal thread on the Metal render pass / command buffer and
//     crashes inside RendererContextMtl::submit().
//   game (API) thread: bgfx::init(), game loop with bgfx::frame(),
//     bgfx::shutdown(). bgfx's internal render thread performs the submit.
// ----------------------------------------------------------------------------
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_gameDone{false};

static void gameMain(const void* nwh) {
    Renderer renderer;
    renderer.init(nwh);
    Game game;

    using clock = std::chrono::steady_clock;
    using ms = std::chrono::milliseconds;
    auto now = clock::now();
    auto nextGravity = now + ms(game.gravityInterval());

    int smokeFrames = 0;
    if (const char* e = ::getenv("TETRIS_SMOKE_TEST")) smokeFrames = atoi(e);
    if (smokeFrames < 0) smokeFrames = 0;
    if (smokeFrames) fprintf(stderr, "[tetris] smoke test: %d frames\n", smokeFrames);

    int shotFrame = 0;
    if (const char* e = ::getenv("TETRIS_SCREENSHOT")) shotFrame = atoi(e);
    if (shotFrame <= 0 && smokeFrames > 4) shotFrame = smokeFrames - 3;  // default: capture near the end
    const char* shotPath = "tetris_shot.bmp";
    if (const char* e = ::getenv("TETRIS_SCREENSHOT_PATH")) shotPath = e;
    if (shotFrame > 0) fprintf(stderr, "[tetris] screenshot at frame %d -> %s\n", shotFrame, shotPath);

    int frameNo = 0;
    while (!g_quit.load()) {
        // Input
        int k;
        while ((k = cocoaPopKey()) != KEY_NONE) {
            if (k == KEY_QUIT) { g_quit.store(true); break; }
            if (k == KEY_RESTART) {
                game.reset();
                nextGravity = clock::now() + ms(game.gravityInterval());
                continue;
            }
            if (k == KEY_PAUSE && !game.gameOver) {
                game.paused = !game.paused;
                nextGravity = clock::now() + ms(game.gravityInterval());
                continue;
            }
            if (!game.gameOver && !game.paused) {
                game.handleGameKey(k);
                if (game.wantGravityReset) {
                    nextGravity = clock::now() + ms(game.gravityInterval());
                    game.wantGravityReset = false;
                }
            }
        }
        // Gravity
        while (!game.gameOver && !game.paused && clock::now() >= nextGravity) {
            game.gravityStep();
            nextGravity += ms(game.gravityInterval());
        }
        // Render
        uint32_t pw, ph; cocoaWindowSize(&pw, &ph);
        renderer.frame(game, pw, ph);
        bgfx::frame();
        ++frameNo;

        if (shotFrame > 0 && frameNo == shotFrame) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, shotPath);
        }

        if (smokeFrames > 0 && --smokeFrames == 0) g_quit.store(true);
        std::this_thread::sleep_for(ms(16));
    }

    game.saveHigh();
    // Tear down bgfx. bgfx::shutdown() signals its internal render thread to
    // exit and joins it, so after this returns no bgfx thread is left running.
    renderer.shutdown();
    g_gameDone.store(true);
    fprintf(stderr, "[tetris] done. high score: %ld\n", game.highScore);
}

int main() {
    cocoaInit();
    void* window = cocoaCreateWindow(640, 640, "Tetris");
    if (!window) {
        fprintf(stderr, "[tetris] failed to create window\n");
        return 1;
    }

    std::thread gameThread(gameMain, window);

    // Pump Cocoa events until the game thread has torn down bgfx (g_gameDone).
    // The actual rendering is driven by bgfx's own internal render thread -- we
    // must NOT call bgfx::renderFrame() here (see the threading notes above).
    while (!g_gameDone.load()) {
        cocoaPumpEvents(0.016);
        if (cocoaShouldQuit()) g_quit.store(true);
    }

    gameThread.join();
    cocoaShutdown();
    return 0;
}
