// games/dissidia/runtime/roster.cpp — extra roster slots (charId 34) by data patching.
//
// See games/dissidia/docs/roster.md for the full map of what consumes the roster. Short version:
//
//  * The game was built with a 35-slot charId space (0 = none, 1..33 = characters, 34 = spare;
//    34 is half-used by the unfinished "org800"). Every code string table is a 35-slot array of
//    `const char*` indexed by charId, and its slot 34 is already inside the array (null today).
//  * The binaries are statically recompiled, so code immediates cannot be patched at runtime —
//    only data can. Overlay data is patched in flight while the game streams OVL_*_APP.ELF
//    into the 0x089DD4C0 window (hooks_io.cpp routes the reads here); EBOOT data is patched in
//    on_boot_context, after the data sections are initialised.
//  * Going past charId 34 needs the recompiled code changed (bounds are `slti 0x23` etc.), which
//    this module cannot do; it refuses such entries.
//
// Config (first match wins): env PSPRECOMP_ROSTER="34=noc100" (';' separated), else
// <mods>/roster.txt with lines "34 noc100" ('#' comments). The code string is what the game
// splices into asset paths (obj/p_<code>_1p.gmo, menu/.../p_<code>_1p.gim, voice_p_<code>...).
// Using an existing code ("fiv100") makes the slot an alias of that character's files.

#include "roster_max.h"
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_io.h"
#include "recomp.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <array>
#include <vector>

extern std::string dissidia_mods_dir();   // hooks_io.cpp
extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_battle_lookup(uint32_t vaddr);   // generated menu overlay function set
extern thread_local uint32_t g_func_ring[32];      // psp_dispatch.cpp: last 32 function entries of this thread
extern thread_local uint32_t g_func_ring_pos;
// hitcam= (camera.cpp reads these): the attacker of a hit-confirmed follow-up, its framing length in ticks, and a
// sequence number that changes with every request
uint32_t g_dissidia_cine_actor = 0u, g_dissidia_cine_seq = 0u;
int g_dissidia_cine_ticks = 0;
void psp_dump_func_ring(const char*);

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t OVL_FILE_HDR = 0x54u;          // ELF header + 1 phdr; body maps to OVL_BASE
constexpr uint32_t SCRATCH = DISSIDIA_SCRATCH;    // free guest RAM (below the user base 0x08800000)
constexpr int kRetailSlots = DISSIDIA_RETAIL_SLOTS;   // charId space the game was built with: 0..34
constexpr int kRosterMax = DISSIDIA_ROSTER_MAX;       // ROSTER_MAX: charId space with the mod: 0..kRosterMax-1
constexpr int kFirstNew = DISSIDIA_FIRST_NEW;         // first charId that is not a plain retail character (33)
constexpr int kGridRetailRows = 33;                   // rows of the game's own Quick Battle record table
constexpr int kGridRows = kGridRetailRows + (kRosterMax - kFirstNew);   // + one row per extra character (43)
constexpr int kListCap = 35;                          // capacity of the selectable list built by FUN_088D5AC8

enum Module { MOD_EBOOT = 0, MOD_BATTLE = 1, MOD_MENU = 2, MOD_EXTRA = 3 };

// 35-slot `const char*[]` indexed by charId (slot 0 is the null entry before "one100").
// `reloc` != 0: the overlay code was patched (tools/roster_patches.py) to read a kRosterMax-slot copy at
// that scratch address instead; the retail entries are copied there while the overlay streams in.
// EBOOT tables cannot be relocated (the EBOOT is recompiled in one piece): see docs/roster.md.
struct PtrTable { Module mod; uint32_t slot0; const char* what; uint32_t reloc; };
constexpr PtrTable k_tables[] = {
    { MOD_EBOOT,  0x089D6CA8u, "voice bank loader (FUN_088A3A94), 35 x 12-byte records built per slot", 0 },
    { MOD_BATTLE, 0x08AFBC68u, "story/encounter scene face-in names (FUN_08AF6B70)", DISSIDIA_TBL_BATTLE },
    { MOD_MENU,   0x089EA3C0u, "arcade last-chara gim (FUN_089E9EB4)", DISSIDIA_TBL_MENU_A },
    { MOD_MENU,   0x089EC568u, "arcade character select p_%s_1p.gim (FUN_089EA4D0)", DISSIDIA_TBL_MENU_B },
    { MOD_MENU,   0x08B57FB8u, "VS screen / battle prep face-in names (FUN_08B558F0)", DISSIDIA_TBL_VS },
};
// u32[charId] -> name text record (valid idx 1..34), read by the info/loading panels; relocated like the above
constexpr PtrTable k_name_table = { MOD_MENU, 0x08A544E0u, "charId -> name text record", DISSIDIA_TBL_NAME };

// tile/art/name/logo: Quick Battle grid record fields of this character (-1 = inherit `like`'s):
//   tile  f0 for the grid tile  (texture g2 entry 12+tile of menu_pk_free_battle_gim_2.bin)
//   art   f0 for the slide-in   (texture entry 12+art over the concatenated gim packs)
//   name  f1, name plate sprite: entity id 0x704F + name (lg0 entries 79..110 are the 128x32 plates)
//   text  record number of the name in text/<lang>/mess_pk_chara_select_0.bin entry 0 (tools/mess_text.py add);
//         written into the charId-indexed u32 table at 0x08A544E0 (valid idx 1..34) read by the info/loading panels
//   logo  f2, game logo sprite index
struct WarpSpec { std::vector<int> motions; float travel = 0.0f, stop = 0.0f, lift = 0.0f; bool flat = false; };   // one warp= token (LIFT -1 = flat)
struct VTrack { std::vector<int> motions; float from = 0.0f, to = 0.0f, max = 0.0f, lift = 0.0f; };   // one vtrack= token (vtrack_step)
struct ApocSet { int arm0 = -1, narm = 0, nids = 0; uint16_t ids[8] = {}; };    // one apocarms= token (apoc_bone_post)
struct PinSpin { int bone = -1; float deg = 0.0f, ax[3] = {0.0f, 0.0f, 1.0f}; };   // pinspin= (apoc_bone_post)
struct ProjBlock { std::vector<int> motions; float from = 0.0f, to = 0.0f; int nfx = 0; int fx_bone[8] = {}; int fx_id[8] = {}; };  // projblock= (can_hit_hook)
struct Entry { int charId; std::string code; uint32_t str_addr; int like; int tile = -1, art = -1, name = -1, logo = -1, text = -1, artl = -1, artr = -1; std::string label;
               float dash = 1.0f;      // dash: R+Triangle dash speed factor (roster.txt dash=F, see dash_move_hook)
               int warp_motion = -1; float warp_travel = 0.0f, warp_stop = 0.0f, warp_lift = 0.0f; // warp=: homing root motion (warp_step_hook); first token
               std::vector<WarpSpec> warps;                      // every warp= token of the line (warps[0] = the fields above; warpfx= acts on its motions)
               std::vector<std::pair<int, float>> warp_reach;    // warpreach=: a warp= motion homes up to this far (default its TRAVEL)
               std::vector<VTrack> vtracks;                      // vtrack=: an attack eases to the target's height (vtrack_step)
               int warp_sword = -1, warp_wisp = -1; float warp_fly = 0.0f, warp_aim = 0.0f, warp_tip = 0.0f;  // warpfx=: sword / wisps (warp_bone_post)
               int exranged = 0;                                 // EX Mode ranged immunity (roster.txt exranged=N)
               int land_act = -1, land_mot[4] = {-1, -1, -1, -1};   // landact=: touchdown action (land_hook)
               int chain_act[4] = {-1, -1, -1, -1};             // chainact=: a chain window (op 0x0467) starts its action (chain_add_hook)
               int hit_magi = -1, hit_act = -1;                 // hitact=: spawning magi HIT_MAGI (an on-hit child) starts HIT_ACT (hit_spawn_hook)
               int hit_cam = 0;                                 // hitcam=TICKS: the camera frames the hitact= action (camera.cpp)
               int wd_ground = -1, wd_air = -1, wd_link = 24; float wd_range = 16.0f, wd_near = 1.0f; bool wd_hp = true;  // warpdash= (dash_start_hook)
               std::vector<int> wd_open;                         // wdopen=: commands whose link starts press 1, not press 2 (warpdash_link)
               int apoc_arm0 = -1, apoc_narm = 0, apoc_nids = 0; uint16_t apoc_ids[8] = {};   // apocarms=: Apocalypsis arms (apoc_bone_post)
               ApocSet apoc_more[3]; int apoc_nmore = 0;     // further apocarms= tokens: more arm sets (apoc_bone_post)
               PinSpin pin_spin[4]; int pin_nspin = 0;       // pinspin=BONE,DEG,AX,AY,AZ: a riding arm that spins
               int at_id = -1, at_slot0 = 4, at_nslot = 0, at_nset = 0;   // armtrail= (weapon trails on floating arms): trail id, slots, arm sets
               int at_bone0[4] = {}, at_count[4] = {}, at_bit0[4] = {}, at_draw[4] = {-1, -1, -1, -1};
               std::vector<ProjBlock> projblocks;               // projblock=: projectiles blocked between two ticks of a motion (can_hit_hook)
               int armiger[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1}; };  // EX Mode parts (armiger=O,F,P,RD,FD[,BD], exarms=ARM0,NARM[,FEPA0])
std::vector<Entry> g_entries;
constexpr uint32_t OPPINFO_MARK = 0x7F00u;   // text id marker for extras: OPPINFO_MARK + charId
bool g_enabled = false;
uint8_t* g_rdram = nullptr;

void parse_spec(const std::string& line) {
    int id = 0, like = 9;   // `like`: charId whose grid record / list entry is cloned (default Bartz)
    char code[16] = {0};
    int consumed = 0;
    int n = std::sscanf(line.c_str(), " %d %15[A-Za-z0-9_]%n", &id, code, &consumed);
    if (n < 2) { consumed = 0; n = std::sscanf(line.c_str(), " %d = %15[A-Za-z0-9_]%n", &id, code, &consumed); }
    if (n < 2) return;
    if (id < 1) return;
    if (id >= kRosterMax) {
        std::fprintf(stderr, "[ROSTER] charId %d >= ROSTER_MAX %d, skipped\n", id, kRosterMax);
        return;
    }
    if (id < kFirstNew)
        std::fprintf(stderr, "[ROSTER] note: charId %d is a retail slot; overriding its code string\n", id);
    if (id >= kRetailSlots && std::getenv("PSPRECOMP_ROSTER_RETAIL_ONLY")) {
        // Opt-out of the ids above the retail charId space (docs/roster.md, "Ids > 34"): skip them.
        std::fprintf(stderr, "[ROSTER] charId %d >= %d skipped (PSPRECOMP_ROSTER_RETAIL_ONLY)\n", id, kRetailSlots);
        return;
    }
    Entry e;
    e.charId = id;
    e.code = code;
    // remaining tokens: [likeCharId] [tile=N] [art=N] [name=N] [logo=N]
    std::string rest = line.substr(static_cast<size_t>(consumed));
    size_t p = 0;
    while (p < rest.size()) {
        while (p < rest.size() && (rest[p] == ' ' || rest[p] == '\t')) p++;
        size_t q = p;
        while (q < rest.size() && rest[q] != ' ' && rest[q] != '\t') q++;
        std::string tok = rest.substr(p, q - p);
        p = q;
        if (tok.empty()) continue;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) { like = std::atoi(tok.c_str()); continue; }
        std::string k = tok.substr(0, eq);
        if (k == "nm") {   // display name for text that has no record in the game's text banks (Opponent Info); '_' = space
            e.label = tok.substr(eq + 1);
            for (char& c : e.label) if (c == '_') c = ' ';
            continue;
        }
        if (k == "warp") {   // homing root motion, e.g. warp=87,12.5,1.8,0.5 or warp=87/88,12.5,1.8,0.5; repeatable (warp_step_hook)
            WarpSpec w;
            const char* v = tok.c_str() + eq + 1;
            bool okm = true;
            for (;;) {                                         // MOTION[/MOTION...] before the first comma
                char* end = nullptr;
                const long m = std::strtol(v, &end, 10);
                if (end == v || m < 0 || m >= 1000 || w.motions.size() >= 32) { okm = false; break; }
                w.motions.push_back(static_cast<int>(m));
                v = end;
                if (*v != '/') break;
                v++;
            }
            float t = 0.0f, st = 0.0f, l = 0.0f;
            const int got = okm && *v == ',' ? std::sscanf(v + 1, "%f,%f,%f", &t, &st, &l) : 0;
            if (got >= 2 && t > 0.0f && t <= 100.0f && st >= 0.0f && st <= 20.0f && ((l >= 0.0f && l <= 10.0f) || l == -1.0f)) {
                w.flat = got >= 3 && l == -1.0f;              // LIFT -1: flat (horizontal homing, the step's own height kept)
                w.travel = t; w.stop = st; w.lift = got >= 3 && !w.flat ? l : 0.0f;
                if (e.warps.empty()) { e.warp_motion = w.motions[0]; e.warp_travel = t; e.warp_stop = st; e.warp_lift = w.lift; }
                e.warps.push_back(w);
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad warp=%s (want MOTION[/MOTION...],TRAVEL,STOP[,LIFT]), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "vtrack") {   // vertical tracking, e.g. vtrack=60,0,12,1.8,0.3; repeatable (vtrack_step)
            VTrack v;
            const char* s = tok.c_str() + eq + 1;
            bool okm = true;
            for (;;) {
                char* end = nullptr;
                const long m = std::strtol(s, &end, 10);
                if (end == s || m < 0 || m >= 1000 || v.motions.size() >= 32) { okm = false; break; }
                v.motions.push_back(static_cast<int>(m));
                s = end;
                if (*s != '/') break;
                s++;
            }
            float fr = 0.0f, to = 0.0f, mx = 0.0f, l = 0.0f;
            const int got = okm && *s == ',' ? std::sscanf(s + 1, "%f,%f,%f,%f", &fr, &to, &mx, &l) : 0;
            if (got >= 3 && fr >= 0.0f && to > fr && to <= 600.0f && mx > 0.0f && mx <= 10.0f && l >= 0.0f && l <= 5.0f) {
                v.from = fr; v.to = to; v.max = mx; v.lift = got >= 4 ? l : 0.0f;
                e.vtracks.push_back(v);
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad vtrack=%s (want MOTION[/MOTION...],FROM,TO,MAX[,LIFT]), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "warpreach") {   // homing reach per motion, e.g. warpreach=87/88,30: those warp= motions go up to 30 m (warp_step_hook)
            std::vector<int> ms;
            const char* v = tok.c_str() + eq + 1;
            bool okm = true;
            for (;;) {
                char* end = nullptr;
                const long m = std::strtol(v, &end, 10);
                if (end == v || m < 0 || m >= 1000 || ms.size() >= 32) { okm = false; break; }
                ms.push_back(static_cast<int>(m));
                v = end;
                if (*v != '/') break;
                v++;
            }
            float rr = 0.0f; char tail = 0;
            const int got = okm && *v == ',' ? std::sscanf(v + 1, "%f%c", &rr, &tail) : 0;
            if (got == 1 && rr > 0.0f && rr <= 100.0f) {
                for (int m : ms) e.warp_reach.emplace_back(m, rr);
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad warpreach=%s (want MOTION[/MOTION...],REACH), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "warpfx") {   // thrown sword + warp wisps of the warp= motion, e.g. warpfx=83,5,1.0,1.124,84 (warp_bone_post)
            int sb = -1, wb = -1; float fly = 0.0f, aim = 0.0f, tip = 0.0f;
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%f,%f,%f,%d", &sb, &fly, &aim, &tip, &wb);
            if (got >= 4 && sb >= -1 && sb < 256 && fly >= 0.0f && fly <= 120.0f && aim >= -5.0f && aim <= 5.0f && tip >= 0.0f && tip <= 10.0f &&
                (got < 5 || (wb >= -1 && wb < 256))) {
                e.warp_sword = sb; e.warp_fly = fly; e.warp_aim = aim; e.warp_tip = tip; e.warp_wisp = got >= 5 ? wb : -1;
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad warpfx=%s (want SWORD,FLY,AIM,TIP[,WISP]), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "landact") {   // touchdown action, e.g. landact=3024,54,55: touchdown while motion 54 / 55 plays starts action 3024 (land_hook)
            int a = -1, m[4] = {-1, -1, -1, -1};
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%d,%d,%d,%d", &a, &m[0], &m[1], &m[2], &m[3]);
            bool ok = got >= 2 && a >= 0 && a < 0xFFFF;
            for (int i = 0; ok && i < got - 1; i++) ok = m[i] >= 0 && m[i] < 1000;
            if (ok) { e.land_act = a; for (int i = 0; i < 4; i++) e.land_mot[i] = i < got - 1 ? m[i] : -1; }
            else std::fprintf(stderr, "[ROSTER] charId %d: bad landact=%s (want ACTION,MOTION[,MOTION...]), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "chainact") {   // a chain window starts its action, e.g. chainact=3026 (chain_add_hook, chain_tick)
            int a[4] = {-1, -1, -1, -1}; char tail = 0;
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%d,%d,%d%c", &a[0], &a[1], &a[2], &a[3], &tail);
            bool ok = got >= 1 && got <= 4;
            for (int i = 0; ok && i < got; i++) ok = a[i] >= 0 && a[i] < 0xFFFF;
            if (ok) { for (int i = 0; i < 4; i++) e.chain_act[i] = i < got ? a[i] : -1; }
            else std::fprintf(stderr, "[ROSTER] charId %d: bad chainact=%s (want ACTION[,ACTION...], up to 4), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "hitcam") {     // hitcam=TICKS: when hitact= starts its action, the camera frames him and his target
            int t = 0; char tail = 0;
            if (std::sscanf(tok.c_str() + eq + 1, "%d%c", &t, &tail) == 1 && t > 0 && t <= 1800) e.hit_cam = t;
            else std::fprintf(stderr, "[ROSTER] charId %d: bad hitcam=%s (want TICKS, 1-1800), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "hitact") {     // an on-hit magi child starts an action, e.g. hitact=2277,3029 (hit_spawn_hook, chain_tick)
            int m = -1, a = -1; char tail = 0;
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%d%c", &m, &a, &tail);
            if (got == 2 && m >= 0 && m < 0xFFFF && a >= 0 && a < 0xFFFF) { e.hit_magi = m; e.hit_act = a; }
            else std::fprintf(stderr, "[ROSTER] charId %d: bad hitact=%s (want MAGI,ACTION), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "warpdash") {   // R+Triangle warp dash, e.g. warpdash=3027,3028,16,1.0,24,1 (dash_start_hook, warpdash_link)
            int g = -1, a = -1, link = 24, hp = 1; float rg = 16.0f, nr = 1.0f; char tail = 0;
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%d,%f,%f,%d,%d%c", &g, &a, &rg, &nr, &link, &hp, &tail);
            if (got >= 4 && got <= 6 && g >= 0 && g < 0xFFFF && a >= 0 && a < 0xFFFF && rg > 0.0f && rg <= 100.0f &&
                nr >= 0.0f && nr <= 20.0f && link > 0 && link <= 600 && (hp == 0 || hp == 1)) {
                e.wd_ground = g; e.wd_air = a; e.wd_range = rg; e.wd_near = nr; e.wd_link = link; e.wd_hp = hp != 0;
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad warpdash=%s (want GROUND,AIR,RANGE,NEAR[,LINK[,HP]]), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "wdopen") {   // warp dash link into press 1 for these commands (hex), e.g. wdopen=AB/AC (warpdash_link)
            std::vector<int> cs;
            const char* v = tok.c_str() + eq + 1;
            bool okc = true;
            for (;;) {
                char* end = nullptr;
                const long c = std::strtol(v, &end, 16);
                if (end == v || c < 0 || c > 0xFFFF || cs.size() >= 16) { okc = false; break; }
                cs.push_back(static_cast<int>(c));
                v = end;
                if (*v != '/') break;
                v++;
            }
            if (okc && *v == 0) e.wd_open = cs;
            else std::fprintf(stderr, "[ROSTER] charId %d: bad wdopen=%s (want CMD[/CMD...] in hex), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "dash") {   // R+Triangle dash speed factor, e.g. dash=1.75 (dash_move_hook)
            float f = std::strtof(tok.c_str() + eq + 1, nullptr);
            e.dash = (f >= 0.25f && f <= 4.0f) ? f : 1.0f;
            continue;
        }
        if (k == "exranged") {   // EX Mode: 1 = projectiles pass through, 2 = also summoned / thrown objects
            e.exranged = std::atoi(tok.c_str() + eq + 1);
            continue;
        }
        if (k == "armtrail") {   // armtrail=ID,SLOT0,NSLOT,BONE0:COUNT:BIT0[:DRAW],...  (armtrail_bone_post / armtrail_consume_hook)
            const char* q = tok.c_str() + eq + 1; char* end = nullptr; bool ok = true;
            long id_ = std::strtol(q, &end, 10); ok = ok && end != q && *end == ','; q = end + 1;
            long s0 = ok ? std::strtol(q, &end, 10) : 0; ok = ok && end != q && *end == ','; q = end + 1;
            long ns = ok ? std::strtol(q, &end, 10) : 0; ok = ok && end != q && *end == ','; q = end + 1;
            ok = ok && id_ >= 0 && id_ <= 0xFFFF && s0 >= 4 && ns >= 1 && s0 + ns <= 16;
            int nset = 0, b0[4] = {}, cn[4] = {}, bt[4] = {}, dr[4] = {-1, -1, -1, -1}, total = 0;
            while (ok && *q) {
                if (nset >= 4) { ok = false; break; }
                long v[4] = {0, 0, 0, -1}; int got = 0;
                while (got < 4) {
                    v[got++] = std::strtol(q, &end, 10);
                    if (end == q) { ok = false; break; }
                    q = end;
                    if (*q == ':') q++; else break;
                }
                if (!ok || got < 3 || v[0] < 0 || v[0] > 254 || v[1] < 1 || v[1] > 32 || v[2] < 0 || v[2] + v[1] > 32 ||
                    (got == 4 && (v[3] < 0 || v[3] > 254))) { ok = false; break; }
                b0[nset] = static_cast<int>(v[0]); cn[nset] = static_cast<int>(v[1]); bt[nset] = static_cast<int>(v[2]);
                dr[nset] = got == 4 ? static_cast<int>(v[3]) : -1; total += cn[nset]; nset++;
                if (*q == ',') q++; else if (*q) { ok = false; break; }
            }
            if (ok && nset > 0 && total <= 32) {
                e.at_id = static_cast<int>(id_); e.at_slot0 = static_cast<int>(s0); e.at_nslot = static_cast<int>(ns); e.at_nset = nset;
                for (int i = 0; i < 4; i++) { e.at_bone0[i] = b0[i]; e.at_count[i] = cn[i]; e.at_bit0[i] = bt[i]; e.at_draw[i] = dr[i]; }
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad armtrail=%s (want ID,SLOT0>=4,NSLOT,BONE0:COUNT:BIT0[:DRAW],... "
                             "SLOT0+NSLOT<=16, at most 4 sets / 32 arms), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "apocarms") {   // Apocalypsis arms riding their magi, e.g. apocarms=168,12,2220,2222,2223,2228 (apoc_bone_post)
            long v[10]; int got = 0; const char* q = tok.c_str() + eq + 1; char* end = nullptr; bool ok = true;
            while (*q && got < 10) {
                v[got++] = std::strtol(q, &end, 10);
                if (end == q) { ok = false; break; }
                q = end;
                if (*q == ',') q++; else if (*q) { ok = false; break; }
            }
            if (ok && !*q && got >= 3 && v[0] >= 0 && v[0] < 256 && v[1] > 0 && v[1] <= 16) {
                ApocSet st; st.arm0 = static_cast<int>(v[0]); st.narm = static_cast<int>(v[1]);
                for (int i = 2; i < got && st.nids < 8; i++)
                    if (v[i] >= 0 && v[i] <= 0xFFFF) st.ids[st.nids++] = static_cast<uint16_t>(v[i]);
                if (e.apoc_narm <= 0) {                     // the first token, as before
                    e.apoc_arm0 = st.arm0; e.apoc_narm = st.narm; e.apoc_nids = st.nids;
                    for (int i = 0; i < 8; i++) e.apoc_ids[i] = st.ids[i];
                } else if (e.apoc_nmore < 3) {
                    e.apoc_more[e.apoc_nmore++] = st;        // a further arm set (e.g. apocarms=81,1,2230)
                } else {
                    std::fprintf(stderr, "[ROSTER] charId %d: more than 4 apocarms= tokens, apocarms=%s ignored\n", id, tok.c_str() + eq + 1);
                }
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad apocarms=%s (want ARM0,NARM,ID1[,ID2..]), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "pinspin") {   // an apocarms= arm spinning while it rides its bullet, e.g. pinspin=81,40,0.844,-0.537,0 (apoc_bone_post)
            int b = -1; float dg = 0.0f, ax = 0.0f, ay = 0.0f, az = 0.0f; char tail = 0;
            const int got = std::sscanf(tok.c_str() + eq + 1, "%d,%f,%f,%f,%f%c", &b, &dg, &ax, &ay, &az, &tail);
            const float l = std::sqrt(ax * ax + ay * ay + az * az);
            if (got == 5 && b >= 0 && b < 256 && std::fabs(dg) <= 180.0f && l > 1e-3f && e.pin_nspin < 4) {
                PinSpin& sp = e.pin_spin[e.pin_nspin++];
                sp.bone = b; sp.deg = dg; sp.ax[0] = ax / l; sp.ax[1] = ay / l; sp.ax[2] = az / l;
            } else {
                std::fprintf(stderr, "[ROSTER] charId %d: bad pinspin=%s (want BONE,DEG,AX,AY,AZ), ignored\n", id, tok.c_str() + eq + 1);
            }
            continue;
        }
        if (k == "projblock") {   // projectiles blocked while a motion plays, e.g. projblock=70/68,8,78,169:541/170:542 (can_hit_hook)
            ProjBlock b; bool ok = true;
            const std::string v = tok.substr(eq + 1);
            std::vector<std::string> f;
            for (size_t a = 0;;) {
                const size_t c = v.find(',', a);
                f.push_back(v.substr(a, c == std::string::npos ? std::string::npos : c - a));
                if (c == std::string::npos) break;
                a = c + 1;
            }
            if (f.size() < 3 || f.size() > 4) ok = false;
            for (size_t a = 0; ok && a <= f[0].size();) {           // MOTION[/MOTION...]
                size_t c = f[0].find('/', a); if (c == std::string::npos) c = f[0].size();
                const std::string one = f[0].substr(a, c - a);
                char* end = nullptr;
                const long mv = std::strtol(one.c_str(), &end, 10);
                if (one.empty() || *end || mv < 0 || mv > 999 || b.motions.size() >= 32) ok = false;
                else b.motions.push_back(static_cast<int>(mv));
                a = c + 1;
            }
            if (ok) {                                                // FROM,TO in timeline ticks (60 per second)
                char* end = nullptr;
                b.from = std::strtof(f[1].c_str(), &end); ok = !f[1].empty() && !*end;
                if (ok) { b.to = std::strtof(f[2].c_str(), &end); ok = !f[2].empty() && !*end; }
                ok = ok && b.from >= 0.0f && b.to >= b.from && b.to <= 100000.0f;
            }
            for (size_t a = 0; ok && f.size() == 4 && a <= f[3].size();) {   // BONE:FEPA[/BONE:FEPA...] deflect sparkles
                size_t c = f[3].find('/', a); if (c == std::string::npos) c = f[3].size();
                const std::string one = f[3].substr(a, c - a);
                int bn = -1, fx = -1; char tail = 0;
                if (b.nfx >= 8 || std::sscanf(one.c_str(), "%d:%d%c", &bn, &fx, &tail) != 2 || bn < 0 || bn > 255 || fx < 0 || fx > 0xFFFF) ok = false;
                else { b.fx_bone[b.nfx] = bn; b.fx_id[b.nfx] = fx; b.nfx++; }
                a = c + 1;
            }
            if (ok) e.projblocks.push_back(b);
            else std::fprintf(stderr, "[ROSTER] charId %d: bad projblock=%s (want MOTION[/MOTION...],FROM,TO[,BONE:FEPA[/BONE:FEPA...]]), ignored\n", id, tok.c_str() + eq + 1);
            continue;
        }
        if (k == "armiger") {    // EX Mode parts: orbit, floor, flash bones, ring and fx draw bones
            int b[6] = {-1, -1, -1, -1, -1, -1}; char tail = 0;
            const int n = std::sscanf(tok.c_str() + eq + 1, "%d,%d,%d,%d,%d,%d%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail);
            if (n == 5 || n == 6)
                for (int i = 0; i < 6; i++) e.armiger[i] = b[i];
            else std::fprintf(stderr, "[ROSTER] bad armiger= token: %s\n", tok.c_str());
            continue;
        }
        if (k == "exarms") {     // EX Mode arms: first arm bone, arm count, first deflect FEPA id (a token of its own so a
                                 // runtime from before it still reads armiger= and ignores this)
            int b[3] = {-1, -1, -1}; char tail = 0;
            const int n = std::sscanf(tok.c_str() + eq + 1, "%d,%d,%d%c", &b[0], &b[1], &b[2], &tail);
            if ((n == 2 || n == 3) && b[0] >= 0 && b[1] > 0 && b[1] <= 16)
                for (int i = 0; i < 3; i++) e.armiger[6 + i] = b[i];
            else std::fprintf(stderr, "[ROSTER] bad exarms= token: %s\n", tok.c_str());
            continue;
        }
        int v = std::atoi(tok.c_str() + eq + 1);
        if (k == "tile") e.tile = v; else if (k == "art") e.art = v; else if (k == "artl") e.artl = v; else if (k == "artr") e.artr = v;
        else if (k == "name") e.name = v; else if (k == "text") e.text = v; else if (k == "logo") e.logo = v;
    }
    e.like = like;
    e.str_addr = SCRATCH + 8u * static_cast<uint32_t>(g_entries.size());
    g_entries.push_back(e);
}

void load_config() {
    if (const char* env = std::getenv("PSPRECOMP_ROSTER")) {
        std::string s = env;
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find(';', p);
            if (q == std::string::npos) q = s.size();
            parse_spec(s.substr(p, q - p));
            p = q + 1;
        }
        return;
    }
    std::ifstream f(dissidia_mods_dir() + "/roster.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        parse_spec(line);
    }
}

void put_u32(uint8_t* base, uint32_t off, uint32_t v) { std::memcpy(base + off, &v, 4); }

}  // namespace

// Called from register_hooks (before data sections / boot).
void dissidia_roster_init(uint8_t* rdram) {
    g_rdram = rdram;
    load_config();
    g_enabled = !g_entries.empty();
    if (!g_enabled) return;
    for (const Entry& e : g_entries) {
        uint8_t* dst = rdram + (e.str_addr & 0x07FFFFFFu);
        std::memset(dst, 0, 8);
        std::memcpy(dst, e.code.c_str(), e.code.size() > 7 ? 7 : e.code.size());
        std::fprintf(stderr, "[ROSTER] charId %d -> \"%s\" (string at 0x%08X)\n", e.charId, e.code.c_str(), e.str_addr);
    }
}

bool dissidia_roster_enabled() { return g_enabled; }

// Called from on_boot_context: EBOOT data lives in rdram now.
void dissidia_roster_boot_patch(uint8_t* rdram) {
    if (!g_enabled) return;
    for (const PtrTable& t : k_tables) {
        if (t.mod != MOD_EBOOT) continue;
        for (const Entry& e : g_entries) {
            if (e.charId >= kRetailSlots) continue;      // EBOOT tables hold 35 slots
            uint32_t at = (t.slot0 + 4u * static_cast<uint32_t>(e.charId)) & 0x07FFFFFFu;
            uint32_t val = e.str_addr;
            // The voice bank loader (FUN_088A3A94) resolves each slot's name in a bank index (a data file
            // read at boot) and never returns for a name the index does not list. A new code string has
            // no voice entry yet, so the slot reuses `like`'s voice bank (no voice of its own).
            if (e.like >= 1 && e.like <= 33 && e.charId >= kFirstNew) {
                uint32_t lp; std::memcpy(&lp, rdram + ((t.slot0 + 4u * static_cast<uint32_t>(e.like)) & 0x07FFFFFFu), 4);
                if (lp) val = lp;
            }
            put_u32(rdram, at, val);
            std::fprintf(stderr, "[ROSTER] eboot: slot[%d] @0x%08X <- 0x%08X (%s)\n", e.charId, at | 0x08000000u, val, t.what);
        }
    }
}

// Which overlay a path names: returns MOD_* or -1.
int dissidia_roster_overlay_of(const char* lower_path) {
    std::string p = lower_path;
    auto ends = [&](const char* s) { size_t n = std::strlen(s); return p.size() >= n && p.compare(p.size() - n, n, s) == 0; };
    if (ends("ovl_battle_app.elf")) return MOD_BATTLE;
    if (ends("ovl_menu_app.elf")) return MOD_MENU;
    if (ends("ovl_extra_app.elf")) return MOD_EXTRA;
    return -1;
}

// The game just read file bytes [off, off+size) of overlay `mod` into dest: patch the table slots
// that fall inside that range (file offset = vaddr - OVL_BASE + 0x54). Relocated tables are filled
// whether or not a roster is configured, because the patched overlay code always reads the copies.
static int g_cur_ovl = -1;   // last overlay streamed into the shared window
void dissidia_roster_patch_overlay(int mod, int64_t off, uint32_t size, uint8_t* dest) {
    if (mod == MOD_MENU || mod == MOD_BATTLE || mod == MOD_EXTRA) g_cur_ovl = mod;
    auto in_range = [&](uint32_t vaddr, int64_t* at) {
        *at = static_cast<int64_t>(vaddr) - OVL_BASE + OVL_FILE_HDR;
        return *at >= off && *at + 4 <= off + static_cast<int64_t>(size);
    };
    auto relocate = [&](const PtrTable& t, bool name_table) {
        if (t.mod != mod || !t.reloc) return;
        uint8_t* guest = g_rdram ? g_rdram + (t.reloc & 0x07FFFFFFu) : nullptr;
        if (!guest) return;
        for (int id = 0; id < kRetailSlots; id++) {         // copy the retail entries that this read delivered
            int64_t at;
            if (in_range(t.slot0 + 4u * static_cast<uint32_t>(id), &at))
                std::memcpy(guest + 4 * id, dest + (at - off), 4);
        }
        if (!g_enabled) return;
        for (const Entry& e : g_entries) {
            if (e.charId >= kRosterMax) continue;
            if (name_table) {
                if (e.text >= 0) put_u32(guest, 4u * static_cast<uint32_t>(e.charId), static_cast<uint32_t>(e.text));
            } else {
                put_u32(guest, 4u * static_cast<uint32_t>(e.charId), e.str_addr);
            }
        }
        static int logged = 0;
        if (logged++ < 8)
            std::fprintf(stderr, "[ROSTER] overlay %d: %s relocated to 0x%08X (%d slots)\n", mod, t.what, t.reloc, kRosterMax);
    };
    for (const PtrTable& t : k_tables) relocate(t, false);
    relocate(k_name_table, true);
    if (mod == MOD_BATTLE) {   // Opponent Info text table: 35 x {text id, alt text id}, 8 bytes per charId
        uint8_t* guest = g_rdram ? g_rdram + (DISSIDIA_TBL_OPPINFO & 0x07FFFFFFu) : nullptr;
        if (guest) {
            for (int id = 0; id < kRetailSlots; id++)
                for (int w = 0; w < 2; w++) {
                    int64_t at;
                    if (in_range(0x08AFB6E8u + 8u * static_cast<uint32_t>(id) + 4u * static_cast<uint32_t>(w), &at))
                        std::memcpy(guest + 8 * id + 4 * w, dest + (at - off), 4);
                }
            for (int id = kRetailSlots; id < kRosterMax; id++) { put_u32(guest, 8u * id, 0xFFFFFFFFu); put_u32(guest, 8u * id + 4, 0xFFFFFFFFu); }
            if (g_enabled)
                for (const Entry& e : g_entries) {   // an extra with a label gets a marker id that oppinfo_name_hook resolves
                    if (e.charId >= kRosterMax || e.label.empty()) continue;
                    put_u32(guest, 8u * e.charId, OPPINFO_MARK + e.charId);
                    put_u32(guest, 8u * e.charId + 4, OPPINFO_MARK + e.charId);
                }
        }
    }
    if (!g_enabled) return;
    for (const PtrTable& t : k_tables) {                  // in-place copies stay consistent (debuggers, hooks)
        if (t.mod != mod) continue;
        for (const Entry& e : g_entries) {
            if (e.charId >= kRetailSlots) continue;
            int64_t at;
            if (in_range(t.slot0 + 4u * static_cast<uint32_t>(e.charId), &at))
                put_u32(dest, static_cast<uint32_t>(at - off), e.str_addr);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Quick Battle "Player Select" grid (menu overlay). Hooks, both reached through RECOMP_LOOKUP:
//  * FUN_08A85ECC returns the 12-byte record table (33 rows at 0x08A864D0: {tile frame, frame,
//    series, aux id, charId, 0}). The hook returns a copy in scratch RAM with row 33 appended for
//    the extra character (cloned from `like`, charId replaced). overlay_menu.toml raises the three
//    recompiled bounds (idx < 34) so row 33 is scanned and accepted.
//  * FUN_088D5AC8 builds the list of selectable characters ({charId, flags} x n, from the EBOOT
//    table at 0x089D7BE0). The wrapper appends a clone of `like`'s entry with the new charId.
// The grid has tile sprites for 34 positions; with more entries the 40-cell layout below takes over.
namespace {
constexpr uint32_t GRID_TABLE_SRC = 0x08A864D0u;
constexpr uint32_t GRID_TABLE_DST = DISSIDIA_GRID_DST;   // kGridRows rows * 12 bytes
FuncPtr g_orig_list_build = nullptr;
int g_grid_logged = 0;

// The roster entries that take a grid row / list slot: every configured character that is not retail.
std::vector<const Entry*> extras() {
    std::vector<const Entry*> v;
    for (const Entry& e : g_entries) if (e.charId >= kFirstNew && e.charId < kRosterMax) v.push_back(&e);
    return v;
}

// The recompiled code does not keep $ra for calls, so the accessor cannot see its caller. The two
// callers that read record field 0 for different textures are wrapped to say who is asking.
enum Caller { CALLER_OTHER = 0, CALLER_TILE = 1, CALLER_ART = 2 };
int g_caller = CALLER_OTHER;
FuncPtr g_orig_placer = nullptr, g_orig_cursor = nullptr;
void placer_wrap(uint8_t* rdram, recomp_context* ctx) {   // FUN_08A7FB54: f0 -> grid tile texture
    static const bool dbg = std::getenv("PSPRECOMP_ROSTER_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[ROSTER] placer obj=0x%08X rec=%d slot=%d\n", (uint32_t)ctx->r[4], (int)ctx->r[5], (int)ctx->r[6]);
    int prev = g_caller; g_caller = CALLER_TILE;
    g_orig_placer(rdram, ctx);
    g_caller = prev;
}
void cursor_wrap(uint8_t* rdram, recomp_context* ctx) {   // FUN_08A84458: f0 -> slide-in art, f2 -> logo
    int prev = g_caller; g_caller = CALLER_ART;
    g_orig_cursor(rdram, ctx);
    g_caller = prev;
}

void grid_table_hook(uint8_t* rdram, recomp_context* ctx) {
    const int who = g_caller;
    const uint8_t* src = rdram + (GRID_TABLE_SRC & 0x07FFFFFFu);
    uint8_t* dst = rdram + (GRID_TABLE_DST & 0x07FFFFFFu);
    std::memcpy(dst, src, kGridRetailRows * 12);
    std::memset(dst + kGridRetailRows * 12, 0xFF, (kGridRows - kGridRetailRows) * 12);
    int row_no = kGridRetailRows;
    for (const Entry* e : extras()) {
        if (row_no >= kGridRows) break;
        for (int i = 0; i < kGridRetailRows - 1; i++) {
            int16_t id; std::memcpy(&id, src + 12 * i + 8, 2);
            if (id != e->like) continue;
            uint8_t* row = dst + row_no * 12;
            std::memcpy(row, src + 12 * i, 12);
            auto put = [&](int field, int v) { int16_t sv = static_cast<int16_t>(v); std::memcpy(row + 2 * field, &sv, 2); };
            put(4, e->charId);
            if (who == CALLER_TILE && e->tile >= 0) put(0, e->tile);
            if (who == CALLER_ART && e->art >= 0) put(0, e->art);
            if (e->name >= 0) put(1, e->name);
            if (e->logo >= 0) put(2, e->logo);
            if (row_no == kGridRetailRows) {   // live experiment hook: `W 0x08700100 <12 bytes + 78563412>` overrides the first extra row
                const uint8_t* st = rdram + (0x08700100u & 0x07FFFFFFu);
                uint32_t magic; std::memcpy(&magic, st + 12, 4);
                if (magic == 0x12345678u) std::memcpy(row, st, 12);
            }
            if (g_grid_logged++ < 4) std::fprintf(stderr, "[ROSTER] grid: row %d = clone of row %d (charId %d) -> charId %d\n", row_no, i, e->like, e->charId);
            row_no++;
            break;
        }
    }
    ctx->r[2] = GRID_TABLE_DST;
}

FuncPtr g_orig_vsload = nullptr;
void vsload_dbg(uint8_t* rdram, recomp_context* ctx) {   // DEBUG: FUN_08AF6B70 player records
    uint32_t a0 = (uint32_t)ctx->r[4];
    for (int r = 0; r < 2; r++) {
        uint32_t base = a0 + 0x174 + 0x6a * r;
        int16_t h[8]; std::memcpy(h, rdram + (base & 0x07FFFFFFu), 16);
        std::fprintf(stderr, "[ROSTER] vsload a0=%08X rec%d: %d %d %d %d %d %d %d %d\n", a0, r, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    }
    g_orig_vsload(rdram, ctx);
    std::fprintf(stderr, "[ROSTER] vsload done, handles:");
    for (int k = 0; k < 11; k++) { uint32_t w; std::memcpy(&w, rdram + ((a0 + 4 * k) & 0x07FFFFFFu), 4); std::fprintf(stderr, " %08X", w); }
    std::fprintf(stderr, "\n");
}
// DEBUG (PSPRECOMP_ROSTER_LOADLOG=1): one line per FUN_088D0810 (gim/file load request) with the path string.
FuncPtr g_orig_loadreq = nullptr;
void loadreq_log(uint8_t* rdram, recomp_context* ctx) {
    uint32_t p = static_cast<uint32_t>(ctx->r[7]);
    char txt[97]; int n = 0;
    if (p >= 0x08000000u && p < 0x0C000000u) for (; n < 96; n++) { char c = static_cast<char>(rdram[(p & 0x07FFFFFFu) + n]); if (c < 32 || c > 126) break; txt[n] = c; }
    txt[n] = 0;
    std::fprintf(stderr, "[ROSTER] loadreq a1=%X a2=%X ra=%08X path=%s\n", (uint32_t)ctx->r[5], (uint32_t)ctx->r[6], (uint32_t)ctx->r[31], txt);
    g_orig_loadreq(rdram, ctx);
}

// FUN_088B3B20(table, charId) binary-searches the character info table (0x68-byte records keyed by a
// word charId, loaded from the disc) and returns the record or 0. FUN_088B3958 wraps it and many
// callers (the battle actor spawn FUN_08888354, the battle/menu overlays) skip a character with no
// record. Extra charIds have none, so return a clone of `like`'s record with the key replaced.
FuncPtr g_orig_charinfo = nullptr;
void charinfo_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t key = static_cast<uint32_t>(ctx->r[5]);
    g_orig_charinfo(rdram, ctx);
    static const bool dbg = std::getenv("PSPRECOMP_ROSTER_DEBUG") != nullptr;
    if (dbg && key < 64) std::fprintf(stderr, "[ROSTER] charinfo lookup %u -> 0x%08X\n", key, (uint32_t)ctx->r[2]);
    if (ctx->r[2] != 0 || key < static_cast<uint32_t>(kFirstNew) || key >= static_cast<uint32_t>(kRosterMax)) return;
    for (const Entry* e : extras()) {
        if (static_cast<uint32_t>(e->charId) != key) continue;
        ctx->r[5] = e->like;
        g_orig_charinfo(rdram, ctx);
        if (ctx->r[2] == 0) return;
        const uint32_t at = DISSIDIA_CHARINFO + 0x68u * key;
        uint8_t* dst = rdram + (at & 0x07FFFFFFu);
        std::memcpy(dst, rdram + (static_cast<uint32_t>(ctx->r[2]) & 0x07FFFFFFu), 0x68);
        std::memcpy(dst, &key, 4);
        ctx->r[2] = static_cast<int32_t>(at);
        return;
    }
}


// FUN_088CB3B4(charId) -> index of charId in the 31-entry id list at 0x089CBC98 (-1 when absent). That list
// is the per-character save record table (0x778-byte records, FUN_088CB164 returns a record or a shared dummy,
// FUN_088CB1EC tests "has a record"). The battle setter in the menu overlay (0x08A48DB0 / 0x08A491E0) skips a
// party slot without a record, so a charId without one never gets an actor struct (blank VS screen, no battle
// data). The table cannot grow (fixed BSS block), so an extra character shares the record of its `like`.
FuncPtr g_orig_saveidx = nullptr, g_orig_saverec = nullptr;
const Entry* extra_of(uint32_t id) {
    id &= 0xFFu;
    if (id < static_cast<uint32_t>(kFirstNew) || id >= static_cast<uint32_t>(kRosterMax)) return nullptr;
    for (const Entry* e : extras()) if (e->charId == static_cast<int>(id)) return e;
    return nullptr;
}
void saveidx_hook(uint8_t* rdram, recomp_context* ctx) {
    if (const Entry* e = extra_of(static_cast<uint32_t>(ctx->r[4]))) ctx->r[4] = e->like;
    g_orig_saveidx(rdram, ctx);
}
// FUN_088CB164(party, charId) -> the 0x778-byte record. The VS screen / battle setup copy the record's own charId
// byte, so sharing like's record would turn the extra character back into `like` (Bartz's name and face on the VS
// screen). An extra character gets a private copy of like's record with its own charId, and with like's abilities
// moved to the charId's own copies of them (system.bin 'ablt' records owned by the charId, made by
// tools/roster_mainbin.py): an ability id is (owner << 8) | n, a set stores only n and composes the id with the set's
// owner byte (FUN_088B7A88), and the Abilities screen (menu FUN_08AD6698) lists only the abilities whose 'ablt' owner
// (+0x1B) is the charId being edited and that the record's learned list holds. With like's ids the screen listed no
// attack at all and refused to close ("no HP attacks enabled", 19 Oct). Record layout (ability block = record + 0x10,
// FUN_088CBA54 / FUN_088B83F0 set the owner bytes the same way): 5 sets of 0xC6 bytes (+0xB6 owner), the learned list at
// +0x3DE (150 x {s16 id, s16 AP, u16 flags}), owner at +0x762, current set at +0x763.
// While the copy still follows like it is made again on every call, until the game writes to it (the Abilities screen
// commits its edits through this record, FUN_08AF3ECC; battles add EXP / AP); from then on it is the character's own
// record (refreshing it would throw the edits away). The memory card save holds the 31 retail records only, so own
// records are kept in a sidecar file of each save (see "Extra records in the save" below).
constexpr uint32_t kSaveRec = 0x778u;
constexpr uint32_t kRecAbil = 0x10u, kAbilSets = 5u, kAbilSetBytes = 0xC6u, kAbilSetOwner = 0xB6u;
constexpr uint32_t kAbilLearned = 0x3DEu, kAbilLearnedRows = 150u, kAbilLearnedRow = 6u, kAbilOwner = 0x762u;
constexpr int kExtraSlots = kRosterMax - kFirstNew;            // private record slot = charId - kFirstNew
enum RecState : uint8_t { REC_FOLLOW = 0, REC_OWN = 1 };       // follows like's record / the character's own record
std::vector<uint8_t> g_saverec_made[kExtraSlots];   // what saverec_hook last wrote into each private record (REC_FOLLOW)
RecState g_saverec_state[kExtraSlots] = {};
uint32_t g_saverec_block = 0;
// The private records live in a host block: the scratch area at DISSIDIA_SAVEREC is inside the volatile memory the
// game loads into, which was fine for a copy refreshed on every call but not for one that keeps the player's changes.
// Allocated at boot (from the ablt hook) while the host heap has room; 0 = none, the scratch area is used instead.
uint32_t saverec_block() {
    static bool tried = false;
    if (!tried && g_enabled && !extras().empty()) {
        tried = true;
        g_saverec_block = psp_kmem_bump_alloc_named((kSaveRec * (kRosterMax - kFirstNew) + 0xFFu) & ~0xFFu, "ROSTER_SAVEREC");
        if (!g_saverec_block) std::fprintf(stderr, "[ROSTER] no host memory for the extra save records: ability edits will not stick\n");
    }
    return g_saverec_block;
}
void derive_saverec(uint8_t* dst, const uint8_t* src, const Entry& e) {
    std::memcpy(dst, src, kSaveRec);
    const uint8_t from = static_cast<uint8_t>(e.like), to = static_cast<uint8_t>(e.charId);
    dst[0] = to;
    uint8_t* ab = dst + kRecAbil;
    if (ab[kAbilOwner] == from) ab[kAbilOwner] = to;
    for (uint32_t s = 0; s < kAbilSets; s++)
        if (ab[kAbilSetBytes * s + kAbilSetOwner] == from) ab[kAbilSetBytes * s + kAbilSetOwner] = to;
    for (uint32_t i = 0; i < kAbilLearnedRows; i++) {   // little endian u16: byte 1 is the owner
        uint8_t* id = ab + kAbilLearned + kAbilLearnedRow * i;
        if (id[1] == from) id[1] = to;
    }
}
// The extras know all of their own attacks from the start (19 Oct: "can we set him to have all moves active?";
// following Bartz's learned list, Noctis had a single HP attack at level 1, Hellfire). The learned list is 150 rows
// {s16 id, s16 AP, u16 flags} filled from the front, id -1 = free: FUN_088B7E30 clears it to {-1, 0, flags | 1} and
// FUN_088B7ED4 learns an id by writing it into the first free row (AP and flags untouched). This does the same for
// every 'ablt' record whose owner byte (+0x1B) is the charId, on every record the hook hands out (followed, own or
// restored from a sidecar), so it also covers saves made before it. The table pointers are the ones FUN_088B1CC0 (or
// ablt_load_hook below) stores. Set kExtrasKnowAllAbilities to false to go back to learning them by level.
constexpr bool kExtrasKnowAllAbilities = true;
constexpr uint32_t kAbltBlockPtr = 0x089D7290u, kAbltRecsPtr = 0x089D7294u, kAbltRecBytes = 0x28u, kAbltOwnerByte = 0x1Bu;
void learn_all_own(uint8_t* rdram, uint8_t* rec, const Entry& e) {
    if (!kExtrasKnowAllAbilities) return;
    auto rd = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x07FFFFFFu), 4); return v; };
    const uint32_t blk = rd(kAbltBlockPtr), recs = rd(kAbltRecsPtr);
    if (blk < 0x08000000u || blk >= 0x0C000000u || recs < 0x08000000u || recs >= 0x0C000000u) return;
    const uint32_t n = rd(blk + 8u);
    if (n == 0 || n > 0x1000u) return;
    uint8_t* learned = rec + kRecAbil + kAbilLearned;
    for (uint32_t r = 0; r < n; r++) {
        const uint8_t* a = rdram + ((recs + kAbltRecBytes * r) & 0x07FFFFFFu);
        if (a[kAbltOwnerByte] != static_cast<uint8_t>(e.charId)) continue;
        for (uint32_t i = 0; i < kAbilLearnedRows; i++) {
            uint8_t* row = learned + kAbilLearnedRow * i;
            if (row[0] == a[0] && row[1] == a[1]) break;                                  // already known
            if (row[0] == 0xFFu && row[1] == 0xFFu) { row[0] = a[0]; row[1] = a[1]; break; } // first free row
        }
    }
}
void saverec_hook(uint8_t* rdram, recomp_context* ctx) {
    const Entry* e = extra_of(static_cast<uint32_t>(ctx->r[5]));
    if (!e) { g_orig_saverec(rdram, ctx); return; }
    ctx->r[5] = e->like;
    g_orig_saverec(rdram, ctx);
    const uint32_t src = static_cast<uint32_t>(ctx->r[2]);
    if (src < 0x08000000u || src >= 0x0C000000u) return;
    const uint32_t slot = static_cast<uint32_t>(e->charId - kFirstNew);
    const uint32_t block = saverec_block();
    const uint32_t dst = (block ? block : static_cast<uint32_t>(DISSIDIA_SAVEREC)) + kSaveRec * slot;
    uint8_t* d = rdram + (dst & 0x07FFFFFFu);
    std::vector<uint8_t>& made = g_saverec_made[slot];
    if (!block) {
        derive_saverec(d, rdram + (src & 0x07FFFFFFu), *e);    // scratch RAM: refreshed on every call, nothing kept
        learn_all_own(rdram, d, *e);
    } else if (g_saverec_state[slot] == REC_OWN) {
        // the character's own record (changed by the game, or restored from the save's sidecar)
        learn_all_own(rdram, d, *e);
    } else if (made.size() != kSaveRec || std::memcmp(d, made.data(), kSaveRec) == 0) {
        derive_saverec(d, rdram + (src & 0x07FFFFFFu), *e);
        learn_all_own(rdram, d, *e);
        made.assign(d, d + kSaveRec);
    } else {
        g_saverec_state[slot] = REC_OWN;
        made.clear();
        std::fprintf(stderr, "[ROSTER] charId %d: save record changed by the game, kept as its own (saved with the game)\n", e->charId);
    }
    ctx->r[2] = static_cast<int32_t>(dst);
}

// Every extra character follows its like again (a save was loaded, or the live save data was created anew).
// True when one of them had a record of its own.
bool reset_extra_records() {
    bool had = false;
    for (int i = 0; i < kExtraSlots; i++) {
        had = had || g_saverec_state[i] == REC_OWN;
        g_saverec_state[i] = REC_FOLLOW;
        g_saverec_made[i].clear();
    }
    return had;
}

// ---------------------------------------------------------------------------------------------
// Extra records in the save. The memory card save (ms0:/PSP/SAVEDATA/<gameName><saveName>/DISSIDIA.BIN: the 0x3ECC8 bytes
// of the save object *(0x089D712C) from +0xF08, FUN_088AC760, with the 31 retail records at +0xF58) has no room for more
// records and is left exactly as the game writes it. The extras' own records go to a sidecar file of each save instead:
//     ms0:/PSP/ROSTER/<gameName><saveName>_<fileName>.roster      (ULES01505GameData00_DISSIDIA.BIN.roster)
// written whenever the game saves that slot and read back whenever it loads it. The savedata calls are watched at the
// sceUtilitySavedata import stubs: InitStart remembers the parameter block, and the first GetStatus that reports FINISHED
// (or ShutdownStart) handles the finished operation. Only successful loads / saves of the game's own data file count
// (game name *(0x089D6010), data size 0x3ECC8; the import of an earlier DISSIDIA save uses another game name and size).
// The sidecar holds the FNV-1a hash and size of the data file as it is on the memory stick after the save; a load uses it
// only while the file still has them, so a save written without it (older runtime, another emulator, a copied save) never
// gets another save's records. A missing, stale or damaged sidecar means: the extras follow their like, as before.
// A load also drops what the session had: extras with a record in the sidecar get it (as their own), the others follow
// their like. FUN_088CB210 (creates the 31 records of the live save object: New Game, and the reset before a load) makes
// every extra follow its like too.
// File layout (little endian): header {char magic[8] "DSRSTR01", u32 header bytes 0x60, u32 record bytes 0x778, u64 data
// file hash, u32 data file size, u32 records n, char save dir[32], char data file[16], u8 0[8]}, then n x {u8 charId,
// u8 like, u8 0[6], 0x778-byte record}, then the u64 FNV-1a hash of everything before it. Written to <name>.tmp, then renamed.
constexpr uint32_t kMainSaveBytes = 0x3ECC8u;
constexpr uint32_t SAVE_OBJECT = 0x089D712Cu, SAVE_PARTY = 0xF58u, GAME_CODE_PTR = 0x089D6010u, PARTY_INIT = 0x088CB210u;
constexpr uint32_t NID_SD_INIT = 0x50C4CD57u, NID_SD_STATUS = 0x8874DBE0u, NID_SD_SHUTDOWN = 0x9790B33Cu;
constexpr uint32_t SDP_RESULT = 0x1Cu, SDP_MODE = 0x30u, SDP_GAME = 0x3Cu, SDP_SAVE = 0x4Cu, SDP_NAMES = 0x60u,
                   SDP_FILE = 0x64u, SDP_DATASIZE = 0x7Cu;            // SceUtilitySavedataParam
constexpr int32_t SD_STATUS_FINISHED = 3;
constexpr char kSidecarMagic[8] = {'D', 'S', 'R', 'S', 'T', 'R', '0', '1'};
constexpr uint32_t kSidecarHeader = 0x60u, kSidecarEntry = 8u;

struct SidecarRecord { uint8_t charId = 0, like = 0; std::vector<uint8_t> rec; };

uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
uint32_t g32(const uint8_t* rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x07FFFFFFu), 4); return v; }
bool guest_ok(uint32_t a, uint32_t n) { return a >= 0x08000000u && a < 0x0C000000u && n <= 0x0C000000u - a; }   // as the savedata HLE (sd_ram_ok)
std::string guest_cstr(const uint8_t* rdram, uint32_t a, size_t max) {
    std::string s;
    for (size_t i = 0; i < max && guest_ok(a + static_cast<uint32_t>(i), 1); i++) {
        const char c = static_cast<char>(rdram[(a + i) & 0x07FFFFFFu]);
        if (c == 0) break;
        s += c;
    }
    return s;
}
bool read_host_file(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}
bool write_host_file_atomic(const std::string& path, const std::vector<uint8_t>& data) {
    for (size_t i = 1; i < path.size(); i++)              // mkdir -p of the folder (a fresh memory stick has none)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f.flush()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
void put_le(std::vector<uint8_t>& b, size_t at, uint64_t v, int n) { for (int i = 0; i < n; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i)); }
uint64_t get_le(const std::vector<uint8_t>& b, size_t at, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v |= uint64_t(b[at + i]) << (8 * i); return v; }

std::vector<uint8_t> sidecar_encode(uint64_t hash, uint32_t size, const std::string& dir, const std::string& file,
                                    const std::vector<SidecarRecord>& recs) {
    std::vector<uint8_t> b(kSidecarHeader + recs.size() * (kSidecarEntry + kSaveRec) + 8, 0);
    std::memcpy(b.data(), kSidecarMagic, 8);
    put_le(b, 0x08, kSidecarHeader, 4);
    put_le(b, 0x0C, kSaveRec, 4);
    put_le(b, 0x10, hash, 8);
    put_le(b, 0x18, size, 4);
    put_le(b, 0x1C, recs.size(), 4);
    std::memcpy(b.data() + 0x20, dir.data(), std::min<size_t>(dir.size(), 31));
    std::memcpy(b.data() + 0x40, file.data(), std::min<size_t>(file.size(), 15));
    size_t at = kSidecarHeader;
    for (const SidecarRecord& r : recs) {
        b[at] = r.charId;
        b[at + 1] = r.like;
        std::memcpy(b.data() + at + kSidecarEntry, r.rec.data(), kSaveRec);
        at += kSidecarEntry + kSaveRec;
    }
    put_le(b, at, fnv1a64(b.data(), at), 8);
    return b;
}
// "" when the sidecar belongs to the data file with this hash / size (records in *out), else why it cannot be used.
std::string sidecar_decode(const std::vector<uint8_t>& b, uint64_t hash, uint32_t size, std::vector<SidecarRecord>* out) {
    out->clear();
    if (b.size() < kSidecarHeader + 8 || std::memcmp(b.data(), kSidecarMagic, 8) != 0) return "not a sidecar";
    const uint64_t n = get_le(b, 0x1C, 4);
    if (get_le(b, 0x08, 4) != kSidecarHeader || get_le(b, 0x0C, 4) != kSaveRec || n > static_cast<uint64_t>(kExtraSlots) ||
        b.size() != kSidecarHeader + n * (kSidecarEntry + kSaveRec) + 8)
        return "damaged (layout)";
    if (get_le(b, b.size() - 8, 8) != fnv1a64(b.data(), b.size() - 8)) return "damaged (checksum)";
    if (get_le(b, 0x10, 8) != hash || get_le(b, 0x18, 4) != size) return "written for another version of this save";
    for (size_t i = 0, at = kSidecarHeader; i < n; i++, at += kSidecarEntry + kSaveRec) {
        SidecarRecord r;
        r.charId = b[at];
        r.like = b[at + 1];
        r.rec.assign(b.begin() + static_cast<long>(at + kSidecarEntry), b.begin() + static_cast<long>(at + kSidecarEntry + kSaveRec));
        out->push_back(std::move(r));
    }
    return "";
}

// The save folder a finished savedata call used: the slot the game named (the list dialog writes the chosen slot back
// into saveName), else for the list modes the slot of the saveNameList whose data file exists (a save: the newest one).
std::string savedata_dir(const uint8_t* rdram, uint32_t p, const std::string& base, const std::string& file, bool saving) {
    const std::string game = guest_cstr(rdram, p + SDP_GAME, 13), save = guest_cstr(rdram, p + SDP_SAVE, 20);
    auto mtime = [&](const std::string& slot, int64_t* t) {
        struct stat st;
        if (::stat((base + "/" + game + slot + "/" + file).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
        *t = static_cast<int64_t>(st.st_mtime);
        return true;
    };
    int64_t t = 0, best_t = 0;
    if (!save.empty() && mtime(save, &t)) return game + save;
    std::vector<std::string> names;
    const uint32_t list = g32(rdram, p + SDP_NAMES);
    for (uint32_t i = 0; list && i < 99 && guest_ok(list + 20 * i, 20); i++) {
        std::string n = guest_cstr(rdram, list + 20 * i, 20);
        if (n.empty()) break;
        names.push_back(n);
    }
    DIR* d = names.empty() ? ::opendir(base.c_str()) : nullptr;   // no list: every slot of this game
    if (d) {
        while (const dirent* de = ::readdir(d)) {
            const std::string n = de->d_name;
            if (n.size() > game.size() && n.compare(0, game.size(), game) == 0) names.push_back(n.substr(game.size()));
        }
        ::closedir(d);
    }
    std::string best;
    for (const std::string& n : names) {
        if (!mtime(n, &t)) continue;
        if (best.empty() || (saving && t > best_t)) { best = n; best_t = t; }
    }
    return best.empty() ? "" : game + best;
}

void savedata_finished(uint8_t* rdram, uint32_t p) {
    if (!g_enabled || extras().empty() || !guest_ok(p, 0x80)) return;
    const uint32_t mode = g32(rdram, p + SDP_MODE);
    const bool load = mode == 0 || mode == 2 || mode == 4 || mode == 15 || mode == 16;
    const bool save = mode == 1 || mode == 3 || mode == 5 || mode == 13 || mode == 14 || mode == 17 || mode == 18;
    if ((!load && !save) || g32(rdram, p + SDP_RESULT) != 0 || g32(rdram, p + SDP_DATASIZE) != kMainSaveBytes) return;
    const uint32_t code = g32(rdram, GAME_CODE_PTR);
    if (guest_ok(code, 1) && guest_cstr(rdram, p + SDP_GAME, 13) != guest_cstr(rdram, code, 13)) return;   // another game's save
    const std::string file = guest_cstr(rdram, p + SDP_FILE, 13);
    if (file.empty() || file.find('/') != std::string::npos) return;
    const std::string base = psp_path_to_host("ms0:/PSP/SAVEDATA");
    const std::string dir = savedata_dir(rdram, p, base, file, save);
    std::vector<uint8_t> data;
    if (dir.empty() || !read_host_file(base + "/" + dir + "/" + file, &data)) {
        if (load) reset_extra_records();
        std::fprintf(stderr, "[ROSTER] save %s (mode %u): data file not found, extra characters' records not %s\n",
                     dir.empty() ? "?" : dir.c_str(), mode, save ? "saved" : "restored (they follow their like)");
        return;
    }
    const uint64_t hash = fnv1a64(data.data(), data.size());
    const uint32_t size = static_cast<uint32_t>(data.size());
    const std::string side = psp_path_to_host("ms0:/PSP/ROSTER") + "/" + dir + "_" + file + ".roster";
    const uint32_t block = saverec_block();
    std::string who;                                       // "34 (Lv 7), 35 (Lv 3)" for the log line
    auto add_who = [&](int charId, const uint8_t* rec) {
        char s[32];
        std::snprintf(s, sizeof(s), "%s%d (Lv %u)", who.empty() ? "" : ", ", charId, static_cast<unsigned>(rec[1]));
        who += s;
    };
    if (save) {
        std::vector<SidecarRecord> recs;
        for (const Entry* e : extras()) {
            const int slot = e->charId - kFirstNew;
            if (!block || g_saverec_state[slot] != REC_OWN) continue;
            SidecarRecord r;
            r.charId = static_cast<uint8_t>(e->charId);
            r.like = static_cast<uint8_t>(e->like);
            const uint8_t* rec = rdram + ((block + kSaveRec * static_cast<uint32_t>(slot)) & 0x07FFFFFFu);
            r.rec.assign(rec, rec + kSaveRec);
            add_who(e->charId, rec);
            recs.push_back(std::move(r));
        }
        const bool ok = write_host_file_atomic(side, sidecar_encode(hash, size, dir, file, recs));
        std::fprintf(stderr, "[ROSTER] save %s/%s written: %s%s %s %s\n", dir.c_str(), file.c_str(),
                     who.empty() ? "no extra character has a record of its own" : "records of charId ", who.c_str(),
                     ok ? "->" : "could NOT be written to", side.c_str());
        return;
    }
    std::vector<SidecarRecord> recs;
    std::vector<uint8_t> sb;
    const std::string why = read_host_file(side, &sb) ? sidecar_decode(sb, hash, size, &recs) : "no sidecar";
    reset_extra_records();
    if (!why.empty() || !block) {
        std::fprintf(stderr, "[ROSTER] save %s/%s loaded: %s (%s): the extra characters follow their like\n", dir.c_str(), file.c_str(),
                     why.empty() ? "no host memory for the records" : why.c_str(), side.c_str());
        return;
    }
    for (const SidecarRecord& r : recs) {
        const Entry* e = extra_of(r.charId);
        if (!e || r.rec[0] != r.charId) continue;          // no longer in roster.txt / not that character's record
        const uint32_t slot = static_cast<uint32_t>(e->charId - kFirstNew);
        std::memcpy(const_cast<uint8_t*>(rdram) + ((block + kSaveRec * slot) & 0x07FFFFFFu), r.rec.data(), kSaveRec);
        g_saverec_state[slot] = REC_OWN;
        add_who(e->charId, r.rec.data());
    }
    std::fprintf(stderr, "[ROSTER] save %s/%s loaded: %s%s (%s), the other extra characters follow their like\n", dir.c_str(),
                 file.c_str(), who.empty() ? "no extra character records in the sidecar" : "restored the records of charId ",
                 who.c_str(), side.c_str());
}

uint32_t g_sd_param = 0;
bool g_sd_pending = false;
FuncPtr g_orig_sd_init = nullptr, g_orig_sd_status = nullptr, g_orig_sd_shutdown = nullptr, g_orig_party_init = nullptr;
void sd_init_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    g_orig_sd_init(rdram, ctx);
    if (ctx->r[2] == 0) { g_sd_param = p; g_sd_pending = true; }
}
void sd_status_hook(uint8_t* rdram, recomp_context* ctx) {
    g_orig_sd_status(rdram, ctx);
    if (static_cast<int32_t>(ctx->r[2]) == SD_STATUS_FINISHED && g_sd_pending) { g_sd_pending = false; savedata_finished(rdram, g_sd_param); }
}
void sd_shutdown_hook(uint8_t* rdram, recomp_context* ctx) {
    g_orig_sd_shutdown(rdram, ctx);
    if (ctx->r[2] == 0 && g_sd_pending) { g_sd_pending = false; savedata_finished(rdram, g_sd_param); }
}
// FUN_088CB210(party): creates the 31 retail records of a save object (FUN_088AB800: New Game, and the reset of the
// save data before a load). For the live object (*(0x089D712C) + 0xF58) the extras follow their like again.
void party_init_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t party = static_cast<uint32_t>(ctx->r[4]), obj = g32(rdram, SAVE_OBJECT);
    g_orig_party_init(rdram, ctx);
    if (obj != 0 && party != obj + SAVE_PARTY) return;           // a scratch save object
    if (reset_extra_records())
        std::fprintf(stderr, "[ROSTER] save data created anew (New Game): the extra characters follow their like again\n");
}

// Opponent Info (battle overlay FUN_08AF8F58 and FUN_08AF9C6C) shows the CPU's name as a text from the battle
// text bank: FUN_0887B3EC(obj, 1, 0, textId) with textId = table[charId] (0x08AFB6E8, 8 bytes per charId, relocated
// to DISSIDIA_TBL_OPPINFO). The bank has no record for an extra, so the table holds OPPINFO_MARK + charId and this
// hook builds the text from the roster.txt `nm=` label instead (a literal string text, the way "Lv%d" is made).
FuncPtr g_orig_textcreate = nullptr, g_orig_textlit = nullptr;
void oppinfo_name_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t kind = static_cast<uint32_t>(ctx->r[5]), id = static_cast<uint32_t>(ctx->r[7]);
    if (kind == 1 && (id & ~0xFFu) == OPPINFO_MARK && g_orig_textlit) {
        if (const Entry* e = extra_of(id & 0xFFu)) {
            uint32_t pool;
            std::memcpy(&pool, rdram + (static_cast<uint32_t>(ctx->r[4]) & 0x07FFFFFFu), 4);
            if (pool == 0) { ctx->r[2] = 0; return; }
            uint8_t* buf = rdram + (DISSIDIA_OPPINFO_STR & 0x07FFFFFFu);
            std::memset(buf, 0, 2 * 32);
            for (size_t i = 0; i < e->label.size() && i < 30; i++) { buf[2 * i] = static_cast<uint8_t>(e->label[i]); }
            uint32_t tp;
            std::memcpy(&tp, rdram + (0x089D6998u & 0x07FFFFFFu), 4);
            ctx->r[4] = static_cast<int32_t>(tp); ctx->r[5] = 0; ctx->r[6] = static_cast<int32_t>(DISSIDIA_OPPINFO_STR); ctx->r[7] = -1;
            ctx->r[8] = ctx->r[9] = ctx->r[10] = ctx->r[11] = 0;
            g_orig_textlit(rdram, ctx);
            return;
        }
    }
    g_orig_textcreate(rdram, ctx);
}

// FUN_088A3C98(slots, charId) = slots + 12 * charId, the voice slot record accessor (the 35 records live inside a
// fixed struct, so ids >= 35 would read the fields behind it). An extra character uses the voice slot of `like`.
FuncPtr g_orig_voiceslot = nullptr;
void voiceslot_hook(uint8_t* rdram, recomp_context* ctx) {
    if (const Entry* e = extra_of(static_cast<uint32_t>(ctx->r[5]))) ctx->r[5] = e->like;
    g_orig_voiceslot(rdram, ctx);
}

// DEBUG: PSPRECOMP_ROSTER_WATCHCPY=<hex addr>: log memcpy/memset calls whose destination covers addr.
FuncPtr g_orig_memcpy = nullptr, g_orig_memset = nullptr;
uint32_t g_wc_addr = 0;
void wc_log(const char* what, recomp_context* ctx, uint8_t* rdram) {
    uint32_t d = (uint32_t)ctx->r[4], a1 = (uint32_t)ctx->r[5], n = (uint32_t)ctx->r[6];
    if (g_wc_addr >= d && g_wc_addr < d + n) {
        uint32_t w = 0;
        if (a1 >= 0x08000000u && a1 < 0x0C000000u) std::memcpy(&w, rdram + (a1 & 0x07FFFFFFu), 4);
        std::fprintf(stderr, "[ROSTER] WC %s dst=%08X src/val=%08X n=%u src[0]=%08X\n", what, d, a1, n, w);
        psp_dump_func_ring("WC");
    }
}
void wc_memcpy(uint8_t* rdram, recomp_context* ctx) { wc_log("memcpy", ctx, rdram); g_orig_memcpy(rdram, ctx); }
void wc_memset(uint8_t* rdram, recomp_context* ctx) { wc_log("memset", ctx, rdram); g_orig_memset(rdram, ctx); }

// The selectable list (FUN_088D5AC8 into a caller buffer of kListCap entries). The Quick Battle caller
// (0x08A87AA0) keeps its buffer inside the screen struct at +0x974, with live fields right behind it (+0xA8C),
// so at most kListCap - 1 entries are written there. Every entry (retail + extras) is also kept here; the
// Quick Battle grid init (FUN_08A80D8C, grid_init_hook) is handed a longer copy so the 40-cell grid can show them.
uint32_t g_list_buf = 0;                       // caller buffer of the last build
std::vector<std::array<uint8_t, 8>> g_list_full;   // every entry of the last build, in order
uint32_t g_list_written = 0;                   // how many of them are in the caller buffer

void list_build_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t list = static_cast<uint32_t>(ctx->r[4]);
    g_orig_list_build(rdram, ctx);
    uint32_t n = static_cast<uint32_t>(ctx->r[2]);
    std::vector<std::array<uint8_t, 8>> v;
    for (uint32_t i = 0; i < n; i++) {
        std::array<uint8_t, 8> e; std::memcpy(e.data(), rdram + ((list + 8u * i) & 0x07FFFFFFu), 8); v.push_back(e);
    }
    auto id_of = [](const std::array<uint8_t, 8>& e) { uint32_t id; std::memcpy(&id, e.data(), 4); return id; };
    auto have = [&](uint32_t id) { for (auto& e : v) if (id_of(e) == id) return true; return false; };
    // Debug aid PSPRECOMP_ROSTER_UNLOCK_ALL=1: list every playable charId 1..32, to see the full grid.
    static const bool unlock_all = std::getenv("PSPRECOMP_ROSTER_UNLOCK_ALL") != nullptr;
    if (unlock_all && !v.empty()) {
        for (uint32_t id = 1; id <= 32; id++) {
            if (have(id)) continue;
            std::array<uint8_t, 8> e = v[0]; std::memcpy(e.data(), &id, 4); v.push_back(e);
        }
    }
    // Append a clone of `like`'s entry per extra character: that clone is the unlock. When `like` is not
    // selectable (locked on this save) the first entry is cloned, so a new character always shows up.
    for (const Entry* e : extras()) {
        const uint32_t nid = static_cast<uint32_t>(e->charId);
        if (have(nid) || v.empty()) continue;   // a list rebuilt on a live screen gets no duplicates
        std::array<uint8_t, 8> c = v[0];
        for (auto& x : v) if (id_of(x) == static_cast<uint32_t>(e->like)) { c = x; break; }
        std::memcpy(c.data(), &nid, 4);
        v.push_back(c);
    }
    const uint32_t cap = static_cast<uint32_t>(kListCap - 1);
    const uint32_t w = v.size() < cap ? static_cast<uint32_t>(v.size()) : cap;
    for (uint32_t i = 0; i < w; i++) std::memcpy(rdram + ((list + 8u * i) & 0x07FFFFFFu), v[i].data(), 8);
    if (v.size() > cap)
        std::fprintf(stderr, "[ROSTER] selectable list: %zu entries, %u in the caller's buffer, the rest kept for the 40-cell grid\n", v.size(), w);
    g_list_buf = list; g_list_full = v; g_list_written = w;
    ctx->r[2] = w;
}

// ---------------------------------------------------------------------------------------------
// 40-cell Quick Battle grid. The retail grid is a 10 x 4 matrix of cells (cell = row * 10 + col) but only has
// 34 tile sprites: row 1 uses columns 3..8, rows 2/3 all ten, row 4 columns 3..10. When more than 34 entries
// (characters + the "?" tile) are selectable, the layout (FUN_08A84FD4) is replaced by a host version that uses
// all 40 cells: row 1 moves two columns left (cells 0..5) and gains cells 6..9, row 4 gains cells 30/31.
// Characters fill cells 0..5, 10..29, 30.. (row 4, the special tiles keep its last cells), then 6..9.
// Entries 0..33 keep the game's own tile sprites (slot i = tile sheet A[2+i], node H[20+i]); entries 34..39 get
// tile sheets created here with the engine's own functions (same layout definition as A[2]), destroyed with the
// screen (FUN_08A7EB80). Everything that turns a slot into a sprite or a position is wrapped:
//   FUN_08A7FB54 placer (only called by the layout), FUN_08A7F398 node positioning (cursor + highlight use the
//   explicit `14 + slot` reference), FUN_08A810A4 tile selected/unselected animation, FUN_08A7EB80 teardown.
// With 34 or fewer entries nothing changes: the game's own layout runs.
// Grid geometry (read from the retail nodes): tile top-left x = 28 + 32 * col, y = 113 + 39 * row.
namespace {
constexpr int kGridCells = 40;
constexpr int kGameSlots = 34;                       // tile sprites the layout has
constexpr int kHostSlots = kGridCells - kGameSlots;  // tile sprites made here
constexpr int kMaxChars = 39;                        // characters handed to the 40-cell grid (+ the special tiles; extra characters are dropped)
constexpr uint32_t FN_GRID_INIT = 0x08A80D8Cu, FN_LAYOUT = 0x08A84FD4u, FN_POSNODE = 0x08A7F398u, FN_SELANIM = 0x08A810A4u,
                   FN_TEARDOWN = 0x08A7EB80u, FN_AFTERPLACE = 0x08A80130u, FN_LAYOUT_TAIL = 0x08A81240u, FN_RECCHECK = 0x08A80A4Cu,
                   FN_CELL_OF = 0x08A7F2B4u, FN_DEFCURSOR = 0x08A812FCu;
constexpr uint32_t EF_NODE_NEW = 0x088DAA40u, EF_NODE_FREE = 0x088DA060u, EF_NODE_SHOW = 0x088D9CD8u, EF_NODE_HIDE = 0x088D9CFCu,
                   EF_NODE_ANIM = 0x088D9F5Cu, EF_SHEET_NEW = 0x088D107Cu, EF_SHEET_FREE = 0x088DAB44u, EF_BIND = 0x088D0984u,
                   EF_FLAG10 = 0x08900E60u, EF_SHEET_DEF = 0x088DA8E8u;
constexpr uint32_t G_HEAP = 0x089D7830u, G_FLAG10_ARG = 0x089D8140u, SEL_ANIMS = 0x08A85B70u, CENTER_TABLE = 0x08A85BF8u;
FuncPtr g_grid_init = nullptr, g_layout = nullptr, g_posnode = nullptr, g_selanim = nullptr, g_teardown = nullptr,
        g_afterplace = nullptr, g_layout_tail = nullptr, g_reccheck = nullptr, g_cell_of = nullptr, g_defcursor = nullptr;
FuncPtr g_node_new = nullptr, g_node_free = nullptr, g_node_show = nullptr, g_node_hide = nullptr, g_node_anim = nullptr,
        g_sheet_new = nullptr, g_sheet_free = nullptr, g_bind = nullptr, g_flag10 = nullptr, g_sheet_def = nullptr;
uint32_t g_list_block = 0;                     // longer copy of the selectable list for the grid
struct Grid40 {
    uint32_t obj = 0;                          // screen object whose layout is the 40-cell one (0 = none)
    uint32_t sheet[kHostSlots] = {}, node[kHostSlots] = {};
    int cell_of_slot[kGridCells];
} g40;
bool g40_log = false;

inline uint32_t gw32(uint8_t* r, uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & 0x07FFFFFFu), 4); return v; }
inline int16_t gr16(uint8_t* r, uint32_t a) { int16_t v; std::memcpy(&v, r + (a & 0x07FFFFFFu), 2); return v; }
inline void gs16(uint8_t* r, uint32_t a, int16_t v) { std::memcpy(r + (a & 0x07FFFFFFu), &v, 2); }
inline void gs32(uint8_t* r, uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x07FFFFFFu), &v, 4); }
inline void gsf(uint8_t* r, uint32_t a, float v) { std::memcpy(r + (a & 0x07FFFFFFu), &v, 4); }
uint32_t gcall(FuncPtr f, uint8_t* rdram, recomp_context* ctx, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
    const recomp_context save = *ctx;
    ctx->r[4] = static_cast<int32_t>(a0); ctx->r[5] = static_cast<int32_t>(a1);
    ctx->r[6] = static_cast<int32_t>(a2); ctx->r[7] = static_cast<int32_t>(a3);
    f(rdram, ctx);
    const uint32_t v = static_cast<uint32_t>(ctx->r[2]);
    *ctx = save;
    return v;
}
inline float cell_x(int cell) { return 28.0f + 32.0f * static_cast<float>(cell % 10); }
inline float cell_y(int cell) { return 113.0f + 39.0f * static_cast<float>(cell / 10); }
// Retail slot -> cell (FUN_08A7FB54): +2, +2 more from slot 6, +2 more from slot 26.
inline int retail_cell(int slot) { return slot + 2 + (slot >= 6 ? 2 : 0) + (slot >= 26 ? 2 : 0); }
uint32_t S_of(uint8_t* r, uint32_t obj) { return gw32(r, obj + 0x28); }
uint32_t H_of(uint8_t* r, uint32_t obj, int i) { return gw32(r, S_of(r, obj) + 0x408u + 4u * static_cast<uint32_t>(i)); }
uint32_t tile_node(uint8_t* r, uint32_t obj, int slot) {
    if (slot >= 0 && slot < kGameSlots) return H_of(r, obj, 20 + slot);
    if (slot >= kGameSlots && slot < kGridCells && g40.obj == obj) return g40.node[slot - kGameSlots];
    return 0;
}
void set_pos(uint8_t* r, uint32_t node, int cell) {
    if (!node) return;
    gsf(r, node + 0x10, cell_x(cell)); gsf(r, node + 0x14, cell_y(cell));
}
// The cursor is S+0x608 (column) / S+0x60C (row); S+0x600 is the slot under it (the hand and highlight are drawn
// from that slot, FUN_08A7F398 reference 14 + slot) and S+0x5FC that cell's record. The game hard-codes the
// default cursor as column 2, row 0 (grid init FUN_08A81BB8 and FUN_08A812FC): slot 0's retail cell. On the
// 40-cell grid that cell holds slot 2, so the hand showed slot 0 (Lightning) while confirm took cell 2 (Laguna).
// Put column/row back on the cell of `slot`.
void cursor_to_slot(uint8_t* r, uint32_t obj, int slot) {
    if (slot < 0 || slot >= kGridCells) return;
    const int cell = g40.cell_of_slot[slot];
    if (cell < 0) return;
    const uint32_t S = S_of(r, obj);
    gs32(r, S + 0x608, static_cast<uint32_t>(cell % 10));
    gs32(r, S + 0x60C, static_cast<uint32_t>(cell / 10));
}

void host_teardown(uint8_t* rdram, recomp_context* ctx) {
    for (int j = 0; j < kHostSlots; j++) {
        if (g40.node[j]) gcall(g_node_free, rdram, ctx, g40.node[j]);
        if (g40.sheet[j]) gcall(g_sheet_free, rdram, ctx, g40.sheet[j]);
        g40.node[j] = g40.sheet[j] = 0;
    }
    g40.obj = 0;
}

// Tile sheet + node for host slot j (34 + j), made like the layout's own: sheet with the tile layout definition
// of A[2], node from quad 0, attributes (scale, colour, flags) copied from the game's node of slot 0, hidden.
bool host_slot(uint8_t* rdram, recomp_context* ctx, uint32_t obj, int j) {
    if (g40.node[j]) return true;
    const uint32_t res = gw32(rdram, obj + 0x1C), A = gw32(rdram, res), base = static_cast<uint16_t>(gr16(rdram, res + 6));
    const uint32_t tile0 = gw32(rdram, A + 4u * (base + 2u)), def = gw32(rdram, tile0 + 0xC);
    const uint32_t heap = gw32(rdram, G_HEAP);
    // FUN_088D107C makes a sheet from the heap's pool (callback form: no callback here); FUN_088DA8E8 then gives it
    // the tile's layout definition, which is what the layout's own sheets (FUN_088D0700) get.
    const uint32_t sh = gcall(g_sheet_new, rdram, ctx, heap, 0, 0);
    if (!sh) return false;
    gcall(g_sheet_def, rdram, ctx, sh, def);
    rdram[(sh + 0x17) & 0x07FFFFFFu] = 0;
    rdram[(sh + 0x14) & 0x07FFFFFFu] |= 1;
    // The heap's sheet list (head +0x24, tail +0x28; sheet +0x24 next, +0x28 prev) is drawn in order and the pool
    // put the new sheet at its head, under the screen's background: move it behind the last tile sheet.
    {
        const uint32_t after = gw32(rdram, A + 4u * (base + 2u + static_cast<uint32_t>(kGameSlots - 1 + j)));
        const uint32_t prev = gw32(rdram, sh + 0x28), next = gw32(rdram, sh + 0x24);
        if (prev) gs32(rdram, prev + 0x24, next); else gs32(rdram, heap + 0x24, next);
        if (next) gs32(rdram, next + 0x28, prev); else gs32(rdram, heap + 0x28, prev);
        const uint32_t t = j == 0 ? after : g40.sheet[j - 1];
        const uint32_t tn = gw32(rdram, t + 0x24);
        gs32(rdram, sh + 0x28, t); gs32(rdram, sh + 0x24, tn); gs32(rdram, t + 0x24, sh);
        if (tn) gs32(rdram, tn + 0x28, sh); else gs32(rdram, heap + 0x28, sh);
    }
    const uint32_t nd = gcall(g_node_new, rdram, ctx, sh, 0, 0);
    if (!nd) { gcall(g_sheet_free, rdram, ctx, sh); return false; }
    const uint32_t ref = H_of(rdram, obj, 20);
    if (ref) {   // depth, position, scale and colour like the game's tile nodes (+0x20 flags, +0x24 owner sheet and the links stay)
        std::memcpy(rdram + (nd & 0x07FFFFFFu), rdram + (ref & 0x07FFFFFFu), 4);
        std::memcpy(rdram + ((nd + 0x10) & 0x07FFFFFFu), rdram + ((ref + 0x10) & 0x07FFFFFFu), 0x10);
        std::memcpy(rdram + ((nd + 0x28) & 0x07FFFFFFu), rdram + ((ref + 0x28) & 0x07FFFFFFu), 4);
        std::memcpy(rdram + ((nd + 0x44) & 0x07FFFFFFu), rdram + ((ref + 0x44) & 0x07FFFFFFu), 4);
    }
    gcall(g_node_hide, rdram, ctx, nd);
    g40.sheet[j] = sh; g40.node[j] = nd;
    if (g40_log) std::fprintf(stderr, "[ROSTER] grid40: host slot %d: sheet %08X (def %08X, like A[2] %08X) node %08X\n", kGameSlots + j, sh, def, tile0, nd);
    return true;
}

// The placer's job for a host slot (see FUN_08A7FB54): cell record + tile image.
void host_place(uint8_t* rdram, recomp_context* ctx, uint32_t obj, int rec, int slot, int cell) {
    const int j = slot - kGameSlots;
    if (!host_slot(rdram, ctx, obj, j)) return;
    const uint32_t S = S_of(rdram, obj), c = S + 0x678u + 8u * static_cast<uint32_t>(cell);
    int f;
    if (rec == -2) f = -1; else if (rec == -3) f = -3; else if (rec == 0x20) f = -2;
    else f = gr16(rdram, GRID_TABLE_DST + 12u * static_cast<uint32_t>(rec));
    gs16(rdram, c + 0, static_cast<int16_t>(gr16(rdram, c) | 1));
    gs16(rdram, c + 4, static_cast<int16_t>(rec));
    gs16(rdram, c + 2, static_cast<int16_t>(slot));
    const uint32_t res = gw32(rdram, obj + 0x1C);
    const uint32_t frame = (static_cast<uint32_t>(static_cast<uint16_t>(gr16(rdram, res + 0x36))) + static_cast<uint32_t>(f)) & 0xFFFFu;
    gcall(g_bind, rdram, ctx, gw32(rdram, G_HEAP), g40.sheet[j], gw32(rdram, obj + 8) & 0xFFu, frame);
    set_pos(rdram, g40.node[j], cell);
}

// Host version of FUN_08A84FD4(obj, mode) for more than 34 entries; returns false (nothing done) for 34 or fewer.
bool layout40(uint8_t* rdram, recomp_context* ctx, uint32_t obj, int mode) {
    const uint32_t S = S_of(rdram, obj), listp = gw32(rdram, obj + 0x20), listn = gw32(rdram, obj + 0x14);
    int16_t ent[kGridCells + 4]; int k = 0;
    gcall(grid_table_hook, rdram, ctx, 0);                // (re)builds the record table at GRID_TABLE_DST, as the layout's own call does
    const uint8_t flag10 = static_cast<uint8_t>(gcall(g_flag10, rdram, ctx, gw32(rdram, G_FLAG10_ARG)));
    for (int r = 0; r < kGridRows && k < kGridCells; r++) {
        const int cid = gr16(rdram, GRID_TABLE_DST + 12u * static_cast<uint32_t>(r) + 8u);
        for (uint32_t i = 0; i < listn; i++) {
            if (static_cast<int>(gw32(rdram, listp + 8u * i)) != cid) continue;
            if (gcall(g_reccheck, rdram, ctx, obj, static_cast<uint32_t>(cid)) == 0) { ent[k++] = static_cast<int16_t>(r); break; }
        }
    }
    if (k == 0) ent[k++] = 0;
    int nchars = k;
    if (mode == 1) ent[k++] = -2;
    else if (mode == 0) { if (!flag10) { ent[k++] = -2; if (gw32(rdram, obj + 0xC) == 1) ent[k++] = -3; } }
    else if (mode == 2) ent[k++] = 0x20;
    else if (mode == 5) {
        if (gw32(rdram, obj + 4) != 0xC && rdram[(gw32(rdram, obj + 0x24) + 0xC8) & 0x07FFFFFFu] == 0) { ent[k++] = -2; ent[k++] = -3; }
    }
    if (k > kGridCells) {                                // keep the special tiles: drop characters from the end
        const int nspec0 = k - nchars, keep = kGridCells - nspec0;
        for (int s = 0; s < nspec0; s++) ent[keep + s] = ent[nchars + s];
        nchars = keep; k = kGridCells;
    }
    if (g40_log) std::fprintf(stderr, "[ROSTER] grid40: layout obj %08X mode %d: %d characters, %d entries (list %u)\n", obj, mode, nchars, k, listn);
    if (k <= kGameSlots) return false;                   // the retail layout fits: let the game do it
    // From here on the layout is ours: same state as the game's prologue (cells and entry list cleared).
    for (int c = 0; c < kGridCells; c++) {
        const uint32_t a = S + 0x678u + 8u * static_cast<uint32_t>(c);
        gs16(rdram, a, 0); gs16(rdram, a + 4, -1); gs16(rdram, a + 2, -1); gs16(rdram, a + 6, 0);
        gs16(rdram, S + 0x628u + 2u * static_cast<uint32_t>(c), -1);
    }
    for (int i = 0; i < k; i++) gs16(rdram, S + 0x628u + 2u * static_cast<uint32_t>(i), ent[i]);
    gs32(rdram, S + 0x610, static_cast<uint32_t>(nchars));   // (the game stores the character count before the specials)
    if (g40.obj && g40.obj != obj) host_teardown(rdram, ctx);
    g40.obj = obj;
    for (int j = 0; j < kHostSlots; j++) if (g40.node[j]) gcall(g_node_hide, rdram, ctx, g40.node[j]);
    // Cells: characters 0..5, 10..29, 30..(39 - specials), 6..9; specials on the last cells of row 4.
    const int nspec = k - nchars;
    std::vector<int> order;
    for (int c = 0; c <= 5; c++) order.push_back(c);
    for (int c = 10; c <= 29; c++) order.push_back(c);
    for (int c = 30; c <= 39 - nspec; c++) order.push_back(c);
    for (int c = 6; c <= 9; c++) order.push_back(c);
    int cell_of_entry[kGridCells];
    for (int i = 0; i < nchars; i++) cell_of_entry[i] = order[static_cast<size_t>(i)];
    for (int s = 0; s < nspec; s++) cell_of_entry[nchars + s] = 40 - nspec + s;
    for (int i = 0; i < kGridCells; i++) g40.cell_of_slot[i] = -1;
    // Game slots: the game's placer writes the retail cell (all distinct for 0..33); the records are moved after.
    struct Rec { int16_t w[4]; } moved[kGameSlots];
    const int ng = k < kGameSlots ? k : kGameSlots;
    for (int i = 0; i < ng; i++) gcall(placer_wrap, rdram, ctx, obj, static_cast<uint32_t>(static_cast<int32_t>(ent[i])), static_cast<uint32_t>(i));
    for (int i = 0; i < ng; i++) {
        const uint32_t a = S + 0x678u + 8u * static_cast<uint32_t>(retail_cell(i));
        for (int w = 0; w < 4; w++) moved[i].w[w] = gr16(rdram, a + 2u * static_cast<uint32_t>(w));
    }
    for (int c = 0; c < kGridCells; c++) {
        const uint32_t a = S + 0x678u + 8u * static_cast<uint32_t>(c);
        gs16(rdram, a, 0); gs16(rdram, a + 4, -1); gs16(rdram, a + 2, -1); gs16(rdram, a + 6, 0);
    }
    for (int i = 0; i < ng; i++) {
        const int cell = cell_of_entry[i];
        const uint32_t a = S + 0x678u + 8u * static_cast<uint32_t>(cell);
        for (int w = 0; w < 4; w++) gs16(rdram, a + 2u * static_cast<uint32_t>(w), moved[i].w[w]);
        g40.cell_of_slot[i] = cell;
        set_pos(rdram, H_of(rdram, obj, 20 + i), cell);
        if (i < 32) set_pos(rdram, H_of(rdram, obj, 59 + i), cell);     // backdrop (slots 0..31 have one)
    }
    for (int i = kGameSlots; i < k; i++) {
        host_place(rdram, ctx, obj, ent[i], i, cell_of_entry[i]);
        g40.cell_of_slot[i] = cell_of_entry[i];
    }
    gcall(g_afterplace, rdram, ctx, obj);
    for (int c = 0; c < kGridCells; c++) {                // the layout's show loop
        const uint32_t a = S + 0x678u + 8u * static_cast<uint32_t>(c);
        const uint16_t fl = static_cast<uint16_t>(gr16(rdram, a));
        if (!(fl & 1)) continue;
        const int slot = gr16(rdram, a + 2);
        if (const uint32_t n = tile_node(rdram, obj, slot)) gcall(g_node_show, rdram, ctx, n);
        if ((fl & 2) && slot >= 0 && slot < 32) gcall(g_node_show, rdram, ctx, H_of(rdram, obj, 59 + slot));
    }
    if (const uint32_t t = gw32(rdram, obj + 0x18)) gcall(g_layout_tail, rdram, ctx, obj, t);
    // Nothing preselected (obj+0x18 == 0): the cursor is still the init's column 2 / row 0 with slot 0 under the
    // hand. Line column/row up with that slot (a no-op after the tail, which sets all three from one cell).
    cursor_to_slot(rdram, obj, static_cast<int>(gw32(rdram, S + 0x600)));
    if (g40_log)
        std::fprintf(stderr, "[ROSTER] grid40: obj %08X mode %d: %d characters + %d special tile(s) on 40 cells\n", obj, mode, nchars, nspec);
    return true;
}

void layout_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    const int mode = static_cast<int>(ctx->r[5]);
    if (layout40(rdram, ctx, obj, mode)) return;
    if (g40.obj == obj) host_teardown(rdram, ctx);       // back to <= 34 entries: retail layout
    g_layout(rdram, ctx);
}

// FUN_08A7F398(obj, node index, reference): with an explicit reference 14 + slot (cursor, highlight) on the
// 40-cell grid the node goes to that slot's cell. Run with slot 0's reference (scale/colour), then move.
void posnode_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    const int h = static_cast<int>(ctx->r[5]), ref = static_cast<int>(ctx->r[6]);
    if (g40.obj != obj || ref < 14 || ref >= 14 + kGridCells || g40.cell_of_slot[ref - 14] < 0) { g_posnode(rdram, ctx); return; }
    const int cell = g40.cell_of_slot[ref - 14];
    ctx->r[6] = 14;
    g_posnode(rdram, ctx);
    if (h >= 0 && h < 98) set_pos(rdram, H_of(rdram, obj, h), cell);
}

// FUN_08A812FC(obj): default cursor (first visit of a side, e.g. the opponent's grid with nobody picked yet).
// The game sets column 2 / row 0 and reads that cell's record and slot; on the 40-cell grid use the first tile's cell.
void defcursor_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    g_defcursor(rdram, ctx);
    if (g40.obj != obj || g40.cell_of_slot[0] < 0) return;
    cursor_to_slot(rdram, obj, 0);
    const uint32_t S = S_of(rdram, obj), c = S + 0x678u + 8u * static_cast<uint32_t>(g40.cell_of_slot[0]);
    gs32(rdram, S + 0x5FC, static_cast<uint32_t>(static_cast<int32_t>(gr16(rdram, c + 4))));
    gs32(rdram, S + 0x600, static_cast<uint32_t>(static_cast<int32_t>(gr16(rdram, c + 2))));
    if (g40_log) std::fprintf(stderr, "[ROSTER] grid40: default cursor -> cell %d (slot 0)\n", g40.cell_of_slot[0]);
}

// FUN_08A810A4(obj, on, charId): selected / unselected animation of a tile; host slots animate their own node.
void selanim_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]);
    if (g40.obj != obj) { g_selanim(rdram, ctx); return; }
    const uint32_t on = static_cast<uint32_t>(ctx->r[5]) & 0xFFu, who = static_cast<uint32_t>(ctx->r[6]);
    const int cell = static_cast<int>(gcall(g_cell_of, rdram, ctx, obj, who));
    if (cell < 0 || cell >= kGridCells) { g_selanim(rdram, ctx); return; }
    const uint32_t a = S_of(rdram, obj) + 0x678u + 8u * static_cast<uint32_t>(cell);
    const int slot = gr16(rdram, a + 2);
    if (slot < kGameSlots) { g_selanim(rdram, ctx); return; }
    uint16_t fl = static_cast<uint16_t>(gr16(rdram, a));
    fl = on ? static_cast<uint16_t>(fl | 4) : static_cast<uint16_t>(fl & ~4u);
    gs16(rdram, a, static_cast<int16_t>(fl));
    const uint32_t anim = gw32(rdram, SEL_ANIMS + (on ? 4u : 0u));
    if (const uint32_t n = tile_node(rdram, obj, slot)) gcall(g_node_anim, rdram, ctx, n, anim, 1);
    ctx->r[2] = 1;
}

void teardown_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g40.obj == static_cast<uint32_t>(ctx->r[4])) host_teardown(rdram, ctx);
    g_teardown(rdram, ctx);
}

// FUN_08A80D8C(..., t0 = list, t1 = count): the Quick Battle grid init. Hand it the whole selectable list.
void grid_init_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t list = static_cast<uint32_t>(ctx->r[8]), n = static_cast<uint32_t>(ctx->r[9]);
    if (list == g_list_buf && n == g_list_written && g_list_full.size() > n) {
        if (!g_list_block) g_list_block = psp_kmem_bump_alloc_named(0x200, "ROSTER_GRID_LIST");
        if (g_list_block) {
            const uint32_t total = g_list_full.size() < static_cast<size_t>(kMaxChars) ? static_cast<uint32_t>(g_list_full.size()) : kMaxChars;
            std::memcpy(rdram + (g_list_block & 0x07FFFFFFu), rdram + (list & 0x07FFFFFFu), 8u * n);   // the caller may have edited these
            for (uint32_t i = n; i < total; i++) std::memcpy(rdram + ((g_list_block + 8u * i) & 0x07FFFFFFu), g_list_full[i].data(), 8);
            ctx->r[8] = static_cast<int32_t>(g_list_block);
            ctx->r[9] = static_cast<int32_t>(total);
            if (g40_log) std::fprintf(stderr, "[ROSTER] grid40: grid init gets %u list entries (buffer holds %u)\n", total, n);
        }
    }
    g_grid_init(rdram, ctx);
}

void grid40_install() {
    if (std::getenv("PSPRECOMP_GRID40_OFF")) return;
    g40_log = std::getenv("PSPRECOMP_GRID40_LOG") != nullptr;
    g_grid_init = ovl_menu_lookup(FN_GRID_INIT); g_layout = ovl_menu_lookup(FN_LAYOUT); g_posnode = ovl_menu_lookup(FN_POSNODE);
    g_selanim = ovl_menu_lookup(FN_SELANIM); g_teardown = ovl_menu_lookup(FN_TEARDOWN); g_afterplace = ovl_menu_lookup(FN_AFTERPLACE);
    g_layout_tail = ovl_menu_lookup(FN_LAYOUT_TAIL); g_reccheck = ovl_menu_lookup(FN_RECCHECK); g_cell_of = ovl_menu_lookup(FN_CELL_OF);
    g_defcursor = ovl_menu_lookup(FN_DEFCURSOR);
    g_node_new = psp_dispatch_probe_lookup(EF_NODE_NEW); g_node_free = psp_dispatch_probe_lookup(EF_NODE_FREE);
    g_node_show = psp_dispatch_probe_lookup(EF_NODE_SHOW); g_node_hide = psp_dispatch_probe_lookup(EF_NODE_HIDE);
    g_node_anim = psp_dispatch_probe_lookup(EF_NODE_ANIM); g_sheet_new = psp_dispatch_probe_lookup(EF_SHEET_NEW);
    g_sheet_free = psp_dispatch_probe_lookup(EF_SHEET_FREE); g_bind = psp_dispatch_probe_lookup(EF_BIND);
    g_flag10 = psp_dispatch_probe_lookup(EF_FLAG10); g_sheet_def = psp_dispatch_probe_lookup(EF_SHEET_DEF);
    const FuncPtr need[] = { g_grid_init, g_layout, g_posnode, g_selanim, g_teardown, g_afterplace, g_layout_tail, g_reccheck, g_cell_of, g_defcursor,
                             g_node_new, g_node_free, g_node_show, g_node_hide, g_node_anim, g_sheet_new, g_sheet_free, g_bind, g_flag10,
                             g_sheet_def, g_orig_placer };
    for (FuncPtr f : need)
        if (!f) { std::fprintf(stderr, "[ROSTER] 40-cell grid NOT installed (a function was not found)\n"); return; }
    psp_dispatch_register(FN_GRID_INIT, grid_init_hook);
    psp_dispatch_register(FN_LAYOUT, layout_hook);
    psp_dispatch_register(FN_POSNODE, posnode_hook);
    psp_dispatch_register(FN_SELANIM, selanim_hook);
    psp_dispatch_register(FN_DEFCURSOR, defcursor_hook);
    psp_dispatch_register(FN_TEARDOWN, teardown_hook);
    std::fprintf(stderr, "[ROSTER] 40-cell Quick Battle grid installed (used when more than %d tiles are listed)\n", kGameSlots);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Battle setup panel ("Enter Battle" screen, menu overlay 0x08AA43EC / 0x08AA44AC / 0x08AA46DC / 0x08AA6148).
// It does not read the grid records: each side binds the big portrait to entity 0x700B + charId and the
// name plate to entity 0x704E + charId (arithmetic on the entity ids of the three free-battle packs).
// For charId 33/34 that lands on placeholder tiles (0x702C/0x702D) and on the first FF logos (0x706F/0x7070),
// which is the "blank portrait + wrong plate" the screen showed for Noctis. FUN_088D0984 binds an entity to
// a sprite; when it is called from those binder functions the entity of an extra character is replaced with
// the portrait / plate entity from its roster.txt line (art= / name=).
// Sides: 0x08AA44AC binds the player's portrait (sprite slot +0, charId from its argument) and is the left-hand
// side; 0x08AA6148, called right after it, binds the same portrait and plate to slots +1 / +4: the layer drawn
// on top at full strength, as far as the code reads (slot +0 looks like the faded copy under it: with only 44AC hooked an extra character's
// portrait looked washed out: 19 Oct). 0x08AA43EC / 0x08AA46DC bind the opponent's (slot +8, charId from the
// opponent settings at panel +1808).
// The game draws both with the same texture and does not mirror either, so a portrait that faces left looks away
// from the opponent on the player's side: roster.txt artl=N / artr=N give the player's (left) / the opponent's (right)
// side another portrait entry, a mirrored copy made by new_character.py (--art-faces) or gim_encode.py mirror.
// Everything else (the grid slide-in) keeps art=.
FuncPtr g_orig_setframe = nullptr;
int g_binder_depth = 0;   // > 0 while one of the setup panel binder functions is running
int g_binder_which = -1;  // index into the binder table of the innermost one running (1 = the player's side)
FuncPtr g_orig_binder[4] = {nullptr, nullptr, nullptr, nullptr};
template <int N> void binder_wrap(uint8_t* rdram, recomp_context* ctx) {
    const int prev = g_binder_which;
    g_binder_depth++;
    g_binder_which = N;
    g_orig_binder[N](rdram, ctx);
    g_binder_which = prev;
    g_binder_depth--;
}
bool in_setup_binder() { return g_binder_depth > 0; }
void setframe_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ent = static_cast<uint32_t>(ctx->r[7]) & 0xFFFFu;
    const uint32_t kArt0 = 0x700Bu + kFirstNew, kPlate0 = 0x704Eu + kFirstNew;   // entities of charId 34
    bool art = ent >= kArt0 && ent < 0x700Bu + kRosterMax, plate = ent >= kPlate0 && ent < 0x704Eu + kRosterMax;
    static const bool dbg0 = std::getenv("PSPRECOMP_ROSTER_DEBUG") != nullptr;
    if (dbg0 && (art || plate)) {
        std::fprintf(stderr, "[ROSTER] setframe entity 0x%04X (%s) binder=%d ring:", ent, art ? "art" : "plate", in_setup_binder() ? 1 : 0);
        for (int k = 1; k <= 32; k++) std::fprintf(stderr, " %08X", g_func_ring[(g_func_ring_pos - k) & 31u]);
        std::fprintf(stderr, "\n");
    }
    if ((art || plate) && in_setup_binder()) {
        int c = static_cast<int>(art ? ent - 0x700Bu : ent - 0x704Eu);
        for (const Entry& e : g_entries) {
            if (e.charId != c) continue;
            const bool player_side = g_binder_which == 1 || g_binder_which == 3;
            const int side_art = player_side ? e.artl : e.artr;
            int idx = art ? (side_art >= 0 ? side_art : e.art) : e.name;
            if (idx < 0) break;
            uint32_t ne = (art ? 0x700Cu : 0x704Fu) + static_cast<uint32_t>(idx);
            static const bool dbg = std::getenv("PSPRECOMP_ROSTER_DEBUG") != nullptr;
            if (dbg) std::fprintf(stderr, "[ROSTER] setup %s charId %d (%s side): entity 0x%04X -> 0x%04X\n", art ? "portrait" : "plate", c,
                                  player_side ? "player" : "opponent", ent, ne);
            ctx->r[7] = ne;
            break;
        }
    }
    g_orig_setframe(rdram, ctx);
}

// DEBUG: PSPRECOMP_ROSTER_TRACE=<hex addr>[,..] logs the arguments of overlay functions (and 0x60 bytes behind every
// pointer-like argument) each time they are entered; the call goes to the original of whichever overlay was streamed last.
FuncPtr g_trace_menu[8] = {}, g_trace_batt[8] = {};
template <int N> void trace_fn(uint8_t* rdram, recomp_context* ctx);
// PSPRECOMP_ROSTER_TRACE_LEN=<words>: how many words behind each pointer argument are dumped (default 0x18).
int trace_words() {
    static const int n = std::getenv("PSPRECOMP_ROSTER_TRACE_LEN") ? static_cast<int>(std::strtoul(std::getenv("PSPRECOMP_ROSTER_TRACE_LEN"), nullptr, 16)) : 0x18;
    return n;
}
uint32_t g_trace_addr[8] = {};
template <int N> void trace_fn(uint8_t* rdram, recomp_context* ctx) {
    // PSPRECOMP_ROSTER_TRACE_GATE=1: only log while the guest byte at 0x08701F00 is non-zero
    // (set it live with the debug socket: `W 0x08701F00 01`), so one screen can be traced out of a long run.
    static const bool gated = std::getenv("PSPRECOMP_ROSTER_TRACE_GATE") != nullptr;
    const bool quiet = gated && rdram[DISSIDIA_GATE & 0x07FFFFFFu] == 0;
    FuncPtr fq = (g_cur_ovl == MOD_BATTLE && g_trace_batt[N]) ? g_trace_batt[N] : g_trace_menu[N] ? g_trace_menu[N] : g_trace_batt[N];
    if (quiet) { fq(rdram, ctx); return; }
    std::fprintf(stderr, "[RTRACE] %08X ovl=%d a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X ra=%08X\n", g_trace_addr[N], g_cur_ovl,
                 (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6], (uint32_t)ctx->r[7], (uint32_t)ctx->r[8], (uint32_t)ctx->r[9], (uint32_t)ctx->r[31]);
    for (int reg : {4, 5, 6, 7, 8, 9}) {
        uint32_t p = (uint32_t)ctx->r[reg];
        if (p < 0x08000000u || p >= 0x0C000000u) continue;
        std::fprintf(stderr, "[RTRACE]   r%d:", reg);
        for (int k = 0; k < trace_words(); k++) { uint32_t w; std::memcpy(&w, rdram + (p & 0x07FFFFFFu) + 4 * k, 4); std::fprintf(stderr, " %08X", w); }
        std::fprintf(stderr, "\n");
        char txt[65]; int n = 0;
        for (; n < 64; n++) { char c = static_cast<char>(rdram[(p & 0x07FFFFFFu) + n]); if (c < 32 || c > 126) break; txt[n] = c; }
        txt[n] = 0;
        if (n >= 4) std::fprintf(stderr, "[RTRACE]   r%d text: %s\n", reg, txt);
    }
    if (std::getenv("PSPRECOMP_ROSTER_TRACE_RING")) {   // callers (the recompiled code keeps no usable $ra)
        std::fprintf(stderr, "[RTRACE]   ring:");
        for (int k = 1; k <= 12; k++) std::fprintf(stderr, " %08X", g_func_ring[(g_func_ring_pos - k) & 31u]);
        std::fprintf(stderr, "\n");
    }
    FuncPtr f = (g_cur_ovl == MOD_BATTLE && g_trace_batt[N]) ? g_trace_batt[N] : g_trace_menu[N] ? g_trace_menu[N] : g_trace_batt[N];
    // PSPRECOMP_ROSTER_TRACE_POST=1: after the call, log v0 and the words behind a0 (out parameters) and behind v0 (a returned record)
    static const bool post = std::getenv("PSPRECOMP_ROSTER_TRACE_POST") != nullptr;
    const uint32_t a0 = static_cast<uint32_t>(ctx->r[4]);
    f(rdram, ctx);
    if (post) {
        std::fprintf(stderr, "[RTRACE]   ret %08X v0=%08X v1=%08X\n", g_trace_addr[N], (uint32_t)ctx->r[2], (uint32_t)ctx->r[3]);
        uint32_t first = 0;      // an out parameter usually starts with a pointer to the record: dump that too
        if (a0 >= 0x08000000u && a0 < 0x0C000000u) std::memcpy(&first, rdram + (a0 & 0x07FFFFFFu), 4);
        for (uint32_t p : {a0, static_cast<uint32_t>(ctx->r[2]), first}) {
            if (p < 0x08000000u || p >= 0x0C000000u) continue;
            std::fprintf(stderr, "[RTRACE]   post @%08X:", p);
            for (int k = 0; k < trace_words(); k++) { uint32_t w; std::memcpy(&w, rdram + (p & 0x07FFFFFFu) + 4 * k, 4); std::fprintf(stderr, " %08X", w); }
            std::fprintf(stderr, "\n");
        }
    }
}
void trace_install() {
    const char* e = std::getenv("PSPRECOMP_ROSTER_TRACE");
    if (!e) return;
    static const FuncPtr fns[8] = { trace_fn<0>, trace_fn<1>, trace_fn<2>, trace_fn<3>, trace_fn<4>, trace_fn<5>, trace_fn<6>, trace_fn<7> };
    int n = 0;
    for (const char* p = e; *p && n < 8;) {
        uint32_t a = static_cast<uint32_t>(std::strtoul(p, nullptr, 16));
        g_trace_addr[n] = a;
        if (a < 0x089DD4C0u || a >= 0x08B63170u) {
            // Below the overlay window: the main table entry (EBOOT function) is the original. The menu overlay's generated set also
            // holds a copy of many of these functions (ovl_menu_func_0887B3EC_entry ...); using that copy as the "original" of a
            // wrapped EBOOT address is what broke the first trace of 0x0887B3EC (menu came up without text, then hung).
            g_trace_menu[n] = g_trace_batt[n] = psp_dispatch_probe_lookup(a);
        } else {
            g_trace_menu[n] = ovl_menu_lookup(a); g_trace_batt[n] = ovl_battle_lookup(a);
            if (!g_trace_menu[n]) g_trace_menu[n] = psp_dispatch_probe_lookup(a);
        }
        if (g_trace_menu[n] || g_trace_batt[n]) psp_dispatch_register(a, fns[n]);
        n++;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}
// Dash speed per character (roster.txt `dash=F`, e.g. `34 noc100 9 ... dash=1.75`).
// R+Triangle puts the actor in state 0x19 (FUN_08A50C08: script 240 / 242 = motion _500A, then state 0x19; vtable
// 0x08A51490 in the battle overlay). Its slot 10, FUN_08A50CD0(out, state, actor), returns this tick's movement:
// nothing until the motion is 24 ticks in (the start-up, frame 12 of _500A), then direction x speed x the actor's time
// step, speed = state +0x28; the actor integrator FUN_0883AF80 adds it to the velocity. Start speed and decay come from
// the character's battle.bin 'spec' record (chrp +4 = spec id; +0x114 start speed: Bartz 0.6, retail 0.45..0.75; decay
// +0x12C over the first +0x120 ticks, then +0x144; 60-tick limit +0xFC). An extra character shares its like's record
// (the spec table sits in a fixed buffer and cannot grow without a relocation hook), so its own dash speed is applied
// here as a factor on that movement: the flight is F times faster (reaches the target in 1/F of the time, F times the
// range), the start-up and the time limit stay. Only actors whose charId has a factor other than 1 are touched.
// The overlay window is shared: the wrapper runs the battle function only while the battle overlay is resident (overlay
// id 1 in the window's first word, as camera.cpp does); otherwise it forwards to the menu overlay's function at that
// address if there is one (there is none: 0x08A50CD0 is inside its FUN_08A50CAC; the extra overlay ends far below).
constexpr uint32_t DASH_MOVE = 0x08A50CD0u;     // battle overlay: dash state slot 10 (movement this tick)
constexpr uint32_t ACTOR_INFO = 0x2F8u;         // actor +0x2F8 -> record whose byte +6 is the charId (chrp lookups use it)
FuncPtr g_dash_batt = nullptr;
float dash_factor(int charId) {
    for (const Entry& e : g_entries) if (e.charId == charId) return e.dash;
    return 1.0f;
}
void dash_move_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), actor = static_cast<uint32_t>(ctx->r[6]);
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) {                                    // not the battle overlay: its own code at this address, if any
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(DASH_MOVE) : nullptr) f(rdram, ctx);
        return;
    }
    g_dash_batt(rdram, ctx);
    if (actor < 0x08000000u || actor >= 0x0C000000u - 0x300u || out < 0x08000000u || out >= 0x0C000000u - 16u) return;
    uint32_t info;
    std::memcpy(&info, rdram + ((actor + ACTOR_INFO) & 0x07FFFFFFu), 4);
    if (info < 0x08000000u || info >= 0x0C000000u - 8u) return;
    const float f = dash_factor(static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]));
    if (f == 1.0f) return;
    float v[3];
    std::memcpy(v, rdram + (out & 0x07FFFFFFu), sizeof v);   // x, y, z (w untouched)
    for (float& c : v) c *= f;
    std::memcpy(rdram + (out & 0x07FFFFFFu), v, sizeof v);
}
void dash_install() {
    bool any = false;
    for (const Entry& e : g_entries) if (e.dash != 1.0f) { any = true; std::fprintf(stderr, "[ROSTER] charId %d: dash x%.2f\n", e.charId, e.dash); }
    if (!any) return;
    g_dash_batt = ovl_battle_lookup(DASH_MOVE);
    if (!g_dash_batt) { std::fprintf(stderr, "[ROSTER] dash speed hook NOT installed (no battle overlay function at %08X)\n", DASH_MOVE); return; }
    psp_dispatch_register(DASH_MOVE, dash_move_hook);
}
// Homing root motion per character (roster.txt `warp=MOTION[/MOTION...],TRAVEL,STOP[,LIFT]`; the token may be repeated, each
// with its own numbers for its motions), e.g. `34 noc100 9 ... warp=87/88,12.5,1.8,0.5`: Noctis's warp dash, ground and
// air (motions 87 / 88). warpfx= below acts on the motions of the first warp= token.
// An attack's travel is its motion's root motion: each tick FUN_0882F784 adds the delta of the `trans_include` bone into
// actor +0x140 and FUN_08832EB8(out, actor) (one caller, the actor integrator FUN_0883AF80) turns it into this tick's world
// displacement (rotated by the actor's yaw, or by its whole orientation under timeline op 0x0062), which the integrator adds
// to the move vector before the floor / wall / body collision. Nothing scales that travel to the target distance or stops
// it at the target: no timeline op or plya field of the 30 retail characters does (0x0402 / 0x0811 only turn, 0x0548 adds a
// fixed-direction velocity), so a fixed lunge overshoots a near target and falls short of a far one, and stays level.
// While the configured motion of this character plays, this wrapper replaces each tick's root-motion step by the same
// fraction of the way from the actor's position to an end point E, the fraction being this step's share of what is left of
// the motion's TRAVEL metres of trans_include (so the timing follows the motion's curve and the last step lands on E):
//   E = S + (T - S) * min(|T - S| - STOP, TRAVEL) / |T - S|,   S = actor position (+0x80) when the travel starts,
//                                                              T = target position now (*(actor +0x2FC) +0x80, the target
//                                                                  0x0402 turns to; position as FUN_08836DAC reads it)
// i.e. straight at the target in 3D (up or down too), at most TRAVEL metres (or REACH metres for a motion listed in
// `warpreach=MOTION[/MOTION...],REACH`: the same TRAVEL metres of trans_include then pace a longer way, e.g. the warp dash
// 87/88 at 30 m), and never closer than STOP metres to it (it
// stops in front of a near target instead of passing through). E follows a target that moves during the travel. LIFT > 0:
// when the target is lower, the travel ends no lower than LIFT above the target's feet. LIFT = -1: flat, homing in the
// horizontal plane only (distance, STOP and REACH horizontal; the step's own vertical part, the motion's plus vtrack='s, is
// kept), for lunges that stay on the floor or at their own height (Noctis's brave dash-ins). TRAVEL is the motion's whole
// trans_include travel, or the part of it up to where the dash-in should arrive (after it is used up, further root motion
// of that motion is left alone, e.g. a flurry's own follow-through). No target, another motion
// or another character: the engine's own step. The motion index is the u16 at +2 of the p_se script row at actor +0x17C
// (FUN_0882F0D4 sets it with the script id at +0x180); motion time is actor +0x1A8 (a restart is a drop in it).
// PSPRECOMP_WARP_LOG=1 logs each homing travel (start, target, end point) and its arrival.
constexpr uint32_t ROOT_STEP = 0x08832EB8u;     // EBOOT: this tick's root-motion displacement (a0 = out vec4, a1 = actor)
constexpr uint32_t ACT_POS = 0x80u, ACT_TARGET = 0x2FCu, ACT_SCRIPT_ROW = 0x17Cu, ACT_SCRIPT = 0x180u, ACT_MOTION_T = 0x1A8u;
FuncPtr g_root_step = nullptr;
bool g_warp_log = false;
struct WarpRun { uint32_t actor = 0; float done = -1.0f, last_t = 0.0f, s[3] = {0.0f, 0.0f, 0.0f}; int steps = 0;
                 float e[3] = {0.0f, 0.0f, 0.0f}; bool have_e = false;            // homing end point E (wisps)
                 float fly_t = -1.0f, p0[3] = {0.0f, 0.0f, 0.0f};                 // thrown sword: ticks since thrown, release point
                 bool s_wrote = false, w_wrote = false;                           // bone matrices written by warp_bone_post
                 int motion = -1, fx_motion = -1;                                 // the warp= motion of this travel / of the bone pass
                 float s_data[16] = {}, s_last[16] = {}, w_data[16] = {}, w_last[16] = {}; };
WarpRun g_warp_runs[8];
int g_warp_next = 0;
template <class T> T guest_rd(const uint8_t* rdram, uint32_t a) { T v; std::memcpy(&v, rdram + (a & 0x07FFFFFFu), sizeof v); return v; }
bool guest_vec3(const uint8_t* rdram, uint32_t a, float* v) {
    std::memcpy(v, rdram + (a & 0x07FFFFFFu), 3 * sizeof(float));
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
const Entry* warp_entry(int charId) {
    for (const Entry& e : g_entries) if (e.charId == charId && !e.warps.empty()) return &e;
    return nullptr;
}
const WarpSpec* warp_spec(const Entry* e, int motion) {   // the warp= token listing this motion (the first one that does)
    if (!e || motion < 0) return nullptr;
    for (const WarpSpec& w : e->warps)
        for (int m : w.motions) if (m == motion) return &w;
    return nullptr;
}
const WarpSpec* warp_fx_spec(const Entry* e, int motion) {   // warpfx= acts on the first token's motions only
    if (!e || e->warps.empty() || motion < 0) return nullptr;
    for (int m : e->warps[0].motions) if (m == motion) return &e->warps[0];
    return nullptr;
}
float warp_reach_of(const Entry* e, const WarpSpec* sp, int motion) {   // warpreach= for this motion, else its TRAVEL
    if (e) for (const auto& r : e->warp_reach) if (r.first == motion) return r.second;
    return sp ? sp->travel : 0.0f;
}
WarpRun& warp_run(uint32_t actor) {
    for (WarpRun& r : g_warp_runs) if (r.actor == actor) return r;
    WarpRun& r = g_warp_runs[g_warp_next++ & 7];
    r = WarpRun{};
    r.actor = actor;
    return r;
}
// Vertical tracking of an attack's start per character (roster.txt `vtrack=MOTION[/MOTION...],FROM,TO,MAX[,LIFT]`, repeatable),
// e.g. `34 noc100 9 ... vtrack=60,0,12,1.8,0.3`: Noctis's Dagger Strike flurry `_654A` (motion 60) eases up or down to the
// foe's height over its dash-in, ticks 0-12, before its first hit (10-16). No timeline op or plya field of the 30 retail
// characters moves an attacker toward the target's height (0x0402 turns the yaw only: its arg 1 just makes the turn
// conditional on FUN_08838074(target), FUN_088360BC; 0x0548 pushes along a fixed pitch; plya +0x40..+0x5C with flag
// +0x18 bit 0 are camera parameters, FUN_08A60BC8 writes them to the camera at actor +788), so this rides on the same
// root-motion step as warp= (FUN_08832EB8, warp_step_hook). While a listed motion plays at motion time t in [FROM, TO)
// and the actor has a target, this tick's step gets a vertical part so that the actor's height (feet, +0x84) follows
//   B + ease(u) * clamp(Y - B, -MAX, MAX),   u = (t + 1 - FROM) / (TO - FROM),  ease = smoothstep,
// B = the height the motion alone would reach this tick (position + engine step - the offset added so far), Y = the
// target's feet height (*(actor +0x2FC) +0x84); a lower target: Y = min(B, target + LIFT). Y is re-read every tick (a
// moving foe is followed), the result never passes Y (no overshoot) and never strays more than MAX from the motion's own
// height; one tick moves at most 2 * MAX / (TO - FROM). After TO the offset stays (no drift back). Horizontal travel,
// yaw and the motion are untouched. No target, another motion or another character: the engine's own step. Not for a
// warp= motion (that one already homes in 3D). PSPRECOMP_WARP_LOG=1 logs the start and end of each tracking.
struct VTrackRun { uint32_t actor = 0; int motion = -1; float last_t = 0.0f, applied = 0.0f, y0 = 0.0f; bool logged = false; };
VTrackRun g_vt_runs[8];
int g_vt_next = 0;
bool g_vtrack_any = false;
int warp_motion_now(const uint8_t* rdram, uint32_t actor);
const VTrack* vtrack_spec(int charId, int motion) {
    if (motion < 0) return nullptr;
    for (const Entry& e : g_entries)
        if (e.charId == charId)
            for (const VTrack& v : e.vtracks)
                for (int m : v.motions) if (m == motion) return &v;
    return nullptr;
}
void vtrack_step(uint8_t* rdram, uint32_t actor, uint32_t out, int charId) {
    if (!g_vtrack_any) return;
    const int motion = warp_motion_now(rdram, actor);
    const VTrack* v = vtrack_spec(charId, motion);
    VTrackRun* r = nullptr;
    for (VTrackRun& x : g_vt_runs) if (x.actor == actor) { r = &x; break; }
    if (!v) { if (r) r->motion = -1; return; }
    if (!r) { r = &g_vt_runs[g_vt_next++ & 7]; *r = VTrackRun{}; r->actor = actor; }
    const float t = guest_rd<float>(rdram, actor + ACT_MOTION_T);
    if (r->motion != motion || t < r->last_t) { r->motion = motion; r->applied = 0.0f; r->logged = false; }   // (re)started
    r->last_t = t;
    if (!(t >= v->from && t < v->to)) {
        if (g_warp_log && r->logged && t >= v->to) {
            std::fprintf(stderr, "[VTRACK] actor %08X motion %d: done, %+.2f m added\n", actor, motion, r->applied);
            r->logged = false;
        }
        return;
    }
    const uint32_t tgt = guest_rd<uint32_t>(rdram, actor + ACT_TARGET);
    float p[3], T[3];
    if (!guest_ok(tgt, 0x100u) || !guest_vec3(rdram, tgt + ACT_POS, T) || !guest_vec3(rdram, actor + ACT_POS, p)) return;
    float oy = guest_rd<float>(rdram, out + 4u);
    if (!std::isfinite(oy)) return;
    const float span = std::max(v->to - v->from, 1.0f);
    const float u = std::min(std::max((t + 1.0f - v->from) / span, 0.0f), 1.0f);
    const float ease = u * u * (3.0f - 2.0f * u);
    const float B = p[1] + oy - r->applied;
    float Y = T[1];
    if (Y < B && v->lift > 0.0f) Y = std::min(B, Y + v->lift);
    const float goal = std::min(std::max(Y - B, -v->max), v->max);
    const float lim = 2.0f * v->max / span;
    const float d = std::min(std::max(ease * goal - r->applied, -lim), lim);
    oy += d;
    r->applied += d;
    std::memcpy(rdram + ((out + 4u) & 0x07FFFFFFu), &oy, sizeof oy);
    if (g_warp_log && !r->logged) {
        std::fprintf(stderr, "[VTRACK] actor %08X motion %d t %.0f: height %.2f, target %.2f -> goal %+.2f m\n", actor, motion, t, p[1], T[1], goal);
        r->logged = true;
    }
}
void warp_step_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t out = static_cast<uint32_t>(ctx->r[4]), actor = static_cast<uint32_t>(ctx->r[5]);
    g_root_step(rdram, ctx);
    if (!guest_ok(actor, 0x400u) || !guest_ok(out, 16u)) return;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return;
    vtrack_step(rdram, actor, out, static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]));
    const Entry* e = warp_entry(static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]));
    if (!e) return;
    const uint32_t row = guest_rd<uint32_t>(rdram, actor + ACT_SCRIPT_ROW);
    const int16_t script = guest_rd<int16_t>(rdram, actor + ACT_SCRIPT);
    int motion = -1;
    if (script >= 0 && guest_ok(row, 8u) && guest_rd<uint16_t>(rdram, row) == static_cast<uint16_t>(script))
        motion = guest_rd<uint16_t>(rdram, row + 2u);
    WarpRun& r = warp_run(actor);
    const float t = guest_rd<float>(rdram, actor + ACT_MOTION_T);
    const WarpSpec* sp = warp_spec(e, motion);
    if (!sp) { r.done = -1.0f; r.have_e = false; r.motion = -1; return; }
    if (r.done >= 0.0f && (t < r.last_t || motion != r.motion)) { r.done = -1.0f; r.have_e = false; }   // started again / another warp motion
    r.last_t = t;
    r.motion = motion;
    float o[4];
    std::memcpy(o, rdram + (out & 0x07FFFFFFu), sizeof o);
    const float m = sp->flat ? std::sqrt(o[0] * o[0] + o[2] * o[2]) : std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    if (!(m > 1e-6f) || r.done >= sp->travel) return;
    float p[3], T[3];
    if (!guest_vec3(rdram, actor + ACT_POS, p)) return;
    const bool start = r.done < 0.0f;
    if (start) { r.done = 0.0f; r.steps = 0; std::memcpy(r.s, p, sizeof p); r.have_e = false; }
    const float rem = sp->travel - r.done;
    const float frac = m >= rem - 1e-4f ? 1.0f : m / rem;     // the last step (float sums) lands exactly on E
    r.done = frac >= 1.0f ? sp->travel : r.done + m;
    r.steps++;
    const uint32_t tgt = guest_rd<uint32_t>(rdram, actor + ACT_TARGET);
    if (!guest_ok(tgt, 0x100u) || !guest_vec3(rdram, tgt + ACT_POS, T)) {   // no target: the motion's own straight travel
        if (start && g_warp_log) std::fprintf(stderr, "[WARP] actor %08X motion %d: no target, own travel\n", actor, motion);
        return;
    }
    const float D[3] = {T[0] - r.s[0], sp->flat ? 0.0f : T[1] - r.s[1], T[2] - r.s[2]};   // flat: horizontal only
    const float d = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
    float E[3] = {r.s[0], r.s[1], r.s[2]};
    if (d > 1e-4f) {
        const float along = std::min(std::max(d - sp->stop, 0.0f), warp_reach_of(e, sp, motion));
        for (int i = 0; i < 3; i++) E[i] = r.s[i] + D[i] * (along / d);
        if (sp->lift > 0.0f && T[1] + sp->lift < r.s[1] && E[1] < T[1] + sp->lift) E[1] = T[1] + sp->lift;
    }
    std::memcpy(r.e, E, sizeof E); r.have_e = true;
    for (int i = 0; i < 3; i++) if (!(sp->flat && i == 1)) o[i] = (E[i] - p[i]) * frac;   // flat: o[1] stays the step's own
    std::memcpy(rdram + (out & 0x07FFFFFFu), o, 3 * sizeof(float));   // w (yaw step) untouched
    if (g_warp_log && (start || frac >= 1.0f))
        std::fprintf(stderr, "[WARP] actor %08X motion %d %s: pos (%.2f %.2f %.2f) target (%.2f %.2f %.2f) dist %.2f -> end (%.2f %.2f %.2f)%s\n",
                     actor, motion, start ? "start" : "arrive", p[0], p[1], p[2], T[0], T[1], T[2], d, E[0], E[1], E[2],
                     start ? "" : (" after " + std::to_string(r.steps) + " steps").c_str());
}
// Warp-Strike's thrown sword and warp wisps (roster.txt `warpfx=SWORD,FLY,AIM,TIP[,WISP]` next to warp=, e.g.
// `34 noc100 9 ... warp=87/88,12.5,1.8,0.5 warpfx=83,5,1.0,1.124,84`). While the warp= motion plays, FUN_0880EA20 (the last
// pass that writes bone matrices before the model is drawn, bone_post_hook below; bone matrices are model space, row
// vectors, translation [12..14], see the armiger notes) is followed by warp_bone_post, which re-places two bones:
//   SWORD  the thrown sword's skin bone (Noctis: 83 `warp_sword`, drawn by draw bone 122). Whenever the motion shows it
//          (scale > 0; the motion keeps its blade along model +Z and flies it straight ahead, which is what an older
//          runtime shows) it is put in the world instead: from the point where the motion first shows it (his hand at
//          the release, frame 7) straight at the target's centre (target position + AIM m up), point first, its point
//          (TIP m along the blade from the bone) arriving there after FLY ticks; never farther than TRAVEL + STOP from
//          the release (REACH + STOP with warpreach=; an out-of-reach target: it stops on that line in the air, where he
//          will strike); then it stays
//          there, following a moving target, until the motion hides it at the strike.
//   WISP   (optional) the bone the warp wisps are skinned to (Noctis: 84 `warp_trail`; the wisp Parts are built along the
//          motion's own straight TRAVEL m on model +Z). Once warp_step_hook has an end point E, the bone is turned so
//          model +Z lies along S -> E (up or down too) and scaled along it by |E - S| / TRAVEL, so the wisps lie on the
//          real path and never stick out behind the start of a short warp; under 5 cm of travel they are hidden (scale 0).
// No target, another motion or another character: the bones as the motion poses them. A frame the engine did not re-pose
// (a bone still holds what this pass wrote) uses the motion's pose of the frame before; when the motion changes, a bone
// still holding this pass's matrix gets the motion's pose back.
float ex_actor_dt(uint8_t* r, uint32_t actor);
uint32_t ex_bone_mat(uint8_t* r, uint32_t gm, int idx);
void mat_mul(const float* A, const float* B, float* C);
bool mat_inv_affine(const float* M, float* O);
constexpr uint32_t ACT_MODEL = 0x394u, ACT_WORLD = 0x330u;
bool warp_fx_wanted() {
    for (const Entry& e : g_entries) if (!e.warps.empty() && (e.warp_sword >= 0 || e.warp_wisp >= 0)) return true;
    return false;
}
int warp_motion_now(const uint8_t* rdram, uint32_t actor) {
    const uint32_t row = guest_rd<uint32_t>(rdram, actor + ACT_SCRIPT_ROW);
    const int16_t script = guest_rd<int16_t>(rdram, actor + ACT_SCRIPT);
    if (script >= 0 && guest_ok(row, 8u) && guest_rd<uint16_t>(rdram, row) == static_cast<uint16_t>(script))
        return guest_rd<uint16_t>(rdram, row + 2u);
    return -1;
}
// row-vector rotation (v' = v R) taking +Z to the unit vector d, about Z x d
void warp_rot_z_to(const float* d, float* R) {
    const float c = d[2], s = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    if (s < 1e-6f) {
        const float z = c < 0.0f ? -1.0f : 1.0f;              // already along +Z, or 180 deg about X
        const float I[9] = {1, 0, 0, 0, z, 0, 0, 0, z};
        std::memcpy(R, I, sizeof I);
        return;
    }
    const float ax = -d[1] / s, ay = d[0] / s, k = 1.0f - c;
    const float Rc[9] = {c + k * ax * ax, k * ax * ay, s * ay,    // column-vector Rodrigues, axis (ax, ay, 0)
                         k * ax * ay, c + k * ay * ay, -s * ax,
                         -s * ay, s * ax, c};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[3 * i + j] = Rc[3 * j + i];
}
// world point / direction -> model space with Wi = inverse of the model -> world matrix (row vectors)
void warp_to_model(const float* Wi, const float* v, float w, float* o) {
    for (int j = 0; j < 3; j++) o[j] = v[0] * Wi[j] + v[1] * Wi[4 + j] + v[2] * Wi[8 + j] + w * Wi[12 + j];
}
// the motion's pose of a bone this frame: the matrix there, unless it is still the one written here (not re-posed)
uint32_t warp_bone_pose(uint8_t* rdram, uint32_t gm, int bone, bool wrote, const float* last, float* data) {
    const uint32_t at = bone >= 0 ? ex_bone_mat(rdram, gm, bone) : 0u;
    if (!at) return 0u;
    float cur[16];
    std::memcpy(cur, rdram + (at & 0x07FFFFFFu), sizeof cur);
    if (!(wrote && std::memcmp(cur, last, sizeof cur) == 0)) std::memcpy(data, cur, sizeof cur);
    return at;
}
void warp_bone_put(uint8_t* rdram, uint32_t at, const float* M, float* last, bool& wrote) {
    std::memcpy(rdram + (at & 0x07FFFFFFu), M, 64);
    std::memcpy(last, M, 64);
    wrote = true;
}
void warp_bone_post(uint8_t* rdram, uint32_t actor) {
    if (!guest_ok(actor, 0x400u)) return;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return;
    const Entry* e = warp_entry(static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]));
    if (!e || (e->warp_sword < 0 && e->warp_wisp < 0)) return;
    const uint32_t ctrl = guest_rd<uint32_t>(rdram, actor + ACT_MODEL);
    if (!guest_ok(ctrl, 0x110u)) return;
    const uint32_t gm = guest_rd<uint32_t>(rdram, ctrl + 0x0Cu);
    if (!guest_ok(gm, 0x24u)) return;
    const int fxm = warp_motion_now(rdram, actor);
    const WarpSpec* sp = warp_fx_spec(e, fxm);
    if (!sp) {   // not (or no longer) a warpfx motion: the motion's own pose
        for (WarpRun& r : g_warp_runs) {
            if (r.actor != actor) continue;
            const int bones[2] = {e->warp_sword, e->warp_wisp};
            const bool wrote[2] = {r.s_wrote, r.w_wrote};
            const float* last[2] = {r.s_last, r.w_last};
            const float* data[2] = {r.s_data, r.w_data};
            for (int k = 0; k < 2; k++) {
                const uint32_t at = wrote[k] && bones[k] >= 0 ? ex_bone_mat(rdram, gm, bones[k]) : 0u;
                if (at && std::memcmp(rdram + (at & 0x07FFFFFFu), last[k], 64) == 0) std::memcpy(rdram + (at & 0x07FFFFFFu), data[k], 64);
            }
            r.s_wrote = r.w_wrote = false; r.fly_t = -1.0f; r.fx_motion = -1;
        }
        return;
    }
    WarpRun& r = warp_run(actor);
    if (r.fx_motion != fxm) { r.fly_t = -1.0f; r.fx_motion = fxm; }   // another warp motion: the sword is thrown anew
    float W[16], Wi[16];
    std::memcpy(W, rdram + ((actor + ACT_WORLD) & 0x07FFFFFFu), sizeof W);
    if (!mat_inv_affine(W, Wi)) return;
    // ---- the thrown sword
    if (const uint32_t at = warp_bone_pose(rdram, gm, e->warp_sword, r.s_wrote, r.s_last, r.s_data)) {
        const float* D = r.s_data;
        r.s_wrote = false;
        if (!(D[8] * D[8] + D[9] * D[9] + D[10] * D[10] > 1e-8f)) {
            r.fly_t = -1.0f;                                    // not shown: not thrown yet / caught again
        } else {
            if (r.fly_t < 0.0f) {                               // first frame shown: the release point
                r.fly_t = 0.0f;
                for (int j = 0; j < 3; j++) r.p0[j] = D[12] * W[j] + D[13] * W[4 + j] + D[14] * W[8 + j] + W[12 + j];
            } else {
                r.fly_t += ex_actor_dt(rdram, actor);
            }
            const uint32_t tgt = guest_rd<uint32_t>(rdram, actor + ACT_TARGET);
            float T[3];
            if (guest_ok(tgt, 0x100u) && guest_vec3(rdram, tgt + ACT_POS, T)) {
                float dir[3] = {T[0] - r.p0[0], T[1] + e->warp_aim - r.p0[1], T[2] - r.p0[2]};
                const float L = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                if (L > 1e-3f) {
                    for (float& x : dir) x /= L;
                    const float reach = std::min(L, warp_reach_of(e, sp, fxm) + sp->stop);
                    const float u = e->warp_fly > 0.0f ? std::min(r.fly_t / e->warp_fly, 1.0f) : 1.0f;
                    const float g = std::max(reach - e->warp_tip, 0.0f) * u;
                    const float gw[3] = {r.p0[0] + dir[0] * g, r.p0[1] + dir[1] * g, r.p0[2] + dir[2] * g};
                    float pm[3], dm[3], Rz[9], M[16];
                    warp_to_model(Wi, gw, 1.0f, pm);
                    warp_to_model(Wi, dir, 0.0f, dm);
                    const float dl = std::sqrt(dm[0] * dm[0] + dm[1] * dm[1] + dm[2] * dm[2]);
                    if (dl > 1e-6f) {
                        for (float& x : dm) x /= dl;
                        warp_rot_z_to(dm, Rz);
                        for (int i = 0; i < 3; i++) {
                            for (int j = 0; j < 3; j++) M[4 * i + j] = D[4 * i] * Rz[j] + D[4 * i + 1] * Rz[3 + j] + D[4 * i + 2] * Rz[6 + j];
                            M[4 * i + 3] = D[4 * i + 3];
                        }
                        M[12] = pm[0]; M[13] = pm[1]; M[14] = pm[2]; M[15] = D[15];
                        warp_bone_put(rdram, at, M, r.s_last, r.s_wrote);
                    }
                }
            }
        }
    }
    // ---- the wisps: model +Z along the real travel, scaled to its length
    if (const uint32_t at = warp_bone_pose(rdram, gm, e->warp_wisp, r.w_wrote, r.w_last, r.w_data)) {
        r.w_wrote = false;
        if (r.have_e) {
            const float V[3] = {r.e[0] - r.s[0], r.e[1] - r.s[1], r.e[2] - r.s[2]};
            const float len = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
            float X[16] = {};
            X[15] = 1.0f;
            float dm[3];
            warp_to_model(Wi, V, 0.0f, dm);
            const float dl = std::sqrt(dm[0] * dm[0] + dm[1] * dm[1] + dm[2] * dm[2]);
            if (len >= 0.05f && dl > 1e-6f) {
                for (float& x : dm) x /= dl;
                float Rz[9];
                warp_rot_z_to(dm, Rz);
                const float f = std::min(len, warp_reach_of(e, sp, fxm)) / sp->travel;
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) X[4 * i + j] = Rz[3 * i + j] * (i == 2 ? f : 1.0f);
            }                                                   // else X = 0: hidden (no travel to show)
            float M[16];
            mat_mul(r.w_data, X, M);
            warp_bone_put(rdram, at, M, r.w_last, r.w_wrote);
        }
    }
}
void warp_install() {
    bool any = false;
    for (const Entry& e : g_entries)
        for (const WarpSpec& w : e.warps) {
            any = true;
            std::string ml;
            for (int m : w.motions) ml += (ml.empty() ? "" : " ") + std::to_string(m);
            std::fprintf(stderr, "[ROSTER] charId %d: motion %s homes on the target (travel %.2f m, stops %.2f m short, %s %.2f m)\n",
                         e.charId, ml.c_str(), w.travel, w.stop, w.flat ? "flat, lift" : "lift", w.lift);
        }
    for (const Entry& e : g_entries)
        for (const auto& r : e.warp_reach)
            std::fprintf(stderr, "[ROSTER] charId %d: motion %d homes up to %.1f m (warpreach=)%s\n", e.charId, r.first, r.second,
                         warp_spec(&e, r.first) ? "" : " - not in any warp= token, no effect");
    for (const Entry& e : g_entries)
        for (const VTrack& v : e.vtracks) {
            g_vtrack_any = true;
            std::string ml;
            for (int m : v.motions) ml += (ml.empty() ? "" : " ") + std::to_string(m);
            std::fprintf(stderr, "[ROSTER] charId %d: motion %s eases to the target's height (ticks %.0f-%.0f, up to %.2f m, lift %.2f m)%s\n",
                         e.charId, ml.c_str(), v.from, v.to, v.max, v.lift, [&] { for (int m : v.motions) if (warp_spec(&e, m)) return " - also a warp= motion, not for that"; return ""; }());
        }
    if (!any && !g_vtrack_any) return;
    for (const Entry& e : g_entries)
        if (!e.warps.empty() && (e.warp_sword >= 0 || e.warp_wisp >= 0)) {
            std::string ml;
            for (int m : e.warps[0].motions) ml += (ml.empty() ? "" : " ") + std::to_string(m);
            std::fprintf(stderr, "[ROSTER] charId %d: warp motion %s thrown sword bone %d (%.1f ticks, aim %.2f m up, tip %.2f m), wisp bone %d\n",
                         e.charId, ml.c_str(), e.warp_sword, e.warp_fly, e.warp_aim, e.warp_tip, e.warp_wisp);
        }
    g_warp_log = std::getenv("PSPRECOMP_WARP_LOG") != nullptr;
    g_root_step = psp_dispatch_probe_lookup(ROOT_STEP);
    if (!g_root_step) { std::fprintf(stderr, "[ROSTER] warp homing hook NOT installed (no function at %08X)\n", ROOT_STEP); return; }
    psp_dispatch_register(ROOT_STEP, warp_step_hook);
}
// Touchdown action per character (roster.txt `landact=ACTION,MOTION[,MOTION...]`, up to 4 motions, e.g.
// `34 noc100 9 ... landact=3024,54,55`: Noctis's Greatsword Down Strike lands into its impact action 3024, script 10096,
// motion 84 `_642A`, when it touches down during the dive `_640A` (54) or the carry `_641A` (55)).
// FUN_08833C98(actor, f12) is the touchdown handler of the air states (0x12 / 1; actor vtable +0xC0, reached from
// FUN_08833818 once timeline op 0x002D allows landing and the height reaches 0): it plays the landing script from
// *(actor + 0x314) + 0xF2 (always 102 `_022A`: every caller of FUN_08834934 passes 0x66), queues stand (script 1) and
// sets state 0, which ends the action. No timeline op or plya field picks another landing per move, so a move cannot
// put a hit, an effect or a recovery on the ground at the moment it lands from an unknown height.
// While one of MOTIONS plays on this character (motion index = u16 +2 of the p_se script row at actor +0x17C, as
// warp_step_hook reads it; checked before the handler runs, as it changes the script), the wrapper runs the engine
// handler, then starts ACTION with the battle overlay's start-action function FUN_08B382B8(actor, input kind, current
// record or 0), the one the command input path FUN_08B2D664 calls. Its a1 is NOT an action id: it is the input kind
// (0-4; kinds 0/1 count as a brave attack, 2/3 as an HP attack, the AI helpers FUN_08B2D80C / FUN_08B2D82C pass 1 / 3),
// which the command selector FUN_0884A294(holder, actor, target, kind) uses to pick the plya record for the press; a2 is
// the record now running (actor +0x101C), so the same action is not restarted. (The first version passed the action id
// as the kind: the selector found nothing and every touchdown fell back to the plain landing.) So the wrapper finds the
// action's record in the actor's own plya table (actor +0x390 -> +0x88: {?, count, records of 0x74 bytes}, u16 id at
// +0; FUN_0884808C's search) and overrides the selector for that one call (plya_select_hook hands back the record and
// points the plya holder *0x089D4974 at the actor's table, as FUN_08849B50 does before it selects). FUN_08B382B8 then
// handles the landed actor itself (0x384 & 0x200 -> state 0), stores the record at actor +0x101C, plays its script
// (+0x1C, FUN_0882FA10), queues its exit script (+0x1E) and returns 1, so the action's motion and timeline (its hit,
// effects, recovery) replace `_022A`; it returns 0 (plain landing stays) if the record has no script in the actor's
// script map, or the actor's gate byte +0x198 has bit 0x80 (and not 0x40) set. Kind 1 (a brave press) is passed.
// Only while the battle overlay is resident (overlay id 1 in its window's first word, as dash_move_hook).
// The guest registers the handler returns with are given back to its caller unchanged. PSPRECOMP_LAND_LOG=1 logs each.
constexpr uint32_t TOUCHDOWN = 0x08833C98u;     // EBOOT: air-state touchdown (a0 = actor, f12)
constexpr uint32_t START_ACTION = 0x08B382B8u;  // battle overlay: start action (a0 = actor, a1 = input kind, a2 = current record or 0)
constexpr uint32_t PLYA_SELECT = 0x0884A294u;   // EBOOT: command selector (a0 = holder, a1 = actor, a2 = target, a3 = kind) -> record
constexpr uint32_t PLYA_HOLDER = 0x089D4974u;   // word -> plya holder; its first word = the plya table searched
constexpr uint32_t ACT_MOVESET = 0x390u;        // actor +0x390 -> moveset; moveset +0x88 = its plya table
constexpr uint32_t ACT_GATE = 0x198u;           // actor byte FUN_08B382B8 gates on (0x40 / 0x80)
constexpr uint32_t PLYA_STRIDE = 0x74u;
constexpr int LAND_KIND = 1;                    // brave input kind
FuncPtr g_touchdown = nullptr, g_start_action = nullptr, g_plya_select = nullptr;
uint32_t g_force_actor = 0, g_force_rec = 0, g_force_table = 0;   // set only while land_hook runs FUN_08B382B8
bool g_land_log = false;
int land_action_for(const uint8_t* rdram, uint32_t actor) {
    if (!guest_ok(actor, 0x400u)) return -1;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return -1;
    const int charId = static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]);
    for (const Entry& e : g_entries) {
        if (e.charId != charId || e.land_act < 0) continue;
        const uint32_t row = guest_rd<uint32_t>(rdram, actor + ACT_SCRIPT_ROW);
        const int16_t script = guest_rd<int16_t>(rdram, actor + ACT_SCRIPT);
        if (script < 0 || !guest_ok(row, 8u) || guest_rd<uint16_t>(rdram, row) != static_cast<uint16_t>(script)) return -1;
        const int motion = guest_rd<uint16_t>(rdram, row + 2u);
        for (int m : e.land_mot) if (m >= 0 && m == motion) return e.land_act;
        return -1;
    }
    return -1;
}
// The actor's plya record for action id `id` (0 if none); *table = the plya table it is in.
uint32_t plya_record(const uint8_t* rdram, uint32_t actor, int id, uint32_t* table) {
    const uint32_t ms = guest_rd<uint32_t>(rdram, actor + ACT_MOVESET);
    if (!guest_ok(ms, 0x8Cu)) return 0;
    const uint32_t t = guest_rd<uint32_t>(rdram, ms + 0x88u);
    if (!guest_ok(t, 8u)) return 0;
    const uint32_t n = guest_rd<uint32_t>(rdram, t + 4u);
    if (n == 0u || n > 4096u || !guest_ok(t, 8u + n * PLYA_STRIDE)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t r = t + 8u + i * PLYA_STRIDE;
        if (guest_rd<uint16_t>(rdram, r) == static_cast<uint16_t>(id)) { *table = t; return r; }
    }
    return 0;
}
bool warpdash_link(uint8_t* rdram, recomp_context* ctx);   // warp dash section: a press while its link is armed
void plya_select_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_force_rec != 0u && static_cast<uint32_t>(ctx->r[5]) == g_force_actor) {
        const uint32_t holder = guest_rd<uint32_t>(rdram, PLYA_HOLDER);
        if (guest_ok(holder, 4u)) std::memcpy(rdram + (holder & 0x07FFFFFFu), &g_force_table, 4);
        ctx->r[2] = static_cast<int32_t>(g_force_rec);
        return;
    }
    if (warpdash_link(rdram, ctx)) return;              // warp dash hit: the next brave's press 2 (warpdash=)
    g_plya_select(rdram, ctx);
}
void land_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    const int act = land_action_for(rdram, actor);
    g_touchdown(rdram, ctx);
    if (act < 0 || !g_start_action || !g_plya_select) return;
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) return;                              // battle overlay not resident
    uint32_t table = 0;
    const uint32_t rec = plya_record(rdram, actor, act, &table);
    if (!rec) {
        if (g_land_log) std::fprintf(stderr, "[LAND] actor %08X touched down: action %d NOT started (no such plya record)\n", actor, act);
        return;
    }
    const uint8_t gate = rdram[(actor + ACT_GATE) & 0x07FFFFFFu];
    const recomp_context after = *ctx;                  // what the handler hands back to its caller
    ctx->r[4] = static_cast<int32_t>(actor); ctx->r[5] = LAND_KIND; ctx->r[6] = 0;
    g_force_actor = actor; g_force_rec = rec; g_force_table = table;
    g_start_action(rdram, ctx);
    g_force_actor = g_force_rec = g_force_table = 0u;
    const uint32_t ok = static_cast<uint32_t>(ctx->r[2]);
    *ctx = after;
    if (g_land_log)
        std::fprintf(stderr, "[LAND] actor %08X touched down: action %d (record %08X) %s, v0 %u, gate %02X, now +0x101C %08X\n",
                     actor, act, rec, ok ? "started" : "NOT started (plain landing)", ok, gate,
                     guest_rd<uint32_t>(rdram, actor + 0x101Cu));
}
void land_install() {
    bool any = false;
    for (const Entry& e : g_entries)
        if (e.land_act >= 0) {
            any = true;
            std::fprintf(stderr, "[ROSTER] charId %d: touchdown during motion %d %d %d %d starts action %d\n", e.charId,
                         e.land_mot[0], e.land_mot[1], e.land_mot[2], e.land_mot[3], e.land_act);
        }
    if (!any) return;
    g_land_log = std::getenv("PSPRECOMP_LAND_LOG") != nullptr;
    g_touchdown = psp_dispatch_probe_lookup(TOUCHDOWN);
    g_plya_select = psp_dispatch_probe_lookup(PLYA_SELECT);
    g_start_action = ovl_battle_lookup(START_ACTION);
    if (!g_touchdown || !g_start_action || !g_plya_select) {
        std::fprintf(stderr, "[ROSTER] touchdown action hook NOT installed (touchdown %s, start action %s, selector %s)\n",
                     g_touchdown ? "ok" : "missing", g_start_action ? "ok" : "missing", g_plya_select ? "ok" : "missing");
        return;
    }
    psp_dispatch_register(PLYA_SELECT, plya_select_hook);
    psp_dispatch_register(TOUCHDOWN, land_hook);
}
// Chain window starts its action (roster.txt `chainact=ACTION[,ACTION...]`, up to 4, e.g. `34 noc100 9 ... chainact=3026`:
// Armiger Assault's press 3 `_657A` goes into its homing leap 3026 `_659B` by itself, while the arms' volley holds the foe).
// Timeline op 0x0467 {0, ACTION, A, B} (ranged dispatcher FUN_08829930, jump table 0x089C7910 index 0x67) points the plya
// holder at the actor's table (FUN_08B34DD4, when actor +0x542 is 0 / 1), looks ACTION up (FUN_0884808C) and adds
// {record, A != 0, B != 0} to the actor's chain list with FUN_08B308D8(actor, record, a, b) (8-byte entries from +0x1028,
// count s16 +0x1068; FUN_08B30904 clears it). The list only OPENS a chain: the press path reads it (FUN_08A77410 matches a
// press's command record against the entries with A set; FUN_08B30930 "an entry has A" lets a brave press through
// mid-action; FUN_08848984 lights the input bits). Nothing in the game starts an entry by itself, so a 0x0467 follow-up
// only ever plays when the player presses (Assault v4's leap waited for a fourth press that never came).
// With the token, the wrapper on FUN_08B308D8 runs the engine function, then, when the record's action id (u16 +0) is
// listed and the actor is this character, remembers {actor, the actor's own record of that id (plya_record), the record
// running now (+0x1020, else +0x101C, as FUN_08B30898)}. The action is started in the actor's next bone pass
// (bone_post_hook, after the timeline walk, so the old timeline is never left half-walked) with FUN_08B382B8(actor, 1, 0)
// and the selector override, as landact= does. It is dropped if the running record changed in between (he was hit, or a
// press already chained) or the battle overlay is gone; a refused start (v0 0) is not retried by the hook (the window
// adds the entry again only if the timeline does). The engine's own entry stays, so a press still chains as before.
// PSPRECOMP_CHAIN_LOG=1 logs every start (the first few are always logged).
constexpr uint32_t CHAIN_ADD = 0x08B308D8u;      // battle overlay: add a chain entry (a0 actor, a1 plya record, a2 A, a3 B)
constexpr uint32_t ACT_RUN = 0x101Cu, ACT_RUN2 = 0x1020u;   // the running record (FUN_08B30898: +0x1020, else +0x101C)
constexpr int kChainPend = 4, kChainFirstLogs = 6;
struct ChainPend { uint32_t actor = 0, rec = 0, table = 0, cur = 0; int act = -1; };
ChainPend g_chain_pend[kChainPend];
FuncPtr g_chain_add = nullptr;
bool g_chain_log = false;
int g_chain_logs = 0;
bool chain_wanted() {
    for (const Entry& e : g_entries) if (e.chain_act[0] >= 0 || e.hit_magi >= 0) return true;
    return false;
}
const Entry* chain_entry(const uint8_t* rdram, uint32_t actor) {
    if (!guest_ok(actor, 0x1070u)) return nullptr;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return nullptr;
    const int cid = static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]);
    for (const Entry& e : g_entries) if (e.charId == cid && e.chain_act[0] >= 0) return &e;
    return nullptr;
}
uint32_t chain_running(const uint8_t* rdram, uint32_t actor) {
    const uint32_t r = guest_rd<uint32_t>(rdram, actor + ACT_RUN2);
    return r ? r : guest_rd<uint32_t>(rdram, actor + ACT_RUN);
}
bool chain_say() { return g_chain_log || g_chain_logs < kChainFirstLogs; }
void chain_add_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) {                                    // not the battle overlay: its own code at this address, if any
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(CHAIN_ADD) : nullptr) f(rdram, ctx);
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]), rec = static_cast<uint32_t>(ctx->r[5]);
    g_chain_add(rdram, ctx);
    const Entry* e = chain_entry(rdram, actor);
    if (!e || !guest_ok(rec, PLYA_STRIDE)) return;
    const int id = guest_rd<uint16_t>(rdram, rec);
    bool listed = false;
    for (int a : e->chain_act) if (a >= 0 && a == id) listed = true;
    if (!listed) return;
    uint32_t table = 0;
    const uint32_t own = plya_record(rdram, actor, id, &table);
    if (!own) {
        if (chain_say()) { g_chain_logs++; std::fprintf(stderr, "[CHAIN] actor %08X: chain window to action %d, no such record in its own table: not started\n", actor, id); }
        return;
    }
    ChainPend* p = nullptr;
    for (ChainPend& x : g_chain_pend) if (x.actor == actor) { p = &x; break; }
    if (!p) for (ChainPend& x : g_chain_pend) if (!x.actor) { p = &x; break; }
    if (!p) p = &g_chain_pend[0];
    p->actor = actor; p->rec = own; p->table = table; p->act = id; p->cur = chain_running(rdram, actor);
}
// the actor's bone pass (bone_post_hook, after the engine's): start a remembered chain action
const Entry* hit_entry(const uint8_t* rdram, uint32_t actor);
void chain_tick(uint8_t* rdram, recomp_context* ctx, uint32_t actor) {
    ChainPend* p = nullptr;
    for (ChainPend& x : g_chain_pend) if (x.actor && x.actor == actor) { p = &x; break; }
    if (!p) return;
    const ChainPend q = *p;
    *p = ChainPend{};
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u || !g_start_action || !g_plya_select || !guest_ok(actor, 0x1070u)) return;
    const uint32_t now = chain_running(rdram, actor);
    if (now != q.cur) {
        if (chain_say()) { g_chain_logs++; std::fprintf(stderr, "[CHAIN] actor %08X: action %d dropped, running record %08X -> %08X since the window\n", actor, q.act, q.cur, now); }
        return;
    }
    const uint8_t gate = rdram[(actor + ACT_GATE) & 0x07FFFFFFu];
    const recomp_context after = *ctx;                  // what the bone pass hands back to its caller
    ctx->r[4] = static_cast<int32_t>(actor); ctx->r[5] = LAND_KIND; ctx->r[6] = 0;
    g_force_actor = actor; g_force_rec = q.rec; g_force_table = q.table;
    g_start_action(rdram, ctx);
    g_force_actor = g_force_rec = g_force_table = 0u;
    const uint32_t ok = static_cast<uint32_t>(ctx->r[2]);
    *ctx = after;
    if (chain_say()) {
        g_chain_logs++;
        std::fprintf(stderr, "[CHAIN] actor %08X: chain window -> action %d (record %08X) %s, v0 %u, gate %02X, now +0x101C %08X\n",
                     actor, q.act, q.rec, ok ? "started" : "NOT started", ok, gate, guest_rd<uint32_t>(rdram, actor + ACT_RUN));
    }
    if (ok) {                                           // hitcam=: the camera frames the hit-confirmed follow-up
        const Entry* he = hit_entry(rdram, actor);
        if (he && he->hit_cam > 0 && he->hit_act == q.act) {
            g_dissidia_cine_actor = actor; g_dissidia_cine_ticks = he->hit_cam; g_dissidia_cine_seq++;
            if (chain_say()) { g_chain_logs++; std::fprintf(stderr, "[HITCAM] actor %08X: action %d framed for %d ticks\n", actor, q.act, he->hit_cam); }
        }
    }
}
// Hit-confirmed follow-up (roster.txt `hitact=MAGI,ACTION`, e.g. `34 noc100 9 ... hitact=2277,3029`: Alterna,
// 2026-10-22: the charge's flash does the first brave damage; only if it hits does Noctis go on into the warp back and
// the cast). Data alone cannot branch an action on a bullet's hit: a magi's link_hit only spawns another magi (MAGI_NOTES
// §4, FUN_08AD37A8 -> FUN_08AD2B84 -> FUN_08AD0978), and a timeline chain window (op 0x0467) never starts its action
// by itself (chainact=). So the flash's on-hit child (magi MAGI) is the signal: every magi emitter is created by
// FUN_08AD0978(a0 record id, a1 owner actor, ...) (the timeline's op 0x0204 through FUN_08836CA8, and every link child
// with a1 = the parent emitter's owner, emitter +0x108). The wrapper runs the engine function, then, when the record is
// the token's MAGI and the owner is this character, remembers {actor, its own plya record of ACTION, the record running
// now} in chainact's pending list; the actor's next bone pass starts it with FUN_08B382B8(actor, 1, 0) and the selector
// override (chain_tick), dropped if the running record changed in between (he was hit). A miss spawns no child, so the
// action (A) just runs to its end. PSPRECOMP_CHAIN_LOG=1 logs every spawn seen (the first few are always logged).
constexpr uint32_t MAGI_SPAWN = 0x08AD0978u;     // battle overlay: create a magi emitter (a0 record id, a1 owner actor)
FuncPtr g_magi_spawn = nullptr;
const Entry* hit_entry(const uint8_t* rdram, uint32_t actor) {
    if (!guest_ok(actor, 0x1070u)) return nullptr;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return nullptr;
    const int cid = static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]);
    for (const Entry& e : g_entries) if (e.charId == cid && e.hit_magi >= 0) return &e;
    return nullptr;
}
void hit_spawn_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) {                                    // not the battle overlay: its own code at this address, if any
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(MAGI_SPAWN) : nullptr) f(rdram, ctx);
        return;
    }
    const int id = static_cast<int>(static_cast<uint32_t>(ctx->r[4]) & 0xFFFFu);
    const uint32_t owner = static_cast<uint32_t>(ctx->r[5]);
    g_magi_spawn(rdram, ctx);
    const Entry* e = hit_entry(rdram, owner);
    if (!e || id != e->hit_magi) return;
    uint32_t table = 0;
    const uint32_t own = plya_record(rdram, owner, e->hit_act, &table);
    if (!own) {
        if (chain_say()) { g_chain_logs++; std::fprintf(stderr, "[HITACT] actor %08X: magi %d hit, no action %d in its own table: not started\n", owner, id, e->hit_act); }
        return;
    }
    ChainPend* p = nullptr;
    for (ChainPend& x : g_chain_pend) if (x.actor == owner) { p = &x; break; }
    if (!p) for (ChainPend& x : g_chain_pend) if (!x.actor) { p = &x; break; }
    if (!p) p = &g_chain_pend[0];
    p->actor = owner; p->rec = own; p->table = table; p->act = e->hit_act; p->cur = chain_running(rdram, owner);
    if (chain_say()) { g_chain_logs++; std::fprintf(stderr, "[HITACT] actor %08X: magi %d spawned (on-hit), action %d queued for the next bone pass\n", owner, id, e->hit_act); }
}
void chain_install() {
    if (!chain_wanted()) return;
    g_chain_log = std::getenv("PSPRECOMP_CHAIN_LOG") != nullptr;
    g_chain_add = ovl_battle_lookup(CHAIN_ADD);
    if (!g_start_action) g_start_action = ovl_battle_lookup(START_ACTION);
    if (!g_plya_select) g_plya_select = psp_dispatch_probe_lookup(PLYA_SELECT);
    if (!g_chain_add || !g_start_action || !g_plya_select) {
        std::fprintf(stderr, "[ROSTER] chain window hook NOT installed (chain add %s, start action %s, selector %s)\n",
                     g_chain_add ? "ok" : "missing", g_start_action ? "ok" : "missing", g_plya_select ? "ok" : "missing");
        return;
    }
    psp_dispatch_register(PLYA_SELECT, plya_select_hook);
    psp_dispatch_register(CHAIN_ADD, chain_add_hook);
    bool hit_any = false;
    for (const Entry& e : g_entries) if (e.hit_magi >= 0) hit_any = true;
    if (hit_any) {
        g_magi_spawn = ovl_battle_lookup(MAGI_SPAWN);
        if (!g_magi_spawn) std::fprintf(stderr, "[ROSTER] hitact hook NOT installed (no battle overlay function at %08X)\n", MAGI_SPAWN);
        else {
            psp_dispatch_register(MAGI_SPAWN, hit_spawn_hook);
            for (const Entry& e : g_entries)
                if (e.hit_magi >= 0)
                    std::fprintf(stderr, "[ROSTER] charId %d: spawning magi %d (an on-hit child) starts action %d%s\n", e.charId, e.hit_magi, e.hit_act,
                                 e.hit_cam > 0 ? " (framed by the camera, hitcam=)" : "");
        }
    }
    for (const Entry& e : g_entries)
        if (e.chain_act[0] >= 0)
            std::fprintf(stderr, "[ROSTER] charId %d: a chain window (timeline op 0x0467) to action %d %d %d %d starts it at once\n",
                         e.charId, e.chain_act[0], e.chain_act[1], e.chain_act[2], e.chain_act[3]);
}
// EX Mode extras per character (roster.txt tokens on the character's line):
//   exranged=N                                   ranged immunity while in EX Mode (N = 1 or 2, 0 = off)
//   projblock=MOTION[/MOTION...],FROM,TO[,BONE:FEPA[/...]]  projectiles blocked between two ticks of a motion (see can_hit_hook)
//   armiger=ORBIT,FLOOR,FLASH,RINGDRAW,FXDRAW[,BODYDRAW]  exarms=ARM0,NARM[,FEPA0]   animate EX-only GMO parts (Noctis's
//            crystal royal arms); ARM0..ARM0+NARM-1 = one skin bone per arm (each spins), FEPA0+k = the deflect
//            sparkle played on arm k when a projectile is deflected
// e.g. `34 noc100 9 ... nm=Noctis exranged=2 armiger=157,158,159,73,74,14 exarms=181,11,510`. Nothing is hooked without
// these (exarms= needs armiger=).
//
// EX Mode state: actor +0x5B2 bit 2 (FUN_08ACAFAC sets it when EX starts, FUN_08ACA77C clears it; every EX trait
// getter FUN_08B343D0.. tests it through FUN_08B34378, which also needs the chrp record at actor +0x5A4).
// charId: actor +0x2F8 -> objentry record, byte +6 (as dash_move_hook); chrp record byte 0 is the charId too.
//
// exranged. Every hit is first checked by FUN_08807D4C(hit object, defender) (EBOOT; callers FUN_08809CEC, the
// per-defender contact pass, and FUN_088080C4, the melee hitbox callback); 0 = this hit object may not hit this
// defender (invulnerable, already hit, same side). Hit objects (base FUN_08807860): u16 +0x2C kind = 1 melee hitbox
// (timeline 0x0200, FUN_0880B8E4), 2 projectile (magi bullet hitbox, FUN_0880B05C from FUN_08AD9CE4 / FUN_08AD7DC4),
// 3 stage hazard (FUN_0880B768); +0x18 / +0x20 the attacking object, whose u8 +0x542 is 3 for summoned / thrown
// objects (wpnp objects: Yuna's aeons, Bartz's thrown weapons; owner at +0x5D4). While the defender is in EX Mode:
// N >= 1 projectiles pass through him (no damage, no stagger, no reflect); N >= 2 hits from summoned / thrown
// objects as well. Melee, direct EX Burst damage (FUN_08808AE8 applies it without this check) and everything
// outside EX Mode are unchanged.
//
// armiger. The character's system.bin chrp row lists the draw bones shown only in EX Mode (+0x3C; Noctis inherits
// Bartz's [73 wep17, 74 wep18], both also in the objentry weapon list) and FUN_08ACAC60 shows / hides them when EX
// starts / ends. That alone did not keep them hidden before the first EX Mode (in game the ring stood around him from
// the battle start), so outside EX Mode (actor +0x5B2 bit 2 clear) this hook keeps both hidden itself, and gives them
// back once when EX Mode starts if it was the one hiding them. It also animates them after the engine posed the model:
// FUN_0880EA20(spring bones, gmo model, actor) is the last pass that writes bone matrices (cape physics, through
// FUN_089B11CC) before the model is drawn. Bone matrices are model space: libgmo model = *(*(actor+0x394)+0x0C),
// bones at +0x0C (0x100 bytes each, count u16 at +0x20), matrix at bone +0x80 (16 floats, translation [12..14]);
// actor +0x330 is the model -> world matrix (a copy of actor +0x00, FUN_088344C8). Visibility table at
// *(actor+0x394)+0x10C, count u8 at +0x8E, {draw bone, shown, alpha} per Part (FUN_0886CB64); alpha 0xFF = opaque
// pass, 1..0xFE = translucent pass with that alpha (FUN_0886ADBC), 0 = not drawn.
//   ORBIT  bone the ring of arms is skinned to (rest at ring height, scale 1): turned about the vertical at a steady
//          rate in world space (independent of motions and of his facing), following the root bone; when the
//          parts appear it rises, scales up and spins up, fading in.
//   FLOOR / FLASH  bones of the activation pieces (rest scale 0 = invisible unless animated here): a floor ring of
//          light that spreads out and a light pillar, during the first kFxEnd seconds.
//   RINGDRAW / FXDRAW  their draw bones (EX-listed), whose visibility entries carry the alpha.
//   BODYDRAW (optional) the body's draw bone: the body turns translucent for a moment inside the light pillar (the
//          FFXV flash); its visibility entry is added if missing ({bone, 1, 0xFF} = drawn as before) and its alpha
//          is always put back to 0xFF.
//   ARM0.. (optional) one bone per arm, rest pose = the arm's place in the ring (model space, same frame as ORBIT's
//          rest). While he is not in EX Mode the hook reads each arm's rest placement relative to ORBIT (L = arm x
//          ORBIT^-1, row-vector matrices); in EX Mode it writes arm = spin(own long axis +Y) x L x ORBIT, so the wheel
//          turns and every weapon spins about its own axis. Spin and orbit speed up while he moves (ground speed from
//          the model -> world matrix, smoothed) and in a short burst when a projectile is deflected.
//   FEPA0  (optional) a projectile stopped by exranged in EX Mode plays FEPA record FEPA0+k (k = the arm facing
//          the attacker) through FUN_088A9DE8(*(*(actor+0x390)+0x5C), id, actor+0x4C8, 0, 0), the call the timeline
//          event 0x0219 makes (FUN_0882CEC0 case 25); at most once per kDeflectCool ticks.
// Without this hook the ring simply stands around him, opaque, and the activation pieces stay invisible.
constexpr uint32_t CAN_HIT = 0x08807D4Cu;        // EBOOT FUN_08807D4C(hit object, defender) -> 0 = cannot hit
constexpr uint32_t BONE_POST = 0x0880EA20u;      // EBOOT FUN_0880EA20(spring data, gmo model, actor)
constexpr uint32_t EX_ACTOR_INFO = 0x2F8u;       // actor -> objentry record (byte +6 charId)
constexpr uint32_t EX_ACTOR_CHRP = 0x5A4u;       // actor -> chrp record (byte 0 charId)
constexpr uint32_t EX_ACTOR_FLAGS = 0x5B2u;      // bit 2: EX Mode
constexpr uint32_t EX_ACTOR_KIND = 0x542u;       // 3 = summoned / thrown object
constexpr uint32_t EX_ACTOR_MODEL = 0x394u;      // model controller
constexpr uint32_t EX_ACTOR_WORLD = 0x330u;      // model -> world matrix
constexpr int kRootBone = 2;
constexpr float kPi = 3.14159265f;
constexpr float kOrbitDegPerSec = 40.0f;         // steady orbit
constexpr float kSpinDeg = 300.0f, kSpinTau = 0.28f;   // extra spin when the arms appear, decaying
constexpr float kRingDrop = 0.13f;               // ring centre = root height - this (0.95 m at rest)
constexpr float kRiseFrom = 0.35f, kRiseTime = 0.40f, kGrowTime = 0.35f;
constexpr int kRingAlpha = 205;                  // crystal translucency once settled
constexpr float kRingFadeIn = 0.22f;
constexpr float kFxEnd = 0.70f, kFxHold = 0.15f; // activation pieces: full for kFxHold, then fade out by kFxEnd
constexpr int kFxAlpha = 235;
constexpr float kFloorFrom = 0.35f, kFloorTo = 1.9f, kFloorTime = 0.50f;     // floor ring radius scale (0.85 m stored)
constexpr float kPillarFrom = 0.55f, kPillarTo = 1.25f, kPillarTime = 0.45f; // pillar radius scale (0.55 m stored)
constexpr float kFloorY = 0.03f, kPillarHalf = 1.15f;
constexpr float kBodyIn = 0.06f, kBodyHold = 0.26f, kBodyOut = 0.46f;     // body alpha 255 -> kBodyAlpha -> 255
constexpr int kBodyAlpha = 96;
constexpr float kArmSpinIdle = 150.0f, kArmSpinRun = 260.0f, kArmSpinBurst = 1080.0f;   // deg/s about each arm's axis
constexpr float kOrbitRun = 1.3f, kOrbitBurst = 1.6f;   // orbit speed factors: x1.3 running, +x1.6 at a deflect
constexpr float kRunFrom = 0.03f, kRunFull = 0.10f;     // ground speed (units / tick) where "running" starts / is full
constexpr float kRunSmooth = 8.0f;                      // ticks
constexpr float kBurstTau = 0.45f;                      // s, decay of the deflect burst
constexpr float kDeflectCool = 10.0f;                   // ticks between deflect sparkles
constexpr uint32_t PLAY_FX = 0x088A9DE8u;               // EBOOT FUN_088A9DE8(fx manager, FEPA id, actor+0x4C8, 0, 0)

FuncPtr g_orig_can_hit = nullptr, g_orig_bone_post = nullptr, g_play_fx = nullptr;

inline bool ex_ok(uint32_t a, uint32_t n) { return a >= 0x08000000u && a < 0x0C000000u && n <= 0x0C000000u - a; }
inline uint8_t* ex_p(uint8_t* r, uint32_t a) { return r + (a & 0x07FFFFFFu); }
inline uint32_t ex_r32(uint8_t* r, uint32_t a) { uint32_t v; std::memcpy(&v, ex_p(r, a), 4); return v; }
inline uint16_t ex_r16(uint8_t* r, uint32_t a) { uint16_t v; std::memcpy(&v, ex_p(r, a), 2); return v; }
inline float ex_rf(uint8_t* r, uint32_t a) { float v; std::memcpy(&v, ex_p(r, a), 4); return v; }

// The roster entry of a main character actor (not a summoned object), or null.
const Entry* ex_entry(uint8_t* r, uint32_t actor) {
    if (!ex_ok(actor, 0x600)) return nullptr;
    const uint32_t info = ex_r32(r, actor + EX_ACTOR_INFO), chrp = ex_r32(r, actor + EX_ACTOR_CHRP);
    if (!ex_ok(info, 8) || !ex_ok(chrp, 4)) return nullptr;
    const int cid = static_cast<int8_t>(*ex_p(r, info + 6u));
    if (*ex_p(r, chrp) != static_cast<uint8_t>(cid) || *ex_p(r, actor + EX_ACTOR_KIND) == 3) return nullptr;
    for (const Entry& e : g_entries) if (e.charId == cid) return &e;
    return nullptr;
}
bool ex_active(uint8_t* r, uint32_t actor) { return (*ex_p(r, actor + EX_ACTOR_FLAGS) & 4) != 0; }

bool ex_ranged_immune(uint8_t* r, uint32_t hit, uint32_t def) {
    if (!ex_ok(hit, 0x80)) return false;
    const Entry* e = ex_entry(r, def);
    if (!e || e->exranged <= 0 || !ex_active(r, def)) return false;
    if (ex_r16(r, hit + 0x2Cu) == 2) return true;                                   // projectile
    if (e->exranged >= 2)
        for (uint32_t off : {0x18u, 0x20u}) {                                        // summoned / thrown object
            const uint32_t a = ex_r32(r, hit + off);
            if (a != def && ex_ok(a, 0x600) && *ex_p(r, a + EX_ACTOR_KIND) == 3) return true;
        }
    return false;
}
// projblock=MOTION[/MOTION...],FROM,TO[,BONE:FEPA[/BONE:FEPA...]] (any number of tokens): while one of MOTIONS plays on
// this character and its motion time is in [FROM, TO] ticks, projectiles (hit object kind 2) and hits of summoned / thrown
// objects (attacker kind 3) cannot hit him: FUN_08807D4C returns 0, as exranged=2 does in EX Mode (the projectile passes
// through: no damage, no stagger, no reflect). Melee is unchanged; brave and HP projectiles alike. Motion index = u16 +2
// of the p_se script row at actor +0x17C (as warp_step_hook); motion time = actor +0x1A8 in timeline ticks (60 / s; the
// ranged event dispatcher FUN_08829930 compares each event's start / end tick with it). A blocked hit plays the listed
// FEPA whose BONE faces the attacker (shown bones first, i.e. scale > 0) through FUN_088A9DE8, as ex_deflect does, at
// most once per kDeflectCool ticks of motion time. Noctis: projblock=70/68,8,78,169:541/... = Apocalypsis (ground
// `_753A` / air `_751A`) from the first arm appearing (frame 4) to the volley's launch (0x0204 at tick 78); FEPA 541-546
// are the blue weapon sparkles on apoc_w01/02/04/06/08/10. PSPRECOMP_PROJBLOCK_LOG=1 logs every block (else the first 5).
bool g_pb_log = false;
int g_pb_logs = 0;
struct PbState { uint32_t actor = 0; int motion = -1; float last = -1e9f; };
PbState g_pb[4];
int g_pb_next = 0;
int pb_motion(uint8_t* r, uint32_t actor) {
    if (!ex_ok(actor, 0x1B0)) return -1;
    const uint32_t row = ex_r32(r, actor + 0x17Cu);
    const int16_t script = static_cast<int16_t>(ex_r16(r, actor + 0x180u));
    if (script < 0 || !ex_ok(row, 8) || ex_r16(r, row) != static_cast<uint16_t>(script)) return -1;
    return ex_r16(r, row + 2u);
}
const ProjBlock* proj_block_active(uint8_t* r, uint32_t def, int* motion, float* t) {
    const Entry* e = ex_entry(r, def);
    if (!e || e->projblocks.empty()) return nullptr;
    const int m = pb_motion(r, def);
    if (m < 0) return nullptr;
    const float mt = ex_rf(r, def + 0x1A8u);
    if (!std::isfinite(mt)) return nullptr;
    for (const ProjBlock& b : e->projblocks)
        for (int mm : b.motions)
            if (mm == m && mt >= b.from && mt <= b.to) { *motion = m; *t = mt; return &b; }
    return nullptr;
}
bool proj_block_kind(uint8_t* r, uint32_t hit, uint32_t def) {
    if (ex_r16(r, hit + 0x2Cu) == 2) return true;                                  // projectile
    for (uint32_t off : {0x18u, 0x20u}) {                                            // summoned / thrown object
        const uint32_t a = ex_r32(r, hit + off);
        if (a != def && ex_ok(a, 0x600) && *ex_p(r, a + EX_ACTOR_KIND) == 3) return true;
    }
    return false;
}
// the deflect sparkle: the listed bone (shown ones first) whose place round him faces the attacker
int proj_block_fx(uint8_t* r, uint32_t def, uint32_t hit, const ProjBlock& b, int motion, float t, recomp_context* ctx) {
    PbState* s = nullptr;
    for (PbState& p : g_pb) if (p.actor == def) s = &p;
    if (!s) { s = &g_pb[g_pb_next++ & 3]; *s = PbState{}; s->actor = def; }
    const bool fresh = s->motion != motion || t < s->last;          // another motion, or it started again
    if (!fresh && t - s->last < kDeflectCool) return -1;
    if (b.nfx <= 0 || !g_play_fx) return -1;
    const uint32_t fxp = ex_r32(r, def + 0x390u);
    if (!ex_ok(fxp, 0x60)) return -1;
    const uint32_t mgr = ex_r32(r, fxp + 0x5Cu);
    if (!ex_ok(mgr, 4)) return -1;
    float W[16];
    for (int i = 0; i < 16; i++) W[i] = ex_rf(r, def + EX_ACTOR_WORLD + 4u * static_cast<uint32_t>(i));
    float dx = W[8], dz = W[10];                                    // no attacker position: his front
    const uint32_t att = ex_r32(r, hit + 0x18u);
    if (att != def && ex_ok(att, 0x100)) {
        const float ax = ex_rf(r, att + 0x80u) - ex_rf(r, def + 0x80u), az = ex_rf(r, att + 0x88u) - ex_rf(r, def + 0x88u);
        if (std::isfinite(ax) && std::isfinite(az) && ax * ax + az * az > 1e-6f) { dx = ax; dz = az; }
    }
    const uint32_t ctrl = ex_r32(r, def + EX_ACTOR_MODEL);
    const uint32_t gm = ex_ok(ctrl, 0x110) ? ex_r32(r, ctrl + 0x0Cu) : 0;
    int best = 0; float bd = -1e30f;
    if (ex_ok(gm, 0x24))
        for (int k = 0; k < b.nfx; k++) {
            const uint32_t at = ex_bone_mat(r, gm, b.fx_bone[k]);
            if (!at) continue;
            float A[16]; std::memcpy(A, ex_p(r, at), 64);
            const float sc = A[0] * A[0] + A[1] * A[1] + A[2] * A[2];
            const float ax = A[12] * W[0] + A[13] * W[4] + A[14] * W[8], az = A[12] * W[2] + A[13] * W[6] + A[14] * W[10];
            const float d = ax * dx + az * dz + (sc > 1e-4f ? 1e6f : 0.0f);
            if (d > bd) { bd = d; best = k; }
        }
    s->motion = motion; s->last = t;
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(mgr); ctx->r[5] = b.fx_id[best]; ctx->r[6] = static_cast<int32_t>(def + 0x4C8u);
    ctx->r[7] = 0; ctx->r[8] = 0;
    g_play_fx(r, ctx);
    *ctx = saved;
    return b.fx_id[best];
}
void ex_deflect(uint8_t* r, uint32_t def, uint32_t hit, recomp_context* ctx);
void can_hit_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t hit = static_cast<uint32_t>(ctx->r[4]), def = static_cast<uint32_t>(ctx->r[5]);
    if (ex_ranged_immune(rdram, hit, def)) {
        ex_deflect(rdram, def, hit, ctx);
        ctx->r[2] = 0;
        return;
    }
    int motion = -1; float t = 0.0f;
    if (ex_ok(hit, 0x80) && proj_block_kind(rdram, hit, def))
        if (const ProjBlock* b = proj_block_active(rdram, def, &motion, &t)) {
            const int fx = proj_block_fx(rdram, def, hit, *b, motion, t, ctx);
            if (g_pb_log || g_pb_logs < 5) {
                g_pb_logs++;
                std::fprintf(stderr, "[PROJBLOCK] actor %08X motion %d tick %.1f: %s %08X (kind %u, from %08X) blocked%s%d\n", def, motion, t,
                             ex_r16(rdram, hit + 0x2Cu) == 2 ? "projectile" : "summoned/thrown hit", hit, ex_r16(rdram, hit + 0x2Cu),
                             ex_r32(rdram, hit + 0x18u), fx >= 0 ? ", sparkle FEPA " : ", no sparkle ", fx);
            }
            ctx->r[2] = 0;
            return;
        }
    g_orig_can_hit(rdram, ctx);
}

struct ArmState { uint32_t actor = 0; bool shown = false; bool body_dim = false; bool forced = false; float ticks = 0.0f; uint32_t seen = 0;
                  float orbit = 0.0f, spin = 0.0f, move = 0.0f, burst = 0.0f, cool = 0.0f, last_x = 0.0f, last_z = 0.0f;
                  bool has_last = false, have_L = false, wrote = false; float L[16][16], last0[16];
                  float ring_away = 1.0f, away_t = 1e9f; };   // 1 = ring out, 0 = given way to a move's own arms (ex_summons_out)
ArmState g_arm[8];
uint32_t g_arm_clock = 0;
ArmState* arm_state(uint32_t actor) {
    ArmState* free_s = nullptr;
    for (ArmState& s : g_arm) {
        if (s.actor == actor) return &s;
        if (!free_s || s.seen < free_s->seen) free_s = &s;
    }
    *free_s = ArmState{}; free_s->actor = actor;
    return free_s;
}
float ease_out(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// FUN_08838300: the actor's time step this frame, in 60 fps ticks
float ex_actor_dt(uint8_t* r, uint32_t actor) {
    const uint32_t m0 = ex_r32(r, 0x089C7FD8u), m1 = ex_r32(r, 0x089C7FDCu);
    const bool own = ((ex_r32(r, actor + 0x388u) & m0) | (ex_r32(r, actor + 0x38Cu) & m1)) != 0;
    float dt = own ? ex_rf(r, actor + 0x3CCu) : ex_rf(r, actor + 0x3C8u) * ex_rf(r, 0x089D5EACu);
    if (!(dt >= 0.0f) || dt > 8.0f) dt = 0.0f;
    return dt;
}
uint32_t ex_vis_entry(uint8_t* r, uint32_t ctrl, int bone) {
    const uint32_t tab = ex_r32(r, ctrl + 0x10Cu);
    const unsigned n = *ex_p(r, ctrl + 0x8Eu);
    if (!ex_ok(tab, 3u * n)) return 0;
    for (unsigned i = 0; i < n; i++) if (*ex_p(r, tab + 3u * i) == static_cast<uint8_t>(bone)) return tab + 3u * i;
    return 0;
}
// FUN_0886CB64's add path: first free entry (bone 0xFF) becomes {bone, shown 1, alpha 0xFF}
uint32_t ex_vis_add(uint8_t* r, uint32_t ctrl, int bone) {
    const uint32_t tab = ex_r32(r, ctrl + 0x10Cu);
    const unsigned n = *ex_p(r, ctrl + 0x8Eu);
    if (bone < 0 || bone > 0xFE || !ex_ok(tab, 3u * n)) return 0;
    for (unsigned i = 0; i < n; i++)
        if (*ex_p(r, tab + 3u * i) == 0xFF) {
            uint8_t* p = ex_p(r, tab + 3u * i);
            p[0] = static_cast<uint8_t>(bone); p[1] = 1; p[2] = 0xFF;
            return tab + 3u * i;
        }
    return 0;
}
void body_restore(uint8_t* r, uint32_t ctrl, const Entry* e, ArmState* s) {
    if (!s->body_dim) return;
    s->body_dim = false;
    const uint32_t bv = ex_vis_entry(r, ctrl, e->armiger[5]);
    if (bv && ex_p(r, bv)[1] == 1) ex_p(r, bv)[2] = 0xFF;
}
uint32_t ex_bone_mat(uint8_t* r, uint32_t gm, int idx) {
    if (idx < 0 || idx >= static_cast<int>(ex_r16(r, gm + 0x20u))) return 0;
    const uint32_t a = ex_r32(r, gm + 0x0Cu) + 0x100u * static_cast<uint32_t>(idx) + 0x80u;
    return ex_ok(a, 64) ? a : 0;
}
// rows 0..2 of the model -> world rotation, normalised; false if it is not upright (then no facing correction)
bool ex_world_rot(uint8_t* r, uint32_t actor, float R[9]) {
    for (int i = 0; i < 3; i++) {
        float l = 0;
        for (int j = 0; j < 3; j++) { R[3 * i + j] = ex_rf(r, actor + EX_ACTOR_WORLD + 16u * i + 4u * j); l += R[3 * i + j] * R[3 * i + j]; }
        if (!(l > 1e-8f) || l > 1e8f) return false;
        l = 1.0f / std::sqrt(l);
        for (int j = 0; j < 3; j++) R[3 * i + j] *= l;
    }
    return std::fabs(R[4]) > 0.98f;                // Y row along +-Y: yaw only
}
void put_mat(uint8_t* r, uint32_t at, const float M3[9], float sx, float sy, float sz, float tx, float ty, float tz) {
    const float s[3] = {sx, sy, sz};
    float m[16];
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) m[4 * i + j] = M3[3 * i + j] * s[i]; m[4 * i + 3] = 0.0f; }
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1.0f;
    std::memcpy(ex_p(r, at), m, sizeof m);
}

// 4x4 row-vector matrices as stored in libgmo bone records (translation in [12..14]): v' = v * M
void mat_mul(const float* A, const float* B, float* C) {
    float T[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            T[4 * i + j] = A[4 * i] * B[j] + A[4 * i + 1] * B[4 + j] + A[4 * i + 2] * B[8 + j] + A[4 * i + 3] * B[12 + j];
    std::memcpy(C, T, sizeof T);
}
bool mat_inv_affine(const float* M, float* O) {          // [R 0; t 1]^-1 = [R^-1 0; -t R^-1 1]
    const float a = M[0], b = M[1], c = M[2], d = M[4], e = M[5], f = M[6], g = M[8], h = M[9], k = M[10];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-9f)) return false;
    const float id = 1.0f / det;
    float R[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                  (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                  (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) O[4 * i + j] = R[3 * i + j]; O[4 * i + 3] = 0.0f; }
    for (int j = 0; j < 3; j++) O[12 + j] = -(M[12] * R[j] + M[13] * R[3 + j] + M[14] * R[6 + j]);
    O[15] = 1.0f;
    return true;
}
void read_mat(uint8_t* r, uint32_t at, float* M) { std::memcpy(M, ex_p(r, at), 64); }
int arm_count(uint8_t* r, uint32_t gm, const Entry* e) {
    if (e->armiger[6] < 0 || e->armiger[7] <= 0) return 0;
    const int n = std::min(e->armiger[7], 16);
    return ex_bone_mat(r, gm, e->armiger[6] + n - 1) ? n : 0;
}
// rest placement of each arm relative to the orbit bone (read while the hook is not writing them)
void arms_learn(uint8_t* r, uint32_t gm, const Entry* e, uint32_t m_orb, ArmState* s) {
    const int n = arm_count(r, gm, e);
    if (!n) return;
    float O[16], Oi[16], A[16];
    read_mat(r, ex_bone_mat(r, gm, e->armiger[6]), A);
    if (s->have_L && s->wrote && std::memcmp(A, s->last0, sizeof A) == 0) return;   // still our own pose: not re-posed
    read_mat(r, m_orb, O);
    if (!mat_inv_affine(O, Oi)) return;
    for (int k = 0; k < n; k++) { read_mat(r, ex_bone_mat(r, gm, e->armiger[6] + k), A); mat_mul(A, Oi, s->L[k]); }
    s->have_L = true; s->wrote = false;
}
// arm k = spin about its own +Y x L_k x the orbit matrix just written
void arms_write(uint8_t* r, uint32_t gm, const Entry* e, uint32_t m_orb, ArmState* s) {
    const int n = arm_count(r, gm, e);
    if (!n || !s->have_L) return;
    float O[16], M[16];
    read_mat(r, m_orb, O);
    for (int k = 0; k < n; k++) {
        const float a = (s->spin + 33.0f * k) * kPi / 180.0f, c = std::cos(a), sn = std::sin(a);
        const float S[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, 0, 0, 0, 1};
        mat_mul(S, s->L[k], M);
        mat_mul(M, O, M);
        std::memcpy(ex_p(r, ex_bone_mat(r, gm, e->armiger[6] + k)), M, 64);
        if (k == 0) { std::memcpy(s->last0, M, 64); s->wrote = true; }
    }
}
// PSPRECOMP_VIS_LOG=1: log the draw-bone visibility table (controller +0x10C, n at +0x8E, {bone, shown, alpha} per
// Part) whenever it changes, with the current script id (actor +0x180), to find draw bones that never get an entry.
void vis_log(uint8_t* r, uint32_t actor, uint32_t ctrl) {
    static const bool on = std::getenv("PSPRECOMP_VIS_LOG") != nullptr;
    if (!on) return;
    static uint8_t last[256 * 3]; static unsigned last_n = 0; static uint32_t last_actor = 0;
    const uint32_t tab = ex_r32(r, ctrl + 0x10Cu);
    const unsigned n = *ex_p(r, ctrl + 0x8Eu);
    if (!n || !ex_ok(tab, 3u * n)) return;
    const uint8_t* t = ex_p(r, tab);
    if (actor == last_actor && n == last_n && std::memcmp(last, t, 3u * n) == 0) return;
    std::memcpy(last, t, 3u * n); last_n = n; last_actor = actor;
    unsigned used = 0;
    std::string line;
    char buf[32];
    for (unsigned i = 0; i < n; i++) {
        if (t[3 * i] == 0xFF) continue;
        used++;
        std::snprintf(buf, sizeof buf, " %u:%u%s", t[3 * i], t[3 * i + 1], t[3 * i + 2] == 0xFF ? "" : (t[3 * i + 2] ? "a" : "z"));
        line += buf;
    }
    std::fprintf(stderr, "[VIS] actor %08X script %u: %u/%u used:%s\n", actor, ex_r32(r, actor + 0x180u), used, n, line.c_str());
}
// EX Mode ring gives way to a move's own summoned arms (Apocalypsis, Armiger Assault / Onslaught: the apocarms= sets)
constexpr float kRingAwayOut = 6.0f, kRingAwayBack = 18.0f;   // ticks to shrink away / grow back
constexpr float kRingAwayHold = 20.0f;                        // ticks with no summoned arm out before it returns
bool ex_summons_out(uint8_t* r, uint32_t ctrl, uint32_t gm, const Entry* e, uint32_t actor);
void armiger_update(uint8_t* r, uint32_t actor) {
    const Entry* e = ex_entry(r, actor);
    if (!e || e->armiger[0] < 0) return;
    const uint32_t ctrl = ex_r32(r, actor + EX_ACTOR_MODEL);
    if (!ex_ok(ctrl, 0x110)) return;
    vis_log(r, actor, ctrl);
    const uint32_t gm = ex_r32(r, ctrl + 0x0Cu);
    if (!ex_ok(gm, 0x24)) return;
    const uint32_t m_orb = ex_bone_mat(r, gm, e->armiger[0]), m_floor = ex_bone_mat(r, gm, e->armiger[1]),
                   m_flash = ex_bone_mat(r, gm, e->armiger[2]), m_root = ex_bone_mat(r, gm, kRootBone);
    if (!m_orb || !m_floor || !m_flash || !m_root) return;               // e.g. the assist model: no such bones
    ArmState* s = arm_state(actor);
    s->seen = ++g_arm_clock;
    {   // EX Burst scripts (8000-8699): the burst motions pose the ring arms themselves (Dynastic Stance eruption in _802A)
        // and timeline 72 shows the ring parts; neither orbit them nor hide them here (22 Oct: "The weapons
        // shouldn't spin around him").
        constexpr int kBurstScript0 = 8000, kBurstScript1 = 8700;
        const int scr = static_cast<int16_t>(ex_r16(r, actor + 0x180u));
        if (scr >= kBurstScript0 && scr < kBurstScript1) { s->shown = false; return; }
    }
    if (!ex_active(r, actor)) {
        // Outside EX Mode the EX-only parts are hidden here, whatever the engine did: draw bone 73 is drawn from the
        // battle start (seen in game 2026-10-20: the ring stood around him before any EX Mode), so its entry is
        // added if missing and kept at {bone, 0, 0} (FUN_0886CB64's hide). EX start shows them again (FUN_08ACAC60).
        for (int k = 3; k <= 4; k++) {
            uint32_t v = ex_vis_entry(r, ctrl, e->armiger[k]);
            if (!v) v = ex_vis_add(r, ctrl, e->armiger[k]);
            if (v) { if (ex_p(r, v)[1] != 0) s->forced = true; ex_p(r, v)[1] = 0; ex_p(r, v)[2] = 0; }
        }
        body_restore(r, ctrl, e, s);
        s->shown = false;
        s->has_last = false; s->burst = 0.0f; s->cool = 0.0f;
        arms_learn(r, gm, e, m_orb, s);              // the engine left the rest poses: remember where each arm sits
        return;
    }
    if (s->forced) {
        // EX Mode started after this hook had hidden parts the engine considered shown: the engine may not show them
        // again (no change in its own lists), so give them back {bone, 1, 0xFF} once.
        s->forced = false;
        for (int k = 3; k <= 4; k++) {
            const uint32_t v = ex_vis_entry(r, ctrl, e->armiger[k]);
            if (v) { ex_p(r, v)[1] = 1; ex_p(r, v)[2] = 0xFF; }
        }
    }
    const uint32_t ring_vis = ex_vis_entry(r, ctrl, e->armiger[3]), fx_vis = ex_vis_entry(r, ctrl, e->armiger[4]);
    if (!ring_vis) return;
    if (*ex_p(r, ring_vis + 1) != 1) {                                    // EX parts hidden: leave the rest pose
        body_restore(r, ctrl, e, s);
        s->shown = false;
        return;
    }
    if (!s->shown) { s->shown = true; s->ticks = 0.0f; s->orbit = 0.0f; s->spin = 0.0f; if (!s->have_L) arms_learn(r, gm, e, m_orb, s); }
    const float dt = ex_actor_dt(r, actor), dts = dt / 60.0f;
    s->ticks += dt;
    const float t = s->ticks / 60.0f;
    if (ex_summons_out(r, ctrl, gm, e, actor)) s->away_t = 0.0f; else s->away_t += dt;
    if (s->away_t < kRingAwayHold) s->ring_away = std::max(0.0f, s->ring_away - dt / kRingAwayOut);
    else s->ring_away = std::min(1.0f, s->ring_away + dt / kRingAwayBack);
    // running: ground speed from the model -> world translation, smoothed; deflect burst decays
    const float px = ex_rf(r, actor + EX_ACTOR_WORLD + 48u), pz = ex_rf(r, actor + EX_ACTOR_WORLD + 56u);
    float run = 0.0f;
    if (s->has_last && dt > 0.0f) {
        const float v = std::sqrt((px - s->last_x) * (px - s->last_x) + (pz - s->last_z) * (pz - s->last_z)) / dt;
        run = clamp01((v - kRunFrom) / (kRunFull - kRunFrom));
    }
    s->last_x = px; s->last_z = pz; s->has_last = true;
    s->move += (run - s->move) * std::min(1.0f, dt / kRunSmooth);
    s->burst *= std::exp(-dts / kBurstTau);
    s->cool = std::max(0.0f, s->cool - dt);
    s->orbit += kOrbitDegPerSec * (1.0f + (kOrbitRun - 1.0f) * s->move + kOrbitBurst * s->burst) * dts;
    s->spin += (kArmSpinIdle + (kArmSpinRun - kArmSpinIdle) * s->move + kArmSpinBurst * s->burst) * dts;
    // orbit angle in world space, then cancel his facing so the ring does not jump when he turns
    float ang = (s->orbit + kSpinDeg * (1.0f - std::exp(-t / kSpinTau))) * kPi / 180.0f;
    float W[9];
    const float c = std::cos(ang), sn = std::sin(ang);
    const float Ry[9] = {c, 0, -sn, 0, 1, 0, sn, 0, c};
    float M3[9];
    if (ex_world_rot(r, actor, W)) {                 // M3 = Ry * W^T (W orthonormal)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M3[3 * i + j] = Ry[3 * i] * W[3 * j] + Ry[3 * i + 1] * W[3 * j + 1] + Ry[3 * i + 2] * W[3 * j + 2];
    } else {
        std::memcpy(M3, Ry, sizeof M3);
    }
    const float rx = ex_rf(r, m_root + 48), ry = ex_rf(r, m_root + 52), rz = ex_rf(r, m_root + 56);
    const float grow = (0.15f + 0.85f * ease_out(t / kGrowTime)) * ease_out(s->ring_away);
    put_mat(r, m_orb, M3, grow, grow, grow, rx, ry - kRingDrop - kRiseFrom * (1.0f - ease_out(t / kRiseTime)), rz);
    arms_write(r, gm, e, m_orb, s);
    ex_p(r, ring_vis)[2] = s->ring_away <= 0.0f ? 0 :
        static_cast<uint8_t>(t >= kRingFadeIn ? kRingAlpha : 1 + (kRingAlpha - 1) * clamp01(t / kRingFadeIn));
    if (t < kFxEnd && fx_vis && *ex_p(r, fx_vis + 1) == 1) {
        const float fr = kFloorFrom + (kFloorTo - kFloorFrom) * ease_out(t / kFloorTime);
        const float pr = kPillarFrom + (kPillarTo - kPillarFrom) * ease_out(t / kPillarTime);
        put_mat(r, m_floor, M3, fr, 1.0f, fr, rx, kFloorY, rz);
        put_mat(r, m_flash, M3, pr, 1.0f, pr, rx, kPillarHalf, rz);
        const float a = t < kFxHold ? 1.0f : 1.0f - clamp01((t - kFxHold) / (kFxEnd - kFxHold));
        ex_p(r, fx_vis)[2] = static_cast<uint8_t>(1 + (kFxAlpha - 1) * a);
    } else if (fx_vis) {
        ex_p(r, fx_vis)[2] = 0;                      // done: not drawn (the bones keep their rest scale 0 anyway)
    }
    if (e->armiger[5] >= 0 && t < kBodyOut) {        // the body flashes translucent inside the pillar
        uint32_t bv = ex_vis_entry(r, ctrl, e->armiger[5]);
        if (!bv) bv = ex_vis_add(r, ctrl, e->armiger[5]);
        if (bv && ex_p(r, bv)[1] == 1) {
            const float k = t < kBodyIn ? t / kBodyIn : (t < kBodyHold ? 1.0f : 1.0f - clamp01((t - kBodyHold) / (kBodyOut - kBodyHold)));
            ex_p(r, bv)[2] = static_cast<uint8_t>(255.0f - (255.0f - kBodyAlpha) * clamp01(k));
            s->body_dim = true;
        }
    } else {
        body_restore(r, ctrl, e, s);
    }
}
// Apocalypsis: the crystal royal arms fly as themselves (roster.txt `apocarms=ARM0,NARM,ID1[,ID2..ID8]`, e.g.
// `34 noc100 9 ... apocarms=168,12,2220,2222,2223,2228`). The move's GMO arms (skin bones ARM0..ARM0+NARM-1, Noctis:
// 168-179 `apoc_w00..11`, one royal arm each) swirl around him and burst out of their crown; each one vanishes (scale 0)
// at the point where a magi bullet of one of the records ID1.. (Noctis: brave volleys 2220 / 2222, HP greatsword 2223
// ground / 2228 air; homing, dodgeable) takes over. Without this hook only the bullets' own visual flies (a small white
// trail); with it each arm is put on its bullet every frame, so every weapon flies as itself, homing like the bullet.
//   Bullets: FUN_08AD20BC(visual, bullet) (battle overlay) builds the matrix of a bullet visual every frame (the
//            FX_FOLLOW_DIR / FX_ALIGN_FLOOR code; it returns at once for a visual that has ended). Bullet +0x00 =
//            position, +0x20 = direction (vec4, world), +0x64 -> the emitter, whose +0x24 is the magi record (u16 id at
//            +0, flags at +0x38). The wrapper calls it, then records position and direction of every bullet of a listed
//            record (pointer + record id; a bullet whose visual is no longer updated counts as gone after kApocLive
//            passes).
//   Arms:    in the bone pass after FUN_0880EA20 (bone_post_hook, as armiger / warpfx), each arm the motion shows
//            (scale > 0, not this pass's own matrix) is remembered (its model-space matrix). A listed bullet not yet
//            carried is given to the free arm shown within the last kApocSeen passes that lies nearest to it in model
//            space (at most kApocReach m away): the arm that just vanished at its hand-off point. That needs no slot
//            order or yaw-sign convention. While its bullet lives, the arm's matrix is written each frame: its last
//            pose turned by the smallest rotation that takes its blade axis (+Y, the tip) from the bullet's previous
//            direction to the current one, at the bullet's position (model space through the inverse of the model ->
//            world matrix at actor +0x330), scale 1. When the bullet is gone the arm is let go (if the bone still holds
//            this pass's matrix it is hidden: rows 0..2 = 0, i.e. its rest scale).
// No token, another character or no listed bullet: nothing changes (the arms keep the motion's pose: hidden after the
// hand-off).
// More arm sets and spin (2026-10-20, Armiger Onslaught's thrown Star of the Rogue): apocarms= may be given more than once
// on a line, each token one set of arms with the magi records they ride (e.g. `apocarms=168,12,2220,2222,2223,2228
// apocarms=81,1,2230`); a bullet only goes to an arm of a set that lists its record (the arm order is the sets' order).
// `pinspin=BONE,DEG,AX,AY,AZ`: while the arm with skin bone BONE rides a bullet it spins DEG degrees per bone pass about
// its own axis (AX,AY,AZ) (bone frame, e.g. a shuriken's face normal), and the arm's axis in that plane nearest its +Y
// (instead of +Y itself) is the one kept along the flight, so a shuriken flies edge-first, spinning flat.
constexpr uint32_t APOC_FXDIR = 0x08AD20BCu;     // battle overlay: per-frame matrix of a bullet's visual (a0 visual, a1 bullet)
constexpr uint32_t BUL_POS = 0x00u, BUL_DIR = 0x20u, BUL_EMIT = 0x64u, EMIT_REC = 0x24u;
constexpr int kApocBullets = 64, kApocActors = 4, kApocArms = 24, kApocSets = 4;
constexpr uint32_t kApocLive = 3u;              // passes without a visual update before a bullet counts as gone
constexpr uint32_t kApocSeen = 12u;             // an arm can take a bullet this many passes after the motion last showed it
constexpr float kApocReach = 3.0f;              // m (model space) between an arm's last shown place and its bullet
struct ApocBullet { uint32_t ptr = 0; uint16_t rec = 0; uint32_t seq = 0; float p[3] = {0, 0, 0}, d[3] = {0, 0, 1}; };
struct ApocArm { uint32_t bullet = 0; uint16_t rec = 0; uint32_t seen = 0; bool have_pose = false, wrote = false;
                 float pose[16] = {}, last[16] = {}, R[9] = {}, d[3] = {0, 1, 0};
                 float spin_deg = 0.0f, spin = 0.0f, ax[3] = {0, 0, 1}; };          // pinspin=: per pass, angle so far, local axis
struct ApocState { uint32_t actor = 0, used = 0; ApocArm arm[kApocArms]; };
ApocBullet g_apoc_bul[kApocBullets];
ApocState g_apoc_st[kApocActors];
uint32_t g_apoc_seq = 1;                        // bumped once per bone pass of an apocarms= actor
FuncPtr g_apoc_fxdir = nullptr;
bool g_apoc_log = false;
// the arm sets of an entry: the first apocarms= token, then the further ones
int apoc_sets_of(const Entry& e, ApocSet* s) {
    int n = 0;
    if (e.apoc_narm > 0 && e.apoc_nids > 0) {
        s[n] = ApocSet{}; s[n].arm0 = e.apoc_arm0; s[n].narm = e.apoc_narm; s[n].nids = e.apoc_nids;
        for (int i = 0; i < e.apoc_nids && i < 8; i++) s[n].ids[i] = e.apoc_ids[i];
        n++;
    }
    for (int k = 0; k < e.apoc_nmore && n < kApocSets; k++)
        if (e.apoc_more[k].narm > 0 && e.apoc_more[k].nids > 0) s[n++] = e.apoc_more[k];
    return n;
}
bool apoc_set_has(const ApocSet& s, uint16_t id) {
    for (int i = 0; i < s.nids; i++) if (s.ids[i] == id) return true;
    return false;
}
bool apoc_wanted() {
    ApocSet s[kApocSets];
    for (const Entry& e : g_entries) if (apoc_sets_of(e, s) > 0) return true;
    return false;
}
bool apoc_rec_wanted(uint16_t id) {
    ApocSet s[kApocSets];
    for (const Entry& e : g_entries) {
        const int n = apoc_sets_of(e, s);
        for (int k = 0; k < n; k++) if (apoc_set_has(s[k], id)) return true;
    }
    return false;
}
bool apoc_entry_has(const Entry* e, uint16_t id) {
    ApocSet s[kApocSets];
    const int n = apoc_sets_of(*e, s);
    for (int k = 0; k < n; k++) if (apoc_set_has(s[k], id)) return true;
    return false;
}
const PinSpin* apoc_spin_of(const Entry* e, int bone) {
    for (int i = 0; i < e->pin_nspin; i++) if (e->pin_spin[i].bone == bone) return &e->pin_spin[i];
    return nullptr;
}
bool apoc_finite3(const float* v) {
    for (int i = 0; i < 3; i++) if (!(v[i] == v[i]) || std::fabs(v[i]) > 1e5f) return false;
    return true;
}
void apoc_fxdir_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bullet = static_cast<uint32_t>(ctx->r[5]);
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) {                                    // not the battle overlay: its own code at this address, if any
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(APOC_FXDIR) : nullptr) f(rdram, ctx);
        return;
    }
    g_apoc_fxdir(rdram, ctx);
    if (!guest_ok(bullet, 0x70u)) return;
    const uint32_t em = guest_rd<uint32_t>(rdram, bullet + BUL_EMIT);
    if (!guest_ok(em, EMIT_REC + 4u)) return;
    const uint32_t rec = guest_rd<uint32_t>(rdram, em + EMIT_REC);
    if (!guest_ok(rec, 0x40u)) return;
    const uint16_t id = guest_rd<uint16_t>(rdram, rec);
    if (!apoc_rec_wanted(id)) return;
    float p[3], d[3];
    std::memcpy(p, rdram + ((bullet + BUL_POS) & 0x07FFFFFFu), sizeof p);
    std::memcpy(d, rdram + ((bullet + BUL_DIR) & 0x07FFFFFFu), sizeof d);
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!apoc_finite3(p) || !apoc_finite3(d) || !(dl > 1e-6f)) return;
    for (float& x : d) x /= dl;
    ApocBullet* slot = nullptr;
    for (ApocBullet& b : g_apoc_bul) if (b.ptr == bullet && b.rec == id) { slot = &b; break; }
    if (!slot) {                                        // a new bullet: a free slot, else the longest unseen
        for (ApocBullet& b : g_apoc_bul) if (!slot || b.ptr == 0 || (slot->ptr != 0 && b.seq < slot->seq)) { slot = &b; if (!b.ptr) break; }
        *slot = ApocBullet{}; slot->ptr = bullet; slot->rec = id;
        if (g_apoc_log) std::fprintf(stderr, "[APOC] bullet %08X record %u at (%.2f %.2f %.2f) dir (%.2f %.2f %.2f)\n", bullet, id, p[0], p[1], p[2], d[0], d[1], d[2]);
    }
    std::memcpy(slot->p, p, sizeof p); std::memcpy(slot->d, d, sizeof d);
    slot->seq = g_apoc_seq;
}
ApocState* apoc_state(uint32_t actor) {
    ApocState* free_s = nullptr;
    for (ApocState& s : g_apoc_st) {
        if (s.actor == actor) return &s;
        if (!free_s || s.used < free_s->used) free_s = &s;
    }
    *free_s = ApocState{}; free_s->actor = actor;
    return free_s;
}
// true while one of the entry's apocarms= arms is out: shown by the motion this pass (model-space scale > 0.05 and its
// draw bone, from the armtrail= set holding it, not hidden: Onslaught's star keeps scale 1 and is shown / hidden through
// draw bone 160) or riding a magi bullet (apoc_bone_post state; looked up, never allocated here)
bool ex_summons_out(uint8_t* r, uint32_t ctrl, uint32_t gm, const Entry* e, uint32_t actor) {
    ApocSet sets[kApocSets];
    const int ns = apoc_sets_of(*e, sets);
    for (int q = 0; q < ns; q++)
        for (int k = 0; k < sets[q].narm; k++) {
            const int bone = sets[q].arm0 + k;
            bool hidden = false;
            for (int si = 0; si < e->at_nset; si++)
                if (bone >= e->at_bone0[si] && bone < e->at_bone0[si] + e->at_count[si] && e->at_draw[si] >= 0) {
                    const uint32_t v = ex_vis_entry(r, ctrl, e->at_draw[si]);
                    hidden = v && ex_p(r, v)[1] != 1;
                }
            if (hidden) continue;
            const uint32_t m = ex_bone_mat(r, gm, bone);
            if (!m) continue;
            const float x = ex_rf(r, m), y = ex_rf(r, m + 4), z = ex_rf(r, m + 8);
            if (x * x + y * y + z * z > 0.0025f) return true;
        }
    for (const ApocState& st : g_apoc_st)
        if (st.actor == actor)
            for (const ApocArm& a : st.arm) if (a.bullet) return true;
    return false;
}
const ApocBullet* apoc_live(uint32_t ptr, uint16_t rec, uint32_t now) {
    for (const ApocBullet& b : g_apoc_bul)
        if (b.ptr == ptr && b.rec == rec) return now - b.seq <= kApocLive ? &b : nullptr;
    return nullptr;
}
// rotate v about the unit axis a by the angle with cosine c and sine s (Rodrigues)
void apoc_rot(float* v, const float* a, float c, float s) {
    const float k = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
    const float x[3] = {a[1] * v[2] - a[2] * v[1], a[2] * v[0] - a[0] * v[2], a[0] * v[1] - a[1] * v[0]};
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + a[i] * k * (1.0f - c);
}
// turn the rows of R (an orthonormal basis, row vectors) by the smallest rotation taking unit u to unit w
void apoc_turn(float* R, const float* u, const float* w) {
    float a[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    const float s = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), c = u[0] * w[0] + u[1] * w[1] + u[2] * w[2];
    if (s < 1e-6f) {
        if (c > 0.0f) return;
        const float t[3] = {std::fabs(u[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(u[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
        a[0] = u[1] * t[2] - u[2] * t[1]; a[1] = u[2] * t[0] - u[0] * t[2]; a[2] = u[0] * t[1] - u[1] * t[0];
        const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        for (float& x : a) x /= l;
        for (int r = 0; r < 3; r++) apoc_rot(R + 3 * r, a, -1.0f, 0.0f);
        return;
    }
    for (float& x : a) x /= s;
    for (int r = 0; r < 3; r++) apoc_rot(R + 3 * r, a, c, s);
}
void apoc_bone_post(uint8_t* rdram, uint32_t actor) {
    if (!guest_ok(actor, 0x400u)) return;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (!guest_ok(info, 8u)) return;
    const int cid = static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]);
    const Entry* e = nullptr;
    ApocSet sets[kApocSets];
    int ns = 0;
    for (const Entry& x : g_entries) if (x.charId == cid && (ns = apoc_sets_of(x, sets)) > 0) { e = &x; break; }
    if (!e) return;
    const uint32_t ctrl = guest_rd<uint32_t>(rdram, actor + ACT_MODEL);
    if (!guest_ok(ctrl, 0x110u)) return;
    const uint32_t gm = guest_rd<uint32_t>(rdram, ctrl + 0x0Cu);
    if (!guest_ok(gm, 0x24u)) return;
    float W[16], Wi[16];
    std::memcpy(W, rdram + ((actor + ACT_WORLD) & 0x07FFFFFFu), sizeof W);
    if (!mat_inv_affine(W, Wi)) return;
    ApocState* s = apoc_state(actor);
    const uint32_t now = ++g_apoc_seq;
    s->used = now;
    // the arms of all sets, in the sets' order: skin bone and set of arm slot k
    int n = 0, bone[kApocArms], set_of[kApocArms];
    for (int si = 0; si < ns; si++)
        for (int k = 0; k < sets[si].narm && n < kApocArms; k++) { bone[n] = sets[si].arm0 + k; set_of[n] = si; n++; }
    uint32_t at[kApocArms] = {};
    // 1. the arms as the motion shows them
    for (int k = 0; k < n; k++) {
        at[k] = ex_bone_mat(rdram, gm, bone[k]);
        if (!at[k]) continue;
        ApocArm& a = s->arm[k];
        float cur[16];
        std::memcpy(cur, rdram + (at[k] & 0x07FFFFFFu), sizeof cur);
        if (a.wrote && std::memcmp(cur, a.last, sizeof cur) == 0) continue;     // still this pass's own matrix
        a.wrote = false;
        if (!a.bullet && cur[4] * cur[4] + cur[5] * cur[5] + cur[6] * cur[6] > 1e-6f) {
            std::memcpy(a.pose, cur, sizeof cur); a.have_pose = true; a.seen = now;
        }
    }
    // 2. let go of arms whose bullet is gone; give new bullets to the nearest arm (of a set listing the record) that
    //    just vanished
    for (int k = 0; k < n; k++) {
        ApocArm& a = s->arm[k];
        if (!a.bullet || apoc_live(a.bullet, a.rec, now)) continue;
        if (g_apoc_log) std::fprintf(stderr, "[APOC] actor %08X arm %d (bone %d): bullet %08X gone\n", actor, k, bone[k], a.bullet);
        a.bullet = 0; a.have_pose = false;
        if (a.wrote && at[k]) {                         // the bone still holds the flight: hide it (rest scale 0)
            float M[16];
            std::memcpy(M, a.last, sizeof M);
            for (int i = 0; i < 12; i++) M[i] = 0.0f;
            std::memcpy(rdram + (at[k] & 0x07FFFFFFu), M, sizeof M);
            std::memcpy(a.last, M, sizeof M);
        }
    }
    for (const ApocBullet& b : g_apoc_bul) {
        if (!b.ptr || now - b.seq > kApocLive || !apoc_entry_has(e, b.rec)) continue;
        bool carried = false;
        for (int k = 0; k < n; k++) if (s->arm[k].bullet == b.ptr && s->arm[k].rec == b.rec) carried = true;
        if (carried) continue;
        float pm[3];
        warp_to_model(Wi, b.p, 1.0f, pm);
        int best = -1; float bd = kApocReach * kApocReach;
        for (int k = 0; k < n; k++) {
            const ApocArm& a = s->arm[k];
            if (a.bullet || !a.have_pose || now - a.seen > kApocSeen || !at[k] || !apoc_set_has(sets[set_of[k]], b.rec)) continue;
            const float dx = pm[0] - a.pose[12], dy = pm[1] - a.pose[13], dz = pm[2] - a.pose[14];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bd) { bd = d2; best = k; }
        }
        if (best < 0) continue;
        ApocArm& a = s->arm[best];
        a.bullet = b.ptr; a.rec = b.rec;
        for (int r = 0; r < 3; r++) {                   // the arm's last shown orientation, rows normalised
            float l = 0.0f;
            for (int j = 0; j < 3; j++) l += a.pose[4 * r + j] * a.pose[4 * r + j];
            l = l > 1e-12f ? 1.0f / std::sqrt(l) : 0.0f;
            for (int j = 0; j < 3; j++) a.R[3 * r + j] = a.pose[4 * r + j] * l;
        }
        a.spin = 0.0f; a.spin_deg = 0.0f;
        float f[3] = {0.0f, 1.0f, 0.0f};                // the arm's axis kept along the flight (bone frame): +Y ...
        if (const PinSpin* sp = apoc_spin_of(e, bone[best])) {   // ... or, spinning, +Y laid into the spin plane
            a.spin_deg = sp->deg; std::memcpy(a.ax, sp->ax, sizeof a.ax);
            const float* A = a.ax;
            float y[3] = {-A[1] * A[0], 1.0f - A[1] * A[1], -A[1] * A[2]};
            float l = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
            if (l < 1e-3f) { y[0] = 1.0f - A[0] * A[0]; y[1] = -A[0] * A[1]; y[2] = -A[0] * A[2]; l = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]); }
            for (int j = 0; j < 3; j++) f[j] = y[j] / l;
        }
        for (int j = 0; j < 3; j++) a.d[j] = f[0] * a.R[j] + f[1] * a.R[3 + j] + f[2] * a.R[6 + j];
        if (g_apoc_log) std::fprintf(stderr, "[APOC] actor %08X arm %d (bone %d) takes bullet %08X (record %u, %.2f m away)\n", actor, best, bone[best], b.ptr, b.rec, std::sqrt(bd));
    }
    // 3. carried arms ride their bullets
    for (int k = 0; k < n; k++) {
        ApocArm& a = s->arm[k];
        if (!a.bullet || !at[k]) continue;
        const ApocBullet* b = apoc_live(a.bullet, a.rec, now);
        if (!b) continue;
        float pm[3], dm[3];
        warp_to_model(Wi, b->p, 1.0f, pm);
        warp_to_model(Wi, b->d, 0.0f, dm);
        const float dl = std::sqrt(dm[0] * dm[0] + dm[1] * dm[1] + dm[2] * dm[2]);
        if (!(dl > 1e-6f)) continue;
        for (float& x : dm) x /= dl;
        apoc_turn(a.R, a.d, dm);
        std::memcpy(a.d, dm, sizeof dm);
        float Rs[9];
        std::memcpy(Rs, a.R, sizeof Rs);
        if (a.spin_deg != 0.0f) {                       // pinspin=: the frame turned about its own spin axis
            a.spin += a.spin_deg * 0.0174532925f;
            if (a.spin > 6.2831853f) a.spin -= 6.2831853f;
            if (a.spin < -6.2831853f) a.spin += 6.2831853f;
            float Am[3];
            for (int j = 0; j < 3; j++) Am[j] = a.ax[0] * a.R[j] + a.ax[1] * a.R[3 + j] + a.ax[2] * a.R[6 + j];
            const float l = std::sqrt(Am[0] * Am[0] + Am[1] * Am[1] + Am[2] * Am[2]);
            if (l > 1e-6f) {
                for (float& x : Am) x /= l;
                const float c = std::cos(a.spin), sn = std::sin(a.spin);
                for (int r = 0; r < 3; r++) apoc_rot(Rs + 3 * r, Am, c, sn);
            }
        }
        float M[16];
        for (int r = 0; r < 3; r++) { for (int j = 0; j < 3; j++) M[4 * r + j] = Rs[3 * r + j]; M[4 * r + 3] = 0.0f; }
        M[12] = pm[0]; M[13] = pm[1]; M[14] = pm[2]; M[15] = 1.0f;
        std::memcpy(rdram + (at[k] & 0x07FFFFFFu), M, sizeof M);
        std::memcpy(a.last, M, sizeof M);
        a.wrote = true;
    }
}
void apoc_install() {
    if (!apoc_wanted()) return;
    for (const Entry& e : g_entries) {
        ApocSet sets[kApocSets];
        const int ns = apoc_sets_of(e, sets);
        for (int si = 0; si < ns; si++) {
            std::string ids;
            for (int i = 0; i < sets[si].nids; i++) ids += (i ? "," : "") + std::to_string(sets[si].ids[i]);
            std::fprintf(stderr, "[ROSTER] charId %d: Apocalypsis arms %d+%d ride magi %s\n", e.charId, sets[si].arm0, sets[si].narm, ids.c_str());
        }
        for (int i = 0; i < e.pin_nspin; i++)
            std::fprintf(stderr, "[ROSTER] charId %d: arm bone %d spins %.1f deg/pass about (%.3f %.3f %.3f) while it flies\n", e.charId,
                         e.pin_spin[i].bone, e.pin_spin[i].deg, e.pin_spin[i].ax[0], e.pin_spin[i].ax[1], e.pin_spin[i].ax[2]);
    }
    g_apoc_log = std::getenv("PSPRECOMP_APOC_LOG") != nullptr;
    g_apoc_fxdir = ovl_battle_lookup(APOC_FXDIR);
    if (!g_apoc_fxdir) { std::fprintf(stderr, "[ROSTER] Apocalypsis arms hook NOT installed (no battle overlay function at %08X)\n", APOC_FXDIR); return; }
    psp_dispatch_register(APOC_FXDIR, apoc_fxdir_hook);
}

// Warp dash per character (roster.txt `warpdash=GROUND,AIR,RANGE,NEAR[,LINK[,HP]]` and optional `wdopen=CMD[/CMD...]`,
// e.g. `34 noc100 9 ... warpdash=3027,3028,20,1.0,24,1 wdopen=AB/AC`): R+Triangle at the enemy becomes a brave warp
// attack (Noctis: a fast Warp-Strike, plya GROUND 3027 / AIR 3028), and a press after it hits starts the next brave's
// press 2 (press 1, its opener, for a command listed in wdopen=, hex). RANGE is only when the dash turns into the warp
// dash; how far the warp goes is the motion's warp= / warpreach=, so RANGE > reach lets it fall short and miss.
// The dash: the player's R+Triangle runs FUN_08A50C08(actor, kind) from the control function FUN_08B3526C (0x08B35F60,
// kind 0, after the can-dash check FUN_08A50AF4); kind 1 is the dodge-derived dash (0x08B36894). It counts an air dash
// (+0x1076 when airborne), plays script 240 / 242 and sets state 0x19 (the dash state, vtable 0x08A51490). This wrapper,
// for kind 0 on a warpdash= character, asks the lock-on target and starts the warp dash instead when it is the enemy
// fighter within RANGE m: GROUND when both he and the enemy are under NEAR m above their floor (+0x3C0), else AIR (not
// the engine's airborne test FUN_088374B0: the dash script has already made it say airborne), through the same forced selector call as
// land_hook (FUN_08B382B8(actor, 1 = brave, 0)), and counts the air dash as the original would.
// The target (all from the code): the lock-on block is *(actor + 0x5AC); FUN_08B3B280(block) is its current target, any
// kind; FUN_08B3B1D4(block) is the same target only when its object kind (byte +0x542, set by the object constructor
// FUN_0883975C) is 6, else 0, and it is what the dash enter FUN_08A5065C flies at (the retail dash otherwise flies along
// the actor's own heading), so it is 0 for an enemy fighter (the first build asked only it, so every dash was the normal
// one). Kinds: 0 fighter (FUN_08B39E5C, both fighters of a battle), 1 assist (its master at +0x1108), 3 summoned / thrown
// object (master at +0x5D4), 5 bullet, 6 the dash's fixed-point target; FUN_0883739C (is fighter) = kind 0 or 1. The
// enemy: the battle setup at 0x0888BC34 stores each fighter's foe in the other's +0x300 (FUN_08836DA4), and FUN_08AB32B8
// (actor) is the actor's enemy fighter as the lock-on command (0x08A96F04, "target = enemy") compares it. The warp dash
// starts when the target is a kind-0 fighter, not the actor, equal to that enemy (FUN_08AB32B8 or +0x300); no lock-on
// target, a kind-6 target, an assist, a summoned object, any other object, out of range, or a refused start: the normal dash.
// The link: the warp dash hit arms it when it lands: FUN_0880AD28(*(defender + 0x3AC) + 0x28, hit object) registers a
// landed hit on a defender (called by FUN_088080C4 after the side test FUN_08807D4C; it sets the hit's bit in the
// defender's hit record and keeps {hit id, power, owner = hit +0x18}), so a hook there arms the link when the hit's owner
// is a warpdash= fighter whose current plya record (+0x1020, else +0x101C, as FUN_08B30898) is the warp dash and the
// defender's record is its enemy's. (The first build read the list at +0x1028..+0x1068 for this; that list is filled by
// a timeline op with constant flags and read by the cancel test FUN_08B309C8, so it says nothing about a hit.) It stays armed
// LINK bone passes (default 24) so a touchdown in between does not lose it. While armed, a brave press (input kind 0, or an
// HP press, kind 2, with HP = 1) reaching the command selector FUN_0884A294 is answered here: the set is the ground one
// after the GROUND warp dash or when the engine says grounded, else the air one; the slot is picked as FUN_0884921C picks it (ground brave 0 / 1 (dir bit 0x80) /
// 2 (0x100), air brave 3 / 4 (0x20) / 5 (0x40), HP 6-8 / 9-11; the direction bits are ctx +0x78 of the selector context
// FUN_08849B50 builds, used when actor +0x5B0 bit 0 is set), the ability is FUN_08B33B08(actor, slot), its 'ablt' record
// gives the command (+0xC), the actor's comb (moveset +0x8C) the command's steps, and the press 2 (step 1; step 0 for a
// one-step command, an HP press or a wdopen= command) is handed back as the selected record. FUN_08B382B8 does not check the comb, so the press
// 2 starts by itself, and its own press 3 chains as usual. Empty slot / unknown command / no record: the engine's selector.
// The first kWdFirstLogs dash decisions, hits and link presses of a run are always logged ([WARPDASH]); with
// PSPRECOMP_WARPDASH_LOG=1 every one is.
constexpr uint32_t DASH_START = 0x08A50C08u;    // battle overlay: start the R+Triangle dash (a0 = actor, a1 = kind)
constexpr uint32_t DASH_TARGET = 0x08B3B1D4u;   // battle overlay: the lock-on target if its kind is 6, else 0 (a0 = the block)
constexpr uint32_t LOCK_TARGET = 0x08B3B280u;   // battle overlay: the lock-on target, any kind (a0 = *(actor + 0x5AC))
constexpr uint32_t ENEMY_OF = 0x08AB32B8u;      // battle overlay: the actor's enemy fighter (a0 = actor), 0 if none
constexpr uint32_t HIT_LAND = 0x0880AD28u;      // EBOOT: a hit lands (a0 = *(defender + 0x3AC) + 0x28, a1 = hit object)
constexpr uint32_t AIRBORNE = 0x088374B0u;      // EBOOT: actor airborne (state flags)
constexpr uint32_t SEL_CONTEXT = 0x08849B50u;   // EBOOT: selector context (a0 = ctx, a1 = actor, a2 = target, a3 = kind)
constexpr uint32_t SLOT_ABILITY = 0x08B33B08u;  // battle overlay: equipped ability of a command slot (a0 = actor, a1 = slot)
constexpr uint32_t ACT_LOCK = 0x5ACu, ACT_OPP = 0x300u, ACT_HEIGHT = 0x3C0u, ACT_REC = 0x101Cu, ACT_REC2 = 0x1020u,
                   ACT_SLOT = 0x107Eu, ACT_AIRDASH = 0x1076u, ACT_DIRS = 0x5B0u, ACT_KIND = 0x542u, ACT_HITREC = 0x3ACu,
                   HIT_OWNER = 0x18u;
constexpr int kWdFirstLogs = 5;
FuncPtr g_dash_start = nullptr, g_dash_target = nullptr, g_airborne = nullptr, g_sel_context = nullptr, g_slot_ability = nullptr,
        g_lock_target = nullptr, g_enemy_of = nullptr, g_hit_land = nullptr;
bool g_wd_log = false, g_wd_reached = false;
int g_wd_dash_logs = 0, g_wd_hit_logs = 0, g_wd_link_logs = 0;
struct WdState { uint32_t actor = 0, frames = 0, armed_until = 0, dash_rec = 0, dash_caller = 0; int act = -1; bool in_dash = false, hit = false, used = false; };
WdState g_wd[8];
int g_wd_next = 0;
int wd_char_id(const uint8_t* rdram, uint32_t actor, uint32_t* info_out = nullptr) {
    if (!guest_ok(actor, 0x1100u)) return -999;
    const uint32_t info = guest_rd<uint32_t>(rdram, actor + ACTOR_INFO);
    if (info_out) *info_out = info;
    if (!guest_ok(info, 8u)) return -999;
    return static_cast<int8_t>(rdram[(info + 6u) & 0x07FFFFFFu]);
}
const Entry* wd_entry(const uint8_t* rdram, uint32_t actor) {
    const int charId = wd_char_id(rdram, actor);
    if (charId == -999) return nullptr;
    for (const Entry& e : g_entries) if (e.charId == charId && e.wd_ground >= 0) return &e;
    return nullptr;
}
int wd_kind(const uint8_t* rdram, uint32_t obj) { return guest_ok(obj, ACT_KIND + 1u) ? rdram[(obj + ACT_KIND) & 0x07FFFFFFu] : -1; }
bool warpdash_wanted() {
    for (const Entry& e : g_entries) if (e.wd_ground >= 0) return true;
    return false;
}
WdState& wd_state(uint32_t actor) {
    for (WdState& s : g_wd) if (s.actor == actor) return s;
    WdState& s = g_wd[g_wd_next++ & 7];
    s = WdState{};
    s.actor = actor;
    return s;
}
// a guest call with up to four word arguments; every register is given back as it was, v0 is returned
uint32_t wd_call(FuncPtr f, uint8_t* rdram, recomp_context* ctx, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0,
                 uint32_t a3 = 0, uint32_t sp = 0) {
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(a0); ctx->r[5] = static_cast<int32_t>(a1);
    ctx->r[6] = static_cast<int32_t>(a2); ctx->r[7] = static_cast<int32_t>(a3);
    if (sp) ctx->r[29] = static_cast<int32_t>(sp);
    f(rdram, ctx);
    const uint32_t v = static_cast<uint32_t>(ctx->r[2]);
    *ctx = saved;
    return v;
}
uint32_t wd_current_record(const uint8_t* rdram, uint32_t actor) {
    const uint32_t r2 = guest_rd<uint32_t>(rdram, actor + ACT_REC2);
    return r2 ? r2 : guest_rd<uint32_t>(rdram, actor + ACT_REC);
}
int wd_record_id(const uint8_t* rdram, uint32_t rec) { return guest_ok(rec, 0x74u) ? guest_rd<uint16_t>(rdram, rec) : -1; }
// The R+Triangle dash, as found from the code (2026-10-22, second in-game run): the dash is ability driven. The player's
// R (dash) input in the control function FUN_08B3526C asks FUN_08A52FE4(actor), which picks the dash type from the equipped
// dash ability (ground 0x38 -> 2, 0x39 -> 4, 0x3A -> 6; airborne 0x11B / 0x11C; -1 = none or the air-dash count +0x1075 is
// used up), then type 6 -> FUN_08A50C08(actor, 1) (state 0x19) and any other type -> FUN_08A53290(actor, type) (0x08B368A8),
// which plays script 240 (242 with 0x38 / 0x39 / 0x3A, 244 for type 3), counts +0x1075 when airborne and switches to
// state 0x13 (vtable 0x08A54888) with the type as its argument. Type 3 comes from a second input (FUN_08B2FBCC(actor, 1),
// 0x08B3682C, with actor byte +0x197 bit 0x40): kept as the normal dash. FUN_08A50C08(actor, 0) (0x08B35F60) needs ability
// 0x11D (FUN_08A50AF4) and flies at a kind-6 point; the CPU's dash is state 0x0D (FUN_08A59CE0, scripts 209-212) and the
// CPU also calls FUN_08A53290 (0x08ACBD30). So the first two builds hooked a path (FUN_08A50C08) the player's dash never takes.
// The decision now sits on the EBOOT state switch FUN_08834C10(actor, state, arg): a switch to 0x13 (any type but 3) or
// 0x19 of a warpdash= fighter is checked here, and when the lock-on target is its enemy within RANGE m the warp dash
// action starts instead (the dash function has already played its script and counted the air dash; the original state
// switch is then skipped, and FUN_08B382B8 sets the action's own state and script). The actor's current state id is
// *(*(actor + 0x3A4)) (FUN_08A50AF4 compares it with 0x13 / 0x19).
constexpr uint32_t STATE_SET = 0x08834C10u;     // EBOOT: switch state (a0 = actor, a1 = new state, a2 = argument)
constexpr uint32_t DASH13_START = 0x08A53290u;  // battle overlay: start the typed dash, state 0x13 (a0 = actor, a1 = type)
constexpr uint32_t CONTROL_FN = 0x08B3526Cu;    // battle overlay: per-frame player control (a0 = actor)
constexpr uint32_t SCRIPT_PLAY = 0x0882FA10u;   // EBOOT: play a script (a0 = actor + 0x130, a1 = script)
constexpr uint32_t ACT_STATEOBJ = 0x3A4u, ACT_SCRIPTS = 0x130u;
constexpr int kWdStateLogs = 8, kWdScriptLogs = 10;
FuncPtr g_state_set = nullptr, g_dash13 = nullptr, g_control = nullptr, g_script_play = nullptr;
bool g_wd_control_logged = false, g_wd_in_start = false;
int g_wd_state_logs = 0, g_wd_dash13_logs = 0, g_wd_c08_logs = 0, g_wd_script_logs = 0;
int wd_state_id(const uint8_t* rdram, uint32_t actor) {
    const uint32_t so = guest_rd<uint32_t>(rdram, actor + ACT_STATEOBJ);
    return guest_ok(so, 4u) ? static_cast<int>(guest_rd<uint32_t>(rdram, so)) : -1;
}
// the warp dash decision for a dash about to start (entry = how it was reached); true = the warp dash action started
bool wd_try_start(uint8_t* rdram, recomp_context* ctx, uint32_t actor, const Entry* e, int charId, const char* entry) {
    const bool logit = g_wd_log || g_wd_dash_logs < kWdFirstLogs;
    if (logit) g_wd_dash_logs++;
    if (!g_start_action || !g_plya_select || !g_lock_target || !g_dash_target || !g_airborne) {
        if (logit) std::fprintf(stderr, "[WARPDASH] dash actor %08X (%s): a function is missing -> normal dash\n", actor, entry);
        return false;
    }
    const char* why = nullptr;
    float dist = -1.0f, p[3] = {0, 0, 0}, t[3] = {0, 0, 0};
    const uint32_t lock = guest_rd<uint32_t>(rdram, actor + ACT_LOCK);
    const uint32_t opp = guest_rd<uint32_t>(rdram, actor + ACT_OPP), turn = guest_rd<uint32_t>(rdram, actor + ACT_TARGET);
    uint32_t tgt = 0, fixed = 0, enemy = 0;
    int tkind = -1;
    guest_vec3(rdram, actor + ACT_POS, p);
    if (wd_kind(rdram, actor) != 0) why = "not a kind-0 fighter (assist?)";
    else if (!guest_ok(lock, 0x140u)) why = "no lock-on block";
    else {
        tgt = wd_call(g_lock_target, rdram, ctx, lock);                 // FUN_08B3B280(*(actor + 0x5AC))
        fixed = wd_call(g_dash_target, rdram, ctx, lock);               // FUN_08B3B1D4: non-zero only for a kind-6 target
        if (g_enemy_of) enemy = wd_call(g_enemy_of, rdram, ctx, actor); // FUN_08AB32B8(actor)
        tkind = wd_kind(rdram, tgt);
        if (!tgt) why = "no lock-on target";
        else if (tgt == actor) why = "target is himself";
        else if (fixed || tkind == 6) why = "kind-6 target (the dash's own fixed-point target)";
        else if (tkind == 1) why = "target is an assist";
        else if (tkind == 3) why = "target is a summoned object";
        else if (tkind != 0) why = "target is not a fighter";
        else if (tgt != enemy && tgt != opp) why = "target is a fighter but not his enemy";
        else if (!guest_ok(tgt, 0x100u) || !guest_vec3(rdram, tgt + ACT_POS, t)) why = "no target position";
        else {
            dist = std::sqrt((t[0] - p[0]) * (t[0] - p[0]) + (t[1] - p[1]) * (t[1] - p[1]) + (t[2] - p[2]) * (t[2] - p[2]));
            if (dist > e->wd_range) why = "out of range";
        }
    }
    if (logit)
        std::fprintf(stderr, "[WARPDASH] dash #%d actor %08X charId %d via %s: lock %08X target %08X (kind %d) kind6 %08X enemy %08X "
                     "+0x300 %08X +0x2FC %08X, pos %.2f %.2f %.2f, target pos %.2f %.2f %.2f, dist %.2f, +0x3C0 %.2f%s%s\n",
                     g_wd_dash_logs, actor, charId, entry, lock, tgt, tkind, fixed, enemy, opp, turn, p[0], p[1], p[2], t[0], t[1], t[2],
                     dist, guest_rd<float>(rdram, actor + ACT_HEIGHT), why ? ": skipped: " : "", why ? why : "");
    if (why) return false;
    // ground or air: both fighters' height above their floor (+0x3C0). The engine's airborne test is no use here: the
    // dash function has already played the dash script, and it says airborne on flat ground (every ground dash of the
    // 22 Oct runs started the air action 3028, +0x3C0 0.00), so a ground warp dash ended in an air action and its link
    // handed a ground press to a fighter the engine had in the air.
    const bool eng_air = (wd_call(g_airborne, rdram, ctx, actor) & 0xFFu) != 0;
    const float own_h = guest_rd<float>(rdram, actor + ACT_HEIGHT), tgt_h = guest_rd<float>(rdram, tgt + ACT_HEIGHT);
    const bool air = !(own_h < e->wd_near && tgt_h < e->wd_near);
    const int act = air ? e->wd_air : e->wd_ground;
    uint32_t table = 0;
    const uint32_t rec = plya_record(rdram, actor, act, &table);
    uint32_t ok = 0;
    const uint8_t gate = rdram[(actor + ACT_GATE) & 0x07FFFFFFu];
    if (rec) {
        g_wd_in_start = true;
        g_force_actor = actor; g_force_rec = rec; g_force_table = table;
        ok = wd_call(g_start_action, rdram, ctx, actor, LAND_KIND, 0);
        g_force_actor = g_force_rec = g_force_table = 0u;
        g_wd_in_start = false;
    }
    if (logit)
        std::fprintf(stderr, "[WARPDASH] dash #%d actor %08X at the enemy %08X, %.2f m, %s (height %.2f, enemy %.2f, near %.2f; engine %s): "
                     "action %d (record %08X) %s, v0 %u, gate %02X\n",
                     g_wd_dash_logs, actor, tgt, dist, air ? "air" : "ground", own_h, tgt_h, e->wd_near, eng_air ? "airborne" : "grounded",
                     act, rec, ok ? "started" : "NOT started -> normal dash", ok, gate);
    if (!ok) return false;
    WdState& s = wd_state(actor);
    s.in_dash = true; s.hit = false; s.used = false; s.dash_rec = rec; s.armed_until = 0; s.act = act;
    return true;
}
// FUN_08834C10 (EBOOT state switch): the dash states 0x13 / 0x19 of a warpdash= fighter are decided here
void state_set_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    const int st = ctx->r[5];
    if ((st != 0x13 && st != 0x19) || g_wd_in_start) { g_state_set(rdram, ctx); return; }
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    const Entry* e = ovl == 1u ? wd_entry(rdram, actor) : nullptr;
    if (!e) { g_state_set(rdram, ctx); return; }
    const uint32_t arg = static_cast<uint32_t>(ctx->r[6]) & 0xFFFFu, ra = static_cast<uint32_t>(ctx->r[31]);
    const uint32_t s0 = static_cast<uint32_t>(ctx->r[16]);
    const int charId = wd_char_id(rdram, actor), from = wd_state_id(rdram, actor), okind = wd_kind(rdram, actor);
    WdState& s = wd_state(actor);
    // 0x13 from FUN_08A53290: arg = the dash type; 0x19 from FUN_08A50C08 (return 0x08A50CB0): s0 = its kind argument
    const int dtype = st == 0x13 ? static_cast<int>(arg) : (ra == 0x08A50CB0u ? static_cast<int>(s0 & 0xFFu) : -1);
    if (g_wd_log || g_wd_state_logs < kWdStateLogs) {
        g_wd_state_logs++;
        std::fprintf(stderr, "[WARPDASH] state %X -> %X: actor %08X charId %d (object kind %d), %s %d, ra %08X, dash called from %08X\n",
                     static_cast<unsigned>(from), static_cast<unsigned>(st), actor, charId, okind,
                     st == 0x13 ? "type" : "kind", dtype, ra, s.dash_caller);
    }
    if (st == 0x13 && dtype == 3) {                      // the second-input dash (script 244): left alone
        if (g_wd_log || g_wd_state_logs <= kWdStateLogs)
            std::fprintf(stderr, "[WARPDASH] actor %08X: type-3 dash (script 244) -> normal dash\n", actor);
        g_state_set(rdram, ctx);
        return;
    }
    char via[64];
    std::snprintf(via, sizeof via, "state %X %s %d", static_cast<unsigned>(st), st == 0x13 ? "type" : "kind", dtype);
    if (wd_try_start(rdram, ctx, actor, e, charId, via)) return;   // the warp dash replaced the dash state
    g_state_set(rdram, ctx);
}
// FUN_08A53290 (the typed dash) and FUN_08A50C08 (the state-0x19 dash): pass-through, logged; the caller is kept for the
// state switch's log line (it tells the player's control 0x08B368B0 / 0x08B36834 from the CPU's 0x08ACBD38)
void dash13_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    if (ovl != 1u) {
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(DASH13_START) : nullptr) f(rdram, ctx);
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]), ra = static_cast<uint32_t>(ctx->r[31]);
    if (wd_entry(rdram, actor)) {
        wd_state(actor).dash_caller = ra;
        if (g_wd_log || g_wd_dash13_logs < kWdFirstLogs) {
            g_wd_dash13_logs++;
            std::fprintf(stderr, "[WARPDASH] typed dash FUN_08A53290: actor %08X type %d, called from %08X, airborne count +0x1075 %u\n",
                         actor, static_cast<int>(ctx->r[5]), ra, rdram[(actor + 0x1075u) & 0x07FFFFFFu]);
        }
    }
    g_dash13(rdram, ctx);
}
void dash_start_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]), ra = static_cast<uint32_t>(ctx->r[31]);
    const int kind = ctx->r[5];
    if (!g_wd_reached) {
        g_wd_reached = true;
        std::fprintf(stderr, "[WARPDASH] FUN_08A50C08 reached: actor %08X kind %d overlay id %u, called from %08X\n", actor, kind, ovl, ra);
    }
    if (ovl != 1u) {                                    // not the battle overlay: its own code at this address, if any
        if (FuncPtr f = ovl == 2u ? ovl_menu_lookup(DASH_START) : nullptr) f(rdram, ctx);
        return;
    }
    if (wd_entry(rdram, actor)) {
        wd_state(actor).dash_caller = ra;
        if (g_wd_log || g_wd_c08_logs < kWdFirstLogs) {
            g_wd_c08_logs++;
            std::fprintf(stderr, "[WARPDASH] state-0x19 dash FUN_08A50C08: actor %08X kind %d, called from %08X\n", actor, kind, ra);
        }
    }
    g_dash_start(rdram, ctx);                            // the decision is made at its state switch (state_set_hook)
}
// FUN_08B3526C (the player's control): logged once per run for a warpdash= fighter, to show it is reached
void control_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!g_wd_control_logged) {
        uint32_t ovl;
        std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
        const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
        if (ovl == 1u && wd_entry(rdram, actor)) {
            g_wd_control_logged = true;
            std::fprintf(stderr, "[WARPDASH] control function FUN_08B3526C reached for actor %08X (charId %d, state %X)\n", actor,
                         wd_char_id(rdram, actor), static_cast<unsigned>(wd_state_id(rdram, actor)));
        }
    }
    g_control(rdram, ctx);
}
// FUN_0882FA10 (play a script): the dash scripts (209-212, 240-244) of a warpdash= fighter, logged
void script_play_hook(uint8_t* rdram, recomp_context* ctx) {
    const int script = ctx->r[5];
    if (((script >= 209 && script <= 212) || (script >= 240 && script <= 244)) && (g_wd_log || g_wd_script_logs < kWdScriptLogs)) {
        const uint32_t actor = static_cast<uint32_t>(ctx->r[4]) - ACT_SCRIPTS;
        uint32_t ovl;
        std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
        if (ovl == 1u && wd_entry(rdram, actor)) {
            g_wd_script_logs++;
            std::fprintf(stderr, "[WARPDASH] script %d on actor %08X (state %X), called from %08X\n", script, actor,
                         static_cast<unsigned>(wd_state_id(rdram, actor)), static_cast<uint32_t>(ctx->r[31]));
        }
    }
    g_script_play(rdram, ctx);
}
// a hit lands (FUN_0880AD28, after the original): the warp dash's hit on the enemy arms the link
void warpdash_on_hit(const uint8_t* rdram, uint32_t hitrec, uint32_t hit) {
    if (!guest_ok(hit, HIT_OWNER + 4u)) return;
    const uint32_t owner = guest_rd<uint32_t>(rdram, hit + HIT_OWNER);
    if (wd_kind(rdram, owner) != 0) return;                         // the hit of a fighter's own attack (not a summon / bullet)
    const Entry* e = wd_entry(rdram, owner);
    if (!e) return;
    const int id = wd_record_id(rdram, wd_current_record(rdram, owner));
    if (id < 0 || (id != e->wd_ground && id != e->wd_air)) return;
    const uint32_t opp = guest_rd<uint32_t>(rdram, owner + ACT_OPP);
    const uint32_t opp_rec = guest_ok(opp, ACT_HITREC + 4u) ? guest_rd<uint32_t>(rdram, opp + ACT_HITREC) + 0x28u : 0u;
    WdState& s = wd_state(owner);
    // every hit of the warp dash on the enemy (the thrown sword's brave hit, if the game credits it to him, then the
    // strike) arms the link or renews its window, until a press has used it (one link per warp dash); a hit whose owner
    // is the bullet itself (kind 5) never gets here
    const bool on_enemy = hitrec == opp_rec;
    const bool arm = on_enemy && !s.used, again = arm && s.hit;
    if (arm) {
        s.hit = true;
        s.armed_until = s.frames + static_cast<uint32_t>(e->wd_link);
    }
    if (g_wd_log || g_wd_hit_logs < kWdFirstLogs) {
        g_wd_hit_logs++;
        std::fprintf(stderr, "[WARPDASH] actor %08X warp dash %d: hit %08X lands on record %08X (the enemy %08X has %08X): %s\n",
                     owner, id, hit, hitrec, opp, opp_rec,
                     arm ? (again ? "HIT, link window renewed" : "HIT, link armed") : !on_enemy ? "not the enemy, not armed" : "link already used");
    }
}
void hit_land_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t hitrec = static_cast<uint32_t>(ctx->r[4]), hit = static_cast<uint32_t>(ctx->r[5]);
    g_hit_land(rdram, ctx);
    warpdash_on_hit(rdram, hitrec, hit);
}
// every bone pass of a warpdash= actor: the clock the link window counts in
void warpdash_tick(uint8_t* rdram, uint32_t actor) {
    if (!warpdash_wanted()) return;
    const Entry* e = wd_entry(rdram, actor);
    if (!e) return;
    WdState& s = wd_state(actor);
    s.frames++;
    const int id = wd_record_id(rdram, wd_current_record(rdram, actor));
    if (!(id >= 0 && (id == e->wd_ground || id == e->wd_air))) s.in_dash = false;
}
// the comb step (0 = opener, 1 = press 2) of a command in the actor's comb, -1 if none
int wd_comb_step(const uint8_t* rdram, uint32_t actor, int cmd, int step) {
    const uint32_t ms = guest_rd<uint32_t>(rdram, actor + ACT_MOVESET);
    if (!guest_ok(ms, 0x90u)) return -1;
    const uint32_t comb = guest_rd<uint32_t>(rdram, ms + 0x8Cu);
    if (!guest_ok(comb, 8u)) return -1;
    const int n = guest_rd<int16_t>(rdram, comb + 2u);
    if (n <= 0 || n > 256 || !guest_ok(comb, 4u + 4u * static_cast<uint32_t>(n))) return -1;
    for (int i = 0; i < n; i++) {
        const uint32_t blk = comb + guest_rd<uint32_t>(rdram, comb + 4u + 4u * static_cast<uint32_t>(i));
        if (!guest_ok(blk, 16u) || guest_rd<int16_t>(rdram, blk) != cmd) continue;
        const int ns = guest_rd<int16_t>(rdram, blk + 2u);
        if (ns <= 0 || ns > 16) return -1;
        const int k = step < ns ? step : 0;
        return guest_rd<uint16_t>(rdram, blk + 8u + 8u * static_cast<uint32_t>(k));
    }
    return -1;
}
int wd_command_of(const uint8_t* rdram, int ability) {      // 'ablt' record +0xC (FUN_088B1D9C's table)
    const uint32_t blk = guest_rd<uint32_t>(rdram, kAbltBlockPtr), recs = guest_rd<uint32_t>(rdram, kAbltRecsPtr);
    if (!guest_ok(blk, 12u) || !guest_ok(recs, 4u)) return -1;
    const uint32_t n = guest_rd<uint32_t>(rdram, blk + 8u);
    if (n == 0 || n > 0x1000u || !guest_ok(recs, n * kAbltRecBytes)) return -1;
    for (uint32_t r = 0; r < n; r++) {
        const uint32_t a = recs + kAbltRecBytes * r;
        if (guest_rd<uint16_t>(rdram, a) == static_cast<uint16_t>(ability)) return guest_rd<int16_t>(rdram, a + 0xCu);
    }
    return -1;
}
// the selector (FUN_0884A294(holder, actor, target, kind)) for a press while the link is armed; true = answered
bool warpdash_link(uint8_t* rdram, recomp_context* ctx) {
    if (!g_sel_context || !g_slot_ability || !g_airborne) return false;
    const uint32_t actor = static_cast<uint32_t>(ctx->r[5]), target = static_cast<uint32_t>(ctx->r[6]);
    const int kind = ctx->r[7];
    const Entry* e = wd_entry(rdram, actor);
    if (!e) return false;
    WdState& s = wd_state(actor);
    if (!s.armed_until || s.frames > s.armed_until) return false;
    if (kind != 0 && !(kind == 2 && e->wd_hp)) return false;
    const float h = guest_rd<float>(rdram, actor + ACT_HEIGHT);
    const bool airborne = (wd_call(g_airborne, rdram, ctx, actor) & 0xFFu) != 0;
    // the set follows the warp dash that armed the link: after the ground one (a ground motion throughout) the ground set;
    // after the air one the engine decides (air set while airborne, ground set once it has touched down). Never a ground
    // press for a fighter the engine has in the air because he is near the floor.
    const bool air = airborne && s.act != e->wd_ground;
    // the selector context, on the guest stack below the caller's frame (as FUN_0884A294 builds it)
    const uint32_t sp = static_cast<uint32_t>(ctx->r[29]);
    const uint32_t buf = (sp - 0x100u) & ~0xFu;
    if (!guest_ok(buf, 0x100u)) return false;
    wd_call(g_sel_context, rdram, ctx, buf, actor, target, static_cast<uint32_t>(kind), buf);
    const uint32_t dirs = guest_rd<uint32_t>(rdram, buf + 0x78u);
    const bool dir_on = (rdram[(actor + ACT_DIRS) & 0x07FFFFFFu] & 1u) != 0;
    const bool hp = kind == 2;
    const int base = (air ? 3 : 0) + (hp ? 6 : 0);
    const uint32_t bit1 = air ? 0x20u : 0x80u, bit2 = air ? 0x40u : 0x100u;
    int slot = -1, ability = -1;
    auto ab = [&](int sl) { return static_cast<int>(static_cast<int16_t>(wd_call(g_slot_ability, rdram, ctx, actor, static_cast<uint32_t>(sl)) & 0xFFFFu)); };
    if (dir_on) {
        if (dirs & bit1) { ability = ab(base + 1); slot = base + 1; }
        if (dirs & bit2) { ability = ab(base + 2); slot = base + 2; }
    }
    if (ability < 0) { ability = ab(base); slot = base; }
    const int cmd = ability >= 0 ? wd_command_of(rdram, ability) : -1;
    bool open = hp;                                          // HP: step 0; wdopen= commands: press 1 (their opener)
    for (int c : e->wd_open) if (c == cmd) open = true;
    const int act = cmd >= 0 ? wd_comb_step(rdram, actor, cmd, open ? 0 : 1) : -1;
    uint32_t table = 0;
    const uint32_t rec = act >= 0 ? plya_record(rdram, actor, act, &table) : 0;
    if (g_wd_log || g_wd_link_logs < kWdFirstLogs) {
        g_wd_link_logs++;
        std::fprintf(stderr, "[WARPDASH] link press actor %08X kind %d after warp dash %d: %s (engine %s, height %.2f), dirs %X, slot %d, ability %04X, command %X, %s %d, record %08X -> %s\n",
                     actor, kind, s.act, air ? "air" : "ground", airborne ? "airborne" : "grounded", h, dirs, slot,
                     ability & 0xFFFF, cmd, hp ? "HP" : open ? "press 1" : "press 2", act, rec, rec ? "linked" : "engine selector");
    }
    if (!rec) return false;
    rdram[(actor + ACT_SLOT) & 0x07FFFFFFu] = static_cast<uint8_t>(slot);
    s.armed_until = 0;                                       // one link per warp dash
    s.used = true;
    ctx->r[2] = static_cast<int32_t>(rec);
    return true;
}
void warpdash_install() {
    bool any = false;
    for (const Entry& e : g_entries)
        if (e.wd_ground >= 0) {
            any = true;
            std::string op;
            for (int c : e.wd_open) { char b[8]; std::snprintf(b, sizeof b, "%s%X", op.empty() ? "" : " ", c); op += b; }
            std::fprintf(stderr, "[ROSTER] charId %d: warp dash %d / %d (range %.1f m, ground under %.2f m, link %d frames, HP link %s, press 1 for %s)\n",
                         e.charId, e.wd_ground, e.wd_air, e.wd_range, e.wd_near, e.wd_link, e.wd_hp ? "on" : "off", op.empty() ? "none" : op.c_str());
        }
    if (!any) return;
    g_wd_log = std::getenv("PSPRECOMP_WARPDASH_LOG") != nullptr;
    g_dash_start = ovl_battle_lookup(DASH_START);
    g_dash_target = ovl_battle_lookup(DASH_TARGET);
    g_lock_target = ovl_battle_lookup(LOCK_TARGET);
    g_enemy_of = ovl_battle_lookup(ENEMY_OF);
    g_slot_ability = ovl_battle_lookup(SLOT_ABILITY);
    if (!g_start_action) g_start_action = ovl_battle_lookup(START_ACTION);
    g_airborne = psp_dispatch_probe_lookup(AIRBORNE);
    g_sel_context = psp_dispatch_probe_lookup(SEL_CONTEXT);
    g_hit_land = psp_dispatch_probe_lookup(HIT_LAND);
    g_state_set = psp_dispatch_probe_lookup(STATE_SET);
    g_script_play = psp_dispatch_probe_lookup(SCRIPT_PLAY);
    g_dash13 = ovl_battle_lookup(DASH13_START);
    g_control = ovl_battle_lookup(CONTROL_FN);
    if (!g_plya_select) g_plya_select = psp_dispatch_probe_lookup(PLYA_SELECT);
    if (!g_dash_start || !g_dash_target || !g_lock_target || !g_slot_ability || !g_start_action || !g_airborne || !g_sel_context ||
        !g_plya_select || !g_hit_land || !g_state_set) {
        std::fprintf(stderr, "[ROSTER] warp dash hook NOT installed (dash %s, kind-6 target %s, lock target %s, slot %s, start %s, "
                     "airborne %s, context %s, selector %s, hit %s, state switch %s)\n",
                     g_dash_start ? "ok" : "missing", g_dash_target ? "ok" : "missing", g_lock_target ? "ok" : "missing",
                     g_slot_ability ? "ok" : "missing", g_start_action ? "ok" : "missing", g_airborne ? "ok" : "missing",
                     g_sel_context ? "ok" : "missing", g_plya_select ? "ok" : "missing", g_hit_land ? "ok" : "missing",
                     g_state_set ? "ok" : "missing");
        g_dash_start = nullptr;
        return;
    }
    if (!g_enemy_of)                                     // optional: +0x300 alone then decides "his enemy"
        std::fprintf(stderr, "[ROSTER] warp dash: no battle overlay function at %08X (enemy of), using +0x300 only\n", ENEMY_OF);
    psp_dispatch_register(PLYA_SELECT, plya_select_hook);
    psp_dispatch_register(DASH_START, dash_start_hook);
    psp_dispatch_register(HIT_LAND, hit_land_hook);
    psp_dispatch_register(STATE_SET, state_set_hook);
    if (g_dash13) psp_dispatch_register(DASH13_START, dash13_hook);
    if (g_control) psp_dispatch_register(CONTROL_FN, control_hook);
    if (g_script_play) psp_dispatch_register(SCRIPT_PLAY, script_play_hook);
    std::fprintf(stderr, "[ROSTER] warp dash decided at the state switch %08X (dash states 0x13 / 0x19); hooks on %08X (selector), "
                 "%08X (hit lands); logged: %08X %s, %08X %s, %08X %s, %08X %s%s\n", STATE_SET, PLYA_SELECT, HIT_LAND,
                 DASH13_START, g_dash13 ? "ok" : "missing", DASH_START, "ok", CONTROL_FN, g_control ? "ok" : "missing",
                 SCRIPT_PLAY, g_script_play ? "ok" : "missing", g_wd_log ? ", every decision logged" : ", first decisions logged");
}

// armtrail= (weapon trails on floating arms) (2026-10-22): the crystal royal arms (Apocalypsis / Armiger Assault /
// EX Burst apoc_w, the EX Mode ring armiger_w, Onslaught's thrown star) draw the engine's own weapon trail while they are
// shown. roster.txt `armtrail=ID,SLOT0,NSLOT,BONE0:COUNT:BIT0[:DRAW],...`, e.g. Noctis
// `armtrail=7700,4,12,168:12:0:180,81:1:23:160,181:11:12:73`: arm j of a set is skin bone BONE0+j, its trail the FEPA
// record with id ID and mask bit BIT0+j (objx: one record per arm on that bone), DRAW the draw bone that shows it (-1 /
// left out: none). The sets' order is the priority.
// Engine (EBOOT / battle overlay disassembly): the timeline op 0x042A {slot, id, bit, scale} (FUN_08B2E1A8) sets bit
// 1<<slot in the actor's trail mask (+0x6D8; cleared every tick by the timeline walker) and stores {id, bit, scale, 0}
// at actor + 0x5D8 + 16 * slot; FUN_0884E660 (once per frame, after every timeline) starts a trail for a slot that
// appears (the first FEPA record with that id whose +0x44 mask has the bit and whose bone is drawn), and stops it when
// the slot goes away or the actor's script changes. So this wrapper fills slots SLOT0..SLOT0+NSLOT-1 for the arms the
// bone pass saw shown (scale > 0.05 and DRAW not hidden in the draw-bone table) right before FUN_0884E660 runs, keeping
// each arm in the same slot while it stays shown (a slot whose record changes would restart its trail); timelines keep
// slots 0..SLOT0-1. More shown arms than slots: a later set's arm gives its slot to an earlier set's arm.
// PSPRECOMP_ARMTRAIL_LOG=1 logs every slot change.
constexpr uint32_t TRAIL_CONSUME = 0x0884E660u;   // EBOOT: per-frame weapon-trail start / stop for every actor
constexpr uint32_t TRAIL_MASK = 0x6D8u, TRAIL_SLOTS = 0x5D8u;
constexpr int kTrailActors = 4, kTrailArms = 32;
struct TrailState { uint32_t actor = 0, seen = 0; int n = 0; bool shown[kTrailArms] = {}; int slot_of[kTrailArms];
                    int arm_in[16]; int bit[kTrailArms] = {}; };
TrailState g_trail[kTrailActors];
uint32_t g_trail_clock = 1;
FuncPtr g_orig_trail_consume = nullptr;
bool g_trail_log = false;
bool armtrail_wanted() {
    for (const Entry& e : g_entries) if (e.at_nset > 0) return true;
    return false;
}
TrailState* trail_state(uint32_t actor, bool create) {
    TrailState* old = nullptr;
    for (TrailState& s : g_trail) {
        if (s.actor == actor) return &s;
        if (!old || s.seen < old->seen) old = &s;
    }
    if (!create) return nullptr;
    *old = TrailState{};
    old->actor = actor;
    for (int& v : old->slot_of) v = -1;
    for (int& v : old->arm_in) v = -1;
    return old;
}
// bone pass (after armiger_update / apoc_bone_post wrote their arms): which arms are shown now
void armtrail_bone_post(uint8_t* r, uint32_t actor) {
    const Entry* e = ex_entry(r, actor);
    if (!e || e->at_nset <= 0) return;
    const uint32_t ctrl = ex_r32(r, actor + EX_ACTOR_MODEL);
    if (!ex_ok(ctrl, 0x110)) return;
    const uint32_t gm = ex_r32(r, ctrl + 0x0Cu);
    if (!ex_ok(gm, 0x24)) return;
    TrailState* s = trail_state(actor, true);
    s->seen = g_trail_clock;
    int n = 0;
    for (int si = 0; si < e->at_nset; si++) {
        bool drawn = true;
        if (e->at_draw[si] >= 0) {
            const uint32_t v = ex_vis_entry(r, ctrl, e->at_draw[si]);
            drawn = !v || ex_p(r, v)[1] == 1;
        }
        for (int j = 0; j < e->at_count[si] && n < kTrailArms; j++, n++) {
            s->bit[n] = e->at_bit0[si] + j;
            bool on = false;
            const uint32_t at = drawn ? ex_bone_mat(r, gm, e->at_bone0[si] + j) : 0;
            if (at) {
                const float y0 = ex_rf(r, at + 16u), y1 = ex_rf(r, at + 20u), y2 = ex_rf(r, at + 24u);
                const float x0 = ex_rf(r, at + 0u), x1 = ex_rf(r, at + 4u), x2 = ex_rf(r, at + 8u);
                const float sy = y0 * y0 + y1 * y1 + y2 * y2, sx = x0 * x0 + x1 * x1 + x2 * x2;
                on = sy > 0.0025f && sx > 0.0025f && sy < 1e6f;
            }
            s->shown[n] = on;
        }
    }
    s->n = n;
}
// slots for the shown arms (sticky; earlier arms win), written into the actor's trail slots
void armtrail_assign(uint8_t* r, TrailState* s, const Entry* e) {
    const int lo = e->at_slot0, hi = e->at_slot0 + e->at_nslot;
    for (int k = 0; k < s->n; k++)                       // arms no longer shown give their slot back
        if (s->slot_of[k] >= 0 && !s->shown[k]) {
            if (g_trail_log) std::fprintf(stderr, "[ARMTRAIL] actor %08X arm %d (bit %d) off slot %d\n", s->actor, k, s->bit[k], s->slot_of[k]);
            s->arm_in[s->slot_of[k]] = -1; s->slot_of[k] = -1;
        }
    for (int k = 0; k < s->n; k++) {
        if (!s->shown[k] || s->slot_of[k] >= 0) continue;
        int slot = -1;
        for (int q = lo; q < hi && slot < 0; q++) if (s->arm_in[q] < 0) slot = q;
        if (slot < 0) {                                  // full: take the slot of the lowest-priority arm after this one
            int worst = -1;
            for (int q = lo; q < hi; q++) if (s->arm_in[q] > k && (worst < 0 || s->arm_in[q] > s->arm_in[worst])) worst = q;
            if (worst < 0) continue;
            const int out = s->arm_in[worst];
            if (g_trail_log) std::fprintf(stderr, "[ARMTRAIL] actor %08X arm %d gives slot %d to arm %d\n", s->actor, out, worst, k);
            s->slot_of[out] = -1; slot = worst;
        }
        s->arm_in[slot] = k; s->slot_of[k] = slot;
        if (g_trail_log) std::fprintf(stderr, "[ARMTRAIL] actor %08X arm %d (bit %d) on slot %d\n", s->actor, k, s->bit[k], slot);
    }
    if (!ex_ok(s->actor, TRAIL_MASK + 4u)) return;
    uint32_t mask = ex_r32(r, s->actor + TRAIL_MASK);
    for (int q = lo; q < hi; q++) {
        const int k = s->arm_in[q];
        if (k < 0) continue;
        const uint32_t at = s->actor + TRAIL_SLOTS + 16u * static_cast<uint32_t>(q);
        const uint32_t v[4] = {static_cast<uint32_t>(e->at_id), static_cast<uint32_t>(s->bit[k]), 0x3F800000u, 0u};   // id, bit, 1.0f, no other actor
        std::memcpy(ex_p(r, at), v, sizeof v);
        mask |= 1u << q;
    }
    std::memcpy(ex_p(r, s->actor + TRAIL_MASK), &mask, 4);
}
void armtrail_consume_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t now = g_trail_clock++;
    for (TrailState& s : g_trail) {
        if (!s.actor || s.seen + 2u < now) continue;     // no bone pass for it lately: gone (its trails stop with the mask)
        const Entry* e = ex_entry(rdram, s.actor);
        if (!e || e->at_nset <= 0) { s.actor = 0; continue; }
        armtrail_assign(rdram, &s, e);
    }
    g_orig_trail_consume(rdram, ctx);
}
void armtrail_install() {
    if (!armtrail_wanted()) return;
    for (const Entry& e : g_entries) {
        if (e.at_nset <= 0) continue;
        std::string sets;
        for (int i = 0; i < e.at_nset; i++)
            sets += (i ? ", " : "") + std::to_string(e.at_bone0[i]) + "+" + std::to_string(e.at_count[i]) + " bits " +
                    std::to_string(e.at_bit0[i]) + ".." + std::to_string(e.at_bit0[i] + e.at_count[i] - 1) +
                    (e.at_draw[i] >= 0 ? " draw " + std::to_string(e.at_draw[i]) : "");
        std::fprintf(stderr, "[ROSTER] charId %d: arm trails id %d on slots %d..%d: %s\n", e.charId, e.at_id, e.at_slot0,
                     e.at_slot0 + e.at_nslot - 1, sets.c_str());
    }
    g_trail_log = std::getenv("PSPRECOMP_ARMTRAIL_LOG") != nullptr;
    g_orig_trail_consume = psp_dispatch_probe_lookup(TRAIL_CONSUME);
    if (g_orig_trail_consume) psp_dispatch_register(TRAIL_CONSUME, armtrail_consume_hook);
    else std::fprintf(stderr, "[ROSTER] arm trails hook NOT installed (no function at %08X)\n", TRAIL_CONSUME);
}

void exflash_bone_post(uint8_t* rdram, uint32_t actor);   // PSPRECOMP_EXFLASH_LOG, below
void bone_post_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[6]);
    g_orig_bone_post(rdram, ctx);
    armiger_update(rdram, actor);
    warp_bone_post(rdram, actor);      // Warp-Strike's thrown sword / wisps (warpfx=)
    apoc_bone_post(rdram, actor);      // Apocalypsis arms riding their magi (apocarms=)
    armtrail_bone_post(rdram, actor);  // which floating arms are shown, for their trails (armtrail=)
    warpdash_tick(rdram, actor);       // warp dash: arm the link when its hit connects (warpdash=)
    chain_tick(rdram, ctx, actor);     // a chain window's action starts here (chainact=)
    exflash_bone_post(rdram, actor);   // PSPRECOMP_EXFLASH_LOG
}

// A projectile stopped by exranged: spin burst, and the deflect sparkle on the arm facing the attacker.
void ex_deflect(uint8_t* r, uint32_t def, uint32_t hit, recomp_context* ctx) {
    const Entry* e = ex_entry(r, def);
    if (!e || e->armiger[0] < 0) return;
    ArmState* s = arm_state(def);
    s->seen = ++g_arm_clock;
    s->burst = 1.0f;
    if (s->cool > 0.0f || e->armiger[8] < 0 || !g_play_fx) return;
    const uint32_t ctrl = ex_r32(r, def + EX_ACTOR_MODEL);
    if (!ex_ok(ctrl, 0x110)) return;
    const uint32_t gm = ex_r32(r, ctrl + 0x0Cu);
    if (!ex_ok(gm, 0x24)) return;
    const int n = arm_count(r, gm, e);
    if (!n) return;
    const uint32_t fxp = ex_r32(r, def + 0x390u);
    if (!ex_ok(fxp, 0x60)) return;
    const uint32_t mgr = ex_r32(r, fxp + 0x5Cu);
    if (!ex_ok(mgr, 4)) return;
    // direction to the attacker (the hit object's attacking object), in world space
    float W[16]; read_mat(r, def + EX_ACTOR_WORLD, W);
    float dx = 0.0f, dz = 1.0f;
    const uint32_t att = ex_r32(r, hit + 0x18u);
    if (att != def && ex_ok(att, 0x400)) {
        dx = ex_rf(r, att + EX_ACTOR_WORLD + 48u) - W[12]; dz = ex_rf(r, att + EX_ACTOR_WORLD + 56u) - W[14];
    }
    int best = 0; float bd = -1e30f;
    for (int k = 0; k < n; k++) {
        float A[16]; read_mat(r, ex_bone_mat(r, gm, e->armiger[6] + k), A);
        const float ax = A[12] * W[0] + A[13] * W[4] + A[14] * W[8], az = A[12] * W[2] + A[13] * W[6] + A[14] * W[10];
        const float d = ax * dx + az * dz;                  // arm offset from his origin . direction to the attacker
        if (d > bd) { bd = d; best = k; }
    }
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(mgr); ctx->r[5] = e->armiger[8] + best; ctx->r[6] = static_cast<int32_t>(def + 0x4C8u);
    ctx->r[7] = 0; ctx->r[8] = 0;
    g_play_fx(r, ctx);
    *ctx = saved;
    s->cool = kDeflectCool;
}

// PSPRECOMP_VIS_LOG=1 also logs, once per (script, bone, kind), every show (FUN_088372B0) / hide (FUN_088372D0) the
// engine applies to an extra character's draw bones, and once per timeline the ranged/keyed counts the event walkers
// see (FUN_08828EEC ranged, FUN_08829104 keyed): to find timeline events that never run.
FuncPtr g_orig_vis_show = nullptr, g_orig_vis_hide = nullptr, g_orig_rwalk = nullptr, g_orig_kwalk = nullptr;
void vis_ev_log(uint8_t* rdram, recomp_context* ctx, const char* kind) {
    const uint32_t actor = static_cast<uint32_t>(ctx->r[4]);
    const unsigned bone = static_cast<unsigned>(ctx->r[5]) & 0xFFu;
    if (!ex_entry(rdram, actor)) return;
    static std::vector<uint64_t> seen;
    const uint64_t key = (static_cast<uint64_t>(ex_r32(rdram, actor + 0x180u) & 0xFFFFu) << 16) | (bone << 1) | (kind[0] == 's');
    if (std::find(seen.begin(), seen.end(), key) != seen.end() || seen.size() > 4000) return;
    seen.push_back(key);
    std::fprintf(stderr, "[VISEV] script %u %s bone %u\n", ex_r32(rdram, actor + 0x180u), kind, bone);
}
void vis_show_hook(uint8_t* rdram, recomp_context* ctx) { vis_ev_log(rdram, ctx, "show"); g_orig_vis_show(rdram, ctx); }
void vis_hide_hook(uint8_t* rdram, recomp_context* ctx) { vis_ev_log(rdram, ctx, "hide"); g_orig_vis_hide(rdram, ctx); }
void walk_log(uint8_t* rdram, recomp_context* ctx, const char* kind) {
    const uint32_t tl = static_cast<uint32_t>(ctx->r[4]);
    static std::vector<uint32_t> seen;
    if (std::find(seen.begin(), seen.end(), tl) != seen.end() || seen.size() > 2000) return;
    seen.push_back(tl);
    if (!ex_ok(tl, 8)) return;
    const uint8_t* t = ex_p(rdram, tl);
    std::fprintf(stderr, "[TLWALK] %s timeline %08X: motion %u ranged %u keyed %u (header %02X %02X %02X %02X %02X %02X %02X %02X)\n", kind, tl,
                 t[0] | (t[1] << 8), t[2], t[3], t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
}
void rwalk_hook(uint8_t* rdram, recomp_context* ctx) { walk_log(rdram, ctx, "ranged"); g_orig_rwalk(rdram, ctx); }
void kwalk_hook(uint8_t* rdram, recomp_context* ctx) { walk_log(rdram, ctx, "keyed"); g_orig_kwalk(rdram, ctx); }
// Battle text lookups, logged (diagnostic for the move-name banner, 22 Oct): every distinct (table, record, caller) the
// battle overlay asks the EBOOT text getter FUN_0887AFBC(table, record, ...) for, and every distinct (ability id, caller)
// it looks up with FUN_088B1D9C (the 'ablt' bsearch). The ability-name table (ability.bin) is at 0x09B49108. Logged
// while the battle overlay is loaded, at most kTxtLogMax lines each, prefixed [TEXTLOG].
FuncPtr g_orig_txt_get = nullptr, g_orig_ablt_find = nullptr;
constexpr size_t kTxtLogMax = 400;
std::vector<std::array<uint32_t, 3>> g_txt_seen, g_ablt_seen;
bool battle_ovl(const uint8_t* rdram) {
    uint32_t ovl;
    std::memcpy(&ovl, rdram + (OVL_BASE & 0x07FFFFFFu), 4);
    return ovl == 1u;
}
bool txtlog_new(std::vector<std::array<uint32_t, 3>>& seen, uint32_t a, uint32_t b, uint32_t c) {
    if (seen.size() >= kTxtLogMax) return false;
    for (const auto& x : seen) if (x[0] == a && x[1] == b && x[2] == c) return false;
    seen.push_back({a, b, c});
    return true;
}
void txt_get_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t table = static_cast<uint32_t>(ctx->r[4]), rec = static_cast<uint32_t>(ctx->r[5]) & 0xFFFFu,
                   ra = static_cast<uint32_t>(ctx->r[31]);
    g_orig_txt_get(rdram, ctx);
    if (battle_ovl(rdram) && txtlog_new(g_txt_seen, table, rec, ra)) {
        std::string txt;
        const uint32_t sp = static_cast<uint32_t>(ctx->r[2]);
        if (sp >= 0x08000000u && sp < 0x0C000000u)
            for (uint32_t k = 0; k < 40; k++) {
                const uint16_t ch = guest_rd<uint16_t>(rdram, sp + 2u * k);
                if (!ch) break;
                txt += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
            }
        std::fprintf(stderr, "[TEXTLOG] text table %08X record %u (0x%X) from %08X -> %08X \"%s\"\n", table, rec, rec, ra, sp, txt.c_str());
    }
}
void ablt_find_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = static_cast<uint32_t>(ctx->r[4]) & 0xFFFFu, ra = static_cast<uint32_t>(ctx->r[31]);
    g_orig_ablt_find(rdram, ctx);
    if (battle_ovl(rdram) && txtlog_new(g_ablt_seen, id, ra, 0)) {
        const uint32_t r = static_cast<uint32_t>(ctx->r[2]);
        const bool ok = r >= 0x08000000u && r < 0x0C000000u;
        std::fprintf(stderr, "[TEXTLOG] ablt %04X from %08X -> name text %u, command %03X\n", id, ra,
                     ok ? guest_rd<uint16_t>(rdram, r + 2u) : 0xFFFFu, ok ? guest_rd<uint16_t>(rdram, r + 0xCu) : 0xFFFFu);
    }
}
// FUN_0887A800(entry, record, count) -> byte offset of the record in a mess-pak string entry: the common step of every
// text getter (0887AFBC, 0887AD78, 0887B3EC, ...). Logged per distinct (entry, record) while the battle overlay is loaded,
// with the decoded text (plain layout: u16 length then UTF-16; "OFF4": u32 length then UTF-16).
FuncPtr g_orig_txt_off = nullptr;
std::vector<std::array<uint32_t, 3>> g_txtoff_seen;
void txt_off_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t entry = static_cast<uint32_t>(ctx->r[4]), rec = static_cast<uint32_t>(ctx->r[5]);
    g_orig_txt_off(rdram, ctx);
    if (!battle_ovl(rdram) || !txtlog_new(g_txtoff_seen, entry, rec, 0)) return;
    const uint32_t off = static_cast<uint32_t>(ctx->r[2]);
    if (entry < 0x08000000u || entry >= 0x0C000000u || off > 0x400000u) return;
    const bool off4 = guest_rd<uint32_t>(rdram, entry) == 0x3446464Fu;   // "OFF4"
    const uint32_t at = entry + off + (off4 ? 4u : 2u);
    std::string txt;
    for (uint32_t k = 0; k < 48; k++) {
        const uint16_t ch = guest_rd<uint16_t>(rdram, at + 2u * k);
        if (!ch) break;
        txt += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?';
    }
    std::fprintf(stderr, "[TEXTLOG] entry %08X record %u (0x%X)%s -> \"%s\"\n", entry, rec, rec, off4 ? " OFF4" : "", txt.c_str());
}
void textlog_install() {
    g_orig_txt_get = psp_dispatch_probe_lookup(0x0887AFBCu);
    g_orig_ablt_find = psp_dispatch_probe_lookup(0x088B1D9Cu);
    if (g_orig_txt_get) psp_dispatch_register(0x0887AFBCu, txt_get_hook);
    if (g_orig_ablt_find) psp_dispatch_register(0x088B1D9Cu, ablt_find_hook);
    g_orig_txt_off = psp_dispatch_probe_lookup(0x0887A800u);
    if (g_orig_txt_off) psp_dispatch_register(0x0887A800u, txt_off_hook);
    std::fprintf(stderr, "[ROSTER] [TEXTLOG] logging battle text lookups (text getter %s, ablt lookup %s)\n",
                 g_orig_txt_get ? "hooked" : "NOT found", g_orig_ablt_find ? "hooked" : "NOT found");
}
void vis_log_install() {
    if (!std::getenv("PSPRECOMP_VIS_LOG")) return;
    g_orig_vis_show = psp_dispatch_probe_lookup(0x088372B0u);
    g_orig_vis_hide = psp_dispatch_probe_lookup(0x088372D0u);
    g_orig_rwalk = psp_dispatch_probe_lookup(0x08828EECu);
    g_orig_kwalk = psp_dispatch_probe_lookup(0x08829104u);
    if (g_orig_vis_show) psp_dispatch_register(0x088372B0u, vis_show_hook);
    if (g_orig_vis_hide) psp_dispatch_register(0x088372D0u, vis_hide_hook);
    if (g_orig_rwalk) psp_dispatch_register(0x08828EECu, rwalk_hook);
    if (g_orig_kwalk) psp_dispatch_register(0x08829104u, kwalk_hook);
    std::fprintf(stderr, "[ROSTER] PSPRECOMP_VIS_LOG: logging the draw-bone table, show/hide calls and timeline walks\n");
}
// PSPRECOMP_EXFLASH_LOG=<bone>[,<fepa id>...] (diagnostic, 23 Oct; request: "there's still no flash at the last hit of ex
// burst"): for an extra character, logs every FEPA play request by id (FUN_088AA5B8(fx manager, out, ..., a3 = FEPA id):
// how many instances it created; 0 = no record or the FX heap refused) for the listed ids (none listed: ids >= 500 and
// the EX ids 72-100), and, per motion run, the frames where skin bone <bone> is drawn (its matrix scale > 1 %): the
// model / world position on every frame it is drawn (world: with the actor's
// matrix at +0x330, as the warp code reads it; compare with [FEPLOG] cam). Noctis: 208 = rc8_burst (the Royal Cleave light burst, also
// keyed at the EX Burst final hit in `_808A` since v8.4). Lines prefixed [EXFLASH], at most 400.
FuncPtr g_orig_fx_by_id = nullptr;
int g_exflash_bone = -1, g_exflash_lines = 0;
std::vector<uint32_t> g_exflash_ids;
struct ExFlashRun { uint32_t actor = 0; int motion = -1; bool on = false; float peak = 0.0f; };
std::vector<ExFlashRun> g_exflash_runs;
bool exflash_say() { return g_exflash_lines < 400; }
void fx_by_id_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mgr = static_cast<uint32_t>(ctx->r[4]), id = static_cast<uint32_t>(ctx->r[7]);
    g_orig_fx_by_id(rdram, ctx);
    const bool want = g_exflash_ids.empty() ? (id >= 500u || (id >= 72u && id <= 100u))
                                            : std::find(g_exflash_ids.begin(), g_exflash_ids.end(), id) != g_exflash_ids.end();
    if (want && exflash_say()) {
        g_exflash_lines++;
        std::fprintf(stderr, "[EXFLASH] fx manager %08X: FEPA id %u -> %d instance(s)\n", mgr, id, static_cast<int>(ctx->r[2]));
    }
}
void exflash_bone_post(uint8_t* rdram, uint32_t actor) {
    if (g_exflash_bone < 0 || !exflash_say() || !ex_entry(rdram, actor)) return;
    const uint32_t ctrl = guest_rd<uint32_t>(rdram, actor + ACT_MODEL);
    if (!guest_ok(ctrl, 0x110u)) return;
    const uint32_t gm = guest_rd<uint32_t>(rdram, ctrl + 0x0Cu);
    if (!guest_ok(gm, 0x24u)) return;
    const uint32_t at = ex_bone_mat(rdram, gm, g_exflash_bone);
    if (!at) return;
    float M[16], W[16];
    std::memcpy(M, rdram + (at & 0x07FFFFFFu), sizeof M);
    std::memcpy(W, rdram + ((actor + ACT_WORLD) & 0x07FFFFFFu), sizeof W);
    const float sc = std::sqrt(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]);
    const int motion = warp_motion_now(rdram, actor);
    ExFlashRun* run = nullptr;
    for (ExFlashRun& x : g_exflash_runs) if (x.actor == actor) run = &x;
    if (!run) { g_exflash_runs.push_back({}); run = &g_exflash_runs.back(); run->actor = actor; }
    if (run->motion != motion) { run->motion = motion; run->on = false; run->peak = 0.0f; }
    if (sc > 0.01f) {   // every frame it is drawn: model and world position (row vectors: p W), the actor's origin
        float w[3];
        for (int j = 0; j < 3; j++) w[j] = M[12] * W[j] + M[13] * W[4 + j] + M[14] * W[8 + j] + W[12 + j];
        g_exflash_lines++;
        std::fprintf(stderr, "[EXFLASH] actor %08X motion %d: bone %d drawn, scale %.2f at model (%.2f %.2f %.2f) world (%.2f %.2f %.2f), "
                     "actor origin (%.2f %.2f %.2f)%s\n", actor, motion, g_exflash_bone, sc, M[12], M[13], M[14], w[0], w[1], w[2],
                     W[12], W[13], W[14], run->on ? "" : " (first frame)");
        run->on = true; run->peak = std::fmax(run->peak, sc);
    } else if (run->on) {
        g_exflash_lines++;
        std::fprintf(stderr, "[EXFLASH] actor %08X motion %d: bone %d hidden again (peak scale %.2f)\n", actor, motion, g_exflash_bone, run->peak);
        run->on = false;
    }
}
void exflash_log_install() {
    const char* env = std::getenv("PSPRECOMP_EXFLASH_LOG");
    if (!env) return;
    char* end = nullptr;
    g_exflash_bone = static_cast<int>(std::strtol(env, &end, 10));
    while (end && *end == ',') {
        const char* s = end + 1;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (end == s) break;
        g_exflash_ids.push_back(static_cast<uint32_t>(v));
    }
    g_orig_fx_by_id = psp_dispatch_probe_lookup(0x088AA5B8u);
    if (g_orig_fx_by_id) psp_dispatch_register(0x088AA5B8u, fx_by_id_hook);
    std::fprintf(stderr, "[ROSTER] PSPRECOMP_EXFLASH_LOG: bone %d, FEPA play-by-id log %s\n", g_exflash_bone,
                 g_orig_fx_by_id ? "hooked" : "NOT found");
}
void exmode_install() {
    exflash_log_install();
    vis_log_install();
    textlog_install();
    armtrail_install();
    bool ranged = false, arm = false;
    for (const Entry& e : g_entries) {
        for (const ProjBlock& b : e.projblocks) {
            ranged = true;
            std::string ms;
            for (int m : b.motions) ms += (ms.empty() ? "" : "/") + std::to_string(m);
            std::fprintf(stderr, "[ROSTER] charId %d: projectiles blocked in motion %s, ticks %.0f-%.0f (%d deflect sparkles)\n", e.charId, ms.c_str(), b.from, b.to, b.nfx);
        }
        if (e.exranged > 0) { ranged = true; std::fprintf(stderr, "[ROSTER] charId %d: immune to %s in EX Mode\n", e.charId, e.exranged >= 2 ? "projectiles and summoned/thrown objects" : "projectiles"); }
        if (e.armiger[0] >= 0) { arm = true; std::fprintf(stderr, "[ROSTER] charId %d: armiger bones %d,%d,%d draw %d,%d arms %d+%d deflect fx %d\n", e.charId, e.armiger[0], e.armiger[1], e.armiger[2], e.armiger[3], e.armiger[4], e.armiger[6], e.armiger[7], e.armiger[8]); }
    }
    if (ranged) {
        g_orig_can_hit = psp_dispatch_probe_lookup(CAN_HIT);
        if (!g_play_fx) g_play_fx = psp_dispatch_probe_lookup(PLAY_FX);
        g_pb_log = std::getenv("PSPRECOMP_PROJBLOCK_LOG") != nullptr;
        if (g_orig_can_hit) psp_dispatch_register(CAN_HIT, can_hit_hook);
        else std::fprintf(stderr, "[ROSTER] EX ranged immunity hook NOT installed (no function at %08X)\n", CAN_HIT);
    }
    if (arm || warp_fx_wanted() || apoc_wanted() || warpdash_wanted() || armtrail_wanted() || chain_wanted()) {     // the bone pass also places Warp-Strike's sword / wisps (warpfx=)
        g_orig_bone_post = psp_dispatch_probe_lookup(BONE_POST);
        g_play_fx = psp_dispatch_probe_lookup(PLAY_FX);
        if (g_orig_bone_post) psp_dispatch_register(BONE_POST, bone_post_hook);
        else std::fprintf(stderr, "[ROSTER] armiger hook NOT installed (no function at %08X)\n", BONE_POST);
    }
}
}  // namespace

// Called from register_hooks.
void dissidia_roster_grid_install() {
    // The menu overlay's grid bounds were raised to kGridRows (overlay_menu.toml), so the record table
    // accessor is always replaced: the extra rows must read as 0xFF (never a charId), not as whatever
    // follows the retail table.
    psp_dispatch_register(0x08A85ECCu, grid_table_hook);
    if (!g_enabled || extras().empty()) return;
    g_orig_placer = ovl_menu_lookup(0x08A7FB54u);
    if (g_orig_placer) psp_dispatch_register(0x08A7FB54u, placer_wrap);
    g_orig_cursor = ovl_menu_lookup(0x08A84458u);
    if (g_orig_cursor) psp_dispatch_register(0x08A84458u, cursor_wrap);
    g_orig_setframe = psp_dispatch_probe_lookup(0x088D0984u);
    if (g_orig_setframe) psp_dispatch_register(0x088D0984u, setframe_hook);
    {   // the setup panel binders (callers of FUN_088D0984 that add the charId to an entity base)
        static const uint32_t addr[4] = { 0x08AA43ECu, 0x08AA44ACu, 0x08AA46DCu, 0x08AA6148u };
        static const FuncPtr wrap[4] = { binder_wrap<0>, binder_wrap<1>, binder_wrap<2>, binder_wrap<3> };
        for (int i = 0; i < 4; i++) {
            g_orig_binder[i] = ovl_menu_lookup(addr[i]);
            if (g_orig_binder[i]) psp_dispatch_register(addr[i], wrap[i]);
        }
    }
    if (const char* w = std::getenv("PSPRECOMP_ROSTER_WATCHCPY")) {
        g_wc_addr = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        g_orig_memcpy = psp_dispatch_probe_lookup(0x08935524u);
        g_orig_memset = psp_dispatch_probe_lookup(0x08935828u);
        if (g_orig_memcpy) psp_dispatch_register(0x08935524u, wc_memcpy);
        if (g_orig_memset) psp_dispatch_register(0x08935828u, wc_memset);
    }
    g_orig_saveidx = psp_dispatch_probe_lookup(0x088CB3B4u);
    if (g_orig_saveidx) psp_dispatch_register(0x088CB3B4u, saveidx_hook);
    g_orig_voiceslot = psp_dispatch_probe_lookup(0x088A3C98u);
    if (g_orig_voiceslot) psp_dispatch_register(0x088A3C98u, voiceslot_hook);
    g_orig_textcreate = psp_dispatch_probe_lookup(0x0887B3ECu);
    g_orig_textlit = psp_dispatch_probe_lookup(0x0887D868u);
    if (g_orig_textcreate && g_orig_textlit && !std::getenv("PSPRECOMP_ROSTER_TRACE")) psp_dispatch_register(0x0887B3ECu, oppinfo_name_hook);
    g_orig_saverec = psp_dispatch_probe_lookup(0x088CB164u);
    if (g_orig_saverec) psp_dispatch_register(0x088CB164u, saverec_hook);
    {   // the extras' own records in a sidecar of each save (savedata import stubs, bound by psp_hle_init before this)
        const uint32_t init = psp_hle_stub_addr_for_nid(NID_SD_INIT), status = psp_hle_stub_addr_for_nid(NID_SD_STATUS),
                       shutdown = psp_hle_stub_addr_for_nid(NID_SD_SHUTDOWN);
        g_orig_sd_init = init ? psp_dispatch_probe_lookup(init) : nullptr;
        g_orig_sd_status = status ? psp_dispatch_probe_lookup(status) : nullptr;
        g_orig_sd_shutdown = shutdown ? psp_dispatch_probe_lookup(shutdown) : nullptr;
        g_orig_party_init = psp_dispatch_probe_lookup(PARTY_INIT);
        if (g_orig_sd_init && g_orig_sd_status && g_orig_sd_shutdown && g_orig_party_init) {
            psp_dispatch_register(init, sd_init_hook);
            psp_dispatch_register(status, sd_status_hook);
            psp_dispatch_register(shutdown, sd_shutdown_hook);
            psp_dispatch_register(PARTY_INIT, party_init_hook);
        } else {
            std::fprintf(stderr, "[ROSTER] savedata hooks NOT installed: the extra characters' records are not saved with the game\n");
        }
    }
    g_orig_charinfo = psp_dispatch_probe_lookup(0x088B3B20u);
    if (g_orig_charinfo) psp_dispatch_register(0x088B3B20u, charinfo_hook);
    g_orig_vsload = ovl_battle_lookup(0x08AF6B70u);
    if (g_orig_vsload && std::getenv("PSPRECOMP_ROSTER_DEBUG")) psp_dispatch_register(0x08AF6B70u, vsload_dbg);
    if (std::getenv("PSPRECOMP_ROSTER_LOADLOG")) {
        g_orig_loadreq = psp_dispatch_probe_lookup(0x088D0810u);
        if (g_orig_loadreq) psp_dispatch_register(0x088D0810u, loadreq_log);
    }
    trace_install();
    dash_install();
    exmode_install();
    warp_install();
    apoc_install();
    land_install();
    warpdash_install();
    chain_install();
    g_orig_list_build = psp_dispatch_probe_lookup(0x088D5AC8u);
    if (g_orig_list_build) psp_dispatch_register(0x088D5AC8u, list_build_hook);
    grid40_install();
    std::fprintf(stderr, "[ROSTER] grid hooks installed (%zu extra character(s), placer %s, cursor %s, setup binder %s)\n", extras().size(),
                 g_orig_placer ? "wrapped" : "NOT found", g_orig_cursor ? "wrapped" : "NOT found", g_orig_setframe ? "wrapped" : "NOT found");
}

// main.bin tables the roster tools grow, and that the game copies into fixed EBOOT buffers sized for (about) the
// retail table, with no bound check. Each loader copies the table into its buffer once (the manager's table pointer
// is still 0), then FUN_register(manager, buffer) stores the pointer. Lookups go through the manager pointer and the
// table's own count (binary search on the row's first byte / halfword), and nothing else reads the buffers, so a
// bigger table is copied into a block of its own from the host heap and registered there instead. Tables that fit
// run the original code. The blocks are permanent (the loaders run once, at boot).
//  * objentry.bin (object list, 0x44-byte records sorted by the u16 id at +0): FUN_08847158 copies it to 0x09AFEE58,
//    a 0x13D30-byte buffer (1,194 records; retail has 1,170). The objs= clones for Y'shtola's aeons took it to 1,207
//    and the extra 884 bytes overwrote the start of the next table (battle.bin 'spec' at 0x09B12B88): every fighter
//    lost walking and landing. Found 19 Oct by bisecting main.bin (only the objentry change reproduces it).
//  * battle.bin 'wpnp' (summoned / thrown weapon objects, 0x3C-byte rows): 84 rows at 0x09B16930 (FUN_08850AB8).
//  * system.bin 'chrp' (character parameters, 0x90-byte rows keyed by the charId byte at +0): 34 rows at 0x09B4D4A0
//    (FUN_088B70EC); charId 35 made it 0x90 bytes too long, over the globals at 0x09B4E7C8..0x09B4E857.
//  * system.bin 'ablt' (abilities): a heap block with room for 808 records (FUN_088B1CC0, ablt_load_hook below).
// The blocks come from the host heap, not the 0x0870xxxx scratch area: that lies inside the PSP volatile memory
// (0x08400000-0x087FFFFF) that the game locks and loads into (sceKernelVolatileMemLock).
// The hooks are installed whether or not a roster is configured.
namespace {
inline uint32_t rd32(const uint8_t* rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x07FFFFFFu), 4); return v; }

// Copy an oversized table into a new host block and register it. False (nothing changed) if no memory is left.
bool relocate_table(uint8_t* rdram, recomp_context* ctx, const char* tag, const char* block, uint32_t src, uint32_t size,
                    uint32_t retail_bytes, FuncPtr reg, uint32_t mgr) {
    const uint32_t buf = psp_kmem_bump_alloc_named((size + 0xFFu) & ~0xFFu, block);
    if (buf == 0) {
        std::fprintf(stderr, "[ROSTER] %s: no host memory for a %u-byte table: the game's %u-byte buffer will overflow\n", tag, size, retail_bytes);
        return false;
    }
    std::memcpy(rdram + (buf & 0x07FFFFFFu), rdram + (src & 0x07FFFFFFu), size);
    std::fprintf(stderr, "[ROSTER] %s: %u records (%u bytes, the game's buffer holds %u) moved to 0x%08X\n",
                 tag, rd32(rdram, buf + 4), size, retail_bytes, buf);
    ctx->r[4] = static_cast<int32_t>(mgr);
    ctx->r[5] = static_cast<int32_t>(buf);
    reg(rdram, ctx);
    return true;
}

struct TableReloc {
    const char* tag;
    const char* block;
    uint32_t copy_fn, register_fn, mgr_global, row, retail_rows;
    FuncPtr orig_copy, reg;
};
TableReloc g_reloc[] = {
    { "wpnp", "ROSTER_WPNP", 0x08850AB8u, 0x08850888u, 0x089D4DA8u, 0x3Cu, 84u, nullptr, nullptr },
    { "chrp", "ROSTER_CHRP", 0x088B70ECu, 0x088B706Cu, 0x089D7368u, 0x90u, 34u, nullptr, nullptr },
};

template <int I>
void table_copy_hook(uint8_t* rdram, recomp_context* ctx) {   // FUN_X(src, size)
    TableReloc& t = g_reloc[I];
    const uint32_t src = static_cast<uint32_t>(ctx->r[4]), size = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t retail = 8u + t.retail_rows * t.row;
    const uint32_t mgr = rd32(rdram, t.mgr_global);
    if (size <= retail || rd32(rdram, mgr) != 0) { t.orig_copy(rdram, ctx); return; }
    if (!relocate_table(rdram, ctx, t.tag, t.block, src, size, retail, t.reg, mgr)) t.orig_copy(rdram, ctx);
}

// FUN_08847158(mpk): objentry.bin. With an MPK (main.bin) it finds the entry with FUN_08869AB4(mpk, name) and copies
// entry+8 bytes from entry+4; with 0 it reads the loose file, at most 0x13D30 bytes (left to the original).
constexpr uint32_t OBJ_LOAD = 0x08847158u, OBJ_FIND = 0x08869AB4u, OBJ_REGISTER = 0x08846C24u;
constexpr uint32_t OBJ_MGR = 0x089D4968u, OBJ_NAME = 0x089D496Cu, OBJ_BUF_BYTES = 0x13D30u;
FuncPtr g_orig_obj_load = nullptr, g_obj_register = nullptr;
void objentry_load_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mpk = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t mgr = rd32(rdram, OBJ_MGR);
    FuncPtr find = psp_dispatch_probe_lookup(OBJ_FIND);
    if (mpk == 0 || rd32(rdram, mgr) != 0 || !find) { g_orig_obj_load(rdram, ctx); return; }
    const recomp_context saved = *ctx;
    ctx->r[4] = static_cast<int32_t>(mpk);
    ctx->r[5] = static_cast<int32_t>(rd32(rdram, OBJ_NAME));
    find(rdram, ctx);
    const uint32_t entry = static_cast<uint32_t>(ctx->r[2]);
    *ctx = saved;
    if (entry == 0) { g_orig_obj_load(rdram, ctx); return; }
    const uint32_t src = rd32(rdram, entry + 4), size = rd32(rdram, entry + 8);
    if (size <= OBJ_BUF_BYTES) { g_orig_obj_load(rdram, ctx); return; }
    if (!relocate_table(rdram, ctx, "objentry", "ROSTER_OBJENTRY", src, size, OBJ_BUF_BYTES, g_obj_register, mgr))
        g_orig_obj_load(rdram, ctx);
}

// FUN_088B1CC0(table, size, mpk): system.bin 'ablt' (abilities: 16-byte header {'ABI ', version, count, 0}, 40-byte
// records sorted by id). It copies the table into a heap block of size + (808 - count) * 40 bytes (room for 808
// records), stores the block at 0x089D7290 and the records at 0x089D7294, then loads ability.bin from the archive (with
// size 0 it does only that). Nothing else writes the table. roster_mainbin.py gives every extra character its own copies
// of its like's records (Vivi, Noctis and Y'shtola: 790 -> 824), and with more than 808 the block is shorter than the
// table: the copy would run over the heap behind it. Such a table goes into a host block instead.
constexpr uint32_t ABLT_LOAD = 0x088B1CC0u, ABLT_HDR = 0x089D7290u, ABLT_RECS = 0x089D7294u;
constexpr uint32_t ABLT_ROWS = 0x328u, ABLT_ROW = 0x28u;
FuncPtr g_orig_ablt_load = nullptr;
void ablt_load_hook(uint8_t* rdram, recomp_context* ctx) {
    saverec_block();   // the extra characters' save records: allocated at boot, while the host heap has room
    const uint32_t src = static_cast<uint32_t>(ctx->r[4]), size = static_cast<uint32_t>(ctx->r[5]);
    if (src == 0 || size < 16u || rd32(rdram, src + 8) <= ABLT_ROWS) { g_orig_ablt_load(rdram, ctx); return; }
    const uint32_t buf = psp_kmem_bump_alloc_named((size + 0xFFu) & ~0xFFu, "ROSTER_ABLT");
    if (buf == 0) {
        std::fprintf(stderr, "[ROSTER] ablt: no host memory for a %u-byte table: the game's block will overflow\n", size);
        g_orig_ablt_load(rdram, ctx);
        return;
    }
    std::memcpy(rdram + (buf & 0x07FFFFFFu), rdram + (src & 0x07FFFFFFu), size);
    put_u32(rdram, ABLT_HDR & 0x07FFFFFFu, buf);
    put_u32(rdram, ABLT_RECS & 0x07FFFFFFu, buf + 0x10u);
    std::fprintf(stderr, "[ROSTER] ablt: %u records (%u bytes, the game's block holds %u) moved to 0x%08X\n",
                 rd32(rdram, buf + 8), size, 0x10u + ABLT_ROWS * ABLT_ROW, buf);
    ctx->r[5] = 0;     // the original now only loads ability.bin (a2 = the archive)
    g_orig_ablt_load(rdram, ctx);
}
}  // namespace

// Called from dissidia_register_hooks; harmless with retail-sized tables (the originals run unchanged).
void dissidia_wpnp_install() {
    static const FuncPtr hooks[] = { table_copy_hook<0>, table_copy_hook<1> };
    static_assert(sizeof(hooks) / sizeof(hooks[0]) == sizeof(g_reloc) / sizeof(g_reloc[0]), "one hook per table");
    for (size_t i = 0; i < sizeof(g_reloc) / sizeof(g_reloc[0]); i++) {
        TableReloc& t = g_reloc[i];
        t.orig_copy = psp_dispatch_probe_lookup(t.copy_fn);
        t.reg = psp_dispatch_probe_lookup(t.register_fn);
        if (!t.orig_copy || !t.reg) { std::fprintf(stderr, "[ROSTER] %s buffer hook NOT installed\n", t.tag); continue; }
        psp_dispatch_register(t.copy_fn, hooks[i]);
    }
    g_orig_obj_load = psp_dispatch_probe_lookup(OBJ_LOAD);
    g_obj_register = psp_dispatch_probe_lookup(OBJ_REGISTER);
    if (!g_orig_obj_load || !g_obj_register) std::fprintf(stderr, "[ROSTER] objentry buffer hook NOT installed\n");
    else psp_dispatch_register(OBJ_LOAD, objentry_load_hook);
    g_orig_ablt_load = psp_dispatch_probe_lookup(ABLT_LOAD);
    if (!g_orig_ablt_load) std::fprintf(stderr, "[ROSTER] ablt buffer hook NOT installed\n");
    else psp_dispatch_register(ABLT_LOAD, ablt_load_hook);
}
