// games/dissidia/runtime/package_mods.h — loose-file overrides for PACKAGE.BIN.
//
// Pure C++17, no runtime dependencies (host-testable: tests/test_package_mods.cpp).
//
// Package format (verified against the EU disc, see README "Modding: loose files"):
//   PACKAGE_INFO.BIN = 16-byte header { u32 0x20090218, "pack", u32 count, u32 0 }
//                      + count * 12-byte entries { u32 crc, u32 sector, u32 size_flags }
//     crc        = BZip2-style CRC32 (poly 0x04C11DB7, MSB-first, init ~0, final ~)
//                  of the LOWERCASE asset path ("obj/p_fiv100_1p.gmo")
//     sector     = byte offset in PACKAGE.BIN / 2048
//     size_flags = (flags << 28) | byte size          (flags: 0..3, load hints)
//   Entries are sorted by crc ascending and tile PACKAGE.BIN contiguously.
//
// An override file replaces an entry: the index served to the game is patched
// (size, and sector when the new file needs more sectors than the old one),
// and reads of PACKAGE.BIN inside the entry's sector span are served from the
// loose file. Grown entries are relocated to virtual sectors past the real end
// of PACKAGE.BIN so they never alias a neighbour.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dissidia {

constexpr uint32_t kIndexMagic = 0x20090218u;
constexpr uint32_t kSectorSize = 2048u;
constexpr uint32_t kSizeMask = 0x0FFFFFFFu;
// The game loads PACKAGE_INFO.BIN into a fixed BSS buffer at 0x09B26BC0 (0x20F8C bytes, from the call
// arguments in FUN_08871938): the retail fallback. hooks_index.cpp moves the buffer to a host-heap block
// of kIndexExtendedBytes (retail entries + kIndexMaxNewEntries) when the patched index no longer fits it.
constexpr uint32_t kIndexBufferBytes = 0x20F8Cu;
constexpr uint32_t kRetailIndexEntries = 11043u;
constexpr uint32_t kIndexMaxNewEntries = 2000u;
constexpr uint32_t kIndexExtendedBytes =
    ((16u + (kRetailIndexEntries + kIndexMaxNewEntries) * 12u) + 0xFFu) & ~0xFFu;   // 0x26400

/// BZip2-style CRC32 of the exact bytes given (callers lowercase first).
uint32_t package_crc(const std::string& bytes);
/// Lowercase, '\\'->'/', strip leading "./" and "/" — the key the game hashes.
std::string normalize_asset_path(const std::string& rel);

struct IndexEntry {
    uint32_t crc = 0;
    uint32_t sector = 0;
    uint32_t size_flags = 0;
    uint32_t size() const { return size_flags & kSizeMask; }
    uint32_t flags() const { return size_flags >> 28; }
    uint32_t sectors() const { return (size() + kSectorSize - 1) / kSectorSize; }
};

struct PackageIndex {
    uint32_t magic = 0, tag = 0, reserved = 0;
    std::vector<IndexEntry> entries;
    bool parse(const uint8_t* data, size_t len, std::string* err);
    std::vector<uint8_t> serialize() const;
    const IndexEntry* find(uint32_t crc) const;   // binary search
};

struct Override {
    uint32_t crc = 0;
    std::string asset;       // normalized asset path, or "by_crc/XXXXXXXX" form
    std::string file;        // loose file on the host
    uint64_t start = 0;      // byte offset in (virtual) PACKAGE.BIN
    uint64_t span = 0;       // bytes reserved (whole sectors)
    uint32_t size = 0;       // file size in bytes
    bool is_new = false;     // crc not present in the original index
    bool relocated = false;  // moved past the real end of PACKAGE.BIN
};

/// reader(dest, n, offset) -> bytes read (short at EOF), used for the parts of a
/// PACKAGE.BIN read that are not overridden.
using BaseReader = std::function<int64_t(uint8_t*, uint32_t, int64_t)>;

class PackageMods {
public:
    /// Scan mods_dir and build the patched index + redirect table.
    /// package_size = size of the real PACKAGE.BIN. Returns false (and err) if the
    /// original index is malformed; returns true with zero overrides if the
    /// directory is missing or empty. index_capacity = bytes the game-side index buffer can hold; NEW
    /// entries beyond it are dropped (loudly), the first ones kept.
    bool build(const std::vector<uint8_t>& original_index, uint64_t package_size,
               const std::string& mods_dir, std::string* log, std::string* err,
               uint32_t index_capacity = kIndexBufferBytes);

    bool empty() const { return overrides_.empty(); }
    const std::vector<Override>& overrides() const { return overrides_; }
    const std::vector<uint8_t>& patched_index() const { return patched_; }

    /// Serve a PACKAGE_INFO.BIN read from the patched index (always true).
    bool read_index(int64_t off, uint32_t size, uint8_t* dest, uint32_t* nread) const;
    /// Serve a PACKAGE.BIN read. Returns false when [off, off+size) touches no
    /// override (caller does the normal host read).
    bool read_package(int64_t off, uint32_t size, uint8_t* dest, uint32_t* nread,
                      const BaseReader& base) const;

private:
    std::vector<Override> overrides_;   // sorted by start, non-overlapping
    std::vector<uint8_t> patched_;
};

/// Reads one asset by path for the game module's own use (UI theme etc.): a
/// loose file under mods_dir (<mods>/<path> or <mods>/by_crc/<crc>[.bin], the
/// same keys the IO overlay serves) wins, else the entry from
/// <data_dir>/PACKAGE.BIN through <data_dir>/PACKAGE_INFO.BIN. The index is
/// parsed once per data_dir. Returns false when the asset is in neither.
bool read_asset(const std::string& data_dir, const std::string& mods_dir, const std::string& asset,
                std::vector<uint8_t>* out);

}  // namespace dissidia
