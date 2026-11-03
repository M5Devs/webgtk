// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal embedder demo. Compiled twice: once against GTK3 (gtk3-demo) and
// once against GTK4 (gtk4-demo). With --selftest it renders a bounded number
// of frames and exits, which is what the CTest suite runs under xvfb.

#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <webgtk/webgtk.h>

#ifndef WEBGTK_GTK4
#define WEBGTK_GTK4 0
#endif

namespace {

// Runs the main context for `iterations` non-blocking passes. Enough for GTK
// to realize the widget and dispatch the GLArea resize/render signals.
void spin(int iterations) {
  for (int i = 0; i < iterations; ++i) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    g_usleep(2000);
  }
}

// Counts pixels that differ from the top-left one; a flat frame scores 0.
int analyze(const std::vector<uint8_t> &pixels) {
  if (pixels.empty()) return -1;
  const uint8_t *first = pixels.data();
  int distinct = 0;
  for (size_t i = 4; i + 3 < pixels.size(); i += 4) {
    if (pixels[i] != first[0] || pixels[i + 1] != first[1] ||
        pixels[i + 2] != first[2])
      ++distinct;
  }
  return distinct;
}

int run_selftest(webgtk_view *view, int width, int height) {
  spin(40);

  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
  if (webgtk_view_render(view, pixels.data(), pixels.size()) != WEBGTK_OK) {
    std::fprintf(stderr, "selftest: render failed\n");
    return 1;
  }

  const int before = analyze(pixels);
  if (before <= 0) {
    std::fprintf(stderr, "selftest: frame is uniform (%d)\n", before);
    return 1;
  }

  // Forwarding input must change what the backend produces.
  webgtk_view_mouse_move(view, width / 4, height / 3, 0);
  webgtk_view_mouse_button(view, width / 4, height / 3, WEBGTK_MOUSE_LEFT, 1, 0);
  webgtk_view_key_event(view, 0x41, 1, 0);
  webgtk_view_text_input(view, "hi");

  std::vector<uint8_t> after(static_cast<size_t>(width) * height * 4, 0);
  if (webgtk_view_render(view, after.data(), after.size()) != WEBGTK_OK) {
    std::fprintf(stderr, "selftest: second render failed\n");
    return 1;
  }
  // Byte-exact comparison: the forwarded input must change the frame.
  if (std::memcmp(pixels.data(), after.data(), pixels.size()) == 0) {
    std::fprintf(stderr, "selftest: input had no visible effect\n");
    return 1;
  }

  std::printf("selftest: OK (backend=%s, %dx%d, %d non-background pixels)\n",
              webgtk_backend_name(view), width, height, before);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
#if WEBGTK_GTK4
  gtk_init();
#else
  gtk_init(&argc, &argv);
#endif

  const char *uri = "data:text/html,<html><body>WebGTK</body></html>";
  bool selftest = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest") == 0)
      selftest = true;
    else
      uri = argv[i];
  }

  const int width = 640;
  const int height = 480;

  webgtk_view *view = webgtk_view_new(width, height);
  if (!view) {
    std::fprintf(stderr, "demo: failed to create view\n");
    return 1;
  }
  webgtk_view_load_uri(view, uri);

  GtkWidget *widget = static_cast<GtkWidget *>(webgtk_view_get_widget(view));
#if WEBGTK_GTK4
  GtkWidget *window = gtk_window_new();
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  gtk_window_set_child(GTK_WINDOW(window), widget);
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_main_loop_quit),
                           loop);
  gtk_widget_set_visible(window, TRUE);
#else
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  gtk_container_add(GTK_CONTAINER(window), widget);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
#endif

  int exit_code = 0;
  if (selftest) {
    exit_code = run_selftest(view, width, height);
  } else {
    std::printf("demo: backend=%s, running main loop (close the window)\n",
                webgtk_backend_name(view));
#if WEBGTK_GTK4
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
#else
    gtk_main();
#endif
  }

  webgtk_view_destroy(view);
  return exit_code;
}
