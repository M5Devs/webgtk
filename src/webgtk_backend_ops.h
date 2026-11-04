// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Backend vtable. The GTK view (webgtk_view.cc) is deliberately ignorant of
// the engine: it owns a surface and forwards every event through this
// interface. The real Blink/V8 backend and the mock backend are
// interchangeable implementations.

#ifndef WEBGTK_BACKEND_OPS_H
#define WEBGTK_BACKEND_OPS_H

#include <cstddef>
#include <cstdint>

#include <webgtk/webgtk.h>

namespace webgtk {

// A CPU-side RGBA8 image, top-left origin, no padding between rows.
struct Bitmap {
  uint8_t *data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  // bytes per row (== width * 4)

  bool empty() const { return data == nullptr || width <= 0 || height <= 0; }
  size_t byte_size() const {
    return stride > 0 ? static_cast<size_t>(stride) * static_cast<size_t>(height)
                      : 0;
  }
};

struct BackendOps {
  // Human-readable backend name, used by the demos and tests.
  const char *(*name)();

  // Lifecycle. `create` returns an opaque backend instance owned by the view.
  void *(*create)(int width, int height);
  void (*destroy)(void *self);

  webgtk_status (*load_uri)(void *self, const char *uri);
  webgtk_status (*load_html)(void *self, const char *html, size_t length);
  webgtk_status (*evaluate_javascript)(
      void *self, const char *script,
      void (*callback)(const char *json_result, void *user_data),
      void *user_data);

  webgtk_status (*set_size)(void *self, int width, int height);

  // Renders one frame into `out`. Implementations own `out.data`; the view
  // copies it out and must not free it.
  webgtk_status (*render)(void *self, Bitmap *out);

  webgtk_status (*mouse_move)(void *self, int x, int y, unsigned modifiers);
  webgtk_status (*mouse_button)(void *self, int x, int y,
                                webgtk_mouse_button button, int pressed,
                                unsigned modifiers);
  webgtk_status (*mouse_scroll)(void *self, int x, int y, double dx, double dy,
                                unsigned modifiers);
  webgtk_status (*key_event)(void *self, uint32_t keysym, int pressed,
                             unsigned modifiers);
  webgtk_status (*text_input)(void *self, const char *utf8);

  // Rich input. These carry the full event, so a backend can build a faithful
  // engine event (click count, hardware keycode, committed text, focus). The
  // scalar ops above stay for embedders that only have the simple form; a
  // backend must implement whichever it advertises and leave the rest NULL.
  webgtk_status (*send_mouse_event)(void *self, const webgtk_mouse_event *event);
  webgtk_status (*send_key_event)(void *self, const webgtk_key_event *event);
  webgtk_status (*send_scroll_event)(void *self,
                                     const webgtk_scroll_event *event);
  webgtk_status (*send_focus_event)(void *self, int focused);

  // Script message handlers. `add` makes the engine route posts addressed to
  // `name` back through the view's dispatch helper; `remove` stops that. Both
  // are optional: a backend that cannot do IPC leaves them NULL and the view
  // reports the handler as unsupported. See kWebGtkBridgeScript.
  webgtk_status (*add_script_message_handler)(void *self, const char *name);
  webgtk_status (*remove_script_message_handler)(void *self, const char *name);

  // Hands the backend its host view so it can deliver script messages with
  // webgtk_view_dispatch_script_message(). Called once, right after create(),
  // before any other call. Optional; NULL for a backend that never posts.
  void (*set_host_view)(void *self, webgtk_view *view);
};

// Defined in webgtk_view.cc. Backends call this on the GTK main thread when
// the page posts a script message. It invokes every callback registered for
// `name` and then emits "script-message-received"; both happen synchronously,
// so a page post is observed with no main-loop round trip.
extern "C" void webgtk_view_dispatch_script_message(webgtk_view *view,
                                                    const char *name,
                                                    const char *payload);

// Thread-safe sibling of the above for backends whose engine runs off the GTK
// main thread: it copies the strings and hops to the main context before
// dispatching. Still event driven, never polled. Returns false if the view is
// gone or the hop could not be queued.
extern "C" bool webgtk_view_post_script_message(webgtk_view *view,
                                                const char *name,
                                                const char *payload);

// A registered handler: the embedder's callback plus its user data.
struct ScriptMessageHandler {
  WebGtkScriptMessageCallback callback = nullptr;
  void *user_data = nullptr;
};

// The page-side bridge. Backends inject this at document start; it is the
// documented, stable half of the IPC contract:
//
//     window.webgtk.postMessage(name, payload)
//
// `__webgtk_internal_post` is the engine-provided sink. It is intentionally
// not defined here: each backend wires its own native hook to it, which keeps
// the bridge free of any one engine's details.
extern const char kWebGtkBridgeScript[];

// Backend registry. Exactly one backend is selected at view construction.
const BackendOps *mock_backend();

// The Blink/V8 engine backend. Only defined when built against the engine
// archive; returns nullptr otherwise so the mock can stand in.
const BackendOps *engine_backend();

// Returns the engine backend when available, otherwise the mock backend.
const BackendOps *default_backend();

}  // namespace webgtk

#endif  // WEBGTK_BACKEND_OPS_H
