#include "hle/psp_audio_sink.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int SINK_RATE = 44100;
// Drop (rather than grow) beyond ~0.5 s of backlog per source.
constexpr size_t MAX_BACKLOG_FRAMES = SINK_RATE / 2;

struct Source {
    std::vector<int16_t> fifo;  // interleaved stereo
    size_t head = 0;            // read offset in int16 units
    // A source only starts draining once `prime` frames are queued (and keeps
    // draining until it runs dry), so a producer that is a little bursty
    // gets one clean re-buffer instead of a stream of micro-dropouts.
    bool primed = false;
    size_t grain = 0;           // frames of the most recent push
    Clock::time_point last_push{};
    // Non-audio-driven pacing timeline (sink disabled, or nothing to queue).
    Clock::time_point deadline{};
    // Stats
    uint64_t pushed_frames = 0;
    uint64_t pushes = 0, underruns = 0, short_frames = 0, overruns = 0;
    uint64_t wait_us = 0, waits = 0, wait_timeouts = 0;
    int64_t push_gap_max_us = 0;
    size_t fill_min = SIZE_MAX, fill_max = 0;
    size_t frames_queued() const { return (fifo.size() - head) / 2; }
};

std::mutex g_mtx;
std::condition_variable g_cv;  // signalled by the SDL callback after consuming
Source g_src[PSP_AUDIO_SRC_COUNT];
std::once_flag g_init_once;
SDL_AudioDeviceID g_dev = 0;
bool g_enabled = true;
FILE* g_dump = nullptr;
uint64_t g_dump_frames = 0, g_dump_nonzero = 0;
// Queue depth (ms) the blocking calls hold the producer at (at least one
// grain). The PSP keeps ~1 grain in flight; the host game loop runs at ~30 fps
// with multi-ms stalls, so keep a little more than that queued.
int g_lat_ms = 64;
int g_dev_samples = 1024;
bool g_stats = false;
uint64_t g_cb_count = 0, g_clip_frames = 0, g_cb_late = 0;
int64_t g_cb_gap_max_us = 0;
Clock::time_point g_cb_last{}, g_stat_last{};

void print_stats_locked() {
    std::fprintf(stderr,
                 "[AUDIO] cb=%llu late_cb=%llu cb_gap_max=%.1fms clip=%llu dumped=%.1fs\n",
                 (unsigned long long)g_cb_count, (unsigned long long)g_cb_late,
                 g_cb_gap_max_us / 1000.0, (unsigned long long)g_clip_frames,
                 g_dump_frames / double(SINK_RATE));
    for (int i = 0; i < PSP_AUDIO_SRC_COUNT; i++) {
        Source& s = g_src[i];
        if (!s.pushes) continue;
        std::fprintf(stderr,
                     "[AUDIO]  src%d grain=%zu pushed=%.1fs pushes=%llu underruns=%llu short=%llu overruns=%llu "
                     "fill=%zu..%zu now=%zu push_gap_max=%.1fms wait_avg=%.2fms timeouts=%llu\n",
                     i, s.grain, s.pushed_frames / double(SINK_RATE),
                     (unsigned long long)s.pushes, (unsigned long long)s.underruns,
                     (unsigned long long)s.short_frames, (unsigned long long)s.overruns,
                     s.fill_min == SIZE_MAX ? 0 : s.fill_min, s.fill_max, s.frames_queued(),
                     s.push_gap_max_us / 1000.0,
                     s.waits ? s.wait_us / 1000.0 / s.waits : 0.0,
                     (unsigned long long)s.wait_timeouts);
        s.fill_min = SIZE_MAX; s.fill_max = 0; s.push_gap_max_us = 0;
    }
}

void mix(int16_t* out, int frames) {
    static std::vector<int32_t> acc;
    acc.assign(static_cast<size_t>(frames) * 2, 0);
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& s : g_src) {
            size_t avail = s.frames_queued();
            if (!s.primed) {
                // Start draining once ~2 grains are queued, or once the
                // producer has gone quiet (play out a short tail).
                bool quiet = now - s.last_push > std::chrono::milliseconds(40);
                if (avail && (avail >= s.grain * 2 || quiet)) s.primed = true;
                else continue;
            }
            size_t n = std::min(avail, static_cast<size_t>(frames));
            if (g_stats) { s.fill_min = std::min(s.fill_min, avail); s.fill_max = std::max(s.fill_max, avail); }
            for (size_t i = 0; i < n * 2; i++) acc[i] += s.fifo[s.head + i];
            s.head += n * 2;
            if (n < static_cast<size_t>(frames)) {
                // Ran dry. It is an underrun only if the producer is still
                // live (pushed within the last 100 ms), not a sound that ended.
                if (now - s.last_push < std::chrono::milliseconds(100)) {
                    s.underruns++;
                    s.short_frames += static_cast<uint64_t>(frames) - n;
                }
                s.primed = false;
            }
            if (s.head == s.fifo.size()) { s.fifo.clear(); s.head = 0; }
            else if (s.head > 65536) {
                s.fifo.erase(s.fifo.begin(), s.fifo.begin() + static_cast<long>(s.head));
                s.head = 0;
            }
        }
    }
    bool clipped = false;
    for (size_t i = 0; i < acc.size(); i++) {
        int v = acc[i];
        if (v > 32767) { v = 32767; clipped = true; }
        else if (v < -32768) { v = -32768; clipped = true; }
        out[i] = static_cast<int16_t>(v);
    }
    if (clipped) g_clip_frames++;
}

void SDLCALL audio_cb(void*, Uint8* stream, int len) {
    int frames = len / 4;
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    const auto t0 = Clock::now();
    mix(out, frames);
    if (g_dump) {
        std::fwrite(out, 4, static_cast<size_t>(frames), g_dump);
        g_dump_frames += static_cast<uint64_t>(frames);
        for (int i = 0; i < frames * 2; i++) if (out[i]) { g_dump_nonzero++; }
    }
    g_cb_count++;
    if (g_cb_last.time_since_epoch().count()) {
        int64_t gap = std::chrono::duration_cast<std::chrono::microseconds>(t0 - g_cb_last).count();
        g_cb_gap_max_us = std::max(g_cb_gap_max_us, gap);
        // A callback arriving > 2x its period late is a device/CPU hiccup.
        if (gap > 2LL * frames * 1000000 / SINK_RATE) g_cb_late++;
    }
    g_cb_last = t0;
    g_cv.notify_all();
    if (g_stats && t0 - g_stat_last > std::chrono::seconds(5)) {
        g_stat_last = t0;
        std::lock_guard<std::mutex> lk(g_mtx);
        print_stats_locked();
        g_cb_gap_max_us = 0;
    }
}

bool open_device() {
    SDL_AudioSpec want{}, have{};
    want.freq = SINK_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = static_cast<Uint16>(g_dev_samples);
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) return false;
    if (have.freq != SINK_RATE || have.format != AUDIO_S16SYS || have.channels != 2) {
        // Never expect this (allowed_changes = 0 makes SDL convert), but be safe.
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

void init_sink() {
    const char* en = std::getenv("PSPRECOMP_AUDIO");
    if (en && std::strcmp(en, "0") == 0) { g_enabled = false; return; }
    if (const char* v = std::getenv("PSPRECOMP_AUDIO_LAT_MS")) g_lat_ms = std::max(0, std::atoi(v));
    if (const char* v = std::getenv("PSPRECOMP_AUDIO_DEV_SAMPLES")) g_dev_samples = std::max(128, std::atoi(v));
    if (const char* v = std::getenv("PSPRECOMP_AUDIO_STATS")) g_stats = *v && *v != '0';
    if (const char* dump = std::getenv("PSPRECOMP_AUDIO_DUMP")) {
        if (*dump) g_dump = std::fopen(dump, "ab");
        if (!g_dump) std::fprintf(stderr, "[AUDIO] cannot open PSPRECOMP_AUDIO_DUMP=%s\n", dump);
    }
    bool ok = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 && open_device();
    if (!ok) {
        std::fprintf(stderr, "[AUDIO] SDL audio unavailable (%s); retrying with the dummy driver\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        ok = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 && open_device();
    }
    if (!ok) {
        std::fprintf(stderr, "[AUDIO] no audio output: %s\n", SDL_GetError());
        g_enabled = false;
        return;
    }
    g_stat_last = Clock::now();
    std::fprintf(stderr, "[AUDIO] sink open: driver=%s%s dev_samples=%d lat_ms=%d%s\n",
                 SDL_GetCurrentAudioDriver(), g_dump ? " (dumping)" : "", g_dev_samples,
                 g_lat_ms, g_stats ? " stats=on" : "");
}

}  // namespace

void psp_audio_push(int source, const int16_t* pcm, int frames, int channels, int volL, int volR) {
    if (!pcm || frames <= 0 || source < 0 || source >= PSP_AUDIO_SRC_COUNT) return;
    std::call_once(g_init_once, init_sink);
    if (!g_enabled) return;
    volL = std::max(0, std::min(volL, 0x8000));
    volR = std::max(0, std::min(volR, 0x8000));
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(g_mtx);
    Source& s = g_src[source];
    s.pushes++;
    if (s.last_push.time_since_epoch().count()) {
        int64_t gap = std::chrono::duration_cast<std::chrono::microseconds>(now - s.last_push).count();
        s.push_gap_max_us = std::max(s.push_gap_max_us, gap);
    }
    s.last_push = now;
    s.grain = static_cast<size_t>(frames);
    s.pushed_frames += static_cast<uint64_t>(frames);
    if (s.frames_queued() + static_cast<size_t>(frames) > MAX_BACKLOG_FRAMES) { s.overruns++; return; }
    size_t base = s.fifo.size();
    s.fifo.resize(base + static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; i++) {
        int l, r;
        if (channels == 1) { l = r = pcm[i]; }
        else { l = pcm[i * 2]; r = pcm[i * 2 + 1]; }
        s.fifo[base + i * 2]     = static_cast<int16_t>((l * volL) >> 15);
        s.fifo[base + i * 2 + 1] = static_cast<int16_t>((r * volR) >> 15);
    }
}

// Pace the caller. Preferred: hold it until the sink has drained `source` to
// at most g_lat_ms of audio, i.e. pace against the real consumer, so the
// producer can neither starve nor overrun the ring however long its own work
// (decode, a slow 30 fps frame) takes. Fallback (sink off, or nothing was
// queued): sleep to an absolute per-source timeline, which also absorbs the
// caller's own compute time instead of adding a full grain on top of it.
void psp_audio_pace(int source, int frames, bool queued) {
    if (frames <= 0 || source < 0 || source >= PSP_AUDIO_SRC_COUNT) return;
    std::call_once(g_init_once, init_sink);
    const auto period = std::chrono::microseconds(int64_t(frames) * 1000000 / SINK_RATE);
    Source& s = g_src[source];
    if (g_enabled && queued) {
        const auto t0 = Clock::now();
        std::unique_lock<std::mutex> lk(g_mtx);
        const size_t target = std::max(static_cast<size_t>(frames),
                                       static_cast<size_t>(g_lat_ms) * SINK_RATE / 1000);
        bool ok = g_cv.wait_for(lk, period * 4 + std::chrono::milliseconds(g_lat_ms),
                                [&] { return s.frames_queued() <= target; });
        s.waits++;
        s.wait_us += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        if (!ok) s.wait_timeouts++;
        return;
    }
    std::unique_lock<std::mutex> lk(g_mtx);
    auto now = Clock::now();
    if (s.deadline < now - std::chrono::milliseconds(100)) s.deadline = now;  // resync after a long gap
    s.deadline += period;
    auto dl = s.deadline;
    lk.unlock();
    std::this_thread::sleep_until(dl);
}

size_t psp_audio_queued_frames(int source) {
    if (source < 0 || source >= PSP_AUDIO_SRC_COUNT) return 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_src[source].frames_queued();
}
