#!/bin/sh
# Host test of the Dissidia savedata dialog theme (needs the extracted disc; skips without).
set -e
here=$(cd "$(dirname "$0")" && pwd)
rt="$here/../../../runtime"
fonts="${TMPDIR:-/tmp}/psp_ui_fonts.cpp"
cmake -DFONT_DIR="$rt/assets/fonts" -DOUT="$fonts" -P "$rt/assets/embed_fonts.cmake"
out="${TMPDIR:-/tmp}/test_savedata_theme"
g++ -std=c++17 -O1 -Wall -Wextra -I"$rt/include" -o "$out" \
  "$here/test_savedata_theme.cpp" "$here/../runtime/savedata_theme.cpp" "$here/../runtime/gim_decode.cpp" \
  "$here/../runtime/package_mods.cpp" "$rt/src/hle/psp_savedata_dialog.cpp" "$fonts" \
  $(pkg-config --cflags --libs freetype2)
"$out"
