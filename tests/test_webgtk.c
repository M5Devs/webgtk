/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * ABI and behaviour tests for the WebGTK scaffold. Written in C on purpose:
 * it proves the public header is consumable without a C++ compiler or any
 * GObject/GTK headers. Run under xvfb because a GtkGLArea needs a display.
 */

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webgtk/webgtk.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    ++checks;                                                   \
    if (!(cond)) {                                              \
      ++failures;                                               \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    }                                                           \
  } while (0)

static void on_result(const char *json, void *user_data) {
  char *buffer = (char *)user_data;
  snprintf(buffer, 256, "%s", json ? json : "");
}

/* --- script message IPC -------------------------------------------------- */

struct message_log {
  int count;
  char handler[64];
  char payload[128];
};

static void on_script_message(webgtk_view *view, const char *handler_name,
                              const char *payload, void *user_data) {
  (void)view;
  struct message_log *log = (struct message_log *)user_data;
  ++log->count;
  snprintf(log->handler, sizeof(log->handler), "%s",
           handler_name ? handler_name : "");
  snprintf(log->payload, sizeof(log->payload), "%s", payload ? payload : "");
}

static int signal_count = 0;
static char signal_payload[128];

static void on_script_message_signal(webgtk_view *view,
                                     const char *handler_name,
                                     const char *payload, void *user_data) {
  (void)view;
  (void)handler_name;
  (void)user_data;
  ++signal_count;
  snprintf(signal_payload, sizeof(signal_payload), "%s", payload ? payload : "");
}

/* A page reaches native code through window.webgtk.postMessage(); the mock
 * replays that call shape natively, so the whole path is exercised without a
 * JavaScript engine. */
static void test_script_messages(void) {
  webgtk_view *view = webgtk_view_new(160, 120);
  CHECK(view != NULL, "view created for IPC test");

  struct message_log log = {0};
  CHECK(webgtk_view_add_script_message_handler(view, "greeting",
                                               on_script_message,
                                               &log) == WEBGTK_OK,
        "handler registration succeeds");

  signal_count = 0;
  g_signal_connect(view, "script-message-received",
                   G_CALLBACK(on_script_message_signal), NULL);

  /* The dispatch must be synchronous: the callback has already run by the time
   * the evaluate call returns, with no main-loop iteration in between. That is
   * the whole point of replacing a polling loop. */
  CHECK(webgtk_view_evaluate_javascript(
            view, "window.webgtk.postMessage('greeting', 'hello')", NULL,
            NULL) == WEBGTK_OK,
        "page post accepted");
  CHECK(log.count == 1, "callback ran synchronously, no polling needed");
  CHECK(strcmp(log.handler, "greeting") == 0, "handler name delivered");
  CHECK(strcmp(log.payload, "hello") == 0, "payload delivered");
  CHECK(signal_count == 1, "script-message-received signal emitted");
  CHECK(strcmp(signal_payload, "hello") == 0, "signal carries the payload");

  /* A post to a name nobody registered is dropped, exactly as the engine
   * would drop it. */
  CHECK(webgtk_view_evaluate_javascript(
            view, "window.webgtk.postMessage('unknown', 'x')", NULL,
            NULL) == WEBGTK_OK,
        "post to unknown handler is accepted but dropped");
  CHECK(log.count == 1, "unknown handler did not fire");

  /* JSON payloads arrive as the string the page passed. */
  CHECK(webgtk_view_evaluate_javascript(
            view,
            "window.webgtk.postMessage(\"greeting\", \"{\\\"n\\\":42}\")", NULL,
            NULL) == WEBGTK_OK,
        "JSON payload post accepted");
  CHECK(log.count == 2, "second post delivered");
  CHECK(strstr(log.payload, "42") != NULL, "JSON payload preserved");

  CHECK(webgtk_view_remove_script_message_handler(view, "greeting") == WEBGTK_OK,
        "handler removal succeeds");
  CHECK(webgtk_view_evaluate_javascript(
            view, "window.webgtk.postMessage('greeting', 'again')", NULL,
            NULL) == WEBGTK_OK,
        "post after removal is accepted");
  CHECK(log.count == 2, "removed handler no longer fires");

  CHECK(webgtk_view_add_script_message_handler(view, NULL, on_script_message,
                                               &log) ==
            WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL handler name is rejected");
  CHECK(webgtk_view_add_script_message_handler(view, "x", NULL, NULL) ==
            WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL callback is rejected");

  webgtk_view_destroy(view);
}

static int count_non_uniform(const unsigned char *pixels, size_t size) {
  int distinct = 0;
  for (size_t i = 4; i + 3 < size; i += 4) {
    if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] ||
        pixels[i + 2] != pixels[2])
      ++distinct;
  }
  return distinct;
}

/* --- input forwarding ---------------------------------------------------- */

/* Colours the mock renderer uses for its input markers. */
static int is_cursor(const unsigned char *p) {
  return p[0] == 240 && p[1] == 180 && p[2] == 60;
}
static int is_marker_on(const unsigned char *p) {
  return p[0] == 80 && p[1] == 220 && p[2] == 120;
}

static const unsigned char *pixel_at(const unsigned char *px, int w, int x,
                                     int y) {
  return px + ((size_t)y * w + x) * 4;
}

/* The mock draws a crosshair centred on the last forwarded pointer position.
 * The row with the most cursor-coloured pixels is the horizontal arm; its
 * midpoint is the forwarded x, and that row index is the forwarded y. */
static int locate_crosshair(const unsigned char *px, int w, int h, int *cx,
                            int *cy) {
  int best_row = -1, best_count = 0, best_min = 0, best_max = 0;
  for (int y = 0; y < h; ++y) {
    int count = 0, min_x = w, max_x = -1;
    for (int x = 0; x < w; ++x) {
      if (!is_cursor(pixel_at(px, w, x, y))) continue;
      ++count;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
    }
    if (count > best_count) {
      best_count = count;
      best_row = y;
      best_min = min_x;
      best_max = max_x;
    }
  }
  if (best_row < 0) return 0;
  *cx = (best_min + best_max) / 2;
  *cy = best_row;
  return 1;
}

/* The last key code is drawn as 8 cells at the bottom-right, LSB first. */
static unsigned decode_key_bits(const unsigned char *px, int w, int h) {
  unsigned value = 0;
  for (int bit = 0; bit < 8; ++bit) {
    const int x = w - 16 - bit * 10 + 4;
    const int y = h - 16 + 4;
    if (is_marker_on(pixel_at(px, w, x, y))) value |= (1u << bit);
  }
  return value;
}

static void test_input_forwarding(void) {
  const int width = 200;
  const int height = 150;
  const size_t bytes = (size_t)width * height * 4;
  unsigned char *pixels = (unsigned char *)malloc(bytes);
  CHECK(pixels != NULL, "input test buffer allocated");

  webgtk_view *view = webgtk_view_new(width, height);
  CHECK(view != NULL, "view created for input test");
  webgtk_view_load_html(view, "<h1>input</h1>", 13);

  int cx = -1, cy = -1;

  /* A rich mouse move must land the crosshair exactly where it was sent. */
  webgtk_mouse_event move = {0};
  move.type = WEBGTK_MOUSE_MOVE;
  move.x = 60;
  move.y = 40;
  move.modifiers = WEBGTK_MOD_SHIFT;
  CHECK(webgtk_view_send_mouse_event(view, &move) == WEBGTK_OK,
        "send_mouse_event accepts a move");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK, "render after move");
  CHECK(locate_crosshair(pixels, width, height, &cx, &cy),
        "crosshair found after move");
  CHECK(cx == 60 && cy == 40, "move forwarded the exact coordinates");

  /* A press records the button, sets the pressed marker and bumps the count. */
  webgtk_mouse_event press = {0};
  press.type = WEBGTK_MOUSE_PRESS;
  press.x = 80;
  press.y = 50;
  press.button = WEBGTK_MOUSE_RIGHT;
  press.click_count = 2;
  CHECK(webgtk_view_send_mouse_event(view, &press) == WEBGTK_OK,
        "send_mouse_event accepts a press");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK, "render after press");
  CHECK(locate_crosshair(pixels, width, height, &cx, &cy),
        "crosshair found after press");
  CHECK(cx == 80 && cy == 50, "press forwarded the exact coordinates");
  CHECK(!is_cursor(pixel_at(pixels, width, 80, 50)),
        "pressed button marker is no longer cursor-coloured");

  /* A scroll is counted and its deltas retained. */
  webgtk_scroll_event scroll = {0};
  scroll.dx = 0.0;
  scroll.dy = -2.0;
  scroll.modifiers = WEBGTK_MOD_CONTROL;
  CHECK(webgtk_view_send_scroll_event(view, &scroll) == WEBGTK_OK,
        "send_scroll_event accepted");

  /* A key press must carry the keyval through to the rendered bit pattern. */
  webgtk_key_event key = {0};
  key.type = WEBGTK_KEY_PRESS;
  key.keyval = 0x41; /* 'A' */
  key.keycode = 38;
  key.text = "A";
  CHECK(webgtk_view_send_key_event(view, &key) == WEBGTK_OK,
        "send_key_event accepts a press");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK, "render after key");
  CHECK(decode_key_bits(pixels, width, height) == 0x41,
        "keyval forwarded to the rendered bit pattern");
  CHECK(is_marker_on(pixel_at(pixels, width, 12, 12)),
        "key marker lit after a key press");

  /* Focus toggles the focus marker. */
  CHECK(webgtk_view_send_focus_event(view, 1) == WEBGTK_OK, "focus in accepted");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK, "render after focus");
  CHECK(is_marker_on(pixel_at(pixels, width, 24, 12)), "focus marker lit");
  CHECK(webgtk_view_send_focus_event(view, 0) == WEBGTK_OK, "focus out accepted");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK,
        "render after blur");
  CHECK(!is_marker_on(pixel_at(pixels, width, 24, 12)), "focus marker cleared");

  /* NULL arguments are refused rather than dereferenced. */
  CHECK(webgtk_view_send_mouse_event(view, NULL) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL mouse event rejected");
  CHECK(webgtk_view_send_key_event(view, NULL) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL key event rejected");
  CHECK(webgtk_view_send_scroll_event(view, NULL) ==
            WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL scroll event rejected");

  free(pixels);
  webgtk_view_destroy(view);
}

static void test_creation(void) {
  webgtk_view *view = webgtk_view_new(320, 240);
  CHECK(view != NULL, "webgtk_view_new returns a view");
  CHECK(webgtk_view_get_widget(view) != NULL, "view exposes a GtkWidget");
  CHECK(strlen(webgtk_backend_name(view)) > 0, "backend has a name");
  webgtk_view_destroy(view);

  CHECK(webgtk_view_new(0, 240) == NULL, "zero width is rejected");
  CHECK(webgtk_view_new(320, -1) == NULL, "negative height is rejected");
}

static void test_loading_and_js(void) {
  webgtk_view *view = webgtk_view_new(160, 120);
  CHECK(view != NULL, "view created for loading test");

  CHECK(webgtk_view_load_uri(view, "https://example.org/") == WEBGTK_OK,
        "load_uri accepts a URI");
  CHECK(webgtk_view_load_uri(view, NULL) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "load_uri rejects NULL");
  CHECK(webgtk_view_load_html(view, "<h1>hi</h1>", 11) == WEBGTK_OK,
        "load_html accepts a buffer");

  char json[256] = {0};
  CHECK(webgtk_view_evaluate_javascript(view, "1 + 1", on_result, json) ==
            WEBGTK_OK,
        "evaluate_javascript succeeds");
  CHECK(strlen(json) > 0, "javascript callback produced a result");
  CHECK(strstr(json, "1 + 1") != NULL, "result echoes the script");

  webgtk_view_destroy(view);
}

static void test_rendering(void) {
  const int width = 200;
  const int height = 150;
  const size_t bytes = (size_t)width * height * 4;
  unsigned char *pixels = (unsigned char *)malloc(bytes);
  CHECK(pixels != NULL, "pixel buffer allocated");

  webgtk_view *view = webgtk_view_new(width, height);
  CHECK(view != NULL, "view created for render test");

  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK,
        "render fills the buffer");
  CHECK(count_non_uniform(pixels, bytes) > 0, "frame is not a flat colour");

  /* Undersized buffers must be refused rather than overrun. */
  CHECK(webgtk_view_render(view, pixels, 4) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "undersized buffer is rejected");
  CHECK(webgtk_view_render(view, NULL, bytes) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "NULL buffer is rejected");

  /* Input forwarding must be visible in the rendered output. */
  unsigned char *before = (unsigned char *)malloc(bytes);
  memcpy(before, pixels, bytes);
  webgtk_view_mouse_move(view, 20, 20, 0);
  webgtk_view_mouse_button(view, 20, 20, WEBGTK_MOUSE_LEFT, 1,
                           WEBGTK_MOD_CONTROL);
  webgtk_view_mouse_scroll(view, 20, 20, 0.0, -1.0, 0);
  webgtk_view_key_event(view, 0x41, 1, 0);
  webgtk_view_text_input(view, "typed");
  CHECK(webgtk_view_render(view, pixels, bytes) == WEBGTK_OK,
        "second render succeeds");
  CHECK(memcmp(before, pixels, bytes) != 0, "input changed the frame");

  CHECK(webgtk_view_set_size(view, 64, 48) == WEBGTK_OK, "set_size succeeds");
  CHECK(webgtk_view_set_size(view, 0, 48) == WEBGTK_ERROR_INVALID_ARGUMENT,
        "set_size rejects bad input");

  free(before);
  free(pixels);
  webgtk_view_destroy(view);
}

static void test_status_strings(void) {
  CHECK(strcmp(webgtk_status_string(WEBGTK_OK), "ok") == 0, "OK string");
  CHECK(strcmp(webgtk_status_string(WEBGTK_ERROR_INVALID_ARGUMENT),
               "invalid argument") == 0,
        "invalid-argument string");
}

int main(int argc, char **argv) {
#if defined(WEBGTK_GTK4)
  (void)argc;
  (void)argv;
  gtk_init();
#else
  gtk_init(&argc, &argv);
#endif

  test_creation();
  test_loading_and_js();
  test_script_messages();
  test_input_forwarding();
  test_rendering();
  test_status_strings();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
