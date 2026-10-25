#!/bin/sh
# Host unit test for the loose-file mod layer (no runtime build, no game run).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/test_package_mods
g++ -std=c++17 -O1 -Wall -Wextra -o "$out" "$here/test_package_mods.cpp" "$here/../runtime/package_mods.cpp"
cd "$here/.."
"$out" "$@"
g++ -std=c++17 -O1 -I"$here/../../../runtime/include" -o "${out}_hooks" "$here/test_hooks_io.cpp" "$here/../runtime/package_mods.cpp"
"${out}_hooks"
