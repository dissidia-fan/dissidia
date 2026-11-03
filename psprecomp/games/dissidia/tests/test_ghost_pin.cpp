// Host test for roster.cpp's ghostpin= (Noctis's dodges leave the warp ghost where they started, 2026-10-29). Fake
// guest RAM and a fake FUN_0880EA20 (bone_post_hook -> ghostpin_bone_post), as test_pin_arms.cpp.
// Checks: the token parses (bad ones ignored); while a listed motion plays every ghost bone stays where it was in the
// world on the motion's first frame although he moves and turns; a frame the engine did not re-pose is not moved twice;
// bones outside FIRST..LAST are untouched; another motion gets the engine's pose back (restored if not re-posed); a new
// listed motion pins from its own first frame; the same motion played again (its time drops) pins from its new start.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_ghost_pin test_ghost_pin.cpp; /tmp/test_ghost_pin
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
int dissidia_frame_ticks() { return 2; }              // the 60 fps clock (dissidia_main.cpp): not used by ghostpin
float dissidia_own_clock_scale() { return 1.0f; }
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
void orig_bone_post(uint8_t*, recomp_context* ctx) { ctx->r[4] = ctx->r[5] = ctx->r[6] = 0; }
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t a) { return a == 0x0880EA20u ? orig_bone_post : orig_other; }

const uint32_t NOC = 0x09210000u, INFO_N = 0x09230000u, CTRL_N = 0x09260000u, GM_N = 0x09261000u, BONES_N = 0x09270000u, ROW = 0x09280000u;
const int NB = 120, G0 = 85, G1 = 87, OTHER = 3;
const uint32_t OVL_WORD = 0x089DD4C0u;
uint32_t bone_mat(int k) { return BONES_N + 0x100u * k + 0x80u; }
void put(uint32_t a, const float* M) { std::memcpy(R(a), M, 64); }
void get(uint32_t a, float* M) { std::memcpy(M, R(a), 64); }
bool near(float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; }
void set_world(float yaw, float x, float y, float z) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float W[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
    std::memcpy(R(NOC + 0x330u), W, 64);
}
void play(int script, int motion) { W32(NOC + 0x17Cu, ROW); W16(ROW, static_cast<uint16_t>(script)); W16(ROW + 2u, static_cast<uint16_t>(motion)); W16(NOC + 0x180u, static_cast<uint16_t>(script)); }
void pose_of(int k, float f, float* M) {             // the engine's pose: a frame a little different per bone and frame
    const float a = 0.3f * k + 0.05f * f, c = std::cos(a), s = std::sin(a);
    const float P[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0.1f * k, 1.0f + 0.01f * f, 0.2f, 1};
    std::memcpy(M, P, 64);
}
void engine_pose(float f) { for (int k : {OTHER, G0, G0 + 1, G1}) { float M[16]; pose_of(k, f, M); put(bone_mat(k), M); } }
void to_world(const float* M, float* o) {             // a bone's world origin
    float W[16]; get(NOC + 0x330u, W);
    for (int j = 0; j < 3; j++) o[j] = M[12] * W[j] + M[13] * W[4 + j] + M[14] * W[8 + j] + W[12 + j];
}
void bone_post() { recomp_context ctx{}; ctx.r[6] = static_cast<int32_t>(NOC); g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx); }

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis ghostpin=16/17,85,87;33 viv100 21 ghostpin=16,90,80;35 ysh100 28 ghostpin=1x,1,2", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x0880EA20u));
    for (const Entry& e : g_entries) {
        if (e.charId == 34) CHECK(e.gp_first == G0 && e.gp_last == G1 && e.gp_motions.size() == 2 && e.gp_motions[1] == 17);
        else CHECK(e.gp_first < 0 && e.gp_motions.empty());          // bad tokens ignored
    }
    W32(OVL_WORD, 1u);
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(NOC + 0x394u, CTRL_N); W32(CTRL_N + 0x0Cu, GM_N); W32(GM_N + 0x0Cu, BONES_N); W16(GM_N + 0x20u, NB);
    // the dodge: motion 16 from (2, 0, 5), yaw 0.3; he moves 4 m and turns while it plays
    play(400, 16);
    float pin[3][3];
    for (int f = 0; f < 12; f++) {
        set_world(0.3f + 0.05f * f, 2.0f + 0.35f * f, 0.02f * f, 5.0f - 0.1f * f);
        const bool repose = f != 6;                       // frame 6: the engine did not re-pose
        if (repose) engine_pose(0.0f);                     // the motion poses the ghost in the same first-frame pose
        bone_post();
        for (int k = 0; k < 3; k++) {
            float M[16], w[3]; get(bone_mat(G0 + k), M); to_world(M, w);
            if (f == 0) std::memcpy(pin[k], w, sizeof w);
            CHECK(near(w[0], pin[k][0]) && near(w[1], pin[k][1]) && near(w[2], pin[k][2]));   // still where it started
        }
        float O[16], P[16]; get(bone_mat(OTHER), O); pose_of(OTHER, 0.0f, P);
        CHECK(std::memcmp(O, P, 64) == 0);                // a bone outside the range is untouched
    }
    // the dodge ends: another motion; the engine's pose stands (and, not re-posed, is given back)
    play(1, 0);
    set_world(0.0f, 9.0f, 0.0f, 0.0f);
    bone_post();                                          // not re-posed this frame
    for (int k = 0; k < 3; k++) { float M[16], P[16]; get(bone_mat(G0 + k), M); pose_of(G0 + k, 0.0f, P); CHECK(std::memcmp(M, P, 64) == 0); }
    engine_pose(3.0f); bone_post();
    for (int k = 0; k < 3; k++) { float M[16], P[16]; get(bone_mat(G0 + k), M); pose_of(G0 + k, 3.0f, P); CHECK(std::memcmp(M, P, 64) == 0); }
    // a new dodge (motion 17) pins from its own first frame
    play(410, 17);
    float start[3];
    for (int f = 0; f < 5; f++) {
        set_world(1.0f, 9.0f + 0.5f * f, 0.0f, 1.0f);
        engine_pose(0.0f); bone_post();
        float M[16], w[3]; get(bone_mat(G1), M); to_world(M, w);
        if (f == 0) std::memcpy(start, w, sizeof w);
        CHECK(near(w[0], start[0]) && near(w[2], start[2]));
    }
    // two air dodges in a row: motion 17 again, its time back at 0 (no other motion between): the ghost moves to
    // where the second one starts and stays there
    auto mtime = [](float t) { std::memcpy(R(NOC + 0x1A8u), &t, 4); };
    for (int f = 0; f < 5; f++) { mtime(2.0f * (f + 5)); set_world(1.0f, 11.5f + 0.5f * f, 0.0f, 1.0f); engine_pose(0.0f); bone_post(); }
    float again[3];
    for (int f = 0; f < 6; f++) {
        mtime(2.0f * f);
        set_world(1.4f, 20.0f + 0.6f * f, 1.0f + 0.2f * f, -3.0f);
        engine_pose(0.0f); bone_post();
        float M[16], w[3]; get(bone_mat(G1), M); to_world(M, w);
        if (f == 0) { std::memcpy(again, w, sizeof w); CHECK(!near(w[0], start[0], 0.5f)); }   // not left at the first dodge
        CHECK(near(w[0], again[0]) && near(w[1], again[1]) && near(w[2], again[2]));
    }
    std::printf("test_ghost_pin: OK\n");
    return 0;
}
