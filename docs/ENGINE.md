# Engine integration

WebGTK keeps the GTK view and the web engine behind one seam,
`webgtk::BackendOps` (see `src/webgtk_backend_ops.h`). Two backends implement
it:

| backend | file | built when |
| --- | --- | --- |
| mock | `src/backend_mock.cc` | always |
| Chromium | `src/backend_chromium.cc` | an engine is configured |

The mock backend renders synthetic frames, so the widget, the demos and the
headless test suite work with no engine at all. That is the default build.

## The rule: nobody compiles Chromium to use WebGTK

A content + blink + v8 build takes hours and needs ~16 cores, 32 GB of RAM and
100 GB of disk. Developers and applications must never do it. Instead the
engine is built **once**, in CI, and published as a prebuilt artifact that
everyone links in seconds.

```
  .github/workflows/build_engine.yml      build once (big runner)
                │
                ▼
  webgtk-engine-linux-x64.tar.gz          published artifact
                │
   scripts/download_engine_prebuilt.sh    fetch + extract
                │
                ▼
  cmake -DWEBGTK_USE_PREBUILT_ENGINE=ON   link in seconds
```

## Using the prebuilt engine

```sh
scripts/download_engine_prebuilt.sh
cmake -S . -B build -DWEBGTK_USE_PREBUILT_ENGINE=ON
cmake --build build
```

CMake resolves the engine in this order:

1. `-DWEBGTK_ENGINE_DIR=<tree>` — compile against a Chromium source tree.
2. `-DWEBGTK_USE_PREBUILT_ENGINE=ON` — link `engine/prebuilt/`.
3. `engine/prebuilt/manifest.env` exists — use it automatically.
4. none — mock backend only.

`download_engine_prebuilt.sh` defaults to the newest GitHub Release asset.
Pass `--url` to fetch from Hugging Face instead, and `--sha256` to verify it:

```sh
scripts/download_engine_prebuilt.sh \
  --url https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk-engine-linux-x64.tar.gz \
  --sha256 <hex>
```

## What the artifact contains

```
lib/libwebgtk_engine.so   the engine, link this
lib/*.so                  its component-library dependency closure
include/                  staged headers
gen/                      generated headers
manifest.env              revision, flags, checksum
webgtk-engine.pc          pkg-config file
```

The engine is built with `is_component_build=true`, so Chromium is a set of
shared libraries. `libwebgtk_engine.so` links them, and the packaging step
walks `ldd` to copy the whole closure into `lib/`, which is what makes the
artifact relocatable. Building one giant static archive was rejected: it needs
whole-archive linking that collides with Chromium's own shared-library layout.

CMake links **every** library in `lib/`, not just `libwebgtk_engine.so`. A
library that only lists the others as dependencies does not resolve their
symbols for its consumers: the backend compiles into `libwebgtk.so`, so its
references to Chromium symbols have to be resolved at that link step, and an
umbrella that pulls the components in as `DT_NEEDED` entries is not enough.

## Building the artifact

`.github/workflows/build_engine.yml` builds and publishes it. Trigger it from
the Actions tab, or push an `engine-v*` tag. It:

1. installs the system dependencies and frees disk space,
2. caches the source archive and the clang/rust toolchains,
3. runs `scripts/build_engine_artifact.sh`,
4. extracts the artifact into `engine/prebuilt/` and builds WebGTK against it,
5. runs the test suite against the prebuilt engine,
6. uploads the artifact, and optionally publishes it to a GitHub Release or to
   Hugging Face.

The same script runs locally on a big machine:

```sh
scripts/build_engine_artifact.sh --dir ./engine --jobs "$(nproc)"
```

### The source archive

`https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk_core_v2.tar.gz`
(685 MB) is a **pruned Chromium source tree**, not a prebuilt engine. Extracted
it is 2.7 GB / ~200k files with no build outputs, and it is missing inputs that
`gn gen` needs. `scripts/build_engine_artifact.sh` restores them:

| missing input | restored by |
| --- | --- |
| `third_party/perfetto` | `git clone` at the `DEPS` revision |
| `third_party/webrtc` | shallow `git fetch` at the `DEPS` revision |
| clang toolchain | `tools/clang/scripts/update.py` |
| Rust toolchain | `tools/rust/update_rust.py` |
| `testing/` and `//testing/libfuzzer` | no-op GN template stubs |
| `build/config/gclient_args.gni` | written by the script |
| `gn` | CIPD |

The `//testing` stubs matter: the real `testing/libfuzzer/fuzzer_test.gni`
imports `//build/config/sanitizers/sanitizers.gni`, and `media/media_options.gni`
depends on that transitive import for `is_asan`. The stub re-exports the import
so downstream GN files still resolve it.

`third_party` is only *partially* pruned: the directories are present but
generated files inside them (for example `third_party/ffmpeg/ffmpeg_options.gni`)
are gone. `scripts/check_engine_deps.py <tree>` lists every GN file the tree
references but does not contain, which is the fastest way to see the remaining
gap.

### Reaching a clean `gn gen`

The pruned tree is missing ~140 GN files that `gn gen` loads at parse time even
though WebGTK never builds the subtrees they belong to. Three scripts close the
gap:

| script | role |
| --- | --- |
| `scripts/patch_gn_stubs.sh` | hand-written stubs for the subtle cases (templates that must re-export an import, flags a file must declare) |
| `scripts/gen_stub_driver.py` | runs `gn gen` in a loop, stubbing whatever it reports missing, until the parse is clean |
| `scripts/collect_gn_stubs.py` / `scripts/apply_gn_stubs.py` | freeze the discovered stubs into `scripts/gn_stubs/` and replay them onto a fresh tree |

On a freshly extracted archive, prime the tree and generate (this is what
`scripts/setup_engine.sh` now does automatically):

```sh
ENGINE_ROOT=${WEBGTK_ENGINE_DIR:-engine}
SRC=$ENGINE_ROOT/chromium
scripts/patch_gn_stubs.sh "$SRC"
python3 scripts/apply_gn_stubs.py "$SRC"
"$ENGINE_ROOT/gnroot/gn" gen "$SRC/out/webgtk" --root-target=//content \
  --args="$DIET_ARGS"
```

`scripts/gn_stubs/` is the committed snapshot of every stub the driver found;
`MANIFEST` lists them and `DATA_FILES` marks the few markerless data files
(`chrome/VERSION`, the `BRANDING` file). Most stubs are empty `group()`
targets; a handful are `config()` (Chromium names those `*_config`), and a few
are `action()`s that emit a file another target's `grit()` reads.

`scripts/gen_stub_driver.py` is the search tool, not part of the normal build:
it exists so that a new tree revision can rediscover the set rather than have
the snapshot guessed at. `gn gen` against the primed tree completes in about
three seconds and produces ~17k targets.

### Build arguments

`scripts/build_engine_artifact.sh` sets these:

- `is_official_build=true`, `is_debug=false`, `symbol_level=0` — release.
- `is_component_build=true` — shared libraries, see above.
- `enable_rust=true` — `//base` has Rust sources; the toolchain is restored.
- `use_kerberos=false` — no vendored `third_party/kerberos`.
- `use_sysroot=false` — build against the host sysroot.
- `is_cfi=false` — CFI asserts it needs ThinLTO, which is off.
- `chrome_pgo_phase=0` — no PGO profiles in a pruned tree.

GN silently ignores arguments that are not declared, so this list stays valid
across tree revisions.

## Compile verification

The pruned archive cannot produce a working engine, but it *can* compile the
one translation unit that talks to Content. That is what
`scripts/verify_engine.sh` does, and it is how API drift in
`src/backend_chromium.cc` is caught without a full build:

```sh
scripts/setup_engine.sh                     # primed tree + out/webgtk
scripts/verify_engine.sh --dir engine       # compile src/backend_chromium.cc
```

The script takes the exact command line `gn` emitted for a `//content` source
file (`ninja -t commands obj/content/public/app/app/content_main_delegate.o`),
rewrites it to build the backend instead, and compiles. Every `-I`, `-isystem`,
`-D` and `-W... -Werror` flag is kept, so the engine's real headers are the
ones the compiler sees and any signature change is a hard error.
`scripts/engine_compile_flags.py` performs the rewrite; it drops only the
depfile, the raw_ptr plugin (a Chromium code-health policy for Chromium's own
`raw_ptr<>`-managed classes, not for embedder code) and color diagnostics.

CMake exposes this as the `webgtk-verify-engine` target in source mode:

```sh
cmake -S . -B build -DWEBGTK_ENGINE_DIR=engine
cmake --build build --target webgtk-verify-engine
```

`WEBGTK_ENGINE_DIR` does **not** link Chromium: there are no build outputs to
link, and the backend cannot be compiled by CMake's own flags. The mock backend
still backs the library, demos and tests; the verify target is the check that
the real backend compiles.

Two inputs the archive prunes are restored by `scripts/setup_engine.sh` because
the compile needs them: the libc++ headers at the `DEPS` revision
(`-nostdinc++` is in the gn flags) and a `testing/gtest/include/gtest/gtest_prod.h`
stub (`//base/gtest_prod_util.h` includes it). Both are in the `gn_stubs`
snapshot or fetched by the setup script, so a clean checkout reaches the same
result.

## Status

The mock build is complete and green: 4/4 tests pass under `xvfb-run`, zero
warnings.

The Chromium backend compiles cleanly against the real engine headers:
`scripts/verify_engine.sh` builds `src/backend_chromium.cc` with gn's own flags
and reports no error and no warning.

The prebuilt path is verified end to end with a stub artifact: the downloader
checksums and extracts, CMake finds `engine/prebuilt/`, compiles
`backend_chromium.cc` with the staged include paths, links `libwebgtk_engine.so`
with a working `RUNPATH`, and the closure collector copies a self-resolving set
of shared libraries.

What is **not** verified is a linked engine: the artifact has never been
produced from a full build, so the backend has not yet been run against a live
Content stack. The first successful `build_engine.yml` run is what closes that
gap.

### What the backend does

- **Startup**: runs `content::ContentMain` on a dedicated engine thread with a
  `HeadlessContentMainDelegate` wrapping a `HeadlessBrowserImpl`. The start
  callback creates a browser context and a `HeadlessWebContents` sized to the
  view.
- **Threading**: every vtable call arrives on the GTK main thread and is posted
  to the engine thread's `SingleThreadTaskRunner`. `render()` is synchronous: it
  posts a `CopyFromSurface` request and waits on a condition variable.
- **Frames**: `CopyFromSurface` returns a premultiplied `SkBitmap`; the callback
  converts it to straight RGBA8, which is what `webgtk::Bitmap` promises.
- **Navigation**: `load_uri` loads a URL; `load_html` loads a `data:` URL.
- **Script**: `evaluate_javascript` runs on the primary main frame and
  serialises the result with `base::WriteJson`.
- **Input**: mouse, wheel and keyboard events go through
  `RenderWidgetHost::ForwardMouseEvent` / `ForwardWheelEvent` /
  `ForwardKeyboardEvent` with Blink event types.

### Remaining work

1. Run `build_engine.yml` on a runner with enough capacity and fix whatever the
   real build reports — the engine target names and `backend_chromium.cc` are
   the likely first failures.
2. Fix any `third_party` gaps the build surfaces, using
   `check_engine_deps.py`.
3. Publish the artifact, then drop the `--url` override from the docs.
