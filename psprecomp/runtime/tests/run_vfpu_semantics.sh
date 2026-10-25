#!/bin/sh
# Differential VFPU semantics test: runtime vfpu_* translations vs PPSSPP's interpreter.
# Needs a PPSSPP checkout (PPSSPP_DIR, default /home/claude/recomp/src/ppsspp); skips (exit 0) if absent.
# Host-only: g++ against the VFPU core, no cmake / SDL / GL.
# Usage: runtime/tests/run_vfpu_semantics.sh [test options: --iters N --op NAME --strict-nan --wild-fails --seed S]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/.."
pp="${PPSSPP_DIR:-/home/claude/recomp/src/ppsspp}"
inc="${PSPRECOMP_OUTPUT_DIR:-/home/claude/recomp/psprecomp_work/output}/include"
if [ ! -f "$pp/Core/MIPS/InterpreterVFPU.cpp" ]; then
  echo "SKIP: PPSSPP checkout not found at $pp (set PPSSPP_DIR)"; exit 0
fi
tmp="${TMPDIR:-/tmp}/vfpu_semantics_build"
mkdir -p "$tmp"
ref="$here/vfpu_ref"
cxx="g++ -std=c++17 -O1 -w"
refinc="-I$ref/shim -I$pp -I$pp/ext -I$ref"
# Reference side (PPSSPP sources compiled unmodified, no PPSSPP-vs-runtime header mixing).
refchanged=0
for f in "$pp/Core/MIPS/InterpreterVFPU.cpp" "$pp/Core/MIPS/MIPSVFPUUtils.cpp" "$ref/ref_adapter.cpp" "$ref/ref_adapter.h"; do
  case "$f" in *.h) [ "$f" -nt "$tmp/ref_adapter.o" ] && rm -f "$tmp/ref_adapter.o"; continue;; esac
  o="$tmp/$(basename "$f" .cpp).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then $cxx $refinc -c "$f" -o "$o"; refchanged=1; fi
done
# The runtime links its own copy of the same PPSSPP code (src/vfpu_ppsspp); give every global the
# reference objects define a PP_ prefix so the two copies coexist in one binary.
if [ "$refchanged" = 1 ] || [ ! -f "$tmp/ref_pp.done" ]; then
  : > "$tmp/ppsyms.txt"
  for o in InterpreterVFPU MIPSVFPUUtils ref_adapter; do
    nm -g --defined-only "$tmp/$o.o" | awk '$2 ~ /^[TDBR]$/ {print $3}' | grep -v '^ref_\|^_Z[0-9]*ref_' >> "$tmp/ppsyms.txt" || true
  done
  sort -u "$tmp/ppsyms.txt" | awk '{print $1" PP_"$1}' > "$tmp/ppsyms.map"
  for o in InterpreterVFPU MIPSVFPUUtils ref_adapter; do
    objcopy --redefine-syms="$tmp/ppsyms.map" "$tmp/$o.o" "$tmp/$o.pp.o"
  done
  touch "$tmp/ref_pp.done"
fi
# Runtime side + test (rebuilt only when a source is newer than the binary).
vend="$rt/src/vfpu_ppsspp"
bin="$tmp/test_vfpu_semantics"
need=0
[ -f "$bin" ] || need=1
for f in "$here/test_vfpu_semantics.cpp" "$rt/src/psp_vfpu_core.cpp" "$rt/src/psp_vfpu_ops.cpp" "$rt/include/psp_vfpu.h" \
         "$vend/psp_vfpu_support.cpp" "$vend/Core/MIPS/InterpreterVFPU.cpp" "$vend/Core/MIPS/MIPSVFPUUtils.cpp" \
         "$ref/ref_adapter.h" "$tmp/ref_adapter.pp.o"; do
  [ "$f" -nt "$bin" ] && need=1
done
if [ "$need" = 1 ]; then
  # The vendored PPSSPP VFPU code is compiled apart from the test (its headers clash with recomp.h only
  # through the entry-point file, which includes both).
  vo="$tmp/vendored"; mkdir -p "$vo"
  vcxx="g++ -std=c++17 -O2 -w -fno-strict-aliasing -I$vend -I$rt/include -I$inc"
  for f in "$rt/src/psp_vfpu_ops.cpp" "$vend/psp_vfpu_support.cpp" "$vend/Core/MIPS/InterpreterVFPU.cpp" "$vend/Core/MIPS/MIPSVFPUUtils.cpp"; do
    $vcxx -c "$f" -o "$vo/$(basename "$f" .cpp).o"
  done
  $cxx -I"$rt/include" -I"$inc" -I"$here" -o "$bin" \
    "$here/test_vfpu_semantics.cpp" "$rt/src/psp_vfpu_core.cpp" "$vo"/*.o \
    "$tmp/InterpreterVFPU.pp.o" "$tmp/MIPSVFPUUtils.pp.o" "$tmp/ref_adapter.pp.o"
fi
"$bin" "$@"
