#!/usr/bin/env bash
# scripts/cloud-dev.sh <command> [args...]
#
# scripts/dev.sh for a container whose only way out is an HTTPS proxy with its own certificate
# authority, as a Claude Code cloud session's is. There, plain scripts/dev.sh fails three ways,
# and this script gets past each before it hands the command to scripts/dev.sh:
#
# 1. No Docker daemon is running. This starts dockerd (its log: .cache/cloud-dev/dockerd.log) when
#    "docker info" fails, and waits for it.
# 2. The image's build cannot fetch over HTTPS: a build's steps do not see the session's proxy,
#    and git, pip and curl inside them do not trust its CA. This builds the dev target from a copy
#    of docker/Dockerfile (.cache/cloud-dev/Dockerfile, written afresh each run) in which every
#    stage trusts the CA, on the host's network with the proxy passed in, and tags it ear-dev.
#    Docker's layer cache makes an unchanged Dockerfile a no-op, as in scripts/dev.sh. The image
#    is for this container only: it carries the proxy's CA, so it is never pushed.
# 3. Configure cannot download mjansson/mdns's release archive (cmake/MdnsDeps.cmake): the proxy
#    refuses GitHub's archive downloads of a repository outside the session's own, though it lets
#    git clone it. This clones the pinned tag into .cache/deps-manual/mdns and, until out/'s cache
#    names that tree, passes CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_MDNS=<it> to the command
#    (the Makefile reads it; CMake keeps it in out/CMakeCache.txt, so later runs need nothing).
#    A clone is not checked against the pin's SHA-256, which is the archive's: the tag is trusted.
#    A CMAKE_ARGS given on make's own command line replaces this one, so name the tree there too.
#
# The command then runs as scripts/dev.sh runs it, on the host's network with the proxy in its
# environment, so configure's other fetches (Boost, knobly, the sendspin node's dependencies) and
# npm work. For example:
#
#   scripts/cloud-dev.sh make test
#   scripts/cloud-dev.sh make web-test
#   scripts/cloud-dev.sh out/src/core/ear_core_tests "[node_host]"
#
# It needs HTTPS_PROXY (or https_proxy) set and the proxy's CA bundle at EAR_PROXY_CA, by default
# /root/.ccr/ca-bundle.crt, where a Claude Code cloud session keeps it. Anywhere else, use
# scripts/dev.sh.
set -euo pipefail

if [[ $# -eq 0 ]]; then
	echo "usage: $0 <command> [args...]" >&2
	exit 2
fi

checkout=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
proxy=${HTTPS_PROXY:-${https_proxy:-}}
ca=${EAR_PROXY_CA:-/root/.ccr/ca-bundle.crt}
if [[ -z $proxy ]]; then
	echo "$0: HTTPS_PROXY is not set: this is for a container behind an HTTPS proxy; use scripts/dev.sh" >&2
	exit 2
fi
if [[ ! -r $ca ]]; then
	echo "$0: the proxy's CA bundle is not at $ca: set EAR_PROXY_CA to it" >&2
	exit 2
fi

work=$checkout/.cache/cloud-dev
mkdir -p "$work/ca"

# 1. The daemon.
if ! docker info >/dev/null 2>&1; then
	if ! command -v dockerd >/dev/null; then
		echo "$0: no Docker daemon is running and dockerd is not installed" >&2
		exit 2
	fi
	nohup dockerd >"$work/dockerd.log" 2>&1 &
	for _ in $(seq 60); do
		docker info >/dev/null 2>&1 && break
		sleep 1
	done
	if ! docker info >/dev/null 2>&1; then
		echo "$0: dockerd did not come up within 60 s: see $work/dockerd.log" >&2
		exit 2
	fi
fi

# 2. The image: every stage trusts the proxy's CA, through the variables git, pip, curl, Python
# and Node each read.
cp "$ca" "$work/ca/proxy-ca.crt"
awk '
	{ print }
	/^FROM / {
		print "COPY --from=proxy-ca proxy-ca.crt /proxy-ca.crt"
		print "ENV GIT_SSL_CAINFO=/proxy-ca.crt SSL_CERT_FILE=/proxy-ca.crt CURL_CA_BUNDLE=/proxy-ca.crt \\"
		print "\tPIP_CERT=/proxy-ca.crt REQUESTS_CA_BUNDLE=/proxy-ca.crt NODE_EXTRA_CA_CERTS=/proxy-ca.crt"
	}
' "$checkout/docker/Dockerfile" >"$work/Dockerfile"
if ! docker build --quiet --network host \
	--build-arg HTTPS_PROXY="$proxy" --build-arg https_proxy="$proxy" \
	--build-context proxy-ca="$work/ca" \
	--target dev --tag ear-dev --file "$work/Dockerfile" "$checkout" >"$work/image.log" 2>&1; then
	echo "$0: the dev image did not build: see $work/image.log" >&2
	exit 1
fi

# 3. mdns, by git at the tag cmake/MdnsDeps.cmake's URL names.
mdns=$checkout/.cache/deps-manual/mdns
if [[ ! -f $mdns/mdns.h ]]; then
	tag=$(sed -n 's|^set(mdns_url .*/refs/tags/\(.*\)\.tar\.gz)$|\1|p' "$checkout/cmake/MdnsDeps.cmake")
	if [[ -z $tag ]]; then
		echo "$0: cmake/MdnsDeps.cmake's mdns_url names no tag this script can read" >&2
		exit 1
	fi
	rm -rf "$mdns"
	mkdir -p "$(dirname "$mdns")"
	git clone --quiet --depth 1 --branch "$tag" -c advice.detachedHead=false \
		https://github.com/mjansson/mdns.git "$mdns"
fi
configure=()
if ! grep -qsx "FETCHCONTENT_SOURCE_DIR_MDNS:PATH=$mdns" "$checkout/out/CMakeCache.txt"; then
	configure=(--env "CMAKE_ARGS=-DFETCHCONTENT_SOURCE_DIR_MDNS=$mdns")
fi

EAR_DEV_IMAGE=ear-dev \
	EAR_DEV_DOCKER_ARGS="--network host --env HTTPS_PROXY=$proxy --env https_proxy=$proxy --env HTTP_PROXY=$proxy --env http_proxy=$proxy --env NO_PROXY=localhost,127.0.0.1 --env no_proxy=localhost,127.0.0.1 ${configure[*]}" \
	exec "$checkout/scripts/dev.sh" "$@"
