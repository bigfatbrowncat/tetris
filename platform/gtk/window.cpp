// GTK4 implementation of the platform-neutral UI layer (ui/window.h).
// X11 and Wayland, one code path — the glsync-x11-cairo-decorations design:
// a resizable window whose content is a GtkGLArea, presented by the
// compositor's frame clock.
//
//   - GDK's X11 backend implements _NET_WM_SYNC_REQUEST internally and the
//     Wayland backend paces on the frame clock: the GL area's "render"
//     signal fires once per displayed frame. No XSync code here.
//   - A GtkHeaderBar titlebar makes the window CSD by construction on both
//     WMs: the compositor draws no server-side decoration — the
//     configuration that resizes cleanly.
//   - GSK's GL-based renderers ("ngl", the default, and "gl") composite the
//     GL area's texture on their own resize schedule (a second, GTK-internal
//     instance of the "resized texture != stretched region" bug) and flicker
//     under X11/Mutter; the "cairo" renderer composites synchronously every
//     frame, and "vulkan" is used on Wayland. The choice is made in uiInit()
//     from the display's type, before GSK picks a renderer.
//
// Frame transfer (renderer -> window), GPU to GPU, no CPU copy:
//   The game renders offscreen: bgfx (Vulkan backend, headless) draws the
//   scene into a texture that is exactly the window's device-pixel size
//   (re-created on resize). Each frame its memory is exported as a dma-buf
//   (bgfx::getTextureDmaBuf — it waits for the GPU, so the fd always refers
//   to a completed frame) and handed here (uiWindowSetFrameImage, same
//   thread as the render callback). This layer imports the dma-buf into the
//   GL area's *natural* GLES2 context (C1, created by GDK) with
//   eglCreateImage(EGL_LINUX_DMA_BUF_EXT, DRM_FORMAT_ABGR8888, ...) +
//   glEGLImageTargetTexture2DOES on a persistent texture name, and blits it
//   1:1 full-frame. Only the texture crosses the Vulkan/GL boundary — the
//   buffer is shared through the kernel's dma-buf/prime; there is no second
//   GL context and no other GPU state shared between the two APIs.
//
//   C1 is GLES2 (gtk_gl_area_set_use_es(TRUE) + required version 2.0): on
//   some GPUs (NVIDIA) any on-screen 3D context for GTK4 that is not GLESv2
//   flickers. Everything this layer draws is GLES2-core.
//
// Per-frame ordering inside the render callback (the game runs on this
// thread, so all state transitions are on one thread):
//   1. glFinish() on C1 — the previous frame's blit read the scene texture
//      (the imported EGLImage); flush it so the image can be destroyed and
//      the texture re-targeted.
//   2. Destroy the previous frame's EGLImage (it owns the dma-buf fd; the
//      driver closes it on destruction).
//   3. driver(w, h, dt) — the frontend: game update, scene render (bgfx,
//      Vulkan, offscreen), bgfx::frame(), getTextureDmaBuf,
//      uiWindowSetFrameImage (the new frame's dma-buf lands in the slot).
//   4. Consume the slot: import the dma-buf into the scene texture
//      (eglCreateImageKHR + glEGLImageTargetTexture2DOES). On import
//      failure close the fd and keep the previous frame's image (the blit
//      below re-copies it — no flicker).
//   5. Clear the area's texture with the scene's background (0x101018 —
//      covers a freshly exposed resize strip, whose pixels the blit does
//      not touch) and draw a full-frame triangle sampling the scene texture
//      1:1 (pinned top-left; the texture is always exactly the window's
//      size, so nothing is stretched).
//
// Orientation: the scene texture's row 0 (v=0) is the *top* of the scene
// (Vulkan NDC +Y), and the window top is NDC y=+1 — the blit triangle's uv
// is therefore v-flipped relative to the classic desktop-GL oversized
// triangle (v=0 at the window bottom).
//
// Epoxy before EGL: it defines __khrplatform_h_ itself, which suppresses the
// duplicate khronos enum in the real KHR/khrplatform.h pulled in by EGL.
#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <libdrm/drm_fourcc.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <deque>
#include <mutex>
#include <set>
#include "ui/window.h"

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static GtkWidget* s_win = nullptr;
static GtkWidget* s_area = nullptr;
static GtkWidget* s_header = nullptr;
static GtkEventController* s_keyCtrl = nullptr;
static GtkEventController* s_focusCtrl = nullptr;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static std::mutex s_keyMutex;
// Key state changes: (logical key, 1 = pressed / 0 = released).
static std::deque<std::pair<int, int>> s_keys;
// Keyvals currently held down: the system's key auto-repeat re-fires
// "key-pressed" while a key is down, but only the first press produces a
// "down" event (the game implements its own key repeater).
static std::set<guint> s_heldKeys;
static volatile bool s_quit = false;

static UiFrameSyncCallback s_frameSyncCb = nullptr;
static void* s_frameSyncUser = nullptr;
static UiFrameDriver s_frameDriver = nullptr;
static void* s_frameDriverUser = nullptr;
static gint64 s_lastFrameNs = 0;

// C1 (the area's context, GLES2) is created by GDK at realize. The scene
// texture name is persistent; each frame's dma-buf is imported as an
// EGLImage and re-targeted onto it (see onRender).
struct GLRes {
    bool ready = false;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext areaCtx = EGL_NO_CONTEXT;
    GLuint sceneTex = 0;                // the persistent scene texture name
    EGLImage currentImage = EGL_NO_IMAGE;  // the frame bound to sceneTex
    // The full-frame blit pipeline (built in C1).
    GLuint blitProg = 0;
    GLuint blitVao = 0;
    GLuint blitVbo = 0;
    GLint blitTexLoc = -1;
};
static GLRes s_gl;

// The per-frame dma-buf handoff (uiWindowSetFrameImage -> onRender). Both
// sides run on the UI thread (the frame driver is invoked from the render
// callback), so no lock is needed.
struct FrameSlot {
    int32_t fd = -1;
    uint64_t modifier = 0;
    uint32_t pitch = 0;
    bool valid = false;
};
static FrameSlot s_frame;

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
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
    const int k = mapKey(keyval);
    if (k != KEY_NONE) {
        std::lock_guard<std::mutex> lk(s_keyMutex);
        // insert() reports whether the keyval was new: the system's
        // auto-repeat re-fires this for a key that is already down — only
        // the first press becomes a "down" event.
        if (s_heldKeys.insert(keyval).second)
            s_keys.push_back({k, 1});
        return TRUE;  // consume so GTK doesn't beep on unmapped keys
    }
    return FALSE;
}

static gboolean onKeyReleased(GtkEventControllerKey* ctrl, guint keyval,
                               guint code, GdkModifierType state, gpointer user) {
    (void)ctrl; (void)code; (void)state; (void)user;
    const int k = mapKey(keyval);
    if (k != KEY_NONE) {
        std::lock_guard<std::mutex> lk(s_keyMutex);
        if (s_heldKeys.erase(keyval) != 0)
            s_keys.push_back({k, 0});
        return TRUE;
    }
    return FALSE;
}

// The window lost focus: the key-ups of still-held keys are not guaranteed
// to arrive — flush them so the game's "held" state cannot go stale.
static void onFocusOut(GtkEventControllerFocus* ctrl, gpointer user) {
    (void)ctrl; (void)user;
    std::lock_guard<std::mutex> lk(s_keyMutex);
    for (guint kv : s_heldKeys) {
        const int k = mapKey(kv);
        if (k != KEY_NONE) s_keys.push_back({k, 0});
    }
    s_heldKeys.clear();
}

static gboolean onCloseRequest(GtkWindow* win, gpointer user) {
    (void)win; (void)user;
    s_quit = true;  // block the raw close; the main loop performs a clean shutdown
    return TRUE;
}

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

// ---------------------------------------------------------------------------
// the full-frame blit (C1, GLES2): one oversized triangle covering the
// viewport, sampling the scene texture 1:1. The scene texture is always
// exactly the window's device size (re-created by the renderer on resize,
// re-imported every frame), so the triangle is a pure 1:1 copy — no
// scaling, no stretching of a stale frame.
//
// The uv is v-flipped (see the file header): the image top (v=0) lands at
// the window top (NDC y=+1).
// ---------------------------------------------------------------------------
static const char *BLIT_VS =
    "attribute vec2 in_pos;   /* NDC, [-1, 1] */\n"
    "attribute vec2 in_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = in_uv;\n"
    "    gl_Position = vec4(in_pos, 0.0, 1.0);\n"
    "}\n";

static const char *BLIT_FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_tex;\n"
    "void main() { gl_FragColor = vec4(texture2D(s_tex, v_uv).rgb, 1.0); }\n";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        g_warning("blit shader compile failed: %s", log);
    }
    return sh;
}

// C1 is current. Builds the blit program + VAO/VBO once.
static void blitBuild(void) {
    if (s_gl.blitProg != 0) return;

    GLuint vs = compileShader(GL_VERTEX_SHADER,   BLIT_VS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, BLIT_FS);

    s_gl.blitProg = glCreateProgram();
    glAttachShader(s_gl.blitProg, vs);
    glAttachShader(s_gl.blitProg, fs);
    glLinkProgram(s_gl.blitProg);

    GLint linked = 0;
    glGetProgramiv(s_gl.blitProg, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(s_gl.blitProg, sizeof log, NULL, log);
        g_warning("blit program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    s_gl.blitTexLoc = glGetUniformLocation(s_gl.blitProg, "s_tex");

    // One triangle covering the viewport (uv overscans to 2; CLAMP_TO_EDGE
    // makes the edge texels repeat, so no seam). v is flipped: the image
    // top (v=0) lands at the window top (NDC y=+1).
    const GLfloat verts[12] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         3.0f, -1.0f, 2.0f, 1.0f,
        -1.0f,  3.0f, 0.0f, -1.0f,
    };
    glGenVertexArrays(1, &s_gl.blitVao);
    glGenBuffers(1, &s_gl.blitVbo);
    glBindVertexArray(s_gl.blitVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_gl.blitVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * 2 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * 2 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// GL setup: grab the area's context (C1) and build the blit pipeline + the
// persistent scene texture. Called once after the area is realized.
// ---------------------------------------------------------------------------
static bool setupGL(void) {
    gtk_gl_area_make_current(GTK_GL_AREA(s_area));
    GError* err = gtk_gl_area_get_error(GTK_GL_AREA(s_area));
    if (err != NULL) {
        g_warning("GL area realize error: %s", err->message);
        return false;
    }

    // The dma-buf import (eglCreateImage + glEGLImageTargetTexture2DOES)
    // needs an EGL context. GDK creates the area's context with GLX when the
    // X11 server has no usable EGL — fail loudly in that case.
    EGLContext c1 = eglGetCurrentContext();
    if (c1 == EGL_NO_CONTEXT) {
        fprintf(stderr,
            "[window] FATAL: the GTK GL context is not EGL (GLX fallback on this"
            " X11 server). The dma-buf frame import requires an EGL context;\n"
            "import into a GLX context is not possible.\n");
        return false;
    }

    s_gl.display = eglGetCurrentDisplay();
    s_gl.areaCtx = c1;

    // The display must be able to import dma-bufs (Mesa: the DRI2 driver).
    const char* exts = eglQueryString(s_gl.display, EGL_EXTENSIONS);
    if (exts == nullptr || strstr(exts, "EGL_EXT_image_dma_buf_import") == nullptr) {
        fprintf(stderr,
            "[window] FATAL: the EGL display lacks EGL_EXT_image_dma_buf_import"
            " (needed to import the renderer's Vulkan frames as dma-bufs).\n");
        return false;
    }

    // The persistent scene texture name. Its storage is re-targeted every
    // frame with glEGLImageTargetTexture2DOES (the imported dma-buf — no
    // glTexImage2D, the storage IS the imported buffer); the parameters are
    // set once and survive the re-target.
    glGenTextures(1, &s_gl.sceneTex);
    glBindTexture(GL_TEXTURE_2D, s_gl.sceneTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // The blit pipeline (C1).
    blitBuild();
    s_gl.ready = true;
    return true;
}

// C1 is current. Imports the dma-buf (fd, modifier, pitch — RGBA8, w x h)
// as an EGLImage. DRM_FORMAT_ABGR8888: the fourcc names list the channels
// MSB to LSB, so the little-endian memory layout is [R, G, B, A] —
// byte-identical to the renderer's VK_FORMAT_R8G8B8A8_UNORM image. The
// caller keeps ownership of the fd in both outcomes (the image pins the GEM
// object by handle, not the fd) and must close it after this returns.
static EGLImage importDmaBuf(int32_t fd, uint64_t modifier, uint32_t pitch,
                             uint32_t w, uint32_t h) {
    const EGLAttrib attrs[] = {
        EGL_WIDTH, (EGLAttrib)w,
        EGL_HEIGHT, (EGLAttrib)h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, (EGLAttrib)fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(modifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(modifier >> 32),
        EGL_NONE,
    };
    // eglCreateImage (core EGL 1.5) — the KHR alias is not a separate
    // exported symbol in Mesa's libEGL.
    return eglCreateImage(s_gl.display, EGL_NO_CONTEXT,
                          EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

// ---------------------------------------------------------------------------
// the render callback — the whole present path (see the file header).
// ---------------------------------------------------------------------------
static gboolean onRender(GtkGLArea* area, GdkGLContext* context, gpointer user) {
    (void)area; (void)context; (void)user;
    if (!s_gl.ready) return TRUE;

    // C1 is current (GTK made it so before emitting "render"); the area's
    // FBO is bound (attach_buffers).
    const int scale = gtk_widget_get_scale_factor(GTK_WIDGET(s_area));
    const uint32_t w = (uint32_t)gtk_widget_get_width(GTK_WIDGET(s_area)) * (uint32_t)scale;
    const uint32_t h = (uint32_t)gtk_widget_get_height(GTK_WIDGET(s_area)) * (uint32_t)scale;
    if (w == 0 || h == 0) return TRUE;

    // 1) The previous frame's blit read the scene texture (the imported
    //    EGLImage); flush it (glFinish() flushes the FBO) so the image can
    //    be destroyed and the texture re-targeted.
    glFinish();

    // 2) The previous frame's image is no longer read — destroy it. (The
    //    dma-buf fd was already closed at import; the image holds only a
    //    GEM-handle reference to the buffer.)
    if (s_gl.currentImage != EGL_NO_IMAGE) {
        eglDestroyImage(s_gl.display, s_gl.currentImage);
        s_gl.currentImage = EGL_NO_IMAGE;
    }

    // 3) Drive the game: update + render the scene (bgfx, Vulkan,
    //    offscreen) + bgfx::frame() + getTextureDmaBuf +
    //    uiWindowSetFrameImage. The driver leaves the new frame's dma-buf
    //    in the slot (and only a completed one — it waits for the GPU).
    if (s_frameDriver != nullptr) {
        double dt = 0.0;
        const gint64 now = g_get_monotonic_time();
        if (s_lastFrameNs != 0) dt = (now - s_lastFrameNs) * 1e-6;
        s_lastFrameNs = now;
        s_frameDriver(w, h, dt, s_frameDriverUser);
    }

    // 4) Consume the slot: import the new frame into the scene texture.
    //    On import failure close the fd and keep the previous frame's image
    //    (the blit below re-copies it — no flicker).
    if (s_frame.valid) {
        const int32_t fd = s_frame.fd;
        const uint64_t modifier = s_frame.modifier;
        const uint32_t pitch = s_frame.pitch;
        s_frame.valid = false;
        EGLImage img = importDmaBuf(fd, modifier, pitch, w, h);
        // Per EGL_EXT_image_dma_buf_import the client keeps ownership of the
        // dma-buf fd across eglCreateImage — the image pins the GEM object by
        // handle, not the fd — so close it here on both outcomes. Skipping
        // this on success leaked one fd every frame.
        ::close(fd);
        if (img != EGL_NO_IMAGE) {
            glBindTexture(GL_TEXTURE_2D, s_gl.sceneTex);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
            glBindTexture(GL_TEXTURE_2D, 0);
            s_gl.currentImage = img;
        } else {
            fprintf(stderr, "[window] dma-buf import failed (error 0x%x); keeping the previous frame\n",
                    eglGetError());
        }
    }

    // 5) Present: clear the area's texture with the scene's background
    //    (covers a freshly exposed resize strip) and copy the scene texture
    //    full-frame, 1:1.
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(16.0f / 255.0f, 16.0f / 255.0f, 24.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_gl.currentImage != EGL_NO_IMAGE) {
        glUseProgram(s_gl.blitProg);
        glUniform1i(s_gl.blitTexLoc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_gl.sceneTex);
        glBindVertexArray(s_gl.blitVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }

    if (getenv("TETRIS_EGL_DEBUG")) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            const char* r = (const char*)glGetString(GL_RENDERER);
            fprintf(stderr, "[dbg] C1 renderer: %s\n", r ? r : "(null)");
        }
        // fboTex is the texture id GTK attached to the area FBO — exactly
        // the image GSK will present this frame.
        GLint fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        GLuint fboTex = 0;
        if (fbo != 0)
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                                  (GLint*)&fboTex);
        fprintf(stderr, "[dbg] frame w=%u h=%u img=%d fbo=%d fboTex=%u sceneTex=%u glerr=0x%x\n",
                w, h, (int)(s_gl.currentImage != EGL_NO_IMAGE), (int)fbo,
                (unsigned)fboTex, (unsigned)s_gl.sceneTex, (unsigned)glGetError());
    }

    return TRUE;  // we handled the draw
}

// ---------------------------------------------------------------------------
// frame pump: GtkGLArea emits "render" only when the widget is drawn, and a
// static scene never invalidates itself (queue_draw *during* the snapshot
// does not schedule the next frame on GTK 4.18 / GNOME Shell). A short timer
// keeps the area invalidated, so the frame clock ticks continuously; the
// actual pacing is the compositor's (the render callback runs once per
// displayed frame, and the game uses the real dt).
// ---------------------------------------------------------------------------
static gboolean framePumpTick(gpointer user) {
    (void)user;
    if (s_area != nullptr) gtk_widget_queue_draw(s_area);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// the UI layer API
// ---------------------------------------------------------------------------
void uiInit(void) {
    gtk_init();

    // Pick the GSK renderer from the display type, before GSK picks one:
    // the GL-based GSK renderers resize the GL area's texture on their own
    // schedule (flicker under X11/Mutter); "cairo" composites synchronously
    // (X11), "vulkan" on Wayland.
    GdkDisplay* display = gdk_display_get_default();
    if (display != NULL) {
        const char* backend_type = G_OBJECT_TYPE_NAME(display);
        // An explicit GSK_RENDERER in the environment wins (test hook).
        if (g_getenv("GSK_RENDERER") == NULL) {
            if (g_strcmp0(backend_type, "GdkWaylandDisplay") == 0) {
                g_setenv("GSK_RENDERER", "vulkan", TRUE);
            } else if (g_strcmp0(backend_type, "GdkX11Display") == 0) {
                g_setenv("GSK_RENDERER", "cairo", TRUE);
            }
        }
    }
}

const UiWindow* uiCreateWindow(uint32_t w, uint32_t h, const char* title) {
    s_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(s_win), title);
    gtk_window_set_default_size(GTK_WINDOW(s_win), (int)w, (int)h);

    // A GtkHeaderBar as the titlebar makes this window CSD: the compositor
    // draws no server-side decoration at all, and the usual window controls
    // are drawn by GTK itself. Both X11 and Wayland.
    s_header = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(s_header), gtk_label_new(title));
    gtk_window_set_titlebar(GTK_WINDOW(s_win), s_header);

    s_area = gtk_gl_area_new();
    // The area's natural context: GLES 2.0 (on some GPUs — e.g. NVIDIA — an
    // on-screen 3D context that is not GLESv2 flickers). The blit pipeline
    // is GLES2-core (VAO/VBO, one oversized triangle, a 1:1 texture copy).
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(s_area), GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(GTK_GL_AREA(s_area), 2, 0);
    gtk_widget_set_focusable(s_area, TRUE);
    g_signal_connect(s_area, "render", G_CALLBACK(onRender), nullptr);
    gtk_window_set_child(GTK_WINDOW(s_win), s_area);

    // The window background shows for a moment before the first frame is
    // painted. Match the renderer's clear color (0x101018) so the transient
    // gap is seamless.
    GdkDisplay* display = gdk_display_get_default();
    GtkCssProvider* cssProvider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cssProvider, "window { background: #101018; }");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(cssProvider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cssProvider);

    // GTK4 widgets do NOT own event controllers: keep our references alive
    // for the window's lifetime and remove them in uiShutdown() before
    // destroy.
    s_keyCtrl = gtk_event_controller_key_new();
    g_signal_connect(s_keyCtrl, "key-pressed", G_CALLBACK(onKeyPressed), nullptr);
    g_signal_connect(s_keyCtrl, "key-released", G_CALLBACK(onKeyReleased), nullptr);
    gtk_widget_add_controller(s_win, s_keyCtrl);

    s_focusCtrl = gtk_event_controller_focus_new();
    g_signal_connect(s_focusCtrl, "focus-out", G_CALLBACK(onFocusOut), nullptr);
    gtk_widget_add_controller(s_win, s_focusCtrl);

    g_signal_connect(s_win, "close-request", G_CALLBACK(onCloseRequest), nullptr);

    gtk_window_present(GTK_WINDOW(s_win));
    gtk_widget_grab_focus(s_area);

    // Pump until the area is realized and has an allocation.
    for (int i = 0; i < 200 &&
          (!gtk_widget_get_realized(s_area) || gtk_widget_get_height(s_area) == 0); i++)
        uiPumpEvents(0.005);

    if (std::getenv("TETRIS_RESIZE_TEST")) g_timeout_add(100, resizeTestTick, nullptr);

    // The continuous frame pump (see framePumpTick).
    g_timeout_add(16, framePumpTick, nullptr);

    if (!setupGL()) {
        GError* gerr = nullptr;
        g_set_error(&gerr, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
                    "EGL setup failed");
        gtk_gl_area_set_error(GTK_GL_AREA(s_area), gerr);
        g_clear_error(&gerr);
    } else {
        // Kick the frame loop: the first "render" may have fired before the
        // GL setup was ready (and returned early), so invalidate once more.
        gtk_widget_queue_draw(s_area);
    }

    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = nullptr;
    s_uiWindow.ndt = nullptr;
    s_uiWindow.offscreen = 1;  // the renderer renders offscreen (bgfx, Vulkan)
    if (getenv("TETRIS_EGL_DEBUG"))
        fprintf(stderr, "[dbg] ready=%d display=%p ctx=%p sceneTex=%u\n",
                (int)s_gl.ready, (void*)s_gl.display, (void*)s_gl.areaCtx,
                (unsigned)s_gl.sceneTex);
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

// Nothing to commit: GTK's frame clock presents the GL area's texture.
void uiCommitFrame(void) {}

void uiSetFrameSyncCallback(UiFrameSyncCallback cb, void* userData) {
    s_frameSyncCb = cb;
    s_frameSyncUser = userData;
}

void uiSetFrameDriver(UiFrameDriver cb, void* userData) {
    s_frameDriver = cb;
    s_frameDriverUser = userData;
}

// Windows composition path only; never called on the GTK path.
void* uiWindowResize(uint32_t w, uint32_t h) {
    (void)w; (void)h;
    return nullptr;
}

int uiWindowReadBackbuffer(uint32_t w, uint32_t h, void* out, uint32_t outSize) {
    (void)w; (void)h; (void)out; (void)outSize;
    return -1;
}

// Linux GL area path: the renderer calls this from the frame driver (inside
// this layer's render callback — same thread, no lock) to hand over the
// frame just rendered as a dma-buf. The fd is consumed by the next import
// in onRender and closed there (the client keeps ownership across
// eglCreateImage in both outcomes).
void uiWindowSetFrameImage(int32_t fd, uint64_t modifier, uint32_t pitch) {
    if (fd < 0) return;
    // Defensive: a pending fd would mean a previous import never consumed it
    // (an early-returned frame) — close it so it cannot leak.
    if (s_frame.valid && s_frame.fd >= 0) {
        ::close(s_frame.fd);
    }
    s_frame.fd = fd;
    s_frame.modifier = modifier;
    s_frame.pitch = pitch;
    s_frame.valid = true;
}

int uiShouldQuit(void) {
    return s_quit ? 1 : 0;
}

int uiPopKey(int* down) {
    std::lock_guard<std::mutex> lk(s_keyMutex);
    if (s_keys.empty()) {
        if (down) *down = 0;
        return KEY_NONE;
    }
    std::pair<int, int> e = s_keys.front();
    s_keys.pop_front();
    if (down) *down = e.second;
    return e.first;
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
}

void uiShutdown(void) {
    if (s_gl.ready) {
        // C1 current for the objects it owns (scene texture, blit pipeline,
        // the imported frame image).
        if (eglGetCurrentContext() != s_gl.areaCtx)
            eglMakeCurrent(s_gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_gl.areaCtx);
        if (s_gl.currentImage != EGL_NO_IMAGE) {
            eglDestroyImage(s_gl.display, s_gl.currentImage);
            s_gl.currentImage = EGL_NO_IMAGE;
        }
        if (s_gl.sceneTex != 0) {
            glDeleteTextures(1, &s_gl.sceneTex);
            s_gl.sceneTex = 0;
        }
        if (s_gl.blitProg != 0) glDeleteProgram(s_gl.blitProg);
        if (s_gl.blitVbo != 0) glDeleteBuffers(1, &s_gl.blitVbo);
        if (s_gl.blitVao != 0) glDeleteVertexArrays(1, &s_gl.blitVao);
        s_gl.ready = false;
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
        if (s_focusCtrl != nullptr) {
            gtk_widget_remove_controller(s_win, s_focusCtrl);
            s_focusCtrl = nullptr;
        }
        gtk_window_destroy(GTK_WINDOW(s_win));
        s_win = nullptr;
        s_area = nullptr;
        s_header = nullptr;
    }
}
