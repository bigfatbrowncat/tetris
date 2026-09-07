// Cocoa implementation of the platform-neutral UI layer (ui/window.h).
#import <Cocoa/Cocoa.h>
#include <mutex>
#include <deque>
#include <atomic>
#include <cstdint>
#include "ui/window.h"

static NSApplication* s_app = nil;
static NSWindow* s_window = nil;
static id s_delegate = nil;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static std::atomic<bool> s_quit{false};
static std::mutex s_keyMutex;
static std::deque<int> s_keys;
static std::atomic<uint32_t> s_pxW{0};
static std::atomic<uint32_t> s_pxH{0};
static UiFrameSyncCallback s_frameSyncCb = nullptr;
static void* s_frameSyncUser = nullptr;

static int mapKey(unsigned short code) {
    switch (code) {
        case 123: case 0:      return KEY_LEFT;    // left / a
        case 124: case 2:      return KEY_RIGHT;   // right / d
        case 125: case 1:      return KEY_DOWN;    // down / s
        case 126: case 13: case 7: return KEY_CW;  // up / w / x
        case 6:                return KEY_CCW;     // z
        case 49:               return KEY_DROP;    // space
        case 35:               return KEY_PAUSE;   // p
        case 15:               return KEY_RESTART; // r
        case 12: case 53:      return KEY_QUIT;    // q / esc
        default:               return KEY_NONE;
    }
}

static void updatePixelSize() {
    if (s_window == nil) return;
    NSView* cv = [s_window contentView];
    CGFloat scale = [s_window backingScaleFactor];
    NSRect b = [cv bounds];
    s_pxW = (uint32_t)(b.size.width * scale + 0.5f);
    s_pxH = (uint32_t)(b.size.height * scale + 0.5f);
}

@interface TetrisView : NSView
@end
@implementation TetrisView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event {
    int k = mapKey((unsigned short)[event keyCode]);
    if (k != KEY_NONE) {
        std::lock_guard<std::mutex> lk(s_keyMutex);
        s_keys.push_back(k);
    }
}
@end

@interface TetrisWinDelegate : NSObject
@end
@implementation TetrisWinDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    s_quit = true; // block the raw close; the main loop performs a clean shutdown
    return NO;
}
- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    updatePixelSize();
    // Continuous resizing: repaint the new size synchronously, right inside the
    // resize notification, instead of waiting for the pump loop's next
    // iteration to catch up (docs/design/macos_metal_window_resizing.md).
    // cb() blocks until the frame is presented, so the canvas lands in step
    // with each drag increment.
    if (s_frameSyncCb) s_frameSyncCb(s_pxW.load(), s_pxH.load(), s_frameSyncUser);
}
@end

void uiInit(void) {
    s_app = [NSApplication sharedApplication];
    [s_app setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSMenu* mainMenu = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Tetris"];
    [appMenu addItemWithTitle:@"Quit Tetris" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [s_app setMainMenu:mainMenu];
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    NSRect rect = NSMakeRect(0, 0, w, h);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    s_window = [[NSWindow alloc] initWithContentRect:rect
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    [s_window setTitle:[NSString stringWithUTF8String:title]];
    [s_window center];

    TetrisView* view = [[TetrisView alloc] initWithFrame:rect];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    // While a resize is in flight (the new Metal frame has not been presented
    // yet), pin the backing store to the top-left instead of letting Core
    // Animation stretch the last presented frame to fill the window. The
    // synchronous repaint in windowDidResize: replaces it within a frame.
    view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
    [s_window setContentView:view];
    // Where the pinned old frame no longer covers a grown window, the window
    // background shows through. Match the renderer's clear color (0x101018 in
    // frontend/renderer.cpp) so the gap during a resize is seamless.
    [s_window setBackgroundColor:[NSColor colorWithSRGBRed:16.0/255.0 green:16.0/255.0 blue:24.0/255.0 alpha:1.0]];

    s_delegate = [[TetrisWinDelegate alloc] init];
    [s_window setDelegate:s_delegate];

    [s_window makeKeyAndOrderFront:s_app];
    [s_window makeFirstResponder:view];
    [s_app activateIgnoringOtherApps:YES];

    updatePixelSize();
    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = s_window;
    return &s_uiWindow;
}

void uiPumpEvents(double timeoutSec) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSec];
    NSEvent* e;
    while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:deadline
                                      inMode:NSDefaultRunLoopMode
                                    dequeue:YES]) != nil) {
        [NSApp sendEvent:e];
    }
}

int uiShouldQuit(void) {
    return s_quit ? 1 : 0;
}

int uiPopKey(void) {
    std::lock_guard<std::mutex> lk(s_keyMutex);
    if (s_keys.empty()) return KEY_NONE;
    int k = s_keys.front();
    s_keys.pop_front();
    return k;
}

void uiWindowSize(uint32_t* w, uint32_t* h) {
    if (w) *w = s_pxW;
    if (h) *h = s_pxH;
}

// macOS commits through the normal CALayer/Metal presentation path; nothing to
// do here.
void uiCommitFrame(void) {}

// Frame-sync hook: windowDidResize: calls cb() with the new content pixel size
// so the renderer repaints synchronously inside the resize notification
// (continuous resizing while dragging the window edge).
void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData) {
    s_frameSyncCb = cb;
    s_frameSyncUser = userData;
}

void uiShutdown(void) {
    if (s_app) [s_app terminate:nil];
}
