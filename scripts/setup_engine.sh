#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fetch and prepare the pruned Chromium engine used by the WebGTK Chromium
# backend, then generate the GN build files for the WebGTK renderer-only root
# target (//third_party/blink/renderer/webgtk:webgtk_engine).
#
# Usage:
#   scripts/setup_engine.sh [--dir <engine-root>] [--root-target <label>]
#                           [--no-deps] [--no-gen]
#
# The engine root defaults to $WEBGTK_ENGINE_DIR, or <repo>/engine when unset.
# The root target defaults to the V3 renderer-only target; pass
# --root-target=//content for the full embedder closure.
# After a successful run the tree looks like:
#
#   <engine-root>/chromium/            pruned Chromium checkout
#   <engine-root>/gnroot/gn            GN binary (CIPD)
#   <engine-root>/chromium/out/webgtk  generated ninja files
#
# Then configure WebGTK with:
#   cmake -S . -B build -DWEBGTK_ENGINE_DIR=<engine-root>

set -euo pipefail

# V3 (webgtk_core_v3.tar.gz) carries the WebGTK engine prune: the shared
# webgtk.gni args, the renderer-only root target, and the Blink platform
# media/WebRTC/WebGPU/XR amputation. scripts/apply_engine_patch.py re-applies
# that source patch after extraction; it is idempotent, so it is a no-op on the
# already-patched V3 archive and a repair for any older archive.
ARCHIVE_URL="https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk_core_v3.tar.gz"
DEFAULT_ROOT_TARGET="//third_party/blink/renderer/webgtk:webgtk_engine"
PERFETTO_URL="https://android.googlesource.com/platform/external/perfetto.git"
# Mirrors github.com/google/perfetto.git; same commits.
PERFETTO_MIRROR="https://github.com/google/perfetto.git"
PERFETTO_REV="5ca4307bd65b26906313b664d7efd7f3729c7767"  # pin from chromium/DEPS
# libc++ headers, pinned by chromium/DEPS (libcxx_revision). The archive drops
# third_party/libc++/src entirely, but the compile-verification build needs the
# headers; the library sources are not needed for it.
LIBCXX_URL="https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx"
LIBCXX_MIRROR="https://github.com/llvm/llvm-project.git"
LIBCXX_REV="9ba7713d3a0f1e179904c984bb626748b15e57d6"  # pin from chromium/DEPS
# libc++'s <charconv> sources include "shared/fp_bits.h", which lives in the
# llvm-libc repo (DEPS: src/third_party/llvm-libc/src), not in libc++.
LLVM_LIBC_URL="https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libc"
LLVM_LIBC_REV="058c23f04d56417fceefbdb21fdd31ac1aeb028f"  # pin from chromium/DEPS
GN_PACKAGE="gn/gn/linux-amd64"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="${WEBGTK_ENGINE_DIR:-$REPO_ROOT/engine}"
ROOT_TARGET="$DEFAULT_ROOT_TARGET"
DO_DEPS=1
DO_GEN=1

while [ $# -gt 0 ]; do
  case "$1" in
    --dir) ENGINE_DIR="$2"; shift 2 ;;
    --root-target) ROOT_TARGET="$2"; shift 2 ;;
    --no-deps) DO_DEPS=0; shift ;;
    --no-gen) DO_GEN=0; shift ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

SRC_DIR="$ENGINE_DIR/chromium"
GN="$ENGINE_DIR/gnroot/gn"

log() { printf '\n==> %s\n' "$*"; }

if [ "$DO_DEPS" = 1 ]; then
  log "Installing system dependencies"
  # Kerberos/GSSAPI headers are needed by the net stack (use_kerberos defaults
  # to true on Linux); the X11 and NSS headers are needed by content and Ozone.
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends \
    build-essential pkg-config python3 ninja-build \
    libkrb5-dev clang \
    libx11-dev libxcomposite-dev libxdamage-dev libxrandr-dev libxtst-dev \
    libnss3-dev libasound2-dev \
    libglib2.0-dev libdbus-1-dev
fi

mkdir -p "$ENGINE_DIR"

# ---------------------------------------------------------------- source tree
if [ ! -d "$SRC_DIR/content" ]; then
  log "Downloading engine archive (V3)"
  wget -c --progress=dot:giga -O "$ENGINE_DIR/webgtk_core_v3.tar.gz" "$ARCHIVE_URL"
  log "Extracting engine archive"
  tar -xzf "$ENGINE_DIR/webgtk_core_v3.tar.gz" -C "$ENGINE_DIR"
  rm -f "$ENGINE_DIR/webgtk_core_v3.tar.gz"
else
  log "Engine source tree already present at $SRC_DIR"
fi

# ------------------------------------------------------- WebGTK engine prune
# Re-apply the V3 source patch (webgtk.gni, the renderer-only root target, and
# the Blink platform media/WebRTC/WebGPU/XR amputation). Idempotent, so it is
# safe on an already-patched tree.
log "Applying the WebGTK V3 engine prune"
"$REPO_ROOT/scripts/apply_engine_patch.py" "$SRC_DIR"

# ------------------------------------------------------------------- perfetto
# The archive ships third_party/perfetto as an empty placeholder; restore it at
# the exact revision chromium/DEPS pins. android.googlesource.com refuses to
# serve this revision by name, so fetch the single commit (the mirrors do
# serve it) instead of cloning the default branch and checking out.
fetch_pinned() {
  local dest="$1" rev="$2" url
  shift 2
  for url in "$@"; do
    rm -rf "$dest"
    if git init -q "$dest" &&
       git -C "$dest" remote add origin "$url" &&
       git -C "$dest" fetch -q --depth 1 origin "$rev" &&
       git -C "$dest" checkout -q FETCH_HEAD; then
      rm -rf "$dest/.git"
      return 0
    fi
    echo "note: fetch of $rev from $url failed, trying next mirror" >&2
  done
  return 1
}

if [ ! -f "$SRC_DIR/third_party/perfetto/BUILD.gn" ]; then
  log "Restoring third_party/perfetto at $PERFETTO_REV"
  fetch_pinned "$SRC_DIR/third_party/perfetto" "$PERFETTO_REV" \
    "$PERFETTO_URL" "$PERFETTO_MIRROR" \
    "https://chromium.googlesource.com/external/github.com/google/perfetto.git" || {
    echo "error: could not fetch perfetto at $PERFETTO_REV" >&2
    exit 1
  }
else
  log "perfetto already restored"
fi

# ---------------------------------------------------------------------- libc++
# The archive drops third_party/libc++/src, but the compile-verification build
# (scripts/verify_engine.sh) needs the headers: gn's compile flags pass
# -nostdinc++ and -isystem the libc++ include directory, so without them every
# standard-library header fails to resolve. Restore the full upstream checkout
# at the exact revision chromium/DEPS pins -- headers *and* the src/ tree.
#
# src/ is not optional: the pruned tree still compiles libc++'s own .cpp files
# (charconv.cpp, filesystem, ...), and src/include/from_chars_floating_point.h
# is part of src/, not include/.
if [ ! -f "$SRC_DIR/third_party/libc++/src/src/charconv.cpp" ]; then
  log "Restoring third_party/libc++ at $LIBCXX_REV"
  LIBCXX_TMP="$ENGINE_DIR/.libcxx-fetch"
  fetch_pinned "$LIBCXX_TMP" "$LIBCXX_REV" "$LIBCXX_URL" "$LIBCXX_MIRROR" || {
    echo "error: could not fetch libc++ at $LIBCXX_REV" >&2
    exit 1
  }
  rm -rf "$SRC_DIR/third_party/libc++/src"
  mv "$LIBCXX_TMP" "$SRC_DIR/third_party/libc++/src"
  # gn generates __config_site from buildtools/third_party/libc++/__config_site.
  cp "$SRC_DIR/buildtools/third_party/libc++/__config_site" \
     "$SRC_DIR/third_party/libc++/src/include/__config_site"
else
  log "libc++ already restored"
fi

# ------------------------------------------------------------------ llvm-libc
# libc++'s <charconv> headers include "shared/fp_bits.h", which lives in the
# llvm-libc repo (DEPS: src/third_party/llvm-libc/src), not in libc++. The
# archive drops it, so without this every charconv translation unit fails.
if [ ! -f "$SRC_DIR/third_party/llvm-libc/src/shared/fp_bits.h" ]; then
  log "Restoring third_party/llvm-libc at $LLVM_LIBC_REV"
  LLVM_LIBC_TMP="$ENGINE_DIR/.libc-fetch"
  fetch_pinned "$LLVM_LIBC_TMP" "$LLVM_LIBC_REV" "$LLVM_LIBC_URL" || {
    echo "error: could not fetch llvm-libc at $LLVM_LIBC_REV" >&2
    exit 1
  }
  rm -rf "$SRC_DIR/third_party/llvm-libc/src"
  mv "$LLVM_LIBC_TMP" "$SRC_DIR/third_party/llvm-libc/src"
else
  log "llvm-libc already restored"
fi

# ------------------------------------------------------- compile-time sources
# Three more things the archive drops, whose files the *compile* needs (gn gen
# never touches them, so the stub driver cannot see them):
#   * third_party/boringssl/src      -- openssl/*.h. base/rand_util.h includes
#                                       third_party/boringssl/src/include/...,
#                                       a path the archive leaves empty.
#   * third_party/vulkan-headers/src -- vulkan/vk_platform.h, reached through
#                                       ANGLE's bundled volk.
#   * base/test/scoped_logging_settings.h -- base/logging.cc includes it (and
#                                       defines the class); base/test/ is
#                                       otherwise dropped and stubbed.
BORINGSSL_REV="5d165bc0408b977a80247598834cddd2c99b788f"      # pin from chromium/DEPS
VULKAN_HEADERS_REV="9fbc2a782ee3f8a4db64e7d991e23da75e893f44"  # pin from chromium/DEPS

if [ ! -f "$SRC_DIR/third_party/boringssl/src/include/openssl/rand.h" ]; then
  log "Restoring third_party/boringssl at $BORINGSSL_REV"
  fetch_pinned "$SRC_DIR/third_party/boringssl/src" "$BORINGSSL_REV" \
    "https://boringssl.googlesource.com/boringssl" || {
    echo "error: could not fetch boringssl at $BORINGSSL_REV" >&2
    exit 1
  }
else
  log "boringssl already restored"
fi

if [ ! -f "$SRC_DIR/third_party/vulkan-headers/src/include/vulkan/vk_platform.h" ]; then
  log "Restoring third_party/vulkan-headers at $VULKAN_HEADERS_REV"
  fetch_pinned "$SRC_DIR/third_party/vulkan-headers/src" "$VULKAN_HEADERS_REV" \
    "https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Headers" || {
    echo "error: could not fetch vulkan-headers at $VULKAN_HEADERS_REV" >&2
    exit 1
  }
else
  log "vulkan-headers already restored"
fi

if [ ! -f "$SRC_DIR/base/test/scoped_logging_settings.h" ]; then
  log "Restoring base/test/scoped_logging_settings.h"
  CHROMIUM_TAG="$(awk -F= '/^MAJOR=/{m=$2} /^MINOR=/{n=$2} /^BUILD=/{b=$2} \
    /^PATCH=/{p=$2} END{print m"."n"."b"."p}' "$SRC_DIR/chrome/VERSION" 2>/dev/null)"
  [ -n "$CHROMIUM_TAG" ] || CHROMIUM_TAG="143.0.7499.0"
  mkdir -p "$SRC_DIR/base/test"
  curl -fsSL \
    "https://chromium.googlesource.com/chromium/src/+/refs/tags/$CHROMIUM_TAG/base/test/scoped_logging_settings.h?format=TEXT" \
    | base64 -d > "$SRC_DIR/base/test/scoped_logging_settings.h" || {
    echo "error: could not fetch base/test/scoped_logging_settings.h" >&2
    exit 1
  }
else
  log "scoped_logging_settings.h already restored"
fi

# ----------------------------------------------------------- host interpreter
# .gn sets script_executable to //third_party/cpython3/host/bin/python3, the
# hermetic interpreter a full checkout ships. The archive keeps the directory
# metadata but drops the binary, so point it at the system python3 (gn only
# needs it to run exec_script helpers such as build/config/linux/pkg-config.py).
CPYTHON="$SRC_DIR/third_party/cpython3/host/bin/python3"
if [ ! -e "$CPYTHON" ]; then
  log "Linking $CPYTHON to the system python3"
  mkdir -p "$(dirname "$CPYTHON")"
  ln -sf "$(command -v python3)" "$CPYTHON"
fi

# ------------------------------------------------------------------- GN binary
if [ ! -x "$GN" ]; then
  log "Installing GN via CIPD"
  CIPD="$ENGINE_DIR/cipd"
  curl -sL "https://chrome-infra-packages.appspot.com/client?platform=linux-amd64&version=latest" -o "$CIPD"
  chmod +x "$CIPD"
  "$CIPD" install --root "$ENGINE_DIR/gnroot" "$GN_PACKAGE" latest
fi
"$GN" --version

# ------------------------------------------------- gclient-generated build args
# //build/config/chrome_build.gni imports this file, which gclient normally
# writes from the gclient_gn_args list in DEPS.
GCLIENT_ARGS="$SRC_DIR/build/config/gclient_args.gni"
if [ ! -f "$GCLIENT_ARGS" ]; then
  log "Generating build/config/gclient_args.gni"
  cat > "$GCLIENT_ARGS" <<'EOF'
# Generated file. Do not edit.
# Written by scripts/setup_engine.sh in place of `gclient runhooks`.
android_ndk_version = "2@30.0.16248370"
build_with_chromium = true
checkout_android = false
checkout_android_prebuilts_build_tools = false
checkout_clang_coverage_tools = false
checkout_copybara = false
checkout_fuchsia = false
checkout_glic_e2e_tests = false
checkout_ios_webkit = false
checkout_mutter = false
checkout_openxr = false
checkout_src_internal = false
checkout_src_internal_infra = false
checkout_clusterfuzz_data = false
cros_boards = ""
cros_boards_with_qemu_images = ""
generate_location_tags = false
EOF
fi

if [ "$DO_GEN" = 0 ]; then
  log "Skipping gn gen (--no-gen)"
  exit 0
fi

# ---------------------------------------------------------------------- gn gen
# V3 diet flags: renderer-only root target, no component build, no printing/
# PDF/extensions/plugins, no thin LTO, system sysroot, no Vulkan/remoting/VR,
# headless-only Ozone, no FFmpeg/libvpx/aom/dav1d/symphonia/VA-API media
# decoders, no WebNN tflite/litert backends, and the WebGTK engine prune
# (webgtk_strip_platform_media_capture) enabled.
#
# enable_nacl and enable_webrtc no longer exist as build args at this revision,
# so passing them only produces "no effect" warnings; the features are dropped
# by other means.
#
# use_kerberos=false: the pruned tree does not vendor third_party/kerberos, so
# Negotiate/SPNEGO auth is dropped rather than pulling in a GSSAPI dependency.
# enable_rust must stay true: //base and //net declare Rust targets
# unconditionally, and build/rust asserts on enable_rust, so a false value
# aborts gn gen. The archive keeps third_party/rust-toolchain, which is all gn
# needs at generation time.
# skia_use_dawn defaults true on Linux, which pulls Skia's Graphite-Dawn backend
# and its <webgpu/webgpu_cpp.h> include. The pruned archive has no Dawn headers,
# and a static-content webview never rasterizes through WebGPU, so disable it.
DIET_ARGS='is_debug=false is_component_build=false symbol_level=0
enable_printing=false enable_pdf=false
enable_extensions=false use_thin_lto=false use_sysroot=false
enable_webui_ntp=false enable_vulkan=false enable_remoting=false
use_kerberos=false enable_rust=true
skia_use_dawn=false
media_use_ffmpeg=false enable_ffmpeg_video_decoders=false
media_use_libvpx=false enable_libaom=false enable_dav1d_decoder=false
media_use_symphonia=false use_vaapi=false enable_media_remoting=false
ozone_platform_wayland=false ozone_platform_headless=true
webnn_use_tflite=false webnn_use_litert=false use_litert_tflite=false
enable_vr=false enable_plugins=false
webgtk_strip_platform_media_capture=true'
DIET_ARGS="$(echo $DIET_ARGS)"

log "Running gn gen (root target $ROOT_TARGET)"
cd "$SRC_DIR"
# GN loads ~140 files the pruned archive drops, even though WebGTK never builds
# those subtrees. scripts/patch_gn_stubs.sh writes the hand-written ones and
# scripts/apply_gn_stubs.py replays the snapshot the stub driver discovered.
log "Restoring missing GN files"
"$REPO_ROOT/scripts/patch_gn_stubs.sh" "$SRC_DIR"
"$REPO_ROOT/scripts/apply_gn_stubs.py" "$SRC_DIR"
if "$GN" gen out/webgtk --root-target="$ROOT_TARGET" --args="$DIET_ARGS"; then
  log "gn gen succeeded: $SRC_DIR/out/webgtk"
  echo "Build the engine with:"
  echo "  ninja -C $SRC_DIR/out/webgtk $ROOT_TARGET"
else
  log "gn gen failed"
  cat <<'EOF'
The pruned archive is missing source dependencies that GN resolves eagerly.
Enumerate them with:

  python3 scripts/check_engine_deps.py <engine-root>/chromium

Known gaps in webgtk_core_v2:
  * testing/                  (fuzzer_test.gni, test.gni) - imported by
                              media/media_options.gni and content
  * third_party/libevent
  * third_party/llvm-build    (clang toolchain)
  * buildtools/linux64        (gn/ninja prebuilts)
  * third_party/rust-toolchain
  * third_party/{ffmpeg,webrtc,pdfium,nasm,dawn}/... .gni files
EOF
  exit 1
fi
