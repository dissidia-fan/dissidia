// Savedata list dialog (sceUtilitySavedata LISTLOAD / LISTSAVE).
//
// Pure logic + a software renderer (FreeType text, SDF shapes): no dependency
// on the runtime beyond FreeType, so it is unit-testable on the host
// (tests/test_savedata_dialog.cpp). Without a theme the look is a generic
// recreation of a PSP-menu style (lavender background, bevelled white panel,
// navy header, royal-blue chevron selection bar, pointing hand). A game
// module can instead install an SdTheme (below) built from the game's own UI
// textures (see games/dissidia/runtime/savedata_theme.cpp); the same layout
// and scaling then draw those sprites. The layout is in
// 480x272 design units and the canvas is rendered at an integer multiple of
// that (the output resolution) so it stays crisp when upscaled. The HLE
// (psp_hle_utility.cpp) feeds it the save directories it found, polls the
// pad each Update/GetStatus and, when the user picks a row or cancels,
// completes the utility call. The rendered frame is published through
// overlay_publish()/overlay_fetch(); the GL presenter
// (psp_ge_draw.cpp present_blit) blits it over the game frame while a
// dialog is active.
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace psp_sddialog {

constexpr int SCREEN_W = 480;   // design units
constexpr int SCREEN_H = 272;
constexpr int MAX_SCALE = 4;

// The presenter publishes the integer scale (1..MAX_SCALE) the overlay should
// be rendered at (output width / 480, rounded up); the HLE reads it each frame.
void set_output_scale(int s);
int output_scale();

// PSP pad bits (sceCtrl).
constexpr uint32_t BTN_UP = 0x0010, BTN_RIGHT = 0x0020, BTN_DOWN = 0x0040,
                   BTN_LEFT = 0x0080, BTN_LTRIGGER = 0x0100,
                   BTN_RTRIGGER = 0x0200, BTN_CIRCLE = 0x2000,
                   BTN_CROSS = 0x4000;

struct SaveEntry {
    std::string dirname;     // full directory name, e.g. ULES01505GameData00
    std::string slot;        // the saveName part ("GameData00")
    std::string title;       // PARAM.SFO TITLE
    std::string savetitle;   // PARAM.SFO SAVEDATA_TITLE
    std::string detail;      // PARAM.SFO SAVEDATA_DETAIL (first line shown)
    time_t mtime = 0;
    uint64_t size_bytes = 0; // total size of the save directory's files
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> icon_rgba;  // decoded ICON0.PNG (RGBA8), empty = none
    bool exists = true;      // false = empty slot ("New Data")
};

// ---------------------------------------------------------------------------
// Theme: the game's own UI art, supplied by the game module.
// ---------------------------------------------------------------------------
// Straight-alpha RGBA8 image (uint32 = 0xAABBGGRR on little endian).
struct SdImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

enum class SdMirror { None, LeftFromRight, RightFromLeft };

// A rectangle of an image drawn with 9-slice scaling. (x,y,w,h) is the source
// rectangle; l/r/t/b are the fixed border widths in source pixels (all zero =
// the whole rectangle is stretched). With `mirror` set, only one end cap is
// stored in the image and the other is its horizontal mirror (the game draws
// its chevron bars that way). Source pixels map 1:1 to design units
// (480x272); the canvas scale multiplies both.
struct SdSprite {
    std::shared_ptr<const SdImage> img;
    int x = 0, y = 0, w = 0, h = 0;
    int l = 0, r = 0, t = 0, b = 0;
    SdMirror mirror = SdMirror::None;
    bool valid() const { return img && w > 0 && h > 0; }
};

struct SdTheme {
    SdSprite background;                // stretched over the whole 480x272 screen
    SdSprite tab;                       // header tab and bottom hint bar (horizontal 3-slice)
    SdSprite row_selected;              // royal-blue chevron bar
    SdSprite row_normal;                // silver chevron bar
    SdSprite cursor;                    // pointing hand, drawn at native size
    SdSprite corner[4];                 // frame corner ornaments: TL, TR, BL, BR
    SdSprite glyph_cross, glyph_circle;     // face-button glyphs
    SdSprite dpad_up, dpad_down;
    // Colours (RGBA8, same packing as SdImage).
    uint32_t text = 0xFF56362E;             // row title
    uint32_t text_dim = 0xFF8C6460;         // row detail
    uint32_t text_selected = 0xFFFFFFFF;
    uint32_t text_selected_dim = 0xFFFFE6D6;
    uint32_t hint_text = 0xFFFFFFFF;
    uint32_t title = 0xFFFFF4F0;            // screen title fill
    uint32_t title_edge = 0xFF5A2E1C;       // and its outline
    uint32_t panel_fill = 0xD8EEE4DC;
    uint32_t panel_edge = 0xFFFFFFFF;
    uint32_t panel_text = 0xFF5A4A40;
    // Optional TrueType data for the body text (the game's text font); empty
    // = the embedded fallback faces. Regular and bold.
    std::shared_ptr<const std::vector<uint8_t>> font_regular, font_bold;
    float font_scale = 1.0f;                // body text size multiplier
    bool valid() const { return background.valid() && row_selected.valid() && tab.valid(); }
};

// Installs / removes the theme (null = built-in look). The provider form lets a
// game module defer building the theme (which reads game data) until the first
// time the dialog is drawn; it is called at most once per install.
void set_theme(std::shared_ptr<const SdTheme> t);
void set_theme_provider(std::shared_ptr<const SdTheme> (*provider)());
std::shared_ptr<const SdTheme> theme();

enum class Kind { Load, Save };
enum class Result { Running, Selected, Cancelled };

// RGBA8 canvas of (SCREEN_W*scale) x (SCREEN_H*scale) pixels (byte order
// R,G,B,A in memory; uint32 = 0xAABBGGRR on LE). Always opaque once rendered.
struct Canvas {
    int scale, w, h;
    std::vector<uint32_t> px;
    explicit Canvas(int s = 1)
        : scale(s < 1 ? 1 : (s > MAX_SCALE ? MAX_SCALE : s)),
          w(SCREEN_W * scale), h(SCREEN_H * scale),
          px(size_t(w) * size_t(h), 0) {}
    static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
    }
    void clear(uint32_t c) { px.assign(px.size(), c); }
};

class ListDialog {
public:
    // confirm_cross: true = CROSS confirms / CIRCLE cancels (the default of
    // the task + Western PSPs); false = CIRCLE confirms (utility
    // common.buttonSwap == 0 on Japanese units).
    void begin(Kind kind, std::vector<SaveEntry> entries, bool confirm_cross,
               int initial_selection = 0);
    // buttons = current pad state; now_ms = monotonic ms (for key repeat).
    Result update(uint32_t buttons, int64_t now_ms);
    int selected() const { return sel_; }
    const SaveEntry* selected_entry() const;
    const std::vector<SaveEntry>& entries() const { return entries_; }
    Kind kind() const { return kind_; }
    // Bumped whenever what render() would draw changes (selection/scroll/begin).
    uint32_t revision() const { return rev_; }
    void render(Canvas& c) const;

private:
    Kind kind_ = Kind::Load;
    std::vector<SaveEntry> entries_;
    bool confirm_cross_ = true;
    int sel_ = 0, top_ = 0;
    uint32_t prev_ = 0;
    uint32_t held_dir_ = 0;
    int64_t next_repeat_ = 0;
    uint32_t rev_ = 0;
    bool armed_ = false;     // ignore buttons held when the dialog opened
    void move(int d);
};

// Cross-thread overlay hand-off (game thread publishes, GL thread fetches).
void overlay_publish(const Canvas& c);   // activates the overlay
void overlay_clear();                    // deactivates it
// Copies the current overlay into `out` (w*h uint32, top row first). Returns
// false when no dialog is active.
bool overlay_fetch(std::vector<uint32_t>& out, int* w = nullptr, int* h = nullptr);

}  // namespace psp_sddialog

// Convenience wrapper with the plain-pointer signature (copies the theme).
void psp_sddialog_set_theme(const psp_sddialog::SdTheme* t);
