#!/bin/sh
# Host unit test for the GIM decoder (no runtime build, no game run).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/test_gim_decode
g++ -std=c++17 -O1 -Wall -Wextra -o "$out" "$here/test_gim_decode.cpp" "$here/../runtime/gim_decode.cpp" "$here/../runtime/package_mods.cpp"
"$out"
