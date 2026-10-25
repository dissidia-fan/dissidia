#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// ================================================================
// SAS (Software Audio Synthesis) voice renderer, modelled on PPSSPP's
// Core/HW/SasAudio.cpp semantics: 32 voices, VAG-ADPCM / PCM / noise
// sources, linear-interpolated pitch (0x1000 = 1.0), ADSR envelope,
// per-voice stereo volume, key on/off, pause, end flags.
//
// Voice lifetime also drives __sceSasGetEndFlag (issue #29: the game's
// sound-completion gate polls it, so voices must really end).
// Reverb is not implemented (settings are accepted, logged once).
//
// Argument convention: PSP syscalls pass a0..a3 in ctx->r[4..7] and
// the 5th+ args in t0.. (ctx->r[8..]) -- same as sceIoLseek's whence
// in psp_hle_io.cpp.
// ================================================================

namespace {

constexpr int SAS_VOICE_COUNT = 32;
constexpr int SAS_PITCH_BASE = 0x1000;   // pitch 0x1000 = 1.0x
constexpr int SAS_PITCH_MAX = 0x4000;
constexpr int SAS_GRAIN_MAX = 2048;
constexpr int32_t SAS_ENV_MAX = 0x40000000;

// PSP curve modes (SetADSRmode)
enum { CURVE_LIN_INC = 0, CURVE_LIN_DEC = 1, CURVE_BENT = 2,
       CURVE_EXP_DEC = 3, CURVE_EXP_INC = 4, CURVE_DIRECT = 5 };
enum EnvState { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum SrcType { SRC_NONE, SRC_VAG, SRC_PCM, SRC_NOISE };

bool g_trace = false;
bool g_trace_init = false;
bool g_logged_keyon = false;
bool g_logged_reverb = false;

/// One envelope stage: rate = number of samples for a full-scale ramp
/// (0 = instant), mode = curve type.
struct Stage {
    int64_t rate = 0;
    int mode = CURVE_LIN_INC;
};

struct SasVoice {
    // source
    SrcType type = SRC_NONE;
    uint32_t addr = 0;
    uint32_t size = 0;          // bytes (VAG) / samples (PCM)
    bool loop = false;
    bool noise = false;
    // decoder
    uint32_t block = 0;         // VAG: next block index to decode
    uint32_t loop_block = 0;
    int vs1 = 0, vs2 = 0;
    int16_t buf[28];
    int buf_pos = 28;           // 28 = empty
    bool src_end = false;
    uint32_t pcm_pos = 0;
    uint32_t noise_state = 0x12345678;
    // resampler
    int32_t s0 = 0, s1 = 0;
    uint32_t frac = 0;          // 16.16
    bool tail = false;
    uint32_t pitch = SAS_PITCH_BASE;
    // state
    bool playing = false;
    bool paused = false;
    bool keyed = false;
    // volume (0x1000 = 1.0)
    int32_t vol_l = 0x1000, vol_r = 0x1000;
    int32_t fx_l = 0, fx_r = 0;
    // envelope
    Stage st[4];                // attack, decay, sustain, release
    int64_t sustain_level = SAS_ENV_MAX;
    int env_state = ENV_OFF;
    int64_t height = 0;

    SasVoice() {
        st[0] = {0, CURVE_LIN_INC};
        st[1] = {0, CURVE_EXP_DEC};
        st[2] = {0, CURVE_LIN_DEC};   // rate 0 = hold
        st[3] = {0, CURVE_LIN_DEC};
        std::memset(buf, 0, sizeof(buf));
    }
};

SasVoice g_voices[SAS_VOICE_COUNT];
int g_grain = 256;
int g_outmode = 0;
// SAS calls come from the PCM thread while voice setup may come from
// other game threads -- guard all state with one mutex.
std::mutex g_sas_mtx;

bool trace_on() {
    if (!g_trace_init) {
        const char* e = std::getenv("PSPRECOMP_SAS_TRACE");
        g_trace = (e != nullptr && e[0] == '1');
        g_trace_init = true;
    }
    return g_trace;
}

#define SAS_TRACE(...) do { if (trace_on()) { \
    std::fprintf(stderr, "[SAS] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

/// Voice index is always a1. Returns nullptr for out-of-range voices.
SasVoice* voice_arg(recomp_context* ctx) {
    uint32_t v = static_cast<uint32_t>(ctx->r[5]);
    if (v >= SAS_VOICE_COUNT) return nullptr;
    return &g_voices[v];
}

const uint8_t* guest_ptr(uint8_t* rdram, uint32_t addr, uint32_t len) {
    uint32_t off = addr & PSP_ADDR_MASK;
    if (off + len > PSP_MEM_SIZE || off + len < off) return nullptr;
    return rdram + off;
}

// ---------------- sources ----------------

const int VAG_F[16][2] = {
    {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}};

void vag_restart(SasVoice& v) {
    v.block = 0; v.loop_block = 0;
    v.vs1 = v.vs2 = 0;
    v.buf_pos = 28;
    v.src_end = false;
    v.pcm_pos = 0;
}

/// Decode the next 16-byte block (PPSSPP VagDecoder semantics).
/// Returns false when the stream has ended (no samples produced).
bool vag_decode_block(uint8_t* rdram, SasVoice& v) {
    if (v.src_end) return false;
    uint32_t nblocks = v.size / 16;
    if (v.size != 0 && v.block >= nblocks) {
        if (v.loop) v.block = v.loop_block;
        else { v.src_end = true; return false; }
        if (v.block >= nblocks) { v.src_end = true; return false; }
    }
    const uint8_t* p = guest_ptr(rdram, v.addr + v.block * 16U, 16);
    if (p == nullptr) { v.src_end = true; return false; }
    int predict = p[0] >> 4;
    int shift = p[0] & 0xF;
    int flags = p[1];
    if (flags == 7) { v.src_end = true; return false; }   // VAG_END_NOW
    if (flags == 6) v.loop_block = v.block;               // loop start
    for (int i = 0; i < 14; i++) {
        uint8_t d = p[2 + i];
        for (int j = 0; j < 2; j++) {
            int nib = j == 0 ? (d & 0xF) : (d >> 4);
            int sample = static_cast<int16_t>(nib << 12) >> shift;
            sample += (v.vs1 * VAG_F[predict][0] + v.vs2 * VAG_F[predict][1]) >> 6;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            v.buf[i * 2 + j] = static_cast<int16_t>(sample);
            v.vs2 = v.vs1;
            v.vs1 = sample;
        }
    }
    v.buf_pos = 0;
    v.block++;
    if (flags == 3) {                 // loop end
        if (v.loop) v.block = v.loop_block;
        else v.src_end = true;
    } else if (flags == 1) {          // end
        v.src_end = true;
    }
    return true;
}

/// Pull one source sample. Returns false at end of data.
bool pull_sample(uint8_t* rdram, SasVoice& v, int32_t& out) {
    if (v.noise) {
        v.noise_state ^= v.noise_state << 13;
        v.noise_state ^= v.noise_state >> 17;
        v.noise_state ^= v.noise_state << 5;
        out = static_cast<int16_t>(v.noise_state >> 8) / 2;
        return true;
    }
    if (v.type == SRC_VAG) {
        if (v.buf_pos >= 28) {
            if (!vag_decode_block(rdram, v)) return false;
        }
        out = v.buf[v.buf_pos++];
        return true;
    }
    if (v.type == SRC_PCM) {
        if (v.pcm_pos >= v.size) {
            if (v.loop && v.size > 0) v.pcm_pos = 0;
            else return false;
        }
        const uint8_t* p = guest_ptr(rdram, v.addr + v.pcm_pos * 2U, 2);
        if (p == nullptr) return false;
        out = static_cast<int16_t>(p[0] | (p[1] << 8));
        v.pcm_pos++;
        return true;
    }
    return false;
}

// ---------------- envelope ----------------

/// Convert an SPU-style 7-bit rate (n) into samples for a full-scale
/// linear ramp. dec selects the decrease step table.
int64_t simple_rate_to_samples(int n, bool dec) {
    n &= 0x7F;
    int shift = n >> 2;
    int step = dec ? (8 - (n & 3)) : (7 - (n & 3));
    int64_t cycles = 1LL << (shift > 11 ? shift - 11 : 0);
    int64_t delta = static_cast<int64_t>(step) << (shift < 11 ? 11 - shift : 0);
    int64_t t = (0x8000LL * cycles) / delta;
    return t < 1 ? 1 : t;
}

void env_enter(SasVoice& v, int state) {
    v.env_state = state;
}

void voice_end(SasVoice& v) {
    v.playing = false;
    v.keyed = false;
    v.env_state = ENV_OFF;
    v.height = 0;
}

void voice_key_on(SasVoice& v) {
    vag_restart(v);
    v.s0 = v.s1 = 0;
    v.frac = 0;
    v.tail = false;
    v.height = 0;
    v.playing = true;
    v.keyed = true;
    env_enter(v, ENV_ATTACK);
    // prime the resampler: s1 holds the first sample (s0 = 0 lead-in)
}

/// One envelope step (called per output sample).
void env_step(SasVoice& v) {
    switch (v.env_state) {
    case ENV_ATTACK: {
        const Stage& s = v.st[0];
        if (s.rate <= 0) { v.height = SAS_ENV_MAX; env_enter(v, ENV_DECAY); break; }
        int64_t inc = SAS_ENV_MAX / s.rate;
        if (inc < 1) inc = 1;
        if ((s.mode == CURVE_BENT || s.mode == CURVE_EXP_INC) &&
            v.height > (SAS_ENV_MAX / 4) * 3)
            inc = inc / 4 > 0 ? inc / 4 : 1;
        v.height += inc;
        if (v.height >= SAS_ENV_MAX) { v.height = SAS_ENV_MAX; env_enter(v, ENV_DECAY); }
        break;
    }
    case ENV_DECAY: {
        const Stage& s = v.st[1];
        if (s.rate <= 0 || v.height <= v.sustain_level) {
            if (s.rate <= 0 || v.height < v.sustain_level) v.height = v.sustain_level;
            env_enter(v, ENV_SUSTAIN);
            break;
        }
        int64_t dec;
        if (s.mode == CURVE_EXP_DEC) {
            dec = (v.height + s.rate - 1) / s.rate;
        } else {
            dec = SAS_ENV_MAX / s.rate;
        }
        if (dec < 1) dec = 1;
        v.height -= dec;
        if (v.height <= v.sustain_level) {
            v.height = v.sustain_level;
            env_enter(v, ENV_SUSTAIN);
        }
        break;
    }
    case ENV_SUSTAIN: {
        const Stage& s = v.st[2];
        if (s.rate <= 0) break;                    // hold
        bool inc_dir = (s.mode == CURVE_LIN_INC || s.mode == CURVE_BENT ||
                        s.mode == CURVE_EXP_INC);
        if (inc_dir) {
            int64_t inc = SAS_ENV_MAX / s.rate;
            if (inc < 1) inc = 1;
            if (s.mode != CURVE_LIN_INC && v.height > (SAS_ENV_MAX / 4) * 3)
                inc = inc / 4 > 0 ? inc / 4 : 1;
            v.height += inc;
            if (v.height > SAS_ENV_MAX) v.height = SAS_ENV_MAX;
        } else {
            int64_t dec = (s.mode == CURVE_EXP_DEC)
                ? (v.height + s.rate - 1) / s.rate : SAS_ENV_MAX / s.rate;
            if (dec < 1) dec = 1;
            v.height -= dec;
            if (v.height <= 0) { voice_end(v); }
        }
        break;
    }
    case ENV_RELEASE: {
        const Stage& s = v.st[3];
        if (s.rate <= 0) { voice_end(v); break; }
        int64_t dec = (s.mode == CURVE_EXP_DEC)
            ? (v.height + s.rate - 1) / s.rate : SAS_ENV_MAX / s.rate;
        if (dec < 1) dec = 1;
        v.height -= dec;
        if (v.height <= 0) voice_end(v);
        break;
    }
    default: break;
    }
}

/// Render one voice for `frames` samples, adding into mix_l/mix_r.
void render_voice(uint8_t* rdram, SasVoice& v, int frames,
                  int32_t* mix_l, int32_t* mix_r) {
    for (int i = 0; i < frames && v.playing; i++) {
        // interpolate between s0 and s1
        int32_t smp = v.s0 + static_cast<int32_t>(
            (static_cast<int64_t>(v.s1 - v.s0) * v.frac) >> 16);
        int32_t env = static_cast<int32_t>(v.height >> 14);  // 0..0x10000
        int64_t scaled = (static_cast<int64_t>(smp) * env) >> 16;
        mix_l[i] += static_cast<int32_t>((scaled * v.vol_l) >> 12);
        mix_r[i] += static_cast<int32_t>((scaled * v.vol_r) >> 12);

        env_step(v);
        if (!v.playing) break;

        v.frac += v.pitch << 4;
        while (v.frac >= 0x10000) {
            v.frac -= 0x10000;
            v.s0 = v.s1;
            if (v.tail) { voice_end(v); break; }
            int32_t n = 0;
            if (pull_sample(rdram, v, n)) v.s1 = n;
            else { v.s1 = 0; v.tail = true; }
        }
    }
}

/// Run all voices for one grain into stereo mix buffers (grain frames).
void sas_render(uint8_t* rdram, int32_t* mix_l, int32_t* mix_r) {
    int frames = g_grain;
    for (int i = 0; i < frames; i++) mix_l[i] = mix_r[i] = 0;
    for (int vi = 0; vi < SAS_VOICE_COUNT; vi++) {
        SasVoice& v = g_voices[vi];
        if (!v.playing || v.paused) continue;
        render_voice(rdram, v, frames, mix_l, mix_r);
    }
}

int16_t clamp16(int32_t x) {
    return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

// ---- HLE handlers ----

void hle_sceSasInit(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, grain=a1, maxVoices=a2, outMode=a3, sampleRate=t0)
    int32_t grain = ctx->r[5];
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    for (auto& v : g_voices) v = SasVoice{};
    if (grain > 0 && grain <= SAS_GRAIN_MAX) g_grain = grain;
    g_outmode = ctx->r[7];
    SAS_TRACE("Init grain=%d maxVoices=%d outMode=%d rate=%d",
              g_grain, ctx->r[6], g_outmode, ctx->r[8]);
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetGrain(uint8_t* rdram, recomp_context* ctx) {
    int32_t grain = ctx->r[5];
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    if (grain > 0 && grain <= SAS_GRAIN_MAX) g_grain = grain;
    SAS_TRACE("SetGrain %d", grain);
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasGetGrain(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ctx->r[2] = g_grain;
    (void)rdram;
}

void hle_sceSasSetOutputmode(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_outmode = ctx->r[5];
    (void)rdram;
}

void hle_sceSasGetOutputmode(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ctx->r[2] = g_outmode;
    (void)rdram;
}

void hle_sceSasSetVoice(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, vagAddr=a2, size=a3, loop=t0)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[6]);
        uint32_t size = static_cast<uint32_t>(ctx->r[7]);
        // Tolerate a leading "VAGp" file header (0x30 bytes).
        const uint8_t* h = guest_ptr(rdram, addr, 4);
        if (h != nullptr && std::memcmp(h, "VAGp", 4) == 0) {
            addr += 0x30;
            if (size >= 0x30) size -= 0x30;
        }
        v->type = SRC_VAG;
        v->noise = false;
        v->addr = addr;
        v->size = size;
        v->loop = ctx->r[8] != 0;
        vag_restart(*v);
        SAS_TRACE("SetVoice v=%d addr=%08x size=%u loop=%d",
                  ctx->r[5], addr, size, ctx->r[8]);
    }
    ctx->r[2] = 0;
}

void hle_sceSasSetVoicePCM(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, pcmAddr=a2, sampleCount=a3, loop=t0)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        v->type = SRC_PCM;
        v->noise = false;
        v->addr = static_cast<uint32_t>(ctx->r[6]);
        v->size = static_cast<uint32_t>(ctx->r[7]);
        v->loop = ctx->r[8] != 0;
        vag_restart(*v);
        SAS_TRACE("SetVoicePCM v=%d addr=%08x n=%u loop=%d",
                  ctx->r[5], v->addr, v->size, ctx->r[8]);
    }
    ctx->r[2] = 0;
}

void hle_sceSasSetPitch(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, pitch=a2)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        int32_t pitch = ctx->r[6];
        if (pitch < 1) pitch = 1;
        if (pitch > SAS_PITCH_MAX) pitch = SAS_PITCH_MAX;
        v->pitch = static_cast<uint32_t>(pitch);
        SAS_TRACE("SetPitch v=%d pitch=%04x", ctx->r[5], pitch);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetVolume(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, l=a2, r=a3, effectL=t0, effectR=t1)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        v->vol_l = ctx->r[6];
        v->vol_r = ctx->r[7];
        v->fx_l = ctx->r[8];
        v->fx_r = ctx->r[9];
        SAS_TRACE("SetVolume v=%d l=%d r=%d fxl=%d fxr=%d", ctx->r[5],
                  v->vol_l, v->vol_r, v->fx_l, v->fx_r);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetNoise(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, freq=a2) -- freq ignored; white noise source.
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        v->noise = true;
        SAS_TRACE("SetNoise v=%d freq=%d", ctx->r[5], ctx->r[6]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetADSR(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, flag=a2, attack=a3, decay=t0, sustain=t1,
    //  release=t2): raw rates = samples for a full-scale ramp.
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        int flag = ctx->r[6];
        const int32_t* rates[4] = {&ctx->r[7], &ctx->r[8], &ctx->r[9], &ctx->r[10]};
        for (int i = 0; i < 4; i++) {
            if (flag & (1 << i)) {
                int64_t r = static_cast<uint32_t>(*rates[i]);
                v->st[i].rate = r;
            }
        }
        SAS_TRACE("SetADSR v=%d flag=%x a=%d d=%d s=%d r=%d", ctx->r[5], flag,
                  ctx->r[7], ctx->r[8], ctx->r[9], ctx->r[10]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetADSRmode(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, flag=a2, attackMode=a3, decayMode=t0,
    //  sustainMode=t1, releaseMode=t2)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        int flag = ctx->r[6];
        const int32_t* modes[4] = {&ctx->r[7], &ctx->r[8], &ctx->r[9], &ctx->r[10]};
        for (int i = 0; i < 4; i++) {
            if (flag & (1 << i)) v->st[i].mode = *modes[i];
        }
        SAS_TRACE("SetADSRmode v=%d flag=%x a=%d d=%d s=%d r=%d", ctx->r[5],
                  flag, ctx->r[7], ctx->r[8], ctx->r[9], ctx->r[10]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetSL(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, sustainLevel=a2) 0..0x40000000
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        int64_t sl = ctx->r[6];
        if (sl < 0) sl = 0;
        if (sl > SAS_ENV_MAX) sl = SAS_ENV_MAX;
        v->sustain_level = sl;
        SAS_TRACE("SetSL v=%d %08x", ctx->r[5], ctx->r[6]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetSimpleADSR(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, env1=a2, env2=a3): SPU-style 16-bit encoding.
    //  env1: bit15 attack bent, 14..8 attack rate, 7..4 decay shift,
    //        3..0 sustain level.
    //  env2: bit15 sustain exp, bit14 sustain decrease, 13..6 sustain rate
    //        (7 bits used), bit5 release exp, 4..0 release shift.
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        uint32_t e1 = static_cast<uint32_t>(ctx->r[6]) & 0xFFFF;
        uint32_t e2 = static_cast<uint32_t>(ctx->r[7]) & 0xFFFF;
        bool att_bent = (e1 & 0x8000) != 0;
        v->st[0].mode = att_bent ? CURVE_BENT : CURVE_LIN_INC;
        v->st[0].rate = simple_rate_to_samples((e1 >> 8) & 0x7F, false);
        v->st[1].mode = CURVE_EXP_DEC;
        v->st[1].rate = simple_rate_to_samples(((e1 >> 4) & 0xF) << 2, true);
        v->sustain_level = static_cast<int64_t>((e1 & 0xF) + 1) * (SAS_ENV_MAX / 16);
        bool s_dec = (e2 & 0x4000) != 0, s_exp = (e2 & 0x8000) != 0;
        int sn = (e2 >> 6) & 0x7F;
        v->st[2].mode = s_dec ? (s_exp ? CURVE_EXP_DEC : CURVE_LIN_DEC)
                              : (s_exp ? CURVE_BENT : CURVE_LIN_INC);
        v->st[2].rate = (sn == 0x7F) ? 0 : simple_rate_to_samples(sn, s_dec);
        v->st[3].mode = (e2 & 0x20) ? CURVE_EXP_DEC : CURVE_LIN_DEC;
        v->st[3].rate = simple_rate_to_samples((e2 & 0x1F) << 2, true);
        SAS_TRACE("SetSimpleADSR v=%d e1=%04x e2=%04x", ctx->r[5], e1, e2);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetKeyOn(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr) {
        if (!g_logged_keyon) {
            g_logged_keyon = true;
            std::fprintf(stderr,
                "[SAS] first key-on: voice=%d format=%s addr=%08x size=%u "
                "loop=%d pitch=%04x vol=%d/%d\n",
                ctx->r[5],
                v->noise ? "noise" : v->type == SRC_VAG ? "VAG-ADPCM"
                  : v->type == SRC_PCM ? "PCM16" : "none",
                v->addr, v->size, v->loop ? 1 : 0, v->pitch,
                v->vol_l, v->vol_r);
        }
        voice_key_on(*v);
        SAS_TRACE("KeyOn v=%d", ctx->r[5]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasSetKeyOff(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    if (v != nullptr && v->playing) {
        v->keyed = false;
        env_enter(*v, ENV_RELEASE);
        SAS_TRACE("KeyOff v=%d", ctx->r[5]);
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void note_reverb(const char* what) {
    if (!g_logged_reverb) {
        g_logged_reverb = true;
        std::fprintf(stderr, "[SAS] reverb (%s) not implemented; ignored\n", what);
    }
}

void hle_sas_rev(uint8_t* rdram, recomp_context* ctx) {
    note_reverb("Rev*");
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

void hle_sceSasCore(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, out=a1)
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    uint32_t out = static_cast<uint32_t>(ctx->r[5]);
    int32_t ml[SAS_GRAIN_MAX], mr[SAS_GRAIN_MAX];
    sas_render(rdram, ml, mr);
    uint32_t off = out & PSP_ADDR_MASK;
    uint32_t frames = static_cast<uint32_t>(g_grain);
    if (out != 0 && off + frames * 4U <= PSP_MEM_SIZE) {
        int16_t* p = reinterpret_cast<int16_t*>(rdram + off);
        for (uint32_t i = 0; i < frames; i++) {
            p[i * 2] = clamp16(ml[i]);
            p[i * 2 + 1] = clamp16(mr[i]);
        }
    }
    ctx->r[2] = 0;
}

void hle_sceSasCoreWithMix(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, inout=a1, leftVol=a2, rightVol=a3)
    // The in/out buffer holds the game's own mix (e.g. decoded ATRAC
    // PCM): it is scaled by the mix volumes (0x1000 = 1.0) and the
    // rendered voices are added on top, clamped.
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    uint32_t out = static_cast<uint32_t>(ctx->r[5]);
    int32_t lvol = ctx->r[6], rvol = ctx->r[7];
    int32_t ml[SAS_GRAIN_MAX], mr[SAS_GRAIN_MAX];
    sas_render(rdram, ml, mr);
    uint32_t off = out & PSP_ADDR_MASK;
    uint32_t frames = static_cast<uint32_t>(g_grain);
    if (out != 0 && off + frames * 4U <= PSP_MEM_SIZE) {
        int16_t* p = reinterpret_cast<int16_t*>(rdram + off);
        for (uint32_t i = 0; i < frames; i++) {
            int32_t l = ((p[i * 2] * lvol) >> 12) + ml[i];
            int32_t r = ((p[i * 2 + 1] * rvol) >> 12) + mr[i];
            p[i * 2] = clamp16(l);
            p[i * 2 + 1] = clamp16(r);
        }
    }
    ctx->r[2] = 0;
}

void hle_sceSasGetEndFlag(uint8_t* rdram, recomp_context* ctx) {
    // Bit i set iff voice i is NOT playing (initially 0xFFFFFFFF).
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    uint32_t mask = 0;
    for (int i = 0; i < SAS_VOICE_COUNT; i++) {
        if (!g_voices[i].playing) mask |= (1U << i);
    }
    ctx->r[2] = static_cast<int32_t>(mask);
    (void)rdram;
}

void hle_sceSasGetPauseFlag(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    uint32_t mask = 0;
    for (int i = 0; i < SAS_VOICE_COUNT; i++) {
        if (g_voices[i].paused) mask |= (1U << i);
    }
    ctx->r[2] = static_cast<int32_t>(mask);
    (void)rdram;
}

void hle_sceSasSetPause(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voiceMask=a1, pause=a2)
    uint32_t voice_mask = static_cast<uint32_t>(ctx->r[5]);
    bool pause = ctx->r[6] != 0;
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    for (int i = 0; i < SAS_VOICE_COUNT; i++) {
        if (voice_mask & (1U << i)) g_voices[i].paused = pause;
    }
    SAS_TRACE("SetPause mask=%08x pause=%d", voice_mask, pause);
    ctx->r[2] = 0;
    (void)rdram;
}

void hle_sceSasGetEnvelopeHeight(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice* v = voice_arg(ctx);
    ctx->r[2] = v != nullptr ? static_cast<int32_t>(v->height) : 0;
    (void)rdram;
}

void hle_sceSasGetAllEnvelopeHeights(
    uint8_t* rdram, recomp_context* ctx
) {
    // (core=a0, heightsPtr=a1) -- write 32 ints.
    uint32_t ptr = static_cast<uint32_t>(ctx->r[5]);
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    if (ptr != 0) {
        for (int i = 0; i < SAS_VOICE_COUNT; i++) {
            psp_mem_write<int32_t>(rdram, ptr + i * 4U,
                static_cast<int32_t>(g_voices[i].height));
        }
    }
    ctx->r[2] = 0;
}

/// Wave-shape / ATRAC3 voice setters: accepted, no effect.
void hle_sas_noop(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

}  // namespace

// ---- Registration ----

void psp_hle_register_sas() {
    psp_hle_register("__sceSasInit", hle_sceSasInit);
    psp_hle_register("__sceSasCore", hle_sceSasCore);
    psp_hle_register("__sceSasCoreWithMix", hle_sceSasCoreWithMix);
    psp_hle_register("__sceSasSetVoice", hle_sceSasSetVoice);
    psp_hle_register("__sceSasSetPitch", hle_sceSasSetPitch);
    psp_hle_register("__sceSasSetKeyOn", hle_sceSasSetKeyOn);
    psp_hle_register("__sceSasSetKeyOff", hle_sceSasSetKeyOff);
    psp_hle_register("__sceSasSetPause", hle_sceSasSetPause);
    psp_hle_register("__sceSasSetGrain", hle_sceSasSetGrain);
    psp_hle_register("__sceSasGetGrain", hle_sceSasGetGrain);
    psp_hle_register("__sceSasGetEndFlag", hle_sceSasGetEndFlag);
    psp_hle_register("__sceSasGetPauseFlag", hle_sceSasGetPauseFlag);
    psp_hle_register("__sceSasGetEnvelopeHeight",
                      hle_sceSasGetEnvelopeHeight);
    psp_hle_register("__sceSasGetAllEnvelopeHeights",
                      hle_sceSasGetAllEnvelopeHeights);
    psp_hle_register("__sceSasSetVoicePCM", hle_sceSasSetVoicePCM);
    psp_hle_register("__sceSasSetVolume", hle_sceSasSetVolume);
    psp_hle_register("__sceSasSetADSR", hle_sceSasSetADSR);
    psp_hle_register("__sceSasSetADSRmode", hle_sceSasSetADSRmode);
    psp_hle_register("__sceSasSetSL", hle_sceSasSetSL);
    psp_hle_register("__sceSasSetSimpleADSR", hle_sceSasSetSimpleADSR);
    psp_hle_register("__sceSasSetNoise", hle_sceSasSetNoise);
    psp_hle_register("__sceSasRevParam", hle_sas_rev);
    psp_hle_register("__sceSasRevType", hle_sas_rev);
    psp_hle_register("__sceSasRevEVOL", hle_sas_rev);
    psp_hle_register("__sceSasRevVON", hle_sas_rev);
    psp_hle_register("__sceSasGetOutputmode", hle_sceSasGetOutputmode);
    psp_hle_register("__sceSasSetOutputmode", hle_sceSasSetOutputmode);
    psp_hle_register("__sceSasSetTriangularWave", hle_sas_noop);
    psp_hle_register("__sceSasSetSteepWave", hle_sas_noop);
    psp_hle_register("__sceSasSetVoiceATRAC3", hle_sas_noop);
    psp_hle_register("__sceSasConcatenateATRAC3", hle_sas_noop);
    psp_hle_register("__sceSasUnsetATRAC3", hle_sas_noop);
}
