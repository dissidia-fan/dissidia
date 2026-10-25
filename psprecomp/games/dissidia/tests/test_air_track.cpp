// Host test for roster.cpp's vertical tracking (roster.txt vtrack=MOTION,FROM,TO,MAX[,LIFT], vtrack_step on the root-motion
// step FUN_08832EB8): fake guest RAM, a fake FUN_08832EB8 that returns the motion's own step, a fake integrator that adds
// the step to the actor position and advances the motion time one tick.
//   sh run_air_track_test.sh   (includes ../runtime/roster.cpp like test_warp_home.cpp)
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

static float g_vz = 0.0f, g_vy = 0.0f;   // the motion's own step this tick (yaw 0)
void orig_root_step(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]);
    const float v[4] = {0.0f, g_vy, g_vz, 0.0f};
    std::memcpy(rdram + (out & 0x07FFFFFFu), v, sizeof v);
    ctx->r[4] = ctx->r[5] = ctx->r[6] = 0;   // clobbers the argument registers like real code
}
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) { return a == 0x08832EB8u ? orig_root_step : orig_other; }

const uint32_t OUT = 0x09200000u, NOC = 0x09210000u, BARTZ = 0x09220000u, FOE = 0x09240000u;
const uint32_t INFO_N = 0x09230000u, INFO_B = 0x09230100u, ROWS = 0x09250000u;
void set_motion(uint32_t actor, int script, int motion, uint32_t row) {
    W16(row, static_cast<uint16_t>(script)); W16(row + 2, static_cast<uint16_t>(motion));
    W32(actor + 0x17C, row); W16(actor + 0x180, static_cast<uint16_t>(script));
    WF(actor + 0x1A8, 0.0f);
}
void tick(uint32_t actor, float vz, float vy = 0.0f) {
    g_vz = vz; g_vy = vy;
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(OUT); ctx.r[5] = static_cast<int32_t>(actor);
    g_hooks.at(0x08832EB8u)(g_ram.data(), &ctx);
    for (int i = 0; i < 3; i++) WF(actor + 0x80 + 4 * i, RF(actor + 0x80 + 4 * i) + RF(OUT + 4 * i));
    WF(actor + 0x1A8, RF(actor + 0x1A8) + 1.0f);
}
bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }
float Y(uint32_t a) { return RF(a + 0x84); }
// Dagger Strike's flurry: 1.8 m of dash-in over ticks 0-12, then a slow drift; `ticks` ticks of it. Checks on the way:
// the height moves monotonically toward the end height and never passes the foe's height (no overshoot).
float flurry(uint32_t actor, int ticks, float foe_vy = 0.0f, float own_vy = 0.0f) {
    const float y0 = Y(actor), sgn0 = Y(FOE) - y0;
    float last = y0;
    for (int t = 0; t < ticks; t++) {
        WF(FOE + 0x84, Y(FOE) + foe_vy);
        tick(actor, t < 12 ? 0.15f : 0.02f, own_vy);
        if (own_vy == 0.0f && foe_vy == 0.0f) {
            CHECK(sgn0 >= 0 ? Y(actor) >= last - 1e-5f : Y(actor) <= last + 1e-5f);               // monotonic
            CHECK(sgn0 >= 0 ? Y(actor) <= Y(FOE) + 1e-4f : Y(actor) >= Y(FOE) - 1e-4f);           // never past the foe
        }
        last = Y(actor);
    }
    return Y(actor) - y0;
}

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69,12.5,1.8,0.5 vtrack=60,0,12,1.8,0.3;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u));
    CHECK(g_vtrack_any);
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x2FCu, FOE); W32(BARTZ + 0x2FCu, FOE);

    // 1. foe 1.5 m higher, 2.5 m ahead: by tick 12 Noctis is at its height, the horizontal travel is the motion's own
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.5f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    {
        for (int t = 0; t < 12; t++) tick(NOC, 0.15f);
        CHECK(near(Y(NOC), 4.5f) && near(RF(NOC + 0x88), 1.8f) && near(RF(NOC + 0x80), 0.0f));
        for (int t = 12; t < 40; t++) tick(NOC, 0.02f);
        CHECK(near(Y(NOC), 4.5f));
    }
    // 1b. the ease: after 3 of 12 ticks well under a third of the way, after 6 about half
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.5f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 3; t++) tick(NOC, 0.15f);
    CHECK(Y(NOC) - 3.0f > 0.0f && Y(NOC) - 3.0f < 0.5f * 1.5f / 3.0f * 2.0f);
    for (int t = 3; t < 6; t++) tick(NOC, 0.15f);
    CHECK(near(Y(NOC) - 3.0f, 0.75f * 1.0f, 0.15f));

    // 2. foe higher than MAX: +1.8 m and no more
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 6.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    CHECK(near(flurry(NOC, 40), 1.8f));
    // 3. foe 1.5 m lower: down to 0.3 m (LIFT) above its feet
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 1.5f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    CHECK(near(flurry(NOC, 40), -1.2f));
    // 3b. foe 0.2 m lower: within LIFT, no move
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 2.8f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    CHECK(near(flurry(NOC, 40), 0.0f));
    // 3c. foe far lower (4 m): -1.8 m
    WV(NOC + 0x80, 0, 5, 0); WV(FOE + 0x80, 0, 1.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    CHECK(near(flurry(NOC, 40), -1.8f));
    // 4. level foe: nothing
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    CHECK(near(flurry(NOC, 40), 0.0f));

    // 5. the foe rises during the tracking (2 cm / tick): ends at its height at tick 12, then stays (no drift after TO)
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 12; t++) { WF(FOE + 0x84, Y(FOE) + 0.02f); tick(NOC, 0.15f); CHECK(Y(NOC) <= Y(FOE) + 1e-4f); }
    CHECK(near(Y(NOC), Y(FOE)));
    {
        const float y12 = Y(NOC);
        for (int t = 12; t < 40; t++) { WF(FOE + 0x84, Y(FOE) + 0.05f); tick(NOC, 0.02f); }
        CHECK(near(Y(NOC), y12));
    }
    // 6. the motion's own vertical travel is kept on top: the end height is still the foe's
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 12; t++) tick(NOC, 0.15f, 0.03f);
    CHECK(near(Y(NOC), 4.0f));

    // 7. restart of the motion (time drops): a new tracking from the new start
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.0f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 20; t++) tick(NOC, 0.1f);
    CHECK(near(Y(NOC), 4.0f));
    WV(FOE + 0x80, 0, 2.5f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 20; t++) tick(NOC, 0.1f);
    CHECK(near(Y(NOC), 2.8f));

    // 8. no target / another motion / another character: untouched
    W32(NOC + 0x2FCu, 0);
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 4.5f, 2.5f);
    set_motion(NOC, 10200, 60, ROWS);
    for (int t = 0; t < 20; t++) tick(NOC, 0.1f);
    CHECK(near(Y(NOC), 3.0f));
    W32(NOC + 0x2FCu, FOE);
    WV(NOC + 0x80, 0, 3, 0);
    set_motion(NOC, 10201, 61, ROWS + 16);
    for (int t = 0; t < 20; t++) tick(NOC, 0.1f);
    CHECK(near(Y(NOC), 3.0f));
    WV(BARTZ + 0x80, 0, 3, 0);
    set_motion(BARTZ, 10200, 60, ROWS + 32);
    for (int t = 0; t < 20; t++) tick(BARTZ, 0.1f);
    CHECK(near(Y(BARTZ), 3.0f));

    // 9. warp= homing of the same character unchanged (motion 69: straight at the target in 3D, 1.8 m short)
    WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 5);
    set_motion(NOC, 10092, 69, ROWS + 48);
    const float CURVE[14] = {0.1f, 0.3f, 0.6f, 0.9f, 1.2f, 1.4f, 1.5f, 1.5f, 1.4f, 1.2f, 0.9f, 0.7f, 0.5f, 0.3f};
    for (int i = 0; i < 4; i++) tick(NOC, 0.0f);
    for (float c : CURVE) tick(NOC, c);
    CHECK(near(RF(NOC + 0x88), 3.2f) && near(Y(NOC), 3.0f));

    std::printf("air track OK\n");
    return 0;
}
