#include "psp_ge.h"
#include "psp_prof.h"
#include "psp_ge_constants.h"
#include "psp_ge_draw.h"
#include "psp_ge_vertex.h"
#include "recomp.h"
#include "hle/psp_hle_kernel.h"  // psp_alloc_stack (needs recomp.h first)

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Skinning diagnostics (PSPRECOMP_SKIN_DIAG): prim sequence number at which each bone matrix slot was last written.
uint32_t g_ge_bone_slot_last[8] = {0};
uint32_t g_ge_skin_prim_seq = 1;


// -- Module state --
static GeState g_ge_state;
static bool g_ge_trace = false;
static int g_total_lists = 0;
static int g_total_prims = 0;
static bool g_first_prim_logged = false;

// -- Signal callback state --
static uint8_t*  g_signal_rdram  = nullptr;
static uint32_t  g_signal_fn     = 0;
static uint32_t  g_signal_arg    = 0;
static int       g_signal_cb_uid = -1;

// -- Finish callback state --
static uint32_t  g_finish_fn  = 0;
static uint32_t  g_finish_arg = 0;

// -- GE-callback guest stack --
// Lazily carved once from the top of PSP user memory (same allocator as
// thread stacks). GE callbacks run guest code that pushes a stack frame
// (a typical finish handler opens with `addiu sp, sp, -0x10`), so a fresh zeroed ctx
// with sp=0 would corrupt low memory. Shared by FINISH and SIGNAL paths.
static uint32_t g_ge_cb_stack_top = 0;

static uint32_t ge_cb_stack(uint8_t* rdram) {
    if (g_ge_cb_stack_top == 0) {
        g_ge_cb_stack_top = psp_alloc_stack(rdram, 0x1000) + 0x1000 - 0x40;  // block's lowest addr -> sp at its top
        std::fprintf(stderr,
            "[GE] Callback guest stack carved: sp=0x%08X\n",
            g_ge_cb_stack_top);
    }
    return g_ge_cb_stack_top;
}

// -- Call stack entry for CALL/RET --
struct GeCallStackEntry {
    uint32_t pc;
    uint32_t offset_addr;
    uint32_t base_addr;
};

// -- Helpers --

/// Resolve a 24-bit address field using the current BASE register.
/// Mirrors PPSSPP getRelativeAddress: (offsetAddr + (base | data)) & 0x0FFFFFFF.
/// OFFSETADDR / ORIGIN shift every VADDR/IADDR/JUMP/CALL target; ignoring the
/// offset points 3D vertex fetches at address 0.
static inline uint32_t resolve_addr(uint32_t data) {
    return (g_ge_state.offset_addr
            + (g_ge_state.base_addr | (data & 0x00FFFFFF))) & 0x0FFFFFFFu;
}

/// Convert 24-bit GE float data to IEEE 754 float.
/// GE encodes floats as the upper 24 bits of a 32-bit IEEE float.
static inline float data_to_float(uint32_t data) {
    uint32_t bits = data << 8;
    float result;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

// Diagnostic ring of the last CALL/JUMP transfers, dumped on the first unknown
// command so a list that ran off the end of a sub-list (e.g. into vertex data) can
// be traced back to the CALL that entered it.
static struct { uint32_t from, to; char kind; int depth; } g_nav_ring[8];
static unsigned g_nav_n = 0;
static inline void ge_nav_note(uint32_t from, uint32_t to, char kind, int depth) {
    auto& e = g_nav_ring[g_nav_n++ & 7];
    e.from = from; e.to = to; e.kind = kind; e.depth = depth;
}
static void ge_nav_dump(uint32_t pc) {
    static int dumps = 0;
    if (dumps++ >= 4) return;
    std::fprintf(stderr, "[GE_NAV] unknown cmd at pc=0x%08X; last CALL/JUMP transfers (oldest first):\n", pc);
    for (unsigned i = 0; i < 8; i++) {
        const auto& e = g_nav_ring[(g_nav_n + i) & 7];
        if (e.from || e.to)
            std::fprintf(stderr, "[GE_NAV]   %c at=0x%08X -> 0x%08X depth=%d\n", e.kind, e.from, e.to, e.depth);
    }
}

// -- Atexit summary --
static void ge_atexit_summary() {
    std::fprintf(stderr,
        "[GE_SUMMARY] Total display lists processed: %d\n",
        g_total_lists);
    std::fprintf(stderr,
        "[GE_SUMMARY] Total PRIM commands: %d\n",
        g_total_prims);
}

// -- Public API --

const GeState& ge_get_state() {
    return g_ge_state;
}

GeState& ge_get_state_mut() {
    return g_ge_state;
}

void ge_init() {
    g_ge_state.reset();

    // Set reasonable defaults
    g_ge_state.framebuf_format = GE_FORMAT_8888;
    // Hardware reset: material ambient white, alpha opaque (PPSSPP defaults),
    // so colourless vertices from a game that never sets them stay visible.
    g_ge_state.material_ambient = 0xFFFFFF;
    g_ge_state.material_alpha = 0xFF;
    g_ge_state.tex_scale_u = 1.0f;
    g_ge_state.tex_scale_v = 1.0f;
    g_ge_state.viewport_x_center = 2048.0f;
    g_ge_state.viewport_y_center = 2048.0f;
    g_ge_state.viewport_z_center = 32767.5f;
    g_ge_state.viewport_x_scale = 240.0f;
    g_ge_state.viewport_y_scale = -136.0f;
    g_ge_state.viewport_z_scale = 32767.5f;
    g_ge_state.min_z = 0;          // depth range default [0, 65535]
    g_ge_state.max_z = 65535;
    g_ge_state.shade_mode = 1;  // gouraud
    g_ge_state.depth_func = GE_COMP_GEQUAL;

    // Check GE trace env var
    const char* trace_env = std::getenv("PSPRECOMP_GE_TRACE");
    g_ge_trace = (trace_env && trace_env[0] == '1');

    if (g_ge_trace) {
        std::fprintf(stderr, "[GE] Trace mode enabled\n");
    }

    g_total_lists = 0;
    g_total_prims = 0;
    g_first_prim_logged = false;
    std::atexit(ge_atexit_summary);
    std::fprintf(stderr, "[GE] Initialized\n");
}

void ge_shutdown() {
    std::fprintf(stderr,
        "[GE] Shutdown (processed %d display lists)\n",
        g_total_lists);
}

void ge_set_signal_callback(
    uint8_t* rdram, uint32_t signal_fn,
    uint32_t signal_arg, int cb_uid
) {
    g_signal_rdram  = rdram;
    g_signal_fn     = signal_fn;
    g_signal_arg    = signal_arg;
    g_signal_cb_uid = cb_uid;
    std::fprintf(stderr,
        "[GE] Signal callback registered: fn=0x%08X arg=0x%08X uid=%d\n",
        signal_fn, signal_arg, cb_uid);
}

void ge_set_finish_callback(
    uint32_t finish_fn, uint32_t finish_arg
) {
    g_finish_fn  = finish_fn;
    g_finish_arg = finish_arg;
    std::fprintf(stderr,
        "[GE] Finish callback registered: fn=0x%08X arg=0x%08X\n",
        finish_fn, finish_arg);
}

uint32_t g_ge_cur_pc = 0;  // pc of the GE command being executed (trace aid)
uint8_t* g_dbg_rdram = nullptr;  // for psp_dispatch.cpp MEM_WATCH debug aid

bool ge_draw_dyn_trace_active();

GeListResult ge_process_display_list(
    uint8_t* rdram,
    uint32_t list_addr,
    uint32_t stall_addr
) {
    g_dbg_rdram = rdram;
    psp_prof::Scope prof_list(psp_prof::S_LIST);
    // Whatever way this list yields (END, stall, error), submit its pending
    // batched draw before returning to the queue.
    struct FlushOnExit { ~FlushOnExit() { ge_draw_flush(); } } flush_on_exit;
    uint32_t pc = list_addr;
    int prim_count = 0;
    int cmd_count = 0;

    // Call stack for CALL/RET
    std::array<GeCallStackEntry, GE_CALL_STACK_SIZE> call_stack{};
    int stack_ptr = 0;

    // Begin list: bind FBO, set viewport
    ge_draw_begin_list();

    // Clear log-once flags for this list (allow re-warn per list)
    // Actually, keep across lists to avoid spam -- only clear on init

    while (cmd_count < GE_MAX_COMMANDS) {
        g_ge_cur_pc = pc;
        // Stall check
        if (stall_addr != 0 && pc >= stall_addr) {
            if (g_ge_trace) {
                std::fprintf(stderr,
                    "[GE] Hit stall addr 0x%08X at pc 0x%08X\n",
                    stall_addr, pc);
            }
            // Return stalled result — caller keeps the list alive
            return GeListResult{pc, false, prim_count};
        }

        uint32_t word = psp_mem_read<uint32_t>(rdram, pc);
        uint8_t cmd = static_cast<uint8_t>(word >> 24);
        uint32_t data = word & 0x00FFFFFF;

        // PSPRECOMP_GE_WATCH=<hex addr>: log every change of that guest word as
        // seen at command-fetch time (debug aid: who patches a list word).
        static const uint32_t watch_addr = std::getenv("PSPRECOMP_GE_WATCH")
            ? (uint32_t)std::strtoul(std::getenv("PSPRECOMP_GE_WATCH"), nullptr, 16) : 0;
        if (watch_addr) {
            static uint32_t last = 0xDEADBEEF; static int n = 0;
            uint32_t cur = psp_mem_read<uint32_t>(rdram, watch_addr);
            if (cur != last && n < 200) {
                n++;
                std::fprintf(stderr, "[WATCH] %08X: %08X -> %08X (list pc=%08X)\n", watch_addr, last, cur, pc);
                last = cur;
            }
        }
        // PSPRECOMP_GE_HEXDUMP=<hex addr>:<hex len>: hex-dump guest memory once,
        // at the PSPRECOMP_GE_HEXDUMP_AFTER'th skinned (vtype 0x431A) PRIM.
        {
            static const char* hd = std::getenv("PSPRECOMP_GE_HEXDUMP");
            static long hd_after = std::getenv("PSPRECOMP_GE_HEXDUMP_AFTER")
                ? std::atol(std::getenv("PSPRECOMP_GE_HEXDUMP_AFTER")) : 1;
            static long hd_n = 0; static bool hd_done = false;
            if (hd && !hd_done && cmd == GE_CMD_PRIM && g_ge_state.vertex_type == 0x431Au && ++hd_n >= hd_after) {
                hd_done = true;
                uint32_t a = (uint32_t)std::strtoul(hd, nullptr, 16);
                const char* c = std::strchr(hd, ':');
                uint32_t len = c ? (uint32_t)std::strtoul(c + 1, nullptr, 16) : 256;
                for (uint32_t o = 0; o < len; o += 16) {
                    std::fprintf(stderr, "[HEX] %08X:", a + o);
                    for (uint32_t k = 0; k < 16; k += 4)
                        std::fprintf(stderr, " %08X", psp_mem_read<uint32_t>(rdram, a + o + k));
                    std::fprintf(stderr, "\n");
                }
            }
        }
        // PSPRECOMP_GE_MEMDUMP=<path>: write rdram[0..0x4200000) = RAM 0x08000000.. then VRAM at +0x4000000 to a file at the
        // first skinned (vtype 0x431A) PRIM, for offline analysis.
        {
            static const char* md = std::getenv("PSPRECOMP_GE_MEMDUMP");
            static bool md_done = false;
            static long md_after = std::getenv("PSPRECOMP_GE_MEMDUMP_AFTER") ? std::atol(std::getenv("PSPRECOMP_GE_MEMDUMP_AFTER")) : 1;
            static long md_n = 0;
            if (md && !md_done && cmd == GE_CMD_PRIM && g_ge_state.vertex_type == 0x431Au && ++md_n >= md_after) {
                md_done = true;
                if (FILE* f = std::fopen(md, "wb")) {
                    std::fwrite(rdram, 1, 0x04200000u, f);
                    std::fclose(f);
                    std::fprintf(stderr, "[GE] memdump written to %s\n", md);
                }
            }
        }
        // PSPRECOMP_GE_RING_DUMP=<prim#>: keep the last 512 commands and dump
        // them at the <n>th non-through PRIM with >= 3 vertices (debug aid).
        static const long ring_at = std::getenv("PSPRECOMP_GE_RING_DUMP")
            ? std::atol(std::getenv("PSPRECOMP_GE_RING_DUMP")) : 0;
        static uint32_t ring_pc[1 << 17], ring_w[1 << 17];
        static unsigned ring_n = 0;
        static long ring_prims = 0;
        if (ring_at) {
            ring_pc[ring_n & 0x1FFFF] = pc; ring_w[ring_n & 0x1FFFF] = word; ring_n++;
            static const unsigned ring_vt = std::getenv("PSPRECOMP_GE_RING_VTYPE")
                ? (unsigned)std::strtoul(std::getenv("PSPRECOMP_GE_RING_VTYPE"), nullptr, 16) : 0;
            if (cmd == GE_CMD_PRIM && !(g_ge_state.vertex_type & 0x800000u)
                && (ring_vt == 0 || g_ge_state.vertex_type == ring_vt)
                && (data & 0xFFFF) >= 3 && ++ring_prims == ring_at) {
                static const long ring_len = std::getenv("PSPRECOMP_GE_RING_LEN")
                    ? std::atol(std::getenv("PSPRECOMP_GE_RING_LEN")) : 512;
                const unsigned want = (unsigned)std::min<long>(ring_len, 1 << 17);
                for (unsigned k = ring_n > want ? ring_n - want : 0; k < ring_n; k++) {
                    const uint32_t w = ring_w[k & 0x1FFFF];
                    const uint32_t c = w >> 24;
                    if (c == 0x2B || c == 0x3B || c == 0x3D || c == 0x3F || c == 0x41 || c == 0) continue;
                    std::fprintf(stderr, "[RING] %08X: %08X\n", ring_pc[k & 0x1FFFF], w);
                }
            }
        }

        if (g_ge_trace) {
            std::fprintf(stderr,
                "[GE:0x%08X] cmd=0x%02X data=0x%06X\n",
                pc, cmd, data);
        }

        switch (cmd) {
        // ---- Control flow ----
        case GE_CMD_NOP:
            break;

        case GE_CMD_END:
            if (g_ge_trace) {
                std::fprintf(stderr,
                    "[GE] END at 0x%08X (%d PRIMs, %d cmds)\n",
                    pc, prim_count, cmd_count);
            }
            ge_draw_end_list();   // marks front buffer dirty (liveness net)
            g_total_lists++;
            // Present is decoupled from list completion (issue #59): it is
            // driven only by the guest's page-flip (sceDisplaySetFrameBuf),
            // with ge_draw_present_if_stale as a no-flip safety net. The
            // per-END auto-present was removed — it published intermediate
            // buffers (e.g. a trailing clear) as if they were frames.
            return GeListResult{pc + 4, true, prim_count};

        case GE_CMD_FINISH: {
            ge_draw_list_end();  // flush (the finish callback runs guest code); GE_SYNC=2 waits for the GPU here
            // FINISH signals that the display list is complete.
            // On real PSP the GE FINISH interrupt fires the registered
            // finish callback (sceGeSetCallback finish_func,
            // registered by the game) with a0 = FINISH data & 0xffff (the token the
            // game embedded in the list -- 0xffff for the frame tick)
            // and a1 = the registered finish arg. The handler pushes a
            // stack frame, so it needs a valid guest sp.
            if (g_finish_fn != 0) {
                FuncPtr fn = RECOMP_LOOKUP(g_finish_fn);
                if (fn) {
                    static bool finish_logged = false;
                    if (!finish_logged) {
                        std::fprintf(stderr,
                            "[GE] finish-cb fn=0x%08X token=0x%04X "
                            "arg=0x%08X\n",
                            g_finish_fn, data & 0xFFFF, g_finish_arg);
                        finish_logged = true;
                    }
                    recomp_context fin_ctx{};
                    fin_ctx.r[4] = static_cast<int64_t>(data & 0xFFFF);
                    fin_ctx.r[5] = static_cast<int64_t>(g_finish_arg);
                    fin_ctx.r[29] = static_cast<int32_t>(
                        ge_cb_stack(rdram));
                    fn(rdram, &fin_ctx);
                } else {
                    static bool finish_miss_logged = false;
                    if (!finish_miss_logged) {
                        std::fprintf(stderr,
                            "[GE] FINISH: RECOMP_LOOKUP(0x%08X) MISS\n",
                            g_finish_fn);
                        finish_miss_logged = true;
                    }
                }
            }
            break;
        }

        case GE_CMD_SIGNAL: {
            ge_draw_flush();  // the signal callback runs guest code
            // SIGNAL behaviors 0x10/0x11/0x12 are GE flow control
            // (PPSSPP GPUCommon::ProcessSignal): JUMP/CALL/RET. They
            // pair with the following END word, whose low 16 bits
            // supply the low half of the target address. The paired
            // END is consumed (skipped) -- it is NOT a list-end.
            // These must NOT fire the user signal callback.
            uint32_t behavior = (data >> 16) & 0xFF;
            uint32_t signal_val = data & 0xFFFF;
            if (behavior == 0x10 || behavior == 0x11) {
                uint32_t end_data =
                    psp_mem_read<uint32_t>(rdram, pc + 4);
                uint32_t target =
                    ((signal_val << 16) | (end_data & 0xFFFF))
                    & 0xFFFFFFFC;  // PPSSPP parity: keep bits 28-31
                                   // so uncached-space pcs still
                                   // compare against stall_addr
                if (g_ge_trace) {
                    std::fprintf(stderr,
                        "[GE] SIGNAL %s at 0x%08X -> 0x%08X\n",
                        behavior == 0x11 ? "CALL" : "JUMP",
                        pc, target);
                }
                if (behavior == 0x11) {
                    if (stack_ptr >= GE_CALL_STACK_SIZE) {
                        // PPSSPP behavior: ignore the call, skip the
                        // paired END and keep processing (do not park
                        // the list as stalled).
                        std::fprintf(stderr,
                            "[GE] SIGNAL CALL stack overflow "
                            "at 0x%08X -- skipping\n", pc);
                        pc += 4;
                        break;
                    }
                    // Return to the word AFTER the paired END
                    call_stack[stack_ptr].pc = pc + 8;
                    call_stack[stack_ptr].offset_addr =
                        g_ge_state.offset_addr;
                    call_stack[stack_ptr].base_addr =
                        g_ge_state.base_addr;
                    stack_ptr++;
                }
                pc = target - 4;  // -4: pc += 4 at end of loop
                break;
            }
            if (behavior == 0x12) {
                if (stack_ptr <= 0) {
                    std::fprintf(stderr,
                        "[GE] SIGNAL RET with empty stack "
                        "at 0x%08X\n", pc);
                    // Skip the paired END and keep processing --
                    // do not treat it as list-end.
                    pc += 4;
                    break;
                }
                stack_ptr--;
                g_ge_state.offset_addr =
                    call_stack[stack_ptr].offset_addr;
                g_ge_state.base_addr =
                    call_stack[stack_ptr].base_addr;
                if (g_ge_trace) {
                    std::fprintf(stderr,
                        "[GE] SIGNAL RET at 0x%08X -> 0x%08X\n",
                        pc, call_stack[stack_ptr].pc);
                }
                pc = call_stack[stack_ptr].pc - 4;
                break;
            }
            // Fire the registered GE signal callback.
            // On real PSP the GE interrupt handler calls this when the
            // SIGNAL command is encountered during list processing.
            // The game's registered signal callback calls sceKernelSetEventFlag
            // which wakes the thread that calls sceKernelSignalSema(uid=259).
            std::fprintf(stderr,
                "[GE] SIGNAL cmd at 0x%08X data=0x%06X fn=0x%08X\n",
                pc, data, g_signal_fn);
            if (g_signal_fn != 0) {
                FuncPtr fn = RECOMP_LOOKUP(g_signal_fn);
                if (fn) {
                    recomp_context sig_ctx{};
                    // a0 = SIGNAL data & 0xffff (the in-list token),
                    // matching the FINISH path. The handler runs guest
                    // code, so give it the carved guest stack.
                    sig_ctx.r[4] = static_cast<int64_t>(data & 0xFFFF);
                    sig_ctx.r[5] = static_cast<int64_t>(g_signal_arg);
                    sig_ctx.r[29] = static_cast<int32_t>(
                        ge_cb_stack(rdram));
                    fn(g_signal_rdram ? g_signal_rdram : rdram, &sig_ctx);
                } else {
                    std::fprintf(stderr,
                        "[GE] SIGNAL: RECOMP_LOOKUP(0x%08X) MISS\n",
                        g_signal_fn);
                }
            }
            break;
        }

        case GE_CMD_JUMP: {
            uint32_t target = resolve_addr(data & 0x00FFFFFC);
            ge_nav_note(pc, target, 'J', stack_ptr);
            pc = target - 4;  // -4 because pc += 4 at end of loop
            break;
        }

        case GE_CMD_CALL: {
            if (stack_ptr >= GE_CALL_STACK_SIZE) {
                std::fprintf(stderr,
                    "[GE] CALL stack overflow at 0x%08X\n", pc);
                return GeListResult{pc, false, prim_count};
            }
            call_stack[stack_ptr].pc = pc + 4;
            call_stack[stack_ptr].offset_addr =
                g_ge_state.offset_addr;
            call_stack[stack_ptr].base_addr =
                g_ge_state.base_addr;
            stack_ptr++;
            uint32_t target = resolve_addr(data & 0x00FFFFFC);
            ge_nav_note(pc, target, 'C', stack_ptr);
            { extern bool ge_draw_dyn_trace_active(); if (ge_draw_dyn_trace_active()) std::fprintf(stderr, "[DT]   CALL at=%08X -> %08X depth=%d\n", pc, target, stack_ptr); }
            pc = target - 4;
            break;
        }

        case GE_CMD_RET: {
            if (stack_ptr <= 0) {
                std::fprintf(stderr,
                    "[GE] RET with empty stack at 0x%08X\n", pc);
                return GeListResult{pc, false, prim_count};
            }
            stack_ptr--;
            g_ge_state.offset_addr =
                call_stack[stack_ptr].offset_addr;
            g_ge_state.base_addr =
                call_stack[stack_ptr].base_addr;
            pc = call_stack[stack_ptr].pc - 4;
            break;
        }

        case GE_CMD_BJUMP:
            // Bounding box conditional jump -- skip for Phase 5
            break;

        // ---- Address / type ----
        case GE_CMD_BASE:
            g_ge_state.base_addr = (data << 8) & 0xFF000000;
            break;

        case GE_CMD_OFFSETADDR:
            g_ge_state.offset_addr = data << 8;
            {
                static int n = 0;
                if (n++ < 40)
                    std::fprintf(stderr, "[GE] OFFSETADDR 0x%08X at pc=0x%08X\n", g_ge_state.offset_addr, pc);
            }
            break;

        case GE_CMD_ORIGIN:
            g_ge_state.offset_addr = pc;
            {
                static int n = 0;
                if (n++ < 40)
                    std::fprintf(stderr, "[GE] ORIGIN at pc=0x%08X\n", pc);
            }
            break;

        case GE_CMD_VADDR:
            g_ge_state.vertex_addr = resolve_addr(data);
            break;

        case GE_CMD_IADDR:
            g_ge_state.index_addr = resolve_addr(data);
            break;

        case GE_CMD_VERTEXTYPE:
            g_ge_state.vertex_type = data;
            break;

        // ---- PRIM (draw) ----
        case GE_CMD_PRIM: {
            int prim_type = (data >> 16) & 0x7;
            int count = data & 0xFFFF;
            prim_count++;
            g_total_prims++;
            if (!g_first_prim_logged) {
                std::fprintf(stderr,
                    "[GE] First PRIM command received! "
                    "list_id=%d type=%d count=%d\n",
                    g_total_lists, prim_type, count);
                g_first_prim_logged = true;
            }
            if (g_ge_trace) {
                std::fprintf(stderr,
                    "[GE] PRIM type=%d count=%d "
                    "vtype=0x%06X vaddr=0x%08X\n",
                    prim_type, count,
                    g_ge_state.vertex_type,
                    g_ge_state.vertex_addr);
            }
            {
                // Always-on cheap stats: how many 3D PRIMs fetch from a null vertex pointer.
                static unsigned long n3d = 0, n3d_null = 0, n3d_nulltex = 0;
                if (!(g_ge_state.vertex_type & 0x800000u) && count >= 3) {
                    n3d++;
                    if ((g_ge_state.vertex_addr & 0x0FFFFFFFu) < 0x08000000u
                        && (g_ge_state.vertex_addr & 0x0F000000u) == 0) n3d_null++;
                    if (g_ge_state.texture_enable && g_ge_state.tex_addr[0] == 0) n3d_nulltex++;
                    if (n3d % 50000 == 0)
                        std::fprintf(stderr, "[GE_3D_STATS] 3d prims=%lu null-vertex-ptr=%lu null-tex-addr=%lu\n", n3d, n3d_null, n3d_nulltex);
                }
            }
            ge_draw_prim(rdram, prim_type, count);
            // The GE advances its vertex (or index) pointer past the vertices
            // a PRIM consumed, so back-to-back PRIMs without a new VADDR walk
            // through the buffer (PPSSPP AdvanceVerts).
            {
                const int idx = ge_vtype_idx(g_ge_state.vertex_type);
                if (idx != GE_VTYPE_IDX_NONE) {
                    g_ge_state.index_addr += static_cast<uint32_t>(count)
                        * (idx == GE_VTYPE_IDX_8BIT ? 1u : idx == GE_VTYPE_IDX_16BIT ? 2u : 4u);
                } else {
                    g_ge_state.vertex_addr += static_cast<uint32_t>(count)
                        * static_cast<uint32_t>(ge_vertex_stride(g_ge_state.vertex_type));
                }
            }
            break;
        }

        // ---- Matrix data ----
        case GE_CMD_WORLDMATRIXNUMBER:
            g_ge_state.world_mtx_num = data & 0xF;
            break;
        case GE_CMD_WORLDMATRIXDATA:
            if (g_ge_state.world_mtx_num < 12) {
                g_ge_state.world_matrix[
                    g_ge_state.world_mtx_num++] =
                    data_to_float(data);
            }
            break;

        case GE_CMD_VIEWMATRIXNUMBER:
            g_ge_state.view_mtx_num = data & 0xF;
            break;
        case GE_CMD_VIEWMATRIXDATA:
            {   // PSPRECOMP_VM_LOG=<n>: log the list address of the first view-matrix word (camera hunt)
                static int vm_left = std::getenv("PSPRECOMP_VM_LOG") ? std::atoi(std::getenv("PSPRECOMP_VM_LOG")) : 0;
                if (vm_left > 0 && data_to_float(data) != 0.f && data_to_float(data) != 1.f) {
                    vm_left--;
                    std::fprintf(stderr, "[VM] list word @%08X = %08X (%g) idx=%u\n", g_ge_cur_pc, data, data_to_float(data), (unsigned)g_ge_state.view_mtx_num);
                }
            }
            if (g_ge_state.view_mtx_num < 12) {
                g_ge_state.view_matrix[
                    g_ge_state.view_mtx_num++] =
                    data_to_float(data);
            }
            break;

        case GE_CMD_PROJMATRIXNUMBER:
            g_ge_state.proj_mtx_num = data & 0xF;
            break;
        case GE_CMD_PROJMATRIXDATA:
            if (g_ge_state.proj_mtx_num < 16) {
                g_ge_state.proj_matrix[
                    g_ge_state.proj_mtx_num++] =
                    data_to_float(data);
            }
            break;

        case GE_CMD_BONEMATRIXNUMBER:
            g_ge_state.bone_mtx_num = data & 0x7F;
            break;
        case GE_CMD_BONEMATRIXDATA:
            static const bool bone_trace = std::getenv("PSPRECOMP_BONE_TRACE") != nullptr;
            if (bone_trace) {
                static int bt = 0;
                if (bt++ < 600)
                    std::fprintf(stderr, "[BONE] idx=%u raw=0x%06X f=%g\n",
                        (unsigned)g_ge_state.bone_mtx_num, data & 0xFFFFFF, data_to_float(data));
            }
            if (g_ge_state.bone_mtx_num < 96) {
                g_ge_bone_slot_last[g_ge_state.bone_mtx_num / 12] = g_ge_skin_prim_seq;
                g_ge_state.bone_matrix[
                    g_ge_state.bone_mtx_num++] =
                    data_to_float(data);
            }
            break;

        case GE_CMD_TGENMATRIXNUMBER:
            g_ge_state.tgen_mtx_num = data & 0xF;
            break;
        case GE_CMD_TGENMATRIXDATA:
            if (g_ge_state.tgen_mtx_num < 12) {
                g_ge_state.tgen_matrix[
                    g_ge_state.tgen_mtx_num++] =
                    data_to_float(data);
            }
            break;

        // ---- Viewport ----
        case GE_CMD_VIEWPORTXSCALE:
            g_ge_state.viewport_x_scale = data_to_float(data);
            break;
        case GE_CMD_VIEWPORTYSCALE:
            g_ge_state.viewport_y_scale = data_to_float(data);
            break;
        case GE_CMD_VIEWPORTZSCALE:
            g_ge_state.viewport_z_scale = data_to_float(data);
            break;
        case GE_CMD_VIEWPORTXCENTER:
            g_ge_state.viewport_x_center = data_to_float(data);
            break;
        case GE_CMD_VIEWPORTYCENTER:
            g_ge_state.viewport_y_center = data_to_float(data);
            break;
        case GE_CMD_VIEWPORTZCENTER:
            g_ge_state.viewport_z_center = data_to_float(data);
            break;

        case GE_CMD_TEXSCALEU:
            g_ge_state.tex_scale_u = data_to_float(data);
            break;
        case GE_CMD_TEXSCALEV:
            g_ge_state.tex_scale_v = data_to_float(data);
            break;
        case GE_CMD_TEXOFFSETU:
            g_ge_state.tex_offset_u = data_to_float(data);
            break;
        case GE_CMD_TEXOFFSETV:
            g_ge_state.tex_offset_v = data_to_float(data);
            break;

        case GE_CMD_OFFSETX:
            g_ge_state.offset_x = data;
            break;
        case GE_CMD_OFFSETY:
            g_ge_state.offset_y = data;
            break;

        // ---- Texture state ----
        case GE_CMD_TEXADDR0: case GE_CMD_TEXADDR1:
        case GE_CMD_TEXADDR2: case GE_CMD_TEXADDR3:
        case GE_CMD_TEXADDR4: case GE_CMD_TEXADDR5:
        case GE_CMD_TEXADDR6: case GE_CMD_TEXADDR7:
            g_ge_state.tex_addr[cmd - GE_CMD_TEXADDR0] = data;
            break;

        case GE_CMD_TEXBUFWIDTH0: case GE_CMD_TEXBUFWIDTH1:
        case GE_CMD_TEXBUFWIDTH2: case GE_CMD_TEXBUFWIDTH3:
        case GE_CMD_TEXBUFWIDTH4: case GE_CMD_TEXBUFWIDTH5:
        case GE_CMD_TEXBUFWIDTH6: case GE_CMD_TEXBUFWIDTH7:
            g_ge_state.tex_bufw[cmd - GE_CMD_TEXBUFWIDTH0] = data;
            break;

        case GE_CMD_TEXSIZE0: case GE_CMD_TEXSIZE1:
        case GE_CMD_TEXSIZE2: case GE_CMD_TEXSIZE3:
        case GE_CMD_TEXSIZE4: case GE_CMD_TEXSIZE5:
        case GE_CMD_TEXSIZE6: case GE_CMD_TEXSIZE7:
            g_ge_state.tex_size[cmd - GE_CMD_TEXSIZE0] = data;
            break;

        case GE_CMD_TEXFORMAT:
            g_ge_state.tex_format = data & 0xF;
            break;
        case GE_CMD_TEXMODE:
            g_ge_state.tex_mode = data;
            break;
        case GE_CMD_TEXFUNC:
            g_ge_state.tex_func = data & 0x7;
            g_ge_state.tex_func_rgba = (data & 0x100) != 0;
            g_ge_state.tex_func_double = (data & 0x10000) != 0;
            break;
        case GE_CMD_TEXFILTER:
            g_ge_state.tex_filter = data;
            break;
        case GE_CMD_TEXWRAP:
            g_ge_state.tex_wrap = data;
            break;
        case GE_CMD_TEXENVCOLOR:
            g_ge_state.tex_env_color = data;
            break;
        case GE_CMD_TEXFLUSH:
        case GE_CMD_TEXSYNC:
            break;  // No-op for our implementation

        // ---- CLUT ----
        case GE_CMD_CLUTADDR:
            g_ge_state.clut_addr = data;
            break;
        case GE_CMD_CLUTADDRUPPER:
            g_ge_state.clut_addr_upper = data;
            break;
        case GE_CMD_CLUTFORMAT:
            g_ge_state.clut_format = data;
            break;
        case GE_CMD_LOADCLUT:
            // Trigger CLUT load (texture cache handles this)
            break;

        // ---- Enable flags ----
        case GE_CMD_TEXTUREMAPENABLE:
            g_ge_state.texture_enable = (data & 1) != 0;
            break;
        case GE_CMD_LIGHTINGENABLE:
            g_ge_state.lighting_enable = (data & 1) != 0;
            break;
        case GE_CMD_ALPHABLENDENABLE:
            g_ge_state.alpha_blend_enable = (data & 1) != 0;
            break;
        case GE_CMD_ALPHATESTENABLE:
            g_ge_state.alpha_test_enable = (data & 1) != 0;
            break;
        case GE_CMD_ZTESTENABLE:
            g_ge_state.depth_test_enable = (data & 1) != 0;
            break;
        case GE_CMD_CULLFACEENABLE:
            g_ge_state.cull_enable = (data & 1) != 0;
            break;
        case GE_CMD_FOGENABLE:
            g_ge_state.fog_enable = (data & 1) != 0;
            break;
        case GE_CMD_STENCILTESTENABLE:
            g_ge_state.stencil_test_enable = (data & 1) != 0;
            break;
        case GE_CMD_COLORTESTENABLE:
            g_ge_state.color_test_enable = (data & 1) != 0;
            break;

        // Ignored enable flags (no effect in Phase 5)
        case GE_CMD_LIGHTENABLE0: case GE_CMD_LIGHTENABLE1:
        case GE_CMD_LIGHTENABLE2: case GE_CMD_LIGHTENABLE3:
            g_ge_state.light_enable[cmd - GE_CMD_LIGHTENABLE0] = (data & 1) != 0;
            break;
        case GE_CMD_DEPTHCLAMPENABLE:
        case GE_CMD_DITHERENABLE:
        case GE_CMD_ANTIALIASENABLE:
        case GE_CMD_PATCHCULLENABLE:
        case GE_CMD_LOGICOPENABLE:
            break;

        // ---- Render state ----
        case GE_CMD_CLEARMODE:
            g_ge_state.clear_mode = (data & 1) != 0;
            g_ge_state.clear_flags = (data >> 8) & 0x7;
            break;

        case GE_CMD_BLENDMODE:
            g_ge_state.blend_mode = data;
            break;
        case GE_CMD_BLENDFIXEDA:
            g_ge_state.blend_fix_a = data;
            break;
        case GE_CMD_BLENDFIXEDB:
            g_ge_state.blend_fix_b = data;
            break;

        case GE_CMD_ALPHATEST:
            g_ge_state.alpha_test = data;
            break;
        case GE_CMD_STENCILTEST:
            g_ge_state.stencil_test = data;
            break;
        case GE_CMD_STENCILOP:
            g_ge_state.stencil_op = data;
            break;
        case GE_CMD_ZTEST:
            g_ge_state.depth_func = data & 0x7;
            break;
        case GE_CMD_ZWRITEDISABLE:
            g_ge_state.depth_write_disable = (data & 1) != 0;
            break;
        case GE_CMD_CULL:
            g_ge_state.cull_face = data & 1;
            break;

        // ---- Framebuffer ----
        case GE_CMD_FRAMEBUFPTR:
            g_ge_state.framebuf_ptr = data;
            ge_draw_note_framebufptr();
            // Bind-on-FRAMEBUFPTR (issue #59): a list that switches its
            // render target mid-stream (clear buffer A then draw buffer B)
            // must composite into the right per-address FBO. Cheap (a bind).
            ge_draw_select_target(g_ge_state.framebuf_ptr);
            break;
        case GE_CMD_FRAMEBUFWIDTH:
            g_ge_state.framebuf_width = data;
            break;
        case GE_CMD_FRAMEBUFPIXFORMAT:
            g_ge_state.framebuf_format = data & 0x3;
            break;

        // ---- Material / ambient ----
        case GE_CMD_SHADEMODE:
            g_ge_state.shade_mode = data & 1;
            break;
        case GE_CMD_AMBIENTCOLOR:
            g_ge_state.ambient_color = data;
            break;
        case GE_CMD_AMBIENTALPHA:
            g_ge_state.ambient_alpha = data;
            break;
        case GE_CMD_MATERIALEMISSIVE:
            g_ge_state.material_emissive = data;
            break;
        case GE_CMD_MATERIALAMBIENT:
            g_ge_state.material_ambient = data;
            break;
        case GE_CMD_MATERIALALPHA:
            g_ge_state.material_alpha = data & 0xFF;
            break;
        case GE_CMD_MATERIALDIFFUSE:
            g_ge_state.material_diffuse = data;
            break;

        // ---- Fog ----
        case GE_CMD_FOGCOLOR:
            g_ge_state.fog_color = data;
            break;
        case GE_CMD_FOG1:
        case GE_CMD_FOG2:
            break;  // Store if needed later

        // ---- Region / scissor ----
        case GE_CMD_REGION1:
            g_ge_state.region1 = data;
            break;
        case GE_CMD_REGION2:
            g_ge_state.region2 = data;
            break;
        case GE_CMD_SCISSOR1:
            g_ge_state.scissor1 = data;
            break;
        case GE_CMD_SCISSOR2:
            g_ge_state.scissor2 = data;
            break;
        case GE_CMD_MINZ:
            g_ge_state.min_z = data & 0xFFFFu;
            break;
        case GE_CMD_MAXZ:
            g_ge_state.max_z = data & 0xFFFFu;
            break;

        // ---- Depth / Z buffer ----
        case GE_CMD_ZBUFPTR:
        case GE_CMD_ZBUFWIDTH:
            break;  // Not needed for Phase 5

        // ---- Mask ----
        case GE_CMD_MASKRGB:
            g_ge_state.mask_rgb = data & 0xFFFFFFu;
            break;
        case GE_CMD_MASKALPHA:
            g_ge_state.mask_alpha = data & 0xFFu;
            break;

        // ---- Color test ----
        case GE_CMD_COLORTEST:
            g_ge_state.color_test = data;
            break;
        case GE_CMD_COLORREF:
            g_ge_state.color_ref = data & 0xFFFFFFu;
            break;
        case GE_CMD_COLORTESTMASK:
            g_ge_state.color_test_mask = data & 0xFFFFFFu;
            break;

        // ---- Misc ignored for Phase 5 ----
        case GE_CMD_MORPHWEIGHT0: case GE_CMD_MORPHWEIGHT1:
        case GE_CMD_MORPHWEIGHT2: case GE_CMD_MORPHWEIGHT3:
        case GE_CMD_MORPHWEIGHT4: case GE_CMD_MORPHWEIGHT5:
        case GE_CMD_MORPHWEIGHT6: case GE_CMD_MORPHWEIGHT7:
        case GE_CMD_PATCHDIVISION: case GE_CMD_PATCHPRIMITIVE:
        case GE_CMD_PATCHFACING:
        case GE_CMD_BEZIER: case GE_CMD_SPLINE:
        case GE_CMD_BOUNDINGBOX:
            break;  // ignored (must not fall into MATERIALSPECULAR)
        case GE_CMD_MATERIALSPECULAR:
            g_ge_state.material_specular = data;
            break;
        case GE_CMD_MATERIALUPDATE:
            g_ge_state.material_update = data & 7;
            break;
        case GE_CMD_LIGHTMODE:
            g_ge_state.light_mode = data & 1;
            break;
        case GE_CMD_MATERIALSPECULARCOEF:
        case GE_CMD_REVERSENORMAL:
        case GE_CMD_TEXMAPMODE: g_ge_state.tex_map_mode = data & 0xFFFFFF; break;
        case GE_CMD_TEXSHADELS: g_ge_state.tex_shade_ls = data & 0xFFFFFF; break;
        case GE_CMD_TEXLEVEL: case GE_CMD_TEXLODSLOPE:
        case GE_CMD_LOGICOP:
        case GE_CMD_DITH0: case GE_CMD_DITH1:
        case GE_CMD_DITH2: case GE_CMD_DITH3:
            break;  // ignored (must not fall into TRANSFERSRC)
        case GE_CMD_TRANSFERSRC:    g_ge_state.xfer_src = data; break;
        case GE_CMD_TRANSFERSRCW:   g_ge_state.xfer_src_w = data; break;
        case GE_CMD_TRANSFERDST:    g_ge_state.xfer_dst = data; break;
        case GE_CMD_TRANSFERDSTW:   g_ge_state.xfer_dst_w = data; break;
        case GE_CMD_TRANSFERSRCPOS: g_ge_state.xfer_src_pos = data; break;
        case GE_CMD_TRANSFERDSTPOS: g_ge_state.xfer_dst_pos = data; break;
        case GE_CMD_TRANSFERSIZE:   g_ge_state.xfer_size = data; break;
        case GE_CMD_TRANSFERSTART: {
            ge_draw_flush();  // the copy may overwrite textures/vertices pending draws use
            // Block transfer (sceGuCopyImage): rectangle copy between guest
            // memory areas, 16 or 32 bpp (PPSSPP GPUCommon::DoBlockTransfer).
            // This is how games upload textures / vertex data into VRAM.
            const GeState& g = g_ge_state;
            const uint32_t sbase = ((g.xfer_src & 0xFFFFF0u) | ((g.xfer_src_w & 0xFF0000u) << 8)) & 0x0FFFFFFFu;
            const uint32_t dbase = ((g.xfer_dst & 0xFFFFF0u) | ((g.xfer_dst_w & 0xFF0000u) << 8)) & 0x0FFFFFFFu;
            const uint32_t sstride = g.xfer_src_w & 0x3FFu, dstride = g.xfer_dst_w & 0x3FFu;
            const uint32_t sx = g.xfer_src_pos & 0x3FFu, sy = (g.xfer_src_pos >> 10) & 0x3FFu;
            const uint32_t dx = g.xfer_dst_pos & 0x3FFu, dy = (g.xfer_dst_pos >> 10) & 0x3FFu;
            const uint32_t w = (g.xfer_size & 0x3FFu) + 1, h = ((g.xfer_size >> 10) & 0x3FFu) + 1;
            const uint32_t bpp = (data & 1) ? 4 : 2;
            static int xfer_n = 0;
            if (xfer_n++ < 24)
                std::fprintf(stderr, "[GE_XFER] 0x%08X(%u,%u,stride %u) -> 0x%08X(%u,%u,stride %u) %ux%u bpp=%u\n",
                             sbase, sx, sy, sstride, dbase, dx, dy, dstride, w, h, bpp);
            if ((dbase & 0x0F000000u) != 0x04000000u) {
                static int nv_n = 0;
                if (nv_n++ < 200)
                    std::fprintf(stderr, "[GE_XFER_NONVRAM] 0x%08X(%u,%u,stride %u) -> 0x%08X(%u,%u,stride %u) %ux%u bpp=%u\n",
                                 sbase, sx, sy, sstride, dbase, dx, dy, dstride, w, h, bpp);
            }
            for (uint32_t row = 0; row < h; row++) {
                const uint32_t s = sbase + ((sy + row) * sstride + sx) * bpp;
                const uint32_t d = dbase + ((dy + row) * dstride + dx) * bpp;
                for (uint32_t k = 0; k < w * bpp; k++)
                    psp_mem_write<uint8_t>(rdram, d + k, psp_mem_read<uint8_t>(rdram, s + k));
            }
            break;
        }

        // ---- Light types / positions / directions / attenuation / colours ----
        case GE_CMD_LIGHTTYPE0: case GE_CMD_LIGHTTYPE1:
        case GE_CMD_LIGHTTYPE2: case GE_CMD_LIGHTTYPE3:
            g_ge_state.light_type[cmd - GE_CMD_LIGHTTYPE0] = data;
            break;
        case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68:
        case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: {
            const int k = cmd - 0x63;  // light = k/3, axis = k%3
            g_ge_state.light_pos[k / 3][k % 3] = data_to_float(data);
            break;
        }
        case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74:
        case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: {
            const int k = cmd - 0x6F;
            g_ge_state.light_dir[k / 3][k % 3] = data_to_float(data);
            break;
        }
        case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x80:
        case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: {
            const int k = cmd - 0x7B;
            g_ge_state.light_att[k / 3][k % 3] = data_to_float(data);
            break;
        }
        case GE_CMD_LKS0: case GE_CMD_LKS1: case GE_CMD_LKS2: case GE_CMD_LKS3:
            g_ge_state.light_spot_exp[cmd - GE_CMD_LKS0] = data_to_float(data);
            break;
        case GE_CMD_LKO0: case GE_CMD_LKO1: case GE_CMD_LKO2: case GE_CMD_LKO3:
            g_ge_state.light_spot_cut[cmd - GE_CMD_LKO0] = data_to_float(data);
            break;
        case 0x8F: case 0x92: case 0x95: case 0x98:
            g_ge_state.light_amb[(cmd - 0x8F) / 3] = data & 0xFFFFFF;
            break;
        case 0x90: case 0x93: case 0x96: case 0x99:
            g_ge_state.light_dif[(cmd - 0x90) / 3] = data & 0xFFFFFF;
            break;
        case 0x91: case 0x94: case 0x97: case 0x9A:
            g_ge_state.light_spc[(cmd - 0x91) / 3] = data & 0xFFFFFF;
            break;

        default:
            // Log-once for truly unknown commands
            if (!g_ge_state.cmd_warned[cmd]) {
                ge_nav_dump(pc);
                std::fprintf(stderr,
                    "[GE] Unknown cmd 0x%02X data=0x%06X "
                    "at pc=0x%08X\n",
                    cmd, data, pc);
                g_ge_state.cmd_warned[cmd] = true;
            }
            break;
        }

        pc += 4;
        cmd_count++;
    }

    if (cmd_count >= GE_MAX_COMMANDS) {
        std::fprintf(stderr,
            "[GE] Hit command limit (%d) without END "
            "at pc=0x%08X\n",
            GE_MAX_COMMANDS, pc);
    }

    g_total_lists++;
    return GeListResult{pc, false, prim_count};
}
