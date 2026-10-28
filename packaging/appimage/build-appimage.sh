#!/usr/bin/env bash
# Build the portable Linux AppImage of the current working tree in Docker.
#
#   ./packaging/appimage/build-appimage.sh
#
# Output: packaging/dist/RawTherapee-<version>-x86_64.AppImage
# Env:    JOBS=N          parallel compile jobs (default: nproc)
#         REBUILD_IMAGE=1 rebuild the builder image (packaging/appimage/Dockerfile)
#         SKIP_TESTS=1    skip the in-container smoke tests
#         CLEAN=1         wipe the persistent build directory first
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

IMAGE="rawtherapee-appimage-builder:ubuntu22.04"
rt_ensure_image "$IMAGE" "$PKG_DIR/appimage"
if [[ "${CLEAN:-0}" == "1" ]]; then
    log "removing $WORK_DIR/appimage/build"
    rm -rf "$WORK_DIR/appimage/build"
fi
log "building AppImage, version $(rt_version)"
rt_run_build "$IMAGE" appimage /repo/packaging/appimage/build-inside.sh \
    "SKIP_TESTS=${SKIP_TESTS:-0}" "NO_STRIP=${NO_STRIP:-}"
log "done: $DIST_DIR/RawTherapee-$(rt_version)-x86_64.AppImage"
