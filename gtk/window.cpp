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
#endif

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include "ui/window.h"

static GtkWidget* s_win = nullptr;
static GtkEventController* s_keyCtrl = nullptr;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static std::atomic<bool> s_quit{false};
static std::mutex s_keyMutex;
static std::deque<int> s_keys;

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

void uiInit(void) {
    gtk_init();
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    s_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_win), title);
    gtk_window_set_default_size(GTK_WINDOW(s_win), (int)w, (int)h);

    // A focusable child so the window reliably receives keyboard events.
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_focusable(area, TRUE);
    gtk_window_set_child(GTK_WINDOW(s_win), area);

    // GTK4 widgets do NOT own event controllers: keep our reference alive for
    // the window's lifetime and remove it in uiShutdown() before destroy.
    s_keyCtrl = gtk_event_controller_key_new();
    g_signal_connect(s_keyCtrl, "key-pressed", G_CALLBACK(onKeyPressed), nullptr);
    gtk_widget_add_controller(s_win, s_keyCtrl);

    g_signal_connect(s_win, "close-request", G_CALLBACK(onCloseRequest), nullptr);

    gtk_window_present(GTK_WINDOW(s_win));
    gtk_widget_grab_focus(area);

    // Pump until the window is realized so the native surface exists.
    for (int i = 0; i < 200 && !gtk_widget_get_realized(s_win); i++)
        uiPumpEvents(0.005);

    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = nullptr;
    GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(s_win));
#ifdef GDK_WINDOWING_X11
    if (surf && GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"  // xid getter, still the only X11 API
        s_uiWindow.nwh = (void*)(uintptr_t)gdk_x11_surface_get_xid(GDK_SURFACE(surf));
#pragma GCC diagnostic pop
    }
#endif
#ifdef GDK_WINDOWING_WAYLAND
    if (surf && GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
        s_uiWindow.nwh = gdk_wayland_surface_get_wl_surface(GDK_SURFACE(surf));
        s_uiWindow.nwhType = UI_NWH_WAYLAND;
    }
#endif
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
    if (s_win != nullptr && gtk_widget_get_realized(s_win)) {
        uint32_t scale = (uint32_t)gtk_widget_get_scale_factor(s_win);
        pw = (uint32_t)gtk_widget_get_width(s_win) * scale;
        ph = (uint32_t)gtk_widget_get_height(s_win) * scale;
    }
    if (w) *w = pw;
    if (h) *h = ph;
}

void uiShutdown(void) {
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
    }
}
