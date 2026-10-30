#!/usr/bin/env bash
# scripts/image-smoke.sh <image>
#
# Checks a run image (docker/Dockerfile's run target) the way it ships, and exits non-zero on
# the first check that fails. CI runs it on the image it builds (.github/workflows/test.yml):
#
#   docker build --target run --tag ear:ci --file docker/Dockerfile . && scripts/image-smoke.sh ear:ci
#
# Two container starts, both as the image's own user (no --user):
# 1. Without SYS_NICE: the user is uid 10001; no compiler or build tool is on the PATH; ear's
#    engine.conf and the built editor (web/index.html) are in /opt/ear; then ear --help, which
#    also proves its shared libraries load. ear carries CAP_SYS_NICE as a permitted-only file
#    capability, so it starts where the capability is not allowed (a marked-effective one would
#    fail here with EPERM).
# 2. With --cap-add SYS_NICE: ear --help again.
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "usage: $0 <image>" >&2
	exit 2
fi
image=$1

echo "image-smoke: $image without SYS_NICE: user, no compiler, files, ear --help"
docker run --rm --entrypoint /bin/sh "$image" -euc '
	uid=$(id -u)
	if [ "$uid" != 10001 ]; then
		echo "image-smoke: runs as uid $uid, not 10001" >&2
		exit 1
	fi
	for tool in cc c++ g++ gcc cmake; do
		if found=$(command -v "$tool"); then
			echo "image-smoke: the run image has a compiler or build tool: $found" >&2
			exit 1
		fi
	done
	for file in /opt/ear/engine.conf /opt/ear/web/index.html; do
		if [ ! -f "$file" ]; then
			echo "image-smoke: $file is missing" >&2
			exit 1
		fi
	done
	exec /opt/ear/ear --help
' >/dev/null

echo "image-smoke: $image with --cap-add SYS_NICE: ear --help"
docker run --rm --cap-add SYS_NICE "$image" --help >/dev/null

echo "image-smoke: $image passed"
