// Host integration test for the sceUtilitySavedata HLE: includes
// psp_hle_utility.cpp directly with the runtime services stubbed, then drives
// savedata_dispatch() on a fake guest memory. Build/run:
//   runtime/tests/run_savedata_hle.sh   (g++ only)
// Covers: encrypted save -> PARAM.SFO (FILE_LIST + SAVEDATA_PARAMS) -> load
// round trip, wrong-key/no-key behaviour, plain legacy saves (fallback), the
// MAKEDATA/READDATA non-secure path, WRITEDATA, and headless LIST selection.
#include "../src/hle/psp_hle_utility.cpp"

#include <sys/mman.h>
#include <cstdlib>

// ---- stubs for the runtime services psp_hle_utility.cpp links against ----
bool psp_png_decode_rgba(const uint8_t*, size_t, std::vector<uint8_t>&, int&, int&, std::string*) { return false; }
std::atomic<bool> g_should_exit{false};
std::atomic<uint32_t> g_host_buttons{0};
std::atomic<uint32_t> g_injected_buttons{0};
std::atomic<int64_t> g_injected_buttons_deadline_ms{0};
static std::string g_root;
std::string psp_path_to_host(const char* p) {
    std::string s = p;
    if (s.rfind("ms0:/", 0) == 0) s = s.substr(5);
    return g_root + "/" + s;
}
int psp_next_uid() { static int u = 1; return u++; }
void psp_audio_push(int, const int16_t*, int, int, int, int) {}
void psp_hle_register(const char*, void (*)(uint8_t*, recomp_context*)) {}
void sched_yield_point() {}
void psp_kernel_check_callbacks(uint8_t*, recomp_context*) {}
static uint32_t g_sdk = 0x03070110;
uint32_t psp_kernel_compiled_sdk_version() { return g_sdk; }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t* R;
static const uint32_t P = 0x08100000, BUF = 0x08200000, OUT = 0x08300000, LIST = 0x08400000;

static void wstr(uint32_t a, const char* s, size_t cap) {
    for (size_t i = 0; i < cap; ++i) psp_mem_write<uint8_t>(R, a + i, i < strlen(s) ? uint8_t(s[i]) : 0);
}
static void setup(uint32_t mode, const char* save, const char* file, uint32_t secver,
                  const uint8_t* key, uint32_t datasize, uint32_t csize = 1536) {
    for (uint32_t i = 0; i < 0x700; i += 4) psp_mem_write<uint32_t>(R, P + i, 0);
    psp_mem_write<uint32_t>(R, P, csize);
    psp_mem_write<uint32_t>(R, P + 8, 1);                 // buttonSwap
    psp_mem_write<uint32_t>(R, P + SAVEDATA_MODE_OFFSET, mode);
    wstr(P + SD_GAMENAME, "ULES01505", 13);
    wstr(P + SD_SAVENAME, save, 20);
    wstr(P + SD_FILENAME, file, 13);
    psp_mem_write<uint32_t>(R, P + SD_DATABUF, BUF);
    psp_mem_write<uint32_t>(R, P + SD_DATABUFSIZE, 0x10000);
    psp_mem_write<uint32_t>(R, P + SD_DATASIZE, datasize);
    wstr(P + SD_SFO, "Test Game", 0x80);
    wstr(P + SD_SFO + 0x80, "Slot title", 0x80);
    wstr(P + SD_SFO + 0x100, "Detail text", 0x400);
    psp_mem_write<uint32_t>(R, P + SD_SECUREVER, secver);
    for (int i = 0; i < 16; ++i) psp_mem_write<uint8_t>(R, P + SD_KEY + i, key ? key[i] : 0);
}
static std::vector<uint8_t> payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = uint8_t(x >> 24); }
    return v;
}
static void put(const std::vector<uint8_t>& v) {
    for (size_t i = 0; i < v.size(); ++i) psp_mem_write<uint8_t>(R, BUF + i, v[i]);
}
static std::vector<uint8_t> get(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = psp_mem_read<uint8_t>(R, BUF + i);
    return v;
}
static void clear_buf() { for (uint32_t i = 0; i < 0x10000; i += 4) psp_mem_write<uint32_t>(R, BUF + i, 0); }
static std::string dirpath(const char* name) { return sd_base() + "/" + name; }

int main() {
    char tmpl[] = "/tmp/sdhleXXXXXX";
    g_root = mkdtemp(tmpl);
    R = static_cast<uint8_t*>(mmap(nullptr, PSP_MEM_SIZE, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK(R != MAP_FAILED);
    uint8_t key[16];
    for (int i = 0; i < 16; ++i) key[i] = uint8_t(0x11 * i + 5);

    // --- secureVersion 0 + key, new SDK => mode 5 ---
    for (size_t n : {1u, 77u, 4000u}) {
        auto data = payload(n, 7 + uint32_t(n));
        put(data);
        setup(3, "GameData00", "DATA.BIN", 0, key, uint32_t(n));
        CHECK(savedata_dispatch(R, P, 3) == 0);
        std::vector<uint8_t> file, sfo;
        CHECK(sd_read_file(dirpath("ULES01505GameData00/DATA.BIN"), file));
        CHECK(file.size() == n + 16);
        CHECK(n < 16 || !std::equal(data.begin(), data.end(), file.begin() + 16));
        CHECK(sd_read_file(dirpath("ULES01505GameData00/PARAM.SFO"), sfo));
        CHECK(sfo.size() % 16 == 0);
        SfoMap m;
        CHECK(sfo_parse(sfo, m));
        CHECK(sfo_get_str(m, "TITLE") == "Test Game");
        CHECK(sfo_get_str(m, "SAVEDATA_TITLE") == "Slot title");
        CHECK(sfo_get_str(m, "SAVEDATA_DIRECTORY") == "ULES01505GameData00");
        CHECK(m["SAVEDATA_PARAMS"].data.size() == 128 && m["SAVEDATA_PARAMS"].data[0] == 0x41);
        CHECK(m["SAVEDATA_FILE_LIST"].data.size() == SD_FILELIST_SIZE);
        CHECK(strcmp(reinterpret_cast<const char*>(m["SAVEDATA_FILE_LIST"].data.data()), "DATA.BIN") == 0);

        // load back
        clear_buf();
        setup(2, "GameData00", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0);
        CHECK(psp_mem_read<uint32_t>(R, P + SD_DATASIZE) == n);
        CHECK(get(n) == data);
        CHECK(sd_cstr(R, P + SD_SFO + 0x80, 0x80) == "Slot title");
        CHECK(psp_mem_read<uint32_t>(R, P + SD_BIND) == 1021);

        // tamper with the file: FILE_LIST hash mismatch is tolerated (PPSSPP), data still loads
        // but a flipped ciphertext bit must change the plaintext
        std::vector<uint8_t> bad = file;
        bad[16 + n / 2] ^= 0x20;
        sd_write_file(dirpath("ULES01505GameData00/DATA.BIN"), bad.data(), bad.size());
        clear_buf();
        setup(2, "GameData00", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0);
        CHECK(get(n) != data);
    }

    // --- missing key with secureVersion 0 => LOAD_PARAM / SAVE_PARAM ---
    {
        auto data = payload(64, 3);
        put(data);
        setup(3, "NoKey", "DATA.BIN", 0, nullptr, 64);
        CHECK(uint32_t(savedata_dispatch(R, P, 3)) == SD_ERR_SAVE_PARAM);
        setup(2, "GameData00", "DATA.BIN", 0, nullptr, 0);
        CHECK(uint32_t(savedata_dispatch(R, P, 2)) == SD_ERR_LOAD_PARAM);
    }

    // --- secureVersion 1 (no key): mode 1 ---
    {
        auto data = payload(500, 11);
        put(data);
        setup(3, "V1", "DATA.BIN", 1, nullptr, 500);
        CHECK(savedata_dispatch(R, P, 3) == 0);
        SfoMap m;
        std::vector<uint8_t> sfo;
        sd_read_file(dirpath("ULES01505V1/PARAM.SFO"), sfo);
        sfo_parse(sfo, m);
        CHECK(m["SAVEDATA_PARAMS"].data[0] == 0x01);
        clear_buf();
        setup(2, "V1", "DATA.BIN", 1, nullptr, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0 && get(500) == data);
    }

    // --- short (pre-secureVersion) param struct => mode 1, key ignored ---
    {
        auto data = payload(100, 21);
        put(data);
        setup(3, "Old", "DATA.BIN", 0, key, 100, 1500);
        CHECK(savedata_dispatch(R, P, 3) == 0);
        clear_buf();
        setup(2, "Old", "DATA.BIN", 0, key, 0, 1500);
        CHECK(savedata_dispatch(R, P, 2) == 0 && get(100) == payload(100, 21));
    }

    // --- legacy plain save (our old format: raw file + SFO without SAVEDATA_PARAMS) loads ---
    {
        std::string d = dirpath("ULES01505Legacy");
        ::mkdir(d.c_str(), 0755);
        auto data = payload(300, 99);
        sd_write_file(d + "/DATA.BIN", data.data(), data.size());
        SfoMap m;
        sfo_set_str(m, "TITLE", "Legacy", 0x80);
        sfo_set_str(m, "SAVEDATA_TITLE", "Old save", 0x80);
        size_t off;
        auto sfo = sfo_write(m, &off);
        sd_write_file(d + "/PARAM.SFO", sfo.data(), sfo.size());
        clear_buf();
        setup(2, "Legacy", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0 && get(300) == data);
        CHECK(sd_cstr(R, P + SD_SFO + 0x80, 0x80) == "Old save");
        // legacy save with *no* SFO at all
        std::string d2 = dirpath("ULES01505Bare");
        ::mkdir(d2.c_str(), 0755);
        sd_write_file(d2 + "/DATA.BIN", data.data(), data.size());
        clear_buf();
        setup(2, "Bare", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0 && get(300) == data);
        // flagged-encrypted SFO but garbage/short file: falls back to plain
        std::string d3 = dirpath("ULES01505Junk");
        ::mkdir(d3.c_str(), 0755);
        uint8_t tiny[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        sd_write_file(d3 + "/DATA.BIN", tiny, 8);
        SfoMap m3;
        sfo_set_str(m3, "TITLE", "J", 0x80);
        sfo_set_bin(m3, "SAVEDATA_PARAMS", std::vector<uint8_t>(128, 0), 128);
        sfo = sfo_write(m3, &off);
        sfo[off] = 0x41;
        sd_write_file(d3 + "/PARAM.SFO", sfo.data(), sfo.size());
        clear_buf();
        setup(2, "Junk", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0);
        CHECK(psp_mem_read<uint32_t>(R, P + SD_DATASIZE) == 8);
    }

    // --- MAKEDATA (14) writes plain, READDATA (15) reads plain, WRITEDATASECURE (18) updates ---
    {
        auto data = payload(48, 5);
        put(data);
        setup(14, "Plain", "DATA.BIN", 0, key, 48);
        CHECK(savedata_dispatch(R, P, 14) == 0);
        std::vector<uint8_t> file;
        sd_read_file(dirpath("ULES01505Plain/DATA.BIN"), file);
        CHECK(file == data);
        clear_buf();
        setup(15, "Plain", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 15) == 0 && get(48) == data);
        // WRITEDATASECURE into the encrypted GameData00 slot, then READDATASECURE
        auto d2 = payload(200, 77);
        put(d2);
        setup(18, "GameData00", "EXTRA.BIN", 0, key, 200);
        for (int i = 0; i < 0x80; i++) psp_mem_write<uint8_t>(R, P + SD_SFO + i, 0);   // no title: keep SFO fields
        CHECK(savedata_dispatch(R, P, 18) == 0);
        clear_buf();
        setup(16, "GameData00", "EXTRA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 16) == 0 && get(200) == d2);
        // original title preserved, two files in the list
        std::vector<uint8_t> sfo;
        sd_read_file(dirpath("ULES01505GameData00/PARAM.SFO"), sfo);
        SfoMap m;
        sfo_parse(sfo, m);
        CHECK(sfo_get_str(m, "TITLE") == "Test Game");
        const auto& fl = m["SAVEDATA_FILE_LIST"].data;
        CHECK(strcmp(reinterpret_cast<const char*>(&fl[0]), "DATA.BIN") == 0);
        CHECK(strcmp(reinterpret_cast<const char*>(&fl[32]), "EXTRA.BIN") == 0);
        CHECK(m["SAVEDATA_PARAMS"].data[0] == 0x41);
        // the original DATA.BIN is still readable after the re-sign
        clear_buf();
        setup(2, "GameData00", "DATA.BIN", 0, key, 0);
        CHECK(savedata_dispatch(R, P, 2) == 0);
        CHECK(psp_mem_read<uint32_t>(R, P + SD_DATASIZE) == 4000);
    }

    // --- headless LISTLOAD: one existing save of a saveNameList is auto-picked ---
    {
        std::string d = dirpath("ULES01505OnlyOne");
        ::mkdir(d.c_str(), 0755);
        auto data = payload(32, 123);
        sd_write_file(d + "/DATA.BIN", data.data(), data.size());
        wstr(LIST, "Missing0", 20);
        wstr(LIST + 20, "OnlyOne", 20);
        wstr(LIST + 40, "", 20);
        setup(4, "", "DATA.BIN", 1, nullptr, 0);
        psp_mem_write<uint32_t>(R, P + SD_SAVENAMELIST, LIST);
        CHECK(sd_resolve_dirname(R, P, 4) == "ULES01505OnlyOne");
        clear_buf();
        CHECK(savedata_dispatch(R, P, 4) == 0 && get(32) == data);
        // collect_entries for the dialog: LISTLOAD hides empty slots, LISTSAVE shows them
        auto le = sd_collect_entries(R, P, true);
        CHECK(le.size() == 1 && le[0].slot == "OnlyOne");
        auto se = sd_collect_entries(R, P, false);
        CHECK(se.size() == 2 && !se[0].exists && se[1].exists);
    }

    if (getenv("KEEP")) { printf("kept %s\n", g_root.c_str()); return g_fail ? 1 : 0; }
    std::string rm = "rm -rf " + g_root;
    if (system(rm.c_str()) != 0) {}
    if (g_fail) { printf("test_savedata_hle: %d FAILURES\n", g_fail); return 1; }
    printf("test_savedata_hle: all passed\n");
    return 0;
}
