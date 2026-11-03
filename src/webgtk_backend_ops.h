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
};

// Backend registry. Exactly one backend is selected at view construction.
const BackendOps *mock_backend();

// The Blink/V8 engine backend. Only defined when built against the engine
// archive; returns nullptr otherwise so the mock can stand in.
const BackendOps *engine_backend();

// Returns the engine backend when available, otherwise the mock backend.
const BackendOps *default_backend();

}  // namespace webgtk

#endif  // WEBGTK_BACKEND_OPS_H
