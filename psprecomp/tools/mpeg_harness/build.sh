#!/bin/sh
# Builds the standalone sceMpeg demux harness (needs libavcodec/libavutil dev).
cd "$(dirname "$0")/../.."
OUT=${PSPRECOMP_OUTPUT_DIR:-/home/claude/recomp/psprecomp_work/output}
g++ -std=c++17 -O1 -DPSPRECOMP_HAVE_AVCODEC -Iruntime/include -Iruntime/src -I"$OUT" -I"$OUT/include" \
  $(pkg-config --cflags libavcodec libavutil) tools/mpeg_harness/mpeg_harness.cpp -o /tmp/mpeg_harness \
  $(pkg-config --libs libavcodec libavutil)
