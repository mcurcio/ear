#!/usr/bin/env bash
# scripts/version-tags.sh [image]
#
# Prints one image reference per release tag (v<version>) on the checked-out commit, as
# <image>:<version> (default image ghcr.io/mcurcio/ear), one per line, and nothing when the
# commit has none. .github/workflows/image.yml pushes the image it built under these.
#
# The tags are read from git, not from release-please's "release created" output, so a rerun
# of a failed or interrupted publish run still pushes the version: release-please made the tag
# on the first run and makes no release on the rerun, but the tag is still on the commit. The
# workflow fetches tags before it runs this.
set -euo pipefail

image=${1:-ghcr.io/mcurcio/ear}

git tag --points-at HEAD --list 'v*' | while read -r tag; do
	echo "$image:${tag#v}"
done
