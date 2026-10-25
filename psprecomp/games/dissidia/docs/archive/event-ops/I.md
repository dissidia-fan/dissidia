# Agent I: ranged ops 0x072C, 0x0466, 0x0278, 0x0038, 0x0062, 0x0353, 0x0539, 0x0037 (+ keyed 0x0037), 0x0134, 0x0133, 0x0030

**Summary**
- Confirmed end to end: **0x072C** = shed falling feather particles from random bones (Sephiroth/Ultimecia/Kefka feathers; arg6 = EX Mode only), **0x0466** = spawn an effect once at an absolute elapsed tick (lifetime + bone attach), **0x0353** = sustained screen shake for the range, **0x0539** = pull the opponent (and its assist) toward you, **0x0038** = zero your velocity every tick, **0x0134** = EX Burst slow motion (whole-game speed = arg0), **0x0133** = dim the stage to arg0 brightness.
- Strong: **0x0062** = root motion is rotated by the actor's full orientation matrix instead of yaw only (earlier note confirmed), **0x0278** = chain window that opens only if a given atkp id hit (or for Firion always), **0x0037** ranged = Brave input restarts the combo from its first move even when the attack gate is closed, **0x0030** = "floating" physics (air-drag inertia, landing keeps momentum).
- Weak: keyed **0x0037** = clears the opponent's +0x188 bit 0x10 (a timed hold state tied to Ultimecia/Aerith); the CPU-only meaning of 0x0278 arg0 (+0x190 bit 0x08).

Conventions: actor = s2 in the dispatcher; owner (s1) = actor+0x130, so s1+0x60+n = actor+0x190+n. Character ids: the first byte of `*(actor+0x5A4)` indexes the file-name table at 0x089D6CA8 (1 = one100, 2 = one200, 3 = two100 (Firion), … 16 = eht200 (Ultimecia), 28 = ten110 (Yuna), 33 = sev120 (Aerith)).
Scratch: `/tmp/claude-0/re/scratch_I/q.py` (retail usage query), `tbl.py` (handler bounds).

---

### 0x072C — Shed falling feathers from random bones (optionally only in EX Mode)

**Kind / args:** ranged, 7 ints `{rate, chance%, bone0, bone1, bone2, bone3, exOnly}`.
- arg0 rate: number of spawn rolls per tick = `timescale × 0.5 × arg0` (seen 1–10).
- arg1 chance: each roll spawns if `rand()%101 < arg1` (5–100).
- arg2–5: bone indices on the actor's skeleton; the list stops at the first -1 (0xFFFFFFFF). At least one is needed.
- arg6: 1 = only while in EX Mode (+0x5B2 bit 4) (seen only in Sephiroth's files).

**What the handler does** (0x0882B4DC–0x0882B6A0):
- Exits unless the actor is a fighter (FUN_0883739C) and +0x5B3 bit 0x80 is clear. That bit is set per fighter by FUN_08B0FD08 (0x08B0FF5C) from a test FUN_08882CBC(…, 0x3F). It looks like a camera visibility/occlusion test (weak).
- If arg6 ≠ 0, it needs EX Mode and requires FUN_08B3406C false. That function is EX Mode && `*(actor+0x2F8)+8 & 4`, and the meaning of that bit is unknown.
- Counts the valid bones n. Then `for i < FUN_08838300(actor)*0.5*arg0`: it uses the MS-rand LCG with its seed at 0x09AFB89C. If `rand%101 < arg1`, it calls `FUN_08854B14(*(0x089D4DC8)+0x4E0, actor, args[2 + rand%n])`.
- FUN_08854B14 is the global feather particle pool: 40 slots × 0x60 bytes (ring buffer at +8, counters +0xC/+0x10). It runs only if the pool has a resource, loaded refcounted by FUN_08854A5C from FUN_088459E0 (a type-3 resource from the character's data).
  - It takes the bone matrix (`FUN_089B0E78(*(*(actor+0x394)+0x84)+8, bone)` → FUN_089B1224) and picks a random point around the bone (radius from the per-bone float table `*(actor+0x394)[0][bone]`, ±0.2 jitter).
  - It stores the particle: pos +0x10, fall speed +0x34 = 0.5, random spin/sway ±π at +0x38/+0x3C/+0x40, lifetime +0x44 = 180–360 ticks, and scale +0x48 = 1.0 or `(*(actor+0x5A4))[0x71]/10`.
  - Texture/model handles +0x4C/+0x50 come from `*(actor+0x390)+0xC+8`: words [0],[1] normally and **[2],[3] in EX Mode**, so it uses a different texture in EX Mode.
- The update is FUN_08854FAC. Particles sway and fall, die when +0x44 runs out, and hit the floor with a ray test (FUN_088976E4). On water surfaces 0x1E/0x1F they spawn a ripple via FUN_0885AD38.

**Who uses it / effect in game:** this is cosmetic only. Feathers flutter down from the listed bones. Nothing reads the particles back.

**Data check** (75 uses, 7 files):
- eht200 (Ultimecia) uses `{1..4, 5..80, 47,48,34,35, 0}` on almost every motion: hits, dodges, blocks, HP attacks (e.g. m44 scr 10010 80–208 `{1,5,…}`, m50 scr 10050 10–12 `{4,50,…}`). These are her wing bones, and her feathers fit.
- sev200 (Sephiroth) uses `{3, 1 or 30, 55,56,57,-1, 1}` on stand (m1, whole motion) and on hit reactions. That is the one-winged EX form, shedding feathers only in EX Mode, which matches arg6 = 1.
- fiv100 (Bartz) m71 scr 8000 is short bursts `{2,50, bone 0}`. Noctis's 071.json copies this from Bartz, so in the mod it sprays feathers from bone 0 if the resource exists.

**Confidence:** confirmed (spawner and particle update traced). Weak: the +0x5B3 bit 0x80 gate and the `*(actor+0x2F8)+8` bit 4 gate.
**In-game test:** put `{0,65535, 0x072C, [10,100,<bone>,-1,-1,-1,0]}` on Noctis's stand motion. Feathers should pour from that bone. Then set arg6 = 1 and check they appear only in EX Mode.

---

### 0x0466 — Spawn effect once at an elapsed tick (with lifetime and bone)

**Kind / args:** ranged, 4 ints `{fepaId, lifetime, fireTick, bone}`.
- arg0: FEPA record in the actor's own effect bank.
- arg1: lifetime in ticks. 0 = the effect's own length; 99999999 = forever.
- arg2: the elapsed tick to fire at.
- arg3: bone to attach to. < 0 = not attached.

**What the handler does** (0x0882CBB0):
- It fires when `int(actor+0x1AC) < arg2 <= int(actor+0x1B0)`.
- actor+0x1AC/+0x1B0 = owner+0x7C/+0x80. These are the previous and current **total elapsed play time** of the motion: FUN_08830444 does `+0x7C = +0x80; +0x80 += dt`. They are reset to 0 at motion start (FUN_0882F524 0x0882F598) and are never wrapped on loop, unlike +0x78.
- Then `fx = FUN_088A9DE8(*(*(actor+0x390)+0x5C), arg0, actor+0x4C8, 0, 0)` and `fx+0xE4 = (float)arg1`.
- If arg3 ≥ 0: `FUN_088A6A38(fx, 0, actor+0x4C8, arg3, -1.0, -1)` attaches it to bone arg3. This is the same spawner and attachment as keyed `effect` 0x0219 and summon 0x0335 (see E.md).
- Nothing removes the effect when the range ends.

**Who uses it / effect in game:** a one-shot effect at an exact elapsed time, and the timing still works in looping or held motions where a keyed event would repeat or miss.

**Data check** (66 uses, 13 files). Almost always range 0–65535, with the tick in arg2:
- for110 (Kain) m37 (scr 240/242/244/800 dash loop) `{500, 99999999, 24, 2}`: a permanent aura on bone 2 after 24 ticks of dashing.
- sev110 (Tifa) m23 scr 10000–10003 range 0–74: three effects `{118/119/120, 48, 24, bones 3/52/41}`, fired together at tick 24 for 48 ticks.
- eht110 (Laguna) m50 `{16,16,52,30}` + `{17,99999999,68,30}`: two effects at ticks 52 and 68 on bone 30.

**Confidence:** confirmed.
**In-game test:** add `{0,65535,0x0466,[<id>,30,10,0]}` to a looping stand motion. The effect should fire once at tick 10 and not on later loops.

---

### 0x0278 — Chain window only if a given attack hit (Firion: always)

**Kind / args:** ranged, 2 ints `{cpuHint, atkpId}`.
- arg0: 1 also sets +0x190 bit 0x08 (read only by the CPU AI). Retail uses 0, and 1 once.
- arg1: atkp id, e.g. 486/715 (Firion) and 2808–2919 (gst110).

**What the handler does** (0x08829CC0):
- If the actor is a fighter and character id (`*(*(actor+0x5A4))`) == 3 (two100 = **Firion** under the 1-based name table), it sets +0x190 |= 0x04 unconditionally, plus |= 0x08 if arg0.
- Otherwise it sets the same bits only if `FUN_08839120(actor, arg1)` is true. That function scans the 6 entries of actor+0x534 (stride 0x40) for `*(entry+0x20)` → id == arg1, i.e. this atkp is in the actor's recent hit list.

**Who uses it / effect in game:**
- +0x190 bit 0x04 is the `chain_window` bit (0x0101). The attack gate vtable +0x150 (FUN_08837B20) accepts it, so the next combo press is taken (A.md).
- Bit 0x08 is read only at 0x08A77838 in the CPU routine FUN_08A77410 (called from FUN_08A75D38), when the motion time is past a planned tick (weak: "CPU keeps pressing").
- So the follow-up input is accepted only on a hit with that atkp. On a whiff the combo can't continue.

**Data check** (63 uses: two100, fiv100, gst110):
- two100 (Firion) m46 scr 10010 0–100 `{0,486}` + `{0,715}` (two atkps accepted).
- fiv100 (Bartz) m50 scr 10020 0–142 `{0,1194}`. Noctis's 050.json copies this, so Noctis needs atkp 1194 in his hit list for the window to open.
- gst110 m49/m62/m66 use many entries with `pad` phase filters (515–522 = phase 3–10 in byte 6), one per hit of a multi-hit string.
- The Firion id-3 shortcut means his windows open whether or not the atkp hit. This is odd but is what the code does. Treat it as strong, because the id → file mapping is inferred from the table.

**Confidence:** strong (mechanism confirmed; the id-3 = Firion mapping and the arg0 CPU meaning are inferred).
**In-game test:** on Noctis 050 (copied from Bartz), whiff the move and press again: no follow-up. Hit and press: follow-up. Then change arg1 to an atkp the move doesn't use: the follow-up should never come out.

---

### 0x0038 — Kill momentum (zero velocity every tick)

**Kind / args:** ranged, no args.

**What the handler does** (0x0882B7F8): `FUN_08837A5C(actor)` = `FUN_089A3A44(actor+0xC0, actor+0xC0, 0.0)`, so the velocity/inertia vector at actor+0xC0 (set by FUN_08835C24, "SetVelocity") becomes zero. It does this every tick of the range.

**Who uses it / effect in game:** any inherited momentum (dash carry-over, knock-back drift, fall speed in +0xC0) is cancelled while the range runs. The character stays in place except for root motion. The engine's landing routine FUN_08833818 calls the same function when the actor is not "floating" (see 0x0030).

**Data check** (48 uses, mostly the hovering fighters):
- thr200 (Cloud of Darkness): whole motion on most moves (m40–53 scr 10010–10044) and from mid-motion on HP attacks (m54 scr 10090 from 40).
- thr200 / for200 (Golbez) / eht200 (Ultimecia) m10–11 scr 221/243, whole motion.
- eht200 m52 scr 10053 ticks 10–40.
- This fits floaters whose attacks shouldn't slide.

**Confidence:** confirmed.
**In-game test:** add 0x0038 over a dash attack's whole motion. The approach slide from the dash should stop dead at tick 0.

---

### 0x0062 — Root motion follows full 3D orientation (not yaw only)

**Kind / args:** ranged, no args. Sets **+0x197 bit 0x08** (0x0882CB9C).

**Who uses it / effect in game:** the only reader is FUN_08832EB8 (0x08832FF0), the root-motion step called from the movement integrator FUN_0883AF80 (0x0883B10C). The result is added to the position.
- Input: the motion's root delta at actor+0x140 (owner+0x10): xyz translation and w = yaw delta. A non-zero w first turns the actor (FUN_08836560 → FUN_08835C54).
- **Bit clear (default):** the translation is rotated by a single Y rotation, angle = wrap(actor yaw +0x9C − owner+0xC) (FUN_089941C0 = fmod by π at 0x089C7E88; FUN_089A4124 builds the rotation). So only yaw is applied.
- **Bit set:** the translation is first transformed by the matrix at the start of the actor object (`FUN_089A3F40(v, actor, v)`), then by the Y rotation of −owner+0xC. If the actor is pitched or rolled, the root motion is too.
- That actor+0x00 is the full orientation matrix is inferred from how it is used. I didn't trace who writes it.

**Data check** (47 uses): moves where the body tumbles or flips:
- nin100 (Zidane) m50 scr 10050 28–40 and m60 50–80.
- ten100 (Tidus) m66/m67 50–72.
- gst120 m57/m58/m64/m71 whole motion.
- six100 (Terra) m50 whole motion.

**Confidence:** strong. The branch is confirmed; the "full orientation matrix" reading of actor+0 is inferred. This confirms the earlier note.
**In-game test:** take a move with forward root motion that is performed while airborne-pitched (e.g. a homing dive), and toggle 0x0062. Without it, the travel stays horizontal. With it, travel follows the pitched facing.

---

### 0x0353 — Screen shake for the range (sustained)

**Kind / args:** ranged, 3 ints `{duration, xAmp%, yAmp%}`.
- arg0: duration in ticks; ≤ 0 → 30. Retail sets it equal to the range length.
- arg1 / arg2: x / y amplitude × 0.01; ≤ 0 → 1.0. Seen 5 or 10.

**What the handler does** (0x0882C488):
- Every tick it sets **+0x197 bit 0x02**.
- If `actor+0x530 == 0`, it creates `actor+0x530 = FUN_088749A0(arg0, 0, arg1*0.01, arg2*0.01)`. FUN_088749A0 is the camera-shake constructor. The keyed `screen_shake` 0x0306 (keyed table code 6, 0x0882D244) and the hit builder FUN_08807E6C also call it, with the same arg handling.
- The object (0x24 bytes, global list at 0x09B48358) is updated by FUN_08874680:
  - offset +0/+4 = noise (64-entry random table 0x09B48258, indexed by tick) × amp × sin(π·t/duration);
  - the shake ends at t ≥ duration (+0x18 = 2).

**Who uses it / effect in game:**
- FUN_08831360 (0x08831624–0x08831664) runs every actor update. If `+0x530` exists and either its +0x1C flag is set or **+0x197 bit 0x02 is no longer set**, it stops the shake (FUN_088745A4) and clears +0x530.
- So the shake starts at the range's first tick, at most one per actor, and is cut when the range ends.
- The EX Burst start (FUN_08A3EB70 0x08A3EC58) kills all shakes.

**Data check** (43 uses, 26 files), all big HP attacks (scr 1009x):
- one200 (Garland) m58 40–90 `{50,10,10}`: duration 50 = range length.
- two100 (Firion) m56 48–138 `{90,10,10}`: 90 = range length.
- two200 (Emperor) m51 228–358 `{130,5,5}` then 358–end `{182,10,10}`: a small shake, then a bigger one.

**Confidence:** confirmed.
**In-game test:** add `{0,60,0x0353,[60,30,30]}` to any attack. You should see a strong shake for one second, cut off early if you shorten `end`.

---

### 0x0539 — Pull the opponent toward you (vacuum)

**Kind / args:** ranged, 5 args `{speed f, outerR f, innerR f, onlyIfReeling, onlyIfNotReeling}`.
- arg0: pull distance per tick (0.02–0.3).
- arg1: max range (3–25).
- arg2: stop distance (0–3).
- arg3: 1 = pull only a target in a hit reaction.
- arg4: 1 = pull only a target not in one.

**What the handler does** (0x0882B808–0x0882BABC). It runs twice: target = opponent (vtable +0x130 → +0x300), then the opponent's +0x1108 partner (probably the assist, if a fighter, skipped when FUN_08837990 is true). For each target:
- **Skip when:**
  - arg3 and the target is not reeling, or arg4 and it is. FUN_0883809C = script 500–798 except 702–704/752–754.
  - FUN_08838178: target state `*(+0x3A4)` ∈ {0x0C, 0x0D, 0x0E, 0x13, 0x19}.
  - The target fighter has +0x5B3 bit 1 or 2.
- **Distance window:** d = |actor+0x80 − target+0x80| (FUN_08837AA4). It requires innerR < d < outerR.
- **Ease-in:** within 3 units of innerR, the strength is scaled by sin((d−innerR)/3 · π/2).
- **Applies the pull:** v = actor+0x100 − target+0x100 (FUN_089A38E0 = vsub), normalized. The magnitude is min(|v|, speed) × ease. `FUN_08836580(target, v)` writes the target's external-motion vector +0x120 (dir + magnitude w), then the target's vtable +0x1C0 is called.

**Who uses it / effect in game:**
- FUN_0883AF80 (0x0883B1A0–0x0883B220) adds `+0x120.xyz × +0x12C × timescale` to the target's position each tick.
- So the foe (and their assist) is dragged toward the user, slowing to a stop at innerR.

**Data check** (38 uses, 23 files): black-hole and vacuum moves.
- fiv200 (Exdeath) m48 scr 10012 0–170 `{0.05,10,2,0,0}`.
- for200 (Golbez) m53 scr 10090 12–50 `{0.1,10,1,0,1}`.
- two100 (Firion) m47 scr 10012 0–18 `{0.2,4,1,1,0}`: a hit-confirmed pull.
- thr100 (Onion Knight) m58: three entries split by pad and the reel flags.

**Confidence:** confirmed (the +0x1108 = assist reading is strong).
**In-game test:** add `{0,60,0x0539,[0.2,15.0,1.5,0,0]}` to a Noctis move. The CPU should be sucked in from up to 15 units and stop about 1.5 away.

---

### 0x0037 (ranged) — Brave press restarts the combo (attack allowed mid-move)

**Kind / args:** ranged, no args. Sets **+0x195 bit 0x01** (0x0882B7B4). 0x003A also sets this bit, together with +0x194 bit 0x80 and +0x195 bit 0x02.

**Who uses it / effect in game:**
- FUN_08B334D0 returns the bit unless +0x5B4 bit 2.
- In the player input handler FUN_08B3526C (0x08B364C0–0x08B36520), when the Brave input check FUN_08B309C8(actor, 0) passes (and not FUN_08B322B8 / FUN_08B34E38) and the bit is set, it calls:
  - **FUN_08B308B8**, which clears the current action record (+0x101C/+0x1020/+0x1024);
  - then `FUN_08B2D664(actor, 2)`, which starts command slot 2 through FUN_08B382B8.
- This happens **even when the normal attack gate s2 (vtable +0x150) is false**. Without the bit, slot 2 starts only if s2 is true.
- Effect: during the range, a Brave press starts the Brave command fresh, as the first move of its chain, not as a combo follow-up.
- The CPU AI checks the same function (FUN_08A83590 0x08A83834).
- A direct reader at FUN_08A7A664 (`lbu 0x195($s1)`) works on a different struct and is not this flag.

**Data check** (35 uses):
- thr200 (Cloud of Darkness): nearly every attack from a mid/late tick to the end (m40 scr 10010 from 20, m54 scr 10090 from 106), all with pad 512. Her moves can be followed by a fresh Brave attack once their active part is over.
- ten200 (Jecht) m48/49/53 short windows (42–50, 56–66, 34–44): his rush-combo restart windows.

**Confidence:** strong. The code path is confirmed; slot 2 = Brave is from round 1's naming of FUN_08B309C8.
**In-game test:** put `{20,65535,0x0037}` on a Noctis Brave move with no chain window, then press ○ after tick 20. A new Brave attack (the first of the chain) should start.

### 0x0037 (keyed) — Release the opponent's timed hold flag (+0x188 bit 0x10)

**Kind / args:** keyed, no args.

**What the handler does** (0x0882E6B8):
- opp = vtable +0x130(actor). If `opp+0x188 & 0x10`, it clears that bit.
- +0x188 is a persistent state word (not cleared per tick).
- Bit 0x10 is set by overlay state callbacks FUN_08A54F08 (0x08A5508C), FUN_08A56E10 (0x08A56EF0), FUN_08A6AC9C, FUN_08A7217C and others.
  - FUN_08A56E10 keeps the bit set while a timer (`*(actor+0x314)+0x10`) is below a threshold.
  - That threshold is changed when the actor at +0x304 is character 0x10 (Ultimecia) or 0x21 (Aerith).
- Readers: FUN_0883A150 (0x0883A6A8) skips the collision response FUN_0883A6FC for contact type 0x13 while the bit is set, and FUN_0888FE28 (0x0888FFE4) keeps an attached effect alive while it is set.

**Data check:** 2 uses, both at f0: sev120_a (Aerith) m9 scr 10001 and eht200 (Ultimecia) m63 scr 10070. These are exactly the two characters the hold-state threshold special-cases, which fits "end the hold this move put on the foe".

**Confidence:** weak on the meaning (mechanism confirmed). Settle by checking FUN_08A56E10's state with the debugger: does the opponent leave the held/frozen state when Ultimecia's 10070 starts?
**In-game test:** remove the keyed 0x0037 from eht200 m63 and see whether the opponent stays frozen/held longer after that move.

---

### 0x0134 — EX Burst slow motion (whole-game speed = arg0)

**Kind / args:** ranged, 1 float = game speed factor (0.1–0.73; 0.2–0.3 typical).

**What the handler does** (0x0882B274):
- `owner+0x88` (= actor+0x1B8) = arg0, and **+0x193 bit 0x80** is set.
- At the start of the next tick, FUN_08829848 (0x08829850–0x0882987C) sees the bit still set and calls `FUN_08A620B0(*(0x08A62280), 0)`, which resets speed slot 0 to normal (2.0). Then the flags are cleared.

**Who uses it / effect in game:**
- The EX Burst controller update FUN_08A3FE10 runs from FUN_088413D0 (0x0884155C) only while the byte at `*(0x089D85E8)` == 1, with `*(0x08A405E0)` as the controller.
- The burst start FUN_08A3EB70 sets its actor at +0x26C = the burster and +0x270 = that actor's opponent. It also kills shakes and ends the chase.
- When `+0x26C`'s +0x193 bit 0x80 is set (0x08A3FF10), it calls `FUN_08A62020(speedObj, slot 0, actor+0x1B8 × 2.0)`; otherwise it resets slot 0.
- speedObj has 10 slots, 2.0 = normal. FUN_08A621E8 multiplies all slots/2 and calls `FUN_08864D98(2.0 × product)` (global time step), or resets when all slots are 2.0.
- So **the whole game runs at arg0 × normal speed while the range is on**.
- It only works for the actor that owns the current EX Burst.
- The sibling op 0x0126 (+0x193 bit 0x40, same float slot) instead scales the **opponent's** own time scale (FUN_08B34CB0(+0x270, slot 1, value), 0x08A3FEE4).

**Data check** (35 uses, 12 files), only in EX Burst scripts:
- one100 (Warrior of Light) scr 8102–8111 from 38 to end, 0.2.
- fiv100 (Bartz) scr 8102–8106, 0.3.
- ten100 (Tidus) scr 8000 from 58 and scr 8101 from 270, 0.2.
- None appear outside 8000–8500.

**Confidence:** confirmed (the chain from flag to global time step is traced; the controller is identified as EX Burst by its data and setup, which is strong).
**In-game test:** in Noctis's EX Burst motions, change 0x0134's arg from 0.3 to 1.0: there should be no slow-mo. Set 0.05: near freeze.

---

### 0x0133 — Dim the stage (stage brightness = arg0)

**Kind / args:** ranged, 1 float = brightness multiplier for R, G and B (0 = black stage, 0.4–0.6 typical).

**What the handler does** (0x0882B738): if `*(*(0x089D85E8))` == 1 (the same battle-state byte as above), it calls `FUN_08AB40DC(*(0x08AB4250), arg0)`. That pushes {arg0, arg0, arg0} into a 4-entry per-tick request list (FUN_08AB40FC: +0xC count, entries at +0x10, stride 0xC).

**Who uses it / effect in game:**
- FUN_08AB3EA0 multiplies 1.0 by all requests, plus an automatic factor (+4) for any fighter or partner whose current action record has flag `+0xF & 2` (FUN_08AB4198/FUN_08AB421C).
- It converts the result to an RGB color (×128 → byte, alpha 0xFF; 0xFF808080 = normal) and sets it on the stage with FUN_08893A20(*(0x089D6AC8), color, 10). That function writes the stage model color (+0x708/+0x54, FUN_089AD2D0).
- Then it clears the list. So the stage darkens only on the ticks the range is active.

**Data check** (30 uses, 8 files):
- two100 (Firion) EX Burst scr 8101–8106/8500, whole motion, 0.6.
- two200 (Emperor) scr 8104–8106 0.0 (pitch black).
- sev200 (Sephiroth) m67/68 scr 10080/10081 0.5 (Supernova-type HP attack).
- eht200 (Ultimecia) m63 scr 10070 0.5.

**Confidence:** confirmed.
**In-game test:** add `{0,65535,0x0133,[0.0]}` to a Noctis HP attack. The stage should go black (fighters and effects stay lit) and return to normal after the move.

---

### 0x0030 — Floating physics (air-drag inertia, keep momentum on landing)

**Kind / args:** ranged, no args. Sets **+0x194 bit 0x20** (0x0882B710).

**Who uses it / effect in game:** FUN_08837C44 = "is floating": true if the actor's float controller at +0x39C has flag bit 1 (FUN_0884E53C), **or** +0x194 bit 0x20. Its readers:
- **FUN_08804674**, the movement callback of the motion-state class (vtable 0x089C69E0 slot +0x2C). When floating, velocity is computed by FUN_088344E8 (+0x39C's +0x90/+0x94 as horizontal/vertical drag, scaled by time step), i.e. inertia decays like air drag. Otherwise it takes the +0xB0 movement vector (zeroed for script 0x66).
- **FUN_08833818**, the landing routine from FUN_0883B470 (0x0883C074/0x0883C2B0) after the FUN_08833898 landing test. When not floating it zeroes velocity (FUN_08837A5C); when floating it keeps it.
- CPU AI FUN_08A93758 (0x08A93AD0).
- Caveat: the floating path dereferences +0x39C without a null check, so a character without that controller could crash. All retail users presumably have one.

**Data check** (30 uses):
- six100 (Terra) / six200 (Kefka) / sev200 (Sephiroth) on motions 2–4 = scripts 150/151/152, whole motion (a float/glide movement set; guess).
- for110 (Kain) m57–59 scr 10051–10053 from 4 (jump attacks).
- six100 m51 scr 10030 0–180.

**Confidence:** strong (the readers are confirmed; "air-drag feel" is from the damping function, not seen in game).
**In-game test:** give a Noctis dash-landing motion 0x0030 for its whole length. He should keep sliding after touching down instead of stopping.

---

## Notes for EVENT_OPS.md
- New flag bits:
  - +0x193 bit 0x80 = 0x0134 (EX Burst slow motion); +0x193 bit 0x40 = 0x0126 (slow the opponent);
  - +0x194 bit 0x20 = 0x0030 (floating);
  - +0x195 bit 0x01 = 0x0037/0x003A (Brave restart);
  - +0x197 bit 0x02 = 0x0353 (shake alive); +0x197 bit 0x08 = 0x0062 (3D root motion).
- Fields:
  - actor+0x1AC/+0x1B0 = previous/current total elapsed motion time (no loop wrap);
  - actor+0xC0 = velocity; actor+0x120 = external pull vector (dir + magnitude);
  - actor+0x530 = active screen-shake object; actor+0x1B8 = float slot for 0x0126/0x0134.
- Character id = `*(*(actor+0x5A4))`, 1-based index into the name table at 0x089D6CA8 (3 = Firion, 16 = Ultimecia, 28 = Yuna, 33 = Aerith).
- Global game speed: object `*(0x08A62280)`, 10 slots (2.0 = normal), FUN_08A62020 set / FUN_08A620B0 reset.
