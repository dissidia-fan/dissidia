#!/bin/sh
# Build and run the VFPU atan2 host test without cmake / the full runtime.
# Usage: runtime/tests/run_vfpu_atan2.sh [path/to/recomp.h dir]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/.."
inc="${1:-${PSPRECOMP_OUTPUT_DIR:-/home/claude/recomp/psprecomp_work/output}/include}"
out="${TMPDIR:-/tmp}/test_vfpu_atan2"
vend="$rt/src/vfpu_ppsspp"
g++ -std=c++17 -O1 -w -fno-strict-aliasing -I"$rt/include" -I"$inc" -I"$vend" -o "$out" \
  "$here/test_vfpu_atan2.cpp" \
  "$rt"/src/psp_vfpu_core.cpp "$rt"/src/psp_vfpu_ops.cpp "$vend"/psp_vfpu_support.cpp \
  "$vend"/Core/MIPS/InterpreterVFPU.cpp "$vend"/Core/MIPS/MIPSVFPUUtils.cpp
"$out"
