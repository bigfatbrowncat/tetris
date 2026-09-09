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
	// 1: the renderer runs headless (nwh/ndt unused) and publishes each frame as
	// raw BGRA8 pixels via uiPushFrame(); the UI layer presents them (GTK draws
	// them into a GtkDrawingArea). 0: the renderer presents to nwh directly.
	uint32_t offscreen;
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

// Commit the frame (present the new window size to the compositor). On Wayland
// this commits the root surface; on other backends it is a no-op. On Wayland
// the content (a subsurface committed independently by the renderer) may lag
// this commit by up to one frame; the window background matches the
// renderer's clear color, so the gap is invisible.
void uiCommitFrame(void);

// Frame-sync hook. On Wayland the rendered content is a subsurface committed
// independently of the root; the UI layer calls cb() from the surface "layout"
// signal (LAYOUT phase: after widget allocation, before PAINT) with the new
// content size in pixels. cb() hands the size to the renderer, which resizes +
// commits the subsurface asynchronously — the root commits the new size
// immediately (no resize lag) and the window background (matched to the
// renderer's clear color) covers the gap until the content lands. On macOS the
// UI layer calls cb() from windowDidResize: so the Metal canvas repaints
// synchronously inside the resize notification (continuous resizing while
// dragging the window edge; the previous frame stays pinned top-left, not
// stretched, until the new one is presented). On other backends this is a
// no-op (the main loop's repaintSynchronous + uiCommitFrame ordering already
// suffices).
typedef void (*UiFrameSyncCallback)(uint32_t pixelW, uint32_t pixelH, void* userData);
void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData);

// Offscreen frame handoff (renderer runs headless, publishes raw pixels).
//
// uiPushFrame(): call from the game thread after the renderer has read back a
// frame. w/h are in device pixels, bgra points at w*h*4 bytes in B,G,R,A
// order, top row first. The UI layer copies it; bgra may be reused/freed
// after the call returns.
void uiPushFrame(uint32_t w, uint32_t h, const uint8_t* bgra);

// uiPresentFrame(): call from the main thread each frame; asks the UI layer to
// repaint with the latest pushed frame if one is pending.
void uiPresentFrame(void);

void uiShutdown(void);

#ifdef __cplusplus
}
#endif
