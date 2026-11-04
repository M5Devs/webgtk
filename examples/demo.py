#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A WebGTK window written in Python, driven only through GObject Introspection.

    python3 examples/demo.py             # GTK4 when available, else GTK3
    python3 examples/demo.py --gtk3      # force GTK3
    python3 examples/demo.py --selftest  # headless check, no window

There is no C glue anywhere in this file: ``from gi.repository import WebGTK``
is the entire binding layer. The typelibs are built next to the libraries, so
run the demo with the build directory on the introspection and library paths::

    GI_TYPELIB_PATH=build LD_LIBRARY_PATH=build python3 examples/demo.py
"""

import argparse
import sys

import gi

PAGE = """<!doctype html>
<html><body style="margin:0;font:16px sans-serif;background:#101418;color:#e6edf3">
  <h1 style="margin:24px">WebGTK</h1>
  <p style="margin:0 24px">This page is rendered by the <b>mock</b> backend
     into a GtkGLArea, and this window is Python.</p>
  <p id="out" style="margin:24px;color:#7ee787">waiting for JavaScript&hellip;</p>
  <button style="margin:0 24px" onclick="window.webgtk.postMessage(
      'clicked', JSON.stringify({x: 1, y: 2}))">Post a message to Python</button>
</body></html>
"""


def pick_namespace(force_gtk3):
    """Selects a (Gtk version, WebGTK version) pair that is actually installed.

    WebGTK is probed first: gi.require_version latches a namespace once loaded,
    so requiring Gtk before knowing the WebGTK typelib exists could pin the
    wrong GTK and make the fallback impossible.
    """
    candidates = [("3.0", "1.0")] if force_gtk3 else [("4.0", "4.0"), ("3.0", "1.0")]
    for gtk_version, webgtk_version in candidates:
        try:
            gi.require_version("WebGTK", webgtk_version)
            gi.require_version("Gtk", gtk_version)
        except ValueError:
            continue
        from gi.repository import Gtk, WebGTK

        return gtk_version, WebGTK, Gtk
    raise SystemExit(
        "No WebGTK typelib found. Build with -DENABLE_INTROSPECTION=ON and set "
        "GI_TYPELIB_PATH to the build directory."
    )


def make_view(WebGTK, gtk_version, width, height):
    """Creates the view, using the constructor for this GTK major version."""
    if gtk_version == "4.0":
        return WebGTK.View.new_gtk4(width, height)
    return WebGTK.View.new_gtk3(width, height)


def drain(Gtk, gtk_version):
    """Runs pending main-loop work so queued signals are delivered."""
    if gtk_version == "4.0":
        from gi.repository import GLib

        context = GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
    else:
        while Gtk.events_pending():
            Gtk.main_iteration()


def selftest(WebGTK, Gtk, gtk_version):
    """Exercises the binding without opening a window; returns an exit code."""
    view = make_view(WebGTK, gtk_version, 320, 200)
    failures = []

    def check(condition, label):
        if not condition:
            failures.append(label)

    check(view is not None, "view is created")
    check(view.get_property("backend-name") == "mock", "mock backend is active")
    check(view.get_property("widget") is not None, "view exposes a widget")

    check(view.load_html_string(PAGE) == WebGTK.Status.OK, "load_html_string")

    results = []
    view.connect("javascript-result", lambda _view, json: results.append(json))
    view.run_javascript("document.title = 'webgtk'; 6 * 7")
    drain(Gtk, gtk_version)
    check(len(results) == 1, "javascript-result fired once")
    check(results and "6 * 7" in results[0], "result carries the script")

    frame = view.render_frame()
    check(frame is not None and frame.get_size() == 320 * 200 * 4,
          "render_frame returns width*height*4 bytes")

    check(int(WebGTK.MouseButton.RIGHT) == 2, "mouse-button enum is bound")

    # The IPC bridge: a page post reaches Python with no polling loop. The mock
    # replays window.webgtk.postMessage() natively, so this runs headless.
    messages = []
    view.connect("script-message-received",
                 lambda _view, name, payload: messages.append((name, payload)))
    view.run_javascript(
        "window.webgtk.postMessage('clicked', JSON.stringify({x: 1, y: 2}))")
    drain(Gtk, gtk_version)
    check(len(messages) == 1, "script-message-received fired once")
    check(messages and messages[0][0] == "clicked", "handler name delivered")
    check(messages and "x" in messages[0][1], "JSON payload delivered")

    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print(f"{len(failures)} failures")
    return 1 if failures else 0


def run_window(WebGTK, Gtk, gtk_version):
    """Opens a window with a WebGTK view and runs the GTK main loop."""
    view = make_view(WebGTK, gtk_version, 800, 600)
    widget = view.get_property("widget")

    window = Gtk.Window(title="WebGTK")
    if gtk_version == "4.0":
        window.set_child(widget)
    else:
        window.add(widget)
    window.set_default_size(800, 600)

    def on_result(_view, json):
        print(f"javascript-result: {json}")

    def on_message(_view, name, payload):
        print(f"script-message-received: {name} -> {payload}")

    view.connect("javascript-result", on_result)
    view.connect("script-message-received", on_message)
    view.load_html_string(PAGE)
    view.run_javascript("document.getElementById('out').textContent = "
                        "'JavaScript ran in the mock engine.'; 1 + 1")

    window.connect("destroy", Gtk.main_quit)
    window.present()
    Gtk.main()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gtk3", action="store_true", help="force the GTK3 view")
    parser.add_argument("--selftest", action="store_true",
                        help="run headless checks instead of opening a window")
    args = parser.parse_args()

    gtk_version, WebGTK, Gtk = pick_namespace(args.gtk3)
    if args.selftest:
        return selftest(WebGTK, Gtk, gtk_version)
    return run_window(WebGTK, Gtk, gtk_version)


if __name__ == "__main__":
    sys.exit(main())
