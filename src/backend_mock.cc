// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Mock backend: a self-contained software renderer with no engine dependency.
// It exists so the view, the build and the tests can be exercised end to end
// before the Blink/V8 archive is linked in, and so the UI can be developed
// without a 685 MB download.
//
// The output is fully deterministic for a given input sequence: same state in,
// same pixels out. Tests rely on that.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "webgtk_backend_ops.h"

namespace webgtk {
namespace {

struct Rgba {
  uint8_t r, g, b, a;
};

constexpr Rgba kBackgroundIdle{24, 24, 28, 255};
constexpr Rgba kBackgroundLoaded{30, 40, 60, 255};
constexpr Rgba kBorder{60, 120, 220, 255};
constexpr Rgba kCursor{240, 180, 60, 255};
constexpr Rgba kKeyOn{80, 220, 120, 255};
constexpr Rgba kKeyOff{200, 70, 70, 255};

struct MockState {
  int width = 0;
  int height = 0;
  std::string uri;
  std::string html;
  bool has_content = false;

  int mouse_x = 0;
  int mouse_y = 0;
  bool mouse_pressed = false;
  unsigned modifiers = 0;
  int click_count = 0;
  int scroll_count = 0;
  uint32_t last_keysym = 0;
  int key_count = 0;
  std::string text;

  // Backing buffer handed to the view; reused between frames.
  std::vector<uint8_t> pixels;
};

void fill_rect(MockState *state, int x0, int y0, int w, int h, Rgba color) {
  const int x1 = std::min(x0 + w, state->width);
  const int y1 = std::min(y0 + h, state->height);
  for (int y = std::max(y0, 0); y < y1; ++y) {
    uint8_t *row = state->pixels.data() +
                   static_cast<size_t>(y) * state->width * 4;
    for (int x = std::max(x0, 0); x < x1; ++x) {
      uint8_t *p = row + static_cast<size_t>(x) * 4;
      p[0] = color.r;
      p[1] = color.g;
      p[2] = color.b;
      p[3] = color.a;
    }
  }
}

void draw_border(MockState *state, Rgba color) {
  fill_rect(state, 0, 0, state->width, 1, color);
  fill_rect(state, 0, state->height - 1, state->width, 1, color);
  fill_rect(state, 0, 0, 1, state->height, color);
  fill_rect(state, state->width - 1, 0, 1, state->height, color);
}

void render_mock(MockState *state) {
  const Rgba background = state->has_content ? kBackgroundLoaded : kBackgroundIdle;
  fill_rect(state, 0, 0, state->width, state->height, background);
  draw_border(state, kBorder);

  // One 8x8 status marker in the top-left corner: green once the mock has seen
  // a key event, red before that.
  fill_rect(state, 8, 8, 8, 8, state->key_count > 0 ? kKeyOn : kKeyOff);

  // A block that follows the pointer, so input forwarding is observable in the
  // rendered output.
  fill_rect(state, state->mouse_x - 8, state->mouse_y - 8, 16, 16, kCursor);

  // One vertical bar per click, from the left edge, so clicks accumulate.
  for (int i = 0; i < state->click_count; ++i) {
    fill_rect(state, 8 + i * 6, state->height - 24, 3, 16, kCursor);
  }
}

void *mock_create(int width, int height) {
  auto *state = new (std::nothrow) MockState();
  if (!state) return nullptr;
  state->width = width;
  state->height = height;
  state->mouse_x = width / 2;
  state->mouse_y = height / 2;
  state->pixels.assign(static_cast<size_t>(width) * height * 4, 0);
  return state;
}

void mock_destroy(void *self) { delete static_cast<MockState *>(self); }

const char *mock_name() { return "mock"; }

webgtk_status mock_load_uri(void *self, const char *uri) {
  auto *state = static_cast<MockState *>(self);
  state->uri = uri ? uri : "";
  state->html.clear();
  state->has_content = true;
  std::fprintf(stderr, "[webgtk-mock] load_uri: %s\n", state->uri.c_str());
  return WEBGTK_OK;
}

webgtk_status mock_load_html(void *self, const char *html, size_t length) {
  auto *state = static_cast<MockState *>(self);
  state->html.assign(html ? html : "", length);
  state->uri.clear();
  state->has_content = true;
  std::fprintf(stderr, "[webgtk-mock] load_html: %zu bytes\n", length);
  return WEBGTK_OK;
}

std::string json_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

webgtk_status mock_evaluate_javascript(
    void *self, const char *script,
    void (*callback)(const char *json_result, void *user_data),
    void *user_data) {
  auto *state = static_cast<MockState *>(self);
  // The mock echoes the script instead of executing it. Callers get a
  // well-formed JSON envelope so plumbing can be tested before V8 exists.
  const std::string json =
      "{\"backend\":\"mock\",\"loaded\":" +
      std::string(state->has_content ? "true" : "false") +
      ",\"script\":\"" + json_escape(script ? script : "") + "\"}";
  if (callback) callback(json.c_str(), user_data);
  return WEBGTK_OK;
}

webgtk_status mock_set_size(void *self, int width, int height) {
  auto *state = static_cast<MockState *>(self);
  if (width <= 0 || height <= 0) return WEBGTK_ERROR_INVALID_ARGUMENT;
  state->width = width;
  state->height = height;
  state->pixels.assign(static_cast<size_t>(width) * height * 4, 0);
  state->mouse_x = std::min(state->mouse_x, width - 1);
  state->mouse_y = std::min(state->mouse_y, height - 1);
  return WEBGTK_OK;
}

webgtk_status mock_render(void *self, Bitmap *out) {
  auto *state = static_cast<MockState *>(self);
  render_mock(state);
  out->data = state->pixels.data();
  out->width = state->width;
  out->height = state->height;
  out->stride = state->width * 4;
  return WEBGTK_OK;
}

webgtk_status mock_mouse_move(void *self, int x, int y, unsigned modifiers) {
  auto *state = static_cast<MockState *>(self);
  state->mouse_x = x;
  state->mouse_y = y;
  state->modifiers = modifiers;
  return WEBGTK_OK;
}

webgtk_status mock_mouse_button(void *self, int x, int y,
                                webgtk_mouse_button button, int pressed,
                                unsigned modifiers) {
  auto *state = static_cast<MockState *>(self);
  state->mouse_x = x;
  state->mouse_y = y;
  state->mouse_pressed = pressed != 0;
  state->modifiers = modifiers;
  if (pressed) ++state->click_count;
  std::fprintf(stderr, "[webgtk-mock] button=%d pressed=%d at %d,%d\n", button,
               pressed, x, y);
  return WEBGTK_OK;
}

webgtk_status mock_mouse_scroll(void *self, int x, int y, double dx, double dy,
                                unsigned modifiers) {
  auto *state = static_cast<MockState *>(self);
  state->mouse_x = x;
  state->mouse_y = y;
  state->modifiers = modifiers;
  ++state->scroll_count;
  std::fprintf(stderr, "[webgtk-mock] scroll dx=%f dy=%f\n", dx, dy);
  return WEBGTK_OK;
}

webgtk_status mock_key_event(void *self, uint32_t keysym, int pressed,
                             unsigned modifiers) {
  auto *state = static_cast<MockState *>(self);
  state->last_keysym = keysym;
  state->modifiers = modifiers;
  if (pressed) ++state->key_count;
  std::fprintf(stderr, "[webgtk-mock] key=0x%x pressed=%d\n", keysym, pressed);
  return WEBGTK_OK;
}

webgtk_status mock_text_input(void *self, const char *utf8) {
  auto *state = static_cast<MockState *>(self);
  state->text += utf8 ? utf8 : "";
  std::fprintf(stderr, "[webgtk-mock] text_input: %s\n", utf8 ? utf8 : "");
  return WEBGTK_OK;
}

const BackendOps kMockOps = {
    mock_name,
    mock_create,
    mock_destroy,
    mock_load_uri,
    mock_load_html,
    mock_evaluate_javascript,
    mock_set_size,
    mock_render,
    mock_mouse_move,
    mock_mouse_button,
    mock_mouse_scroll,
    mock_key_event,
    mock_text_input,
};

}  // namespace

const BackendOps *mock_backend() { return &kMockOps; }

// The real Blink/V8 backend is linked in only when the engine archive is
// available. Until then this returns nullptr and the mock stands in.
const BackendOps *engine_backend() {
#if defined(WEBGTK_HAVE_ENGINE)
  extern const BackendOps *engine_backend_impl();
  return engine_backend_impl();
#else
  return nullptr;
#endif
}

const BackendOps *default_backend() {
  const BackendOps *engine = engine_backend();
  return engine ? engine : mock_backend();
}

}  // namespace webgtk
