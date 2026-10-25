# FX components, part F: every channel outside A–E, plus the shared tables

Agent F, 24 Oct 2026. Source: the static recompilation (`work/output/generated/batch_*.cpp`), the decrypted
EBOOT (`work/EBOOT.dec.elf`, data tables) and the FEPs of the 31 retail fighters plus the 9 library FEPs
(`effect/system/system_effect.mpk` ×4, `system_effect_w.mpk` ×4, `effect/region/r_system-effe.mpk` ×1).
Scripts and dumps: `work/fx_re/scratch_F/` (`census.py`, `vt.py`, `reg.py`, `usage.py`, `chsum.py`, `tables.py`,
`m/chNNN.txt` = every non-stub method of each F channel as compacted C).
How sure each claim is: **confirmed** = read end to end in code; **strong** = code read, but one link
(meaning of a field or a callee) is inferred; **guess** = plausible, not traced.

## Summary

* **Dispatch (confirmed).** `0x089D95D0[ch]` holds a *slot address* `0x089D9858 + 4·type`. The registry code at
  `0x0894DD74` (inside FUN_0894DCC8) writes a vtable pointer into 96 slots, types 0–96 except 12, which no channel uses (type 0 = stub vtable
  `0x089DB1D0`; 66 channels map to it). A vtable is old-GCC style: two header words, then 9 entries of 8 bytes
  `{s16 this-delta, s16, fnptr}` at vt+8 … vt+72. Every method is called as
  `fn(this, call_ctx, param_block, state_block [, t0]; f12 = time)`, where `call_ctx+136` = effect instance,
  `+140` = runtime element (node instance), `+144` = the current Track, `+172` = the screen-copy object,
  `+176` = the per-frame vertex arena.
* **Method roles (confirmed by the callers).** m0 (vt+8) create, called by FUN_0894A104 with the init ParamSet,
  returns 0 = fail; m1 (vt+16) load key values, FUN_0894A3EC (init set and every new segment); m2 (vt+24)
  per-frame update = node list 0 (FUN_0894BA60 phase 2); m3 (vt+32) draw = list 1 (phase 3 and the render walkers
  FUN_08949E2C / FUN_0894A960); m4 (vt+40) release = list 2 (FUN_0894C938); m5 (vt+48) segment handler = list 3
  (phase 5, called from FUN_0894A2AC when a new key segment starts); m6 (vt+56) first-frame = list 5 (phase 7, only
  while element flag 4 is clear); m7 (vt+64) post-draw = list 6 (phase 8, FUN_0894A738 from FUN_0894A090);
  m8 (vt+72) not called by any dispatcher found (only types 54 and 60 override it). Lists 4 and 7 have no caller
  in FUN_0894BA60.
* **Track links (confirmed).** Track `{u16 ch, u16 state_off, u32 n, ptr u16 data}`: `data[0..n-1]` are the
  indices of other tracks of the same node this component reads, `data[n..2n-1]` their state offsets; the state
  block of track k is at `element + 96 + state_off`. Components read and write *other* channels' state this way
  (e.g. 62 multiplies the colour state by 61's state). The u16 list therefore is a dependency list, not keys.
* **Census.** 54 channel ids are used by nodes; 25 of them are F's. Channels assigned to other agents but used by
  **no** node in any of the 40 FEPs: 30, 53, 54, 55, 56, 57, 63, 78, 108, 119.
* **What F's channels do.** Eight draw: 107 full-screen colour flash (2D sprite rect), 110 zoom blur and 111
  rotation blur (the screen copy redrawn as a full-screen grid), 101 shockwave ring (screen-texture refraction
  disc + coloured ring), 100 spark streaks (GU_LINE_STRIP particles), 102 and 103 line sparks (GU_LINES), 95 one-pixel
  points (GU_POINTS), 82 a ribbon mesh along a path, 58 a one-off strip draw with its own matrices. The rest
  are modifiers or bookkeeping: 61+62 flicker/pulse envelope on colour, 25/31/32/33/34/39 randomisers, 19 gravity,
  29 face-along-velocity, 109 scalar driver feeding 110/111, 151 spawns child nodes, 66/68/70 build trail paths
  for 79/80/82. **No F channel plays sound, moves the camera, adds lights or fog**; screen effects are only the
  framebuffer redraws (110, 111, 101) and the flash (107). (Camera shake, hit-stop and sound are therefore driven
  outside the FEP channel system, e.g. by FXSE / timeline events.)
* **Shared blend switch (confirmed,** e.g. FUN_0895A16C, FUN_0895BDD4, FUN_08983CD8**).** Drawing F components
  pick one of three `sceGuBlendFunc` (FUN_089A12A4: op, src, dst, fixA, fixB) settings from a param byte:
  0 → `ADD, DOUBLE_SRC_ALPHA, ONE_MINUS_DOUBLE_SRC_ALPHA` (alpha blend, alpha 128 = opaque);
  1 → `ADD, DOUBLE_SRC_ALPHA, FIX(0xFFFFFF)` (additive); 2 → `REVERSE_SUBTRACT, DOUBLE_SRC_ALPHA, FIX(0xFFFFFF)`
  (dst − src·a, subtractive). WebGL: `a = min(1, 2·A/255)`; mode 0 `blendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`,
  mode 1 `blendFunc(SRC_ALPHA, ONE)`, mode 2 `blendEquation(FUNC_REVERSE_SUBTRACT); blendFunc(SRC_ALPHA, ONE)`.
  The colour they send is `clamp(colourState(ch15/16/17) × instance colour(inst+64), 0, 255)` packed ABGR.
* **Random numbers (confirmed).** Every randomiser uses the MSVC LCG at `0x089DA120`:
  `seed = seed·214013 + 2531011; r = ((seed>>16)&0x7FFF)/32767`. A preview can use any PRNG.

## Census of channel use (all 40 FEPs)

`nodes` = nodes having a track of that channel, `effects` = effects containing such a node, `fighters` = of the 31
retail fighters (Aerith sev120 and Chaos org200 have no `obj/p_*.objx` in this disc and are not fighters),
`lib nodes` = nodes in the 9 library FEPs. Sizes are the descriptor values at `0x089D9A98`.

| ch | owner | nodes | effects | fighters (of 31) | lib nodes | param B | state B | type | role (F: this note) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | A | 13580 | 2344 | 31 | 646 | 12 | 32 | 1 |  |
| 2 | A | 231 | 97 | 27 | 0 | 28 | 48 | 2 |  |
| 3 | A | 3276 | 1145 | 31 | 98 | 44 | 64 | 3 |  |
| 4 | A | 13331 | 2216 | 31 | 582 | 12 | 32 | 4 |  |
| 5 | A | 1539 | 760 | 30 | 24 | 28 | 48 | 5 |  |
| 6 | A | 1292 | 592 | 31 | 93 | 44 | 64 | 6 |  |
| 7 | A | 5874 | 1875 | 31 | 293 | 12 | 32 | 7 |  |
| 8 | A | 789 | 462 | 29 | 26 | 28 | 48 | 8 |  |
| 9 | A | 9502 | 2063 | 31 | 381 | 44 | 64 | 9 |  |
| 10 | C | 1878 | 970 | 31 | 94 | 31 | 7 | 10 |  |
| 11 | B | 4589 | 1611 | 31 | 228 | 26 | 0 | 11 |  |
| 14 | A | 16161 | 2428 | 31 | 698 | 6 | 0 | 14 |  |
| 15 | A | 3806 | 1600 | 31 | 137 | 16 | 32 | 15 |  |
| 16 | A | 566 | 369 | 30 | 0 | 32 | 48 | 16 |  |
| 17 | A | 11307 | 2281 | 31 | 545 | 48 | 64 | 17 |  |
| 18 | D | 1001 | 648 | 31 | 48 | 24 | 4 | 18 |  |
| 19 | F | 2 | 2 | 1 | 0 | 2 | 0 | 19 | gravity: acc.y += v*0.00098 |
| 20 | E | 5544 | 1920 | 31 | 224 | 75 | 32 | 20 |  |
| 21 | D | 2487 | 1327 | 31 | 113 | 277 | 14 | 21 |  |
| 22 | D | 499 | 321 | 31 | 12 | 44 | 0 | 22 |  |
| 23 | D | 3333 | 1468 | 31 | 94 | 44 | 0 | 23 |  |
| 24 | D | 906 | 553 | 31 | 25 | 44 | 0 | 24 |  |
| 25 | F | 73 | 49 | 13 | 7 | 54 | 0 | 25 | colour randomiser (value/vel/acc, once) |
| 26 | E | 223 | 154 | 27 | 15 | 26 | 6 | 26 |  |
| 29 | F | 11 | 11 | 6 | 0 | 0 | 16 | 28 | orient rotation along velocity |
| 31 | F | 2 | 1 | 1 | 0 | 16 | 0 | 30 | position randomiser |
| 32 | F | 15 | 15 | 5 | 0 | 16 | 0 | 31 | rotation randomiser |
| 33 | F | 15 | 15 | 7 | 0 | 16 | 0 | 32 | scale randomiser |
| 34 | F | 1 | 1 | 0 | 1 | 21 | 0 | 33 | colour randomiser (once) |
| 39 | F | 6 | 3 | 1 | 0 | 21 | 0 | 37 | colour randomiser (every key) |
| 58 | F | 1 | 1 | 1 | 0 | 146 | 4 | 47 | custom-matrix strip draw (1 use) |
| 61 | F | 602 | 368 | 31 | 34 | 74 | 56 | 48 | two-level pulse/flicker envelope |
| 62 | F | 602 | 368 | 31 | 34 | 0 | 32 | 49 | colour *= envelope(61) |
| 66 | F | 1 | 1 | 1 | 0 | 36 | 4 | 50 | trail path generator (variant) |
| 68 | F | 85 | 80 | 21 | 4 | 39 | 4 | 52 | trail path: sampled position history |
| 70 | F | 3 | 3 | 2 | 0 | 76 | 4 | 54 | trail path generator (variant, physics) |
| 79 | E | 122 | 107 | 28 | 4 | 0 | 16 | 56 |  |
| 80 | C | 86 | 82 | 21 | 4 | 72 | 8 | 57 |  |
| 82 | F | 1 | 1 | 1 | 0 | 148 | 12 | 80 | ribbon mesh along path 70 |
| 95 | F | 1 | 1 | 1 | 0 | 40 | 8 | 86 | point (1 px) particle emitter |
| 100 | F | 357 | 334 | 30 | 0 | 69 | 4 | 91 | spark streaks (LINE_STRIP particles) |
| 101 | F | 69 | 64 | 19 | 1 | 85 | 4 | 58 | shockwave ring (screen refraction + coloured ring) |
| 102 | F | 16 | 15 | 6 | 7 | 72 | 8 | 92 | line sparks, spherical burst (GU_LINES) |
| 103 | F | 13 | 13 | 7 | 3 | 112 | 8 | 93 | line rays/streaks (GU_LINES) |
| 105 | C/E | 188 | 129 | 30 | 0 | 86 | 32 | 59 |  |
| 106 | E | 188 | 129 | 30 | 0 | 76 | 4 | 60 |  |
| 107 | F | 193 | 191 | 31 | 0 | 28 | 4 | 61 | screen colour flash (2D rect) |
| 109 | F | 258 | 257 | 31 | 7 | 8 | 4 | 63 | scalar driver (value+speed) |
| 110 | F | 254 | 253 | 31 | 7 | 21 | 0 | 64 | screen zoom blur (framebuffer re-draw) |
| 111 | F | 4 | 4 | 4 | 0 | 18 | 0 | 65 | screen rotation blur (framebuffer re-draw) |
| 150 | E | 92 | 60 | 15 | 0 | 23 | 3 | 71 |  |
| 151 | F | 99 | 71 | 20 | 0 | 21 | 1 | 72 | spawn child nodes (burst) |

## Channel → type → vtable (all 162 channels)

`type` = (slot − 0x089D9858)/4. Methods are the function pointers at vt+12+8k (k = 0..8); `·` = the shared stub
(0894E378 returns 1; 0894E380/88/90/98/A8/B0/B8 return; 0894E3A0 returns 0). Roles of m0–m8 as in the summary.
Many method addresses are entry points inside a larger recompiled function (look them up as labels `L_<addr>`).

| ch | type | slot | vtable | desc {u8,u8,u16,param,state} | m0 create | m1 key | m2 update | m3 draw | m4 release | m5 seg | m6 first | m7 post | m8 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | 089D985C | 089DB450 | {1,2,0,12,32} | 08951860 | 08951924 | · | · | · | 089519A4 | 089519E0 | · | · |
| 2 | 2 | 089D9860 | 089DB4F0 | {2,3,0,28,48} | 08951E98 | 08951FCC | 089520D0 | · | · | 089520FC | 089521BC | · | · |
| 3 | 3 | 089D9864 | 089DB4A0 | {3,4,0,44,64} | 08951A98 | 08951CDC | 08951D00 | · | · | 08951D54 | 08951DE0 | · | · |
| 4 | 4 | 089D9868 | 089DB540 | {1,2,0,12,32} | 08952274 | 08952338 | · | · | · | 089523B8 | · | · | · |
| 5 | 5 | 089D986C | 089DB5E0 | {2,3,0,28,48} | 0895270C | 08952840 | 08952944 | · | · | 08952984 | · | · | · |
| 6 | 6 | 089D9870 | 089DB590 | {3,4,0,44,64} | 089523F4 | 089525A0 | 08952604 | · | · | 08952680 | · | · | · |
| 7 | 7 | 089D9874 | 089DB630 | {1,2,0,12,32} | 08952A44 | 08952B08 | · | · | · | 08952B78 | · | · | · |
| 8 | 8 | 089D9878 | 089DB6D0 | {2,3,0,28,48} | 08952ED8 | 0895300C | 08953100 | · | · | 08953158 | · | · | · |
| 9 | 9 | 089D987C | 089DB680 | {3,4,0,44,64} | 08952BAC | 08952D58 | 08952DB8 | · | · | 08952E4C | · | · | · |
| 10 | 10 | 089D9880 | 089DBA40 | {10,3,2,31,7} | 089556EC | 0895590C | 08955914 | 08955A84 | · | · | · | · | · |
| 11 | 11 | 089D9884 | 089DBA90 | {9,0,2,26,0} | 08955E84 | 08955E8C | · | 08955E94 | · | · | · | · | · |
| 13 | 13 | 089D988C | 089DB180 | {0,0,1,0,0} | 0894ECF0 | · | · | 0894ECF8 | · | · | · | · | · |
| 14 | 14 | 089D9890 | 089DB3B0 | {6,0,3,6,0} | 08950F5C | 0895115C | 08951330 | · | · | · | 08951164 | · | · |
| 15 | 15 | 089D9894 | 089DB090 | {1,2,0,16,32} | 0894E3C0 | 0894E4BC | · | · | · | 0894E574 | · | · | · |
| 16 | 16 | 089D9898 | 089DB130 | {2,3,0,32,48} | 0894E8E4 | 0894EBA8 | 0894EA58 | · | · | 0894EAB8 | · | · | · |
| 17 | 17 | 089D989C | 089DB0E0 | {3,4,0,48,64} | 0894E5B0 | 0894E7AC | 0894E7D0 | · | · | 0894E858 | · | · | · |
| 18 | 18 | 089D98A0 | 089DB220 | {9,1,1,24,4} | 0894ED48 | 0894ED58 | 0894ED6C | · | · | 0894ED64 | · | · | · |
| 19 | 19 | 089D98A4 | 089DB860 | {1,0,1,2,0} | 089541DC | 089541E4 | · | · | · | · | · | · | · |
| 20 | 20 | 089D98A8 | 089DBB80 | {16,4,2,75,32} | 08956164 | 089564FC | · | 08956200 | · | 08956820 | · | · | · |
| 21 | 21 | 089D98AC | 089DBBD0 | {64,4,3,277,14} | 089569C0 | 08956AD0 | 08957344 | 08958DB4 | 08956C64 | 08956CB8 | · | · | · |
| 22 | 22 | 089D98B0 | 089DC6F8 | {9,0,1,44,0} | 0896DA28 | 0896DA30 | · | · | · | · | · | · | · |
| 23 | 23 | 089D98B4 | 089DC7E8 | {9,0,1,44,0} | 0896E234 | 0896E23C | · | · | · | · | · | · | · |
| 24 | 24 | 089D98B8 | 089DC8D8 | {10,0,1,44,0} | 0896E9A8 | 0896E9B0 | · | · | · | · | · | · | · |
| 25 | 25 | 089D98BC | 089DC608 | {9,0,1,54,0} | 0896D11C | 0896D124 | · | · | · | · | · | · | · |
| 26 | 26 | 089D98C0 | 089DB310 | {9,2,1,26,6} | 0894FF60 | 0894FF74 | 0894FF88 | · | · | 0894FF80 | · | · | · |
| 27 | 27 | 089D98C4 | 089DB720 | {7,1,2,17,1} | 08953218 | 08953224 | 0895322C | · | · | · | · | · | · |
| 29 | 28 | 089D98C8 | 089DB900 | {0,4,2,0,16} | 08954290 | · | 089542E0 | · | · | · | · | 089542A4 | · |
| 30 | 29 | 089D98CC | 089DB360 | {10,2,1,28,6} | 08950628 | 0895063C | 08950650 | · | · | 08950648 | · | · | · |
| 31 | 30 | 089D98D0 | 089DC658 | {5,0,1,16,0} | 0896D644 | 0896D64C | · | · | · | · | · | · | · |
| 32 | 31 | 089D98D4 | 089DC748 | {5,0,1,16,0} | 0896DE44 | 0896DE4C | · | · | · | · | · | · | · |
| 33 | 32 | 089D98D8 | 089DC838 | {5,0,1,16,0} | 0896E5C0 | 0896E5C8 | · | · | · | · | · | · | · |
| 34 | 33 | 089D98DC | 089DC568 | {6,0,1,21,0} | 0896CD20 | 0896CD28 | · | · | · | · | · | · | · |
| 36 | 34 | 089D98E0 | 089DC6A8 | {5,0,1,16,0} | 0896D98C | 0896D994 | · | · | · | 0896D9DC | · | · | · |
| 37 | 35 | 089D98E4 | 089DC798 | {5,0,1,16,0} | 0896E198 | 0896E1A0 | · | · | · | 0896E1E8 | · | · | · |
| 38 | 36 | 089D98E8 | 089DC888 | {5,0,1,16,0} | 0896E90C | 0896E914 | · | · | · | 0896E95C | · | · | · |
| 39 | 37 | 089D98EC | 089DC5B8 | {6,0,1,21,0} | 0896D080 | 0896D088 | · | · | · | 0896D0D0 | · | · | · |
| 40 | 38 | 089D98F0 | 089DBAE0 | {13,9,2,63,74} | 08956104 | 0895610C | 08956114 | 0895611C | 08956124 | 0895612C | · | · | · |
| 41 | 39 | 089D98F4 | 089DBB30 | {20,13,2,112,106} | 08956134 | 0895613C | 08956144 | 0895614C | 08956154 | 0895615C | · | · | · |
| 46 | 40 | 089D98F8 | 089DB770 | {3,5,1,48,68} | 0895353C | 08953618 | 0895363C | · | 089537A4 | 089537AC | · | · | · |
| 50 | 41 | 089D98FC | 089DC1F8 | {7,4,2,13,88} | · | 08964250 | 08964268 | 08964258 | · | 08964260 | · | · | · |
| 53 | 42 | 089D9900 | 089DC068 | {3,1,2,24,4} | 0896178C | 08961818 | 08961820 | 08961980 | 08961C20 | 08961C84 | · | · | · |
| 54 | 43 | 089D9904 | 089DC0B8 | {18,2,2,189,8} | 08961CB8 | 08961FDC | 08961FE4 | 08962218 | 08962144 | 089621E4 | · | · | · |
| 55 | 44 | 089D9908 | 089DC108 | {6,2,2,36,8} | 089628D8 | 08962964 | 0896296C | 08962B64 | 08962ACC | 08962B30 | · | · | · |
| 56 | 45 | 089D990C | 089DC158 | {5,1,2,28,4} | 0896320C | 08963298 | 089632A0 | 089636FC | 08963B4C | 08963BB0 | · | · | · |
| 57 | 46 | 089D9910 | 089DC1A8 | {6,1,2,28,4} | 08963BE4 | 08963C70 | 08963C78 | 08963E70 | 08963DD8 | 08963E3C | · | · | · |
| 58 | 47 | 089D9914 | 089DBDB0 | {28,1,2,146,4} | 0895A488 | 0895A518 | 0895A520 | 0895A548 | 0895A668 | · | · | · | · |
| 59 | 74 | 089D9980 | 089DD1C0 | {11,1,4,32,4} | 08983A24 | 08983AB4 | 08983AD4 | · | 08983B44 | 08983B6C | · | · | · |
| 60 | 75 | 089D9984 | 089DD170 | {64,2,5,201,8} | 08983720 | 08983840 | 08983898 | 089838F8 | 0898399C | 089839F0 | · | · | · |
| 61 | 48 | 089D9918 | 089DB810 | {11,8,0,74,56} | 08953984 | 089539EC | 08953A50 | · | 08953CB0 | 08953CB8 | · | · | · |
| 62 | 49 | 089D991C | 089DB7C0 | {0,2,2,0,32} | 08953838 | 08953894 | 089538EC | · | 08953974 | 0895397C | · | · | · |
| 63 | 76 | 089D9988 | 089DD088 | {12,4,2,33,40} | 0897FCB8 | 0897FCF4 | 0897FD2C | 0897FF44 | 0897FF3C | · | · | · | · |
| 64 | 77 | 089D998C | 089DCD68 | {17,7,3,104,40} | 0897B2B0 | 0897B3D0 | 0897B3F4 | 0897B560 | 0897B84C | 0897B8E4 | · | · | · |
| 65 | 78 | 089D9990 | 089DCDB8 | {12,6,3,54,36} | 0897C524 | 0897C644 | 0897C668 | 0897C798 | 0897C9B4 | 0897CA4C | · | · | · |
| 66 | 50 | 089D9920 | 089DC338 | {8,1,2,36,4} | 08965DD4 | 08965E8C | 08965E94 | · | 08965F08 | 08965F44 | · | · | · |
| 67 | 51 | 089D9924 | 089DC2E8 | {11,1,2,48,4} | 08965C44 | 08965CFC | 08965D04 | · | 08965D78 | 08965DB4 | · | · | · |
| 68 | 52 | 089D9928 | 089DC388 | {10,1,2,39,4} | 08965F64 | 08966010 | 08966018 | · | 0896608C | 089660C8 | · | · | · |
| 69 | 53 | 089D992C | 089DC298 | {9,1,2,40,4} | 08965AB4 | 08965B6C | 08965B74 | · | 08965BE8 | 08965C24 | · | · | · |
| 70 | 54 | 089D9930 | 089DC478 | {9,1,2,76,4} | 08967388 | 08967430 | 08967438 | · | 089674AC | 089674E8 | · | · | 089674F0 |
| 78 | 55 | 089D9934 | 089DB9F0 | {5,2,0,13,20} | 08954DD0 | 08954F3C | 08954F44 | · | 08954FD0 | · | · | · | · |
| 79 | 56 | 089D9938 | 089DB8B0 | {0,1,0,0,16} | 08954240 | · | 08954280 | · | 08954288 | · | · | · | · |
| 80 | 57 | 089D993C | 089DC3D8 | {13,2,3,72,8} | 089660D0 | 0896638C | 08966584 | 089667E8 | 08966700 | 089667E0 | · | · | · |
| 81 | 79 | 089D9994 | 089DCE08 | {14,1,3,100,4} | 0897CD04 | 0897CDC4 | 0897CDF0 | 0897CE4C | 0897CE68 | 0897CEA0 | · | · | · |
| 82 | 80 | 089D9998 | 089DCE58 | {29,3,3,148,12} | 0897CEA8 | 0897D0B4 | 0897D1A0 | 0897D238 | 0897D254 | 0897D28C | · | · | · |
| 90 | 81 | 089D999C | 089DCFE8 | {15,1,2,61,4} | 0897EC44 | 0897EDD0 | 0897EDD8 | 0897EE38 | 0897EEC8 | 0897EF54 | · | · | · |
| 91 | 82 | 089D99A0 | 089DCEA8 | {10,2,2,28,8} | 0897D294 | 0897D44C | 0897D71C | 0897D454 | 0897D670 | 0897D714 | · | · | · |
| 92 | 83 | 089D99A4 | 089DCCC8 | {12,2,2,44,8} | 08979838 | 089799FC | 08979CFC | 08979A04 | 08979C50 | 08979CF4 | · | · | · |
| 93 | 84 | 089D99A8 | 089DCB88 | {11,2,2,76,8} | 08976758 | 08976910 | 08976BE0 | 08976918 | 08976B34 | 08976BD8 | · | · | · |
| 94 | 85 | 089D99AC | 089DCF48 | {11,2,2,32,8} | 0897DF60 | 0897E118 | 0897E3E8 | 0897E120 | 0897E33C | 0897E3E0 | · | · | · |
| 95 | 86 | 089D99B0 | 089DD038 | {11,2,2,40,8} | 0897EF5C | 0897F114 | 0897F3E4 | 0897F11C | 0897F338 | 0897F3DC | · | · | · |
| 96 | 87 | 089D99B4 | 089DCD18 | {12,2,2,44,8} | 0897A470 | 0897A628 | 0897A928 | 0897A630 | 0897A87C | 0897A920 | · | · | · |
| 97 | 88 | 089D99B8 | 089DCBD8 | {14,2,2,81,8} | 0897712C | 089772F0 | 089775C0 | 089772F8 | 08977514 | 089775B8 | · | · | · |
| 98 | 89 | 089D99BC | 089DCC28 | {15,2,2,53,8} | 08977CB4 | 08977E78 | 08978178 | 08977E80 | 089780CC | 08978170 | · | · | · |
| 99 | 90 | 089D99C0 | 089DCAE8 | {12,2,2,36,8} | 089753D4 | 0897558C | 0897585C | 08975594 | 089757B0 | 08975854 | · | · | · |
| 100 | 91 | 089D99C4 | 089DCEF8 | {15,1,2,69,4} | 0897DC04 | 0897DE30 | 0897DE38 | 0897DEB0 | 0897DF14 | 0897DF58 | · | · | · |
| 101 | 58 | 089D9940 | 089DBD10 | {20,1,2,85,4} | 08959E44 | 08959EB8 | 08959EDC | 08959EFC | 08959F18 | 08959F48 | · | · | · |
| 102 | 92 | 089D99C8 | 089DCA98 | {15,2,2,72,8} | 08974AC0 | 08974C80 | 08974DCC | 08974C88 | 08974D20 | 08974DC4 | · | · | · |
| 103 | 93 | 089D99CC | 089DCB38 | {14,2,2,112,8} | 08975DDC | 08975F9C | 089760E8 | 08975FA4 | 0897603C | 089760E0 | · | · | · |
| 104 | 94 | 089D99D0 | 089DCC78 | {15,2,2,77,8} | 08978A20 | 08978BF0 | 08979078 | 08978BF8 | 08978FCC | 08979070 | · | · | · |
| 105 | 59 | 089D9944 | 089DC248 | {25,4,3,86,32} | 08964A5C | 08964B18 | 08964B40 | 089656D8 | 08964CE4 | 08964CEC | · | · | · |
| 106 | 60 | 089D9948 | 089DC428 | {7,1,1,76,4} | 0896722C | 089672BC | 089672C4 | · | 08967338 | 08967358 | · | · | 08967360 |
| 107 | 61 | 089D994C | 089DBD60 | {10,1,2,28,4} | 08959F70 | 0895A068 | 0895A098 | 0895A16C | 0895A37C | 0895A434 | · | · | · |
| 108 | 62 | 089D9950 | 089DB9A0 | {6,3,1,25,21} | 089545B8 | 0895467C | 08954684 | · | 08954704 | · | · | · | · |
| 109 | 63 | 089D9954 | 089DBC20 | {2,1,0,8,4} | 0895956C | 089595FC | 08959620 | · | 08959640 | 08959660 | · | · | · |
| 110 | 64 | 089D9958 | 089DBC70 | {15,0,3,21,0} | 08959688 | 08959718 | 08959720 | 08959770 | 089597C0 | 08959814 | · | · | · |
| 111 | 65 | 089D995C | 089DBE50 | {8,0,4,18,0} | 0895A6C8 | 0895A758 | 0895A760 | 0895A7B0 | 0895A800 | 0895A854 | · | · | · |
| 112 | 95 | 089D99D4 | 089DCF98 | {29,1,2,99,4} | 0897E8F0 | 0897EB1C | 0897EB24 | 0897EB9C | 0897EC00 | 0897EC3C | · | · | · |
| 113 | 66 | 089D9960 | 089DBEA0 | {7,1,2,16,4} | 0895A85C | 0895A8FC | 0895A904 | 0895A978 | 0895A994 | 0895A9C4 | · | · | · |
| 114 | 67 | 089D9964 | 089DBEF0 | {6,1,0,48,4} | 0895A9CC | 0895AA5C | 0895AA80 | · | 0895AB18 | 0895AB40 | · | · | · |
| 119 | 68 | 089D9968 | 089DBCC0 | {13,1,2,58,4} | 0895981C | 08959A10 | 08959A48 | 08959AF4 | 08959A7C | 08959AB8 | · | · | · |
| 120 | 96 | 089D99D8 | 089DD120 | {23,1,2,114,4} | 089802C4 | 08980354 | 08980494 | 089805B4 | 08980648 | 08980830 | · | · | · |
| 121 | 69 | 089D996C | 089DBE00 | {30,1,2,204,4} | 0895A698 | 0895A6A0 | 0895A6A8 | 0895A6B0 | 0895A6B8 | 0895A6C0 | · | · | · |
| 132 | 70 | 089D9970 | 089DB400 | {2,0,0,2,0} | 089517CC | 089517D4 | 089517DC | · | · | · | · | · | · |
| 150 | 71 | 089D9974 | 089DB2C0 | {7,2,1,23,3} | 0894F91C | 0894F92C | 0894F938 | · | · | 0894F984 | · | · | · |
| 151 | 72 | 089D9978 | 089DB270 | {7,1,1,21,1} | 0894F378 | 0894F384 | 0894F390 | · | · | 0894F3DC | · | · | · |
| 161 | 73 | 089D997C | 089DB950 | {9,0,0,36,0} | 0895451C | 08954524 | · | · | · | · | · | · | · |

Channels mapped to type 0 (slot 089D9858, vtable 089DB1D0, every method a stub at 0894ED00..0894ED40, i.e. not implemented): 12, 28, 35, 42, 43, 44, 45, 47, 48, 49, 51, 52, 71, 72, 73, 74, 75, 76, 77, 83, 84, 85, 86, 87, 88, 89, 115, 116, 117, 118, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 152, 153, 154, 155, 156, 157, 158, 159, 160. Channel 0 has no slot.

## Per-channel notes (F's channels)

Notation: `P+n` = byte n of the channel's parameter block (init set and every key segment carry one), `S+n` = its
state block, `dep[i]` = the i-th linked track's state (see "Track links"), `t` = frames since the current key
segment began (f12 from FUN_0894BA60), `dt` = this tick's effect time step (instance+352, written by FUN_0894B48C).

### 61 + 62: flicker / pulse envelope on colour (type 48 / 49) — confirmed

Always paired (602 nodes each, every fighter). 61 has no links; 62 links `(colour 17 or 16, 61)`.
Lists: 0 (update), 2 (release), 3 (segment, no-op).

61 parameters (74 B): `P+0 vec4 A` (multiplier target A, 0..1 per RGBA), `P+16 vec4 jitterA`, `P+32 vec4 B`,
`P+48 vec4 jitterB`, `P+64 u16 holdA`, `P+66 u16 holdB`, `P+68 u16 ramp`, `P+70 u8 jitterA on`, `P+71 u8 jitterA
mode`, `P+72 u8 jitterB on`, `P+73 u8 jitterB mode`. State (56 B): `S+0 vec4 mult` (m0 sets 1,1,1,1, *not* A),
`S+16 vec4 rate`, `S+32 f32 timer`, `S+36/40/44/48 u32` thresholds `holdA, holdA+ramp, holdA+ramp+holdB,
holdA+2·ramp+holdB` (= period), `S+52 stage`.
Update (m2 0x08953A50): skipped while `ctx+0 & 1` or `instance+12 != 0` (pause); stage = 0 if timer < holdA,
1 if < holdA+ramp, 2 if < holdA+ramp+holdB, else 3. Entering stage 1 calls 0x089540D4 → FUN_08953CC0 with target B;
entering stage 3 calls 0x0895415C with target A: `rate = (clamp(target + jitter, 0, 1) − mult) / ramp`
(ramp 0 → no division). Jitter per component, uniform: mode byte 0 → ±range, 1 → ±range, 2 → ±range (negated;
same distribution); `on = 0` → no jitter. A skipped stage (0 ↔ 2 in one tick) applies one step and zeroes the rate.
Every tick `mult += rate`, then `timer += 0.5·dt`; when `timer ≥ period` it wraps to 0.
62 (m2 0x089538EC): `colour = clamp(colour × mult, 0, 255)` component-wise on dep[0] (the colour channel state, values
in the 128 = 1.0 scale), every frame after the colour channel has updated (node list order). 62's m0/m1 copy the colour
vec4 into its own state but nothing reads it back.
Typical data: A = (1,1,1,1), B = (1,1,1,0.5), holds 0, ramp 1 or 2 → alpha flickers 1 ↔ 0.5 every 1–2 frames.

```js
// per element, after colour (ch17) has been evaluated for this frame
function env61Init(s){ s.mult=[1,1,1,1]; s.rate=[0,0,0,0]; s.timer=0; s.stage=-1; }
function env61Tick(p, s, dt, rnd){
  const th=[p.holdA, p.holdA+p.ramp, p.holdA+p.ramp+p.holdB, p.holdA+2*p.ramp+p.holdB];
  const st = s.timer<th[0]?0 : s.timer<th[1]?1 : s.timer<th[2]?2 : 3;
  if (st!==s.stage){
    const toB = st===1 || (st===0 && s.stage===2), toA = st===3 || (st===2 && s.stage===0);
    if (toB||toA){ const T=toB?p.B:p.A, J=toB?p.jitB:p.jitA, on=toB?p.jitBOn:p.jitAOn;
      for(let i=0;i<4;i++){ const tgt=Math.min(1,Math.max(0,T[i]+(on?(rnd()*2-1)*J[i]:0)));
        s.rate[i]=(tgt-s.mult[i])/(p.ramp>0?p.ramp:1); }
      if (st===0||st===2){ for(let i=0;i<4;i++) s.mult[i]+=s.rate[i]; s.rate.fill(0); } }
    s.stage=st; }
  for(let i=0;i<4;i++) s.mult[i]+=s.rate[i];
  s.timer += 0.5*dt; if (s.timer>=th[3]) s.timer=0;
}
function apply62(colour, s){ for(let i=0;i<4;i++) colour[i]=Math.min(255,Math.max(0,colour[i]*s.mult[i])); }
```
Open: whether `dt` is 1 or 2 per displayed frame (the 0.5 factor suggests the envelope counts in 30 Hz frames).

### 107: screen colour flash (type 61) — confirmed

193 nodes, all fighters (hit effects: `*_HIT`, `*_FINISH`). Links `(colour 17, flags 14)`.
Parameters (28 B): `P+0 s32 x`, `P+4 s32 y`, `P+8 s32 w`, `P+12 s32 h` (pixels), `P+16 f32 v0`, `P+20 f32 speed`,
`P+24 u8 blend` (0/1/2 as in the summary), `P+25 u8 useRect`. State: a 12-byte object `{f32 value, f32 base,
ptr verts}` (allocated from the per-frame arena, FUN_08971134). `value = base + speed·t` is kept but never read
by the draw (it would have been a depth; vertices get z = 0).
Draw (m3 0x0895A16C): `sceGuDisable(DEPTH_TEST, TEXTURE_2D, SCISSOR, CULL)`; blend from `P+24`;
`sceGuColor(clamp(colour17 × instance colour, 0, 255))`; `sceGuDrawArray(GU_SPRITES, 0x800100 = through-mode
s16 xyz, 2 verts)`; corners `(x, y)–(min(x+w, 480), min(y+h, 272))`, or the whole 480×272 screen when `useRect = 0`.
Data: x = y = 0, w = 512, h = 448, useRect = 1, blend = 1 (additive) in 374 of 386 param blocks → a full-screen
additive flash whose strength is the colour channel's alpha (e.g. Warrior of Light F_HIT: rgb 128/124/113, alpha
4.6 → 30.5 → 0 over 16 frames).
Preview: draw after the scene, in screen space, no depth:
```js
function drawFlash(gl, p, rgba /*0..255 after ch17×instance*/){
  const a=Math.min(1,2*rgba[3]/255);
  setBlend(gl,p.blend);            // 0 alpha, 1 add, 2 reverse-subtract (see summary)
  const x0=p.useRect?p.x:0, y0=p.useRect?p.y:0, x1=p.useRect?Math.min(p.x+p.w,480):480, y1=p.useRect?Math.min(p.y+p.h,272):272;
  drawScreenRect(x0/480,y0/272,x1/480,y1/272,[rgba[0]/255,rgba[1]/255,rgba[2]/255,a]); // flat colour, no texture
}
```
(Untextured GE colour is used as c/255, so rgb 128 is half intensity: keep that, do not double it.)

### 109: scalar driver (type 63) — confirmed

258 nodes; no links; linked by 110 (254) and 111 (4). Parameters (8 B): `P+0 f32 value`, `P+4 f32 speed`.
State: pointer to a 16-byte object `{value, base, ?, acc(=0)}` (FUN_0895B77C alloc, FUN_0895B49C ctor,
FUN_0895B4E4 init). Update FUN_0895B70C: `value = base + speed·t + ½·acc·t²` (acc is always 0 from data);
key handlers FUN_0895B508 / FUN_0895B608 re-base for continuity. It drives the strength of 110/111.
Data: value 5 with speed 0, 0.1, 0.2 or 0.5 (most), 20/1, 3/0.

### 110: screen zoom blur (type 64) — confirmed for the geometry, strong for the buffer choice

254 nodes, every fighter's hit and finish effects. Links `(colour 17 or 15, flags 14, scalar 109)`.
Parameters (21 B): `P+0 s16 sx`, `P+2 s16 sy` (fixed screen centre, used when `P+12 ≠ 0`), `P+4..7` 0,
`P+8 u16`, `P+10 u16` (350/350, 200/150, 400/400; **not read** by any 110 method), `P+12 u8 screenFixed`,
`P+13..16 u8` (not read), `P+17 u8 blend`, `P+18 u8 feedback`, `P+19 u8 passes`, `P+20 u8 bilinear`.
Create (FUN_0895B244): allocates two 10-vertex strips (vertex = `f32 u, v, x, y, z`, vtype 0x183) covering the
whole screen in 4 columns (x step = screenW/4, z = 128). Update (FUN_0895B98C): centre `(cx, cy)` = the element's
world position through the view-projection matrix (FUN_08946AC4 + vtfm4 + perspective divide), or `(sx, sy)`
when `screenFixed`; clamped to the buffer. With `s = value(109)`, `S = buf+20` (screen width), `W = buf+36`,
`H = buf+40` (copy-buffer size), the sampled rectangle is
`L = 2·s·cx/S`, `R = W − 2·s·(W−1−cx)/S`, `T = 2·s·cy/S`, `B = H − 2·s·(H−1−cy)/S` (each clamped to the buffer),
written as normalised u/v into the strip (5 u steps from L to R). That is the screen magnified about (cx, cy)
by `1/(1 − 2s/S)` (s = 5 → 2 %). Draw (FUN_0895BDD4): depth/scissor/cull off, texture on, blend from `P+17`,
colour = clamp(colour × instance colour), `sceGuDrawArray(TRIANGLE_STRIP, 0x183, 10)` `passes` times, texture =
the screen-copy object at `ctx+172` (FUN_0897371C: `TexImage` from buffer `+8` when `feedback` = 1, i.e. the
copy is refreshed between passes, else from `+12`; filter from `P+20`). Typical data: blend 0, passes 1,
feedback 0, bilinear 1, follow world position. The visual is a radial "zoom" smear at the hit point whose
opacity follows the colour channel's alpha.
Preview (post-process pass, after the 3D scene and before UI):
```js
// sceneTex = the scene rendered so far; centre from the element's world position
function zoomBlurPass(gl, sceneTex, centrePx /*[cx,cy] in 480x272 space*/, s, rgba, p){
  const S=480, k=2*s/S;                       // magnification step per pass
  const uv0=[k*centrePx[0]/S, k*centrePx[1]/272], uv1=[1-k*(S-1-centrePx[0])/S, 1-k*(271-centrePx[1])/272];
  for(let i=0;i<p.passes;i++){
    setBlend(gl,p.blend);
    drawFullScreenQuad(sceneTex, uv0, uv1, [rgba[0]/255,rgba[1]/255,rgba[2]/255,Math.min(1,2*rgba[3]/255)]);
    if (p.feedback) copyFramebufferTo(sceneTex);   // each pass zooms the previous result
  }
}
```

### 111: screen rotation blur (type 65) — strong

4 nodes (Golbez, Exdeath, Tifa, Gabranth finishers). Links `(rotation 4, colour 17, flags 14, scalar 109)`.
Same machinery as 110 (shares FUN_0895B244, FUN_0895B774, the screen-copy object and the blend switch), but the
draw FUN_0895C37C builds a rotation (`vrot`) from the rotation channel / 109 value and rotates the full-screen
quad's texture coordinates about the centre before drawing. Parameters (18 B): `P+0 f32` (−1 or −0.8),
`P+4 f32` (128 or −0.1), `P+12 u8 blend`, `P+13 u8`, `P+14 u8 passes`, `P+15 u8` (enable/feedback).
Preview: same as 110 with the UVs rotated by the angle instead of scaled.

### 101: shockwave ring (type 58) — confirmed for the primitives, strong for the field meanings

69 nodes, 19 fighters (`EX_AURA_APP`, `*_BOM`, `SONIC`, shots). Links `(colour 17/15/16, flags 14)`.
Lists 0–3. Object 0x44 B (FUN_0895E058 alloc, FUN_0895C890 init). Parameters (85 B):
`P+0 vec2 A`, `P+12 f32 B`, `P+16 vec2 A.vel`, `P+28 f32 B.vel`, `P+32 vec2 A.acc`, `P+44 f32 B.acc` (A and B are
evaluated `v + vel·t + ½·acc·t²`, FUN_0895CB28/CC24, only while `P+76` (A) / `P+77` (B) are set);
`P+48 vec3` axis (used with `P+84` = billboard/orient mode by FUN_08947B88), `P+60 f32 alphaInner`,
`P+64 f32 alphaOuter` (×1/128 into the vertex alpha), `P+68 f32 ringScale` (0.5–1.0), `P+72 f32`,
`P+79 u8 blend`, `P+80 u8 liveScreen` (screen-copy buffer select), `P+83 u8 segments` (8/12/16).
Geometry (FUN_0895DA4C): unit circle of `segments` points (360°/N steps), four vertex arrays.
Draw (FUN_0895D0BC): binds the screen copy as texture, then FUN_0895DEC4 draws a **TRIANGLE_FAN** (vtype 0x183,
uv + xyz floats) whose UVs are the projected ring positions displaced (screen-space refraction), and FUN_0895E528
draws a **TRIANGLE_STRIP** annulus (vtype 0x19F: uv, colour 8888, xyz) with vertex colour
`colour × instance colour`, alpha `×alphaInner/128` on the inner edge and `×alphaOuter/128` on the outer edge.
Data: B = 0.95–1.5 with B.vel −0.19…+0.005 (shrinking or growing), alphaInner 128/64, alphaOuter 0 (fades out).
Guess on the split: A = ring radii (inner, outer), B = refraction strength (UV scale of the sampled screen).
Preview: a camera-facing ring mesh; render the inner disc with the scene texture at UVs
`centreUV + (vertexUV − centreUV)·B` (B < 1 magnifies, B > 1 shrinks), then the ring with a radial alpha gradient.

### 100: spark streaks (type 91) — strong

357 nodes, 30 fighters (most `*_HIT`, guards). Links `(colour 15 or 17, flags 14)`. A C++ particle system
(object 24 B, base ctor FUN_089842BC, class vtable 0x089DD230: update FUN_08984508, draw FUN_08984DEC).
Parameters (69 B), as copied by FUN_0898514C: `P+16 vec4 colourHead`, `P+32 f32 length?`, `P+36 f32 ±`,
`P+40 f32 speed`, `P+44 f32 ±speed`, `P+48 f32` (gravity/drag, used by FUN_08984B40), `P+52 f32 rate`
(sparks per frame, fractional carry), `P+56 f32 coneAngle` (degrees, ±half), `P+60 u16 maxSparks`,
`P+62 u16 life`, `P+64 u16 ±life`, `P+66 u8 pointsPerSpark`, `P+67 u8`, `P+68 u8 blend`.
Emission (FUN_08984574): per new spark, two random rotations (yaw/pitch within ±cone/2) of a base frame, speed
`speed ± rnd`, life `life ± rnd` frames, alpha 1.0; its `pointsPerSpark` points (32 B each: colour vec4, position)
get a colour gradient from colourHead (/128) along the strip. Draw: texture off, blend from `P+68`,
vertex colour `sat(colourChannel × instance colour × pointColour)` with alpha × spark alpha,
`sceGuDrawArrayN(GU_LINE_STRIP, 0x19C (colour 8888 + float xyz), points, sparks)`.
Preview: `THREE.LineSegments`/`Line` per spark with vertex colours, points trailing the head position.

### 102: line-spark burst (type 92) — strong

16 nodes (charge / aura effects). Links `(colour 15, flags 14)`. Records of 96 B per line.
Parameters (72 B): `P+0 vec4 colourA`, `P+16 vec4 colourB` (128 scale; head/tail colours), `P+32 f32`, `P+36 f32
len`, `P+40 f32 ±len`, `P+44 f32 speed`, `P+48 f32 ±speed`, `P+52 f32 rMin`, `P+56 f32 rMax`, `P+60 f32 rate`
(lines per frame), `P+64 u16 maxLines`, `P+66 u16 life`, `P+68 u16 ±life`, `P+70 u8`.
Emission (FUN_08974DCC): random direction (normalised random vector), start on a shell of radius in
[rMin, rMax], velocity/length along the direction; alpha falls linearly over the second half of the life.
Draw: `sceGuDrawArray(GU_LINES, 0x19C)` two coloured vertices per line. Preview: line segments radiating from (or,
with negative speed, converging to) the element origin.

### 103: line rays (type 93) — strong for the primitive, guess for the layout

13 nodes (`LIGHT_WAVE`, `EX_SKY`, `TOUKA`). Links `(colour 15, flags 14)`. Same family and draw path as 102
(`GU_LINES`, 0x19C, FUN_089C30F4), parameters 112 B (counts at `P+40 u16` 20/50, `P+42 u16` 15, colours as
vec4 near the end of the block). Preview as 102.

### 95: point particles (type 86) — strong

1 node (Warrior of Light `SHIELD_HIT`). Links `(colour 15, flags 14)`. Draw FUN_0897F11C:
`sceGuDrawArray(GU_POINTS, 0x19C, n)` — one-pixel points with vertex colour. Parameters (40 B):
`vec3 (1,1,1)`, `f32 0.02`, `f32 0.01`, …, `f32 10`, `u16 40`, `u16 40`, `u8 3`, … Preview: `THREE.Points`, size 1 px.

### 82: ribbon along a path (type 80) — strong

1 node (Zidane `EX_FINISH_KISEKI`). Links `(colour 17, flags 14, path 70)`. Draw FUN_0897D238 →
`sceGuDrawArray(TRIANGLE_STRIP, 0x119C / 0x119F)` (16-bit indices, colour 8888, float xyz, optional float uv).

### 66, 68, 70: trail path generators (types 50, 52, 54) — confirmed for 68, guess for 66/70

Linked by 80 (C's ribbon flipbook; 83 of 85 nodes) and 82; they link `(flags 14, 79)`. 68 (85 nodes, 21 fighters)
creates a 0x38-byte object (FUN_0896B650/FUN_0896AAB0/FUN_0896ABA4) holding a ring buffer of `P+32 s16 + 1`
positions; each update (FUN_0896B710) records the element's position (local or world, `P+37`) and resamples the
history into `P+34 u16` evenly spaced points (arc length) that the ribbon draws along. 66 (1 node) and 70
(3 nodes, 76-byte params with gravity-like floats −0.1, 0.5, 0.3) are other path builders (66: FUN_08969F74…,
70: FUN_0896BD58…, 70 also overrides m8). Preview: keep a per-element position history; hand it to the ribbon.

### 151: spawn child nodes (type 72) — confirmed

99 nodes, 20 fighters. Links one position track (1, 3 or 2). Parameters (21 B): `P+0 f32 count`, `P+4 f32
jitter`, `P+8 f32 scaleB`, `P+12 f32 scaleA`, `P+16 s16 node` (a node index of the same body; with bit 0x1000 an
index into the body's serial-list group, FUN_089469E0), `P+18 s16` (must be ≥ 0), rest 0. m1 and m5 set a
trigger byte, m2 (FUN_0894F3E8) then spawns `floor(count)` instances of that node (FUN_0894C244 / FUN_08946A5C)
at the parent's position + uniform ±jitter per axis, with the parent transform (FUN_089488A8).
So a child burst happens when the element starts and at every new key segment.
Preview: on those events instantiate the child node at the jittered position.

### Randomisers: 25, 31, 32, 33, 34, 39 — confirmed

All add uniform random offsets to another channel's state once (m1: only while element flag 2, "started", is
clear) — 39 instead re-rolls at every key (m1 without the check, plus m5). Mode byte per component:
0 → `±range`, 1 → `[0, range]`, 2 → `[−range, 0]`, 3 → 0 (25 treats 3+ like 0).
* 25 (73 nodes; links colour 15/16/17): `P+0 vec4 range0`, `P+16 vec4 range1`, `P+32 vec4 range2`,
  `P+48/50/52 u8 enable`, `P+49/51/53 u8 mode`; range0 goes to dep `S+16` and `S+0` (base and current value), range1 to dep `S+32`, range2 to dep `S+48` (S+32 = velocity, S+48 = acceleration, the same layout 29 and 19 read from channel 3: strong).
* 31 (position; links 3), 32 (rotation; links 4/5/6), 33 (scale; links 9): `P+0 vec3 range`, `P+12 u8 target`
  (0 → value: dep S+16 and S+0, 1 → dep S+32, 2 → dep S+48), `P+13..15 u8 mode x,y,z`.
* 34 (once) and 39 (every key) on colour 17: `P+0 vec4 range`, `P+16 u8 target`, `P+17..20 u8 mode`.
Tables: mode/enable byte offsets at 0x089DA0C0 (34/39), 0x089DA0C8 (25), 0x089DA0D8 (31–33).

### 19: gravity (type 19) — confirmed

2 nodes (Kuja). Links `(3)`. m1: `dep.S+52 (acceleration y) += P+0 s16 / 100 × 0.098` (P = 100 → +0.098/frame²).

### 29: face along velocity (type 28) — confirmed

11 nodes. Links `(position 3, rotation 4)`. Lists 0 and 6. m2 reads the velocity of the position channel
(type-dependent: zero for 1, rate for 2, vel + acc·t for 3), and adds `rot.x += deg(atan2(−vz, −vy))`,
`rot.z += deg(atan2(vx, √(vy²+vz²)))` (the pre-update values are kept in its 16-byte state); m7 (post-draw)
restores the rotation. So the element is drawn with its local axis turned toward its velocity without the
offset accumulating. Euler order: A's rotation channel.

### 58: one-off strip draw (type 47) — guess

1 node (Lightning `RAIKOU_FINISH`). Links `(16, 14)`. m3 FUN_0895A548 sets its own matrices (FUN_089A04F4),
draws through FUN_089425DC (`TRIANGLE_STRIP`, vtype 0x81, shared with FUN_08942FB8). Params include 400, 300, 1500,
300, 1.0 and u16 10, 10. Not needed for a preview of other characters.

## What a 3D preview should do with F's channels

* Order per element: evaluate A's transform and colour channels, then 25/31–34/39 (once or per key), 19, 61→62,
  29 (temporary), then draw. Post-process passes (110, 111, 101's refraction, 107) run after the scene in the
  order the effect lists them; each needs a copy of the scene rendered so far (WebGL: render to a texture, or
  `copyTexSubImage2D` before the pass).
* 107 is a screen-space rectangle; 110/111 are full-screen quads over the scene copy; 101 is a world-space ring
  that samples the scene copy; 100/102/103/95 are line/point primitives with vertex colours (WebGL line width is
  1 px, which matches the PSP).

## Open points

* Whether `dt` (instance+352) is 1 or 2 per displayed frame; 61's envelope advances by `0.5·dt`, the emitters by `dt`.
* 101: which of A (vec2) / B is the radius and which the refraction scale (the primitives and alpha fields are
  confirmed, this split is a guess); `P+72`.
* 103, 95, 82, 66, 70, 58, 111 field layouts are only partly read (low use: 1–15 nodes each, except 103 with 13).
* m8 (vt+72) has no caller found; types 54 (ch 70) and 60 (ch 106) override it.
* 110 `P+8/P+10` (350/350 etc.) are never read by type 64's code; they may be authoring-tool leftovers.
