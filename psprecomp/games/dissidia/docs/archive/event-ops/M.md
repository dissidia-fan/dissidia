# M: rarely used keyed ops (0x011E 0x0220 0x0633 0x011F 0x042D 0x0024 0x0B36 0x032E 0x042F 0x0109 0x0037k) + homing 0x25/0x27/0x28

**Summary**
- Alpha (see-through) ops: 0x011F fades the OPPONENT back in, 0x0220 fades the OPPONENT out (to a floor), 0x0109 fades SELF in. They drive actor +0x3B0/+0x3B4/+0x3B8, which a per-tick ramp copies to +0x370, and the draw code reads that as alpha (×128). 0x011E switches the fighter's form/stance (Cecil Dark Knight <-> Paladin), 0x0B36 gives a timed buff plus a pulsing body tint, and 0x0024 stops any turn in progress. 0x0037k un-freezes the opponent's animation.
- Conditional spawns: 0x0633 fires one of several projectiles depending on how often the button was pressed during the move (Laguna). 0x032E rolls a rare variant (1-in-N chance, needs a save-unlock bit), and 0x042F (effect) / 0x042D (projectile) then pick their alternate ids (Tidus).
- Homing: 0x0925 (code 0x25) is the ground/horizontal homing dash. It is common (110 retail uses), and its 9-arg layout is confirmed by the data. 0x27/0x28 have no retail uses. 0x28 takes 11 args, not 9.

Common facts used below:
- keyed handler regs: s0 = event, args at s0+8; s1 = s2 = actor (owner+0x30); s3 = owner = the motion player (actor+0x130), so s3+0x54 = actor+0x184.
- Every vtable (base 0x089C7C78, 0x08A61CF8, 0x08AB71A0, fighter 0x08B3A9A8) has slot +0x130 = 0x08836D9C, `return actor+0x300` (the 1v1 opponent). Entries are 8 bytes {s16 this-adjust, pad, fn}.
- vtable slot +0x118 = fire a projectile (magi): base 0x08836CA8 / fighter 0x08B304C4, which call `FUN_08AD0978(magi_id, actor, actor, 0, arg, 100, tag)`. This is the same path as keyed 0x0204 `fire_projectile` (handler 0x0882D2DC, which calls slot +0x118 with args[0], args[1]).
- FUN_0883739C(actor) = "actor kind (+0x542) is 0 or 1" (a real fighter).
- RNG: LCG at 0x09AF8B00 (`x = x*0x343FD + 0x269EC3; r = (x>>16)&0x7FFF`).

---

### 0x011E — Switch form / stance (Cecil Paladin <-> Dark Knight)
**Kind / args:** keyed, 1 arg: form index (0, 1, ... into the fighter's form table). The callee also accepts -1 (next form, cycling) and -2 (a random other form). Retail uses only 0 and 1.

**What the handler does (0x0882DD5C):** if FUN_0883739C(actor), it calls `FUN_08B33DDC(actor, arg0)`, which:
- returns at once if `(s8)actor[0xD60]` (current form) already equals arg0;
- otherwise calls `FUN_08ACF2A0(actor+0xD5C, actor, arg0)`, the form switcher. It uses the form table `*(actor+0xD5C)`: header +2 = count, +6 = cycle mode; entries are 0x20 bytes at +0x10. For the new entry it:
  - entry+1 != 0: refuses unless EX Mode is on (actor+0x5B2 bit 0x04) and FUN_08B3406C allows it;
  - stores the index (actor+0xD60);
  - entry+0 != 0xFF: `FUN_0886B18C(*(actor+0x394), entry[0])` sets model+0x8D (the mesh/texture variant the model draws);
  - entry+2 != -1: `actor+0x39C = FUN_0884E58C(global, id)` (a parameter set; +0x39C is also what the turn code reads);
  - clears the motion player's script-remap slots (FUN_0882FE00) and adds up to 4 `{from script, to script}` pairs from entry+0x10 (FUN_0882FDC4 → motion player +0x160..);
  - if the current script (+0x180) is one of the remapped ones, restarts it (FUN_0882FA10);
- then spawns a transition effect: entry+0xE (or entry+0xA, chosen by global `*(0x08A405E0)+0x278`), if > 0, through `FUN_088A9DE8(*(actor+0x390)+0x34+0x28, id, actor+0x4C8, 0, 0)`.

**Who uses it / effect in game:** the model variant byte (+0x8D) chooses which mesh set is drawn; the script remap changes which motion each move id plays. The engine calls the same switcher from FUN_08B33EA8 (forms 0/1/2/-1, by `*(actor+0x101C)+0x20`), from FUN_08B3526C and from the 0x08ACAxxx manager functions (EX Mode / revenge).

**Data check (11 uses):**
- Cecil (for100) EX Burst: 8000 @0 {0}, 8101 @20 {1}, 8103 @40 {0}, 8501 @12 {0}, 8500 @158 {1}. Cecil changes between Dark Knight and Paladin during the burst, which fits.
- Onion Knight (thr100): chase motions 810–821 @0 {1}, x=512.

**Confidence:** **strong** (the mechanism is clear; "form" = Cecil's two jobs is inferred from the data). Test: put `0x011E {1}` at frame 0 of a Cecil/Bartz-based move; the mesh variant should change and an effect should play.

---

### 0x011F — Fade the opponent back in over N ticks
**Kind / args:** keyed, 1 int: N ticks (0 = at once).

**What the handler does (0x0882DDB8):** opp = vtable+0x130 (actor+0x300; there is no null check). It calls `FUN_08834D60(opp, (float)N)`: `opp+0x3B4 = (N==0) ? 1.0 : 1.0/N` (the alpha rate per tick).

**Who uses it / effect in game:** the per-tick alpha ramp `FUN_08833404` (called from FUN_088281E0 and FUN_08850F44; both are listed in function tables at 0x089C775C / 0x089C8B8C):
- while +0x3B4 != 0: `+0x3B0 += +0x3B4 × timescale` (FUN_08838300, or a global step when state mask 0x089C7F30 matches);
- clamps at the floor +0x3B8 (on reaching it, the rate is set to 0) and at 1.0 (rate set to 0);
- copies the result to **+0x370**.

FUN_088281E0 then makes +0x370 = min(own +0x3B0, `*(actor+0x550)`+0x3B0). The draw code reads +0x370 as the model alpha: ×128.0 → int at 0x08AC65B0, and it multiplies vertex/colour alpha bytes at 0x08AC6EC4, 0x08AC72E8, 0x08AC7C14, 0x08AC845C; also the 7-part draw in FUN_08839D84 (0x0883A074). In short, +0x3B0 = opacity (0..1), +0x3B4 = fade speed, +0x3B8 = fade floor.

**Data check (7 uses, all EX Burst scripts 8102/8500/8600):**
- Emperor (two200) 8102 @20 {2}.
- Exdeath (fiv200) 8102 @12 {4}.
- Sephiroth (sev200) 8500 @144 {6}, then 0x0220 {30,0} @188.
- Cloud (sev100) / Sephiroth 8600 @0 {0}: snaps the opponent fully visible when the burst ends.

**Confidence:** **confirmed** (writer → ramp → draw alpha). The target is the opponent, which is unusual but clear from vtable slot +0x130 in every class. Test: in a burst, change {6} to {120}; the victim should take 2 s to reappear.

---

### 0x0220 — Fade the opponent out over N ticks, down to a floor
**Kind / args:** keyed, 2 ints: {N ticks, floor alpha in % (0–100)}.

**What the handler does (0x0882DE08):** `FUN_08834D30(opp, (float)N, arg1/100.0)`: `opp+0x3B4 = (N==0) ? -1.0 : -1.0/N`, `opp+0x3B8 = arg1/100`.

**Who uses it / effect in game:** the same ramp as 0x011F, going down. The alpha falls by 1/N per tick (from wherever it is) and stops at the floor. With N=0 the opponent vanishes in one tick.

**Data check (6 uses):**
- Emperor (two200) 8104/8105 and 8106 @0 {0,0}: the victim disappears at once.
- Cloud of Darkness (thr200) 8501–8503 @100 {12,0}, 8500 @212 {18,0}.
- Sephiroth (sev200) 8500 @188 {30,0}.
- Yuna (ten110) 8500/8700 @104 {10,0}.
- All are in EX Burst finishers, where the victim is swallowed or vanishes on camera.

**Confidence:** **confirmed.** Test: use {30, 50}; the opponent should fade to half-transparent and stay there until something restores it (0x011F / 0x0121).

---

### 0x0109 — Fade self in over N ticks
**Kind / args:** keyed, 1 int: N ticks.

**What the handler does (0x0882D56C):** `FUN_08834D60(actor, (float)N)`: own +0x3B4 = 1/N (or 1 if N=0). It is the self version of 0x011F.

**Related ops (not assigned):**
- keyed code 0x0A (0x0882D58C) = self fade-out `FUN_08834D30(actor, N, floor/100)`, the self version of 0x0220;
- keyed 0x0121 (0x0882DE70) = set the opponent's alpha at once, `FUN_08834DF4(opp, a/100)`: +0x3B0 = +0x370 = a/100, rate 0;
- the ramp also starts from actor+0x2A8 when flag block +0x195 bit 0x08 is set (0x08833404).

**Data check (1 use):** Emperor (two200) 8102 @0 {128}: he fades in over about 2 s at the start of a burst stage.

**Confidence:** **confirmed** (same chain as 0x011F). Test: put `0x0220`-style self fade-out (code 0x0A {0,0}) then 0x0109 {60}; the character should fade in over 1 s.

---

### 0x0633 — Fire a projectile depending on the button-press count
**Kind / args:** keyed, 6: {magi id, magi arg, count, if ≤, if ==, if ≥}. The last three are booleans.

**What the handler does (0x0882D340):**
- Does nothing if the actor's state mask `(+0x388,+0x38C) & (0, 0x4000)` (0x089C7908) is set; 0x0204 has the same guard.
- Only for fighters. c = `actor+0x107F` (u8).
- Fires if any enabled test holds: (arg3 && c ≤ arg2) || (arg4 && c == arg2) || (arg5 && c ≥ arg2). It then calls vtable+0x118(actor, arg0, arg1), which is `fire_projectile`.

**Who uses it / effect in game:** `actor+0x107F` is cleared when an action starts (FUN_08B382B8 at 0x08B387A8, FUN_08B39E5C). It is incremented in the fighter update FUN_08B3526C (0x08B367C4) whenever FUN_08B326C4 is true. That function checks the pad (`FUN_08A64244(*(actor+0x5C4), 1)` → pad+0x530 input test) or asks the CPU controller (`*(actor+0x5A0)` vtable). So it counts button presses (or CPU "presses") made during the move.

**Data check (6 uses, Laguna eht110):** at the same frame there are three events: 10020 @28 {1539,0,0,0,1,0} (0 presses), {1540,0,1,0,1,0} (exactly 1), {1541,0,2,0,0,1} (2 or more). 10050 @38 → 1547 / 1619 / 1620. One projectile fires per case, and the cases don't overlap.

**Confidence:** **strong** (the condition logic is confirmed; "button presses" is inferred from FUN_08B326C4 reading the pad, and the exact button is unknown). Test: on Laguna 10020, tap attack 0/1/3 times after starting the move; you should get three different shots.

---

### 0x032E — Roll the rare variant for this move (sets the "alt" bit)
**Kind / args:** keyed, 3 ints: {min value, unlock bit index, 1-in-N chance}.

**What the handler does (0x0882E50C):** only for fighters.
- roll = (arg2 ≤ 0) ? true : (rand() % arg2 == 0).
- If roll, and `*(u8*)*(actor+0x52C) ≥ arg0`, and bit `arg1` is set in the per-player record's bitfield: `owner(motion player)+0x54 |= 0x40`.
- Details:
  - The byte at `*(actor+0x52C)` is byte 0 of a 0x2C-byte fighter status block. It defaults to 1 and is copied from a source record by FUN_088429A4; probably the level.
  - The bitfield is `0x09AFB8A0 + (s16)actor[0x10CC]*0x1060 + 0xD34 + (arg1>>5)*4`. It is copied (0x200 bytes) by FUN_088F9320 from the save block `*(0x089D712C)+0x1DF88`, so it is a save/unlock flag set.

**Who uses it / effect in game:** motion player +0x54 bit 0x40 is read only by 0x042F and 0x042D (below), which then use their alternate ids. It lives on the motion player, so it applies to the current motion.

**Data check (4 uses, Tidus ten100):** {1, 454, 10} at frame 0 of motions 50 (scripts 10020/10050/10051), 51 (10021), 59 (10095), 63 (10096). Each of these motions also has 0x042F and/or 0x042D. So 10% of the time, if unlock 454 is owned, the move plays with other effects/projectiles.

**Confidence:** **strong** (code clear; what unlock 454 is and what +0x52C byte 0 is are unknown). Test: set arg2 = 1 (always) on Tidus 10020; the alternate effect 80 and magi 1441 should appear every time, provided the save has bit 454.

---

### 0x042F — Effect with a variant: {normal effect, lifetime, alt effect, alt lifetime}
**Kind / args:** keyed, 4 ints. The effect id is the record id, as in 0x0219. Lifetime in ticks; 0 leaves the effect's own lifetime.

**What the handler does (0x0882E02C):** if `owner+0x54 & 0x40` (set by 0x032E), it uses {arg2, arg3}, else {arg0, arg1}. Then `fx = FUN_088A9DE8(*(actor+0x390)+0x34+0x28, id, actor+0x4C8, 0, 0)` and `fx+0xE4 = (float)lifetime`. This is identical to keyed 0x0219 (0x0882DFE4) apart from the switch.

**Data check:** Tidus 10020 @0 {25,0,80,0}, 10021 @0 {69,0,81,0}.

**Confidence:** **confirmed** (code). Test: swap ids 25 and 80.

---

### 0x042D — Projectile with a variant: {normal magi, arg, alt magi, alt arg}
**Kind / args:** keyed, 4 ints.

**What the handler does (0x0882D4C4):** same state guard as 0x0204. If `owner+0x54 & 0x40`, it calls vtable+0x118(actor, arg2, arg3); else vtable+0x118(actor, arg0, arg1). That is `fire_projectile`.

**Data check (6 uses, Tidus):** 10020 @38 {271,0,1441,0}; 10021 @40 {572,0,1445,0}; 10095 @60 {267,0,1442,0}; 10096 @22 {268,0,1443,0} and @102 {269,0,1444,0}. Every one has a 0x032E roll at frame 0 of the same motion.

**Confidence:** **confirmed** (code). Test: as for 0x032E.

---

### 0x0024 — Stop turning (desired facing = current facing)
**Kind / args:** keyed, 0 args.

**What the handler does (0x0882E280):** calls `FUN_08835CAC(actor)`, which sets `actor+0x320 = actor+0x9C`.

**Who uses it / effect in game:**
- +0x320 is the desired yaw. FUN_08835C54 / FUN_08835C7C write it as atan2 of a direction (FUN_089A457C).
- +0x9C is the current yaw (part of the orientation block at +0x80, rebuilt by FUN_08835B3C).
- The actor update at 0x08831430 compares them and, when they differ and +0x39C is set, turns +0x9C toward +0x320 at `+0x3D0 × FUN_0884E494(+0x39C) × timescale`.
- Copying the current yaw into the desired yaw ends any turn in progress. The character stops rotating and keeps the heading it has this tick.

**Data check (4 uses):** Garland (one200, and _a) 10000 @80 x=4; Cecil (for100, and _a) 10021 @16. Both are mid-move.

**Confidence:** **strong** (the turn consumer is clear; whether later turn ops in the same motion rewrite +0x320 is not checked). Test: on a move with a slow turn toward the foe, add 0x0024 early; the character should stop rotating at that frame.

---

### 0x0B36 — Timed buff with a pulsing body glow
**Kind / args:** keyed, 11: {f duration ticks, a0, a1, a2, b0, b1, b2, R, G, B, pulse period}.
- duration is a float; the rest are ints.
- a*/b* are signed bytes; R, G, B and the period are converted to float.

**What the handler does (0x0882E660):** for fighters only, `FUN_08B34F40(actor, …)` stores:
- +0x1274 = duration;
- bytes +0x1288..+0x128A = a0..a2, +0x128B..+0x128D = b0..b2;
- +0x127C/+0x1280/+0x1284 = R/G/B;
- +0x1278 = period.

**Who uses it / effect in game:**
- Timer: the fighter update (vtable+0x18, FUN_08B37604, at 0x08B37B50) subtracts the global step each tick. When it reaches ≤ 0, it zeroes the six bytes. Fighter init (FUN_08B39E5C, 0x08B3A53C) clears it all.
- Glow: the colour routine (vtable+0x40, FUN_08B37BCC, 0x08B37DF0) runs while the timer is > 0. It computes `p = (sin(+0x318 / period)+1)/2` (FUN_089A47A4), and each colour channel = 0x80 + p×R/G/B. That is a pulsing additive tint, unless an option global forces plain 0x80.
- Stats: the hit/damage calculation FUN_08AE45B8 (0x08AE530C, 0x08AE55E0) and FUN_08AE70E4 (0x08AE790C) add the bytes while the timer is > 0:
  - the attack record's flag `+8 & 0x80` chooses set a (clear) or set b (set);
  - byte 0 is added to one total of one fighter (s7);
  - byte 1 is added to a total of the other fighter (s2);
  - byte 2 is added in FUN_08AE70E4.
- So these are three damage-formula modifiers, given separately for two attack categories. Which stat each one is (attack / defence / …) and which category bit 0x80 means are **unknown**.
- FUN_08A7C1DC (0x08A7CFAC) also checks whether the timer is active.

**Data check (4 uses, Onion Knight thr100):** motions 47/48 (scripts 10060–10063), two alternatives at frame 0 chosen by x (x byte 0 = owner variant):
- x=1: {600, 10,10,3, 0,0,0, 0,0,217, 2}, a 10 s buff of set a, blue pulse;
- x=2: {600, 0,0,0, 10,10,3, 217,0,200, 2}, set b, magenta pulse.

**Confidence:** **strong** for the timer, glow and "stat bonus in the damage calc"; **weak** for which stats. Test: use the x=1 form on another character; they should pulse blue for 10 s and deal or take different damage for one attack type only.

---

### 0x0037 (keyed) — Release the opponent's frozen animation
**Kind / args:** keyed, 0 args.

**What the handler does (0x0882E6B8):** opp = vtable+0x130 (actor+0x300). If opp exists and `opp+0x188 & 0x10`, it clears that bit. opp+0x188 is the opponent's motion player +0x58.

**Who uses it / effect in game:**
- The motion advance FUN_08830444 (0x0883049C) feeds dt = 0 to the animation when motion player +0x58 bit 0x10 is set, so the pose is frozen.
- Something else sets that bit, for example:
  - FUN_08A54F08 (0x08A5508C), together with an instant alpha set (FUN_08834DF4);
  - FUN_08A56E10 (a timed toggle);
  - FUN_08A56F98;
  - the script-command handlers FUN_08A6AC9C / FUN_08A6ADB4.
- Others read it too: FUN_0883A150 (0x0883A6A8), FUN_0888FE28, FUN_08AD1588.
- So the event resumes an opponent whose animation was frozen (time stop / held pose).
- The ranged 0x0037 (0x0882B7B4, someone else's) instead sets the actor's own flag +0x195 bit 0x01.

**Data check (2 uses):** Ultimecia (eht200) 10070 @0; sev120_a (Tifa or Aerith; code order not checked) 10001 @0. Both are at the very start of the move.

**Confidence:** **strong** for the code (clears the opponent's motion-freeze bit); **weak** for which game situation it is meant for. Test: on Ultimecia, remove it from 10070 and use the move right after something that freezes the foe; the foe should stay frozen.

---

### Homing variants 0x25 / 0x27 / 0x28 (see out1/F.md for the shared homing state 0xF)
All three call the homing setup `FUN_08A60BC8(actor, a1, a2, a3=alt target, f12 speed, f13 vertical speed, f14 track ticks, f15 height, f16 angle rad, f17 distance, f18 arrive radius, f19 max ticks)`.

| op as used | handler | a1/a2 | target check | layout |
|---|---|---|---|---|
| **0x0925** (9 args, 110 retail uses) | 0x0882D9E8 | 1/0 | yes (vtable+0x120; no target → nothing) | {speed, track ticks, angle°, distance, arrive radius, max ticks, skip-target-checks (0x40), stop-if-target-busy (0x80), alt target}. Vertical speed and height are fixed at 0, so the dash is horizontal only. Always enters state 0xF. Bit 4 means it ends in state 0 (ground) and doesn't set +0x384 bit 0x1000. |
| 0x0D27 (13 args, unused) | 0x0882DAA0 | 0/1 | no | Same as 0x0D10, {speed, track, height, angle°, dist, arrive, max, vspeed(≤0 → speed), no-rise, no-fall, skip-checks, stop-if-busy, alt target}. Enters state 0xF only if airborne. Bit 8 = fixed direction (straight dash). |
| 0x0B28 (11 args, unused) | 0x0882DBA4 | 1/1 | no | {speed, track, angle°, dist, arrive, max, no-rise (0x10), no-fall (0x20), skip-checks (0x40), stop-if-busy (0x80), alt target}. Always state 0xF. **Correction to F.md:** 0x28 takes 11 args; it also reads the no-rise / no-fall flags at args 6/7. |

**Data check for 0x0925:**
- It is a ground move-in: Cecil (for100) 10000 @0 {0.6, 12, 0, 2.5, -1, 12, 0,0,0}; Kain (for110) 10000 @0 {0.22, 20, 0, 1.0, …}; Gilgamesh (fiv210) 10005 @18 {1.1, 8, 0, 1.5, -1, 4, 0, 1, 0}.
- Arg5 (max ticks) usually equals arg1 (track ticks), and arg4 = -1 (no arrive radius) as in 0x0D10.
- Cloud (sev100) 10090 has three chained dashes @38/76/116 with arrive radius 0. 10091 has a float 20.0 in the flag slot (non-zero → flag 0x40 set).
- Duplicates with x=1/2/3/4 (Golbez for200 042) are walker alternatives.

**Confidence:** 0x25 **confirmed** (code plus 110 uses that match the layout). 0x27/0x28 **strong** on code, with no data.
