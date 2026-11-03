#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Report GN files a pruned Chromium tree references but does not contain.

The engine archive is pruned for size, so `gn gen` stops at the first missing
import. This walks every .gn/.gni file, collects the //-rooted paths they
import or read, and prints the ones that do not exist. Run it to see the full
gap in one pass instead of discovering it one gn error at a time.

    python3 scripts/check_engine_deps.py /path/to/chromium
"""

import argparse
import os
import re
import sys

REF_RE = re.compile(r'(?:import|read_file)\s*\(\s*"(//[^"]+)"')


def collect(root):
    refs = set()
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('.git', 'out')]
        for name in filenames:
            if not name.endswith(('.gn', '.gni')):
                continue
            path = os.path.join(dirpath, name)
            try:
                with open(path, errors='ignore') as handle:
                    text = handle.read()
            except OSError:
                continue
            scanned += 1
            refs.update(REF_RE.findall(text))
    return scanned, refs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', help='Chromium source root (contains .gn)')
    args = parser.parse_args()

    if not os.path.isfile(os.path.join(args.root, '.gn')):
        print(f"error: {args.root} does not look like a Chromium source root "
              "(no .gn file)", file=sys.stderr)
        return 2

    scanned, refs = collect(args.root)
    # Some refs are resolved through GN variables ($foo_dir); skip those.
    literal = sorted(r for r in refs if '$' not in r)
    missing = [r for r in literal if not os.path.exists(os.path.join(args.root, r[2:]))]

    print(f"scanned {scanned} GN files, {len(literal)} literal references")
    if not missing:
        print("all literal references resolve")
        return 0

    print(f"\n{len(missing)} missing:")
    for ref in missing:
        print(f"  {ref}")
    return 1


if __name__ == '__main__':
    sys.exit(main())
