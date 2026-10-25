#!/bin/sh
# Host test for roster.cpp's EX Mode hooks (roster.txt exranged= / armiger=): fake guest RAM, fake originals, no runtime
# build, no game run. Needs the generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_exmode
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_exmode.cpp"
"$out"
