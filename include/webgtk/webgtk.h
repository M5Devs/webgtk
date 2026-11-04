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

/*
 * Rich input events.
 *
 * The scalar helpers further down (webgtk_view_mouse_move and friends) are
 * thin wrappers over these structs; the struct form is the one to use when the
 * embedder already has the extra detail, because it carries the modifiers,
 * click count, hardware keycode and committed text the engine needs to build a
 * faithful input event. Coordinates are in backing-buffer pixels, top-left
 * origin, matching webgtk_view_render.
 */
typedef enum {
  WEBGTK_MOUSE_PRESS = 0,
  WEBGTK_MOUSE_RELEASE = 1,
  WEBGTK_MOUSE_MOVE = 2
} webgtk_mouse_event_type;

typedef struct {
  webgtk_mouse_event_type type;
  double x;
  double y;
  webgtk_mouse_button button; /* only meaningful for press/release */
  unsigned modifiers;         /* WEBGTK_MOD_* bitmask */
  int click_count;            /* 1 for a single click, 2 for a double, ... */
} webgtk_mouse_event;

typedef enum {
  WEBGTK_KEY_PRESS = 0,
  WEBGTK_KEY_RELEASE = 1
} webgtk_key_event_type;

typedef struct {
  webgtk_key_event_type type;
  uint32_t keyval;    /* GDK keyval */
  uint32_t keycode;   /* hardware keycode */
  unsigned modifiers; /* WEBGTK_MOD_* bitmask */
  const char *text;   /* UTF-8 committed text, or NULL when not printable */
} webgtk_key_event;

typedef struct {
  double dx;
  double dy;
  unsigned modifiers; /* WEBGTK_MOD_* bitmask */
} webgtk_scroll_event;

typedef struct webgtk_view webgtk_view;

/*
 * Script message callback. `view` is the view the message arrived on,
 * `handler_name` the registered handler it was posted to and `payload` the
 * UTF-8 string the page passed (objects are JSON-encoded by the injected
 * bridge). Both strings are owned by the caller and only valid for the
 * duration of the call.
 *
 * Invoked synchronously from the GTK main thread, on the same turn the
 * backend delivered the message.
 */
typedef void (*WebGtkScriptMessageCallback)(webgtk_view *view,
                                            const char *handler_name,
                                            const char *json_or_text_payload,
                                            void *user_data);

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

/*
 * Registers `handler_name` as a script message handler. Registering a name
 * again replaces the previous callback. The page reaches it through the
 * injected bridge:
 *
 *     window.webgtk.postMessage("handler_name", payload)
 *
 * `payload` may be a string or any JSON-serialisable value; non-strings are
 * stringified before dispatch. Registration may happen before or after the
 * page loads; the bridge is installed on every document. Returns
 * WEBGTK_ERROR_INVALID_ARGUMENT if `view`, `handler_name` or `callback` is
 * NULL, or the name is empty.
 */
WEBGTK_EXPORT webgtk_status webgtk_view_add_script_message_handler(
    webgtk_view *view, const char *handler_name,
    WebGtkScriptMessageCallback callback, void *user_data);

/* Removes a handler registered with webgtk_view_add_script_message_handler.
 * Removing an unknown name is a no-op. Returns WEBGTK_ERROR_INVALID_ARGUMENT
 * if `view` or `handler_name` is NULL. */
WEBGTK_EXPORT webgtk_status webgtk_view_remove_script_message_handler(
    webgtk_view *view, const char *handler_name);

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

/*
 * Rich input forwarding. These carry the full event (modifiers, click count,
 * hardware keycode, committed text) straight through to the backend, which is
 * what the GTK3/GTK4 widgets use internally. Prefer them when the detail is
 * available. Each returns WEBGTK_ERROR_INVALID_ARGUMENT if `view` or `event`
 * is NULL.
 */
WEBGTK_EXPORT webgtk_status webgtk_view_send_mouse_event(
    webgtk_view *view, const webgtk_mouse_event *event);
WEBGTK_EXPORT webgtk_status webgtk_view_send_key_event(
    webgtk_view *view, const webgtk_key_event *event);
WEBGTK_EXPORT webgtk_status webgtk_view_send_scroll_event(
    webgtk_view *view, const webgtk_scroll_event *event);

/* Reports that the view gained or lost keyboard focus. */
WEBGTK_EXPORT webgtk_status webgtk_view_send_focus_event(webgtk_view *view,
                                                         int focused);

WEBGTK_EXPORT const char *webgtk_status_string(webgtk_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WEBGTK_WEBGTK_H */
