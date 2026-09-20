/*
 * gtk4_demo.c -- same demo (two 100px triangles anchored to opposite
 * corners, resizable window) rebuilt on GTK4 instead of raw Xlib/EGL.
 *
 * Why GTK4 side-steps the whole problem we spent this session on:
 *   - GDK's X11 backend already implements _NET_WM_SYNC_REQUEST internally,
 *     for every GTK app, automatically. We don't write any XSync code here.
 *   - A window with a GtkHeaderBar as its titlebar is CSD by construction:
 *     Mutter never draws server-side decorations for it, which is exactly
 *     the configuration that resized cleanly for you with the Xlib version.
 *   - GtkGLArea handles context/surface/resize plumbing; we only provide a
 *     "render" callback.
 *
 * GtkGLArea defaults to a *core* GL profile, which drops glBegin/glOrtho
 * (compatibility-only). So the two triangles are drawn with a minimal
 * shader + VBO instead of immediate mode -- everything else about the
 * scene (positions, colors, sizes) is unchanged from the Xlib version.
 *
 * Renderer choice matters a lot here: GSK's GL-based renderers ("ngl",
 * GTK4's default, and the classic "gl") have their own resize/flicker bugs
 * independent of Mutter's -- see the comment in main(). This forces
 * GSK_RENDERER=cairo, which measurably does not have that problem.
 *
 * Build:
 *   gcc -O2 -Wall -o glsync-x11-cairo-decorations glsync-x11-cairo-decorations.c $(pkg-config --cflags --libs gtk4 epoxy)
 *
 * Run:
 *   ./gtk4_demo
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <epoxy/gl.h>
#include <string.h>

typedef struct {
    GLuint program;
    GLuint vao;
    GLuint vbo;
} GLState;

static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 in_pos;   /* already in NDC, [-1, 1] */\n"
    "layout(location = 1) in vec3 in_color;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "    v_color = in_color;\n"
    "    gl_Position = vec4(in_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = vec4(v_color, 1.0); }\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        g_warning("shader compile failed: %s", log);
    }
    return sh;
}

/* Called once, when the GL area's context is ready. */
static void on_realize(GtkGLArea *area, gpointer user_data)
{
    GLState *gl = (GLState *)user_data;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL) {
        g_warning("GL area realize error: %s",
                  gtk_gl_area_get_error(area)->message);
        return;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);

    gl->program = glCreateProgram();
    glAttachShader(gl->program, vs);
    glAttachShader(gl->program, fs);
    glLinkProgram(gl->program);

    GLint linked = 0;
    glGetProgramiv(gl->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(gl->program, sizeof log, NULL, log);
        g_warning("program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &gl->vao);
    glGenBuffers(1, &gl->vbo);

    glBindVertexArray(gl->vao);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    /* Buffer contents are re-uploaded every frame in on_render(), since the
     * two triangles' NDC positions depend on the current widget size. This
     * just reserves storage and describes the vertex layout up front:
     * each vertex is (x, y, r, g, b) as 5 floats. */
    glBufferData(GL_ARRAY_BUFFER, 2 * 3 * 5 * sizeof(GLfloat), NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

/* Called every time the area needs to repaint -- including every step of
 * an interactive resize. GDK's frame clock, not us, decides when this
 * fires and paces it against the compositor; that pacing is the direct
 * GTK4 equivalent of the XSync counter dance in the Xlib version. */
static gboolean on_render(GtkGLArea *area, GdkGLContext *context, gpointer user_data)
{
    GLState *gl = (GLState *)user_data;
    (void)context;

    GtkWidget *widget = GTK_WIDGET(area);
    int scale = gtk_widget_get_scale_factor(widget);
    int w = gtk_widget_get_width(widget)  * scale;
    int h = gtk_widget_get_height(widget) * scale;
    if (w <= 0 || h <= 0)
        return TRUE;

    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Same two triangles as the Xlib version, same pixel size (100px),
     * same corners -- just expressed directly in NDC instead of going
     * through glOrtho. NDC x = pixel_x / w * 2 - 1 (and mirrored for y,
     * since GL's NDC y increases upward same as our old pixel-space y). */
    const float S = 100.0f;
    #define NDC_X(px) ((px) / (float)w * 2.0f - 1.0f)
    #define NDC_Y(py) ((py) / (float)h * 2.0f - 1.0f)

    GLfloat verts[2 * 3 * 5] = {
        /* bottom-right triangle */
        NDC_X(w - S), NDC_Y(0),      1.00f, 0.42f, 0.12f,
        NDC_X(w),     NDC_Y(0),      0.25f, 0.95f, 0.35f,
        NDC_X(w),     NDC_Y(S),      0.30f, 0.55f, 1.00f,
        /* top-left triangle */
        NDC_X(S),     NDC_Y(h),      1.00f, 0.42f, 0.12f,
        NDC_X(0),     NDC_Y(h),      0.25f, 0.95f, 0.35f,
        NDC_X(0),     NDC_Y(h - S),  0.30f, 0.55f, 1.00f,
    };
    #undef NDC_X
    #undef NDC_Y

    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof verts, verts);

    glUseProgram(gl->program);
    glBindVertexArray(gl->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    return TRUE; /* we handled the draw */
}

static void activate(GtkApplication *gtk_app, gpointer user_data)
{
    GLState *gl = (GLState *)user_data;

    GtkWidget *window = gtk_application_window_new(gtk_app);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 500);

    /* A GtkHeaderBar as the titlebar makes this window CSD: Mutter draws no
     * server-side decoration for it at all. show-title-buttons defaults to
     * TRUE, so the usual minimize/maximize/close controls are drawn by GTK
     * itself, styled to match the current theme -- "carefully drawn"
     * decorations, without us hand-rolling button hit-testing. */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header),
                                    gtk_label_new("EGL resize sync demo (GTK4 CSD)"));
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GtkWidget *gl_area = gtk_gl_area_new();
    /* Request desktop GL 3.3 core explicitly, rather than letting GTK pick
     * whatever it defaults to (which can be GLES depending on platform) --
     * our shaders above are written for desktop GLSL 330. */
    gtk_gl_area_set_use_es(GTK_GL_AREA(gl_area), FALSE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl_area), 3, 3);

    g_signal_connect(gl_area, "realize", G_CALLBACK(on_realize), gl);
    g_signal_connect(gl_area, "render",  G_CALLBACK(on_render),  gl);

    gtk_window_set_child(GTK_WINDOW(window), gl_area);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    /* GSK's GL-based renderers ("ngl", the default, and the classic "gl")
     * composite GtkGLArea's output as a texture into the window's own GL
     * surface, resizing that texture on their own schedule -- a second,
     * GTK-internal instance of the same family of bug we chased at the
     * Mutter/window-actor level: a resized texture briefly not matching the
     * region it's stretched into. The Cairo renderer doesn't have this
     * problem because it composites everything (reading the GL area back
     * via glReadPixels into a Cairo surface) synchronously every frame,
     * tightly coupled to the widget's actual allocation -- no separate GPU
     * buffer pool to fall out of sync with. Must be set before GTK/GDK
     * picks a renderer, so this has to be the first thing in main(). */
    //g_setenv("GSK_RENDERER", "vulkan", TRUE);

    gtk_init();

    // 2. Grab the runtime default display object
    GdkDisplay *display = gdk_display_get_default();

    if (display != NULL) {
        // Get the exact GObject class type name as a string
        const char *backend_type = G_OBJECT_TYPE_NAME(display);

        // 3. Evaluate the string dynamically at runtime
        if (g_strcmp0(backend_type, "GdkWaylandDisplay") == 0) {
            g_setenv("GSK_RENDERER", "vulkan", TRUE);
            g_print("Dynamic Detect -> Wayland: Setting GSK_RENDERER to vulkan\n");
        } 
        else if (g_strcmp0(backend_type, "GdkX11Display") == 0) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
            g_print("Dynamic Detect -> X11: Setting GSK_RENDERER to cairo\n");
        } 
        else {
            g_print("Dynamic Detect -> Other backend (%s). Leaving defaults.\n", backend_type);
        }
    } else {
        g_print("display No\n");
    }
    
    GLState gl;
    memset(&gl, 0, sizeof gl);

    GtkApplication *gtk_app = gtk_application_new("org.example.glsyncdemo",
                                                   G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), &gl);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    return status;
}
