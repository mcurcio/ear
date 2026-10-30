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
#    engine.conf and the built editor (web/index.html) are in /opt/ear; the recordings folder
#    /var/lib/ear/recordings is writable; then ear --help, which
#    also proves its shared libraries load. ear carries CAP_SYS_NICE as a permitted-only file
#    capability, so it starts where the capability is not allowed (a marked-effective one would
#    fail here with EPERM).
# 2. With --cap-add SYS_NICE: ear --help again.
# Then ear itself, twice, on a sample graph (white noise into a file node writing rec.wav in the
# image's recordings folder, /var/lib/ear/recordings, which must be the ear user's),
# without and with --cap-add SYS_NICE: once it prints "control listening on", GET / must return
# the built editor's index.html and graph.get must answer (scripts/rpc-call.mjs, run with the dev
# image's Node in a container sharing ear's network). Without SYS_NICE graph.get says
# "realtime": false and ear still runs; with it, "realtime": true (the binary's permitted
# capability, made effective by Engine::create). Each run is stopped with SIGTERM and must exit 0
# and leave rec.wav in the recordings folder.
#
# The dev image (docker/Dockerfile's dev target) is EAR_DEV_IMAGE if set (as scripts/dev.sh), or
# is built and loaded here as ear-dev from the same Dockerfile (its layers are already cached
# where the run image was just built).
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
	if [ ! -w /var/lib/ear/recordings ]; then
		echo "image-smoke: /var/lib/ear/recordings is not writable by uid 10001" >&2
		exit 1
	fi
	exec /opt/ear/ear --help
' >/dev/null

echo "image-smoke: $image with --cap-add SYS_NICE: ear --help"
docker run --rm --cap-add SYS_NICE "$image" --help >/dev/null

checkout=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
dev_image=${EAR_DEV_IMAGE:-}
if [[ -z $dev_image ]]; then
	dev_image=ear-dev
	docker buildx build --quiet --load --target dev --tag "$dev_image" --file "$checkout/docker/Dockerfile" "$checkout" >/dev/null
fi

scratch=$(mktemp -d)
containers=()
cleanup() {
	# --volumes: each run's anonymous recordings volume goes with its container.
	if [[ ${#containers[@]} -gt 0 ]]; then docker rm --force --volumes "${containers[@]}" >/dev/null 2>&1 || true; fi
	rm -rf "$scratch"
}
trap cleanup EXIT

cat >"$scratch/ear.json" <<'JSON'
{
  "graphFrequency": 48000,
  "blockSize": 256,
  "nodes": [
    {"name": "noise", "type": "white"},
    {"name": "rec", "type": "file", "settings": {"path": "rec.wav"}}
  ],
  "routes": [{"from": {"node": "noise", "port": "MONO"}, "to": {"node": "rec", "port": "FL"}}]
}
JSON
chmod 644 "$scratch/ear.json"
expected_index=$(docker run --rm --entrypoint cat "$image" /opt/ear/web/index.html)

# run_ear <realtime: true|false> [docker run options...]
run_ear() {
	local realtime=$1
	shift
	local container
	container=$(docker run --detach "$@" --volume "$scratch/ear.json:/etc/ear/ear.json:ro" "$image")
	containers+=("$container")

	# Waits for the line (an event); the read deadline only fails a start that never comes.
	local line listening=
	while IFS= read -r -t 30 line; do
		echo "  ear: $line"
		if [[ $line == "control listening on "* ]]; then
			listening=yes
			break
		fi
	done < <(docker logs --follow "$container" 2>&1)
	if [[ -z $listening ]]; then
		echo "image-smoke: ear never said it was listening" >&2
		docker logs "$container" >&2 || true
		exit 1
	fi

	local index
	index=$(docker run --rm --network "container:$container" "$dev_image" \
		node --input-type=module --eval \
		'const r = await fetch("http://127.0.0.1:7777/"); if (!r.ok) { console.error(r.status); process.exit(1); } process.stdout.write(await r.text());')
	if [[ $index != "$expected_index" ]]; then
		echo "image-smoke: GET / did not return /opt/ear/web/index.html" >&2
		exit 1
	fi

	local graph
	graph=$(docker run --rm --network "container:$container" --volume "$checkout/scripts:/scripts:ro" "$dev_image" \
		node /scripts/rpc-call.mjs ws://127.0.0.1:7777/rpc graph.get)
	echo "  graph.get: $graph"
	if [[ $graph != *"\"realtime\":$realtime"* ]]; then
		echo "image-smoke: graph.get does not say \"realtime\":$realtime" >&2
		exit 1
	fi

	docker stop --time 30 "$container" >/dev/null
	local status
	status=$(docker inspect --format '{{.State.ExitCode}}' "$container")
	if [[ $status != 0 ]]; then
		echo "image-smoke: ear exited with $status after SIGTERM" >&2
		docker logs "$container" >&2 || true
		exit 1
	fi

	rm -f "$scratch/rec.wav"
	if ! docker cp "$container:/var/lib/ear/recordings/rec.wav" "$scratch/rec.wav" >/dev/null || [[ ! -s $scratch/rec.wav ]]; then
		echo "image-smoke: the file node left no rec.wav in /var/lib/ear/recordings" >&2
		docker logs "$container" >&2 || true
		exit 1
	fi
}

echo "image-smoke: $image runs a graph without SYS_NICE: GET /, graph.get says not realtime"
run_ear false

echo "image-smoke: $image runs a graph with --cap-add SYS_NICE: graph.get says realtime"
run_ear true --cap-add SYS_NICE

echo "image-smoke: $image passed"
