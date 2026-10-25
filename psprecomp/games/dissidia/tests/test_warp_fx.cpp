// Host test for roster.cpp's Warp-Strike bone pass (roster.txt warpfx=SWORD,FLY,AIM,TIP[,WISP] with warp=): fake guest
// RAM, a fake FUN_08832EB8 (the motion's straight step), a fake integrator, a fake engine pose of the two bones each tick
// and FUN_0880EA20 (bone_post_hook -> warp_bone_post). Checks the thrown sword flies from the release point straight at
// the target, point first, arriving after FLY ticks and staying there; the wisp bone lies along S -> E scaled by
// |E - S| / TRAVEL; frames the engine does not re-pose do not compound; another motion gets the motion's own pose back.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_warp_fx test_warp_fx.cpp; /tmp/test_warp_fx
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

static std::map<uint32_t, float> g_step;
void orig_root_step(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), actor = static_cast<uint32_t>(ctx->r[5]);
    const float v[4] = {0.0f, 0.0f, g_step[actor], 0.0f};
    std::memcpy(rdram + (out & 0x07FFFFFFu), v, sizeof v);
    ctx->r[4] = ctx->r[5] = ctx->r[6] = 0;
}
static int g_bone_orig = 0;
void orig_bone_post(uint8_t*, recomp_context* ctx) { g_bone_orig++; ctx->r[4] = ctx->r[5] = ctx->r[6] = 0; }
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x08832EB8u) return orig_root_step;
    if (a == 0x0880EA20u) return orig_bone_post;
    return orig_other;
}

const uint32_t OUT = 0x09200000u, NOC = 0x09210000u, BARTZ = 0x09220000u, FOE = 0x09240000u;
const uint32_t INFO_N = 0x09230000u, INFO_B = 0x09230100u, ROWS = 0x09250000u;
const uint32_t CTRL_N = 0x09260000u, GM_N = 0x09261000u, BONES_N = 0x09270000u;
const uint32_t CTRL_B = 0x09280000u, GM_B = 0x09281000u, BONES_B = 0x09290000u;
const int NB = 201, SWORD = 83, WISP = 84;
const float TIP = 1.124f, AIM = 1.0f, FLY = 5.0f, TRAVEL = 12.5f, STOP = 1.8f;
const float G[3] = {0.5246f, 1.3036f, 0.2729f};
const float HAND[3] = {0.157f, 0.95f, 0.17f};

uint32_t bone_mat(uint32_t bones, int k) { return bones + 0x100u * k + 0x80u; }
void put(uint32_t a, const float* M) { std::memcpy(R(a), M, 64); }
void get(uint32_t a, float* M) { std::memcpy(M, R(a), 64); }
void set_motion(uint32_t actor, int script, int motion, uint32_t row) {
    W16(row, static_cast<uint16_t>(script)); W16(row + 2, static_cast<uint16_t>(motion));
    W32(actor + 0x17C, row); W16(actor + 0x180, static_cast<uint16_t>(script));
    WF(actor + 0x1A8, 0.0f);
}
float g_yaw = 0.0f;
void world(uint32_t actor) {     // model -> world: yaw about +Y, then the actor position (row vectors)
    const float c = std::cos(g_yaw), s = std::sin(g_yaw);
    const float W[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, RF(actor + 0x80), RF(actor + 0x84), RF(actor + 0x88), 1};
    std::memcpy(R(actor + 0x330u), W, 64);
}
// the engine's pose of the two bones at this tick of motion `m` (sword shown ticks 14-33 of motion 69, of its ground copy
// 85 and of a test motion 90, blade +Z)
void engine_pose(uint32_t bones, int m, int tick) {
    const bool shown = (m == 69 || m == 85 || m == 90) && tick >= 14 && tick < 34;
    const float k = shown ? 1.0f : 0.0f;
    const float S[16] = {k, 0, 0, 0, 0, k, 0, 0, 0, 0, k, 0, HAND[0], HAND[1], HAND[2] + (shown ? 2.0f * (tick - 14) : 0.0f), 1};
    put(bone_mat(bones, SWORD), S);
    const float P[16] = {8, 0, 0, 0, 0, 8, 0, 0, 0, 0, 8, 0, G[0], G[1], G[2], 1};
    put(bone_mat(bones, WISP), P);
}
void bone_post(uint32_t actor) {
    recomp_context ctx{};
    ctx.r[4] = 0; ctx.r[5] = 0; ctx.r[6] = static_cast<int32_t>(actor);
    g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx);
}
const float CURVE[14] = {0.1f, 0.3f, 0.6f, 0.9f, 1.2f, 1.4f, 1.5f, 1.5f, 1.4f, 1.2f, 0.9f, 0.7f, 0.5f, 0.3f};
// one tick: root step + integrator, then the engine poses the model (unless repose=false) and the bone pass runs
void tick(uint32_t actor, uint32_t bones, int m, int t, bool repose = true) {
    g_step[actor] = (t >= 20 && t < 34) ? CURVE[t - 20] : 0.0f;
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(OUT); ctx.r[5] = static_cast<int32_t>(actor);
    g_hooks.at(0x08832EB8u)(g_ram.data(), &ctx);
    for (int i = 0; i < 3; i++) WF(actor + 0x80 + 4 * i, RF(actor + 0x80 + 4 * i) + RF(OUT + 4 * i));
    WF(actor + 0x1A8, RF(actor + 0x1A8) + 1.0f);
    world(actor);
    if (repose) engine_pose(bones, m, t);
    bone_post(actor);
}
// world position / direction of a model-space point / direction
void to_world(uint32_t actor, const float* p, float w, float* o) {
    float W[16]; get(actor + 0x330u, W);
    for (int j = 0; j < 3; j++) o[j] = p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + w * W[12 + j];
}
struct Sword { float grip[3], dir[3], tip[3], scale; };
Sword sword(uint32_t actor, uint32_t bones) {
    float M[16]; get(bone_mat(bones, SWORD), M);
    Sword s;
    to_world(actor, M + 12, 1.0f, s.grip);
    to_world(actor, M + 8, 0.0f, s.dir);
    s.scale = std::sqrt(s.dir[0] * s.dir[0] + s.dir[1] * s.dir[1] + s.dir[2] * s.dir[2]);
    for (int i = 0; i < 3; i++) { if (s.scale > 0) s.dir[i] /= s.scale; s.tip[i] = s.grip[i] + s.dir[i] * TIP; }
    return s;
}
float len3(const float* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
float dist3(const float* a, const float* b) { const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return len3(d); }
bool near(float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; }
bool near3(const float* a, const float* b, float tol = 2e-3f) { return dist3(a, b) <= tol; }

// a whole Warp-Strike (ticks 0-40) with checks on the way; returns the actor start S (world)
struct Run { float p0[3], aim[3], S[3], E[3]; };
Run warp(uint32_t foe, float ax, float ay, float az, float yaw, bool target = true) {
    Run r{};
    g_yaw = yaw;
    WV(NOC + 0x80, 0, 3, 0); WV(foe + 0x80, ax, 3 + ay, az);
    W32(NOC + 0x2FCu, target ? foe : 0u);
    set_motion(NOC, 10092, 69, ROWS);
    for (int t = 0; t < 14; t++) {
        tick(NOC, BONES_N, 69, t);
        CHECK(sword(NOC, BONES_N).scale < 1e-6f);                      // not thrown yet: the motion's pose (hidden)
    }
    to_world(NOC, HAND, 1.0f, r.p0);
    r.aim[0] = ax; r.aim[1] = 3 + ay + AIM; r.aim[2] = az;
    std::memcpy(r.S, R(NOC + 0x80), 12);
    for (int t = 14; t < 41; t++) {
        tick(NOC, BONES_N, 69, t);
        if (t == 14) {
            const Sword s = sword(NOC, BONES_N);
            CHECK(near3(s.grip, r.p0) && near(s.scale, 1.0f));          // leaves the hand
            if (target) {
                float d[3] = {r.aim[0] - r.p0[0], r.aim[1] - r.p0[1], r.aim[2] - r.p0[2]}; const float L = len3(d);
                for (float& x : d) x /= L;
                CHECK(near3(s.dir, d, 1e-3f));                           // pointing at the target from the start
            }
        }
        if (t == 34) std::memcpy(r.E, R(NOC + 0x80), 12);
    }
    return r;
}

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69,12.5,1.8,0.5 warpfx=83,5,1.0,1.124,84;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && g_hooks.count(0x0880EA20u));
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x394u, CTRL_N); W32(CTRL_N + 0x0Cu, GM_N); W32(GM_N + 0x0Cu, BONES_N); W16(GM_N + 0x20u, NB);
    W32(BARTZ + 0x394u, CTRL_B); W32(CTRL_B + 0x0Cu, GM_B); W32(GM_B + 0x0Cu, BONES_B); W16(GM_B + 0x20u, NB);
    WF(NOC + 0x3C8u, 1.0f); WF(BARTZ + 0x3C8u, 1.0f); WF(0x089D5EACu, 1.0f);   // dt 1 tick per call

    // 1. level target 8 m ahead: the sword arrives after FLY ticks, point at the target's centre, and stays there
    //    while he travels; the wisp bone is scaled along +Z by 6.2 / 12.5
    {
        g_yaw = 0.0f;
        WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 8); W32(NOC + 0x2FCu, FOE);
        set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 14; t++) tick(NOC, BONES_N, 69, t);
        const float p0[3] = {HAND[0], 3 + HAND[1], HAND[2]}, aim[3] = {0, 3 + AIM, 8};
        float d[3] = {aim[0] - p0[0], aim[1] - p0[1], aim[2] - p0[2]}; const float L = len3(d); for (float& x : d) x /= L;
        for (int t = 14; t < 20; t++) {
            tick(NOC, BONES_N, 69, t);
            const Sword s = sword(NOC, BONES_N);
            const float u = std::min((t - 14) / FLY, 1.0f);
            float want[3]; for (int i = 0; i < 3; i++) want[i] = p0[i] + d[i] * (L - TIP) * u;
            CHECK(near3(s.grip, want) && near3(s.dir, d, 1e-3f));
            if (t == 19) CHECK(near3(s.tip, aim));
            float M[16]; get(bone_mat(BONES_N, WISP), M);
            CHECK(near(M[10], 8.0f) && near(M[0], 8.0f));                  // no travel yet: the motion's own pose
        }
        for (int t = 20; t < 34; t++) {
            tick(NOC, BONES_N, 69, t);
            CHECK(near3(sword(NOC, BONES_N).tip, aim));                     // stays at the target while he warps
            float M[16]; get(bone_mat(BONES_N, WISP), M);
            const float f = (8.0f - STOP) / TRAVEL;
            CHECK(near(M[10], 8.0f * f) && near(M[0], 8.0f) && near(M[5], 8.0f) && near(M[8], 0.0f) && near(M[9], 0.0f));
            CHECK(near(M[12], G[0]) && near(M[13], G[1]) && near(M[14], G[2] * f));   // about the model origin
        }
        CHECK(near(RF(NOC + 0x88), 8.0f - STOP));
        tick(NOC, BONES_N, 69, 34);
        CHECK(sword(NOC, BONES_N).scale < 1e-6f);                          // the strike: hidden again, as the motion says
        // frames the engine does not re-pose: no compounding
        float M1[16], M2[16];
        get(bone_mat(BONES_N, WISP), M1);
        tick(NOC, BONES_N, 69, 35, false); tick(NOC, BONES_N, 69, 36, false);
        get(bone_mat(BONES_N, WISP), M2);
        CHECK(std::memcmp(M1, M2, 64) == 0);
        // the next motion (Hazard Raid press 1, motion 49) on a frame the engine does not re-pose: bone 84 gets the
        // motion's own pose back, and from then on is left alone
        set_motion(NOC, 10010, 49, ROWS + 16);
        tick(NOC, BONES_N, 49, 0, false);
        get(bone_mat(BONES_N, WISP), M2);
        CHECK(near(M2[10], 8.0f) && near(M2[14], G[2]) && near(M2[0], 8.0f));
        for (int t = 1; t < 10; t++) {
            tick(NOC, BONES_N, 49, t);
            get(bone_mat(BONES_N, WISP), M2);
            CHECK(near(M2[10], 8.0f) && near(M2[14], G[2]));
        }
    }
    // 2. target up, to the side, and Noctis turned 90 deg: blade along the flight in world space, wisp +Z along S -> E
    {
        const Run r = warp(FOE, 4.0f, 3.0f, 6.0f, 1.5708f);
        const Sword s = sword(NOC, BONES_N);
        CHECK(s.scale < 1e-6f);
        // re-run the flight and look at tick 19 and the wisp during the travel
        g_yaw = 1.5708f; WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 4, 6, 6); set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 20; t++) tick(NOC, BONES_N, 69, t);
        CHECK(near3(sword(NOC, BONES_N).tip, r.aim));
        for (int t = 20; t < 28; t++) tick(NOC, BONES_N, 69, t);
        float M[16]; get(bone_mat(BONES_N, WISP), M);
        float zw[3]; to_world(NOC, M + 8, 0.0f, zw);                          // wisp +Z in world
        const float V[3] = {r.E[0] - r.S[0], r.E[1] - r.S[1], r.E[2] - r.S[2]};
        const float lv = len3(V);
        CHECK(near(len3(zw), 8.0f * lv / TRAVEL, 5e-3f));
        for (int i = 0; i < 3; i++) CHECK(near(zw[i] / len3(zw), V[i] / lv, 1e-3f));
        float xw[3]; to_world(NOC, M, 0.0f, xw);
        CHECK(near(len3(xw), 8.0f) && near(xw[0] * V[0] + xw[1] * V[1] + xw[2] * V[2], 0.0f, 1e-2f));   // across: unscaled
        for (int t = 28; t < 41; t++) tick(NOC, BONES_N, 69, t);
    }
    // 3. far target (30 m): the sword stops TRAVEL + STOP from the release, on the line to the target
    {
        const Run r = warp(FOE, 0.0f, 0.0f, 30.0f, 0.0f);
        g_yaw = 0.0f; WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 30); set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 25; t++) tick(NOC, BONES_N, 69, t);
        const Sword s = sword(NOC, BONES_N);
        CHECK(near(dist3(s.tip, r.p0), TRAVEL + STOP));
        for (int t = 25; t < 41; t++) tick(NOC, BONES_N, 69, t);
    }
    // 4. no target: both bones as the motion poses them
    {
        g_yaw = 0.0f; WV(NOC + 0x80, 0, 3, 0); W32(NOC + 0x2FCu, 0); set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 34; t++) {
            tick(NOC, BONES_N, 69, t);
            float M[16]; get(bone_mat(BONES_N, SWORD), M);
            CHECK(near(M[14], t >= 14 ? HAND[2] + 2.0f * (t - 14) : HAND[2]) && near(M[8], 0.0f) && near(M[10], t >= 14 ? 1.0f : 0.0f));
            get(bone_mat(BONES_N, WISP), M);
            CHECK(near(M[10], 8.0f) && near(M[14], G[2]));
        }
        W32(NOC + 0x2FCu, FOE);
    }
    // 5. a near target already in reach (1 m): no travel -> wisps hidden (scale 0); the sword still goes to it
    {
        g_yaw = 0.0f; WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 1); set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 25; t++) tick(NOC, BONES_N, 69, t);
        float M[16]; get(bone_mat(BONES_N, WISP), M);
        CHECK(near(M[0], 0.0f) && near(M[5], 0.0f) && near(M[10], 0.0f));   // travel 0: wisps scaled to nothing
        CHECK(near(RF(NOC + 0x88), 0.0f));
        CHECK(near(sword(NOC, BONES_N).tip[2], 1.0f, 0.2f) || dist3(sword(NOC, BONES_N).grip, sword(NOC, BONES_N).tip) > 1.0f);
        for (int t = 25; t < 41; t++) tick(NOC, BONES_N, 69, t);
    }
    // 6. back-to-back: the second throw starts from its own release point
    {
        const Run a = warp(FOE, 0.0f, 0.0f, 6.0f, 0.0f);
        (void)a;
        g_yaw = 0.0f; WV(FOE + 0x80, 0, 3, 20);
        const float x0 = RF(NOC + 0x88);
        WF(NOC + 0x1A8, 0.0f);
        for (int t = 0; t < 15; t++) tick(NOC, BONES_N, 69, t);
        const Sword s = sword(NOC, BONES_N);
        const float p0[3] = {HAND[0], 3 + HAND[1], x0 + HAND[2]};
        CHECK(near3(s.grip, p0));
        for (int t = 15; t < 41; t++) tick(NOC, BONES_N, 69, t);
    }
    // 7. Bartz's motion 69 (no warp= entry): untouched
    {
        WV(BARTZ + 0x80, 0, 3, 0); W32(BARTZ + 0x2FCu, FOE); set_motion(BARTZ, 10092, 69, ROWS + 32);
        for (int t = 0; t < 34; t++) {
            tick(BARTZ, BONES_B, 69, t);
            float M[16]; get(bone_mat(BONES_B, SWORD), M);
            CHECK(near(M[14], t >= 14 ? HAND[2] + 2.0f * (t - 14) : HAND[2]));
            get(bone_mat(BONES_B, WISP), M);
            CHECK(near(M[10], 8.0f));
        }
    }
    // 9. `warp=69/85,12.5,1.8,0.5 warp=90,6,1,0 warpfx=..`: the ground Warp-Strike 85 gets the same sword flight and
    //    wisps as 69 (from the floor); motion 90 (a later warp= token: homing only) keeps the motion's own sword / wisp pose
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69/85,12.5,1.8,0.5 warp=90,6,1,0 warpfx=83,5,1.0,1.124,84;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && g_hooks.count(0x0880EA20u));
    {
        g_yaw = 0.0f;
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 8); W32(NOC + 0x2FCu, FOE);
        set_motion(NOC, 10097, 85, ROWS);
        for (int t = 0; t < 14; t++) { tick(NOC, BONES_N, 85, t); CHECK(sword(NOC, BONES_N).scale < 1e-6f); }
        const float p0[3] = {HAND[0], HAND[1], HAND[2]}, aim[3] = {0, AIM, 8};
        for (int t = 14; t < 20; t++) tick(NOC, BONES_N, 85, t);
        CHECK(near3(sword(NOC, BONES_N).tip, aim));
        {
            const Sword s0 = sword(NOC, BONES_N);
            float d[3] = {aim[0] - p0[0], aim[1] - p0[1], aim[2] - p0[2]}; const float L = len3(d); for (float& x : d) x /= L;
            CHECK(near3(s0.dir, d, 1e-3f));
        }
        for (int t = 20; t < 34; t++) {
            tick(NOC, BONES_N, 85, t);
            CHECK(near3(sword(NOC, BONES_N).tip, aim));
            float M[16]; get(bone_mat(BONES_N, WISP), M);
            CHECK(near(M[10], 8.0f * (8.0f - STOP) / TRAVEL));
        }
        CHECK(near(RF(NOC + 0x88), 8.0f - STOP) && near(RF(NOC + 0x84), 0.0f));
        for (int t = 34; t < 41; t++) tick(NOC, BONES_N, 85, t);
        // motion 90: homing (its own 6 m) but the bones as the motion poses them
        WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 5);
        set_motion(NOC, 10200, 90, ROWS + 48);
        for (int t = 0; t < 34; t++) {
            g_step[NOC] = (t >= 20 && t < 34) ? CURVE[t - 20] * 6.0f / 12.5f : 0.0f;
            recomp_context ctx{};
            ctx.r[4] = static_cast<int32_t>(OUT); ctx.r[5] = static_cast<int32_t>(NOC);
            g_hooks.at(0x08832EB8u)(g_ram.data(), &ctx);
            for (int i = 0; i < 3; i++) WF(NOC + 0x80 + 4 * i, RF(NOC + 0x80 + 4 * i) + RF(OUT + 4 * i));
            WF(NOC + 0x1A8, RF(NOC + 0x1A8) + 1.0f);
            world(NOC); engine_pose(BONES_N, 90, t); bone_post(NOC);
            float M[16]; get(bone_mat(BONES_N, SWORD), M);
            CHECK(near(M[14], t >= 14 ? HAND[2] + 2.0f * (t - 14) : HAND[2]) && near(M[10], t >= 14 ? 1.0f : 0.0f));
            get(bone_mat(BONES_N, WISP), M);
            CHECK(near(M[10], 8.0f) && near(M[14], G[2]));
        }
        CHECK(near(RF(NOC + 0x88), 5.0f - 1.0f));                         // its own STOP 1 m
    }
    // 10. warpreach=90,30 on a warpfx motion (the warp dash): a 25 m target is reached (the sword's point at its centre,
    //     no 14.3 m stop), he ends 1.8 short, and the wisps are scaled to the whole way (23.2 / 12.5); a 45 m target: the
    //     sword stops REACH + STOP from the release
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69/90,12.5,1.8,0.5 warpreach=90,30 warpfx=83,5,1.0,1.124,84;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && g_hooks.count(0x0880EA20u));
    {
        g_yaw = 0.0f;
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 25); W32(NOC + 0x2FCu, FOE);
        set_motion(NOC, 10200, 90, ROWS + 48);
        for (int t = 0; t < 20; t++) tick(NOC, BONES_N, 90, t);
        const float aim[3] = {0, AIM, 25};
        CHECK(near3(sword(NOC, BONES_N).tip, aim));
        for (int t = 20; t < 34; t++) {
            tick(NOC, BONES_N, 90, t);
            float M[16]; get(bone_mat(BONES_N, WISP), M);
            CHECK(near(M[10], 8.0f * (25.0f - STOP) / TRAVEL, 5e-3f));
        }
        CHECK(near(RF(NOC + 0x88), 25.0f - STOP, 5e-3f));
        for (int t = 34; t < 41; t++) tick(NOC, BONES_N, 90, t);
        WV(NOC + 0x80, 0, 0, 0); WV(FOE + 0x80, 0, 0, 45);
        set_motion(NOC, 10200, 90, ROWS + 48);
        for (int t = 0; t < 25; t++) tick(NOC, BONES_N, 90, t);
        const float p0[3] = {HAND[0], HAND[1], HAND[2]};
        CHECK(near(dist3(sword(NOC, BONES_N).tip, p0), 30.0f + STOP, 5e-3f));
        for (int t = 25; t < 41; t++) tick(NOC, BONES_N, 90, t);
        CHECK(near(RF(NOC + 0x88), 30.0f, 5e-3f));
        // Warp-Strike 69 in the same token: still 12.5 + 1.8
        const Run r = warp(FOE, 0.0f, 0.0f, 30.0f, 0.0f);
        g_yaw = 0.0f; WV(NOC + 0x80, 0, 3, 0); WV(FOE + 0x80, 0, 3, 30); set_motion(NOC, 10092, 69, ROWS);
        for (int t = 0; t < 25; t++) tick(NOC, BONES_N, 69, t);
        CHECK(near(dist3(sword(NOC, BONES_N).tip, r.p0), TRAVEL + STOP));
        for (int t = 25; t < 41; t++) tick(NOC, BONES_N, 69, t);
    }
    // 8. tokens: bad warpfx= is ignored (no bone pass without armiger=); warpfx= alone installs the bone pass
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=69,12.5,1.8,0.5 warpfx=83,abc", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && !g_hooks.count(0x0880EA20u));
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 warp=69,12.5,1.8,0.5", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08832EB8u) && !g_hooks.count(0x0880EA20u));
    std::printf("ok test_warp_fx (%d bone passes)\n", g_bone_orig);
    return 0;
}
