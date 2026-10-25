#pragma once
// sceMpeg HLE (PSMF / MPEG-PS movie playback). See psp_hle_mpeg.cpp.
// Registered from psp_hle_register_all_modules() via psp_hle_register_mpeg()
// (declared in hle/psp_hle.h).
#include "hle/psp_hle.h"

// Movie frame registry for the GE texture cache. sceMpeg writes each decoded
// frame into a guest buffer; this reports (by exact buffer address, masked to
// the 128 MB space) whether `masked_addr` is such a buffer in GE pixel format
// `pixel_format`, the serial of its latest frame (changes on every write) and
// the number of valid rows. Thread-safe (written by the game thread, read by
// the GE thread).
#include <cstdint>
bool psp_mpeg_frame_info(uint32_t masked_addr, int pixel_format, uint32_t* serial, int* rows);
