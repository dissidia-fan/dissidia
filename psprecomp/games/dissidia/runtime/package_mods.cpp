// games/dissidia/runtime/package_mods.cpp — see package_mods.h.
#include "package_mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dissidia {

namespace {

const uint32_t* crc_table() {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            t[i] = c;
        }
        init = true;
    }
    return t;
}

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

void walk(const std::string& root, const std::string& rel, std::vector<std::string>* out) {
    std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == ".." || n[0] == '.') continue;
        names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        if (::stat((root + "/" + r).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(root, r, out);
        else if (S_ISREG(st.st_mode)) out->push_back(r);
    }
}

bool parse_hex8(const std::string& s, uint32_t* out) {
    if (s.size() != 8) return false;
    uint32_t v = 0;
    for (char ch : s) {
        int d = std::isdigit((unsigned char)ch) ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

int64_t pread_all(const std::string& file, uint8_t* dest, uint32_t n, int64_t off) {
    int fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    int64_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, dest + got, n - got, off + got);
        if (r <= 0) break;
        got += r;
    }
    ::close(fd);
    return got;
}

}  // namespace

uint32_t package_crc(const std::string& bytes) {
    const uint32_t* T = crc_table();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : bytes) c = (c << 8) ^ T[(c >> 24) ^ b];
    return ~c;
}

std::string normalize_asset_path(const std::string& rel) {
    std::string p;
    for (char ch : rel) p.push_back(ch == '\\' ? '/' : (char)std::tolower((unsigned char)ch));
    size_t i = 0;
    while (i < p.size() && (p[i] == '/' || (p[i] == '.' && i + 1 < p.size() && p[i + 1] == '/'))) i += (p[i] == '/') ? 1 : 2;
    return p.substr(i);
}

bool PackageIndex::parse(const uint8_t* d, size_t len, std::string* err) {
    if (len < 16) { if (err) *err = "index shorter than header"; return false; }
    magic = rd32(d); tag = rd32(d + 4); uint32_t n = rd32(d + 8); reserved = rd32(d + 12);
    if (magic != kIndexMagic) { if (err) *err = "bad index magic"; return false; }
    if (16 + (uint64_t)n * 12 > len) { if (err) *err = "index truncated"; return false; }
    entries.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        entries[i].crc = rd32(d + 16 + 12 * i);
        entries[i].sector = rd32(d + 20 + 12 * i);
        entries[i].size_flags = rd32(d + 24 + 12 * i);
    }
    return true;
}

std::vector<uint8_t> PackageIndex::serialize() const {
    std::vector<uint8_t> o(16 + entries.size() * 12);
    wr32(&o[0], magic); wr32(&o[4], tag); wr32(&o[8], (uint32_t)entries.size()); wr32(&o[12], reserved);
    for (size_t i = 0; i < entries.size(); i++) {
        wr32(&o[16 + 12 * i], entries[i].crc);
        wr32(&o[20 + 12 * i], entries[i].sector);
        wr32(&o[24 + 12 * i], entries[i].size_flags);
    }
    return o;
}

const IndexEntry* PackageIndex::find(uint32_t crc) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), crc,
                               [](const IndexEntry& e, uint32_t c) { return e.crc < c; });
    return (it != entries.end() && it->crc == crc) ? &*it : nullptr;
}

bool PackageMods::build(const std::vector<uint8_t>& original_index, uint64_t package_size,
                        const std::string& mods_dir, std::string* log, std::string* err,
                        uint32_t index_capacity) {
    overrides_.clear();
    patched_ = original_index;
    PackageIndex idx;
    if (!idx.parse(original_index.data(), original_index.size(), err)) return false;
    auto note = [&](const std::string& s) { if (log) { *log += s; *log += '\n'; } };

    std::vector<std::string> files;
    walk(mods_dir, "", &files);
    if (files.empty()) return true;

    uint64_t next_virtual = (package_size + kSectorSize - 1) / kSectorSize;   // sectors
    uint32_t orig_count = (uint32_t)idx.entries.size();
    uint32_t added = 0;
    std::vector<IndexEntry> new_entries;

    for (const std::string& rel : files) {
        struct stat st;
        if (::stat((mods_dir + "/" + rel).c_str(), &st) != 0) continue;
        std::string norm = normalize_asset_path(rel);
        uint32_t crc = 0;
        std::string asset = norm;
        // by_crc/<8 hex digits>[.anything]
        if (norm.compare(0, 7, "by_crc/") == 0 && norm.find('/', 7) == std::string::npos) {
            std::string stem = norm.substr(7);
            stem = stem.substr(0, stem.find('.'));
            if (!parse_hex8(stem, &crc)) { note("[MODS] skip (bad by_crc name): " + rel); continue; }
        } else {
            if (norm.find('/') == std::string::npos &&
                (norm.compare(0, 6, "readme") == 0 || norm == "roster.txt")) continue;   // roster.txt: roster.cpp config
            // textures/… is the texture-pack tree (psp_tex_pack.cpp serves it by xxhash id); the game never
            // requests those paths, and a dump of 1,000+ PNGs would otherwise flood the package index below.
            if (norm.compare(0, 9, "textures/") == 0) continue;
            crc = package_crc(norm);
        }
        if (st.st_size <= 0 || (uint64_t)st.st_size > kSizeMask) { note("[MODS] skip (size out of range): " + rel); continue; }
        bool dup = false;
        for (auto& o : overrides_) if (o.crc == crc) { note("[MODS] skip (duplicate of " + o.file + "): " + rel); dup = true; }
        if (dup) continue;

        Override ov;
        ov.crc = crc; ov.asset = asset; ov.file = mods_dir + "/" + rel; ov.size = (uint32_t)st.st_size;
        uint64_t need = (ov.size + kSectorSize - 1) / kSectorSize;
        const IndexEntry* e = idx.find(crc);
        if (e) {
            uint32_t flags = e->flags();
            const uint32_t old_size = e->size();
            IndexEntry ne = *e;
            if (need <= e->sectors()) {
                ov.start = (uint64_t)e->sector * kSectorSize;
                ov.span = (uint64_t)e->sectors() * kSectorSize;
                // sectors() may be 0 for an empty original; keep at least `need`
                if (ov.span < need * kSectorSize) ov.span = need * kSectorSize;
            } else {
                ov.relocated = true;
                ne.sector = (uint32_t)next_virtual;
                ov.start = next_virtual * kSectorSize;
                ov.span = need * kSectorSize;
                next_virtual += need;
            }
            ne.size_flags = (flags << 28) | ov.size;
            // patch in place in the original-order table
            for (auto& x : idx.entries) if (x.crc == crc) x = ne;
            note("[MODS] override " + asset + " (" + std::to_string(ov.size) + " B, was " +
                 std::to_string(old_size) + " B" + (ov.relocated ? ", relocated" : "") + ")");
        } else {
            ov.is_new = true; ov.relocated = true;
            IndexEntry ne;
            ne.crc = crc; ne.sector = (uint32_t)next_virtual; ne.size_flags = (1u << 28) | ov.size;   // flags=1: see README
            ov.start = next_virtual * kSectorSize; ov.span = need * kSectorSize;
            next_virtual += need;
            new_entries.push_back(ne);
            added++;
            note("[MODS] NEW entry " + asset + " crc=" + [&]{ char b[16]; std::snprintf(b, sizeof b, "%08x", crc); return std::string(b); }() +
                 " (" + std::to_string(ov.size) + " B) — only reachable if the game requests this path");
        }
        overrides_.push_back(ov);
    }

    if (!new_entries.empty()) {
        uint64_t total = 16 + (uint64_t)(orig_count + new_entries.size()) * 12;
        size_t room = index_capacity > 16 + (uint64_t)orig_count * 12
            ? (size_t)((index_capacity - 16 - (uint64_t)orig_count * 12) / 12) : 0;
        if (total > index_capacity) {
            // Keep the first `room` new entries (scan order) and drop only the excess, loudly: dropping
            // everything made a whole roster vanish because of unrelated files in the mods tree.
            note("[MODS] WARNING: too many NEW entries for the game's index buffer (" + std::to_string(total) +
                 " > " + std::to_string(index_capacity) + " B): keeping the first " + std::to_string(room) +
                 " of " + std::to_string(new_entries.size()) + " and dropping the rest");
            std::vector<uint32_t> kept_crc;
            for (size_t i = 0; i < new_entries.size() && i < room; i++) kept_crc.push_back(new_entries[i].crc);
            std::vector<Override> keep;
            for (auto& o : overrides_) {
                if (!o.is_new) { keep.push_back(o); continue; }
                bool k = false; for (uint32_t c : kept_crc) k |= c == o.crc;
                if (k) keep.push_back(o); else note("[MODS]   dropped NEW entry " + o.file);
            }
            overrides_ = keep;
            new_entries.resize(std::min(new_entries.size(), room));
        }
        for (auto& n : new_entries) idx.entries.push_back(n);
        std::sort(idx.entries.begin(), idx.entries.end(),
                  [](const IndexEntry& a, const IndexEntry& b) { return a.crc < b.crc; });
    }
    std::sort(overrides_.begin(), overrides_.end(),
              [](const Override& a, const Override& b) { return a.start < b.start; });
    patched_ = idx.serialize();
    return true;
}

bool PackageMods::read_index(int64_t off, uint32_t size, uint8_t* dest, uint32_t* nread) const {
    if (off < 0 || (uint64_t)off >= patched_.size()) { *nread = 0; return true; }
    uint32_t n = (uint32_t)std::min<uint64_t>(size, patched_.size() - (uint64_t)off);
    std::memcpy(dest, patched_.data() + off, n);
    *nread = n;
    return true;
}

bool PackageMods::read_package(int64_t off, uint32_t size, uint8_t* dest, uint32_t* nread,
                               const BaseReader& base) const {
    if (overrides_.empty() || off < 0 || size == 0) return false;
    const uint64_t lo = (uint64_t)off, hi = lo + size;
    // first override whose end is > lo
    auto it = std::lower_bound(overrides_.begin(), overrides_.end(), lo,
                               [](const Override& o, uint64_t v) { return o.start + o.span <= v; });
    if (it == overrides_.end() || it->start >= hi) return false;

    uint64_t pos = lo;
    uint32_t done = 0;
    while (pos < hi) {
        while (it != overrides_.end() && it->start + it->span <= pos) ++it;
        uint64_t seg_end;
        bool in_ov = (it != overrides_.end() && it->start <= pos);
        if (in_ov) seg_end = std::min<uint64_t>(hi, it->start + it->span);
        else seg_end = (it != overrides_.end()) ? std::min<uint64_t>(hi, it->start) : hi;
        uint32_t n = (uint32_t)(seg_end - pos);
        if (in_ov) {
            uint64_t rel = pos - it->start;
            uint32_t from_file = 0;
            if (rel < it->size) {
                uint32_t want = (uint32_t)std::min<uint64_t>(n, it->size - rel);
                int64_t got = pread_all(it->file, dest + done, want, (int64_t)rel);
                from_file = got > 0 ? (uint32_t)got : 0;
                if (from_file < want) std::memset(dest + done + from_file, 0, want - from_file);
                from_file = want;
            }
            if (from_file < n) std::memset(dest + done + from_file, 0, n - from_file);   // sector padding
        } else {
            int64_t got = base ? base(dest + done, n, (int64_t)pos) : 0;
            if (got < 0) got = 0;
            if ((uint32_t)got < n) { done += (uint32_t)got; break; }   // EOF: short read
        }
        done += n;
        pos += n;
    }
    *nread = done;
    return true;
}

namespace {

bool read_whole_file(const std::string& path, std::vector<uint8_t>* out) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return false;
    out->resize((size_t)st.st_size);
    int64_t got = pread_all(path, out->data(), (uint32_t)st.st_size, 0);
    if (got != st.st_size) { out->clear(); return false; }
    return true;
}

bool first_existing(const std::string& dir, const char* a, const char* b, std::string* found) {
    struct stat st;
    for (const char* n : {a, b}) {
        std::string p = dir + "/" + n;
        if (::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { *found = p; return true; }
    }
    return false;
}

}  // namespace

bool read_asset(const std::string& data_dir, const std::string& mods_dir, const std::string& asset,
                std::vector<uint8_t>* out) {
    const std::string norm = normalize_asset_path(asset);
    const uint32_t crc = package_crc(norm);
    if (!mods_dir.empty()) {
        char hex[32];
        std::snprintf(hex, sizeof hex, "by_crc/%08x", crc);
        std::string by_crc = std::string(hex) + ".bin";
        if (read_whole_file(mods_dir + "/" + norm, out)) return true;
        if (read_whole_file(mods_dir + "/" + hex, out)) return true;
        if (read_whole_file(mods_dir + "/" + by_crc, out)) return true;
    }
    static std::string cached_dir;
    static PackageIndex idx;
    static std::string pkg_file;
    if (cached_dir != data_dir) {
        cached_dir.clear();
        idx = PackageIndex();
        std::string info;
        if (!first_existing(data_dir, "PACKAGE_INFO.BIN", "package_info.bin", &info)) return false;
        if (!first_existing(data_dir, "PACKAGE.BIN", "package.bin", &pkg_file)) return false;
        std::vector<uint8_t> raw;
        std::string err;
        if (!read_whole_file(info, &raw) || !idx.parse(raw.data(), raw.size(), &err)) return false;
        cached_dir = data_dir;
    }
    const IndexEntry* e = idx.find(crc);
    if (!e || e->size() == 0) return false;
    out->resize(e->size());
    int64_t got = pread_all(pkg_file, out->data(), e->size(), (int64_t)e->sector * kSectorSize);
    if (got != (int64_t)e->size()) { out->clear(); return false; }
    return true;
}

}  // namespace dissidia
