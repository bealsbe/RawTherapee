#!/usr/bin/env bash
# Cross-compile the portable Windows x86_64 build of the current working tree
# in Docker (Fedora + MinGW-w64) and zip it.
#
#   ./packaging/windows/build-windows.sh
#
# Output: packaging/dist/RawTherapee-<version>-win64-portable.zip
# Env:    JOBS=N          parallel compile jobs (default: nproc)
#         REBUILD_IMAGE=1 rebuild the builder image (packaging/windows/Dockerfile)
#         SKIP_TESTS=1    skip the Wine smoke tests
#         CLEAN=1         wipe the persistent build directories first
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

IMAGE="rawtherapee-windows-builder:fedora44"
rt_ensure_image "$IMAGE" "$PKG_DIR/windows"
if [[ "${CLEAN:-0}" == "1" ]]; then
    log "removing $WORK_DIR/windows/{build,libraw-build}"
    rm -rf "$WORK_DIR/windows/build" "$WORK_DIR/windows/libraw-build"
fi
log "building portable Windows zip, version $(rt_version)"
rt_run_build "$IMAGE" windows /repo/packaging/windows/build-inside.sh \
    "SKIP_TESTS=${SKIP_TESTS:-0}"
log "done: $DIST_DIR/RawTherapee-$(rt_version)-win64-portable.zip"
