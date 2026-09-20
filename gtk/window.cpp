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
//   C1 — the GL area's context, created by GDK. The render callback runs
//        with C1 current; the callback draws into the area's own offscreen
//        texture, which GSK then composites into the window.
//   C2 — a 4.3-core context sharing C1 (same share group), current in
//        surfaceless mode (EGL_KHR_surfaceless_context) — on a 1x1 pbuffer on
//        displays lacking that extension. bgfx adopts C2 (via
//        Init.platformData.context) *and the surface current on it*, and skips
//        every swap when that surface is EGL_NO_SURFACE. It renders the scene
//        into sceneTex (a texture this layer owns) through an FBO; no view
//        ever targets a default framebuffer, so nothing is ever presented.
//   sceneTex — RGBA8, kept at the window's device-pixel size. Because C1 and
//        C2 share, the callback can bind the name bgfx wrote in the other
//        context.
//
// Per-frame ordering inside the render callback (bgfx is single-threaded and
// the game runs on this thread, so all GL state transitions are on one
// thread):
//   1. glFinish() on C1 — the previous frame's blit read sceneTex; flush it
//      so the texture may be rewritten. glFinish() flushes the FBO/texture
//      writes of the current context's stream.
//   2. Re-create sceneTex at the new size if the window resized (a freshly
//      exposed strip must never show the desktop: step 3 clears it with the
//      scene's background color first). This runs raw GL on C1 and must
//      restore the FBO binding it finds (on a resize frame that is the area
//      FBO GTK bound in attach_buffers), or step 4's blit would miss it.
//   3. driver(w, h, dt) — the frontend: game update, scene render into
//      sceneTex with C2, bgfx::frame(), glFinish() on C2.
//   4. Re-bind C1, clear the area's texture with the scene's clear color and
//      draw a full-frame quad sampling sceneTex 1:1 (pinned top-left; the
//      texture is always exactly the window size, so nothing is stretched).
// Epoxy before EGL: it defines __khrplatform_h_ itself, which suppresses the
// duplicate khronos enum in the real KHR/khrplatform.h pulled in by EGL.
#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include "ui/window.h"

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static GtkWidget* s_win = nullptr;
static GtkWidget* s_area = nullptr;
static GtkWidget* s_header = nullptr;
static GtkEventController* s_keyCtrl = nullptr;
static UiWindow s_uiWindow = {UI_NWH_DEFAULT, nullptr};
static std::mutex s_keyMutex;
static std::deque<int> s_keys;
static volatile bool s_quit = false;

static UiFrameSyncCallback s_frameSyncCb = nullptr;
static void* s_frameSyncUser = nullptr;
static UiFrameDriver s_frameDriver = nullptr;
static void* s_frameDriverUser = nullptr;
static gint64 s_lastFrameNs = 0;

// The GL resource pair. C1 (the area's context) is created by GDK at
// realize; C2 and the scene texture are created here, both sharing C1's
// share group.
struct GLRes {
    bool ready = false;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext areaCtx = EGL_NO_CONTEXT;    // C1
    EGLSurface areaSurf = EGL_NO_SURFACE;   // C1's window surface
    EGLContext bgfxCtx = EGL_NO_CONTEXT;    // C2 (shares C1)
    EGLSurface pbuffer  = EGL_NO_SURFACE;   // C2's 1x1 placeholder surface
    GLuint sceneTex = 0;                    // the shared scene texture
    uint32_t texW = 0, texH = 0;
    // The full-frame blit pipeline (built in C1).
    GLuint blitProg = 0;
    GLuint blitVao = 0;
    GLuint blitVbo = 0;
    GLint blitTexLoc = -1;
};
static GLRes s_gl;

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
// the full-frame blit (C1): one oversized triangle covering the viewport,
// sampling sceneTex 1:1. sceneTex is always exactly the window's device size
// (re-created on resize, step 2), so the quad is a pure 1:1 copy — no
// scaling, no stretching of a stale frame.
// ---------------------------------------------------------------------------
static const char *BLIT_VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 in_pos;   /* NDC, [-1, 1] */\n"
    "layout(location = 1) in vec2 in_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = in_uv;\n"
    "    gl_Position = vec4(in_pos, 0.0, 1.0);\n"
    "}\n";

static const char *BLIT_FS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D s_tex;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = vec4(texture(s_tex, v_uv).rgb, 1.0); }\n";

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
    // makes the edge texels repeat, so no seam).
    const GLfloat verts[12] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         3.0f, -1.0f, 2.0f, 0.0f,
        -1.0f,  3.0f, 0.0f, 2.0f,
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

// C1 is current. Keeps sceneTex allocated at (w, h); on (re)creation it is
// filled with the scene's clear color so a blit before the first rendered
// frame (or a freshly exposed resize strip) shows the background, never
// undefined storage.
static void sceneTexEnsure(uint32_t w, uint32_t h) {
    if (s_gl.sceneTex == 0) glGenTextures(1, &s_gl.sceneTex);
    if (s_gl.sceneTex == 0) return;
    if (s_gl.texW == w && s_gl.texH == h) return;

    glBindTexture(GL_TEXTURE_2D, s_gl.sceneTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s_gl.texW = w;
    s_gl.texH = h;

    // Clear the fresh storage with the scene's background (0x101018).
    // Save/restore the FBO binding: on a resize frame GTK has already bound
    // the GL area's FBO (attach_buffers, before the render signal) and the
    // full-frame blit later draws into it. Binding 0 here would send the
    // blit to the default framebuffer and leave the freshly re-allocated
    // area texture holding the previous frame re-pitched at the new size —
    // the per-row horizontal shift seen while resizing.
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_gl.sceneTex, 0);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(16.0f / 255.0f, 16.0f / 255.0f, 24.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glDeleteFramebuffers(1, &fbo);
}

// ---------------------------------------------------------------------------
// GL setup: grab the area's context (C1), create C2 sharing it on a pbuffer,
// and the scene texture. Called once after the area is realized.
// ---------------------------------------------------------------------------
static bool setupGL(uint32_t w, uint32_t h) {
    gtk_gl_area_make_current(GTK_GL_AREA(s_area));
    GError* err = gtk_gl_area_get_error(GTK_GL_AREA(s_area));
    if (err != NULL) {
        g_warning("GL area realize error: %s", err->message);
        return false;
    }

    // GDK creates the area's context with GLX when the X11 server has no
    // usable EGL. This bgfx build's GL backend is EGL-only, and a GLX
    // context cannot share with an EGL one — fail loudly in that case.
    EGLContext c1 = eglGetCurrentContext();
    if (c1 == EGL_NO_CONTEXT) {
        fprintf(stderr,
            "[window] FATAL: the GTK GL context is not EGL (GLX fallback on this"
            " X11 server). The EGL bgfx backend requires an EGL display context;"
            " texture sharing with a GLX context is not possible.\n");
        return false;
    }

    s_gl.display  = eglGetCurrentDisplay();
    s_gl.areaCtx  = c1;
    s_gl.areaSurf = eglGetCurrentSurface(EGL_DRAW);

    // The client API is bound per-thread, per-display. This thread must be
    // bound to the desktop OpenGL API to create desktop contexts (GDK does
    // the same before creating the area's context).
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[window] FATAL: eglBindAPI(EGL_OPENGL_API) failed (error 0x%x)\n", eglGetError());
        return false;
    }

    // C2 needs a way to be current. Prefer surfaceless
    // (EGL_KHR_surfaceless_context): the Wayland platform display the GL
    // area's context lives on exposes no PBUFFER configs, and surfaceless
    // needs no surface at all.
    const char* exts = eglQueryString(s_gl.display, EGL_EXTENSIONS);
    const bool surfaceless =
        exts != nullptr && strstr(exts, "EGL_KHR_surfaceless_context") != nullptr;

    // A config, when one is needed (the pbuffer fallback).
    EGLConfig cfg = NULL;
    if (!surfaceless) {
        const EGLint cfgAttrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };
        EGLint num = 0;
        if (!eglChooseConfig(s_gl.display, cfgAttrs, &cfg, 1, &num) || num < 1) {
            fprintf(stderr, "[window] FATAL: no EGL pbuffer config (error 0x%x)\n", eglGetError());
            return false;
        }
    }

    // C2: desktop core 4.3 (bgfx's OpenGL requirement), sharing C1's group.
    // Some platform displays (Wayland) expose only *configless* contexts —
    // their configs exist for visual matching but cannot create contexts
    // (GDK itself creates the area's context configless). Try config-based
    // first, then configless (EGL_MESA_configless_context).
    if (getenv("TETRIS_EGL_DEBUG")) {
        const char* v1 = (const char*)glGetString(GL_VERSION);
        fprintf(stderr, "[dbg] C1 GL version: %s\n", v1 ? v1 : "(null)");
        EGLint c1cfgId = 0;
        eglQueryContext(s_gl.display, c1, EGL_CONFIG_ID, &c1cfgId);
        fprintf(stderr, "[dbg] C1 config id=%d\n", c1cfgId);
        // a real window config, like GDK's
        EGLint wcfg[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        EGLConfig wc = NULL; EGLint wn = 0;
        eglChooseConfig(s_gl.display, wcfg, &wc, 1, &wn);
        fprintf(stderr, "[dbg] window config: %d found\n", wn);
        for (int maj = 3; maj <= 5; maj++)
        for (int min = 0; min <= 6; min++) {
            if (maj == 3 && min < 3) continue;
            if (maj == 5 && min > 2) continue;
            if (wc) {
                EGLint a[] = { EGL_CONTEXT_MAJOR_VERSION, maj, EGL_CONTEXT_MINOR_VERSION, min, EGL_NONE };
                EGLContext t = eglCreateContext(s_gl.display, wc, EGL_NO_CONTEXT, a);
                if (t != EGL_NO_CONTEXT) {
                    fprintf(stderr, "[dbg] withconfig %d.%d OK (no profile) err=0x%x\n", maj, min, eglGetError());
                    eglDestroyContext(s_gl.display, t);
                }
            }
            EGLint ap[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                            EGL_CONTEXT_MAJOR_VERSION, maj, EGL_CONTEXT_MINOR_VERSION, min, EGL_NONE };
            EGLContext tp = eglCreateContext(s_gl.display, NULL, EGL_NO_CONTEXT, ap);
            if (tp != EGL_NO_CONTEXT) {
                fprintf(stderr, "[dbg] configless %d.%d OK (core)\n", maj, min);
                eglDestroyContext(s_gl.display, tp);
            }
        }
        EGLint as_[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
        EGLContext ts = eglCreateContext(s_gl.display, NULL, c1, as_);
        fprintf(stderr, "[dbg] configless 4.3-core share C1: %p err=0x%x\n", (void*)ts, eglGetError());
        if (ts != EGL_NO_CONTEXT) eglDestroyContext(s_gl.display, ts);
        if (wc) {
            EGLContext tw = eglCreateContext(s_gl.display, wc, c1, as_);
            fprintf(stderr, "[dbg] withconfig 4.3-core share C1: %p err=0x%x\n", (void*)tw, eglGetError());
            if (tw != EGL_NO_CONTEXT) eglDestroyContext(s_gl.display, tw);
        }
    }

    const EGLint ctxAttrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_NONE,
    };
    s_gl.bgfxCtx = eglCreateContext(s_gl.display, cfg, c1, ctxAttrs);
    if (s_gl.bgfxCtx == EGL_NO_CONTEXT)
        s_gl.bgfxCtx = eglCreateContext(s_gl.display, NULL, c1, ctxAttrs);
    if (s_gl.bgfxCtx == EGL_NO_CONTEXT)
        s_gl.bgfxCtx = eglCreateContext(s_gl.display, NULL, c1, NULL);
    if (s_gl.bgfxCtx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[window] FATAL: cannot create the shared EGL context (error 0x%x)\n", eglGetError());
        return false;
    }

    if (!surfaceless) {
        const EGLint pbAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        s_gl.pbuffer = eglCreatePbufferSurface(s_gl.display, cfg, pbAttrs);
        if (s_gl.pbuffer == EGL_NO_SURFACE) {
            fprintf(stderr, "[window] FATAL: cannot create the 1x1 pbuffer (error 0x%x)\n", eglGetError());
            return false;
        }
    }
    // Make C2 current — on the pbuffer, or surfaceless (EGL_NO_SURFACE).
    // bgfx adopts whichever surface is current at bgfx::init time.
    if (!eglMakeCurrent(s_gl.display, s_gl.pbuffer, s_gl.pbuffer, s_gl.bgfxCtx)) {
        fprintf(stderr, "[window] FATAL: cannot make the shared context current (error 0x%x)\n", eglGetError());
        return false;
    }

    // The scene texture (shared group: visible from C1 and C2 alike).
    glGenTextures(1, &s_gl.sceneTex);
    sceneTexEnsure(w, h);

    // The blit pipeline lives in C1.
    if (!eglMakeCurrent(s_gl.display, s_gl.areaSurf, s_gl.areaSurf, s_gl.areaCtx)) {
        fprintf(stderr, "[window] FATAL: cannot re-bind the area context (error 0x%x)\n", eglGetError());
        return false;
    }
    blitBuild();
    s_gl.ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// the render callback — the whole present path (see the file header).
// ---------------------------------------------------------------------------
static gboolean onRender(GtkGLArea* area, GdkGLContext* context, gpointer user) {
    (void)area; (void)context; (void)user;
    if (getenv("TETRIS_EGL_DEBUG")) {
        static int n = 0;
        fprintf(stderr, "[dbg] onRender #%d ready=%d\n", ++n, (int)s_gl.ready);
    }
    if (!s_gl.ready) return TRUE;

    // C1 is current (GTK made it so before emitting "render").
    const int scale = gtk_widget_get_scale_factor(GTK_WIDGET(s_area));
    const uint32_t w = (uint32_t)gtk_widget_get_width(GTK_WIDGET(s_area)) * (uint32_t)scale;
    const uint32_t h = (uint32_t)gtk_widget_get_height(GTK_WIDGET(s_area)) * (uint32_t)scale;
    if (w == 0 || h == 0) return TRUE;

    // 1) The previous frame's blit read sceneTex from C1's stream; flush it
    //    so the texture may be re-written (glFinish() flushes the FBO).
    glFinish();

    // 2) Keep the shared scene texture at the current device size.
    sceneTexEnsure(w, h);

    // 3) Drive the game: update + render the scene into sceneTex (C2) +
    //    bgfx::frame() + glFinish() on C2.
    if (s_frameDriver != nullptr) {
        double dt = 0.0;
        const gint64 now = g_get_monotonic_time();
        if (s_lastFrameNs != 0) dt = (now - s_lastFrameNs) * 1e-6;
        s_lastFrameNs = now;
        s_frameDriver(w, h, dt, s_frameDriverUser);
    }

    // 4) Present: re-bind C1, clear with the scene's background (covers a
    //    freshly exposed resize strip) and copy sceneTex full-frame, 1:1.
    if (!eglMakeCurrent(s_gl.display, s_gl.areaSurf, s_gl.areaSurf, s_gl.areaCtx)) {
        fprintf(stderr, "[window] FATAL: cannot re-bind the area context (error 0x%x)\n", eglGetError());
        return TRUE;
    }
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(16.0f / 255.0f, 16.0f / 255.0f, 24.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_gl.sceneTex != 0) {
        glUseProgram(s_gl.blitProg);
        glUniform1i(s_gl.blitTexLoc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_gl.sceneTex);
        glBindVertexArray(s_gl.blitVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
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
        if (g_strcmp0(backend_type, "GdkWaylandDisplay") == 0) {
            g_setenv("GSK_RENDERER", "vulkan", TRUE);
        } else if (g_strcmp0(backend_type, "GdkX11Display") == 0) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
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
    // Desktop GL 3.3 core for the blit pipeline (the scene texture and the
    // full-frame quad); the game itself renders in the 4.3 context C2.
    gtk_gl_area_set_use_es(GTK_GL_AREA(s_area), FALSE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(s_area), 3, 3);
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

    // GTK4 widgets do NOT own event controllers: keep our reference alive for
    // the window's lifetime and remove it in uiShutdown() before destroy.
    s_keyCtrl = gtk_event_controller_key_new();
    g_signal_connect(s_keyCtrl, "key-pressed", G_CALLBACK(onKeyPressed), nullptr);
    gtk_widget_add_controller(s_win, s_keyCtrl);

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

    const int scale = gtk_widget_get_scale_factor(s_area);
    const uint32_t devW = (uint32_t)gtk_widget_get_width(s_area) * (uint32_t)scale;
    const uint32_t devH = (uint32_t)gtk_widget_get_height(s_area) * (uint32_t)scale;
    if (!setupGL(devW, devH)) {
        GError* gerr = nullptr;
        g_set_error(&gerr, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
                    "EGL context pair setup failed");
        gtk_gl_area_set_error(GTK_GL_AREA(s_area), gerr);
        g_clear_error(&gerr);
    } else {
        // Kick the frame loop: the first "render" may have fired before the
        // GL pair was ready (and returned early), so invalidate once more.
        gtk_widget_queue_draw(s_area);
    }

    s_uiWindow.nwhType = UI_NWH_DEFAULT;
    s_uiWindow.nwh = nullptr;
    s_uiWindow.ndt = nullptr;
    s_uiWindow.offscreen = 1;  // the renderer renders into the shared scene texture
    s_uiWindow.eglDisplay     = s_gl.ready ? (void*)s_gl.display  : nullptr;
    s_uiWindow.eglContext     = s_gl.ready ? (void*)s_gl.bgfxCtx  : nullptr;
    s_uiWindow.eglPbuffer     = s_gl.ready ? (void*)s_gl.pbuffer  : nullptr;
    s_uiWindow.eglAreaSurface = s_gl.ready ? (void*)s_gl.areaSurf : nullptr;
    s_uiWindow.sceneTex       = s_gl.ready ? s_gl.sceneTex : 0;
    if (getenv("TETRIS_EGL_DEBUG"))
        fprintf(stderr, "[dbg] ready=%d display=%p ctx=%p pbuf=%p areaSurf=%p sceneTex=%u\n",
                (int)s_gl.ready, s_uiWindow.eglDisplay, s_uiWindow.eglContext,
                s_uiWindow.eglPbuffer, s_uiWindow.eglAreaSurface, s_uiWindow.sceneTex);
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
}

void uiShutdown(void) {
    if (s_gl.ready) {
        // C1 current for the objects it owns (scene texture, blit pipeline).
        eglMakeCurrent(s_gl.display, s_gl.areaSurf, s_gl.areaSurf, s_gl.areaCtx);
        if (s_gl.sceneTex != 0) {
            glDeleteTextures(1, &s_gl.sceneTex);
            s_gl.sceneTex = 0;
        }
        if (s_gl.blitProg != 0) glDeleteProgram(s_gl.blitProg);
        if (s_gl.blitVbo != 0) glDeleteBuffers(1, &s_gl.blitVbo);
        if (s_gl.blitVao != 0) glDeleteVertexArrays(1, &s_gl.blitVao);
        // Release and destroy C2 (it must go away before the window destroy
        // releases C1, the context it shares with).
        eglMakeCurrent(s_gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_gl.bgfxCtx != EGL_NO_CONTEXT) {
            eglDestroyContext(s_gl.display, s_gl.bgfxCtx);
            s_gl.bgfxCtx = EGL_NO_CONTEXT;
        }
        if (s_gl.pbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(s_gl.display, s_gl.pbuffer);
            s_gl.pbuffer = EGL_NO_SURFACE;
        }
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
        gtk_window_destroy(GTK_WINDOW(s_win));
        s_win = nullptr;
        s_area = nullptr;
        s_header = nullptr;
    }
}
