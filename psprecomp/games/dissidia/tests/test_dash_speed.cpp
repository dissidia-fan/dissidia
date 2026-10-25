// Host test for roster.cpp's dash speed hook (roster.txt dash=F): fake guest RAM, fake battle-overlay original.
//   g++ -std=c++17 -I<repo>/runtime/include -I<repo>/work/output/include -I<repo>/games/dissidia/runtime
//       -o /tmp/test_dash_speed test_dash_speed.cpp; /tmp/test_dash_speed
// (copy next to games/dissidia/tests/test_roster_hooks.cpp; it includes ../runtime/roster.cpp the same way)
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
static int g_batt_calls = 0;
void orig_dash_move(uint8_t* rdram, recomp_context* ctx) {   // writes (1, 2, 3, 1) to out, clobbers the argument registers
    g_batt_calls++;
    const float v[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    std::memcpy(rdram + (static_cast<uint32_t>(ctx->r[4]) & 0x07FFFFFFu), v, sizeof v);
    ctx->r[4] = ctx->r[5] = ctx->r[6] = 0;
}
FuncPtr ovl_battle_lookup(uint32_t a) { return a == 0x08A50CD0u ? orig_dash_move : nullptr; }
thread_local uint32_t g_func_ring[32];
thread_local uint32_t g_func_ring_pos;
void psp_dump_func_ring(const char*) {}
static std::map<uint32_t, FuncPtr> g_hooks;
std::string psp_path_to_host(const char* p) { return p; }
uint32_t psp_hle_stub_addr_for_nid(uint32_t) { return 0; }
void psp_dispatch_register(uint32_t a, FuncPtr f) { g_hooks[a] = f; }
uint32_t psp_kmem_bump_alloc_named(uint32_t, const char*) { return 0; }
void orig_other(uint8_t*, recomp_context*) {}
FuncPtr psp_dispatch_probe_lookup(uint32_t) { return orig_other; }
uint8_t* R(uint32_t a) { return g_ram.data() + (a & 0x07FFFFFFu); }
void W32(uint32_t a, uint32_t v) { std::memcpy(R(a), &v, 4); }
float RF(uint32_t a) { float f; std::memcpy(&f, R(a), 4); return f; }

void run(uint32_t out, uint32_t actor) {
    recomp_context ctx{};
    ctx.r[4] = static_cast<int32_t>(out); ctx.r[5] = 0x09300000; ctx.r[6] = static_cast<int32_t>(actor);
    g_hooks.at(0x08A50CD0u)(g_ram.data(), &ctx);
}

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9 nm=Noctis dash=1.75;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(g_hooks.count(0x08A50CD0u));
    const uint32_t OUT = 0x09200000u, NOC = 0x09210000u, BARTZ = 0x09220000u, INFO_N = 0x09230000u, INFO_B = 0x09230100u;
    W32(0x089DD4C0u, 1);                                  // battle overlay resident
    W32(NOC + 0x2F8u, INFO_N); R(INFO_N)[6] = 34;
    W32(BARTZ + 0x2F8u, INFO_B); R(INFO_B)[6] = 9;
    run(OUT, NOC);
    CHECK(g_batt_calls == 1);
    CHECK(RF(OUT) == 1.75f && RF(OUT + 4) == 3.5f && RF(OUT + 8) == 5.25f && RF(OUT + 12) == 1.0f);
    run(OUT, BARTZ);                                      // no factor: untouched
    CHECK(g_batt_calls == 2 && RF(OUT) == 1.0f && RF(OUT + 8) == 3.0f);
    W32(0x089DD4C0u, 2);                                  // menu overlay resident: the battle function is not run
    run(OUT, NOC);
    CHECK(g_batt_calls == 2);
    // no dash= token anywhere: no hook at all
    g_entries.clear(); g_hooks.clear();
    setenv("PSPRECOMP_ROSTER", "34 noc100 9", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    CHECK(!g_hooks.count(0x08A50CD0u));
    std::printf("ok test_dash_speed\n");
    return 0;
}
