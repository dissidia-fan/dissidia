//! FPU translation semantics, checked by compiling the REAL emitted C++ plus the
//! real `recomp.h` and running it on the host. Expected values follow PPSSPP's
//! interpreter (Core/MIPS/Interpreter.cpp: Int_FPUComp / Int_FPU2op) and the PSP
//! hardware: exact unordered compare conditions, cvt.w.s following FCR31[1:0],
//! and saturating float->int (NaN and +overflow -> 0x7FFFFFFF, -overflow ->
//! 0x80000000).

use std::process::Command;

use psp_emitter::cpp_generator::CppGenerator;
use psp_emitter::function::{emit_function, ImportMap};
use psp_ir::{BasicBlock, DecodedFunction, FpReg, MipsOp, Reg};

fn emit(name: &str, mut ops: Vec<MipsOp>) -> String {
    ops.push(MipsOp::Jr { rs: Reg::Gpr(31) });
    ops.push(MipsOp::Nop {});
    let func = DecodedFunction {
        vaddr: 0x08804000,
        name: name.to_string(),
        cpp_name: name.to_string(),
        size: (ops.len() * 4) as u32,
        blocks: vec![BasicBlock { vaddr: 0x08804000, instrs: ops }],
        is_mid_entry_parent: false,
        mid_entry_addrs: vec![],
        coalesced: false,
    };
    let mut gen = CppGenerator::new();
    emit_function(&func, &mut gen, &ImportMap::new());
    gen.take_output()
}

#[test]
fn fpu_compare_convert_and_fcr31_match_ppsspp() {
    let mut fns = String::new();
    for c in 0..16u8 {
        fns += &emit(
            &format!("cmp{c}"),
            vec![MipsOp::CCond { cond: c, fs: FpReg(12), ft: FpReg(13) }],
        );
    }
    fns += &emit("cvtw", vec![MipsOp::CvtWS { fd: FpReg(2), fs: FpReg(12) }]);
    fns += &emit("truncw", vec![MipsOp::TruncWS { fd: FpReg(2), fs: FpReg(12) }]);
    fns += &emit("roundw", vec![MipsOp::RoundWS { fd: FpReg(2), fs: FpReg(12) }]);
    fns += &emit("ceilw", vec![MipsOp::CeilWS { fd: FpReg(2), fs: FpReg(12) }]);
    fns += &emit("floorw", vec![MipsOp::FloorWS { fd: FpReg(2), fs: FpReg(12) }]);
    fns += &emit("ctc1", vec![MipsOp::Ctc1 { rt: Reg::Gpr(4), fs: FpReg(31) }]);
    fns += &emit("cfc1", vec![MipsOp::Cfc1 { rt: Reg::Gpr(2), fs: FpReg(31) }]);

    let recomp_h = CppGenerator::emit_recomp_h();
    let src = format!(
        r#"{recomp_h}
#include <cstdio>
#include <cmath>
void psp_trace_checkpoint(uint32_t) {{}}
void sched_preempt(recomp_context*) {{}}
{fns}
static int fails = 0;
#define CHECK(c, msg) do {{ if (!(c)) {{ std::printf("FAIL: %s\n", msg); fails++; }} }} while (0)
typedef void (*Fn)(uint8_t*, recomp_context*);
static bool cmp(Fn f, float a, float b) {{
    recomp_context ctx = {{}}; ctx.f[12] = a; ctx.f[13] = b; uint8_t r = 0; f(&r, &ctx); return ctx.fpu_cc;
}}
static int32_t cvt(Fn f, float a, uint32_t fcr = 0) {{
    recomp_context ctx = {{}}; ctx.f[12] = a; ctx.fcr31 = fcr; uint8_t r = 0; f(&r, &ctx); return (int32_t)ctx.fi[2];
}}
int main() {{
    Fn cf[16] = {{cmp0,cmp1,cmp2,cmp3,cmp4,cmp5,cmp6,cmp7,cmp8,cmp9,cmp10,cmp11,cmp12,cmp13,cmp14,cmp15}};
    const float nan = std::nanf("");
    const float vals[4][2] = {{{{1.f, 2.f}}, {{2.f, 2.f}}, {{3.f, 2.f}}, {{nan, 2.f}}}};
    for (int c = 0; c < 16; ++c) for (int i = 0; i < 5; ++i) {{
        float a = i < 4 ? vals[i][0] : 2.f, b = i < 4 ? vals[i][1] : nan;
        bool un = std::isnan(a) || std::isnan(b);
        bool exp;
        switch (c & 7) {{
            case 0: exp = false; break;
            case 1: exp = un; break;
            case 2: exp = !un && a == b; break;
            case 3: exp = a == b || un; break;
            case 4: exp = a < b; break;
            case 5: exp = a < b || un; break;
            case 6: exp = a <= b; break;
            default: exp = a <= b || un; break;
        }}
        if (cmp(cf[c], a, b) != exp) {{ std::printf("FAIL cond %d a=%g b=%g\n", c, a, b); fails++; }}
    }}
    // cvt.w.s: default mode = nearest-even; modes 1/2/3 = trunc/ceil/floor.
    CHECK(cvt(cvtw, 2.5f) == 2 && cvt(cvtw, 3.5f) == 4 && cvt(cvtw, -2.5f) == -2, "cvt nearest-even");
    CHECK(cvt(cvtw, 2.7f, 1) == 2 && cvt(cvtw, -2.7f, 1) == -2, "cvt trunc mode");
    CHECK(cvt(cvtw, 2.1f, 2) == 3 && cvt(cvtw, -2.1f, 2) == -2, "cvt ceil mode");
    CHECK(cvt(cvtw, 2.9f, 3) == 2 && cvt(cvtw, -2.1f, 3) == -3, "cvt floor mode");
    // Saturation for every converter.
    Fn all[5] = {{cvtw, truncw, roundw, ceilw, floorw}};
    for (int k = 0; k < 5; ++k) {{
        CHECK(cvt(all[k], nan) == 0x7FFFFFFF, "NaN -> INT_MAX");
        CHECK(cvt(all[k], 3e9f) == 0x7FFFFFFF, "+overflow -> INT_MAX");
        CHECK(cvt(all[k], INFINITY) == 0x7FFFFFFF, "+inf -> INT_MAX");
        CHECK(cvt(all[k], -3e9f) == (int32_t)0x80000000u, "-overflow -> INT_MIN");
        CHECK(cvt(all[k], -INFINITY) == (int32_t)0x80000000u, "-inf -> INT_MIN");
        CHECK(cvt(all[k], 2147483520.f) == 2147483520, "largest float below 2^31 is exact");
    }}
    CHECK(cvt(truncw, -2.9f) == -2 && cvt(ceilw, 2.1f) == 3 && cvt(floorw, -2.1f) == -3, "directed modes");
    CHECK(cvt(roundw, 0.5f) == 0 && cvt(roundw, 1.5f) == 2 && cvt(roundw, 2.5f) == 2 && cvt(roundw, -0.5f) == 0,
          "round.w.s half-to-even, ignoring FCR31");
    CHECK(cvt(roundw, 2.7f, 1) == 3, "round.w.s ignores fcr31 mode");
    // ctc1/cfc1 round trip: only writable bits kept, cc mirrored into bit 23.
    {{
        recomp_context ctx = {{}}; uint8_t r = 0;
        ctx.r[4] = (int32_t)0xFFFFFFFFu; ctc1(&r, &ctx);
        CHECK(ctx.fcr31 == 0x0181FFFFu && ctx.fpu_cc, "ctc1 masks and sets cc");
        cfc1(&r, &ctx);
        CHECK((uint32_t)ctx.r[2] == 0x0181FFFFu, "cfc1 returns fcr31 with cc bit");
        ctx.r[4] = 0x00000003; ctc1(&r, &ctx);
        CHECK(!ctx.fpu_cc && ctx.fcr31 == 3, "ctc1 clears cc");
        cfc1(&r, &ctx);
        CHECK(ctx.r[2] == 3, "cfc1 reads rounding bits back");
    }}
    if (fails == 0) std::printf("OK\n");
    return fails ? 1 : 0;
}}
"#
    );

    let dir = std::env::temp_dir().join(format!("psprecomp_fpu_{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("mkdir");
    let cpp = dir.join("fpu_semantics.cpp");
    let bin = dir.join("fpu_semantics");
    std::fs::write(&cpp, &src).expect("write");
    let cc = Command::new("c++")
        .args(["-std=c++17", "-O1", "-w", "-o"])
        .arg(&bin)
        .arg(&cpp)
        .output()
        .expect("host c++ compiler");
    assert!(
        cc.status.success(),
        "emitted C++ must compile:\n{}",
        String::from_utf8_lossy(&cc.stderr)
    );
    let run = Command::new(&bin).output().expect("run");
    let out = String::from_utf8_lossy(&run.stdout);
    assert!(run.status.success(), "FPU semantics mismatches:\n{out}");
    let _ = std::fs::remove_dir_all(&dir);
}
