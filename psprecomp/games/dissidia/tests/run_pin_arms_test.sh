#!/bin/sh
# Host test for roster.cpp's arms riding magi with several arm sets and spin (roster.txt apocarms= repeated, pinspin=):
# fake guest RAM, fake FUN_08AD20BC / FUN_0880EA20 originals, fake bullets; no runtime build, no game run. Needs the
# generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_pin_arms
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_pin_arms.cpp"
"$out"
