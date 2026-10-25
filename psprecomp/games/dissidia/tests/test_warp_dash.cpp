// Host test for roster.cpp's warp dash (roster.txt warpdash=GROUND,AIR,RANGE,NEAR[,LINK[,HP]]): fake guest RAM and fakes of
// the EBOOT state switch FUN_08834C10 (where the warp dash is decided), the typed dash FUN_08A53290 (R+Triangle: script
// 240, the air-dash count +0x1075, state 0x13 with the type) and the state-0x19 dash FUN_08A50C08 (+0x1076, state 0x19), the lock-on target FUN_08B3B280 and its kind-6 filter FUN_08B3B1D4, the enemy
// FUN_08AB32B8, the start action FUN_08B382B8 (asks the selector through the dispatcher, like the real one), the slot
// ability FUN_08B33B08, and the EBOOT airborne test FUN_088374B0, selector context FUN_08849B50, command selector
// FUN_0884A294 and hit landing FUN_0880AD28; an 'ablt' table, a comb and plya tables; object kinds at +0x542.
// Built like the other roster tests:
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_warp_dash test_warp_dash.cpp; /tmp/test_warp_dash
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
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
uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, R(a), 2); return v; }
uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, R(a), 4); return v; }

const uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, ENEMY = 0x09228000u, CORE = 0x0922C000u, ASSIST = 0x09238000u,
               SUMMON = 0x0923C000u, EHIT = 0x09240000u, CHIT = 0x09240400u, HITOBJ = 0x09240800u, BULLET = 0x09241000u,
               BHIT = 0x09241800u;
const uint32_t INFO_N = 0x09230000u, INFO_B = 0x09230100u, MS_N = 0x09260000u, TAB_N = 0x09261000u, COMB = 0x09264000u;
const uint32_t HOLDER = 0x09280000u, OTHER_TAB = 0x09290000u, SEL_REC = 0x09291008u, LOCKBLK = 0x092A0000u;
const uint32_t T_ABLT_BLK = 0x092B0000u, T_ABLT_RECS = 0x092B1000u, SP0 = 0x09FF0000u;
uint32_t rec_addr(uint32_t tab, int i) { return tab + 8u + 0x74u * static_cast<uint32_t>(i); }
const int IDS[] = {1934, 1935, 1936, 1939, 3012, 3013, 1946, 1947, 3011, 3020, 3027, 3028, 3014, 3015};
uint32_t rec_of(int id) { for (int i = 0; i < 14; i++) if (IDS[i] == id) return rec_addr(TAB_N, i); return 0; }
void make_tables() {
    W32(MS_N + 0x88u, TAB_N); W32(TAB_N + 4u, 14);
    for (int i = 0; i < 14; i++) { W16(rec_addr(TAB_N, i), static_cast<uint16_t>(IDS[i])); W16(rec_addr(TAB_N, i) + 0x1Cu, static_cast<uint16_t>(10000 + i)); }
    // comb: 0xA2 Blitz [1934 1935 1936], 0xA4 Hazard [1939 3012 3013], 0xAA Onslaught [1946 1947 3011], 0xA7 HP [3020],
    //       0xAB one-step-only test command [3014]
    struct C { int id; std::vector<int> st; } cs[] = {{0xA2, {1934, 1935, 1936}}, {0xA4, {1939, 3012, 3013}},
                                                      {0xAA, {1946, 1947, 3011}}, {0xA7, {3020}}, {0xAB, {3014}}};
    W32(MS_N + 0x8Cu, COMB); W16(COMB, 3); W16(COMB + 2, 5);
    uint32_t o = 4 + 4 * 5;
    for (int i = 0; i < 5; i++) {
        W32(COMB + 4 + 4u * i, o);
        W16(COMB + o, static_cast<uint16_t>(cs[i].id)); W16(COMB + o + 2, static_cast<uint16_t>(cs[i].st.size()));
        W16(COMB + o + 4, 0xFFFF); W16(COMB + o + 6, 0xFFFF);
        for (size_t k = 0; k < cs[i].st.size(); k++) { W16(COMB + o + 8 + 8 * k, static_cast<uint16_t>(cs[i].st[k])); W16(COMB + o + 12 + 8 * k, 0xFFFF); }
        o += 8 + 8 * static_cast<uint32_t>(cs[i].st.size());
    }
    // ablt: 220C -> A2, 2202 -> A4, 2205 -> AA, 2208 -> A7 (HP), 2206 -> AB, 0901 -> A2 (Bartz's)
    struct A { int id, cmd; } as[] = {{0x0901, 0xA2}, {0x2202, 0xA4}, {0x2205, 0xAA}, {0x2206, 0xAB}, {0x2208, 0xA7}, {0x220C, 0xA2}};
    W32(kAbltBlockPtr, T_ABLT_BLK); W32(kAbltRecsPtr, T_ABLT_RECS); W32(T_ABLT_BLK + 8, 6);
    for (int i = 0; i < 6; i++) { W16(T_ABLT_RECS + 0x28u * i, static_cast<uint16_t>(as[i].id)); W16(T_ABLT_RECS + 0x28u * i + 0xC, static_cast<uint16_t>(as[i].cmd)); }
}

// ---- fakes
static int g_dash_calls = 0, g_sa_calls = 0, g_sel_calls = 0, g_ctx_calls = 0, g_sa_refuse = 0, g_target_calls = 0, g_hit_calls = 0;
static uint32_t g_target = 0, g_enemy = ENEMY, g_dirs = 0, g_lock_seen = 0, g_ctx_buf = 0, g_ctx_sp = 0;
static int g_slot_ab[12];
const uint32_t STOBJ = 0x092C0000u;
static int g_state_calls = 0, g_last_state = -1, g_other_state_calls = 0;
// FUN_08834C10's original: the actor's state object becomes the new state; a dash state switch = a normal dash
void orig_state(uint8_t*, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]); const int st = ctx->r[5];
    g_state_calls++; g_last_state = st;
    if (st == 0x13 || st == 0x19) g_dash_calls++; else g_other_state_calls++;
    W32(STOBJ + 0x10u * (actor >> 16 & 0xFu), static_cast<uint32_t>(st)); W32(actor + 0x3A4u, STOBJ + 0x10u * (actor >> 16 & 0xFu));
    ctx->r[4] = ctx->r[5] = ctx->r[6] = 0;
}
void switch_state(uint8_t* rdram, uint32_t actor, int st, uint32_t arg, uint32_t ra, uint32_t s0) {
    recomp_context c{};
    c.r[4] = static_cast<int32_t>(actor); c.r[5] = st; c.r[6] = static_cast<int32_t>(arg); c.r[31] = static_cast<int32_t>(ra);
    c.r[16] = static_cast<int32_t>(s0); c.r[29] = static_cast<int32_t>(0x09FE0000u);
    g_hooks.count(0x08834C10u) ? g_hooks.at(0x08834C10u)(rdram, &c) : orig_state(rdram, &c);
}
static bool g_air = false;
// FUN_08A50C08(actor, kind): +0x1076 when airborne, script 240, state 0x19 (s0 = kind at the switch, return 0x08A50CB0)
void orig_dash(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]); const uint32_t kind = static_cast<uint32_t>(ctx->r[5]);
    if (g_air) R(actor + 0x1076u)[0]++;
    W16(actor + 0x180, 240);
    switch_state(rdram, actor, 0x19, 0, 0x08A50CB0u, kind);
}
// FUN_08A53290(actor, type): script 240 (244 for type 3), +0x1075 when airborne, state 0x13 with the type
void orig_dash13(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]); const uint32_t type = static_cast<uint32_t>(ctx->r[5]);
    W16(actor + 0x180, type == 3 ? 244 : 240);
    if (g_air) R(actor + 0x1075u)[0]++;
    switch_state(rdram, actor, 0x13, type, 0x08A53394u, type);
}
// FUN_08B3B280: the lock-on target, any kind; FUN_08B3B1D4: the same only if its kind is 6
void fake_lock(uint8_t*, recomp_context* ctx) { g_target_calls++; g_lock_seen = static_cast<uint32_t>(ctx->r[4]); ctx->r[2] = static_cast<int32_t>(g_target); ctx->r[4] = 0; }
void fake_target(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = static_cast<int32_t>(g_target && R(g_target + 0x542u)[0] == 6 ? g_target : 0u); ctx->r[4] = 0;
}
void fake_enemy(uint8_t*, recomp_context* ctx) { ctx->r[2] = static_cast<int32_t>(g_enemy); ctx->r[4] = 0; }
void orig_hit(uint8_t*, recomp_context* ctx) { g_hit_calls++; ctx->r[4] = ctx->r[5] = 0; }
void fake_airborne(uint8_t*, recomp_context* ctx) { ctx->r[2] = g_air ? 1 : 0; ctx->r[4] = ctx->r[5] = 0; }
void fake_context(uint8_t*, recomp_context* ctx) {
    g_ctx_calls++; g_ctx_buf = static_cast<uint32_t>(ctx->r[4]); g_ctx_sp = static_cast<uint32_t>(ctx->r[29]);
    W32(g_ctx_buf + 0x78u, g_dirs); W32(g_ctx_buf + 0x7Cu, static_cast<uint32_t>(ctx->r[7]));
    W32(R32(0x089D4974u), TAB_N);                                 // FUN_08848078: holder -> the actor's table
    ctx->r[4] = ctx->r[5] = ctx->r[6] = ctx->r[7] = 0;
}
void fake_slot(uint8_t*, recomp_context* ctx) { const int s = ctx->r[5]; ctx->r[2] = s >= 0 && s < 12 ? g_slot_ab[s] : -1; ctx->r[4] = ctx->r[5] = 0; }
void orig_select(uint8_t*, recomp_context* ctx) { g_sel_calls++; W32(R32(0x089D4974u), OTHER_TAB); ctx->r[2] = static_cast<int32_t>(SEL_REC); }
void fake_start_action(uint8_t* rdram, recomp_context* ctx) {   // FUN_08B382B8(actor, kind, current)
    g_sa_calls++;
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]); const int kind = ctx->r[5]; const uint32_t cur = static_cast<uint32_t>(ctx->r[6]);
    recomp_context s = *ctx;
    s.r[4] = static_cast<int32_t>(R32(0x089D4974u)); s.r[5] = static_cast<int32_t>(actor);
    s.r[6] = static_cast<int32_t>(R32(actor + 0x2FCu)); s.r[7] = kind;
    g_hooks.at(0x0884A294u)(rdram, &s);
    const uint32_t rec = static_cast<uint32_t>(s.r[2]);
    uint32_t ret = 0;
    if (rec && !g_sa_refuse && rec != cur) {
        W32(actor + 0x101Cu, rec); W16(actor + 0x180, R16(rec + 0x1Cu)); ret = 1;
        switch_state(rdram, actor, 0x0F, 0, 0x08B38678u, 0);       // the action's own state (FUN_08B382B8 -> 0x0F)
    }
    ctx->r[2] = static_cast<int32_t>(ret); ctx->r[4] = ctx->r[5] = ctx->r[7] = 0;
}
void orig_other(uint8_t*, recomp_context*) {}
static bool g_have_overlay = true;
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x0884A294u) return orig_select;
    if (a == 0x088374B0u) return fake_airborne;
    if (a == 0x08849B50u) return fake_context;
    if (a == 0x0880AD28u) return orig_hit;
    if (a == 0x08834C10u) return orig_state;
    return orig_other;
}
FuncPtr ovl_battle_lookup(uint32_t a) {
    if (!g_have_overlay) return nullptr;
    if (a == 0x08B382B8u) return fake_start_action;
    if (a == 0x08A50C08u) return orig_dash;
    if (a == 0x08A53290u) return orig_dash13;
    if (a == 0x08B3B1D4u) return fake_target;
    if (a == 0x08B3B280u) return fake_lock;
    if (a == 0x08AB32B8u) return fake_enemy;
    if (a == 0x08B33B08u) return fake_slot;
    return nullptr;
}
void install(const char* spec) {
    g_entries.clear(); g_hooks.clear();
    for (auto& s : g_wd) s = WdState{};
    setenv("PSPRECOMP_ROSTER", spec, 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
}
// R+Triangle through the dispatcher: the typed dash FUN_08A53290 (kind 0 -> type 2, the ground-dash ability's; kind 1 ->
// type 3, the second-input dash that stays normal)
void dash(uint32_t actor, int kind) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(actor); ctx.r[5] = kind == 0 ? 2 : 3; ctx.r[29] = static_cast<int32_t>(SP0); ctx.r[16] = 0x5150;
    ctx.r[31] = static_cast<int32_t>(0x08B368B0u);
    g_hooks.at(0x08A53290u)(g_ram.data(), &ctx);
    CHECK(ctx.r[29] == static_cast<int32_t>(SP0) && ctx.r[16] == 0x5150);
}
// the state-0x19 dash FUN_08A50C08(actor, kind) (kind 1 = the 0x3A ability's type 6, kind 0 = the 0x11D dash)
void dash19(uint32_t actor, int kind) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(actor); ctx.r[5] = kind; ctx.r[29] = static_cast<int32_t>(SP0); ctx.r[16] = 0x5150;
    ctx.r[31] = static_cast<int32_t>(kind ? 0x08B3689Cu : 0x08B35F68u);
    g_hooks.at(0x08A50C08u)(g_ram.data(), &ctx);
    CHECK(ctx.r[29] == static_cast<int32_t>(SP0) && ctx.r[16] == 0x5150);
}
// a command press: FUN_08B382B8 asks the selector
recomp_context press(uint32_t actor, int kind) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(R32(0x089D4974u)); ctx.r[5] = static_cast<int32_t>(actor); ctx.r[6] = static_cast<int32_t>(ENEMY);
    ctx.r[7] = kind; ctx.r[29] = static_cast<int32_t>(SP0); ctx.r[17] = 0x7777;
    g_hooks.at(0x0884A294u)(g_ram.data(), &ctx);
    CHECK(ctx.r[29] == static_cast<int32_t>(SP0) && ctx.r[17] == 0x7777);
    return ctx;
}
void place(uint32_t a, float x, float y, float z) { WF(a + 0x80, x); WF(a + 0x84, y); WF(a + 0x88, z); }
// FUN_0880AD28(*(defender + 0x3AC) + 0x28, hit object): a hit of `hit` lands on that defender
void land(uint32_t defender_rec, uint32_t hit) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(defender_rec); ctx.r[5] = static_cast<int32_t>(hit); ctx.r[29] = static_cast<int32_t>(SP0);
    const int h0 = g_hit_calls;
    g_hooks.at(0x0880AD28u)(g_ram.data(), &ctx);
    CHECK(g_hit_calls == h0 + 1);
}
// the warp dash hit: lands on the enemy (connected) or nothing happens (a whiff)
void hit_list(uint32_t actor, int n, bool connected) {
    W32(HITOBJ + 0x18u, actor);
    if (connected && n > 0) land(EHIT + 0x28u, HITOBJ);
}
void ticks(uint32_t actor, int n) { for (int i = 0; i < n; i++) warpdash_tick(g_ram.data(), actor); }

int main() {
    W32(OVL_BASE, 1);
    W32(0x089D4974u, HOLDER);
    install("34 noc100 9 nm=Noctis warpdash=3027,3028,16,1.0,24,1;33 viv100 21");
    CHECK(g_hooks.count(0x08A50C08u) && g_hooks.count(0x0884A294u) && g_hooks.count(0x0880AD28u) && g_hooks.count(0x08834C10u) &&
          g_hooks.count(0x08A53290u));
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    // object kinds (+0x542): fighters 0 (RAM is zero), the dash's fixed-point target 6, an assist 1, a summoned object 3,
    // a bullet 5; each fighter's hit record *(+0x3AC) + 0x28
    R(CORE + 0x542u)[0] = 6; R(ASSIST + 0x542u)[0] = 1; R(SUMMON + 0x542u)[0] = 3; R(BULLET + 0x542u)[0] = 5;
    place(ASSIST, 1, 0, 6); place(SUMMON, -1, 0, 6); place(BARTZ, 2, 0, 5);
    W32(ENEMY + 0x3ACu, EHIT); W32(CORE + 0x3ACu, CHIT);
    W32(BHIT + 0x18u, BULLET); W32(BULLET + 0x2F8u, INFO_N);
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x390u, MS_N); W32(BARTZ + 0x390u, MS_N);
    make_tables();
    W32(NOC + 0x5ACu, LOCKBLK); W32(NOC + 0x300u, ENEMY); W32(NOC + 0x2FCu, ENEMY);
    place(NOC, 0, 0, 0); place(ENEMY, 0, 0, 10); place(CORE, 3, 0, 4);
    for (int& v : g_slot_ab) v = -1;
    g_slot_ab[0] = 0x220C; g_slot_ab[1] = 0x2202; g_slot_ab[3] = 0x2205; g_slot_ab[4] = 0x2206; g_slot_ab[6] = 0x2208;

    // 1. grounded R+Triangle at the enemy 10 m away: the ground warp dash starts (kind 1, forced record), no dash
    g_target = ENEMY;
    dash(NOC, 0);
    CHECK(g_target_calls == 1 && g_lock_seen == LOCKBLK);
    CHECK(g_sa_calls == 1 && g_dash_calls == 0 && R32(NOC + 0x101Cu) == rec_of(3027) && R(NOC + 0x1075u)[0] == 0);
    CHECK(g_sel_calls == 0 && R32(HOLDER) == TAB_N);
    // 2. airborne (3 m up): the air one; the air dash counted once (by the dash function, not again by the hook)
    g_air = true; W32(NOC + 0x101Cu, 0); WF(NOC + 0x3C0u, 3.0f);
    dash(NOC, 0);
    CHECK(g_sa_calls == 2 && g_dash_calls == 0 && R32(NOC + 0x101Cu) == rec_of(3028) && R(NOC + 0x1075u)[0] == 1);
    g_air = false; WF(NOC + 0x3C0u, 0.0f);
    // 2b. ground or air by the heights, not the engine's test: on the floor while the engine says airborne (the dash
    //     script) -> the ground one; on the floor but the enemy 5 m up -> the air one
    {
        const int s0 = g_sa_calls;
        g_air = true; W32(NOC + 0x101Cu, 0); dash(NOC, 0);
        CHECK(g_sa_calls == s0 + 1 && R32(NOC + 0x101Cu) == rec_of(3027));
        g_air = false; WF(ENEMY + 0x3C0u, 5.0f); W32(NOC + 0x101Cu, 0); dash(NOC, 0);
        CHECK(g_sa_calls == s0 + 2 && R32(NOC + 0x101Cu) == rec_of(3028));
        WF(ENEMY + 0x3C0u, 0.0f); R(NOC + 0x1075u)[0] = 1;
        g_sa_calls -= 2; g_target_calls -= 2;                      // keep the counts below as they were
    }
    // 3. the normal dash: another target (an EX core), out of range, no target, the dodge-derived dash (kind 1), refused
    g_target = CORE; dash(NOC, 0);
    CHECK(g_dash_calls == 1 && g_sa_calls == 2);
    g_target = ENEMY; place(ENEMY, 0, 0, 17); dash(NOC, 0);
    CHECK(g_dash_calls == 2 && g_sa_calls == 2);
    place(ENEMY, 0, 2, 10);
    g_target = 0; dash(NOC, 0);
    CHECK(g_dash_calls == 3 && g_sa_calls == 2);
    g_target = ENEMY; dash(NOC, 1);
    CHECK(g_dash_calls == 4 && g_sa_calls == 2 && g_target_calls == 5);
    g_sa_refuse = 1; dash(NOC, 0);
    CHECK(g_dash_calls == 5 && g_sa_calls == 3 && R(NOC + 0x1075u)[0] == 1 && g_last_state == 0x13);
    g_sa_refuse = 0;
    // 3b. what counts as "the enemy": an assist, a summoned object, a fighter that is not his enemy, himself, no lock-on
    //     block: the normal dash; the enemy by FUN_08AB32B8 alone or by +0x300 alone: the warp dash
    {
        const int d0 = g_dash_calls, s0 = g_sa_calls;
        g_target = ASSIST; dash(NOC, 0);
        g_target = SUMMON; dash(NOC, 0);
        g_target = BARTZ; dash(NOC, 0);
        g_target = NOC; dash(NOC, 0);
        g_target = ENEMY; W32(NOC + 0x5ACu, 0); dash(NOC, 0); W32(NOC + 0x5ACu, LOCKBLK);
        CHECK(g_dash_calls == d0 + 5 && g_sa_calls == s0);
        W32(NOC + 0x300u, 0); W32(NOC + 0x101Cu, 0); dash(NOC, 0);        // +0x300 empty, FUN_08AB32B8 says ENEMY
        CHECK(g_dash_calls == d0 + 5 && g_sa_calls == s0 + 1 && R32(NOC + 0x101Cu) == rec_of(3027));
        W32(NOC + 0x300u, ENEMY); g_enemy = 0; W32(NOC + 0x101Cu, 0); dash(NOC, 0);   // no enemy function answer, +0x300
        CHECK(g_dash_calls == d0 + 5 && g_sa_calls == s0 + 2 && R32(NOC + 0x101Cu) == rec_of(3027));
        g_enemy = ENEMY;
        g_target_calls -= 6; g_dash_calls -= 5; g_sa_calls -= 2;    // keep the counts below as they were
    }
    // 4. Bartz (no warpdash=): his own dash, nothing asked
    dash(BARTZ, 0);
    CHECK(g_dash_calls == 6 && g_sa_calls == 3 && g_target_calls == 6);

    // 4b. the state-0x19 dash (FUN_08A50C08): kind 1 (the 0x3A ability) at the enemy -> the warp dash; at a kind-6 point
    //     (the 0x11D dash at an EX core) -> its state 0x19; the start action's own state switch (0x0F) is passed through
    {
        const int d0 = g_dash_calls, s0 = g_sa_calls, o0 = g_other_state_calls;
        g_target = ENEMY; W32(NOC + 0x101Cu, 0);
        dash19(NOC, 1);
        CHECK(g_sa_calls == s0 + 1 && g_dash_calls == d0 && R32(NOC + 0x101Cu) == rec_of(3027) && g_other_state_calls == o0 + 1 &&
              g_last_state == 0x0F);
        g_target = CORE; dash19(NOC, 0);
        CHECK(g_sa_calls == s0 + 1 && g_dash_calls == d0 + 1 && g_last_state == 0x19);
        g_air = true; g_target = ENEMY; W32(NOC + 0x101Cu, 0); WF(NOC + 0x3C0u, 3.0f);
        dash19(NOC, 1);                                            // in the air: the air one, +0x1076 counted once
        CHECK(g_sa_calls == s0 + 2 && R32(NOC + 0x101Cu) == rec_of(3028) && R(NOC + 0x1076u)[0] == 1);
        g_air = false; WF(NOC + 0x3C0u, 0.0f);
        // a switch to any other state is not looked at (no lock-on question)
        const int t0 = g_target_calls;
        switch_state(g_ram.data(), NOC, 0x06, 0, 0x08834000u, 0);
        switch_state(g_ram.data(), NOC, 0x0D, 0, 0x08A59D30u, 0);   // the CPU's dash state
        CHECK(g_target_calls == t0 && g_last_state == 0x0D);
        // Bartz's typed dash and state 0x19: his own
        switch_state(g_ram.data(), BARTZ, 0x13, 2, 0x08A53394u, 2);
        CHECK(g_target_calls == t0 && g_last_state == 0x13);
        g_target_calls -= 2; g_sa_calls -= 2; g_dash_calls -= 2;   // keep the counts below as they were
    }
    // 5. the link: not armed before a hit (a press goes to the engine's selector)
    W32(NOC + 0x101Cu, 0);
    dash(NOC, 0);                                                  // ground warp dash 3027
    CHECK(R32(NOC + 0x101Cu) == rec_of(3027));
    hit_list(NOC, 1, false); ticks(NOC, 5);
    recomp_context c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == 1);
    // not armed by: his hit landing on another object's record, a bullet's hit (owner kind 5, even one whose +0x2F8 reads
    // as Noctis), or his hit while his current record is not the warp dash
    W32(HITOBJ + 0x18u, NOC); land(CHIT + 0x28u, HITOBJ);
    land(EHIT + 0x28u, BHIT);
    W32(NOC + 0x101Cu, rec_of(1934)); land(EHIT + 0x28u, HITOBJ); W32(NOC + 0x101Cu, rec_of(3027));
    ticks(NOC, 1);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == 2);
    // the hit connects: armed; a neutral circle on the ground = slot 0 (Blitz) -> its press 2, 1935
    hit_list(NOC, 1, true); ticks(NOC, 1);
    WF(NOC + 0x3C0u, 0.0f);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935) && g_sel_calls == 2 && R(NOC + 0x107Eu)[0] == 0);
    CHECK(g_ctx_sp == g_ctx_buf && g_ctx_buf < SP0 && g_ctx_buf >= SP0 - 0x100u && R32(HOLDER) == TAB_N);
    // one link per hit (a second landing of the same warp dash does not re-arm it)
    hit_list(NOC, 1, true);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == 3);
    // 5b. the thrown sword's hit, if the game credits it to Noctis, arms the link; the strike's hit later renews the
    //     window (a press 40 frames after the sword, 20 after the strike, still links); a strike that whiffs after the
    //     sword hit: the window runs from the sword hit and expires; after a press used it, a hit does not re-arm
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); ticks(NOC, 1);
    W32(HITOBJ + 0x18u, NOC); land(EHIT + 0x28u, HITOBJ);                 // the sword (credited to Noctis)
    ticks(NOC, 20);
    land(EHIT + 0x28u, HITOBJ);                                            // the strike
    ticks(NOC, 20);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    land(EHIT + 0x28u, HITOBJ); ticks(NOC, 1);                             // used: no re-arm in the same warp dash
    {
        const int sel = g_sel_calls;
        c = press(NOC, 0);
        CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == sel + 1);
    }
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); ticks(NOC, 1);
    land(EHIT + 0x28u, HITOBJ);                                            // the sword hits, the strike whiffs
    ticks(NOC, 30);
    {
        const int sel = g_sel_calls;
        c = press(NOC, 0);
        CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == sel + 1);
    }
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); ticks(NOC, 1);
    land(EHIT + 0x28u, BHIT);                                              // a sword bullet's own hit (owner kind 5): nothing
    ticks(NOC, 1);
    {
        const int sel = g_sel_calls;
        c = press(NOC, 0);
        CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == sel + 1);
    }
    g_sel_calls -= 3;                                                      // keep the counts below as they were
    // 6. a direction (slot 1 = Hazard Raid) with directional slots on: its press 2, the launcher 3012; through start action
    W32(NOC + 0x101Cu, rec_of(3027)); R(NOC + 0x5B0u)[0] = 1; g_dirs = 0x80;
    hit_list(NOC, 1, false); ticks(NOC, 1);                       // a new warp dash run (dash_start_hook resets)
    dash(NOC, 0);
    hit_list(NOC, 1, true); ticks(NOC, 1);
    {
        recomp_context s{}; s.r[4] = static_cast<int32_t>(NOC); s.r[5] = 0; s.r[6] = 0; s.r[29] = static_cast<int32_t>(SP0);
        fake_start_action(g_ram.data(), &s);                       // the command input path: FUN_08B382B8(actor, 0, 0)
        CHECK(s.r[2] == 1 && R32(NOC + 0x101Cu) == rec_of(3012) && R(NOC + 0x107Eu)[0] == 1);
    }
    // direction bits ignored when the directional slots are off: slot 0
    dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    R(NOC + 0x5B0u)[0] = 0;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    R(NOC + 0x5B0u)[0] = 1; g_dirs = 0;
    // 7. the air warp dash, airborne: the air set (slot 3 = Onslaught -> 1947), even under NEAR (0.4 m: the engine still
    //    has him in the air, so no ground press); touched down (engine grounded): the ground set
    g_air = true; W32(NOC + 0x101Cu, 0); WF(NOC + 0x3C0u, 3.0f); dash(NOC, 0);
    CHECK(R32(NOC + 0x101Cu) == rec_of(3028));
    hit_list(NOC, 2, true); ticks(NOC, 1);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1947) && R(NOC + 0x107Eu)[0] == 3);
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 2, true); ticks(NOC, 1);
    WF(NOC + 0x3C0u, 0.4f);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1947) && R(NOC + 0x107Eu)[0] == 3);
    WF(NOC + 0x3C0u, 3.0f); W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 2, true); ticks(NOC, 1);
    g_air = false; WF(NOC + 0x3C0u, 0.0f);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935) && R(NOC + 0x107Eu)[0] == 0);
    // 7b. the ground warp dash links into the ground set even if the engine's test says airborne
    W32(NOC + 0x101Cu, 0); dash(NOC, 0);
    CHECK(R32(NOC + 0x101Cu) == rec_of(3027));
    hit_list(NOC, 1, true); ticks(NOC, 1);
    g_air = true;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935) && R(NOC + 0x107Eu)[0] == 0);
    g_air = true; WF(NOC + 0x3C0u, 3.0f);
    // air slot 4 (Dagger Strike, one-step command here): its step 0 (no press 2 to skip to)
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 2, true); ticks(NOC, 1);
    WF(NOC + 0x3C0u, 3.0f); g_dirs = 0x20;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(3014) && R(NOC + 0x107Eu)[0] == 4);
    g_dirs = 0; g_air = false; WF(NOC + 0x3C0u, 0.0f);
    // 8. HP press (kind 2): slot 6 -> Royal Cleave's step 0, 3020; with HP=0 the engine's selector
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    c = press(NOC, 2);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(3020) && R(NOC + 0x107Eu)[0] == 6);
    // other kinds (the AI's 1 / 3, kind 4) never link
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    const int sel0 = g_sel_calls;
    press(NOC, 1); press(NOC, 3); press(NOC, 4);
    CHECK(g_sel_calls == sel0 + 3);
    c = press(NOC, 0);                                             // still armed for the player's circle
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    // 9. empty slot: the engine's selector (and the link stays armed)
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    g_slot_ab[0] = -1;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC);
    g_slot_ab[0] = 0x220C;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    // 10. the link survives leaving the warp dash (touchdown: no current record) for LINK frames, then expires
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    W32(NOC + 0x101Cu, 0); ticks(NOC, 20);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    W32(NOC + 0x101Cu, 0); ticks(NOC, 30);
    const int sel1 = g_sel_calls;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC && g_sel_calls == sel1 + 1);
    // a whiff never arms
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 0, false); ticks(NOC, 40);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC);
    // 11. menu overlay resident: the dash call (and the state switch) is not ours
    W32(OVL_BASE, 2);
    const int d0 = g_dash_calls, s0 = g_sa_calls;
    dash(NOC, 0);
    CHECK(g_dash_calls == d0 && g_sa_calls == s0);
    W32(OVL_BASE, 1);
    // 12. HP link off
    install("34 noc100 9 warpdash=3027,3028,16,1.0,24,0");
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    c = press(NOC, 2);
    CHECK(static_cast<uint32_t>(c.r[2]) == SEL_REC);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1935));
    // 12b. wdopen=: Blitz (A2) links into its press 1, 1934; Hazard Raid (A4) still press 2; HP unchanged; bad token
    install("34 noc100 9 warpdash=3027,3028,20,1.0,24,1 wdopen=A2/AB");
    CHECK(g_entries[0].wd_open.size() == 2 && g_entries[0].wd_open[0] == 0xA2 && g_entries[0].wd_open[1] == 0xAB);
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(1934) && R(NOC + 0x107Eu)[0] == 0);
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    R(NOC + 0x5B0u)[0] = 1; g_dirs = 0x80;
    c = press(NOC, 0);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(3012));
    R(NOC + 0x5B0u)[0] = 0; g_dirs = 0;
    W32(NOC + 0x101Cu, 0); dash(NOC, 0); hit_list(NOC, 1, true); ticks(NOC, 1);
    c = press(NOC, 2);
    CHECK(static_cast<uint32_t>(c.r[2]) == rec_of(3020));
    install("34 noc100 9 warpdash=3027,3028,20,1.0,24,1 wdopen=A2/zz");
    CHECK(g_entries[0].wd_open.empty() && g_hooks.count(0x08A50C08u));
    // 13. tokens: four values (defaults for LINK / HP), bad ones, none, and no battle overlay: no hooks
    install("34 noc100 9 warpdash=3027,3028,12,0.8");
    CHECK(g_hooks.count(0x08A50C08u) && g_entries[0].wd_link == 24 && g_entries[0].wd_hp && g_entries[0].wd_range == 12.0f);
    install("34 noc100 9 warpdash=3027,3028");
    CHECK(!g_hooks.count(0x08A50C08u) && !g_hooks.count(0x08834C10u));
    install("34 noc100 9 warpdash=3027,3028,16,1.0,24,1,9");
    CHECK(!g_hooks.count(0x08A50C08u));
    install("34 noc100 9 warpdash=3027,x,16,1.0");
    CHECK(!g_hooks.count(0x08A50C08u));
    install("34 noc100 9 nm=Noctis");
    CHECK(!g_hooks.count(0x08A50C08u) && !g_hooks.count(0x0884A294u));
    g_have_overlay = false;
    install("34 noc100 9 warpdash=3027,3028,16,1.0,24,1");
    CHECK(!g_hooks.count(0x08A50C08u));
    g_have_overlay = true;
    std::printf("ok test_warp_dash (%d dashes started, %d normal dashes, %d engine selector calls)\n", g_sa_calls, g_dash_calls, g_sel_calls);
    return 0;
}
