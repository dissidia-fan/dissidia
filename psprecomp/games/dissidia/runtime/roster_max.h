// games/dissidia/runtime/roster_max.h — the one place the roster size is defined.
//
// Read by roster.cpp (table sizes, grid rows, spec validation) and, as text, by tools/roster_patches.py
// (the slti immediates in overlay_menu.toml) and tools/roster_mainbin.py (--max). Keep the two defines
// plain integer literals on their own lines.
#pragma once

// charId slots the game was built with: 0 = none, 1..33 retail, 34 = the unfinished ORG800 (spare).
#define DISSIDIA_RETAIL_SLOTS 35
// charId slots with the mod: DISSIDIA_RETAIL_SLOTS + 8 extra cells of the 10x4 Quick Battle grid
// (rows 1 and 4 only use columns 3..8, which leaves 8 free cells). Valid charIds are 0..ROSTER_MAX-1.
#define DISSIDIA_ROSTER_MAX 43
// first charId that is not a plain retail character: 33 (the unused SEV120 slot) and 34 (ORG800 stub) are spare.
#define DISSIDIA_FIRST_NEW 33

// Guest scratch RAM (below the user base 0x08800000, free) used by the roster module. The relocated
// tables are DISSIDIA_ROSTER_MAX u32 entries (<= 0xB0 bytes) and are addressed by `lui hi` + `addiu lo`
// pairs patched into the overlays (tools/roster_patches.py), so every base needs hi == 0x0870.
#define DISSIDIA_SCRATCH      0x08700000   // 8-byte code strings, one per extra character
#define DISSIDIA_GRID_DST     0x08702000   // Quick Battle record table: 12 bytes x (33 + ROSTER_MAX - FIRST_NEW) rows
#define DISSIDIA_GATE         0x08701F00   // debug: trace gate byte (PSPRECOMP_ROSTER_TRACE_GATE)
#define DISSIDIA_TBL_MENU_A   0x08700400   // menu overlay: arcade last-chara gim names (was 0x089EA3C0)
#define DISSIDIA_TBL_MENU_B   0x087004C0   // menu overlay: arcade select p_%s_1p.gim names (was 0x089EC568)
#define DISSIDIA_TBL_NAME     0x08700580   // menu overlay: charId -> name text record (was 0x08A544E0)
#define DISSIDIA_TBL_BATTLE   0x08700640   // battle overlay: story/encounter face-in names (was 0x08AFBC68)
#define DISSIDIA_TBL_VS       0x08700700   // menu overlay: VS screen / battle prep face-in names (was 0x08B57FB8)
#define DISSIDIA_TBL_OPPINFO  0x08701A00   // battle overlay: Opponent Info name text ids, 8 bytes x ROSTER_MAX (was 0x08AFB6E8)
#define DISSIDIA_OPPINFO_STR  0x08701C00   // host-side: UTF-16 name buffer for the Opponent Info text (64 bytes)
#define DISSIDIA_CHARINFO     0x08700800   // host-side: 0x68-byte character info records for extra charIds (ROSTER_MAX x 0x68 < 0x1800)
#define DISSIDIA_SAVEREC      0x08710000   // host-side: 0x778-byte per-character save records for extra charIds ((ROSTER_MAX - 34) x 0x778 < 0x4400)

