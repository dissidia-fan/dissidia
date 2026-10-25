#pragma once
// Minimal host audio sink: per-source FIFOs mixed to one 44.1kHz stereo s16
// stream played through SDL2 audio. Fed by the sceAudio* HLE handlers.
//
// Env:
//   PSPRECOMP_AUDIO=0            disable the sink entirely
//   PSPRECOMP_AUDIO_DUMP=<path>  append the mixed s16le stereo 44.1kHz PCM to
//                                <path> (raw, no header) for offline checks
//                                (works with SDL_AUDIODRIVER=dummy too)
#include <cstddef>
#include <cstdint>

enum : int {
    PSP_AUDIO_SRC_CHANNEL0 = 0,   // sceAudioCh* channels 0..7
    PSP_AUDIO_SRC_OUTPUT2  = 8,   // sceAudioOutput2*
    PSP_AUDIO_SRC_COUNT    = 9,
};

// Queue `frames` frames. `pcm` is interleaved s16: stereo if channels==2,
// mono (duplicated to both sides) if channels==1. volL/volR use the PSP scale
// (0x8000 = unity). Never blocks. Safe from any thread.
void psp_audio_push(int source, const int16_t* pcm, int frames, int channels,
                    int volL, int volR);

// Block the caller for one `frames`-frame grain of `source`. If the grain was
// queued (queued=true) and the sink is live this waits for the sink to drain
// (real backpressure); otherwise it sleeps to an absolute timeline.
//   PSPRECOMP_AUDIO_LAT_MS=N         queue depth held, in ms (default 64)
//   PSPRECOMP_AUDIO_DEV_SAMPLES=N    SDL device buffer in frames (default 1024)
//   PSPRECOMP_AUDIO_STATS=1          [AUDIO] underrun/ring-fill line every 5 s
void psp_audio_pace(int source, int frames, bool queued);

// Frames currently queued for `source` (sceAudioGetChannelRestLength).
size_t psp_audio_queued_frames(int source);
