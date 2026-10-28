# Packaging: portable builds of the working tree

Everything in this directory turns **the current checkout, uncommitted changes
included** into two self-contained artifacts that end users run without
installing anything:

| Target     | Artifact (in `packaging/dist/`)                        | How the user runs it                                   |
|------------|--------------------------------------------------------|--------------------------------------------------------|
| `appimage` | `RawTherapee-<version>-x86_64.AppImage`                | `chmod +x` and run (any x86_64 Linux with glibc >= 2.34) |
| `windows`  | `RawTherapee-<version>-win64-portable.zip`             | unzip, double-click `rawtherapee.exe` (64-bit Windows) |

`<version>` is `git describe --tags --always --dirty`, e.g.
`5.13-32-gb2ee504c4-dirty`; `-dirty` means the tree had uncommitted changes
to tracked files when it was built. The same string is shown in
*Help > About* and by `rawtherapee-cli --version`.

Both builds run **inside Docker containers**. Nothing is installed on the
host, which only needs `git`, `docker` (the invoking user must be allowed to
run it) and bash. GitHub Actions runs the same scripts
(`.github/workflows/masks-release.yml`): pushing a `masks-*` tag builds both
packages and publishes them as a GitHub release of that name.

```
./packaging/release.sh appimage    # Linux AppImage
./packaging/release.sh windows     # portable Windows zip (cross-compiled)
./packaging/release.sh all         # both
./packaging/release.sh version     # print the version string that will be used
./packaging/release.sh clean       # delete packaging/work (build caches) and packaging/dist
```

The first run of each target builds a Docker *builder image* (needs network,
10–20 minutes). Subsequent runs reuse the image and the persistent build
directories under `packaging/work/`, so a rebuild after a code change takes
about as long as an incremental native build plus a minute of packaging.
`packaging/work/` and `packaging/dist/` are git-ignored.

Useful environment variables (all targets):

| Variable          | Effect                                                                 |
|-------------------|------------------------------------------------------------------------|
| `JOBS=N`          | parallel compile jobs (default: `nproc`)                               |
| `REBUILD_IMAGE=1` | rebuild the builder image (after editing a Dockerfile, or to refresh)  |
| `PULL=1`          | with `REBUILD_IMAGE=1`, also re-pull the base image                    |
| `CLEAN=1`         | wipe that target's build directory first (full rebuild)                |
| `SKIP_TESTS=1`    | skip the in-container smoke tests                                      |
| `DOCKER=podman`   | use another Docker-compatible CLI                                      |

## Layout

```
packaging/
├── release.sh                 front door (appimage | windows | all | version | clean)
├── common.sh                  shared helpers: version, ReleaseInfo.cmake, docker run
├── appimage/
│   ├── Dockerfile             Ubuntu 22.04 builder image (Exiv2 0.28, libjxl, linuxdeploy)
│   ├── build-appimage.sh      host-side entry point
│   └── build-inside.sh        runs in the container: configure, build, linuxdeploy, tests
├── windows/
│   ├── Dockerfile             Fedora 44 + mingw64-* cross toolchain and libraries
│   ├── build-windows.sh       host-side entry point
│   └── build-inside.sh        runs in the container: LibRaw, configure, build, bundle, tests
├── dist/                      output (git-ignored)
└── work/                      persistent build dirs and logs (git-ignored)
```

How the tree gets into the container: the repository is bind-mounted
read-only at `/repo`, `rsync`ed into `packaging/work/<target>/src`
(excluding `build/`, `.git/`, `packaging/work`, `packaging/dist` and
`__pycache__`) and built out of tree in `packaging/work/<target>/build`.
Because the container has no `.git`, the host writes a `ReleaseInfo.cmake`
(the same file `tools/generateReleaseInfo` produces for source tarballs,
plus the `-dirty` marker) which `UpdateInfo.cmake` picks up instead of
calling git. No file of the source tree is modified; nothing outside
`packaging/` is touched.

## AppImage (`packaging/appimage/`)

Follows `.github/workflows/appimage.yml` step by step, in an Ubuntu 22.04
container (glibc 2.35, so the result runs on distributions from 2022 on):

* builder image: Ubuntu packages for GTK/gtkmm, lensfun, librsvg, lcms2,
  fftw, libiptcdata, libtiff, libcanberra, libwebp...; **Exiv2 0.28.8** and
  **libjxl 0.11.2** built from source (Ubuntu's are too old); `fmt` pre-cloned
  at the commit `cmake/Dependencies.cmake` pins so the build itself is
  offline; `linuxdeploy` + `linuxdeploy-plugin-gtk` (with the forced
  `GTK_THEME` export removed, as upstream does); the lensfun database
  refreshed with `lensfun-update-data` at image build time.
* configure: `-DBUILD_BUNDLE=ON -DBUNDLE_BASE_INSTALL_DIR=/ -DCMAKE_INSTALL_PREFIX=/usr
  -DPROC_TARGET_NUMBER=1 -DLENSFUNDBDIR=../share/lensfun/version_1 -DWITH_JXL=ON`,
  Release, OpenMP on, LTO off; the vendored LibRaw is built by the tree's own
  CMake glue.
* install with `DESTDIR=AppDir/usr/bin` (so `rawtherapee`, `rawtherapee-cli`
  and all data, including `segmentation/` and `icons/`, sit in `usr/bin`),
  `share/` moved to `usr/share`, lensfun DB and a partial Adwaita icon theme
  added, then `linuxdeploy --plugin gtk --output appimage` (with
  `APPIMAGE_EXTRACT_AND_RUN=1`, no FUSE needed in Docker).
* smoke tests, all inside the container and with a `PATH` that contains **no
  python**: `rawtherapee-cli --version` prints the expected version;
  `rawtherapee-cli -c` converts a generated PNG to JPEG using the bundled
  profiles/ICC data; the GUI (`AppRun`) is started under Xvfb and must still
  be alive after 12 s.

Running it on a machine without FUSE: `./RawTherapee-*.AppImage --appimage-extract-and-run`.
The CLI is reachable via `./RawTherapee-*.AppImage --appimage-extract` and
then `squashfs-root/usr/bin/rawtherapee-cli`.

## Portable Windows build (`packaging/windows/`)

Upstream builds Windows binaries natively with MSYS2 on a GitHub runner
(`.github/workflows/windows.yml`). Here the same thing is done as a
**cross-compile in a Fedora 44 container** with Fedora's MinGW-w64 toolchain,
because Fedora packages nearly the whole GTK stack for MinGW
(`mingw64-gtkmm30`, `mingw64-exiv2` 0.28, `mingw64-librsvg2`, `mingw64-fftw`,
`mingw64-lcms2`, `mingw64-libtiff`, `mingw64-libjpeg-turbo`, `mingw64-libpng`,
`mingw64-expat`, `mingw64-zlib`, `mingw64-libgomp`, `mingw64-adwaita-icon-theme`...).

What the Dockerfile adds on top, all from source and pinned:

* **lensfun 0.3.4** (CMake), **libiptcdata 1.0.5** (autotools, library only) and
  **libjxl 0.11.2** (CMake, vendored highway/skcms, system brotli and lcms2):
  Fedora has no MinGW packages for these.
* an **SVG gdk-pixbuf loader** compiled from librsvg's public-API-only C
  loader (`io-svg.c`, librsvg 2.56.4) against the sysroot librsvg: Fedora's
  `loaders.cache` lists it but the mingw librsvg package does not ship it,
  and GTK needs it for the Adwaita symbolic icons.
* compiled GSettings schemas (`gschemas.compiled`), `fmt` pre-cloned, the
  lensfun database tarball, and Wine + Xvfb for the smoke tests.

At build time (`build-inside.sh`):

1. **LibRaw**: `external/LibRaw.cmake` runs `./configure` without `--host`,
   which cannot cross-compile, so the vendored `external/libraw` sources are
   configured out of tree with `--host=x86_64-w64-mingw32` (OpenMP on, shared)
   and installed into the MinGW sysroot; RawTherapee is then configured with
   `-DWITH_SYSTEM_LIBRAW=ON` and finds it via `libraw_r.pc`. The build dir is
   kept, so this is incremental too. No CMake file of the tree is changed.
2. **RawTherapee**: `cmake -DCMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-mingw64.cmake
   -DCMAKE_BUILD_TYPE=Release -DPROC_TARGET_NUMBER=1 -DLENSFUNDBDIR=share/lensfun
   -DWITH_SYSTEM_LIBRAW=ON -DWITH_JXL=AUTO -DBUNDLE_BASE_INSTALL_DIR=<stage>`
   (the same options as `windows.yml`), then `cmake --build` / `--install`.
   `BUILD_BUNDLE` is forced on for Windows, so `rawtherapee.exe`,
   `rawtherapee-cli.exe` and all data land flat in the stage directory.
   Two cross-compile quirks are handled by the script, not by source changes:
   `-D_USE_MATH_DEFINES` is added to the compiler flags (the tree is built with
   `-std=c++17`, under which mingw's `<math.h>` hides `M_PI`/`M_SQRT2`, used
   unqualified in `rtengine/maskgeometry.cc`), and an empty
   `rtdata/WindowsInnoSetup.iss` is created before `cmake --install`
   (`UpdateInfo.cmake` only generates it when the *host* is Windows; the
   file is removed from the bundle afterwards).
3. **Bundle**, mirroring the "Bundle dependencies" step of `windows.yml`:
   lensfun DB -> `share/lensfun/*.xml`; `lib/gdk-pixbuf-2.0/2.10.0/{loaders,loaders.cache}`;
   `share/glib-2.0/schemas/gschemas.compiled`; Adwaita `symbolic/{actions,devices,mimetypes,places,status,ui}`
   + `index.theme`, hicolor `index.theme`; `share/gtk-3.0/settings.ini`;
   `gspawn-win64-helper*.exe` and `gdbus.exe`; then every DLL reachable from
   the two exes and the pixbuf loaders through a recursive import-table walk
   (`x86_64-w64-mingw32-objdump -p`, the cross equivalent of `ldd`/`ntldd`;
   names not found in the sysroot are Windows system DLLs). Exes and DLLs
   are stripped.
4. **Smoke tests under Wine**: `rawtherapee-cli.exe --version` must print the
   version; `rawtherapee-cli.exe -c` must convert a PNG to JPEG; `rawtherapee.exe`
   is started under Xvfb for 20 s (best effort: a failure here is reported but
   does not fail the build, Wine is not Windows).
5. zip of the folder `RawTherapee-<version>-win64-portable/`.

The resulting folder is double-click runnable: `rawtherapee.exe` at the top
level, all DLLs beside it, `share/` and `lib/` next to it (GTK and gdk-pixbuf
locate their data relative to the directory the DLLs live in), and RawTherapee's
own data (`languages/`, `profiles/`, `iccprofiles/`, `icons/`, `segmentation/`...)
in the same directory, exactly as the upstream zip from `windows.yml`.
Settings and cache go to `%LOCALAPPDATA%\RawTherapee5-dev-portable` (the
`CACHE_NAME_SUFFIX` of this build), so the portable build does not interfere
with an installed RawTherapee.

## The AI masks backend (`rtdata/segmentation/`)

Both bundles ship `segmentation/rt_segment_server.py` and its `README.md`
next to the executable, but **not** Python, PyTorch or the models (several GB,
hardware specific). RawTherapee starts without them: at startup the engine
tries to launch `<Command> <segmentation/rt_segment_server.py>`; if the
interpreter is missing the Subject / Sky / Background / Object entries of the
Masks tool are simply listed as unavailable and everything else works. The
smoke tests verify this by running both bundles with no `python` on `PATH`
(and under Wine, where there is none at all).

To enable the AI masks, install Python 3.10+ with `numpy pillow torch
transformers huggingface_hub` (see `segmentation/README.md` in the bundle for
models and GPU notes) and point RawTherapee at it in the `options` file,
group `[Segmentation]`:

```
[Segmentation]
Enabled=true
Command=python3        ; interpreter that has torch/transformers
Script=                ; empty = the bundled segmentation/rt_segment_server.py
Device=auto            ; auto | cuda | cpu | mps
```

Where the `options` file lives:

* **AppImage**: `~/.config/RawTherapee5-dev-AppImage/options` (created on first
  start). `Command` can be an absolute path, e.g. `Command=/home/me/venv/bin/python`.
* **Windows portable**: `%LOCALAPPDATA%\RawTherapee5-dev-portable\options`.
  Use the full path of the interpreter, e.g.
  `Command=C:\Users\me\AppData\Local\Programs\Python\Python312\python.exe`
  or a venv's `Scripts\python.exe`; `python3` alone only works if it is on `PATH`.
  To make a truly self-contained USB-stick setup, create `options` next to
  `rawtherapee.exe` instead: a portable build reads `options` from its own
  directory first and then also keeps settings and cache in `mysettings/` and
  `mycache/` beside the exe.

`Script=` may point at a copy of `rt_segment_server.py` elsewhere (useful to
hack on the backend without rebuilding). Weights are downloaded on first use
into the Hugging Face cache; SAM 3 needs access to the gated
`facebook/sam3` repository (`hf auth login`), otherwise SAM 2.1 + SegFormer is
used, and `segformer` alone is the CPU-friendly choice.

## Reproducing from scratch

```
git clone <this repo> && cd RawTherapee        # or use the existing checkout
./packaging/release.sh all
ls -l packaging/dist/
```

Everything that needs the network (base images, Fedora/Ubuntu packages,
Exiv2/libjxl/lensfun/libiptcdata/fmt sources, linuxdeploy, lensfun DB) is
fetched during the two `docker build`s and pinned by version/commit in the
Dockerfiles; the per-build steps run offline. To refresh: `REBUILD_IMAGE=1 PULL=1`.

## Known limitations

* Linux: x86_64 only; needs glibc >= 2.34 (Ubuntu 22.04 / Debian 12 / Fedora 35
  and newer). The AppImage uses the X11 backend (`GDK_BACKEND=x11`, set by the
  linuxdeploy GTK hook) and runs under XWayland on Wayland desktops.
* Windows: x86_64 only, no arm64; no installer (the upstream Inno Setup step is
  intentionally skipped, the zip is the deliverable). The GUI is smoke-tested
  under Wine, not on real Windows.
* The builder images are not rebuilt automatically when a Dockerfile changes:
  run with `REBUILD_IMAGE=1`.
* `AboutThisBuild.txt` (and *Help > About*) carries version, branch, commit
  and commit date, but the "Build date / OS / UUID" lines are empty: the
  container has no `.git`, so `UpdateInfo.cmake` takes the `ReleaseInfo.cmake`
  path, which (as for upstream source tarballs) does not fill them in.
