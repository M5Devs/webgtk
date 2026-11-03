#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Drive `gn gen` to a clean parse by stubbing missing BUILD.gn targets.

The pruned engine archive drops whole subtrees, but GN still *loads* a
BUILD.gn whenever a dep label points into it. `gn gen` reports one such miss
at a time; this loops, and for each "Unable to load <dir>/BUILD.gn" error it
appends an empty group() for the referenced target so the next run gets
further. It stops when gn gen succeeds or when it hits an error that is not a
missing-BUILD.gn load.

    python3 scripts/gen_stub_driver.py [engine-root] [-- gn-args...]

The stubs it writes are throwaway scaffolding for a local parse. Once it
reports success, freeze them with `scripts/collect_gn_stubs.py` so a fresh
engine tree can be primed by `scripts/apply_gn_stubs.py` instead of searching
again.
"""

import os
import re
import subprocess
import sys

LOAD_RE = re.compile(r'Unable to load "([^"]+)"')
# A dep label may name a target ("//a/b:target") or rely on the default
# target name, which is the directory's basename ("//a/b" -> "b").
LABEL_RE = re.compile(r'"//([A-Za-z0-9_./\-]+)(?::([A-Za-z0-9_./\-]+))?"')
# `gn gen` reports missing deps in bulk as "Unresolved dependencies" followed
# by "  needs //dir:target(//toolchain)" lines. A stub BUILD.gn may exist for
# the directory yet still lack the specific target, so recover each label.
UNRESOLVED_RE = re.compile(r'needs (//[A-Za-z0-9_./\-]+(?::[A-Za-z0-9_./\-]+)?)\(')
# A stub may define a target where a config is wanted (or vice-versa). gn points
# at the *usage* site first, then names the definition:
#   ERROR at //a/BUILD.gn:46:15: Item type does not match.
#     configs = [ "//b:name" ]
#   The type of //b:name(//toolchain) here is a config but was previously seen
#   as a target.
#   See //b/BUILD.gn:8:1:
#     group("name") {
ITEM_TYPE_RE = re.compile(
    r'The type of (\S+)\(.*?\)\s*\nhere is a (\w+) but was previously seen as a (\w+)\.'
    r'.*?See (\S+?):(\d+):\d+:\s*\n\s*(\w+)\("([^"]+)"\)',
    re.S)

STUB_HEADER = ("# Stub created by scripts/patch_gn_stubs.sh for the pruned\n"
               "# engine archive: the real tree is absent, so these targets\n"
               "# are empty groups that keep dep labels resolvable.\n")


def stub_target(root, label):
    """Append an empty stub for `label` to its directory's BUILD.gn.

    Returns the "//dir:target" string it stubbed, or None if the target was
    already present (which means the real error is something else).
    """
    body = label[2:]
    label_dir, _, target = body.partition(':')
    label_dir = label_dir.rstrip('/')
    target = target or os.path.basename(label_dir)
    # Chromium names configs "*_config"; a dep list cannot tell a config from a
    # target, so use the name to avoid creating a target where a config is
    # expected (which gn then rejects with "Item type does not match").
    kind = 'config' if target.endswith('_config') else 'group'
    path = os.path.join(root, label_dir, 'BUILD.gn')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        existing = open(path).read()
        if f'{kind}("{target}")' in existing:
            return None
        text = existing
    else:
        text = STUB_HEADER
    with open(path, 'w') as f:
        f.write(text)
        f.write(f'\n{kind}("{target}") {{\n}}\n')
    return f'//{label_dir}:{target}'


def fix_item_type(root, path, line, kind, wanted):
    """Flip a stub's definition between a target and a config.

    `path`/`line` locate the definition gn flagged, `kind` is what the stub
    declares now, and `wanted` is the type earlier references expect.
    Returns the "//dir:name" it fixed, or None if the line did not change.
    """
    full = os.path.join(root, path.lstrip('/'))
    if not os.path.exists(full):
        return None
    lines = open(full).read().splitlines()
    idx = line - 1
    if not (0 <= idx < len(lines)):
        return None
    old = f'{kind}("'
    if old not in lines[idx]:
        return None
    # gn calls every non-config item a "target"; the generic spelling is not a
    # real function, so pick a concrete one.
    concrete = {'config': 'config', 'target': 'group'}
    new = f'{concrete.get(wanted, wanted)}("'
    name = lines[idx].split(old, 1)[1].split('"', 1)[0]
    lines[idx] = lines[idx].replace(old, new, 1)
    with open(full, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return f'{path}:{name} ({kind} -> {wanted})'


def run_gn(root, args):
    cmd = [os.path.join(os.path.dirname(root), 'gnroot', 'gn'), 'gen',
           'out/webgtk', '--root-target=//content', '--args=' + args]
    return subprocess.run(cmd, cwd=root, capture_output=True, text=True)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'engine/chromium'
    rest = sys.argv[2:]
    if rest and rest[0] == '--':
        rest = rest[1:]
    args = ' '.join(rest) or (
        'is_debug=false is_component_build=false symbol_level=0 '
        'enable_printing=false enable_pdf=false '
        'enable_extensions=false use_thin_lto=false use_sysroot=false '
        'enable_webui_ntp=false enable_vulkan=false enable_remoting=false '
        'use_kerberos=false enable_rust=true')

    fixes = {}
    for step in range(1, 400):
        proc = run_gn(root, args)
        out = proc.stdout + proc.stderr
        if proc.returncode == 0:
            print(f"[{step}] gn gen succeeded")
            return 0
        m = LOAD_RE.search(out)
        if not m:
            # Bulk missing-dependency report: stub every label it names, then
            # re-run. If nothing new was stubbed, the error is real.
            labels = UNRESOLVED_RE.findall(out)
            stubbed = []
            for label in labels:
                got = stub_target(root, label)
                if got:
                    stubbed.append(got)
            if stubbed:
                print(f"[{step}] stubbed {len(stubbed)} unresolved deps: "
                      + ', '.join(stubbed[:6]) + (' ...' if len(stubbed) > 6 else ''))
                continue
            tm = ITEM_TYPE_RE.search(out)
            if tm:
                label = tm.group(1)
                got = fix_item_type(root, tm.group(4), int(tm.group(5)),
                                    tm.group(6), tm.group(3))
                if got:
                    # Two targets can want opposite types for the same label;
                    # flipping back and forth never converges, so stop.
                    if fixes.get(label) == got.split(' ')[-1]:
                        print(f"[{step}] {label} is referenced as both a target "
                              f"and a config; cannot satisfy both")
                        return 1
                    fixes[label] = got.split(' ')[-1]
                    print(f"[{step}] fixed item type {got}")
                    continue
            print(f"[{step}] stopped on a non-load error:\n{out[:1500]}")
            return 1
        missing_path = os.path.relpath(m.group(1), root)
        if not missing_path.endswith('BUILD.gn'):
            # A missing .gni or data file loaded via import()/read_file(). An
            # empty placeholder satisfies the load; only parse-time symbols it
            # was expected to define can still fail, and those surface next.
            full = os.path.join(root, missing_path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, 'w') as f:
                f.write("# Stub created by scripts/patch_gn_stubs.sh for the "
                        "pruned engine archive.\n")
            print(f"[{step}] stubbed {missing_path}")
            continue
        build_dir = os.path.dirname(missing_path)
        # The label that triggered the load is on the following line.
        tail = out[m.end():m.end() + 400]
        lm = LABEL_RE.search(tail)
        if lm:
            label_dir = lm.group(1)
            target = lm.group(2) or os.path.basename(label_dir.rstrip('/'))
        else:
            # The label may use a GN variable, e.g.
            # "$devtools_frontend_path:devtools_frontend_resources". Recover the
            # target from the ":target" suffix and take the directory from the
            # load error itself.
            tm = re.search(r':([A-Za-z0-9_./\-]+)"', tail)
            # A relative label like "test/cctest" names no target; GN then uses
            # the directory basename. The load error already gave the directory.
            label_dir = build_dir
            target = tm.group(1) if tm else os.path.basename(build_dir)
        path = os.path.join(root, label_dir, 'BUILD.gn')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.exists(path):
            body = open(path).read()
            if f'group("{target}")' in body:
                print(f"[{step}] {label_dir}:{target} already stubbed but still failing")
                return 1
        else:
            body = ("# Stub created by scripts/patch_gn_stubs.sh for the pruned\n"
                    "# engine archive: the real tree is absent, so these targets\n"
                    "# are empty groups that keep dep labels resolvable.\n")
        with open(path, 'w') as f:
            f.write(body)
            f.write(f'\ngroup("{target}") {{\n}}\n')
        print(f"[{step}] stubbed //{label_dir}:{target}")

    print("gave up after too many iterations")
    return 1


if __name__ == '__main__':
    sys.exit(main())
