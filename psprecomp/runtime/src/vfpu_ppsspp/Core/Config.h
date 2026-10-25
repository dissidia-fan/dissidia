// Stand-in for PPSSPP's Core/Config.h: only the VFPU accuracy switch the interpreter reads.
#pragma once
struct VfpuConfig { bool bAccurateVdot = true; };
extern VfpuConfig g_Config;
