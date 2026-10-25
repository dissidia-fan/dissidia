// games/dissidia/runtime/camera.cpp — analogue right-stick camera for Dissidia 012 battles.
//
// How the battle camera works (reverse-engineered, see games/dissidia/CAMERA.md):
//   * The camera manager (a0 of ovl_battle FUN_08B3CA24, one per battle; 0x09AE8C70 in our runs)
//     is itself a camera object: view matrix at +0x00, eye +0x40, target +0x50, up +0x60, proj at
//     +0xC0.... FUN_08B3CA24 runs once per frame: it blends up to 8 camera-mode slots
//     (objects at manager+0x6D0[i]; d-pad rotation, lock-on framing, ... all live in those modes),
//     producing a final eye/target/up on its own stack (sp+0xA0 eye, sp+0xB0 target, sp+0x30 up),
//     then calls   SetEye(mgr,&eye) SetTarget(mgr,&tgt) SetUp(mgr,&up) BuildLookAt(mgr)   (FUN_0885EAA8 /
//     EAC4 / EAE0 / E860), adds a shake offset, repeats the four calls, and copies the result to the render
//     camera (FUN_088A5F1C).
//   * We wrap FUN_08B3CA24 (to learn the manager and run once per frame) and the EBOOT's SetEye
//     (FUN_0885EAA8): on the first SetEye(mgr, ...) of each update we rotate the eye vector, in place on
//     the guest stack, about the target (which sits 0x10 after the eye on the same stack frame) by the
//     accumulated stick offset. Everything downstream (lookAt, shake, render copy, movement-relative-to-camera
//     code that reads the manager's matrix) then sees the rotated camera. The game's own state (d-pad
//     rotation, lock-on, auto-centring, collision pull-in) is untouched: the stick is an offset on top.
//
// Options: PSPRECOMP_CAM_FREE=0 disables (stick then acts as D-pad, see psp_hle_ctrl.cpp), PSPRECOMP_CAM_SENS
// (1.0 = 150 deg/s yaw at full deflection), PSPRECOMP_CAM_INVERT_Y=1, PSPRECOMP_CAM_PITCH_MIN/MAX (deg,
// elevation of the eye above the target; default -5..80), PSPRECOMP_CAM_RECENTER=<seconds> (time constant
// for the offset to drift back to zero while the stick is idle; default 0 = hold),
// PSPRECOMP_CAM_LOG=1 is the GE-level log; PSPRECOMP_CAMX_LOG=<n> logs n updates of this module.

#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_extra_lookup(uint32_t vaddr);
extern uint32_t g_dissidia_cine_actor, g_dissidia_cine_seq;   // roster.cpp (hitcam=)
extern int g_dissidia_cine_ticks;

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t CAM_UPDATE = 0x08B3CA24u;   // ovl_battle: per-frame camera manager update
constexpr uint32_t CAM_SETEYE = 0x0885EAA8u;   // EBOOT: camera->eye = *a1

uint8_t* g_ram = nullptr;
FuncPtr g_orig_update = nullptr, g_orig_seteye = nullptr;
uint32_t g_mgr = 0;
bool g_in_update = false;
int g_eye_calls = 0;
bool g_free = true, g_invert = false;
float g_sens = 1.0f, g_pitch_min = -5.f, g_pitch_max = 80.f, g_recenter = 0.f;
float g_yaw = 0.f, g_pitch = 0.f;   // accumulated offsets (radians)
double g_last_t = 0;
int g_log_left = 0;
// hitcam= framing (roster.cpp posts the request when a hitact= action starts)
bool g_cine_on = true;
uint32_t g_cine_seen = 0, g_cine_actor = 0;
double g_cine_t0 = 0, g_cine_len = 0;
float g_cine_side = 0.f;
int g_cine_logs = 0;
constexpr uint32_t ACT_POS = 0x80u, ACT_TARGET = 0x2FCu;    // actor position (vec3), its target actor (roster.cpp)

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t now_ms() { return static_cast<int64_t>(now_s() * 1000.0); }

float rdf(uint32_t a) { float f; std::memcpy(&f, g_ram + (a & 0x07FFFFFFu), 4); return f; }
void wrf(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &f, 4); }

bool ram_ok(uint32_t a, uint32_t n) { return a >= 0x08800000u && a + n <= 0x0C000000u; }
bool rdvec(uint32_t a, float* v) {
    if (!ram_ok(a, 12)) return false;
    for (int i = 0; i < 3; i++) { v[i] = rdf(a + 4 * i); if (!std::isfinite(v[i]) || std::fabs(v[i]) > 2000.f) return false; }
    return true;
}
float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

// The hitcam framing weight now (0 = the game's camera), and the eye / look-at it wants; false when off.
// A push-in on the game camera's own line: eye = Noctis's chest - (35 % closer) along the game eye's direction, at
// least 2.5 m away, and backed out to stay >= 12 m from the Alterna sphere's centre (foe + 3 m; v8h, v8j, v8k).
// Look-at = his chest + 35 % of the way to the foe's centre, 0.3 m up (Alterna v8f: at 60 % / 0.6 m the look-at
// followed the foe held up in the sphere and Noctis's raised-arm cast and throw left the frame; at 35 % he stays in
// it, the sphere behind him, and the downward slam keeps the foe in view as it falls).
bool cine_frame(const float* eye, const float* tgt, float* E, float* F, float* w) {
    (void)tgt;
    if (!g_cine_on || !g_cine_actor) return false;
    const double t = now_s() - g_cine_t0;
    if (t < 0 || t > g_cine_len) { g_cine_actor = 0; return false; }
    *w = std::fmin(smooth01(static_cast<float>(t / 0.25)), smooth01(static_cast<float>((g_cine_len - t) / 0.4)));
    float P[3], T[3];
    uint32_t tg;
    if (!rdvec(g_cine_actor + ACT_POS, P)) return false;
    if (!ram_ok(g_cine_actor + ACT_TARGET, 4)) return false;
    std::memcpy(&tg, g_ram + ((g_cine_actor + ACT_TARGET) & 0x07FFFFFFu), 4);
    if (!ram_ok(tg, 0x100) || !rdvec(tg + ACT_POS, T)) return false;
    const float C[3] = {P[0], P[1] + 1.2f, P[2]};          // Noctis's chest
    float v[3] = {eye[0] - C[0], eye[1] - C[1], eye[2] - C[2]};
    const float L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (L < 1e-3f) return false;
    float L2 = std::fmin(L, std::fmax(2.5f, 0.65f * L));         // 35 % closer, not under 2.5 m
    // Alterna v8h-v8k: the glass sphere (radius 3 m, its shards out to 3.15 m) is centred 3 m above the foe;
    // a push-in that brings the eye within kSphereClear of that centre puts the lens in / against the shards
    // (23:20: the screen filled with the crack lattice). Back the eye out along the same line until it clears,
    // at most 30 % past the game's own distance.
    constexpr float kSphereClear = 12.0f;
    const float S[3] = {T[0], T[1] + 3.0f, T[2]};
    auto dist_s = [&](float l) {
        float d2 = 0.f;
        for (int i = 0; i < 3; i++) { const float e = C[i] + v[i] / L * l - S[i]; d2 += e * e; }
        return std::sqrt(d2);
    };
    while (dist_s(L2) < kSphereClear && L2 < 1.3f * L) L2 = std::fmin(1.3f * L, L2 + 0.25f);
    for (int i = 0; i < 3; i++) E[i] = C[i] + v[i] / L * L2;
    for (int i = 0; i < 3; i++) F[i] = C[i] + 0.35f * ((T[i] + (i == 1 ? 1.3f : 0.f)) - C[i]);
    F[1] += 0.3f;
    if (g_cine_logs < 4) {
        g_cine_logs++;
        std::fprintf(stderr, "[HITCAM] push-in on actor %08X: eye %.1f -> %.1f m from him, %.1f m from the sphere, look-at (%.1f,%.1f,%.1f), w %.2f\n",
                     g_cine_actor, L, L2, dist_s(L2), F[0], F[1], F[2], *w);
    }
    return true;
}

uint32_t overlay_id() { uint32_t id; std::memcpy(&id, g_ram + (OVL_BASE & 0x07FFFFFFu), 4); return id; }

// CAM_UPDATE is a battle-overlay address; the menu/extra overlays map other code there.
void dissidia_cam_update(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = overlay_id();
    if (id != 1) {
        FuncPtr f = id == 2 ? ovl_menu_lookup(CAM_UPDATE) : id == 3 ? ovl_extra_lookup(CAM_UPDATE) : nullptr;
        if (f) f(rdram, ctx);
        return;
    }
    const double t = now_s();
    double dt = t - g_last_t;
    if (g_last_t == 0 || dt > 1.0) { g_yaw = g_pitch = 0.f; dt = 0; }   // new battle / long pause: reset
    g_last_t = t;
    if (dt > 0.1) dt = 0.1;
    g_mgr = static_cast<uint32_t>(ctx->r[4]);
    float sx = 0, sy = 0;
    bool active = psp_ctrl_right_stick(&sx, &sy);
    g_rstick_consumed_until_ms.store(now_ms() + 400, std::memory_order_relaxed);
    constexpr float kRate = 2.6f;   // rad/s at full deflection and sens 1.0
    if (active) {
        g_yaw   += sx * g_sens * kRate * static_cast<float>(dt);
        g_pitch += (g_invert ? -sy : sy) * g_sens * kRate * 0.6f * static_cast<float>(dt);
    } else if (g_recenter > 0.f && dt > 0) {
        float k = std::exp(-static_cast<float>(dt) / g_recenter);
        g_yaw *= k; g_pitch *= k;
    }
    // Yaw is unbounded but kept in (-pi, pi].
    const float PI = 3.14159265f;
    if (g_yaw > PI) g_yaw -= 2 * PI; else if (g_yaw < -PI) g_yaw += 2 * PI;
    if (g_dissidia_cine_seq != g_cine_seen) {         // a new hitcam= request
        g_cine_seen = g_dissidia_cine_seq;
        g_cine_actor = g_dissidia_cine_actor; g_cine_t0 = t; g_cine_len = g_dissidia_cine_ticks / 60.0; g_cine_side = 0.f;
    }
    g_in_update = true; g_eye_calls = 0;
    g_orig_update(rdram, ctx);
    g_in_update = false;
}

// SetEye(cam, vec): rotate vec about the target stored 0x10 after it, once per update.
void dissidia_cam_seteye(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cam = static_cast<uint32_t>(ctx->r[4]), eye = static_cast<uint32_t>(ctx->r[5]);
    float E[3], F[3], w = 0.f;
    if (g_in_update && cam == g_mgr && g_eye_calls == 0) {
        float ev[3] = {rdf(eye), rdf(eye + 4), rdf(eye + 8)}, tv[3] = {rdf(eye + 0x10), rdf(eye + 0x14), rdf(eye + 0x18)};
        if (cine_frame(ev, tv, E, F, &w) && w > 0.f) {    // hitcam=: frame the attacker and his target from the side
            for (int i = 0; i < 3; i++) {
                wrf(eye + 4 * i, ev[i] + (E[i] - ev[i]) * w);
                wrf(eye + 0x10 + 4 * i, tv[i] + (F[i] - tv[i]) * w);
            }
            g_eye_calls++;
        }
    }
    if (g_in_update && cam == g_mgr && g_eye_calls++ == 0 && (g_yaw != 0.f || g_pitch != 0.f)) {
        const uint32_t tg = eye + 0x10;
        float ex = rdf(eye), ey = rdf(eye + 4), ez = rdf(eye + 8);
        float tx = rdf(tg), ty = rdf(tg + 4), tz = rdf(tg + 8);
        float dx = ex - tx, dy = ey - ty, dz = ez - tz;
        float r = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (r > 1e-3f) {
            float horiz = std::sqrt(dx * dx + dz * dz);
            float az = std::atan2(dx, dz);               // yaw of the eye around the target
            float el = std::atan2(dy, horiz);            // elevation
            const float d2r = 3.14159265f / 180.f;
            float nel = el + g_pitch;
            float lo = g_pitch_min * d2r, hi = g_pitch_max * d2r;
            // Clamp the *result*, but never fight the game if it is already outside our window.
            float lo_e = std::fmin(lo, el), hi_e = std::fmax(hi, el);
            if (nel < lo_e) nel = lo_e; else if (nel > hi_e) nel = hi_e;
            g_pitch = nel - el;   // keep the accumulator inside the window (no wind-up)
            float naz = az + g_yaw;
            float ch = std::cos(nel) * r;
            wrf(eye,     tx + std::sin(naz) * ch);
            wrf(eye + 4, ty + std::sin(nel) * r);
            wrf(eye + 8, tz + std::cos(naz) * ch);
        }
    }
    if (g_log_left > 0 && g_in_update && cam == g_mgr && g_eye_calls == 1) {
        g_log_left--;
        float ex = rdf(eye), ey = rdf(eye + 4), ez = rdf(eye + 8);
        float tx = rdf(eye + 0x10), ty = rdf(eye + 0x14), tz = rdf(eye + 0x18);
        std::fprintf(stderr, "[CAMX] yaw=%.2fdeg pitch_off=%.2fdeg yaw_off=%.2fdeg eye=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f)\n",
            std::atan2(ex - tx, ez - tz) * 57.29578f, g_pitch * 57.29578f, g_yaw * 57.29578f, ex, ey, ez, tx, ty, tz);
    }
    g_orig_seteye(rdram, ctx);
}

}  // namespace

void dissidia_camera_install(uint8_t* rdram) {
    const char* e = std::getenv("PSPRECOMP_HITCAM");
    g_cine_on = !(e && e[0] == '0');
    e = std::getenv("PSPRECOMP_CAM_FREE");
    g_free = !(e && e[0] == '0');
    if (!g_free) { std::fprintf(stderr, "[CAM] analogue camera disabled (PSPRECOMP_CAM_FREE=0)\n"); return; }
    g_ram = rdram;
    if ((e = std::getenv("PSPRECOMP_CAM_SENS"))) g_sens = static_cast<float>(std::atof(e));
    g_invert = (e = std::getenv("PSPRECOMP_CAM_INVERT_Y")) && e[0] && e[0] != '0';
    if ((e = std::getenv("PSPRECOMP_CAM_PITCH_MIN"))) g_pitch_min = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_PITCH_MAX"))) g_pitch_max = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAM_RECENTER"))) g_recenter = static_cast<float>(std::atof(e));
    if ((e = std::getenv("PSPRECOMP_CAMX_LOG"))) g_log_left = std::atoi(e);
    g_orig_update = ovl_battle_lookup(CAM_UPDATE);
    g_orig_seteye = psp_dispatch_probe_lookup(CAM_SETEYE);
    if (!g_orig_update || !g_orig_seteye) {
        std::fprintf(stderr, "[CAM] hooks NOT installed (update %p seteye %p)\n", (void*)g_orig_update, (void*)g_orig_seteye);
        return;
    }
    psp_dispatch_register(CAM_UPDATE, dissidia_cam_update);
    psp_dispatch_register(CAM_SETEYE, dissidia_cam_seteye);
    std::fprintf(stderr, "[CAM] analogue camera installed (sens %.2f%s, pitch %.0f..%.0f deg, recenter %.1fs)\n",
                 g_sens, g_invert ? ", inverted" : "", g_pitch_min, g_pitch_max, g_recenter);
}
