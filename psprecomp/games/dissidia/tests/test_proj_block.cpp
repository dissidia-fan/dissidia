// Host test for roster.cpp's projblock= (projectiles blocked between two ticks of a motion; Apocalypsis's arms):
// fake guest RAM, fake originals. Includes ../runtime/roster.cpp like test_exmode.cpp.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_proj_block test_proj_block.cpp && /tmp/test_proj_block
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
static int g_can_hit_calls = 0, g_fx_calls = 0;
static int32_t g_fx_args[5];
void orig_can_hit(uint8_t*, recomp_context* ctx) { g_can_hit_calls++; ctx->r[2] = 1; }
void orig_other(uint8_t*, recomp_context*) {}
void fake_play_fx(uint8_t*, recomp_context* ctx) { g_fx_calls++; for (int i = 0; i < 4; i++) g_fx_args[i] = ctx->r[4 + i]; g_fx_args[4] = ctx->r[8];
                                                   ctx->r[2] = 0x1234; ctx->r[4] = ctx->r[5] = ctx->r[6] = ctx->r[16] = -1; }
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x08807D4Cu) return orig_can_hit;
    if (a == 0x088A9DE8u) return fake_play_fx;
    return orig_other;
}
uint8_t* R(uint32_t a) { return g_ram.data() + (a & 0x07FFFFFFu); }
void W32(uint32_t a, uint32_t v) { std::memcpy(R(a), &v, 4); }
void W16(uint32_t a, uint16_t v) { std::memcpy(R(a), &v, 2); }
void WF(uint32_t a, float v) { std::memcpy(R(a), &v, 4); }

constexpr uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, SUMMON = 0x09230000u;
constexpr uint32_t INFO_N = 0x09240000u, INFO_B = 0x09240100u, CHRP_N = 0x09240200u, CHRP_B = 0x09240300u;
constexpr uint32_t HIT = 0x09250000u, ROW = 0x09250800u, ROWB = 0x09250900u;
constexpr uint32_t CTRL = 0x09260000u, GM = 0x09261000u, BONES = 0x09270000u, FXP = 0x09263000u, MGR = 0x09264000u;
constexpr int NBONES = 192;

int can_hit(uint32_t hit, uint32_t def) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(hit); ctx.r[5] = static_cast<int32_t>(def); ctx.r[2] = 7;
    g_hooks.at(0x08807D4Cu)(g_ram.data(), &ctx);
    CHECK(ctx.r[4] == static_cast<int32_t>(hit) && ctx.r[5] == static_cast<int32_t>(def));   // registers given back
    return ctx.r[2];
}
uint32_t bone_mat(int i) { return BONES + 0x100u * i + 0x80u; }
void set_bone(int i, float tx, float ty, float tz, float s = 1.0f) {
    float m[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, tx, ty, tz, 1};
    std::memcpy(R(bone_mat(i)), m, sizeof m);
}
void play(uint32_t actor, uint32_t row, int script, int motion, float tick) {   // the p_se script row the engine sets
    W32(actor + 0x17Cu, row); W16(row, static_cast<uint16_t>(script)); W16(row + 2u, static_cast<uint16_t>(motion));
    W16(actor + 0x180u, static_cast<uint16_t>(script)); WF(actor + 0x1A8u, tick);
}

int main() {
    // Noctis: blocked in motions 70 / 68 between ticks 8 and 78, sparkles on six arms; a bad token is ignored; Vivi none
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis exranged=2 projblock=70/68,8,78,169:541/170:542/172:543/174:544/176:545/178:546"
                               " projblock=70,8 projblock=x,1,2 projblock=70,9,3 projblock=70,1,2,169:x;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08807D4Cu));
    CHECK(g_entries.size() == 2 && g_entries[0].projblocks.size() == 1 && g_entries[1].projblocks.empty());
    const ProjBlock& pb = g_entries[0].projblocks[0];
    CHECK(pb.motions.size() == 2 && pb.motions[0] == 70 && pb.motions[1] == 68 && pb.from == 8.0f && pb.to == 78.0f && pb.nfx == 6);
    CHECK(pb.fx_bone[0] == 169 && pb.fx_id[0] == 541 && pb.fx_bone[5] == 178 && pb.fx_id[5] == 546);

    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34; W32(NOC + 0x5A4u, CHRP_N); R(CHRP_N)[0] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9; W32(BARTZ + 0x5A4u, CHRP_B); R(CHRP_B)[0] = 9;
    R(SUMMON + 0x542u)[0] = 3;
    WF(NOC + 0x80u, 5.0f); WF(NOC + 0x88u, 7.0f);                // positions
    WF(BARTZ + 0x80u, 15.0f); WF(BARTZ + 0x88u, 7.0f);           // Bartz 10 m to Noctis's world +X
    float Wm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 0, 7, 1};   // model -> world: no turn
    std::memcpy(R(NOC + 0x330u), Wm, sizeof Wm);
    W32(NOC + 0x394u, CTRL); W32(CTRL + 0x0Cu, GM); W32(GM + 0x0Cu, BONES); W16(GM + 0x20u, NBONES);
    W32(NOC + 0x390u, FXP); W32(FXP + 0x5Cu, MGR);
    // the six sparkle arms round him: 169 at +X (front-right), 170 at -X, the others at +Z / -Z
    set_bone(169, 1.2f, 1.5f, 0.0f); set_bone(170, -1.2f, 1.5f, 0.0f); set_bone(172, 0.0f, 1.5f, 1.2f);
    set_bone(174, 0.0f, 1.5f, -1.2f); set_bone(176, 0.6f, 1.6f, 0.9f); set_bone(178, -0.6f, 1.6f, -0.9f);

    W16(HIT + 0x2Cu, 2); W32(HIT + 0x18u, BARTZ); W32(HIT + 0x20u, BARTZ);   // a projectile from Bartz
    // not in a listed motion: the original decides
    play(NOC, ROW, 300, 69, 40.0f);
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 1 && g_fx_calls == 0);
    // Apocalypsis (ground) before the arms appear / inside the window / after the launch
    play(NOC, ROW, 301, 70, 5.0f);
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 2);
    play(NOC, ROW, 301, 70, 40.0f);
    CHECK(can_hit(HIT, NOC) == 0 && g_can_hit_calls == 2 && g_fx_calls == 1);
    CHECK(g_fx_args[0] == static_cast<int32_t>(MGR) && g_fx_args[1] == 541 && g_fx_args[2] == static_cast<int32_t>(NOC + 0x4C8u) &&
          g_fx_args[3] == 0 && g_fx_args[4] == 0);               // the arm at +X faces Bartz
    WF(NOC + 0x1A8u, 45.0f);                                      // within the cooldown: blocked, no new sparkle
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == 1);
    WF(NOC + 0x1A8u, 50.0f);
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == 2);
    WF(NOC + 0x1A8u, 78.0f);
    CHECK(can_hit(HIT, NOC) == 0);
    WF(NOC + 0x1A8u, 78.5f);
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 3);
    // melee is not blocked; a summoned / thrown object's melee hit is
    WF(NOC + 0x1A8u, 60.0f);
    W16(HIT + 0x2Cu, 1);
    CHECK(can_hit(HIT, NOC) == 1 && g_can_hit_calls == 4);
    W32(HIT + 0x18u, SUMMON);
    CHECK(can_hit(HIT, NOC) == 0 && g_can_hit_calls == 4);
    W32(HIT + 0x18u, BARTZ); W16(HIT + 0x2Cu, 2);
    // the air version, restarted (motion time dropped): blocked with a fresh sparkle; attacker now to his -X
    WF(BARTZ + 0x80u, -5.0f);
    play(NOC, ROW, 302, 68, 20.0f);
    const int before = g_fx_calls;
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == before + 1 && g_fx_args[1] == 542);
    // a hidden arm (scale 0) is passed over for a shown one
    set_bone(170, -1.2f, 1.5f, 0.0f, 0.0f);
    WF(NOC + 0x1A8u, 35.0f);
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_args[1] != 542);
    set_bone(170, -1.2f, 1.5f, 0.0f);
    // the same motion on Bartz (no token): normal
    play(BARTZ, ROWB, 301, 70, 40.0f);
    W32(HIT + 0x18u, NOC); W32(HIT + 0x20u, NOC);
    CHECK(can_hit(HIT, BARTZ) == 1);
    W32(HIT + 0x18u, BARTZ); W32(HIT + 0x20u, BARTZ);
    // a script row that doesn't match the script id: no motion, normal
    W16(ROW, 999);
    CHECK(can_hit(HIT, NOC) == 1);
    W16(ROW, 302);
    // an object of Noctis's (kind 3) is not him
    R(NOC + 0x542u)[0] = 3;
    CHECK(can_hit(HIT, NOC) == 1);
    R(NOC + 0x542u)[0] = 0;
    CHECK(can_hit(HIT, NOC) == 0);
    // EX Mode keeps its own path (ex_deflect, no armiger= here: no sparkle from it) and still blocks
    R(NOC + 0x5B2u)[0] = 4;
    const int fx0 = g_fx_calls;
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == fx0);
    R(NOC + 0x5B2u)[0] = 0;
    // bad pointers fall through to the original
    CHECK(can_hit(0x00001000u, NOC) == 1);
    CHECK(can_hit(HIT, 0x00002000u) == 1);
    // no FEPA player / no fx manager: still blocked, just no sparkle
    W32(FXP + 0x5Cu, 0);
    WF(NOC + 0x1A8u, 70.0f);
    const int fx1 = g_fx_calls;
    CHECK(can_hit(HIT, NOC) == 0 && g_fx_calls == fx1);

    // a roster line without projblock= installs nothing for it
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08807D4Cu) && g_entries.size() == 1 && g_entries[0].projblocks.empty());
    std::printf("ok test_proj_block (%d original calls, %d sparkles)\n", g_can_hit_calls, g_fx_calls);
    return 0;
}
