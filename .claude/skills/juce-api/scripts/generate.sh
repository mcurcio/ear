#!/usr/bin/env bash
# Generate the machine-readable JUCE API reference from the vendored headers.
# Output: .cache/juce-api/ (gitignored). Needs Docker and python3.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
out="$repo/.cache/juce-api"
image="ear-juce-doxygen:bookworm"

docker build -q -t "$image" "$here" >/dev/null

rm -rf "$out"
mkdir -p "$out"
docker run --rm \
  -v "$repo/juce/modules:/work/juce/modules:ro" \
  -v "$repo/juce/docs/doxygen:/work/juce/docs/doxygen:ro" \
  -v "$here/Doxyfile:/overlay/Doxyfile:ro" \
  -v "$out:/out" \
  -w /work/juce/docs/doxygen \
  "$image" doxygen /overlay/Doxyfile 2>"$out/doxygen.log"

python3 "$here/convert.py" "$out/xml" "$out" --repo "$repo"
rm -rf "$out/xml"
