#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build the WebGTK Chromium engine and package it as a relocatable artifact.
#
# Run this on a machine with plenty of RAM and disk (the Chromium build wants
# 16 GB+ and ~60 GB). It is what .github/workflows/build_engine.yml calls.
#
# Usage:
#   scripts/build_engine_artifact.sh [--dir <engine-root>] [--out <tar.gz>]
#                                    [--jobs N] [--skip-build] [--no-deps]
#
# Produces <out> (default webgtk-engine-linux-x64.tar.gz) containing:
#
#   lib/libwebgtk_engine.so     engine, link this
#   lib/*.so                    its component-library dependency closure
#   include/                    staged public headers
#   gen/                        generated headers
#   manifest.env                key=value metadata (revision, flags, sizes)
#   webgtk-engine.pc            pkg-config file
#
# The engine source archive is a *pruned* Chromium tree, so this script
# restores what gn gen needs: perfetto at the DEPS pin, the clang toolchain
# (tools/clang/scripts/update.py), a Rust VERSION file, and no-op stubs for the
# pruned //testing templates. See docs/ENGINE.md.

set -euo pipefail

ARCHIVE_URL="https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk_core_v2.tar.gz"
PERFETTO_URL="https://android.googlesource.com/platform/external/perfetto.git"
PERFETTO_MIRROR="https://github.com/google/perfetto.git"
PERFETTO_REV="5ca4307bd65b26906313b664d7efd7f3729c7767"
# rustc revision pinned by chromium/DEPS; the VERSION file holds it verbatim.
RUSTC_REVISION="8a9dd7abdf013ec10cfeaaa3685feba18ef1c90c"
# Clang revision pinned by tools/clang/scripts/update.py.
CLANG_REVISION="llvmorg-24-init-14453-ge063be79"
# WebRTC is pruned from the archive but //content imports //third_party/webrtc.
WEBRTC_URL="https://webrtc.googlesource.com/src.git"
WEBRTC_REV="d677f6c7dcdf7ec26c4841605938c6a4fc1948ec"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="${WEBGTK_ENGINE_DIR:-$REPO_ROOT/engine}"
OUT_TAR="$REPO_ROOT/webgtk-engine-linux-x64.tar.gz"
JOBS="$(nproc)"
DO_DEPS=1
DO_BUILD=1

while [ $# -gt 0 ]; do
  case "$1" in
    --dir) ENGINE_DIR="$2"; shift 2 ;;
    --out) OUT_TAR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --skip-build) DO_BUILD=0; shift ;;
    --no-deps) DO_DEPS=0; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

SRC_DIR="$ENGINE_DIR/chromium"
OUT_DIR="$SRC_DIR/out/Release"
GN="$ENGINE_DIR/gnroot/gn"
STAGE="$ENGINE_DIR/stage"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# GN build arguments. See docs/ENGINE.md for why each one is here.
#
#   is_component_build=true  builds Chromium as many shared libraries. That is
#                            what keeps the final link under the linker's symbol
#                            limits and gives libwebgtk_engine.so real .so deps.
#   enable_rust=true         //base has Rust sources; the toolchain is restored
#                            above.
#   use_kerberos=false       no vendored third_party/kerberos in the archive.
#   use_sysroot=false        build against the host sysroot, not a downloaded
#                            Debian one.
#   is_cfi=false             CFI asserts it needs ThinLTO, which is off.
#   chrome_pgo_phase=0       no PGO profiles in a pruned tree.
DIET_ARGS='is_official_build=true is_debug=false is_component_build=true
symbol_level=0 enable_printing=false enable_pdf=false enable_webrtc=false
enable_extensions=false use_sysroot=false enable_remoting=false
use_kerberos=false enable_rust=true is_clang=true treat_warnings_as_errors=false
is_cfi=false chrome_pgo_phase=0'

# ------------------------------------------------------------------ source tree
if [ ! -d "$SRC_DIR/content" ]; then
  log "Downloading engine archive"
  mkdir -p "$ENGINE_DIR"
  wget -c --progress=dot:giga -O "$ENGINE_DIR/webgtk_core_v2.tar.gz" "$ARCHIVE_URL"
  tar -xzf "$ENGINE_DIR/webgtk_core_v2.tar.gz" -C "$ENGINE_DIR"
  rm -f "$ENGINE_DIR/webgtk_core_v2.tar.gz"
fi

# --------------------------------------------------------------------- restore
if [ "$DO_DEPS" = 1 ]; then
  if [ ! -f "$SRC_DIR/third_party/perfetto/BUILD.gn" ]; then
    log "Restoring third_party/perfetto at $PERFETTO_REV"
    rm -rf "$SRC_DIR/third_party/perfetto"
    git clone "$PERFETTO_URL" "$SRC_DIR/third_party/perfetto" \
      || { rm -rf "$SRC_DIR/third_party/perfetto"
           git clone "$PERFETTO_MIRROR" "$SRC_DIR/third_party/perfetto"; }
    git -C "$SRC_DIR/third_party/perfetto" checkout "$PERFETTO_REV"
  fi

  # Clang and Rust are GCS objects, not git deps. These scripts are the
  # supported way to fetch them and write the revision markers gn expects.
  if [ ! -x "$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/clang" ]; then
    log "Downloading the clang toolchain ($CLANG_REVISION)"
    ( cd "$SRC_DIR" && python3 tools/clang/scripts/update.py )
  fi

  if [ ! -x "$SRC_DIR/third_party/rust-toolchain/bin/rustc" ]; then
    log "Downloading the Rust toolchain ($RUSTC_REVISION)"
    ( cd "$SRC_DIR" && python3 tools/rust/update_rust.py )
  fi

  # //content imports //third_party/webrtc even with enable_webrtc=false.
  if [ ! -f "$SRC_DIR/third_party/webrtc/webrtc.gni" ]; then
    log "Restoring third_party/webrtc at $WEBRTC_REV"
    rm -rf "$SRC_DIR/third_party/webrtc"
    mkdir -p "$SRC_DIR/third_party/webrtc"
    git -C "$SRC_DIR/third_party/webrtc" init -q
    git -C "$SRC_DIR/third_party/webrtc" remote add origin "$WEBRTC_URL"
    git -C "$SRC_DIR/third_party/webrtc" fetch --depth 1 -q origin "$WEBRTC_REV"
    git -C "$SRC_DIR/third_party/webrtc" checkout -q FETCH_HEAD
  fi

  # //testing is pruned and is not a DEPS dependency, so it cannot be restored.
  # The diet build never instantiates the test/fuzzer templates; defining them
  # as no-ops lets gn gen resolve the imports without pulling in test sources.
  log "Writing no-op stubs for the pruned //testing templates"
  mkdir -p "$SRC_DIR/testing/libfuzzer"
  cat > "$SRC_DIR/testing/test.gni" <<'EOF'
# Stub for the pruned //testing/test.gni. The WebGTK engine build never
# instantiates these templates; they exist so gn can resolve the import.
template("test") {
  not_needed(invoker, "*")
}
template("source_set_with_test") {
  not_needed(invoker, "*")
}
template("test_lib") {
  not_needed(invoker, "*")
}
EOF
  cat > "$SRC_DIR/testing/libfuzzer/fuzzer_test.gni" <<'EOF'
# Stub for the pruned //testing/libfuzzer/fuzzer_test.gni.
#
# The real file imports //build/config/sanitizers/sanitizers.gni, and
# //media/media_options.gni relies on that transitive import for is_asan.
# Keep the import so downstream GN files still resolve those globals.
import("//build/config/sanitizers/sanitizers.gni")

template("fuzzer_test") {
  not_needed(invoker, "*")
}
template("fuzzer_test_sources") {
  not_needed(invoker, "*")
}
EOF

  GCLIENT_ARGS="$SRC_DIR/build/config/gclient_args.gni"
  if [ ! -f "$GCLIENT_ARGS" ]; then
    log "Generating build/config/gclient_args.gni"
    cat > "$GCLIENT_ARGS" <<'EOF'
# Generated file. Do not edit.
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

  # gn is a CIPD package; fetch it once.
  if [ ! -x "$GN" ]; then
    log "Installing GN via CIPD"
    CIPD="$ENGINE_DIR/cipd"
    curl -sL "https://chrome-infra-packages.appspot.com/client?platform=linux-amd64&version=latest" -o "$CIPD"
    chmod +x "$CIPD"
    "$CIPD" install --root "$ENGINE_DIR/gnroot" gn/gn/linux-amd64 latest
  fi
fi

[ -x "$GN" ] || die "gn not found at $GN (run without --no-deps)"

# --------------------------------------------------- engine target + gn gen
# libwebgtk_engine.so links the Chromium components the WebGTK backend uses.
# In a component build those are themselves shared libraries, so this is a
# normal link against libcontent.so, libbase.so and friends, and the artifact
# carries their dependency closure.
#
# NOTE: the target names below are the ones the WebGTK backend includes from.
# If gn reports an unknown target, correct it here; nothing else depends on the
# exact names.
ENGINE_GN="$SRC_DIR/webgtk_engine.gni"
log "Writing //webgtk_engine.gni"
cat > "$ENGINE_GN" <<'EOF'
# Generated by scripts/build_engine_artifact.sh.
shared_library("webgtk_engine") {
  deps = [
    "//content",
    "//content/public/app",
    "//headless:headless_browser",
    "//ui/gfx",
    "//ui/gfx/geometry",
    "//ui/ozone",
    "//url",
  ]
}
EOF

log "Running gn gen (root target //webgtk_engine)"
cd "$SRC_DIR"
# shellcheck disable=SC2086
"$GN" gen out/Release --root-target=//webgtk_engine \
  --args="$(echo $DIET_ARGS)"

if [ "$DO_BUILD" = 0 ]; then
  log "Skipping ninja (--skip-build)"
  exit 0
fi

log "Building //webgtk_engine with $JOBS jobs"
ninja -C out/Release webgtk_engine

# ------------------------------------------------------------------- stage
log "Staging headers and libraries into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/include" "$STAGE/gen"

# Every header under the source root is needed to compile an embedder: Chromium
# has no "public headers only" package, and the backend reaches into
# components/, headless/lib and third_party/blink.
( cd "$SRC_DIR" && find . -name '*.h' -o -name '*.inc' -o -name '*.def' ) \
  | while read -r rel; do
      dest="$STAGE/include/${rel#./}"
      mkdir -p "$(dirname "$dest")"
      cp -a "$SRC_DIR/${rel#./}" "$dest"
    done

# Generated headers live in the build directory.
if [ -d "$OUT_DIR/gen" ]; then
  cp -a "$OUT_DIR/gen/." "$STAGE/gen/"
fi

# Collect the engine library and everything it needs from the build tree. A
# component build spreads the engine over many .so files; ldd tells us exactly
# which ones, so the artifact is relocatable.
log "Collecting the shared library closure"
cp -a "$OUT_DIR/libwebgtk_engine.so" "$STAGE/lib/"

collect_deps() {
  local so="$1"
  LD_LIBRARY_PATH="$OUT_DIR:${LD_LIBRARY_PATH:-}" ldd "$so" 2>/dev/null \
    | awk '/=> \//{print $3}' | while read -r dep; do
        case "$dep" in
          "$OUT_DIR"/*) echo "${dep#"$OUT_DIR"/}" ;;
        esac
      done
}

# Breadth-first over the closure: a copied library may need others.
declare -A seen=()
queue=("libwebgtk_engine.so")
while [ ${#queue[@]} -gt 0 ]; do
  next=()
  for so in "${queue[@]}"; do
    [ -n "${seen[$so]:-}" ] && continue
    seen[$so]=1
    for dep in $(collect_deps "$STAGE/lib/$so"); do
      [ -n "${seen[$dep]:-}" ] && continue
      cp -a "$OUT_DIR/$dep" "$STAGE/lib/"
      next+=("$dep")
    done
  done
  queue=("${next[@]+"${next[@]}"}")
done

log "Packaged $(find "$STAGE/lib" -name '*.so' | wc -l) shared libraries"
# ------------------------------------------------------------------ metadata
CHROMIUM_REV="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
{
  echo "artifact=webgtk-engine"
  echo "os=linux"
  echo "arch=x64"
  echo "clang_revision=$CLANG_REVISION"
  echo "rustc_revision=$RUSTC_REVISION"
  echo "chromium_revision=$CHROMIUM_REV"
  echo "perfetto_revision=$PERFETTO_REV"
  echo "gn_args=$(echo $DIET_ARGS)"
  echo "engine_sha256=$(sha256sum "$STAGE/lib/libwebgtk_engine.so" | cut -d' ' -f1)"
} > "$STAGE/manifest.env"

cat > "$STAGE/webgtk-engine.pc" <<'EOF'
prefix=${pcfiledir}/..
includedir=${prefix}/include
libdir=${prefix}/lib

Name: webgtk-engine
Description: WebGTK pruned Chromium engine
Version: 1.0.0
Cflags: -I${includedir} -I${prefix}/gen
Libs: -L${libdir} -lwebgtk_engine
EOF

# ------------------------------------------------------------------- package
log "Packaging $OUT_TAR"
( cd "$STAGE" && tar -czf "$OUT_TAR" . )

log "Done"
printf 'artifact: %s\n' "$OUT_TAR"
printf 'size:     %s\n' "$(du -h "$OUT_TAR" | cut -f1)"
printf 'sha256:   %s\n' "$(sha256sum "$OUT_TAR" | cut -d' ' -f1)"
