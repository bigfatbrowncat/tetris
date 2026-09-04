// GTK4 implementation of the platform-neutral UI layer (ui/window.h).
// X11 and Wayland are both supported: the native handle (XID / wl_surface)
// is extracted once the window is realized.
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#if GTK_CHECK_VERSION(4, 18, 0)
#include <gdk/x11/gdkx.h>
#else
#include <gtk/gtk-x11.h>
#endif
#endif
#ifdef GDK_WINDOWING_WAYLAND
#if GTK_CHECK_VERSION(4, 18, 0)
#include <gdk/wayland/gdkwayland.h>
#else
#include <gtk/gtk-wayland.h>
#endif
#include <wayland-client.h>
#endif

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include "ui/window.h"

static GtkWidget* s_win = nullptr;
static GtkWidget* s_area = nullptr;
static GtkWidget* s_header = nullptr;
static GdkSurface* s_gdkSurface = nullptr;
static GtkEventController* s_keyCtrl = nullptr;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static int s_headerHeightPx = 0;
static int s_scale = 1;
static std::atomic<bool> s_quit{false};
static std::mutex s_keyMutex;
static std::deque<int> s_keys;
static UiFrameSyncCallback s_frameSyncCb = nullptr;
static void* s_frameSyncUser = nullptr;

#ifdef GDK_WINDOWING_WAYLAND
static struct wl_compositor* s_compositor = nullptr;
static struct wl_subcompositor* s_subcompositor = nullptr;
static struct wl_surface* s_contentSurface = nullptr;
static struct wl_subsurface* s_contentSub = nullptr;
static struct wl_surface* s_rootSurface = nullptr;
static struct wl_display* s_wlDisplay = nullptr;
static int s_subPosX = INT_MIN;
static int s_subPosY = INT_MIN;
static std::atomic<bool> s_sizePrinted{false};
#endif

static int mapKey(guint keyval) {
    switch (keyval) {
        case GDK_KEY_Left:  case GDK_KEY_a: return KEY_LEFT;
        case GDK_KEY_Right: case GDK_KEY_d: return KEY_RIGHT;
        case GDK_KEY_Down:  case GDK_KEY_s: return KEY_DOWN;
        case GDK_KEY_Up:    case GDK_KEY_w: case GDK_KEY_x: return KEY_CW;
        case GDK_KEY_z:     return KEY_CCW;
        case GDK_KEY_space: return KEY_DROP;
        case GDK_KEY_p:     return KEY_PAUSE;
        case GDK_KEY_r:     return KEY_RESTART;
        case GDK_KEY_q:     case GDK_KEY_Escape: return KEY_QUIT;
        default:            return KEY_NONE;
    }
}

static gboolean onKeyPressed(GtkEventControllerKey* ctrl, guint keyval,
                             guint code, GdkModifierType state, gpointer user) {
    (void)ctrl; (void)code; (void)state; (void)user;
    int k = mapKey(keyval);
    if (k != KEY_NONE) {
        std::lock_guard<std::mutex> lk(s_keyMutex);
        s_keys.push_back(k);
        return TRUE;  // consume so GTK doesn't beep on unmapped keys
    }
    return FALSE;
}

static gboolean onCloseRequest(GtkWindow* win, gpointer user) {
    (void)win; (void)user;
    s_quit = true;  // block the raw close; the main loop performs a clean shutdown
    return TRUE;
}



#ifdef GDK_WINDOWING_WAYLAND
static int onContentSurfaceDispatcher(const void* user_data, void* target,
                                      uint32_t opcode, const struct wl_message* msg,
                                      union wl_argument* args) {
    (void)user_data; (void)target; (void)msg; (void)args;
    fprintf(stderr, "[window] content surface event opcode=%u\n", opcode);
    return 0;
}

static int headerHeightPx(GtkWidget* header) {
    if (header == nullptr) return 0;
    uint32_t scale = (uint32_t)gtk_widget_get_scale_factor(header);
    s_scale = (int)scale;
    return (int)(gtk_widget_get_height(header) * (int64_t)scale + 0.5);
}

static int headerHeightLogical(GtkWidget* header) {
    if (header == nullptr) return 0;
    return (int)gtk_widget_get_height(header);
}

// The subsurface is positioned in the toplevel wl_surface's coordinate space,
// whose origin is the outer edge of the CSD shadow -- not the visible window.
// gtk_native_get_surface_transform() gives the shadow offset (surface origin ->
// widget origin), so the content area (below the header) sits at
// (shadow + area_window_pos). bgfx renders at the origin of its own surface, so
// the subsurface must be placed exactly at the content area's top-left.
static void updateContentSubPosition() {
    if (s_contentSub == nullptr || s_win == nullptr || s_area == nullptr) return;
    if (!gtk_widget_get_realized(s_win) || !gtk_widget_get_realized(s_area)) return;
    double stx = 0.0, sty = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(s_win), &stx, &sty);
    gdouble ax = 0.0, ay = 0.0;
    gtk_widget_translate_coordinates(s_area, s_win, 0, 0, &ax, &ay);
    int px = (int)(stx + ax);
    int py = (int)(sty + ay);
    if (px != s_subPosX || py != s_subPosY) {
        fprintf(stderr, "[window] subsurface pos -> (%d, %d) [shadow (%.0f, %.0f) + area (%.0f, %.0f)]\n",
                px, py, stx, sty, ax, ay);
        wl_subsurface_set_position(s_contentSub, (int32_t)px, (int32_t)py);
        s_subPosX = px;
        s_subPosY = py;
    }
}

static void onHeaderScaleFactorChanged(GObject* obj, GParamSpec* pspec, gpointer user) {
    (void)obj; (void)pspec; (void)user;
    if (s_header != nullptr) {
        s_headerHeightPx = headerHeightPx(s_header);
    }
    updateContentSubPosition();
}

// Fired by the surface "layout" signal (frame clock LAYOUT phase). GTK's own
// handler (GtkNative's surface_layout_cb) is connected before ours and performs
// the widget allocation, so s_area already has the new size here. We resize +
// present the content subsurface synchronously, BEFORE the PAINT phase commits
// the root surface — so the content never lags the committed window size.
static void onContentLayout(GdkSurface* surface, int width, int height, gpointer user) {
    (void)surface; (void)width; (void)height; (void)user;
    if (s_frameSyncCb == nullptr || s_contentSub == nullptr) return;
    uint32_t w = 0, h = 0;
    uiWindowSize(&w, &h);
    if (w > 0 && h > 0) {
        s_frameSyncCb(w, h, s_frameSyncUser);
        // Re-align the subsurface (the CSD shadow offset can change on resize);
        // the position takes effect with the content commit the callback just did.
        updateContentSubPosition();
    }
}

static void onRegistryGlobal(void* data, struct wl_registry* registry,
                              uint32_t name, const char* interface,
                              uint32_t version) {
    (void)data;
    if (interface == nullptr) return;
    if (strcmp(interface, "wl_compositor") == 0) {
        s_compositor = (struct wl_compositor*)wl_registry_bind(registry, name,
                                                                &wl_compositor_interface, version);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        s_subcompositor = (struct wl_subcompositor*)wl_registry_bind(registry, name,
                                                                     &wl_subcompositor_interface, version);
    }
}

static void onRegistryGlobalRemove(void* data, struct wl_registry* registry,
                                   uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static bool bindWaylandGlobals(struct wl_display* wlDisplay) {
    if (s_compositor != nullptr && s_subcompositor != nullptr) return true;
    struct wl_registry* registry = wl_display_get_registry(wlDisplay);
    if (registry == nullptr) return false;

    struct wl_registry_listener listener = {};
    listener.global = onRegistryGlobal;
    listener.global_remove = onRegistryGlobalRemove;
    wl_registry_add_listener(registry, &listener, nullptr);
    wl_display_roundtrip(wlDisplay);
    wl_registry_destroy(registry);

    return s_compositor != nullptr && s_subcompositor != nullptr;
}
#endif

void uiInit(void) {
    gtk_init();
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    s_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_win), title);
    gtk_window_set_default_size(GTK_WINDOW(s_win), (int)w, (int)h);

    // A focusable child so the window reliably receives keyboard events.
    s_area = gtk_label_new("");
    gtk_widget_set_focusable(s_area, TRUE);
    gtk_window_set_child(GTK_WINDOW(s_win), s_area);

    GdkDisplay* display = gdk_display_get_default();
#ifdef GDK_WINDOWING_WAYLAND
    const bool wayland = GDK_IS_WAYLAND_DISPLAY(display);
    if (wayland) {
        g_setenv("GSK_RENDERER", "cairo", TRUE);
        g_setenv("GDK_GL_DISABLE", "1", TRUE);
        s_header = gtk_header_bar_new();
        GtkWidget* titleLabel = gtk_label_new(title);
        gtk_header_bar_set_title_widget(GTK_HEADER_BAR(s_header), titleLabel);
        gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(s_header), ":close");
        gtk_window_set_titlebar(GTK_WINDOW(s_win), s_header);
        g_signal_connect(s_header, "notify::scale-factor", G_CALLBACK(onHeaderScaleFactorChanged), nullptr);
    }
#else
    (void)display;
#endif

    // GTK4 widgets do NOT own event controllers: keep our reference alive for
    // the window's lifetime and remove it in uiShutdown() before destroy.
    s_keyCtrl = gtk_event_controller_key_new();
    g_signal_connect(s_keyCtrl, "key-pressed", G_CALLBACK(onKeyPressed), nullptr);
    gtk_widget_add_controller(s_win, s_keyCtrl);

    g_signal_connect(s_win, "close-request", G_CALLBACK(onCloseRequest), nullptr);

    gtk_window_present(GTK_WINDOW(s_win));
    gtk_widget_grab_focus(s_area);

    // Pump until the window is realized and the content child has an allocation.
    for (int i = 0; i < 200 &&
         (!gtk_widget_get_realized(s_win) || gtk_widget_get_height(s_area) == 0); i++)
        uiPumpEvents(0.005);

    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = nullptr;
    s_uiWindow.ndt = nullptr;
    GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(s_win));
    s_gdkSurface = surf;
#ifdef GDK_WINDOWING_X11
    if (surf && GDK_IS_X11_DISPLAY(display)) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"  // xid getter, still the only X11 API
        s_uiWindow.nwh = (void*)(uintptr_t)gdk_x11_surface_get_xid(GDK_SURFACE(surf));
#pragma GCC diagnostic pop
    }
#endif
#ifdef GDK_WINDOWING_WAYLAND
    if (surf && GDK_IS_WAYLAND_DISPLAY(display)) {
        // bgfx's EGL display must come from the same wl_display that owns the
        // wl_surface, otherwise wl_egl_window/surface creation fails.
        struct wl_display* wlDisplay = gdk_wayland_display_get_wl_display(display);
        struct wl_surface* rootSurface = gdk_wayland_surface_get_wl_surface(GDK_SURFACE(surf));
        s_wlDisplay = wlDisplay;
        s_rootSurface = rootSurface;
        // Resize + present the content subsurface in the LAYOUT phase (before
        // the PAINT phase commits the root), keeping it in lockstep with the
        // window size on resize. Connected after GtkNative's allocator so
        // s_area is already at the new size when this runs.
        g_signal_connect(surf, "layout", G_CALLBACK(onContentLayout), nullptr);
        bool globalsOk = (wlDisplay != nullptr) && bindWaylandGlobals(wlDisplay);
        if (wlDisplay != nullptr && rootSurface != nullptr && s_header != nullptr &&
            globalsOk) {
            s_contentSurface = wl_compositor_create_surface(s_compositor);
            s_contentSub = wl_subcompositor_get_subsurface(s_subcompositor,
                                                           s_contentSurface,
                                                           rootSurface);
            s_headerHeightPx = headerHeightPx(s_header);
            wl_surface_set_buffer_scale(s_contentSurface, (int32_t)s_scale);
            wl_subsurface_set_position(s_contentSub, 0, (int32_t)headerHeightLogical(s_header));
            wl_subsurface_place_above(s_contentSub, rootSurface);
            wl_proxy_add_dispatcher((struct wl_proxy*)s_contentSurface,
                                    onContentSurfaceDispatcher, nullptr, nullptr);
            fprintf(stderr, "[window] content surface created scale=%d headerLogical=%d headerPx=%d\n",
                    s_scale, headerHeightLogical(s_header), s_headerHeightPx);
            wl_surface_commit(rootSurface);
            wl_display_flush(wlDisplay);
            s_uiWindow.nwh = s_contentSurface;
        } else {
            s_uiWindow.nwh = rootSurface;
        }
        s_uiWindow.ndt = wlDisplay;
        s_uiWindow.nwhType = UI_NWH_WAYLAND;
    }
#endif
    return &s_uiWindow;
}

// Pump GTK events (and let the allocation update) WITHOUT committing the root
// surface. The caller commits the frame via uiCommitFrame() only after it has
// synchronously resized the GL canvas (see main.cpp), so the buffer resize and
// the window resize are presented in the same frame.
void uiPumpEvents(double timeoutSec) {
    GMainContext* ctx = g_main_context_default();
    gint64 deadline = g_get_monotonic_time() + (gint64)(timeoutSec * 1e6);
    do {
        if (g_main_context_pending(ctx))
            g_main_context_iteration(ctx, FALSE);
        else
            g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

#ifdef GDK_WINDOWING_WAYLAND
// Commit the root surface (and re-align the content subsurface). Call once per
// frame, AFTER the GL canvas has been resized for the new size.
void uiCommitFrame(void) {
    if (s_rootSurface == nullptr || s_wlDisplay == nullptr) return;
    // Keep the subsurface aligned with the content area (the CSD shadow offset
    // can only be known once the compositor has mapped the window).
    updateContentSubPosition();
    wl_surface_commit(s_rootSurface);
    wl_display_flush(s_wlDisplay);
}
#else
void uiCommitFrame(void) {}
#endif

void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData) {
    s_frameSyncCb = cb;
    s_frameSyncUser = userData;
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
    uint32_t pw = 0, ph = 0;
    if (s_area != nullptr && gtk_widget_get_realized(s_area)) {
        uint32_t scale = (uint32_t)gtk_widget_get_scale_factor(s_area);
        pw = (uint32_t)gtk_widget_get_width(s_area) * scale;
        ph = (uint32_t)gtk_widget_get_height(s_area) * scale;
    }
    if (w) *w = pw;
    if (h) *h = ph;
    if (pw != 0 && ph != 0 && !s_sizePrinted.exchange(true)) {
        fprintf(stderr, "[window] size %ux%u header=%d\n", pw, ph, s_headerHeightPx);
    }
}

void uiShutdown(void) {
#ifdef GDK_WINDOWING_WAYLAND
    // Destroy the bgfx-owned content surface only after bgfx has shut down.
    if (s_contentSub != nullptr) {
        wl_subsurface_destroy(s_contentSub);
        s_contentSub = nullptr;
    }
    if (s_contentSurface != nullptr) {
        wl_surface_destroy(s_contentSurface);
        s_contentSurface = nullptr;
    }
    if (s_subcompositor != nullptr) {
        wl_subcompositor_destroy(s_subcompositor);
        s_subcompositor = nullptr;
    }
    if (s_compositor != nullptr) {
        wl_compositor_destroy(s_compositor);
        s_compositor = nullptr;
    }
    s_headerHeightPx = 0;
#endif
    if (s_win != nullptr) {
        // The widget does not own the controller (it is not ref'd on add);
        // gtk_widget_remove_controller() performs the final unref, so we must
        // not unref it again. It must be detached before the window is
        // finalized (gtk_widget_finalize asserts no controllers remain).
        if (s_keyCtrl != nullptr) {
            gtk_widget_remove_controller(s_win, s_keyCtrl);
            s_keyCtrl = nullptr;
        }
        gtk_window_destroy(GTK_WINDOW(s_win));
        s_win = nullptr;
        s_area = nullptr;
        s_header = nullptr;
    }
}
