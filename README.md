# WebGTK

A lightweight GTK3/GTK4 webview built on the pruned Blink/V8 core, positioned as
a drop-in replacement for WebKitGTK. The view renders the engine's offscreen
(OSR) frames as an OpenGL texture inside a `GtkGLArea`, so it works on both GTK
majors and needs no compositor.

## Layout

```
include/webgtk/webgtk.h   Public C ABI (the only supported embedder interface)
src/webgtk_backend_ops.h  Backend vtable: the seam between view and engine
src/webgtk_view.cc        GObject view wrapping GtkGLArea (GTK3 + GTK4)
src/backend_mock.cc       Deterministic software backend, no engine required
examples/webgtk_demo.cc   Embedder demo, built for GTK3 and GTK4
tests/test_webgtk.c       C ABI and behaviour tests
```

The view never talks to Blink, V8 or the mock directly. Everything engine
specific goes through `webgtk::BackendOps`, so the same view binary works with
the real engine and with the mock renderer.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Produces `libwebgtk.so` (GTK3), `libwebgtk4.so` (GTK4), `gtk3-demo` and
`gtk4-demo`. A `GtkGLArea` needs a display, so the tests run under `xvfb-run`.

Debug builds enable ASan and UBSan. `tests/lsan.supp` suppresses the
process-lifetime allocations the system GL/text stack makes, which
LeakSanitizer would otherwise report.

## Linking the engine

The Blink/V8 archive is optional. Without it everything builds and tests against
the mock backend. To link the real engine, point the build at a tree containing
`include/` and `lib/`:

```sh
cmake -S . -B build -DWEBGTK_ENGINE_DIR=/path/to/engine
```

That defines `WEBGTK_HAVE_ENGINE`, links `libwebgtk_engine`, and switches
`default_backend()` from the mock to the engine.

## Using a view

```c
#include <webgtk/webgtk.h>

webgtk_view *view = webgtk_view_new(800, 600);
webgtk_view_load_uri(view, "https://example.org/");
gtk_container_add(GTK_CONTAINER(window),
                  (GtkWidget *)webgtk_view_get_widget(view));
```

Mouse, scroll, key and text input are forwarded with the `webgtk_view_mouse_*`
and `webgtk_view_key_event` / `webgtk_view_text_input` calls. Call
`webgtk_view_render` to pull the current frame into a caller-owned RGBA8 buffer,
which is also how the headless tests assert on output.

`webgtk_view_destroy` releases everything the view owns. If the widget was added
to a container it is detached first; the view holds its own reference to the
widget, so it is safe to destroy the view before or after the window.

## License

LGPL-2.1-or-later. See `LICENSE`.