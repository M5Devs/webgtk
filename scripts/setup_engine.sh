#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fetch and prepare the pruned Chromium engine used by the WebGTK Chromium
# backend, then generate the GN build files for //content.
#
# Usage:
#   scripts/setup_engine.sh [--dir <engine-root>] [--no-deps] [--no-gen]
#
# The engine root defaults to $WEBGTK_ENGINE_DIR, or <repo>/engine when unset.
# After a successful run the tree looks like:
#
#   <engine-root>/chromium/            pruned Chromium checkout
#   <engine-root>/gnroot/gn            GN binary (CIPD)
#   <engine-root>/chromium/out/webgtk  generated ninja files
#
# Then configure WebGTK with:
#   cmake -S . -B build -DWEBGTK_ENGINE_DIR=<engine-root>

set -euo pipefail

ARCHIVE_URL="https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk_core_v2.tar.gz"
PERFETTO_URL="https://android.googlesource.com/platform/external/perfetto.git"
# Mirrors github.com/google/perfetto.git; same commits.
PERFETTO_MIRROR="https://github.com/google/perfetto.git"
PERFETTO_REV="5ca4307bd65b26906313b664d7efd7f3729c7767"  # pin from chromium/DEPS
GN_PACKAGE="gn/gn/linux-amd64"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="${WEBGTK_ENGINE_DIR:-$REPO_ROOT/engine}"
DO_DEPS=1
DO_GEN=1

while [ $# -gt 0 ]; do
  case "$1" in
    --dir) ENGINE_DIR="$2"; shift 2 ;;
    --no-deps) DO_DEPS=0; shift ;;
    --no-gen) DO_GEN=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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
  log "Downloading engine archive"
  wget -c --progress=dot:giga -O "$ENGINE_DIR/webgtk_core_v2.tar.gz" "$ARCHIVE_URL"
  log "Extracting engine archive"
  tar -xzf "$ENGINE_DIR/webgtk_core_v2.tar.gz" -C "$ENGINE_DIR"
  rm -f "$ENGINE_DIR/webgtk_core_v2.tar.gz"
else
  log "Engine source tree already present at $SRC_DIR"
fi

# ------------------------------------------------------------------- perfetto
# The archive ships third_party/perfetto as an empty placeholder; restore it at
# the exact revision chromium/DEPS pins.
if [ ! -f "$SRC_DIR/third_party/perfetto/BUILD.gn" ]; then
  log "Restoring third_party/perfetto at $PERFETTO_REV"
  rm -rf "$SRC_DIR/third_party/perfetto"
  if ! git clone "$PERFETTO_URL" "$SRC_DIR/third_party/perfetto"; then
    echo "note: android.googlesource.com failed, retrying via GitHub mirror" >&2
    rm -rf "$SRC_DIR/third_party/perfetto"
    git clone "$PERFETTO_MIRROR" "$SRC_DIR/third_party/perfetto"
  fi
  git -C "$SRC_DIR/third_party/perfetto" checkout "$PERFETTO_REV"
else
  log "perfetto already restored"
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
# Diet flags: content-only root target, no component build, no NaCl/printing/
# PDF/WebRTC/extensions, no thin LTO, system sysroot, no Vulkan/remoting.
#
# use_kerberos=false: the pruned tree does not vendor third_party/kerberos, so
# Negotiate/SPNEGO auth is dropped rather than pulling in a GSSAPI dependency.
# enable_rust=false: the archive ships no Rust toolchain.
DIET_ARGS='is_debug=false is_component_build=false symbol_level=0
enable_nacl=false enable_printing=false enable_pdf=false enable_webrtc=false
enable_extensions=false use_thin_lto=false use_sysroot=false
enable_webui_ntp=false enable_vulkan=false enable_remoting=false
use_kerberos=false enable_rust=false'
DIET_ARGS="$(echo $DIET_ARGS)"

log "Running gn gen (root target //content)"
cd "$SRC_DIR"
if "$GN" gen out/webgtk --root-target=//content --args="$DIET_ARGS"; then
  log "gn gen succeeded: $SRC_DIR/out/webgtk"
  echo "Build the engine with:"
  echo "  ninja -C $SRC_DIR/out/webgtk content"
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
