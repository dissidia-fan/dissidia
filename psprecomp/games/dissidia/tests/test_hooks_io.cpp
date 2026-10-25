// Integration test of hooks_io.cpp (classify_open + overlay_read) against the
// real PACKAGE.BIN / PACKAGE_INFO.BIN, with the IO HLE replaced by stubs.
// Identity override: dropping the disc's own bytes for an entry (with one byte
// flipped) must read back exactly the disc bytes except that byte, through the
// same lseek+read pattern the game uses, and must leave other entries alone.
#include "../runtime/package_mods.h"
#include "hle/psp_hle_io.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static PspIoPolicy g_policy;
static std::string g_disc0, g_ms0;
void psp_io_set_policy(const PspIoPolicy& p) { g_policy = p; }
std::string psp_io_ms0_host_path() { return g_ms0; }
std::string psp_path_to_host(const char* p) {
    std::string s = p;
    size_t i = s.find(':'); if (i != std::string::npos) s = s.substr(i + 1);
    return g_disc0 + s;
}
extern void dissidia_install_mod_io();
// roster.cpp is not linked into this test: no overlay is ever a roster target here.
bool dissidia_roster_enabled() { return false; }
uint8_t* g_dbg_rdram = nullptr;
void psp_dump_func_ring(const char*) {}
uint32_t dissidia_model_budget_request(uint32_t) { return 0; }
uint32_t dissidia_model_budget() { return 0; }
void psp_texpack_set_dir_provider(std::string (*)()) {}
void psp_fx_set_dir_provider(std::string (*)()) {}
int dissidia_roster_overlay_of(const char*) { return -1; }
void dissidia_roster_patch_overlay(int, int64_t, uint32_t, uint8_t*) {}
#include "../runtime/hooks_io.cpp"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    const char* d0 = std::getenv("PSPRECOMP_DISC0");
    g_disc0 = d0 ? d0 : "/home/claude/recomp/psprecomp_work/disc0";
    std::string data = g_disc0 + "/PSP_GAME/USRDIR/DATA/";
    int pfd = ::open((data + "PACKAGE.BIN").c_str(), O_RDONLY);
    if (pfd < 0) { std::printf("SKIP: no PACKAGE.BIN\n"); return 0; }

    char tmpl[] = "/tmp/hooksio_XXXXXX";
    std::string root = mkdtemp(tmpl);
    g_ms0 = root + "/ms0";
    setenv("PSPRECOMP_MODS", (root + "/mods").c_str(), 1);

    // read the disc's own copy of an entry
    std::string asset = "obj/p_fiv100_1p.gmo";
    FILE* f = std::fopen((data + "PACKAGE_INFO.BIN").c_str(), "rb");
    std::vector<uint8_t> ib(200000); size_t n = std::fread(ib.data(), 1, ib.size(), f); ib.resize(n); std::fclose(f);
    dissidia::PackageIndex idx; std::string err; CHECK(idx.parse(ib.data(), ib.size(), &err));
    const dissidia::IndexEntry* e = idx.find(dissidia::package_crc(asset));
    CHECK(e != nullptr);
    std::vector<uint8_t> disc(e->size());
    CHECK(::pread(pfd, disc.data(), disc.size(), (off_t)e->sector * 2048) == (ssize_t)disc.size());
    std::vector<uint8_t> mod = disc; mod[12345] ^= 0xFF;
    ::mkdir(root.c_str(), 0755); ::mkdir((root + "/mods").c_str(), 0755); ::mkdir((root + "/mods/obj").c_str(), 0755);
    FILE* o = std::fopen((root + "/mods/" + asset).c_str(), "wb"); std::fwrite(mod.data(), 1, mod.size(), o); std::fclose(o);

    dissidia_install_mod_io();
    CHECK(g_policy.classify_open && g_policy.overlay_read);
    CHECK(g_policy.classify_open("DISC0:/PSP_GAME/USRDIR/DATA/OVL_MENU_APP.ELF") == 0);
    int ti = g_policy.classify_open("DISC0:/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN");
    int tp = g_policy.classify_open("DISC0:/PSP_GAME/USRDIR/DATA/PACKAGE.BIN");
    CHECK(ti != 0 && tp != 0 && ti != tp);

    // index as the game reads it: same size, only this entry's size differs (same here)
    std::vector<uint8_t> got(ib.size() + 100); uint32_t nr = 0;
    CHECK(g_policy.overlay_read(ti, -1, 0, (uint32_t)got.size(), got.data(), &nr));
    CHECK(nr == ib.size() && std::equal(ib.begin(), ib.end(), got.begin()));   // same-size override: index unchanged

    // game-style chunked read of the entry through PACKAGE.BIN
    std::vector<uint8_t> out; std::vector<uint8_t> buf(70000);
    for (uint32_t pos = 0; pos < e->size();) {
        uint32_t want = std::min<uint32_t>(e->size() - pos, (pos % 3 == 0) ? 572 : 65536);
        uint32_t k = 0;
        bool hit = g_policy.overlay_read(tp, pfd, (int64_t)e->sector * 2048 + pos, want, buf.data(), &k);
        CHECK(hit);
        out.insert(out.end(), buf.begin(), buf.begin() + k); pos += k; if (!k) break;
    }
    CHECK(out == mod);
    // another entry untouched: hook declines
    const dissidia::IndexEntry& other = idx.entries[5];
    CHECK(!g_policy.overlay_read(tp, pfd, (int64_t)other.sector * 2048, 128, buf.data(), &nr) || other.sector == e->sector);

    std::string rm = "rm -rf " + root; (void)!std::system(rm.c_str());
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
