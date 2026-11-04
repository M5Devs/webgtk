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
#include <map>
#include <new>
#include <string>

#include <webgtk/webgtk.h>
#include <webgtk/webgtk-view.h>

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

unsigned map_modifiers(guint state) {
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

/* The instance. First member must be GObject so the GObject macros work. */
struct webgtk_view {
  GObject parent_instance;

  GtkWidget *area;
  GtkIMContext *im_context;
  const webgtk::BackendOps *ops;
  void *backend;

  /* Script message handlers, keyed by name. Stored in a map so lookup is O(log
   * n) and the callback pointers stay put while dispatch runs. */
  std::map<std::string, webgtk::ScriptMessageHandler> script_handlers;

  webgtk::Bitmap frame;
  size_t frame_capacity;

  GlBlitter blitter;
  bool realized;
  bool gl_failed;
  bool destroyed;
  int width;
  int height;

  /* Input bookkeeping. GtkEventControllerScroll has no cursor position, so the
   * last motion point supplies the coordinates for a scroll's hit test. */
  double pointer_x;
  double pointer_y;
  bool pointer_valid;
  bool key_pressed;
  bool focused;
};

namespace {

/* Queued on the main context so the signal is always emitted from the GTK
 * main thread, even when the backend answers synchronously from another. */
struct JsEmission {
  webgtk_view *view;
  char *json;
};

gboolean emit_js_result(gpointer data) {
  auto *emission = static_cast<JsEmission *>(data);
  if (!emission->view->destroyed)
    g_signal_emit_by_name(emission->view, "javascript-result", emission->json);
  g_object_unref(emission->view);
  g_free(emission->json);
  delete emission;
  return G_SOURCE_REMOVE;
}

void on_js_result(const char *json, void *user_data) {
  auto *emission = new (std::nothrow) JsEmission;
  if (!emission) return;
  emission->view = WEBGTK_VIEW(user_data);
  emission->json = g_strdup(json ? json : "null");
  g_object_ref(emission->view);
  g_main_context_invoke(nullptr, emit_js_result, emission);
}

/* Tears down everything the instance owns. Idempotent, and called both from
 * dispose() and from the C ABI's webgtk_view_destroy(). */
void teardown(webgtk_view *view) {
  if (view->destroyed) return;
  view->destroyed = true;

  if (view->area) {
    GtkWidget *area = view->area;
    view->area = nullptr;
    g_object_set_data(G_OBJECT(area), "webgtk-view", nullptr);
    /* Detach first so the container cannot deliver further signals to a view
     * that is going away; dropping our reference then finalises the widget. */
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
  if (view->ops && view->backend) {
    view->ops->destroy(view->backend);
    view->backend = nullptr;
  }
  std::free(view->frame.data);
  view->frame.data = nullptr;
  view->frame_capacity = 0;
  view->frame = webgtk::Bitmap{};
}

/* Builds the backing GtkGLArea and wires every input path to `view`. */
GtkWidget *create_area(webgtk_view *view);

}  // namespace

/* -------------------------------------------------- script message dispatch -- */

/* Called by a backend the moment the page posts a message. Everything here is
 * synchronous: registered callbacks run, then the signal is emitted, so an
 * embedder observes the post on the same main-loop turn that delivered it. */
extern "C" void webgtk_view_dispatch_script_message(webgtk_view *view,
                                                    const char *name,
                                                    const char *payload) {
  if (!view || view->destroyed) return;
  const char *handler = name ? name : "";
  const char *text = payload ? payload : "";

  /* Look up before invoking: a callback may unregister handlers (or register
   * new ones) re-entrantly, which would invalidate an iterator held across the
   * call. Copying the pair keeps the callback and its user data stable. */
  auto it = view->script_handlers.find(handler);
  if (it != view->script_handlers.end()) {
    webgtk::ScriptMessageHandler entry = it->second;
    if (entry.callback) entry.callback(view, handler, text, entry.user_data);
  }

  g_signal_emit_by_name(view, "script-message-received", handler, text);
}

namespace {

/* Payload carried from a backend thread to the GTK main thread. */
struct PostedMessage {
  webgtk_view *view;  // strong reference, dropped by the destroy notify
  char *name;
  char *payload;
};

char *dup_string(const char *s) {
  const size_t n = std::strlen(s) + 1;
  auto *copy = static_cast<char *>(g_malloc(n));
  std::memcpy(copy, s, n);
  return copy;
}

gboolean deliver_posted_message(gpointer data) {
  auto *msg = static_cast<PostedMessage *>(data);
  webgtk_view_dispatch_script_message(msg->view, msg->name, msg->payload);
  return G_SOURCE_REMOVE;
}

void free_posted_message(gpointer data) {
  auto *msg = static_cast<PostedMessage *>(data);
  g_free(msg->name);
  g_free(msg->payload);
  g_object_unref(msg->view);
  delete msg;
}

}  // namespace

extern "C" bool webgtk_view_post_script_message(webgtk_view *view,
                                                const char *name,
                                                const char *payload) {
  if (!view) return false;
  auto *msg = new PostedMessage{view, dup_string(name ? name : ""),
                                dup_string(payload ? payload : "")};
  g_object_ref(view);  // keep the view alive until the idle runs
  g_idle_add_full(G_PRIORITY_DEFAULT, deliver_posted_message, msg,
                  free_posted_message);
  return true;
}

/* GObject plumbing lives in extern "C" so the generated
 * webgtk_view_get_type() has C linkage and matches the public header. */
extern "C" {

G_DEFINE_TYPE(WebGtkView, webgtk_view, G_TYPE_OBJECT)

static void webgtk_view_init(webgtk_view *view) {
  view->area = nullptr;
  view->im_context = nullptr;
  view->ops = nullptr;
  view->backend = nullptr;
  new (&view->script_handlers)
      std::map<std::string, webgtk::ScriptMessageHandler>();
  view->frame = webgtk::Bitmap{};
  view->frame_capacity = 0;
  view->blitter = GlBlitter{};
  view->realized = false;
  view->gl_failed = false;
  view->destroyed = false;
  view->width = 0;
  view->height = 0;
  view->pointer_x = 0.0;
  view->pointer_y = 0.0;
  view->pointer_valid = false;
  view->key_pressed = false;
  view->focused = false;
}

static void webgtk_view_dispose(GObject *object) {
  teardown(WEBGTK_VIEW(object));
  G_OBJECT_CLASS(webgtk_view_parent_class)->dispose(object);
}

static void webgtk_view_finalize(GObject *object) {
  auto *view = WEBGTK_VIEW(object);
  /* Last line of defence: the GL context may still be current during dispose,
   * so the blitter is released here, after the widget is gone. */
  if (view->blitter.ready) blitter_shutdown(&view->blitter);
  view->script_handlers.~map();
  G_OBJECT_CLASS(webgtk_view_parent_class)->finalize(object);
}

/* Property ids, kept in one place. */
enum {
  PROP_0,
  PROP_WIDGET,
  PROP_BACKEND_NAME,
};

static void webgtk_view_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *spec) {
  auto *view = WEBGTK_VIEW(object);
  switch (prop_id) {
    case PROP_WIDGET:
      g_value_set_object(value, view->area);
      break;
    case PROP_BACKEND_NAME:
      g_value_set_string(value, view->ops ? view->ops->name() : "");
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, spec);
      break;
  }
}

static void webgtk_view_class_init(WebGtkViewClass *klass) {
  auto *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = webgtk_view_dispose;
  object_class->finalize = webgtk_view_finalize;
  object_class->get_property = webgtk_view_get_property;

  /*
   * "widget" is the child to add to a container. It is a read-only object
   * property rather than a GtkContainer child because WebGtkView is a plain
   * GObject: the widget is the embeddable surface, the view is the controller.
   */
  g_object_class_install_property(
      object_class, PROP_WIDGET,
      g_param_spec_object("widget", "Widget",
                          "The GtkGLArea to add to a container", GTK_TYPE_WIDGET,
                          (GParamFlags)(G_PARAM_READABLE |
                                        G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      object_class, PROP_BACKEND_NAME,
      g_param_spec_string("backend-name", "Backend name",
                          "Name of the active rendering backend", "",
                          (GParamFlags)(G_PARAM_READABLE |
                                        G_PARAM_STATIC_STRINGS)));

  g_signal_new("javascript-result", WEBGTK_TYPE_VIEW, G_SIGNAL_RUN_LAST, 0,
               nullptr, nullptr, g_cclosure_marshal_VOID__STRING,
               G_TYPE_NONE, 1, G_TYPE_STRING);

  /* The binding-friendly face of the IPC bridge: one signal, two string
   * arguments, so a Python or GJS listener needs no registration call. */
  g_signal_new("script-message-received", WEBGTK_TYPE_VIEW, G_SIGNAL_RUN_LAST,
               0, nullptr, nullptr, g_cclosure_marshal_generic, G_TYPE_NONE, 2,
               G_TYPE_STRING, G_TYPE_STRING);
}

}  // extern "C"

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

#if WEBGTK_GTK4
/* GTK4 gestures/controllers do not hand over the modifier state, but the
 * controller can report the state of the event it is currently handling. */
unsigned current_modifiers(GtkEventController *controller) {
  return map_modifiers(
      gtk_event_controller_get_current_event_state(controller));
}
#endif

/* Dispatch helpers prefer the rich struct op and fall back to the scalar one,
 * so a backend only has to implement the shape it can support. */
webgtk_status dispatch_mouse(webgtk_view *view, const webgtk_mouse_event *event) {
  if (view->ops->send_mouse_event)
    return view->ops->send_mouse_event(view->backend, event);
  if (event->type == WEBGTK_MOUSE_MOVE) {
    return view->ops->mouse_move
               ? view->ops->mouse_move(view->backend, static_cast<int>(event->x),
                                       static_cast<int>(event->y),
                                       event->modifiers)
               : WEBGTK_ERROR_NOT_IMPLEMENTED;
  }
  return view->ops->mouse_button
             ? view->ops->mouse_button(
                   view->backend, static_cast<int>(event->x),
                   static_cast<int>(event->y), event->button,
                   event->type == WEBGTK_MOUSE_PRESS, event->modifiers)
             : WEBGTK_ERROR_NOT_IMPLEMENTED;
}

webgtk_status dispatch_key(webgtk_view *view, const webgtk_key_event *event) {
  if (view->ops->send_key_event)
    return view->ops->send_key_event(view->backend, event);
  return view->ops->key_event
             ? view->ops->key_event(view->backend, event->keyval,
                                    event->type == WEBGTK_KEY_PRESS,
                                    event->modifiers)
             : WEBGTK_ERROR_NOT_IMPLEMENTED;
}

webgtk_status dispatch_scroll(webgtk_view *view,
                              const webgtk_scroll_event *event) {
  if (view->ops->send_scroll_event)
    return view->ops->send_scroll_event(view->backend, event);
  // The scalar op needs a position; the last motion point is the best estimate
  // and is what the engine would hit-test against anyway.
  const int x = view->pointer_valid ? static_cast<int>(view->pointer_x)
                                    : view->width / 2;
  const int y = view->pointer_valid ? static_cast<int>(view->pointer_y)
                                    : view->height / 2;
  return view->ops->mouse_scroll
             ? view->ops->mouse_scroll(view->backend, x, y, event->dx, event->dy,
                                       event->modifiers)
             : WEBGTK_ERROR_NOT_IMPLEMENTED;
}

void dispatch_focus(webgtk_view *view, bool focused) {
  view->focused = focused;
  if (view->ops->send_focus_event)
    view->ops->send_focus_event(view->backend, focused ? 1 : 0);
}

/* Printable text for a key event, or NULL. Control and Alt combinations are
 * treated as commands (Ctrl+C must not look like the letter "c"), and the
 * keypad/function keys map to no character at all. */
const char *key_text(guint keyval, unsigned modifiers) {
  if (modifiers & (WEBGTK_MOD_CONTROL | WEBGTK_MOD_ALT)) return nullptr;
  const gunichar ch = gdk_keyval_to_unicode(keyval);
  if (ch == 0 || !g_unichar_isprint(ch)) return nullptr;
  static char buffer[8];
  const int len = g_unichar_to_utf8(ch, buffer);
  buffer[len] = '\0';
  return buffer;
}

#if WEBGTK_GTK4
/* --- GTK4: event controllers and gestures --- */

void webgtk_on_motion(GtkEventControllerMotion *controller, double x, double y,
                      gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->pointer_x = x;
  view->pointer_y = y;
  view->pointer_valid = true;
  webgtk_mouse_event event{};
  event.type = WEBGTK_MOUSE_MOVE;
  event.x = x;
  event.y = y;
  event.modifiers = current_modifiers(GTK_EVENT_CONTROLLER(controller));
  dispatch_mouse(view, &event);
}

void webgtk_on_button_pressed(GtkGesture *gesture, int n_press, double x,
                              double y, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  // A click gives the view keyboard focus, so subsequent key events arrive.
  GtkWidget *widget =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (widget) gtk_widget_grab_focus(widget);
  view->pointer_x = x;
  view->pointer_y = y;
  view->pointer_valid = true;
  const guint button =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  webgtk_mouse_event event{};
  event.type = WEBGTK_MOUSE_PRESS;
  event.x = x;
  event.y = y;
  event.button = map_button(button);
  event.modifiers = current_modifiers(GTK_EVENT_CONTROLLER(gesture));
  event.click_count = n_press > 0 ? n_press : 1;
  dispatch_mouse(view, &event);
}

void webgtk_on_button_released(GtkGesture *gesture, int n_press, double x,
                               double y, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  view->pointer_x = x;
  view->pointer_y = y;
  view->pointer_valid = true;
  const guint button =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  webgtk_mouse_event event{};
  event.type = WEBGTK_MOUSE_RELEASE;
  event.x = x;
  event.y = y;
  event.button = map_button(button);
  event.modifiers = current_modifiers(GTK_EVENT_CONTROLLER(gesture));
  event.click_count = n_press > 0 ? n_press : 1;
  dispatch_mouse(view, &event);
}

gboolean webgtk_on_scroll(GtkEventControllerScroll *controller, double dx,
                          double dy, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  webgtk_scroll_event event{};
  event.dx = dx;
  event.dy = dy;
  event.modifiers = current_modifiers(GTK_EVENT_CONTROLLER(controller));
  dispatch_scroll(view, &event);
  return TRUE;
}

gboolean webgtk_on_key_pressed(GtkEventControllerKey *, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  webgtk_key_event event{};
  event.type = WEBGTK_KEY_PRESS;
  event.keyval = keyval;
  event.keycode = keycode;
  event.modifiers = map_modifiers(state);
  event.text = key_text(keyval, event.modifiers);
  view->key_pressed = true;
  return dispatch_key(view, &event) == WEBGTK_OK ? TRUE : FALSE;
}

void webgtk_on_key_released(GtkEventControllerKey *, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  webgtk_key_event event{};
  event.type = WEBGTK_KEY_RELEASE;
  event.keyval = keyval;
  event.keycode = keycode;
  event.modifiers = map_modifiers(state);
  event.text = nullptr;
  view->key_pressed = false;
  dispatch_key(view, &event);
}

void webgtk_on_focus_enter(GtkEventControllerFocus *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  dispatch_focus(view, true);
}

void webgtk_on_focus_leave(GtkEventControllerFocus *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return;
  dispatch_focus(view, false);
}
#else
/* --- GTK3: traditional widget event signals --- */

/* GTK3 reports the click count as distinct event types rather than a field. */
int click_count_for(const GdkEventButton *event) {
  switch (event->type) {
    case GDK_2BUTTON_PRESS:
      return 2;
    case GDK_3BUTTON_PRESS:
      return 3;
    default:
      return 1;
  }
}

gboolean webgtk_on_motion_notify(GtkWidget *, GdkEventMotion *event,
                                 gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  view->pointer_x = event->x;
  view->pointer_y = event->y;
  view->pointer_valid = true;
  webgtk_mouse_event out{};
  out.type = WEBGTK_MOUSE_MOVE;
  out.x = event->x;
  out.y = event->y;
  out.modifiers = map_modifiers(event->state);
  dispatch_mouse(view, &out);
  return FALSE;
}

gboolean webgtk_on_button_press(GtkWidget *, GdkEventButton *event,
                                gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  // A click gives the view keyboard focus, so subsequent key events arrive.
  if (view->area) gtk_widget_grab_focus(view->area);
  view->pointer_x = event->x;
  view->pointer_y = event->y;
  view->pointer_valid = true;
  webgtk_mouse_event out{};
  out.type = WEBGTK_MOUSE_PRESS;
  out.x = event->x;
  out.y = event->y;
  out.button = map_button(event->button);
  out.modifiers = map_modifiers(event->state);
  out.click_count = click_count_for(event);
  dispatch_mouse(view, &out);
  return TRUE;
}

gboolean webgtk_on_button_release(GtkWidget *, GdkEventButton *event,
                                  gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  view->pointer_x = event->x;
  view->pointer_y = event->y;
  view->pointer_valid = true;
  webgtk_mouse_event out{};
  out.type = WEBGTK_MOUSE_RELEASE;
  out.x = event->x;
  out.y = event->y;
  out.button = map_button(event->button);
  out.modifiers = map_modifiers(event->state);
  out.click_count = click_count_for(event);
  dispatch_mouse(view, &out);
  return TRUE;
}

gboolean webgtk_on_scroll_event(GtkWidget *, GdkEventScroll *event,
                                gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  view->pointer_x = event->x;
  view->pointer_y = event->y;
  view->pointer_valid = true;
  webgtk_scroll_event out{};
  switch (event->direction) {
    case GDK_SCROLL_UP:
      out.dy = 1.0;
      break;
    case GDK_SCROLL_DOWN:
      out.dy = -1.0;
      break;
    case GDK_SCROLL_LEFT:
      out.dx = 1.0;
      break;
    case GDK_SCROLL_RIGHT:
      out.dx = -1.0;
      break;
    case GDK_SCROLL_SMOOTH:
      out.dx = event->delta_x;
      out.dy = event->delta_y;
      break;
    default:
      return FALSE;
  }
  out.modifiers = map_modifiers(event->state);
  dispatch_scroll(view, &out);
  return TRUE;
}

gboolean webgtk_on_key_press(GtkWidget *, GdkEventKey *event, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  webgtk_key_event out{};
  out.type = WEBGTK_KEY_PRESS;
  out.keyval = event->keyval;
  out.keycode = event->hardware_keycode;
  out.modifiers = map_modifiers(event->state);
  out.text = key_text(event->keyval, out.modifiers);
  view->key_pressed = true;
  // Feed the IM context so committed text still reaches text_input; the raw
  // key is forwarded regardless, because the engine needs shortcuts too.
  if (view->im_context)
    gtk_im_context_filter_keypress(view->im_context, event);
  return dispatch_key(view, &out) == WEBGTK_OK ? TRUE : FALSE;
}

gboolean webgtk_on_key_release(GtkWidget *, GdkEventKey *event, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  webgtk_key_event out{};
  out.type = WEBGTK_KEY_RELEASE;
  out.keyval = event->keyval;
  out.keycode = event->hardware_keycode;
  out.modifiers = map_modifiers(event->state);
  out.text = nullptr;
  view->key_pressed = false;
  dispatch_key(view, &out);
  return TRUE;
}

gboolean webgtk_on_focus_in(GtkWidget *, GdkEventFocus *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  dispatch_focus(view, true);
  return FALSE;
}

gboolean webgtk_on_focus_out(GtkWidget *, GdkEventFocus *, gpointer data) {
  auto *view = static_cast<webgtk_view *>(data);
  if (view->destroyed) return FALSE;
  dispatch_focus(view, false);
  return FALSE;
}
#endif

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

  /* An IM context shared by both paths: it turns key sequences into committed
   * text, which is what reaches text_input. */
  view->im_context = gtk_im_context_simple_new();
  g_signal_connect(view->im_context, "commit", G_CALLBACK(webgtk_on_im_commit),
                   view);

#if WEBGTK_GTK4
  GtkEventController *motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(webgtk_on_motion), view);
  gtk_widget_add_controller(area, motion);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed", G_CALLBACK(webgtk_on_button_pressed), view);
  g_signal_connect(click, "released", G_CALLBACK(webgtk_on_button_released),
                   view);
  gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));

  GtkEventController *scroll =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(scroll, "scroll", G_CALLBACK(webgtk_on_scroll), view);
  gtk_widget_add_controller(area, scroll);

  GtkEventController *key = gtk_event_controller_key_new();
  gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key),
                                          view->im_context);
  g_signal_connect(key, "key-pressed", G_CALLBACK(webgtk_on_key_pressed), view);
  g_signal_connect(key, "key-released", G_CALLBACK(webgtk_on_key_released),
                   view);
  gtk_widget_add_controller(area, key);

  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(webgtk_on_focus_enter), view);
  g_signal_connect(focus, "leave", G_CALLBACK(webgtk_on_focus_leave), view);
  gtk_widget_add_controller(area, focus);
#else
  /* GTK3 forwards input through traditional widget signals. */
  g_signal_connect(area, "motion-notify-event",
                   G_CALLBACK(webgtk_on_motion_notify), view);
  g_signal_connect(area, "button-press-event",
                   G_CALLBACK(webgtk_on_button_press), view);
  g_signal_connect(area, "button-release-event",
                   G_CALLBACK(webgtk_on_button_release), view);
  g_signal_connect(area, "scroll-event", G_CALLBACK(webgtk_on_scroll_event),
                   view);
  g_signal_connect(area, "key-press-event", G_CALLBACK(webgtk_on_key_press),
                   view);
  g_signal_connect(area, "key-release-event", G_CALLBACK(webgtk_on_key_release),
                   view);
  g_signal_connect(area, "focus-in-event", G_CALLBACK(webgtk_on_focus_in), view);
  g_signal_connect(area, "focus-out-event", G_CALLBACK(webgtk_on_focus_out),
                   view);
#endif

  return area;
}

}  // namespace

/* ---------------------------------------------------------------- C ABI -- */

extern "C" {

/* Shared by the C ABI constructor and the GTK-major-specific ones. */
static webgtk_view *webgtk_view_construct(int width, int height) {
  if (!valid_size(width, height)) return nullptr;

  auto *view = WEBGTK_VIEW(g_object_new(WEBGTK_TYPE_VIEW, nullptr));

  view->width = width;
  view->height = height;
  view->ops = webgtk::default_backend();
  if (!view->ops) {
    g_object_unref(view);
    return nullptr;
  }
  view->backend = view->ops->create(width, height);
  if (!view->backend) {
    g_object_unref(view);
    return nullptr;
  }
  /* Give the backend its host view before anything can post: the mock uses it
   * to deliver posts synchronously, the engine to marshal them across. */
  if (view->ops->set_host_view) view->ops->set_host_view(view->backend, view);
  view->area = create_area(view);
  if (!view->area) {
    g_object_unref(view);
    return nullptr;
  }
  return view;
}

webgtk_view *webgtk_view_new(int width, int height) {
  return webgtk_view_construct(width, height);
}

#if defined(WEBGTK_GTK4) && WEBGTK_GTK4
WebGtkView *webgtk_view_new_gtk4(int width, int height) {
  return webgtk_view_construct(width, height);
}
#else
WebGtkView *webgtk_view_new_gtk3(int width, int height) {
  return webgtk_view_construct(width, height);
}
#endif

void webgtk_view_destroy(webgtk_view *view) {
  if (!view) return;
  teardown(view);
  g_object_unref(view);
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

webgtk_status webgtk_view_add_script_message_handler(
    webgtk_view *view, const char *handler_name,
    WebGtkScriptMessageCallback callback, void *user_data) {
  if (!view || !handler_name || !handler_name[0] || !callback)
    return WEBGTK_ERROR_INVALID_ARGUMENT;
  if (view->destroyed) return WEBGTK_ERROR_INVALID_ARGUMENT;

  /* Record the handler first, then tell the backend. If the backend cannot do
   * IPC the handler stays registered but unreachable, and the return value
   * says so rather than silently pretending the post will arrive. */
  view->script_handlers[handler_name] =
      webgtk::ScriptMessageHandler{callback, user_data};

  if (!view->ops->add_script_message_handler)
    return WEBGTK_ERROR_NOT_IMPLEMENTED;
  return view->ops->add_script_message_handler(view->backend, handler_name);
}

webgtk_status webgtk_view_remove_script_message_handler(
    webgtk_view *view, const char *handler_name) {
  if (!view || !handler_name) return WEBGTK_ERROR_INVALID_ARGUMENT;
  view->script_handlers.erase(handler_name);

  if (!view->ops->remove_script_message_handler) return WEBGTK_OK;
  return view->ops->remove_script_message_handler(view->backend, handler_name);
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

/* The scalar helpers build the same rich event the GTK widgets do and go
 * through the same dispatch, so both entry points behave identically. */
webgtk_status webgtk_view_mouse_move(webgtk_view *view, int x, int y,
                                     unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  webgtk_mouse_event event{};
  event.type = WEBGTK_MOUSE_MOVE;
  event.x = x;
  event.y = y;
  event.modifiers = modifiers;
  return webgtk_view_send_mouse_event(view, &event);
}

webgtk_status webgtk_view_mouse_button(webgtk_view *view, int x, int y,
                                       webgtk_mouse_button button, int pressed,
                                       unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  webgtk_mouse_event event{};
  event.type = pressed ? WEBGTK_MOUSE_PRESS : WEBGTK_MOUSE_RELEASE;
  event.x = x;
  event.y = y;
  event.button = button;
  event.modifiers = modifiers;
  event.click_count = 1;
  return webgtk_view_send_mouse_event(view, &event);
}

webgtk_status webgtk_view_mouse_scroll(webgtk_view *view, int x, int y,
                                       double dx, double dy,
                                       unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  /* The rich scroll event has no position, so record the caller's before
   * dispatching; the scalar op path in dispatch_scroll reads it back. */
  view->pointer_x = x;
  view->pointer_y = y;
  view->pointer_valid = true;
  webgtk_scroll_event event{};
  event.dx = dx;
  event.dy = dy;
  event.modifiers = modifiers;
  return webgtk_view_send_scroll_event(view, &event);
}

webgtk_status webgtk_view_key_event(webgtk_view *view, uint32_t keysym,
                                    int pressed, unsigned modifiers) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  webgtk_key_event event{};
  event.type = pressed ? WEBGTK_KEY_PRESS : WEBGTK_KEY_RELEASE;
  event.keyval = keysym;
  event.keycode = keysym;
  event.modifiers = modifiers;
  event.text = pressed ? key_text(keysym, modifiers) : nullptr;
  return webgtk_view_send_key_event(view, &event);
}

webgtk_status webgtk_view_text_input(webgtk_view *view, const char *utf8) {
  if (!view || !utf8) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->text_input(view->backend, utf8);
}

webgtk_status webgtk_view_send_mouse_event(webgtk_view *view,
                                           const webgtk_mouse_event *event) {
  if (!view || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  if (view->destroyed) return WEBGTK_ERROR_INVALID_ARGUMENT;
  view->pointer_x = event->x;
  view->pointer_y = event->y;
  view->pointer_valid = true;
  return dispatch_mouse(view, event);
}

webgtk_status webgtk_view_send_key_event(webgtk_view *view,
                                         const webgtk_key_event *event) {
  if (!view || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  if (view->destroyed) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return dispatch_key(view, event);
}

webgtk_status webgtk_view_send_scroll_event(webgtk_view *view,
                                            const webgtk_scroll_event *event) {
  if (!view || !event) return WEBGTK_ERROR_INVALID_ARGUMENT;
  if (view->destroyed) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return dispatch_scroll(view, event);
}

webgtk_status webgtk_view_send_focus_event(webgtk_view *view, int focused) {
  if (!view) return WEBGTK_ERROR_INVALID_ARGUMENT;
  if (view->destroyed) return WEBGTK_ERROR_INVALID_ARGUMENT;
  dispatch_focus(view, focused != 0);
  return WEBGTK_OK;
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

/* ------------------------------------------- introspection-only entry points */

webgtk_status webgtk_view_load_html_string(WebGtkView *view, const char *html) {
  if (!view || !html) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->load_html(view->backend, html, std::strlen(html));
}

webgtk_status webgtk_view_run_javascript(WebGtkView *view, const char *script) {
  if (!view || !script) return WEBGTK_ERROR_INVALID_ARGUMENT;
  return view->ops->evaluate_javascript(view->backend, script, on_js_result,
                                        view);
}

GBytes *webgtk_view_render_frame(WebGtkView *view) {
  if (!view) return nullptr;
  webgtk::Bitmap bitmap{};
  if (view->ops->render(view->backend, &bitmap) != WEBGTK_OK) return nullptr;
  if (!publish_frame(view, bitmap)) return nullptr;
  return g_bytes_new(view->frame.data, view->frame.byte_size());
}

}  // extern "C"

/* ------------------------------------------------------------ enum GTypes -- */

/* Hand-written boxed enum GTypes. g-ir-scanner picks these up from the
 * "get_type" naming convention, which is what turns the two C enums into real
 * Python enums instead of bare ints. */
GType webgtk_status_get_type(void) {
  static gsize id = 0;
  if (g_once_init_enter(&id)) {
    static const GEnumValue values[] = {
        {WEBGTK_OK, "WEBGTK_OK", "ok"},
        {WEBGTK_ERROR_INVALID_ARGUMENT, "WEBGTK_ERROR_INVALID_ARGUMENT",
         "invalid-argument"},
        {WEBGTK_ERROR_NOT_IMPLEMENTED, "WEBGTK_ERROR_NOT_IMPLEMENTED",
         "not-implemented"},
        {WEBGTK_ERROR_INTERNAL, "WEBGTK_ERROR_INTERNAL", "internal"},
        {0, nullptr, nullptr},
    };
    GType type = g_enum_register_static("WebGtkStatus", values);
    g_once_init_leave(&id, type);
  }
  return (GType)id;
}

GType webgtk_mouse_button_get_type(void) {
  static gsize id = 0;
  if (g_once_init_enter(&id)) {
    static const GEnumValue values[] = {
        {WEBGTK_MOUSE_LEFT, "WEBGTK_MOUSE_LEFT", "left"},
        {WEBGTK_MOUSE_MIDDLE, "WEBGTK_MOUSE_MIDDLE", "middle"},
        {WEBGTK_MOUSE_RIGHT, "WEBGTK_MOUSE_RIGHT", "right"},
        {0, nullptr, nullptr},
    };
    GType type = g_enum_register_static("WebGtkMouseButton", values);
    g_once_init_leave(&id, type);
  }
  return (GType)id;
}
