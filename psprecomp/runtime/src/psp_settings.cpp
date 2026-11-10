// psp_settings.cpp — in-game settings menu. See include/psp_settings.h.
#include "psp_settings.h"
#include "psp_runtime.h"
#include "psp_event_loop.h"
#include "psp_ge_draw.h"
#include "hle/psp_savedata_dialog.h"
#include "hle/psp_hle_adhoc.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---- saved settings --------------------------------------------------------
std::string settings_path() {
    const char* p = std::getenv("PSPRECOMP_SETTINGS");
    return (p && p[0]) ? p : "settings.ini";
}
std::map<std::string, std::string> g_saved;   // what settings.ini holds (keys the user changed in the menu)

void read_file() {
    g_saved.clear();
    std::ifstream f(settings_path());
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        g_saved[k] = v;
    }
}
void write_file(const char* key, const std::string& value);
void save(const char* key, const std::string& value) {
    g_saved[key] = value;
    write_file(key, value);
}
void unsave(const char* key) { g_saved.erase(key); }   // dropped at the next save()
void write_file(const char* key, const std::string& value) {
    const std::string path = settings_path();
    std::ofstream f(path, std::ios::trunc);
    if (!f) { std::fprintf(stderr, "[SETTINGS] cannot write %s\n", path.c_str()); return; }
    f << "# Written by the in-game settings menu. Delete a line (or the file) to go back to the launch options.\n";
    for (const auto& kv : g_saved) f << kv.first << '=' << kv.second << '\n';
    std::fprintf(stderr, "[SETTINGS] saved %s=%s to %s\n", key, value.c_str(), path.c_str());
}

// ---- state -----------------------------------------------------------------
std::atomic<bool> g_open{false};
std::atomic<int> g_parked{0};          // guest threads waiting in psp_settings_guest_wait
bool g_enabled = true;
bool g_wait_release = false;           // after closing: game input stays blocked until all released
bool g_confirm_cross = true;
PspFrameRateHook g_fps{nullptr, 0, nullptr, nullptr};

enum Row { ROW_DISPLAY, ROW_RES, ROW_SCALE, ROW_FPS, ROW_QUIT,
           ROW_NET_MODE, ROW_NET_SERVER, ROW_NET_ROOM, ROW_NET_PEERS, ROW_NET_STATUS,   // Network page
           ROW_PERF, ROW_FX, ROW_SYNC, ROW_RING };   // the last four: Debug page
std::vector<Row> g_rows;
int g_sel = 0;
constexpr int PAGE_SYSTEM = 0, PAGE_NETWORK = 1, PAGE_DEBUG = 2, PAGES = 3;
const char* const k_page_names[PAGES] = {"System", "Network", "Debug"};
int g_page = 0;                        // L / R shoulder, Q / E or Tab: next page
int g_sel_page[PAGES] = {0, 0, 0};
int g_ring_next = -1;                  // vertex upload mode saved for the next launch (-1 = unchanged)
bool g_quit_armed = false;
bool g_dirty = true;
int g_drawn_scale = 0;
int64_t g_last_present = 0;
int g_pending_rs = 0;                  // render scale waiting for the game to park

// Display: fullscreen or a window. Window sizes: k x 480x272, or the Steam Deck's 1280 x 800 (k = K_DECK; the
// picture is letterboxed as on the Deck), or a size set by hand (k = -1, listed so the row can show it).
struct ResOpt { int k; int w, h; };
constexpr int K_DECK = 100, DECK_W = 1280, DECK_H = 800;
std::vector<ResOpt> g_res;
int g_win_sel = -1;                    // the window size used in Window mode (index into g_res)

// Navigation input (menu only).
uint32_t g_dir_held = 0;               // 1 up, 2 down, 4 left, 8 right (pad d-pad / left stick)
int64_t g_dir_next = 0;
int16_t g_lx = 0, g_ly = 0;
bool g_l3 = false, g_r3 = false;
uint32_t g_pad_dpad = 0;

SDL_Window* win() { return psp_get_sdl_window(); }

void build_res_options() {
    g_res.clear();
    SDL_Window* w = win();
    SDL_Rect r{0, 0, 0, 0};
    const int disp = w ? SDL_GetWindowDisplayIndex(w) : 0;
    if (SDL_GetDisplayUsableBounds(disp < 0 ? 0 : disp, &r) != 0 || r.w <= 0) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(disp < 0 ? 0 : disp, &dm) == 0) { r.w = dm.w; r.h = dm.h; }
    }
    int kmax = r.w > 0 ? std::min(r.w / 480, (r.h - 40) / 272) : 2;
    kmax = std::max(1, std::min(kmax, 8));
    for (int k = 1; k <= kmax; ++k) g_res.push_back({k, 480 * k, 272 * k});
    // The Steam Deck's screen as a window (to test the Deck's layout on a desktop), placed by width.
    if (r.w >= DECK_W && r.h - 40 >= DECK_H) {
        auto at = std::find_if(g_res.begin(), g_res.end(), [](const ResOpt& o) { return o.w > DECK_W; });
        g_res.insert(at, {K_DECK, DECK_W, DECK_H});
    }
}
bool is_fullscreen() {
    SDL_Window* w = win();
    return w && (SDL_GetWindowFlags(w) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}
int res_index(int ww, int wh) {   // the option of that size, added as a hand-set size if there is none
    for (size_t i = 0; i < g_res.size(); ++i)
        if (g_res[i].w == ww && g_res[i].h == wh) return int(i);
    g_res.push_back({-1, ww, wh});
    return int(g_res.size()) - 1;
}
// The window size row's choice: the window itself when windowed; in fullscreen the size last used, else the saved
// one, else the largest whole multiple that fits.
void pick_window_size() {
    SDL_Window* w = win();
    if (w && !is_fullscreen()) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        g_win_sel = res_index(ww, wh);
        return;
    }
    if (g_win_sel >= 0 && g_win_sel < int(g_res.size())) {   // keep the earlier choice (the list may have been rebuilt)
        return;
    }
    int ww = 0, wh = 0;
    auto sz = g_saved.find("window_size");
    auto k = g_saved.find("window_scale");
    if (sz != g_saved.end() && std::sscanf(sz->second.c_str(), "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
        g_win_sel = res_index(ww, wh);
    } else if (k != g_saved.end() && std::atoi(k->second.c_str()) > 0) {
        const int n = std::atoi(k->second.c_str());
        g_win_sel = res_index(480 * n, 272 * n);
    } else {
        g_win_sel = 0;
        for (size_t i = 0; i < g_res.size(); ++i) if (g_res[i].k > 0 && g_res[i].k != K_DECK) g_win_sel = int(i);
    }
}

int fps_index() {
    if (!g_fps.get) return 0;
    const int f = g_fps.get();
    for (int i = 0; i < g_fps.count; ++i) if (g_fps.rates[i] == f) return i;
    return 0;
}
constexpr int k_max_rs = 4;

// ---- Network page --------------------------------------------------------------------------------------
constexpr const char* NET_DEFAULT_SERVER = "party.schindlershadow.com:27500";
bool g_net_internet = true;
std::string g_net_server = NET_DEFAULT_SERVER, g_net_room, g_net_peers, g_net_status;
bool g_net_loaded = false;
int64_t g_status_next = 0;

// Text entry for the server / room / peers rows: a keyboard types directly; a controller picks a letter
// (left / right), adds it (Cross) and deletes (Circle), as on a console.
struct Edit {
    bool active = false;
    int field = 0;          // 0 server, 1 room, 2 peers
    std::string buf;
    int ci = 0;             // index into the letter list
    int sel = 1;            // selected row of the editor
} g_edit;
const char* const k_edit_chars = "abcdefghijklmnopqrstuvwxyz0123456789.-_:,ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
constexpr int EDIT_ROWS = 5;   // text, letter, delete, done, cancel
const char* edit_label() { return g_edit.field == 0 ? "Server" : g_edit.field == 1 ? "Room" : "Direct Peers"; }
size_t edit_max() { return g_edit.field == 0 ? 63 : g_edit.field == 1 ? 31 : 127; }

void net_load() {
    if (g_net_loaded) return;
    g_net_loaded = true;
    const char* m = std::getenv("PSPRECOMP_ADHOC_MODE");
    const char* sv = std::getenv("PSPRECOMP_ADHOC_SERVER");
    const char* rm = std::getenv("PSPRECOMP_ADHOC_ROOM");
    const char* pe = std::getenv("PSPRECOMP_ADHOC_PEERS");
    if (m && *m) g_net_internet = !(m[0] == 'l' || m[0] == 'L');
    else if (pe && *pe) g_net_internet = false;
    if (sv && *sv) g_net_server = sv;
    if (rm) g_net_room = rm;
    if (pe) g_net_peers = pe;
}
void net_apply() {   // use and remember the Network page's values
    psp_adhoc_configure(g_net_internet, g_net_server.c_str(), g_net_room.c_str(), g_net_peers.c_str());
    save("net_mode", g_net_internet ? "internet" : "lan");
    save("net_server", g_net_server);
    save("net_room", g_net_room);
    save("net_peers", g_net_peers);
    g_status_next = 0;
}
std::string& edit_target() { return g_edit.field == 0 ? g_net_server : g_edit.field == 1 ? g_net_room : g_net_peers; }
void edit_begin(int field) {
    g_edit = Edit{};
    g_edit.active = true;
    g_edit.field = field;
    g_edit.buf = edit_target();
    SDL_StartTextInput();
    g_dirty = true;
}
void edit_end() { g_edit.active = false; SDL_StopTextInput(); g_dirty = true; }
void edit_commit() {
    std::string v = g_edit.buf;
    while (!v.empty() && v.back() == ' ') v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    if (g_edit.field == 0 && v.empty()) v = NET_DEFAULT_SERVER;
    edit_target() = v;
    edit_end();
    net_apply();
}
void edit_append(char c) {
    if (c < 0x20 || c > 0x7e || g_edit.buf.size() >= edit_max()) return;
    g_edit.buf += c;
    g_dirty = true;
}
void edit_backspace() { if (!g_edit.buf.empty()) { g_edit.buf.pop_back(); g_dirty = true; } }

std::vector<psp_sddialog::MenuRow> make_edit_rows() {
    std::vector<psp_sddialog::MenuRow> out(EDIT_ROWS);
    std::string shown = g_edit.buf + "_";
    if (shown.size() > 30) shown = "..." + shown.substr(shown.size() - 27);
    out[0].label = edit_label();
    out[0].value = shown;
    out[0].help = std::string(edit_label()) + "\nType on a keyboard, or pick letters below. Enter or Done saves; Esc or Cancel keeps the old value.";
    const char c = k_edit_chars[g_edit.ci];
    out[1].label = "Letter";
    out[1].value = c == ' ' ? "Space" : std::string(1, c);
    out[1].can_dec = out[1].can_inc = true;
    out[1].help = "Letter\nLeft / right choose a character, Cross adds it.";
    out[2].label = "Delete";
    out[2].help = "Delete\nRemoves the last character. Circle does the same.";
    out[3].label = "Done";
    out[3].help = "Done\nSaves the new value and uses it at once.";
    out[4].label = "Cancel";
    out[4].help = "Cancel\nKeeps the old value.";
    return out;
}

std::vector<psp_sddialog::MenuRow> make_rows() {
    if (g_edit.active) return make_edit_rows();
    std::vector<psp_sddialog::MenuRow> out;
    char buf[96];
    for (Row r : g_rows) {
        psp_sddialog::MenuRow m;
        switch (r) {
        case ROW_DISPLAY: {
            const bool fs = is_fullscreen();
            m.label = "Display";
            if (fs) {
                int dw = 0, dh = 0;
                if (win()) SDL_GetWindowSize(win(), &dw, &dh);
                std::snprintf(buf, sizeof buf, "Fullscreen  %d x %d", dw, dh);
                m.value = buf;
            } else m.value = "Window";
            m.can_dec = fs;    // left: Window
            m.can_inc = !fs;   // right: Fullscreen
            m.help = "Fullscreen or window\nFullscreen fills the screen. Saved. (F11 / Alt+Enter switch for this session only.)";
            break;
        }
        case ROW_RES: {
            const int i = g_win_sel < 0 || g_win_sel >= int(g_res.size()) ? 0 : g_win_sel;
            const ResOpt& o = g_res[size_t(i)];
            m.label = "Window Size";
            if (o.k == K_DECK) std::snprintf(buf, sizeof buf, "%d x %d (Deck)", o.w, o.h);
            else std::snprintf(buf, sizeof buf, "%d x %d", o.w, o.h);
            m.value = buf;
            int lo = i - 1, hi = i + 1;
            while (lo >= 0 && g_res[size_t(lo)].k < 0) lo--;
            while (hi < int(g_res.size()) && g_res[size_t(hi)].k < 0) hi++;
            m.can_dec = lo >= 0 || o.k < 0;
            m.can_inc = hi < int(g_res.size());
            m.help = is_fullscreen()
                ? "Window size\nUsed when Display is Window: multiples of the PSP's 480 x 272, or the Deck's screen."
                : "Window size\nMultiples of the PSP's 480 x 272, or the Deck's screen (1280 x 800). Saved.";
            break;
        }
        case ROW_SCALE: {
            const int n = g_pending_rs ? g_pending_rs : ge_draw_render_scale();
            m.label = "Render Scale";
            std::snprintf(buf, sizeof buf, "%dx  %d x %d", n, 480 * n, 272 * n);
            m.value = buf;
            m.can_dec = n > 1;
            m.can_inc = n < k_max_rs;
            m.help = "Internal 3D resolution\nHigher is sharper and costs more GPU time (Steam Deck: 2x or 3x).";
            break;
        }
        case ROW_FPS: {
            const int i = fps_index();
            m.label = "Frame Rate";
            std::snprintf(buf, sizeof buf, "%d fps", g_fps.rates[i]);
            m.value = buf;
            m.can_dec = i > 0;
            m.can_inc = i + 1 < g_fps.count;
            m.help = "Battle logic rate\n60 runs battles at 60 updates a second (twice the PSP's work). Menus stay at 30.";
            break;
        }
        case ROW_NET_MODE:
            m.label = "Network";
            m.value = g_net_internet ? "Internet" : "LAN";
            m.can_dec = !g_net_internet;
            m.can_inc = g_net_internet;
            m.help = "Network\nInternet: play with anyone through the party server (the default). LAN: find players on your own network, or at the addresses under Direct Peers. Saved.";
            break;
        case ROW_NET_SERVER:
            m.label = "Server";
            m.value = g_net_server;
            m.help = "Party server\nThe relay server for Internet play: a host name, or host:port. Press Cross to edit. Saved.";
            break;
        case ROW_NET_ROOM:
            m.label = "Room";
            m.value = g_net_room.empty() ? "Public" : g_net_room;
            m.help = "Room\nPlayers in the same room see each other. Leave it empty for the public room, or use a name only your friends know. Press Cross to edit. Saved.";
            break;
        case ROW_NET_PEERS:
            m.label = "Direct Peers";
            m.value = g_net_peers.empty() ? "None" : g_net_peers;
            m.help = "Direct peers\nExtra addresses to contact directly, in addition to the local network: host or host:port, comma separated. Press Cross to edit. Saved.";
            break;
        case ROW_NET_STATUS:
            m.label = "Status";
            m.value = g_net_status.empty() ? "..." : g_net_status;
            m.help = "Network status\nWhether the server answers and how many players are in your room (or nearby, on a LAN). Communications mode starts the connection.";
            break;
        case ROW_QUIT:
            m.label = "Quit Game";
            m.value = g_quit_armed ? "Press again to quit" : "";
            m.help = g_quit_armed ? "Quit\nUnsaved progress is lost. Press again to quit, or move away to cancel."
                                  : "Quit\nClose the game and return to the desktop / Steam.  (L / R: Debug page)";
            break;
        case ROW_PERF: {
            const bool on = ge_draw_perf_overlay();
            m.label = "Performance Overlay";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Frame counter\nFrames per second and frame times in the top-left corner. Saved.  (L / R: System page)";
            break;
        }
        case ROW_FX: {
            const bool avail = ge_draw_fx_available(), on = ge_draw_fx_shaders();
            m.label = "FX Shaders";
            m.value = !avail ? "Not loaded" : (on ? "On" : "Off");
            m.can_dec = avail && on; m.can_inc = avail && !on;
            m.help = avail ? "Effect shader overrides (mods/fx_shaders)\nOff draws effects the original way. This session only."
                           : "Effect shader overrides (mods/fx_shaders)\nNone loaded at start (no manifest, or PSPRECOMP_FX_SHADERS=0).";
            break;
        }
        case ROW_SYNC: {
            static const char* names[5] = {"Off", "Every batch", "Every list", "Every frame", "Flush only"};
            const int s = ge_draw_sync_mode();
            m.label = "GPU Sync";
            m.value = names[s < 0 || s > 4 ? 0 : s];
            m.can_dec = s > 0; m.can_inc = s < 4;
            m.help = "Flicker test (--ge-sync)\nWaits for the GPU at each step: slower, rules out timing. This session only.";
            break;
        }
        case ROW_RING: {
            const int cur = ge_draw_ring_mode(), nx = g_ring_next >= 0 ? g_ring_next : cur;
            m.label = "Vertex Upload";
            std::snprintf(buf, sizeof buf, "%s%s", nx ? "Map" : "Sub", nx != cur ? "  (next launch)" : "");
            m.value = buf;
            m.can_dec = nx == 1; m.can_inc = nx == 0;
            m.help = "Flicker test (PSPRECOMP_GE_RING)\nHow vertices reach the GPU. Saved; applies when the game next starts.";
            break;
        }
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ---- applying --------------------------------------------------------------
void size_window(const ResOpt& o) {
    SDL_Window* w = win();
    if (!w) return;
    SDL_SetWindowSize(w, o.w, o.h);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(w)),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(w)));
}
void apply_display(bool fullscreen) {
    SDL_Window* w = win();
    if (!w || fullscreen == is_fullscreen()) return;
    if (fullscreen) {
        if (SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
            std::fprintf(stderr, "[SETTINGS] fullscreen failed: %s\n", SDL_GetError());
        save("display", "fullscreen");
    } else {
        SDL_SetWindowFullscreen(w, 0);
        if (g_win_sel >= 0 && g_win_sel < int(g_res.size())) size_window(g_res[size_t(g_win_sel)]);
        save("display", "window");
    }
    std::fprintf(stderr, "[SETTINGS] display: %s\n", fullscreen ? "fullscreen" : "window");
}
// Window size: resizes the window now, or (in fullscreen) is remembered for Window mode. Saved either way.
void apply_res(int i) {
    if (i < 0 || i >= int(g_res.size()) || g_res[size_t(i)].k < 0) return;
    g_win_sel = i;
    const ResOpt& o = g_res[size_t(i)];
    if (!is_fullscreen()) size_window(o);
    if (o.k == K_DECK) {
        unsave("window_scale");
        save("window_size", std::to_string(o.w) + "x" + std::to_string(o.h));
    } else {
        unsave("window_size");
        save("window_scale", std::to_string(o.k));
    }
    std::fprintf(stderr, "[SETTINGS] window size %dx%d%s\n", o.w, o.h, is_fullscreen() ? " (for Window mode)" : "");
}
void apply_render_scale(int n) {
    n = std::max(1, std::min(n, k_max_rs));
    g_pending_rs = n;   // applied in tick once the game is parked
    save("render_scale", std::to_string(n));
}
void apply_fps(int i) {
    if (!g_fps.set || i < 0 || i >= g_fps.count) return;
    g_fps.set(g_fps.rates[i]);
    save("fps", std::to_string(g_fps.rates[i]));
}

void build_rows();

void change(int d) {   // left / right on the selected row
    if (g_edit.active) {
        if (g_edit.sel == 1) {
            const int n = int(std::strlen(k_edit_chars));
            g_edit.ci = (g_edit.ci + d + n) % n;
            g_dirty = true;
        }
        return;
    }
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    switch (g_rows[size_t(g_sel)]) {
    case ROW_DISPLAY:
        apply_display(d > 0);
        break;
    case ROW_RES: {
        int i = g_win_sel < 0 ? 0 : g_win_sel;
        if (g_res[size_t(i)].k < 0) {   // leave a hand-set size: the nearest listed size that way
            const int cw = g_res[size_t(i)].w;
            i = -1;
            for (int j = 0; j < int(g_res.size()); ++j) {
                const ResOpt& o = g_res[size_t(j)];
                if (o.k < 0) continue;
                if (d > 0 && o.w > cw) { i = j; break; }   // first larger
                if (d < 0 && o.w < cw) i = j;              // last smaller
            }
        } else {
            i += d;
            while (i >= 0 && i < int(g_res.size()) && g_res[size_t(i)].k < 0) i += d;
        }
        if (i >= 0 && i < int(g_res.size())) apply_res(i);
        break;
    }
    case ROW_SCALE: {
        const int n = (g_pending_rs ? g_pending_rs : ge_draw_render_scale()) + d;
        if (n >= 1 && n <= k_max_rs) apply_render_scale(n);
        break;
    }
    case ROW_FPS: {
        const int i = fps_index() + d;
        if (i >= 0 && i < g_fps.count) apply_fps(i);
        break;
    }
    case ROW_QUIT: break;
    case ROW_NET_MODE:
        if (g_net_internet != (d < 0)) {   // left: Internet, right: LAN
            g_net_internet = d < 0;
            net_apply();
            g_sel_page[g_page] = g_sel;
            build_rows();   // the rows differ: server and room, or direct peers
        }
        break;
    case ROW_NET_SERVER: case ROW_NET_ROOM: case ROW_NET_PEERS: case ROW_NET_STATUS: break;
    case ROW_PERF:
        if ((d > 0) != ge_draw_perf_overlay()) {
            ge_draw_set_perf_overlay(d > 0);
            save("perf_overlay", d > 0 ? "1" : "0");
        }
        break;
    case ROW_FX:
        if (ge_draw_fx_available() && (d > 0) != ge_draw_fx_shaders()) ge_draw_set_fx_shaders(d > 0);
        break;
    case ROW_SYNC: {
        const int s = ge_draw_sync_mode() + d;
        if (s >= 0 && s <= 4) ge_draw_set_sync_mode(s);
        break;
    }
    case ROW_RING: {
        const int nx = d > 0 ? 1 : 0;
        const int cur = g_ring_next >= 0 ? g_ring_next : ge_draw_ring_mode();
        if (nx != cur) {
            g_ring_next = nx;
            save("ge_ring", nx ? "map" : "sub");
        }
        break;
    }
    }
    g_dirty = true;
}

void build_rows() {
    if (g_page == PAGE_NETWORK) {
        net_load();
        g_rows = {ROW_NET_MODE};
        if (g_net_internet) { g_rows.push_back(ROW_NET_SERVER); g_rows.push_back(ROW_NET_ROOM); }
        else g_rows.push_back(ROW_NET_PEERS);
        g_rows.push_back(ROW_NET_STATUS);
    } else if (g_page == PAGE_DEBUG) {
        g_rows = {ROW_PERF, ROW_FX, ROW_SYNC, ROW_RING};
    } else {
        g_rows = {ROW_DISPLAY, ROW_RES, ROW_SCALE};
        if (g_fps.get && g_fps.count > 1) g_rows.push_back(ROW_FPS);
        g_rows.push_back(ROW_QUIT);
    }
    g_sel = std::min(std::max(g_sel_page[g_page], 0), int(g_rows.size()) - 1);
}

void switch_page() {
    g_sel_page[g_page] = g_sel;
    g_page = (g_page + 1) % PAGES;
    build_rows();
    g_quit_armed = false;
    g_dirty = true;
}

void set_open(bool open) {
    if (open == g_open.load()) return;
    if (open) {
        build_res_options();
        pick_window_size();
        net_load();
        build_rows();
        g_quit_armed = false;
        g_dirty = true;
        g_dir_held = 0;
        g_open = true;
        std::fprintf(stderr, "[SETTINGS] menu open (game paused)\n");
    } else {
        g_sel_page[g_page] = g_sel;
        if (g_edit.active) edit_end();
        g_open = false;
        g_wait_release = true;
        psp_sddialog::menu_overlay_clear();
        ge_draw_present_overlay_now();
        std::fprintf(stderr, "[SETTINGS] menu closed\n");
    }
}

void move_sel(int d) {
    if (g_edit.active) { g_edit.sel = (g_edit.sel + d + EDIT_ROWS) % EDIT_ROWS; g_dirty = true; return; }
    const int n = int(g_rows.size());
    if (!n) return;
    g_sel = (g_sel + d + n) % n;
    g_quit_armed = false;
    g_dirty = true;
}

void confirm() {
    if (g_edit.active) {
        switch (g_edit.sel) {
        case 1: edit_append(k_edit_chars[g_edit.ci]); break;
        case 2: edit_backspace(); break;
        case 3: edit_commit(); break;
        case 4: edit_end(); break;
        default: break;
        }
        return;
    }
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    switch (g_rows[size_t(g_sel)]) {
    case ROW_QUIT:
        if (!g_quit_armed) { g_quit_armed = true; g_dirty = true; return; }
        std::fprintf(stderr, "[SETTINGS] quit from the settings menu\n");
        g_should_exit.store(true);
        set_open(false);
        return;
    case ROW_DISPLAY:
        apply_display(!is_fullscreen());
        break;
    case ROW_RES: {   // cycle forward
        const int i = g_win_sel < 0 ? -1 : g_win_sel;
        int j = (i + 1) % int(g_res.size());
        while (g_res[size_t(j)].k < 0) j = (j + 1) % int(g_res.size());
        apply_res(j);
        break;
    }
    case ROW_SCALE: {
        const int n = g_pending_rs ? g_pending_rs : ge_draw_render_scale();
        apply_render_scale(n % k_max_rs + 1);
        break;
    }
    case ROW_FPS:
        apply_fps((fps_index() + 1) % g_fps.count);
        break;
    case ROW_NET_MODE: change(g_net_internet ? 1 : -1); return;
    case ROW_NET_SERVER: edit_begin(0); return;
    case ROW_NET_ROOM: edit_begin(1); return;
    case ROW_NET_PEERS: edit_begin(2); return;
    case ROW_NET_STATUS: break;
    case ROW_PERF: change(ge_draw_perf_overlay() ? -1 : 1); return;
    case ROW_FX: change(ge_draw_fx_shaders() ? -1 : 1); return;
    case ROW_SYNC: ge_draw_set_sync_mode((ge_draw_sync_mode() + 1) % 5); break;
    case ROW_RING: change((g_ring_next >= 0 ? g_ring_next : ge_draw_ring_mode()) ? -1 : 1); return;
    }
    g_dirty = true;
}

void nav(uint32_t dir) {
    if (dir & 1) move_sel(-1);
    else if (dir & 2) move_sel(1);
    else if (dir & 4) change(-1);
    else if (dir & 8) change(1);
}

uint32_t stick_dir() {
    const int t = 16000;
    if (g_ly < -t) return 1;
    if (g_ly > t) return 2;
    if (g_lx < -t) return 4;
    if (g_lx > t) return 8;
    return 0;
}
void held_dir_update() {   // pad d-pad / stick with repeat (keyboard uses the OS key repeat)
    const uint32_t d = g_pad_dpad ? g_pad_dpad : stick_dir();
    if (d == 0) { g_dir_held = 0; return; }
    const int64_t t = now_ms();
    if (d != g_dir_held) { g_dir_held = d; g_dir_next = t + 380; nav(d); }
    else if (t >= g_dir_next) { g_dir_next = t + 110; nav(d); }
}

uint32_t dpad_bit(uint8_t b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 4;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 8;
    default: return 0;
    }
}

}  // namespace

void psp_settings_set_frame_rate_hook(const PspFrameRateHook& hook) { g_fps = hook; }

void psp_settings_load_early() {
    const char* e = std::getenv("PSPRECOMP_SETTINGS_MENU");
    g_enabled = !(e && e[0] == '0');
    e = std::getenv("PSPRECOMP_SWAP_AB");
    g_confirm_cross = !(e && e[0] && e[0] != '0');
    read_file();
    if (g_saved.empty()) return;
    std::string applied;
    auto put = [&](const char* var, const std::string& v) {
        setenv(var, v.c_str(), 1);
        applied += std::string(" ") + var + "=" + v;
    };
    auto it = g_saved.find("display");
    if (it != g_saved.end() && it->second == "fullscreen") put("PSPRECOMP_FULLSCREEN", "1");
    else if (it != g_saved.end() && it->second == "window") {
        put("PSPRECOMP_FULLSCREEN", "0");
        auto k = g_saved.find("window_scale");
        if (k != g_saved.end() && std::atoi(k->second.c_str()) > 0) { put("PSPRECOMP_SCALE", k->second); unsetenv("PSPRECOMP_WINDOW"); }
    }
    if ((it = g_saved.find("render_scale")) != g_saved.end() && std::atoi(it->second.c_str()) > 0)
        put("PSPRECOMP_RENDER_SCALE", it->second);
    if ((it = g_saved.find("fps")) != g_saved.end() && std::atoi(it->second.c_str()) > 0) put("PSPRECOMP_FPS", it->second);
    if ((it = g_saved.find("window_size")) != g_saved.end() && g_saved.count("display") && g_saved["display"] == "window"
        && it->second.find('x') != std::string::npos) {
        put("PSPRECOMP_WINDOW", it->second);
        unsetenv("PSPRECOMP_SCALE");
    }
    if ((it = g_saved.find("net_mode")) != g_saved.end() && (it->second == "internet" || it->second == "lan"))
        put("PSPRECOMP_ADHOC_MODE", it->second);
    if ((it = g_saved.find("net_server")) != g_saved.end() && !it->second.empty()) put("PSPRECOMP_ADHOC_SERVER", it->second);
    if ((it = g_saved.find("net_room")) != g_saved.end()) put("PSPRECOMP_ADHOC_ROOM", it->second);
    if ((it = g_saved.find("net_peers")) != g_saved.end()) put("PSPRECOMP_ADHOC_PEERS", it->second);
    if ((it = g_saved.find("perf_overlay")) != g_saved.end()) put("PSPRECOMP_PERF_HUD", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("ge_ring")) != g_saved.end() && (it->second == "map" || it->second == "sub"))
        put("PSPRECOMP_GE_RING", it->second);
    std::fprintf(stderr, "[SETTINGS] %s:%s\n", settings_path().c_str(), applied.c_str());
}

bool psp_settings_menu_open() { return g_open.load(); }

bool psp_settings_handle_event(const SDL_Event& ev) {
    if (!g_enabled) return false;
    const bool open = g_open.load();
    switch (ev.type) {
    case SDL_KEYDOWN: {
        const SDL_Keycode k = ev.key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_F1) {
            if (open && g_edit.active) { if (!ev.key.repeat) edit_end(); return true; }   // cancel the edit only
            if (!ev.key.repeat) set_open(!open);
            return true;
        }
        if (!open) return false;
        if (g_edit.active) {   // letters arrive as SDL_TEXTINPUT; only navigation keys act here
            switch (k) {
            case SDLK_UP: move_sel(-1); break;
            case SDLK_DOWN: move_sel(1); break;
            case SDLK_LEFT: change(-1); break;
            case SDLK_RIGHT: change(1); break;
            case SDLK_BACKSPACE: edit_backspace(); break;
            case SDLK_RETURN: case SDLK_KP_ENTER:
                if (!ev.key.repeat) { if (g_edit.sel == 1 || g_edit.sel == 2 || g_edit.sel == 4) confirm(); else edit_commit(); }
                break;
            default: break;
            }
            return false;
        }
        switch (k) {
        case SDLK_UP: move_sel(-1); break;
        case SDLK_DOWN: move_sel(1); break;
        case SDLK_LEFT: change(-1); break;
        case SDLK_RIGHT: change(1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: case SDLK_SPACE:
            if (!ev.key.repeat) confirm();
            break;
        case SDLK_z: case SDLK_BACKSPACE:
            if (!ev.key.repeat) set_open(false);
            break;
        case SDLK_q: case SDLK_e: case SDLK_TAB: case SDLK_PAGEUP: case SDLK_PAGEDOWN:
            if (!ev.key.repeat) switch_page();
            break;
        default: break;
        }
        return false;   // the event loop still tracks key state (the game sees none while blocked)
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
        const uint8_t b = ev.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_LEFTSTICK) g_l3 = down;
        if (b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) g_r3 = down;
        if (down && (b == SDL_CONTROLLER_BUTTON_GUIDE ||
                     ((b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) && g_l3 && g_r3))) {
            set_open(!open);
            return true;
        }
        if (const uint32_t d = dpad_bit(b)) {
            if (down) g_pad_dpad = d; else if (g_pad_dpad == d) g_pad_dpad = 0;
        }
        if (!open || !down) return false;
        const uint8_t ok = g_confirm_cross ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
        const uint8_t back = g_confirm_cross ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
        if (g_edit.active) {   // Circle deletes (or cancels when empty), Start saves
            if (b == ok) confirm();
            else if (b == back) { if (g_edit.buf.empty()) edit_end(); else edit_backspace(); }
            else if (b == SDL_CONTROLLER_BUTTON_START) edit_commit();
            return false;
        }
        if (b == ok) confirm();
        else if (b == back || b == SDL_CONTROLLER_BUTTON_START) set_open(false);
        else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) switch_page();
        return false;
    }
    case SDL_TEXTINPUT:
        if (open && g_edit.active) {
            for (const char* c = ev.text.text; *c; ++c) edit_append(*c);
            return true;
        }
        return false;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_lx = ev.caxis.value;
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_ly = ev.caxis.value;
        return false;
    case SDL_WINDOWEVENT:
        if (open) {   // size change (or F11): the overlay scale and the display rows follow
            if (!is_fullscreen()) pick_window_size();
            g_dirty = true;
        }
        return false;
    default:
        return false;
    }
}

void psp_settings_tick() {
    if (!g_open.load()) return;
    held_dir_update();
    // A render scale change waits until the game has parked (no display list in flight).
    if (g_pending_rs && g_parked.load() > 0) {
        if (!ge_draw_set_render_scale(g_pending_rs))
            std::fprintf(stderr, "[SETTINGS] render scale change refused (PSPRECOMP_GE_AB is on)\n");
        g_pending_rs = 0;
        g_dirty = true;
    }
    const int scale = psp_sddialog::output_scale();
    const int64_t t = now_ms();
    if (g_page == PAGE_NETWORK && !g_edit.active && t >= g_status_next) {   // keep the status line live
        g_status_next = t + 500;
        char st[128];
        psp_adhoc_status(st, sizeof st);
        if (g_net_status != st) { g_net_status = st; g_dirty = true; }
    }
    if (g_dirty || scale != g_drawn_scale) {
        g_dirty = false;
        g_drawn_scale = scale;
        psp_sddialog::Canvas c(scale);
        psp_sddialog::render_menu(c, "Settings", k_page_names[g_page], make_rows(), g_edit.active ? g_edit.sel : g_sel, g_confirm_cross);
        psp_sddialog::menu_overlay_publish(c);
        ge_draw_present_overlay_now();
        g_last_present = t;
    } else if (t - g_last_present >= 100) {   // keep the window fresh while the game is paused
        ge_draw_present_overlay_now();
        g_last_present = t;
    }
}

bool psp_settings_block_input(uint32_t raw_buttons) {
    if (g_open.load()) return true;
    if (g_wait_release) {
        if (raw_buttons == 0) g_wait_release = false;
        else return true;
    }
    return false;
}

void psp_settings_guest_wait() {
    if (!g_open.load(std::memory_order_relaxed)) return;
    g_parked.fetch_add(1);
    while (g_open.load() && !g_should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    g_parked.fetch_sub(1);
}
