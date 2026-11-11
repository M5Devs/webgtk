#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install the hermetic toolchain the pruned Chromium engine needs to *compile*.
#
# scripts/setup_engine.sh restores the source dependencies and runs `gn gen`.
# This installs the compilers and tools the generated ninja files invoke:
#
#   * Chromium's official Clang package (compiler, plugins, builtins), pinned by
#     chromium/DEPS and installed by the tree's own tools/clang/scripts/update.py
#   * the Chromium Rust toolchain, installed by tools/rust/update_rust.py
#   * Node.js, its node_modules, TypeScript and mold, which chromium/DEPS pins
#     as GCS and CIPD packages and which the content build's code generators and
#     the linker need
#
# Usage:
#   scripts/setup_toolchain.sh [engine-root]
#
# The engine root defaults to $WEBGTK_ENGINE_DIR, or <repo>/engine when unset.
# The script is idempotent: each component is skipped when already present.
# Run scripts/setup_gn_env.sh first (it installs the system libraries), then
# this, then scripts/engine_autoheal.py to drive the compile.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="${1:-${WEBGTK_ENGINE_DIR:-$REPO_ROOT/engine}}"
SRC_DIR="$ENGINE_DIR/chromium"
AUTOHEAL="$REPO_ROOT/scripts/engine_autoheal.py"
CIPD="$ENGINE_DIR/cipd"
CIPD_URL="https://chrome-infra-packages.appspot.com/client?platform=linux-amd64&version=latest"

# The archive is a Linux x86-64 Chromium checkout, so the host-specific
# dependency paths below are the Linux x64 ones from chromium/DEPS.
NODE_BIN_DEP="third_party/node/linux"
NODE_MODULES_DEP="third_party/node/node_modules"
TYPESCRIPT_DEP="third_party/typescript/linux-amd64/src"
MOLD_DEP="buildtools/third_party/mold/cipd"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -f "$SRC_DIR/DEPS" ] || die "no Chromium tree at $SRC_DIR; run scripts/setup_engine.sh first"
[ -f "$AUTOHEAL" ] || die "missing $AUTOHEAL"

# ------------------------------------------------------------- prerequisites
for tool in python3 git curl ninja; do
  command -v "$tool" >/dev/null 2>&1 || \
    die "$tool is required; run scripts/setup_gn_env.sh first"
done

# ----------------------------------------------------------------- CIPD client
# Node modules and the TypeScript compiler come from CIPD, as does mold. The
# client itself is a small static binary; setup_engine.sh installs the same one.
if [ ! -x "$CIPD" ]; then
  log "Installing the CIPD client"
  curl -fsSL "$CIPD_URL" -o "$CIPD"
  chmod +x "$CIPD"
fi

# ------------------------------------------------------------------- Clang
# The tree's own updater reads CLANG_REVISION from tools/clang/scripts/update.py
# (which matches the revision chromium/DEPS pins) and unpacks the compiler,
# its plugins and the compiler-rt builtins into third_party/llvm-build.
log "Installing Chromium Clang"
( cd "$SRC_DIR" && python3 tools/clang/scripts/update.py )
CLANG="$SRC_DIR/third_party/llvm-build/Release+Asserts/bin/clang"
[ -x "$CLANG" ] || die "clang not installed at $CLANG"
"$CLANG" --version | head -1

# -------------------------------------------------------------------- Rust
# enable_rust=true in the gn args, so the Rust toolchain is required. The
# updater installs the exact rustc/cargo chromium/DEPS pins.
log "Installing the Chromium Rust toolchain"
( cd "$SRC_DIR" && python3 tools/rust/update_rust.py )
RUSTC="$SRC_DIR/third_party/rust-toolchain/bin/rustc"
[ -x "$RUSTC" ] || die "rustc not installed at $RUSTC"
"$RUSTC" --version

# ----------------------------------------------- Node.js, node_modules, tools
# These are GCS and CIPD dependencies, absent from the pruned archive. The
# autoheal driver already knows how to fetch a dependency from its DEPS entry
# (bucket + object + checksum for GCS, package + instance for CIPD), so the
# prefetch mode is reused here rather than re-implementing the download.
log "Fetching Node.js, node_modules, TypeScript and mold"
python3 "$AUTOHEAL" "$ENGINE_DIR" \
  --prefetch "$NODE_BIN_DEP" \
  --prefetch "$NODE_MODULES_DEP" \
  --prefetch "$TYPESCRIPT_DEP" \
  --prefetch "$MOLD_DEP" \
  || die "could not fetch one of the pinned tools (see the log above)"

NODE="$SRC_DIR/third_party/node/linux/node-linux-x64/bin/node"
[ -x "$NODE" ] || die "node not installed at $NODE"
printf 'node %s\n' "$("$NODE" --version)"

TSC="$SRC_DIR/third_party/typescript/linux-amd64/src/lib/tsc"
[ -e "$TSC" ] || die "TypeScript not installed at $TSC"
printf 'typescript present at %s\n' "${TSC#"$SRC_DIR"/}"

MOLD="$SRC_DIR/buildtools/third_party/mold/cipd/mold"
[ -x "$MOLD" ] || die "mold not installed at $MOLD"
printf 'mold %s\n' "$("$MOLD" --version 2>/dev/null | head -1)"

log "Toolchain ready"
printf 'Run: python3 %s %s\n' "${AUTOHEAL#"$REPO_ROOT"/}" "$ENGINE_DIR"
