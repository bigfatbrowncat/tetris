// ============================================================================
//  macOS renderer — the platform part of tetris::frontend::Renderer.
//  ---------------------------------------------------------------------------
//  Metal: bgfx auto-selects the Metal backend and presents to the window
//  directly (the threaded mode: the game thread owns bgfx — init/frame/
//  shutdown; the UI thread only pumps events).
//
//  The scene construction and the draw flow are the base class
//  (frontend/renderer.{h,cpp}); this file implements only the platform hooks
//  and the frontend's createRenderer() factory for this platform.
// ============================================================================
#pragma once

#include "frontend/renderer.h"

namespace tetris::platform::cocoa {

class Renderer : public tetris::frontend::Renderer {
private:
    bool platformInit(const UiWindow* win, bgfx::Init& init) override;
    bool platformResize(uint32_t w, uint32_t h) override;
    void platformPrepareView() override;
    void platformSyncPresent() override;
    void platformRequestScreenShot(const char* path) override;
};

}  // namespace tetris::platform::cocoa
