# L: keyed ops 0x0000, 0x0311, 0x0312, 0x0123, 0x0117 (keyed + ranged), 0x0925, 0x0121, 0x0131, 0x021D, 0x051B, 0x002B, plus the keyed-walker `x` filter

**Summary**
- **Movement and state ops.** 0x0000 = "enter the movement state the motion's starter armed" (jump take-off, dodge slide, knock-back flight). 0x0123 = "take off: switch to air-action state 0x12". 0x0925 = **ground homing lunge** (state 0xF), not a weapon trail. 0x002B = snap facing relative to the opponent (EX Burst 8600). All are confirmed or strong.
- **Sound, look and presentation ops.** 0x0311 / 0x0312 = sound only if a draw part is visible / only if a given model slot is active. Keyed 0x0117 = eject a gun **cartridge** ("cart" model) from a bone (Squall, Laguna). Ranged 0x0117 = spring-bone rest pose follows the animation. 0x021D = move-name caption window. 0x051B = full-screen colour flash/fade. 0x0121 = set the opponent's time scale (EX Burst freeze/unfreeze). 0x0131 = posts a one-shot battle-manager message 0x1F {value, actor}; which listener uses it is unknown.
- **Keyed walker `x` filter.** x byte 0 = the **phase/variant column of the motion_map row** of the script now playing. It lets a shared motion carry events for only one of its scripts (600 vs 610, 550 vs 500, 3000 vs 300). Bytes 1–3 are further filters: fighter mode / EX Mode, actor+0xD60, actor+0x1079.

---

### Keyed walker `x` filter (FUN_08829104) — what `*(*(owner+0x4C)+4)` is

**Chain:** FUN_0882F17C (motion player tick) → FUN_08828EC0 → FUN_08829104(a0 = keyed list, a1 = `*(*(player+0x4C)+4)` (byte), a2 = mode s1, a3 = s2, t0 = s3, t1 = s4, t2 = handler 0x0882CEC0, t3 = player, f12/f13 = previous/current time).

**`player+0x4C`:** written only at 0x0882F148 in FUN_0882F0D4 (play script):
- `player+0x50 = script`
- `player+0x4C = bsearch(script, table *(player+0x48)+8, count *(+0x48)+4, 8-byte rows)` (FUN_08934944)
- then the motion `lhu 2(v0)` is played.

The 8-byte row is the motion_map record `{u16 script, u16 motion, u32 phase}` (charmod_objx.py `<HHI`). So `*(*(owner+0x4C)+4)` = **low byte of the `phase` column of the motion_map row of the current script**.

**Filter rules per event (all must pass):**
| x byte | rule | values seen |
|---|---|---|
| 0 | 0 = always; else must equal the current script's motion_map phase | 1/2 split 600 vs 610, 550/560 vs 500/510, 901 vs 911; 3 = script 3000 (EX Revenge) in motion 300/822/3000; 3 = script 242 in the dash motion |
| 1 | 0 = always. Otherwise compared with the mode s1 set in FUN_0882F17C: 1 = normal fighter; 2 = fighter in EX Mode (actor+0x5B2 bit 0x04 and FUN_08B3406C false); 6 = actor kind 1 (+0x542==1); 3 = kind 2 with +0x580 bit 0x20. Byte value 4 = EX Mode and +0x5B6 bit 0x40; 5 = EX Mode without it. When s1 == 6, byte 1 or 6 passes. | 0x200 (byte1 = 2) on Squall's 0x0117 = only in EX Mode |
| 2 | 0 = always; else byte−1 == (s8)actor+0xD60 | |
| 3 | 0 = always; else byte−1 == (s8)actor+0x1079. actor+0x1079 = byte 7 of the fighter's setup record (+0x2F8), written at 0x08B3A194. Meaning unknown: costume or voice set? | 0x02000000 / 0x03000000 on paired 0x0303 sounds |

**Data check:**
- In the base objx files, x byte 0 matches a phase of one of the scripts mapped to that motion in 1701 of 2407 keyed uses.
- Most misses (616) are in the `_a` objx files. Their motion_map (22 rows for one100_a) doesn't contain the phases that the timelines' x refers to (x=3 on the dash where `_a` maps only 240/phase 0). So `_a` timelines are probably played through a different (base or merged) script table. Unknown, worth checking before relying on `x` in `_a`.
- **Modder rule:** to have an event fire only when a shared motion is played as script S, set x byte 0 to S's motion_map phase (S's phase must be non-zero and unique within that motion).

**Confidence:** **confirmed** for byte 0 (traced from the store to the compare). Bytes 1–2 are **strong** for the mechanism; the meaning of +0xD60 and +0x1079 is unknown.

---

### 0x0000 — Enter the armed movement state (take-off / dodge slide / launch)

**Kind / args:** keyed, 0 args. Handler: keyed[0x00] = 0x0882CF1C.

**What the handler does:**
- Exits if `actor+0x314` (the movement controller) is null.
- Reads `mode = *(ctrl+0xF4)`. If mode < 7, it jumps through 0x089C7BE8 and calls `FUN_08834C10(actor, state, 0)` (switch state; the state object comes from FUN_08A5D2B8, the table at *(0x089D85F0)+0x18C):

| mode | state | who arms it (writes ctrl+0xF4) |
|---|---|---|
| 0 | 1 (free fall / ballistic air) | default. Written 0 by the launch/jump setup FUN_08A5BE70 (sets vertical speed from a height, 0x08A5BF4C; called by FUN_08A5BF84 and many wrappers), by FUN_08A5C404, by FUN_08A5E2FC (plays script 202) and by FUN_08A6158C (blow-away 700/750) |
| 1 | 6 | FUN_08A61AC8 (0x08A61AEC), which plays script 410 (**dodge**) |
| 2 | 4 | FUN_08A58A5C: FUN_08A5BF84 (launch with a velocity vector) then mode 2. Called from FUN_08831F04/08832034/0883241C/08834274 and from FUN_08A46208/08A4B234 |
| 3 | 0xC | no writer found |
| 4 | 0xD | no writer found |
| 5 | 0xE | FUN_08A5CB50 (0x08A5CC58): stores a destination point at ctrl+0xD0 and plays script 220. Called from FUN_08ACBD58 (manager code) |
| 6 | 0xF (homing) | FUN_08A60BC8 when bit 4 is clear (keyed 0x0D10) |
| 7 | — (no-op) | FUN_08A527CC (0x08A52898) |

- The only reader of ctrl+0xF4 in the EBOOT and overlay is this handler (0x0882CF2C).

**Who uses it / effect in game:** the motion's starter sets up velocity and parameters and arms a mode, but the actor stays in its current state (a crouch/wind-up on the ground) until the timeline reaches this event. At that frame the actor switches into the matching movement state.

**Data check (304 uses, all 59 objx):**
- Script 100 (jump) @4, 29×. The 4-tick jump squat, then take-off. ranged 0x0015 ground-follow covers 0–4 and can_act starts at 8 (one200 motion 2).
- Script 410 (dodge) @4, 29×, with invincible 0–20.
- Script 220 @16, 29×: ground-follow 0–16, then a body hit (atkp 427) from 20 to the end.
- Hit reactions @0 (500/510/550/560, 501/…, 901/911) and 601 @10. Several carry x=1/2, so they fire only for one variant of a shared motion (600 not 610; 911 not 901; 550/560 not 500/510).
- Noctis-like p_one200 048 (script 10007) @4 with x=16 sits next to a 0x0D10.

**Confidence:** **confirmed** for the mechanism (pending mode → state switch; the sole reader). The state names for 4, 6 and 0xE are **strong/weak** (inferred from who arms them).

**In-game test:** remove the @4 event from the jump motion (script 100). The fighter should crouch and never leave the ground (or leave only when the motion ends).

---

### 0x0311 — Sound, only if a draw part is visible

**Kind / args:** keyed, 3 args `{part, sound id, positional}`. Handler: keyed[0x11] = 0x0882DC70 → `FUN_088384A0(actor, arg0, arg1, arg2!=0)`.

**What the handler does:**
- `FUN_0886CC38(*(actor+0x394), arg0)` scans the draw-part visibility table (model+0x10C, 3-byte rows `{bone, shown, alpha}`, count at +0x8E) for part arg0. It returns its `shown` flag, or 1 if the part isn't listed.
- Only if that is 1 does it play the sound: `FUN_088A24D4(1, arg1 + FUN_08842828(setup), 1, 0)`, the same sound-bank base as 0x0303.
- If arg2 != 0, it uses a world position from FUN_08838D14. Otherwise it is attached to actor+0x510.
- Unlike 0x0303 (FUN_0883834C), it does not check the actor+0x388/0x38C mute mask.

**Who uses it / effect:** a per-part (per-weapon/costume) sound choice.

**Data check (193 uses, only Cloud sev100 and Squall eht100):**
- Cloud uses parts 51/52 in pairs at the same frame with different ids: sev100 050 @20 `{51, 2200001}` + `{52, 2200004}`, @40 `{51, 2200002}` + `{52, 2200005}`. Parts 51/52 are also toggled by 0x011C in his motions 066/067.
- Squall uses parts 48/49: eht100 048 @14 `{48, 2400039}` + `{49, 2400043}`.
- So the swing/gunblade sound follows whichever weapon mesh is drawn (alt costume weapons). arg2 is always 0.

**Confidence:** **confirmed** (code), and the data fits.

**In-game test:** hide part 51 on Cloud for a whole move. Only the 52-variant sounds should play.

---

### 0x0312 — Sound, only if a given model slot (form) is active

**Kind / args:** keyed, 3 args `{model slot, sound id, positional}`. Handler: keyed[0x12] = 0x0882DC98 → `FUN_088385B0(actor, arg0, arg1, arg2!=0)`.

**What the handler does:** `FUN_0886B1DC(*(actor+0x394))` returns the index (0..count at +0x8C) of the model slot whose GMO equals the current one, `*(*(model+0x84)+8)`. It returns −1 if none. The sound plays only if that index == arg0; the play path is the same as 0x0311.
- The current slot is switched by FUN_0886A54C from the pending byte +0x8D.
- +0x8D is set by FUN_0886B18C, called from FUN_08A2B5B4 / FUN_08A2B98C (character moves with scripts 10051/10052/10070) and FUN_08ACAC60 / FUN_08ACF2A0 (EX-Mode code), i.e. form changes.

**Data check (165 uses):**
- Cecil for100 uses slots 0 and 1 (Dark Knight / Paladin forms).
- Terra six100, Cloud of Darkness thr200 and Squall eht100 use slot 0 only (normal form, not EX/trance), e.g. six100 048 (10003) @36/@62 `{0, 2000044}`.
- Also on basic scripts: 102 landing, 300, 400s, 810/820.

**Confidence:** **confirmed** for the mechanism. Which slot is which form is **strong**.

**In-game test:** Cecil, same move in each form. Slot-1 sounds play only as a Paladin (or Dark Knight, whichever is slot 1).

---

### 0x0123 — Take off: switch to the air-action state (0x12)

**Kind / args:** keyed, 1 arg `{only_if_no_target}`. Handler: keyed[0x23] = 0x0882E214.

**What the handler does:**
- If arg0 != 0 and the actor has a target (vtable +0x120 = FUN_08836D8C → actor+0x2FC), it does nothing.
- Otherwise, if the current state (`*(actor+0x3A4)` → +0) isn't 0x12, it calls `FUN_08834C10(actor, 0x12, 0)`.

**Who uses it / effect:** state 0x12 = air action (see move-system: airborne actions run in 0x12; landing in 0x12 runs vtable +0xC0 → landing motion, stand). The move continues as an airborne action with air physics and landing detection.

**Data check (169 uses, all 59 objx):**
- It sits exactly where ground-follow 0x0015 ends and air-motion 0x000A begins:
  - Tidus ten100 047 (10010) @22: 0x0015 0–22, 0x000A 22–100, hit 22–34.
  - Zidane nin100 050 (10050) @28: 0x0015 0–28, 0x000A 28–100.
  - for110 043 (10000) @20: 0x0015 0–20, 0x000A 20–94.
- Motion 22 (scripts 300/822/3000) @0 with x=3, so it fires only for script 3000 (EX Revenge): the fighter goes into the air state for EX Revenge.
- arg0=1 only on Jecht ten200 060 / ten200_a 017 (10081–10088).

**Confidence:** **confirmed**.

**In-game test:** remove it from Tidus 047. He should stay in a ground state: no landing transition, and he may snap to the floor.

---

### 0x0117 (keyed) — Eject a gun cartridge from a bone

**Kind / args:** keyed, 1 arg `{bone}` (GMO bone index). Handler: keyed[0x17] = 0x0882DD8C → `FUN_088525F8(effMgr+0x6B00, actor, arg0, (s16)setup+2)`, where effMgr = *(0x089D4DC8).

**What the handler does:**
- It finds the manager slot (2 slots) registered for this character id. Slots are registered by FUN_08852378 from FUN_088459E0 (0x088464A8), which loads the resource named **"cart"** (string 0x089C89A8) for characters that have one.
- Skips if setup byte 8 bit 0 is set.
- It takes bone arg0's world matrix (FUN_089B0E78 / FUN_089B1224) and adds random offsets and a random direction (LCG at 0x09AFB89C).
- It writes an entry into a 10-entry ring (80-byte entries, index at +0x330/+0x334) with a lifetime of 300.0 ticks (0x43960000).
- Updated and drawn by FUN_08852310 / the effMgr tick.

**Who uses it / effect:** a spent cartridge flies out of the gun and lies around for 5 s.

**Data check (120 keyed uses):**
- Laguna eht110 / eht110_a only `[54]` (machine gun), at several frames per burst, e.g. 047 (10004) @8/@20/@32.
- Squall eht100 / eht100_a `[46]` with x=0x200, so only in EX Mode (x byte 1 = 2), e.g. eht100 053 (10030) @12 with hit 12–18 and 057 (10040) @12. That fits Squall's gunblade trigger in EX Mode.

**Confidence:** **confirmed** (the resource name "cart" and the ring buffer).

**In-game test:** Laguna's machine gun with the events removed. No casings on the floor.

### 0x0117 (ranged) — Spring-bone group rests on the animated pose

**Kind / args:** ranged, 1 arg `{physics group}`. Handler: ranged[0x17] = 0x0882ABAC.

**What the handler does:**
- Fighters only (FUN_0883739C true, or kind 2/4): `obj = *(actor+0x398)` (spring-bone set).
- For every 0x170-byte entry whose `*(e+0x158)` record's **s16 at +6** == arg0, it sets `e+0x151 = 1` every tick. Note that 0x0227 matches the bone at +0; this op matches the field at +6, likely a chain/group id.

**Who uses it / effect:** physics update FUN_0880EA20.
- At 0x0880EEE8, if e+0x151 is set, the spring's reference orientation for this tick is built from the bone's current animated local matrix (FUN_089B11B4 → FUN_089A4C80 / FUN_089A4DE4, stored to e+0x90). Then it clears +0x151 (0x0880EF34).
- Otherwise it uses the stored rest pose e+0x50.
- So the spring still simulates, but it swings around the keyframed pose rather than the bind pose. Compare 0x0227, which snaps or pins the bone onto the animation.

**Data check (10 ranged uses):** whole-motion ranges 0–65535 only.
- Firion two100 and Bartz fiv100 stand motion 000 (scripts 1/10/1120): group 0 (cape/hair).
- Sephiroth sev200 059–061 (10060–10062): group 3, together with 0x0227 `{5,30}`.

**Confidence:** **strong** (the flag path is traced; the visual result is inferred).

**In-game test:** remove it from Firion's stand motion. His cape should hang in its bind pose instead of following the idle animation.

---

### 0x0925 — Ground homing lunge toward the target (state 0xF, ground variant)

Older tools called this `weapon_trail`; that is wrong. The real weapon trail is 0x042A with record 9999.

**Kind / args:** keyed, 9 args. Handler: keyed[0x25] = 0x0882D9E8.

| arg | meaning (FUN_08A60BC8 field) | seen |
|---|---|---|
| 0 | horizontal speed, m/tick (J+0x14) | 0.02–2.0, mostly 0.2–0.6 |
| 1 | tracking ticks (J+0x1C) | 4–40, mostly 6–16 |
| 2 | angle around target, degrees (×π/180 → J+0x24) | always 0 |
| 3 | stand-off distance from target, m (J+0x28) | −4.5–3.0, mostly 1.0–2.0 |
| 4 | arrive radius (J+0x2C), −1 = off | −1 (104), 0 (6) |
| 5 | max duration ticks (J+0x30) | usually = arg1 |
| 6 | J+0x34 \|= 0x40: ignore target reactions (FUN_08A60E7C) | 0/1 |
| 7 | J+0x34 \|= 0x80: stop when target busy (FUN_08A60E94) | 0/1 |
| 8 | alternate target (a3) | 0 |

**What the handler does:**
- Exits if there is no target (vtable +0x120).
- `FUN_08A60BC8(actor, a1=1, a2=0, a3=arg8, speed=arg0, vspeed=0, track=arg1, height=0, angle=rad(arg2), dist=arg3, arrive=arg4, maxdur=arg5)`, then `FUN_08834C10(actor, 0xF, 0)` (always, not only when airborne).
- Because a1=1 (J+0x34 bit 4), the pending mode isn't armed and actor+0x384 bit 0x1000 isn't set.
- Vertical speed 0 means the movement is horizontal only, and the state exits to state 0 (ground), not 0x12 (see out1/F.md, state 0xF).

**Who uses it / effect:** state 0xF per tick (FUN_08A60EAC / FUN_08A60838) slides the fighter along the ground toward a point `dist` metres short of the target, re-aiming each tick for `track` ticks.

**Data check (110 uses, 8 characters):** always inside a whole-motion ground-follow 0x0015, starting at or just before the move and ending at the first hit.
- Cecil for100 045 (10000) @0 `{0.6, 12, 0, 2.5, …}`, hit 12–18.
- Cloud sev100 044 (10007) @10 `{0.45, 12, 0, 1.5, -1, 10}`, hit 10–16.
- Gabranth gst100 043 (10000) @16 `{0.75, 8, 0, 0.5}`, hit 16–24.
- for110 043 @0 `{0.22, 20, 0, 1.0, -1, 20}`, ground 0–20, then 0x0123 take-off at 20.
- Several use x to pick per-variant parameters (for200 x=2/3/4, fiv210 x=0x200 = EX Mode only).

**Confidence:** **confirmed** (handler → the same homing setup as 0x0D10; data are all ground lunges).

**In-game test:** set arg0 to 0 on Cecil 045. He should attack in place without closing in.

---

### 0x0121 — Set the opponent's time scale (freeze / unfreeze during EX Burst)

**Kind / args:** keyed, 1 arg `{percent}`. Handler: keyed[0x21] = 0x0882DE70.

**What the handler does:**
- `opp = vtable+0x130` (FUN_08836D9C → actor+0x300, the 1v1 opponent). It is called twice; the first result is discarded.
- `FUN_08834DF4(opp, arg0/100)`: opp+0x3B0 = scale, opp+0x3B4 (ramp) = 0, opp+0x370 = scale.

**Who uses it / effect:**
- actor+0x370 multiplies the actor's per-tick motion step (e.g. 0x0883A074).
- +0x3B0 is returned by the motion-player speed getter FUN_08814780 (vtable 0x089C7004).
- The per-tick updater FUN_08833404 (0x0883346C–0x0883351C) only moves +0x3B0 toward 1.0 when the ramp +0x3B4 != 0. With ramp 0 the value holds until another op changes it.
- Sibling ops: keyed 0x1F (FUN_08834D60, ramp back to 1 over arg0 ticks) and 0x20 (FUN_08834D30, ramp down to arg1/100 over arg0 ticks).

**Data check (33 uses, all EX Burst scripts 8101–8600):**
- `[0]` at frame 0 of the EX Burst start/minigame motions freezes the foe: sev100 070 (8101), for100 070 (8101), two200 058 (8101), sev200 073 (8500).
- `[100]` at frame 0 of the finisher (8501–8503) or near its end unfreezes: thr200 066 (8501–8503) @0, ten110 077 @180.

**Confidence:** **confirmed** for the field writes. That +0x3B0/+0x370 is the opponent's playback speed is **strong**.

**In-game test:** change the 8101 `[0]` to `[50]`. The foe should move in slow motion during the EX Burst input phase.

---

### 0x0131 — Post a battle-manager message 0x1F `{arg0, actor}` (value per move variant)

**Kind / args:** keyed, 1 arg `{value}`. Handler: keyed[0x31] = 0x0882E61C. Fighters only: `actor+0x6DC = arg0`.

**Who uses it:**
- The only reader is FUN_08AFD424 (battle-manager update, 0x08AFD5D8). For each of the 2 fighters, if `+0x6DC != 0` it calls `FUN_08AFD170(mgr, 0x1F, &{value, actor})` and clears +0x6DC (0x08AFD5FC). It is also cleared at fighter init (0x08B3A48C).
- FUN_08AFD170 broadcasts the message to the listener list at mgr+0xD0 (vtable +8 of each listener). The manager's own switch (table 0x08AFDDC8) ignores 0x1F.
- Which listener acts on 0x1F was not found: it could be a HUD element, an announcer or a counter.
- So this is a one-shot notification with a number, processed on the next manager tick. Value 0 = nothing.

**Data check (24 uses):**
- gst130 (18) and gst130_a (5): all @4, in motions shared by 10–15 variant scripts (10021–10035, 10051–10081). x byte 0 picks the value for each variant (1–6, 23, 60), e.g. gst130 048 @4 `[1]` x=8, `[3]` x=6, `[5]` x=7.
- sev120_a: 1 use.
- It looks like "tell the game which variant of the move this is" (e.g. for a HUD/command display). This is a guess.

**Confidence:** **strong** for the mechanism (write → post message 0x1F → clear). The effect is **unknown**.

**What would settle it:** find the listener whose vtable +8 handles 0x1F, or in game change gst130 048's `[1]` to `[0]` and watch the HUD.

---

### 0x021D — Show the move-name caption (window) at the top of the screen

**Kind / args:** keyed, 2 args `{caption index, flag}`. Handler: keyed[0x1D] = 0x0882DD24 (fighters only) → `FUN_08856D48(effMgr+0x69C0, actor, arg0, arg1)` → `FUN_08856D70(mgr, actor, pack = *(*(actor+0x390)+0xCC), arg0, arg1)`.

**What the handler does:**
- At most 6 per frame (mgr+0x134). Skips if arg0 == −1 or the pack is null.
- The entry stores the actor and FUN_0887AD78(pack, 0, arg0, …), a 2D layout item: image/text arg0 of the character's pack.
- It also stores setup byte 9, age 0, lifetime 90.0 ticks (0x089C8D68), flag byte = arg1, arg0 and the pack.
- It places the item at screen (240, 24), the top centre of 480×272 (0x08856ED0–0x08856EE8), with colour 0xFF808080.
- The manager is initialised with the "wind" (window) resource, string 0x089C89B8 (FUN_08856ACC at 0x0884659C).
- The same call is made for spawned objects (FUN_0880B9C4, args from obj+0x214/+0x218).

**Data check (22 uses, all EX Bursts or name-showing moves):**
- Index 1 at frame 0 of 8500/8102 (EX Burst name).
- Index 2 on the finish variants 8501–8503 (two200 063, fiv200 071, six200 056).
- Onion Knight thr100 8103/8104/8500/8501 → 13/12/15/14 (branch names).
- Gilgamesh gst110 074 (10119…) @28 → 12/14 with x = 4/5/10/11 (the weapon he drew, picked per script variant).
- arg1 always 0.

**Confidence:** **strong** (position, lifetime and resource are from code; that the image is the move name comes from usage).

**In-game test:** change Gilgamesh's 12 to another index. A different caption should appear at the top of the screen.

---

### 0x051B — Full-screen colour flash / fade

**Kind / args:** keyed, 5 args `{fade-in ticks, hold ticks, fade-out ticks, colour, unused}`. Handler: keyed[0x1B] = 0x0882D05C.

**What the handler does:**
- `f = FUN_08873CE0(0, 1, 0)` gets screen fader #0 (pool at 0x09B47B50, 0x240 bytes each; FUN_0887391C resets its keys).
- Colour by arg3: 0 = white (255,255,255,255); 1 = black (0,0,0,255); 2 = red (255,0,0,255).
- It adds keys with FUN_08873B64(f, colour, time) (up to 16 keys at +0x20, count at +0x220):
  - if arg0 > 0: transparent at t=0, then colour at t=arg0;
  - if arg1 > 0: colour at arg0 (or 0) and at arg0+arg1;
  - if arg2 > 0: the end colour from 0x09AF84C0 (BSS, presumably transparent) at arg0+arg1+arg2.
- arg4 is never read.

**Data check (14 uses, all EX Burst):**
- `{0,0,N}` = start white and fade out over N ticks: sev200 072 (8102/8501–8503) N=42; two200 059 (8102) N=120; fiv200 071 N=42.
- `{20,0,8}` = fade in to white over 20 ticks, then out over 8 (gst120 088–090 @160).
- `{0,4,0}` = hold white for 4 ticks (two200 061/065).

**Confidence:** **confirmed** for the key building. That the end colour is transparent is **strong**.

**In-game test:** set arg3 = 2 on two200 059. The flash should be red.

---

### 0x002B — Snap facing to the line from the opponent (EX Burst end)

**Kind / args:** keyed, 0 args. Handler: keyed[0x2B] = 0x0882DECC.

**What the handler does:**
- `opp = vtable+0x130` (actor+0x300).
- `d = actor+0x80 − opp+0x80` (vsub at FUN_089A38E0).
- VFPU normalize.
- `FUN_08835BA4(actor, d)`: yaw = atan2(d.x, d.z) (FUN_089A457C) → FUN_08835C54 → actor+0x320.

**Who uses it / effect:** an instant yaw set. FUN_08A5065C uses the same call with `camera − self` to face the camera. By the same convention, `self − opponent` means the fighter turns its **back** to the opponent. That is an inference: the sign convention isn't proven.

**Data check (13 uses):** only script 8600 (the last EX Burst motion, after 8500), always @0, in 13 fighters (one100 077, fiv100 079, eht100 078 …). These are air motions (0x000A over the whole motion).

**Confidence:** **strong** that it snaps the yaw along the opponent axis. Facing **away** is **weak**.

**In-game test:** watch the last frame after an EX Burst. If the fighter always ends with its back to the foe, the convention is confirmed.
