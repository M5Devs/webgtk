#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate GN stub targets into the snapshot from a list of labels.

The pruned engine archive drops whole subtrees (WebRTC, Dawn, FFmpeg, ...),
but GN still loads a BUILD.gn whenever a dep label points into it. Writing one
stub at a time by hand does not scale: //third_party/webrtc alone is referenced
by roughly two hundred distinct labels from //content, //media and the
overrides. This turns a label list into group()/config() stubs under
scripts/gn_stubs/, merging into any BUILD.gn already there so hand-written
targets survive.

    scripts/gen_stubs.py <label>...            # e.g. //a/b:c
    scripts/gen_stubs.py --from-file labels.txt
    scripts/gen_stubs.py --engine <root>       # discover from gn gen output

Labels default to a group() target. Pass `name_config` (a trailing _config) and
the target is emitted as an empty config instead; `--force-config` / `--force-
target` override the guess for a specific label. The distinction matters: a
label used in a `configs` list must be a config, and one used in `deps` must be
a target, or gn reports "Item type does not match".
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "gn_stubs")

MARKER = ("# Stub created by scripts/patch_gn_stubs.sh for the pruned\n"
          "# engine archive: the real tree is absent, so these targets\n"
          "# are empty groups that keep dep labels resolvable.\n")


def parse_label(label):
    body = label[2:] if label.startswith("//") else label
    label_dir, _, target = body.partition(":")
    label_dir = label_dir.rstrip("/")
    target = target or os.path.basename(label_dir)
    return label_dir, target


def add_stub(label_dir, target, kind):
    """Merge a stub definition into scripts/gn_stubs/<dir>/BUILD.gn."""
    path = os.path.join(OUT_DIR, label_dir, "BUILD.gn")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    decl = f'{kind}("{target}")'
    if os.path.exists(path):
        text = open(path).read()
        if decl in text:
            return False
    else:
        text = MARKER
    with open(path, "w") as f:
        f.write(text)
        if not text.endswith("\n"):
            f.write("\n")
        f.write(f'\n{decl} {{\n}}\n')
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("labels", nargs="*")
    ap.add_argument("--from-file")
    ap.add_argument("--force-config", action="append", default=[])
    ap.add_argument("--force-target", action="append", default=[])
    args = ap.parse_args()

    labels = list(args.labels)
    if args.from_file:
        with open(args.from_file) as f:
            labels += [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]

    force_config = set(args.force_config)
    force_target = set(args.force_target)
    added = 0
    for label in labels:
        label_dir, target = parse_label(label)
        if label in force_config:
            kind = "config"
        elif label in force_target:
            kind = "group"
        else:
            kind = "config" if target.endswith("_config") else "group"
        if add_stub(label_dir, target, kind):
            added += 1
    print(f"generated {added} new stubs ({len(labels)} labels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
