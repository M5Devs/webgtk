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

static int count_non_uniform(const unsigned char *pixels, size_t size) {
  int distinct = 0;
  for (size_t i = 4; i + 3 < size; i += 4) {
    if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] ||
        pixels[i + 2] != pixels[2])
      ++distinct;
  }
  return distinct;
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
  test_rendering();
  test_status_strings();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
