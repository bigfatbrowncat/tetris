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

void uiShutdown(void);

#ifdef __cplusplus
}
#endif
