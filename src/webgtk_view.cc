// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The GTK view: a GObject widget that wraps a GtkGLArea and presents the
// engine's offscreen (OSR) frames as a texture.
//
// The view knows nothing about Blink, V8 or the mock. Everything engine
// specific is behind the webgtk::BackendOps vtable, which makes the same view
// binary usable with the real engine and with the mock renderer.
//
// The file compiles against either GTK3 or GTK4; WEBGTK_GTK4 selects the
// GTK4 code paths at compile time.

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <webgtk/webgtk.h>

#include "webgtk_backend_ops.h"

#ifndef WEBGTK_GTK4
#define WEBGTK_GTK4 0
#endif

namespace {

constexpr int kMaxDimension = 32768;

bool valid_size(int width, int height) {
  return width > 0 && height > 0 && width <= kMaxDimension &&
         height <= kMaxDimension;
}

unsigned map_modifiers(GdkModifierType state) {
  unsigned out = 0;
  if (state & GDK_SHIFT_MASK) out |= WEBGTK_MOD_SHIFT;
  if (state & GDK_CONTROL_MASK) out |= WEBGTK_MOD_CONTROL;
  // GTK3 calls the Alt modifier MOD1; GTK4 renamed the mask to ALT.
#if WEBGTK_GTK4
  if (state & GDK_ALT_MASK) out |= WEBGTK_MOD_ALT;
#else
  if (state & GDK_MOD1_MASK) out |= WEBGTK_MOD_ALT;
#endif
  if (state & GDK_SUPER_MASK) out |= WEBGTK_MOD_SUPER;
  return out;
}

webgtk_mouse_button map_button(guint button) {
  switch (button) {
    case GDK_BUTTON_MIDDLE:
      return WEBGTK_MOUSE_MIDDLE;
    case GDK_BUTTON_SECONDARY:
      return WEBGTK_MOUSE_RIGHT;
    default:
      return WEBGTK_MOUSE_LEFT;
  }
}

// A textured quad used to blit the OSR frame. Written against a small subset
// of GLSL shared by desktop GL 1.50/3.30 and GLES 3.0 so the same code runs on
// core-profile and ES contexts.
struct GlBlitter {
  GLuint program = 0;
  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint texture = 0;
  bool ready = false;
  int texture_width = 0;
  int texture_height = 0;
};

const char *vertex_shader_source(bool use_es, bool legacy) {
  if (use_es) {
    return "#version 300 es\n"
           "in vec2 a_pos;\n"
           "in vec2 a_uv;\n"
           "out vec2 v_uv;\n"
           "void main() {\n"
           "  v_uv = a_uv;\n"
           "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
           "}\n";
  }
  if (legacy) {
    return "#version 150\n"
           "in vec2 a_pos;\n"
           "in vec2 a_uv;\n"
           "out vec2 v_uv;\n"
           "void main() {\n"
           "  v_uv = a_uv;\n"
           "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
           "}\n";
  }
  return "#version 330 core\n"
         "in vec2 a_pos;\n"
         "in vec2 a_uv;\n"
         "out vec2 v_uv;\n"
         "void main() {\n"
         "  v_uv = a_uv;\n"
         "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
         "}\n";
}

const char *fragment_shader_source(bool use_es, bool legacy) {
  if (use_es) {
    return "#version 300 es\n"
           "precision mediump float;\n"
           "in vec2 v_uv;\n"
           "out vec4 o_color;\n"
           "uniform sampler2D u_texture;\n"
           "void main() {\n"
           "  o_color = texture(u_texture, v_uv);\n"
           "}\n";
  }
  if (legacy) {
    return "#version 150\n"
           "in vec2 v_uv;\n"
           "out vec4 o_color;\n"
           "uniform sampler2D u_texture;\n"
           "void main() {\n"
           "  o_color = texture(u_texture, v_uv);\n"
           "}\n";
  }
  return "#version 330 core\n"
         "in vec2 v_uv;\n"
         "out vec4 o_color;\n"
         "uniform sampler2D u_texture;\n"
         "void main() {\n"
         "  o_color = texture(u_texture, v_uv);\n"
         "}\n";
}

GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) return 0;
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    g_warning("webgtk: shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool blitter_init(GlBlitter *blitter, GdkGLContext *context) {
  const bool use_es = context && gdk_gl_context_get_use_es(context);
  int major = 0, minor = 0;
  if (context) gdk_gl_context_get_version(context, &major, &minor);
  const bool legacy = !use_es && (major < 3 || (major == 3 && minor < 3));

  GLuint vs = compile_shader(GL_VERTEX_SHADER,
                             vertex_shader_source(use_es, legacy));
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER,
                             fragment_shader_source(use_es, legacy));
  if (vs == 0 || fs == 0) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return false;
  }

  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  // Explicit locations keep the shaders valid on GLSL 1.50, which has no
  // layout(location=...) qualifier.
  glBindAttribLocation(program, 0, "a_pos");
  glBindAttribLocation(program, 1, "a_uv");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[1024] = {0};
    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
    g_warning("webgtk: shader link failed: %s", log);
    glDeleteProgram(program);
    return false;
  }

  // Fullscreen triangle strip in clip space; UVs are flipped vertically so
  // the top-left-origin OSR bitmap lands the right way up.
  static const GLfloat vertices[] = {
      -1.0f, -1.0f, 0.0f, 1.0f,  // bottom-left
      1.0f,  -1.0f, 1.0f, 1.0f,  // bottom-right
      -1.0f, 1.0f,  0.0f, 0.0f,  // top-left
      1.0f,  1.0f,  1.0f, 0.0f,  // top-right
  };

  glGenVertexArrays(1, &blitter->vao);
  glGenBuffers(1, &blitter->vbo);
  glGenTextures(1, &blitter->texture);

  glBindVertexArray(blitter->vao);
  glBindBuffer(GL_ARRAY_BUFFER, blitter->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        reinterpret_cast<void *>(0));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        reinterpret_cast<void *>(2 * sizeof(GLfloat)));

  glBindTexture(GL_TEXTURE_2D, blitter->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  blitter->program = program;
  blitter->ready = true;
  return true;
}

void blitter_shutdown(GlBlitter *blitter) {
  if (blitter->texture) glDeleteTextures(1, &blitter->texture);
  if (blitter->vbo) glDeleteBuffers(1, &blitter->vbo);
  if (blitter->vao) glDeleteVertexArrays(1, &blitter->vao);
  if (blitter->program) glDeleteProgram(blitter->program);
  *blitter = GlBlitter{};
}

void blitter_draw(GlBlitter *blitter, const webgtk::Bitmap &bitmap) {
  if (!blitter->ready || bitmap.empty()) return;

  glBindTexture(GL_TEXTURE_2D, blitter->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (bitmap.width != blitter->texture_width ||
      bitmap.height != blitter->texture_height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.width, bitmap.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, bitmap.data);
    blitter->texture_width = bitmap.width;
    blitter->texture_height = bitmap.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap.width, bitmap.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, bitmap.data);
  }

  glUseProgram(blitter->program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, blitter->texture);
  glUniform1i(glGetUniformLocation(blitter->program, "u_texture"), 0);
  glBindVertexArray(blitter->vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace

struct webgtk_view {
  GtkWidget *area;
  GtkIMContext *im_context;
  const webgtk::BackendOps *ops;
  void *backend;

  webgtk::Bitmap frame;
  size_t frame_capacity;

  GlBlitter blitter;
  bool realized;
  bool gl_failed;
  bool destroyed;
  int width;
  int height;
};

namespace {

// Copies a backend frame into the view's own buffer. Backends keep ownership of
// the bitmap they return, so the view must never alias it.
bool publish_frame(webgtk_view *view, const webgtk::Bitmap &bitmap) {
  const size_t needed = bitmap.byte_size();
  if (needed == 0) return false;
  if (needed > view->frame_capacity) {
    void *resized = std::realloc(view->frame.data, needed);
    if (!resized) return false;
    view->frame.data = static_cast<uint8_t *>(resized);
    view->frame_capacity = needed;
  }
  std::memcpy(view->frame.data, bitmap.data, needed);
  view->frame.width = bitmap.width;
  view->frame.height = bitmap.height;
  view->frame.stride = bitmap.stride;
  return true;
}

// Pulls one frame from the backend and, when a GL context is live, presents it.
void render_frame(webgtk_view *view) {
  if (view->destroyed || view->backend == nullptr) return;

  webgtk::Bitmap bitmap{};
  if (view->ops->render(view->backend, &bitmap) != WEBGTK_OK) return;
  if (!publish_frame(view, bitmap)) return;

  if (!view->realized || view->gl_failed || !view->blitter.ready) return;
  GtkGLArea *area = GTK_GL_AREA(view->area);
  gtk_gl_area_make_current(area);
  if (gtk_gl_area_get_error(area) != nullptr) return;
  glViewport(0, 0, view->width, view->height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_DEPTH_TEST);
  blitter_draw(&view->blitter, view->frame);
}

extern "C" {

gboolean webgtk_on_realize(GtkGLArea *area, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  gtk_gl_area_make_current(area);
  if (GError *error = gtk_gl_area_get_error(area)) {
    g_warning("webgtk: GL context unavailable, rendering to memory only: %s",
              error->message);
    view->gl_failed = true;
    view->realized = true;
    return TRUE;
  }
  GdkGLContext *context = gtk_gl_area_get_context(area);
  if (!blitter_init(&view->blitter, context)) view->gl_failed = true;
  view->realized = true;
  return TRUE;
}

void webgtk_on_unrealize(GtkGLArea *area, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (!view->gl_failed) {
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) == nullptr) blitter_shutdown(&view->blitter);
  }
  view->realized = false;
}

void webgtk_on_resize(GtkGLArea *, int width, int height, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->width = width;
  view->height = height;
  if (view->ops->set_size) view->ops->set_size(view->backend, width, height);
  render_frame(view);
}

gboolean webgtk_on_render(GtkGLArea *area, GdkGLContext *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  (void)area;
  if (view->destroyed) return TRUE;
  render_frame(view);
  return TRUE;
}

void webgtk_on_motion(GtkEventControllerMotion *, double x, double y,
                      gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->ops->mouse_move(view->backend, static_cast<int>(x),
                        static_cast<int>(y), 0);
}

void webgtk_on_button_pressed(GtkGesture *gesture, int, double x, double y,
                              gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  const guint button =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  view->ops->mouse_button(view->backend, static_cast<int>(x),
                          static_cast<int>(y), map_button(button), 1, 0);
}

void webgtk_on_button_released(GtkGesture *gesture, int, double x, double y,
                               gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  const guint button =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  view->ops->mouse_button(view->backend, static_cast<int>(x),
                          static_cast<int>(y), map_button(button), 0, 0);
}

gboolean webgtk_on_scroll(GtkEventControllerScroll *, double dx, double dy,
                          gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  // GtkEventControllerScroll has no cursor position; the last motion event
  // supplies the coordinates for the engine's hit test.
  view->ops->mouse_scroll(view->backend, view->width / 2, view->height / 2, dx,
                          dy, 0);
  return TRUE;
}

gboolean webgtk_on_key_pressed(GtkEventControllerKey *, guint keyval, guint,
                               GdkModifierType state, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  return view->ops->key_event(view->backend, keyval, 1, map_modifiers(state)) ==
                 WEBGTK_OK
             ? TRUE
             : FALSE;
}

void webgtk_on_key_released(GtkEventControllerKey *, guint keyval, guint,
                            GdkModifierType state, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->ops->key_event(view->backend, keyval, 0, map_modifiers(state));
}

void webgtk_on_im_commit(GtkIMContext *, gchar *text, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->ops->text_input(view->backend, text);
}

// The widget can outlive the view when the embedder keeps it in a container;
// drop the back-pointer so a later destroy does not touch freed widgets.
void webgtk_on_widget_finalized(gpointer data, GObject *) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view) view->area = nullptr;
}

void webgtk_on_widget_destroyed(GtkWidget *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view) view->area = nullptr;
}

}  // extern "C"

}  // namespace

namespace {

GtkWidget *create_area(webgtk_view *view) {
  GtkWidget *area = gtk_gl_area_new();
  // Hold our own reference. Containers sink the floating reference, so without
  // this the widget would be destroyed the moment it is removed from a parent.
  g_object_ref_sink(area);
  gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), FALSE);
  gtk_widget_set_size_request(area, view->width, view->height);
#if WEBGTK_GTK4
  gtk_widget_set_focusable(area, TRUE);
  gtk_widget_set_hexpand(area, TRUE);
  gtk_widget_set_vexpand(area, TRUE);
#else
  gtk_widget_set_can_focus(area, TRUE);
  gtk_widget_add_events(area, GDK_POINTER_MOTION_MASK |
                                  GDK_BUTTON_PRESS_MASK |
                                  GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK |
                                  GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK |
                                  GDK_KEY_RELEASE_MASK);
#endif

  g_object_set_data(G_OBJECT(area), "webgtk-view", view);
  g_object_weak_ref(G_OBJECT(area), webgtk_on_widget_finalized, view);
  g_signal_connect(area, "destroy", G_CALLBACK(webgtk_on_widget_destroyed),
                   view);

  g_signal_connect(area, "realize", G_CALLBACK(webgtk_on_realize), view);
  g_signal_connect(area, "unrealize", G_CALLBACK(webgtk_on_unrealize), view);
  g_signal_connect(area, "resize", G_CALLBACK(webgtk_on_resize), view);
  g_signal_connect(area, "render", G_CALLBACK(webgtk_on_render), view);

#if WEBGTK_GTK4
  GtkEventController *motion = gtk_event_controller_motion_new();
#else
  GtkEventController *motion = gtk_event_controller_motion_new(area);
#endif
  g_signal_connect(motion, "motion", G_CALLBACK(webgtk_on_motion), view);
#if WEBGTK_GTK4
  gtk_widget_add_controller(area, motion);
#endif

#if WEBGTK_GTK4
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
#else
  GtkGesture *click = gtk_gesture_multi_press_new(area);
#endif
  g_signal_connect(click, "pressed", G_CALLBACK(webgtk_on_button_pressed), view);
  g_signal_connect(click, "released", G_CALLBACK(webgtk_on_button_released),
                   view);
#if WEBGTK_GTK4
  gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
#endif

#if WEBGTK_GTK4
  GtkEventController *scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
#else
  GtkEventController *scroll = gtk_event_controller_scroll_new(
      area, GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
#endif
  g_signal_connect(scroll, "scroll", G_CALLBACK(webgtk_on_scroll), view);
#if WEBGTK_GTK4
  gtk_widget_add_controller(area, scroll);
#endif

#if WEBGTK_GTK4
  GtkEventController *key = gtk_event_controller_key_new();
#else
  GtkEventController *key = gtk_event_controller_key_new(area);
#endif
  view->im_context = gtk_im_context_simple_new();
  g_signal_connect(view->im_context, "commit", G_CALLBACK(webgtk_on_im_commit),
                   view);
  gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key),
                                          view->im_context);
  g_signal_connect(key, "key-pressed", G_CALLBACK(webgtk_on_key_pressed), view);
  g_signal_connect(key, "key-released", G_CALLBACK(webgtk_on_key_released),
                   view);
#if WEBGTK_GTK4
  gtk_widget_add_controller(area, key);
#endif

  return area;
}

}  // namespace

/* ---------------------------------------------------------------- C ABI -- */

extern "C" {

webgtk_view *webgtk_view_new(int width, int height) {
  if (!valid_size(width, height)) return nullptr;

  auto *view = new (std::nothrow) webgtk_view{};
  if (!view) return nullptr;

  view->width = width;
  view->height = height;
  view->ops = webgtk::default_backend();
  if (!view->ops) {
    delete view;
    return nullptr;
  }
  view->backend = view->ops->create(width, height);
  if (!view->backend) {
    delete view;
    return nullptr;
  }
  view->area = create_area(view);
  return view;
}

void webgtk_view_destroy(webgtk_view *view) {
  if (!view) return;
  view->destroyed = true;

  if (view->area) {
    GtkWidget *area = view->area;
    view->area = nullptr;
    g_object_set_data(G_OBJECT(area), "webgtk-view", nullptr);
    // Detach first so the container cannot deliver further signals to this
    // view; dropping our reference then finalises the widget.
    GtkWidget *parent = gtk_widget_get_parent(area);
    if (parent) {
#if WEBGTK_GTK4
      gtk_window_set_child(GTK_WINDOW(parent), nullptr);
#else
      gtk_container_remove(GTK_CONTAINER(parent), area);
#endif
    }
    g_object_unref(area);
  }
  if (view->im_context) {
    g_object_unref(view->im_context);
    view->im_context = nullptr;
  }
  if (view->ops && view->backend) view->ops->destroy(view->backend);

  std::free(view->frame.data);
  delete view;
}

void *webgtk_view_get_widget(webgtk_view *view) {
  return view ? static_cast<void *>(view->area) : nullptr;
}

const char *webgtk_backend_name(webgtk_view *view) {
  if (!view || !view->ops) return "";
  return view->ops->name();
}

webgtk_status webgtk_view_load_uri(webgtk_view *view, const char *uri) {
  if (!view || !uri) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->load_uri(view->backend, uri);
}

webgtk_status webgtk_view_load_html(webgtk_view *view, const char *html,
                                    size_t length) {
  if (!view || (!html && length > 0)) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->load_html(view->backend, html, length);
}

webgtk_status webgtk_view_evaluate_javascript(
    webgtk_view *view, const char *script,
    void (*callback)(const char *json_result, void *user_data),
    void *user_data) {
  if (!view || !script) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->evaluate_javascript(view->backend, script, callback,
                                        user_data);
}

webgtk_status webgtk_view_set_size(webgtk_view *view, int width, int height) {
  if (!view || !valid_size(width, height))
    return WEBGTK_ERROR_INVALID_ARGUMENT;
  view->width = width;
  view->height = height;
  if (view->area)
    gtk_widget_set_size_request(view->area, width, height);
  return view->ops->set_size ? view->ops->set_size(view->backend, width, height)
                             : WEBGTK_OK;
}

webgtk_status webgtk_view_render(webgtk_view *view, uint8_t *buffer,
                                 size_t buffer_size) {
  if (!view || !buffer) return WEBGTK_ERROR_INVALID_ARGUMENT;
  // Render into a scratch bitmap: the backend owns that memory and must not
  // be handed the view's own buffer, which it would overwrite and leak.
  webgtk::Bitmap bitmap{};
  if (view->ops->render(view->backend, &bitmap) != WEBGTK_OK)
    return WEBGTK_ERROR_INTERNAL;
  if (!publish_frame(view, bitmap)) return WEBGTK_ERROR_INTERNAL;
  if (buffer_size < view->frame.byte_size())
    return WEBGTK_ERROR_INVALID_ARGUMENT;
  std::memcpy(buffer, view->frame.data, view->frame.byte_size());
  return WEBGTK_OK;
}

webgtk_status webgtk_view_mouse_move(webgtk_view *view, int x, int y,
                                     unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->mouse_move(view->backend, x, y, modifiers);
}

webgtk_status webgtk_view_mouse_button(webgtk_view *view, int x, int y,
                                       webgtk_mouse_button button, int pressed,
                                       unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->mouse_button(view->backend, x, y, button, pressed,
                                 modifiers);
}

webgtk_status webgtk_view_mouse_scroll(webgtk_view *view, int x, int y,
                                       double dx, double dy,
                                       unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->mouse_scroll(view->backend, x, y, dx, dy, modifiers);
}

webgtk_status webgtk_view_key_event(webgtk_view *view, uint32_t keysym,
                                    int pressed, unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->key_event(view->backend, keysym, pressed, modifiers);
}

webgtk_status webgtk_view_text_input(webgtk_view *view, const char *utf8) {
  if (!view || !utf8) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->text_input(view->backend, utf8);
}

const char *webgtk_status_string(webgtk_status status) {
  switch (status) {
    case WEBGTK_OK:
      return "ok";
    case WEBGTK_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
    case WEBGTK_ERROR_NOT_IMPLEMENTED:
      return "not implemented";
    case WEBGTK_ERROR_INTERNAL:
      return "internal error";
  }
  return "unknown";
}

}  // extern "C"
