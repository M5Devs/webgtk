# Engine build pipeline

How the pruned Chromium engine archive is taken from a tarball to a compiled
`//content` tree, and how the build repairs itself when the pruning removed
something it needs.

`docs/ENGINE.md` covers the *integration* seam (how WebGTK links a prebuilt
engine). This document covers the *build*: producing that engine from the
archive in `engine/webgtk_core_v2.tar.gz`.

## Why a self-healing build

The archive is a Chromium checkout pruned for size. Pruning removes whole
subtrees: git dependencies are shipped as empty `src/` directories, CIPD and
GCS tools are dropped, and some vendored files are gone. The build files
(`BUILD.gn`, `DEPS`) are the *unpruned* originals, so they still reference
everything.

Rather than hand-maintain a list of what to restore, the pipeline reads the
same `DEPS` file Chromium's own `gclient` reads and restores each dependency
from the source and revision it records. The build is driven in a loop that
stops at the first thing it cannot resolve, classifies the failure, repairs it,
and resumes. A full run reaches thousands of compiled objects without a human
in the loop.

## The pipeline

Run these in order. Each is idempotent, so re-running is safe and cheap.

```
  1. scripts/setup_gn_env.sh      host libraries (apt) + 3 files GN reads at gen time
  2. scripts/setup_engine.sh      fetch + extract the archive, restore source deps, gn gen
  3. scripts/setup_toolchain.sh   clang, rust, node, typescript, mold
  4. scripts/engine_autoheal.py   drive ninja, repairing what it trips over
```

### 1. Host libraries — `setup_gn_env.sh`

Installs the apt packages the GN build files probe with `pkg-config`
(libva, libdrm, alsa, pulse, GTK, X11, and so on) and writes three files the
archive omits that GN reads at generation time: `dri.pc`,
`build/util/LASTCHANGE.committime` and the Chromium `BRANDING` file.

```sh
sudo scripts/setup_gn_env.sh [engine-root]
```

The container drops apt packages between sessions but keeps `/workspace`, so
re-run this first in a fresh session.

### 2. Source tree and GN files — `setup_engine.sh`

Downloads `webgtk_core_v2.tar.gz` from Hugging Face, extracts it, restores the
source dependencies GN needs at generation time (`third_party/perfetto`,
libc++ headers), installs the GN binary via CIPD, and runs `gn gen` for
`//content`.

```sh
scripts/setup_engine.sh [--dir <engine-root>] [--no-deps] [--no-gen]
```

`gn gen` needs empty stand-in `BUILD.gn` files for the subtrees the archive
dropped, because a build file it does need still imports them at parse time.
`scripts/apply_gn_stubs.py` replays the frozen snapshot in `scripts/gn_stubs/`
(195 files); `scripts/gen_stub_driver.py` rediscovers them by iteration if the
snapshot is ever insufficient.

On success the tree looks like:

```
<engine-root>/chromium/            pruned Chromium checkout
<engine-root>/gnroot/gn            GN binary (CIPD)
<engine-root>/chromium/out/webgtk  generated ninja files
```

### 3. Compiler toolchain — `setup_toolchain.sh`

`gn gen` only needs the source tree; *compiling* needs the compilers and tools
the generated ninja files invoke. This script installs them, each at the exact
revision `chromium/DEPS` pins:

| tool | source | installed by |
| --- | --- | --- |
| Clang 24 (compiler, plugins, builtins) | `chromium-browser-clang` GCS | `tools/clang/scripts/update.py` |
| Rust toolchain (rustc, cargo, bindgen) | `chromium-browser-clang` GCS | `tools/rust/update_rust.py` |
| Node.js | `chromium-nodejs` GCS | `engine_autoheal.py --prefetch` |
| `node_modules` | `chromium-nodejs` GCS | `engine_autoheal.py --prefetch` |
| TypeScript compiler | CIPD | `engine_autoheal.py --prefetch` |
| mold linker | CIPD | `engine_autoheal.py --prefetch` |

```sh
scripts/setup_toolchain.sh [engine-root]
```

The two update scripts are Chromium's own; they read the pinned revision from
the tree. The rest are `dep_type: gcs` and `dep_type: cipd` entries in `DEPS`,
which the autoheal driver already knows how to fetch, so the script calls its
`--prefetch` mode rather than re-implementing the download.

`mold` matters even though nothing links yet: `build/config/compiler` sets
`use_mold` on by default for a Linux clang build, and lists
`//buildtools/third_party/mold/cipd/mold` as an input to the Rust toolchain's
flag files. Without it, ninja cannot even plan the build.

### 4. Drive the compile — `engine_autoheal.py`

```sh
python3 scripts/engine_autoheal.py <engine-root> [--out out/webgtk] \
    [--target content] [--jobs N]
```

It runs `ninja -k 0 <target>`, reads the first unresolved input, classifies it,
repairs it, and re-runs. Progress and every repair are printed as they happen.

## What the driver repairs

| symptom | source used | how |
| --- | --- | --- |
| file under an empty git dependency | `DEPS` git entry | clone the URL at the pinned revision |
| file under a GCS-provisioned tree | `DEPS` GCS entry | download the bucket object, verify sha256, unpack |
| file under a CIPD-provisioned tree | `DEPS` CIPD entry | `cipd install <package> <instance>` |
| file in a vendored Rust crate | crates.io | fetch `name-version.crate` named by the vendored `Cargo.toml` |
| file in the Chromium tree itself | googlesource | fetch at the tree's own release tag from `chrome/VERSION` |
| missing generated header | ninja | build it by path |

Dependency paths nest, so the driver picks the **longest matching prefix**:
`third_party/gperf` is a git dependency while `third_party/gperf/cipd` inside
it is a CIPD one, and the file that is missing belongs to whichever is more
specific.

### What it refuses to do

The driver never guesses at a compile error. When ninja fails for a reason it
cannot classify it stops and reports:

- a missing **host package** (`pkg-config` not found) — run `setup_gn_env.sh`
- a **compile diagnostic** — print the first errors and stop
- an **already-repaired path** that is still missing — stop, to avoid a loop
- anything else — print the tail of the ninja log and stop

The exit code says which: `0` success, `1` unclassified ninja error, `3` host
package missing, `4` compile error, `5` repair loop, `6` fetch cap reached,
`7` a repair failed, `9` an input it could not classify, `10` round cap.

## Resuming

Each repair is appended to `engine/autoheal_state.json`. Re-running picks up
where it left off: files already restored are present, and the checkpoint stops
a path that was repaired once from being repaired again in a loop. To start
clean, delete the file.

Because ninja itself is incremental, a resumed run only recompiles what was
not already built. Killing the build at any point and re-running the same
command loses at most the objects in flight.

## Requirements

A `//content` build is large. Measured on the archive:

| resource | minimum | comfortable |
| --- | --- | --- |
| disk | 25 GB | 60 GB |
| RAM | 8 GB | 16 GB |
| cores | 4 | 16+ |

The extracted source tree is ~7.6 GB and `out/webgtk` grows past 15 GB. With 4
cores, expect the compile to run for many hours; it is a background job, not an
interactive one.

`--jobs` caps parallelism. On a memory-limited machine, keep it below the core
count: some translation units (v8, blink) need a few GB each.

## Running on a big runner or in CI

The whole pipeline is four commands and is designed to be checkpointed, so it
suits a long-lived or resumable runner:

```sh
sudo scripts/setup_gn_env.sh
scripts/setup_engine.sh
scripts/setup_toolchain.sh
python3 scripts/engine_autoheal.py engine --jobs "$(nproc)"
```

On CI, cache `<engine-root>/chromium/out/webgtk` and
`engine/autoheal_state.json` between runs. The first run does the long
dependency-restore and compile; later runs only build what changed.

`scripts/build_engine_artifact.sh` wraps the build and packages the result, and
`.github/workflows/build_engine.yml` runs it on a large runner. See
`docs/ENGINE.md` for how the packaged artifact is consumed.

## Files

| file | role |
| --- | --- |
| `scripts/setup_gn_env.sh` | host apt libraries, `gn gen` support files |
| `scripts/setup_engine.sh` | archive fetch/extract, source deps, `gn gen` |
| `scripts/setup_toolchain.sh` | clang, rust, node, typescript, mold |
| `scripts/engine_autoheal.py` | ninja driver and dependency repair |
| `scripts/apply_gn_stubs.py` | replay the frozen GN stub snapshot |
| `scripts/gn_stubs/` | the frozen stubs and the file manifest |
| `scripts/gen_stub_driver.py` | rediscover stubs if the snapshot is short |
| `scripts/build_engine_artifact.sh` | build and package the engine |
| `scripts/verify_engine.sh` | compile-verification build |
