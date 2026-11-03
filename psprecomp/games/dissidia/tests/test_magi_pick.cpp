// Host test for roster.cpp's magipick= hook (a fired magi becomes one of a set: Noctis's elemancy flask), built on
// test_hit_action's fakes; the fake FUN_08AD0978 records the record id it was asked to create.
// Original header of those fakes: chain-window action hook (roster.txt chainact=ACTION[,ACTION...]): fake guest RAM, a fake
// battle-overlay FUN_08B308D8 (timeline op 0x0467 adds {record, a, b} to the actor's chain list), a fake EBOOT bone pass
// FUN_0880EA20, a fake EBOOT command selector FUN_0884A294 and a fake FUN_08B382B8 (start action) that, like the real
// one, asks the selector (through the dispatcher) for the record of the input kind it is given and starts that record.
// Built like the other roster tests (run_chain_action_test.sh).
#include ROSTER_CPP
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
uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, R(a), 2); return v; }
uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, R(a), 4); return v; }

const uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, INFO_N = 0x09230000u, INFO_B = 0x09230100u;
const uint32_t MS_N = 0x09260000u, TAB_N = 0x09261000u, MS_B = 0x09270000u, TAB_B = 0x09271000u;
const uint32_t HOLDER = 0x09280000u, OTHER_TAB = 0x09290000u, SEL_REC = 0x09291008u;
uint32_t rec_addr(uint32_t tab, int i) { return tab + 8u + 0x74u * static_cast<uint32_t>(i); }
void make_table(uint32_t ms, uint32_t tab, std::initializer_list<std::pair<int, int>> recs) {   // {action id, script}
    W32(ms + 0x88u, tab); W32(tab + 4u, static_cast<uint32_t>(recs.size()));
    int i = 0;
    for (auto& r : recs) { W16(rec_addr(tab, i), static_cast<uint16_t>(r.first)); W16(rec_addr(tab, i) + 0x1Cu, static_cast<uint16_t>(r.second)); i++; }
}

static int g_add_calls = 0, g_bone_calls = 0, g_sa_calls = 0, g_sel_calls = 0, g_sa_refuse = 0;
static uint32_t g_sa_actor = 0; static int g_sa_kind = -1, g_sa_cur = -1;
void fake_chain_add(uint8_t*, recomp_context* ctx) {          // FUN_08B308D8: append {record, a, b}, count +1
    g_add_calls++;
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    const int16_t n = static_cast<int16_t>(R16(actor + 0x1068u));
    W32(actor + 0x1028u + 8u * static_cast<uint32_t>(n), static_cast<uint32_t>(ctx->r[5]));
    R(actor + 0x102Cu + 8u * static_cast<uint32_t>(n))[0] = static_cast<uint8_t>(ctx->r[6]);
    R(actor + 0x102Du + 8u * static_cast<uint32_t>(n))[0] = static_cast<uint8_t>(ctx->r[7]);
    W16(actor + 0x1068u, static_cast<uint16_t>(n + 1));
    ctx->r[5] = n + 1; ctx->r[8] = n; ctx->r[9] = 0x77;          // what it leaves in its registers
}
void fake_bone_post(uint8_t*, recomp_context* ctx) {          // FUN_0880EA20: hands back its own registers
    g_bone_calls++;
    ctx->r[2] = 0x4321; ctx->r[3] = 0x8765; ctx->f[0] = 1.5f;
    ctx->r[4] = ctx->r[5] = ctx->r[6] = ctx->r[7] = 0;
}
void orig_select(uint8_t*, recomp_context* ctx) {             // the engine's selector: kind 1 -> SEL_REC
    g_sel_calls++;
    W32(R32(0x089D4974u), OTHER_TAB);
    ctx->r[2] = ctx->r[7] == 1 ? static_cast<int32_t>(SEL_REC) : 0;
}
void fake_start_action(uint8_t* rdram, recomp_context* ctx) {   // FUN_08B382B8(actor, kind, current)
    g_sa_calls++;
    g_sa_actor = static_cast<uint32_t>(ctx->r[4]); g_sa_kind = ctx->r[5]; g_sa_cur = ctx->r[6];
    recomp_context s = *ctx;
    s.r[4] = static_cast<int32_t>(R32(0x089D4974u)); s.r[5] = static_cast<int32_t>(g_sa_actor);
    s.r[6] = static_cast<int32_t>(R32(g_sa_actor + 0x2FCu)); s.r[7] = g_sa_kind;
    g_hooks.at(0x0884A294u)(rdram, &s);
    const uint32_t rec = static_cast<uint32_t>(s.r[2]);
    uint32_t ret = 0;
    if (rec && !g_sa_refuse && rec != static_cast<uint32_t>(g_sa_cur)) {
        W32(g_sa_actor + 0x101Cu, rec);
        W16(g_sa_actor + 0x180, R16(rec + 0x1Cu));
        W16(g_sa_actor + 0x1068u, 0);                             // FUN_08B30904: the chain list is cleared
        ret = 1;
    }
    ctx->r[2] = static_cast<int32_t>(ret); ctx->r[3] = 0x9999; ctx->f[0] = -1.0f; ctx->r[4] = ctx->r[5] = ctx->r[7] = 0;
}
void orig_other(uint8_t*, recomp_context*) {}
static bool g_have_add = true, g_have_overlay = true;
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x0884A294u) return orig_select;
    if (a == 0x0880EA20u) return fake_bone_post;
    return orig_other;
}
static int g_spawn_calls = 0; static bool g_have_spawn = true;
static int g_spawn_id = -1;
void fake_magi_spawn(uint8_t*, recomp_context* ctx) {      // FUN_08AD0978(id, owner, ...): returns an emitter
    g_spawn_calls++; g_spawn_id = ctx->r[4]; ctx->r[2] = 0x09500000; ctx->r[4] = ctx->r[5] = 0;
}
FuncPtr ovl_battle_lookup(uint32_t a) {
    if (a == 0x08AD0978u) return g_have_spawn ? fake_magi_spawn : nullptr;
    if (a == 0x08B382B8u) return g_have_overlay ? fake_start_action : nullptr;
    if (a == 0x08B308D8u) return g_have_add ? fake_chain_add : nullptr;
    return nullptr;
}

// timeline op 0x0467 {0, id, 1, 1} through the dispatcher: the record the engine looked up in the holder's table
void window(uint32_t actor, uint32_t rec) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(actor); ctx.r[5] = static_cast<int32_t>(rec); ctx.r[6] = 1; ctx.r[7] = 1; ctx.r[29] = 0x09FE0000;
    g_hooks.at(0x08B308D8u)(g_ram.data(), &ctx);
    CHECK(ctx.r[9] == 0x77 || !g_have_add || R32(OVL_BASE) != 1u);   // the engine function's registers come back
}
// one bone pass of an actor through the dispatcher: returns the registers its caller sees
recomp_context bone_pass(uint32_t actor) {
    recomp_context ctx{};
    ctx.r[4] = 0x09400000; ctx.r[5] = 0x09410000; ctx.r[6] = static_cast<int32_t>(actor); ctx.r[29] = 0x09FF0000;
    g_hooks.at(0x0880EA20u)(g_ram.data(), &ctx);
    return ctx;
}
uint32_t select(uint32_t actor, int kind) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(R32(0x089D4974u)); ctx.r[5] = static_cast<int32_t>(actor); ctx.r[7] = kind;
    g_hooks.at(0x0884A294u)(g_ram.data(), &ctx);
    return static_cast<uint32_t>(ctx.r[2]);
}
void install(const char* spec) {
    g_entries.clear(); g_hooks.clear();
    for (ChainPend& p : g_chain_pend) p = ChainPend{};
    for (PickState& p : g_pick) p = PickState{};       // a new battle's actors
    g_start_action = nullptr; g_plya_select = nullptr; g_chain_add = nullptr;
    setenv("PSPRECOMP_ROSTER", spec, 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
}


void spawn(uint32_t owner, int id) {
    recomp_context ctx{};
    ctx.r[4] = id; ctx.r[5] = static_cast<int32_t>(owner); ctx.r[29] = 0x09FD0000;
    g_hooks.at(0x08AD0978u)(g_ram.data(), &ctx);
    CHECK(R32(OVL_BASE) != 1u || ctx.r[2] == 0x09500000);       // the engine's return value comes back
}
int fire(uint32_t owner, int id) { spawn(owner, id); return g_spawn_id; }
int main() {
    W32(OVL_BASE, 1);
    W32(0x089D4974u, HOLDER);
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    // 1. random: only the listed ids, never the same twice in a row, every one turns up
    install("34 noc100 9 magipick=2285,r,2285,2286,2287");
    CHECK(g_hooks.count(0x08AD0978u));
    CHECK(!g_hooks.count(0x08B308D8u));                       // no chainact= / hitact=: only the spawn wrapper
    int seen[3] = {0, 0, 0}, last = -1;
    for (int i = 0; i < 300; i++) {
        const int got = fire(NOC, 2285);
        CHECK(got >= 2285 && got <= 2287);
        CHECK(got != last);
        seen[got - 2285]++; last = got;
    }
    CHECK(seen[0] > 50 && seen[1] > 50 && seen[2] > 50);
    // 2. other records, other characters, a bogus owner: untouched
    CHECK(fire(NOC, 2281) == 2281);
    CHECK(fire(BARTZ, 2285) == 2285);
    CHECK(fire(0x00001000u, 2285) == 2285);
    // 3. the record id's upper half is kept (a0 is a u16 id in a register)
    {
        recomp_context ctx{}; ctx.r[4] = static_cast<int32_t>(0x00010000u | 2285u); ctx.r[5] = static_cast<int32_t>(NOC); ctx.r[29] = 0x09FD0000;
        g_hooks.at(0x08AD0978u)(g_ram.data(), &ctx);
        CHECK((static_cast<uint32_t>(g_spawn_id) >> 16) == 1u && (g_spawn_id & 0xFFFF) >= 2285 && (g_spawn_id & 0xFFFF) <= 2287);
    }
    // 4. in turn
    install("34 noc100 9 magipick=2285,c,2285,2286,2287");
    for (int i = 0; i < 6; i++) CHECK(fire(NOC, 2285) == 2285 + i % 3);
    // 5. beside hitact= (one wrapper does both)
    install("34 noc100 9 hitact=2277,3029 magipick=2285,c,2286,2287");
    CHECK(g_hooks.count(0x08AD0978u) && g_hooks.count(0x0884A294u));
    CHECK(fire(NOC, 2285) == 2286 && fire(NOC, 2285) == 2287 && fire(NOC, 2277) == 2277);
    // 6. a single id: always that one
    install("34 noc100 9 magipick=2285,r,2287");
    for (int i = 0; i < 5; i++) CHECK(fire(NOC, 2285) == 2287);
    // 6b. several sets (one per charge level): each base picks from its own ids; the last pick index is shared
    install("34 noc100 9 magipick=2285,r,2285,2286,2287 magipick=2291,r,2291,2292,2293 magipick=2294,c,2294,2295,2296");
    for (int i = 0; i < 60; i++) {
        const int a = fire(NOC, 2285), b = fire(NOC, 2291);
        CHECK(a >= 2285 && a <= 2287 && b >= 2291 && b <= 2293 && b - 2291 != a - 2285);
    }
    CHECK(fire(NOC, 2290) == 2290);
    { const int c = fire(NOC, 2294); CHECK(c >= 2294 && c <= 2296); }
    // 7. the menu overlay is resident: nothing runs
    W32(OVL_BASE, 2);
    const int sp = g_spawn_calls;
    spawn(NOC, 2285);
    CHECK(g_spawn_calls == sp);
    W32(OVL_BASE, 1);
    // 8. bad tokens: no hook
    install("34 noc100 9 magipick=2285,x,2286");
    CHECK(!g_hooks.count(0x08AD0978u));
    install("34 noc100 9 magipick=2285,r");
    CHECK(!g_hooks.count(0x08AD0978u));
    install("34 noc100 9 magipick=2285,r,1,2,3,4,5,6,7");
    CHECK(!g_hooks.count(0x08AD0978u));
    install("34 noc100 9 magipick=2285,r,2286,abc");
    CHECK(!g_hooks.count(0x08AD0978u));
    g_have_spawn = false;
    install("34 noc100 9 magipick=2285,r,2286,2287");
    CHECK(!g_hooks.count(0x08AD0978u));
    g_have_spawn = true;
    std::printf("ok test_magi_pick (%d spawns; random split %d / %d / %d)\n", g_spawn_calls, seen[0], seen[1], seen[2]);
    return 0;
}
