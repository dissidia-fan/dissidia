#!/bin/sh
# Renders every FX shader override (work/mods/fx_shaders) at several lab ticks
# into a contact sheet, with the runtime's own shader code, headless (EGL).
# Linux only: Mesa + EGL (Steam Deck desktop mode, any Linux box; on a box
# without a GPU Mesa's llvmpipe renders it). Not for macOS (no EGL).
#   fx_shader_preview.sh [out.png]
#   FX_PREVIEW_BENCH=1280x800 fx_shader_preview.sh   # + GPU ms of a full-screen pass per shader
# Needs: g++, libEGL / libGL headers (Debian/Ubuntu: libegl-dev libgl-dev), zlib, python3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)            # psprecomp/
tmp=${TMPDIR:-/tmp}/fx_shader_preview
rm -rf "$tmp"; mkdir -p "$tmp"
cp -R "$root/work/mods/fx_shaders" "$tmp/fx"
python3 "$here/fx_shader_preview_samples.py" "$root/work/mods/fx_shaders/manifest.json" "$tmp/fx/manifest.json" "$tmp/samples.txt"
g++ -std=c++17 -O1 -w -I"$root/runtime/include" -o "$tmp/fx_shader_preview" \
    "$here/fx_shader_preview.cpp" "$root/runtime/src/psp_fx_shader.cpp" "$root/runtime/src/psp_fx_manifest.cpp" \
    "$root/runtime/src/psp_ge_shader.cpp" "$root/runtime/src/psp_tex_pack.cpp" -x c "$root/runtime/src/glad.c" \
    -lEGL -lz -ldl
PSPRECOMP_FX_SHADERS_DIR="$tmp/fx" "$tmp/fx_shader_preview" "$tmp/samples.txt" "${1:-$tmp/sheet.png}" 2>"$tmp/log.txt" \
    || { cat "$tmp/log.txt"; exit 1; }
grep -v "\] match \|^\[FXSHADER\]   \|\] texture " "$tmp/log.txt" || true
