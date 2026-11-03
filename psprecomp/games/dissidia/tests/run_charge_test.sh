#!/bin/sh
# Host test for roster.cpp's charge= hook (hold to charge): fake guest RAM, a fake FUN_08B30B54
# (HP button held) and test_magi_pick's fakes; no runtime build, no game run. Needs
# the generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output). ROSTER_CPP overrides the roster.cpp under test.
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
src=${ROSTER_CPP:-$here/../runtime/roster.cpp}
out=${TMPDIR:-/tmp}/test_charge
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -DROSTER_CPP="\"$src\"" -o "$out" "$here/test_charge.cpp"
"$out"
