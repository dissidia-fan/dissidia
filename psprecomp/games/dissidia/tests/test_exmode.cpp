// Host test for roster.cpp's EX Mode hooks (roster.txt exranged=N, armiger=...): fake guest RAM, fake originals.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_exmode test_exmode.cpp && /tmp/test_exmode
// (lives next to test_roster_hooks.cpp / test_dash_speed.cpp; includes ../runtime/roster.cpp the same way)
#include "../runtime/roster.cpp"
#include <cmath>
#include <cstdlib>
#include <map>

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
static int g_can_hit_calls = 0, g_bone_calls = 0, g_fx_calls = 0;
static int32_t g_fx_args[5];
void orig_can_hit(uint8_t*, recomp_context* ctx) { g_can_hit_calls++; ctx->r[2] = 1; }
void orig_bone_post(uint8_t*, recomp_context* ctx) { g_bone_calls++; ctx->r[4] = ctx->r[5] = ctx->r[6] = 0; }
void orig_other(uint8_t*, recomp_context*) {}
void fake_play_fx(uint8_t*, recomp_context* ctx) { g_fx_calls++; for (int i = 0; i < 4; i++) g_fx_args[i] = ctx->r[4 + i]; g_fx_args[4] = ctx->r[8];
                                                   ctx->r[2] = 0x1234; ctx->r[4] = ctx->r[5] = ctx->r[6] = ctx->r[16] = -1; }
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x08807D4Cu) return orig_can_hit;
    if (a == 0x0880EA20u) return orig_bone_post;
    if (a == 0x088A9DE8u) return fake_play_fx;
    return orig_other;
}
uint8_t* R(uint32_t a) { return g_ram.data() + (a & 0x07FFFFFFu); }
void W32(uint32_t a, uint32_t v) { std::memcpy(R(a), &v, 4); }
void W16(uint32_t a, uint16_t v) { std::memcpy(R(a), &v, 2); }
void WF(uint32_t a, float v) { std::memcpy(R(a), &v, 4); }
float RF(uint32_t a) { float f; std::memcpy(&f, R(a), 4); return f; }

// guest layout
constexpr uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, SUMMON = 0x09230000u;
constexpr uint32_t INFO_N = 0x09240000u, INFO_B = 0x09240100u, CHRP_N = 0x09240200u, CHRP_B = 0x09240300u;
constexpr uint32_t HIT = 0x09250000u;
constexpr uint32_t CTRL = 0x09260000u, GM = 0x09261000u, BONES = 0x09270000u, VIS = 0x09262000u;
constexpr int NBONES = 192;
constexpr int ARM0 = 181, NARM = 11;
constexpr uint32_t FXP = 0x09263000u, MGR = 0x09264000u;

int can_hit(uint32_t hit, uint32_t def) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(hit); ctx.r[5] = static_cast<int32_t>(def); ctx.r[2] = 7;
    g_hooks.at(0x08807D4Cu)(g_ram.data(), &ctx);
    return ctx.r[2];
}
void bone_post(uint32_t actor) {
    recomp_context ctx{};
    ctx.r[4] = 0x09280000; ctx.r[5] = static_cast<int32_t>(GM); ctx.r[6] = static_cast<int32_t>(actor);
    g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx);
}
uint32_t bone_mat(int i) { return BONES + 0x100u * i + 0x80u; }
void set_mat_identity(int i, float tx, float ty, float tz, float s = 1.0f) {
    float m[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, tx, ty, tz, 1};
    std::memcpy(R(bone_mat(i)), m, sizeof m);
}

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis exranged=2 armiger=157,158,159,73,74,14 exarms=181,11,510;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08807D4Cu) && g_hooks.count(0x0880EA20u));
    // actors: Noctis (charId 34) and Bartz (9)
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34; W32(NOC + 0x5A4u, CHRP_N); R(CHRP_N)[0] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9; W32(BARTZ + 0x5A4u, CHRP_B); R(CHRP_B)[0] = 9;
    R(SUMMON + 0x542u)[0] = 3;                               // a summoned / thrown object (wpnp)
    // ---- ranged immunity
    W16(HIT + 0x2Cu, 2); W32(HIT + 0x18u, BARTZ); W32(HIT + 0x20u, BARTZ);   // a projectile from Bartz
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 1);  // not in EX: normal
    R(NOC + 0x5B2u)[0] = 4;                                 // EX Mode
    CHECK(can_hit(HIT, NOC) == 0 && g_can_hit_calls == 1);  // projectile passes through
    W16(HIT + 0x2Cu, 1);                                    // melee hitbox
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 2);
    W32(HIT + 0x18u, SUMMON);                               // melee hit of a thrown weapon / aeon
    CHECK(can_hit(HIT, NOC) == 0 && g_can_hit_calls == 2);
    W32(HIT + 0x18u, BARTZ);
    W16(HIT + 0x2Cu, 2); R(BARTZ + 0x5B2u)[0] = 4;
    CHECK(can_hit(HIT, BARTZ) == 1 && g_can_hit_calls == 3); // Bartz in EX: no token, normal
    R(NOC + 0x542u)[0] = 3;                                  // an object of Noctis's (kind 3) is not him
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 4);
    R(NOC + 0x542u)[0] = 0;
    R(CHRP_N)[0] = 9;                                        // chrp / objentry disagree: not trusted
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 5);
    R(CHRP_N)[0] = 34;
    CHECK(can_hit(0x00001000u, NOC) == 1);                   // bad pointers fall through to the original
    CHECK(can_hit(HIT, 0x00002000u) == 1);

    // ---- armiger
    W32(NOC + 0x394u, CTRL); W32(CTRL + 0x0Cu, GM); W32(GM + 0x0Cu, BONES); W16(GM + 0x20u, NBONES);
    W32(CTRL + 0x10Cu, VIS); R(CTRL + 0x8Eu)[0] = 4;
    const uint8_t vis[12] = {64, 0, 0, 73, 0, 0, 74, 0, 0, 0xFF, 1, 0xFF};
    std::memcpy(R(VIS), vis, sizeof vis);
    WF(NOC + 0x3C8u, 1.0f); WF(0x089D5EACu, 1.0f);           // dt 1 tick per call
    const float yaw = 0.5f * 3.14159265f;                     // facing: 90 degrees about Y
    float Wm[16] = {std::cos(yaw), 0, -std::sin(yaw), 0, 0, 1, 0, 0, std::sin(yaw), 0, std::cos(yaw), 0, 5, 0, 7, 1};
    std::memcpy(R(NOC + 0x330u), Wm, sizeof Wm);
    set_mat_identity(2, 0.1f, 1.08f, 0.2f);                   // root
    set_mat_identity(157, 0, 0.95f, 0);                       // rest poses as libgmo would leave them
    set_mat_identity(158, 0, 0.03f, 0, 0.0f);
    set_mat_identity(159, 0, 1.15f, 0, 0.0f);
    for (int k = 0; k < NARM; k++)                            // arm k on the ring: angle 2 pi k / 11, radius 1.1
        set_mat_identity(ARM0 + k, 1.1f * std::cos(6.2831853f * k / NARM), 0.95f, 1.1f * std::sin(6.2831853f * k / NARM));
    W32(NOC + 0x390u, FXP); W32(FXP + 0x5Cu, MGR);
    bone_post(NOC);                                           // EX parts hidden: nothing changes
    CHECK(g_bone_calls == 1 && RF(bone_mat(157) + 52) == 0.95f && R(VIS + 5)[0] == 0);
    // not in EX Mode, but the engine has the EX parts shown (in game the ring stood around him from the battle start):
    // the hook hides them and leaves the bones alone
    R(NOC + 0x5B2u)[0] = 0;
    R(VIS + 4)[0] = 1; R(VIS + 5)[0] = 0xFF; R(VIS + 7)[0] = 1; R(VIS + 8)[0] = 0xFF;
    bone_post(NOC);
    CHECK(R(VIS + 4)[0] == 0 && R(VIS + 5)[0] == 0 && R(VIS + 7)[0] == 0 && R(VIS + 8)[0] == 0);
    CHECK(RF(bone_mat(157) + 52) == 0.95f);
    // ... and a part with no visibility entry yet (drawn unconditionally) gets one, hidden
    R(VIS + 3)[0] = 0xFF; R(VIS + 4)[0] = 1; R(VIS + 5)[0] = 0xFF;
    bone_post(NOC);
    CHECK(R(VIS + 3)[0] == 73 && R(VIS + 4)[0] == 0 && R(VIS + 5)[0] == 0 && R(VIS + 9)[0] == 0xFF);
    // EX Mode starts and the engine does not show them again (nothing changed in its own lists): the hook does
    R(NOC + 0x5B2u)[0] = 4;
    bone_post(NOC);
    CHECK(R(VIS + 4)[0] == 1 && R(VIS + 7)[0] == 1 && R(VIS + 5)[0] > 0 && R(VIS + 5)[0] < 40 && RF(bone_mat(157) + 52) != 0.95f);
    // EX Mode ends, the engine again does nothing: hidden, body opaque
    R(NOC + 0x5B2u)[0] = 0;
    bone_post(NOC);
    CHECK(R(VIS + 4)[0] == 0 && R(VIS + 7)[0] == 0 && R(VIS + 9)[0] == 14 && R(VIS + 11)[0] == 255);
    // back to the engine-driven sequence below (EX on, parts hidden by the engine, rest poses)
    R(NOC + 0x5B2u)[0] = 4;
    set_mat_identity(157, 0, 0.95f, 0);
    set_mat_identity(158, 0, 0.03f, 0, 0.0f);
    set_mat_identity(159, 0, 1.15f, 0, 0.0f);
    R(VIS + 4)[0] = 1; R(VIS + 5)[0] = 0xFF; R(VIS + 7)[0] = 1; R(VIS + 8)[0] = 0xFF;   // EX starts: engine shows 73/74
    bone_post(NOC);
    const float t1 = 1.0f / 60.0f;
    // ring: low, small, spinning up; at the root's x/z
    CHECK(std::fabs(RF(bone_mat(157) + 48) - 0.1f) < 1e-6f && std::fabs(RF(bone_mat(157) + 56) - 0.2f) < 1e-6f);
    CHECK(RF(bone_mat(157) + 52) < 1.08f - 0.13f - 0.2f);
    const float row0 = std::sqrt(RF(bone_mat(157)) * RF(bone_mat(157)) + RF(bone_mat(157) + 4) * RF(bone_mat(157) + 4) + RF(bone_mat(157) + 8) * RF(bone_mat(157) + 8));
    CHECK(row0 > 0.15f && row0 < 0.3f);
    CHECK(R(VIS + 5)[0] > 0 && R(VIS + 5)[0] < 40);           // fading in (translucent pass)
    CHECK(R(VIS + 8)[0] == 235);                              // activation pieces at full
    CHECK(R(VIS + 9)[0] == 14 && R(VIS + 10)[0] == 1 && R(VIS + 11)[0] < 255 && R(VIS + 11)[0] > 96);   // body entry added, dimming
    auto rown = [&](int b, int row) { float l = 0; for (int j = 0; j < 3; j++) l += RF(bone_mat(b) + 16 * row + 4 * j) * RF(bone_mat(b) + 16 * row + 4 * j); return std::sqrt(l); };
    CHECK(rown(158, 0) > 0.3f && rown(158, 0) < 0.5f && std::fabs(rown(158, 1) - 1.0f) < 1e-5f && std::fabs(RF(bone_mat(158) + 52) - 0.03f) < 1e-6f);
    CHECK(rown(159, 0) > 0.5f && rown(159, 0) < 0.7f && std::fabs(RF(bone_mat(159) + 52) - 1.15f) < 1e-6f);
    (void)t1;
    for (int i = 0; i < 14; i++) bone_post(NOC);             // 0.25 s: body at its most translucent
    CHECK(R(VIS + 11)[0] == 96);
    for (int i = 0; i < 105; i++) bone_post(NOC);            // 2 s in
    CHECK(R(VIS + 11)[0] == 255);                             // body opaque again
    CHECK(R(VIS + 5)[0] == 205 && R(VIS + 8)[0] == 0);
    CHECK(std::fabs(RF(bone_mat(157) + 52) - (1.08f - 0.13f)) < 1e-4f);
    // world orientation of the ring = M3 * W must be a pure rotation about Y by the orbit angle, whatever the facing
    auto world_angle = [&]() {
        float M[9], Wr[9], Ww[9];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { M[3 * i + j] = RF(bone_mat(157) + 16 * i + 4 * j); Wr[3 * i + j] = RF(NOC + 0x330u + 16 * i + 4 * j); }
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Ww[3 * i + j] = M[3 * i] * Wr[j] + M[3 * i + 1] * Wr[3 + j] + M[3 * i + 2] * Wr[6 + j];
        CHECK(std::fabs(Ww[4] - 1.0f) < 1e-4f && std::fabs(Ww[1]) < 1e-4f && std::fabs(Ww[3]) < 1e-4f);
        return std::atan2(-Ww[2], Ww[0]);
    };
    const float a0 = world_angle();
    // he turns 90 degrees the other way: one more tick, the ring's world angle moves by one tick of orbit only
    float W2[16] = {std::cos(-yaw), 0, -std::sin(-yaw), 0, 0, 1, 0, 0, std::sin(-yaw), 0, std::cos(-yaw), 0, 5, 0, 7, 1};
    std::memcpy(R(NOC + 0x330u), W2, sizeof W2);
    bone_post(NOC);
    float d = world_angle() - a0;
    while (d > 3.14159f) d -= 6.28318f;
    while (d < -3.14159f) d += 6.28318f;
    CHECK(std::fabs(d) < 0.03f);                               // 40 deg/s -> 0.0116 rad per tick
    // slow motion (dt 0.25): the orbit slows down with him
    WF(0x089D5EACu, 0.25f);
    const float a1 = world_angle(); bone_post(NOC); float d2 = world_angle() - a1;
    CHECK(std::fabs(d2) < 0.005f && std::fabs(d2) > 0.001f);
    WF(0x089D5EACu, 1.0f);
    // ---- the arms: on the ring around the orbit centre, upright, each spinning about its own axis
    auto mat = [&](int b, float* M) { std::memcpy(M, R(bone_mat(b)), 64); };
    auto rel_angle = [&](int k) {                             // arm k's rotation about +Y relative to the orbit
        float A[16], O[16], Oi[16], Rm[16]; mat(ARM0 + k, A); mat(157, O); CHECK(mat_inv_affine(O, Oi)); mat_mul(A, Oi, Rm);
        return std::atan2(-Rm[2], Rm[0]);
    };
    auto wrap = [](float d) { while (d > 3.14159f) d -= 6.28318f; while (d < -3.14159f) d += 6.28318f; return d; };
    {
        float O[16]; mat(157, O);
        for (int k = 0; k < NARM; k++) {
            float A[16]; mat(ARM0 + k, A);
            const float dx = A[12] - O[12], dy = A[13] - O[13], dz = A[14] - O[14];
            CHECK(std::fabs(std::sqrt(dx * dx + dz * dz) - 1.1f) < 2e-3f && std::fabs(dy) < 1e-4f);      // grow is 1 now
            CHECK(std::fabs(std::sqrt(A[4] * A[4] + A[5] * A[5] + A[6] * A[6]) - 1.0f) < 1e-3f);           // rigid
        }
    }
    float a0s = rel_angle(0); bone_post(NOC); float d_idle = wrap(rel_angle(0) - a0s);
    CHECK(std::fabs(std::fabs(d_idle) - 150.0f / 60.0f * 3.14159265f / 180.0f) < 2e-3f);                 // 150 deg/s idle
    CHECK(std::fabs(wrap(rel_angle(5) - rel_angle(0)) - wrap(33.0f * 5 * 3.14159265f / 180.0f)) < 2e-3f);     // phases differ
    // he runs: 0.12 units / tick -> spin and orbit speed up (smoothed)
    float Wr[16]; std::memcpy(Wr, R(NOC + 0x330u), 64);
    for (int i = 0; i < 40; i++) { Wr[14] += 0.12f; std::memcpy(R(NOC + 0x330u), Wr, 64); bone_post(NOC); }
    a0s = rel_angle(0); Wr[14] += 0.12f; std::memcpy(R(NOC + 0x330u), Wr, 64); bone_post(NOC);
    const float d_run = wrap(rel_angle(0) - a0s);
    CHECK(std::fabs(std::fabs(d_run) - 260.0f / 60.0f * 3.14159265f / 180.0f) < 0.01f);
    for (int i = 0; i < 40; i++) bone_post(NOC);              // stands still again: back to idle
    a0s = rel_angle(0); bone_post(NOC);
    CHECK(std::fabs(std::fabs(wrap(rel_angle(0) - a0s)) - std::fabs(d_idle)) < 0.004f);
    // a projectile from Bartz (to his +X in world space) is deflected: no hit, a sparkle on the arm facing Bartz, a burst
    float Wb[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, Wr[12] + 10.0f, 0, Wr[14], 1};
    std::memcpy(R(BARTZ + 0x330u), Wb, 64);
    W16(HIT + 0x2Cu, 2); W32(HIT + 0x18u, BARTZ); W32(HIT + 0x20u, BARTZ);
    const int calls0 = g_can_hit_calls;
    CHECK(can_hit(HIT, NOC) == 0 && g_can_hit_calls == calls0 && g_fx_calls == 1);
    CHECK(g_fx_args[0] == static_cast<int32_t>(MGR) && g_fx_args[2] == static_cast<int32_t>(NOC + 0x4C8u) && g_fx_args[3] == 0 && g_fx_args[4] == 0);
    {
        const int k = g_fx_args[1] - 510; CHECK(k >= 0 && k < NARM);
        float A[16], W[16]; mat(ARM0 + k, A); std::memcpy(W, R(NOC + 0x330u), 64);
        float best = -1e9f;
        for (int j = 0; j < NARM; j++) { float B[16]; mat(ARM0 + j, B); best = std::max(best, B[12] * W[0] + B[13] * W[4] + B[14] * W[8]); }
        CHECK(std::fabs((A[12] * W[0] + A[13] * W[4] + A[14] * W[8]) - best) < 1e-5f);   // the arm furthest towards +X
    }
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == 1);         // same volley next tick: no second sparkle (cooldown)
    a0s = rel_angle(0); bone_post(NOC);
    const float d_burst = wrap(rel_angle(0) - a0s);
    CHECK(std::fabs(d_burst) > 3.0f * std::fabs(d_idle));       // spins much faster right after the deflect
    for (int i = 0; i < 12; i++) bone_post(NOC);
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == 2);         // cooldown over: the next hit sparkles again
    for (int i = 0; i < 180; i++) bone_post(NOC);             // the burst dies away
    a0s = rel_angle(0); bone_post(NOC);
    CHECK(std::fabs(std::fabs(wrap(rel_angle(0) - a0s)) - std::fabs(d_idle)) < 0.004f);
    R(NOC + 0x5B2u)[0] = 0; CHECK(can_hit(HIT, NOC) == 1 && g_fx_calls == 2); R(NOC + 0x5B2u)[0] = 4;   // not in EX: a hit
    // EX ends: engine hides the parts -> untouched; EX again -> the intro replays
    R(VIS + 4)[0] = 0; R(VIS + 5)[0] = 0; R(VIS + 7)[0] = 0; R(VIS + 8)[0] = 0;
    set_mat_identity(157, 0, 0.95f, 0);
    bone_post(NOC);
    CHECK(RF(bone_mat(157) + 52) == 0.95f);
    R(VIS + 4)[0] = 1; R(VIS + 5)[0] = 0xFF; R(VIS + 7)[0] = 1; R(VIS + 8)[0] = 0xFF;
    bone_post(NOC);
    CHECK(R(VIS + 5)[0] < 40 && R(VIS + 8)[0] == 235);
    for (int i = 0; i < 10; i++) bone_post(NOC);
    CHECK(R(VIS + 11)[0] < 255);
    R(VIS + 4)[0] = 0; R(VIS + 7)[0] = 0;                    // EX ends mid-flash: the body is made opaque again
    bone_post(NOC);
    CHECK(R(VIS + 11)[0] == 255 && R(VIS + 10)[0] == 1);
    // a model without the bones (assist): nothing written
    W16(GM + 0x20u, 83); set_mat_identity(2, 9, 9, 9);
    std::memcpy(R(bone_mat(157)), std::vector<float>(16, 3.0f).data(), 64);
    bone_post(NOC);
    CHECK(RF(bone_mat(157)) == 3.0f);
    W16(GM + 0x20u, NBONES);
    // Bartz (no token): never touched
    W32(BARTZ + 0x394u, CTRL);
    std::memcpy(R(bone_mat(157)), std::vector<float>(16, 3.0f).data(), 64);
    bone_post(BARTZ);
    CHECK(RF(bone_mat(157)) == 3.0f);
    // no tokens anywhere: no hooks
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08807D4Cu) && !g_hooks.count(0x0880EA20u));
    std::printf("ok test_exmode\n");
    return 0;
}
