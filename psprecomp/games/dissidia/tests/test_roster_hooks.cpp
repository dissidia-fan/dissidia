// Host test for roster.cpp: the ablt relocation, the extras' private save records and their sidecar files, driven
// against a fake guest RAM, fake originals and fake sceUtilitySavedata stubs (no runtime build, no game run, no game data).
//   tests/run_roster_hooks_test.sh
// roster.cpp is included so the test can reach its internal state.
#include "../runtime/roster.cpp"
#include <map>
#include <cstdlib>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::vector<uint8_t> g_ram(0x08000000u);
std::string dissidia_mods_dir() { return "/nonexistent"; }
FuncPtr ovl_menu_lookup(uint32_t) { return nullptr; }
FuncPtr ovl_battle_lookup(uint32_t) { return nullptr; }
thread_local uint32_t g_func_ring[32];
thread_local uint32_t g_func_ring_pos;
void psp_dump_func_ring(const char*) {}
static std::map<uint32_t, FuncPtr> g_hooks;
static std::string g_ms0;
std::string psp_path_to_host(const char* p) { std::string s(p); return g_ms0 + "/" + s.substr(5); }
uint32_t psp_hle_stub_addr_for_nid(uint32_t nid) { return nid == 0x50C4CD57u ? 0x089C5858u : nid == 0x8874DBE0u ? 0x089C5868u : nid == 0x9790B33Cu ? 0x089C5878u : 0; }
void psp_dispatch_register(uint32_t a, FuncPtr f) { g_hooks[a] = f; }
static uint32_t g_bump = 0x0A800000u;   // host heap blocks lie above the PSP user memory
uint32_t psp_kmem_bump_alloc_named(uint32_t size, const char*) { uint32_t a = g_bump; g_bump += (size + 0xFFu) & ~0xFFu; return a; }
uint8_t* R(uint32_t a) { return g_ram.data() + (a & 0x07FFFFFFu); }
void W16(uint32_t a, uint16_t v) { std::memcpy(R(a), &v, 2); }
uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, R(a), 2); return v; }
uint32_t R32_(uint32_t a) { uint32_t v; std::memcpy(&v, R(a), 4); return v; }
void W32(uint32_t a, uint32_t v) { std::memcpy(R(a), &v, 4); }

// originals
constexpr uint32_t BARTZ = 0x09100000u;
static int g_saverec_calls = 0, g_ablt_calls = 0; static int32_t g_ablt_a1 = -1, g_ablt_a2 = -1;
void orig_saverec(uint8_t*, recomp_context* ctx) { g_saverec_calls++; CHECK(ctx->r[5] == 9 || ctx->r[5] == 21); ctx->r[2] = ctx->r[5] == 9 ? BARTZ : BARTZ + 0x1000; }
void orig_saveidx(uint8_t*, recomp_context* ctx) { ctx->r[2] = 8; }
void orig_ablt(uint8_t*, recomp_context* ctx) { g_ablt_calls++; g_ablt_a1 = ctx->r[5]; g_ablt_a2 = ctx->r[6]; }
void orig_other(uint8_t*, recomp_context*) {}
static int g_hle_status = 3, g_party_inits = 0;
void hle_init(uint8_t*, recomp_context* ctx) { ctx->r[2] = 0; }
void hle_status(uint8_t*, recomp_context* ctx) { ctx->r[2] = g_hle_status; }
void hle_shutdown(uint8_t*, recomp_context* ctx) { ctx->r[2] = 0; }
void orig_party_init(uint8_t*, recomp_context*) { g_party_inits++; }
FuncPtr psp_dispatch_probe_lookup(uint32_t a) {
    if (a == 0x088CB164u) return orig_saverec;
    if (a == 0x088CB3B4u) return orig_saveidx;
    if (a == 0x088B1CC0u) return orig_ablt;
    if (a == 0x089C5858u) return hle_init;
    if (a == 0x089C5868u) return hle_status;
    if (a == 0x089C5878u) return hle_shutdown;
    if (a == 0x088CB210u) return orig_party_init;
    return orig_other;
}

int main() {
    setenv("PSPRECOMP_ROSTER", "34 noc100 9;33 viv100 21", 1);
    dissidia_roster_init(g_ram.data());
    dissidia_roster_grid_install();
    dissidia_wpnp_install();
    CHECK(g_hooks.count(0x088CB164u) && g_hooks.count(0x088B1CC0u));
    recomp_context ctx{};

    // ablt: retail-sized table runs the original unchanged
    const uint32_t SRC = 0x09200000u;
    W32(SRC + 8, 790);
    ctx = recomp_context{}; ctx.r[4] = SRC; ctx.r[5] = 16 + 790 * 40; ctx.r[6] = 0x09300000;
    g_hooks[0x088B1CC0u](g_ram.data(), &ctx);
    CHECK(g_ablt_calls == 1 && g_ablt_a1 == 16 + 790 * 40 && R32_(0x089D7290u) == 0);
    // 824 records: copied to a host block, the original only does the ability.bin part
    W32(SRC + 8, 824); W16(SRC + 16, 0x0101); W16(SRC + 16 + 823 * 40, 0x40AA);
    ctx = recomp_context{}; ctx.r[4] = SRC; ctx.r[5] = 16 + 824 * 40; ctx.r[6] = 0x09300000;
    g_hooks[0x088B1CC0u](g_ram.data(), &ctx);
    const uint32_t blk = R32_(0x089D7290u);
    CHECK(g_ablt_calls == 2 && g_ablt_a1 == 0 && g_ablt_a2 == 0x09300000 && blk >= 0x0A000000u);
    CHECK(R32_(0x089D7294u) == blk + 16 && R32_(blk + 8) == 824 && R16(blk + 16) == 0x0101 && R16(blk + 16 + 823 * 40) == 0x40AA);
    std::printf("ablt hook ok (block %08X)\n", blk);

    // Bartz's record
    uint8_t* b = R(BARTZ);
    b[0] = 9; b[1] = 1;
    uint8_t* ab = b + 0x10;
    ab[0x762] = 9; ab[0x763] = 0;
    for (int s = 0; s < 5; s++) { ab[0xC6 * s + 0xB6] = 9; ab[0xC6 * s + 0] = 0x0C; ab[0xC6 * s + 0x5A] = 0x25; }
    for (int i = 0; i < 150; i++) W16(BARTZ + 0x10 + 0x3DE + 6 * i, 0xFFFF);
    W16(BARTZ + 0x10 + 0x3DE + 0, 0x090C); W16(BARTZ + 0x10 + 0x3DE + 6, 0x4025); W16(BARTZ + 0x10 + 0x3DE + 12, 0x0902);
    W16(BARTZ + 0x10 + 0x3DE + 14, 120);    // AP of 0x0902

    auto call = [&](uint32_t cid) { ctx = recomp_context{}; ctx.r[4] = 0x09400000; ctx.r[5] = cid; g_hooks[0x088CB164u](g_ram.data(), &ctx); return static_cast<uint32_t>(ctx.r[2]); };
    uint32_t n1 = call(34);
    CHECK(n1 >= 0x0A000000u && n1 != BARTZ);
    uint8_t* n = R(n1);
    CHECK(n[0] == 34 && n[1] == 1 && n[0x10 + 0x762] == 34);
    for (int s = 0; s < 5; s++) CHECK(n[0x10 + 0xC6 * s + 0xB6] == 34 && n[0x10 + 0xC6 * s] == 0x0C && n[0x10 + 0xC6 * s + 0x5A] == 0x25);
    CHECK(R16(n1 + 0x10 + 0x3DE) == 0x220C && R16(n1 + 0x10 + 0x3DE + 6) == 0x4025 && R16(n1 + 0x10 + 0x3DE + 12) == 0x2202);
    CHECK(R16(n1 + 0x10 + 0x3DE + 14) == 120 && R16(n1 + 0x10 + 0x3DE + 18) == 0xFFFF);
    CHECK(b[0] == 9 && R16(BARTZ + 0x10 + 0x3DE) == 0x090C);   // like's record untouched
    // Bartz levels up while Noctis is untouched: Noctis follows
    b[1] = 5;
    uint32_t n2 = call(34);
    CHECK(n2 == n1 && n[1] == 5);
    // the game writes Noctis's record (Abilities screen commit): kept from now on
    n[0x10 + 0x763] = 2; n[0x10 + 0xC6 * 2 + 3] = 0x01;
    b[1] = 7;
    uint32_t n3 = call(34);
    CHECK(n3 == n1 && n[0x10 + 0x763] == 2 && n[0x10 + 0xC6 * 2 + 3] == 0x01 && n[1] == 5);
    call(34);
    CHECK(n[0x10 + 0x763] == 2 && n[1] == 5);
    // charId 33 has its own slot
    uint32_t v = call(33);
    CHECK(v == n1 - 0x778u && R(v)[0] == 33);
    (void)v;
    std::printf("saverec hook ok (charId 34 at %08X, 33 at %08X)\n", n1, v);
    // a retail charId goes straight to the original
    ctx = recomp_context{}; ctx.r[5] = 9; g_hooks[0x088CB164u](g_ram.data(), &ctx);
    CHECK(static_cast<uint32_t>(ctx.r[2]) == BARTZ);

    // ---------------- persistence ----------------
    std::string tmpl = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/roster_ms0_XXXXXX";
    std::vector<char> tb(tmpl.begin(), tmpl.end()); tb.push_back(0);
    CHECK(mkdtemp(tb.data()) != nullptr);
    g_ms0 = tb.data();
    const std::string SD = g_ms0 + "/PSP/SAVEDATA";
    auto put_file = [](const std::string& path, const std::vector<uint8_t>& d) {
        for (size_t i = 1; i < path.size(); i++) if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
        std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(d.data()), d.size());
    };
    auto get_file = [](const std::string& path) { std::vector<uint8_t> v; read_host_file(path, &v); return v; };
    std::vector<uint8_t> save0(kMainSaveBytes), save1(kMainSaveBytes);
    for (size_t i = 0; i < save0.size(); i++) { save0[i] = uint8_t(i * 7); save1[i] = uint8_t(i * 11); }
    put_file(SD + "/ULES01505GameData00/DISSIDIA.BIN", save0);
    const uint32_t OBJ = 0x09600000u, P = OBJ + 0x1E0u, CODE = 0x09500000u, NAMES = 0x09500100u;
    W32(0x089D6010u, CODE); std::memcpy(R(CODE), "ULES01505", 10);
    W32(0x089D712Cu, OBJ);
    auto param = [&](uint32_t mode, const char* save, int32_t result, uint32_t size = kMainSaveBytes, const char* game = "ULES01505", uint32_t names = 0) {
        std::memset(R(P), 0, 0x600);
        W32(P + 0x30, mode); W32(P + 0x1C, static_cast<uint32_t>(result)); W32(P + 0x7C, size); W32(P + 0x60, names);
        std::strcpy(reinterpret_cast<char*>(R(P + 0x3C)), game); std::strcpy(reinterpret_cast<char*>(R(P + 0x4C)), save);
        std::strcpy(reinterpret_cast<char*>(R(P + 0x64)), "DISSIDIA.BIN");
    };
    auto run_sd = [&](bool via_shutdown = false) {
        ctx = recomp_context{}; ctx.r[4] = P; g_hooks[0x089C5858u](g_ram.data(), &ctx); CHECK(ctx.r[2] == 0);
        g_hle_status = via_shutdown ? 2 : 3;
        ctx = recomp_context{}; g_hooks[0x089C5868u](g_ram.data(), &ctx);
        ctx = recomp_context{}; g_hooks[0x089C5868u](g_ram.data(), &ctx);
        ctx = recomp_context{}; g_hooks[0x089C5878u](g_ram.data(), &ctx);
    };
    const uint32_t noc = n1;              // charId 34's private record (OWN since the commit above)
    const std::string SIDE0 = g_ms0 + "/PSP/ROSTER/ULES01505GameData00_DISSIDIA.BIN.roster";
    const std::string SIDE1 = g_ms0 + "/PSP/ROSTER/ULES01505GameData01_DISSIDIA.BIN.roster";
    CHECK(g_saverec_state[34 - kFirstNew] == REC_OWN && g_saverec_state[33 - kFirstNew] == REC_FOLLOW);
    // 1. a save writes the sidecar with charId 34 only (33 still follows its like)
    param(1, "GameData00", 0); run_sd();
    std::vector<uint8_t> side = get_file(SIDE0);
    CHECK(side.size() == 0x60 + (8 + 0x778) + 8 && side[0x60] == 34 && side[0x61] == 9);
    std::vector<uint8_t> saved(R(noc), R(noc) + 0x778);
    CHECK(std::memcmp(side.data() + 0x68, saved.data(), 0x778) == 0);
    std::vector<SidecarRecord> recs;
    CHECK(sidecar_decode(side, fnv1a64(save0.data(), save0.size()), kMainSaveBytes, &recs).empty() && recs.size() == 1);
    // 2. "restart": nothing kept in memory, the record follows Bartz again
    reset_extra_records(); std::memset(R(noc), 0, 0x778);
    b[1] = 42; call(34);
    CHECK(R(noc)[1] == 42 && R(noc)[0x10 + 0x763] == 0);
    // ... and loading the save brings back the saved record (mode 0 at boot), which then stays the character's own
    param(0, "GameData00", 0); run_sd();
    CHECK(std::memcmp(R(noc), saved.data(), 0x778) == 0 && g_saverec_state[34 - kFirstNew] == REC_OWN);
    b[1] = 50; call(34);
    CHECK(std::memcmp(R(noc), saved.data(), 0x778) == 0);
    R(BARTZ + 0x1000)[1] = 9; CHECK(R(call(33))[1] == 9);   // charId 33 is not in the sidecar: follows its like (21)
    // 3. the save file changed without the sidecar (another emulator, an older build): ignored, follow the like
    std::vector<uint8_t> other = save0; other[100] ^= 1; put_file(SD + "/ULES01505GameData00/DISSIDIA.BIN", other);
    param(4, "GameData00", 0); run_sd();
    CHECK(g_saverec_state[34 - kFirstNew] == REC_FOLLOW); call(34); CHECK(R(noc)[1] == 50 && R(noc)[0] == 34);
    put_file(SD + "/ULES01505GameData00/DISSIDIA.BIN", save0);
    // 4. a damaged sidecar: ignored
    side[0x70] ^= 0xFF; put_file(SIDE0, side);
    param(0, "GameData00", 0); run_sd();
    CHECK(g_saverec_state[34 - kFirstNew] == REC_FOLLOW);
    side[0x70] ^= 0xFF; put_file(SIDE0, side);
    std::vector<uint8_t> trunc(side.begin(), side.end() - 9); put_file(SIDE0, trunc);
    param(0, "GameData00", 0); run_sd(); CHECK(g_saverec_state[34 - kFirstNew] == REC_FOLLOW);
    put_file(SIDE0, side);
    param(0, "GameData00", 0); run_sd(true);          // the shutdown fallback handles it too
    CHECK(g_saverec_state[34 - kFirstNew] == REC_OWN && std::memcmp(R(noc), saved.data(), 0x778) == 0);
    // 5. two slots: a list save to slot 01 (the dialog-less path picks the newest data file of the name list)
    R(noc)[1] = 77;                                     // Noctis levels up in this session
    std::strcpy(reinterpret_cast<char*>(R(NAMES)), "GameData00"); std::strcpy(reinterpret_cast<char*>(R(NAMES + 20)), "GameData01");
    R(NAMES + 40)[0] = 0;
    ::sleep(1); put_file(SD + "/ULES01505GameData01/DISSIDIA.BIN", save1);
    param(5, "", 0, kMainSaveBytes, "ULES01505", NAMES); run_sd();
    std::vector<uint8_t> side1 = get_file(SIDE1);
    CHECK(side1.size() == side.size() && side1[0x60 + 8 + 1] == 77);
    CHECK(get_file(SIDE0) == side);                    // slot 00's sidecar untouched
    param(4, "GameData00", 0); run_sd();                // load slot 00: level as saved there
    CHECK(R(noc)[1] == saved[1]);
    param(4, "GameData01", 0); run_sd();
    CHECK(R(noc)[1] == 77);
    // 6. New Game (FUN_088CB210 on the live save object): follow the like again; a scratch object does not count
    ctx = recomp_context{}; ctx.r[4] = 0x09700000; g_hooks[0x088CB210u](g_ram.data(), &ctx);
    CHECK(g_saverec_state[34 - kFirstNew] == REC_OWN);
    ctx = recomp_context{}; ctx.r[4] = OBJ + 0xF58; g_hooks[0x088CB210u](g_ram.data(), &ctx);
    CHECK(g_party_inits == 2 && g_saverec_state[34 - kFirstNew] == REC_FOLLOW);
    // a new game saved into slot 00: its sidecar now has no extra record
    put_file(SD + "/ULES01505GameData00/DISSIDIA.BIN", save1);
    param(3, "GameData00", 0); run_sd();
    std::vector<uint8_t> side0b = get_file(SIDE0);
    CHECK(side0b.size() == 0x68 && sidecar_decode(side0b, fnv1a64(save1.data(), save1.size()), kMainSaveBytes, &recs).empty() && recs.empty());
    // 7. ignored: a failed load, the earlier game's save (data import), a listing
    const std::vector<uint8_t> before = get_file(SIDE1);
    R(noc)[1] = 5; g_saverec_state[34 - kFirstNew] = REC_OWN;
    param(4, "GameData01", 1); run_sd();                                   // cancelled
    param(4, "GameData01", 0, 0x1234, "ULUS10437"); run_sd();              // import
    param(11, "GameData01", 0); run_sd();                                  // LIST
    param(5, "GameData01", int32_t(0x80110385)); run_sd();                 // save failed
    CHECK(R(noc)[1] == 5 && g_saverec_state[34 - kFirstNew] == REC_OWN && get_file(SIDE1) == before);
    std::printf("persistence ok (%s)\n", g_ms0.c_str());
    // 8. the extras know all of their own abilities: ids owned by the charId fill free learned rows, once each
    {
        reset_extra_records();
        const uint32_t recs = R32_(0x089D7294u);
        auto own = [&](int r, uint16_t id, uint8_t owner) { W16(recs + 40 * r, id); R(recs + 40 * r)[0x1B] = owner; };
        own(5, 0x2208, 34); own(6, 0x220C, 34); own(7, 0x2105, 33);
        const uint32_t nl = call(34);
        const uint32_t L = nl + 0x10 + 0x3DE;
        CHECK(R16(L) == 0x220C && R16(L + 6) == 0x4025 && R16(L + 12) == 0x2202);        // the like's learned rows kept
        CHECK(R16(L + 18) == 0x2208 && R16(L + 24) == 0xFFFF);                             // 0x220C not added twice
        call(34); CHECK(R16(L + 18) == 0x2208 && R16(L + 24) == 0xFFFF);                   // idempotent, still followed
        CHECK(g_saverec_state[34 - kFirstNew] == REC_FOLLOW);
        for (int i = 0; i < 150; i++) W16(BARTZ + 0x1000 + 0x10 + 0x3DE + 6 * i, 0xFFFF);  // Shantotto's list: empty
        const uint32_t vl = call(33) + 0x10 + 0x3DE;
        CHECK(R16(vl) == 0x2105 && R16(vl + 6) == 0xFFFF);                                  // only its own ids
        own(5, 0, 0); own(6, 0, 0); own(7, 0, 0);
        std::printf("learn-all ok\n");
    }
    std::printf("ok test_roster_hooks\n");
    return 0;
}
