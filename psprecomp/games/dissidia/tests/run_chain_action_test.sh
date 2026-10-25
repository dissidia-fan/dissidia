#!/bin/sh
# Host test for roster.cpp's chain-window action (roster.txt chainact=): fake guest RAM, fake FUN_08B308D8 (op 0x0467's
# chain entry), bone pass, selector and start action; no runtime build, no game run. Needs the generated recomp.h
# (PSPRECOMP_OUTPUT_DIR, default work/output).
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=${PSPRECOMP_OUTPUT_DIR:-$here/../../../work/output}
out=${TMPDIR:-/tmp}/test_chain_action
g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$here/../../../runtime/include" -I"$gen/include" -I"$here/../runtime" \
    -o "$out" "$here/test_chain_action.cpp"
"$out"
