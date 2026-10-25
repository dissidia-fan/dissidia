#!/bin/sh
# Build and run the sceUtilitySavedata HLE host test (g++ only, no cmake).
# Needs recomp.h (generated); same lookup as run_vfpu_atan2.sh.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
fonts="${TMPDIR:-/tmp}/psp_ui_fonts.cpp"
cmake -DFONT_DIR="$here/../assets/fonts" -DOUT="$fonts" -P "$here/../assets/embed_fonts.cmake"
rt="$here/.."
inc="${1:-${PSPRECOMP_OUTPUT_DIR:-/home/claude/recomp/psprecomp_work/output}/include}"
out="${TMPDIR:-/tmp}/test_savedata_hle"
g++ -std=c++17 -O1 -I"$rt/include" -I"$inc" -o "$out" \
  "$here/test_savedata_hle.cpp" \
  "$rt/src/hle/psp_savedata_crypto.cpp" "$rt/src/hle/psp_savedata_dialog.cpp" "$fonts" \
  $(pkg-config --cflags --libs freetype2) -lpthread
"$out"
