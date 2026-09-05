# Rendering with bgfx + Vulkan on GTK4/Wayland without the resize white-strip

A design note and adoption guide for the Tetris frontend. It covers **what**
flickers, **why**, the **design decisions** we made to eliminate it, **how** each
piece is implemented, and a **step-by-step recipe** for porting the approach to
another app.

> TL;DR — On Wayland, bgfx draws into a `wl_subsurface` (the *content*) while
> GTK owns the root surface (the *header bar / CSD*). A white strip appears on
> resize whenever the content is still at the old size in the frame the window
> grows. The fix is to **resize + present the content in the frame-clock
> LAYOUT phase, before the root is committed (PAINT phase)**, and to run Mesa's
> Vulkan WSI in `mailbox` mode so the compositor's commit-timer can't fault on
> the double commit that path produces.
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

Two things had to be true simultaneously to kill it:

1. **The content must be committed at the new size in the *same frame* the root
   commits at the new size** — and specifically *before* that frame is
   presented.
2. **No protocol error** may abort the commit path mid-resize (see §4.5).

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
the **new** pixel size, but the root has **not** been committed yet. If we
resize + present the content subsurface *here*, it lands in the same frame as
the root commit → no strip.

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
| When to resize content | **LAYOUT phase** via the `"layout"` signal | Content commits at the new size *before* the root commit (PAINT) → same frame, no strip. |
| How to wait for the present | **Synchronous repaint**: block the UI thread until the frame is presented | The root must not commit until the content has actually landed. |
| Present-latency handling | **3-frame rule** (`endFrame` + 2 extra `bgfx::frame()`) | In bgfx's multithreaded mode frame *N* is only guaranteed presented after two further `frame()` calls return. |
| VSync / present mode | `mailbox` (forced via `MESA_VK_WSI_PRESENT_MODE`) on Wayland; **no FIFO fallback** | The Vulkan spec (`VK_KHR_wayland_surface`) *requires* Wayland implementations to expose `VK_PRESENT_MODE_MAILBOX_KHR` — Wayland is inherently mailbox. We probe for it and fail loudly if absent. (`fifo` would additionally fault on the resize double commit via the commit-timer, §4.5.) |
| Reset flags | `0` (no `BGFX_RESET_VSYNC`) on Wayland, `BGFX_RESET_VSYNC` elsewhere | Consistent with `mailbox` (no VSync) on Wayland; VSync elsewhere. |
| Fallback resize | Main-loop `repaintSynchronous()` each iteration | Covers non-Wayland and any frame where the `"layout"` signal didn't fire. No-op when size is unchanged. |

The guiding principle: **the content subsurface must never be observed by the
compositor at the old size in a frame where the root is at the new size.**
Everything below is in service of that.

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
        s_frameSyncCb(w, h, s_frameSyncUser);   // -> repaintSynchronous (blocks)
        updateContentSubPosition();           // re-align for CSD shadow change
    }
}
```

The app registers the callback **before** starting the game thread (`main.cpp`):

```cpp
static void onFrameSync(uint32_t w, uint32_t h, void* user) {
    static_cast<TetrisFrontend*>(user)->repaintSynchronous(w, h);
}
...
TetrisFrontend frontend;
uiSetFrameSyncCallback(&onFrameSync, &frontend);   // before game thread
std::thread gameThread(&TetrisFrontend::run, &frontend, window);
```

On non-Wayland platforms (and for the macOS target) `uiSetFrameSyncCallback` is
a **no-op** (`macos/window.mm`) — the callback is simply never invoked, and the
main-loop fallback (§4.4) does the resizing.

### 4.4 Synchronous repaint + the 3-frame present rule

`repaintSynchronous` is the "resize now and wait" primitive. It is safe to call
from the UI thread mid-frame-clock:

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
                          -> repaintSynchronous(w,h)  [BLOCKS]
                               game thread: render -> endFrame -> syncPresent
                               content subsurface committed at (w,h)
             PAINT   -> root surface will commit at (w,h)
  drain keys
  repaintSynchronous(pw,ph)          # no-op (already resized by LAYOUT)
  uiCommitFrame()                     # commit the ROOT at the new size
```

The invariant: **content commit at the new size happens *before* the root
commit at the new size.** Because both are presented in the same frame, the
compositor never sees the old-sized content under a new-sized window.

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
and call your synchronous-repaint primitive. Make the setter a **no-op** on
non-Wayland targets.

### Step 3 — Implement synchronous repaint

A `repaintSynchronous(w,h)` that:
1. Returns immediately if the size is unchanged.
2. Stores the target size, sets a "sync resize" flag, and wakes the game thread.
3. Blocks on a condition variable until the game thread reports
   `lastRendered == (w,h)`.

### Step 4 — Implement the 3-frame present rule

On the game thread, after rendering a resize frame, issue
`endFrame()` (frame *N*) **plus** two more `bgfx::frame()` calls (a
`syncPresent()`), *then* update `lastRendered` and notify. This is what makes
"presented" actually mean "on screen" given bgfx's internal render thread.

### Step 5 — Probe and force the Vulkan present mode (Wayland/Mesa)

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

### Step 6 — Keep a main-loop fallback

Call `repaintSynchronous(currentSize)` every main-loop iteration. On Wayland it
is a no-op (the LAYOUT hook already did it); on other platforms it is the
actual resize path.

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
- **Not waiting for the present.** Skipping the 3-frame rule makes the root
  commit before the content is on screen → the strip returns.
- **Running in `fifo` VSync mode.** The commit-timer double-commit fault
  (§4.5). `mailbox` is spec-required on Wayland and there is **no `fifo`
  fallback** — probe for it and fail loudly if a driver doesn't expose it.
- **Setting `MESA_VK_WSI_PRESENT_MODE` after `bgfx::init()`.** Too late — Mesa
  reads it during swapchain creation.
- **Freezing nothing on the resize frame.** Without `m_syncResize`, gravity /
  input advance mid-resize and the frame is visually wrong.

---

## 8. Where each piece lives (file map)

| Concern | File |
|---|---|
| Vulkan init, `probeWsi` (mailbox probe + driver name), `setenv` mailbox, `syncPresent` (3-frame rule) | `frontend/renderer.cpp` |
| `syncPresent()` / `render()` / `endFrame()` API | `frontend/renderer.h` |
| `repaintSynchronous`, game loop, 3-frame call site | `frontend/tetris_frontend.cpp` |
| `m_syncResize`, `m_pxW/H`, `m_lastRenderedW/H`, cv/mutex | `frontend/tetris_frontend.h` |
| Subsurface creation, `"layout"` handler, `uiSetFrameSyncCallback`, `updateContentSubPosition` | `gtk/window.cpp` |
| `UiFrameSyncCallback` typedef, `UI_NWH_WAYLAND`, `uiSetFrameSyncCallback` | `ui/window.h` |
| `onFrameSync` + callback registration, main-loop ordering | `main.cpp` |
| No-op `uiSetFrameSyncCallback` (macOS) | `macos/window.mm` |
| SPIR-V shader build | `scripts/build_shaders.py` |
| Shader staging into build dir | `CMakeLists.txt` |
