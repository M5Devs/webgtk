/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * WebGTK - GObject interface.
 *
 * This is the language-binding surface: WebGtkView is a real GObject, so the
 * library is usable from PyGObject, JavaScript, Rust, Vala and anything else
 * that speaks GObject Introspection. The flat C ABI in <webgtk/webgtk.h> stays
 * dependency-free for embedders that only want the handful of functions.
 *
 * WebGtkView *composes* a GtkGLArea rather than deriving from it. The child is
 * exposed through the read-only "widget" property; add that widget to your
 * container:
 *
 *     WebGtkView *view = webgtk_view_new_gtk3(800, 600);
 *     gtk_container_add(GTK_CONTAINER(window),
 *                       GTK_WIDGET(webgtk_view_get_widget(view)));
 *
 * The header is compiled once per GTK major version: WEBGTK_GTK4 selects the
 * GTK4 flavour of the constructor and the matching GtkWidget type.
 */

#ifndef WEBGTK_WEBGTK_VIEW_H
#define WEBGTK_WEBGTK_VIEW_H

#include <glib-object.h>
#include <gtk/gtk.h>

#include <webgtk/webgtk.h>

G_BEGIN_DECLS

/* The GObject instance type. It is the same object the C ABI calls
 * `webgtk_view`, so the two APIs are freely interchangeable. */
typedef webgtk_view WebGtkView;
typedef struct _WebGtkViewClass WebGtkViewClass;

/* Camel-case aliases of the C ABI enums. g-ir-scanner matches a C type to its
 * GType by name, so the exported API uses these spellings. */
typedef webgtk_status WebGtkStatus;
typedef webgtk_mouse_button WebGtkMouseButton;

#define WEBGTK_TYPE_VIEW (webgtk_view_get_type())
#define WEBGTK_VIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBGTK_TYPE_VIEW, WebGtkView))
#define WEBGTK_IS_VIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), WEBGTK_TYPE_VIEW))
#define WEBGTK_VIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), WEBGTK_TYPE_VIEW, WebGtkViewClass))
#define WEBGTK_IS_VIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), WEBGTK_TYPE_VIEW))
#define WEBGTK_VIEW_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj), WEBGTK_TYPE_VIEW, WebGtkViewClass))

struct _WebGtkViewClass {
  GObjectClass parent_class;

  /* Signals. */
  void (*javascript_result)(WebGtkView *view, const char *json);

  /* Keep this last so the struct can grow without breaking subclasses. */
  gpointer _webgtk_reserved[8];
};

WEBGTK_EXPORT GType webgtk_view_get_type(void) G_GNUC_CONST;

/* Enum GTypes, so bindings see webgtk_status and webgtk_mouse_button as real
 * enums instead of opaque integers. */
WEBGTK_EXPORT GType webgtk_status_get_type(void) G_GNUC_CONST;
WEBGTK_EXPORT GType webgtk_mouse_button_get_type(void) G_GNUC_CONST;

/*
 * Constructors. Exactly one is exported per library, matching the GTK major
 * version it was built against, so a typelib never advertises a constructor
 * that would fail at run time.
 */
#if defined(WEBGTK_GTK4) && WEBGTK_GTK4
WEBGTK_EXPORT WebGtkView *webgtk_view_new_gtk4(int width, int height);
#else
WEBGTK_EXPORT WebGtkView *webgtk_view_new_gtk3(int width, int height);
#endif

/* Loads HTML from a NUL-terminated string. The size-taking variant stays in
 * the flat C ABI; bindings get this one. */
WEBGTK_EXPORT WebGtkStatus webgtk_view_load_html_string(WebGtkView *view,
                                                        const char *html);

/*
 * Evaluates `script` and emits "javascript-result" with the JSON encoded
 * result. Emission is marshalled to the main context, so the signal arrives on
 * the GTK main thread whether or not the backend is asynchronous.
 */
WEBGTK_EXPORT WebGtkStatus webgtk_view_run_javascript(WebGtkView *view,
                                                      const char *script);

/*
 * Renders one frame and returns it as RGBA8 bytes (width * height * 4, top
 * left origin). Returns NULL if the backend failed. The caller owns the
 * GBytes.
 */
WEBGTK_EXPORT GBytes *webgtk_view_render_frame(WebGtkView *view);

G_END_DECLS

#endif /* WEBGTK_WEBGTK_VIEW_H */
