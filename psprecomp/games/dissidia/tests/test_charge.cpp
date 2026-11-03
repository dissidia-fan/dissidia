// Host test for roster.cpp's charge= hook (hold to charge: Noctis's Elemancy), built on test_magi_pick's fakes, plus a fake
// battle-overlay FUN_08B30B54 (the HP button is still held) and the actor's script row / motion time.
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
static bool g_held = true, g_have_held = true; static int g_held_calls = 0;
void fake_hp_held(uint8_t*, recomp_context* ctx) { g_held_calls++; ctx->r[2] = g_held ? 1 : 0; ctx->r[4] = ctx->r[5] = 0; ctx->r[9] = 0x55; }
FuncPtr ovl_battle_lookup(uint32_t a) {
    if (a == 0x08B30B54u) return g_have_held ? fake_hp_held : nullptr;
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
    for (ChargeRun& c : g_charge) c = ChargeRun{};
    g_hp_held = nullptr;
    g_start_action = nullptr; g_plya_select = nullptr; g_chain_add = nullptr;
    setenv("PSPRECOMP_ROSTER", spec, 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
}



const uint32_t ROW = 0x092A0000u;
void play(uint32_t actor, int script, int motion, float t) {      // the actor's script row (+0x17C / +0x180) and motion time
    W16(ROW + 8u * static_cast<uint32_t>(motion), static_cast<uint16_t>(script)); W16(ROW + 8u * static_cast<uint32_t>(motion) + 2u, static_cast<uint16_t>(motion));
    W32(actor + 0x17Cu, ROW + 8u * static_cast<uint32_t>(motion)); W16(actor + 0x180u, static_cast<uint16_t>(script));
    std::memcpy(R(actor + 0x1A8u), &t, 4);
}
// runs the charge motion from tick 0, the button released at tick `rel` (-1: never), bone passes every 2 ticks up to
// `upto`; returns the action started (-1 none) and the tick
int run(float rel, float upto, float* at, int motion = 94, int script = 10106) {
    W32(NOC + 0x101Cu, rec_addr(TAB_N, 0)); W32(NOC + 0x1020u, 0);
    const int sa = g_sa_calls;
    *at = -1.0f;
    for (float t = 0.0f; t <= upto; t += 2.0f) {
        g_held = rel < 0.0f || t < rel;
        play(NOC, script, motion, t);
        const recomp_context c = bone_pass(NOC);
        CHECK(c.r[2] == 0x4321 && c.r[3] == 0x8765);       // the bone pass's own registers come back
        if (g_sa_calls != sa && *at < 0.0f) {
            *at = t;
            const uint32_t rec = R32(NOC + 0x101Cu);
            return R16(rec);
        }
    }
    return g_sa_calls != sa ? R16(R32(NOC + 0x101Cu)) : -1;
}
int main() {
    W32(OVL_BASE, 1);
    W32(0x089D4974u, HOLDER);
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    W32(NOC + 0x390u, MS_N); W32(BARTZ + 0x390u, MS_B);
    make_table(MS_N, TAB_N, {{3034, 10106}, {3035, 10107}, {3036, 10108}, {3037, 10109}, {3038, 10110}, {3039, 10111}, {3040, 10112}, {3041, 10113}});
    make_table(MS_B, TAB_B, {{3034, 10106}});
    install("34 noc100 9 charge=94,20,50,90,120,3035,3036,3037");
    CHECK(g_hooks.count(0x0880EA20u) && g_hooks.count(0x0884A294u));
    float at;
    // 1. a tap: released at once -> level 1, started as soon as the wind-up is done (MIN)
    CHECK(run(2, 200, &at) == 3035 && at == 20.0f);
    CHECK(g_sa_kind == 1 && g_sa_cur == 0 && g_sa_actor == NOC);
    // 2. released at 40 (< T2) -> level 1 at once; at 60 -> level 2; at 100 -> level 3
    CHECK(run(40, 200, &at) == 3035 && at == 40.0f);
    CHECK(run(60, 200, &at) == 3036 && at == 60.0f);
    CHECK(run(100, 200, &at) == 3037 && at == 100.0f);
    // 3. never released: full charge at MAX
    CHECK(run(-1, 200, &at) == 3037 && at == 120.0f);
    // 4. the selector override is only for that one start: a normal press selects as the engine does
    CHECK(select(NOC, 1) == SEL_REC);
    // 5. no release while still held and before MAX; one start per run; restart of the motion arms it again
    {
        const int sa = g_sa_calls;
        g_held = true;
        for (float t = 0.0f; t < 120.0f; t += 2.0f) { play(NOC, 10106, 94, t); bone_pass(NOC); }
        CHECK(g_sa_calls == sa);
        g_held = false;
        play(NOC, 10106, 94, 118.0f); bone_pass(NOC);
        CHECK(g_sa_calls == sa + 1);
        play(NOC, 10106, 94, 119.0f); bone_pass(NOC);    // still the same run (the fake start changed the script: put it back)
        CHECK(g_sa_calls == sa + 1);
        play(NOC, 10106, 94, 0.0f); bone_pass(NOC);      // started again
        play(NOC, 10106, 94, 22.0f); bone_pass(NOC);
        CHECK(g_sa_calls == sa + 2);
    }
    // 6. another motion, another character: nothing; the hold test is not asked
    {
        const int sa = g_sa_calls, hc = g_held_calls;
        g_held = false;
        play(NOC, 10050, 60, 30.0f); bone_pass(NOC);
        play(BARTZ, 10106, 94, 30.0f); bone_pass(BARTZ);
        CHECK(g_sa_calls == sa && g_held_calls == hc);
    }
    // 7. the menu overlay is resident: nothing runs
    {
        W32(OVL_BASE, 2);
        const int sa = g_sa_calls;
        g_held = false;
        play(NOC, 10106, 94, 0.0f); bone_pass(NOC); play(NOC, 10106, 94, 30.0f); bone_pass(NOC);
        CHECK(g_sa_calls == sa);
        W32(OVL_BASE, 1);
    }
    // 8. no hold test in the overlay: always level 1 at MIN
    g_have_held = false;
    install("34 noc100 9 charge=94,20,50,90,120,3035,3036,3037");
    CHECK(run(-1, 200, &at) == 3035 && at == 20.0f);
    g_have_held = true;
    // 9. a missing release record: not started, no crash
    install("34 noc100 9 charge=94,20,50,90,120,3035,3036,3999");
    CHECK(run(-1, 200, &at) == -1);
    // 9b. two charges (air and ground): each motion releases into its own actions
    install("34 noc100 9 charge=94,32,64,100,130,3035,3036,3037 charge=96,30,60,90,120,3039,3040,3041");
    CHECK(run(2, 200, &at) == 3035 && at == 32.0f);
    CHECK(run(2, 200, &at, 96, 10110) == 3039 && at == 30.0f);
    CHECK(run(70, 200, &at, 96, 10110) == 3040 && at == 70.0f);
    CHECK(run(-1, 200, &at, 96, 10110) == 3041 && at == 120.0f);
    CHECK(run(-1, 200, &at) == 3037 && at == 130.0f);
    install("34 noc100 9 charge=94,32,64,100,130,3035,3036,3037 charge=94,30,60,90,120,3039,3040,3041");   // same motion twice: the second is ignored
    CHECK(run(-1, 200, &at) == 3037 && at == 130.0f);
    // 10. bad tokens: no charge
    install("34 noc100 9 charge=94,20,90,50,120,3035,3036,3037");
    CHECK(!charge_wanted());
    install("34 noc100 9 charge=94,20,50,90,120,3035,3036");
    CHECK(!charge_wanted());
    install("34 noc100 9 charge=94,20,50,90,120,3035,3036,3037,1");
    CHECK(!charge_wanted());
    std::printf("ok test_charge (%d starts, %d hold tests)\n", g_sa_calls, g_held_calls);
    return 0;
}
