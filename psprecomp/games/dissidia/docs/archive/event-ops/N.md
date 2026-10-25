# Agent N (round 2): open questions from round 1

**Summary**
- **0x041E args are used in retail after all (22 of ~81 uses):** `{1,0,0,0}` ×11, `{1,0,1,0}` ×7, `{0,0,1,0}` ×4, all at the end of character attack moves (10000+) plus Cloud's dodge/block. arg0/arg2/arg3 make the in-action override give up jump, dash, R+△ and stick steering; the fighter may still attack, block or dodge. arg2 = the new move must be a *different* action. arg3 = it must be the *same* action. arg1 turns off the hit-confirm cancel. Confidence: strong.
- **Camera presets:** the presets come from a runtime-loaded `camr` section (FUN_088135D4 → FUN_08811598). The table is at `*(0x089D479C)` and the count is `*(*(0x089D4790)+8)`, so it is not in the EBOOT image. Each record is 0x38 bytes: two spring pairs, 8 blend rates, a look distance and flags (1 smooth, 2/4 look-at, 8 bone up, 0x10 extra post step, 0x20/0x40 time-stop handling, 0x100 occlusion fade). Retail uses presets 1, 2, 4 and 5.
- **Push-apart masks:** round 1 mis-computed the addresses. They are constants at **0x089C8130/38/40**, not 0x089D8130 (lui 0x89D − 0x7EC0 = 0x089C8140): lo bit 0x8000 = anchored (battle intro), hi bit 0x800 = "I take the whole push", hi bit 0x1000 = "nobody moves" (both are set only on the world map). **Null timelines:** these are timelines with **zero ranged events**. The game's header is 8 bytes, and the decoder's "header slot" is really just the first ranged event.

Conventions: actor offsets. The flag block is actor+0x190 (= owner+0x60). "bit 0xNN" is a byte mask. Addresses ≥ 0x089DD4C0 are in the battle overlay (all.s covers both).

---

### Q1. `can_act_now` 0x041E: what the 4 args do

**Handler (0x0882AF34):**
- Always: +0x193 |= 0x01 (the in-action override) and +0x190 |= 0x01 (can act).
- arg0: +0x193 |= 0x02, +0x199 |= 0x04.
- arg1: +0x198 |= 0x04.
- arg2: +0x193 |= 0x02, +0x199 |= 0x04, +0x198 |= 0x40.
- arg3: +0x193 |= 0x02, +0x199 |= 0x04, +0x198 |= 0x80.

The arg values are tested only for non-zero.

**What each bit gates**

| Bit | Reader | Effect |
|---|---|---|
| +0x193 & 0x01 (always) | FUN_08B2D664 (start command), 0x08B2D6E0 | If starting the picked action fails while in an action, the combo context is cleared (FUN_08B308B8 zeroes +0x101C/+0x1020/+0x1024) and FUN_08B382B8 is retried with a2 = the current action. **This is what lets a fresh, neutral command start out of the recovery.** On success it sets +0x5B5 |= 2. |
| +0x193 & 0x01 without +0x199 & 0x04 | **FUN_08B2F5E0 = can jump** (0x08B2F658; caller 0x08B35E44 plays script 100 and then 101/102). **FUN_08B2FBCC = can dash** (0x08B2FC58; callers 0x08B367DC / 0x08B3684C → FUN_08A53290 plays script 240/242/244; ability ids 0x38–0x3A, count at +0x1075). **FUN_08B305E8 = R+△ dash and others** (0x08B30668). Block path 0x08B368FC: sets +0x5B9 bit 8, which FUN_08B2FBCC reads at 0x08B2FD50. | Returns "yes" at once, even in an action. |
| +0x199 & 0x04 (arg0/2/3) | same places | Cancels that early "yes", so the normal path runs. The normal path needs **not in an action** (FUN_088377C0) plus +0x190 bit 0. So during the move's own recovery **jump, dash and R+△ are refused** until the action ends. Attack (vtable +0x150 reads +0x190 bit 0), block (+0x158) and dodge (FUN_08B3A5BC, 0x08B3A694 checks only +0x193 & 1) are not affected. |
| +0x193 & 0x02 (arg0/2/3) | **FUN_08B30544** (0x08B305C4). Caller FUN_08B36D2C 0x08B37280 → FUN_08B2D2CC, which sets the move speed +0xBC from the analog stick (+0xB0, ×+0x3D4). | Blocks **stick steering/drift** in the current state. *This is the reader round 1 could not find.* |
| +0x198 & 0x04 (arg1) | FUN_08B2FD90 a1 = 0 path (0x08B2FE6C) | Turns off the "hit-confirm" in-action attack cancel (+0x380 & 0x1000 or +0x384 & 0x08000000). No other reader. Unused in retail. |
| +0x198 & 0x40 (arg2) | **FUN_08B382B8 start action**, 0x08B3837C–0x08B383A4 | The newly picked action record s3 must **differ** from a2 (the current action, +0x101C). Otherwise the start is refused, so you cannot repeat the same move out of this recovery. |
| +0x198 & 0x80 (arg3) | same, 0x08B383E0–0x08B383FC | The new action must be the **same** action as the current one, so only a repeat is allowed. Unused in retail. |

The CPU AI mirrors these bits: FUN_08A7C1DC reads +0x199 & 4 (0x08A7C550), +0x198 & 0x40 (0x08A7D1CC) and +0x198 & 0x80 (0x08A7D298). FUN_08A7B33C reads +0x199 & 2 (0x08A7B388), a bit 0x041E does not set.

**Modder summary:**
- `{0,0,0,0}`: from here, act freely (attack, block, dodge, jump, dash, steer).
- `{1,0,0,0}`: attack, block or dodge only. No jump, dash, R+△ or stick steering until the motion ends.
- `{1,0,1,0}` / `{0,0,1,0}`: as `{1,0,0,0}`, and the follow-up may not be the same move again.

**Data check (`retail_timelines.json`):** 59× `{0,0,0,0}` (the recovery motions listed in out1/D.md), plus:
- `{1,0,0,0}` ×11, always at the end of attack moves, e.g.:
  - one100 46 (10003/10005) from 40
  - fiv200 56–62 (10020–10044) from 20–60
  - eht110 52 (10020) from 52
  - ten200 44/45/51 from 76/100
  - also sev100 18 (440 dodge) / 20, 21 (470/471 block) from 56/50
- `{1,0,1,0}` ×7:
  - one200 55 (10030) from 80
  - for100 49 (10011) from 122
  - sev200 58 / eht100 58 / eht200 54 / gst130 48 (10021–10035)
- `{0,0,1,0}` ×4:
  - thr100 47/48 (10060–10063) from 20
  - for200 42 (10000–10014) from 100
  - gst120 85/86 (10109/10110, which contain only 0x041E plus 0x0015/0x000A) from 20

That fits "move ends: you may attack again, but not jump or dash out of it, and not repeat this move" for those characters.

**Confidence:** strong (each bit is traced to its reader and the gated action). "Steering" for FUN_08B2D2CC is medium. **Tests:**
1. On Noctis, change a combo-end 0x041E to `{1,0,0,0}`. Jump and dash during the tail should then be refused, while attack and block still work.
2. With `{0,0,1,0}`, re-pressing the same move during the tail should do nothing, while a different move starts.

---

### Q2. `motion_camera` 0x030C presets

**Where the table lives:**
- FUN_088135D4 loads a battle parameter pack by section tag (`atkp, spec, nekp, scrb, camr, chrg, scra, wpnp` at 0x089C6ED4..). The **`camr`** section is passed to **FUN_08811598**, which copies it into a buffer:
  - `*(0x089D4790)` = header (0x10 bytes); **+8 = preset count** (the clamp in FUN_08B3E840: arg0 ≥ count → 0; a negative value is not clamped).
  - `*(0x089D4794)` = header+0x10: 8 bytes, with a float at +4 copied to cam+0xD4 on enter.
  - `*(0x089D4798)` = header+0x18: 0x378 bytes (unknown).
  - `*(0x089D479C)` = header+0x390: **the preset records, 0x38 bytes each** (FUN_08B3E8B0: base + idx·0x38).
- The data is not in the EBOOT image or the mod folders, so the **number of presets and their values are unknown**: we need the battle pack that holds `camr`. Retail uses 1, 2, 4 and 5, so there are at least 6.

**Record layout (0x38 bytes), mode-2 object vtable 0x08B3F118:** enter = FUN_08B3E404, exit = FUN_08B3E580, update = FUN_08B3E6D0, smoothing = FUN_08B3E954 (slot +0x78).

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| +0x00, +0x04 | f32 ×2 | Spring constants for the **eye position** (used only with flag 1) | FUN_08B3E954 → FUN_08B3B950 (VFPU spring-damper, ×dt from FUN_08B3B4FC) |
| +0x08, +0x0C | f32 ×2 | Spring constants for the **look-at target and up vector** (flag 1) | same, for +0x80/+0xA0 and +0x40/+0x60 |
| +0x10 / +0x14 | f32 | Blend-**in** / blend-**out** rate, vector 0 (eye) | FUN_08B3E404 / E580 → FUN_08B3BECC → FUN_08B3BD28. Weight starts at the rate and grows by rate·dt each frame up to 1 (FUN_08B3CB90..). **0 means a hard cut (1.0).** |
| +0x18 / +0x1C | f32 | Blend in / out, vector 1 (look-at) | FUN_08B3BF00 → BD9C |
| +0x20 / +0x24 | f32 | Blend in / out, vector 2 (up) | FUN_08B3BF34 → BE10 |
| +0x28 / +0x2C | f32 | Blend in / out, scalar (FOV) | FUN_08B3BF68 → BE84 |
| +0x30 | f32 | Look-at distance along the bone direction (used when neither flag 2 nor 4 is set) | 0x08B3E798 |
| +0x34 | u32 | flags | below |

Flags at +0x34:
- **0x01** spring-smoothed follow. Off = eye, target and up snap to the bone each frame (0x08B3E9C4).
- **0x02** look at the opponent (vtable +0x130 → +0x300). **0x04** look at the actor itself (+0x100) and snap the target. Neither = look along the bone, +0x30 ahead.
- **0x08** keep the bone's up vector. Otherwise up = (0,1,0).
- **0x10** call vtable +0x80 (FUN_08B3E8D0 → FUN_08B3D8C0) after smoothing; probably a stage/collision clamp (weak).
- **0x20 clear** → on enter, actor+0x388 lo |= 0x40000000 (mask 0x08B3F240); cleared on exit (0x08B3F248). FUN_08837F6C (update-list filter, from FUN_088412E4) uses that bit to **keep the actor updating while +0x380 bit 0x04000000 (pause/time-stop) is set**. So by default the camera owner keeps animating during a freeze; flag 0x20 opts out.
- **0x40** → on enter the **opponent's** +0x380 |= 0x00800000, cleared on exit (0x08B3E69C, mask 0xFF7FFFFF). FUN_08837F6C drops actors with that bit from the update list, so **the opponent is frozen** for the camera's duration.
- **0x100** → the camera manager FUN_08B3CA24 (0x08B3CE14, slot +0x68 = FUN_08B3E888) runs FUN_08B3C1E0 (a per-fighter alpha table at mgr+0x6F0) instead of resetting it to 1.0. Probably **fades fighters that block the view** (weak).
- FOV: taken from the camera bone (FUN_08836A30 output → cam+0xD0), not from the record.

**Retail usage:**

| Args | Uses | Where |
|---|---|---|
| `{1,0,0}` | 58 | scripts 200/203 (motion 8, every character) |
| `{5,0,0}` | 177 | EX Bursts 8000–8112, 8500–8503, 8700; two200 10080 |
| `{5,1,0}` | 15 | `camera2_include`: two200 8104/5, thr200/for100/six100/nin100/ten100/ten200/eht200 8101–8503 |
| `{2,0,0}` | 1 | eht200 64 (10071) |
| `{4,0,0}` | 1 | gst100 64 (Shantotto 8500) |

Preset 5 is the standard EX-Burst camera; 1 is used for the 200/203 pose.

**Confidence:** layout and flags are confirmed from the code (except the 0x10/0x100 names, which are weak). Preset contents are unknown until the `camr` data is extracted. **Test:** dump `camr` from the battle pack and print the records; in game, compare `{1,0,0}` vs `{5,0,0}` on one EX Burst to see whether the opponent freezes (flag 0x40).

---

### Q3. `face_texture` 0x0214 groups

**Code:**
- FUN_0886D8D4 parses each GMO TexAnm block (type 0x0F/0xF1, strings "target:/source:/default:/TexAnm" at 0x089C959C). The group id stored at entry +0x20 is just the number in the block's name. **The name is not kept and no code maps group ids to body parts**, so which part a group drives is decided by each model's TexAnm "target:" texture.
- One more system writes these groups: **EX Mode**. FUN_08ACAC60 (0x08ACAEC8–0x08ACAF78) applies up to 4 `{group, pattern}` pairs from the per-character record `*(actor+0x5A4)` (bytes +0x10..+0x13 = groups, 0xFF = none; +0x14..+0x17 = patterns; record from FUN_088B70C8 → runtime table `*(0x089D7368)`). It resets those groups to default on EX end (FUN_0886AA98). The values are runtime data and were not available here.
- The EX-Burst camera GMOs in the mod folders (`*_d_gs_2.bin`, `*_d_te_2.bin`, `*_d_fi_2.bin`) have no TexAnm blocks. The character body GMOs are not in the workspace.

**Usage per character** (group: pattern×count; `_a` merged):

| char | g0 | g1 | g2 | g3 |
|---|---|---|---|---|
| one100 | 1×15 3×17 4×1 | same | 4×6 5×8 6×1 | – |
| two100 | 1 2×32 3×20 4×4 6 | same | 1×4 2×29 3 4×30 | – |
| two200 | 2×4 3×17 4×2 | same | 1 2 3×20 4×17 5×10 | – |
| thr100 | 2×36 3×19 4×4 6×7 | ≈same | 2×34 3 4×43 5×7 6×4 | – |
| thr200 | 2×15 3×15 4×19 | same | 2×18 3×15 4×24 | – |
| for100 | 2×4 3×33 4×20 | same | 5×30 6×19 | – |
| for110 | 2×14 3×17 4×2 6 | same | 5×11 6×18 | – |
| fiv100 | 2×30 3×17 4×3 | same | 5×8 6×24 7×9 | – |
| six100 | 2×7 3×32 4×17 5 | same | 2×9 3×17 4 | – |
| six200 | 0 2×2 3×5 4×23 6×7 7×4 8×2 9×4 | ≈same | 2×12 3×26 4×3 | – |
| sev100 | 1×6 2×48 3×17 5 | 2×24 3×35 | – | – |
| sev110 | 0×2 4×38 5×18 6×2 | ≈same | 1 2×26 3×30 4 | – |
| sev200 | 1×5 2×17 3×2 | same | 1×3 3×17 5 | – |
| eht100 | 1×2 2×55 3×20 | ≈same | 1×3 2×42 3×25 | – |
| eht110 | 0×8 1 2×49 3×20 4 5 | ≈same | 2×10 3×12 4×3 5×37 | – |
| eht200 | 1 2 3×17 | same | 2×17 | – |
| nin100 | 2×43 3×20 4×3 5 | ≈same | 2×46 3×14 4×3 5 | – |
| nin200 | 1 2×2 3×16 4×12 | same | 1 2×24 3×11 4×3 | – |
| ten100 | 2×14 3×17 5×2 | same | 2×13 4×18 5×9 | – |
| ten110 | 2×4 3×10 5×17 | same | 1 2×35 3×16 4 | – |
| ten200 | 2×35 3×17 4×2 | 2×34 3×26 4×6 | – | – |
| gst100 (Shantotto) | 1×4 2×8 4×16 | same | 2×16 3×12 4×40 | **1×7 2×10 3×7 4×12 5×11 6×10 7×5** |
| gst110 | 1×10 4×19 | same | 1×15 2×62 3×22 4×15 | – |
| gst120 | 2×30 3×19 | ≈same | 5×16 6×42 | – |
| gst130 | 2 4×17 6×29 | same | 2×4 3×7 | – |

No 0x0214 at all: one200, for200, fiv200/210, sev120 (the helmeted or masked ones).

- g0 and g1 match in about 90% of motions. Where they differ (sev100 ×31, ten200 ×19, eht110 ×15) one side has a different pattern or range, e.g. eht110 37: g0 = 0, g1 = 2. That fits **left/right eye** (winks, a squint on one side).
- g2 changes inside attacks, around the hits. That fits the **mouth**.
- **g3 (Shantotto only, 62 uses):** always the whole motion (0–65535), and one value per move:
  - dodges 400–460 → 5; blocks 470/471 → 6
  - ground spells 10000–10002 → 4
  - 10080–10098 → 6, 1, 4, 5, 3, 2 in turn
  - 10060–10078: phases 1–3 vs 4–6 (pad) get different values, e.g. 10060–62 → 3 and 10100–02 → 6
  - EX 8000 → 4; 8101/8500/8600 → 7

  So g3 is a **per-move held texture that is not driven by blinking or the mouth**, most likely a brow/expression overlay or a hand/rod sign texture. It cannot be settled without her GMO's TexAnm target names.

**Confidence:** the mechanism is confirmed; the group → body-part mapping is strong for g0/g1 = eyes and g2 = mouth, and weak for g3. **Test:** export Shantotto's GMO (gst100), list the TexAnm blocks and their `target:` texture names; or in game set `{3,1}` vs `{3,7}` over a long idle and see what changes.

---

### Q4. Push-apart state masks (out1/B.md "0x089D8130–48")

**Correction:** the resolver FUN_0883E3BC loads them with `lui 0x89D` plus offsets −0x7EC0 / −0x7EC8 / −0x7ED0, so the addresses are **0x089C8140 / 0x089C8138 / 0x089C8130**. These are **constants in the EBOOT image**, not runtime words. They are tested against the 64-bit actor word **+0x388 (lo) / +0x38C (hi)**.

| Constant | Value (lo, hi) | Test site | Meaning |
|---|---|---|---|
| 0x089C8140 | (0x8000, 0) | 0x0883E42C/E484/E4E8 | "anchored" (same role as op 0x0049's +0x195 & 0x80) |
| 0x089C8138 | (0, 0x800) | 0x0883E554 | if self has it → mode 3: **self takes the whole push**, the other side does not move |
| 0x089C8130 | (0, 0x1000) | 0x0883E588 | else, if self has it → mode 0: **nobody moves** in self's pass |

The runtime bits in +0x388/+0x38C are written by the following code (a scan of every `sw` to +0x388/+0x38C with its rodata mask; only the push-apart resolver reads these 3 bits):
- **lo 0x8000 (anchored):**
  - **FUN_08B0DB90** (battle-scene vtable 0x089D6A54 slot, called from FUN_08888868): at battle start it places each fighter (`*(0x089D6A10)+…+0x38`) on its start point and sets 0x8000 (mask 0x08B11DF0).
  - **FUN_08B0E04C** (from FUN_08B0DDC8 / FUN_08B0DEA0, end of intro) clears it again (0x08B11DF8 = ~0x8000) and also clears lo 0x20000000.
  - So during the **entry/intro poses the fighters cannot be shoved**.
  - Also set by FUN_08814FA0 (a generic object being disabled; also lo 0x4000/0x40000) and FUN_08AB68C4 (overlay object init; mask 0x08AB73D8).
- **hi 0x800 / hi 0x1000:** only in the **world map** (strings "WORLDMAP_CHAIN::MainTask", "worldmap.bin" next to the masks at 0x089CE390–0x089CE3A8; scene vtable 0x089D840C).
  - FUN_08918E90 (map init, from FUN_08913C24) gives the player's piece (+0x6D0) hi 0x800 and clears 0x1000.
  - FUN_089156D8 (per frame, from FUN_08913E10), for the 4 other pieces at +0x704..: if the player's piece is airborne, or FUN_08A6420C(piece+0x5C4) is true, they get 0x800 (and lose 0x1000); otherwise they get 0x1000 (and lose 0x800).
  - Net effect: a grounded player walking into map pieces is pushed out while the pieces stay put. When the player is jumping (or the piece is in that special state), the pieces yield instead.

**Effect in battle:** hi 0x800/0x1000 never occur on battle fighters. Lo 0x8000 occurs only during the intro. So for moves, `cant_be_pushed` 0x0049 (+0x195 & 0x80) is the only lever.

**Confidence:** strong for the constants (read from the image) and the writers; the scene names come from adjacent strings. **Test:** none needed for battle; on the world map, walking into an enemy piece should leave the piece in place.

---

### Q5. The 40 null (undecodable) retail timelines

**What the game reads:**
- Timeline container lookup **FUN_088298E4**: `{u32 ?, u32 count @+4, u32 offset[count] @+8}`, offsets relative to the container. It picks the **first timeline whose u16 @+0 equals the motion id** (a linear search). Slot order does not matter. In retail_timelines.json the slot index and the motion number differ, e.g. one100 slot 79 = motion 80.
- **Header = 8 bytes:** `u16 motion; u8 nRanged @+2; u8 nKeyed @+3; u16 keyedOffset @+4; u16 size @+6`. Neither walker reads `size`.
- **Ranged walker FUN_08828EEC** (called through FUN_08828E94 with handler 0x08829930):
  - starts at **+8**; event = `{u16 start, u16 end, u8 code, u8 nArgs, u8 phase, u8 cond, u8 f0, u8 f1, u8 x2, u8 x3, u32 args[nArgs]}`, size 12 + 4·n
  - fires when start ≤ t and (end == 0xFFFF or t < end), with these filters:
    - **phase** (pad low byte, +6): 0 = any, else == `*(*(owner+0x4C)+4)`
    - **cond** (pad high byte, +7): ctx 0 = no filter; ctx 1 normal, 2 EX Mode, 3 kind-2 flag, 6 kind 1 (mode 1 also passes in ctx 6); mode 4 = EX with a3 = 0, mode 5 = its complement
    - **x byte 0/1** (+8/+9): 0 = any, else −1 == actor+0xD60 / actor+0x1079 (s8)
- **Keyed walker FUN_08829104:** count @+3, offset @+4; event `{u16 frame, u8 code, u8 n, u8 phase, u8 cond, u8 f0, u8 f1, args}`, with the same filters.

**Why charmod_objx.ev_decode fails:**
- It treats h[4..7] plus the following u32 and args as a mandatory "header slot", and computes `n_r = (h[1] & 0xFF) − 1`.
- That slot is just **ranged event 0**. With **nRanged = 0** the keyed events start at +8, n_r = −1, and the decoder returns None. ev_encode also cannot produce such a file (it always writes rs[0]).
- Proof from the mod folder's own raw dumps: `yshtola/moveset/w_ysh10[1-5]/timelines/*_raw.bin` are exactly `08 00 | 00 01 | 08 00 | 1c 00` followed by one keyed 0x0335 event. That is motion 8, 0 ranged, 1 keyed, keyed at +8, size 0x1C, with no ranged slot at all.

**The 40 nulls** (by motion id, from motion_map against the decoded motions):
- **Hurt 550/560 and 551/561** (motions 25/26 or 24/25): one200, for200, fiv200, fiv210.
- **500/501/510/511/550/551 in the `_a` files** (motions 5/6): one200_a, for200_a, fiv200_a, fiv210_a, sev120_a.
- **Entry/victory/defeat-type scripts 1100/1110/1200/1210/1300:**
  - one100 (79: 1110)
  - one200 (70–73)
  - two200 (68)
  - for100 (76–78)
  - for200 (70)
  - fiv210 (78)
  - sev100 (82–84)
  - sev200 (76)
  - eht200 (72 or 73)
  - ten100 (73/74)
  - ten200 (72)
  - gst110 (93)
  - gst120 (93–95)

The same scripts on other characters decode with only 1–2 ranged events: 0x0015 alone, or 0x0214 face pairs. The null characters are exactly the ones with no `face_texture` at all (one200, for200, fiv200/210, sev120). Their hurt and pose motions are therefore keyed-only (voice/sound/footstep). That is consistent with **nRanged = 0**.

**Decoder fix:** parse the 8-byte header; read nRanged events from +8 with no special slot; keep `ranged[0]` only as a normal event; on encode allow an empty ranged list. Also slice each blob by `size` (u16 @+6) rather than the next offset, in case of padding. Keep pad (phase/cond) and x (filter bytes) as they are.

**Confidence:** the layout is confirmed from both walkers and the lookup; that the 40 retail nulls are zero-ranged timelines is strong (shown for the w_ysh raw files; inferred for p_*). **Check:** rerun the decoder with the fix on the 59 objx; all 40 should decode and re-encode byte-exact.
