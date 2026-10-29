#!/usr/bin/env bash
# Generate the machine-readable JUCE API reference from the headers of a JUCE source tree.
# Usage: generate.sh <JUCE source tree>  (ensure.sh passes the one CMake fetched)
# Output: .cache/juce-api/ (gitignored). Needs Docker and python3.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
juce="$(cd "${1:?usage: generate.sh <JUCE source tree>}" && pwd)"
out="$repo/.cache/juce-api"
image="ear-juce-doxygen:bookworm"

docker build -q -t "$image" "$here" >/dev/null

rm -rf "$out"
mkdir -p "$out"
docker run --rm \
  -v "$juce/modules:/work/juce/modules:ro" \
  -v "$juce/docs/doxygen:/work/juce/docs/doxygen:ro" \
  -v "$here/Doxyfile:/overlay/Doxyfile:ro" \
  -v "$out:/out" \
  -w /work/juce/docs/doxygen \
  "$image" doxygen /overlay/Doxyfile 2>"$out/doxygen.log"

python3 "$here/convert.py" "$out/xml" "$out" --repo "$repo" --juce "$juce"
rm -rf "$out/xml"
