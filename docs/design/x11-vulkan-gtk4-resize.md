# Synchronizing X11 Window Resizing to VSync with bgfx

By design, **X11 operates asynchronously and on an event-driven basis**. When you drag a window corner, your mouse triggers high-frequency hardware events (often **500Hz to 1000Hz**), forcing the X Server to push out immediate `ConfigureNotify` events. This floods your applications with demands to redraw before your monitor has a chance to refresh, resulting in stuttering, lag, or trailing black borders.

While you cannot directly bind or synchronize the raw mouse cursor's motion directly to VSync, you can implement a **cooperative or synchronized resize loop** inside your application. This technique throttles your response to mouse input by ignoring intermediate events until your application has finished painting and syncing its current frame.

---

## 1. Core Mechanics: Event Compression & VSync Throttling

To prevent your application from choking on heavy pipeline allocations during a drag operation, you must implement two architectural steps:
1. **Event Compression:** Drain the entire X11 event queue at the beginning of the frame. Capture *only* the final, most recent window dimensions and ignore the intermediate ones.
2. **VSync Throttling:** Use your rendering loop's frame submission boundary as a "master brake." This blocks your main loop thread until the GPU swapchain hits the VBlank interval, giving the X11 server time to aggregate mouse inputs natively.

---

## 2. Implementation Pattern (Raw X11 & bgfx)

Because **bgfx** operates on a **Render Thread** model (where API calls on the main thread are queued up and sent to an isolated graphics thread), you must execute `bgfx::reset()` exactly once per frame on the main application loop, rather than inside asynchronous event handlers.

Below is the complete architectural pattern using raw Xlib and bgfx:

```cpp
#include <X11/Xlib.h>
#include <bgfx/bgfx.h>
#include <cstdint>   // uintptr_t

int main() {
    // ... [Your standard X11 Window Setup code here] ...
   
    // 1. Initialize bgfx with VSync enabled
    bgfx::Init init;
    init.type = bgfx::RendererType::Count; // Auto-select (Vulkan/GL/Metal/Direct3D)
    init.resolution.width = 800;
    init.resolution.height = 600;
    init.resolution.reset = BGFX_RESET_VSYNC; // Crucial for locking the frame rate
   
    // Pass native X11 handles to bgfx
    bgfx::PlatformData pd;
    pd.ndt = display;
    pd.nwh = (void*)(uintptr_t)window;
    init.platformData = pd;
   
    bgfx::init(init);

    // Track state variables
    bool running = true;
    bool needs_resize = false;
    uint32_t next_width = 800;
    uint32_t next_height = 600;

    // 2. Main Application Loop
    while (running) {
        XEvent event;

        // A. Drain the X11 queue completely for this frame.
        // If the user drags the mouse, dozens of ConfigureNotify events will hit.
        // We skip processing until we capture the ABSOLUTE LATEST size.
        while (XPending(display)) {
            XNextEvent(display, &event);

            if (event.type == ConfigureNotify) {
                // Prevent crash or bad allocations if window is minimized
                if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
                    next_width = event.xconfigure.width;
                    next_height = event.xconfigure.height;
                    needs_resize = true;
                }
            }
            else if (event.type == ClientMessage) {
                // Handle window close, etc.
                running = false;
            }
        }

        // B. Apply the size adjustment ONCE per frame
        if (needs_resize) {
            // Safely command bgfx to recreate internal swapchains/render targets
            bgfx::reset(next_width, next_height, BGFX_RESET_VSYNC);
           
            // Re-configure your primary view rect to match the new back-buffer
            bgfx::setViewRect(0, 0, 0, (uint16_t)next_width, (uint16_t)next_height);
           
            needs_resize = false; // Reset toggle
        }

        // C. Render your scene
        bgfx::touch(0);
        // ... submit draw calls (bgfx::submit) ...

        // D. The Master Brake
        // This function blocks the loop. Because BGFX_RESET_VSYNC is enabled,
        // bgfx will hold this thread until the GPU swapchain hits the VBlank interval.
        bgfx::frame();
    }

    bgfx::shutdown();
    return 0;
}
```
