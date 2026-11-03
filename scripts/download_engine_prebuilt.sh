#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fetch a prebuilt WebGTK Chromium engine and unpack it under engine/prebuilt/.
#
# This is the fast path: developers never build Chromium from source. Point
# CMake at the result and libwebgtk.so links in seconds.
#
# Usage:
#   scripts/download_engine_prebuilt.sh [--dest <dir>] [--url <url>]
#                                       [--sha256 <hex>] [--force]
#
# Defaults to the newest GitHub release asset. Override --url to pull from the
# Hugging Face dataset instead:
#
#   https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk-engine-linux-x64.tar.gz
#
# Then:
#   cmake -S . -B build -DWEBGTK_USE_PREBUILT_ENGINE=ON
#
# Exit codes: 0 ok, 1 download/verify failure, 2 usage.

set -euo pipefail

DEFAULT_REPO="M5Devs/webgtk"
ASSET="webgtk-engine-linux-x64.tar.gz"
DEFAULT_URL="https://github.com/$DEFAULT_REPO/releases/latest/download/$ASSET"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/engine/prebuilt"
URL="$DEFAULT_URL"
SHA256=""
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --dest) DEST="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --sha256) SHA256="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [ -f "$DEST/manifest.env" ] && [ "$FORCE" = 0 ]; then
  log "Prebuilt engine already present at $DEST (use --force to replace)"
  sed -n 's/^/    /p' "$DEST/manifest.env"
  exit 0
fi

for tool in curl tar; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

log "Downloading $URL"
if ! curl -fL --retry 3 --retry-delay 2 -o "$TMP/$ASSET" "$URL"; then
  die "download failed. If the engine has not been published yet, build it with
       scripts/build_engine_artifact.sh, or pass --url to an existing artifact."
fi

if [ -n "$SHA256" ]; then
  log "Verifying checksum"
  actual="$(sha256sum "$TMP/$ASSET" | cut -d' ' -f1)"
  [ "$actual" = "$SHA256" ] || die "checksum mismatch: expected $SHA256, got $actual"
else
  echo "note: no --sha256 given, skipping checksum verification" >&2
fi

log "Extracting into $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TMP/$ASSET" -C "$DEST"

[ -f "$DEST/manifest.env" ] || die "artifact has no manifest.env; not a WebGTK engine package"
if [ ! -e "$DEST/lib/libwebgtk_engine.so" ] && [ ! -f "$DEST/lib/libwebgtk_engine.a" ]; then
  die "artifact has no engine library under lib/"
fi

log "Installed"
sed -n 's/^/    /p' "$DEST/manifest.env"
cat <<EOF

Configure WebGTK against it:

  cmake -S . -B build -DWEBGTK_USE_PREBUILT_ENGINE=ON
  cmake --build build

EOF
