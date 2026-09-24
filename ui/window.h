// Platform-neutral window + keyboard input layer for the bgfx Tetris port.
// Exposes a small C API so the C++ game (main.cpp) stays free of
// Objective-C++ / GTK / Win32+D3D11 details. Implemented by
// macos/window.mm (Cocoa), gtk/window.cpp (GTK4, X11 + Wayland) and
// windows/window.cpp (Win32 + DirectComposition + D3D11).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native window handle kinds, mirroring bgfx::NativeWindowHandleType.
enum {
	UI_NWH_DEFAULT = 0,   // platform's primary window (NSWindow* / X11 XID)
	UI_NWH_WAYLAND = 1    // wl_surface*
};

// Logical keys mapped from the platform's key codes.
enum {
	KEY_NONE = 0,
	KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP,
	KEY_CW, KEY_CCW, KEY_DROP, KEY_PAUSE, KEY_RESTART, KEY_QUIT
};

// Native window data handed to bgfx::Init.platformData.
typedef struct {
	uint32_t nwhType;   // UI_NWH_*
	void*    nwh;
	void*    ndt;      // native display: X11 Display* / Wayland wl_display* (or NULL)
	// 1: the renderer renders offscreen: on Linux (the GTK GL area path,
	//    X11 + Wayland) into a shared GL texture that the UI layer presents
	//    as a full-frame quad (GPU to GPU, no CPU copy, nwh/ndt unused); on
	//    Windows into the UI layer's full-window D3D11 scene texture
	//    (context/backBuffer below), which the UI layer blits to the
	//    composition swap chain's back buffer and presents through
	//    DirectComposition. 0: the renderer presents to nwh directly (macOS).
	uint32_t offscreen;
	// GTK GL area path only (offscreen == 1); all values are EGL types:
	//   eglDisplay      — the EGLDisplay everything shares.
	//   eglContext      — the EGL context bgfx adopts (bgfx::Init.platformData
	//                     .context); shares the GL area's context and is
	//                     current (surfaceless, or on a 1x1 pbuffer).
	//   eglPbuffer      — the surface C2 is current on (never presented);
	//                     NULL when the context is surfaceless.
	//   eglAreaSurface  — the GL area context's window surface (NULL when the
	//                     context is surfaceless, e.g. on Wayland; the UI
	//                     layer re-binds the context surfaceless before
	//                     presenting the quad).
	//   sceneTex        — GL texture name (RGBA8) the renderer draws the scene
	//                     into; owned by the UI layer, kept at the window's
	//                     device-pixel size, visible in every context of the
	//                     share group.
	// NULL / 0 on other platforms.
	void*    eglDisplay;
	void*    eglContext;
	void*    eglPbuffer;
	void*    eglAreaSurface;
	uint32_t sceneTex;
	// Windows composition path only (offscreen == 1); all D3D11 types:
	//   context    — the ID3D11Device* bgfx adopts (platformData.context);
	//                bgfx AddRefs it, the UI layer owns the base reference.
	//   backBuffer — the ID3D11RenderTargetView* over the UI layer's
	//                full-window scene texture (platformData.backBuffer) —
	//                the surface bgfx renders the game into. bgfx never
	//                releases it; the UI layer re-creates it on resize
	//                (uiWindowResize) and re-points bgfx at it with
	//                bgfx::setPlatformData() + bgfx::reset(). The UI layer
	//                then blits the scene texture to the swap chain's back
	//                buffer and presents (the swap chain itself is fully
	//                owned by the UI layer).
	// NULL on other platforms.
	void*    context;
	void*    backBuffer;
} UiWindow;

// Create the UI toolkit (app + menu). Call once on the main thread.
void uiInit(void);

// Create a titled, resizable window and return its native window data
// (for bgfx::Init.platformData). Must be called on the main thread.
const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title);

// Pump UI events non-blockingly (keys, resize, close). Call on the main thread each frame.
void uiPumpEvents(double timeoutSec);

// True once the user asked to quit (Q/Esc or the window close button).
int uiShouldQuit(void);

// Pop one mapped key; returns KEY_NONE if none pending.
int uiPopKey(void);

// Current drawable (pixel) size of the window content.
void uiWindowSize(uint32_t* w, uint32_t* h);

// Commit the frame (present the new window size to the compositor). On other
// backends this is a no-op (GTK's frame clock already presents; the GL area
// path draws into an offscreen texture that GSK composites, so there is no
// independent surface to commit).
void uiCommitFrame(void);

// Frame-sync hook. On macOS the UI layer calls cb() from windowDidResize: so
// the Metal canvas repaints synchronously inside the resize notification
// (continuous resizing while dragging the window edge; the previous frame
// stays pinned top-left, not stretched, until the new one is presented).
// On the GTK GL area path it is never called: the render callback below
// already knows the new size and resizes the scene texture itself.
typedef void (*UiFrameSyncCallback)(uint32_t pixelW, uint32_t pixelH, void* userData);
void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData);

// Game driver (the callback-mode UI paths: the GTK GL area, and the Windows
// composition window). The UI layer calls cb(pixelW, pixelH, dtSeconds,
// userData) once per displayed frame, on the UI thread — from the GtkGLArea
// "render" callback (paced by the compositor's frame clock) or from
// uiPumpEvents / the resize handler (paced by the vsync present) — with the
// UI layer's rendering context current. cb() updates the game and renders
// the scene (single-threaded bgfx driven on this thread: bgfx::frame() does
// the GPU submit inline on the caller); the UI layer then presents (the GL
// area blit, or the composition swap chain's Present). Never called on other
// platforms.
typedef void (*UiFrameDriver)(uint32_t pixelW, uint32_t pixelH, double dt, void* userData);
void uiSetFrameDriver(UiFrameDriver cb, void* userData);

// Windows composition path only: resize the composition swap chain to
// (w, h) and (re)create the UI layer's full-window scene texture (bgfx's
// render target + the blit's source); returns the (new) scene RTV for
// bgfx::setPlatformData(), or NULL on failure. Called by the renderer on a
// size change, on the UI thread. No-op on other platforms (returns NULL).
void* uiWindowResize(uint32_t w, uint32_t h);

// Windows composition path only: read the full-window scene texture (the
// frame bgfx just rendered; the blit copies it 1:1 to the swap chain's back
// buffer, so it is exactly the frame about to be presented) back into `out`
// as RGBA rows, top to bottom (w*h*4 bytes at most). Returns the number of
// bytes written, or -1 on error / on other platforms
// (bgfx::requestScreenShot is used there).
int uiWindowReadBackbuffer(uint32_t w, uint32_t h, void* out, uint32_t outSize);

void uiShutdown(void);

#ifdef __cplusplus
}
#endif
