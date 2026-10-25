// Standalone host harness for runtime/src/hle/psp_hle_mpeg.cpp.
// Maps a fake guest RAM, loads a .PMF into a guest ring buffer 2048-byte
// packets at a time (as sceMpegRingbufferPut would), and drives the real demux
// (GetAvcAu / GetAtracAu paths) + libavcodec decode. Prints AU boundaries,
// PTS, ring accounting. Usage: mpeg_harness file.pmf [maxVideoAus] [-v]
// Build: see tools/mpeg_harness/build.sh
#include "../../runtime/src/hle/psp_hle_mpeg.cpp"
#include <sys/mman.h>

// ---- link stubs ----
void psp_hle_register(const char*, HleFunc) {}
uint32_t psp_alloc_stack(uint8_t*, uint32_t) { return 0x09000000; }
FuncPtr RECOMP_LOOKUP(uint32_t) { return nullptr; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage\n"); return 1; }
    int maxAus = argc > 2 ? std::atoi(argv[2]) : 200;
    bool verbose = argc > 3;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 1; }
    uint8_t* rd = (uint8_t*)mmap(nullptr, PSP_MEM_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    std::vector<uint8_t> hdr(0x800);
    std::fread(hdr.data(), 1, 0x800, f);
    std::memcpy(rd + (0x08900000 & PSP_ADDR_MASK), hdr.data(), 0x800);
    Psmf p;
    parse_psmf(rd, 0x08900000, p, true);

    const uint32_t PACKETS = 0x264, RB = 0x08A00000, DATA = 0x08B00000;
    Ring rb{rd, RB};
    rb.set(RB_PACKETS, PACKETS); rb.set(RB_FREE, PACKETS); rb.set(RB_DATA, DATA);
    MpegCtx m;
    m.ringbuf = RB; m.avcCh = 0; m.atracCh = 0;
    std::fseek(f, p.streamOffset, SEEK_SET);

    auto put = [&](int n) {  // emulate Put with a file-reading callback
        n = std::min<int>(n, (int)rb.get(RB_FREE));
        int total = 0;
        while (n > 0) {
            uint32_t w = rb.get(RB_WRITTEN), wo = w % PACKETS;
            int chunk = std::min<int>(n, PACKETS - wo);
            size_t got = std::fread(rd + ((DATA + wo * PACKET_SIZE) & PSP_ADDR_MASK), PACKET_SIZE, chunk, f);
            if (got == 0) { m.eof = true; break; }
            rb.set(RB_WRITTEN, w + got); rb.set(RB_FREE, rb.get(RB_FREE) - got);
            total += got; n -= got;
            if ((int)got < chunk) break;
        }
        return total;
    };

    int vAus = 0, aAus = 0, frames = 0, aFrames = 0, lastFree = -1;
    int64_t prevPts = -1, prevAPts = -1;
    bool savedPpm = false;
    int idle = 0;
    while (vAus < maxAus && idle < 1000) {
        uint32_t freeP = rb.get(RB_FREE);
        if ((int)freeP != lastFree && verbose)
            std::printf("  ring: read=%u written=%u free=%u\n", rb.get(RB_READ), rb.get(RB_WRITTEN), freeP);
        lastFree = freeP;
        if (freeP >= 32) put(32);
        bool progressed = false;
        if (fill_video(rd, m)) {
            VideoAu a = std::move(m.vidQ.front()); m.vidQ.pop_front();
            int nalAud = a.data.size() > 5;
            std::printf("V AU %3d size=%6zu pts=%lld dts=%lld dPts=%lld  ringRead=%u free=%u\n", vAus,
                        a.data.size(), (long long)a.pts, (long long)a.dts,
                        prevPts < 0 ? 0 : (long long)(a.pts - prevPts), rb.get(RB_READ), rb.get(RB_FREE));
            (void)nalAud;
            prevPts = a.pts;
            vAus++;
            if (decode_video(m, a)) {
                frames++;
                if (!savedPpm) {
                    savedPpm = true;
                    FILE* o = std::fopen("/tmp/mpeg_frame0.ppm", "wb");
                    std::fprintf(o, "P6 %d %d 255\n", m.vw, m.vh);
                    for (size_t i = 0; i < m.rgba.size(); i += 4) std::fwrite(&m.rgba[i], 1, 3, o);
                    std::fclose(o);
                    std::printf("  first decoded frame %dx%d -> /tmp/mpeg_frame0.ppm\n", m.vw, m.vh);
                }
            }
            progressed = true;
        }
        while (fill_audio(rd, m)) {
            AudioAu a = std::move(m.audQ.front()); m.audQ.pop_front();
            if (aAus < 12 || verbose)
                std::printf("A AU %3d size=%4zu params=0x%04X pts=%lld dPts=%lld\n", aAus, a.data.size(),
                            a.params, (long long)a.pts, prevAPts < 0 ? 0 : (long long)(a.pts - prevAPts));
            prevAPts = a.pts;
            std::vector<int16_t> pcm;
            if (decode_audio(m, a, pcm)) {
                int64_t e = 0; for (auto v : pcm) e += std::abs(v);
                if (aAus < 3 || aAus % 100 == 0) std::printf("  decoded audio frame mean|s|=%lld\n", (long long)(e / pcm.size()));
                aFrames++;
            }
            aAus++;
            progressed = true;
        }
        idle = progressed ? 0 : idle + 1;
    }
    std::printf("SUMMARY: videoAUs=%d decodedFrames=%d audioAUs=%d decodedAudio=%d ringRead=%u written=%u free=%u\n",
                vAus, frames, aAus, aFrames, rb.get(RB_READ), rb.get(RB_WRITTEN), rb.get(RB_FREE));
    return 0;
}
