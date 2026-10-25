// Host test for roster.cpp's Apocalypsis arms (roster.txt apocarms=ARM0,NARM,ID1[,ID2..]): fake guest RAM, a fake
// FUN_08AD20BC (a bullet visual's per-frame matrix; the wrapper records listed bullets), fake bullets / emitters / magi
// records, a fake engine pose of the 12 arm bones (shown in the crown, then gone at their hand-off) and FUN_0880EA20
// (bone_post_hook -> apoc_bone_post). Checks: each bullet is carried by the arm that vanished nearest to it, whatever
// order the bullets come in; the arm sits on its bullet (model space, through the model -> world matrix) with its blade
// (+Y) along the bullet's direction, scale 1, turning smoothly from its last shown pose; a gone bullet lets its arm go
// (hidden); other records, other characters and the menu overlay are left alone; without the token nothing is hooked.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_apoc_arms test_apoc_arms.cpp; /tmp/test_apoc_arms
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>
#include <algorithm>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
static int g_fx_calls = 0;
void orig_fxdir(uint8_t*, recomp_context* ctx) { g_fx_calls++; ctx->r[4] = ctx->r[5] = 0; }   // clobbers a0/a1 like real code
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
float RF(uint32_t a) { float f; std::memcpy(&f, R(a), 4); return f; }
static int g_bone_orig = 0;
void orig_bone_post(uint8_t*, recomp_context* ctx) { g_bone_orig++; ctx->r[4] = ctx->r[5] = ctx->r[6] = 0; }
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x0880EA20u) return orig_bone_post;
    return orig_other;
}

const uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, INFO_N = 0x09230000u, INFO_B = 0x09230100u;
const uint32_t CTRL_N = 0x09260000u, GM_N = 0x09261000u, BONES_N = 0x09270000u;
const uint32_t CTRL_B = 0x09280000u, GM_B = 0x09281000u, BONES_B = 0x09290000u;
const uint32_t BUL = 0x09300000u, EMIT = 0x09310000u, REC = 0x09320000u;   // bullets 0x100 apart, emitters / records 0x40
const int NB = 201, ARM0 = 168, NARM = 12;
const uint32_t OVL_WORD = 0x089DD4C0u;

uint32_t bone_mat(uint32_t bones, int k) { return bones + 0x100u * k + 0x80u; }
void put(uint32_t a, const float* M) { std::memcpy(R(a), M, 64); }
void get(uint32_t a, float* M) { std::memcpy(M, R(a), 64); }
float len3(const float* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
float dist3(const float* a, const float* b) { const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return len3(d); }
float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool near(float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; }

float g_yaw = 0.0f, g_pos[3] = {2.0f, 0.0f, -1.0f};
void world(uint32_t actor) {     // model -> world: yaw about +Y, then the position (row vectors)
    const float c = std::cos(g_yaw), s = std::sin(g_yaw);
    const float W[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, g_pos[0], g_pos[1], g_pos[2], 1};
    std::memcpy(R(actor + 0x330u), W, 64);
}
void to_world(const float* p, float w, float* o) {
    float W[16]; get(NOC + 0x330u, W);
    for (int j = 0; j < 3; j++) o[j] = p[0] * W[j] + p[1] * W[4 + j] + p[2] * W[8 + j] + w * W[12 + j];
}
void to_model(const float* p, float w, float* o) {   // inverse of the rigid world matrix
    float W[16]; get(NOC + 0x330u, W);
    float q[3] = {p[0] - w * W[12], p[1] - w * W[13], p[2] - w * W[14]};
    for (int i = 0; i < 3; i++) o[i] = q[0] * W[4 * i] + q[1] * W[4 * i + 1] + q[2] * W[4 * i + 2];
}

// the crown: arm k's slot angle 30*s (s = k except bones 0/1 swapped, like the GMO), hand-off at f39 (f51 for slot 0)
int slot_of(int k) { return k == 0 ? 1 : (k == 1 ? 0 : k); }
void handoff(int k, float* p, float* d) {
    const int s = slot_of(k);
    const float a = 0.5235988f * s, e = (s == 0 ? 65.0f : 55.0f) * 0.0174533f;
    const int dd = std::min(s, 12 - s);
    d[0] = std::sin(a) * std::cos(e); d[1] = std::sin(e); d[2] = std::cos(a) * std::cos(e);
    const float h = 1.4f + 0.15f * dd;
    p[0] = std::sin(a) * 1.95f + 1.35f * d[0]; p[1] = h + 1.35f * d[1]; p[2] = std::cos(a) * 1.95f + 1.35f * d[2];
}
int vanish_frame(int k) { return slot_of(k) == 0 ? 51 : 39; }
// the engine's pose of the arms at frame f: shown (scale 1, blade +Y along the burst, a roll) until the hand-off,
// at 0.94 m short of the hand-off point on the frame before; then the rest pose (scale 0)
void engine_pose(uint32_t bones, int f) {
    for (int k = 0; k < NARM; k++) {
        float p[3], d[3]; handoff(k, p, d);
        float M[16] = {};
        if (f < vanish_frame(k)) {
            const float back = f == vanish_frame(k) - 1 ? 0.41f : 1.35f;   // 0.94 m out the frame before, else in the crown
            float x[3] = {d[2], 0.0f, -d[0]}; const float lx = len3(x); for (float& v : x) v /= lx;   // horizontal
            const float z[3] = {x[1] * d[2] - x[2] * d[1], x[2] * d[0] - x[0] * d[2], x[0] * d[1] - x[1] * d[0]};
            for (int j = 0; j < 3; j++) { M[j] = x[j]; M[4 + j] = d[j]; M[8 + j] = z[j]; M[12 + j] = p[j] - back * d[j]; }
            M[15] = 1.0f;
        } else {
            M[13] = 1.4f; M[15] = 1.0f;                                   // rest: scale 0 at (0, 1.4, 0)
        }
        put(bone_mat(bones, ARM0 + k), M);
    }
}
void bone_post(uint32_t actor) {
    recomp_context ctx{};
    ctx.r[6] = static_cast<int32_t>(actor);
    g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx);
}
// a bullet: world position / direction, record id
uint32_t bullet(int i) { return BUL + 0x100u * i; }
void make_bullet(int i, uint16_t rec_id) {
    const uint32_t b = bullet(i), em = EMIT + 0x40u * i, rec = REC + 0x40u * i;
    W32(b + 0x64u, em); W32(em + 0x24u, rec); W16(rec, rec_id);
}
void set_bullet(int i, const float* p, const float* d) {
    for (int j = 0; j < 3; j++) { WF(bullet(i) + 4 * j, p[j]); WF(bullet(i) + 0x20u + 4 * j, d[j]); }
    WF(bullet(i) + 12, 1.0f); WF(bullet(i) + 0x2Cu, 0.0f);
}
void visual(int i) {
    recomp_context ctx{};
    ctx.r[4] = 0x09400000; ctx.r[5] = static_cast<int32_t>(bullet(i));
    g_hooks.at(0x08AD20BCu)(g_ram.data(), &ctx);
}
struct Arm { float pos[3], x[3], y[3], scale; bool ortho; };
Arm arm(int k) {
    float M[16]; get(bone_mat(BONES_N, ARM0 + k), M);
    Arm a;
    for (int j = 0; j < 3; j++) { a.pos[j] = M[12 + j]; a.x[j] = M[j]; a.y[j] = M[4 + j]; }
    a.scale = len3(a.y);
    const float* r0 = M; const float* r1 = M + 4; const float* r2 = M + 8;
    a.ortho = near(len3(r0), 1.0f) && near(len3(r1), 1.0f) && near(len3(r2), 1.0f) && near(dot3(r0, r1), 0.0f) &&
              near(dot3(r0, r2), 0.0f) && near(dot3(r1, r2), 0.0f);
    return a;
}

int main() {
    // no token: nothing hooked for the bullets
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69,12.5,1.8,0.5;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08AD20BCu));
    // with the token (and the other Noctis tokens around it)
    g_hooks.clear(); g_entries.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis warp=69,12.5,1.8,0.5 apocarms=168,12,2220,2222,2223,2228 warpfx=83,5,1.0,1.124,84;33 viv100 21;"
                               "35 ysh100 28 apocarms=1,0,5", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08AD20BCu) && g_hooks.count(0x0880EA20u));
    const Entry* en = nullptr;
    for (const Entry& e : g_entries) if (e.charId == 34) en = &e;
    CHECK(en && en->apoc_arm0 == 168 && en->apoc_narm == 12 && en->apoc_nids == 4 && en->apoc_ids[3] == 2228 && en->warp_sword == 83);
    for (const Entry& e : g_entries) if (e.charId == 35) CHECK(e.apoc_narm == 0);          // bad token ignored
    W32(OVL_WORD, 1u);                                                                       // battle overlay resident
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x394u, CTRL_N); W32(CTRL_N + 0x0Cu, GM_N); W32(GM_N + 0x0Cu, BONES_N); W16(GM_N + 0x20u, NB);
    W32(BARTZ + 0x394u, CTRL_B); W32(CTRL_B + 0x0Cu, GM_B); W32(GM_B + 0x0Cu, BONES_B); W16(GM_B + 0x20u, NB);
    g_yaw = 0.7f; world(NOC); world(BARTZ);
    // bullets 0..10 = the brave arms (records 2220 / 2222), bullet 11 = the greatsword (2223); bullet 12 another record
    for (int i = 0; i < 12; i++) make_bullet(i, static_cast<uint16_t>(i == 11 ? 2223 : (i < 6 ? 2220 : 2222)));
    make_bullet(12, 2201);
    int arm_of_bullet[12];
    for (int i = 0; i < 11; i++) arm_of_bullet[i] = i + 1 < 2 ? 0 : i + 1;   // arms 0, 2..11 (slots 1..11)
    arm_of_bullet[0] = 0;
    { int k = 2; for (int i = 1; i < 11; i++) arm_of_bullet[i] = k++; }
    arm_of_bullet[11] = 1;                                                      // bone 1 = slot 0, the greatsword
    float bp[13][3], bd[13][3];
    bool alive[13] = {};
    float prevY[NARM][3] = {}, prevX[NARM][3] = {};
    const float target[3] = {g_pos[0] + 10.0f * std::sin(g_yaw), 1.1f, g_pos[2] + 10.0f * std::cos(g_yaw)};
    for (int f = 0; f < 90; f++) {
        engine_pose(BONES_N, f);
        engine_pose(BONES_B, f);
        // bullets: born at their arm's hand-off point on its vanish frame, then fly (turning toward the target)
        for (int i = 0; i < 13; i++) {
            const int k = i < 12 ? arm_of_bullet[i] : 3;
            const int born = i < 12 ? vanish_frame(k) : 40;
            const int dies = i < 12 ? born + 14 + i : 60;
            if (f == born) {
                float p[3], d[3]; handoff(k, p, d);
                to_world(p, 1.0f, bp[i]); to_world(d, 0.0f, bd[i]); alive[i] = true;
            } else if (alive[i]) {
                float want[3] = {target[0] - bp[i][0], target[1] - bp[i][1], target[2] - bp[i][2]};
                const float L = len3(want); for (float& x : want) x /= L;
                for (int j = 0; j < 3; j++) bd[i][j] = 0.8f * bd[i][j] + 0.2f * want[j];
                const float l = len3(bd[i]); for (float& x : bd[i]) x /= l;
                for (int j = 0; j < 3; j++) bp[i][j] += 0.9f * bd[i][j];
            }
            if (f >= dies) alive[i] = false;
            if (alive[i]) set_bullet(i, bp[i], bd[i]);
        }
        // the visual pass: listed and unlisted bullets, in a shuffled order; on odd frames after the bone pass
        int order[13]; for (int i = 0; i < 13; i++) order[i] = (i * 7 + f * 3) % 13;
        const bool late = f % 2 == 1;
        if (!late) for (int i : order) if (alive[i]) visual(i);
        bone_post(NOC);
        bone_post(BARTZ);
        if (late) for (int i : order) if (alive[i]) visual(i);
        // checks
        for (int i = 0; i < 12; i++) {
            const int k = arm_of_bullet[i];
            const int born = vanish_frame(k), dies = born + 14 + i;
            const Arm a = arm(k);
            if (f < born) { CHECK(a.scale > 0.5f); continue; }                     // the motion's crown pose
            if (f >= born + 1 && f < dies) {                                       // riding its bullet (a frame late at worst)
                CHECK(near(a.scale, 1.0f) && a.ortho);
                float pm[3], dm[3]; to_model(bp[i], 1.0f, pm); to_model(bd[i], 0.0f, dm);
                CHECK(dist3(a.pos, pm) < 1.0f);                                    // at most one step of 0.9 m behind
                CHECK(dot3(a.y, dm) > 0.9f);                                       // blade along the flight
                if (f == born + 1) {                                               // keeps the roll it was shown with
                    float p[3], d[3]; handoff(k, p, d);
                    float ex[3] = {d[2], 0.0f, -d[0]}; const float l = len3(ex); for (float& v : ex) v /= l;
                    CHECK(dot3(a.x, ex) > 0.9f);
                }
                if (f > born + 1) {                                                // turns smoothly, no roll spin: the
                    CHECK(dot3(a.y, prevY[k]) > 0.8f);                             // test's bullets turn <= ~26 deg a
                    CHECK(dot3(a.x, prevX[k]) >= dot3(a.y, prevY[k]) - 0.02f);     // frame pair; X turns no more than Y
                }
                std::memcpy(prevY[k], a.y, sizeof a.y); std::memcpy(prevX[k], a.x, sizeof a.x);
            }
            if (f >= dies + 4) CHECK(a.scale < 1e-6f);                             // gone: let go, hidden
        }
        if (f == 45) {                                                             // exact while the visual ran first
            for (int i = 0; i < 11; i++) {
                const int k = arm_of_bullet[i];
                const Arm a = arm(k);
                float pm[3], dm[3]; to_model(bp[i], 1.0f, pm); to_model(bd[i], 0.0f, dm);
                CHECK(dist3(a.pos, pm) < 0.91f && dot3(a.y, dm) > 0.95f);
            }
        }
        for (int k = 0; k < NARM; k++) {                                           // Bartz: exactly the engine's pose
            float M[16], E[16]; get(bone_mat(BONES_B, ARM0 + k), M);
            engine_pose(BONES_B + 0x100000u, f); get(bone_mat(BONES_B + 0x100000u, ARM0 + k), E);
            CHECK(std::memcmp(M, E, 64) == 0);
        }
    }
    // a frame the engine does not re-pose after the bullets are gone: the arms stay hidden, nothing compounds
    {
        float M1[16], M2[16];
        get(bone_mat(BONES_N, ARM0 + 4), M1);
        bone_post(NOC); bone_post(NOC);
        get(bone_mat(BONES_N, ARM0 + 4), M2);
        CHECK(std::memcmp(M1, M2, 64) == 0 && len3(M2 + 4) < 1e-6f);
    }
    // another overlay in the window: the battle function is not run, nothing recorded
    {
        W32(OVL_WORD, 2u);
        const int calls = g_fx_calls;
        make_bullet(20, 2220);
        const float p[3] = {0, 0, 0}, d[3] = {0, 0, 1};
        set_bullet(20, p, d);
        visual(20);
        CHECK(g_fx_calls == calls);
        for (const ApocBullet& b : g_apoc_bul) CHECK(b.ptr != bullet(20));
        W32(OVL_WORD, 1u);
    }
    CHECK(g_fx_calls > 0 && g_bone_orig > 0);
    std::printf("ok test_apoc_arms (%d visual calls, %d bone passes)\n", g_fx_calls, g_bone_orig);
    return 0;
}
