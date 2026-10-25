#include "psp_event_loop.h"
#include "psp_runtime.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_ge_draw.h"

#include <SDL.h>
#include <glad/glad.h>
void gl_stats_install();
#include <csignal>
#include <cstdio>
#include <atomic>
#include <cstdlib>

// --- KHR_debug driver messages (PSPRECOMP_GL_DEBUG=1) -----------------------
// glad was generated without KHR_debug; fetch the entry points by hand.
typedef void (*GlDebugProcFn)(unsigned source, unsigned type, unsigned id, unsigned severity,
                                       int length, const char* message, const void* user);
typedef void (*PfnDebugMessageCallback)(GlDebugProcFn cb, const void* user);
typedef void (*PfnDebugMessageControl)(unsigned source, unsigned type, unsigned severity,
                                                int count, const unsigned* ids, unsigned char enabled);
static void gl_debug_cb(unsigned source, unsigned type, unsigned id, unsigned severity,
                                 int, const char* message, const void*) {
    static int n = 0;
    if (severity == 0x826B /*GL_DEBUG_SEVERITY_NOTIFICATION*/) return;
    if (++n > 2000) return;   // keep the log bounded
    const char* sev = severity == 0x9146 ? "HIGH" : severity == 0x9147 ? "MED" : severity == 0x9148 ? "LOW" : "?";
    const char* ty = type == 0x824C ? "ERROR" : type == 0x824D ? "DEPRECATED" : type == 0x824E ? "UNDEFINED"
                   : type == 0x824F ? "PORTABILITY" : type == 0x8250 ? "PERFORMANCE" : type == 0x8251 ? "OTHER" : "?";
    std::fprintf(stderr, "[GL_DEBUG] %s/%s src=0x%X id=%u: %s\n", ty, sev, source, id, message);
}
static void gl_debug_install() {
    auto cb = (PfnDebugMessageCallback)SDL_GL_GetProcAddress("glDebugMessageCallback");
    auto ctl = (PfnDebugMessageControl)SDL_GL_GetProcAddress("glDebugMessageControl");
    if (!cb) { std::fprintf(stderr, "[GL_DEBUG] KHR_debug not available\n"); return; }
    glEnable(0x92E0 /*GL_DEBUG_OUTPUT*/);
    glEnable(0x8242 /*GL_DEBUG_OUTPUT_SYNCHRONOUS*/);
    cb(gl_debug_cb, nullptr);
    if (ctl) ctl(0x1100 /*DONT_CARE*/, 0x1100, 0x1100, 0, nullptr, 1);
    std::fprintf(stderr, "[GL_DEBUG] driver debug output on\n");
}
#include <chrono>
#include <thread>
#include <map>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// --- Global state definitions (declared extern in psp_runtime.h) ---
std::atomic<bool> g_should_exit{false};
std::atomic<int>  g_alive_threads{0};
std::thread::id   g_main_thread_id;
std::atomic<uint32_t> g_host_buttons{0};

// ---------------------------------------------------------------------------
// Host input -> PSP controller mapping (generic; no game-specific code)
//
// PSP button bits: SELECT 0x0001, START 0x0008, UP 0x0010, RIGHT 0x0020,
//   DOWN 0x0040, LEFT 0x0080, LTRIGGER 0x0100, RTRIGGER 0x0200,
//   TRIANGLE 0x1000, CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000.
//
// Keyboard:
//   Arrows        -> D-pad AND left analogue stick (full deflection)
//                    (D-pad only if PSPRECOMP_ARROWS_DPAD_ONLY=1)
//   X / Enter     -> CROSS      Z -> CIRCLE     A -> SQUARE   S -> TRIANGLE
//   Q -> LTRIGGER               W -> RTRIGGER
//   Space -> START              Tab -> SELECT
//
// SDL game controller (all connected pads, hot-plug aware):
//   A -> CROSS        B -> CIRCLE      X -> SQUARE      Y -> TRIANGLE
//   LEFTSHOULDER  -> LTRIGGER    TRIGGERLEFT  (>50%) -> LTRIGGER (or PSPRECOMP_PAD_L2)
//   RIGHTSHOULDER -> RTRIGGER    TRIGGERRIGHT (>50%) -> RTRIGGER (or PSPRECOMP_PAD_R2)
//   DPAD up/right/down/left -> UP/RIGHT/DOWN/LEFT
//   START -> START    BACK -> SELECT    GUIDE -> ignored
//   Left stick -> analogue lx/ly, 0..255 (centre 128), ~8% deadzone applied
//                 to the PSP-scale value (SDL y-down == PSP y-down).
//   Right stick -> D-pad bits (>50% deflection, applied in the sceCtrl HLE) by
//                 default; PSPRECOMP_RSTICK_DPAD=0 disables. A game module that
//                 uses the stick itself (Dissidia analogue camera) sets
//                 g_rstick_consumed_until_ms and the D-pad conversion pauses.
//
// Environment:
//   PSPRECOMP_SWAP_AB=1            swap confirm/cancel on the pad:
//                                  A->CIRCLE, B->CROSS (X/Y unchanged)
//   PSPRECOMP_PAD_L2=<buttons>     what the analogue triggers press instead of L / R, as PSP
//   PSPRECOMP_PAD_R2=<buttons>     button names joined by '+' (L R CROSS CIRCLE SQUARE TRIANGLE
//                                  UP DOWN LEFT RIGHT START SELECT; case-insensitive), e.g.
//                                  R+CROSS. All are held together for as long as the trigger is
//                                  past 50%. "none" disables the trigger. The shoulder buttons
//                                  (L1/R1) always stay L / R. An unknown name logs and is ignored.
//   PSPRECOMP_ARROWS_DPAD_ONLY=1   arrows do not drive the analogue stick
//   PSPRECOMP_RSTICK_DPAD=0        right stick does NOT act as D-pad (default on)
//   PSPRECOMP_GAMECONTROLLERDB=f   SDL gamecontrollerdb mapping file to load
//   PSPRECOMP_PAD_LOG=1            log every pad button event (SDL name -> PSP bit); the
//                                  "Controller opened" line always prints GUID + SDL mapping
//
// Sources are tracked separately (keyboard, pad buttons, pad triggers,
// right-stick dpad) and OR-ed into g_host_buttons; analogue goes to
// g_host_analog (packed lx | ly<<8). The debug socket's injected buttons
// are OR-ed in by the sceCtrl HLE.
// ---------------------------------------------------------------------------
std::atomic<uint32_t> g_host_analog{0x8080};
std::atomic<uint32_t> g_host_ranalog{0x8080};
std::atomic<int64_t> g_rstick_consumed_until_ms{0};

namespace {
constexpr uint32_t P_SELECT = 0x0001, P_START = 0x0008, P_UP = 0x0010,
    P_RIGHT = 0x0020, P_DOWN = 0x0040, P_LEFT = 0x0080, P_L = 0x0100,
    P_R = 0x0200, P_TRI = 0x1000, P_CIRCLE = 0x2000, P_CROSS = 0x4000,
    P_SQUARE = 0x8000;

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

bool g_swap_ab = false, g_arrows_dpad_only = false;

uint32_t g_kb_buttons = 0;      // keyboard keys
uint32_t g_pad_buttons = 0;     // pad digital buttons (all pads)
uint32_t g_pad_triggers = 0;    // analogue triggers as L/R
int16_t  g_rx_raw = 0, g_ry_raw = 0;  // right stick (SDL range)
uint8_t  g_kb_arrows = 0;       // bit0 up, 1 down, 2 left, 3 right
int16_t  g_lx_raw = 0, g_ly_raw = 0;  // left stick (SDL range)
std::map<SDL_JoystickID, SDL_GameController*> g_pads;

uint32_t g_trig_l_mask = P_L, g_trig_r_mask = P_R;  // PSPRECOMP_PAD_L2 / _R2
bool g_trig_l_down = false, g_trig_r_down = false;

// "R+CROSS" -> P_R | P_CROSS. Returns fallback if unset/empty or nothing parses.
uint32_t parse_button_set(const char* env, uint32_t fallback) {
    const char* v = std::getenv(env);
    if (!v || !v[0]) return fallback;
    static const struct { const char* name; uint32_t bit; } names[] = {
        {"L", P_L}, {"R", P_R}, {"CROSS", P_CROSS}, {"X", P_CROSS}, {"CIRCLE", P_CIRCLE},
        {"SQUARE", P_SQUARE}, {"TRIANGLE", P_TRI}, {"UP", P_UP}, {"DOWN", P_DOWN},
        {"LEFT", P_LEFT}, {"RIGHT", P_RIGHT}, {"START", P_START}, {"SELECT", P_SELECT},
        {"NONE", 0}};
    uint32_t mask = 0;
    bool any = false;
    const char* p = v;
    while (*p) {
        const char* e = p;
        while (*e && *e != '+' && *e != ',') ++e;
        size_t n = static_cast<size_t>(e - p);
        bool found = false;
        for (const auto& nm : names) {
            if (std::strlen(nm.name) == n && SDL_strncasecmp(nm.name, p, n) == 0) {
                mask |= nm.bit; found = any = true; break;
            }
        }
        if (!found && n)
            std::fprintf(stderr, "[RT] %s: unknown button '%.*s' ignored\n", env, static_cast<int>(n), p);
        p = *e ? e + 1 : e;
    }
    if (!any) return fallback;
    std::fprintf(stderr, "[RT] %s=%s -> psp 0x%04x\n", env, v, mask);
    return mask;
}

void update_triggers() {
    g_pad_triggers = (g_trig_l_down ? g_trig_l_mask : 0) | (g_trig_r_down ? g_trig_r_mask : 0);
}

uint32_t key_to_psp_button(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return P_UP;
        case SDLK_RIGHT:  return P_RIGHT;
        case SDLK_DOWN:   return P_DOWN;
        case SDLK_LEFT:   return P_LEFT;
        case SDLK_x:
        case SDLK_RETURN: return P_CROSS;
        case SDLK_z:      return P_CIRCLE;
        case SDLK_a:      return P_SQUARE;
        case SDLK_s:      return P_TRI;
        case SDLK_q:      return P_L;
        case SDLK_w:      return P_R;
        case SDLK_SPACE:  return P_START;
        case SDLK_TAB:    return P_SELECT;
        default:          return 0;
    }
}

uint32_t pad_button_to_psp(uint8_t b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return g_swap_ab ? P_CIRCLE : P_CROSS;
        case SDL_CONTROLLER_BUTTON_B: return g_swap_ab ? P_CROSS : P_CIRCLE;
        case SDL_CONTROLLER_BUTTON_X: return P_SQUARE;
        case SDL_CONTROLLER_BUTTON_Y: return P_TRI;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return P_L;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return P_R;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return P_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return P_RIGHT;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return P_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return P_LEFT;
        case SDL_CONTROLLER_BUTTON_START: return P_START;
        case SDL_CONTROLLER_BUTTON_BACK:  return P_SELECT;
        default: return 0;  // GUIDE etc. ignored
    }
}

// SDL axis (-32768..32767) -> PSP 0..255, centre 128, 8% deadzone.
uint8_t axis_to_psp(int16_t v) {
    int p = (static_cast<int>(v) + 32768) >> 8;
    if (p < 0) p = 0;
    if (p > 255) p = 255;
    if (std::abs(p - 128) < 10) p = 128;  // ~8% of 128
    return static_cast<uint8_t>(p);
}

uint8_t axis_to_psp_raw(int16_t v) {
    int p = (static_cast<int>(v) + 32768) >> 8;
    return static_cast<uint8_t>(p < 0 ? 0 : p > 255 ? 255 : p);
}

// Recompute the published host state from all sources.
void publish_input() {
    g_host_buttons.store(g_kb_buttons | g_pad_buttons | g_pad_triggers,
                         std::memory_order_relaxed);
    g_host_ranalog.store(static_cast<uint32_t>(axis_to_psp_raw(g_rx_raw)) |
                         (static_cast<uint32_t>(axis_to_psp_raw(g_ry_raw)) << 8),
                         std::memory_order_relaxed);
    uint8_t lx = axis_to_psp(g_lx_raw), ly = axis_to_psp(g_ly_raw);
    if (lx == 128 && ly == 128 && !g_arrows_dpad_only && g_kb_arrows) {
        if (g_kb_arrows & 4) lx = 0;
        if (g_kb_arrows & 8) lx = 255;
        if (g_kb_arrows & 1) ly = 0;
        if (g_kb_arrows & 2) ly = 255;
        if ((g_kb_arrows & 12) == 12) lx = 128;  // opposing keys cancel
        if ((g_kb_arrows & 3) == 3) ly = 128;
    }
    g_host_analog.store(lx | (static_cast<uint32_t>(ly) << 8),
                        std::memory_order_relaxed);
}

void pad_open(int device_index) {
    SDL_GameController* gc = SDL_GameControllerOpen(device_index);
    if (!gc) {
        std::fprintf(stderr, "[RT] Controller open failed (%d): %s\n",
                     device_index, SDL_GetError());
        return;
    }
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    if (g_pads.count(id)) { SDL_GameControllerClose(gc); return; }
    g_pads[id] = gc;
    char guid[40] = "";
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(gc)), guid, sizeof guid);
    char* mapping = SDL_GameControllerMapping(gc);
    std::fprintf(stderr, "[RT] Controller opened: %s (id %d, guid %s)\n[RT]   mapping: %s\n",
                 SDL_GameControllerName(gc), static_cast<int>(id), guid, mapping ? mapping : "(none)");
    if (mapping) SDL_free(mapping);
}

void pad_close(SDL_JoystickID id) {
    auto it = g_pads.find(id);
    if (it == g_pads.end()) return;
    std::fprintf(stderr, "[RT] Controller removed: %s (id %d)\n",
                 SDL_GameControllerName(it->second), static_cast<int>(id));
    SDL_GameControllerClose(it->second);
    g_pads.erase(it);
    if (g_pads.empty()) {  // nothing left to hold buttons down
        g_pad_buttons = g_pad_triggers = 0;
        g_trig_l_down = g_trig_r_down = false;
        g_lx_raw = g_ly_raw = g_rx_raw = g_ry_raw = 0;
    }
}

void pad_axis(uint8_t axis, int16_t v) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: g_lx_raw = v; break;
        case SDL_CONTROLLER_AXIS_LEFTY: g_ly_raw = v; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            g_trig_l_down = v > 16383;
            update_triggers();
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            g_trig_r_down = v > 16383;
            update_triggers();
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX: g_rx_raw = v; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: g_ry_raw = v; break;
        default: break;
    }
}

}  // namespace

static void toggle_fullscreen(SDL_Window* w) {
    if (!w) return;
    bool fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (SDL_SetWindowFullscreen(w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
        std::fprintf(stderr, "[RT] fullscreen toggle failed: %s\n", SDL_GetError());
}

// Handle one SDL event: window close, keyboard and game controller state.
static void handle_sdl_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_QUIT:
        g_should_exit.store(true);
        return;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (ev.key.repeat) return;  // ignore OS key repeat
        bool down = ev.type == SDL_KEYDOWN;
        SDL_Keycode k = ev.key.keysym.sym;
        if (down && (k == SDLK_F11 ||
                     ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (ev.key.keysym.mod & KMOD_ALT)))) {
            toggle_fullscreen(SDL_GetWindowFromID(ev.key.windowID));
            return;
        }
        uint8_t arrow = k == SDLK_UP ? 1 : k == SDLK_DOWN ? 2 :
                        k == SDLK_LEFT ? 4 : k == SDLK_RIGHT ? 8 : 0;
        if (arrow) {
            if (down) g_kb_arrows |= arrow; else g_kb_arrows &= ~arrow;
        }
        uint32_t bit = key_to_psp_button(k);
        if (bit) {
            if (down) g_kb_buttons |= bit; else g_kb_buttons &= ~bit;
        }
        if (bit || arrow) publish_input();
        return;
    }
    case SDL_CONTROLLERDEVICEADDED:
        pad_open(ev.cdevice.which);  // 'which' is a device index here
        return;
    case SDL_CONTROLLERDEVICEREMOVED:
        pad_close(ev.cdevice.which);  // instance id here
        publish_input();
        return;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        uint32_t bit = pad_button_to_psp(ev.cbutton.button);
        static const bool pad_log = env_flag("PSPRECOMP_PAD_LOG");
        if (pad_log)
            std::fprintf(stderr, "[PAD] %s %s -> psp 0x%04x\n", SDL_GameControllerGetStringForButton(
                             static_cast<SDL_GameControllerButton>(ev.cbutton.button)),
                         ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", bit);
        if (!bit) return;
        if (ev.type == SDL_CONTROLLERBUTTONDOWN) g_pad_buttons |= bit;
        else g_pad_buttons &= ~bit;
        publish_input();
        return;
    }
    case SDL_CONTROLLERAXISMOTION:
        pad_axis(ev.caxis.axis, ev.caxis.value);
        publish_input();
        return;
    case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(SDL_GetWindowFromID(ev.window.windowID), &dw, &dh);
            std::fprintf(stderr, "[RT] window %dx%d (drawable %dx%d)\n",
                         ev.window.data1, ev.window.data2, dw, dh);
        }
        if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            // KEYUP is not delivered after focus loss; release keyboard
            // state so no key sticks. Pads keep working (background events
            // are enabled).
            g_kb_buttons = 0;
            g_kb_arrows = 0;
            publish_input();
        }
        return;
    default:
        return;
    }
}

// --- Static state (SDL window + GL context, module-private) ---
static SDL_Window*  g_window     = nullptr;
static SDL_GLContext g_gl_context = nullptr;

// ---------------------------------------------------------------------------
// psp_runtime_init_sdl — create SDL window + GL 3.3 core context + GLAD
// ---------------------------------------------------------------------------
int psp_runtime_init_sdl() {
    // Record main thread id for GL_THREAD_CHECK
    g_main_thread_id = std::this_thread::get_id();

    // 1. Initialize SDL video subsystem
    g_swap_ab          = env_flag("PSPRECOMP_SWAP_AB");
    g_arrows_dpad_only = env_flag("PSPRECOMP_ARROWS_DPAD_ONLY");
    g_trig_l_mask      = parse_button_set("PSPRECOMP_PAD_L2", P_L);
    g_trig_r_mask      = parse_button_set("PSPRECOMP_PAD_R2", P_R);
    // Keep pad input flowing even when the window is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr,
            "[RT] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // 1b. Optional user gamecontrollerdb mapping file
    if (const char* db = std::getenv("PSPRECOMP_GAMECONTROLLERDB")) {
        if (db[0]) {
            int n = SDL_GameControllerAddMappingsFromFile(db);
            if (n < 0)
                std::fprintf(stderr, "[RT] gamecontrollerdb '%s': %s\n",
                             db, SDL_GetError());
            else
                std::fprintf(stderr,
                    "[RT] gamecontrollerdb '%s': %d mappings loaded\n", db, n);
        }
    }

    // 2. Set GL attributes: OpenGL 3.3 core profile, double-buffered
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // PSPRECOMP_GL_DEBUG=1: ask for a debug context so the driver reports
    // errors, undefined behaviour and performance notes (KHR_debug) to the log.
    const bool gl_debug = std::getenv("PSPRECOMP_GL_DEBUG") != nullptr;
    if (gl_debug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

    // 3. Create the window. Size: PSPRECOMP_WINDOW=<w>x<h> (explicit), else
    //    PSPRECOMP_SCALE=<n> * 480x272, else the largest integer scale that
    //    fits the desktop's usable area (min 1). Always resizable; drawable
    //    size is queried at present time (high-DPI aware).
    int win_w = 480, win_h = 272;
    {
        int ww = 0, wh = 0;
        const char* wenv = std::getenv("PSPRECOMP_WINDOW");
        const char* senv = std::getenv("PSPRECOMP_SCALE");
        if (wenv && std::sscanf(wenv, "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
            win_w = ww; win_h = wh;
        } else {
            int scale = senv ? std::atoi(senv) : 0;
            if (scale < 1) {
                SDL_Rect r{0, 0, 0, 0};
                if (SDL_GetDisplayUsableBounds(0, &r) != 0 || r.w <= 0) {
                    SDL_DisplayMode dm;
                    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { r.w = dm.w; r.h = dm.h; }
                }
                // Leave room for the title bar / dock / panels vertically.
                scale = (r.w > 0) ? std::min(r.w / 480, (r.h - 40) / 272) : 1;
                if (scale < 1) scale = 1;
            }
            win_w = 480 * scale; win_h = 272 * scale;
        }
    }
    const char* fs_env = std::getenv("PSPRECOMP_FULLSCREEN");
    uint32_t wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                      SDL_WINDOW_ALLOW_HIGHDPI;
    if (fs_env && fs_env[0] && fs_env[0] != '0') wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_window = SDL_CreateWindow(
        "PSPrecomp Runtime",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        win_w, win_h, wflags);
    if (!g_window) {
        std::fprintf(stderr,
            "[RT] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 4. Create GL context (must precede gladLoadGL — Pitfall 6)
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        std::fprintf(stderr,
            "[RT] SDL_GL_CreateContext failed: %s\n",
            SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    // 5. Load GL function pointers via GLAD2 (MUST be after context creation)
    //    GLAD2 requires a loader function; SDL2 provides SDL_GL_GetProcAddress.
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        std::fprintf(stderr, "[RT] gladLoadGL failed\n");
        SDL_GL_DeleteContext(g_gl_context);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    gl_stats_install();   // PSPRECOMP_GL_STATS=1 call counters (psp_gl_stats.cpp)

    // No driver-side vsync: the guest's vblank is simulated (hle display) and
    // a blocking swap would stall the render thread for up to a refresh.
    SDL_GL_SetSwapInterval(std::getenv("PSPRECOMP_VSYNC") ? 1 : 0);

    // 6. Print GL info for diagnostics
    std::fprintf(stderr, "[RT] GL: %s\n",
        glGetString(GL_VERSION));
    std::fprintf(stderr, "[RT] GL renderer: %s (%s)\n",
        glGetString(GL_RENDERER), glGetString(GL_VENDOR));
    if (gl_debug) gl_debug_install();

    // 7. Clear to black and swap — show initial frame
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(g_window);

    std::fprintf(stderr, "[RT] SDL/GL initialized (480x272)\n");
    return 0;
}

// ---------------------------------------------------------------------------
// psp_event_loop — main-thread loop: SDL events + render queue + alive check
// ---------------------------------------------------------------------------
void psp_event_loop(uint8_t* rdram) {
    while (!g_should_exit.load()) {
        // 1. Pump SDL events (window close, keyboard input)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_sdl_event(ev);
        }

        // 2. Drain render queue — execute pending GL work from game threads
        render_queue_process();

        // 2a. Liveness net (issue #59): present the front buffer if lists
        //     were processed but no real page-flip arrived recently. No-op
        //     when a flip just presented (not dirty / within budget).
        ge_draw_present_if_stale(rdram);

        // 2b. Service any pending debug-socket screenshot request here on
        //     the main (GL) thread — the socket thread never touches GL.
        ge_draw_service_screenshot_request();

        // 3. Check if any game threads are still alive
        if (g_alive_threads.load() == 0) {
            std::fprintf(stderr,
                "[RT] All game threads finished — lingering 3s to flush render\n");
            auto linger_end = std::chrono::steady_clock::now()
                              + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < linger_end
                   && !g_should_exit.load()) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    handle_sdl_event(ev);
                }
                render_queue_process();
                ge_draw_present_if_stale(rdram);
                ge_draw_service_screenshot_request();
                SDL_Delay(16);  // ~60fps drain rate
            }
            break;
        }

        // 4. Brief sleep — 1ms balances CPU usage vs responsiveness.
        //    Skip delay when GE lists are pending to drain them faster.
        render_queue_wait_for_work(1);
    }

    // Final drain to unblock any game thread blocked in render_queue_post
    render_queue_process();
}

// ---------------------------------------------------------------------------
// Signal handling — SIGTERM / SIGINT set g_should_exit
// ---------------------------------------------------------------------------
static void signal_handler(int sig) {
    (void)sig;
    g_should_exit.store(true);
}

void psp_install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// ---------------------------------------------------------------------------
// psp_runtime_shutdown — clean shutdown: threads, render queue, SDL/GL
// ---------------------------------------------------------------------------
SDL_Window* psp_get_sdl_window() {
    return g_window;
}

void psp_runtime_shutdown() {
    // 1. Ensure exit flag is set (in case called without signal)
    g_should_exit.store(true);

    // 2. Shutdown scheduler — wake all threads, join with timeout
    psp_scheduler_shutdown();

    // 3. Final render queue drain
    render_queue_process();

    // 4. Destroy GL context
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = nullptr;
    }

    // 5. Destroy SDL window
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }

    // 5b. Close controllers
    for (auto& kv : g_pads) SDL_GameControllerClose(kv.second);
    g_pads.clear();

    // 6. Quit SDL
    SDL_Quit();

    std::fprintf(stderr, "[RT] Shutdown complete\n");
}
