// Cocoa window + keyboard input layer for the bgfx Tetris port.
// Exposes a small C API so the C++ game (main.cpp) stays Objective-C++ free.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logical keys mapped from macOS key codes.
enum {
	KEY_NONE = 0,
	KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP,
	KEY_CW, KEY_CCW, KEY_DROP, KEY_PAUSE, KEY_RESTART, KEY_QUIT
};

// Create NSApplication + menu. Call once on the main thread.
void cocoaInit(void);

// Create a titled, resizable window and return its NSWindow* (for bgfx::Init.platformData.nwh).
// Must be called on the main thread.
void* cocoaCreateWindow(uint32_t w, uint32_t h, const char* title);

// Pump Cocoa events non-blockingly (keyDown, resize, close). Call on the main thread each frame.
void cocoaPumpEvents(double timeoutSec);

// True once the user asked to quit (Q/Esc or the window close button).
int cocoaShouldQuit(void);

// Pop one mapped key; returns KEY_NONE if none pending.
int cocoaPopKey(void);

// Current drawable (pixel) size of the window content.
void cocoaWindowSize(uint32_t* w, uint32_t* h);

void cocoaShutdown(void);

#ifdef __cplusplus
}
#endif
