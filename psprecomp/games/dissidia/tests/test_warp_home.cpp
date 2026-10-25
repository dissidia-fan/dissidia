// Host test for roster.cpp's homing root-motion hook (roster.txt warp=MOTION,TRAVEL,STOP[,LIFT]): fake guest RAM, a fake
// FUN_08832EB8 that returns the motion's straight step, and a fake integrator that adds the step to the actor position.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_warp_home test_warp_home.cpp; /tmp/test_warp_home
// (lives next to test_roster_hooks.cpp / test_dash_speed.cpp; it includes ../runtime/roster.cpp the same way)
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
FuncPtr ovl_battle_lookup(uint32_t) { return nullptr; }
thread_local uint32_t g_func_ring[32];
thread_local uint32_t g_func_ring_pos;
void psp_dump_func_ring(const char*) {}
static std::map<uint32_t, FuncPtr> g_hooks;
std::string psp_path_to_host(const char* p) { return p; }
uint32_t psp_hle_stub_addr_for_nid(uint32_t) { return 0; }
void psp_dispatch_register(uint32_t a, FuncPtr f) { g_hooks[a] = f; }
uint32_t psp_kmem_bump_alloc_named(uint32_t, const char*) { return 0; }
uint8_t* R(uint32_t a) { return g_ram.data() + (a & 0x07FFFFFFu); }
void W32(uint32_t a, uint32_t v) { std::memcpy(R(a), &v, 4); }
void W16(uint32_t a, uint16_t v) { std::memcpy(R(a), &v, 2); }
void WF(uint32_t a, float v) { std::memcpy(R(a), &v, 4); }
float RF(uint32_t a) { float f; std::memcpy(&f, R(a), 4); return f; }
void WV(uint32_t a, float x, float y, float z) { WF(a, x); WF(a + 4, y); WF(a + 8, z); WF(a + 12, 1.0f); }

// the motion's root motion: g_step[actor] metres along +z this call (as FUN_08832EB8 would return it, yaw 0)
static std::map<uint32_t, float> g_step, g_stepy;   // g_stepy: the step's own vertical part (case 14)
static int g_root_calls = 0;
void orig_root_step(uint8_t* rdram, recomp_context* ctx) {
    g_root_calls++;
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), actor = static_cast<uint32_t>(ctx->r[5]);
    const float v[4] = {0.0f, g_stepy[actor], g_step[actor], 0.0f};
    std::memcpy(rdram + (out & 0x07FFFFFFu), v, sizeof v);
    ctx->r[4] = ctx->r[5] = ctx->r[6] = 0;   // clobbers the argument registers like real code
}
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) { return a == 0x08832EB8u ? orig_root_step : orig_other; }

const uint32_t OUT = 0x09200000u, NOC = 0x09210000u, BARTZ = 0x09220000u, FOE = 0x09240000u;
const uint32_t INFO_N = 0x09230000u, INFO_B = 0x09230100u, ROWS = 0x09250000u;

void set_motion(uint32_t actor, int script, int motion, uint32_t row) {   // as FUN_0882F0D4: script id + p_se row
    W16(row, static_cast<uint16_t>(script)); W16(row + 2, static_cast<uint16_t>(motion));
    W32(actor + 0x17C, row); W16(actor + 0x180, static_cast<uint16_t>(script));
    WF(actor + 0x1A8, 0.0f);
}
// one tick of the integrator: root step -> position += step; motion time += 1
void tick(uint32_t actor, float step) {
    g_step[actor] = step;
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(OUT); ctx.r[5] = static_cast<int32_t>(actor);
    g_hooks.at(0x08832EB8u)(g_ram.data(), &ctx);
    for (int i = 0; i < 3; i++) WF(actor + 0x80 + 4 * i, RF(actor + 0x80 + 4 * i) + RF(OUT + 4 * i));
    WF(actor + 0x1A8, RF(actor + 0x1A8) + 1.0f);
}
// the Warp-Strike curve in ticks: 12.5 m of trans_include spread over 14 ticks (uneven, like a smoothstep)
const float CURVE[14] = {0.1f, 0.3f, 0.6f, 0.9f, 1.2f, 1.4f, 1.5f, 1.5f, 1.4f, 1.2f, 0.9f, 0.7f, 0.5f, 0.3f};
float curve_sum() { float s = 0; for (float c : CURVE) s += c; return s; }
void warp(uint32_t actor, float foe_vx = 0.0f) {   // the whole travel; the foe may drift along x
    for (int i = 0; i < 4; i++) tick(actor, 0.0f);   // wind-up: no root motion
    for (float c : CURVE) { WF(FOE + 0x80, RF(FOE + 0x80) + foe_vx); tick(actor, c); }
    for (int i = 0; i < 4; i++) tick(actor, 0.0f);   // strike / recovery
}
float dist(uint32_t a, uint32_t b) {
    float s = 0; for (int i = 0; i < 3; i++) { const float d = RF(a + 0x80 + 4 * i) - RF(b + 0x80 + 4 * i); s += d * d; }
    return std::sqrt(s);
}
bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

int main() {
    CHECK(near(curve_sum(), 12.5f));
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69,12.5,1.8,0.5;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u));
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x2FCu, FOE); W32(BARTZ + 0x2FCu, FOE);

    // 1. near target, 5 m ahead and level: stops 1.8 m short (3.2 m of travel), never past it
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 5);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(dist(NOC, FOE), 1.8f) && near(RF(NOC + 0x88), 3.2f) && near(RF(NOC + 0x80), 0.0f) && near(RF(NOC + 0x84), 3.0f));

    // 2. far target, 30 m away to the side (+x) and 4 m higher: 12.5 m straight at it, in 3D
    WV(NOC + 0x80, 0, 2, 0); WV(FOE + 0x80, 30, 6, 0);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    {
        const float d0 = std::sqrt(30.f * 30.f + 4.f * 4.f);
        CHECK(near(RF(NOC + 0x80), 30.0f * 12.5f / d0) && near(RF(NOC + 0x84), 2.0f + 4.0f * 12.5f / d0) && near(RF(NOC + 0x88), 0.0f));
        CHECK(near(dist(NOC, FOE), d0 - 12.5f));
    }

    // 3a. target below and far ahead (on the floor, 2 m down, 10 m ahead): the line would end 0.35 m above its feet,
    //     LIFT keeps it at 0.5 m; still short of the target
    WV(NOC + 0x80, 0, 2, 0); WV(FOE + 0x80, 0, 0, 10);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x84), 0.5f, 2e-3f) && dist(NOC, FOE) >= 1.8f - 1e-3f && RF(NOC + 0x88) < 10.0f);
    // 3b. steep dive (6 m down, 4 m ahead): along the line, 1.8 m short of it, above the lift
    WV(NOC + 0x80, 0, 6, 0); WV(FOE + 0x80, 0, 0, 4);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(dist(NOC, FOE), 1.8f) && RF(NOC + 0x84) > 0.5f && RF(NOC + 0x88) < 4.0f);

    // 4. already in contact (1 m away): no travel at all
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 1);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 0.0f) && near(RF(NOC + 0x80), 0.0f));

    // 5. target moves during the travel: the end point follows it (ends 1.8 m from where it is now)
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 8);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC, 0.2f);
    CHECK(near(dist(NOC, FOE), 1.8f, 0.05f));

    // 6. no target: the motion's own straight 12.5 m
    W32(NOC + 0x2FCu, 0);
    WV(NOC + 0x80, 0, 3, 0);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f));
    W32(NOC + 0x2FCu, FOE);

    // 7. another motion of Noctis, and Bartz's motion 69: untouched
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 5, 3, 0);
    set_motion(NOC, 10010, 49, ROWS + 16);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f) && near(RF(NOC + 0x80), 0.0f));
    WV(BARTZ + 0x80, 0, 3, 0);
    set_motion(BARTZ, 10092, 69, ROWS + 32);
    warp(BARTZ);
    CHECK(near(RF(BARTZ + 0x88), 12.5f) && near(RF(BARTZ + 0x80), 0.0f));

    // 8. back-to-back Warp-Strikes (the motion restarts without another motion between): each one homes from its own start
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 6);
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 4.2f));
    WV(FOE + 0x80, 0, 3, 20);
    WF(NOC + 0x1A8, 0.0f);                       // restart: motion time drops
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 4.2f + 12.5f));

    // 9. stale script row (row's script differs from +0x180): treated as another motion
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 5, 3, 0);
    set_motion(NOC, 10092, 69, ROWS);
    W16(NOC + 0x180, 10093);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f));

    // 11. several motions and tokens: `warp=69/85,12.5,1.8,0.5 warp=90,6,1,0` (Warp-Strike in the air and on the ground,
    //     and a second token with its own numbers): 85 homes like 69 (here from the floor to a grounded foe, so it stays on
    //     the floor, and up to an airborne one); 90 with TRAVEL 6 / STOP 1; 86 untouched; a switch to the other warp
    //     motion without a time drop starts a new travel from where he is
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69/85,12.5,1.8,0.5 warp=90,6,1,0;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u));
    W32(NOC + 0x2FCu, FOE);
    WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 6);                      // ground Warp-Strike, foe on the floor 6 m ahead
    set_motion(NOC, 10097, 85, ROWS);
    warp(NOC);
    CHECK(near(dist(NOC, FOE), 1.8f) && near(RF(NOC + 0x84), 0.0f) && near(RF(NOC + 0x88), 4.2f));
    WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 5, 9);                      // foe in the air: straight up at it, in 3D
    set_motion(NOC, 10097, 85, ROWS);
    warp(NOC);
    {
        const float d0 = std::sqrt(5.f * 5.f + 9.f * 9.f), f = (d0 - 1.8f) / d0;
        CHECK(near(RF(NOC + 0x84), 5.0f * f) && near(RF(NOC + 0x88), 9.0f * f) && near(dist(NOC, FOE), 1.8f));
    }
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 30);                     // the air motion still homes (12.5 m)
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f));
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 4);                      // second token: TRAVEL 6, STOP 1 (curve x 6 / 12.5)
    set_motion(NOC, 10200, 90, ROWS + 48);
    for (int i = 0; i < 4; i++) tick(NOC, 0.0f);
    for (float c : CURVE) tick(NOC, c * 6.0f / 12.5f);
    CHECK(near(dist(NOC, FOE), 1.0f) && near(RF(NOC + 0x88), 3.0f));
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 40);
    set_motion(NOC, 10200, 90, ROWS + 48);
    for (int i = 0; i < 4; i++) tick(NOC, 0.0f);
    for (float c : CURVE) tick(NOC, c * 6.0f / 12.5f);
    CHECK(near(RF(NOC + 0x88), 6.0f));                                      // at most its own 6 m
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 5, 3, 0);                      // motion 86: not listed, the motion's own travel
    set_motion(NOC, 10098, 86, ROWS + 64);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f) && near(RF(NOC + 0x80), 0.0f));
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 8);                      // 69 half way, then 85 without a time drop
    set_motion(NOC, 10092, 69, ROWS);
    for (int i = 0; i < 4; i++) tick(NOC, 0.0f);
    for (int i = 0; i < 7; i++) tick(NOC, CURVE[i]);
    {
        const float z1 = RF(NOC + 0x88);
        CHECK(z1 > 0.5f && z1 < 6.2f);
        W16(ROWS + 64, 10097); W16(ROWS + 66, 85); W32(NOC + 0x17C, ROWS + 64); W16(NOC + 0x180, 10097);   // keeps +0x1A8
        for (float c : CURVE) tick(NOC, c);
        CHECK(near(dist(NOC, FOE), 1.8f) && near(RF(NOC + 0x88), 6.2f));   // a new travel from z1 that still ends 1.8 short
    }
    // 13. warpreach=87/88,30 (the warp dash): 87 / 88 go up to 30 m with the same 12.5 m of trans_include pacing the way,
    //     still STOP short; Warp-Strike 69 / 85 in the same token keep their 12.5 m; bad warpreach= tokens are ignored
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69/85/87/88,12.5,1.8,0.5 warpreach=87/88,30;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && g_entries[0].warp_reach.size() == 2);
    W32(NOC + 0x2FCu, FOE);
    WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 25);                     // 25 m: all the way, 1.8 short
    set_motion(NOC, 10099, 87, ROWS);
    warp(NOC);
    CHECK(near(dist(NOC, FOE), 1.8f) && near(RF(NOC + 0x88), 23.2f, 2e-3f));
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 45);                     // 45 m: 30 m and no further
    set_motion(NOC, 10100, 88, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 30.0f, 2e-3f));
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 25);                     // Warp-Strike 69: still 12.5 m
    set_motion(NOC, 10092, 69, ROWS);
    warp(NOC);
    CHECK(near(RF(NOC + 0x88), 12.5f));
    WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 6);                      // a near foe: 87 still stops 1.8 short
    set_motion(NOC, 10099, 87, ROWS);
    warp(NOC);
    CHECK(near(dist(NOC, FOE), 1.8f) && near(RF(NOC + 0x88), 4.2f));
    for (const char* bad : {"34 noc100 9 warp=69/85/87/88,12.5,1.8,0.5 warpreach=87/88,0",
                            "34 noc100 9 warp=69/85/87/88,12.5,1.8,0.5 warpreach=87/88,101",
                            "34 noc100 9 warp=69/85/87/88,12.5,1.8,0.5 warpreach=87/x,30",
                            "34 noc100 9 warp=69/85/87/88,12.5,1.8,0.5 warpreach=87,30,1",
                            "34 noc100 9 warp=69/85/87/88,12.5,1.8,0.5 warpreach=30"}) {
        g_entries.clear(); g_hooks.clear();
        setenv("PSPRECOMP_ROSTER", bad, 1);
        dissidia_roster_init(g_ram.data());
        dissidia_roster_grid_install();
        CHECK(g_hooks.count(0x08832EB8u) && g_entries[0].warp_reach.empty());
        W32(NOC + 0x2FCu, FOE);
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 25);
        set_motion(NOC, 10099, 87, ROWS);
        warp(NOC);
        CHECK(near(RF(NOC + 0x88), 12.5f));                                 // the token's own TRAVEL
    }

    // 14. flat homing, `warp=45,8,1.2,-1 warpreach=45,5` (Noctis's Blitz dash-in): horizontal only, 1.2 m short of the foe,
    //     at most 5 m; the step's own vertical part is kept (a foe 3 m up doesn't lift him); a shorter TRAVEL homes only its
    //     first metres and the rest of the motion's root motion is its own; LIFT other than -1 below 0 is rejected
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=69,12.5,1.8,0.5 warp=45,8,1.2,-1 warpreach=45,5 warp=60,2,1.2,-1 warpreach=60,6", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_entries[0].warps.size() == 3 && g_entries[0].warps[1].flat && !g_entries[0].warps[0].flat);
    W32(NOC + 0x2FCu, FOE);
    {
        auto lunge = [](float total, float stepy) {   // CURVE scaled to `total` m, a constant vertical step
            for (float c : CURVE) { g_stepy[NOC] = stepy; tick(NOC, c * total / 12.5f); }
            g_stepy[NOC] = 0.0f;
        };
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 3, 3, 4);                  // 5 m away horizontally, 3 m up
        set_motion(NOC, 10003, 45, ROWS);
        lunge(8.0f, 0.0f);
        CHECK(near(RF(NOC + 0x84), 0.0f) && near(std::hypot(RF(NOC + 0x80) - 3.0f, RF(NOC + 0x88) - 4.0f), 1.2f, 2e-3f));
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 20);                 // far: 5 m, not the motion's 8
        set_motion(NOC, 10003, 45, ROWS);
        lunge(8.0f, 0.0f);
        CHECK(near(RF(NOC + 0x88), 5.0f, 2e-3f));
        WV(NOC + 0x80, 0, 2, 0); WV(FOE + 0x80, 0, 6, 4);                  // own vertical step kept (0.1 m x 14 ticks)
        set_motion(NOC, 10003, 45, ROWS);
        lunge(8.0f, 0.1f);
        CHECK(near(RF(NOC + 0x84), 2.0f + 1.4f, 2e-3f) && near(RF(NOC + 0x88), 2.8f, 2e-3f));
        // TRAVEL 2 of a 5 m motion: a 6 m dash-in, then its own steps after the one that crossed 2 m (5 - 2.4 = 2.6 m)
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 30);
        set_motion(NOC, 10054, 60, ROWS);
        lunge(5.0f, 0.0f);
        CHECK(near(RF(NOC + 0x88), 6.0f + 2.6f, 2e-3f));
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 2);                  // near foe: a 0.8 m dash-in, then its own 2.6 m
        set_motion(NOC, 10054, 60, ROWS);
        lunge(5.0f, 0.0f);
        CHECK(near(RF(NOC + 0x88), 0.8f + 2.6f, 2e-3f));
    }
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=45,8,1.2,-0.5", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08832EB8u));

    // 12. bad motion lists: rejected (warp=69/,..; warp=/85,..; 69/85 with a bad number), and a bad second token keeps
    //     the good first one
    for (const char* bad : {"34 noc100 9 warp=69/,12.5,1.8", "34 noc100 9 warp=/85,12.5,1.8", "34 noc100 9 warp=69/85,abc",
                            "34 noc100 9 warp=69/85;12.5,1.8", "34 noc100 9 warp=69/1000,12.5,1.8"}) {
        g_entries.clear(); g_hooks.clear();
        setenv("PSPRECOMP_ROSTER", bad, 1);
        dissidia_roster_init(g_ram.data());
        dissidia_roster_grid_install();
        CHECK(!g_hooks.count(0x08832EB8u));
    }
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=69/85,12.5,1.8,0.5 warp=90/abc,6,1", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && g_entries.size() == 1 && g_entries[0].warps.size() == 1 && g_entries[0].warps[0].motions.size() == 2);

    // 10. bad token and no token: no hook
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=69,abc", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08832EB8u));
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08832EB8u));
    std::printf("ok test_warp_home (%d root steps)\n", g_root_calls);
    return 0;
}
