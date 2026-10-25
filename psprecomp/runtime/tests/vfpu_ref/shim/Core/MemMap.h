// Flat 64 KiB test memory at guest base 0x08000000 for lv/sv tests.
#pragma once
#include <cstring>
#include "Common/CommonTypes.h"
namespace Memory {
extern unsigned char g_mem[0x10000];
inline bool IsValid4AlignedAddress(u32 a) { return (a & 3) == 0 && a >= 0x08000000u && a + 4 <= 0x08010000u; }
inline unsigned char *Ptr(u32 a) { return g_mem + (a - 0x08000000u); }
inline const unsigned char *GetPointerUnchecked(u32 a) { return Ptr(a); }
inline unsigned char *GetPointerWriteUnchecked(u32 a) { return Ptr(a); }
inline u32 ReadUnchecked_U32(u32 a) { u32 v; memcpy(&v, Ptr(a), 4); return v; }
inline float ReadUnchecked_Float(u32 a) { float v; memcpy(&v, Ptr(a), 4); return v; }
inline void WriteUnchecked_U32(u32 v, u32 a) { memcpy(Ptr(a), &v, 4); }
inline void WriteUnchecked_Float(float v, u32 a) { memcpy(Ptr(a), &v, 4); }
}
