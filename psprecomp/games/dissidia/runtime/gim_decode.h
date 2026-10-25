// games/dissidia/runtime/gim_decode.h — decoder for the menu GIM textures and
// the 'menu pak' archives that hold them (tools/gim_encode.py and
// tools/menu_pack.py are the Python counterparts).
//
// Pure C++17, no runtime dependencies (host-testable: tests/test_gim_decode.cpp).
//
// GIM: "MIG.00.1PSP" file of nested blocks { u16 id, u16 pad, u32 size, u32
// next, u32 data }. Block 2/3 = file/picture containers, 4 = image, 5 = palette.
// An image/palette block carries { u16 hdr, u16 pad, u16 format, u16 order,
// u16 w, u16 h, ..., u32 plane_offset at +0x1C (data starts at header +
// offset) }. Formats: 0 RGB565, 1 RGBA5551, 2 RGBA4444, 3 RGBA8888, 4 INDEX4,
// 5 INDEX8, 6 INDEX16, 7 INDEX32; order 1 = PSP swizzled (16 byte x 8 row
// blocks). Indexed images take the picture's palette block.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dissidia {

struct GimImage {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;   // straight alpha, uint32 = 0xAABBGGRR (R in the low byte)
};

/// Number of image blocks in a GIM file (0 if it is not a GIM).
int gim_image_count(const uint8_t* d, size_t n);
/// Decode image `index` to RGBA8. Returns false (and *err) for malformed or
/// unsupported data.
bool gim_decode(const uint8_t* d, size_t n, int index, GimImage* out, std::string* err = nullptr);

/// 'menu pak' / 'mess pak' archive: entry `i` as a pointer+length into `d`
/// (len 0 = empty slot). Returns false if the archive is malformed or i is out of range.
bool menu_pak_entry(const uint8_t* d, size_t n, int i, const uint8_t** p, size_t* len);

}  // namespace dissidia
