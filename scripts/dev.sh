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
#   scripts/dev.sh make web-test
#   EAR_DEV_PUBLISH=5173:5173 scripts/dev.sh bash -c 'cd web && npm run dev -- --host 0.0.0.0'
#
# Mounts: the main checkout (the parent of git's common directory) at the same path inside the
# container, so every worktree under it, its .cache/ and the absolute paths CMake records in
# out/ are the same inside and out. A worktree outside the main checkout is mounted too.
#
# Image: docker/Dockerfile's dev target, built on every run and tagged ear-dev (Docker's layer cache
# makes an unchanged Dockerfile a no-op), so the image always matches the checkout. CI builds
# the image itself (with its own cache) and names it in EAR_DEV_IMAGE, which skips the build.
#
# ccache: CCACHE_DIR is the main checkout's .cache/ccache (gitignored), shared by every
# worktree and cached by CI. npm's cache (npm_config_cache) is the main checkout's .cache/npm
# the same way.
#
# Ports: EAR_DEV_PUBLISH, when set, is passed to docker run as --publish (for example 5173:5173
# for the editor's dev server, or 7777:7777 for ear's control port).
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
	docker build --quiet --target dev --tag "$image" --file "$checkout/docker/Dockerfile" "$checkout" >/dev/null
fi

# An interactive terminal gets one inside too (for a shell: scripts/dev.sh bash).
tty=()
if [[ -t 0 && -t 1 ]]; then
	tty=(--interactive --tty)
fi

publish=()
if [[ -n ${EAR_DEV_PUBLISH:-} ]]; then
	publish=(--publish "$EAR_DEV_PUBLISH")
fi

mkdir -p "$main_checkout/.cache/ccache" "$main_checkout/.cache/npm"
exec docker run --rm ${tty[@]+"${tty[@]}"} ${publish[@]+"${publish[@]}"} "${mounts[@]}" \
	--user "$(id -u):$(id -g)" \
	--env HOME=/tmp \
	--env CCACHE_DIR="$main_checkout/.cache/ccache" \
	--env npm_config_cache="$main_checkout/.cache/npm" \
	--workdir "$checkout" \
	"$image" "$@"
