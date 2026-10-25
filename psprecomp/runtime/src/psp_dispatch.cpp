#include "psp_scheduler.h"
#include <cstring>
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>

/// Cached STRICT mode flag — checked once on first call.
static bool g_strict_mode = false;
static bool g_strict_checked = false;

/// Per-address invocation counter for LOOKUP_MISS diagnostics.
/// Logs each unique address on first hit only to reduce noise.
static std::unordered_map<uint32_t, int> g_miss_counts;

/// Last function address from psp_trace_checkpoint (defined below, always recorded).
thread_local uint32_t g_last_func_addr = 0;

/// No-op stub returned when a lookup miss occurs in non-STRICT mode.
/// Sets v0 (ctx->r[2]) to 0 for deterministic return value behavior.
static thread_local uint32_t g_last_miss_addr = 0;

// [V438] ring buffer of recent function entries (poor-man's backtrace).
// Declared here so noop_stub (below) can read it; written by
// psp_trace_checkpoint. Cheap: one array write per checkpoint.
thread_local uint32_t g_func_ring[32] = {0};
thread_local uint32_t g_func_ring_pos = 0;

// [#35] Cross-thread copy of the dispatched-function ring for the debug
// socket's I command. The per-thread g_func_ring above is thread_local and
// unreadable from the socket thread, so psp_trace_checkpoint also appends
// to this shared ring with relaxed atomics (interleaves all game threads;
// ordering across threads is approximate -- diagnostics only).
static std::atomic<uint32_t> g_shared_func_ring[64];
static std::atomic<uint32_t> g_shared_func_ring_pos{0};

// [#35] LOOKUP_MISS counters readable from the debug socket thread.
// The g_miss_counts map below is mutated without a lock from game threads,
// so the socket must NOT iterate it (rehash mid-read). These atomics carry
// the two numbers the I command needs.
static std::atomic<uint32_t> g_miss_unique{0};
static std::atomic<uint64_t> g_miss_total{0};

void psp_dispatch_get_miss_stats(uint32_t* unique_addrs,
                                 uint64_t* total_calls) {
    if (unique_addrs)
        *unique_addrs = g_miss_unique.load(std::memory_order_relaxed);
    if (total_calls)
        *total_calls = g_miss_total.load(std::memory_order_relaxed);
}

int psp_dispatch_get_recent_funcs(uint32_t* out, int max) {
    uint32_t pos = g_shared_func_ring_pos.load(std::memory_order_relaxed);
    int n = 0;
    // Oldest first: walk forward from the slot the next write would claim.
    for (int i = 0; i < 64 && n < max; i++) {
        uint32_t v = g_shared_func_ring[(pos + static_cast<uint32_t>(i)) & 63u]
                         .load(std::memory_order_relaxed);
        if (v) out[n++] = v;
    }
    return n;
}

// Game-module miss handler (#47 P5 seam): everything address-keyed that
// used to live inline here (Patapon's IO-slot dump, [BND_VTABLE_MISS]
// punch list, the 0x438 corrupt-vtable workaround) registers through this
// slot from the game module's register_hooks. nullptr = no handler.
static PspLookupMissHandler g_miss_handler = nullptr;

void psp_dispatch_set_miss_handler(PspLookupMissHandler fn) {
    g_miss_handler = fn;
}

static PspOverlayResolver g_overlay_resolver = nullptr;

void psp_dispatch_set_overlay_resolver(PspOverlayResolver fn) {
    g_overlay_resolver = fn;
}

static void noop_stub(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = g_last_miss_addr;
    int& c = g_miss_counts[addr];
    if (c <= 5) {
        std::fprintf(stderr,
            "[LOOKUP_MISS_CTX] addr=0x%08X caller=0x%08X a0=0x%08X a1=0x%08X sp=0x%08X"
            " r16=0x%08X r17=0x%08X r21=0x%08X\n",
            addr,
            g_last_func_addr,
            static_cast<uint32_t>(ctx->r[4]),
            static_cast<uint32_t>(ctx->r[5]),
            static_cast<uint32_t>(ctx->r[29]),
            static_cast<uint32_t>(ctx->r[16]),
            static_cast<uint32_t>(ctx->r[17]),
            static_cast<uint32_t>(ctx->r[21]));
    }

    // Game-module miss handler (#47 P5 seam) — may log address-keyed
    // diagnostics and apply title-specific workarounds before the generic
    // deterministic return value below.
    if (g_miss_handler != nullptr) {
        g_miss_handler(rdram, ctx, addr, c);
    }

    ctx->r[2] = 0;
}
static bool g_miss_atexit_registered = false;

/// Dump all LOOKUP_MISS addresses sorted by count at program exit.
/// Output format is suitable for feeding back into Ghidra analysis
/// as "force function start" hints for future dispatch table expansion.
static void psp_dump_lookup_misses() {
    if (g_miss_counts.empty()) return;

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_miss_counts.begin(), g_miss_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    int total_misses = 0;
    for (const auto& [addr, count] : sorted) {
        total_misses += count;
    }

    std::fprintf(stderr,
        "\n[LOOKUP_MISS_SUMMARY] %zu unique addresses, "
        "%d total calls\n",
        sorted.size(),
        total_misses);

    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "  0x%08X (count=%d)\n", addr, count);
    }

    std::fprintf(stderr,
        "[LOOKUP_MISS_SUMMARY] END (%zu addresses)\n",
        sorted.size());
}

// [#47 P1] Probe mode: psp_dispatch_probe_lookup() resolves an address
// WITHOUT engaging the miss machinery — no log, no counters, no STRICT
// abort — and yields nullptr when the address is absent. Used at boot
// (single-threaded, right after psp_init_dispatch_table()) to resolve the
// guest functions that runtime hooks wrap, replacing the former link-time
// `extern FUN_*` references into the generated output. Not thread-safe by
// design; boot-time only.
static bool g_probe_lookup = false;

FuncPtr psp_dispatch_probe_lookup(uint32_t vaddr) {
    g_probe_lookup = true;
    FuncPtr fn = RECOMP_LOOKUP(vaddr);
    g_probe_lookup = false;
    return fn;
}

/// Called by RECOMP_LOOKUP (in dispatch.cpp) when an address is not in the
/// dispatch table. Logs each unique miss on first hit and returns a noop
/// stub or aborts if PSPRECOMP_STRICT=1.
FuncPtr psp_on_lookup_miss(uint32_t vaddr) {
    // Overlay seam: a resident overlay's function set answers before this
    // counts as a miss (see psp_dispatch_set_overlay_resolver).
    if (g_overlay_resolver) {
        if (FuncPtr fn = g_overlay_resolver(vaddr)) return fn;
    }
    // Probe mode (#47 P1): report "absent" to psp_dispatch_probe_lookup
    // without any side effects.
    if (g_probe_lookup) {
        return nullptr;
    }

    // One-time check for STRICT mode environment variable
    if (!g_strict_checked) {
        const char* env = std::getenv("PSPRECOMP_STRICT");
        g_strict_mode = (env && env[0] == '1');
        g_strict_checked = true;
    }

    // Register atexit handler on first miss
    if (!g_miss_atexit_registered) {
        g_miss_atexit_registered = true;
        std::atexit(psp_dump_lookup_misses);
    }

    // Per-address miss counting -- log on first hit only
    int& count = g_miss_counts[vaddr];
    count++;
    g_miss_total.fetch_add(1, std::memory_order_relaxed);
    if (count == 1) {
        g_miss_unique.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
            "[LOOKUP_MISS] addr=0x%08X (first hit)\n", vaddr);
    }

    // Store for noop_stub context logging
    g_last_miss_addr = vaddr;

    // STRICT mode: abort on any lookup miss (RUNTIME-11)
    if (g_strict_mode) {
        std::fprintf(stderr,
            "STRICT: aborting on LOOKUP_MISS 0x%08X\n", vaddr);
        std::abort();
    }

    return noop_stub;
}

// Forward declaration for atexit registration
static void psp_dump_pc_trace();

/// PC tracing checkpoint — called at every generated function entry.
/// When PSPRECOMP_PC_TRACE=1, logs function address with frequency counting.
/// No-op when env var is absent or not "1" (single branch cost).
static bool g_pc_trace = false;
static bool g_pc_trace_checked = false;
static std::unordered_map<uint32_t, int> g_func_counts;
static std::mutex g_pc_trace_mutex;
static int g_total_entries = 0;

// Previous dispatched-function address, kept per thread so game-module
// diagnostics (games/<name>/hooks_dispatch.cpp) can report caller chains.
thread_local uint32_t g_prev_func_addr = 0;

// PSPRECOMP_MEM_WATCH=<hex addr>: at every function entry, check that guest
// word and, when it changes, log the old/new value with the last function
// entries (poor-man's "who wrote this"). Needs g_dbg_rdram (set by the GE
// thread on its first list; defined in psp_ge.cpp).
extern uint8_t* g_dbg_rdram;
static uint32_t g_mem_watch_addr = 0;
static bool g_mem_watch_checked = false;
static uint32_t g_mem_watch_last = 0xDEADBEEFu;
static int g_mem_watch_n = 0;

// Debug socket `M <hex addr>`: (re)arm the MEM_WATCH at run time. Heap addresses differ from run to run, so
// the watch address usually has to be located in the running game first.
void psp_mem_watch_set(uint32_t addr) {
    g_mem_watch_checked = true;
    g_mem_watch_addr = addr;
    g_mem_watch_n = 0;
    if (g_dbg_rdram) std::memcpy(&g_mem_watch_last, g_dbg_rdram + (addr & 0x07FFFFFFu), 4);
}

void psp_dump_func_ring(const char* tag) {
    std::fprintf(stderr, "[%s] ring:", tag);
    for (int k = 1; k <= 12; k++)
        std::fprintf(stderr, " %08X", g_func_ring[(g_func_ring_pos - k) & 31u]);
    std::fprintf(stderr, "\n");
}

void psp_trace_checkpoint(uint32_t addr) {
    if (!g_mem_watch_checked) {
        g_mem_watch_checked = true;
        if (const char* e = std::getenv("PSPRECOMP_MEM_WATCH"))
            g_mem_watch_addr = static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
    }
    if (g_mem_watch_addr && g_dbg_rdram && g_mem_watch_n < 400) {
        uint32_t cur;
        std::memcpy(&cur, g_dbg_rdram + (g_mem_watch_addr & 0x07FFFFFFu), 4);
        static const char* hi_env = std::getenv("PSPRECOMP_MEM_WATCH_HI");  // only log when new value's top byte == this (hex)
        const bool hi_ok = !hi_env || (cur >> 24) == (uint32_t)std::strtoul(hi_env, nullptr, 16);
        if (cur != g_mem_watch_last && !hi_ok) g_mem_watch_last = cur;
        if (cur != g_mem_watch_last) {
            g_mem_watch_n++;
            std::fprintf(stderr, "[MEM_WATCH] %08X: %08X -> %08X  entering=0x%08X ring:",
                g_mem_watch_addr, g_mem_watch_last, cur, addr);
            for (int k = 1; k <= 32; k++)
                std::fprintf(stderr, " %08X", g_func_ring[(g_func_ring_pos - k) & 31u]);
            std::fprintf(stderr, "\n");
            g_mem_watch_last = cur;
        }
    }

    // PSPRECOMP_LKP_LOG=1: log unique (container,id) lookups made by Dissidia FUN_0884808C (116-byte record table search)
    {
        static int on = -1; static int n = 0;
        if (on < 0) on = std::getenv("PSPRECOMP_LKP_LOG") ? 1 : 0;
        if (on && addr == 0x0884808Cu && g_dbg_rdram && n < 3000) {
            if (PspThread* t = sched_current_thread()) {
                auto rw = [&](uint32_t a) { uint32_t w = 0; if ((a & 0x0F000000u) == 0x08000000u || (a & 0x0F000000u) == 0x09000000u) std::memcpy(&w, g_dbg_rdram + (a & 0x07FFFFFFu), 4); return w; };
                uint32_t a0 = (uint32_t)t->ctx.r[4], id = (uint32_t)t->ctx.r[5];
                uint32_t tab = rw(a0), cnt = rw(tab + 4);
                static std::unordered_map<uint64_t, int> seen;
                uint64_t key = ((uint64_t)a0 << 32) | (id & 0xFFFF) | ((uint64_t)(cnt & 0xFFFF) << 16);
                if (seen.emplace(key, 1).second) {
                    n++;
                    std::fprintf(stderr, "[LKP] cont=%08X tab=%08X cnt=%u id=%u ids:", a0, tab, cnt, id);
                    for (uint32_t i = 0; i < cnt && i < 24; i++) {
                        uint32_t w = rw(tab + 8 + i * 116); std::fprintf(stderr, " %u", w & 0xFFFF);
                    }
                    std::fprintf(stderr, " caller=%08X\n", g_prev_func_addr);
                }
            }
        }
    }

    // PSPRECOMP_ARG_WATCH=<hex func>[,<hex off>]: log a0..a3 (+ the guest word at a2+off) at entry.
    {
        static uint32_t aw_addr = 0, aw_off = 0; static bool aw_init = false; static int aw_n = 0;
        if (!aw_init) {
            aw_init = true;
            if (const char* e = std::getenv("PSPRECOMP_ARG_WATCH")) {
                aw_addr = (uint32_t)std::strtoul(e, nullptr, 16);
                if (const char* c = std::strchr(e, ',')) aw_off = (uint32_t)std::strtoul(c + 1, nullptr, 16);
            }
        }
        if (aw_addr && addr == aw_addr && g_dbg_rdram && aw_n < 4000) {
            if (PspThread* t = sched_current_thread()) {
                uint32_t a2 = (uint32_t)t->ctx.r[6], w = 0;
                static std::unordered_map<uint32_t, uint32_t> aw_last;
                if ((a2 & 0x0F000000u) == 0x08000000u || (a2 & 0x0F000000u) == 0x09000000u) std::memcpy(&w, g_dbg_rdram + ((a2 + aw_off) & 0x07FFFFFFu), 4);
                static const bool aw_all = std::getenv("PSPRECOMP_ARG_WATCH_ALL") != nullptr;   // log every entry, no dedupe
                auto it = aw_last.find(a2);
                if (!aw_all && it != aw_last.end() && it->second == w) goto aw_skip;
                aw_last[a2] = w; aw_n++;
                std::fprintf(stderr, "[ARG_WATCH] %08X a0=%08X a1=%08X a2=%08X a3=%08X [a2+%X]=%08X\n", addr, (uint32_t)t->ctx.r[4], (uint32_t)t->ctx.r[5], a2, (uint32_t)t->ctx.r[7], aw_off, w);
                aw_skip:;
            }
        }
    }

    if (!g_pc_trace_checked) {
        const char* env = std::getenv("PSPRECOMP_PC_TRACE");
        g_pc_trace = (env && env[0] == '1');
        g_pc_trace_checked = true;
        if (g_pc_trace) {
            std::atexit(psp_dump_pc_trace);
        }
    }
    g_prev_func_addr = g_last_func_addr;
    g_last_func_addr = addr;
    g_func_ring[(g_func_ring_pos++) & 31u] = addr;
    // [#35] shared (cross-thread) ring for the debug socket I command.
    // One relaxed fetch_add + store per function entry; measured noise is
    // acceptable for a diagnostics-first runtime.
    g_shared_func_ring[g_shared_func_ring_pos.fetch_add(
        1, std::memory_order_relaxed) & 63u]
        .store(addr, std::memory_order_relaxed);

    // PSPRECOMP_FIRSTCALL=<hex gate addr>:<out path>: first-call / call-count log of every function entered
    // while the guest byte at the gate address is non-zero (set it with the debug socket `W <addr> 01`; the value is
    // the phase number, 1..15). Writes to its own file (the stderr log is capped): `F <phase> <addr> prev=<caller>` on the
    // first call of a function in a phase, and `C <phase> <addr> <count>` for every function when the phase changes
    // or the gate drops to 0. Diff two runs (retail vs extra actor) to find the first function that only one of them calls.
    {
        static int fc_init = 0; static uint32_t fc_gate = 0; static FILE* fc_out = nullptr;
        static std::mutex fc_mx; static std::unordered_map<uint32_t, uint32_t> fc_cnt[16]; static uint32_t fc_phase = 0;
        if (!fc_init) {
            fc_init = 1;
            if (const char* e = std::getenv("PSPRECOMP_FIRSTCALL")) {
                fc_gate = (uint32_t)std::strtoul(e, nullptr, 16);
                if (const char* c = std::strchr(e, ':')) fc_out = std::fopen(c + 1, "w");
                if (!fc_out) fc_gate = 0;
            }
        }
        if (fc_gate && g_dbg_rdram) {
            uint32_t ph = g_dbg_rdram[fc_gate & 0x07FFFFFFu] & 15u;
            if (ph || fc_phase) {
                std::lock_guard<std::mutex> lk(fc_mx);
                if (ph != fc_phase) {
                    if (fc_phase) {
                        for (auto& kv : fc_cnt[fc_phase]) std::fprintf(fc_out, "C %u %08X %u\n", fc_phase, kv.first, kv.second);
                        std::fflush(fc_out);
                    }
                    fc_phase = ph;
                }
                if (ph) {
                    uint32_t& n = fc_cnt[ph][addr];
                    if (n++ == 0) std::fprintf(fc_out, "F %u %08X prev=%08X\n", ph, addr, g_prev_func_addr);
                }
            }
        }
    }

    // (The PSPRECOMP_SPLEAK shadow-stack sp-leak detector that hung here was
    // a game-specific investigation probe — deleted in #47 Phase 5.)
    if (!g_pc_trace) return;
    g_total_entries++;

    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    int& c = g_func_counts[addr];
    c++;
    if (c <= 3 || c % 100000 == 0) {
        std::fprintf(stderr,
            "[PC-TRACE] func=0x%08X count=%d\n", addr, c);
    }
}

void psp_dump_func_counts(const char* path) {
    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    if (FILE* f = std::fopen(path, "w")) {
        for (auto& kv : g_func_counts) std::fprintf(f, "%08X %d\n", kv.first, kv.second);
        std::fclose(f);
    }
}

/// Dump the most-called functions sorted by count.
/// Called from a timer thread after a few seconds.
static void psp_dump_pc_trace() {
    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    std::fprintf(stderr,
        "\n[PC-TRACE] === DUMP (total entries=%d, last=0x%08X) ===\n",
        g_total_entries, g_last_func_addr);

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_func_counts.begin(), g_func_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    int shown = 0;
    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "[PC-TRACE]   0x%08X  calls=%d\n", addr, count);
        if (++shown >= 20) break;
    }
    std::fprintf(stderr, "[PC-TRACE] === END DUMP ===\n");
}
