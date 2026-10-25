#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef PSPRECOMP_HAVE_AVCODEC
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
#endif

// ================================================================
// sceAtrac3plus HLE: ATRAC3 / ATRAC3+ decoding through libavcodec.
//
// Semantics follow PPSSPP's Core/HLE/sceAtrac.cpp + AtracCtx.cpp (the
// "v1" Atrac class): a context owns a RIFF/WAVE (.at3) image in guest
// memory, DecodeData emits ONE codec frame per call (1024 samples for
// ATRAC3, 2048 for ATRAC3+) as interleaved stereo s16, skipping the
// leading firstSampleOffset + codec-delay samples, and sets the finish
// flag when the last sample (fact - 1) has been produced.
//
// Buffer states supported:
//   ALL_DATA_LOADED  - whole file in memory.
//   HALFWAY_BUFFER   - whole-file-sized buffer, partially filled;
//                      AddStreamData appends, then it becomes ALL.
//   STREAMED         - small ring buffer; GetStreamDataInfo/AddStreamData
//                      refill it. A looping stream (SetLoopNum != 0 and a
//                      smpl loop) asks for the loop start's file offset once
//                      the end of the data is in the ring, as PPSSPP's
//                      STREAMED_LOOP_FROM_END does; the ring remembers which
//                      file frames it holds (`segs`), so the decoder skips
//                      the tail past the loop end and carries on from the
//                      loop start. (Dissidia's battle BGM is streamed; without
//                      this it went silent ~100 s into every battle.)
//                      Loop-with-trailer (second buffer) is not modelled.
// Without PSPRECOMP_HAVE_AVCODEC the bookkeeping is identical but
// DecodeData writes silence.
// ================================================================

namespace {

// PPSSPP ErrorCodes.h
constexpr uint32_t ERR_NO_ATRACID          = 0x80630003;
constexpr uint32_t ERR_INVALID_CODECTYPE   = 0x80630004;
constexpr uint32_t ERR_BAD_ATRACID         = 0x80630005;
constexpr uint32_t ERR_UNKNOWN_FORMAT      = 0x80630006;
constexpr uint32_t ERR_WRONG_CODECTYPE     = 0x80630007;
constexpr uint32_t ERR_ALL_DATA_LOADED     = 0x80630009;
constexpr uint32_t ERR_NO_DATA             = 0x80630010;
constexpr uint32_t ERR_SIZE_TOO_SMALL      = 0x80630011;
constexpr uint32_t ERR_INCORRECT_READ_SIZE = 0x80630013;
constexpr uint32_t ERR_BAD_ALIGNMENT       = 0x80630014;
constexpr uint32_t ERR_BAD_SAMPLE          = 0x80630015;
constexpr uint32_t ERR_BAD_FIRST_RESET     = 0x80630016;
constexpr uint32_t ERR_BAD_SECOND_RESET    = 0x80630017;
constexpr uint32_t ERR_ADD_DATA_TOO_BIG    = 0x80630018;
constexpr uint32_t ERR_NO_LOOP_INFO        = 0x80630021;
constexpr uint32_t ERR_SECOND_BUF_NOT_NEED = 0x80630022;
constexpr uint32_t ERR_ALL_DATA_DECODED    = 0x80630024;
constexpr uint32_t ERR_ILLEGAL_ADDRESS     = 0x80000023;

constexpr int CODEC_AT3PLUS = 0x1000;
constexpr int CODEC_AT3     = 0x1001;

enum AtracState {
    ST_NO_DATA = 1,
    ST_ALL_DATA_LOADED = 2,
    ST_HALFWAY_BUFFER = 3,
    ST_STREAMED = 4,
};

constexpr int32_t REMAIN_ALLDATA_ON_MEMORY = -1;
constexpr int32_t REMAIN_NONLOOP_ON_MEMORY = -2;

constexpr int MAX_ATRAC_IDS = 8;

struct AtracCtx {
    bool used = false;
    int codec = 0;              // CODEC_AT3 / CODEC_AT3PLUS
    int state = ST_NO_DATA;

    // Track (from RIFF headers)
    int channels = 2;
    int outChannels = 2;
    uint32_t sampleRate = 44100;
    uint32_t blockAlign = 0;
    uint32_t fileSize = 0;
    uint32_t dataOff = 0;
    uint32_t dataSize = 0;
    int firstSampleOffset = 0;
    int endSample = 0;          // last playable sample index (fact - 1)
    int loopStart = -1;         // raw smpl values (decoded-stream units)
    int loopEnd = -1;
    std::vector<uint8_t> extradata;

    // Guest buffer
    uint32_t buffer = 0;
    uint32_t bufferSize = 0;
    uint32_t readSize = 0;      // bytes of file valid in the buffer (ALL/HALFWAY)

    // Playback
    int currentSample = 0;
    int loopNum = 0;

    // Streamed ring
    uint32_t ringPos = 0, ringValid = 0, fileRead = 0;
    std::vector<uint8_t> frame0;  // priming frame copy (streamed)
    struct Seg { uint32_t fileOff, len; };
    std::vector<Seg> segs;      // file ranges held in the ring, in ring order from ringPos
    int64_t lastDec = -1;       // file frame index last fed to the decoder (streamed)

    // Decoder
    int decNext = 0;            // index of the next frame the decoder expects
    std::vector<int16_t> pcm;   // last decoded frame (spf * outChannels)
#ifdef PSPRECOMP_HAVE_AVCODEC
    AVCodecContext* av = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frm = nullptr;
#endif

    int spf() const { return codec == CODEC_AT3PLUS ? 2048 : 1024; }
    int extra() const { return codec == CODEC_AT3PLUS ? 0x170 : 0x45; }
    int offsetFull() const { return firstSampleOffset + extra(); }
    uint32_t dataEnd() const { return std::min<uint32_t>(fileSize, dataOff + dataSize); }
    bool loops() const { return loopNum != 0 && loopStart >= 0; }
    // File offset a looping stream rereads from: one frame before the loop start's frame (decoder priming).
    uint32_t loopFileOff() const {
        int64_t k = std::max<int64_t>(0, static_cast<int64_t>(loopStart) / spf() - 1);
        return dataOff + static_cast<uint32_t>(k) * blockAlign;
    }
    // Bytes the stream reads up to before wrapping (looping) or stopping.
    uint32_t streamLimit() const { return loops() ? dataEnd() : fileSize; }
    void segPush(uint32_t off, uint32_t len) {
        if (!len) return;
        if (!segs.empty() && segs.back().fileOff + segs.back().len == off) segs.back().len += len;
        else segs.push_back({off, len});
    }
};

std::mutex g_mtx;
AtracCtx g_ctx[MAX_ATRAC_IDS];

AtracCtx* get_ctx(int id) {
    if (id < 0 || id >= MAX_ATRAC_IDS || !g_ctx[id].used) return nullptr;
    return &g_ctx[id];
}

// ---------------- decoder wrapper ----------------

void dec_close(AtracCtx& c) {
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (c.frm) av_frame_free(&c.frm);
    if (c.pkt) av_packet_free(&c.pkt);
    if (c.av) avcodec_free_context(&c.av);
#endif
    c.decNext = 0;
}

bool dec_open(AtracCtx& c) {
    dec_close(c);
    c.pcm.assign(static_cast<size_t>(c.spf()) * c.outChannels, 0);
#ifdef PSPRECOMP_HAVE_AVCODEC
    const AVCodec* codec = avcodec_find_decoder(
        c.codec == CODEC_AT3PLUS ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    if (!codec) { std::fprintf(stderr, "[ATRAC] libavcodec has no decoder\n"); return false; }
    c.av = avcodec_alloc_context3(codec);
    if (!c.av) return false;
    c.av->sample_rate = static_cast<int>(c.sampleRate);
    c.av->block_align = static_cast<int>(c.blockAlign);
    av_channel_layout_default(&c.av->ch_layout, c.channels);
    if (c.codec == CODEC_AT3) {
        // ATRAC3 needs the 14-byte WAVE extradata. Use the file's, else
        // build the same one PPSSPP builds.
        uint8_t ex[14] = {0};
        if (c.extradata.size() == 14) {
            std::memcpy(ex, c.extradata.data(), 14);
        } else {
            bool joint = c.blockAlign != 0x60u * c.channels;
            ex[0] = 1; ex[3] = static_cast<uint8_t>(c.channels << 3);
            ex[6] = joint; ex[8] = joint; ex[10] = 1;
        }
        c.av->extradata = static_cast<uint8_t*>(av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(c.av->extradata, ex, 14);
        c.av->extradata_size = 14;
    }
    if (avcodec_open2(c.av, codec, nullptr) < 0) {
        std::fprintf(stderr, "[ATRAC] avcodec_open2 failed (codec=%#x ch=%d align=%u)\n",
                     c.codec, c.channels, c.blockAlign);
        avcodec_free_context(&c.av);
        return false;
    }
    c.pkt = av_packet_alloc();
    c.frm = av_frame_alloc();
    return c.pkt && c.frm;
#else
    return true;
#endif
}

void dec_flush(AtracCtx& c) {
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (c.av) avcodec_flush_buffers(c.av);
#else
    (void)c;
#endif
}

#ifdef PSPRECOMP_HAVE_AVCODEC
inline int16_t f2s16(float v) {
    long x = std::lrintf(v * 32768.0f);
    return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}
#endif

// Decode one codec frame (blockAlign bytes at `data`) into c.pcm. A null
// `data` means "no data available": silence. Returns false on decode error.
bool dec_frame(AtracCtx& c, const uint8_t* data) {
    const size_t n = static_cast<size_t>(c.spf()) * c.outChannels;
    if (c.pcm.size() != n) c.pcm.assign(n, 0);
    if (!data) { std::fill(c.pcm.begin(), c.pcm.end(), 0); return true; }
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (!c.av) { std::fill(c.pcm.begin(), c.pcm.end(), 0); return false; }
    std::vector<uint8_t> buf(c.blockAlign + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(buf.data(), data, c.blockAlign);
    c.pkt->data = buf.data();
    c.pkt->size = static_cast<int>(c.blockAlign);
    int r = avcodec_send_packet(c.av, c.pkt);
    if (r < 0) { std::fill(c.pcm.begin(), c.pcm.end(), 0); return false; }
    r = avcodec_receive_frame(c.av, c.frm);
    if (r < 0) { std::fill(c.pcm.begin(), c.pcm.end(), 0); return false; }
    const int ns = std::min(c.frm->nb_samples, c.spf());
    const int srcCh = c.frm->ch_layout.nb_channels;
    std::fill(c.pcm.begin(), c.pcm.end(), 0);
    for (int ch = 0; ch < c.outChannels; ch++) {
        int sc = std::min(ch, srcCh - 1);
        for (int i = 0; i < ns; i++) {
            int16_t s = 0;
            switch (static_cast<AVSampleFormat>(c.frm->format)) {
            case AV_SAMPLE_FMT_FLTP: s = f2s16(reinterpret_cast<const float*>(c.frm->data[sc])[i]); break;
            case AV_SAMPLE_FMT_FLT:  s = f2s16(reinterpret_cast<const float*>(c.frm->data[0])[i * srcCh + sc]); break;
            case AV_SAMPLE_FMT_S16P: s = reinterpret_cast<const int16_t*>(c.frm->data[sc])[i]; break;
            case AV_SAMPLE_FMT_S16:  s = reinterpret_cast<const int16_t*>(c.frm->data[0])[i * srcCh + sc]; break;
            default: break;
            }
            c.pcm[static_cast<size_t>(i) * c.outChannels + ch] = s;
        }
    }
    av_frame_unref(c.frm);
    return true;
#else
    std::fill(c.pcm.begin(), c.pcm.end(), 0);
    return true;
#endif
}

// ---------------- RIFF analysis ----------------

struct Track {
    int codec = 0, channels = 0;
    uint32_t sampleRate = 0, blockAlign = 0, fileSize = 0, dataOff = 0, dataSize = 0;
    int firstSampleOffset = 0, endSample = 0, loopStart = -1, loopEnd = -1;
    std::vector<uint8_t> extradata;
};

inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// `d` has `avail` readable bytes (the guest buffer's valid part).
uint32_t analyze(const uint8_t* d, uint32_t avail, Track& t) {
    if (!d || avail < 72) return ERR_SIZE_TOO_SMALL;
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return ERR_UNKNOWN_FORMAT;
    t.fileSize = rd32(d + 4) + 8;
    uint32_t off = 12;
    bool haveFmt = false, haveData = false;
    uint32_t factSamples = 0;
    while (off + 8 <= avail) {
        uint32_t csz = rd32(d + off + 4);
        const uint8_t* body = d + off + 8;
        uint32_t bodyAvail = avail - (off + 8);
        if (std::memcmp(d + off, "fmt ", 4) == 0 && csz >= 16 && bodyAvail >= 16) {
            uint16_t tag = rd16(body);
            t.channels = rd16(body + 2);
            t.sampleRate = rd32(body + 4);
            t.blockAlign = rd16(body + 12);
            if (tag == 0x270) t.codec = CODEC_AT3;
            else if (tag == 0xFFFE) t.codec = CODEC_AT3PLUS;
            else return ERR_UNKNOWN_FORMAT;
            if (csz >= 18 && bodyAvail >= 18) {
                uint32_t cb = rd16(body + 16);
                if (cb && 18 + cb <= std::min(csz, bodyAvail))
                    t.extradata.assign(body + 18, body + 18 + cb);
            }
            haveFmt = true;
        } else if (std::memcmp(d + off, "fact", 4) == 0 && csz >= 4 && bodyAvail >= 4) {
            factSamples = rd32(body);
            if (csz >= 8 && bodyAvail >= 8) t.firstSampleOffset = static_cast<int>(rd32(body + 4));
        } else if (std::memcmp(d + off, "smpl", 4) == 0 && csz >= 36 + 24 && bodyAvail >= 36 + 24) {
            if (rd32(body + 28) >= 1) {  // numSampleLoops
                t.loopStart = static_cast<int>(rd32(body + 36 + 8));
                t.loopEnd = static_cast<int>(rd32(body + 36 + 12));
            }
        } else if (std::memcmp(d + off, "data", 4) == 0) {
            t.dataOff = off + 8;
            t.dataSize = csz;
            haveData = true;
            break;
        }
        off += 8 + ((csz + 1) & ~1u);
    }
    if (!haveFmt || !haveData) return ERR_UNKNOWN_FORMAT;
    if (t.channels < 1 || t.channels > 2 || t.blockAlign == 0) return ERR_UNKNOWN_FORMAT;
    if (t.sampleRate == 0) t.sampleRate = 44100;
    if (t.fileSize < t.dataOff) return ERR_UNKNOWN_FORMAT;
    if (t.dataSize > t.fileSize - t.dataOff) t.dataSize = t.fileSize - t.dataOff;
    const int spf = t.codec == CODEC_AT3PLUS ? 2048 : 1024;
    if (factSamples == 0) {
        // No fact chunk: derive from the data size (PPSSPP does the same).
        t.endSample = static_cast<int>((t.dataSize / t.blockAlign) * spf) - 1;
    } else {
        t.endSample = static_cast<int>(factSamples) - 1;
    }
    return 0;
}

// ---------------- context setup ----------------

uint32_t ctx_set_data(AtracCtx& c, const Track& t, uint32_t buffer, uint32_t readSize,
                      uint32_t bufferSize, int outCh, const uint8_t* rdram) {
    if (readSize < 72) return ERR_SIZE_TOO_SMALL;
    c.codec = t.codec;
    c.channels = t.channels;
    c.outChannels = outCh;
    c.sampleRate = t.sampleRate;
    c.blockAlign = t.blockAlign;
    c.fileSize = t.fileSize;
    c.dataOff = t.dataOff;
    c.dataSize = t.dataSize;
    c.firstSampleOffset = t.firstSampleOffset;
    c.endSample = t.endSample;
    c.loopStart = t.loopStart;
    c.loopEnd = t.loopEnd;
    c.extradata = t.extradata;
    c.buffer = buffer;
    c.bufferSize = bufferSize;
    c.readSize = std::min(readSize, t.fileSize);
    c.currentSample = 0;
    c.loopNum = 0;
    c.frame0.clear();

    if (bufferSize >= t.fileSize) {
        c.state = c.readSize >= t.fileSize ? ST_ALL_DATA_LOADED : ST_HALFWAY_BUFFER;
    } else {
        if (c.readSize < t.dataOff || c.readSize - t.dataOff < 2u * t.blockAlign) return ERR_SIZE_TOO_SMALL;
        c.state = ST_STREAMED;
        // PPSSPP treats the first (priming) frame as already consumed.
        c.ringPos = t.dataOff + t.blockAlign;
        c.ringValid = c.readSize - c.ringPos;
        c.fileRead = c.readSize;
        const uint8_t* b = rdram + (buffer & PSP_ADDR_MASK);
        c.frame0.assign(b + t.dataOff, b + t.dataOff + t.blockAlign);
        c.segs.clear();
        c.segPush(c.ringPos, c.ringValid);
    }
    if (!dec_open(c)) {
        std::fprintf(stderr, "[ATRAC] decoder unavailable; stream will play silence\n");
    }
    if (c.state == ST_STREAMED) {
        dec_frame(c, c.frame0.data());  // priming frame 0
        c.decNext = 1;
        c.lastDec = 0;
    }
    return 0;
}

int alloc_ctx(int codec) {
    for (int i = 0; i < MAX_ATRAC_IDS; i++) {
        if (!g_ctx[i].used) {
            g_ctx[i] = AtracCtx{};
            g_ctx[i].used = true;
            g_ctx[i].codec = codec;
            g_ctx[i].state = ST_NO_DATA;
            return i;
        }
    }
    return -1;
}

void free_ctx(int id) {
    dec_close(g_ctx[id]);
    g_ctx[id] = AtracCtx{};
}

// ---------------- guest memory helpers ----------------

inline bool addr_ok(uint32_t a) { return a != 0 && (a & PSP_ADDR_MASK) < PSP_MEM_SIZE; }
inline void w32(uint8_t* rdram, uint32_t a, uint32_t v) { if (addr_ok(a)) psp_mem_write<uint32_t>(rdram, a, v); }

inline bool range_ok(uint32_t a, uint64_t n) {
    return addr_ok(a) && static_cast<uint64_t>(a & PSP_ADDR_MASK) + n <= PSP_MEM_SIZE;
}

// ---------------- playback logic ----------------

int remaining_frames(const AtracCtx& c) {
    if (c.state == ST_ALL_DATA_LOADED) return REMAIN_ALLDATA_ON_MEMORY;
    if (c.state == ST_HALFWAY_BUFFER) {
        // frames between the decode cursor and the end of the loaded data
        int k = (c.currentSample + c.offsetFull()) / c.spf();
        int64_t curOff = static_cast<int64_t>(c.dataOff) + static_cast<int64_t>(k) * c.blockAlign;
        int64_t rem = static_cast<int64_t>(c.readSize) - curOff;
        return rem < 0 ? 0 : static_cast<int>(rem / c.blockAlign);
    }
    if (!c.loops() && c.fileRead >= c.fileSize) return REMAIN_NONLOOP_ON_MEMORY;
    return static_cast<int>(c.ringValid / c.blockAlign);
}

void seek_to(AtracCtx& c, int sample) {
    c.currentSample = sample;
    // Priming happens lazily in decode_data. Force a re-sync.
    c.decNext = -1;
}

// Locate guest bytes of codec frame k. Returns nullptr if not available.
const uint8_t* frame_ptr(const AtracCtx& c, const uint8_t* rdram, int64_t k) {
    if (k < 0) return nullptr;
    uint64_t off = static_cast<uint64_t>(c.dataOff) + static_cast<uint64_t>(k) * c.blockAlign;
    if (off + c.blockAlign > c.readSize || off + c.blockAlign > c.fileSize) return nullptr;
    if (off + c.blockAlign > c.bufferSize) return nullptr;
    return rdram + (c.buffer & PSP_ADDR_MASK) + off;
}

uint32_t decode_data(AtracCtx& c, uint8_t* rdram, uint32_t outAddr, int* numSamples, int* finish, int* remains) {
    const int spf = c.spf();
    *numSamples = 0;
    *finish = 0;
    *remains = 0;

    if (c.currentSample >= c.endSample && c.loopNum == 0) {
        *finish = 1;
        return ERR_ALL_DATA_DECODED;
    }

    const int offFull = c.offsetFull();
    const int64_t n = static_cast<int64_t>(c.currentSample) + offFull;
    const int64_t k = n / spf;
    const int skip = static_cast<int>(n % spf);
    int maxSamples = c.endSample + 1 - c.currentSample;
    // A looping track jumps right after its loop end sample, not at the end of that codec frame.
    if (c.loopNum != 0 && c.loopStart >= 0 && c.loopEnd >= 0 && c.currentSample <= c.loopEnd - offFull)
        maxSamples = std::min(maxSamples, c.loopEnd - offFull + 1 - c.currentSample);
    int ns = std::min(spf - skip, maxSamples);
    if (ns < 0) ns = 0;

    bool got = false;
    if (c.state == ST_STREAMED) {
        const uint32_t ringEnd = std::max<uint32_t>((c.bufferSize / c.blockAlign) * c.blockAlign, c.blockAlign);
        std::vector<uint8_t> tmp(c.blockAlign);
        // Take the frame at ringPos out of the ring (copied into tmp when `keep`).
        auto pop = [&](uint32_t n, bool keep) {
            if (keep)
                for (uint32_t i = 0; i < n; i++)
                    tmp[i] = rdram[(c.buffer & PSP_ADDR_MASK) + ((c.ringPos + i) % ringEnd)];
            c.ringPos = (c.ringPos + n) % ringEnd;
            c.ringValid = c.ringValid > n ? c.ringValid - n : 0;
            c.segs.front().fileOff += n;
            c.segs.front().len -= n;
            if (c.segs.front().len == 0) c.segs.erase(c.segs.begin());
        };
        while (!c.segs.empty() && c.bufferSize >= c.blockAlign) {
            AtracCtx::Seg& s = c.segs.front();
            if (s.len < c.blockAlign) {           // partial frame at a jump: drop it, or wait for more data
                if (c.segs.size() > 1) { pop(s.len, false); continue; }
                break;
            }
            if (s.fileOff < c.dataOff) { pop(std::min(s.len, c.dataOff - s.fileOff), false); continue; }
            const int64_t fi = (static_cast<int64_t>(s.fileOff) - c.dataOff) / c.blockAlign;
            if (fi == k || fi == k - 1) {
                if (c.lastDec != fi - 1) dec_flush(c);  // discontinuity (loop jump / reset)
                pop(c.blockAlign, true);
                const bool ok = dec_frame(c, tmp.data());
                c.lastDec = fi;
                if (fi == k) { got = ok; break; }
                continue;                            // that was the priming frame before k
            }
            // Behind the cursor, or the tail past the loop end with the wrapped data already queued: skip it.
            if (fi < k - 1 || c.segs.size() > 1) { pop(c.blockAlign, false); continue; }
            break;                                   // ahead of the cursor with nothing else queued: wait
        }
        c.decNext = static_cast<int>(k) + 1;
    } else {
        const uint8_t* fp = frame_ptr(c, rdram, k);
        if (fp) {
            // Make sure the decoder is positioned right before frame k.
            if (c.decNext < 0 || c.decNext > k || k - c.decNext > 2) {
                dec_flush(c);
                c.decNext = static_cast<int>(std::max<int64_t>(0, k - 1));
            }
            while (c.decNext < k) {
                dec_frame(c, frame_ptr(c, rdram, c.decNext));  // priming output is discarded
                c.decNext++;
            }
            got = dec_frame(c, fp);
            c.decNext = static_cast<int>(k) + 1;
        }
    }

    if (outAddr && ns > 0) {
        const size_t bytes = static_cast<size_t>(ns) * c.outChannels * 2;
        if (range_ok(outAddr, bytes)) {
            uint8_t* dst = rdram + (outAddr & PSP_ADDR_MASK);
            if (got) {
                std::memcpy(dst, c.pcm.data() + static_cast<size_t>(skip) * c.outChannels, bytes);
            } else {
                std::memset(dst, 0, bytes);
            }
        }
    }

    *numSamples = ns;
    c.currentSample += ns;

    const bool hitEnd = c.currentSample >= c.endSample;
    const int loopEndUser = c.loopEnd - offFull;
    if ((hitEnd || c.currentSample > loopEndUser) && c.loopNum != 0 && c.loopStart >= 0) {
        seek_to(c, std::max(0, c.loopStart - offFull));
        if (c.loopNum > 0) c.loopNum--;
    } else if (hitEnd) {
        *finish = 1;
        c.currentSample += spf - ns;
    }
    *remains = remaining_frames(c);
    return 0;
}

int next_samples(const AtracCtx& c) {
    if (c.currentSample >= c.endSample) return 0;
    const int spf = c.spf();
    int skip = c.offsetFull();
    int firstSamples = (spf - (skip % spf)) % spf;
    int num = c.endSample + 1 - c.currentSample;
    if (c.currentSample == 0 && firstSamples != 0) num = firstSamples;
    int unaligned = (skip + c.currentSample) % spf;
    if (unaligned != 0) num = spf - unaligned;
    return std::min(num, spf);
}

int bitrate_of(const AtracCtx& c) {
    int br = static_cast<int>((c.blockAlign * 352800u) / 1000u);
    if (c.codec == CODEC_AT3PLUS) br = ((br >> 11) + 8) & 0xFFFFFFF0;
    else br = (br + 511) >> 10;
    return br;
}

// ---------------- HLE entry points ----------------
// All handlers: a0..a3 = r[4..7], 5th arg = r[8].

inline uint32_t A(recomp_context* ctx, int i) { return static_cast<uint32_t>(ctx->r[4 + i]); }
inline void ret(recomp_context* ctx, uint32_t v) { ctx->r[2] = static_cast<int32_t>(v); }

// Returns the context, or sets an error return value and returns null.
AtracCtx* managed(recomp_context* ctx, bool needData = true) {
    AtracCtx* c = get_ctx(static_cast<int>(A(ctx, 0)));
    if (!c) { ret(ctx, ERR_BAD_ATRACID); return nullptr; }
    if (needData && c->state == ST_NO_DATA) { ret(ctx, ERR_NO_DATA); return nullptr; }
    return c;
}

void hle_GetAtracID(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    int codec = static_cast<int>(A(ctx, 0));
    if (codec != CODEC_AT3 && codec != CODEC_AT3PLUS) { ret(ctx, ERR_INVALID_CODECTYPE); return; }
    int id = alloc_ctx(codec);
    ret(ctx, id < 0 ? ERR_NO_ATRACID : static_cast<uint32_t>(id));
}

void hle_ReleaseAtracID(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    int id = static_cast<int>(A(ctx, 0));
    if (!get_ctx(id)) { ret(ctx, ERR_BAD_ATRACID); return; }
    free_ctx(id);
    ret(ctx, 0);
}

// Shared by SetData / SetHalfwayBuffer. readSize==bufferSize for SetData.
uint32_t set_data_common(uint8_t* rdram, AtracCtx& c, uint32_t buffer, uint32_t readSize, uint32_t bufferSize) {
    if (!range_ok(buffer, std::max<uint32_t>(readSize, 1))) return ERR_ILLEGAL_ADDRESS;
    Track t;
    uint32_t e = analyze(rdram + (buffer & PSP_ADDR_MASK), readSize, t);
    if (e) return e;
    if (c.codec && c.codec != t.codec) return ERR_WRONG_CODECTYPE;
    if (bufferSize < readSize) bufferSize = readSize;
    return ctx_set_data(c, t, buffer, readSize, bufferSize, 2, rdram);
}

void hle_SetData(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx, false);
    if (!c) return;
    uint32_t sz = A(ctx, 2);
    ret(ctx, set_data_common(rdram, *c, A(ctx, 1), sz, sz));
}

void hle_SetHalfwayBuffer(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx, false);
    if (!c) return;
    uint32_t rs = A(ctx, 2), bs = A(ctx, 3);
    if (rs > bs) { ret(ctx, ERR_INCORRECT_READ_SIZE); return; }
    ret(ctx, set_data_common(rdram, *c, A(ctx, 1), rs, bs));
}

uint32_t set_and_get_id(uint8_t* rdram, uint32_t buf, uint32_t rs, uint32_t bs) {
    if (!range_ok(buf, std::max<uint32_t>(rs, 1))) return ERR_ILLEGAL_ADDRESS;
    Track t;
    uint32_t e = analyze(rdram + (buf & PSP_ADDR_MASK), rs, t);
    if (e) return e;
    int id = alloc_ctx(t.codec);
    if (id < 0) return ERR_NO_ATRACID;
    e = ctx_set_data(g_ctx[id], t, buf, rs, std::max(bs, rs), 2, rdram);
    if (e) { free_ctx(id); return e; }
    std::fprintf(stderr,
        "[ATRAC] id=%d %s ch=%d align=%u state=%d fileSize=%u endSample=%d firstOff=%d loop=%d..%d buf=0x%08X\n",
        id, t.codec == CODEC_AT3PLUS ? "AT3+" : "AT3", t.channels, t.blockAlign,
        g_ctx[id].state, t.fileSize, t.endSample, t.firstSampleOffset, t.loopStart, t.loopEnd, buf);
    return static_cast<uint32_t>(id);
}

void hle_SetDataAndGetID(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    int32_t sz = static_cast<int32_t>(A(ctx, 1));
    if (sz < 0) sz = 0x10000000;
    ret(ctx, set_and_get_id(rdram, A(ctx, 0), static_cast<uint32_t>(sz), static_cast<uint32_t>(sz)));
}

void hle_SetHalfwayBufferAndGetID(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t rs = A(ctx, 1), bs = A(ctx, 2);
    if (rs > bs) { ret(ctx, ERR_INCORRECT_READ_SIZE); return; }
    ret(ctx, set_and_get_id(rdram, A(ctx, 0), rs, bs));
}

void hle_DecodeData(uint8_t* rdram, recomp_context* ctx) {
    // (id, outPcm, *outSamples, *outFinish, *outRemainFrame)
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    uint32_t outAddr = A(ctx, 1);
    if (outAddr & 1) { ret(ctx, ERR_BAD_ALIGNMENT); return; }
    int ns = 0, fin = 0, rem = 0;
    uint32_t r = decode_data(*c, rdram, outAddr, &ns, &fin, &rem);
    w32(rdram, A(ctx, 2), static_cast<uint32_t>(ns));
    w32(rdram, A(ctx, 3), static_cast<uint32_t>(fin));
    if (r == 0) w32(rdram, static_cast<uint32_t>(ctx->r[8]), static_cast<uint32_t>(rem));
    ret(ctx, r);
}

void hle_GetSoundSample(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    int off = c->offsetFull();
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->endSample));
    w32(rdram, A(ctx, 2), static_cast<uint32_t>(c->loopStart < 0 ? -1 : c->loopStart - off));
    w32(rdram, A(ctx, 3), static_cast<uint32_t>(c->loopEnd < 0 ? -1 : c->loopEnd - off));
    ret(ctx, 0);
}

void hle_GetNextDecodePosition(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    if (!addr_ok(A(ctx, 1))) { ret(ctx, 0); return; }
    if (c->currentSample >= c->endSample) {
        w32(rdram, A(ctx, 1), 0);
        ret(ctx, ERR_ALL_DATA_DECODED);
        return;
    }
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->currentSample));
    ret(ctx, 0);
}

void hle_GetNextSample(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(next_samples(*c)));
    ret(ctx, 0);
}

void hle_GetMaxSample(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->spf()));
    ret(ctx, 0);
}

void hle_GetBitrate(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(bitrate_of(*c)));
    ret(ctx, 0);
}

void hle_GetChannel(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->channels));
    ret(ctx, 0);
}

void hle_GetOutputChannel(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->outChannels));
    ret(ctx, 0);
}

void hle_GetLoopStatus(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(c->loopNum));
    w32(rdram, A(ctx, 2), c->loopStart >= 0 ? 1u : 0u);
    ret(ctx, 0);
}

void hle_GetInternalErrorInfo(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    w32(rdram, A(ctx, 1), 0);
    ret(ctx, 0);
}

void hle_SetLoopNum(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    if (c->loopStart < 0) { ret(ctx, ERR_NO_LOOP_INFO); return; }
    c->loopNum = static_cast<int32_t>(A(ctx, 1));
    ret(ctx, 0);
}

void hle_GetRemainFrame(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    if (!addr_ok(A(ctx, 1))) { ret(ctx, ERR_ILLEGAL_ADDRESS); return; }
    w32(rdram, A(ctx, 1), static_cast<uint32_t>(remaining_frames(*c)));
    ret(ctx, 0);
}

void stream_info(const AtracCtx& c, uint32_t* writeOff, uint32_t* writable, uint32_t* readOffset) {
    if (c.state == ST_ALL_DATA_LOADED) {
        *writeOff = 0; *writable = 0; *readOffset = 0;
    } else if (c.state == ST_HALFWAY_BUFFER) {
        *writeOff = c.readSize;
        *writable = c.fileSize - c.readSize;
        *readOffset = c.readSize;
    } else {
        const uint32_t end = std::max<uint32_t>((c.bufferSize / c.blockAlign) * c.blockAlign, c.blockAlign);
        uint32_t ext = c.ringPos + c.ringValid;
        uint32_t off, wr;
        if (ext < end) { off = ext; wr = end - ext; }
        else { off = ext - end; wr = c.ringPos - off; }
        uint32_t ro = c.fileRead;
        if (c.loops() && ro >= c.dataEnd()) ro = c.loopFileOff();  // wrap to the loop start
        const uint32_t lim = c.streamLimit();
        if (ro >= lim) { ro = 0; off = 0; wr = 0; }
        else if (wr > lim - ro) wr = lim - ro;
        *writeOff = off; *writable = wr; *readOffset = ro;
    }
}

void hle_GetStreamDataInfo(uint8_t* rdram, recomp_context* ctx) {
    // (id, u8** writePtr, u32* writableBytes, u32* readOffset)
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    uint32_t off, wr, ro;
    stream_info(*c, &off, &wr, &ro);
    w32(rdram, A(ctx, 1), c->buffer + off);
    w32(rdram, A(ctx, 2), wr);
    w32(rdram, A(ctx, 3), ro);
    ret(ctx, 0);
}

void hle_AddStreamData(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    uint32_t add = A(ctx, 1);
    if (c->state == ST_ALL_DATA_LOADED) { ret(ctx, ERR_ALL_DATA_LOADED); return; }
    uint32_t off, wr, ro;
    stream_info(*c, &off, &wr, &ro);
    if (add > wr) { ret(ctx, ERR_ADD_DATA_TOO_BIG); return; }
    if (c->state == ST_HALFWAY_BUFFER) {
        c->readSize = std::min(c->fileSize, c->readSize + add);
        if (c->readSize >= c->fileSize) c->state = ST_ALL_DATA_LOADED;
    } else {
        uint32_t real = ro < c->streamLimit() ? std::min(add, c->streamLimit() - ro) : 0;
        c->segPush(ro, real);
        if (add > real) c->segPush(c->fileSize, add - real);  // bytes past the end: skipped by the decoder
        c->fileRead = ro + real;
        c->ringValid += add;
    }
    ret(ctx, 0);
}

struct ResetInfo { uint32_t wp, wr, mn, fp; };

uint32_t reset_info(const AtracCtx& c, int sample, ResetInfo out[2]) {
    if (static_cast<uint32_t>(sample) + c.firstSampleOffset >
        static_cast<uint32_t>(c.endSample) + c.firstSampleOffset)
        return ERR_BAD_SAMPLE;
    const int spf = c.spf();
    if (c.state == ST_ALL_DATA_LOADED) {
        out[0] = {c.buffer, 0, 0, 0};
    } else if (c.state == ST_HALFWAY_BUFFER) {
        int64_t k = (static_cast<int64_t>(sample) + c.offsetFull()) / spf;
        int64_t needOff = static_cast<int64_t>(c.dataOff) + (k + 1) * c.blockAlign;
        int64_t mn = needOff - static_cast<int64_t>(c.readSize);
        out[0] = {c.buffer + c.readSize, c.fileSize - c.readSize,
                  static_cast<uint32_t>(mn > 0 ? mn : 0), c.readSize};
    } else {
        int64_t k = (static_cast<int64_t>(sample) + c.offsetFull()) / spf;
        int64_t fpos = c.dataOff + std::max<int64_t>(0, k - 1) * c.blockAlign;
        const uint32_t aligned = std::max<uint32_t>((c.bufferSize / c.blockAlign) * c.blockAlign, c.blockAlign);
        int64_t wr = std::min<int64_t>(static_cast<int64_t>(c.fileSize) - fpos, aligned);
        out[0] = {c.buffer, static_cast<uint32_t>(wr > 0 ? wr : 0), c.blockAlign * 2, static_cast<uint32_t>(fpos)};
    }
    out[1] = {c.buffer, 0, 0, 0};
    return 0;
}

void hle_GetBufferInfoForResetting(uint8_t* rdram, recomp_context* ctx) {
    // (id, sample, AtracResetBufferInfo* out) -- 2 x {writePtr, writable, minWrite, filePos}
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    ResetInfo ri[2];
    uint32_t e = reset_info(*c, static_cast<int>(A(ctx, 1)), ri);
    if (e) { ret(ctx, e); return; }
    uint32_t out = A(ctx, 2);
    if (!addr_ok(out)) { ret(ctx, ERR_ILLEGAL_ADDRESS); return; }
    for (int i = 0; i < 2; i++) {
        w32(rdram, out + i * 16 + 0, ri[i].wp);
        w32(rdram, out + i * 16 + 4, ri[i].wr);
        w32(rdram, out + i * 16 + 8, ri[i].mn);
        w32(rdram, out + i * 16 + 12, ri[i].fp);
    }
    ret(ctx, 0);
}

void hle_ResetPlayPosition(uint8_t* rdram, recomp_context* ctx) {
    // (id, sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf)
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    int sample = static_cast<int>(A(ctx, 1));
    uint32_t b1 = A(ctx, 2), b2 = A(ctx, 3);
    ResetInfo ri[2];
    uint32_t e = reset_info(*c, sample, ri);
    if (e) { ret(ctx, e); return; }
    if (b1 < ri[0].mn || b1 > ri[0].wr) { ret(ctx, ERR_BAD_FIRST_RESET); return; }
    if (b2 < ri[1].mn || b2 > ri[1].wr) { ret(ctx, ERR_BAD_SECOND_RESET); return; }

    if (c->state == ST_HALFWAY_BUFFER) {
        c->readSize = std::min(c->fileSize, c->readSize + b1);
        if (c->readSize >= c->fileSize) c->state = ST_ALL_DATA_LOADED;
    } else if (c->state == ST_STREAMED) {
        // The guest wrote b1 bytes at the buffer start, from file offset ri[0].fp.
        c->fileRead = ri[0].fp + b1;
        // The frame at ring offset 0 is the priming frame for the next one.
        c->ringPos = c->blockAlign;
        c->ringValid = b1 > c->blockAlign ? b1 - c->blockAlign : 0;
        c->segs.clear();
        c->segPush(ri[0].fp + c->blockAlign, c->ringValid);
        dec_flush(*c);
        c->lastDec = -2;
        if (b1 >= c->blockAlign && range_ok(c->buffer, c->blockAlign)) {
            dec_frame(*c, rdram + (c->buffer & PSP_ADDR_MASK));
            c->lastDec = (static_cast<int64_t>(ri[0].fp) - c->dataOff) / c->blockAlign;
        }
        int64_t k = (static_cast<int64_t>(sample) + c->offsetFull()) / c->spf();
        c->currentSample = sample;
        c->decNext = static_cast<int>(k);
        ret(ctx, 0);
        return;
    }
    seek_to(*c, sample);
    ret(ctx, 0);
}

void hle_GetSecondBufferInfo(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    // Loop-with-trailer streams (which need a second buffer) aren't modelled.
    w32(rdram, A(ctx, 1), 0);
    w32(rdram, A(ctx, 2), 0);
    ret(ctx, ERR_SECOND_BUF_NOT_NEED);
}

void hle_SetSecondBuffer(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    ret(ctx, ERR_SECOND_BUF_NOT_NEED);
}

void hle_IsSecondBufferNeeded(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    AtracCtx* c = managed(ctx);
    if (!c) return;
    ret(ctx, 0);
}

void hle_Reinit(uint8_t*, recomp_context* ctx) { ret(ctx, 0); }
void hle_StartEntry(uint8_t*, recomp_context* ctx) { ret(ctx, 0); }

}  // namespace

void psp_hle_register_atrac() {
    psp_hle_register("sceAtracGetAtracID", hle_GetAtracID);
    psp_hle_register("sceAtracReleaseAtracID", hle_ReleaseAtracID);
    psp_hle_register("sceAtracSetData", hle_SetData);
    psp_hle_register("sceAtracSetDataAndGetID", hle_SetDataAndGetID);
    psp_hle_register("sceAtracSetHalfwayBuffer", hle_SetHalfwayBuffer);
    psp_hle_register("sceAtracSetHalfwayBufferAndGetID", hle_SetHalfwayBufferAndGetID);
    psp_hle_register("sceAtracDecodeData", hle_DecodeData);
    psp_hle_register("sceAtracGetSoundSample", hle_GetSoundSample);
    psp_hle_register("sceAtracGetNextDecodePosition", hle_GetNextDecodePosition);
    psp_hle_register("sceAtracGetNextSample", hle_GetNextSample);
    psp_hle_register("sceAtracGetMaxSample", hle_GetMaxSample);
    psp_hle_register("sceAtracGetBitrate", hle_GetBitrate);
    psp_hle_register("sceAtracGetChannel", hle_GetChannel);
    psp_hle_register("sceAtracGetOutputChannel", hle_GetOutputChannel);
    psp_hle_register("sceAtracGetLoopStatus", hle_GetLoopStatus);
    psp_hle_register("sceAtracGetInternalErrorInfo", hle_GetInternalErrorInfo);
    psp_hle_register("sceAtracSetLoopNum", hle_SetLoopNum);
    psp_hle_register("sceAtracGetRemainFrame", hle_GetRemainFrame);
    psp_hle_register("sceAtracGetStreamDataInfo", hle_GetStreamDataInfo);
    psp_hle_register("sceAtracAddStreamData", hle_AddStreamData);
    psp_hle_register("sceAtracGetBufferInfoForResetting", hle_GetBufferInfoForResetting);
    psp_hle_register("sceAtracResetPlayPosition", hle_ResetPlayPosition);
    psp_hle_register("sceAtracGetSecondBufferInfo", hle_GetSecondBufferInfo);
    psp_hle_register("sceAtracSetSecondBuffer", hle_SetSecondBuffer);
    psp_hle_register("sceAtracIsSecondBufferNeeded", hle_IsSecondBufferNeeded);
    psp_hle_register("sceAtracReinit", hle_Reinit);
    psp_hle_register("sceAtracStartEntry", hle_StartEntry);
}
