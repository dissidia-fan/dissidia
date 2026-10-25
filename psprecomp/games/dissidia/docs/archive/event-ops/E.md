# E: keyed ops 0x0315, 0x060F, 0x0335, 0x0108

**Summary**
- 0x0315 = **play a character voice line** (voice id, plus two variant filters). It goes through a per-actor voice channel at actor+0xCF0 with priority and anti-repeat checks. Confirmed.
- 0x060F = **footstep / landing / body-impact sound**, picked by the stage surface under the actor, plus a queued **ground effect** (dust etc.) at a foot or body bone. Confirmed for the sound and queue; the exact look of the ground effect is unconfirmed. 0x0335 = **play the summoner's FEPA effect on a summon** (Yuna's aeons), attached to the summon's bone, with a lifetime override. Strong.
- 0x0108 = **forced hit**: applies atkp id arg0 straight to the actor's opponent with no hit volume (used for EX Burst LIMIT_ATK hits). Confirmed. It is not an effect or sound id.

Conventions: keyed handler `FUN_0882CEC0`, s0 = event, s1 = actor; args start at event+8. Jump table 0x089C7B08 was checked for the entries 0x08→0x0882D5F0, 0x0F→0x0882D6D0, 0x15→0x0882DCE8, 0x35→0x0882E188. "Kind" means the byte actor+0x542, written by the actor constructor `FUN_0883975C` (0x08839A2C) from its a1. `FUN_0883739C(actor)` returns true when kind is 0 or 1, i.e. fighter objects built by `FUN_08B39E5C` (kind 0 from battle setup `FUN_0888BA0C`, kind 1 from `FUN_08A61C3C`). Kind 3 objects are summons (`FUN_08850BE0`, spawned from `FUN_0884FA84`, owner stored at +0x5D4 by `FUN_08851058`). Kind 2 objects come from `FUN_08805C6C`; kind 7 is unknown.

---

### 0x0315 — Voice line (character voice)

**Kind / args:** keyed, 3 args `{voiceId, onlyIfAltOff, onlyIfAltOn}`.
- arg0 = voice id, without the variant offset. Ids follow the pattern `CC 1 0 nnn`: Bartz 1810xxx, Shantotto 3010xxx, Yuna 21410xxx / 2410003, and one 21610010 in Noctis. The SE ids used by the `sound` op (0x0303) are `CC 0 0 nnn` (e.g. 1800052).
- arg1 / arg2 are flags (any nonzero value counts as 1). All 274 retail uses are `0, 0`, which means always play.

**What the handler does** (0x0882DCE8):
- If `FUN_0883739C(actor)` is true (fighters only), it calls `FUN_08B33F8C(actor, arg0, arg1!=0, arg2!=0)`. Other object kinds do nothing.

`FUN_08B33F8C` (0x08B33F8C):
- b = byte actor+0x5B8. If `b & 0x80`, it returns and nothing plays. This is a voice mute: `FUN_08A30A70` sets the bit at 0x08A30AF8 around a forced motion start and clears it right after.
- alt = `b & 0x20`. This bit is set at battle setup in `FUN_0888BA0C` (0x0888BD34) from a per-fighter setup flag. Meaning unknown; guess: alternate voice set or costume.
- Variant filter:
  - When alt is set, the line plays if arg2 is set or arg1 is clear.
  - When alt is clear, the line plays if arg1 is set or arg2 is clear.
  - In other words, arg1 = "only without alt", arg2 = "only with alt", and both or neither = always.
- id += `FUN_08842828(*(actor+0x2F8), 1, alt)`. That returns +10000 if `chardata+8 & 1`, +30000 if alt, and +50000 if `chardata+8 & 8`. The +10000 and +30000 apply only to the voice kind (a1=1). The sound op calls it with a1=0, so SE ids only ever get the +50000.
- It then calls `FUN_0884EA08(actor+0xCF0, id, 1)`.

`FUN_0884EA08` (0x0884EA08) is a single voice slot per actor:
- It looks up the id in a sorted global table of 16-byte records with `FUN_088A4364` (binary search over the tables at 0x089D6D4C / 0x089D6D50). Record fields: +8 priority, +0xA minimum re-trigger time, +0xC volume, +0xE flags.
- A line with lower priority than the one playing is dropped. At equal priority, the same line inside its repeat interval is dropped.
- Otherwise it stops the current voice (`FUN_088A1E44`) unless that voice's record has flag 1, then starts the new one with `FUN_088A24D4(1, id, 2, 0)`.
- `FUN_088A24D4` is the same sound-request function that the `sound` op path uses (`FUN_0883834C` → `FUN_088A24D4(1, id, 1, 0)`). Its third argument is the category: 1 for SE, 2 for this voice slot.
- It then sets the handle's volume to the record's +0xC (handle+0x3C) and to the global setting `*(0x089D69E0)+0x18` (handle+0x38). That global is probably the voice-volume option (guess).

**Who uses it / effect in game:** the actor plays a voice clip. One voice per character at a time, with priority, so a newer line cuts off a weaker one. The engine adds the language/costume-style variant offsets.

**Data check:**
- Attack starts: Bartz/Noctis 046 (hits 18–27…), event at f18 = first hit; 047 f13 = first hit; 052 f8 = first hit.
- Non-attack motions at f0: Vivi/Shantotto dash (scr 100, 3010033), guard, idle chatter (m000 f238). The same id 3010039 appears in every damage-reaction motion (scr 500–611), which fits a pain grunt.
- It is a voice for every action, not a "game-wide effect".
- `x` values 1–6 are seen on duplicated events (e.g. Vivi m022 has the same id twice, with x = 1 and x = 2). The handler does not read x; how the walker uses it is unknown.

**Confidence:** confirmed (handler → voice channel → sound request, with id ranges matching voice banks). In-game test: change arg0 in one attack to another `CC10nnn` id and listen. Setting arg2 = 1 should silence the line for fighters without the 0x5B8 bit 0x20.

---

### 0x060F — Footstep / landing sound by surface, plus ground dust at a foot

**Kind / args:** keyed, 6 args `{kind, rayProbe, volProbe, footA, footB, fixedPos}`. All except kind are booleans (`!=0`).
- arg0 kind 0–6: a column in the stage's surface-sound table. Seen in data: 0 step, 1 landing, 2 body impact / jump, 3 second impact (guesses from use; see below). Values 4–6 are not seen in this data.
- arg1 / arg2: how the surface under the actor is probed.
- arg3 / arg4: which foot/body bone gets the ground effect.
- arg5: sound placed at a fixed position (1) or following the actor (0).

**What the handler does** (0x0882D6D0):
1. If kind(actor) == 7, it returns.
2. `s3 = FUN_0883DA88(actor, arg0, arg1!=0, arg2!=0, arg5!=0)`, which plays the surface sound and returns the material id.
3. Target queue: if fighter, actor+0xD08; if kind 2, actor+0xB1C; otherwise stop (the sound has already played).
4. Switch on arg0, using table 0x089C7C08 (7 entries):
   - **0 and 6**: if arg3, `FUN_08853E24(queue, material, bone = *(*(actor+0x394)+0xA4+0x54), arg2, scale 0.1, 1.0, flag 1)`. If arg4, the same with bone `+0xA4+0x50`.
   - **1, 2, 3**: one entry at bone `+0xA4+0x4C`, scale `FUN_08837014(actor)*0.7`. `FUN_08837014` = `*(actor+0x3A0)+0x18`, probably the body collision radius (guess). In this branch arg3/arg4 are not checked.
   - **4, 5**: sound only.
   - The three values at `*(actor+0x394)+0xA4+{0x4C,0x50,0x54}` are bone indices. `FUN_08838D14` passes +0x4C to the bone-matrix getter `FUN_088369A4`, with -1 = none. So +0x4C is the body centre and +0x50 / +0x54 are the two feet (which foot is which is a guess).

`FUN_0883DA88` (0x0883DA88), the surface sound:
- Returns 0 without playing if `actor+0x388/0x38C & mask(0x089C7FE0/E4)`. The `sound` op `FUN_0883834C` uses the same gate. It also returns 0 if global `*(0x089D6AC8)+0x4D0 != -1`.
- Footwear class s3 comes from the struct at `*(actor+0x39C)`, field +7 / +8 / +2. +8 is used when EX Mode (+0x5B2 bit 4) is on. Values are 0–3.
- Probe origin is the actor position (+0x80), or a point from its collision body at +0x3A0. The surface is probed through the stage collision:
  - arg1: line probe `FUN_0889BEB0` (type 2) from y+0.5 along the global vector at 0x09AF8B10 (filled at runtime; presumably down).
  - arg2: volume probe `FUN_088978C4` (type 4), radius 1.5 × body radius.
  - Neither: try the line probe, then the volume probe.
- Material = `(hit+0x3C >> 16) & 0x1F`.
- Sound: `FUN_08896B40(stageSnd, material, kind, footwear)` reads `id = *(tbl+4 → [material*0x90 + kind*0x10 + footwear*4 + 0x20])`. That is per material 7 kinds × 4 footwear classes. It plays with `FUN_088A24D4(1, id, 1, 0)`, the same SE path as the `sound` op.
- If arg5, the sound position is set once to bone +0x4C (or the actor position). Otherwise handle+0x54 = actor+0x510, so the sound follows the actor.

`FUN_08853E24` queue entry (0x18 bytes, at most 2 per tick, count at +0x30):
- `{+0 material, +4 u16 bone, +6 u8 1, +7 u8 arg2, +0xC 1.0, +0x10 scale, +0x14 1.0}`.

Consumer: `FUN_0885436C(actor+0xD08, actor)`, called every tick from the fighter update `FUN_08B2D524` (0x08B2D5AC).
- For each entry it takes the bone matrix. If +7 == 0 it snaps the point to the ground with a probe (`FUN_088976E4`, type 0x22).
- It then spawns ground effects through the global effect pools at `*(0x089D4DC8)+0x160/+0x174/+0x188/+0x7180` (`FUN_0885AD38`, `FUN_08856374`, `FUN_0885BA18`, `FUN_08853D3C`, `FUN_088542D8`).
- Some materials have special cases: 0x1E/0x1F (likely water) and 6/0x12/0xD → `FUN_0884AD14`. Exact visuals unknown (dust/splash/footprint guessed).
- It clears the queue after processing.

**Who uses it / effect in game:** footstep, landing and impact sounds that change with the stage floor material and the character's footwear (EX Mode can change the footwear class). Each comes with a small ground effect at the stepping foot.

**Data check** (150 uses):
- Walk/run steps alternate foot flags: Shantotto m001 f17 `{0,1,0,0,1,1}`, f37 `{0,1,0,1,0,1}`; Bartz m016 (scr 400) at f20 / f46 with alternating foot flags. Many attacks put steps between hits (Vivi m043/m044).
- Kind 1 at f2–3 of scr 102: the landing motion, played after fall scr 101.
- Kind 2 at f2 and kind 3 at f42–46 of scr 702/752 (knock-down bounces). Then kind 0 steps and, for Yuna, kind 1 at f64–66 while recovering. Kind 2 with probe arg2 = 1 at f4 of the airborne variants 703/753.
- This fits "footstep / landing / impact sound", not "effect end".

**Confidence:**
- Confirmed: sound selection and playback, plus the queue to the ground-effect processor.
- Strong: arg meanings (probe modes, foot bones, fixed position).
- Weak: names for kinds 2/3 and what the ground effects look like.
- In-game test: put `{0,1,0,1,0,1}` on a motion and compare on grass vs stone stages. Then try `{0,1,0,0,0,1}` (no dust) and `{4,1,0,0,0,1}` (a different sound column).

---

### 0x0335 — Summon: play the summoner's effect attached to the summon

**Kind / args:** keyed, 3 args `{fepaId, lifetime, bone}`.
- arg0 = FEPA record id in the **owner's** (summoner's) effect bank. Yuna's aeon files use one constant per aeon: w_ysh102 = 6, w_ysh101 = 7, w_ysh103 = 8, w_ysh104 = 9, w_ysh105 = 10.
- arg1 = lifetime override in ticks. 0 = no timer (the effect runs its own course). Seen: 30–40, only on scr 8101.
- arg2 = bone index on the summon to attach to. Always 0 (root).

**What the handler does** (0x0882E188):
- Only runs when kind(actor) == 3 (summon) and the owner's effect bank `*(*(*(actor+0x5D4)+0x390)+0x34+0x28)` is not null.
- `fx = FUN_088A9DE8(ownerBank, arg0, actor+0x4C8, 0, 0)`. This is the same effect spawner as keyed 0x19 (the keyed `effect`, 0x0882DFE4), which uses the actor's **own** bank `*(actor+0x390)+0x5C` and actor+0x4C8 as emitter.
- If fx: `fx+0xE4 = (float)arg1`, then `FUN_088A6A38(fx, 0, actor+0x4C8, arg2, -1.0, -1)`. That sets fx+0xDC |= 1 (attached), +0xF0 = 0 (mode: bone), +0xF4 = arg2 (bone), +0xF8 = -1, +0xFC = -1.0 and +0xD0 = actor+0x4C8 (follow target).
- fx+0xE4 is the remaining lifetime. In `FUN_088A8B00` (0x088A8F9C) it decreases by dt each tick, and when it reaches 0 the effect ends via `FUN_088A6AA4`. A value ≤ 0 means no timer. The default comes from the record's +0x30 (`FUN_088A68FC`, 0x088A6928).
- Bone attachment is read in `FUN_088A7770` and `FUN_088A82D8`: when mode +0xF0 == 0, the effect follows bone +0xF4 of the target object's skeleton.

**Who uses it / effect in game:** a summoned aeon shows one of its summoner's effects (Yuna's FEPA bank), stuck to the aeon's root bone. Most likely the aeon's appear/aura effect (guess; the FEPA records were not decoded).

**Data check** (40 uses, all in Yuna's `w_ysh10x` aeon files):
- On f0 of nearly every aeon action motion (w_ysh101 m000/m003/m004/m006/m007; w_ysh105 m000/m002/m004/m006/m008/m009).
- On scr 8101 it comes later, with a lifetime: w_ysh101 m005 f24 `{7,36,0}` (hit 42); w_ysh105 m007 f60 `{10,38,0}` (hit 66).
- It never appears in a fighter's own p_xxx file, which fits the kind-3 gate. A modder must use it only in summon timelines; in a fighter timeline it does nothing.

**Confidence:**
- Strong: summon-only effect spawn from the owner's bank, with lifetime and bone attach.
- Weak: what effect ids 6–10 look like.
- In-game test: change arg0 in w_ysh101 m000 from 7 to 6 and see which aeon effect plays. Set arg1 to 10 and see it cut off early.

---

### 0x0108 — Forced hit on the opponent (atkp id, no hit volume)

**Kind / args:** keyed, 1 arg `{atkpId}`. It is an atkp (hit profile) id: the "0x6b3…" values are 0x06B3 = 1715 = `LIMIT_ATK02`.

**What the handler does** (0x0882D5F0):
- `tgt = vtable+0x120(actor)`. For fighters (vtables 0x089C7C78 and 0x08B3A9A8) this is `FUN_08836D8C`, which returns actor+0x2FC.
- If null, it uses `vtable+0x130` → actor+0x300.
- For summons (vtable 0x089C8AD8: `FUN_088513CC` / `FUN_088513FC`) it forwards to the owner's +0x2FC / +0x300.
- These are the opponent pointers, set together at battle setup (`FUN_0890F668` 0x089101C8..0x089101FC, `FUN_08919A14`). That they mean "opponent" is strong but not proven.
- If tgt: `FUN_08836DAC(actor, tgt, &pos)` (contact point between the two bodies), then `FUN_08808468(actor, arg0, tgt, &pos, 1)`.
- `FUN_08808468` pushes `{attacker, target, pos, s16 id, u8 1}` into a 32-slot global queue at 0x09AF84F0, 0x18 bytes per slot, -1 = free.
- `FUN_088076D4` drains the queue: for each slot it calls `FUN_08808AE8(attacker, id, target, pos, 1)` and then resets the queue (`FUN_0880766C`).

`FUN_08808AE8`:
- Skips the hit if target+0x190 & 0x40 (`FUN_088378C4`; the per-tick flag that blocks hits).
- Allocates an attack object (`FUN_08807A00`) and initialises it with `FUN_08807860(atk, attacker, attacker, id, -1)`. That stores id at +0x24 and sets atk+0x34 = `FUN_08837F14(attacker, id)`, a binary search of the attacker's atkp table (+0x49C, 0x44-byte records) with the global fallback `FUN_0880BE84`.
- `FUN_08807E6C(atk, target, atkp, 0, 1)` builds the hit, including the hit sound atkp+0x30 via `FUN_088A24D4`.
- `FUN_08808748` applies it (calls `FUN_088A6A38` for the hit effect).
- Copies pos into the hit, then calls the target's vtable+0x70 (receive hit) and `FUN_08813180`.

**Who uses it / effect in game:** deals the atkp's damage and reaction to the opponent at that frame, wherever they are. There is no hit-window volume, no rehit group and no range check. Retail uses it for scripted EX Burst hits.

**Data check** (17 uses, all Bartz/Noctis):
- scr 8103–8106 (m074–m077): `0x0108` at f16 with LIMIT_ATK02/04/06/08, inside a `hit_window` 12–22 that uses LIMIT_ATK03/05/… So each EX Burst step lands one forced hit plus one volume hit.
- scr 8500 (m078, the EX Burst): 12 × LIMIT_ATKV (1760) every 6 ticks from f112–178, then LIMIT_ATK10 at f242 just before the final hit_window 244–256 (LIMIT_ATK11).
- This is a guaranteed multi-hit, which fits an EX Burst cinematic.

**Confidence:**
- Confirmed for the mechanism (atkp lookup → queued forced hit on the opponent).
- Strong on +0x2FC being the opponent.
- In-game test: add `{frame: N, op: 0x0108, args:[<some atkp id>]}` to a normal attack and check the opponent takes the hit from across the stage.
