#!/usr/bin/env bash
# scripts/dev.sh <command> [args...]
#
# Runs a command in EAR's build image (docker/Dockerfile), in this checkout, as the calling
# user, and exits with the command's status. EAR builds and is tested only on Linux; this is
# how a Mac (or any Linux host without the packages) builds it, and CI runs its build the same
# way. For example:
#
#   scripts/dev.sh make test
#   scripts/dev.sh make test CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_BOOST=/path/to/boost-src
#
# Mounts: the main checkout (the parent of git's common directory) at the same path inside the
# container, so every worktree under it, its .cache/ and the absolute paths CMake records in
# out/ are the same inside and out. A worktree outside the main checkout is mounted too.
#
# Image: built from docker/Dockerfile on every run and tagged ear-dev (Docker's layer cache
# makes an unchanged Dockerfile a no-op), so the image always matches the checkout. CI builds
# the image itself (with its own cache) and names it in EAR_DEV_IMAGE, which skips the build.
#
# ccache: CCACHE_DIR is the main checkout's .cache/ccache (gitignored), shared by every
# worktree and cached by CI.
set -euo pipefail

if [[ $# -eq 0 ]]; then
	echo "usage: $0 <command> [args...]" >&2
	exit 2
fi

checkout=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
main_checkout=$(dirname "$(git -C "$checkout" rev-parse --path-format=absolute --git-common-dir)")

mounts=(--volume "$main_checkout:$main_checkout")
case "$checkout/" in
	"$main_checkout"/*) ;;
	*) mounts+=(--volume "$checkout:$checkout") ;;
esac

image=${EAR_DEV_IMAGE:-}
if [[ -z $image ]]; then
	image=ear-dev
	docker build --quiet --tag "$image" --file "$checkout/docker/Dockerfile" "$checkout" >/dev/null
fi

# An interactive terminal gets one inside too (for a shell: scripts/dev.sh bash).
tty=()
if [[ -t 0 && -t 1 ]]; then
	tty=(--interactive --tty)
fi

mkdir -p "$main_checkout/.cache/ccache"
exec docker run --rm ${tty[@]+"${tty[@]}"} "${mounts[@]}" \
	--user "$(id -u):$(id -g)" \
	--env HOME=/tmp \
	--env CCACHE_DIR="$main_checkout/.cache/ccache" \
	--workdir "$checkout" \
	"$image" "$@"
