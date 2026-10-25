#pragma once
struct ShimCompatFlags { bool ForceAccurateVdot = false; };
struct ShimCompat { const ShimCompatFlags &flags() const { static ShimCompatFlags f; return f; } };
struct ShimCoreParam { ShimCompat compat; };
inline ShimCoreParam &PSP_CoreParameter() { static ShimCoreParam p; return p; }
