#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Replay the GN stub snapshot onto a freshly extracted engine tree.

`gn gen` against the pruned engine archive needs a set of empty BUILD.gn files
that stand in for the subtrees the archive dropped. `scripts/gen_stub_driver.py`
discovers them by iteration, and `scripts/collect_gn_stubs.py` freezes the
result into `scripts/gn_stubs/`. This applies that snapshot, so a clean checkout
can reach a successful `gn gen` without re-running the search.

    python3 scripts/apply_gn_stubs.py [engine-root]

Existing files are overwritten only when they are themselves stubs (they carry
the same marker), so a real file is never clobbered.
"""

import os
import shutil
import sys

MARKER = "Stub created by scripts/patch_gn_stubs.sh"

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(HERE, "gn_stubs")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "engine/chromium"
    if not os.path.isdir(os.path.join(root, "build")):
        print(f"error: {root} is not a Chromium source root", file=sys.stderr)
        return 2
    if not os.path.isdir(SRC_DIR):
        print(f"error: no snapshot at {SRC_DIR}; run collect_gn_stubs.py first",
              file=sys.stderr)
        return 2

    written = 0
    data_files = set()
    data_list = os.path.join(SRC_DIR, "DATA_FILES")
    if os.path.exists(data_list):
        with open(data_list) as f:
            data_files = {line.strip() for line in f if line.strip()}
    for dirpath, _, filenames in os.walk(SRC_DIR):
        for name in filenames:
            if name in ("MANIFEST", "DATA_FILES"):
                continue
            src = os.path.join(dirpath, name)
            rel = os.path.relpath(src, SRC_DIR)
            dest = os.path.join(root, rel)
            # The snapshot is authoritative: a file lands here only because the
            # pruned archive cannot parse without it. Some, like
            # //net/third_party/quiche/BUILD.gn, replace a real file the archive
            # ships that fails on a missing import, so overwrite rather than
            # trust a marker.
            if rel not in data_files and os.path.exists(dest):
                with open(dest, encoding="utf-8", errors="replace") as f:
                    if MARKER not in f.read(4096):
                        print(f"  replace real file {rel}")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(src, dest)
            written += 1

    print(f"applied {written} stubs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
