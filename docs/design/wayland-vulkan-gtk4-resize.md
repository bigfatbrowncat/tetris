# Rendering with bgfx + Vulkan on GTK4/Wayland without the resize white-strip

A design note and adoption guide for the Tetris frontend. It covers **what**
flickers, **why**, the **design decisions** we made to eliminate it, **how** each
piece is implemented, and a **step-by-step recipe** for porting the approach to
another app.

> TL;DR — On Wayland, bgfx draws into a `wl_subsurface` (the *content*) while
> GTK owns the root surface (the *header bar / CSD*). A white strip appears on
> resize whenever the content is still at the old size in the frame the window
> grows. The original fix — resize + present the content *synchronously* in the
> frame-clock LAYOUT phase, before the root is committed — killed the strip but
> made the **window itself** lag the pointer during a drag, because the UI
> thread blocked inside the frame clock until the content was re-presented.
>
> The current fix keeps the strip invisible without blocking: the root commits
> the new size **immediately** (no resize lag), and the content subsurface
> catches up **asynchronously** within a frame. The one-frame gap is invisible
> because the window background (GTK CSS) matches the renderer's clear color
> (`0x101018`); when the window *shrinks* there is no gap at all, because the
> compositor clips the larger subsurface to the parent's bounds. Mesa's Vulkan
> WSI still runs in `mailbox` mode (spec-required on Wayland; §4.5).
>
> The synchronous path (blocking `repaintSynchronous` + the 3-frame present
> rule, §4.4) is kept for **X11 and macOS**, where the canvas is not a
> separately-committed surface and a one-frame gap would show through.
>
> `mailbox` is not optional on Wayland: the Vulkan spec
> (`VK_KHR_wayland_surface`, revision 6) **requires** every Wayland surface
> implementation to expose `VK_PRESENT_MODE_MAILBOX_KHR`. So the frontend has
> **no FIFO fallback** — it probes for `mailbox` at init and fails loudly (with
> a driver diagnostic) if a spec-compliant driver somehow doesn't expose it
> (see §4.5).

---

## 1. The problem

Drag a GTK4 window edge on Wayland and a **white strip** flashes along the edge
for one or two frames. Cause: the window (root surface) grows in a given
frame, but the GL/Vulkan content (a separate subsurface) is still rendered at
the previous size, so the newly-exposed area has no content under it.

The first fix guaranteed that **the content is committed at the new size in the
*same frame* the root commits at the new size** — but it did so by *blocking*
the UI thread inside the frame clock (LAYOUT phase) until the content was
re-presented. During a drag, every resize step then cost a full
render + present + 2 extra `bgfx::frame()` round trips, so **the window size
itself lagged the pointer by several steps** — a new, visible problem.

The requirements are therefore:

1. **No visible strip** on resize (the original problem).
2. **No resize lag**: the root surface must commit the new size in the very
   frame the compositor delivers it.
3. **No protocol error** in the commit path (see §4.5).

Because the content is a *separate surface* (the compositor keeps showing its
last committed buffer until a new one arrives), requirement 2 only has to cost
a one-frame, invisible gap for requirement 1 — which the matching window
background (§4.4) provides.

We also switched the backend from OpenGL to **Vulkan** (bgfx) because Vulkan
gives us a clean, well-defined swapchain + present model and is what the
project standardized on.

---

## 2. The architecture we build on

Understanding the moving parts is required before the fix makes sense.

### 2.1 Three threads

| Thread | Runs | Owns |
|---|---|---|
| UI (main) thread | GTK event loop | the GTK window, the `wl_display`, key input |
| Game (API) thread | `TetrisFrontend::run()` | all `bgfx::` API calls, the backend |
| bgfx internal render thread | spawned by `bgfx::init()` | the actual GPU submit / present |

The API thread never calls `bgfx::renderFrame()` — because it never did so
*before* `bgfx::init()`, bgfx spawns its own internal render thread. The API
thread only *submits* work (`bgfx::frame()`); the render thread does the GPU
submit and the present. This split is the source of the **present-latency
rule** in §4.4.

### 2.2 The content is a `wl_subsurface`

On Wayland the root `wl_surface` (GTK's) carries the CSD header bar. The
bgfx content lives on a **child surface** attached as a `wl_subsurface` of the
root, placed just below the header. So there are two surfaces the compositor
presents together:

```
root wl_surface  (GTK: header bar / CSD / shadow)
└── content wl_subsurface  (bgfx/Vulkan swapchain — the actual game)
```

The subsurface must be positioned in the **root's** coordinate space, whose
origin is the *outer edge of the CSD shadow*, not the visible window. The
offset is recovered from `gtk_native_get_surface_transform()` (shadow) +
`gtk_widget_translate_coordinates()` (content area top-left). See
`updateContentSubPosition()`.

### 2.3 The GTK4 frame clock (the key insight)

When a resize is pending, GTK drives a **frame clock** with distinct phases:

1. **PREPARE**
2. **LAYOUT** — widgets are re-allocated. GTK's own `surface_layout_cb`
   (in `gtk4`, `gtknative.c:183`) runs here and gives `s_area` its new size.
3. **PAINT** — drawing, then the **root surface commit** (present).

The **`"layout"` signal** on the `GdkSurface` is emitted during the LAYOUT
phase. That is our hook point: at that instant `uiWindowSize()` already returns
the **new** pixel size, so we hand it to the game thread (non-blocking) at the
earliest moment in the frame. The root then commits the new size right away in
PAINT (no lag) and the content subsurface catches up asynchronously (§4.4).

> The hook must be connected **after** GTK's own `surface_layout_cb` so that
> `s_area` is already at the new size when it fires. `g_signal_connect()`
> appends to the end of the handler list, so connecting it in
> `uiCreateWindow()` (after `gtk_window_present`) is sufficient.

---

## 3. Design decisions (and why)

| Decision | Choice | Why |
|---|---|---|
| Backend | bgfx **Vulkan** (Linux), Metal (macOS) | Defined swapchain/present model; project standard. |
| Where content lives | `wl_subsurface` under the GTK root | Lets GTK keep the CSD header; content is a separate, independently-committed surface. |
| When the resize is requested | **LAYOUT phase** via the `"layout"` signal (non-blocking) | As early as possible in the frame clock; the handler must *not* block, or the root commit (and thus the window size) lags the pointer. |
| How the content catches up | **Asynchronously**: the game thread resets + presents the subsurface on its next iteration (woken immediately by the resize) | The compositor keeps showing the old buffer until the new one arrives → no blank flash; the gap is ≤ 1 frame. |
| Hiding the one-frame gap | Window background (GTK CSS) == renderer clear color (`#101018`) | On grow, the newly exposed area shows the window background behind the subsurface — identical to the scene's letterbox margin, so the gap is invisible. On shrink there is no gap (the subsurface is clipped to the parent). |
| Sync backends (X11, macOS) | **Synchronous repaint**: block the UI thread until the frame is presented | No separately-committed surface there — a one-frame gap shows through (garbage/undefined X11 area, stretched Metal canvas), so the canvas must land in the same frame as the window resize. |
| Present-latency handling (sync backends) | **3-frame rule** (`endFrame` + 2 extra `bgfx::frame()`) | In bgfx's multithreaded mode frame *N* is only guaranteed presented after two further `frame()` calls return. |
| VSync / present mode | `mailbox` (forced via `MESA_VK_WSI_PRESENT_MODE`) on Wayland; **no FIFO fallback** | The Vulkan spec (`VK_KHR_wayland_surface`) *requires* Wayland implementations to expose `VK_PRESENT_MODE_MAILBOX_KHR` — Wayland is inherently mailbox. We probe for it and fail loudly if absent. (`fifo` would additionally fault on the resize double commit via the commit-timer, §4.5.) |
| Reset flags | `0` (no `BGFX_RESET_VSYNC`) on Wayland, `BGFX_RESET_VSYNC` elsewhere | Consistent with `mailbox` (no VSync) on Wayland; VSync elsewhere. |
| Fallback resize | Main-loop `setWindowSize()` (Wayland) / `repaintSynchronous()` (else) each iteration | Covers any frame where the `"layout"`/resize hook didn't fire. No-op when size is unchanged. |

The guiding principle: **the root surface must commit the new window size in
the frame the compositor delivers it (no resize lag), and the content
subsurface may be at the old size for at most one frame after that — with the
exposed area showing a background identical to the renderer's clear color, so
the gap is not visible.** Everything below is in service of that.

---

## 4. How it's implemented

### 4.1 The Vulkan switch (shaders)

Shaders are compiled to **SPIR-V** on Linux (`shaders/vk/`) and Metal on macOS.
The shared frontend code `#include "shaders/vs_quad.h"`; CMake stages the
correct headers into the build dir so the include resolves to SPIR-V on Linux.

- `scripts/build_shaders.py`: `PROFILE = "metal" if IS_MACOS else "spirv"`,
  `OUT_DIR = "vk/"` on Linux. Invokes bgfx's `shadercRelease` with
  `-p spirv --platform linux`, producing a "VSH"/"FSH" binary wrapper with the
  SPIR-V blob embedded.
- `CMakeLists.txt`: a `shaders` target runs the script; a second custom command
  **copies** `shaders/vk/*.h` into `${CMAKE_BINARY_DIR}/gen-shaders/shaders/`,
  which is added to the include path *before* the source tree, so the SPIR-V
  headers win over the macOS ones.
- `frontend/renderer.cpp`:

```cpp
bgfx::Init init;
#if BX_PLATFORM_OSX
    init.type = bgfx::RendererType::Count;   // auto-select -> Metal
#else
    init.type = bgfx::RendererType::Vulkan;  // SPIR-V headers (shaders/vk/)
#endif
init.fallback = true;
init.platformData.nwh = win->nwh;          // wl_surface* on Wayland
init.platformData.ndt = win->ndt;          // wl_display* on Wayland
init.platformData.type = (bgfx::NativeWindowHandleType::Enum)win->nwhType;
init.resolution.reset = m_resetFlags;
```

### 4.2 The content subsurface (created in `gtk/window.cpp`)

On Wayland, `uiCreateWindow()` grabs the GTK root's `wl_surface` and
`wl_display`, creates a child surface, and parents it as a subsurface:

```cpp
struct wl_display* wlDisplay = gdk_wayland_display_get_wl_display(display);
struct wl_surface* rootSurface = gdk_wayland_surface_get_wl_surface(GDK_SURFACE(surf));
// ... bind wl_compositor + wl_subcompositor (bindWaylandGlobals) ...
s_contentSurface = wl_compositor_create_surface(s_compositor);
s_contentSub = wl_subcompositor_get_subsurface(s_subcompositor,
                                               s_contentSurface, rootSurface);
wl_surface_set_buffer_scale(s_contentSurface, (int32_t)s_scale);
wl_subsurface_set_position(s_contentSub, 0, headerHeightLogical(s_header));
wl_subsurface_place_above(s_contentSub, rootSurface);
// bgfx renders into THIS surface:
s_uiWindow.nwh = s_contentSurface;   // (not the root)
s_uiWindow.ndt = wlDisplay;
s_uiWindow.nwhType = UI_NWH_WAYLAND;
```

Notes that bit us:

- **`nwh` is the content subsurface, not the root.** bgfx renders into the
  subsurface; GTK keeps painting the header into the root.
- **`ndt` must be the same `wl_display` that owns the surface**, otherwise
  surface creation fails.
- **`buffer_scale` = the surface scale factor** (e.g. 2 on a HiDPI display) or
  the render is half-size.
- **Subsurface position is in root coordinates**, origin at the shadow edge —
  `updateContentSubPosition()` computes `shadow + area_pos`. Re-run it on
  resize because the CSD shadow offset can change.

### 4.3 The LAYOUT-phase hook (the core of the fix)

A `UiFrameSyncCallback` is registered from the app and invoked from the
`"layout"` signal:

```cpp
// ui/window.h
typedef void (*UiFrameSyncCallback)(uint32_t pixelW, uint32_t pixelH, void* userData);
void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData);
```

The GTK layer wires it up (Wayland branch of `uiCreateWindow()`):

```cpp
g_signal_connect(surf, "layout", G_CALLBACK(onContentLayout), nullptr);
```

and the handler (`gtk/window.cpp`):

```cpp
static void onContentLayout(GdkSurface* surface, int width, int height, gpointer user) {
    if (s_frameSyncCb == nullptr || s_contentSub == nullptr) return;
    uint32_t w = 0, h = 0;
    uiWindowSize(&w, &h);                 // new pixel size (already allocated)
    if (w > 0 && h > 0) {
        s_frameSyncCb(w, h, s_frameSyncUser);   // -> setWindowSize (non-blocking on Wayland)
        updateContentSubPosition();           // re-align for CSD shadow change
    }
}
```

The app registers the callback **before** starting the game thread (`main.cpp`).
The callback picks the blocking or the async resize path per backend:

```cpp
static bool s_asyncResize = false;   // set from window->nwhType in main()

static void onFrameSync(uint32_t w, uint32_t h, void* user) {
    TetrisFrontend* fe = static_cast<TetrisFrontend*>(user);
    if (s_asyncResize) fe->setWindowSize(w, h);        // Wayland: async
    else fe->repaintSynchronous(w, h);                 // macOS: blocking
}
...
s_asyncResize = (window->nwhType == UI_NWH_WAYLAND);
TetrisFrontend frontend;
uiSetFrameSyncCallback(&onFrameSync, &frontend);   // before game thread
std::thread gameThread(&TetrisFrontend::run, &frontend, window);
```

The hook is invoked on Wayland (from the `"layout"` signal) and on macOS (from
`windowDidResize:`); on X11 it is never invoked — the main-loop fallback (§4.4)
does the resizing there.

### 4.4 The resize paths

**Wayland — asynchronous catch-up (no blocking).** The frame-clock hook only
*requests* the new size (`setWindowSize`): it stores the target size, sets
`m_resizePending`, and notifies the game thread — returning in microseconds.
The game thread's 16 ms pacing wait is interrupted by the flag, so it resets
bgfx to the new size and presents the subsurface on its very next iteration.
Meanwhile GTK's PAINT phase commits the root at the new size **immediately** —
the window tracks the pointer with zero added latency.

The at-most-one-frame gap is invisible by construction:

- **Grow:** the newly exposed area has no subsurface under it, so the
  compositor shows the root's pixels there — the window background. `gtk/window.cpp`
  sets it to `#101018`, the renderer's clear color (`0x101018ff` in
  `frontend/renderer.cpp`), via a `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION` CSS
  provider. The scene's letterbox margin is the same clear color, so the gap is
  seamless (same trick as `macos/window.mm`, which sets the NSWindow background).
- **Shrink:** the subsurface is *larger* than the content area, but the
  compositor clips a subsurface to its parent's bounds — nothing is exposed.

```cpp
// gtk/window.cpp, Wayland branch of uiCreateWindow()
GtkCssProvider* cssProvider = gtk_css_provider_new();
gtk_css_provider_load_from_string(cssProvider, "window { background: #101018; }");
gtk_style_context_add_provider_for_display(
    display, GTK_STYLE_PROVIDER(cssProvider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
g_object_unref(cssProvider);
```

**X11 / macOS — synchronous repaint + the 3-frame present rule.** These
backends have no separately-committed content surface (X11: bgfx draws into the
window directly; macOS: the Metal layer), so a one-frame gap would show
undefined/garbage pixels or a stretched canvas. `repaintSynchronous` is the
"resize now and wait" primitive; it is safe to call from the UI thread
mid-frame-clock / inside `windowDidResize:`:

```cpp
// frontend/tetris_frontend.cpp
void TetrisFrontend::repaintSynchronous(uint32_t w, uint32_t h) {
    if (m_pxW.load() == w && m_pxH.load() == h) return;   // no-op if unchanged
    m_pxW.store(w); m_pxH.store(h);
    m_syncResize.store(true);          // freeze the game (no gravity) this frame
    m_resizePending.store(true);
    m_frameCv.notify_all();            // wake the game thread
    std::unique_lock<std::mutex> lock(m_repaintMutex);
    m_repaintCv.wait(lock, [this, w, h] {
        return m_lastRenderedW.load() == w && m_lastRenderedH.load() == h;
    });
}
```

The game thread, when it renders a resize, calls the 3-frame sequence:

```cpp
m_renderer->render(snap, w, h);      // submit frame N
m_renderer->endFrame();              // bgfx::frame()  -> N
if (syncResize) m_renderer->syncPresent();   // bgfx::frame() x2 -> N+1, N+2
m_lastRenderedW.store(w);            // now safe: N is presented
m_lastRenderedH.store(h);
m_repaintCv.notify_all();            // unblock the UI thread
```

`syncPresent()` (`frontend/renderer.cpp`) is the **3-frame rule**:

```cpp
// Block until the frame submitted by the preceding endFrame() has been
// presented. In bgfx's multithreaded mode frame N is presented by the
// internal render thread while processing frame N+1, so the present of
// frame N is only guaranteed once two further bgfx::frame() calls have
// returned.
void syncPresent() {
    if (!ok) return;
    bgfx::frame();
    bgfx::frame();
}
```

**Why 3 frames:** `endFrame()` submits frame *N* but returns before it is
presented (the internal render thread does the submit/present). The present of
*N* is only guaranteed once the render thread has advanced past *N+1* and
*N+2*. Two extra `bgfx::frame()` calls push the pipeline far enough that *N*
has hit the screen. `m_syncResize` also **freezes the game** (skips gravity /
input) for that repaint frame so the resize frame is pure presentation.

### 4.5 The present mode: `mailbox`, and why there is no fallback

There are **two independent reasons** to run Mesa in `mailbox` on Wayland, and
together they remove any `fifo` fallback.

**(a) The spec requires it.** `VK_KHR_wayland_surface` (revision 6, issue #2)
mandates that every Wayland surface implementation expose
`VK_PRESENT_MODE_MAILBOX_KHR` — "Wayland is an inherently mailbox window system
and mailbox support is required for some Wayland compositor interactions to
work as expected." So a conformant Wayland driver *must* offer `mailbox`; an
app that only supports `fifo` on Wayland would be the broken party.

**(b) `fifo` faults on the resize double commit.** Running the resize path under
the default `fifo` (VSync) present mode produced a **fatal Wayland protocol
error**:

```
wl_display#1.error(wp_commit_timer_v1#97, 1, "Commit already has timestamp")
```

Root cause, from `WAYLAND_DEBUG=1`:

- In `fifo` mode, Mesa's Vulkan WSI uses the **commit-timing protocol**
  (`wp_commit_timer_v1.set_timestamp`) to schedule each commit at the next
  vsync.
- The compositor stores **one** timestamp per surface and clears it when it
  presents.
- The resize path produces a **double commit** on the content subsurface within
  a single vsync (attach → damage → `set_timestamp` → … → commit → … → commit).
- The **second** `set_timestamp` (same `tv_sec`, different `tv_nsec`) arrives
  before the compositor has cleared the first → `timestamp_exists` (error 1).

`mailbox` does **not** use the commit-timer at all (`set_timestamp` count drops
to 0), so the fault cannot occur.

**Implementation — probe, then force or fail.** Because `mailbox` is both
required and the only mode that works, the frontend probes for it at init
(before `bgfx::init()`) and **fails loudly** rather than falling back:

```cpp
// frontend/renderer.cpp, in Impl::init(), before bgfx::init()
if (win->nwhType == UI_NWH_WAYLAND) {
    // VK_KHR_wayland_surface REQUIRES mailbox on Wayland — there is no fifo
    // fallback. probeWsi() opens a throwaway VkInstance over the content
    // wl_surface, checks VK_PRESENT_MODE_MAILBOX_KHR, and records the GPU
    // driver name for the diagnostic.
    const WsiProbe probe = probeWsi(win->nwh, win->ndt);
    if (probe.mailbox) {
        fprintf(stderr, "[tetris] WSI present mode: mailbox (driver: %s)\n",
                probe.driverName.c_str());
        setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 0);
    } else {
        const char* server = (win->nwhType == UI_NWH_WAYLAND) ? "Wayland" : "X11";
        fprintf(stderr,
            "[tetris] FATAL: VK_PRESENT_MODE_MAILBOX_KHR is not available on this system.\n"
            "        graphics server : %s\n"
            "        dri version     : dri3 (Vulkan)\n"
            "        driver          : %s\n"
            "        VK_PRESENT_MODE_MAILBOX_KHR is required on %s "
            "(mandated by VK_KHR_wayland_surface) but the driver does not expose it.\n",
            server, probe.driverName.c_str(), server);
        return false;   // refuse to start rather than misbehave in fifo
    }
}
```

Caveats observed while tuning this:

- The env var must be set **before** `bgfx::init()` (Mesa reads it when
  creating the swapchain).
- `mailbox` presents without waiting for vsync — fine for a game (we drive
  our own pacing via the 3-frame rule and the 60 Hz UI loop).
- `immediate` and `fifo_relaxed` **hung** the app on this driver (Intel Arc /
  ANV, Mesa 25.0.7); `fifo` faulted as above — `mailbox` is the only value
  that works.
- This only matters on Wayland + Mesa Vulkan. It is a no-op on other stacks.
- The no-mailbox path is a fail-loud diagnostic for a **non-conformant**
  driver; on a spec-compliant Wayland driver it should never fire.

---

## 5. Per-frame ordering (the contract)

```
UI thread (per loop iteration):
  uiPumpEvents(0.016)
      └─ (on resize) frame clock:
             LAYOUT  -> "layout" signal -> onContentLayout
                          -> setWindowSize(w,h)   [non-blocking]
             PAINT   -> root surface commits at (w,h)  <- immediately, no lag
  drain keys
  setWindowSize(pw,ph)                # no-op if unchanged (fallback)
  uiCommitFrame()                     # commit the ROOT at the new size

game thread (woken by m_resizePending, same or next vsync):
  bgfx::reset(w,h) -> render -> endFrame()
  -> content subsurface commits at (w,h)   <- within one frame of the root
```

The invariant: **the root commits the new window size in the frame the
compositor delivers it; the content subsurface may be at the old size for at
most one frame after that, and the exposed area shows the window background,
which equals the renderer's clear color — so the gap is not visible.** On X11
and macOS the synchronous path (§4.4) instead guarantees *content commit at the
new size before the window commits at the new size*.

---

## 6. Adoption guide (port this to your app)

### Prerequisites

- bgfx with the **Vulkan** backend built (Linux).
- A `wl_compositor` + `wl_subcompositor` global on the compositor (standard).
- GTK4 on Wayland, **with a CSD header bar** (the header is what forces the
  content onto a subsurface). If you have no header, you can render directly
  into the root and the subsurface dance simplifies — but you lose the
  two-surface model this design relies on.
- C++17 (atomics, `std::thread`, condition variables).

### Step 1 — Split the surfaces

Create a child `wl_surface`, parent it as a `wl_subsurface` of the GTK root,
scale it, position it below the header, and hand **the child** to bgfx as the
native window (`nwh`), with `ndt` = the same `wl_display`. Do **not** hand bgfx
the root.

### Step 2 — Add the frame-sync callback

Add a `UiFrameSyncCallback` (or equivalent) to your UI layer. On Wayland,
connect it to the `GdkSurface` `"layout"` signal **after** `gtk_window_present`
so it runs after GTK's own allocator. In the handler, read the new pixel size
and hand it to your renderer **without blocking** (a `setWindowSize` that
stores the size and wakes the game thread). The handler runs inside the frame
clock's LAYOUT phase — blocking it delays the root commit and the window lags
the pointer during a drag.

### Step 3 — Match the window background to the clear color

Set the GTK window background (CSS provider at application priority) to the
exact clear color of the renderer. The one-frame async gap then shows a
seamless band instead of a white strip.

### Step 4 — (Sync backends only) implement synchronous repaint

For backends without a separately-committed content surface (X11, macOS), a
`repaintSynchronous(w,h)` that:
1. Returns immediately if the size is unchanged.
2. Stores the target size, sets a "sync resize" flag, and wakes the game thread.
3. Blocks on a condition variable until the game thread reports
   `lastRendered == (w,h)`.

### Step 5 — (Sync backends only) implement the 3-frame present rule

On the game thread, after rendering a resize frame, issue
`endFrame()` (frame *N*) **plus** two more `bgfx::frame()` calls (a
`syncPresent()`), *then* update `lastRendered` and notify. This is what makes
"presented" actually mean "on screen" given bgfx's internal render thread.

### Step 6 — Probe and force the Vulkan present mode (Wayland/Mesa)

`mailbox` is spec-required on Wayland (§4.5), so there is **no `fifo` fallback**.
In the renderer's `init()`, before `bgfx::init()`, if the window is Wayland:
probe for `VK_PRESENT_MODE_MAILBOX_KHR` (and grab the driver name), and either
force `mailbox` or **fail loudly** with a diagnostic:

```cpp
const WsiProbe probe = probeWsi(win->nwh, win->ndt);   // { mailbox, driverName }
if (probe.mailbox) {
    setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 0);  // force it
} else {
    // log FATAL: graphics server / dri version / driver / "mailbox not available"
    return false;   // refuse to start rather than misbehave in fifo
}
```

### Step 7 — Keep a main-loop fallback

Call `setWindowSize(currentSize)` (Wayland) / `repaintSynchronous(currentSize)`
(X11, macOS) every main-loop iteration. It is a no-op when the size is
unchanged; it covers any frame where the layout/resize hook didn't fire.

### Verification

- **Build:** `cmake --build build` — confirm no errors and that the SPIR-V
  headers (`shaders/vk/*.h`) are staged (not the Metal ones).
- **Smoke run:** run with a frame-count env var (here `TETRIS_SMOKE_TEST=N`) and
  confirm it reaches the frame count, renders a screenshot, and **exits 0**.
- **Protocol check:** run under `WAYLAND_DEBUG=1` and confirm **no**
  `wl_display#*.error` lines and **zero** `set_timestamp` calls.
- **The real test (manual):** drag each window edge / corner to grow and
  shrink. The white strip must not appear. This cannot be automated here
  (no `wtype`/`ydotool` in the dev environment), so it is a required manual
  gate.

---

## 7. Pitfalls (things that cost time)

- **Handing bgfx the root instead of the subsurface.** The header then fights
  the content for the same surface. `nwh` must be the content child.
- **Forgetting `buffer_scale`.** Content renders at 1/2 (or 1/N) size on HiDPI.
- **Positioning the subsurface in widget coords.** It must be in **root**
  coords (shadow origin). Use `gtk_native_get_surface_transform` +
  `translate_coordinates`.
- **Connecting the `"layout"` handler too early.** It must run *after*
  GTK's own `surface_layout_cb`, or `uiWindowSize()` returns the old size.
- **Blocking the frame clock on resize (Wayland).** If the `"layout"` handler
  waits for the content to be re-presented, the root commit (PAINT) is delayed
  by a render + present round trip per drag step → the window size lags the
  pointer. The gap is hidden by the matching background instead (§4.4).
- **Forgetting to match the window background.** The async catch-up gap only
  stays invisible while the CSS background equals the renderer's clear color.
  Change one without the other and the white strip returns.
- **Not waiting for the present (sync backends only).** On X11/macOS, skipping
  the 3-frame rule lets the window commit before the canvas is on screen → the
  strip returns.
- **Running in `fifo` VSync mode.** The commit-timer double-commit fault
  (§4.5). `mailbox` is spec-required on Wayland and there is **no `fifo`
  fallback** — probe for it and fail loudly if a driver doesn't expose it.
- **Setting `MESA_VK_WSI_PRESENT_MODE` after `bgfx::init()`.** Too late — Mesa
  reads it during swapchain creation.
- **Freezing nothing on a synchronous resize frame.** Without `m_syncResize`,
  gravity / input advance mid-resize and the frame is visually wrong (sync
  backends only; the async Wayland path never freezes the game).

---

## 8. Where each piece lives (file map)

| Concern | File |
|---|---|
| Vulkan init, `probeWsi` (mailbox probe + driver name), `setenv` mailbox, `syncPresent` (3-frame rule) | `frontend/renderer.cpp` |
| `syncPresent()` / `render()` / `endFrame()` API | `frontend/renderer.h` |
| `setWindowSize` (async) / `repaintSynchronous` (sync), game loop, 3-frame call site | `frontend/tetris_frontend.cpp` |
| `m_syncResize`, `m_pxW/H`, `m_resizePending`, `m_lastRenderedW/H`, cv/mutex | `frontend/tetris_frontend.h` |
| Subsurface creation, `"layout"` handler, matching window background (CSS), `uiSetFrameSyncCallback`, `updateContentSubPosition` | `gtk/window.cpp` |
| `UiFrameSyncCallback` typedef, `UI_NWH_WAYLAND`, `uiSetFrameSyncCallback` | `ui/window.h` |
| `s_asyncResize` + `onFrameSync` (async vs blocking), callback registration, main-loop ordering | `main.cpp` |
| Blocking `windowDidResize:` repaint + matching NSWindow background (macOS) | `macos/window.mm` |
| SPIR-V shader build | `scripts/build_shaders.py` |
| Shader staging into build dir | `CMakeLists.txt` |
