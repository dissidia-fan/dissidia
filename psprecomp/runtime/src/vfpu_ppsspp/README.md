# vfpu_ppsspp

PPSSPP's VFPU interpreter, vendored so the recompiled code's VFPU ops have the PSP's exact
semantics (prefix lifetime and quirks, min/max ordering, table-driven sin/cos/rcp/rsqrt/sqrt/exp2/log2/asin,
bit-exact dot products, NaN/denormal fixups, vrnd, conversions).

PPSSPP is GPL v2 or later; this project is GPL v2.

Files copied from PPSSPP (https://github.com/hrydgard/ppsspp), unmodified unless noted:

- `Core/MIPS/InterpreterVFPU.cpp`, `InterpreterVFPU.h`, `MIPSVFPUUtils.cpp`, `MIPSVFPUUtils.h`
- `Core/Opcode.h`, `Common/CommonTypes.h`, `CommonFuncs.h`, `Common.h`, `BitScan.h`,
  `Common/Math/SIMDHeaders.h`, `math_util.h`, `Common/Data/Convert/SmallDataConvert.h`, `ppsspp_config.h`

Replaced by small stand-ins written for this project:

- `Core/MIPS/MIPS.h`: pointer-based `MIPSState` over the live `recomp_context` (no state copy per op);
  the VFPU prefix helpers, `VCondition` and `voffset` declaration are PPSSPP's.
- `Core/MemMap.h`: guest memory = the runtime's rdram (0x07FFFFFF mask).
- `Core/Config.h`, `Core/Compatibility.h`, `Core/System.h`, `Core/Core.h`, `Core/Reporting.h`,
  `Core/MIPS/Interpreter.h`, `Core/MIPS/MIPSTables.h`, `Common/StringUtils.h`: logging and config stubs.
- `psp_vfpu_support.cpp`: `voffset`, `cst_constants`, `vfpuPrefixConstants` (from `MIPS.cpp`).

`src/psp_vfpu_ops.cpp` is the runtime-facing API: it builds the opcode word from the fields the emitter
passes and calls the interpreter function. `runtime/tests/run_vfpu_semantics.sh` compares those entry points
against an unmodified PPSSPP checkout.

To update: copy the same files from a newer PPSSPP and re-run the semantics test.
