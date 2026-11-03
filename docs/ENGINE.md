# Engine integration

WebGTK keeps the GTK view and the web engine behind one seam,
`webgtk::BackendOps` (see `src/webgtk_backend_ops.h`). Two backends implement
it:

| backend | file | built when |
| --- | --- | --- |
| mock | `src/backend_mock.cc` | always |
| Chromium | `src/backend_chromium.cc` | `-DWEBGTK_ENGINE_DIR=<dir>` |

The mock backend renders synthetic frames, so the widget, the demos and the
headless test suite work with no engine at all. That is the default build.

## What the engine archive actually is

The archive at
`https://huggingface.co/datasets/M5-Dev/webgtk-chromium-diet/resolve/main/webgtk_core_v2.tar.gz`
(685 MB) is a **pruned Chromium source tree**, not a prebuilt engine. After
extraction it is 2.7 GB / ~200k files of `.cc` and `.h`, with no build outputs.

It is missing the inputs `gn gen` needs, so it cannot be built as published:

- `testing/` (the whole directory) — `media/media_options.gni` and `content`
  import `//testing/libfuzzer/fuzzer_test.gni` and `//testing/test.gni`
- `third_party/libevent`, `third_party/kerberos`
- `third_party/llvm-build` (the clang toolchain)
- `buildtools/linux64` (the `gn` and `ninja` prebuilts)
- `third_party/rust-toolchain`
- `.gni` files for `ffmpeg`, `webrtc`, `pdfium`, `nasm`, `dawn`, and others

`scripts/check_engine_deps.py` lists all of them in one pass:

```
python3 scripts/check_engine_deps.py /path/to/chromium
```

## Preparing the tree

`scripts/setup_engine.sh` performs the steps that do work and then runs
`gn gen`:

```sh
scripts/setup_engine.sh --dir /path/to/engine
```

It downloads the archive, restores `third_party/perfetto` at the revision
`chromium/DEPS` pins, installs `gn` via CIPD, writes
`build/config/gclient_args.gni` (which `gclient runhooks` would normally
produce), and runs:

```
gn gen out/webgtk --root-target=//content --args="...diet flags..."
```

`gn gen` stops on the first missing import, so it does not finish until the
missing inputs above are restored. The script reports the gap and points at
`check_engine_deps.py`.

## Building WebGTK against the engine

Once the tree generates ninja files and the engine is built:

```sh
ninja -C /path/to/engine/chromium/out/webgtk content
cmake -S . -B build -DWEBGTK_ENGINE_DIR=/path/to/engine
cmake --build build
```

`WEBGTK_ENGINE_DIR` adds `src/backend_chromium.cc` to the build, defines
`WEBGTK_HAVE_ENGINE`, and adds the engine root plus
`out/webgtk/gen` to the include path (a Chromium embedder compiles against the
checkout, not a staged SDK).

## Status of `src/backend_chromium.cc`

The backend is written against the APIs in the archive and is **not yet
compile-verified**. With `WEBGTK_ENGINE_DIR` pointed at the extracted tree the
compiler resolves real Chromium headers and stops at the first *generated*
header:

```
base/debug/debugging_buildflags.h: No such file or directory
```

That file is produced by the engine build, so this is the expected boundary
until `gn gen` succeeds. Every API the backend calls was read out of the tree,
so the calls match the source revision; the first compile against a complete
checkout is still the real review.

### What the backend does

- **Startup**: runs `content::ContentMain` on a dedicated engine thread with a
  `HeadlessContentMainDelegate` wrapping a `HeadlessBrowserImpl`. The browser's
  start callback creates a browser context and a `HeadlessWebContents` sized to
  the requested view.
- **Threading**: every vtable call arrives on the GTK main thread and is posted
  to the engine thread's `SingleThreadTaskRunner`. `render()` is synchronous:
  it posts a `CopyFromSurface` request and waits on a condition variable.
- **Frames**: `CopyFromSurface` returns a premultiplied `SkBitmap`; the callback
  converts it to straight RGBA8, which is what `webgtk::Bitmap` promises.
- **Navigation**: `load_uri` loads a URL; `load_html` loads a `data:` URL, so it
  needs no temp file.
- **Script**: `evaluate_javascript` runs on the primary main frame and serialises
  the result with `base::WriteJson`.
- **Input**: mouse, wheel and keyboard events go through
  `RenderWidgetHost::ForwardMouseEvent` / `ForwardWheelEvent` /
  `ForwardKeyboardEvent` with Blink event types.

### Remaining work

1. Restore the missing inputs so `gn gen` completes.
2. Build `//content` and the engine, then build WebGTK against it.
3. Compile `src/backend_chromium.cc` and fix whatever the compiler reports.
4. Package the engine as `libwebgtk_engine.a` (or a set of archives) and a
   staged `include/` tree so consumers do not need the whole checkout.
5. Run the GTK3/GTK4 test suite against the Chromium backend under `xvfb-run`.
