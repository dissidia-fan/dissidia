#!/bin/sh
# Build and run the savedata list dialog host test (g++ only).
# DUMP_DIALOG=/path/out.ppm renders a sample frame for eyeballing.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
fonts="${TMPDIR:-/tmp}/psp_ui_fonts.cpp"
cmake -DFONT_DIR="$here/../assets/fonts" -DOUT="$fonts" -P "$here/../assets/embed_fonts.cmake"
out="${TMPDIR:-/tmp}/test_savedata_dialog"
g++ -std=c++17 -O1 -Wall -Wextra -o "$out" \
  "$here/test_savedata_dialog.cpp" "$here/../src/hle/psp_savedata_dialog.cpp" "$fonts" \
  $(pkg-config --cflags --libs freetype2)
"$out"
