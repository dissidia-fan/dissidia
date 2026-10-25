#!/bin/sh
# Host test for roster.cpp's projblock= (projectiles blocked between two ticks of a motion, e.g. while Apocalypsis's
# arms circle Noctis): fake guest RAM, fake FUN_08807D4C / FUN_088A9DE8; no runtime build, no game run. Needs the
# generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_proj_block
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_proj_block.cpp"
"$out"
