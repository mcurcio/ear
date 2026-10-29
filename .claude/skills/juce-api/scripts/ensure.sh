#!/usr/bin/env bash
# Print the path of the JUCE API reference, (re)generating it first when it is
# missing or was built from a different juce/modules tree than the one checked in.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
out="$repo/.cache/juce-api"
want="juce_modules_tree=$(git -C "$repo" rev-parse HEAD:juce/modules)"

if ! grep -qx "$want" "$out/MANIFEST" 2>/dev/null; then
  echo "juce-api: generating reference into $out" >&2
  "$here/generate.sh"
fi
echo "$out"
