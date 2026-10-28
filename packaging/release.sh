#!/usr/bin/env bash
# Front door of the packaging pipeline.
#
#   ./packaging/release.sh appimage   -> packaging/dist/RawTherapee-<ver>-x86_64.AppImage
#   ./packaging/release.sh windows    -> packaging/dist/RawTherapee-<ver>-win64-portable.zip
#   ./packaging/release.sh all        -> both
#   ./packaging/release.sh version    -> print <ver>
#   ./packaging/release.sh clean      -> remove packaging/work (build caches) and dist
#
# <ver> = `git describe --tags --always --dirty` of the current checkout, so an
# uncommitted tree yields e.g. 5.13-32-gb2ee504c4-dirty. See packaging/README.md.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() { sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

cmd="${1:-}"
case "$cmd" in
    appimage) bash "$PKG_DIR/appimage/build-appimage.sh" ;;
    windows)  bash "$PKG_DIR/windows/build-windows.sh" ;;
    all)
        bash "$PKG_DIR/appimage/build-appimage.sh"
        bash "$PKG_DIR/windows/build-windows.sh"
        ;;
    version)  rt_version ;;
    clean)    rm -rf "$WORK_DIR" "$DIST_DIR"; log "removed $WORK_DIR and $DIST_DIR" ;;
    -h|--help|help|"") usage 0 ;;
    *) echo "unknown target: $cmd" >&2; usage 1 ;;
esac
if [[ "$cmd" == "appimage" || "$cmd" == "windows" || "$cmd" == "all" ]]; then
    log "artifacts in $DIST_DIR:"
    ls -lh "$DIST_DIR" | sed 's/^/    /'
fi
