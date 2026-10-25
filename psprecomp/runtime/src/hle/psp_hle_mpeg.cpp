#include "psp_hle_mpeg.h"
#include "psp_prof.h"
#include "hle/psp_hle_kernel.h"  // psp_alloc_stack
#include "psp_memory.h"
#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#ifdef PSPRECOMP_HAVE_AVCODEC
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#endif

// ================================================================
// sceMpeg HLE: PSMF (MPEG-2 program stream, H.264 video + ATRAC3+ audio).
//
// Semantics follow PPSSPP's Core/HLE/sceMpeg.cpp. The guest owns the ring
// buffer (SceMpegRingbuffer + packet data); sceMpegRingbufferPut calls the
// guest's callback to fill it. Packets (2048-byte MPEG-PS sectors) are
// consumed from the guest ring lazily by a host demuxer when an AU is
// requested (sceMpegGetAvcAu / GetAtracAu), which advances packetsRead /
// packetsFree like the real library does. Decoding uses libavcodec (H.264,
// ATRAC3+); YCbCr -> RGB is done here (BT.601), so no libswscale is needed.
//
// PSMF layout used (PPSSPP): header "PSMF", version @4, stream offset @8,
// stream size @0xC (all BE); first/last PTS @0x56/@0x5C (BE u32), stream
// count @0x80 (BE u16). The stream proper is 2048-byte MPEG-PS packs:
// 0x1E0+n = AVC video PES, 0x1BD = private stream 1 carrying ATRAC3+ with a
// 4-byte sub-header (0xA0+ch, ...) followed by ES frames, each prefixed by
// an 8-byte header 0F D0 <code1> <code2> ...; codec params = (code1<<8|code2):
// sample-rate index = bits 15..13, channel id = bits 12..10 (1 mono, 2
// stereo), payload size = (params & 0x3FF)*8 + 8.
//
// Env: PSPRECOMP_MOVIE_TRACE=1 logs one line per sceMpeg call;
//      PSPRECOMP_MOVIE_STATS=1 logs per-60-frame decode/convert/write cost.
// ================================================================

namespace {

constexpr uint32_t ERR_NO_DATA       = 0x80618001;
constexpr uint32_t ERR_BAD_VERSION   = 0x80610002;
constexpr uint32_t ERR_NO_MEMORY     = 0x80610022;
constexpr uint32_t ERR_INVALID_ADDR  = 0x80610103;
constexpr uint32_t ERR_INVALID_VALUE = 0x806101FE;

constexpr uint32_t MPEG_MEMSIZE        = 0x10000;
constexpr uint32_t MPEG_AVC_ES_SIZE    = 2048;
constexpr uint32_t MPEG_ATRAC_ES_SIZE  = 2112;
constexpr uint32_t MPEG_ATRAC_OUT_SIZE = 8192;   // 2048 samples * 2ch * s16
constexpr uint32_t PACKET_SIZE         = 2048;
constexpr int      ATRAC_SAMPLES       = 2048;

// SceMpegRingbuffer guest layout
constexpr uint32_t RB_PACKETS = 0x00, RB_READ = 0x04, RB_WRITTEN = 0x08,
                   RB_FREE = 0x0C, RB_PKTSIZE = 0x10, RB_DATA = 0x14,
                   RB_CB = 0x18, RB_CBARG = 0x1C, RB_UPPER = 0x20,
                   RB_SEMA = 0x24, RB_MPEG = 0x28, RB_GP = 0x2C;

enum StreamType { ST_AVC = 0, ST_ATRAC = 1, ST_PCM = 2, ST_DATA = 3 };

bool g_trace = false;
bool g_trace_init = false;

// PSPRECOMP_MOVIE_STATS=1: one [MOVIE] stats line per 60 decoded frames with
// the per-frame host cost split (all ms) and the call cadence the game drives.
struct MovieStats {
    bool on = std::getenv("PSPRECOMP_MOVIE_STATS") != nullptr;
    uint32_t n = 0;
    double avc = 0, csc = 0, wr = 0, aud = 0, call = 0, avc_max = 0, call_max = 0, gap_max = 0;
    uint32_t aud_n = 0;
    uint64_t last_call = 0, win_t0 = 0;
    int64_t vpts = 0, apts = 0;   // last video / audio AU PTS (90 kHz)
    void frame(double a, double c, double w, double total) {
        n++; avc += a; csc += c; wr += w; call += total;
        avc_max = std::max(avc_max, a); call_max = std::max(call_max, total);
        uint64_t t = psp_prof::now();
        if (last_call) gap_max = std::max(gap_max, (t - last_call) / 1e6);
        last_call = t;
        if (!win_t0) win_t0 = t;
        if (n >= 60) {
            const double wall = (t - win_t0) / 1e9;
            std::fprintf(stderr,
                "[MOVIE] stats/frame (n=%u in %.2fs = %.1f fps, audio %.1f AU/s): avc=%.2f(max %.2f) csc=%.2f write=%.2f call=%.2f(max %.2f) "
                "audio=%.3f/call decode-call-gap-max=%.1f ms vpts-apts=%+.0f ms\n",
                n, wall, n / wall, aud_n / wall, avc / n, avc_max, csc / n, wr / n, call / n, call_max,
                aud_n ? aud / aud_n : 0.0, gap_max, (vpts - apts) / 90.0);
            win_t0 = t;
            n = 0; avc = csc = wr = aud = call = avc_max = call_max = gap_max = 0; aud_n = 0;
        }
    }
};
MovieStats g_ms;
double g_t_avc = 0, g_t_csc = 0, g_t_wr = 0;
inline double ms_since(uint64_t t0) { return (psp_prof::now() - t0) / 1e6; }

void tr(const char* name, recomp_context* ctx, uint32_t ret) {
    if (!g_trace_init) {
        const char* e = std::getenv("PSPRECOMP_MOVIE_TRACE");
        g_trace = e && e[0] == '1';
        g_trace_init = true;
    }
    if (!g_trace) return;
    std::fprintf(stderr, "[MOVIE] %s(%08x,%08x,%08x,%08x) -> %08x\n", name,
                 (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6],
                 (uint32_t)ctx->r[7], ret);
}

inline uint32_t A(const recomp_context* c, int i) { return (uint32_t)c->r[4 + i]; }
inline void RET(recomp_context* c, uint32_t v) { c->r[2] = (int32_t)v; }

inline bool addr_ok(uint32_t a) { return a >= 0x00010000u && (a & PSP_ADDR_MASK) < PSP_MEM_SIZE; }
inline bool range_ok(uint32_t a, uint64_t n) {
    return addr_ok(a) && (uint64_t)(a & PSP_ADDR_MASK) + n <= PSP_MEM_SIZE;
}
inline uint32_t r32(uint8_t* rd, uint32_t a) { return addr_ok(a) ? psp_mem_read<uint32_t>(rd, a) : 0; }
inline void w32(uint8_t* rd, uint32_t a, uint32_t v) { if (addr_ok(a)) psp_mem_write<uint32_t>(rd, a, v); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

// ---------------- PSMF header ----------------

struct Psmf {
    bool valid = false;
    uint32_t version = 0, streamOffset = 0, streamSize = 0;
    uint32_t firstPts = 0, lastPts = 0, numStreams = 0;
};
Psmf g_last_psmf;  // QueryStreamSize has no mpeg handle: remember the last header

bool parse_psmf(uint8_t* rd, uint32_t addr, Psmf& out, bool log) {
    if (!range_ok(addr, 0x100)) return false;
    const uint8_t* h = rd + (addr & PSP_ADDR_MASK);
    if (std::memcmp(h, "PSMF", 4) != 0) return false;
    out.valid = true;
    out.version = be32(h + 4);
    out.streamOffset = be32(h + 8);
    out.streamSize = be32(h + 0xC);
    out.firstPts = be32(h + 0x56);
    out.lastPts = be32(h + 0x5C);
    out.numStreams = be16(h + 0x80);
    static bool logged = false;
    if ((log || g_trace) && !logged) {
        logged = true;
        std::fprintf(stderr,
            "[MOVIE] PSMF header: ver=%.4s streamOffset=0x%X streamSize=0x%X "
            "firstPTS=%u lastPTS=%u (%.2fs) streams=%u\n",
            (const char*)(h + 4), out.streamOffset, out.streamSize, out.firstPts,
            out.lastPts, (out.lastPts - out.firstPts) / 90000.0, out.numStreams);
        for (uint32_t i = 0; i < out.numStreams && i < 8; i++) {
            const uint8_t* e = h + 0x82 + i * 16;
            std::fprintf(stderr,
                "[MOVIE]   stream %u: %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x\n", i, e[0], e[1], e[2],
                e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12],
                e[13], e[14], e[15]);
        }
    }
    return true;
}

// ---------------- decoders ----------------

struct VideoAu {
    std::vector<uint8_t> data;
    int64_t pts = -1, dts = -1;
};
struct AudioAu {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    uint32_t params = 0;
};

struct Stream { int type; int num; };

struct MpegCtx {
    uint32_t handle = 0;      // value written to *mpeg (mpegData + 0x30)
    uint32_t ringbuf = 0;
    uint32_t frameWidth = 0, mode = 0, ddrTop = 0;
    int pixelMode = 3;        // GE pixel format: 0=565 1=5551 2=4444 3=8888
    Psmf psmf;
    std::map<uint32_t, Stream> streams;
    bool esUsed[4] = {false, false, false, false};
    int avcCh = -1, atracCh = -1;
    bool eof = false;         // Put's callback returned 0 packets: file ended

    // demux state
    // Video ES is split into AUs at H.264 access-unit delimiters (00 00 01 09);
    // PES PTS/DTS marks (buffer offset, pts, dts) attach to the first AU that
    // starts at/after the mark (PSMF only stamps every ~16th AU).
    struct VMark { int64_t off, pts, dts; };
    std::vector<uint8_t> vidBuf; std::deque<VMark> vidMarks;
    size_t vidScan = 4;
    int64_t vidLastPts = 90000 - 3003, vidLastDts = 90000 - 3003 - 3003;
    std::deque<VideoAu> vidQ, vidIssued;
    std::vector<uint8_t> audBuf;
    std::deque<std::pair<int64_t, int64_t>> audMarks;  // (offset in audBuf, pts)
    double audPts = 0; bool audPtsInit = false;
    std::deque<AudioAu> audQ, audIssued;
    uint32_t videoFrames = 0;

    // output
    int vw = 0, vh = 0;
    std::vector<uint8_t> rgba;   // last decoded frame, RGBA8888
    bool haveFrame = false;

#ifdef PSPRECOMP_HAVE_AVCODEC
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    uint32_t adecParams = 0;
    AVFrame* frm = nullptr;
#endif
    ~MpegCtx();
};

MpegCtx::~MpegCtx() {
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (frm) av_frame_free(&frm);
    if (vdec) avcodec_free_context(&vdec);
    if (adec) avcodec_free_context(&adec);
#endif
}

std::map<uint32_t, std::unique_ptr<MpegCtx>> g_ctxs;
uint32_t g_next_stream_uid = 0x6D700001;

MpegCtx* get_ctx(uint8_t* rd, uint32_t mpegPtr) {
    if (!addr_ok(mpegPtr)) return nullptr;
    auto it = g_ctxs.find(r32(rd, mpegPtr));
    return it == g_ctxs.end() ? nullptr : it->second.get();
}

// ---------------- ring buffer access ----------------

struct Ring {
    uint8_t* rd; uint32_t a;
    uint32_t get(uint32_t off) const { return r32(rd, a + off); }
    void set(uint32_t off, uint32_t v) const { w32(rd, a + off, v); }
};

// ---------------- PES demux ----------------

int64_t read_ts(const uint8_t* p) {
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) |
           ((int64_t)(p[2] >> 1) << 15) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

constexpr int64_t VIDEO_FRAME_TICKS = 3003;

void emit_video_au(MpegCtx& c, size_t len) {
    VideoAu au;
    au.data.assign(c.vidBuf.begin(), c.vidBuf.begin() + len);
    bool fresh = false;
    int64_t pts = 0, dts = 0;
    while (!c.vidMarks.empty() && c.vidMarks.front().off <= 0) {
        pts = c.vidMarks.front().pts;
        dts = c.vidMarks.front().dts;
        fresh = true;
        c.vidMarks.pop_front();
    }
    if (!fresh) { pts = c.vidLastPts + VIDEO_FRAME_TICKS; dts = c.vidLastDts + VIDEO_FRAME_TICKS; }
    c.vidLastPts = pts; c.vidLastDts = dts;
    au.pts = pts; au.dts = dts;
    c.vidBuf.erase(c.vidBuf.begin(), c.vidBuf.begin() + len);
    for (auto& m : c.vidMarks) m.off -= (int64_t)len;
    c.vidScan = 4;
    c.vidQ.push_back(std::move(au));
}

// Cut complete AUs out of vidBuf: an AU ends where the next AUD starts.
void extract_video_aus(MpegCtx& c) {
    const std::vector<uint8_t>& b = c.vidBuf;
    for (;;) {
        size_t i = c.vidScan;
        bool found = false;
        for (; i + 4 <= b.size(); i++) {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 9) { found = true; break; }
        }
        if (!found) { c.vidScan = b.size() >= 4 ? b.size() - 3 : 4; return; }
        size_t k = (b[i - 1] == 0) ? i - 1 : i;   // include the 4-byte start code's zero_byte
        if (k == 0) { c.vidScan = i + 1; continue; }
        emit_video_au(c, k);
    }
}

void finish_video_au(MpegCtx& c) {  // EOF: whatever is buffered is the last AU
    if (!c.vidBuf.empty()) emit_video_au(c, c.vidBuf.size());
}

void extract_audio_frames(MpegCtx& c) {
    for (;;) {
        // resync to 0F D0
        size_t skip = 0;
        while (skip + 1 < c.audBuf.size() && !(c.audBuf[skip] == 0x0F && c.audBuf[skip + 1] == 0xD0)) skip++;
        if (skip) {
            c.audBuf.erase(c.audBuf.begin(), c.audBuf.begin() + skip);
            for (auto& m : c.audMarks) m.first -= (int64_t)skip;
        }
        if (c.audBuf.size() < 8) return;
        uint32_t params = be16(&c.audBuf[2]);
        uint32_t payload = (params & 0x3FF) * 8 + 8;
        size_t total = payload + 8;
        if (c.audBuf.size() < total) return;
        AudioAu au;
        au.params = params;
        au.data.assign(c.audBuf.begin() + 8, c.audBuf.begin() + total);
        bool fresh = false;
        while (!c.audMarks.empty() && c.audMarks.front().first <= 0) {
            c.audPts = (double)c.audMarks.front().second;
            c.audPtsInit = true;
            fresh = true;
            c.audMarks.pop_front();
        }
        if (!fresh) c.audPts += (double)ATRAC_SAMPLES * 90000.0 / 44100.0;
        au.pts = (int64_t)(c.audPts + 0.5);
        c.audBuf.erase(c.audBuf.begin(), c.audBuf.begin() + total);
        for (auto& m : c.audMarks) m.first -= (int64_t)total;
        c.audQ.push_back(std::move(au));
    }
}

void parse_sector(MpegCtx& c, const uint8_t* s) {
    uint32_t pos = 0;
    while (pos + 6 <= PACKET_SIZE) {
        if (s[pos] != 0 || s[pos + 1] != 0 || s[pos + 2] != 1) { pos++; continue; }
        uint8_t id = s[pos + 3];
        if (id == 0xBA) {  // pack header
            if ((s[pos + 4] & 0xC0) == 0x40) pos += 14 + (s[pos + 13] & 7);
            else pos += 12;
            continue;
        }
        if (id < 0xBB) { pos += 4; continue; }
        uint32_t len = be16(s + pos + 4);
        uint32_t end = std::min<uint32_t>(pos + 6 + len, PACKET_SIZE);
        const bool isVideo = c.avcCh >= 0 && id == (uint8_t)(0xE0 + c.avcCh);
        if (isVideo || id == 0xBD) {
            uint32_t p = pos + 6;
            int64_t pts = -1, dts = -1;
            if (p + 3 <= end && (s[p] & 0xC0) == 0x80) {
                uint8_t fl = s[p + 1] >> 6;
                uint32_t hl = s[p + 2];
                if ((fl & 2) && p + 3 + 5 <= end) pts = read_ts(s + p + 3);
                if (fl == 3 && p + 3 + 10 <= end) dts = read_ts(s + p + 8);
                p += 3 + hl;
            }
            if (p > end) p = end;
            if (id == 0xBD) {
                // 4-byte sub-header [?,?,?,first-AU pointer]; ES frames follow
                if (c.atracCh >= 0 && p + 4 <= end) {
                    if (pts >= 0) c.audMarks.emplace_back((int64_t)c.audBuf.size(), pts);
                    c.audBuf.insert(c.audBuf.end(), s + p + 4, s + end);
                    extract_audio_frames(c);
                }
            } else {
                if (pts >= 0) c.vidMarks.push_back({(int64_t)c.vidBuf.size(), pts, dts >= 0 ? dts : pts});
                c.vidBuf.insert(c.vidBuf.end(), s + p, s + end);
                extract_video_aus(c);
            }
        }
        pos = end;
    }
}

// Consume one packet from the guest ring. Returns false when empty.
bool demux_one(uint8_t* rd, MpegCtx& c) {
    if (!c.ringbuf) return false;
    Ring rb{rd, c.ringbuf};
    uint32_t packets = rb.get(RB_PACKETS);
    int32_t rdn = (int32_t)rb.get(RB_READ), wr = (int32_t)rb.get(RB_WRITTEN);
    if (packets == 0 || wr - rdn <= 0) return false;
    uint32_t addr = rb.get(RB_DATA) + ((uint32_t)rdn % packets) * PACKET_SIZE;
    if (range_ok(addr, PACKET_SIZE)) parse_sector(c, rd + (addr & PSP_ADDR_MASK));
    rb.set(RB_READ, (uint32_t)(rdn + 1));
    rb.set(RB_FREE, packets - (uint32_t)(wr - (rdn + 1)));
    return true;
}

bool fill_video(uint8_t* rd, MpegCtx& c) {
    while (c.vidQ.empty()) {
        if (!demux_one(rd, c)) {
            if (c.eof) finish_video_au(c);  // last AU of the stream
            break;
        }
    }
    return !c.vidQ.empty();
}

bool fill_audio(uint8_t* rd, MpegCtx& c) {
    while (c.audQ.empty()) {
        if (!demux_one(rd, c)) break;
    }
    return !c.audQ.empty();
}

// ---------------- video decode / output ----------------

#ifdef PSPRECOMP_HAVE_AVCODEC
bool convert_frame(MpegCtx& c, const AVFrame* f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    if (!d || d->nb_components < 3 || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_BITSTREAM)) ||
        d->comp[0].depth != 8) {
        static bool once = false;
        if (!once) { once = true; std::fprintf(stderr, "[MOVIE] unsupported decoded pix_fmt %d\n", f->format); }
        return false;
    }
    psp_prof::Scope prof_csc(psp_prof::S_MPEG_CSC);
    const uint64_t t_csc0 = g_ms.on ? psp_prof::now() : 0;
    struct TimerEnd { uint64_t t0; ~TimerEnd() { if (g_ms.on) g_t_csc += ms_since(t0); } } timer_end{t_csc0};
    const int w = f->width, h = f->height, cw = d->log2_chroma_w, ch = d->log2_chroma_h;
    const bool full = f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P ||
                      f->format == AV_PIX_FMT_YUVJ444P || f->color_range == AVCOL_RANGE_JPEG;
    c.vw = w; c.vh = h;
    c.rgba.resize((size_t)w * h * 4);
    if (cw == 1 && ch == 1 && !full && (w & 1) == 0) {
        // 4:2:0 limited-range fast path (the PSMF case): one chroma
        // evaluation per 2x2 block and 32-bit stores; same BT.601 integer
        // maths as the generic loop below.
        for (int y = 0; y < h; y++) {
            const uint8_t* Y = f->data[0] + (size_t)y * f->linesize[0];
            const uint8_t* U = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
            const uint8_t* V = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
            uint32_t* o = reinterpret_cast<uint32_t*>(&c.rgba[(size_t)y * w * 4]);
            for (int x = 0; x < w; x += 2) {
                const int u = U[x >> 1] - 128, v = V[x >> 1] - 128;
                const int rc = 409 * v + 128, gc = -100 * u - 208 * v + 128, bc = 516 * u + 128;
                for (int k = 0; k < 2; k++) {
                    const int cc = 298 * (Y[x + k] - 16);
                    const int r = std::clamp((cc + rc) >> 8, 0, 255);
                    const int g = std::clamp((cc + gc) >> 8, 0, 255);
                    const int b = std::clamp((cc + bc) >> 8, 0, 255);
                    // RGBA byte order (little-endian): R | G<<8 | B<<16 | A<<24
                    o[x + k] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
                }
            }
        }
        return true;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t* Y = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t* U = f->data[1] + (size_t)(y >> ch) * f->linesize[1];
        const uint8_t* V = f->data[2] + (size_t)(y >> ch) * f->linesize[2];
        uint8_t* o = &c.rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            int yy = Y[x], u = U[x >> cw] - 128, v = V[x >> cw] - 128, r, g, b;
            if (full) {
                r = yy + ((359 * v + 128) >> 8);
                g = yy - ((88 * u + 183 * v + 128) >> 8);
                b = yy + ((454 * u + 128) >> 8);
            } else {
                int cc = 298 * (yy - 16);
                r = (cc + 409 * v + 128) >> 8;
                g = (cc - 100 * u - 208 * v + 128) >> 8;
                b = (cc + 516 * u + 128) >> 8;
            }
            o[0] = (uint8_t)std::clamp(r, 0, 255);
            o[1] = (uint8_t)std::clamp(g, 0, 255);
            o[2] = (uint8_t)std::clamp(b, 0, 255);
            o[3] = 255;
            o += 4;
        }
    }
    return true;
}
#endif

// Decode one AVC AU; returns true when a new picture is available in c.rgba.
bool decode_video(MpegCtx& c, const VideoAu& au) {
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (!c.vdec) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { std::fprintf(stderr, "[MOVIE] no H.264 decoder in libavcodec\n"); return false; }
        c.vdec = avcodec_alloc_context3(codec);
        c.vdec->thread_count = 1;
        c.vdec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        c.vdec->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        if (avcodec_open2(c.vdec, codec, nullptr) < 0) {
            std::fprintf(stderr, "[MOVIE] H.264 open failed\n");
            avcodec_free_context(&c.vdec);
            return false;
        }
        c.frm = av_frame_alloc();
    }
    AVPacket* pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int)au.data.size()) < 0) { av_packet_free(&pkt); return false; }
    std::memcpy(pkt->data, au.data.data(), au.data.size());
    pkt->pts = au.pts < 0 ? AV_NOPTS_VALUE : au.pts;
    pkt->dts = au.dts < 0 ? AV_NOPTS_VALUE : au.dts;
    const uint64_t t_avc0 = psp_prof::now();
    int r = avcodec_send_packet(c.vdec, pkt);
    av_packet_free(&pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) return false;
    bool got = false;
    double csc_ms = 0;
    for (;;) {
        int rr = avcodec_receive_frame(c.vdec, c.frm);
        if (rr < 0) break;
        const double before = g_t_csc;
        if (convert_frame(c, c.frm)) got = true;
        csc_ms += g_t_csc - before;
        av_frame_unref(c.frm);
    }
    const uint64_t dt = psp_prof::now() - t_avc0;
    // Decode time excludes the colour conversion (accounted separately).
    if (psp_prof::enabled) psp_prof::ns[psp_prof::S_MPEG_AVC] += dt - (uint64_t)(csc_ms * 1e6);
    if (g_ms.on) g_t_avc += dt / 1e6 - csc_ms;
    return got;
#else
    (void)au;
    c.vw = 480; c.vh = 272;
    c.rgba.assign((size_t)c.vw * c.vh * 4, 0);
    for (size_t i = 3; i < c.rgba.size(); i += 4) c.rgba[i] = 255;
    return true;
#endif
}

// Registry of guest movie frame buffers (see psp_mpeg_frame_info).
struct MovieBuf {
    std::atomic<uint32_t> addr{0};     // masked guest address, 0 = unused
    std::atomic<uint32_t> serial{0};
    std::atomic<int> rows{0};
    std::atomic<int> format{0};
};
constexpr int MOVIE_BUFS = 4;
MovieBuf g_movie_bufs[MOVIE_BUFS];
std::atomic<uint32_t> g_movie_serial{0};
std::atomic<int> g_movie_next{0};

void movie_buf_written(uint32_t dst, int rows, int format) {
    const uint32_t a = dst & PSP_ADDR_MASK;
    int slot = -1;
    for (int i = 0; i < MOVIE_BUFS; i++)
        if (g_movie_bufs[i].addr.load() == a) { slot = i; break; }
    if (slot < 0) slot = g_movie_next.fetch_add(1) % MOVIE_BUFS;
    MovieBuf& b = g_movie_bufs[slot];
    b.addr.store(0);   // hide the slot while it is retargeted
    b.rows.store(rows);
    b.format.store(format);
    b.serial.store(++g_movie_serial);
    b.addr.store(a);
}

uint32_t bytes_per_pixel(int mode) { return mode == 3 ? 4 : 2; }

// Write c.rgba into guest memory at `dst` with `stride` pixels per row.
void write_frame(uint8_t* rd, const MpegCtx& c, uint32_t dst, uint32_t stride, int pixelMode) {
    if (c.rgba.empty() || !addr_ok(dst)) return;
    psp_prof::Scope prof_wr(psp_prof::S_MPEG_WR);
    const uint64_t t_wr0 = g_ms.on ? psp_prof::now() : 0;
    struct TimerEnd { uint64_t t0; ~TimerEnd() { if (g_ms.on) g_t_wr += ms_since(t0); } } timer_end{t_wr0};
    const uint32_t bpp = bytes_per_pixel(pixelMode);
    const int w = c.vw, h = c.vh;
    if (stride < (uint32_t)w) stride = (uint32_t)w;
    if (!range_ok(dst, (uint64_t)(h - 1) * stride * bpp + (uint64_t)w * bpp)) return;
    uint8_t* base = rd + (dst & PSP_ADDR_MASK);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = &c.rgba[(size_t)y * w * 4];
        uint8_t* o = base + (size_t)y * stride * bpp;
        if (pixelMode == 3) {
            std::memcpy(o, s, (size_t)w * 4);
        } else {
            for (int x = 0; x < w; x++, s += 4, o += 2) {
                uint16_t p;
                uint32_t R = s[0], G = s[1], B = s[2];
                switch (pixelMode) {
                case 0: p = (uint16_t)((R >> 3) | ((G >> 2) << 5) | ((B >> 3) << 11)); break;
                case 1: p = (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | 0x8000); break;
                default: p = (uint16_t)((R >> 4) | ((G >> 4) << 4) | ((B >> 4) << 8) | 0xF000); break;
                }
                std::memcpy(o, &p, 2);
            }
        }
    }
    movie_buf_written(dst, h, pixelMode);
    if (std::getenv("PSPRECOMP_MOVIE_TEXDBG"))
    {
        uint32_t hh = 2166136261u;
        const uint32_t m0 = dst & PSP_ADDR_MASK;
        for (int y = 0; y < 272; y += 3) for (int x = 0; x < 2048; x += 61) hh = (hh ^ rd[m0 + y * 2048 + x]) * 16777619u;
        std::fprintf(stderr, "[MVWRITE] dst=%08X serial=%u hash=%08X\n", m0, g_movie_serial.load(), hh);
    }
}

// ---------------- audio decode ----------------

bool decode_audio(MpegCtx& c, const AudioAu& au, std::vector<int16_t>& pcm) {
    pcm.assign((size_t)ATRAC_SAMPLES * 2, 0);
#ifdef PSPRECOMP_HAVE_AVCODEC
    if (!c.adec || c.adecParams != au.params) {
        if (c.adec) avcodec_free_context(&c.adec);
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (!codec) { std::fprintf(stderr, "[MOVIE] no ATRAC3+ decoder in libavcodec\n"); return false; }
        static const int rates[8] = {32000, 44100, 48000, 88200, 96000, 0, 0, 0};
        int chId = (au.params >> 10) & 7;
        int ch = chId == 1 ? 1 : 2;
        c.adec = avcodec_alloc_context3(codec);
        c.adec->sample_rate = rates[(au.params >> 13) & 7] ? rates[(au.params >> 13) & 7] : 44100;
        c.adec->block_align = (int)au.data.size();
        av_channel_layout_default(&c.adec->ch_layout, ch);
        if (avcodec_open2(c.adec, codec, nullptr) < 0) {
            std::fprintf(stderr, "[MOVIE] ATRAC3+ open failed\n");
            avcodec_free_context(&c.adec);
            return false;
        }
        c.adecParams = au.params;
        if (!c.frm) c.frm = av_frame_alloc();
        std::fprintf(stderr, "[MOVIE] ATRAC3+ audio: params=0x%04X rate=%d ch=%d block=%zu\n",
                     au.params, c.adec->sample_rate, ch, au.data.size());
    }
    AVPacket* pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int)au.data.size()) < 0) { av_packet_free(&pkt); return false; }
    std::memcpy(pkt->data, au.data.data(), au.data.size());
    int r = avcodec_send_packet(c.adec, pkt);
    av_packet_free(&pkt);
    if (r < 0) return false;
    if (avcodec_receive_frame(c.adec, c.frm) < 0) return false;
    const int ns = std::min(c.frm->nb_samples, ATRAC_SAMPLES);
    const int srcCh = c.frm->ch_layout.nb_channels;
    for (int i = 0; i < ns; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int sc = std::min(ch, srcCh - 1);
            int16_t s = 0;
            switch ((AVSampleFormat)c.frm->format) {
            case AV_SAMPLE_FMT_FLTP: { float v = ((const float*)c.frm->data[sc])[i] * 32768.f; s = (int16_t)std::clamp(v, -32768.f, 32767.f); break; }
            case AV_SAMPLE_FMT_FLT:  { float v = ((const float*)c.frm->data[0])[i * srcCh + sc] * 32768.f; s = (int16_t)std::clamp(v, -32768.f, 32767.f); break; }
            case AV_SAMPLE_FMT_S16P: s = ((const int16_t*)c.frm->data[sc])[i]; break;
            case AV_SAMPLE_FMT_S16:  s = ((const int16_t*)c.frm->data[0])[i * srcCh + sc]; break;
            default: break;
            }
            pcm[(size_t)i * 2 + ch] = s;
        }
    }
    av_frame_unref(c.frm);
    return true;
#else
    (void)c; (void)au;
    return true;
#endif
}

// ---------------- SceMpegAu helpers ----------------

void au_write(uint8_t* rd, uint32_t a, int64_t pts, int64_t dts, uint32_t esBuf, uint32_t esSize) {
    w32(rd, a + 0, (uint32_t)((uint64_t)pts >> 32));
    w32(rd, a + 4, (uint32_t)pts);
    w32(rd, a + 8, (uint32_t)((uint64_t)dts >> 32));
    w32(rd, a + 12, (uint32_t)dts);
    w32(rd, a + 16, esBuf);
    w32(rd, a + 20, esSize);
}

// ---------------- guest ring-buffer callback ----------------

uint32_t g_cb_stack = 0;

int32_t call_ring_cb(uint8_t* rd, const recomp_context* caller, uint32_t fn_addr,
                     uint32_t data, int32_t packets, uint32_t arg) {
    FuncPtr fn = RECOMP_LOOKUP(fn_addr);
    if (!fn) {
        std::fprintf(stderr, "[MOVIE] ring callback 0x%08X: LOOKUP_MISS\n", fn_addr);
        return 0;
    }
    // psp_alloc_stack returns the LOWEST address of the block; sp starts at the top.
    if (!g_cb_stack) g_cb_stack = psp_alloc_stack(rd, 0x8000) + 0x8000 - 0x40;
    recomp_context c = *caller;   // inherit gp and friends
    c.r[4] = (int32_t)data;
    c.r[5] = packets;
    c.r[6] = (int32_t)arg;
    c.r[29] = (int32_t)g_cb_stack;
    c.r[31] = 0;
    fn(rd, &c);
    return (int32_t)c.r[2];
}

// ================================================================
// HLE handlers
// ================================================================

// PSPRECOMP_NO_MPEG=1: report the MPEG library as unavailable (fast test cycles).
bool no_mpeg() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("PSPRECOMP_NO_MPEG");
        v = (e && e[0] == '1') ? 1 : 0;
        if (v) std::fprintf(stderr, "[MOVIE] disabled by PSPRECOMP_NO_MPEG\n");
    }
    return v == 1;
}

void h_Init(uint8_t*, recomp_context* c) {
    uint32_t r = no_mpeg() ? 0x80610005u : 0;
    RET(c, r); tr("sceMpegInit", c, r);
}
void h_Finish(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegFinish", c, 0); }

void h_QueryMemSize(uint8_t*, recomp_context* c) { RET(c, MPEG_MEMSIZE); tr("sceMpegQueryMemSize", c, MPEG_MEMSIZE); }

void h_Create(uint8_t* rd, recomp_context* c) {
    uint32_t mpeg = A(c, 0), data = A(c, 1), size = A(c, 2), rbuf = A(c, 3);
    uint32_t fw = A(c, 4), mode = A(c, 5), ddr = A(c, 6);
    uint32_t ret = 0;
    if (no_mpeg()) ret = 0x80610005u;
    else if (!addr_ok(mpeg) || !addr_ok(data)) ret = ERR_INVALID_ADDR;
    else if ((int32_t)size < (int32_t)MPEG_MEMSIZE) ret = ERR_NO_MEMORY;
    else {
        auto ctxp = std::make_unique<MpegCtx>();
        MpegCtx* mc = ctxp.get();
        mc->handle = data + 0x30;
        mc->ringbuf = rbuf;
        mc->frameWidth = fw; mc->mode = mode; mc->ddrTop = ddr;
        if (range_ok(data, 0x30)) {
            std::memcpy(rd + (data & PSP_ADDR_MASK), "LIBMPEG\0", 8);
            std::memcpy(rd + (data & PSP_ADDR_MASK) + 8, "001\0", 4);
        }
        w32(rd, data + 12, 0xFFFFFFFFu);
        if (addr_ok(rbuf)) {
            w32(rd, data + 16, rbuf);
            w32(rd, rbuf + RB_MPEG, data);
        }
        w32(rd, mpeg, mc->handle);
        g_ctxs[mc->handle] = std::move(ctxp);
    }
    RET(c, ret);
    tr("sceMpegCreate", c, ret);
}

void h_Delete(uint8_t* rd, recomp_context* c) {
    if (addr_ok(A(c, 0))) g_ctxs.erase(r32(rd, A(c, 0)));
    RET(c, 0); tr("sceMpegDelete", c, 0);
}

void h_QueryStreamOffset(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t buf = A(c, 1), out = A(c, 2), ret = 0;
    Psmf p;
    if (!parse_psmf(rd, buf, p, true)) { w32(rd, out, 0); ret = ERR_INVALID_VALUE; }
    else if (p.streamOffset == 0 || (p.streamOffset & 2047)) { w32(rd, out, 0); ret = ERR_INVALID_VALUE; }
    else {
        w32(rd, out, p.streamOffset);
        g_last_psmf = p;
        if (m) m->psmf = p;
    }
    RET(c, ret); tr("sceMpegQueryStreamOffset", c, ret);
}

void h_QueryStreamSize(uint8_t* rd, recomp_context* c) {
    uint32_t buf = A(c, 0), out = A(c, 1), ret = 0;
    Psmf p;
    if (!parse_psmf(rd, buf, p, true)) { w32(rd, out, 0); ret = ERR_INVALID_VALUE; }
    else if (p.streamSize & 2047) { w32(rd, out, 0); ret = ERR_INVALID_VALUE; }
    else { w32(rd, out, p.streamSize); g_last_psmf = p; }
    RET(c, ret); tr("sceMpegQueryStreamSize", c, ret);
}

void h_RegistStream(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    int type = (int)A(c, 1), num = (int)A(c, 2);
    uint32_t ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else {
        ret = g_next_stream_uid++;
        m->streams[ret] = Stream{type, num};
        if (type == ST_AVC) m->avcCh = num & 0xF;
        else if (type == ST_ATRAC) m->atracCh = num & 0xF;
    }
    RET(c, ret); tr("sceMpegRegistStream", c, ret);
}

void h_UnRegistStream(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    if (m) {
        auto it = m->streams.find(A(c, 1));
        if (it != m->streams.end()) {
            if (it->second.type == ST_AVC) m->avcCh = -1;
            if (it->second.type == ST_ATRAC) m->atracCh = -1;
            m->streams.erase(it);
        }
    }
    RET(c, 0); tr("sceMpegUnRegistStream", c, 0);
}

void h_FlushAllStream(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegFlushAllStream", c, 0); }
void h_FlushStream(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegFlushStream", c, 0); }
void h_ChangeGetAuMode(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegChangeGetAuMode", c, 0); }

void h_MallocAvcEsBuf(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t ret = 0;
    if (m) for (int i = 0; i < 4; i++) if (!m->esUsed[i]) { m->esUsed[i] = true; ret = i + 1; break; }
    RET(c, ret); tr("sceMpegMallocAvcEsBuf", c, ret);
}

void h_FreeAvcEsBuf(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t id = A(c, 1);
    if (m && id >= 1 && id <= 4) m->esUsed[id - 1] = false;
    RET(c, 0); tr("sceMpegFreeAvcEsBuf", c, 0);
}

void h_InitAu(uint8_t* rd, recomp_context* c) {
    uint32_t esBuf = A(c, 1), au = A(c, 2), ret = 0;
    if (!addr_ok(au)) ret = ERR_INVALID_ADDR;
    else au_write(rd, au, -1, -1, esBuf, 0);
    RET(c, ret); tr("sceMpegInitAu", c, ret);
}

void h_QueryAtracEsSize(uint8_t* rd, recomp_context* c) {
    w32(rd, A(c, 1), MPEG_ATRAC_ES_SIZE);
    w32(rd, A(c, 2), MPEG_ATRAC_OUT_SIZE);
    RET(c, 0); tr("sceMpegQueryAtracEsSize", c, 0);
}

// ---- ring buffer ----

void h_RbQueryMemSize(uint8_t*, recomp_context* c) {
    uint32_t r = (uint32_t)((int32_t)A(c, 0) * (104 + 2048));
    RET(c, r); tr("sceMpegRingbufferQueryMemSize", c, r);
}

void h_RbConstruct(uint8_t* rd, recomp_context* c) {
    uint32_t rb = A(c, 0), packets = A(c, 1), data = A(c, 2), size = A(c, 3);
    uint32_t cb = A(c, 4), cbArg = A(c, 5), ret = 0;
    if (!addr_ok(rb)) ret = ERR_INVALID_ADDR;
    else if ((int32_t)packets <= 0) ret = ERR_INVALID_VALUE;
    else if ((int32_t)size < 0 || size < packets * PACKET_SIZE) ret = ERR_NO_MEMORY;
    else {
        Ring r{rd, rb};
        r.set(RB_PACKETS, packets);
        r.set(RB_READ, 0);
        r.set(RB_WRITTEN, 0);
        r.set(RB_FREE, packets);
        r.set(RB_PKTSIZE, PACKET_SIZE);
        r.set(RB_DATA, data);
        r.set(RB_CB, cb);
        r.set(RB_CBARG, cbArg);
        r.set(RB_UPPER, data + packets * PACKET_SIZE);
        r.set(RB_SEMA, 0);
        r.set(RB_MPEG, 0);
        r.set(RB_GP, (uint32_t)c->r[28]);
    }
    RET(c, ret); tr("sceMpegRingbufferConstruct", c, ret);
}

void h_RbDestruct(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegRingbufferDestruct", c, 0); }

void h_RbAvailableSize(uint8_t* rd, recomp_context* c) {
    uint32_t ret = 0;
    if (addr_ok(A(c, 0))) ret = r32(rd, A(c, 0) + RB_FREE);
    RET(c, ret); tr("sceMpegRingbufferAvailableSize", c, ret);
}

// sceMpegRingbufferPut(ringbuffer, numPackets, available) -> packets added
void h_RbPut(uint8_t* rd, recomp_context* c) {
    uint32_t rba = A(c, 0);
    int32_t num = (int32_t)A(c, 1), avail = (int32_t)A(c, 2);
    int32_t ret = 0;
    if (!addr_ok(rba)) { RET(c, ERR_INVALID_ADDR); tr("sceMpegRingbufferPut", c, ERR_INVALID_ADDR); return; }
    Ring rb{rd, rba};
    MpegCtx* m = nullptr;
    {
        uint32_t mp = rb.get(RB_MPEG);
        auto it = g_ctxs.find(mp + 0x30);
        if (it != g_ctxs.end()) m = it->second.get();
    }
    const uint32_t packets = rb.get(RB_PACKETS);
    num = std::min(num, avail);
    num = std::min<int32_t>(num, (int32_t)rb.get(RB_FREE));
    int32_t total = 0;
    int guard = 0;
    while (num > 0 && packets && guard++ < 4) {
        uint32_t written = rb.get(RB_WRITTEN);
        uint32_t wo = written % packets;
        int32_t chunk = std::min<int32_t>(num, (int32_t)(packets - wo));
        int32_t got = call_ring_cb(rd, c, rb.get(RB_CB), rb.get(RB_DATA) + wo * PACKET_SIZE,
                                   chunk, rb.get(RB_CBARG));
        if (got < 0) { if (total == 0) total = got; break; }
        if (got == 0) { if (m) m->eof = true; break; }
        got = std::min(got, chunk);
        rb.set(RB_WRITTEN, written + (uint32_t)got);
        rb.set(RB_FREE, rb.get(RB_FREE) - (uint32_t)got);
        total += got;
        num -= got;
        if (got < chunk) break;
    }
    ret = total;
    RET(c, (uint32_t)ret); tr("sceMpegRingbufferPut", c, (uint32_t)ret);
}

// ---- access units ----

void h_GetAvcAu(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t au = A(c, 2), attr = A(c, 3), ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else if (!addr_ok(au)) ret = ERR_INVALID_ADDR;
    else if (!fill_video(rd, *m)) ret = ERR_NO_DATA;
    else {
        VideoAu a = std::move(m->vidQ.front());
        m->vidQ.pop_front();
        int64_t pts = a.pts, dts = a.dts;
        m->vidIssued.push_back(std::move(a));
        au_write(rd, au, pts, dts, r32(rd, au + 16), MPEG_AVC_ES_SIZE);
        w32(rd, attr, 1);
    }
    RET(c, ret); tr("sceMpegGetAvcAu", c, ret);
}

void h_GetAtracAu(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t au = A(c, 2), attr = A(c, 3), ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else if (!addr_ok(au)) ret = ERR_INVALID_ADDR;
    else if (!fill_audio(rd, *m)) ret = ERR_NO_DATA;
    else {
        AudioAu a = std::move(m->audQ.front());
        m->audQ.pop_front();
        int64_t pts = a.pts;
        m->audIssued.push_back(std::move(a));
        au_write(rd, au, pts, pts, r32(rd, au + 16), MPEG_ATRAC_ES_SIZE);
        w32(rd, attr, 1);
    }
    RET(c, ret); tr("sceMpegGetAtracAu", c, ret);
}

void h_GetPcmAu(uint8_t*, recomp_context* c) { RET(c, ERR_NO_DATA); tr("sceMpegGetPcmAu", c, ERR_NO_DATA); }

// ---- decode ----

uint32_t resolve_buffer(uint8_t* rd, uint32_t bufferAddr) {
    // PPSSPP: bufferAddr points at a u32 holding the real frame buffer.
    // Accept a direct VRAM address too (VRAM contents are not a pointer).
    auto is_vram = [](uint32_t a) { uint32_t p = a & 0x1FFFFFFFu; return p >= 0x04000000u && p < 0x04200000u; };
    auto is_ram = [](uint32_t a) { uint32_t p = a & 0x1FFFFFFFu; return p >= 0x08000000u && p < 0x0C000000u; };
    if (is_vram(bufferAddr)) return bufferAddr;
    uint32_t v = r32(rd, bufferAddr);
    if (is_vram(v) || is_ram(v)) return v;
    return 0;   // not a plausible frame buffer: never scribble over the caller's stack
}

uint32_t decode_common(uint8_t* rd, MpegCtx& m, uint32_t frameWidth, uint32_t bufferAddr,
                       uint32_t initAddr, bool toGuest) {
    if (!m.vidIssued.empty()) {
        VideoAu au = std::move(m.vidIssued.front());
        m.vidIssued.pop_front();
        const uint64_t t_call0 = g_ms.on ? psp_prof::now() : 0;
        g_t_avc = g_t_csc = g_t_wr = 0;
        bool got = decode_video(m, au);
        if (got) { m.haveFrame = true; m.videoFrames++; }
        if (got && toGuest) {
            uint32_t fw = frameWidth ? frameWidth : (m.frameWidth ? m.frameWidth : 512);
            write_frame(rd, m, resolve_buffer(rd, bufferAddr), fw, m.pixelMode);
        }
        g_ms.vpts = au.pts;
        if (g_ms.on && got) g_ms.frame(g_t_avc, g_t_csc, g_t_wr, ms_since(t_call0));
        w32(rd, initAddr, got ? 1 : 0);
    } else {
        w32(rd, initAddr, 0);
    }
    return 0;
}

void h_AvcDecode(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else {
        static const bool dbg = std::getenv("PSPRECOMP_MOVIE_TEXDBG") != nullptr;
        if (dbg) std::fprintf(stderr, "[MVDEC] ra=%08X bufptr=%08X words=%08X %08X %08X %08X a4=%08X\n", (uint32_t)c->r[31], A(c, 3),
                              r32(rd, A(c, 3)), r32(rd, A(c, 3) + 4), r32(rd, A(c, 3) + 8), r32(rd, A(c, 3) + 12), A(c, 4));
        if (dbg) { uint32_t S = r32(rd, 0x089D7F00u);
            std::fprintf(stderr, "[MVRING] S=%08X N=%u cnt=%u base=%08X stride=%08X flags=%08X f320=%08X\n", S, r32(rd, S + 368), r32(rd, S + 372),
                         r32(rd, S + 364), r32(rd, S + 380), r32(rd, S + 16), r32(rd, S + 320)); }
        const uint32_t i0 = r32(rd, A(c, 4));
        ret = decode_common(rd, *m, A(c, 2), A(c, 3), A(c, 4), true);
        if (dbg) std::fprintf(stderr, "[MVDEC]   init before=%u after=%u words after=%08X %08X %08X\n", i0, r32(rd, A(c, 4)),
                              r32(rd, A(c, 3)), r32(rd, A(c, 3) + 4), r32(rd, A(c, 3) + 8));
    }
    RET(c, ret); tr("sceMpegAvcDecode", c, ret);
}

void h_AvcDecodeDetail(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t d = A(c, 1);
    if (m && addr_ok(d)) {
        w32(rd, d + 0, 0);
        w32(rd, d + 4, m->videoFrames);
        w32(rd, d + 8, (uint32_t)m->vw);
        w32(rd, d + 12, (uint32_t)m->vh);
        for (int i = 16; i < 32; i += 4) w32(rd, d + i, 0);
        w32(rd, d + 32, m->haveFrame ? 1 : 0);
    }
    RET(c, 0); tr("sceMpegAvcDecodeDetail", c, 0);
}

void h_AvcDecodeStop(uint8_t* rd, recomp_context* c) {
    w32(rd, A(c, 3), 0);
    RET(c, 0); tr("sceMpegAvcDecodeStop", c, 0);
}

void h_AvcDecodeFlush(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegAvcDecodeFlush", c, 0); }

void h_AvcDecodeMode(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t ret = 0, ma = A(c, 1);
    if (!m) ret = ERR_INVALID_VALUE;
    else if (addr_ok(ma)) {
        int32_t pm = (int32_t)r32(rd, ma + 4);
        if (pm >= 0 && pm <= 3) m->pixelMode = pm; else ret = ERR_INVALID_VALUE;
    }
    RET(c, ret); tr("sceMpegAvcDecodeMode", c, ret);
}

// YCbCr path: decode now, keep the RGB frame host-side; sceMpegAvcCsc
// writes it out. (Real hardware emits MB-blocked YCbCr; not modelled.)
void h_AvcDecodeYCbCr(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t ret = m ? decode_common(rd, *m, 0, 0, A(c, 3), false) : ERR_INVALID_VALUE;
    RET(c, ret); tr("sceMpegAvcDecodeYCbCr", c, ret);
}

void h_AvcDecodeStopYCbCr(uint8_t* rd, recomp_context* c) {
    w32(rd, A(c, 2), 0);
    RET(c, 0); tr("sceMpegAvcDecodeStopYCbCr", c, 0);
}

void h_AvcQueryYCbCrSize(uint8_t* rd, recomp_context* c) {
    uint32_t w = A(c, 2), h = A(c, 3);
    w32(rd, A(c, 4), ((w + 15) / 16) * ((h + 15) / 16) * 384);
    RET(c, 0); tr("sceMpegAvcQueryYCbCrSize", c, 0);
}

void h_AvcInitYCbCr(uint8_t*, recomp_context* c) { RET(c, 0); tr("sceMpegAvcInitYCbCr", c, 0); }

// sceMpegAvcCsc(mpeg, source, range, frameWidth, dest)
void h_AvcCsc(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else if (m->haveFrame) {
        uint32_t fw = A(c, 3) ? A(c, 3) : (m->frameWidth ? m->frameWidth : 512);
        write_frame(rd, *m, A(c, 4), fw, m->pixelMode);
    }
    RET(c, ret); tr("sceMpegAvcCsc", c, ret);
}

void h_AtracDecode(uint8_t* rd, recomp_context* c) {
    MpegCtx* m = get_ctx(rd, A(c, 0));
    uint32_t buf = A(c, 2), ret = 0;
    if (!m) ret = ERR_INVALID_VALUE;
    else {
        std::vector<int16_t> pcm((size_t)ATRAC_SAMPLES * 2, 0);
        if (!m->audIssued.empty()) {
            AudioAu au = std::move(m->audIssued.front());
            m->audIssued.pop_front();
            psp_prof::Scope prof_aud(psp_prof::S_MPEG_AUD);
            const uint64_t t_a0 = g_ms.on ? psp_prof::now() : 0;
            decode_audio(*m, au, pcm);
            g_ms.apts = au.pts;
            if (g_ms.on) { g_ms.aud += ms_since(t_a0); g_ms.aud_n++; }
        }
        if (range_ok(buf, MPEG_ATRAC_OUT_SIZE))
            std::memcpy(rd + (buf & PSP_ADDR_MASK), pcm.data(), MPEG_ATRAC_OUT_SIZE);
    }
    RET(c, ret); tr("sceMpegAtracDecode", c, ret);
}

}  // namespace

bool psp_mpeg_frame_info(uint32_t masked_addr, int pixel_format, uint32_t* serial, int* rows) {
    for (int i = 0; i < MOVIE_BUFS; i++) {
        MovieBuf& b = g_movie_bufs[i];
        if (b.addr.load() != masked_addr || masked_addr == 0) continue;
        if (b.format.load() != pixel_format) return false;
        *serial = b.serial.load();
        *rows = b.rows.load();
        return true;
    }
    return false;
}

void psp_hle_register_mpeg() {
    psp_hle_register("sceMpegInit", h_Init);
    psp_hle_register("sceMpegFinish", h_Finish);
    psp_hle_register("sceMpegQueryMemSize", h_QueryMemSize);
    psp_hle_register("sceMpegCreate", h_Create);
    psp_hle_register("sceMpegDelete", h_Delete);
    psp_hle_register("sceMpegQueryStreamOffset", h_QueryStreamOffset);
    psp_hle_register("sceMpegQueryStreamSize", h_QueryStreamSize);
    psp_hle_register("sceMpegRegistStream", h_RegistStream);
    psp_hle_register("sceMpegUnRegistStream", h_UnRegistStream);
    psp_hle_register("sceMpegFlushAllStream", h_FlushAllStream);
    psp_hle_register("sceMpegFlushStream", h_FlushStream);
    psp_hle_register("sceMpegChangeGetAuMode", h_ChangeGetAuMode);
    psp_hle_register("sceMpegMallocAvcEsBuf", h_MallocAvcEsBuf);
    psp_hle_register("sceMpegFreeAvcEsBuf", h_FreeAvcEsBuf);
    psp_hle_register("sceMpegInitAu", h_InitAu);
    psp_hle_register("sceMpegQueryAtracEsSize", h_QueryAtracEsSize);
    psp_hle_register("sceMpegRingbufferQueryMemSize", h_RbQueryMemSize);
    psp_hle_register("sceMpegRingbufferConstruct", h_RbConstruct);
    psp_hle_register("sceMpegRingbufferDestruct", h_RbDestruct);
    psp_hle_register("sceMpegRingbufferAvailableSize", h_RbAvailableSize);
    psp_hle_register("sceMpegRingbufferPut", h_RbPut);
    psp_hle_register("sceMpegGetAvcAu", h_GetAvcAu);
    psp_hle_register("sceMpegGetAtracAu", h_GetAtracAu);
    psp_hle_register("sceMpegGetPcmAu", h_GetPcmAu);
    psp_hle_register("sceMpegAvcDecode", h_AvcDecode);
    psp_hle_register("sceMpegAvcDecodeDetail", h_AvcDecodeDetail);
    psp_hle_register("sceMpegAvcDecodeStop", h_AvcDecodeStop);
    psp_hle_register("sceMpegAvcDecodeFlush", h_AvcDecodeFlush);
    psp_hle_register("sceMpegAvcDecodeMode", h_AvcDecodeMode);
    psp_hle_register("sceMpegAvcDecodeYCbCr", h_AvcDecodeYCbCr);
    psp_hle_register("sceMpegAvcDecodeStopYCbCr", h_AvcDecodeStopYCbCr);
    psp_hle_register("sceMpegAvcQueryYCbCrSize", h_AvcQueryYCbCrSize);
    psp_hle_register("sceMpegAvcInitYCbCr", h_AvcInitYCbCr);
    psp_hle_register("sceMpegAvcCsc", h_AvcCsc);
    psp_hle_register("sceMpegAtracDecode", h_AtracDecode);
}
