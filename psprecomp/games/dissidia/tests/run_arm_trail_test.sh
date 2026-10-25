#!/bin/sh
# Host test for roster.cpp's weapon trails on floating arms (roster.txt armtrail=):
# fake guest RAM, fake FUN_0880EA20 / FUN_0884E660 originals; no runtime build, no game run. Needs the
# generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_arm_trail
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_arm_trail.cpp"
"$out"
