#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Snapshot the auto-generated GN stubs so `gn gen` is reproducible.

`scripts/gen_stub_driver.py` discovers, one `gn gen` run at a time, which
BUILD.gn files the pruned engine archive is missing. It writes each stub into
the engine tree, which is not under version control, so the work is lost when
the tree is re-extracted. This copies every stub it produced into
`scripts/gn_stubs/`, preserving the engine-relative path, together with a
manifest. `scripts/apply_gn_stubs.py` replays the snapshot onto a fresh tree.

Run it once the driver reports "gn gen succeeded":

    python3 scripts/collect_gn_stubs.py [engine-root]
"""

import os
import shutil
import sys

# The driver's own header is the marker; hand-written stubs carry it too.
MARKER = "Stub created by scripts/patch_gn_stubs.sh"

# Small data files the stubs read that are not .gn/.gni and have no comment
# syntax to carry the marker. build/config/rust.gni and
# build/config/compiler/BUILD.gn read the VERSION/revision files, and the
# branding/plist files feed grit() and version generation.
EXTRA_FILES = [
    "chrome/VERSION",
    "chrome/app/theme/chromium/BRANDING",
    "chrome/app/app-Info.plist",
    "build/util/LASTCHANGE.committime",
    "third_party/rust-toolchain/VERSION",
    "third_party/llvm-build/Release+Asserts/cr_build_revision",
    # //base/gtest_prod_util.h includes this header; the archive drops
    # //testing. It has no BUILD.gn role, only the FRIEND_TEST macro.
    "testing/gtest/include/gtest/gtest_prod.h",
]

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "gn_stubs")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "engine/chromium"
    if not os.path.isdir(os.path.join(root, "build")):
        print(f"error: {root} is not a Chromium source root", file=sys.stderr)
        return 2

    shutil.rmtree(OUT_DIR, ignore_errors=True)
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        # Skip anything that is not part of the source tree.
        dirnames[:] = [d for d in dirnames if d not in (".git", "out")]
        for name in filenames:
            if not name.endswith((".gn", ".gni", ".py")):
                continue
            full = os.path.join(dirpath, name)
            try:
                with open(full, encoding="utf-8") as f:
                    head = f.read(4096)
            except (OSError, UnicodeDecodeError):
                continue
            if MARKER not in head:
                continue
            rel = os.path.relpath(full, root)
            dest = os.path.join(OUT_DIR, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(full, dest)
            found.append(rel)

    for rel in EXTRA_FILES:
        full = os.path.join(root, rel)
        if not os.path.exists(full):
            continue
        dest = os.path.join(OUT_DIR, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(full, dest)
        found.append(rel)

    with open(os.path.join(OUT_DIR, "MANIFEST"), "w") as f:
        f.write("\n".join(sorted(found)) + "\n")
    # Data files have no marker, so the applier needs to be told they are ours.
    with open(os.path.join(OUT_DIR, "DATA_FILES"), "w") as f:
        f.write("\n".join(EXTRA_FILES) + "\n")
    print(f"collected {len(found)} stubs into {os.path.relpath(OUT_DIR)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
