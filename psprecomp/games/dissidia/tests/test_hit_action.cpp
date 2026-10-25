// Host test for roster.cpp's hitact= hook (an on-hit magi child starts an action), built on test_chain_action's fakes.
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
void fake_magi_spawn(uint8_t*, recomp_context* ctx) {      // FUN_08AD0978(id, owner, ...): returns an emitter
    g_spawn_calls++; ctx->r[2] = 0x09500000; ctx->r[4] = ctx->r[5] = 0;
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
int main() {
    W32(OVL_BASE, 1);
    W32(0x089D4974u, HOLDER);
    install("34 noc100 9 nm=Noctis chainact=3026 hitact=2277,3029 hitcam=300;33 viv100 21");
    CHECK(g_hooks.count(0x08AD0978u) && g_hooks.count(0x0884A294u) && g_hooks.count(0x0880EA20u));
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x390u, MS_N); W32(BARTZ + 0x390u, MS_B);
    make_table(MS_N, TAB_N, {{3025, 10097}, {3026, 10098}, {3029, 10101}});
    make_table(MS_B, TAB_B, {{3029, 10101}});
    const uint32_t R3025 = rec_addr(TAB_N, 0), R3029 = rec_addr(TAB_N, 2);
    // 1. A (3025) runs; the flash's other magi (2276 itself, the void 2267) start nothing
    W32(NOC + 0x101Cu, R3025); W32(NOC + 0x1020u, 0); W16(NOC + 0x180, 10097);
    spawn(NOC, 2276); spawn(NOC, 2267);
    bone_pass(NOC);
    CHECK(g_spawn_calls == 2 && g_sa_calls == 0 && R32(NOC + 0x101Cu) == R3025);
    // 2. the flash hit: its child 2277 spawns -> B (3029) starts in his next bone pass, not before
    spawn(NOC, 2277);
    CHECK(g_spawn_calls == 3 && g_sa_calls == 0);
    recomp_context c = bone_pass(NOC);
    CHECK(g_sa_calls == 1 && g_sa_actor == NOC && g_sa_kind == 1 && R32(NOC + 0x101Cu) == R3029 && R16(NOC + 0x180) == 10101);
    CHECK(g_dissidia_cine_seq == 1u && g_dissidia_cine_actor == NOC && g_dissidia_cine_ticks == 300);   // hitcam=
    CHECK(c.r[2] == 0x4321 && c.r[3] == 0x8765 && c.f[0] == 1.5f);
    CHECK(select(NOC, 1) == SEL_REC);                           // the override does not outlive the start
    bone_pass(NOC);
    CHECK(g_sa_calls == 1);
    // 3. a miss: no 2277, A runs to its end, nothing starts
    W32(NOC + 0x101Cu, R3025); W16(NOC + 0x180, 10097);
    spawn(NOC, 2276); bone_pass(NOC); bone_pass(NOC);
    CHECK(g_sa_calls == 1 && R32(NOC + 0x101Cu) == R3025);
    // 4. hit, but Noctis is hit himself before his bone pass (the running record changed): dropped
    spawn(NOC, 2277);
    W32(NOC + 0x101Cu, 0);
    bone_pass(NOC);
    CHECK(g_sa_calls == 1 && R32(NOC + 0x101Cu) == 0);
    // 5. someone else (Bartz, no token) spawning a 2277: nothing; a bogus owner pointer: nothing
    W32(BARTZ + 0x101Cu, 0);
    spawn(BARTZ, 2277); bone_pass(BARTZ);
    spawn(0x00001000u, 2277);
    CHECK(g_sa_calls == 1);
    // 6. the menu overlay is resident: the battle function is not run
    W32(NOC + 0x101Cu, R3025);
    W32(OVL_BASE, 2);
    const int sp = g_spawn_calls;
    spawn(NOC, 2277);
    W32(OVL_BASE, 1);
    bone_pass(NOC);
    CHECK(g_spawn_calls == sp && g_sa_calls == 1);
    // 7. his table has no 3029: not queued
    make_table(MS_N, TAB_N, {{3025, 10097}, {3026, 10098}});
    spawn(NOC, 2277); bone_pass(NOC);
    CHECK(g_sa_calls == 1);
    make_table(MS_N, TAB_N, {{3025, 10097}, {3026, 10098}, {3029, 10101}});
    // 8. chainact still works beside it (one pending list): a 0x0467 window to 3026
    window(NOC, rec_addr(TAB_N, 1)); bone_pass(NOC);
    CHECK(g_sa_calls == 2 && R32(NOC + 0x101Cu) == rec_addr(TAB_N, 1));
    // 9. tokens
    install("34 noc100 9 hitact=2277,3029");
    CHECK(g_hooks.count(0x08AD0978u) && g_hooks.count(0x0880EA20u) && g_hooks.count(0x0884A294u));
    W32(NOC + 0x101Cu, R3025);
    spawn(NOC, 2277); bone_pass(NOC);
    CHECK(g_sa_calls == 3 && R32(NOC + 0x101Cu) == R3029);
    CHECK(g_dissidia_cine_seq == 1u);                            // no hitcam= token: no framing request
    install("34 noc100 9 hitact=2277");
    CHECK(!g_hooks.count(0x08AD0978u));
    install("34 noc100 9 hitact=2277,3029,5");
    CHECK(!g_hooks.count(0x08AD0978u));
    install("34 noc100 9 chainact=3026");
    CHECK(!g_hooks.count(0x08AD0978u) && g_hooks.count(0x08B308D8u));
    g_have_spawn = false;
    install("34 noc100 9 hitact=2277,3029");
    CHECK(!g_hooks.count(0x08AD0978u));
    g_have_spawn = true;
    std::printf("ok test_hit_action (%d spawns, %d bone passes, %d actions started)\n", g_spawn_calls, g_bone_calls, g_sa_calls);
    return 0;
}
