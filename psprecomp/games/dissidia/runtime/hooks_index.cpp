// games/dissidia/runtime/hooks_index.cpp — lift the package-index capacity (the "~210 new entries" ceiling).
//
// The game reads PACKAGE_INFO.BIN (16-byte header + 12-byte {crc, sector, size|flags} entries, 11043 in retail)
// into a fixed BSS buffer: 0x20F8C bytes at 0x09B26BC0, with the next global variable at 0x09B47B50. That left
// room for only ~210 loose-file NEW entries (a scaffolded character needs ~30, so six characters). The buffer
// base is a literal (lui 0x09B2 / addiu 0x6BC0) in exactly four EBOOT functions, found by scanning every
// recompiled batch (EBOOT and all three overlays) and the data of every image for 0x09B26BC0..0x09B26BE0:
//
//   FUN_08871938(obj)        loader: vtable call (open + read) with dst = 0x09B26BC0, size = 0x20F8C
//   FUN_08871674()           entry count  = *(u32*)(base + 8)
//   FUN_08871684(idx)        &entry[idx]  = base + 16 + 12 * idx, or 0 when idx is negative / >= count
//   FUN_088713E4(obj, path)  find by name: crc (FUN_08871288), binary search over [0, count] (note: hi starts at
//                            count, so it may read entry[count]), then caches the name at obj+8 and the entry
//                            pointer at obj+88 (FUN_08935B50 compares the name, FUN_089359E8 / FUN_08935BB4 copy it)
//
// Everything else reaches entries through the pointer FUN_088713E4 / FUN_08871684 return, so those four functions
// are the whole interface. When the patched index (hooks_io.cpp / package_mods.cpp) no longer fits the retail buffer,
// the loader hook allocates kIndexExtendedBytes (retail entries + kIndexMaxNewEntries, 0x26400) from the host heap
// (psp_kmem_bump_alloc_named), zero-fills it, loads the index there, and the other three hooks use that base. While the
// patched index fits (retail, empty mods, a few overrides) every hook calls the original function: nothing changes.
// PSPRECOMP_INDEX_RELOCATE=1 relocates even when the index fits (regression aid: retail through the hooks). The count is a u32 in the file header, so it needs no widening. PSPRECOMP_INDEX_EXTENDED=0 disables the hooks
// (back to the retail ceiling).
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "package_mods.h"
#include "recomp.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

uint32_t dissidia_package_index_bytes(const std::string& psp_info_path);
void dissidia_package_index_extend(bool on);

namespace {

constexpr uint32_t kRetailBase = 0x09B26BC0u;
constexpr uint32_t kPathPtrGlobal = 0x089D5FECu;   // word: pointer to the index file name; the loader passes it to the open call
constexpr uint32_t kCbGlobal = 0x089D5FE8u;        // word: argument of the second (wait / finish) vtable call

constexpr uint32_t kLoader = 0x08871938u, kCount = 0x08871674u, kEntryAt = 0x08871684u, kFind = 0x088713E4u;
constexpr uint32_t kHashName = 0x08871288u, kNameCmp = 0x08935B50u, kStrLen = 0x089359E8u, kStrCpy = 0x08935BB4u;

FuncPtr g_orig_load = nullptr, g_orig_count = nullptr, g_orig_entry = nullptr, g_orig_find = nullptr;
uint32_t g_base = kRetailBase;
bool g_relocated = false;
bool g_decided = false;

inline uint32_t rd32(uint8_t* rdram, uint32_t a) { return psp_mem_read<uint32_t>(rdram, a); }
inline int16_t rd16s(uint8_t* rdram, uint32_t a) { return static_cast<int16_t>(psp_mem_read<uint16_t>(rdram, a)); }
inline void call(uint8_t* rdram, recomp_context* ctx, uint32_t addr) { RECOMP_LOOKUP(addr)(rdram, ctx); }

std::string guest_cstr(uint8_t* rdram, uint32_t a) {
    std::string s;
    if (a < 0x08000000u || a >= 0x0C000000u) return s;
    for (int i = 0; i < 256; i++) {
        char c = static_cast<char>(rdram[(a + i) & 0x07FFFFFFu]);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

// Decide once, before the first load, where the index goes.
void decide(uint8_t* rdram) {
    if (g_decided) return;
    g_decided = true;
    // The word points at the bare name ("package_info.bin"); the game's open wrapper prefixes its data root, which
    // is the same fixed string classify_open sees (hooks_io.cpp): DISC0:/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN.
    std::string path = guest_cstr(rdram, rd32(rdram, kPathPtrGlobal));
    for (char& c : path) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!path.empty()) path = "DISC0:/PSP_GAME/USRDIR/DATA/" + path;
    std::fprintf(stderr, "[MODS] package index: loader file name \"%s\" (word at 0x%08X = 0x%08X)\n", path.c_str(), kPathPtrGlobal, rd32(rdram, kPathPtrGlobal));
    const uint32_t need = path.empty() ? 0 : dissidia_package_index_bytes(path);
    static const bool force = std::getenv("PSPRECOMP_INDEX_RELOCATE") != nullptr;   // test aid: relocate even when it fits
    if (need <= dissidia::kIndexBufferBytes && !(force && !path.empty())) {
        if (need) std::fprintf(stderr, "[MODS] package index: %u B fits the retail buffer (%u B at 0x%08X): not relocated\n",
                               need, dissidia::kIndexBufferBytes, kRetailBase);
        else if (path.empty()) std::fprintf(stderr, "[MODS] package index: file name not found at 0x%08X: not relocated\n", kPathPtrGlobal);
        return;
    }
    const uint32_t block = psp_kmem_bump_alloc_named(dissidia::kIndexExtendedBytes, "PACKAGE_INDEX");
    if (!block) {
        std::fprintf(stderr, "[MODS] package index: host heap exhausted (%u B wanted): NOT relocated, the patched index will overrun the retail buffer\n",
                     dissidia::kIndexExtendedBytes);
        return;
    }
    std::memset(rdram + (block & 0x07FFFFFFu), 0, dissidia::kIndexExtendedBytes);
    g_base = block;
    g_relocated = true;
    std::fprintf(stderr, "[MODS] package index: patched index %u B vs retail buffer %u B: relocated to host heap 0x%08X (%u B = %u entries)%s\n",
                 need, dissidia::kIndexBufferBytes, block, dissidia::kIndexExtendedBytes, (dissidia::kIndexExtendedBytes - 16) / 12, force ? " [forced]" : "");
}

void load_hook(uint8_t* rdram, recomp_context* ctx) {
    decide(rdram);
    if (!g_relocated) { g_orig_load(rdram, ctx); return; }
    // Same body as FUN_08871938 with dst / size replaced.
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    uint32_t o4 = rd32(rdram, obj + 4);
    uint32_t t = rd32(rdram, o4 + 340) + 40;
    uint32_t fn = rd32(rdram, t + 4);
    ctx->r[4] = static_cast<int32_t>(o4 + static_cast<int32_t>(rd16s(rdram, t)));
    ctx->r[5] = static_cast<int32_t>(rd32(rdram, kPathPtrGlobal));
    ctx->r[6] = static_cast<int32_t>(g_base);
    ctx->r[7] = static_cast<int32_t>(dissidia::kIndexExtendedBytes);
    call(rdram, ctx, fn);
    o4 = rd32(rdram, obj + 4);
    t = rd32(rdram, o4 + 340) + 184;
    uint32_t cb = rd32(rdram, kCbGlobal);
    fn = rd32(rdram, t + 4);
    ctx->r[4] = static_cast<int32_t>(o4 + static_cast<int32_t>(rd16s(rdram, t)));
    ctx->r[5] = static_cast<int32_t>(cb);
    ctx->r[6] = static_cast<int32_t>(cb);
    call(rdram, ctx, fn);
    psp_mem_write<uint32_t>(rdram, obj, static_cast<uint32_t>(ctx->r[2]));
}

void count_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!g_relocated) { g_orig_count(rdram, ctx); return; }
    ctx->r[2] = static_cast<int32_t>(rd32(rdram, g_base + 8));
}

void entry_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!g_relocated) { g_orig_entry(rdram, ctx); return; }
    const int32_t idx = static_cast<int32_t>(ctx->r[5]);
    const uint32_t cnt = rd32(rdram, g_base + 8);
    if (idx < 0 || !(cnt - 1u >= static_cast<uint32_t>(idx))) { ctx->r[2] = 0; return; }
    ctx->r[2] = static_cast<int32_t>(g_base + 16u + 12u * static_cast<uint32_t>(idx));
}

void find_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!g_relocated) { g_orig_find(rdram, ctx); return; }
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]), path = static_cast<uint32_t>(ctx->r[5]);
    if (rd32(rdram, obj + 88) != 0) {   // cached name still equal: same entry
        ctx->r[4] = static_cast<int32_t>(path); ctx->r[5] = static_cast<int32_t>(obj + 8); ctx->r[6] = 0x50;
        call(rdram, ctx, kNameCmp);
        if (ctx->r[2] == 0) { ctx->r[2] = static_cast<int32_t>(rd32(rdram, obj + 88)); return; }
    }
    ctx->r[4] = static_cast<int32_t>(path);
    call(rdram, ctx, kHashName);
    const uint32_t crc = static_cast<uint32_t>(ctx->r[2]);
    const uint32_t cnt = rd32(rdram, g_base + 8);
    int32_t lo = 0, hi = static_cast<int32_t>(cnt);   // as the original: hi starts at count
    uint32_t entry = 0, cur = 0;
    do {
        const int32_t sum = hi + lo;
        const int32_t mid = (sum + static_cast<int32_t>(static_cast<uint32_t>(sum) >> 31)) >> 1;
        entry = g_base + 16u + 12u * static_cast<uint32_t>(mid);
        cur = rd32(rdram, entry);
        if (cur == crc) break;
        if (cur < crc) lo = mid + 1; else hi = mid - 1;
    } while (hi - lo >= 0);
    if (cur != crc) { ctx->r[2] = 0; return; }
    ctx->r[4] = static_cast<int32_t>(path);
    call(rdram, ctx, kStrLen);
    ctx->r[4] = static_cast<int32_t>(obj + 8); ctx->r[5] = static_cast<int32_t>(path); ctx->r[6] = 0x50;
    call(rdram, ctx, kStrCpy);
    psp_mem_write<uint32_t>(rdram, obj + 88, entry);
    ctx->r[2] = static_cast<int32_t>(entry);
}

}  // namespace

void dissidia_index_install() {
    const char* off = std::getenv("PSPRECOMP_INDEX_EXTENDED");
    if (off && off[0] == '0') { std::fprintf(stderr, "[MODS] package index: extension disabled (PSPRECOMP_INDEX_EXTENDED=0), retail ceiling\n"); return; }
    g_orig_load = psp_dispatch_probe_lookup(kLoader);
    g_orig_count = psp_dispatch_probe_lookup(kCount);
    g_orig_entry = psp_dispatch_probe_lookup(kEntryAt);
    g_orig_find = psp_dispatch_probe_lookup(kFind);
    if (!g_orig_load || !g_orig_count || !g_orig_entry || !g_orig_find) {
        std::fprintf(stderr, "[MODS] package index hooks NOT installed (a target function is missing): retail ceiling\n");
        return;
    }
    psp_dispatch_register(kLoader, load_hook);
    psp_dispatch_register(kCount, count_hook);
    psp_dispatch_register(kEntryAt, entry_hook);
    psp_dispatch_register(kFind, find_hook);
    dissidia_package_index_extend(true);
    std::fprintf(stderr, "[MODS] package index capacity: retail buffer %u B (0x%08X), extended %u B = %u entries (%u retail + %u new) from the host heap when needed\n",
                 dissidia::kIndexBufferBytes, kRetailBase, dissidia::kIndexExtendedBytes,
                 (dissidia::kIndexExtendedBytes - 16) / 12, dissidia::kRetailIndexEntries,
                 (dissidia::kIndexExtendedBytes - 16) / 12 - dissidia::kRetailIndexEntries);
}
