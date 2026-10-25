// Host test for roster.cpp's arms-riding-magi hook with several arm sets and spin (roster.txt `apocarms=` given more than
// once, `pinspin=BONE,DEG,AX,AY,AZ`; Armiger Onslaught's thrown Star of the Rogue, 2026-10-20). Fake guest RAM, a fake
// FUN_08AD20BC (bullet visual pass) and FUN_0880EA20 (bone_post_hook -> apoc_bone_post), as test_apoc_arms.cpp.
// Checks: the tokens parse (first set in the old fields, further sets after it, bad pinspin= ignored); a bullet only goes
// to an arm of a set that lists its record (a 2220 bullet next to the shown star is not taken by it); the star rides its
// 2230 bullet with the axis in its spin plane along the flight, its spin axis across it, turning DEG per pass, rows
// orthonormal; it is hidden once the bullet is gone; an Apocalypsis arm still takes its own bullet; one token as before.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_pin_arms test_pin_arms.cpp; /tmp/test_pin_arms
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>
#include <algorithm>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
static int g_fx_calls = 0;
void orig_fxdir(uint8_t*, recomp_context* ctx) { g_fx_calls++; ctx->r[4] = ctx->r[5] = 0; }
FuncPtr ovl_battle_lookup(uint32_t a) { return a == 0x08AD20BCu ? orig_fxdir : nullptr; }
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
static int g_bone_orig = 0;
void orig_bone_post(uint8_t*, recomp_context* ctx) { g_bone_orig++; ctx->r[4] = ctx->r[5] = ctx->r[6] = 0; }
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) { return a == 0x0880EA20u ? orig_bone_post : orig_other; }

const uint32_t NOC = 0x09210000u, INFO_N = 0x09230000u, CTRL_N = 0x09260000u, GM_N = 0x09261000u, BONES_N = 0x09270000u;
const uint32_t BUL = 0x09300000u, EMIT = 0x09310000u, REC = 0x09320000u;
const int NB = 201, STAR = 81, ARM0 = 168, NARM = 12;
const uint32_t OVL_WORD = 0x089DD4C0u;
uint32_t bone_mat(int k) { return BONES_N + 0x100u * k + 0x80u; }
void put(uint32_t a, const float* M) { std::memcpy(R(a), M, 64); }
void get(uint32_t a, float* M) { std::memcpy(M, R(a), 64); }
float len3(const float* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float dist3(const float* a, const float* b) { const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return len3(d); }
bool near(float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; }
const float YAW = 0.4f, POS[3] = {1.0f, 2.0f, -3.0f};
void world() {
    const float c = std::cos(YAW), s = std::sin(YAW);
    const float W[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, POS[0], POS[1], POS[2], 1};
    std::memcpy(R(NOC + 0x330u), W, 64);
}
void to_world(const float* p, float w, float* o) {
    float W[16]; get(NOC + 0x330u, W);
    for (int j = 0; j < 3; j++) o[j] = p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + w * W[12 + j];
}
void to_model(const float* p, float w, float* o) {
    float W[16]; get(NOC + 0x330u, W);
    float q[3] = {p[0] - w * W[12], p[1] - w * W[13], p[2] - w * W[14]};
    for (int i = 0; i < 3; i++) o[i] = q[0] * W[4 * i] + q[1] * W[4 * i + 1] + q[2] * W[4 * i + 2];
}
const float HAND[3] = {0.5f, 1.2f, 0.4f};
void crown(int k, float* p) { const float a = 0.5235988f * k; p[0] = 5.0f * std::sin(a); p[1] = 2.5f; p[2] = 5.0f * std::cos(a); }
void engine_pose(int f) {
    float M[16] = {};
    if (f < 10) {                       // the star in the hand: a rolled frame, scale 1
        const float c = std::cos(0.3f), s = std::sin(0.3f);
        const float S[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, HAND[0], HAND[1], HAND[2], 1};
        std::memcpy(M, S, sizeof M);
    } else { M[13] = 1.97f; M[15] = 1.0f; }
    put(bone_mat(STAR), M);
    for (int k = 0; k < NARM; k++) {    // the crown, 5 m out (out of reach of the star's bullets), gone at f20
        float A[16] = {}, p[3]; crown(k, p);
        if (f < 20) { A[0] = 1; A[5] = 1; A[10] = 1; A[12] = p[0]; A[13] = p[1]; A[14] = p[2]; A[15] = 1; }
        else { A[13] = 1.4f; A[15] = 1; }
        put(bone_mat(ARM0 + k), A);
    }
}
void bone_post() { recomp_context ctx{}; ctx.r[6] = static_cast<int32_t>(NOC); g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx); }
uint32_t bullet(int i) { return BUL + 0x100u * i; }
void make_bullet(int i, uint16_t rec_id) { W32(bullet(i) + 0x64u, EMIT + 0x40u * i); W32(EMIT + 0x40u * i + 0x24u, REC + 0x40u * i); W16(REC + 0x40u * i, rec_id); }
void set_bullet(int i, const float* p, const float* d) {
    for (int j = 0; j < 3; j++) { WF(bullet(i) + 4 * j, p[j]); WF(bullet(i) + 0x20u + 4 * j, d[j]); }
    WF(bullet(i) + 12, 1.0f); WF(bullet(i) + 0x2Cu, 0.0f);
}
void visual(int i) { recomp_context ctx{}; ctx.r[4] = 0x09400000; ctx.r[5] = static_cast<int32_t>(bullet(i)); g_hooks.at(0x08AD20BCu)(g_ram.data(), &ctx); }

int main() {
    // parsing: the first apocarms= in the old fields, the next as a further set; pinspin= (normalised); bad ones ignored
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis apocarms=81,1,2230 pinspin=81,40,0.844,-0.537,0 "
                               "apocarms=168,12,2220,2222,2223,2228 warpfx=83,5,1.0,1.124,84;33 viv100 21 pinspin=81,40,0,0,0 "
                               "pinspin=81,400,1,0,0 pinspin=81,40,1,0;35 ysh100 28 apocarms=1,2,5", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08AD20BCu) && g_hooks.count(0x0880EA20u));
    const Entry* en = nullptr;
    for (const Entry& e : g_entries) {
        if (e.charId == 34) en = &e;
        if (e.charId == 33) CHECK(e.pin_nspin == 0 && e.apoc_narm == 0);
        if (e.charId == 35) CHECK(e.apoc_arm0 == 1 && e.apoc_narm == 2 && e.apoc_nmore == 0);   // one token: as before
    }
    CHECK(en && en->apoc_arm0 == STAR && en->apoc_narm == 1 && en->apoc_nids == 1 && en->apoc_ids[0] == 2230);
    CHECK(en->apoc_nmore == 1 && en->apoc_more[0].arm0 == ARM0 && en->apoc_more[0].narm == NARM && en->apoc_more[0].nids == 4 &&
          en->apoc_more[0].ids[3] == 2228 && en->warp_sword == 83);
    CHECK(en->pin_nspin == 1 && en->pin_spin[0].bone == STAR && near(en->pin_spin[0].deg, 40.0f) && near(len3(en->pin_spin[0].ax), 1.0f, 1e-4f));
    W32(OVL_WORD, 1u);
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(NOC + 0x394u, CTRL_N); W32(CTRL_N + 0x0Cu, GM_N); W32(GM_N + 0x0Cu, BONES_N); W16(GM_N + 0x20u, NB);
    world();
    // bullet 0: the star's 2230 from the hand at f10, straight ahead; bullet 1: a 2220 from the same point the other way
    // (straight down; nobody may take it: the star's set does not list 2220, the crown stays over 3 m away); bullet 2: a 2220 from arm 3's
    // place when it vanishes (f20): arm 3 takes it
    make_bullet(0, 2230); make_bullet(1, 2220); make_bullet(2, 2220);
    float hw[3], d0[3], d1[3], p0[3], p1[3], p2[3], d2[3];
    to_world(HAND, 1.0f, hw);
    const float dm0[3] = {0.0f, 0.0f, 1.0f}, dm1[3] = {0.0f, -1.0f, 0.0f}, dm2[3] = {0.0f, 1.0f, 0.0f};
    to_world(dm0, 0.0f, d0); to_world(dm1, 0.0f, d1); to_world(dm2, 0.0f, d2);
    std::memcpy(p0, hw, sizeof hw); std::memcpy(p1, hw, sizeof hw);
    { float c3[3]; crown(3, c3); to_world(c3, 1.0f, p2); }
    const float A_l[3] = {0.844f / 0.99999f, -0.537f, 0.0f};
    float prevR[9] = {}; bool have_prev = false; int spins = 0;
    for (int f = 0; f < 40; f++) {
        engine_pose(f);
        const bool a0 = f >= 10 && f < 22, a1 = f >= 10 && f < 22, a2 = f >= 20 && f < 32;
        if (f > 10 && a0) for (int j = 0; j < 3; j++) p0[j] += 0.8f * d0[j];
        if (f > 10 && a1) for (int j = 0; j < 3; j++) p1[j] += 0.8f * d1[j];
        if (f > 20 && a2) for (int j = 0; j < 3; j++) p2[j] += 0.8f * d2[j];
        if (a0) set_bullet(0, p0, d0);
        if (a1) set_bullet(1, p1, d1);
        if (a2) set_bullet(2, p2, d2);
        if (a1) visual(1);
        if (a0) visual(0);
        if (a2) visual(2);
        bone_post();
        float M[16]; get(bone_mat(STAR), M);
        if (f >= 11 && f < 22) {                                    // the star rides bullet 0
            float pm[3], dm[3]; to_model(p0, 1.0f, pm); to_model(d0, 0.0f, dm);
            CHECK(dist3(M + 12, pm) < 1e-3f);
            for (int r = 0; r < 3; r++) CHECK(near(len3(M + 4 * r), 1.0f));
            CHECK(near(dot3(M, M + 4), 0.0f) && near(dot3(M, M + 8), 0.0f) && near(dot3(M + 4, M + 8), 0.0f));
            float A[3]; for (int j = 0; j < 3; j++) A[j] = A_l[0] * M[j] + A_l[1] * M[4 + j] + A_l[2] * M[8 + j];
            const float la = len3(A); for (float& x : A) x /= la;
            CHECK(std::fabs(dot3(A, dm)) < 0.02f);                  // the spin axis across the flight: edge-first
            float Rk[9]; for (int r = 0; r < 3; r++) for (int j = 0; j < 3; j++) Rk[3 * r + j] = M[4 * r + j];
            if (have_prev) {                                        // 40 deg about its axis per pass (straight flight)
                float tr = 0.0f; for (int r = 0; r < 3; r++) for (int j = 0; j < 3; j++) tr += prevR[3 * r + j] * Rk[3 * r + j];
                const float ang = std::acos(std::max(-1.0f, std::min(1.0f, (tr - 1.0f) / 2.0f))) * 57.29578f;
                CHECK(ang > 38.0f && ang < 42.0f);
                spins++;
            }
            std::memcpy(prevR, Rk, sizeof Rk); have_prev = true;
            float p1m[3]; to_model(p1, 1.0f, p1m);
            if (f > 12) CHECK(dist3(M + 12, p1m) > 1.0f);           // not on the 2220 bullet
        }
        if (f >= 26) CHECK(len3(M + 4) < 1e-6f);                   // let go (kApocLive passes after its last visual): hidden
        for (int k = 0; k < NARM; k++) {                            // the 2220 bullet next to the star: nobody
            float A[16]; get(bone_mat(ARM0 + k), A);
            float p1m[3]; to_model(p1, 1.0f, p1m);
            if (f >= 11 && f < 22) CHECK(dist3(A + 12, p1m) > 1.0f);
        }
        if (f >= 21 && f < 32) {                                    // arm 3 rides bullet 2, blade (+Y) along it, no spin
            float A[16]; get(bone_mat(ARM0 + 3), A);
            float pm[3], dm[3]; to_model(p2, 1.0f, pm); to_model(d2, 0.0f, dm);
            CHECK(dist3(A + 12, pm) < 1e-3f && dot3(A + 4, dm) > 0.999f);
        }
    }
    CHECK(spins >= 9 && g_fx_calls > 0 && g_bone_orig > 0);
    std::printf("ok test_pin_arms (%d spin steps, %d visual calls, %d bone passes)\n", spins, g_fx_calls, g_bone_orig);
    return 0;
}
