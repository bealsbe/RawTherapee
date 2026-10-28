#!/usr/bin/env bash
# Runs INSIDE the rawtherapee-windows-builder container (see build-windows.sh).
# Cross-compiles RawTherapee for Windows x86_64 with Fedora's MinGW-w64
# toolchain and assembles a portable folder (exe + DLLs + data), zipped.
#
# Mounts:  /repo  the git checkout, read-only (uncommitted changes included)
#          /work  persistent scratch: source copy, LibRaw and CMake build dirs
#          /dist  where the finished zip goes
# Env:     RT_VERSION, JOBS, HOST_UID, HOST_GID, SKIP_TESTS (optional)
set -euo pipefail

R="${MINGW_PREFIX:-/usr/x86_64-w64-mingw32/sys-root/mingw}"
T="${MINGW_TRIPLET:-x86_64-w64-mingw32}"
TOOLCHAIN=/usr/share/mingw/toolchain-mingw64.cmake
export PKG_CONFIG_LIBDIR="$R/lib/pkgconfig:$R/share/pkgconfig"
unset PKG_CONFIG_PATH

SRC=/work/src
BUILD=/work/build
NAME="RawTherapee-${RT_VERSION}-win64-portable"
STAGE="/work/$NAME"
OUT="$NAME.zip"

chown_back() { chown -R "$HOST_UID:$HOST_GID" /work /dist 2>/dev/null || true; }
trap chown_back EXIT

echo "==> Syncing source tree (version $RT_VERSION)"
mkdir -p "$SRC"
rsync -a --delete \
    --exclude '/build/' --exclude '/.git/' --exclude '/packaging/work/' --exclude '/packaging/dist/' \
    --exclude '/ReleaseInfo.cmake' --exclude '__pycache__/' --exclude '*.pyc' \
    /repo/ "$SRC/"
cp /work/ReleaseInfo.cmake "$SRC/ReleaseInfo.cmake"

############################################################################
echo "==> LibRaw (vendored external/libraw) with autotools, --host=$T"
# external/LibRaw.cmake runs ./configure without --host, which cannot
# cross-compile, so the same sources are built here out of tree and installed
# into the MinGW sysroot; RawTherapee is then configured with
# WITH_SYSTEM_LIBRAW=ON and finds it through pkg-config (libraw_r.pc).
LR_SRC=/work/libraw-src
LR_BUILD=/work/libraw-build
mkdir -p "$LR_SRC" "$LR_BUILD"
rsync -a "$SRC/external/libraw/" "$LR_SRC/"      # no --delete: keeps the generated configure
if [[ ! -x "$LR_SRC/configure" || "$LR_SRC/configure.ac" -nt "$LR_SRC/configure" || "$LR_SRC/Makefile.am" -nt "$LR_SRC/configure" ]]; then
    (cd "$LR_SRC" && autoreconf --install --force >/dev/null)
fi
(
    cd "$LR_BUILD"
    if [[ ! -f config.status ]] || [[ "$LR_SRC/configure" -nt config.status ]]; then
        "$LR_SRC/configure" \
            --host="$T" --build="$(gcc -dumpmachine)" --prefix="$R" \
            --disable-examples --enable-openmp --disable-static --enable-shared \
            CC="$T-gcc" CXX="$T-g++" \
            CFLAGS="-O3 -DNDEBUG" \
            CXXFLAGS="-O3 -DNDEBUG -std=gnu++11 -Wno-error=unknown-pragmas -DCMS_NO_REGISTER_KEYWORD=1" \
            PKG_CONFIG="$T-pkg-config"
    fi
    # -no-undefined is a libtool flag: required to let libtool build a DLL.
    make -j"$JOBS" LDFLAGS="-no-undefined" >/dev/null
    make install >/dev/null
)
"$T-pkg-config" --modversion libraw_r | sed 's/^/    libraw_r /'
ls "$R"/bin/libraw_r-*.dll | sed 's/^/    /'

############################################################################
echo "==> Configuring RawTherapee (same options as .github/workflows/windows.yml)"
cmake -G Ninja -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DPKG_CONFIG_EXECUTABLE="/usr/bin/$T-pkg-config" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCACHE_NAME_SUFFIX="5-dev-portable" \
    -DPROC_TARGET_NUMBER=1 \
    -DLENSFUNDBDIR="share/lensfun" \
    -DWITH_SYSTEM_LIBRAW=ON \
    -DWITH_JXL=AUTO \
    -DBUILD_SHARED=OFF -DBUILD_SHARED_LIBS=OFF \
    -DOPTION_OMP=ON -DWITH_LTO=OFF -DWITH_PROF=OFF -DWITH_SAN=OFF \
    -DWITH_SYSTEM_KLT=OFF \
    -DBUNDLE_BASE_INSTALL_DIR="$STAGE" \
    -DFETCHCONTENT_SOURCE_DIR_FMT=/opt/src/fmt \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DCMAKE_C_FLAGS="-D_USE_MATH_DEFINES" \
    -DCMAKE_CXX_FLAGS="-D_USE_MATH_DEFINES"
# _USE_MATH_DEFINES: RawTherapee compiles with -std=c++17 (strict), under which
# mingw's <math.h> hides M_PI/M_SQRT2 that a few sources use unqualified.

echo "==> Building with $JOBS jobs"
cmake --build "$BUILD" -j "$JOBS"

echo "==> Installing into $STAGE"
rm -rf "$STAGE"
# UpdateInfo.cmake generates WindowsInnoSetup.iss only when run on a Windows
# host (script-mode WIN32); the install step wants it, so provide an empty
# one -- the installer is not built here and the file is removed below.
mkdir -p "$BUILD/rtdata" && : > "$BUILD/rtdata/WindowsInnoSetup.iss"
cmake --install "$BUILD" >/dev/null
rm -f "$STAGE"/*.iss
test -f "$STAGE/rawtherapee.exe" && test -f "$STAGE/rawtherapee-cli.exe"
test -f "$STAGE/segmentation/rt_segment_server.py" || { echo "segmentation backend script missing"; exit 1; }

############################################################################
echo "==> Lensfun database"
mkdir -p "$STAGE/share/lensfun"
if [[ -d /opt/lensfun-db/version_1 ]]; then
    cp /opt/lensfun-db/version_1/*.xml "$STAGE/share/lensfun/"
else
    cp "$R"/share/lensfun/version_1/*.xml "$STAGE/share/lensfun/"
fi

echo "==> GTK runtime: gdk-pixbuf loaders, GSettings schemas, icon themes, settings.ini"
mkdir -p "$STAGE/lib/gdk-pixbuf-2.0/2.10.0"
cp -R "$R/lib/gdk-pixbuf-2.0/2.10.0/loaders" "$STAGE/lib/gdk-pixbuf-2.0/2.10.0/"
cp "$R/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" "$STAGE/lib/gdk-pixbuf-2.0/2.10.0/"
mkdir -p "$STAGE/share/glib-2.0/schemas"
cp "$R/share/glib-2.0/schemas/gschemas.compiled" "$STAGE/share/glib-2.0/schemas/"
mkdir -p "$STAGE/share/icons/Adwaita/symbolic" "$STAGE/share/icons/hicolor"
for d in actions devices mimetypes places status ui; do
    [[ -d "$R/share/icons/Adwaita/symbolic/$d" ]] && cp -R "$R/share/icons/Adwaita/symbolic/$d" "$STAGE/share/icons/Adwaita/symbolic/"
done
cp "$R/share/icons/Adwaita/index.theme" "$STAGE/share/icons/Adwaita/"
cp "$R/share/icons/hicolor/index.theme" "$STAGE/share/icons/hicolor/"
mkdir -p "$STAGE/share/gtk-3.0"
printf '[Settings]\ngtk-button-images=1\n' > "$STAGE/share/gtk-3.0/settings.ini"
# GLib helpers: gspawn-* are required by Glib::spawn_* (used to start the AI
# segmentation backend), gdbus for the GTK D-Bus plumbing.
cp "$R"/bin/gspawn-win64-helper.exe "$R"/bin/gspawn-win64-helper-console.exe "$R"/bin/gdbus.exe "$STAGE/"

############################################################################
echo "==> Collecting DLLs (recursive import walk, like ldd/ntldd in upstream CI)"
declare -A SEEN
collect() {
    local f="$1" dep found
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        local key="${dep,,}"
        [[ -n "${SEEN[$key]:-}" ]] && continue
        SEEN[$key]=1
        found="$(find "$R/bin" -maxdepth 1 -iname "$dep" | head -n1)"
        if [[ -n "$found" ]]; then
            cp "$found" "$STAGE/"
            collect "$found"
        fi   # otherwise a Windows system DLL (kernel32, msvcrt, ...)
    done < <("$T-objdump" -p "$f" 2>/dev/null | awk '/DLL Name:/{print $3}')
}
for seed in "$STAGE"/*.exe "$STAGE"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll; do
    collect "$seed"
done
echo "    $(ls "$STAGE"/*.dll | wc -l) DLLs"

echo "==> Stripping"
"$T-strip" "$STAGE"/*.exe "$STAGE"/*.dll "$STAGE"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll 2>/dev/null || true

############################################################################
if [[ "${SKIP_TESTS:-0}" != "1" ]]; then
    echo "==> Smoke tests under Wine"
    export WINEPREFIX=/tmp/wineprefix WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
    export XDG_RUNTIME_DIR=/tmp/xdg-runtime; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
    wineboot --init >/dev/null 2>&1 || true
    echo "  - rawtherapee-cli.exe --version (no python anywhere)"
    (cd "$STAGE" && wine ./rawtherapee-cli.exe --version 2>/dev/null | tr -d '\r' > /tmp/cli.txt) || true
    cat /tmp/cli.txt
    grep -q "RawTherapee, version $RT_VERSION" /tmp/cli.txt || { echo "    FAIL: version string not printed"; exit 1; }

    echo "  - rawtherapee-cli.exe converting a PNG with the bundled default profile"
    python3 -I - /tmp/in.png <<'PY'
import struct, zlib, sys
w = h = 32
rows = b''
for y in range(h):
    rows += b'\x00' + b''.join(bytes([(x*8)&255, (y*8)&255, 128]) for x in range(w))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
    cp /tmp/in.png "$STAGE/in.png"
    (cd "$STAGE" && wine ./rawtherapee-cli.exe -o out.jpg -j90 -c in.png > /tmp/convert.log 2>&1) || { cat /tmp/convert.log; exit 1; }
    test -s "$STAGE/out.jpg" || { cat /tmp/convert.log; echo "    FAIL: no output image"; exit 1; }
    echo "    ok: $(stat -c %s "$STAGE/out.jpg") bytes"
    rm -f "$STAGE/in.png" "$STAGE/out.jpg"

    echo "  - rawtherapee.exe GUI under Xvfb + Wine, 20 s (best effort)"
    Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    (cd "$STAGE" && DISPLAY=:99 wine ./rawtherapee.exe > /tmp/gui.log 2>&1) &
    GUI_PID=$!
    sleep 20
    if kill -0 "$GUI_PID" 2>/dev/null; then
        echo "    ok: GUI still running after 20 s"
        kill "$GUI_PID" 2>/dev/null || true; wineserver -k 2>/dev/null || true
    else
        echo "    WARNING: GUI exited early under Wine (not necessarily a packaging problem). Log:"
        head -40 /tmp/gui.log
    fi
    kill "$XVFB_PID" 2>/dev/null || true
    rm -rf "$STAGE/mycache" "$STAGE/mysettings"   # created only if options are missing; keep the zip clean
fi

############################################################################
echo "==> Zipping"
rm -f "/dist/$OUT"
(cd /work && zip -q -r -X "/dist/$OUT" "$NAME")
cp -f "$BUILD/AboutThisBuild.txt" "/dist/${NAME}-AboutThisBuild.txt"
ls -l "/dist/$OUT"
echo "==> Done"
