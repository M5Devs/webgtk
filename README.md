# WebGTK

A lightweight GTK3/GTK4 webview built on the pruned Blink/V8 core, positioned as
a drop-in replacement for WebKitGTK. The view renders the engine's offscreen
(OSR) frames as an OpenGL texture inside a `GtkGLArea`, so it works on both GTK
majors and needs no compositor.

## Layout

```
include/webgtk/webgtk.h   Public C ABI (the only supported embedder interface)
include/webgtk/webgtk-view.h  GObject Introspection surface (bindings)
src/webgtk_backend_ops.h  Backend vtable: the seam between view and engine
src/webgtk_view.cc        GObject view wrapping GtkGLArea (GTK3 + GTK4)
src/backend_mock.cc       Deterministic software backend, no engine required
src/backend_chromium.cc   Real Blink/V8 backend (built when an engine is set)
examples/webgtk_demo.cc   Embedder demo, built for GTK3 and GTK4
examples/demo.py          PyGObject demo, drives the typelib only
tests/test_webgtk.c       C ABI and behaviour tests
scripts/                  Engine download/build tooling and GN stubs
.github/workflows/        Engine build and publish pipeline
docs/ENGINE.md            Engine integration notes and current status
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

The Blink/V8 engine is optional. Without it everything builds and tests against
the mock backend. To build the real backend, download the prebuilt engine and
point CMake at it:

```sh
scripts/download_engine_prebuilt.sh
cmake -S . -B build -DWEBGTK_USE_PREBUILT_ENGINE=ON
cmake --build build
```

That adds `src/backend_chromium.cc`, defines `WEBGTK_HAVE_ENGINE`, and links
`libwebgtk_engine.so` from `engine/prebuilt/`. It takes seconds: the engine is
compiled once in CI (`.github/workflows/build_engine.yml`), never on your
machine.

`-DWEBGTK_ENGINE_DIR=<tree>` instead compiles against a Chromium source tree,
which is only useful when working on the engine itself. See `docs/ENGINE.md`.

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

## Talking to the page

The page and the embedder share a small bridge. From JavaScript:

```js
window.webgtk.postMessage("clicked", JSON.stringify({x: 1, y: 2}));
```

The message is delivered to native code synchronously, with no polling loop.
In C, register a handler for the name:

```c
static void on_clicked(webgtk_view *view, const char *name,
                       const char *payload, void *user_data) {
  printf("%s: %s\n", name, payload);
}

webgtk_view_add_script_message_handler(view, "clicked", on_clicked, NULL);
```

In a GI binding, connect the signal instead; it fires for every message and
needs no registration:

```python
view.connect("script-message-received",
             lambda _v, name, payload: print(name, payload))
```

The bridge is injected at document start, so `window.webgtk` exists before the
page's own scripts run. Payloads are strings; JSON-encode objects in the page
and decode them natively.

## Language bindings

The same libraries ship GObject Introspection data, so the view is usable from
any GI-aware language with no C glue. A typelib is built per GTK major version
(`WebGTK-1.0` for GTK3, `WebGTK-4.0` for GTK4), next to the shared library:

```sh
cmake -S . -B build -DENABLE_INTROSPECTION=ON   # default; needs g-ir-scanner
GI_TYPELIB_PATH=build LD_LIBRARY_PATH=build python3 examples/demo.py
```

```python
import gi
gi.require_version("WebGTK", "1.0")
from gi.repository import Gtk, WebGTK

view = WebGTK.View.new_gtk3(800, 600)
view.load_html_string("<h1>Hello</h1>")
view.connect("javascript-result", lambda _v, json: print(json))
view.run_javascript("1 + 1")
window.add(view.widget)
```

`WebGtkView` is a real GObject that *composes* a `GtkGLArea`; the widget to add
to a container is the read-only `widget` property. The flat C ABI in
`<webgtk/webgtk.h>` stays dependency-free for embedders that only want the
handful of functions.

## License

LGPL-2.1-or-later. See `LICENSE`.