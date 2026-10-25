// Test stand-in for the generated header: a game with NO host-heap keys in game.toml
// (the default case) — the RECOMP_HOST_HEAP_* macros are absent and must default to the
// historical single region ending at 0x0B000000.
#pragma once
#define RECOMP_GAME_ID         "test"
#define RECOMP_BOOT_PATH       "disc0:/PSP_GAME/SYSDIR/BOOT.BIN"
#define RECOMP_HEAP_OVERRIDE   0x00000000U
#define RECOMP_ASSET_LAYER_BND 0
