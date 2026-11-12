#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Apply the WebGTK V3 engine prune to a restored Chromium tree.

scripts/setup_engine.sh extracts a pruned Chromium archive and then generates
GN files. The V3 prune is a *source* patch on top of that archive -- it is not a
stock Chromium build arg -- so it is lost every time the archive is re-extracted
and must be re-applied. This script does that, idempotently:

  * third_party/blink/renderer/webgtk/webgtk.gni   (new) shared GN args
  * third_party/blink/renderer/webgtk/BUILD.gn     (new) root target
  * third_party/blink/renderer/platform/BUILD.gn         gated dep/source removal

Run it after the tree is extracted and before `gn gen`:

    scripts/apply_engine_patch.py <engine-root>/chromium
"""

import os
import sys

# Sources under //third_party/blink/renderer/platform that implement the
# media-capture / WebRTC / WebGPU(Dawn) / WebXR stacks and are dropped when
# webgtk_strip_platform_media_capture is on. Matched by path prefix.
STRIPPED_SOURCE_PREFIXES = (
    "language_detection/",
    "video_capture/",
    "exported/video_capture/",
    "webrtc/",
    "peerconnection/",
    "p2p/",
    "graphics/gpu/dawn_",
    "graphics/gpu/webgpu_",
    "graphics/gpu/xr_frame_transport",
)

# Kept in sync with the edits below; used as the idempotency marker.
MARKER = "webgtk_strip_platform_media_capture"

WEBGTK_GNI = """\
# Copyright 2026 The WebGTK Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Shared WebGTK engine-pruning arguments. Import this from any BUILD.gn that
# needs to gate a hard structural dependency the WebGTK embedder does not use.

declare_args() {
  # Drop Blink's hard structural deps on the media-capture / WebRTC /
  # WebGPU(Dawn) / WebXR / on-device-language-detection stacks. WebGTK renders
  # static content: it never calls getUserMedia, RTCPeerConnection, WebXR or
  # WebGPU, and does not run on-device translation.
  #
  # WARNING: this is a deliberate source patch of the upstream engine, not a
  # stock Chromium argument. The tree no longer matches the published prebuilt
  # object artifacts and must be rebuilt from source.
  webgtk_strip_platform_media_capture = false
}
"""

ROOT_BUILD_GN = """\
# Copyright 2026 The WebGTK Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Renderer-only engine root target for WebGTK.
#
# WebGTK embeds the offscreen (OSR) rendering stack: Blink (DOM/CSS/layout) +
# V8 + Skia + the graphics/ozone plumbing that produces the surface. It does
# NOT need the browser process, so this target deliberately does not depend on
# //content/browser, the sync/policy/autofill/optimization_guide components, or
# the auxiliary services they drag in.
#
# It lives under //third_party/blink so it satisfies the `visibility` lists on
# Blink's own components (//third_party/blink/*) and on //ui/ozone/platform,
# which are the two restricted deps below.

group("webgtk_engine") {
  deps = [
    # --- Blink: DOM, CSS, layout, bindings -------------------------------
    # :core already pulls //third_party/blink/renderer/bindings/core/v8
    # transitively (it is a public_dep of core, and that target's visibility
    # list only admits core).
    "//third_party/blink/renderer/core",
    "//third_party/blink/renderer/platform",
    # NOTE: //third_party/blink/renderer/controller is deliberately NOT
    # depended on. It exists to service the *Web API modules* (getUserMedia,
    # WebRTC, WebXR, WebGL, WebNN, WebAudio, Gamepad, ...) via
    # controller -> modules, and that single edge is what drags the whole
    # modules/ + media/ + webrtc/ + dawn closure into a static-content webview
    # that never calls any of them. Measured: dropping it removes ~5.6k .o
    # (23,057 -> 17,423). Add it back only if the embedder needs Web APIs.

    # --- JavaScript engine -------------------------------------------------
    "//v8",

    # --- Rasterization / compositing --------------------------------------
    "//skia",
    "//cc",
    "//ui/gl",
    "//ui/gfx",
    "//ui/gfx/geometry",
    # //gpu:gpu is not directly depend-able (its visibility is //gpu/*); the
    # renderer reaches the command-buffer targets it needs through cc/blink.

    # --- Ozone (platform window / surface) --------------------------------
    "//ui/ozone:ozone",

    # --- Core utilities the above need ------------------------------------
    "//base",
    "//url",
  ]
}
"""

PLATFORM_IMPORT_ANCHOR = """\
visibility = [
  ":*",
  "//:gn_all",
  "//third_party/blink/public:all_blink",
  "//third_party/blink/renderer/platform/heap:test_support",
]
"""

PLATFORM_IMPORT_ADDED = PLATFORM_IMPORT_ANCHOR + """
import("//third_party/blink/renderer/webgtk/webgtk.gni")
"""

# Tail of component("platform")'s deps list; the gate block is inserted here.
PLATFORM_DEPS_TAIL = """\
    "//ui/gl:buildflags",
    "//ui/native_theme",
  ]
"""

DAWN_OLD = """\
  if (use_dawn) {
    deps += [ "//third_party/dawn/src/dawn/wire" ]
  } else {
    # If Dawn is not used, use the proc table. This gives the WebGPU
    # implementation something to link to, though WebGPU won't be supported.
    deps += [ "//third_party/dawn/src/dawn:proc" ]
  }
"""

DAWN_NEW = """\
  if (!%s) {
    if (use_dawn) {
      deps += [ "//third_party/dawn/src/dawn/wire" ]
    } else {
      # If Dawn is not used, use the proc table. This gives the WebGPU
      # implementation something to link to, though WebGPU won't be supported.
      deps += [ "//third_party/dawn/src/dawn:proc" ]
    }
  }
""" % MARKER

H265_OLD = """\
  if (rtc_use_h265) {
    sources += [
      "peerconnection/h265_parameter_sets_tracker.cc",
      "peerconnection/h265_parameter_sets_tracker.h",
    ]
  }
"""

H265_NEW = """\
  if (rtc_use_h265 && !%s) {
    sources += [
      "peerconnection/h265_parameter_sets_tracker.cc",
      "peerconnection/h265_parameter_sets_tracker.h",
    ]
  }
""" % MARKER


def platform_stripped_sources(text):
    """Path prefixes of component("platform") sources to drop, as they appear."""
    start = text.index('component("platform")')
    s = text.index("sources = [", start)
    depth = 0
    end = None
    for i in range(s, len(text)):
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                end = i
                break
    block = text[s:end]
    found = []
    for line in block.splitlines():
        stripped = line.strip()
        if stripped.startswith('"') and stripped.endswith('",'):
            path = stripped[1:-2]
            if path.startswith(STRIPPED_SOURCE_PREFIXES):
                found.append(path)
    return found


def build_gate_block(sources):
    src_lines = "\n".join('      "%s",' % s for s in sources)
    return """
  if (%s) {
    # WebGTK: drop media-capture / WebRTC / WebGPU / XR / ML-language-detection.
    sources -= [
%s
    ]
    public_deps -= [
      "//components/language_detection/core",
      "//media/capture:capture_lib",
      "//media/capture/mojom:video_capture_blink",
      "//third_party/dawn/include/dawn:cpp_headers",
    ]
    deps -= [
      "//components/language_detection/content/common",
      "//components/language_detection/content/common:common_blink",
      "//components/language_detection/content/renderer",
      "//components/translate/core/language_detection",
      "//components/webrtc:net_address_utils",
      "//device/vr/public/mojom:vr_service_blink",
      "//media",
      "//media/capture:capture_switches",
      "//media/mojo/clients",
      "//media/webrtc",
      "//third_party/webrtc_overrides:webrtc_component",
    ]
  }
""" % (MARKER, src_lines)


def write_if_changed(path, content):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == content:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    return True


def patch_platform(path):
    with open(path) as f:
        text = f.read()
    if MARKER in text:
        return "already patched"

    for name, anchor in (
        ("import", PLATFORM_IMPORT_ANCHOR),
        ("deps tail", PLATFORM_DEPS_TAIL),
        ("dawn block", DAWN_OLD),
        ("h265 block", H265_OLD),
    ):
        if text.count(anchor) != 1:
            raise SystemExit(
                "error: %s: expected exactly one %r anchor, found %d "
                "(tree differs from the V3 baseline)" % (path, name, text.count(anchor))
            )

    text = text.replace(PLATFORM_IMPORT_ANCHOR, PLATFORM_IMPORT_ADDED, 1)
    text = text.replace(PLATFORM_DEPS_TAIL,
                        PLATFORM_DEPS_TAIL + build_gate_block(platform_stripped_sources(text)),
                        1)
    text = text.replace(DAWN_OLD, DAWN_NEW, 1)
    text = text.replace(H265_OLD, H265_NEW, 1)

    with open(path, "w") as f:
        f.write(text)
    return "patched"


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    src = argv[1]
    webgtk_dir = os.path.join(src, "third_party/blink/renderer/webgtk")
    platform = os.path.join(src, "third_party/blink/renderer/platform/BUILD.gn")

    if not os.path.isdir(os.path.dirname(platform)):
        print("error: %s is not a Chromium tree" % src, file=sys.stderr)
        return 1

    wrote_gni = write_if_changed(os.path.join(webgtk_dir, "webgtk.gni"), WEBGTK_GNI)
    wrote_gn = write_if_changed(os.path.join(webgtk_dir, "BUILD.gn"), ROOT_BUILD_GN)
    result = patch_platform(platform)

    print("webgtk.gni   : %s" % ("written" if wrote_gni else "unchanged"))
    print("webgtk/BUILD : %s" % ("written" if wrote_gn else "unchanged"))
    print("platform     : %s" % result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
