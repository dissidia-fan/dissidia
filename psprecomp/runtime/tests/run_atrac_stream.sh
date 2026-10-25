#!/bin/sh
# Build and run the sceAtrac streaming/loop host test (g++ + libavcodec via pkg-config, e.g. brew ffmpeg).
# Default track: Dissidia's battle BGM B14.AT3 (streamed, loops at ~100 s), 2.6 passes, 98000-byte ring.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/.."
inc="${PSPRECOMP_OUTPUT_DIR:-$rt/../work/output}/include"
trk="${1:-$rt/../work/disc0/PSP_GAME/USRDIR/DATA/BGM/B14.AT3}"
out="${TMPDIR:-/tmp}/test_atrac_stream"
stub="${TMPDIR:-/tmp}/atrac_stub"
mkdir -p "$stub/hle" && cp "$here/atrac_stub_psp_hle.h" "$stub/hle/psp_hle.h"
c++ -std=c++17 -O1 -DPSPRECOMP_HAVE_AVCODEC -I"$stub" -I"$inc" -I"$rt/include" -o "$out" \
  "$here/test_atrac_stream.cpp" $(pkg-config --cflags --libs libavcodec libavutil)
cd "${TMPDIR:-/tmp}" && "$out" "$trk" "${2:-98000}" "${3:-2.6}"
