#!/usr/bin/env bash
# Runs INSIDE the rawtherapee-appimage-builder container (see build-appimage.sh).
#
# Mounts:  /repo  the git checkout, read-only (uncommitted changes included)
#          /work  persistent scratch: source copy + CMake build dir (incremental)
#          /dist  where the finished AppImage goes
# Env:     RT_VERSION, JOBS, HOST_UID, HOST_GID, SKIP_TESTS (optional)
set -euo pipefail

SRC=/work/src
BUILD=/work/build
APPDIR=$BUILD/AppDir
OUT="RawTherapee-${RT_VERSION}-x86_64.AppImage"

chown_back() { chown -R "$HOST_UID:$HOST_GID" /work /dist 2>/dev/null || true; }
trap chown_back EXIT

echo "==> Syncing source tree (version $RT_VERSION)"
mkdir -p "$SRC"
rsync -a --delete \
    --exclude '/build/' --exclude '/.git/' --exclude '/packaging/work/' --exclude '/packaging/dist/' \
    --exclude '/ReleaseInfo.cmake' --exclude '__pycache__/' --exclude '*.pyc' \
    /repo/ "$SRC/"
cp /work/ReleaseInfo.cmake "$SRC/ReleaseInfo.cmake"

echo "==> Configuring (same options as .github/workflows/appimage.yml)"
cmake -G Ninja -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCACHE_NAME_SUFFIX="5-dev-AppImage" \
    -DPROC_TARGET_NUMBER=1 \
    -DBUILD_BUNDLE=ON \
    -DBUNDLE_BASE_INSTALL_DIR=/ \
    -DOPTION_OMP=ON \
    -DWITH_LTO=OFF -DWITH_PROF=OFF -DWITH_SAN=OFF \
    -DWITH_SYSTEM_KLT=OFF -DWITH_SYSTEM_LIBRAW=OFF \
    -DWITH_JXL=ON \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DLENSFUNDBDIR="../share/lensfun/version_1" \
    -DFETCHCONTENT_SOURCE_DIR_FMT=/opt/src/fmt \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON

echo "==> Building with $JOBS jobs"
cmake --build "$BUILD" -j "$JOBS"

echo "==> Installing into AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR/usr/bin" cmake --install "$BUILD"
# BINDIR=DATADIR=. : everything lands in usr/bin; the freedesktop bits
# (applications/, icons/hicolor, metainfo, man) are under usr/bin/share.
mv "$APPDIR/usr/bin/share" "$APPDIR/usr/share"
rm -f "$APPDIR"/usr/bin/*.iss

echo "==> Lensfun database"
mkdir -p "$APPDIR/usr/share/lensfun"
if [[ -d /var/lib/lensfun-updates/version_1 ]]; then
    cp -R /var/lib/lensfun-updates/version_1 "$APPDIR/usr/share/lensfun/"
else
    cp -R /usr/share/lensfun/version_1 "$APPDIR/usr/share/lensfun/"
fi

echo "==> Adwaita icon theme (partial, as upstream CI)"
mkdir -p "$APPDIR/usr/bin/icons/Adwaita"
for dir in actions devices mimetypes places status ui; do
    while IFS= read -r found; do
        rel="$(echo "$found" | awk -F/ '{print $(NF-1)"/"$NF}')"
        mkdir -p "$APPDIR/usr/bin/icons/Adwaita/$rel"
        cp -R "$found/." "$APPDIR/usr/bin/icons/Adwaita/$rel"
    done < <(find /usr/share/icons/Adwaita -name "$dir" -type d)
done
cp /usr/share/icons/Adwaita/index.theme "$APPDIR/usr/bin/icons/Adwaita/"

test -f "$APPDIR/usr/bin/segmentation/rt_segment_server.py" || { echo "segmentation backend script missing from AppDir"; exit 1; }
test -f "$APPDIR/usr/share/applications/rawtherapee.desktop" || { echo "desktop file missing"; exit 1; }

echo "==> Packaging with linuxdeploy (+gtk plugin)"
cd "$BUILD"
rm -f ./*.AppImage
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/usr/local/lib"
export PATH="/opt/linuxdeploy:$PATH"     # linuxdeploy finds linuxdeploy-plugin-gtk.sh on PATH
export OUTPUT="$OUT"
export APPIMAGE_EXTRACT_AND_RUN=1         # no FUSE inside Docker
export NO_STRIP="${NO_STRIP:-}"
/opt/linuxdeploy/linuxdeploy-x86_64.AppImage \
    --appdir "$APPDIR" \
    --plugin gtk \
    --output appimage
test -f "$OUT" || { echo "linuxdeploy did not produce $OUT"; exit 1; }
chmod +x "$OUT"

if [[ "${SKIP_TESTS:-0}" != "1" ]]; then
    echo "==> Smoke tests"
    T=$(mktemp -d)
    cd "$T"
    "$BUILD/$OUT" --appimage-extract >/dev/null
    ROOT="$T/squashfs-root"
    # A PATH with basic shell tools but NO python at all: the AI backend must
    # simply be unavailable and RawTherapee must still run.
    mkdir -p "$T/bin"
    for tool in bash sh env dirname basename readlink sed grep awk cat ls cp mv rm mkdir tr head tail sort uniq find uname id which xdg-open; do
        p="$(command -v "$tool" 2>/dev/null || true)"; [[ -n "$p" ]] && ln -sf "$p" "$T/bin/$tool"
    done
    NOPY_PATH="$T/bin"
    ! env PATH="$NOPY_PATH" command -v python3 >/dev/null 2>&1 || { echo "python3 leaked into test PATH"; exit 1; }
    echo "  - rawtherapee-cli --version (no python on PATH)"
    # (rawtherapee-cli exits non-zero after printing its version, hence || true)
    env -i HOME="$T" PATH="$NOPY_PATH" "$ROOT/usr/bin/rawtherapee-cli" --version > "$T/cli.txt" 2>&1 || true
    cat "$T/cli.txt"
    grep -q "RawTherapee, version $RT_VERSION" "$T/cli.txt" || { echo "version string mismatch"; exit 1; }

    echo "  - rawtherapee-cli converting a PNG with the bundled default profile"
    python3 - "$T/in.png" <<'PY'
import struct, zlib, sys
w = h = 32
rows = b''
for y in range(h):
    rows += b'\x00' + b''.join(bytes([(x*8)&255, (y*8)&255, 128]) for x in range(w))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
    env -i HOME="$T" PATH="$NOPY_PATH" "$ROOT/usr/bin/rawtherapee-cli" -o "$T/out.jpg" -j90 -c "$T/in.png" > "$T/convert.log" 2>&1 || { cat "$T/convert.log"; exit 1; }
    test -s "$T/out.jpg" || { cat "$T/convert.log"; echo "conversion produced no output"; exit 1; }
    echo "    ok: $(stat -c %s "$T/out.jpg") bytes"

    echo "  - GUI launch via AppRun under Xvfb, 12 s, no python on PATH"
    Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    (cd "$T" && env -i HOME="$T" DISPLAY=:99 PATH="$NOPY_PATH" "$ROOT/AppRun" > "$T/gui.log" 2>&1) &
    GUI_PID=$!
    sleep 12
    if kill -0 "$GUI_PID" 2>/dev/null; then
        echo "    ok: GUI still running after 12 s"
        kill "$GUI_PID" 2>/dev/null || true
    else
        echo "    FAIL: GUI exited early. Log:"; cat "$T/gui.log"; kill "$XVFB_PID"; exit 1
    fi
    kill "$XVFB_PID" 2>/dev/null || true
    grep -i -E 'segment|python' "$T/gui.log" || true
    rm -rf "$T"
fi

echo "==> Done"
mv -f "$BUILD/$OUT" "/dist/$OUT"
cp -f "$BUILD/AboutThisBuild.txt" "/dist/${OUT%.AppImage}-AboutThisBuild.txt"
ls -l "/dist/$OUT"
