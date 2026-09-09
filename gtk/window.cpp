// GTK4 implementation of the platform-neutral UI layer (ui/window.h).
// X11 and Wayland are both supported. The renderer runs headless (offscreen):
// it publishes each frame as raw BGRA8 pixels via uiPushFrame(), and we draw
// them into a GtkDrawingArea. The window's whole content is that drawing area,
// so there is no native handle to extract and no independent surface to commit.
//
// Resizing: onDraw() is non-blocking and always covers the full allocation —
// first the renderer's clear color, then the latest published frame pinned
// top-left and clipped to the area. So a freshly exposed strip is covered by
// the clear color (which matches the scene background) immediately, with no
// desktop gap; the game thread renders the new size on its own schedule and
// the next draw shows it at full size.
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#if GTK_CHECK_VERSION(4, 18, 0)
#include <gdk/wayland/gdkwayland.h>
#else
#include <gtk/gtk-wayland.h>
#endif
#endif

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "ui/window.h"

static GtkWidget* s_win = nullptr;
static GtkWidget* s_area = nullptr;
static GtkWidget* s_header = nullptr;
static GtkEventController* s_keyCtrl = nullptr;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static int s_headerHeightPx = 0;
static std::atomic<bool> s_quit{false};
static std::mutex s_keyMutex;
static std::deque<int> s_keys;
static std::atomic<bool> s_sizePrinted{false};
static UiFrameSyncCallback s_frameSyncCb = nullptr;
static void* s_frameSyncUser = nullptr;

// Latest frame published by the game thread (raw BGRA8, top row first) and the
// UI-thread's persistent cairo surface that presents it.
struct FrameStore {
    std::mutex mu;
    std::vector<uint8_t> buf;   // BGRA pixels (written by the game thread)
    uint32_t w = 0, h = 0;
    uint64_t seq = 0;           // incremented on each uiPushFrame
    uint64_t presented = 0;    // last seq drawn by onDraw
    // UI-thread-owned, updated per draw; recreated when the frame size changes.
    cairo_surface_t* surf = nullptr;
    uint32_t surfW = 0, surfH = 0;
};
static FrameStore s_frame;

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
static int headerHeightPx(GtkWidget* header) {
    if (header == nullptr) return 0;
    uint32_t scale = (uint32_t)gtk_widget_get_scale_factor(header);
    return (int)(gtk_widget_get_height(header) * (int64_t)scale + 0.5);
}

static void onHeaderScaleFactorChanged(GObject* obj, GParamSpec* pspec, gpointer user) {
    (void)obj; (void)pspec; (void)user;
    if (s_header != nullptr) {
        s_headerHeightPx = headerHeightPx(s_header);
    }
}
#endif

// TEMP-TEST: TETRIS_RESIZE_TEST grows the window 640->1240 in steps (like a
// drag), releases the constraint, then maximizes (big grow) and unmaximizes
// (big shrink back) to exercise both directions.
static gboolean resizeTestTick(gpointer user) {
    (void)user;
    static int step = 0;
    GtkWindow* win = GTK_WINDOW(s_win);
    switch (step) {
        case 24:
            gtk_widget_set_size_request(GTK_WIDGET(win), -1, -1);  // release the constraint
            break;
        case 25:
            gtk_window_maximize(win);                  // big grow
            break;
        case 27:
            gtk_window_unmaximize(win);                // big shrink
            break;
        default:
            if (step < 24) {
                const int t = step * 600 / 23;
                gtk_widget_set_size_request(GTK_WIDGET(win), 640 + t, 640 + t);
            }
            break;
    }
    step++;
    return step <= 28 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

// Draw callback (GTK_PAINT). width/height are logical pixels; cr is pre-scaled
// by the scale factor. We paint a device-size frame 1:1 in device pixels, so we
// undo that scaling for the frame. The clear-color underlay covers the full
// allocation first (see the file header).
static void onDraw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user) {
    (void)user;
    uint32_t scale = (uint32_t)gtk_widget_get_scale_factor(GTK_WIDGET(area));
    uint32_t devW = (uint32_t)width * scale;
    uint32_t devH = (uint32_t)height * scale;

    // 1) Cover the whole allocation with the renderer's clear color, so a
    //    freshly exposed strip never shows the desktop (no gap).
    cairo_set_source_rgb(cr, 16.0 / 255.0, 16.0 / 255.0, 24.0 / 255.0);
    cairo_paint(cr);

    // 2) Draw the latest published frame at native size, pinned top-left and
    //    clipped to the area (1:1 device pixels -> undo the scale factor).
    //    The frame is BGRA8; we convert it to an opaque CAIRO_FORMAT_RGB24
    //    surface (bytes B,G,R, no alpha). The scene is fully opaque, and an
    //    explicit opaque format avoids any premultiplied-alpha misinterpretation
    //    of the straight-alpha readback bytes by the GSK renderer.
    cairo_surface_t* surf = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_frame.mu);
        if (s_frame.seq != 0 && s_frame.buf.size() == (size_t)s_frame.w * s_frame.h * 4) {
            if (s_frame.surf == nullptr || s_frame.surfW != s_frame.w ||
                s_frame.surfH != s_frame.h) {
                // Drops our reference only; any in-flight GSK reference keeps the
                // old surface alive until that frame has rendered.
                if (s_frame.surf != nullptr) cairo_surface_destroy(s_frame.surf);
                s_frame.surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
                                                          s_frame.w, s_frame.h);
                s_frame.surfW = s_frame.w;
                s_frame.surfH = s_frame.h;
            }
            // RGB24 is a 32-bit format: 4 bytes/pixel laid out [B,G,R,0]. Our
            // BGRA frame is [B,G,R,A]. A straight memcpy lands B,G,R correctly;
            // cairo ignores the 4th byte of an RGB24 surface (no alpha), so the
            // carried-over A is harmless. (Writing 3 bytes/pixel here would
            // misalign against cairo's 4-byte stride and scramble the colors.)
            memcpy(cairo_image_surface_get_data(s_frame.surf), s_frame.buf.data(),
                   s_frame.buf.size());
            surf = s_frame.surf;
            s_frame.presented = s_frame.seq;
        }
    }
    if (surf != nullptr) {
        cairo_scale(cr, 1.0 / (double)scale, 1.0 / (double)scale);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        // surf is persistent (owned by s_frame); GSK holds its own reference
        // until the frame has rendered, so we do not destroy it here.
    }

    // 3) Hand the new size to the game thread (non-blocking) so it renders this
    //    size on its next iteration.
    if (s_frameSyncCb != nullptr) {
        s_frameSyncCb(devW, devH, s_frameSyncUser);
    }
}

void uiInit(void) {
    gtk_init();
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    s_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_win), title);
    gtk_window_set_default_size(GTK_WINDOW(s_win), (int)w, (int)h);

    // The whole content is a drawing area that presents the frames the
    // (headless) renderer publishes.
    s_area = gtk_drawing_area_new();
    gtk_widget_set_focusable(s_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s_area), onDraw, nullptr, nullptr);
    gtk_window_set_child(GTK_WINDOW(s_win), s_area);

    GdkDisplay* display = gdk_display_get_default();
    // The window background shows for a moment during a resize, before the first
    // frame at the new size is painted. Match the renderer's clear color
    // (0x101018, frontend/renderer.cpp) so the transient gap is seamless.
    GtkCssProvider* cssProvider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cssProvider, "window { background: #101018; }");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(cssProvider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cssProvider);

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(display)) {
        g_setenv("GSK_RENDERER", "cairo", TRUE);
        g_setenv("GDK_GL_DISABLE", "1", TRUE);
        s_header = gtk_header_bar_new();
        GtkWidget* titleLabel = gtk_label_new(title);
        gtk_header_bar_set_title_widget(GTK_HEADER_BAR(s_header), titleLabel);
        gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(s_header), ":close");
        gtk_window_set_titlebar(GTK_WINDOW(s_win), s_header);
        g_signal_connect(s_header, "notify::scale-factor",
                         G_CALLBACK(onHeaderScaleFactorChanged), nullptr);
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
    if (std::getenv("TETRIS_RESIZE_TEST")) g_timeout_add(100, resizeTestTick, nullptr);

    // The renderer runs headless: no native window handle is needed.
    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = nullptr;
    s_uiWindow.ndt = nullptr;
    s_uiWindow.offscreen = 1;
    return &s_uiWindow;
}

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

// Nothing to commit: GTK's own frame clock presents the drawing area.
void uiCommitFrame(void) {}

void uiPushFrame(uint32_t w, uint32_t h, const uint8_t* bgra) {
    std::lock_guard<std::mutex> lk(s_frame.mu);
    const size_t n = (size_t)w * (size_t)h * 4u;
    if (s_frame.buf.size() != n) s_frame.buf.resize(n);
    if (bgra != nullptr && n != 0) memcpy(s_frame.buf.data(), bgra, n);
    s_frame.w = w;
    s_frame.h = h;
    s_frame.seq++;
}

void uiPresentFrame(void) {
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(s_frame.mu);
        fresh = (s_frame.seq != s_frame.presented);
    }
    if (fresh && s_area != nullptr) gtk_widget_queue_draw(s_area);
}

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
    {
        std::lock_guard<std::mutex> lk(s_frame.mu);
        if (s_frame.surf != nullptr) {
            cairo_surface_destroy(s_frame.surf);
            s_frame.surf = nullptr;
        }
        s_frame.buf.clear();
    }
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
