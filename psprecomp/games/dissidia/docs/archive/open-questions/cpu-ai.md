# CPU AI: from "CPU tick" to "CPU starts an action" (1 Nov 2026)

Static study (Capstone disassembly of EBOOT + battle overlay, no runtime). Goal: every gate a character's data must
pass for the CPU to act, because Noctis (charId 34 in objentry/chrp, the roster's slot 35, built on Bartz fiv100)
does nothing as CPU. Confidence: **confirmed** = read in the code path; **strong** = code read, meaning inferred;
**guess** = not traced. Helper scripts: `~/re/cpu/fn.py` (print a function), `who.py` (callers), `brat2.py` (dump brat).

## 1. Who gets a brain, and in which mode (the first gate)

* Every fighter owns a brain at `actor+0x5C4` (0x1430 bytes, ctor battle 0x08A637B8; VCKing101 battle-moveset.md §7).
  The fighter update calls the tick **FUN_08A64400** at 0x08B3773C whenever `actor+0x5C4 != 0` (confirmed).
* The brain's control mode is `brain+0x24 +0x30` (set by FUN_08A976E4 through FUN_08A64078 / FUN_08B2F2E4).
  The ctor FUN_08A971A8 sets it to **-1**. Tick dispatch (confirmed): mode 0 / 2 = main path; 1 = module +0xC10
  only; 3 = module +0x138C; 4 / 5 = module +0x13A0; **anything else (-1) = return, nothing at all**. Mode 2 skips the
  decision part (0x08A64984) and only maps a few states to commands: it is the human side's helper mode.
* Battle start, EBOOT 0x0888BE30..0x0888BFA8 (confirmed): per side, setup block `0x09AFB8A0 + side*0x1060`:
  `+0xF34` controller type: **0 = human** (brain left at -1, or mode 2 when setup flag 0x40 is clear),
  **1 = CPU** (FUN_08B2F2E4(actor,0) = mode 0, level `+0xF36`, strength `+0xF38`), **2 = CPU with a recorded
  play style** (as 1, plus FUN_08B2F274 loads the 0x118 play-style block at `+0xF40`). Battle mode 6 makes both CPU.
* Who writes `+0xF34` (battle request, `*(0x089D8508)`, consumer EBOOT 0x0891CFC4; types from VCKing roster-and-save.md):
  `FUN_088F8D34` clears it to 0 for every source. Types enmp (0x088FA058), built CPU (FUN_088FA2AC -> 0x088FA3B4),
  compact record (0x088FA528/588), stored record (0x088FA810) set 1/2 directly.
  **Type 0 "own save record" FUN_088F9BB0 with the CPU flag** sets `+0xF34 = 2` only if
  `FUN_088C04B4(save+0x124A4, charId)` finds the character's play-style record: FUN_088C04F8 searches the 31-byte
  list at **EBOOT 0x089CBA68 = {1..22, 24..32}** and indexes 0x57C-byte records. **charId 34 is not in it, so
  `+0xF34` stays 0 and the fighter is human-controlled with no pad: it stands completely still** (confirmed code;
  whether the mode tested uses request type 0 is not traced). roster.cpp hooks none of this.
  Check at runtime: halfword `0x09AFB8A0 + side*0x1060 + 0xF34`, and word `*(actor+0x5C4) + 0x54` (mode).

## 2. The per-tick chain (FUN_08A64400, mode 0)

1. `brain+0` enabled, `brain+0xC != 1`; target choice FUN_08A63C8C (opponent vs its assist), FUN_08A63F74
   (Feral Chaos 0x20 special).
2. Builds the shared command context: FUN_08848078(plya = objdata+0x88), **FUN_08AB7F40(ctx, comb = objdata+0x8C,
   setup record = actor+0x30C, form = FUN_08ACF234(actor+0xD5C), -1)**: for every equipped ability id
   (FUN_088CBDFC over the set at record+0x10+0xC6*`+0x773`) -> ablt record (FUN_088B1D9C, no null check) ->
   ablt+0x0C command; every comb entry whose +0 or +4 equals it gets its bit in the 256-bit mask ctx+0x24 (confirmed).
3. Exit if no target (FUN_08AB32B8 = vtable+0x130 then alive check) or `actor+0x380 & 2` (pre-fight flag; cleared
   by the battle director, EBOOT 0x0891420C etc.) (confirmed).
4. Sub-module updates (situation analyser +0x20 -> FUN_08A82558/FUN_08A83590 fills 0x22 situation flags of 12 bytes
   at +0x1B8+0x10: e.g. flag 0xE = own state `*(actor+0x3A4) == 0x18` (attack) and not +0x193&1; flag 2 = flag 0 and
   not +0x193&1; flag 0xD = ability 0x114 equipped and FUN_088380F8).
5. Global pause `*(*(0x08A405E0)+0x278)` or `actor+0x5B5 & 1` -> no new decision.
6. **Busy gate** (0x08A64610..0x08A646E0, confirmed): while flag 2, 0xE or 0xD (or FUN_08A84174: state 0x384/0x385/
   0x38E/0x38F) is up, the brain waits in state 1 **until `+0x193 & 1` (event 0x041E) or `+0x194 & 4` (event 0x0028,
   dodge_cancel)** is set in the running motion. Without either the CPU decides again only when the action ends.
7. Otherwise FUN_08A9FF80 / FUN_08A9B3D4 (module +0x11C0; Feral Chaos 32 check) -> state 3, else state 2 (target
   is the opponent) or 1. The state machine at +0x530 (module 0x08A96398, states 1..0x1E, FUN_08A965E0) runs the
   behaviours; FUN_08A64244(brain, cmd) is how the fighter control asks it for a command (dash = 0x15 in
   FUN_08B30D68, directions 0x17.. ; brain commands are OR'd with the pad at actor+0x5A0) (strong).

## 3. Attack choice: FUN_08A7C1DC(brain, request)

Called from ~40 sites (0x08A66724, 0x08A67764, 0x08A69718, ...) with a request: +0 situation (<6), +4 (<4),
+8 (<3), +0xC (<3), +0x10 window mode (1..6), +0x14, +0x18 attack class (<5), +0x1C, +0x20, +0x24, +0x28, +0x29,
+0x2A, +0x2B. Returns -1 on any missing input. Steps (confirmed unless marked):

1. `objdata = actor+0x390`; **brat = objdata+0xA0**; FUN_08810678 returns it only if loaded and
   **brat id (header u32 after the 1 at +8) == `*(s16*)(*(actor+0x2F8)+2)`** = objentry record +2. Bartz: 4/4;
   Noctis: brat id 4, P_NOC100_* objentry +2 = 4: passes. brat N must be < 256.
2. Needs chrp (`actor+0x5A4`), objdata, setup record (`actor+0x30C`). seco owner row (FUN_08AB2340) is optional.
3. For each form (FUN_08AB3610 count; ctx rebuilt per form) and each comb entry i (FUN_08AB8100):
   * entry command >= 0; stage-0 plya exists (FUN_08A7BEC0 -> FUN_08AB332C);
   * **mask bit i set (equipped) or stage-0 plya +0x0F & 4**;
   * first stage without plya flag 0x1000 exists (FUN_08A7BF1C); air/ground (any stage with plya flag 0x10,
     FUN_08A7C004) matches the request mode;
   * plya +0x18 &4, stage-0 +0x18 &0x40, attack class: request+0x18 1 -> plya+0x0E in {0,0xB}, 2 -> 1, 3 -> {3,4,5};
   * **FUN_08A652F4: the command must sit in one of the 12 set slots** (slot -> FUN_08B33B6C/BD4 ability id ->
     ablt +0x0C) or stage-0 +0x0F & 4; slot kind FUN_08A64FB0;
   * **plya window**: distance +0x24/+0x28, height +0x2C/+0x30 (FUN_08AB33DC..3430, widened by an offset)
     checked per request+0x10 (1 both, 2 height, 3 min dist, 4 max dist+height, 5/6 variants); distance excess becomes
     a penalty;
   * flags: 0x04000000 vs request+0x20; plya+0x0E 6..10 (assist kinds) vs request+0x24/+0x2C; +0x29 needs +0x18&0x20;
     stage-0 +0x0F&4 must equal request+0x2A; FUN_08A7C0F4; +0x2B rejects plya+0x15&2; 0x40000000 = HP-percent/
     ability 0x53 gate; +0x0F&0x10 needs actor+0x1274 > 0; plya+0x14 vs form `actor+0x106A` with flag 0x2000;
   * **brat lookup by comb command** (linear, stride 0x1C); absent -> skipped;
   * weight: FUN_08A7BDF4: pair (off,n) of `situation` (request+0), index `off + min(level, n-1)` (level =
     FUN_08AB1248(brain+8)), code = weights[index] -> FUN_08AACA88(seco, code) (table seco+0x15C/+0x160 {code, f32});
     **weight 0 -> skipped**. In play-style mode (FUN_08A9771C) FUN_08A977D4 gives the weight instead.
4. Pick: weighted random over survivors (FUN_08AA578C = rand * 2^-16 times the weight sum, 0x08A7E0FC) (strong).

## 4. Per-character inputs the AI reads

brat (objdata+0xA0, id must equal objentry+2), comb (objdata+0x8C), plya (objdata+0x88; +0x0E class, +0x0F flags,
+0x14 form, +0x15, +0x18 flags, +0x24..+0x30 window), the setup record actor+0x30C (equipped set, set index +0x773,
learned list), ablt (+0x0C command), chrp actor+0x5A4 (byte 0 charId for seco rows; chrp +6 read from the target,
FUN_088B3958 at 0x08A779D4), objentry actor+0x2F8 (+2 brat key), level/strength (+0xF36/+0xF38), play-style block.
seco owner table (seco+0x1CC count, +0x1D0 rows of 0x1C, FUN_08AACD30, keyed by chrp charId): **no row -> NULL ->
callers return 0 (that feature off)** (confirmed for FUN_08AB2108/22B4/2340/7EA90). Hard-coded ids in the AI:
0x20 Feral Chaos (0x08A63F98, 0x08A9DF74, 0x08A9FF80), 23 Chaos (0x08A78DBC); the only "<= 32" list is the play-style
list in §1.

## 5. Timeline events that gate the CPU

0x041E (+0x193&1) and 0x0028 (+0x194&4): own decision point inside an action (§2.6). 0x0022 (+0x193&4,
FUN_08837A24) and 0x0029: read only on the opponent (threat / punish hints). R 0x0278 arg0 -> +0x190&8, read at
0x08A77838 (CPU keeps pressing). Chain window cpu flag: snapshot 0x08A83828..3C. None of them is needed for the idle
motion: no gate requires a flag in the stand motion (strong; idle = not in state 0x18).

## 6. "Stands still" causes, ranked for Noctis (charId 34, Bartz clone)

1. **Controller type 0** (§1): request type 0 + CPU flag + charId not in 0x089CBA68 -> brain mode -1/2, no
   movement at all. Fix: runtime hook FUN_088C04F8 (map extra -> like charId, Bartz 9) or force `+0xF34 = 1`
   for extras when the CPU flag is set. Strong mechanism; trigger path to confirm with the two reads above.
2. **Empty candidate set**: the CPU's set holds Noctis ability ids whose ablt +0x0C has no comb entry (+0/+4),
   disabled ids (command -2), or commands missing from brat / weight code 3; or no attack whose plya window covers
   the distance. The AI then never attacks (whether it still walks is not traced: guess it approaches/guards).
   Fix: data (set, ablt, comb, brat rows, plya +0x24..+0x30).
3. **Busy forever**: a motion that keeps state 0x18 without 0x041E/0x0028 (looping action) -> waits in state 1. Guess.
4. Engine-wide gates (no target, +0x380&2): would hit every fighter. Low.
5. Missing seco row: graceful. Low.
