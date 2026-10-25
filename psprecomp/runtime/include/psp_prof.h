#pragma once
// Env-gated renderer profiler (PSPRECOMP_PROF=1). Prints a [PROF] line every
// time ge_present_frame's 5 s [FPS] window closes. When disabled each scope
// costs one predictable branch. Single-writer (render/main thread) counters.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <string>
#include <map>

#ifdef __APPLE__
#include <pthread.h>
#else
#include <sys/syscall.h>
#endif
namespace psp_prof {
inline long gettid_() {
#ifdef __APPLE__
    uint64_t tid = 0; pthread_threadid_np(nullptr, &tid); return (long)tid;
#else
    return syscall(SYS_gettid);
#endif
}

enum Slot {
    S_PRIM = 0,     // whole ge_draw_prim
    S_DECODE,       // ge_decode_vertices
    S_XFORM,        // ge_transform_vertices (matrix, lighting, skinning is in decode)
    S_PACK,         // pack into PackedVertex
    S_UPLOAD,       // VBO upload
    S_UNIFORM,      // shader use + uniforms
    S_TEX,          // texture bind (lookup / decode / upload)
    S_GLSTATE,      // blend/depth/cull/mask GL state
    S_DRAW,         // glDrawArrays
    S_CLEAR,        // clear-mode prims
    S_LIST,         // ge_process_display_list (everything GE on the main thread)
    S_PRESENT,      // ge_present_frame (blit + swap)
    S_MPEG_AVC,     // sceMpeg H.264 decode (libavcodec)
    S_MPEG_CSC,     // sceMpeg YUV->RGBA
    S_MPEG_WR,      // sceMpeg frame write into guest memory
    S_MPEG_AUD,     // sceMpeg ATRAC3+ decode
    S_N
};
static const char* const k_names[S_N] = {
    "prim", "decode", "xform", "pack", "upload", "uniform", "tex", "glstate",
    "draw", "clear", "list", "present", "mpegavc", "mpegcsc", "mpegwr", "mpegaud"};

inline const bool enabled = std::getenv("PSPRECOMP_PROF") != nullptr;
inline uint64_t ns[S_N] = {};
inline uint64_t draws = 0, verts = 0, tex_binds = 0, tex_miss = 0, tex_bytes = 0, clears = 0;
inline uint64_t cmds = 0, batches = 0;
inline uint64_t fb_snaps = 0, fb_snap_px = 0;   // feedback snapshots (whole bound target copied before a draw that samples it)
inline uint64_t movie_binds = 0, movie_uploads = 0, movie_ns = 0;   // movie texture fast path
// draw categories: 0 through, 1 3D plain, 2 3D skinned, 3 3D lit (any), 4 lit+skinned
inline uint64_t cat_draws[5] = {}, cat_verts[5] = {};
inline uint64_t same_tex_draws = 0;
enum { G_WORLD, G_VIEW, G_PROJ, G_BONES, G_TEX, G_TEXFN, G_BLEND, G_DEPTH, G_LIGHT, G_VTYPE, G_VP, G_UVX, G_N };
inline uint64_t grp_changes[G_N] = {};
inline const char* const k_grp[G_N] = {"world","view","proj","bones","tex","texfn","blend","depth/cull/mask/atest","light/material","vtype","viewport","uvxf"};

inline uint64_t now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Scope {
    Slot s; uint64_t t0;
    explicit Scope(Slot slot) : s(slot), t0(enabled ? now() : 0) {}
    ~Scope() { if (enabled) ns[s] += now() - t0; }
};

// Event sequence trace (PSPRECOMP_PROF_SEQ=1): "[SEQ] <ms> <thread> <what>".
inline const bool seq_enabled = std::getenv("PSPRECOMP_PROF_SEQ") != nullptr;
inline void seq(const char* what, long arg = 0) {
    if (!seq_enabled) return;
    static const uint64_t t0 = now();
    std::fprintf(stderr, "[SEQ] %.3f t%ld %s %ld\n", (now() - t0) / 1e6,
                 (long)gettid_(), what, arg);
}
// Per-thread CPU time (jiffies) from /proc/self/task, delta since last report.
inline void thread_cpu_report(double dt) {
    static std::map<std::string, unsigned long> last;
    static long hz = sysconf(_SC_CLK_TCK);
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    std::string out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[128];
        std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        char buf[1024]; size_t n = std::fread(buf, 1, sizeof buf - 1, f); buf[n] = 0;
        std::fclose(f);
        char* lp = std::strchr(buf, '('); char* rp = std::strrchr(buf, ')');
        if (!lp || !rp) continue;
        std::string comm(lp + 1, rp - lp - 1);
        unsigned long ut = 0, st = 0;
        // fields after ')': state(3) ... utime(14) stime(15); 1st token after ')' is field 3
        char* p = rp + 2; int field = 3;
        char* tok = strtok(p, " ");
        while (tok) { if (field == 14) ut = strtoul(tok, nullptr, 10); if (field == 15) { st = strtoul(tok, nullptr, 10); break; } tok = strtok(nullptr, " "); field++; }
        std::string key = std::string(e->d_name) + ":" + comm;
        unsigned long tot = ut + st, prev = last[key];
        last[key] = tot;
        double pct = (tot - prev) * 100.0 / hz / dt;
        if (pct >= 3.0) { char t[96]; std::snprintf(t, sizeof t, " %s=%.0f%%", comm.c_str(), pct); out += t; }
    }
    closedir(d);
    std::fprintf(stderr, "[PROF] threads(cpu):%s\n", out.c_str());
}

inline void report(double dt, uint32_t frames) {
    if (!enabled) return;
    double f = frames ? frames : 1;
    std::fprintf(stderr, "[PROF] per-frame (n=%u, %.1fs): draws=%.0f batches=%.0f verts=%.0f clears=%.0f texbinds=%.0f texmiss=%.2f texKB=%.0f cmds=%.0f | ms:",
        frames, dt, draws / f, batches / f, verts / f, clears / f, tex_binds / f, tex_miss / f, tex_bytes / f / 1024.0, cmds / f);
    for (int i = 0; i < S_N; i++) std::fprintf(stderr, " %s=%.2f", k_names[i], ns[i] / 1e6 / f);
    std::fprintf(stderr, "\n");
    if (movie_binds)
        std::fprintf(stderr, "[PROF] movie texture: binds=%.2f uploads=%.2f upload_ms=%.3f (per frame)\n",
            movie_binds / f, movie_uploads / f, movie_ns / 1e6 / f);
    movie_binds = movie_uploads = movie_ns = 0;
    std::fprintf(stderr, "[PROF] cat(draws/verts per frame): through=%.0f/%.0f 3d=%.0f/%.0f skinned=%.0f/%.0f lit=%.0f/%.0f lit+skin=%.0f/%.0f sametex=%.0f\n",
        cat_draws[0] / f, cat_verts[0] / f, cat_draws[1] / f, cat_verts[1] / f, cat_draws[2] / f, cat_verts[2] / f,
        cat_draws[3] / f, cat_verts[3] / f, cat_draws[4] / f, cat_verts[4] / f, same_tex_draws / f);
    std::fprintf(stderr, "[PROF] feedback snapshots/frame=%.1f (%.1f Mpx copied/frame)\n", fb_snaps / f, fb_snap_px / f / 1e6);
    fb_snaps = fb_snap_px = 0;
    std::fprintf(stderr, "[PROF] state-group changes/frame:");
    for (int i = 0; i < G_N; i++) std::fprintf(stderr, " %s=%.0f", k_grp[i], grp_changes[i] / f);
    std::fprintf(stderr, "\n");
    std::memset(grp_changes, 0, sizeof grp_changes);
    std::memset(cat_draws, 0, sizeof cat_draws); std::memset(cat_verts, 0, sizeof cat_verts); same_tex_draws = 0;
    thread_cpu_report(dt);
    std::memset(ns, 0, sizeof ns);
    draws = batches = verts = tex_binds = tex_miss = tex_bytes = clears = cmds = 0;
}

}  // namespace psp_prof
