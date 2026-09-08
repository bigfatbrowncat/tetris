# Smooth macOS Metal Window Resizing

To fix the "judder" or stretched content scaling issue during macOS Metal window resizing, you must decouple the rendering loop from AppKit's synchronous window resize handler and properly configure your `CAMetalLayer` or `MTKView` placement policy.

## Core Solutions for Smooth Metal Resizing

* **Acknowledge the macOS Behavior:** macOS often stretches the existing Metal backing store texture to fill the frame when a new frame fails to arrive synchronously during an active window drag, creating a visual judder or wobbly effect.
* **Configure Layer Placement Policy:** For high-DPI views, set your view's `layerContentsPlacement` to top-left and enable `autoResizeDrawable` to stabilize the backing store position.
  ```objc
  view.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
  view.autoResizeDrawable = true;
  ```
* **Handle Explicit Redraws:** Offload resizing redraws from display link timers if they lag. Synchronize explicit rendering calls inside `viewDidResize` or layout subviews rather than waiting for the main run-loop cadence to catch up.
* **Pin Swapchain Resources:** Avoid immediate deallocation or race conditions on resource recreation during active resizes by using a deferred release queue for Metal drawables and command buffers.
