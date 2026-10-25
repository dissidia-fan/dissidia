// Host test for roster.cpp's touchdown-action hook (roster.txt landact=ACTION,MOTION[,MOTION...]): fake guest RAM, a fake
// FUN_08833C98 (the air-state touchdown: plays `_022A`, returns with its own registers), a fake EBOOT command selector
// FUN_0884A294 (holder, actor, target, kind) and a fake battle-overlay FUN_08B382B8 (start action) that, like the real
// one, asks the selector (through the dispatcher) for the record of the input kind it is given and starts that record.
// Built like the other roster tests:
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_land_action test_land_action.cpp; /tmp/test_land_action
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
uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, R(a), 2); return v; }
uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, R(a), 4); return v; }

const uint32_t NOC = 0x09210000u, BARTZ = 0x09220000u, INFO_N = 0x09230000u, INFO_B = 0x09230100u, ROWS = 0x09250000u;
const uint32_t MS_N = 0x09260000u, TAB_N = 0x09261000u, MS_B = 0x09270000u, TAB_B = 0x09271000u;
const uint32_t HOLDER = 0x09280000u, OTHER_TAB = 0x09290000u, SEL_REC = 0x09291008u;
uint32_t rec_addr(uint32_t tab, int i) { return tab + 8u + 0x74u * static_cast<uint32_t>(i); }
void make_table(uint32_t ms, uint32_t tab, std::initializer_list<std::pair<int, int>> recs) {   // {action id, script}
    W32(ms + 0x88u, tab); W32(tab + 4u, static_cast<uint32_t>(recs.size()));
    int i = 0;
    for (auto& r : recs) { W16(rec_addr(tab, i), static_cast<uint16_t>(r.first)); W16(rec_addr(tab, i) + 0x1Cu, static_cast<uint16_t>(r.second)); i++; }
}

static int g_td_calls = 0, g_sa_calls = 0, g_sel_calls = 0, g_sa_refuse = 0;
static uint32_t g_sa_actor = 0; static int g_sa_kind = -1, g_sa_cur = -1;
void orig_touchdown(uint8_t* rdram, recomp_context* ctx) {     // plays `_022A` (script 102), queues stand, state 0
    g_td_calls++;
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    W16(actor + 0x180, 102);
    ctx->r[2] = 0x1234; ctx->r[3] = 0x5678; ctx->f[0] = 2.5f;      // what it hands back to its caller
    ctx->r[4] = ctx->r[5] = ctx->r[6] = ctx->r[7] = 0;            // clobbers the argument registers like real code
}
// the engine's selector: picks a record for an input kind 0-4 (here: kind 1 -> SEL_REC in another table), none otherwise
void orig_select(uint8_t*, recomp_context* ctx) {
    g_sel_calls++;
    W32(R32(0x089D4974u), OTHER_TAB);                             // FUN_08849B50 points the holder at the actor's table
    ctx->r[2] = ctx->r[7] >= 0 && ctx->r[7] <= 4 && ctx->r[7] == 1 ? static_cast<int32_t>(SEL_REC) : 0;
}
void fake_start_action(uint8_t* rdram, recomp_context* ctx) {   // FUN_08B382B8(actor, kind, current)
    g_sa_calls++;
    g_sa_actor = static_cast<uint32_t>(ctx->r[4]); g_sa_kind = ctx->r[5]; g_sa_cur = ctx->r[6];
    recomp_context s = *ctx;
    s.r[4] = static_cast<int32_t>(R32(0x089D4974u)); s.r[5] = static_cast<int32_t>(g_sa_actor);
    s.r[6] = static_cast<int32_t>(R32(g_sa_actor + 0x2FCu)); s.r[7] = g_sa_kind;
    g_hooks.at(0x0884A294u)(rdram, &s);                           // jal FUN_0884A294 (through the dispatch table)
    const uint32_t rec = static_cast<uint32_t>(s.r[2]);
    uint32_t ret = 0;
    if (rec && !g_sa_refuse && rec != static_cast<uint32_t>(g_sa_cur)) {
        W32(g_sa_actor + 0x101Cu, rec);
        W16(g_sa_actor + 0x180, R16(rec + 0x1Cu));                // its script now plays
        ret = 1;
    }
    ctx->r[2] = static_cast<int32_t>(ret); ctx->r[3] = 0x9999; ctx->f[0] = -1.0f; ctx->r[4] = ctx->r[5] = ctx->r[7] = 0;
}
void orig_other(uint8_t*, recomp_context*) {}
static bool g_have_overlay = true, g_have_select = true;
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x08833C98u) return orig_touchdown;
    if (a == 0x0884A294u) return g_have_select ? orig_select : nullptr;
    return orig_other;
}
FuncPtr ovl_battle_lookup(uint32_t a) { return a == 0x08B382B8u && g_have_overlay ? fake_start_action : nullptr; }

void set_motion(uint32_t actor, int script, int motion, uint32_t row) {   // as FUN_0882F0D4: script id + p_se row
    W16(row, static_cast<uint16_t>(script)); W16(row + 2, static_cast<uint16_t>(motion));
    W32(actor + 0x17C, row); W16(actor + 0x180, static_cast<uint16_t>(script));
}
// one touchdown through the dispatcher: returns the registers its caller sees
recomp_context touchdown(uint32_t actor) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(actor); ctx.r[29] = 0x09FF0000; ctx.f[12] = 0.75f;
    g_hooks.at(0x08833C98u)(g_ram.data(), &ctx);
    return ctx;
}
// a command press elsewhere in the game: the selector is asked directly
uint32_t select(uint32_t actor, int kind) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(R32(0x089D4974u)); ctx.r[5] = static_cast<int32_t>(actor); ctx.r[7] = kind;
    g_hooks.at(0x0884A294u)(g_ram.data(), &ctx);
    return static_cast<uint32_t>(ctx.r[2]);
}
void install(const char* spec) {
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", spec, 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
}

int main() {
    W32(OVL_BASE, 1);                                            // battle overlay resident
    W32(0x089D4974u, HOLDER);                                    // the plya holder
    install("34 noc100 9 nm=Noctis landact=3024,54,55;33 viv100 21");
    CHECK(g_hooks.count(0x08833C98u) && g_hooks.count(0x0884A294u));
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x390u, MS_N); W32(BARTZ + 0x390u, MS_B);
    make_table(MS_N, TAB_N, {{1934, 10003}, {1949, 10040}, {1950, 10041}, {3024, 10096}});
    make_table(MS_B, TAB_B, {{1934, 10003}, {1949, 10040}});
    const uint32_t REC3024 = rec_addr(TAB_N, 3);

    // 1. Noctis lands during the dive (motion 54): the engine handler runs, then FUN_08B382B8(actor, 1, 0) starts the
    //    record of action 3024 (the selector hands it back, holder pointed at Noctis's table); the caller gets the
    //    handler's own return registers back
    set_motion(NOC, 10040, 54, ROWS);
    recomp_context c = touchdown(NOC);
    CHECK(g_td_calls == 1 && g_sa_calls == 1 && g_sa_actor == NOC && g_sa_kind == 1 && g_sa_cur == 0);
    CHECK(g_sel_calls == 0);                                     // the engine selector was not asked
    CHECK(R32(NOC + 0x101Cu) == REC3024 && R16(NOC + 0x180) == 10096 && R32(HOLDER) == TAB_N);
    CHECK(c.r[2] == 0x1234 && c.r[3] == 0x5678 && c.f[0] == 2.5f && c.r[29] == 0x09FF0000);
    // 1b. outside the hook the selector is the engine's again
    CHECK(select(NOC, 1) == SEL_REC && g_sel_calls == 1 && R32(HOLDER) == OTHER_TAB);
    CHECK(select(NOC, 3024) == 0 && g_sel_calls == 2);         // what v3 did: no record for "kind 3024"
    // 2. ... and during the carry (motion 55)
    set_motion(NOC, 10041, 55, ROWS); W32(NOC + 0x101Cu, rec_addr(TAB_N, 2));
    touchdown(NOC);
    CHECK(g_td_calls == 2 && g_sa_calls == 2 && R32(NOC + 0x101Cu) == REC3024 && R16(NOC + 0x180) == 10096);
    // 3. any other motion of Noctis (a plain fall `_021A`, motion 3; Hazard Raid's dive, motion 59): plain landing
    set_motion(NOC, 101, 3, ROWS);
    touchdown(NOC);
    set_motion(NOC, 10050, 59, ROWS);
    touchdown(NOC);
    CHECK(g_td_calls == 4 && g_sa_calls == 2 && R16(NOC + 0x180) == 102);
    // 4. Bartz in his own motion 54: plain landing
    set_motion(BARTZ, 10040, 54, ROWS + 16);
    touchdown(BARTZ);
    CHECK(g_td_calls == 5 && g_sa_calls == 2);
    // 5. stale script row (the row's script is not the one at +0x180): plain landing
    set_motion(NOC, 10040, 54, ROWS);
    W16(NOC + 0x180, 10041);
    touchdown(NOC);
    CHECK(g_td_calls == 6 && g_sa_calls == 2);
    // 6. the action refuses to start (its gate): the landing the handler already began stays
    g_sa_refuse = 1;
    set_motion(NOC, 10040, 54, ROWS); W32(NOC + 0x101Cu, rec_addr(TAB_N, 1));
    c = touchdown(NOC);
    CHECK(g_td_calls == 7 && g_sa_calls == 3 && R16(NOC + 0x180) == 102 && c.r[2] == 0x1234 && R32(NOC + 0x101Cu) == rec_addr(TAB_N, 1));
    g_sa_refuse = 0;
    CHECK(select(NOC, 1) == SEL_REC && g_sel_calls == 3);       // the override does not outlive the call
    // 7. another overlay in the window (menu): no start
    W32(OVL_BASE, 2);
    set_motion(NOC, 10040, 54, ROWS);
    touchdown(NOC);
    CHECK(g_td_calls == 8 && g_sa_calls == 3);
    W32(OVL_BASE, 1);
    // 8. bad actor pointer: just the engine handler
    touchdown(0x00001000u);
    CHECK(g_td_calls == 9 && g_sa_calls == 3);
    // 9. no plya record for the action in this actor's table (or no table): plain landing, start action not called
    make_table(MS_N, TAB_N, {{1934, 10003}, {1949, 10040}});
    set_motion(NOC, 10040, 54, ROWS);
    touchdown(NOC);
    CHECK(g_td_calls == 10 && g_sa_calls == 3 && R16(NOC + 0x180) == 102);
    W32(NOC + 0x390u, 0);
    touchdown(NOC);
    CHECK(g_td_calls == 11 && g_sa_calls == 3);
    W32(NOC + 0x390u, MS_N);
    make_table(MS_N, TAB_N, {{1934, 10003}, {1949, 10040}, {1950, 10041}, {3024, 10096}});

    // 10. tokens: bad, missing, and no battle-overlay / selector function: no hook; one motion only is fine
    install("34 noc100 9 landact=3024");
    CHECK(!g_hooks.count(0x08833C98u));
    install("34 noc100 9 landact=x,54");
    CHECK(!g_hooks.count(0x08833C98u));
    install("34 noc100 9 nm=Noctis");
    CHECK(!g_hooks.count(0x08833C98u) && !g_hooks.count(0x0884A294u));
    g_have_overlay = false;
    install("34 noc100 9 landact=3024,54,55");
    CHECK(!g_hooks.count(0x08833C98u) && !g_hooks.count(0x0884A294u));
    g_have_overlay = true; g_have_select = false;
    install("34 noc100 9 landact=3024,54,55");
    CHECK(!g_hooks.count(0x08833C98u) && !g_hooks.count(0x0884A294u));
    g_have_select = true;
    install("34 noc100 9 landact=3024,55");
    CHECK(g_hooks.count(0x08833C98u) && g_hooks.count(0x0884A294u));
    set_motion(NOC, 10040, 54, ROWS);
    touchdown(NOC);
    CHECK(g_sa_calls == 3);
    set_motion(NOC, 10041, 55, ROWS); W32(NOC + 0x101Cu, 0);
    touchdown(NOC);
    CHECK(g_sa_calls == 4 && R32(NOC + 0x101Cu) == REC3024);
    std::printf("ok test_land_action (%d touchdowns, %d actions started, %d engine selector calls)\n", g_td_calls, g_sa_calls, g_sel_calls);
    return 0;
}
