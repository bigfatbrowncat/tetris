// ============================================================================
//  Windows implementation of the platform-neutral UI layer (ui/window.h).
//  ---------------------------------------------------------------------------
//  A never-flickering, freely resizable Direct3D 11 window via
//  DirectComposition — a port of the "noflicker_directx_window" proof of
//  concept (the only non-flickering resizable D3D11 window existing for
//  Windows today): its window class, swap-chain configuration, and
//  per-resize present sequence are preserved.
//
//  Presentation (POC-faithful, D3D11 logic only): bgfx renders the game into
//  a full-window scene texture owned by this layer (RGBA8, render target +
//  shader resource; bgfx adopts it through platformData.backBuffer = the RTV
//  over it — it borrows the pointer and never releases it), and this layer
//  blits that texture to the composition swap chain's back buffer with a
//  full-screen quad — the POC's DrawTriangle, "one texture scaled to the
//  full frame" (per-frame RTV over GetBuffer(0), clear, draw, release after
//  the present). The swap chain is 100% owned by this layer, so the POC's
//  exact resize present sequence is preserved, surrounded by two presents
//  per frame cycle so that NO flip is ever in flight when the buffers are
//  resized (the POC's never-flicker precondition):
//    normal frame — Present(1, 0): a vsync-paced flip that UPDATES the
//                   displayed content (the game animates between resizes).
//                   It leaves at most one flip in flight;
//    resize — before ResizeBuffers, a "drain" present: the current scene
//                   (the last rendered frame — re-blitting it, so no
//                   regression to an older frame) is presented with the
//                   POC's own pair: the Intel output's vblank waited first
//                   (POC), then Present(0, DXGI_PRESENT_RESTART) — which
//                   DISCARDS the in-flight flip of the previous Normal
//                   present — then a blocking
//                   Present(1, DXGI_PRESENT_DO_NOT_SEQUENCE): when the
//                   buffers are resized, no present is outstanding (an
//                   in-flight flip left across ResizeBuffers can land as a
//                   glitch frame — flicker);
//    resize (WM_NCCALCSIZE), before returning —
//                   Present(0, DXGI_PRESENT_RESTART) (discard outstanding
//                   queued presents, queue the new-size frame ASAP) +
//                   Present(1, DXGI_PRESENT_DO_NOT_SEQUENCE) (block until
//                   the new-size frame has actually scanned out), with the
//                   Intel output's vblank waited in between when the system
//                   has an Intel GPU (POC: "immediate" rendering on Intel is
//                   not actually immediate).
//  The window has no GDI redirection surface (WS_EX_NOREDIRECTIONBITMAP) —
//  what you see is the composition swap chain (FLIP_SEQUENTIAL, 2 back
//  buffers, B8G8R8A8, AlphaMode IGNORE) bound to the window through a
//  composition target/visual, created between the window's construction and
//  its ShowWindow (as in the POC). On resize: the drain present (above) has
//  fully displayed the in-flight flip, the pipeline state is cleared,
//  ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_
//  GDI_COMPATIBLE) (safe — no flip is in flight and no back-buffer ref is
//  live), the scene texture is (re)created,
//  one frame at the new size is rendered + blitted, and the two presents
//  land the window frame and its content in the same compositor update.
//  Dragging the border resizes continuously, without flicker.
//
//  Threading (bgfx): the game runs on this (UI) thread. The renderer latches
//  single-threaded bgfx (bgfx::renderFrame before init) and renders into the
//  scene texture; uiPumpEvents drives one game frame per displayed frame,
//  paced by the vsync present. All D3D11 calls — bgfx's rendering, the
//  blit, ResizeBuffers, Present — happen on one thread, the POC's
//  single-threaded discipline; there is no cross-thread D3D in this module.
//  (The blit's draw is GPU-ordered after bgfx's draws of the same frame:
//  same immediate context.)
//
//  Lifetime: the UI layer owns the factory, the device, the immediate
//  context, the swap chain, the scene texture (+SRV/RTV) and the blit
//  pipeline. bgfx AddRefs the device and borrows the scene RTV pointer (it
//  re-reads it from platformData on every reset and never releases it);
//  bgfx creates and owns its own D24S8 depth texture (backBufferDS is NULL).
//  uiShutdown() runs after bgfx::shutdown(), so no GPU work is in flight
//  when the resources go away.
// ============================================================================

#include "ui/window.h"

#ifndef WINVER
#define WINVER 0x0603
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kClassName = L"TetrisNoFlickerWindow";
constexpr UINT kResizeTestTimer = 1;
// POC: INTEL_VENDOR_ID.
constexpr UINT kIntelVendorId = 0x8086;

// POC: TextureVertex (GraphicContents.h) — position + texture coordinate.
struct BlitVertex {
    float x, y, z, tx, ty;
};

// POC: FullScreenImageGraphicContents::getVertices — a full-frame
// TRIANGLESTRIP in NDC, uv (0,0) top-left. The POC's k is 1 (the 760/width
// scaling is commented out in the POC), so the quad is size-independent and
// the vertex buffer is created once, at init.
const BlitVertex kBlitVerts[6] = {
    {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f},
    { 1.0f,  1.0f, 0.0f, 1.0f, 0.0f},
    { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
    {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
    {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f},
};

// POC: the full-screen image shader, verbatim (DrawTriangle compiles it
// with D3DCompile2 at ps_4_0/vs_4_0, D3DCOMPILE_DEBUG).
const char* kBlitShaderSrc =
    "Texture2D txDiffuse : register( t0 );\n"
    "SamplerState samLinear : register( s0 );\n"
    "\n"
    "struct VSInput {\n"
    "\tfloat4 position : POSITION;\n"
    "\tfloat2 Tex  : TEXCOORD0;\n"
    "};\n"
    "struct PSInput {\n"
    "\tfloat4 position : SV_POSITION;\n"
    "\tfloat2 Tex  : TEXCOORD0;\n"
    "};\n"
    "PSInput VSMain(VSInput input) {\n"
    "\tPSInput output;\n"
    "\toutput.position = input.position;\n"
    "\toutput.Tex = input.Tex;\n"
    "\treturn output;\n"
    "}\n"
    "float4 PSMain(PSInput input) : SV_TARGET {\n"
    "\t return txDiffuse.Sample( samLinear, input.Tex );\n"
    "}\n";

// POC: the blit's clear color (DrawTriangle); the quad covers the whole
// frame, so it is only insurance against an undefined back buffer.
const float kClearColor[4] = {0.0f, 0.2f, 0.4f, 1.0f};

struct Win {
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;

    // DXGI / D3D11 (owned by this layer; see the file header for the
    // division of ownership with bgfx). Factory2: CreateSwapChainForComposition
    // is an IDXGIFactory2 method (it inherits the factory1 enum used below).
    IDXGIFactory2* factory = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* dctx = nullptr;
    IDXGISwapChain1* swapChain = nullptr;  // composition swap chain
    uint32_t scW = 0, scH = 0;             // current swap chain (back buffer) size

    // The full-window scene texture (this layer owns the storage): bgfx
    // renders the game into it (sceneRtv, which bgfx borrows and never
    // releases), the blit samples it (sceneSrv) and scales it to the full
    // frame. (Re)created on every resize (uiWindowResize).
    ID3D11Texture2D* sceneTex = nullptr;
    ID3D11ShaderResourceView* sceneSrv = nullptr;
    ID3D11RenderTargetView* sceneRtv = nullptr;

    // The blit pipeline — the POC's DrawTriangle resources, created once at
    // init (the POC recreates them on every resize; they are
    // size-independent).
    ID3D11Buffer* blitVb = nullptr;
    ID3D11InputLayout* blitLayout = nullptr;
    ID3D11VertexShader* blitVs = nullptr;
    ID3D11PixelShader* blitPs = nullptr;
    ID3D11SamplerState* blitSampler = nullptr;

    // DirectComposition binding: window -> target -> visual -> swap chain.
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;

    // Intel workaround (POC): "immediate" rendering on Intel is not actually
    // immediate — a manual WaitForVBlank between the render and the present
    // of a resize frame avoids flicker on Intel displays.
    IDXGIAdapter* intelAdapter = nullptr;
    IDXGIOutput* intelOutput = nullptr;
    std::vector<IDXGIAdapter*> adapters;

    // Window state.
    uint32_t pxW = 0, pxH = 0;      // current client size (device pixels)
    bool shown = false;             // ShowWindow is deferred to the first pump
    bool presentedOnce = false;     // a frame has been presented
    bool frameDrivenThisPump = false;  // a resize frame was driven during the drain
    bool quit = false;

    // Input.
    std::mutex keyMutex;
    std::deque<int> keys;

    // Frame driver (the game), called once per displayed frame from this
    // thread (uiPumpEvents / WM_NCCALCSIZE).
    UiFrameDriver frameDriver = nullptr;
    void* frameDriverUser = nullptr;
    UiFrameSyncCallback frameSyncCb = nullptr;  // stored, never called (see ui/window.h)
    void* frameSyncUser = nullptr;

    // Frame timing (dt for the game).
    LARGE_INTEGER perfFreq = {};
    LARGE_INTEGER lastFrameTick = {};

    bool d3dDead = false;           // device lost: stop rendering / presenting
    int resizeTestStep = -1;        // TETRIS_RESIZE_TEST (>= 0 while active)
};

Win g;
UiWindow g_uiWindow = {UI_NWH_DEFAULT, nullptr};

std::wstring toWide(const char* s) {
    if (s == nullptr || *s == '\0') return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n - 1);
    return w;
}

// Physical key codes (layout-independent, like macOS's keyCodes); mirrors
// mapKey() on the other platforms.
int mapKey(WPARAM vk) {
    switch (vk) {
        case VK_LEFT:  case 'A':  return KEY_LEFT;
        case VK_RIGHT: case 'D':  return KEY_RIGHT;
        case VK_DOWN:  case 'S':  return KEY_DOWN;
        case VK_UP:    case 'W':  case 'X':  return KEY_CW;
        case 'Z':                    return KEY_CCW;
        case VK_SPACE:               return KEY_DROP;
        case 'P':                    return KEY_PAUSE;
        case 'R':                    return KEY_RESTART;
        case 'Q':    case VK_ESCAPE: return KEY_QUIT;
        default:                     return KEY_NONE;
    }
}

double frameDt() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = 0.0;
    if (g.perfFreq.QuadPart > 0 && g.lastFrameTick.QuadPart != 0)
        dt = (double)(now.QuadPart - g.lastFrameTick.QuadPart) / (double)g.perfFreq.QuadPart;
    g.lastFrameTick = now;
    return dt;
}

// --- Intel workaround: port of the POC's D3DContextBase logic -------------
// (POC: "If there is an Intel adapter in the system, we have to synchronize
//  with it manually, because we can face flickering instead. No idea, why,
//  but 'immediate' rendering on Intel GPU is still not immediate.")

void lookForIntelOutput() {
    if (g.intelOutput != nullptr) { g.intelOutput->Release(); g.intelOutput = nullptr; }
    if (g.intelAdapter != nullptr) {
        IDXGIOutput* output = nullptr;
        for (UINT i = 0; g.intelAdapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor != nullptr) {
                g.intelOutput = output;  // keep the ref (POC: the first output with a monitor)
                break;
            }
            output->Release();
        }
    }
}

// POC: called between the draw and the swap chain Present of a resize frame.
void syncIntelOutput() {
    if (g.intelOutput != nullptr) g.intelOutput->WaitForVBlank();
}

// Present/resize error policy (POC's checkDeviceRemoved): a removed/reset
// device is logged and rendering stops (recovery would mean recreating the
// device + swap chain; the process is exiting anyway via the quit path);
// DXGI_STATUS_OCCLUDED (the window is fully covered) is normal and ignored;
// anything else is logged.
bool checkPresentHr(const char* what, HRESULT hr) {
    if (hr == DXGI_STATUS_OCCLUDED) return true;
    if (SUCCEEDED(hr)) return true;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        fprintf(stderr, "[tetris] %s: device lost (0x%08X)\n", what, (unsigned)hr);
        g.d3dDead = true;
        return false;
    }
    fprintf(stderr, "[tetris] %s failed: 0x%08X\n", what, (unsigned)hr);
    return false;
}

// --- DirectComposition ------------------------------------------------------

// The DCompContext creation/destruction is fundamentally asymmetric (POC):
// the resources are cleaned up in WM_DESTROY, but the binding is created
// between the construction of the window and showing it.
bool dcompBind(HWND hwnd) {
    HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g.dcomp));
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] DCompositionCreateDevice failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.dcomp->CreateTargetForHwnd(hwnd, FALSE, &g.target);
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] CreateTargetForHwnd failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.dcomp->CreateVisual(&g.visual);
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] CreateVisual failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.target->SetRoot(g.visual);
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] SetRoot failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.visual->SetContent(g.swapChain);
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] SetContent failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.dcomp->Commit();
    if (FAILED(hr)) {
        fprintf(stderr, "[tetris] DComp Commit failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    return true;
}

void dcompUnbind() {
    if (g.visual != nullptr) { g.visual->Release(); g.visual = nullptr; }
    if (g.target != nullptr) { g.target->Release(); g.target = nullptr; }
    if (g.dcomp != nullptr) { g.dcomp->Release(); g.dcomp = nullptr; }
}

// --- D3D11 setup (POC's D3DContextBase + D3DContext constructors) ----------

// The blit pipeline — the POC's DrawTriangle, with the per-frame resource
// creation hoisted to init: the POC re-compiles the shaders and re-creates
// the vertex buffer on every resize; the HLSL and the vertices are
// size-independent, so they are created once. (The presentation logic —
// per-frame back-buffer RTV, clear, draw, present sequence — stays
// POC-exact in blitToSwapChain().)
bool createBlitPipeline() {
    ID3DBlob* ps = nullptr, *psErr = nullptr;
    ID3DBlob* vs = nullptr, *vsErr = nullptr;
    bool ok = false;
    HRESULT hr;

    do {
        // POC: the pixel shader is compiled first.
        hr = D3DCompile2(kBlitShaderSrc, std::strlen(kBlitShaderSrc), nullptr, nullptr, nullptr,
                         "PSMain", "ps_4_0", D3DCOMPILE_DEBUG, 0, 0, nullptr, 0, &ps, &psErr);
        if (FAILED(hr) || ps == nullptr) {
            fprintf(stderr, "[tetris] blit pixel shader compile failed: 0x%08X%s%s\n",
                    (unsigned)hr,
                    psErr != nullptr ? ": " : "",
                    psErr != nullptr ? (const char*)psErr->GetBufferPointer() : "");
            break;
        }
        hr = g.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.blitPs);
        if (FAILED(hr) || g.blitPs == nullptr) {
            fprintf(stderr, "[tetris] CreatePixelShader failed: 0x%08X\n", (unsigned)hr);
            break;
        }

        hr = D3DCompile2(kBlitShaderSrc, std::strlen(kBlitShaderSrc), nullptr, nullptr, nullptr,
                         "VSMain", "vs_4_0", D3DCOMPILE_DEBUG, 0, 0, nullptr, 0, &vs, &vsErr);
        if (FAILED(hr) || vs == nullptr) {
            fprintf(stderr, "[tetris] blit vertex shader compile failed: 0x%08X%s%s\n",
                    (unsigned)hr,
                    vsErr != nullptr ? ": " : "",
                    vsErr != nullptr ? (const char*)vsErr->GetBufferPointer() : "");
            break;
        }
        hr = g.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.blitVs);
        if (FAILED(hr) || g.blitVs == nullptr) {
            fprintf(stderr, "[tetris] CreateVertexShader failed: 0x%08X\n", (unsigned)hr);
            break;
        }

        // POC: the input layout (POSITION @ 0, TEXCOORD @ 12 — BlitVertex's layout).
        const D3D11_INPUT_ELEMENT_DESC elems[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = g.device->CreateInputLayout(elems, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &g.blitLayout);
        if (FAILED(hr) || g.blitLayout == nullptr) {
            fprintf(stderr, "[tetris] CreateInputLayout failed: 0x%08X\n", (unsigned)hr);
            break;
        }

        // POC: the full-screen quad vertex buffer.
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)sizeof(kBlitVerts);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.StructureByteStride = sizeof(BlitVertex);
        D3D11_SUBRESOURCE_DATA svd = {};
        svd.pSysMem = kBlitVerts;
        hr = g.device->CreateBuffer(&bd, &svd, &g.blitVb);
        if (FAILED(hr) || g.blitVb == nullptr) {
            fprintf(stderr, "[tetris] blit vertex buffer creation failed: 0x%08X\n", (unsigned)hr);
            break;
        }

        // The POC's shader declares `SamplerState samLinear : register(s0)`
        // but never binds a sampler object (it relies on the slot default);
        // bind an explicit linear sampler so the intent is unambiguous.
        D3D11_SAMPLER_DESC ssd = {};
        ssd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ssd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        ssd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ssd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ssd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = g.device->CreateSamplerState(&ssd, &g.blitSampler);
        if (FAILED(hr) || g.blitSampler == nullptr) {
            fprintf(stderr, "[tetris] CreateSamplerState failed: 0x%08X\n", (unsigned)hr);
            break;
        }

        ok = true;
    } while (false);

    if (ps != nullptr) ps->Release();
    if (psErr != nullptr) psErr->Release();
    if (vs != nullptr) vs->Release();
    if (vsErr != nullptr) vsErr->Release();
    if (!ok) {
        if (g.blitVb != nullptr) { g.blitVb->Release(); g.blitVb = nullptr; }
        if (g.blitLayout != nullptr) { g.blitLayout->Release(); g.blitLayout = nullptr; }
        if (g.blitVs != nullptr) { g.blitVs->Release(); g.blitVs = nullptr; }
        if (g.blitPs != nullptr) { g.blitPs->Release(); g.blitPs = nullptr; }
        if (g.blitSampler != nullptr) { g.blitSampler->Release(); g.blitSampler = nullptr; }
    }
    return ok;
}

// (Re)create the full-window scene texture — the surface bgfx renders the
// game into, and the blit's source:
//   sceneRtv — bgfx's back buffer (platformData.backBuffer); bgfx borrows
//              the pointer and never releases it;
//   sceneSrv — the blit's source (t0).
// The UI layer owns the storage; the texture is (re)created on every resize
// (uiWindowResize) and at init. R8G8B8A8_UNORM: bgfx's TextureFormat::RGBA8
// on D3D11 (the s_textureFormat table in renderer_d3d11.cpp) — the format
// bgfx renders the scene in.
bool createSceneTexture(uint32_t w, uint32_t h) {
    if (g.sceneRtv != nullptr) { g.sceneRtv->Release(); g.sceneRtv = nullptr; }
    if (g.sceneSrv != nullptr) { g.sceneSrv->Release(); g.sceneSrv = nullptr; }
    if (g.sceneTex != nullptr) { g.sceneTex->Release(); g.sceneTex = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g.device->CreateTexture2D(&td, nullptr, &g.sceneTex);
    if (FAILED(hr) || g.sceneTex == nullptr) {
        fprintf(stderr, "[tetris] scene texture creation failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    hr = g.device->CreateShaderResourceView(g.sceneTex, &svd, &g.sceneSrv);
    if (FAILED(hr) || g.sceneSrv == nullptr) {
        fprintf(stderr, "[tetris] scene SRV creation failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = g.device->CreateRenderTargetView(g.sceneTex, nullptr, &g.sceneRtv);
    if (FAILED(hr) || g.sceneRtv == nullptr) {
        fprintf(stderr, "[tetris] scene RTV creation failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    return true;
}

bool createD3D(uint32_t w, uint32_t h) {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory));
    if (FAILED(hr) || g.factory == nullptr) {
        fprintf(stderr, "[tetris] CreateDXGIFactory2 failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    // Look for the adapters (and specifically an Intel GPU) in the system
    // (POC: stop enumerating once Intel is found).
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; g.factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc = {};
        if (FAILED(adapter->GetDesc(&desc))) { adapter->Release(); continue; }
        g.adapters.push_back(adapter);
        if (desc.VendorId == kIntelVendorId) {
            g.intelAdapter = adapter;
            adapter = nullptr;
            break;
        }
    }
    // POC: the constructor's first reposition() calls lookForIntelOutput —
    // do the same so the vblank sync is active from the first frame.
    lookForIntelOutput();

    // Create the D3D device (POC: hardware driver, BGRA support, the
    // default feature level list).
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &g.device, nullptr, &g.dctx);
    if (FAILED(hr) || g.device == nullptr || g.dctx == nullptr) {
        fprintf(stderr, "[tetris] D3D11CreateDevice failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    // Create the swap chain — the POC's configuration, verbatim:
    //   FLIP_SEQUENTIAL + 2 buffers — the back buffer's contents survive
    //   until presented (no flicker; the screenshot readback sees the frame
    //   about to be presented);
    //   B8G8R8A8 + AlphaMode IGNORE — an opaque composition window;
    //   (the POC creates it at 1x1 and resizes from the first WM_NCCALCSIZE;
    //   we create it at the window's real client size — the same first
    //   frame, without the detour).
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = g.factory->CreateSwapChainForComposition(g.device, &scd, nullptr, &g.swapChain);
    if (FAILED(hr) || g.swapChain == nullptr) {
        fprintf(stderr, "[tetris] CreateSwapChainForComposition failed: 0x%08X\n", (unsigned)hr);
        return false;
    }
    g.scW = w; g.scH = h;

    if (!createBlitPipeline()) return false;
    return createSceneTexture(w, h);
}

// The three present sequences this layer uses (see the file header).
enum class PresentMode {
    // Present(1, 0): a vsync-paced flip that updates the displayed content.
    Normal,
    // The POC's resize pair: syncIntelOutput(), Present(0, RESTART) (discard
    // outstanding queued presents, queue a frame ASAP), release the
    // back-buffer refs, Present(1, DO_NOT_SEQUENCE) (block until the frame
    // has actually scanned out).
    Resize,
    // Run just before a resize's ResizeBuffers: the current scene (the last
    // rendered frame — re-blitting it, so no regression) is presented with
    // the SAME POC pair as Resize: the RESTART discards the in-flight flip
    // of the previous Normal present (a lone DO_NOT_SEQUENCE does not
    // discard it — the flip could then survive ResizeBuffers and land as a
    // glitch frame), and the blocking DO_NOT_SEQUENCE leaves no present
    // outstanding when the buffers are resized.
    Drain,
};

// Blit the scene texture to the swap chain's back buffer — the POC's
// DrawTriangle (a full-screen quad sampling the texture, "one texture scaled
// to the full frame") plus this layer's present (see PresentMode):
//   Normal — Present(1, 0): vsync-paced and — unlike a lone
//   DO_NOT_SEQUENCE on this composition swap chain — it actually UPDATES the
//   displayed content, so the game animates between resizes;
//   Resize — the POC's resize present sequence: syncIntelOutput(), then
//   Present(0, DXGI_PRESENT_RESTART) (discard outstanding queued presents
//   and queue the new-size frame ASAP), release the back-buffer refs, then
//   Present(1, DXGI_PRESENT_DO_NOT_SEQUENCE) (block until the new-size frame
//   has actually scanned out, so the window frame and its content land in
//   the same compositor update);
//   Drain — the POC pair applied to the just-blitted current scene (see
//   PresentMode::Drain): it discards the in-flight flip of the previous
//   Normal present and leaves no present outstanding when the caller
//   resizes the buffers (the POC's never-flicker precondition).
bool blitToSwapChain(PresentMode mode) {
    if (g.d3dDead || g.swapChain == nullptr || g.dctx == nullptr ||
        g.device == nullptr || g.sceneSrv == nullptr) {
        return false;
    }

    // POC: the full-screen image state (viewport, vertex buffer, shaders,
    // texture, input layout, topology).
    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = (float)g.scW;
    vp.Height = (float)g.scH;
    g.dctx->RSSetViewports(1u, &vp);
    const UINT stride = sizeof(BlitVertex);
    const UINT offset = 0;
    g.dctx->IASetVertexBuffers(0, 1, &g.blitVb, &stride, &offset);
    g.dctx->PSSetShader(g.blitPs, nullptr, 0);
    g.dctx->VSSetShader(g.blitVs, nullptr, 0);
    g.dctx->PSSetShaderResources(0, 1, &g.sceneSrv);
    g.dctx->PSSetSamplers(0, 1, &g.blitSampler);
    g.dctx->IASetInputLayout(g.blitLayout);
    g.dctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // POC: "After this point and before swapChain->Present(), we should
    // render as fast as possible."
    ID3D11Resource* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = g.swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr) || bb == nullptr) {
        return checkPresentHr("GetBuffer", hr);
    }
    hr = g.device->CreateRenderTargetView(bb, nullptr, &rtv);
    if (FAILED(hr) || rtv == nullptr) {
        bb->Release();
        return checkPresentHr("CreateRenderTargetView(blit)", hr);
    }

    g.dctx->ClearRenderTargetView(rtv, kClearColor);
    g.dctx->OMSetRenderTargets(1, &rtv, nullptr);  // POC: no depth for the blit
    g.dctx->Draw(6, 0);

    if (mode == PresentMode::Normal) {
        if (!checkPresentHr("Present", g.swapChain->Present(1, 0))) {
            bb->Release();
            rtv->Release();
            return false;
        }
    } else {
        // Resize and Drain both take the POC pair: sync with the Intel
        // output, then discard outstanding queued presents and queue the
        // frame ASAP. (For Drain the frame is the current scene at the OLD
        // size — the RESTART is what discards the in-flight flip of the
        // previous Normal present; a lone blocking present does not
        // discard it.)
        syncIntelOutput();
        if (!checkPresentHr(mode == PresentMode::Resize ? "Present(RESTART)" : "Present(drain RESTART)",
                            g.swapChain->Present(0, DXGI_PRESENT_RESTART))) {
            bb->Release();
            rtv->Release();
            return false;
        }
    }
    // POC: release the back buffer + its RTV after the (first) present —
    // the driver holds its own references until the present completes.
    bb->Release();
    rtv->Release();

    if (mode != PresentMode::Normal) {
        // POC: wait for a vblank to really make sure our frame is ready
        // before the window finishes resizing (Resize) / before the
        // caller's ResizeBuffers runs (Drain).
        return checkPresentHr(mode == PresentMode::Resize ? "Present(DO_NOT_SEQUENCE)" : "Present(drain DON)",
                              g.swapChain->Present(1, DXGI_PRESENT_DO_NOT_SEQUENCE));
    }
    return true;
}

// Release everything createD3D() created (and uiWindowResize() re-created).
// Order: the scene texture's views before the texture, the swap chain after
// (a last in-flight flip is waited out first, bounded — a flip completes by
// the next vsync), the device last — the POC's destructor order, plus the
// scene + blit resources.
void releaseD3D() {
    if (g.sceneRtv != nullptr) { g.sceneRtv->Release(); g.sceneRtv = nullptr; }
    if (g.sceneSrv != nullptr) { g.sceneSrv->Release(); g.sceneSrv = nullptr; }
    if (g.sceneTex != nullptr) { g.sceneTex->Release(); g.sceneTex = nullptr; }
    if (g.blitVb != nullptr) { g.blitVb->Release(); g.blitVb = nullptr; }
    if (g.blitLayout != nullptr) { g.blitLayout->Release(); g.blitLayout = nullptr; }
    if (g.blitVs != nullptr) { g.blitVs->Release(); g.blitVs = nullptr; }
    if (g.blitPs != nullptr) { g.blitPs->Release(); g.blitPs = nullptr; }
    if (g.blitSampler != nullptr) { g.blitSampler->Release(); g.blitSampler = nullptr; }
    if (g.swapChain != nullptr) { g.swapChain->Release(); g.swapChain = nullptr; }
    if (g.dctx != nullptr) { g.dctx->Release(); g.dctx = nullptr; }
    if (g.device != nullptr) { g.device->Release(); g.device = nullptr; }
    if (g.intelOutput != nullptr) { g.intelOutput->Release(); g.intelOutput = nullptr; }
    for (IDXGIAdapter* a : g.adapters) if (a != nullptr) a->Release();
    g.adapters.clear();
    g.intelAdapter = nullptr;
    if (g.factory != nullptr) { g.factory->Release(); g.factory = nullptr; }
    g.scW = 0; g.scH = 0;
    g.d3dDead = true;
}

// ---------------------------------------------------------------------------
//  WndProc
// ---------------------------------------------------------------------------

// TEMP-TEST (mirrors the GTK module): TETRIS_RESIZE_TEST grows the window
// 640->1240 in steps (like a drag), then maximizes (big grow) and restores
// (big shrink back) to exercise both directions.
void resizeTestTick(HWND hwnd) {
    int step = g.resizeTestStep;
    switch (step) {
        case 25:  ShowWindow(hwnd, SW_MAXIMIZE); break;  // big grow
        case 27:  ShowWindow(hwnd, SW_RESTORE); break;   // big shrink back
        default:
            if (step >= 0 && step <= 23) {
                const int size = 640 + step * 600 / 23;
                RECT rc = {};
                GetWindowRect(hwnd, &rc);
                SetWindowPos(hwnd, nullptr, rc.left, rc.top, size, size,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
    }
    if (++g.resizeTestStep > 28) {
        g.resizeTestStep = -1;
        KillTimer(hwnd, kResizeTestTimer);
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN: {
            int k = mapKey(wParam);
            if (k != KEY_NONE) {
                std::lock_guard<std::mutex> lk(g.keyMutex);
                g.keys.push_back(k);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wParam == kResizeTestTimer) resizeTestTick(hwnd);
            return 0;

        case WM_CLOSE:
            // Q / Esc / the window close button (Alt+F4): do not destroy the
            // window now — set the flag; the main loop performs a clean
            // shutdown (bgfx::shutdown before the D3D resources go away).
            g.quit = true;
            return 0;

        case WM_DPICHANGED: {
            // Per-Monitor V2: move/resize the window to the rect Windows
            // suggests (expressed in the old DPI's units); the re-scaled
            // size arrives as WM_NCCALCSIZE and takes the usual resize path.
            const RECT* suggested = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_NCCALCSIZE: {
            // Use the result of DefWindowProc's WM_NCCALCSIZE handler to get
            // the upcoming client rect (POC). Technically, when wParam is
            // TRUE, lParam points to NCCALCSIZE_PARAMS, but its first member
            // is a RECT with the same meaning as the one lParam points to
            // when wParam is FALSE.
            DefWindowProcW(hwnd, msg, wParam, lParam);
            RECT* rect = (RECT*)lParam;
            if (rect->right > rect->left && rect->bottom > rect->top) {
                const uint32_t w = (uint32_t)(rect->right - rect->left);
                const uint32_t h = (uint32_t)(rect->bottom - rect->top);
                g.pxW = w; g.pxH = h;
                if (w != g.scW || h != g.scH || !g.presentedOnce) {
                    // Never-flicker ordering (POC): the new-size frame is
                    // rendered and presented BEFORE this handler returns, so
                    // the window frame and its content land in the same
                    // compositor update. The frame driver (the game) detects
                    // the size change: uiWindowResize (the present wait,
                    // ResizeBuffers, the scene-texture re-creation) then
                    // bgfx renders the new frame into the scene texture;
                    // the blit below scales it to the full frame and
                    // completes the POC's two-present resize sequence.
                    // (This also fires during CreateWindow, before the D3D
                    // setup and before the driver exist — both guarded.)
                    if (g.frameDriver != nullptr) {
                        g.frameDriver(w, h, frameDt(), g.frameDriverUser);
                        g.frameDrivenThisPump = true;
                    }
                    if (blitToSwapChain(PresentMode::Resize)) {
                        g.presentedOnce = true;
                    }
                }
            }
            // We're never preserving the client area, so we always return 0.
            return 0;
        }

        case WM_DESTROY:
            // Destroy the DirectComposition context properly, so that the
            // window fades away beautifully (POC).
            dcompUnbind();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------
//  the UI layer API
// ---------------------------------------------------------------------------

void uiInit(void) {
    // Per-Monitor V2 DPI awareness: the window, the swap chain and the scene
    // texture are sized in real device pixels (no virtualized scaling).
    // SetProcessDpiAwarenessContext is Windows 10 1703+; fall back to the
    // process-wide SetProcessDPIAware on older systems.
    typedef BOOL (WINAPI* SetDpiCtxFn)(HANDLE);
    typedef BOOL (WINAPI* SetDpiAwareFn)(void);
    HMODULE self = GetModuleHandleW(nullptr);
    SetDpiCtxFn setCtx = self ? (SetDpiCtxFn)(void*)GetProcAddress(self, "SetProcessDpiAwarenessContext") : nullptr;
    if (setCtx != nullptr) {
        setCtx((HANDLE)(intptr_t)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    } else {
        SetDpiAwareFn setAware = self ? (SetDpiAwareFn)(void*)GetProcAddress(self, "SetProcessDPIAware") : nullptr;
        if (setAware != nullptr) setAware();
    }

    QueryPerformanceFrequency(&g.perfFreq);
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    g.hinst = GetModuleHandleW(nullptr);

    // Register the window class (POC: extra pointer, arrow cursor, no
    // background brush — nothing is ever GDI-painted).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.cbClsExtra = sizeof(void*);  // POC: "Extra pointer"
    wc.hInstance = g.hinst;
    // UNICODE is not defined in this translation unit, so IDC_ARROW expands
    // to the ANSI string literal "32512" — build the wide ordinal explicitly.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    if (wc.hCursor == nullptr || RegisterClassExW(&wc) == 0) {
        fprintf(stderr, "[tetris] cannot register the window class (err=%lu)\n", GetLastError());
        return nullptr;
    }

    // Create the window with WS_EX_NOREDIRECTIONBITMAP, since all
    // presentation is happening through DirectComposition (POC).
    // Not shown yet: the first frame is ready only after the renderer init
    // (bgfx) — the window is shown on the first uiPumpEvents, with the first
    // frame already presented. The DirectComposition binding (below) still
    // happens between the construction and showing of the window, as in the
    // POC.
    g.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, toWide(title).c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              (int)w, (int)h, nullptr, nullptr, g.hinst, nullptr);
    if (g.hwnd == nullptr) {
        fprintf(stderr, "[tetris] CreateWindowEx failed (err=%lu)\n", GetLastError());
        return nullptr;
    }

    // The swap chain is created at the window's real client size (POC: 1x1,
    // then resized from the first WM_NCCALCSIZE — the same first frame,
    // without the detour).
    RECT cr = {};
    GetClientRect(g.hwnd, &cr);
    const uint32_t cw = (uint32_t)(cr.right - cr.left);
    const uint32_t ch = (uint32_t)(cr.bottom - cr.top);
    g.pxW = cw; g.pxH = ch;
    if (!createD3D(cw, ch)) {
        DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
        releaseD3D();
        return nullptr;
    }
    if (!dcompBind(g.hwnd)) {
        DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
        releaseD3D();
        return nullptr;
    }

    if (std::getenv("TETRIS_RESIZE_TEST") != nullptr) {
        g.resizeTestStep = 0;
        SetTimer(g.hwnd, kResizeTestTimer, 100, nullptr);
    }

    g_uiWindow.nwhType = UI_NWH_DEFAULT;
    g_uiWindow.nwh = g.hwnd;
    g_uiWindow.ndt = nullptr;
    g_uiWindow.offscreen = 1;   // the renderer renders into this layer's scene texture
    g_uiWindow.context = g.device;
    g_uiWindow.backBuffer = g.sceneRtv;  // the scene texture's RTV (bgfx's render target)
    return &g_uiWindow;
}

void uiPumpEvents(double timeoutSec) {
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeoutSec * 1000.0);
    g.frameDrivenThisPump = false;

    // The window is shown once the renderer is initialized (uiCreateWindow
    // runs before the renderer init, the first pump runs after it); the
    // DirectComposition binding is already in place (created before
    // ShowWindow, as in the POC).
    if (!g.shown && g.hwnd != nullptr) {
        g.shown = true;
        ShowWindow(g.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g.hwnd);  // best effort (the console may keep focus)
    }

    // 1) Drain the message queue (DispatchMessage -> wndProc). A resize
    //    (WM_NCCALCSIZE) renders + blits + presents the new-size frame
    //    inline, before the pump continues — the POC's never-flicker
    //    ordering.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g.quit = true; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 2) Drive + blit + present one frame unless the pump already did (a
    //    resize frame). Present(1, 0) paces the loop (vsync) and updates
    //    the displayed content. The FIRST frame takes the POC's resize
    //    present sequence (RESTART + DO_NOT_SEQUENCE) — as the POC's first
    //    reposition does: its first present starts the flip state with a
    //    RESTART (a plain DO_NOT_SEQUENCE on a freshly created swap chain
    //    is rejected, DXGI_ERROR_INVALID_CALL, until then).
    if (!g.frameDrivenThisPump && g.frameDriver != nullptr) {
        g.frameDriver(g.pxW, g.pxH, frameDt(), g.frameDriverUser);
        if (blitToSwapChain(g.presentedOnce ? PresentMode::Normal : PresentMode::Resize))
            g.presentedOnce = true;
    }

    // 3) Sleep the remainder of the budget (usually ~0: the vsync present
    //    consumed it) so a fast display / an early-accepted present keeps the
    //    loop at ~ the requested rate.
    const ULONGLONG now = GetTickCount64();
    if (now < deadline) Sleep((DWORD)(deadline - now));
}

int uiShouldQuit(void) {
    return g.quit ? 1 : 0;
}

int uiPopKey(void) {
    std::lock_guard<std::mutex> lk(g.keyMutex);
    if (g.keys.empty()) return KEY_NONE;
    int k = g.keys.front();
    g.keys.pop_front();
    return k;
}

void uiWindowSize(uint32_t* w, uint32_t* h) {
    if (w != nullptr) *w = g.pxW;
    if (h != nullptr) *h = g.pxH;
}

// Nothing to commit: the presents happen in the pump / in WM_NCCALCSIZE.
void uiCommitFrame(void) {}

// The resize frame is driven directly from WM_NCCALCSIZE (the frame driver),
// so the frame-sync callback is not used on this platform.
void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData) {
    g.frameSyncCb = cb;
    g.frameSyncUser = userData;
}

void uiSetFrameDriver(UiFrameDriver cb, void* userData) {
    g.frameDriver = cb;
    g.frameDriverUser = userData;
}

void* uiWindowResize(uint32_t w, uint32_t h) {
    // Resize the composition swap chain to (w, h) and (re)create the
    // full-window scene texture (bgfx's render target + the blit's source);
    // returns the (new) scene RTV — the renderer hands it to
    // bgfx::setPlatformData() + bgfx::reset(). Called on the UI thread
    // (single-threaded bgfx) on a size change, inside the frame driver
    // (before WM_NCCALCSIZE returns).
    if (g.d3dDead || g.swapChain == nullptr || g.device == nullptr) return nullptr;
    if (w != g.scW || h != g.scH) {
        // The last Normal present (Present(1, 0)) leaves a flip in flight
        // until its vsync; ResizeBuffers must not run with one outstanding,
        // and a flip left across ResizeBuffers can land afterwards as a
        // glitch frame (flicker). Drain it with the POC's own pair (see
        // PresentMode::Drain): the CURRENT scene is re-blitted (the last
        // rendered frame — the new-size frame is not rendered yet, so there
        // is no regression to an older one), the RESTART discards the
        // in-flight flip, and the blocking DO_NOT_SEQUENCE leaves nothing
        // outstanding. (Skipped before the first frame: nothing was ever
        // presented, so nothing is in flight.)
        // EXPERIMENT: drain disabled to isolate the resize pulse cause.
        // if (g.presentedOnce && !blitToSwapChain(PresentMode::Drain)) return nullptr;
        // ResizeBuffers requires that ALL outstanding references to the swap
        // chain's buffers are released first (MSDN). Satisfied: the per-frame
        // back-buffer RTV is released after each present, and the drain
        // present above has completed (blocking), so no flip is in flight.
        //  ClearState() — drop the last blit's (and bgfx's) pipeline
        //  bindings from the immediate context, then resize.
        g.dctx->ClearState();
        // (POC: DXGI_FORMAT_UNKNOWN keeps the existing format,
        // DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE as in reposition.)
        HRESULT hr = g.swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                                                DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
        if (!checkPresentHr("ResizeBuffers", hr)) return nullptr;
        // The scene texture must match the new client size. (bgfx holds the
        // old RTV pointer — it never releases it; the renderer re-points it
        // at the new one. On failure the swap chain is already at the new
        // size while g.scW/g.scH keep the old one, so the next frame
        // retries this path.)
        if (!createSceneTexture(w, h)) return nullptr;
        g.scW = w; g.scH = h;
        // POC: re-find the Intel output on every resize (the window may have
        // crossed a monitor boundary).
        lookForIntelOutput();
    }
    return g.sceneRtv;
}

int uiWindowReadBackbuffer(uint32_t w, uint32_t h, void* out, uint32_t outSize) {
    (void)w; (void)h;
    // Read the full-window scene texture (the frame bgfx just rendered — the
    // blit copies it 1:1 to the swap chain's back buffer, so this is exactly
    // the frame about to be presented) back into `out`, as RGBA rows, top
    // to bottom. (bgfx's own requestScreenShot cannot work in
    // external-back-buffer mode — its D3D11 capture path expects its own
    // swap chain.)
    if (g.d3dDead || g.device == nullptr || g.dctx == nullptr || g.sceneTex == nullptr) return -1;
    D3D11_TEXTURE2D_DESC desc = {};
    g.sceneTex->GetDesc(&desc);
    if (out == nullptr || outSize < desc.Width * desc.Height * 4) {
        return -1;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = g.device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr) || staging == nullptr) {
        return -1;
    }
    int rc = -1;
    g.dctx->CopyResource(staging, g.sceneTex);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(g.dctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        uint8_t* dst = (uint8_t*)out;
        const uint8_t* src = (const uint8_t*)mapped.pData;
        for (UINT y = 0; y < desc.Height; y++)
            memcpy(dst + (size_t)y * desc.Width * 4, src + (size_t)y * mapped.RowPitch,
                   (size_t)desc.Width * 4);
        g.dctx->Unmap(staging, 0);
        rc = (int)((size_t)desc.Width * desc.Height * 4);
    }
    staging->Release();
    return rc;
}

void uiShutdown(void) {
    if (g.hwnd != nullptr && g.resizeTestStep >= 0) {
        KillTimer(g.hwnd, kResizeTestTimer);
        g.resizeTestStep = -1;
    }
    if (g.hwnd != nullptr) {
        // WM_DESTROY (dispatched inside DestroyWindow) unbinds the
        // DirectComposition context.
        DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
        UnregisterClassW(kClassName, g.hinst);
    }
    // bgfx::shutdown() has already run (frontend.shutdownNow() before this
    // call): the device is released here, along with everything else.
    releaseD3D();
}
