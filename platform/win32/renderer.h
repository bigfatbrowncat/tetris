// ============================================================================
//  Windows renderer — the platform part of tetris::frontend::Renderer.
//  ---------------------------------------------------------------------------
//  Direct3D 11 composition path: the UI layer (platform/win32/window.cpp)
//  owns the D3D11 device, the composition swap chain and a full-window
//  scene texture (created in uiCreateWindow). bgfx adopts the device
//  (Init.platformData.context — it AddRefs it) and renders into the scene
//  texture (Init.platformData.backBuffer = its RTV — bgfx borrows the
//  pointer and never releases it); the UI layer blits the scene texture to
//  the swap chain's back buffer and presents it through DirectComposition.
//
//  Threading (bgfx): the UI (Win32) thread owns bgfx — single-threaded
//  (bgfx::renderFrame latched before init, from this thread), driven from
//  the message pump once per displayed frame, exactly like the Linux path.
//
//  The scene construction and the draw flow are the base class
//  (frontend/renderer.{h,cpp}); this file implements only the platform hooks
//  and the frontend's createRenderer() factory for this platform.
// ============================================================================
#pragma once

#include "frontend/renderer.h"

namespace tetris::platform::win32 {

class Renderer : public tetris::frontend::Renderer {
private:
    bool platformInit(const UiWindow* win, bgfx::Init& init) override;
    bool platformResize(uint32_t w, uint32_t h) override;
    void platformPrepareView() override;
    void platformSyncPresent() override;
    void platformRequestScreenShot(const char* path) override;

    // The UI layer's ID3D11Device* (adopted via Init.platformData.context)
    // and the HWND — kept to re-issue bgfx::setPlatformData with the *same*
    // context/ndt when the back-buffer RTV is re-created on a resize
    // (setPlatformData requires them unchanged and re-reads only the
    // backBuffer).
    void*    m_d3dDevice = nullptr;
    void*    m_nwh = nullptr;
    uint32_t m_nwhType = UI_NWH_DEFAULT;
};

}  // namespace tetris::platform::win32
