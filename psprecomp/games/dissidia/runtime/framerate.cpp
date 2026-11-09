// games/dissidia/runtime/framerate.cpp — native 60 Hz battle logic for Dissidia 012.
//
// How the game paces itself (EBOOT, byte-identical US/EU):
//   * One frame timer object, pointer at 0x089D69E0, built by FUN_08883E74: +0x04 mode (0), +0x0C vcount of the last
//     flip, +0x10 frame interval in vblanks (2), +0x14 vblanks this frame, +0x18 dt (float), +0x24/+0x25 dt enables.
//   * FUN_08883F9C(timer, useNonCB, flag) ends every frame of every loop (battle, menus, loading, movies): it waits
//     for vblanks until `interval` have passed since the last flip, swaps, and stores dt = vblanks x 1.0 in +0x18.
//     In mode 0 the vblank count is clamped to the interval, so dt is a fixed step: 2.0 at 30 Hz, in 1/60 s ticks.
//   * Nothing but the constructor writes the interval, and only FUN_08883F54 (inside the frame end) reads it.
//   * FUN_08864DD8 copies dt into the global frame step 0x089D5EAC (or a slow-motion value derived from it).
//   * The battle code is written against that step: 85 reads of timer+0x18 and 146 of 0x089D5EAC in the battle
//     overlay, plus FUN_08838300 (an actor's step = actor clock x global step, 217 call sites). Motion time,
//     timeline events ((prev, now] windows), movement (speed x dt), decays (pow(a, dt/2)), effect lifetimes and the
//     camera all advance by it. So interval 1 gives dt 1.0 and the same game at 60 updates a second.
//
// What does NOT follow the step (each fixed here, only while running at 60):
//   * Pad auto-repeat (FUN_08871BD0): the repeat timer counts down by max(step, 2.0). Its reload values
//     (pad manager *0x089D6008 +0x25C delay 30, +0x260 interval 4) are scaled by 2/step for the call.
//   * Actors on their own clock (actor+0x3CC, used while the rest of the world is slowed/stopped): the battle
//     overlay sets it to a constant 2.0 (0x08A374B0, 0x08A37868, 0x08A37960, 0x08B46BE0). FUN_08838300's result
//     for such an actor is scaled by interval/2.
//   * Battle camera mode 2 (FUN_08B3B4FC, camera +0x100 == 2) uses camera +0x104, a constant 2.0 from its
//     constructor: scaled by interval/2.
//   * Effect components that step once per update instead of x dt (docs/archive/open-questions/fx.md item 7): the
//     ch61 envelope, the ch95/100/102/103 point and line emitters, the ch68/70 trail paths, the ch105 locus UV scroll
//     and the ch106 locus history (weapon trails). See "Effect components" below.
// Mod code that counts per call rather than per tick uses dissidia_frame_ticks() (roster.cpp warp-dash link).
//
// Scope: the battle overlay only by default. Menus mostly count frames (4 dt reads in the menu overlay), so they
// keep 30 Hz. PSPRECOMP_FPS_SCOPE=all applies 60 everywhere (for testing; menus then run fast).
//
// Options: PSPRECOMP_FPS=30|60 (default 30; the settings menu changes it live and saves it),
// PSPRECOMP_FPS_SCOPE=battle|all, PSPRECOMP_FPS_LOG=1 (logic rate, dt, step and how often body poses change, every
// 5 s, plus each switch), PSPRECOMP_FX60=0 (leave the effect components below as they are, for A/B checks).

#include "psp_game_module.h"
#include "psp_settings.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t FRAME_END = 0x08883F9Cu;     // EBOOT: per-frame end (a0 = frame timer)
constexpr uint32_t TIMER_PTR = 0x089D69E0u;     // EBOOT: frame timer object pointer
constexpr uint32_t FRAME_STEP = 0x089D5EACu;    // EBOOT: global frame step (float, 1/60 s ticks)
constexpr uint32_t PAD_SET = 0x08871BD0u;       // EBOOT: pad button state + auto-repeat
constexpr uint32_t PAD_MGR_PTR = 0x089D6008u;   // EBOOT: pad manager pointer (+0x25C delay, +0x260 interval)
constexpr uint32_t ACTOR_DT = 0x08838300u;      // EBOOT: an actor's time step
constexpr uint32_t OWN_MASK0 = 0x089C7FD8u, OWN_MASK1 = 0x089C7FDCu;
constexpr uint32_t CAM_DT = 0x08B3B4FCu;        // battle overlay: a camera's time step

uint8_t* g_ram = nullptr;
FuncPtr g_orig_frame_end = nullptr, g_orig_pad = nullptr, g_orig_actor_dt = nullptr, g_orig_cam_dt = nullptr;
std::atomic<int> g_target{30};     // requested rate (settings / env)
int g_interval = 2;                // interval written on the last frame end (2 = 30 Hz, 1 = 60 Hz)
bool g_scope_all = false, g_log = false;
int g_switch_logs = 0;
uint64_t g_tick = 1;               // frame ends seen (the effect system updates once per frame)
// logic-rate log
int64_t g_win_t0 = 0;
int g_win_frames = 0;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void w32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }
bool ok(uint32_t a) { return a >= 0x08800000u && a < 0x0BFFFF00u; }
uint32_t overlay_id() { return r32(OVL_BASE); }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PSPRECOMP_FPS_LOG: does body animation change every frame? FUN_089B2974 builds a skeleton's matrices from the
// local poses the motion evaluator wrote (bone i at node+0x0C base + 256 i: scale +80, rotation quaternion +96,
// translation +112). Per skeleton seen in a frame, the local poses are hashed and compared with the previous
// frame's. If the evaluator samples motions at fractional frames, poses change on (nearly) every 60 Hz frame; if
// it rounds to whole 30 fps frames, on about half of them. Logged with the logic rate every 5 s.
constexpr uint32_t SKEL_BUILD = 0x089B2974u;
FuncPtr g_orig_skel = nullptr;
struct SkelSeen { uint32_t node = 0, hash = 0; int64_t frame = -1; };
SkelSeen g_skel[16];
int64_t g_frame_no = 0;
int g_pose_changed = 0, g_pose_same = 0;

void skel_build_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t node = static_cast<uint32_t>(ctx->r[4]);
    if (ok(node) && overlay_id() == 1) {
        const uint32_t base = r32(node + 0x0Cu);
        uint32_t n = 0;
        std::memcpy(&n, g_ram + ((node + 32u) & 0x07FFFFFFu), 2);
        n &= 0xFFFFu;
        if (ok(base) && n >= 8 && n <= 200) {   // a character skeleton (weapons / props have few bones)
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t o = 80; o < 128; o += 4) { h ^= r32(base + 256u * i + o); h *= 16777619u; }
            SkelSeen* slot = nullptr;
            for (auto& sk : g_skel) if (sk.node == node) { slot = &sk; break; }
            if (!slot) for (auto& sk : g_skel) if (sk.frame < g_frame_no - 30) { slot = &sk; sk = SkelSeen{}; sk.node = node; break; }
            if (slot && slot->frame != g_frame_no) {   // first build of this skeleton this frame
                if (slot->frame == g_frame_no - 1) (h != slot->hash ? g_pose_changed : g_pose_same)++;
                slot->hash = h;
                slot->frame = g_frame_no;
            }
        }
    }
    g_orig_skel(rdram, ctx);
}

void fx_log_window();

void frame_end_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t timer = static_cast<uint32_t>(ctx->r[4]);
    g_tick++;
    if (ok(timer)) {
        // In an ad-hoc session every player runs at the lowest rate any of them asked for, so the
        // lock-step battle stays in sync (psp_adhoc_group_fps_cap() is 0 when not connected).
        const int cap = psp_adhoc_group_fps_cap();
        const bool want60 = g_target.load(std::memory_order_relaxed) == 60 && cap != 30 && (g_scope_all || overlay_id() == 1);
        const int interval = want60 ? 1 : 2;
        if (r32(timer + 0x10u) != static_cast<uint32_t>(interval)) {
            w32(timer + 0x10u, static_cast<uint32_t>(interval));
            if (g_log || g_switch_logs < 12) {
                g_switch_logs++;
                std::fprintf(stderr, "[FPS] game logic now %d Hz (frame interval %d, overlay %u)\n", 60 / interval, interval,
                             overlay_id());
            }
        }
        g_interval = interval;
    }
    g_orig_frame_end(rdram, ctx);
    if (g_log && ok(timer)) {
        const int64_t t = now_ms();
        if (!g_win_t0) g_win_t0 = t;
        g_win_frames++;
        g_frame_no++;
        if (t - g_win_t0 >= 5000) {
            const int pose_n = g_pose_changed + g_pose_same;
            std::fprintf(stderr, "[FPS] logic %.1f Hz, interval %d, dt %.2f, step %.2f, overlay %u; body poses changed on %d of %d "
                         "frame-to-frame checks (%.0f%%)\n",
                         g_win_frames * 1000.0 / double(t - g_win_t0), g_interval, rdf(timer + 0x18u), rdf(FRAME_STEP),
                         overlay_id(), g_pose_changed, pose_n, pose_n ? 100.0 * g_pose_changed / pose_n : 0.0);
            g_win_t0 = t;
            g_win_frames = 0;
            g_pose_changed = g_pose_same = 0;
            fx_log_window();
        }
    }
}

void pad_set_hook(uint8_t* rdram, recomp_context* ctx) {
    const float step = rdf(FRAME_STEP);
    const uint32_t mgr = r32(PAD_MGR_PTR);
    if (step > 0.f && step < 2.f && ok(mgr)) {   // the repeat timer always counts down >= 2 per call
        const float delay = rdf(mgr + 0x25Cu), every = rdf(mgr + 0x260u), k = 2.f / step;
        wrf(mgr + 0x25Cu, delay * k);
        wrf(mgr + 0x260u, every * k);
        g_orig_pad(rdram, ctx);
        wrf(mgr + 0x25Cu, delay);
        wrf(mgr + 0x260u, every);
        return;
    }
    g_orig_pad(rdram, ctx);
}

void actor_dt_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    g_orig_actor_dt(rdram, ctx);
    if (g_interval != 2 && ok(actor)) {
        const bool own = ((r32(actor + 0x388u) & r32(OWN_MASK0)) | (r32(actor + 0x38Cu) & r32(OWN_MASK1))) != 0;
        if (own) ctx->f[0] *= static_cast<float>(g_interval) * 0.5f;   // own clock: a fixed 2.0 per frame
    }
}

void cam_dt_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = overlay_id();
    if (id != 1) {   // the same address in another overlay is another function
        FuncPtr f = id == 2 ? ovl_menu_lookup(CAM_DT) : id == 3 ? ovl_extra_lookup(CAM_DT) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const uint32_t cam = static_cast<uint32_t>(ctx->r[4]);
    g_orig_cam_dt(rdram, ctx);
    if (g_interval != 2 && ok(cam) && r32(cam + 0x100u) == 2u) ctx->f[0] *= static_cast<float>(g_interval) * 0.5f;
}

// ---- Effect components ------------------------------------------------------------------------------------------
// The effect runtime advances its components by dt = instance+352 (the global step x the handle's scale, written by
// FUN_0894B48C): 2.0 per update on the PSP, 1.0 per update at 60 Hz. Most components scale by it. These step once
// per *update* instead (F.md, fx.md item 7), so at 60 Hz they ran twice as fast / lived half as long:
//   ch61  envelope     m2 0x08953A50  mult += rate per update while timer += 0.5 dt: each ramp lasts twice as many
//                                     updates with the same rate, overshoots 2x, and ch62's clamp makes it strobe
//   ch95  points       m2 0x0897F3E4  life -1, motion and history per update; only emission (acc += rate dt) by dt
//   ch100 streaks      m2 0x0897DE38  (-> FUN_08984508: emission by dt, FUN_08984B40 steps the sparks per update)
//   ch102 line burst   m2 0x08974DCC  as ch95
//   ch103 line rays    m2 0x089760E8  as ch95
//   ch68  trail path   m2 0x08966018  (-> FUN_0896B710) one ring-buffer sample per update: ch80 ribbons half as long
//   ch70  smoke path   m2 0x08967438  (-> FUN_0896C9B0) shift + drift per update
//   ch105 locus UVs    m2 0x08964B40  u -= P+48, v -= P+52 per update
// At 60 Hz each runs on every other update of its own state block (a3), with instance+352 doubled for the call: on
// those updates it does exactly what the PSP does on a 30 Hz frame (dt 2, one step), and in between it holds, as the
// PSP's picture does. Emitters read dt through the current runner (*0x09B6F478 +136), which is the same instance;
// both are scaled (once each) to be safe.
//   ch106 locus history (FUN_0896808C, from m2 0x089672C4; the weapon / body trails): `t += dt`, one shift
// (pos[i] = pos[i-1] + vel[i-1]) when t >= last + 1 -- at dt 1 that is every update, so the trail was half as long --
// and pos[0] = the current point on every update. Here the shift updates run with dt x 2; the updates between run
// with dt 0 (no shift, t unchanged, but pos[0] still follows the bone, so the trail head stays on the weapon); and
// before each shift update pos[0] of both point buffers (object +0 / +4, n x {vec4 pos, vec4 vel}) is put back to
// the point of the previous shift update, so the shift copies the sample the PSP would have. Wrapped at the inner
// function so roster.cpp's lingering-trail hook on the m2 still decides first whether it runs at all.
// A state block whose memory is reused by a new element within one update of the old one's last step only delays
// the new one's first step by one 60 Hz update.
constexpr uint32_t CUR_RUNNER = 0x09B6F478u;   // EBOOT: runner being updated (+136 instance); read by the emitters
constexpr uint32_t LOCUS_HIST = 0x0896808Cu;   // EBOOT: ch106 history update (a0 object, a1 runner, a2 params)
struct FxHookDef { uint32_t addr; const char* what; };
constexpr FxHookDef k_fx_hooks[] = {
    {0x08953A50u, "ch61 envelope"},   {0x0897F3E4u, "ch95 points"},     {0x0897DE38u, "ch100 streaks"},
    {0x08974DCCu, "ch102 lines"},     {0x089760E8u, "ch103 rays"},      {0x08966018u, "ch68 trail path"},
    {0x08967438u, "ch70 smoke path"}, {0x08964B40u, "ch105 locus UVs"},
};
constexpr int kFxHooks = static_cast<int>(sizeof(k_fx_hooks) / sizeof(k_fx_hooks[0]));
FuncPtr g_fx_orig[kFxHooks] = {};
FuncPtr g_orig_locus_hist = nullptr;
bool g_fx60 = true;

struct FxStep { uint64_t tick = 0; bool ran = false, have = false; uint8_t pos0[32] = {}; };
std::unordered_map<uint32_t, FxStep> g_fx_step;
uint64_t g_fx_pruned = 0;
int g_fx_runs = 0, g_fx_holds = 0;

// Run this update? Alternates per state block; a second call in the same frame repeats the first decision.
bool fx_step_due(FxStep& s) {
    const bool run = s.tick == g_tick ? s.ran : !(s.tick + 1 == g_tick && s.ran);
    s.tick = g_tick;
    s.ran = run;
    return run;
}
FxStep& fx_step_of(uint32_t key) {
    if (g_tick - g_fx_pruned >= 1024) {   // forget blocks not seen for a while (ended effects)
        g_fx_pruned = g_tick;
        for (auto it = g_fx_step.begin(); it != g_fx_step.end();) it = g_tick - it->second.tick > 8 ? g_fx_step.erase(it) : std::next(it);
    }
    return g_fx_step[key];
}
// Scale instance+352 of the runner's instance (and the current runner's, if different) for one call.
struct DtScale {
    uint32_t addr[2] = {};
    float was[2] = {};
    int n = 0;
    DtScale(uint32_t runner, float k) {
        const uint32_t cur = r32(CUR_RUNNER);
        const uint32_t inst[2] = {ok(runner) ? r32(runner + 136u) : 0u, ok(cur) ? r32(cur + 136u) : 0u};
        for (uint32_t in : inst) {
            if (!ok(in) || (n && addr[0] == in + 352u)) continue;
            addr[n] = in + 352u;
            was[n] = rdf(addr[n]);
            wrf(addr[n], was[n] * k);
            n++;
        }
    }
    ~DtScale() { for (int i = n - 1; i >= 0; i--) wrf(addr[i], was[i]); }
};
bool fx_fix_on() { return g_interval != 2 && g_fx60; }

template <int I>
void fx_every_other_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!fx_fix_on()) { g_fx_orig[I](rdram, ctx); return; }
    if (!fx_step_due(fx_step_of(static_cast<uint32_t>(ctx->r[7])))) { g_fx_holds++; return; }
    g_fx_runs++;
    DtScale dt(static_cast<uint32_t>(ctx->r[5]), 2.0f);
    g_fx_orig[I](rdram, ctx);
}
template <int... I> struct FxHookFns { static constexpr FuncPtr fns[] = {fx_every_other_hook<I>...}; };
using FxFns = FxHookFns<0, 1, 2, 3, 4, 5, 6, 7>;
static_assert(sizeof(FxFns::fns) / sizeof(FxFns::fns[0]) == kFxHooks, "one hook function per address");

void locus_hist_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    if (!fx_fix_on() || !ok(obj)) { g_orig_locus_hist(rdram, ctx); return; }
    FxStep& s = fx_step_of(obj);
    const bool first = g_ram[(obj + 8u) & 0x07FFFFFFu] != 0;   // the history is (re)filled on its next shift
    bool run = fx_step_due(s);
    if (first && !run) run = s.ran = true;
    const uint32_t a = r32(obj), b = r32(obj + 4u);
    const bool bufs = ok(a) && ok(b);
    if (!run) {
        g_fx_holds++;
        DtScale dt(static_cast<uint32_t>(ctx->r[5]), 0.0f);
        g_orig_locus_hist(rdram, ctx);
        return;
    }
    g_fx_runs++;
    if (bufs && s.have && !first) {
        std::memcpy(g_ram + (a & 0x07FFFFFFu), s.pos0, 16);
        std::memcpy(g_ram + (b & 0x07FFFFFFu), s.pos0 + 16, 16);
    }
    {
        DtScale dt(static_cast<uint32_t>(ctx->r[5]), 2.0f);
        g_orig_locus_hist(rdram, ctx);
    }
    s.have = bufs;
    if (bufs) {
        std::memcpy(s.pos0, g_ram + (a & 0x07FFFFFFu), 16);
        std::memcpy(s.pos0 + 16, g_ram + (b & 0x07FFFFFFu), 16);
    }
}

void fx_log_window() {
    if (!g_fx_runs && !g_fx_holds) return;
    std::fprintf(stderr, "[FPS] effect components stepped at 30 Hz: %d steps, %d held updates, %zu state blocks\n", g_fx_runs,
                 g_fx_holds, g_fx_step.size());
    g_fx_runs = g_fx_holds = 0;
}

void fx_install() {
    const char* e = std::getenv("PSPRECOMP_FX60");
    g_fx60 = !(e && e[0] == '0');
    int n = 0;
    for (int i = 0; i < kFxHooks; i++) {
        g_fx_orig[i] = psp_dispatch_probe_lookup(k_fx_hooks[i].addr);
        if (g_fx_orig[i]) { psp_dispatch_register(k_fx_hooks[i].addr, FxFns::fns[i]); n++; }
        else std::fprintf(stderr, "[FPS] effect hook %s NOT installed (no function at %08X)\n", k_fx_hooks[i].what, k_fx_hooks[i].addr);
    }
    g_orig_locus_hist = psp_dispatch_probe_lookup(LOCUS_HIST);
    if (g_orig_locus_hist) { psp_dispatch_register(LOCUS_HIST, locus_hist_hook); n++; }
    else std::fprintf(stderr, "[FPS] effect hook ch106 locus history NOT installed (no function at %08X)\n", LOCUS_HIST);
    std::fprintf(stderr, "[FPS] %d/%d effect component hooks installed (60 Hz: stepped at 30 Hz)%s\n", n, kFxHooks + 1,
                 g_fx60 ? "" : "; off (PSPRECOMP_FX60=0)");
}

int fps_get() { return g_target.load(); }
void fps_set(int fps) {
    fps = fps == 60 ? 60 : 30;
    if (g_target.exchange(fps) != fps) std::fprintf(stderr, "[FPS] target frame rate %d\n", fps);
    psp_adhoc_set_local_fps(fps);   // told to the other players
}
const int k_rates[] = {30, 60};

}  // namespace

// Ticks (1/60 s) one game frame advances: 2 at 30 Hz, 1 at 60 Hz.
int dissidia_frame_ticks() { return g_interval; }
// Factor for a value the game keeps as "ticks per 30 Hz frame" (the actor own clock): 1 at 30 Hz, 0.5 at 60 Hz.
float dissidia_own_clock_scale() { return static_cast<float>(g_interval) * 0.5f; }

void dissidia_framerate_install(uint8_t* rdram) {
    g_ram = rdram;
    const char* e = std::getenv("PSPRECOMP_FPS");
    if (e && std::atoi(e) == 60) g_target = 60;
    g_scope_all = (e = std::getenv("PSPRECOMP_FPS_SCOPE")) && std::strcmp(e, "all") == 0;
    g_log = (e = std::getenv("PSPRECOMP_FPS_LOG")) && e[0] && e[0] != '0';
    g_orig_frame_end = psp_dispatch_probe_lookup(FRAME_END);
    g_orig_pad = psp_dispatch_probe_lookup(PAD_SET);
    g_orig_actor_dt = psp_dispatch_probe_lookup(ACTOR_DT);
    g_orig_cam_dt = ovl_battle_lookup(CAM_DT);
    if (!g_orig_frame_end || !g_orig_pad || !g_orig_actor_dt || !g_orig_cam_dt) {
        std::fprintf(stderr, "[FPS] hooks NOT installed (frame end %p pad %p actor dt %p camera dt %p): 30 Hz only\n",
                     (void*)g_orig_frame_end, (void*)g_orig_pad, (void*)g_orig_actor_dt, (void*)g_orig_cam_dt);
        return;
    }
    psp_dispatch_register(FRAME_END, frame_end_hook);
    psp_dispatch_register(PAD_SET, pad_set_hook);
    psp_dispatch_register(ACTOR_DT, actor_dt_hook);
    psp_dispatch_register(CAM_DT, cam_dt_hook);
    if (g_log && (g_orig_skel = psp_dispatch_probe_lookup(SKEL_BUILD))) psp_dispatch_register(SKEL_BUILD, skel_build_hook);
    fx_install();
    psp_settings_set_frame_rate_hook({k_rates, 2, fps_get, fps_set});
    psp_adhoc_set_local_fps(g_target.load());
    std::fprintf(stderr, "[FPS] frame rate %d (battle logic; scope %s)\n", g_target.load(), g_scope_all ? "all" : "battle");
}
