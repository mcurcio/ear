#!/usr/bin/env bash
# Print the path of the JUCE API reference, (re)generating it first when it is missing or was
# built from another JUCE commit than the one CMakeLists.txt pins (juce_commit). When the
# fetched JUCE tree is missing or at another commit, configure the project first: CMake's
# FetchContent is the only thing that fetches JUCE.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
out="$repo/.cache/juce-api"
juce="$repo/.cache/deps/juce-src"  # FetchContent's source dir for JUCE (AGENTS.md)
commit="$(sed -n 's/^set(juce_commit \([0-9a-f]\{40\}\)).*/\1/p' "$repo/CMakeLists.txt")"
if [ -z "$commit" ]; then
  echo "juce-api: no set(juce_commit <sha>) line in $repo/CMakeLists.txt" >&2
  exit 1
fi

juce_at_pin() { [ "$(git -C "$juce" rev-parse HEAD 2>/dev/null)" = "$commit" ]; }

if ! grep -qx "juce_commit=$commit" "$out/MANIFEST" 2>/dev/null; then
  if ! juce_at_pin; then
    echo "juce-api: configuring the project to fetch JUCE $commit" >&2
    cmake -S "$repo" -B "$repo/out" >&2
    if ! juce_at_pin; then
      echo "juce-api: $juce is not JUCE $commit after configuring" >&2
      exit 1
    fi
  fi
  echo "juce-api: generating reference into $out" >&2
  "$here/generate.sh" "$juce"
fi
echo "$out"
