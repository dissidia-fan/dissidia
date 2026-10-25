// Host test for roster.cpp's weapon trails on floating arms (roster.txt `armtrail=`, 2026-10-22). Fake guest RAM, a fake
// FUN_0880EA20 (bone_post_hook -> armtrail_bone_post) and FUN_0884E660 (the trail consumer; armtrail_consume_hook fills
// the slots right before it). Checks: the token parses (sets, bits, draw bones; bad ones ignored); the hooks install;
// shown arms get slots SLOT0.. in priority order with id 7700 and their own bit; the timeline's slots 0..3 and their
// mask bits are left as they are; slots are sticky (an arm keeps its slot, a freed slot is reused); hidden arms (scale 0,
// or their draw bone hidden in the draw-bone table) get none; an earlier set's arm takes the slot of a later set's arm
// when all are full, never the other way; the EX ring only trails while its draw bone is shown; another character's
// actor and an actor with no bone pass get nothing; PSPRECOMP_ARMTRAIL_LOG switches the log on.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_arm_trail test_arm_trail.cpp; /tmp/test_arm_trail
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
uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, R(a), 4); return v; }
void W16(uint32_t a, uint16_t v) { std::memcpy(R(a), &v, 2); }
static int g_bone_orig = 0, g_consume_orig = 0;
static uint32_t g_seen_mask = 0;
void orig_bone_post(uint8_t*, recomp_context* ctx) { g_bone_orig++; }
void orig_other(uint8_t*, recomp_context*) {}
const uint32_t NOC = 0x09210000u, BAR = 0x09410000u;
void orig_consume(uint8_t*, recomp_context*) { g_consume_orig++; g_seen_mask = R32(NOC + 0x6D8u); }
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x0880EA20u) return orig_bone_post;
    if (a == 0x0884E660u) return orig_consume;
    return orig_other;
}

const uint32_t INFO_N = 0x09230000u, CHRP_N = 0x09240000u, CTRL_N = 0x09260000u, GM_N = 0x09261000u, BONES_N = 0x09270000u,
               VIS_N = 0x09280000u;
const uint32_t INFO_B = 0x09430000u, CHRP_B = 0x09440000u, CTRL_B = 0x09460000u, GM_B = 0x09461000u, BONES_B = 0x09470000u;
const int NB = 206, APOC0 = 168, STAR = 81, RING0 = 181;
const int D_APOC = 180, D_STAR = 160, D_RING = 73;
uint32_t bone_mat(uint32_t bones, int k) { return bones + 0x100u * k + 0x80u; }
void arm(uint32_t bones, int k, float s) {
    float M[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0.1f * k, 1.0f, 0.5f, 1};
    std::memcpy(R(bone_mat(bones, k)), M, 64);
}
void vis(int bone, int state) {          // draw-bone table entry {bone, state, alpha}
    for (unsigned i = 0; i < 56; i++) {
        uint8_t* p = R(VIS_N + 3 * i);
        if (p[0] == bone || p[0] == 0xFF) { p[0] = static_cast<uint8_t>(bone); p[1] = static_cast<uint8_t>(state); p[2] = 0xFF; return; }
    }
}
void actor(uint32_t a, uint32_t info, uint32_t chrp, uint32_t ctrl, uint32_t gm, uint32_t bones, int cid) {
    W32(a + 0x2F8u, info); R(info)[6] = static_cast<uint8_t>(cid);
    W32(a + 0x5A4u, chrp); R(chrp)[0] = static_cast<uint8_t>(cid);
    R(a + 0x542u)[0] = 0;
    W32(a + 0x394u, ctrl); W32(ctrl + 0x0Cu, gm); W32(gm + 0x0Cu, bones); W16(gm + 0x20u, NB);
    const float W[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(R(a + 0x330u), W, 64);
}
void bone_post(uint32_t a) {
    recomp_context ctx{}; ctx.r[6] = static_cast<int32_t>(a);
    g_hooks[0x0880EA20u](g_ram.data(), &ctx);
}
// a frame: the timeline walker clears the mask and sets slot 0 (a Blitz trail), the bone pass, then the consumer
void frame(bool bone_pass = true, bool other = false) {
    W32(NOC + 0x6D8u, 1u);
    const uint32_t s0[4] = {9999, 5, 0x3F800000u, 0};
    std::memcpy(R(NOC + 0x5D8u), s0, 16);
    if (bone_pass) bone_post(NOC);
    if (other) bone_post(BAR);
    recomp_context ctx{};
    g_hooks[0x0884E660u](g_ram.data(), &ctx);
}
struct Slot { uint32_t id, bit, scale, other; };
Slot slot(uint32_t a, int q) { Slot s; std::memcpy(&s, R(a + 0x5D8u + 16u * q), 16); return s; }
int slot_of_bit(uint32_t a, int bit) {
    const uint32_t m = R32(a + 0x6D8u);
    for (int q = 0; q < 16; q++) if ((m >> q & 1) && slot(a, q).id == 7700 && static_cast<int>(slot(a, q).bit) == bit) return q;
    return -1;
}
int count_trails(uint32_t a) {
    const uint32_t m = R32(a + 0x6D8u); int n = 0;
    for (int q = 4; q < 16; q++) n += (m >> q) & 1;
    return n;
}
void check_slot0() {
    CHECK(g_seen_mask & 1u);
    const Slot s = slot(NOC, 0);
    CHECK(s.id == 9999 && s.bit == 5 && s.scale == 0x3F800000u);
    for (int q = 1; q < 4; q++) CHECK(!(g_seen_mask >> q & 1));
}

int main() {
    setenv("PSPRECOMP_ARMTRAIL_LOG", "1", 1);
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis armtrail=7700,4,12,168:12:0:180,81:1:23:160,181:11:12:73;"
                               "33 viv100 21 armtrail=7700,3,12,168:12:0;35 ysh100 28 armtrail=7700,4,13,168:12:0;"
                               "36 aaa100 9 armtrail=7700,4,4,168:12;37 bbb100 9 armtrail=7700,4,4,168:12:0:180:5", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x0884E660u) && g_hooks.count(0x0880EA20u));
    CHECK(g_trail_log);
    const Entry* en = nullptr;
    for (const Entry& e : g_entries) {
        if (e.charId == 34) en = &e;
        if (e.charId == 33 || e.charId == 35 || e.charId == 36 || e.charId == 37) CHECK(e.at_nset == 0);   // slot 3 / 4+13 > 16 / no bit / 5 fields
    }
    CHECK(en && en->at_id == 7700 && en->at_slot0 == 4 && en->at_nslot == 12 && en->at_nset == 3);
    CHECK(en->at_bone0[0] == 168 && en->at_count[0] == 12 && en->at_bit0[0] == 0 && en->at_draw[0] == 180);
    CHECK(en->at_bone0[1] == 81 && en->at_count[1] == 1 && en->at_bit0[1] == 23 && en->at_draw[1] == 160);
    CHECK(en->at_bone0[2] == 181 && en->at_count[2] == 11 && en->at_bit0[2] == 12 && en->at_draw[2] == 73);

    actor(NOC, INFO_N, CHRP_N, CTRL_N, GM_N, BONES_N, 34);
    actor(BAR, INFO_B, CHRP_B, CTRL_B, GM_B, BONES_B, 9);          // a retail Bartz: never touched
    W32(CTRL_N + 0x10Cu, VIS_N); R(CTRL_N + 0x8Eu)[0] = 56;
    std::memset(R(VIS_N), 0xFF, 3 * 56);
    vis(D_RING, 0);                                                 // EX ring hidden (not in EX Mode)
    for (int k = 0; k < NB; k++) { arm(BONES_N, k, 0.0f); arm(BONES_B, k, 1.0f); }
    for (int k = 0; k < 11; k++) arm(BONES_N, RING0 + k, 1.0f);    // the ring bones are posed, but its draw bone is hidden
    W32(BAR + 0x6D8u, 0);

    // 1. nothing shown: no trails, the timeline's slot 0 untouched
    frame(true, true);
    check_slot0();
    CHECK(count_trails(NOC) == 0 && g_consume_orig == 1 && R32(BAR + 0x6D8u) == 0);

    // 2. the 12 Apocalypsis arms shown: slots 4..15 in order, bits 0..11, id 7700, scale 1
    for (int k = 0; k < 12; k++) arm(BONES_N, APOC0 + k, 1.0f);
    frame(true, true);
    check_slot0();
    for (int k = 0; k < 12; k++) {
        CHECK(slot_of_bit(NOC, k) == 4 + k);
        const Slot s = slot(NOC, 4 + k);
        CHECK(s.scale == 0x3F800000u && s.other == 0);
    }
    CHECK(R32(NOC + 0x6D8u) == 0xFFF1u && g_seen_mask == 0xFFF1u && R32(BAR + 0x6D8u) == 0);

    // 3. sticky: arm 3 hides -> its slot (7) frees, the others keep theirs; the star shows -> it takes slot 7
    arm(BONES_N, APOC0 + 3, 0.0f);
    frame();
    CHECK(slot_of_bit(NOC, 3) == -1 && count_trails(NOC) == 11);
    for (int k = 0; k < 12; k++) if (k != 3) CHECK(slot_of_bit(NOC, k) == 4 + k);
    arm(BONES_N, STAR, 1.0f);
    frame();
    CHECK(slot_of_bit(NOC, 23) == 7 && count_trails(NOC) == 12);
    // the star's draw bone hidden: it loses its trail
    vis(D_STAR, 0);
    frame();
    CHECK(slot_of_bit(NOC, 23) == -1 && count_trails(NOC) == 11);
    vis(D_STAR, 1);
    frame();
    CHECK(slot_of_bit(NOC, 23) == 7);
    // 4. priority: arm 3 comes back with every slot full: it takes the star's slot, never another arm's
    arm(BONES_N, APOC0 + 3, 1.0f);
    frame();
    CHECK(slot_of_bit(NOC, 3) == 7 && slot_of_bit(NOC, 23) == -1 && count_trails(NOC) == 12);
    for (int k = 0; k < 12; k++) CHECK(slot_of_bit(NOC, k) == 4 + k);
    check_slot0();

    // 5. EX Mode: the ring's draw bone shown while the apoc arms fill the slots: the ring gets none
    vis(D_RING, 1);
    frame();
    CHECK(count_trails(NOC) == 12);
    for (int k = 0; k < 11; k++) CHECK(slot_of_bit(NOC, 12 + k) == -1);
    // the move ends (apoc arms gone, star gone): the ring's 11 arms take slots, bits 12..22
    for (int k = 0; k < 12; k++) arm(BONES_N, APOC0 + k, 0.0f);
    arm(BONES_N, STAR, 0.0f);
    frame();
    CHECK(count_trails(NOC) == 11);
    for (int k = 0; k < 11; k++) CHECK(slot_of_bit(NOC, 12 + k) >= 4);
    const int ring0_slot = slot_of_bit(NOC, 12);
    // two Apocalypsis arms appear: they take the free slot first, then a ring arm's (the last ring arm), the rest stay
    arm(BONES_N, APOC0 + 0, 1.0f); arm(BONES_N, APOC0 + 1, 1.0f);
    frame();
    CHECK(count_trails(NOC) == 12 && slot_of_bit(NOC, 0) >= 4 && slot_of_bit(NOC, 1) >= 4);
    CHECK(slot_of_bit(NOC, 12) == ring0_slot && slot_of_bit(NOC, 22) == -1);
    // EX Mode ends: the ring hides, its trails stop
    vis(D_RING, 0);
    frame();
    CHECK(count_trails(NOC) == 2);
    check_slot0();

    // 6. no bone pass for the actor any more (it left the battle): its slots are no longer written
    frame(false);
    frame(false);
    frame(false);
    CHECK(count_trails(NOC) == 0 && R32(NOC + 0x6D8u) == 1u);
    CHECK(R32(BAR + 0x6D8u) == 0);
    std::printf("arm trail tests passed (%d consumer calls)\n", g_consume_orig);
    return 0;
}
