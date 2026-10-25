// Reduced replacement for PPSSPP's Core/MIPS/MIPS.h, used by the vendored VFPU interpreter.
//
// PPSSPP's MIPSState owns its register file; here it only holds pointers into the live
// recomp_context (and the guest memory base), so an op costs no state copy. The VFPU prefix helpers,
// VCondition and the voffset tables are copied from PPSSPP unchanged.
//
// Copyright (C) 2012 PPSSPP Project (GPL v2 or later); adapted for psprecomp.
#pragma once

#include <cstddef>
#include <cstring>
#include <cstdint>

#include "Common/CommonTypes.h"
#include "Core/Opcode.h"

typedef Memory::Opcode MIPSOpcode;

enum {
	VFPU_CTRL_SPREFIX,
	VFPU_CTRL_TPREFIX,
	VFPU_CTRL_DPREFIX,
	VFPU_CTRL_CC,
	VFPU_CTRL_INF4,
	VFPU_CTRL_RSV5,
	VFPU_CTRL_RSV6,
	VFPU_CTRL_REV,
	VFPU_CTRL_RCX0,
	VFPU_CTRL_RCX1,
	VFPU_CTRL_RCX2,
	VFPU_CTRL_RCX3,
	VFPU_CTRL_RCX4,
	VFPU_CTRL_RCX5,
	VFPU_CTRL_RCX6,
	VFPU_CTRL_RCX7,

	VFPU_CTRL_MAX,
	//unknown....
};

// The fields of the VFPU prefixes. Lane i of an S or T prefix takes the lane its swizzle names, or
// with the constant bit, the constant the swizzle and abs bit pick, then the abs and negate. Lane i
// of a D prefix has a saturation (1 = [0:1], 3 = [-1:1]) and a write mask bit, set to keep the lane.
inline int VFPUPrefixSwizzle(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }
inline bool VFPUPrefixAbs(u32 prefix, int i) { return (prefix >> (8 + i)) & 1; }
inline bool VFPUPrefixIsConstant(u32 prefix, int i) { return (prefix >> (12 + i)) & 1; }
inline bool VFPUPrefixNegate(u32 prefix, int i) { return (prefix >> (16 + i)) & 1; }
inline int VFPUPrefixSat(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }
inline u8 VFPUPrefixWriteMask(u32 prefix) { return (prefix >> 8) & 0xF; }
inline bool VFPUPrefixWriteMask(u32 prefix, int i) { return (prefix >> (8 + i)) & 1; }

// The constants of an S or T prefix, by swizzle + 4 * abs.
extern const float vfpuPrefixConstants[8];

// The value of a lane with the constant bit, negate and all.
inline float VFPUPrefixConstant(u32 prefix, int i) {
	const float c = vfpuPrefixConstants[VFPUPrefixSwizzle(prefix, i) + (VFPUPrefixAbs(prefix, i) << 2)];
	return VFPUPrefixNegate(prefix, i) ? -c : c;
}

enum VCondition
{
	VC_FL,
	VC_EQ,
	VC_LT,
	VC_LE,
	VC_TR,
	VC_NE,
	VC_GE,
	VC_GT,
	VC_EZ,
	VC_EN,
	VC_EI,
	VC_ES,
	VC_NZ,
	VC_NN,
	VC_NI,
	VC_NS
};

// In memory, we order the VFPU registers differently. 
// Games use columns a whole lot more than rows, and it would thus be good if columns
// were contiguous in memory. Also, matrices aren't but should be.
extern const float cst_constants[32];
extern u8 voffset[128];
extern u8 fromvoffset[128];


class MIPSState {
public:
	// Views of the caller's recomp_context, set up by the psp_vfpu wrapper before every op.
	u32 *r = nullptr;           // GPRs
	union {
		float *v;               // VFPU registers, PPSSPP layout (mtx*16 + col*4 + row)
		u32 *vi;
	};
	u32 *vfpuCtrl = nullptr;    // VFPU control registers
	u32 pc = 0;
	u32 lo = 0, hi = 0;

	MIPSState() : v(nullptr) {}

	u8 VfpuWriteMask() const {
		return VFPUPrefixWriteMask(vfpuCtrl[VFPU_CTRL_DPREFIX]);
	}
	bool VfpuWriteMask(int i) const {
		return VFPUPrefixWriteMask(vfpuCtrl[VFPU_CTRL_DPREFIX], i);
	}
};

// Set by the wrapper for the duration of one op (thread-local: game threads run on OS threads).
extern thread_local MIPSState *currentMIPS;
