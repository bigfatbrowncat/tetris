// Platform-neutral window + keyboard input layer for the bgfx Tetris port.
// Exposes a small C API so the C++ game (main.cpp) stays free of
// Objective-C++ / GTK details. Implemented by macos/window.mm (Cocoa) and
// gtk/window.cpp (GTK4, X11 + Wayland).
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
	// 1: the renderer renders offscreen into a shared GL texture that the UI
	//    layer presents as a full-frame quad inside a GtkGLArea (the GTK GL
	//    area path, X11 + Wayland — GPU to GPU, no CPU copy). nwh/ndt are
	//    unused. 0: the renderer presents to nwh directly (macOS).
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

// GTK GL area path only: the game driver. The UI layer calls
// cb(devicePixelW, devicePixelH, dtSeconds, userData) from the GtkGLArea
// "render" callback — once per displayed frame, paced by the compositor's
// frame clock — with the GL area's context current. cb() updates the game and
// renders the scene into UiWindow.sceneTex (single-threaded bgfx driven on
// this thread: bgfx::frame() followed by glFinish() so the texture write is
// complete on return). The UI layer then presents sceneTex as a full-frame
// quad into the window. Never called on other platforms.
typedef void (*UiFrameDriver)(uint32_t pixelW, uint32_t pixelH, double dt, void* userData);
void uiSetFrameDriver(UiFrameDriver cb, void* userData);

void uiShutdown(void);

#ifdef __cplusplus
}
#endif
