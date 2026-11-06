#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Turn a gn-generated Chromium compile command into one that builds a WebGTK
translation unit.

Chromium's build emits, for every source file, a `clang++ ... -c <file>` command
line. `ninja -t commands` prints it verbatim. This script reads one such line
and rewrites it to compile a WebGTK source instead, keeping every flag that
decides API compatibility:

  * all -I / -isystem include paths, in their original order, so the exact
    headers from the engine tree are the ones the compiler sees;
  * every -D define (buildflags, platform switches, feature flags);
  * every -W... warning, including -Werror, so drift is a hard failure.

Only these are changed:

  * the source file and its -o output are replaced;
  * the depfile (-MD / -MF) is dropped, since nothing consumes it;
  * the raw_ptr plugin is dropped. Its field check (check-raw-ptr-fields and
    friends) is a Chromium code-health policy for Chromium's own
    raw_ptr<>-managed classes; WebGTK is embedder code, so the check does not
    apply and would reject ordinary pointer members in a public header;
  * -fcolor-diagnostics becomes -fno-color-diagnostics so the caller can parse
    clang's output without ANSI escapes.

The engine's own working directory matters: gn emits include paths relative to
the output directory (for example `-I../..` and `-Igen`), so the command must be
run from <engine>/out/webgtk. The generated script changes into it itself.

Usage:
    engine_compile_flags.py --cmd <cmd.txt> --repo <repo-root> \\
        --source <file.cc> --object <out.o> --script <out.sh>
"""

import argparse
import os
import shlex


def strip_plugin(tokens, i):
    """Return the index past a raw-ptr-plugin group starting at `i`, or None.

    The group is `-Xclang -add-plugin -Xclang raw-ptr-plugin` followed by any
    number of `-Xclang -plugin-arg-raw-ptr-plugin -Xclang <arg>` pairs.
    """
    if (tokens[i] != "-Xclang" or i + 3 >= len(tokens)
            or tokens[i + 1] != "-add-plugin" or tokens[i + 2] != "-Xclang"
            or tokens[i + 3] != "raw-ptr-plugin"):
        return None
    j = i + 4
    while (j + 3 < len(tokens) and tokens[j] == "-Xclang"
           and tokens[j + 1] == "-plugin-arg-raw-ptr-plugin"
           and tokens[j + 2] == "-Xclang"):
        j += 4
    return j


def rewrite(tokens, repo, source, obj):
    keep = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("-MD", "-MF"):
            i += 2 if t == "-MF" else 1
            continue
        if t.endswith(".cc") or t.endswith(".c") or t == "-c":
            i += 1
            continue
        if t == "-o":
            i += 2
            continue
        end = strip_plugin(tokens, i)
        if end is not None:
            i = end
            continue
        keep.append("-fno-color-diagnostics" if t == "-fcolor-diagnostics" else t)
        i += 1

    keep += [
        "-c", source,
        "-o", obj,
        "-DWEBGTK_HAVE_ENGINE=1",
        "-I" + os.path.join(repo, "include"),
        "-I" + os.path.join(repo, "src"),
    ]
    return keep


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cmd", required=True, help="file with one clang++ command")
    ap.add_argument("--repo", required=True, help="WebGTK repository root")
    ap.add_argument("--source", required=True, help="source file to compile")
    ap.add_argument("--object", required=True, help="object file to write")
    ap.add_argument("--script", required=True, help="script to generate")
    ap.add_argument("--cwd", default=".", help="directory the script runs in")
    args = ap.parse_args()

    tokens = shlex.split(open(args.cmd).read().strip())
    keep = rewrite(tokens, args.repo, args.source, args.object)
    body = " ".join(shlex.quote(k) for k in keep)
    with open(args.script, "w") as f:
        f.write("#!/usr/bin/env bash\n")
        f.write("set -e\n")
        f.write(f"cd {shlex.quote(args.cwd)}\n")
        f.write(f"exec {body}\n")
    os.chmod(args.script, 0o755)


if __name__ == "__main__":
    main()
