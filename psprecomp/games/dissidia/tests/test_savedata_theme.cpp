// Host test for savedata_theme.cpp: builds the Dissidia dialog theme from the real
// disc (skips without it), renders the list dialog with it and checks the result is
// a different, opaque picture from the generic look.
//   tests/run_savedata_theme_test.sh        DUMP_DIR=/path writes PPM frames for eyeballing
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include "hle/psp_hle_font.h"
#include "hle/psp_hle_io.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

std::string dissidia_mods_dir() { return std::getenv("MODS") ? std::getenv("MODS") : ""; }
std::string psp_path_to_host(const char* p) {
    std::string s = p;
    size_t i = s.find(':'); if (i != std::string::npos) s = s.substr(i + 1);
    const char* d0 = std::getenv("PSPRECOMP_DISC0");
    return std::string(d0 ? d0 : "/home/claude/recomp/psprecomp_work/disc0") + s;
}
bool psp_font_host_files(std::string* r, std::string* b) {
    const char* p = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    struct stat st;
    if (::stat(p, &st) != 0) return false;
    *r = p; *b = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
    return true;
}
extern void dissidia_install_savedata_theme();

using namespace psp_sddialog;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void dump(const Canvas& c, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
    for (uint32_t p : c.px) { unsigned char rgb[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)}; fwrite(rgb, 1, 3, f); }
    fclose(f);
}

int main() {
    std::string probe = psp_path_to_host("disc0:/PSP_GAME/USRDIR/DATA/PACKAGE.BIN");
    struct stat st;
    if (::stat(probe.c_str(), &st) != 0) { std::printf("SKIP: no disc (%s)\n", probe.c_str()); return 0; }

    std::vector<SaveEntry> es;
    for (int i = 0; i < 6; i++) {
        SaveEntry e;
        e.slot = "GameData0" + std::to_string(i);
        e.dirname = "ULES01505" + e.slot;
        e.title = "DISSIDIA 012 duodecim";
        e.savetitle = "GAME DATA";
        e.detail = "PLAYER: Vaan   Time: 1:41:2" + std::to_string(i);
        e.mtime = 1760630400 + i * 86400;
        e.size_bytes = 280000;
        if (i == 5) { e.exists = false; }
        es.push_back(e);
    }
    ListDialog d;
    d.begin(Kind::Load, es, true, 1);
    Canvas generic(2);
    set_theme(nullptr);
    d.render(generic);

    dissidia_install_savedata_theme();   // provider: built on first render
    Canvas themed(2);
    d.render(themed);
    CHECK(theme() != nullptr);
    CHECK(themed.px != generic.px);
    bool opaque = true;
    for (uint32_t p : themed.px) opaque = opaque && (p >> 24) == 255;
    CHECK(opaque);

    const char* dir = std::getenv("DUMP_DIR");
    if (dir) {
        for (int sc : {1, 2, 4}) {
            Canvas c(sc);
            d.render(c);
            dump(c, std::string(dir) + "/theme_load_x" + std::to_string(sc) + ".ppm");
        }
        ListDialog s;
        s.begin(Kind::Save, es, false, 5);
        Canvas c(2);
        s.render(c);
        dump(c, std::string(dir) + "/theme_save_x2.ppm");
        ListDialog e;
        e.begin(Kind::Load, {}, true);
        Canvas c0(2);
        e.render(c0);
        dump(c0, std::string(dir) + "/theme_empty_x2.ppm");
        set_theme(nullptr);
        Canvas g(2);
        d.render(g);
        dump(g, std::string(dir) + "/generic_load_x2.ppm");
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "test_savedata_theme: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
