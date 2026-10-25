// Host test for runtime/package_mods.{h,cpp} against the real PACKAGE_INFO.BIN.
//   tests/run_package_mods_test.sh [PACKAGE_INFO.BIN]
// (default: $PSPRECOMP_DISC0/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN, else
// /home/claude/recomp/psprecomp_work/disc0/...). Skips (exit 0) with a message
// when the file is absent: the repo ships no game data.
#include "../runtime/package_mods.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace dissidia;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void spit(const std::string& p, const std::vector<uint8_t>& d) {
    size_t s = p.find_last_of('/');
    std::string dir;
    for (size_t i = 0; i < p.size(); i++) if (p[i] == '/' && i > 0) ::mkdir(p.substr(0, i).c_str(), 0755);
    (void)s; (void)dir;
    std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), d.size());
}
static uint8_t pat(uint64_t off) { return (uint8_t)((off * 2654435761u) >> 13); }   // fake PACKAGE.BIN content

int main(int argc, char** argv) {
    std::string info_path = argc > 1 ? argv[1] : "";
    if (info_path.empty()) {
        const char* d0 = std::getenv("PSPRECOMP_DISC0");
        info_path = std::string(d0 ? d0 : "/home/claude/recomp/psprecomp_work/disc0") + "/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN";
    }
    std::vector<uint8_t> orig = slurp(info_path);
    if (orig.empty()) { std::printf("SKIP: %s not found\n", info_path.c_str()); return 0; }

    // ---- CRC ----
    CHECK(package_crc("") == 0x00000000u);                    // ~0xFFFFFFFF
    CHECK(package_crc("123456789") == 0xFC891918u);           // CRC-32/BZIP2 check value
    CHECK(normalize_asset_path(".\\OBJ\\P_FIV100_1P.GMO") == "obj/p_fiv100_1p.gmo");

    // ---- index format ----
    PackageIndex idx; std::string err;
    CHECK(idx.parse(orig.data(), orig.size(), &err));
    CHECK(idx.magic == kIndexMagic);
    CHECK(16 + idx.entries.size() * 12 == orig.size());
    CHECK(idx.serialize() == orig);
    bool sorted = true, tiled = true;
    uint64_t end = 0;
    for (size_t i = 0; i + 1 < idx.entries.size(); i++) sorted &= idx.entries[i].crc < idx.entries[i + 1].crc;
    std::vector<IndexEntry> by_sector = idx.entries;
    std::sort(by_sector.begin(), by_sector.end(), [](auto& a, auto& b) { return a.sector < b.sector; });
    for (auto& e : by_sector) { tiled &= (e.sector == end); end = (uint64_t)e.sector + e.sectors(); }
    CHECK(sorted); CHECK(tiled);
    uint64_t package_size = end * kSectorSize;
    std::printf("index: %zu entries, PACKAGE.BIN = %llu bytes (%llu sectors)\n", idx.entries.size(),
                (unsigned long long)package_size, (unsigned long long)end);

    // ---- shipped path list: every line must hash to a real entry ----
    {
        std::string pl = "package_paths.txt";
        const char* cands[] = {"../package_paths.txt", "games/dissidia/package_paths.txt", "package_paths.txt"};
        std::ifstream f; for (auto c : cands) { f.open(c); if (f) break; f.clear(); }
        if (f) {
            std::string line; size_t n = 0, bad = 0; std::vector<char> hit(idx.entries.size(), 0);
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                n++;
                const IndexEntry* e = idx.find(package_crc(line));
                if (!e) { bad++; continue; }
                hit[e - idx.entries.data()] = 1;
            }
            size_t named = 0; for (char h : hit) named += h;
            std::printf("package_paths.txt: %zu paths, %zu unmatched, %zu/%zu entries named (%.1f%%)\n", n, bad, named,
                        idx.entries.size(), 100.0 * named / idx.entries.size());
            CHECK(bad == 0);
            CHECK(idx.find(package_crc("obj/p_fiv100_1p.gmo")) != nullptr);
        } else std::printf("note: package_paths.txt not found from cwd, skipped\n");
    }

    // ---- overrides ----
    char tmpl[] = "/tmp/pkgmods_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    const std::string gmo = "obj/p_fiv100_1p.gmo";
    const std::string objx = "obj/p_fiv100.objx";
    const IndexEntry* eg = idx.find(package_crc(gmo));
    const IndexEntry* eo = idx.find(package_crc(objx));
    CHECK(eg && eo);
    if (!eg || !eo) return 1;
    IndexEntry g0 = *eg, o0 = *eo;
    // neighbour entry by sector
    const IndexEntry* neigh = nullptr;
    for (auto& e : idx.entries) if (e.sector == g0.sector + g0.sectors()) neigh = &e;
    CHECK(neigh != nullptr);

    std::vector<uint8_t> small(g0.size() - 100), big(o0.sectors() * kSectorSize + 5000);
    for (size_t i = 0; i < small.size(); i++) small[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 13 + 5);
    std::vector<uint8_t> by(3000, 0xAB), nw(2500, 0xCD);
    spit(dir + "/OBJ/P_FIV100_1P.GMO", small);          // upper-case path: case-insensitive, shrinks in place
    spit(dir + "/obj/p_fiv100.objx", big);               // grows: relocated
    char hexname[64]; std::snprintf(hexname, sizeof hexname, "/by_crc/%08x.bin", neigh->crc);
    spit(dir + hexname, by);                             // by_crc override of the neighbour
    spit(dir + "/obj/p_newchar999_1p.gmo", nw);          // unknown crc: new entry
    spit(dir + "/README.txt", {1, 2, 3});                // ignored

    PackageMods mods; std::string log;
    CHECK(mods.build(orig, package_size, dir, &log, &err));
    std::printf("%s", log.c_str());
    CHECK(mods.overrides().size() == 4);

    PackageIndex pidx; CHECK(pidx.parse(mods.patched_index().data(), mods.patched_index().size(), &err));
    CHECK(pidx.entries.size() == idx.entries.size() + 1);
    bool psorted = true; for (size_t i = 0; i + 1 < pidx.entries.size(); i++) psorted &= pidx.entries[i].crc < pidx.entries[i + 1].crc;
    CHECK(psorted);
    const IndexEntry* pg = pidx.find(package_crc(gmo));
    const IndexEntry* po = pidx.find(package_crc(objx));
    const IndexEntry* pn = pidx.find(neigh->crc);
    const IndexEntry* pw = pidx.find(package_crc("obj/p_newchar999_1p.gmo"));
    CHECK(pg && po && pn && pw);
    CHECK(pg->sector == g0.sector && pg->size() == small.size() && pg->flags() == g0.flags());
    CHECK(po->sector >= end && po->size() == big.size() && po->flags() == o0.flags());   // relocated past real end
    CHECK(pn->sector == neigh->sector && pn->size() == by.size());
    CHECK(pw->sector >= end && pw->size() == nw.size());
    // nothing else changed
    size_t changed = 0;
    for (auto& e : idx.entries) {
        const IndexEntry* p = pidx.find(e.crc);
        if (!p || p->sector != e.sector || p->size_flags != e.size_flags) changed++;
    }
    CHECK(changed == 3);

    BaseReader base = [&](uint8_t* d, uint32_t n, int64_t o) -> int64_t {
        if (o >= (int64_t)package_size) return 0;
        uint32_t m = (uint32_t)std::min<uint64_t>(n, package_size - o);
        for (uint32_t i = 0; i < m; i++) d[i] = pat(o + i);
        return m;
    };
    auto read_entry = [&](const IndexEntry& e, uint32_t chunk) {
        std::vector<uint8_t> out; std::vector<uint8_t> buf(chunk);
        for (uint32_t pos = 0; pos < e.size(); pos += chunk) {
            uint32_t want = std::min(chunk, e.size() - pos), n = 0;
            if (!mods.read_package((int64_t)e.sector * kSectorSize + pos, want, buf.data(), &n, base)) {
                n = (uint32_t)base(buf.data(), want, (int64_t)e.sector * kSectorSize + pos);
            }
            out.insert(out.end(), buf.begin(), buf.begin() + n);
        }
        return out;
    };
    CHECK(read_entry(*pg, 4096) == small);
    CHECK(read_entry(*pg, 572) == small);          // odd game-style chunking
    CHECK(read_entry(*po, 65536) == big);          // relocated, beyond real EOF
    CHECK(read_entry(*pn, 1000) == by);
    CHECK(read_entry(*pw, 777) == nw);

    // an un-overridden entry reads through (hook declines)
    {
        const IndexEntry* other = nullptr;
        for (auto& e : idx.entries) if (e.crc != g0.crc && e.crc != o0.crc && e.crc != neigh->crc && e.sector > neigh->sector + neigh->sectors() + 5) { other = &e; break; }
        std::vector<uint8_t> b(64); uint32_t n = 0;
        CHECK(!mods.read_package((int64_t)other->sector * kSectorSize, 64, b.data(), &n, base));
    }
    // read spanning: tail of un-overridden entry + into overridden entry's start (gmo's predecessor)
    {
        int64_t start = (int64_t)g0.sector * kSectorSize - 100;
        std::vector<uint8_t> b(300); uint32_t n = 0;
        CHECK(mods.read_package(start, 300, b.data(), &n, base));
        CHECK(n == 300);
        bool ok = true;
        for (int i = 0; i < 100; i++) ok &= (b[i] == pat(start + i));
        for (int i = 0; i < 200; i++) ok &= (b[100 + i] == small[i]);
        CHECK(ok);
    }
    // sector padding after a shrunken file is zero, and neighbour begins untouched
    {
        int64_t tail = (int64_t)g0.sector * kSectorSize + small.size();
        std::vector<uint8_t> b(50); uint32_t n = 0;
        CHECK(mods.read_package(tail, 50, b.data(), &n, base) && n == 50);
        bool z = true; for (auto v : b) z &= (v == 0);
        CHECK(z);
        int64_t nb = (int64_t)neigh->sector * kSectorSize;   // neighbour is overridden by_crc: serves loose file
        (void)nb;
    }
    // index reads
    {
        std::vector<uint8_t> b(40); uint32_t n = 0;
        CHECK(mods.read_index(0, 40, b.data(), &n) && n == 40 && std::memcmp(b.data(), mods.patched_index().data(), 40) == 0);
        CHECK(mods.read_index((int64_t)mods.patched_index().size() + 10, 40, b.data(), &n) && n == 0);
    }
    // empty / missing mods dir: no overrides, index identical
    {
        PackageMods m2; std::string l2;
        CHECK(m2.build(orig, package_size, dir + "/does_not_exist", &l2, &err) && m2.empty());
        CHECK(m2.patched_index() == orig);
    }
    // index capacity: 400 NEW entries overflow the retail buffer (first `room` kept, rest dropped), all fit the extended one
    {
        std::string d2 = dir + "_many";
        (void)!std::system(("rm -rf " + d2 + " && mkdir -p " + d2).c_str());
        for (int i = 0; i < 400; i++) spit(d2 + "/obj/many" + std::to_string(i) + ".gmo", std::vector<uint8_t>(16, (uint8_t)i));
        (void)!std::system(("mkdir -p " + d2 + "/textures && for i in $(seq 1 2000); do : > " + d2 + "/textures/t$i.png; done").c_str());   // never indexed
        const size_t retail_n = idx.entries.size();
        const size_t room = (kIndexBufferBytes - 16 - retail_n * 12) / 12;
        PackageMods small_cap, big_cap; std::string l3, l4;
        CHECK(small_cap.build(orig, package_size, d2, &l3, &err));
        CHECK(small_cap.overrides().size() == room);
        CHECK(small_cap.patched_index().size() <= kIndexBufferBytes);
        CHECK(l3.find("WARNING: too many NEW entries") != std::string::npos);
        CHECK(big_cap.build(orig, package_size, d2, &l4, &err, kIndexExtendedBytes));
        CHECK(big_cap.overrides().size() == 400);
        CHECK(big_cap.patched_index().size() == 16 + (retail_n + 400) * 12);
        CHECK(l4.find("WARNING") == std::string::npos);
        PackageIndex bi; CHECK(bi.parse(big_cap.patched_index().data(), big_cap.patched_index().size(), &err));
        bool bs = true; for (size_t i = 0; i + 1 < bi.entries.size(); i++) bs &= bi.entries[i].crc < bi.entries[i + 1].crc;
        CHECK(bs);
        CHECK(kIndexExtendedBytes >= 16 + (retail_n + kIndexMaxNewEntries) * 12);
        (void)!std::system(("rm -rf " + d2).c_str());
    }
    std::string rm = "rm -rf " + dir; (void)!std::system(rm.c_str());
    if (g_fail == 0) std::printf("ALL PASSED\n"); else std::printf("%d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
