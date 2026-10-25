#!/bin/sh
# Host test for roster.cpp (ablt relocation, extra characters' save records, save sidecars): fake guest RAM and savedata
# stubs, no runtime build, no game run, no game data. Needs the generated recomp.h (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_roster_hooks
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_roster_hooks.cpp"
"$out"
