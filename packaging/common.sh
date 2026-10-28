# Shared helpers for the packaging scripts. Sourced, not executed.
#
# Everything runs in Docker containers; the host only needs git, docker and
# coreutils. The repository is bind-mounted READ-ONLY into the container at
# /repo and copied (rsync) into a persistent work directory, so uncommitted
# changes are included and rebuilds are incremental.

set -euo pipefail

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$PKG_DIR/.." && pwd)"
DIST_DIR="$PKG_DIR/dist"
WORK_DIR="$PKG_DIR/work"

DOCKER="${DOCKER:-docker}"
JOBS="${JOBS:-$(nproc)}"

log() { printf '\033[1;34m[packaging]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[packaging] error:\033[0m %s\n' "$*" >&2; exit 1; }

# Version string: `git describe --tags --always --dirty`, e.g.
# 5.13-32-gb2ee504c4-dirty. "-dirty" marks uncommitted changes to tracked files.
rt_version() {
    git -C "$REPO_DIR" describe --tags --always --dirty 2>/dev/null \
        || die "git describe failed (is $REPO_DIR a git checkout?)"
}

# Write a ReleaseInfo.cmake for the source copy inside the container. With this
# file present UpdateInfo.cmake does not need git (the container has no .git).
# Mirrors tools/generateReleaseInfo, plus the dirty marker in GIT_DESCRIBE.
rt_write_release_info() {
    local out="$1"
    local describe branch commit date since_tag since_branch numeric
    describe="$(rt_version)"
    branch="$(git -C "$REPO_DIR" symbolic-ref --short -q HEAD || true)"
    commit="$(git -C "$REPO_DIR" rev-parse --short --verify HEAD)"
    date="$(git -C "$REPO_DIR" show -s --format=%cd --date=format:%Y-%m-%d)"
    # shellcheck disable=SC2046
    since_tag="$(git -C "$REPO_DIR" rev-list --count HEAD --not $(git -C "$REPO_DIR" tag --merged HEAD))"
    since_branch="$(git -C "$REPO_DIR" rev-list --count HEAD --not --tags)"
    [[ -n "$branch" ]] || branch="$describe"
    if [[ -z "$since_tag" ]]; then
        numeric="0.0.0"
    else
        numeric="${describe%%-*}.${since_tag}"
    fi
    cat > "$out" <<EOF
set(GIT_DESCRIBE $describe)
set(GIT_BRANCH $branch)
set(GIT_COMMIT $commit)
set(GIT_COMMIT_DATE $date)
set(GIT_COMMITS_SINCE_TAG $since_tag)
set(GIT_COMMITS_SINCE_BRANCH $since_branch)
set(GIT_VERSION_NUMERIC_BS $numeric)
EOF
}

# Build (or reuse) a builder image. $1 = image tag, $2 = directory with the
# Dockerfile. Set REBUILD_IMAGE=1 to force a rebuild, PULL=1 to refresh the base.
rt_ensure_image() {
    local tag="$1" dir="$2"
    if [[ "${REBUILD_IMAGE:-0}" != "1" ]] && "$DOCKER" image inspect "$tag" >/dev/null 2>&1; then
        log "using existing image $tag (REBUILD_IMAGE=1 to rebuild)"
        return
    fi
    log "building image $tag from $dir/Dockerfile (one-time, needs network)"
    "$DOCKER" build ${PULL:+--pull} ${DOCKER_BUILD_ARGS:-} -t "$tag" -f "$dir/Dockerfile" "$dir"
}

# Run the in-container build script. $1 = image, $2 = work subdir name,
# $3 = path of the script inside the container, remaining args = env KEY=VALUE.
# Runs as root inside the container (needed to install the cross-compiled
# LibRaw into the MinGW sysroot); the build script chowns its outputs back to
# the invoking user.
rt_run_build() {
    local image="$1" name="$2" script="$3"; shift 3
    local work="$WORK_DIR/$name"
    mkdir -p "$work" "$DIST_DIR"
    rt_write_release_info "$work/ReleaseInfo.cmake"
    local envargs=()
    for kv in "$@"; do envargs+=(-e "$kv"); done
    "$DOCKER" run --rm --security-opt label=disable ${DOCKER_RUN_ARGS:-} \
        ${RT_INTERACTIVE:+-it} \
        -v "$REPO_DIR:/repo:ro" \
        -v "$work:/work" \
        -v "$DIST_DIR:/dist" \
        -e "RT_VERSION=$(rt_version)" \
        -e "JOBS=$JOBS" \
        -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" \
        "${envargs[@]}" \
        "$image" bash "$script"
}
