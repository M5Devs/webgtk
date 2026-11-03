/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * WebGTK - a lightweight GTK webview built on the pruned Blink/V8 core.
 *
 * Public C ABI. This header is the only supported interface for embedders;
 * the GTK view is a GObject internally, but callers never need GObject or
 * GTK headers to drive a view.
 */

#ifndef WEBGTK_WEBGTK_H
#define WEBGTK_WEBGTK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define WEBGTK_EXPORT __declspec(dllexport)
#else
#define WEBGTK_EXPORT __attribute__((visibility("default")))
#endif

/* Status codes returned across the C ABI. */
typedef enum {
  WEBGTK_OK = 0,
  WEBGTK_ERROR_INVALID_ARGUMENT = 1,
  WEBGTK_ERROR_NOT_IMPLEMENTED = 2,
  WEBGTK_ERROR_INTERNAL = 3
} webgtk_status;

/* Mouse button identifiers for input forwarding. */
typedef enum {
  WEBGTK_MOUSE_LEFT = 0,
  WEBGTK_MOUSE_MIDDLE = 1,
  WEBGTK_MOUSE_RIGHT = 2
} webgtk_mouse_button;

typedef struct webgtk_view webgtk_view;

/* Modifier bitmask for input forwarding (matches common GTK/X11 masks). */
enum {
  WEBGTK_MOD_SHIFT = 1 << 0,
  WEBGTK_MOD_CONTROL = 1 << 1,
  WEBGTK_MOD_ALT = 1 << 2,
  WEBGTK_MOD_SUPER = 1 << 3
};

/*
 * Returns the native GtkWidget pointer for embedding in a GTK container.
 * The returned pointer is owned by the view; callers must not unref it.
 */
WEBGTK_EXPORT void *webgtk_view_get_widget(webgtk_view *view);

/* Creates a new view. `width`/`height` are the initial backing-buffer size in
 * physical pixels. Returns NULL only on allocation failure. */
WEBGTK_EXPORT webgtk_view *webgtk_view_new(int width, int height);

/* Name of the active rendering backend ("mock" or "engine"). */
WEBGTK_EXPORT const char *webgtk_backend_name(webgtk_view *view);

/*
 * Destroys a view previously created with webgtk_view_new and frees every
 * resource it owns. If the widget was added to a GTK container it is removed
 * from that container first.
 */
WEBGTK_EXPORT void webgtk_view_destroy(webgtk_view *view);

/* Loads `uri` (http(s)://, file:// or data:). Returns WEBGTK_OK on accept. */
WEBGTK_EXPORT webgtk_status webgtk_view_load_uri(webgtk_view *view,
                                                 const char *uri);

/* Loads HTML from a caller-owned UTF-8 buffer. The buffer must outlive the
 * call; the backend copies whatever it needs. */
WEBGTK_EXPORT webgtk_status webgtk_view_load_html(webgtk_view *view,
                                                  const char *html,
                                                  size_t length);

/*
 * Evaluates `script` and, on completion, invokes `callback` with the JSON
 * encoded result. The callback may be invoked synchronously (mock backend) or
 * asynchronously (real engine). `callback` may be NULL.
 */
WEBGTK_EXPORT webgtk_status webgtk_view_evaluate_javascript(
    webgtk_view *view, const char *script,
    void (*callback)(const char *json_result, void *user_data),
    void *user_data);

/* Resizes the backing buffer. */
WEBGTK_EXPORT webgtk_status webgtk_view_set_size(webgtk_view *view, int width,
                                                 int height);

/* Runs one frame and copies the RGBA8 backing buffer into `buffer`, which must
 * hold at least width * height * 4 bytes. Returns WEBGTK_ERROR_INVALID_ARGUMENT
 * if `buffer` is too small or the view has no size. */
WEBGTK_EXPORT webgtk_status webgtk_view_render(webgtk_view *view,
                                               uint8_t *buffer,
                                               size_t buffer_size);

/* Input forwarding. Coordinates are in backing-buffer pixels. */
WEBGTK_EXPORT webgtk_status webgtk_view_mouse_move(webgtk_view *view, int x,
                                                   int y, unsigned modifiers);
WEBGTK_EXPORT webgtk_status webgtk_view_mouse_button(webgtk_view *view,
                                                     int x, int y,
                                                     webgtk_mouse_button button,
                                                     int pressed,
                                                     unsigned modifiers);
WEBGTK_EXPORT webgtk_status webgtk_view_mouse_scroll(webgtk_view *view, int x,
                                                     int y, double dx,
                                                     double dy,
                                                     unsigned modifiers);
WEBGTK_EXPORT webgtk_status webgtk_view_key_event(webgtk_view *view,
                                                  uint32_t keysym, int pressed,
                                                  unsigned modifiers);
/* Sends a UTF-8 string as committed text input (no keycode). */
WEBGTK_EXPORT webgtk_status webgtk_view_text_input(webgtk_view *view,
                                                   const char *utf8);

WEBGTK_EXPORT const char *webgtk_status_string(webgtk_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEBGTK_WEBGTK_H */
