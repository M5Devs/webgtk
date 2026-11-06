#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build WebGTK and produce the release artifacts in dist/:
#
#   webgtk-<ver>-linux-<arch>.tar.gz   portable install tree
#   libwebgtk0_<ver>-1_<arch>.deb      runtime libraries + typelibs
#   libwebgtk-dev_<ver>-1_<arch>.deb   headers, .pc files, .gir, linker symlinks
#   webgtk-demos_<ver>-1_<arch>.deb    GTK3/GTK4 demos
#
# Usage:
#   scripts/package_release.sh [--build-dir <dir>] [--dist <dir>]
#                              [--generators TGZ,DEB] [--no-build]
#
# The default is a Release build (no sanitizers), which is what a distributed
# library must be: a Debug build links the ASan/UBSan runtimes and would fail on
# a machine that does not have them.
#
# Exit codes: 0 ok, 1 build or packaging failure, 2 usage.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-package"
DIST_DIR="$REPO_ROOT/dist"
GENERATORS="TGZ;DEB"
DO_BUILD=1

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --dist) DIST_DIR="$2"; shift 2 ;;
    --generators) GENERATORS="$2"; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

log() { printf '==> %s\n' "$*"; }

# A Release build ships a library anyone can link; the sanitizer runtimes a
# Debug build needs are not present on a normal machine.
if [ "$DO_BUILD" -eq 1 ]; then
  log "Configuring $BUILD_DIR (Release)"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DWEBGTK_BUILD_TESTS=OFF
  log "Building"
  cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

log "Packaging ($GENERATORS) into $DIST_DIR"
cpack --config "$BUILD_DIR/CPackConfig.cmake" \
      -G "$GENERATORS" -B "$DIST_DIR"

log "Artifacts:"
find "$DIST_DIR" -maxdepth 1 \
     \( -name '*.tar.gz' -o -name '*.deb' \) -printf '    %f\n' | sort
