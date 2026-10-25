# G: ranged ops 0x0216, 0x0126, 0x0013, 0x0548, 0x0811, 0x046D, 0x0743, 0x003A

**Summary**
- **Movement / facing.** 0x0548 = **straight-line lunge**: constant velocity along the facing direction, pitched by arg0 degrees (+ = up), at arg2 per tick, enters air-action state 0x12. arg3/arg4 make it the no-target fallback / the continuation after a homing dash (0x0D10) stops. 0x046D = **turn toward the target** (lock-on target, falling back to the opponent), the same as 0x040B. 0x0811 has two modes: arg2 = 0 turns the real facing toward the target, and arg2 = 1 is a **visual body-swivel toward the target** (yaw offset added to the model matrix, with hold and decay). All are confirmed, except one inferred link noted under 0x0811.
- **Combat.** 0x0743 = **hit window that also fires a projectile on contact** (magi id arg2) and can show a caption (arg4 = 1 → caption arg5, the same as keyed 0x021D). 0x003A = **dodge tail: may start a Brave attack, an HP attack or EX input** even though the normal "can attack" gate is still closed (the sum of 0x0036 + 0x0037 + one more bit). 0x0013 = **no body collision** (passes through fighters, neither pushes nor is pushed). All confirmed in code; 0x003A's input classes are strong.
- **Presentation.** 0x0216 = pin **all spring bones of physics group arg0** to the animation (the group version of 0x0227), at most arg1 °/tick. 0x0126 = **EX Burst only: the victim's speed multiplier** (arg0 = 0 freezes, 0.2 = slow motion), reset to 1.0 when the range ends. Confirmed / strong.

Conventions: actor = dispatcher s2/s3; owner s1 = motion player = actor+0x130, so s1+0x60+n = actor+0x190+n (the per-tick flag block, cleared each tick). args start at event+0xC. "vtable+N" = `*(actor+0x544)+N`, with entries `{s16 this-adjust, fn}` (fn at +N+4). Fighter vtables 0x089C7C78 / 0x08B3A9A8: +0x120 → FUN_08836D8C = actor+0x2FC (current target); +0x130 → actor+0x300 (opponent); +0x118 = fire projectile; +0xB8 = start hit. Scratch scripts: `scratch_G/usage.py` (retail usage stats), `scratch_G/flagscan.py` (finds `addiu r,x,0x190|0x60 ; lbu n(r) ; andi mask` readers).

---

### 0x0216 — Pin a physics group (hair/cloth chain) to the animation

**Kind / args:** ranged, 2 args `{group, maxDegPerTick}`. arg0 = physics **parameter-set / group index** (the s16 at +6 of each spring-bone record). arg1 = maximum correction per tick in degrees (180 = snap, 360 = always snap). Seen: arg0 0–7 (mostly 0–2); arg1 30 (≈55 %), 180 (≈40 %), 360.

**What the handler does (0x0882A9C8):** only for actor kinds 0/1 (FUN_0883739C), 2 or 4; otherwise it exits. `obj = *(actor+0x398)` is the spring-bone set (count +0x20, 0x170-byte entries at +0x34). For **every** entry e whose record `*(e+0x158)` has s16 +6 == arg0:
- If `e+0x166 != actor+0x180` (the script changed since this entry was last pinned) or `e+0x164 == 0`: `e+0x164 = 1`, which restarts the measured blend.
- `e+0x166 = script id`, `e+0x150 = 0` (re-seat request), `e+0x168 = arg1·π/180`.
It runs every tick of the range.

The record's +6 is the index into the 0x1C-byte parameter table at obj+0x28; the init code stores `e+0x15C = obj+0x28 + 0x1C·rec[+6]` (0x0880DC08–0x0880DC2C). So all bones that share one physics parameter set (one hair or cloth chain) are matched. 0x0227 (round 1, agent C) does the same for a **single** bone (rec +0, via FUN_0880CD9C) and does not track the script.

**Who uses it / effect in game:** the same consumer as 0x0227: the physics update FUN_0880EA20 (called from FUN_08B30814 at 0x08B3086C). When e+0x150 == 0 it re-seats the bone on its animated pose, rotating at most e+0x168 per tick while e+0x164 == 1, and snapping once e+0x164 == 2. It then sets e+0x150 = 1, so the spring simulation resumes one tick after the range ends. e+0x166 is read only by this handler (no other `0x166(` access in all.s). Its only job is to restart the gradual blend when a new script starts while the chain is still pinned.

**Data check (642 uses, 23 fighters):**
- 574 of the 642 uses span the whole motion (0–65535). They are concentrated in EX Burst scripts 8000–8600 (~240), intro/victory 1100/1200 (~72), and character moves (252).
- Noctis p_noc100 071–078 (EX Burst) all have {0,180}: the main chain is locked to the animation for the cinematic.
- one200 063 (8101) uses {1,360}; for110 076 (8501) uses {0,180}; thr200 020 (scripts 300/822/3000) uses {0,30} over 0–32, a soft blend.
- This fits a group version of 0x0227.

**Confidence:** confirmed for the mechanism, because it is the same consumer as 0x0227 (strong there for the "animated pose" target). Which group number is hair and which is skirt/cape is per model and not known. **Test:** on a Noctis EX Burst motion, change {0,180} to {0,5}. The hair/cloth chain should lag visibly behind the animation and slowly settle onto it.

---

### 0x0126 — EX Burst: victim's speed (slow-motion / freeze factor)

**Kind / args:** ranged, 1 arg, float `factor`. Seen: 0 (47), 0.01–0.1, 0.2 (46), 0.3–0.8, 1.0. Ranges are usually whole-motion or start mid-motion (e.g. 132–end).

**What the handler does (0x0882B254):** `actor+0x1B8 = arg0` (s1+0x88) and **actor+0x193 |= 0x40**. Its neighbour 0x0034 (0x0882B274, retail 0x0134) writes the same float with bit 0x80.

**Who uses it / effect in game:** the only reader of +0x193 bit 0x40 (flagscan) is **FUN_08A3FE10**, the per-frame update of the EX Burst controller. The controller object is `*(0x08A405E0)`; it is called from FUN_088413D0 at 0x0884155C. The 0x012B handler also calls into this object (FUN_08A3EAB4). The controller's +0x26C is the bursting fighter (the one whose timeline runs the op), +0x270 is the other fighter, and +0x278 is the burst phase.
- At 0x08A3FEB8–0x08A3FF08: if `burster+0x193 & 0x40`, call `FUN_08B34CB0(victim, slot 1, burster+0x1B8)`; otherwise call `FUN_08B34CB0(victim, 1, 1.0)`.
- **FUN_08B34CB0** stores the factor in `victim+0x1250[slot]` (6 slots), multiplies all 6, and calls vtable+0x1A8 → **FUN_08B34D24**. That sets `victim+0x3C8` (the actor time scale, read by FUN_08838300 for motion and velocity) and passes it on to up to 16 child objects (list at +0xD64: projectiles and summons).
- So while the range runs, the victim's animation, movement and its projectiles run at arg0 × normal speed. They return to 1.0 on the first controller update after the range.
- For reference, the 0x80 bit (0x0134) feeds `FUN_08A62020(*(0x08A62280), 2·arg0, 0)` in the same function, a different global slot.

**Data check (236 uses, 29 fighters):** used only in EX Burst scripts 8000/8100/8500 (8100: 123, 8500: 85, 8000: 28). Examples:
- sev200 073 (8500) {0} over 0–134: the victim is frozen while the burst plays.
- gst120 088 (8101) {0.7} from tick 132.
- for110 077 (8500) {0.45} from 92.
This fits slow-motion / freeze of the opponent during the burst cinematic. Note the different mechanism from keyed 0x0121 (agent L: opp+0x3B0 time scale).

**Confidence:** confirmed (handler → flag → controller → time-scale slot → +0x3C8). That +0x26C is the burster and +0x270 the victim follows from the read (the bit is read on +0x26C) and from the usage. **Test:** in an EX Burst timeline change {0} to {1.0}; the opponent should keep animating instead of freezing.

---

### 0x0013 — No body collision (pass through fighters)

**Kind / args:** ranged, 0 args.

**What the handler does (0x0882A984):** `actor+0x192 |= 0x02` for this tick. Nothing else.

**Who uses it / effect in game:** the only reader of +0x192 bit 0x02 is **FUN_088375D4** ("takes part in body collision"). It returns true only if all of these hold:
- state mask (+0x388/+0x38C & 0x089C7F10 = 0x1000) is set
- **not** +0x192 bit 0x02
- the collision capsule list `*(actor+0x468)` exists with count > 0
Callers:
- **FUN_08833360** (vtable +0xAC: the body push-apart pass, see 0x0049 in out1/B.md). It skips any other actor for which FUN_088375D4 is false (0x088333AC), so a flagged fighter is not a collision partner for anyone.
- **FUN_0883AF80** (the movement integrator) at 0x0883B32C. It calls vtable +0xA8 → FUN_08833360 (its own push-apart pass) only if FUN_088375D4(self) is true.
So while flagged, the fighter neither pushes nor is pushed: it moves straight through other fighters' bodies. This is distinct from 0x0049 (which makes others yield to you).

**Data check (227 uses, all 30 fighters):** 156 of the 227 uses span the whole motion.
- Dash motions (scripts 240/242/244, e.g. one200 037, eht200 037, ten200_a 007).
- Field actions (scripts 200s: 59).
- EX Bursts (~94) and character moves (73), e.g. Noctis 051 FF09_AIR_UPPER 0–68, nin100 ... 46–92.
Dashing through the opponent and cinematic grabs both need this.

**Confidence:** confirmed. **Test:** remove 0x0013 from the dash motion (037) and dash into a standing opponent; you should now bump/slide off instead of passing through.

---

### 0x0548 — Straight-line lunge (fixed-direction velocity)

**Kind / args:** ranged, 5 args `{pitchDeg (float), yawDeg (float), speed (float), onlyWithoutTarget (int), afterHomingStops (int)}`.
- arg0 (0 in 143 of 225; also ±20/45/55/75/90).
- arg1 is always 0.
- arg2: 0.03–1.0, mostly 0.1–0.35. One −0.2 = backwards.
- arg3 = 1 in 198 of 225.
- arg4 = 1 in 85 of 225.

**What the handler does (0x0882BDC4):**
1. **Gate.**
   - If the actor is in state 0xF (homing dash, from keyed 0x0D10):
     - arg4 != 0: act only if the homing block flag `*(actor+0x314)+0x48+0x34 & 2` ("homing stopped") is set.
     - Otherwise, if arg3 != 0 and vtable+0x120 (current target, actor+0x2FC) is non-null, exit.
     - The state is not changed.
   - Otherwise: if arg3 != 0 and there is a current target, exit. If the state is not 0x12, `FUN_08834C10(actor, 0x12, 0)` (air-action state) and set +0x190 |= 2 (air motion, as 0x000A) on that tick.
2. **Direction.**
   - axis = cross(actor+0x90, actor+0xA0) (VFPU vcrsp at 0x089A3B30), normalised. actor+0x90 is the facing vector: FUN_08835B3C writes it from the yaw.
   - Rotate the facing vector about that axis by arg0·π/180 (Rodrigues matrix built inline at 0x0882C010–0x0882C0D0).
   - Then apply FUN_089A4124 with arg1·π/180 (a yaw rotation; arg1 is never used), and normalise.
3. **Speed.** `speed = arg2 × FUN_08838300(actor)` (the actor time scale +0x3C8).
4. **Apply.** `FUN_08835034 → FUN_088339FC(actor, {dir, speed})` **adds** dir·speed to the per-tick velocity at actor+0xE0 (stored as unit dir + length at +0xEC).

**Who uses it / effect in game:** actor+0xE0 is zeroed every tick at the start of the actor update (FUN_08833910 → FUN_08833658 → FUN_089A3890, before the motion and its events advance). FUN_0883AF80 then moves the actor by `+0xE0·+0xEC` (0x0883B034). So while the range runs the fighter travels at a **constant arg2 units per tick** along the pitched facing direction. Other velocity sources in the same tick (e.g. the homing dash) add to it.

**Sign of arg0 (+ = up):** in one100 050 / 052, mirrored variants pair a 0x0D10 homing dash with a lunge:
- 050: homing destination 2 below the target, flag "no falling" (arg9), lunge **+45°**.
- 052: destination 2 above the target, "no rising" (arg8), lunge **−45°**.
- Positive therefore goes with the rising variant.
- Bartz-based Noctis 044 (FF01_NEAR3, a rising ground slash that also sets 0x000A) uses +45° at speed 0.07.
- Y'shtola 056/057 (BR_A_SONIC2(_EX)) use 90° (straight up/down) with arg3 = 0.

**Data check (225 uses, 26 fighters, character moves only):**
- 182 of the arg3 = 1 uses sit in timelines that also have a keyed 0x0D10. With a target, the move homes; without one (0x0D10 does nothing without a target), it lunges straight.
- arg4 = 1 uses come after the 0x0D10 frame, e.g. one100 049: 0x0D10 at frame 20, lunge {0,0,0.09,1,1} over 20–50. They keep the fighter moving once homing gave up.
- Typical ranges are 0–12 / 2–20 ticks.

**Confidence:** confirmed for gate, state switch and velocity. Strong for the pitch sign: the code alone fixes it only up to the axis convention; the data pairs settle it. **Test:**
1. Noctis 051 (0x0548 {0,0,0.25,1,0} over 0–40): with no lock-on target the attack should glide forward 0.25/tick; set arg2 = 1.0 and it should shoot off much further.
2. Set arg0 = 45: it should rise diagonally.

---

### 0x0811 — Turn to target (arg2 = 0) / swivel the body toward the target (arg2 = 1)

**Kind / args:** ranged, 8 args `{deg/tick (int), away (int), mode (int), limitL (float deg), limitR (float deg), holdTicks (float), smoothing (float 0–1), decay (float 0–1)}`.

**What the handler does (0x0882A418):**
- **mode (arg2) = 0:** clamp arg0 to 0–180 and call `FUN_08836488(actor, arg1 != 0, arg0·π/180)`. If there is a current target (+0x2FC), it gets the direction (FUN_08836DAC) and calls FUN_08836120. That turns the actor's facing (yaw +0xAC via FUN_08835CF8, plus the +0xAC pitch term) toward it by at most arg0 per tick, scaled by time scale. With **arg1 != 0 the direction is negated** (turn away). This is a real facing change, like 0x0402 but without 0x0402's target-state checks.
- **mode != 0:** `FUN_08A62C58(actor+0x1120, …)`. actor+0x1120 is an **aim-offset object** with 3 slots of 0x60 bytes at +0x10. The function returns early if there is no current target. Otherwise it takes a type-2 slot (FUN_08A62FA0) and writes:
  - type +0x20 = 2 (aim at the current target; type 1 = the opponent via vtable+0x130)
  - bones +0x24/+0x28 = −1, weights +0x2C/+0x30 = 0
  - limits +0x34 = arg3 (left), +0x38 = arg4 (right); pitch limits +0x3C/+0x40 = 0
  - rate +0x44 = arg0, hold +0x48 = arg5, smoothing +0x4C = arg6, decay +0x50 = arg7
  - auto +0x54 = 1, stick +0x55 = 0, phase +0x58 = 0
  - arg1 is not read in this mode.

**Who uses it / effect in game (mode 1):**
- **FUN_08A6307C**, called per tick from 0x08B379F8, runs each slot.
  - Phase 0 (refreshed every tick while the event runs): it measures the yaw between the facing vector (+0x90) and the direction to the target (positions +0x100). It accumulates at most `arg0·π/180·timescale` per tick into slot+0, clamped to [−arg3°, +arg4°]. The pitch is clamped to 0, then it goes to phase 1.
  - Phase 1: after arg5 ticks without a refresh → phase 2.
  - Phase 2: the offset decays by `arg7^(dt/2)` until it is below a small epsilon, then the slot is freed.
  - The displayed value slot+0x10 follows slot+0 with factor `1 − (1−arg6)^(dt/2)` (arg6 = 1 → instant).
  - The slot is freed early when the fighter is neither in an action (+0x380 & 8) nor in can-act (+0x190 bit 0).
- **FUN_08A62F00** (via fighter vtable +0x208 → FUN_08B34C94) returns the sum of slot+0x10 × (1 − bone weights). For this op the bone weights are 0, so it returns the full yaw offset.
- **FUN_0883D074** (the actor model-matrix builder) adds that offset to the facing yaw +0xAC (0x0883D2A4–0x0883D2BC). It then rebuilds the rotation matrix at actor+0x40 about a pivot height (FUN_088370FC).
- Net effect: the **rendered body swivels toward the target** by up to arg3/arg4 degrees, while the movement facing (+0x90/+0xAC) is unchanged. Because the bones are posed from that matrix, bone-attached hit volumes and spawn points should follow (inferred, not traced).
- The bone-based variant of the same system (bones +0x24/+0x28 with weights, FUN_08A62390, applied before the physics) is driven by ops 0x0D63/0x0D64 (0x0882C980/0x0882C9FC), not by this op.

**Data check (190 uses, 29 fighters; 16 in the header slot):**
- Mode 0: {20,0,0,…} over 0–2 in the guard motions 470/471 (29 uses): the guard snaps toward the foe. Also {4/10/30,0,0,…} on a few moves.
- Mode 1:
  - {5,0,1,30,30,36,0.8,0.9}: ±30°, then hold 36 ticks.
  - {4,0,1,90,90,70,0.8,0.9} over whole beam/charge moves, e.g. six100 058 (10092), ten100 059 (10095).
  - Noctis p_noc100_a 012 (FF12 katana) {45,0,1,45,0,30,1,0.75} over 0–2: a quick asymmetric swivel (left only), held 30 ticks.

**Confidence:**
- Mode 0: confirmed.
- Mode 1: confirmed through to the model matrix (actor+0x40). That the swivel is purely visual for movement but moves hitboxes is inferred.

**Test:** on Noctis 012 change arg3 from 45 to 0. The model should stop twisting toward a target standing off to the side. Its travel direction should be the same in both cases.

---

### 0x046D — Turn toward the target (lock-on target, else opponent)

**Kind / args:** ranged, 4 args `{maxDegPerTick, onlyWhileTargetReels, allowVsGuard/Dodge, skipIfTargetReels}`. Seen: {180,0,1,0} (66), {45,0,1,0} (59), {90,0,0,0} (29), {180,0,0,0}, {10/30/5/360,0,1,0}. arg1 is always 0.

**What the handler does (0x0882A1E8):**
1. `t = vtable+0x120` (current target, +0x2FC). If null, `t = vtable+0x130` (opponent, +0x300). If both are null, exit.
2. **arg3 != 0** and FUN_08838074(t) (t's script is 500–798, a hit reaction): exit.
3. **arg2 == 0** and FUN_08837458(t) (t's script is 400–469 evade/guard, or t+0x194 bit 0x10): exit. This is lifted when the actor is in an action (+0x380 & 8), is a fighter, and the current action record (FUN_08B30898) has +0x18 bit 0x10.
4. Turn with rate = arg0·π/180:
   - **arg1 != 0** → FUN_088360BC(actor, t, rate): turns only if t **is** in a hit reaction (500–798), then calls FUN_08835F60.
   - Otherwise → FUN_08835F60(actor, t, rate): flat direction actor→t, rotate the yaw by at most rate per tick (FUN_08835CF8).

**Who uses it / effect in game:** direct yaw change. It is the same code as 0x040B (out1/D.md) except for the target choice (0x040B: opponent only) and the extra arg1 option, which is shared with 0x0402.

**Data check (170 uses, 30 fighters; 71 in the header slot):**
- Scripts 300/822/3000 (118 uses; motion shared with EX Revenge 3000). There it appears as {180,0,1,0} over 0–2 (snap to face) and {45,0,1,0} over 2–end (keep tracking), e.g. one200 022, ten100 024, eht200 022.
- Dodge script 440 {90,0,0,0} over 0–2 (29 uses, e.g. one100 018, for100 020): the dodge faces the foe unless the foe is itself dodging or guarding.
- A few character moves, e.g. thr100 051 {30,0,1,0} 2–68.

**Confidence:** confirmed. **Test:** on the 440 dodge, set arg0 = 0; the dodge should no longer turn toward the opponent at its start.

---

### 0x0743 — Hit window that fires a projectile on contact (and optionally shows a caption)

**Kind / args:** ranged, 7 args `{atkp id, hit group, magi id, magiArg, showCaption (0/1), captionIndex, captionFlag}`.
- arg0/arg1: as 0x0200 hit_window (e.g. 975/33, 1446/47, 2838/13).
- arg2: a projectile (magi) id, e.g. 1197, 942, 1861–1866, 2269. 2269 exists in Noctis/Bartz's projectiles.json.
- arg3 is always 0.
- arg4 = 1 only on "gst110" (12 uses) with arg5 9–15, arg6 0.

**What the handler does (0x08829B64):** the same as hit_window 0x0200 (0x08829A80), with one addition:
- It returns early if the state mask 0x089C7908 matches.
- `h = FUN_08833B80(actor, arg0, arg1)`: raises the hit-group bits at +0x448/+0x44C and calls vtable+0xB8 → FUN_08833B2C → FUN_0883E108. That finds or creates the 0x220-byte **melee hit object** (vtable 0x089C6D18; init FUN_0880B8E4 sets +0x20C..+0x218 = −1).
- **Then `h+0x20C = arg2`, `h+0x210 = arg3`, and if arg4 == 1: `h+0x214 = arg5`, `h+0x218 = arg6`.**
- Finally `FUN_0882FD08(owner, start, end)`, the same as 0x0200.

**Who uses it / effect in game:** hit object vtable slot +0x30 = **FUN_0880B9C4**. It is called from the per-defender hit path FUN_08808EE4 at 0x08809068, after the defender passed the vtable +0x138/+0x148 checks. With defender d and attacker = h+0x18:
- If `h+0x20C >= 0` and d+0x542 == 0 (a main fighter, not an assist): `attacker.vtable+0x118(attacker, h+0x20C, h+0x210)`. In the fighter vtable that is FUN_08B304C4 → FUN_08AD0978(magi id, attacker, attacker, 0, arg, 100, current action id, attacker+0x10F8). This is the **same call that keyed 0x0204 fire_projectile makes** (keyed[4] = 0x0882D2DC → vtable+0x118 with its two args). So the attacker **fires projectile arg2 the moment this hitbox connects**.
- If `h+0x214 >= 0`, d is a fighter of kind 0 and the attacker is a fighter: `FUN_08856D48(*(0x089D4DC8)+0x69C0, attacker, arg5, arg6)`. That is the call keyed 0x021D makes: the **move-name caption window** (agent L, out/L.md).
- Then the normal hit (FUN_088080C4).

**Data check (143 uses, 13 fighters; 128 in character moves, 14 in EX Bursts 8100):**
- Noctis 042 (scripts 813/821, a Chase strike) uses {1125,5,2269,…} over 56–60. It is the hit that spawns the follow-up projectile 2269.
- eht100 045 (10003) uses {1444,47,940,…}; for200 042 (10000…) uses {1019,13,1192,…}.
- Gilgamesh-type gst110 054/062/066/071 use arg4 = 1 with captions 9–15. These are weapon names, so on contact the drawn weapon's name pops up. This matches L's finding that keyed 0x021D shows indices 12/14 on gst110.

**Confidence:** confirmed (setter → hit object fields → on-hit slot → fire-projectile vtable / caption manager). That the spawned magi is an "extra effect/projectile" rather than damage depends on the magi record. **Test:** in Noctis 042 change arg2 from 2269 to −1; the hit should land but the follow-up projectile should no longer appear. Whiff the move; no projectile should appear either way.

---

### 0x003A — Dodge tail: Brave, HP and EX input allowed (attack-cancel out of a dodge)

**Kind / args:** ranged, 0 args.

**What the handler does (0x0882B774):** `actor+0x195 |= 0x01 | 0x02` and `actor+0x194 |= 0x80`. Its siblings set the bits singly: code 0x36 (0x0882B7A0) only +0x194 bit 0x80; code 0x37 (0x0882B7B4, retail 0x0037) only +0x195 bit 0x01.

**Who uses it / effect in game:** each bit has one getter. Each getter returns 0 when `actor+0x5B4 & 2`:
- **FUN_08B33508** → +0x194 bit 0x80. **"Brave attack allowed"**: in the fighter control code at 0x08B36580–0x08B365F0, when the ○ / Brave input check FUN_08B30E10 passes (and not FUN_08B321F4): if the bit is set, it clears the current action record (FUN_08B308B8: +0x101C/+0x1020/+0x1024 = 0) and starts input class 0 (`FUN_08B2D664(actor, 0)`). Without the bit this requires vtable+0x150 "can start attack".
- **FUN_08B334D0** → +0x195 bit 0x01. **"HP attack allowed"**: at 0x08B364C0–0x08B36540 the same pattern with the parallel input check FUN_08B309C8 and class 2 (`FUN_08B2D664(actor, 2)`).
- **FUN_08B33544** → +0x195 bit 0x02. **"EX input allowed"**: at 0x08B3638C–0x08B363B4, when the EX input check FUN_08B31108 passes (it reads the EX Mode bit +0x5B2&4 and EX data) and the normal gate FUN_08B2FAEC fails, this bit lets it through. It then leads to the EX-module calls (FUN_08ACAAC8/FUN_08ACA64C on actor+0x784), FUN_08B33DDC or `FUN_08B2D664(actor, 4)`.
- The CPU AI snapshot (0x08A83828–0x08A8383C) copies G2 / G1 into its context bytes +0x10C / +0x118, so the CPU also knows when it may attack out of a dodge.

**Data check (128 uses, 29 fighters):** 119 uses are the dodge motions (scripts 400 / 440 / 460) at their tail: 26–56 for 400/440 and 40–56 for 460. Examples: one100 016 (400), two200 018 (440), sev110 019 (460). The other 9 are on character moves from some tick to the end (e.g. eht110 057 82–end). This fits "the last part of a dodge can be cancelled into a Brave or HP attack or EX Mode", as in 012.

**Confidence:** confirmed for the bits and the gating code. Strong for which button each input check is: ○ = Brave per the round-1 note on FUN_08B30E10; the □/HP and EX labels for FUN_08B309C8 and FUN_08B31108 come from the parallel structure and the EX-Mode reads. **Test:** remove 0x003A from the 400 dodge. Pressing ○ or □ during ticks 26–56 of the dodge should no longer start an attack; the press is ignored until the dodge's can-act window.

---

### Notes for the op table (EVENT_OPS.md)
| Name (suggested) | Op | What it does | Confidence |
|---|---|---|---|
| `pin_physics_group` | 0x0216 | All spring bones of physics group arg0 follow the animation, ≤ arg1°/tick | confirmed (mechanism) |
| `exburst_victim_speed` | 0x0126 | EX Burst: opponent's speed × arg0 (0 = frozen) for the range | confirmed |
| `no_body_collision` | 0x0013 | Pass through fighters (no push either way) | confirmed |
| `lunge` | 0x0548 | Constant velocity along the facing, pitch arg0° (+ up), speed arg2/tick; arg3 = only without a target; arg4 = only after the homing dash stops; enters state 0x12 | confirmed (sign strong) |
| `turn_or_swivel_to_target` | 0x0811 | arg2 = 0: turn facing to target (arg1 = away); arg2 = 1: visual body swivel toward target within ±arg3/arg4°, hold arg5, smooth arg6, decay arg7 | confirmed |
| `turn_to_target` | 0x046D | As 0x040B but uses the lock-on target (fallback opponent); arg1 = only while target reels | confirmed |
| `hit_window_fire_on_hit` | 0x0743 | Hit window {atkp, group} that fires magi arg2 on contact; arg4 = 1 shows caption arg5 on contact | confirmed |
| `dodge_attack_cancel` | 0x003A | Brave (+0x194&0x80), HP (+0x195&1) and EX (+0x195&2) input allowed now | confirmed (button mapping strong) |
