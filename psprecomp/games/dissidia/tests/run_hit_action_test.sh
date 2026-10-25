#!/bin/sh
# Host test for roster.cpp's hitact= hook (an on-hit magi child starts an action): fake guest RAM, fake FUN_08AD0978 (magi
# emitter create), bone pass, selector and start action (test_chain_action's fakes); no runtime build, no game run. Needs
# the generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output). ROSTER_CPP overrides the roster.cpp under test.
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
src=${ROSTER_CPP:-$here/../runtime/roster.cpp}
out=${TMPDIR:-/tmp}/test_hit_action
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -DROSTER_CPP="\"$src\"" -o "$out" "$here/test_hit_action.cpp"
"$out"
