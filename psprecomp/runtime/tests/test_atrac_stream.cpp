// Host test for the sceAtrac HLE streaming path: streams a real looping AT3+
// BGM file through it the way Dissidia's player does (small ring,
// GetStreamDataInfo/AddStreamData, SetLoopNum(-1)) and fails if the music goes
// silent, e.g. after the first loop (the battle BGM bug fixed 2026-10-22).
// Run: runtime/tests/run_atrac_stream.sh [file.AT3] [ringBytes] [passes]
#include "../src/hle/psp_hle_atrac.cpp"
#include <sys/mman.h>
#include <map>
#include <string>
#include <cstdlib>
static std::map<std::string, HleFunc> g_fn;
void psp_hle_register(const char* n, HleFunc f) { g_fn[n] = f; }
static uint8_t* R;
static recomp_context C;
static int32_t call(const char* n, uint32_t a0=0, uint32_t a1=0, uint32_t a2=0, uint32_t a3=0, uint32_t a4=0) {
    C.r[4]=a0; C.r[5]=a1; C.r[6]=a2; C.r[7]=a3; C.r[8]=a4; g_fn.at(n)(R, &C); return C.r[2];
}
static uint32_t rd(uint32_t a) { return psp_mem_read<uint32_t>(R, a); }
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "B14.AT3";
    const uint32_t ringSize = argc > 2 ? strtoul(argv[2], 0, 0) : 98000;
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
    std::vector<uint8_t> file; { fseek(f,0,SEEK_END); file.resize(ftell(f)); fseek(f,0,SEEK_SET); if (fread(file.data(),1,file.size(),f) != file.size()) return 2; fclose(f); }
    R = (uint8_t*)mmap(nullptr, PSP_MEM_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    psp_hle_register_atrac();
    const uint32_t BUF = 0x09000000, OUT = 0x09800000, V = 0x09900000;
    uint32_t first = std::min<uint32_t>(ringSize, file.size());
    memcpy(R + (BUF & PSP_ADDR_MASK), file.data(), first);
    int id = call("sceAtracSetHalfwayBufferAndGetID", BUF, first, ringSize);
    if (id < 0) { printf("SetHalfway failed %08x\n", id); return 1; }
    call("sceAtracGetSoundSample", id, V, V+4, V+8);
    int endS = rd(V), ls = rd(V+4), le = rd(V+8);
    printf("endSample %d loop %d..%d (%.1f s track)\n", endS, ls, le, endS/44100.0);
    if (!getenv("NOLOOP")) printf("SetLoopNum -> %08x\n", call("sceAtracSetLoopNum", id, (uint32_t)-1));
    const double total = argc > 3 ? atof(argv[3]) : 2.6;  // track lengths to play
    std::vector<int16_t> pcm;
    long long played = 0; int adds = 0, errs = 0;
    while (played < (long long)(endS * total)) {
        // refill like the game: whenever there's room
        call("sceAtracGetStreamDataInfo", id, V, V+4, V+8);
        uint32_t wp = rd(V), wr = rd(V+4), ro = rd(V+8);
        if (wr > 0 && ro < file.size()) {
            uint32_t n = std::min<uint32_t>(wr, file.size() - ro);
            memcpy(R + (wp & PSP_ADDR_MASK), file.data() + ro, n);
            int32_t e = call("sceAtracAddStreamData", id, n);
            if (e) { printf("AddStreamData %08x\n", e); errs++; }
            adds++;
        }
        int32_t e = call("sceAtracDecodeData", id, OUT, V+16, V+20, V+24);
        int ns = rd(V+16), fin = rd(V+20);
        if (e) { printf("DecodeData err %08x at %lld\n", e, played); errs++; break; }
        for (int i = 0; i < ns*2; i++) pcm.push_back(psp_mem_read<int16_t>(R, OUT + 2*i));
        played += ns;
        if (fin) { printf("finish flag at %lld samples (%.1f s)\n", played, played/44100.0); break; }
        if (ns == 0) { printf("ns=0 at %lld\n", played); errs++; break; }
    }
    // loudness per second
    int silentSecs = 0, firstSilent = -1;
    int secs = (int)(pcm.size()/2/44100);
    for (int s = 0; s < secs; s++) {
        double acc = 0; for (int i = 0; i < 44100*2; i++) { double v = pcm[(size_t)s*88200+i]; acc += v*v; }
        double rms = sqrt(acc/88200);
        if (rms < 30) { silentSecs++; if (firstSilent < 0) firstSilent = s; }
    }
    printf("played %.1f s, %d refills, silent seconds %d (first at %d s)\n", played/44100.0, adds, silentSecs, firstSilent);
    // The second pass must repeat the first exactly: the loop ends after sample le and restarts at ls.
    double corr = -2;
    const long long p2 = (long long)le + 1 + 44100*5, p1 = (long long)ls + 44100*5;
    if (ls >= 0 && (size_t)(p2+44100)*2 <= pcm.size()) {
        double num=0, d1=0, d2=0;
        for (long long i = 0; i < 44100*2; i++) { double x=pcm[p1*2+i], y=pcm[p2*2+i]; num+=x*y; d1+=x*x; d2+=y*y; }
        corr = num/sqrt(d1*d2+1e-9);
        printf("pass 2 vs pass 1, 5 s after the loop start: correlation %.6f\n", corr);
    }
    FILE* o = fopen("out.raw","wb"); fwrite(pcm.data(),2,pcm.size(),o); fclose(o);
    const bool bad = errs || silentSecs > 2 || (corr > -2 && corr < 0.999);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
