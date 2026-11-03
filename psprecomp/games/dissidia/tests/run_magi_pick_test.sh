#!/bin/sh
# Host test for roster.cpp's magipick= hook (a fired magi becomes one of a set): fake guest RAM and a fake FUN_08AD0978 (magi
# emitter create, test_hit_action's fakes); no runtime build, no game run. Needs
# the generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output). ROSTER_CPP overrides the roster.cpp under test.
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
src=${ROSTER_CPP:-$here/../runtime/roster.cpp}
out=${TMPDIR:-/tmp}/test_magi_pick
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -DROSTER_CPP="\"$src\"" -o "$out" "$here/test_magi_pick.cpp"
"$out"
