#!/bin/sh
# Before/after of a character's live lighting (docs/character-lighting.md), headless (EGL): the game's flat look on
# top, the lit body below, from a few camera angles, drawn with the runtime's own uber-shader and FX shader code.
# Linux only (Mesa + EGL: Steam Deck desktop mode, any Linux box; llvmpipe without a GPU). Not for macOS (no EGL).
#   char_light_preview.sh <mods/<name>/models/<model>.gmo> [<skin file>] [out.png]
#   LIGHT='{"key":0.9,"bands":3}' char_light_preview.sh ...   # try body.lighting settings without a build
#   ANGLES="0 45 90 180" char_light_preview.sh ...
# Needs: g++, libEGL / libGL headers (Debian/Ubuntu: libegl-dev libgl-dev), zlib, python3 + numpy + Pillow.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)            # psprecomp/
tmp=${TMPDIR:-/tmp}/char_light_preview
rm -rf "$tmp"; mkdir -p "$tmp/fx/lit"
model=$1; shift
skin=; hd=
case "$1" in *.skin) skin=$1; shift; [ -d "$(dirname "$skin")/hd" ] && hd="--hd $(dirname "$skin")/hd" ;; esac
out=${1:-$tmp/sheet.png}
face=$(HERE="$here" python3 -c "import json,sys,os;sys.path.insert(0,os.environ['HERE']);import charmod_light as L;print(L.settings({'lighting':json.loads(sys.argv[1])})['face'])" "${LIGHT:-true}")
python3 "$here/char_light_dump.py" "$model" $skin "$tmp/mesh" $hd --face "$face"
cp "$root/games/dissidia/fx_shaders/lit/character.frag" "$tmp/fx/lit/"
HERE="$here" python3 -c "
import json, sys, os
sys.path.insert(0, os.environ['HERE'])
import charmod_light as L
tmp, light = sys.argv[1], json.loads(sys.argv[2])
ids = [int(l, 16) for l in open(tmp + '/mesh/ids.txt')]
json.dump({'version': 1, 'draws': L.fx_draws('preview', ids, L.settings({'lighting': light}))},
          open(tmp + '/fx/manifest.json', 'w'), indent=1)
" "$tmp" "${LIGHT:-true}"
g++ -std=c++17 -O1 -w -I"$root/runtime/include" -o "$tmp/char_light_preview" \
    "$here/char_light_preview.cpp" "$root/runtime/src/psp_fx_shader.cpp" "$root/runtime/src/psp_fx_manifest.cpp" \
    "$root/runtime/src/psp_ge_shader.cpp" "$root/runtime/src/psp_tex_pack.cpp" -x c "$root/runtime/src/glad.c" \
    -lEGL -lz -ldl
PSPRECOMP_FX_SHADERS_DIR="$tmp/fx" "$tmp/char_light_preview" "$tmp/mesh/mesh.bin" "$out" ${ANGLES:-20 70 160}
