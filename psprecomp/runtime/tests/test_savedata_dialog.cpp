// Host unit test for the savedata list dialog state machine + renderer.
// Build/run: runtime/tests/run_savedata_dialog.sh  (g++ only)
#include "../src/hle/psp_savedata_dialog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace psp_sddialog;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<SaveEntry> make(int n) {
    std::vector<SaveEntry> v;
    for (int i = 0; i < n; i++) {
        SaveEntry e;
        e.slot = "Slot0" + std::to_string(i);
        e.dirname = "ULES01505" + e.slot;
        e.title = "Dissidia 012";
        e.savetitle = "Save " + std::to_string(i);
        e.detail = "Chapter " + std::to_string(i) + "\nline2";
        e.mtime = 1700000000 + i * 3600;
        v.push_back(e);
    }
    return v;
}

int main() {
    // Navigation, selection, clamping, held-button arming.
    {
        ListDialog d;
        d.begin(Kind::Load, make(8), true);
        int64_t t = 0;
        // CROSS still held from the game's own menu: must be ignored.
        CHECK(d.update(BTN_CROSS, t) == Result::Running);
        CHECK(d.update(BTN_CROSS, t += 16) == Result::Running);
        CHECK(d.update(0, t += 16) == Result::Running);
        CHECK(d.selected() == 0);
        CHECK(d.update(BTN_DOWN, t += 16) == Result::Running);
        CHECK(d.selected() == 1);
        CHECK(d.update(BTN_DOWN, t += 16) == Result::Running);   // held, before repeat delay
        CHECK(d.selected() == 1);
        CHECK(d.update(BTN_DOWN, t += 400) == Result::Running);  // repeat fires
        CHECK(d.selected() == 2);
        CHECK(d.update(0, t += 16) == Result::Running);
        for (int i = 0; i < 20; i++) { d.update(BTN_DOWN, t += 500); d.update(0, t += 16); }
        CHECK(d.selected() == 7);                                // clamped at end
        for (int i = 0; i < 20; i++) { d.update(BTN_UP, t += 500); d.update(0, t += 16); }
        CHECK(d.selected() == 0);
        d.update(BTN_DOWN, t += 16); d.update(0, t += 16);
        CHECK(d.update(BTN_CROSS, t += 16) == Result::Selected);
        CHECK(d.selected_entry() && d.selected_entry()->slot == "Slot01");
    }
    // Cancel with circle; swapped buttons.
    {
        ListDialog d;
        d.begin(Kind::Save, make(2), true);
        d.update(0, 0);
        CHECK(d.update(BTN_CIRCLE, 16) == Result::Cancelled);
        ListDialog s;
        s.begin(Kind::Save, make(2), false);   // circle confirms
        s.update(0, 0);
        CHECK(s.update(BTN_CROSS, 16) == Result::Cancelled);
        s.update(0, 32);
        CHECK(s.update(BTN_CIRCLE, 48) == Result::Selected);
    }
    // Empty list: confirm does nothing, cancel works.
    {
        ListDialog d;
        d.begin(Kind::Load, {}, true);
        d.update(0, 0);
        CHECK(d.update(BTN_CROSS, 16) == Result::Running);
        d.update(0, 32);
        CHECK(d.update(BTN_CIRCLE, 48) == Result::Cancelled);
    }
    // Render smoke test: draws something, selected row differs from background,
    // overlay hand-off round-trips, ASCII dump for eyeballing with -v.
    {
        ListDialog d;
        auto es = make(7);
        es[2].exists = false;
        d.begin(Kind::Save, es, true);
        Canvas c(2);
        d.render(c);
        CHECK(c.w == SCREEN_W * 2 && c.h == SCREEN_H * 2);
        size_t lit = 0;
        for (uint32_t p : c.px) lit += (p != c.px[0]);
        CHECK(lit > 20000);
        // Every pixel is opaque.
        bool opaque = true;
        for (uint32_t p : c.px) opaque = opaque && (p >> 24) == 255;
        CHECK(opaque);
        // Moving the selection changes the revision and the picture.
        Canvas c2(2);
        uint32_t r0 = d.revision();
        d.update(0, 0);
        d.update(BTN_DOWN, 16);
        CHECK(d.revision() != r0);
        d.render(c2);
        CHECK(c2.px != c.px);
        std::vector<uint32_t> out;
        int ow = 0, oh = 0;
        CHECK(!overlay_fetch(out));
        overlay_publish(c);
        CHECK(overlay_fetch(out, &ow, &oh) && out == c.px && ow == c.w && oh == c.h);
        overlay_clear();
        CHECK(!overlay_fetch(out));
        set_output_scale(9);
        CHECK(output_scale() == MAX_SCALE);
        set_output_scale(1);
        if (getenv("DUMP_DIALOG")) {
            // DUMP_SCALE=n renders at n x (default 3) with a synthetic ICON0.
            int sc = getenv("DUMP_SCALE") ? atoi(getenv("DUMP_SCALE")) : 3;
            auto es2 = make(7);
            es2[2].exists = false;
            for (auto& e : es2) {
                e.size_bytes = 1181696;
                if (!e.exists) continue;
                e.icon_w = 144; e.icon_h = 80;
                for (int y = 0; y < 80; y++)
                    for (int x = 0; x < 144; x++) {
                        uint8_t v = uint8_t(x * 255 / 143), w = uint8_t(y * 255 / 79);
                        e.icon_rgba.insert(e.icon_rgba.end(), {v, w, uint8_t(255 - v), 255});
                    }
            }
            ListDialog d2;
            d2.begin(getenv("DUMP_SAVE") ? Kind::Save : Kind::Load, es2, true, getenv("DUMP_SAVE") ? 2 : 1);
            Canvas cc(sc);
            d2.render(cc);
            FILE* f = fopen(getenv("DUMP_DIALOG"), "wb");
            fprintf(f, "P6\n%d %d\n255\n", cc.w, cc.h);
            for (uint32_t p : cc.px) { fputc(p & 255, f); fputc((p >> 8) & 255, f); fputc((p >> 16) & 255, f); }
            fclose(f);
        }
    }
    // Theme: a synthetic one built from flat-colour images exercises the 9-slice / mirrored-cap
    // sprite path; clearing it restores the built-in look.
    {
        auto flat = [](int w, int h, uint32_t col) {
            auto im = std::make_shared<SdImage>();
            im->w = w; im->h = h; im->px.assign(size_t(w) * size_t(h), col);
            return std::shared_ptr<const SdImage>(im);
        };
        const uint32_t BG = Canvas::rgba(10, 200, 30), BAR = Canvas::rgba(200, 20, 20), TAB = Canvas::rgba(20, 20, 200);
        SdTheme t;
        auto spr = [](std::shared_ptr<const SdImage> im) { SdSprite s; s.img = im; s.w = im->w; s.h = im->h; return s; };
        t.background = spr(flat(48, 27, BG));
        t.tab = spr(flat(24, 12, TAB));
        t.tab.l = 6; t.tab.t = 3; t.tab.b = 3; t.tab.mirror = SdMirror::RightFromLeft;
        t.row_selected = spr(flat(24, 12, BAR));
        t.row_selected.r = 6; t.row_selected.t = 3; t.row_selected.b = 3; t.row_selected.mirror = SdMirror::LeftFromRight;
        CHECK(t.valid());
        SdTheme bad;
        CHECK(!bad.valid());
        set_theme(std::make_shared<const SdTheme>(bad));
        CHECK(theme() == nullptr);                      // an invalid theme is refused
        psp_sddialog_set_theme(&t);
        CHECK(theme() != nullptr);
        ListDialog d;
        d.begin(Kind::Load, make(3), true);
        Canvas c(2);
        d.render(c);
        // Background shows at a corner; the selected bar's body (row 0, mid-screen) is the bar colour.
        CHECK(c.px[size_t(2) * size_t(c.w) + 2] == BG || (c.px[size_t(2) * size_t(c.w) + 2] & 0xFF) < 40);
        const int bar_y = int((70 + 16) * 2);
        CHECK(c.px[size_t(bar_y) * size_t(c.w) + size_t(c.w / 2 + 300)] == BAR);
        // Provider form: called once, lazily.
        static int calls = 0;
        set_theme_provider([]() -> std::shared_ptr<const SdTheme> { ++calls; return nullptr; });
        CHECK(calls == 0);
        CHECK(theme() == nullptr && theme() == nullptr && calls == 1);
        set_theme(nullptr);
        Canvas g(2);
        d.render(g);
        CHECK(g.px != c.px);
    }
    if (g_fail) { printf("test_savedata_dialog: %d FAILURES\n", g_fail); return 1; }
    printf("test_savedata_dialog: all passed\n");
    return 0;
}
