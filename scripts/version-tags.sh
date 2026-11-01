#!/usr/bin/env bash
# scripts/version-tags.sh [image]
#
# Prints one image reference per release tag (v<version>) on the checked-out commit, as
# <image>:<version> (default image ghcr.io/mcurcio/ear), one per line, and nothing when the
# commit has none. .github/workflows/test.yml's publish job pushes the image it built under
# these.
#
# The tags are read from git: the publish job checks out the release's tag (on its
# CHANGELOG.md commit) when the release job made one, else the merged commit, with tags
# fetched. A rerun of a failed publish keeps the release job's output, so it checks out the
# same tag and still pushes the version.
set -euo pipefail

image=${1:-ghcr.io/mcurcio/ear}

git tag --points-at HEAD --list 'v*' | while read -r tag; do
	echo "$image:${tag#v}"
done
