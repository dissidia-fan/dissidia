// Guest memory access for the vendored VFPU interpreter: the runtime's flat rdram, addressed with
// the same 0x07FFFFFF mask as psp_mem_read/psp_mem_write. The wrapper sets g_rdram per op.
#pragma once
#include <cstring>
#include "Common/CommonTypes.h"
namespace Memory {
extern thread_local unsigned char *g_rdram;
inline bool IsValid4AlignedAddress(u32 a) { return (a & 3) == 0; }
inline unsigned char *Ptr(u32 a) { return g_rdram + (a & 0x07FFFFFFu); }
inline const unsigned char *GetPointerUnchecked(u32 a) { return Ptr(a); }
inline unsigned char *GetPointerWriteUnchecked(u32 a) { return Ptr(a); }
inline u32 ReadUnchecked_U32(u32 a) { u32 v; memcpy(&v, Ptr(a), 4); return v; }
inline float ReadUnchecked_Float(u32 a) { float v; memcpy(&v, Ptr(a), 4); return v; }
inline void WriteUnchecked_U32(u32 v, u32 a) { memcpy(Ptr(a), &v, 4); }
inline void WriteUnchecked_Float(float v, u32 a) { memcpy(Ptr(a), &v, 4); }
}
