#!/bin/sh
# Build and run the savedata crypto host test (g++ only, no cmake / runtime).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/test_savedata_crypto"
g++ -std=c++17 -O1 -Wall -Wextra -o "$out" \
  "$here/test_savedata_crypto.cpp" "$here/../src/hle/psp_savedata_crypto.cpp"
"$out"
