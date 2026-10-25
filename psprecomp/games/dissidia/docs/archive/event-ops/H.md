# Round 2, letter H: 0x0518, 0x0047, 0x0074, 0x017C, 0x001D, 0x044F, 0x0145, 0x0210, 0x004B

**Summary**
- **Confirmed** (code traced end to end): 0x0047 (dodge invincibility 20→40 ticks with ability 0x136), 0x004B (push mass 1000 instead of 100), 0x017C (wake-up invincibility for arg0 ticks), 0x001D (chase dash: enters state 0xA, which closes the gap to the opponent).
- **Strong:** 0x0518 (float and steer: air-action state with stick movement, arg0 = turn-rate multiplier, arg1 = move-speed multiplier), 0x0210 (turn the body to face its own velocity, used in blow-away), 0x044F (ground forward slide at arg1 units per tick), 0x0074 and 0x0145 (the first 4 ticks of a dodge and of a guard are a "perfect" window, but only with abilities 0x41 and 0x40).
- **Not resolved:** the in-game names of abilities 0x136, 0x41 and 0x40 (no ability-name data in the workspace), the meaning of 0x0518 args 2–4 (weak), and clash-matrix result codes 1/3/4 (needed to say exactly what a perfect guard does).

Conventions: dispatcher s1 = motion player at actor+0x130 (so s1+0x60 = actor+0x190, the per-tick flag block), s2 = actor, s0 = event, args at s0+0xC. FUN_0883739C = "actor kind is 0/1 (fighter)". FUN_088BF4CC(FUN_08835778(actor), id) = "has ability id" (bit test in the ability bitset at +8, then a sorted-table lookup). FUN_08838300 = per-tick time scale. Constant 0x089C78F0 = π, so `x*π/180` = degrees to radians. Counts and examples come from retail_timelines.json (30 fighters, 59 objx).

---

### 0x0518 — Float and steer: move with the stick during the move (air-action state)
**Kind / args:** ranged, 5 args. 127 uses, mostly Kuja (nin200), Ultimecia (eht200), Golbez, Sephiroth, Terra, CoD, Tidus, Exdeath and Gilgamesh, all in character moves (10000+).
- arg0 float = turn-rate multiplier (→ actor+0x3D0). Seen 0, 0.15–1.0.
- arg1 float = move-speed multiplier (→ actor+0x3D4). Seen 0, 0.3–6.0. Most common: `{1.0, 1.0, 0, 0, 0}` (50×).
- arg2 int (bool) → +0x192 |= 0x40. Seen 1 in Garland (one200) and CoD.
- arg3 int (bool) → +0x192 |= 0x10. Seen 1 in Garland 043 and Firion/Emperor two200 044.
- arg4 int (bool) → +0x198 |= 0x01. Seen 1 only in Tidus (ten100) 064.

**What the handler does (0x0882ACAC):**
1. If the actor is a fighter and `+0x5B3 & 1` or `+0x5B3 & 2` is set, it does nothing. Those bits are set by FUN_08AC4960 (0x08AC4AF8/0x08AC4B0C), a battle-manager state machine whose purpose I could not identify.
2. Otherwise it sets `+0x192 |= 0x08`, `+0x3D0 = arg0`, `+0x3D4 = arg1`, and the arg2–arg4 bits listed above.
3. If `+0x380 & 0x02000000` is clear and the state `*(+0x3A4)` is not 0x12, it switches to **state 0x12** (air action) with FUN_08834C10(actor, 0x12, 0).
4. It sets `+0x190 |= 0x02`, the same bit as `air_motion` 0x000A (handler 0x08829A6C).

The sibling op 0x0225 (0x0882ADE4) does steps 2's +0x3D0/+0x3D4 writes but sets +0x192 |= 0x20, with no state change and no air bit.

**Who uses it / effect in game:**
- **+0x190 bit 0x02** makes FUN_0883752C true, and FUN_088374B0 ("is airborne") calls it. The actor counts as airborne for the range.
- **+0x192 bit 0x08**, in FUN_08B30544 (0x08B30588), called by the player movement update FUN_08B36D2C (0x08B37280). The function is true when all of these hold:
  - `+0x5B2 & 1`;
  - the current state's flags `*(+0x3A4)+4 & 0x40`;
  - (`can_act` +0x190 bit 0x01, OR this bit, OR 0x0225's bit +0x192 0x20);
  - NOT +0x192 bit 0x80;
  - NOT +0x193 bit 0x02.

  When it is true, FUN_08B2D2CC applies stick movement. It turns the move vector `+0xB0` into a speed `+0xBC` = (character walk or air speed from params FUN_0884E48C/0884E484) × **+0x3D4**. So **0x0518 lets the stick move the character during the move**, which is otherwise blocked because `can_act` is off.
- **+0x3D0** is the turn-rate multiplier. FUN_08831360 (0x08831460) turns yaw +0x9C toward +0x320 at `rate × +0x3D0 × timescale`. FUN_08833658 resets +0x3D0, +0x3D4, +0x3D8 and +0x3DC to 1.0 each tick; abilities 0x11F and 0x120 add to +0x3D4 and +0x3D8 (FUN_08B2E050). So arg0 = 0 means "can't turn", and arg1 = 0 means "can't move".
- **arg2 (+0x192 0x40)**, in FUN_08804674 (0x088046C8). This is a state velocity method (it is referenced from vtable data at 0x089C6A0C and 0x08A54EDC). With the bit set, it builds the displacement from actor+0x90 instead of the stick vector +0xB0. I believe +0x90 is the facing vector (0x044F also rotates it). **weak**
- **arg3 (+0x192 0x10)**, in FUN_08B2DD60 (0x08B2DDB8), called from FUN_08B37604. It lets the "turn to face the movement direction" step (FUN_08835C7C(actor, +0xB0)) run even while in an action (FUN_088377C0). **strong**
- **arg4 (+0x198 0x01)**, in FUN_08B36D2C (0x08B36EFC) and FUN_08B38A0C (0x08B38DAC). The stick vector +0x550 is re-expressed through the actor's matrix (relative to the character's facing). **weak**

**Data check:**
- Ultimecia eht200 043/046/049/053 have `{1,1,0,0,0}` over 0..65535, and 044/045/047/048/050/051 have `{0,1,...}` (move but don't turn). These are her spell motions, which fits "move while casting".
- CoD thr200 039 has 0..70 `{1,1}`, and 061 has 6..22 `{0.3, 6.0}` (a fast short drift).
- Tidus ten100 064 (script 10097) has arg4 = 1 on phase-split rows (pad 256/512).

**Confidence:** strong for the base behaviour (stick movement allowed + air-action state + airborne flag, with speed and turn multipliers). Weak for args 2–4. The state 0x12 flags word could not be read statically (the state table is built at runtime: FUN_08A5D2B8 → `*(0x089D85F0)+0x18C`).

**Test:**
1. Put `0x0518 {1.0, 1.0, 0, 0, 0}` over a whole ground attack and hold the stick. The character should drift and turn during the attack.
2. Change to `{0, 1, ...}`: it should still drift, but not turn.
3. Change to `{1, 0, ...}`: it should turn, but not move.
4. Also check whether a ground move becomes airborne (a landing motion plays at the end).

### 0x0047 — Extended dodge invincibility (needs ability 0x136)
**Kind / args:** ranged, no args.

**What the handler does (0x0882A76C):** if FUN_088BF4CC(FUN_08835778(actor), **0x136**) is true, it sets `+0x190 |= 0x20` (the same bit as `invincible` 0x0006). Confirmed. 0x136 is the only use of that ability id in the game code. Its name is unknown: no ability-name data is in the workspace.

**Who uses it / effect in game:** the same consumer as 0x0006 (see out1/A.md). FUN_088377D0 reads the bit (invulnerable), and the hit is skipped in FUN_08808EE4.

**Data check:**
- All 29 fighters' dodge motions (scripts 400/410/440/460) have `0x0006 0..20` plus `0x0047 0..40`. So the ability doubles the dodge's invincibility from 20 to 40 ticks.
- The one exception is Tifa sev110 motions 023/024 (shared by scripts 10000–10013), 0..24: the same ability also gives invincibility at the start of her moves.

**Confidence:** confirmed (code), but the ability's name is unknown.

**Test:** equip candidate dodge abilities one at a time. Dodge an attack that arrives about 25–35 ticks into the dodge: it should whiff only with ability 0x136. A memory watch on the ability bitset (+8 of the struct from FUN_08835778, bit 0x136) would identify the ability directly.

### 0x0074 — Perfect-dodge window: first 4 ticks of a dodge (needs ability 0x41)
**Kind / args:** ranged, no args.

**What the handler does (0x0882BD88):** if the actor has ability **0x41**, it sets `+0x198 |= 0x10`. 0x41 is the only use of that id.

**Who uses it / effect in game:** FUN_08B2DC38, the per-tick dodge watcher (0x08B2DD14). When a dodging actor is invulnerable (FUN_088377D0) and FUN_08B2FFF8 says an attack will connect within 4.0 ticks (constant at 0x08B3ABC4), it does three things once per dodge:
1. Sets `+0x5B1 |= 8`.
2. Posts event 4 (FUN_088593B4).
3. **If +0x198 bit 0x10 is set**, calls FUN_08B34FB8:
   - `+0x5B4 |= 1`. While the actor stays in evade scripts, FUN_08B2DC38 then holds invincibility slot 4 (`+0x126C`, which sets `+0x380 |= 0x20000000`, read by FUN_088377D0). So the actor stays invulnerable for the rest of the dodge. It is cleared when the actor leaves evade.
   - `+0x1290 = 90.0` and `+0x5C0 |= 0x2000`. FUN_08B37BCC counts the 90 ticks down and tints the model. The HUD code FUN_08B15F80 (0x08B16424) shows status icon 0xA7 while the bit is set.

I found no other gameplay reader of +0x5C0 0x2000 or +0x1290.

**Data check:** all 29 fighters have it in dodge motions 016–019 (scripts 400/410/440/460), always ticks 0..4. That fits a "dodged at the last moment" window.

**Confidence:** strong. The chain is clear; the in-game name of the ability and of the icon is unknown.

**Test:** with ability 0x41 equipped, dodge just as an attack lands. Expect a body glow and HUD icon for about 1.5 s, and invincibility until the dodge ends. Without the ability, or dodging early, expect neither.

### 0x017C — Wake-up invincibility: invincible for arg0 ticks if you leave the knockdown here
**Kind / args:** ranged, 1 arg: arg0 float = duration in ticks. It is always 8.0 in retail.

**What the handler does (0x0882B7C8):** fighters only. It sets `+0x199 |= 1` and `+0x1270 = arg0`.

**Who uses it / effect in game:**
- FUN_08B2ED7C is fighter vtable +0x68 (entries 0x08B3AA14 and 0x08A61D64). It is called by FUN_0882F0D4 just before the motion player switches to a new script (`sh new, +0x50` → actor+0x180).
  - If +0x199 bit 1 is set and the **new script is outside 500..798** (not a hit reaction), it calls FUN_08B34E98(actor, 5).
  - That sets invincibility slot 5 (`+0x126D = 1`, `+0x380 |= 0x20000000`). FUN_088377D0 then reports the actor as invulnerable.
- FUN_08B372D8 (0x08B37590..0x08B375DC), while slot 5 is set:
  - counts `+0x1270` down by the time scale;
  - clears the slot (FUN_08B34ED0(actor, 5)) when the timer reaches 0, **or as soon as the actor is in an action** (FUN_088377C0, +0x380 & 8).

So when you recover from a knockdown, you get arg0 ticks of invincibility. Attacking ends it at once.

**Data check:** all 29 fighters have it in motions for scripts 702, 703/704, 752 and 753/754, always ticks 70..98 (the last ~30 ticks of the ~100-tick down/recovery motions after blow-away 700/750). This is the window where the fighter gets up.

**Confidence:** confirmed (setter → slot 5 → invulnerable check; timer and cancel).

**Test:** get blown into a wall/floor, and have the CPU attack exactly as you get up. Hits in the first 8 ticks should whiff. Change arg0 to 60 to make it obvious, then attack during it to see it end early.

### 0x001D — Chase dash: close in on the opponent (enters state 0xA)
**Kind / args:** ranged, no args.

**What the handler does (0x0882AEFC):**
- If the state `*(+0x3A4)` is not 0xA, it calls FUN_08A58098(actor, end − motion time). Motion time is owner+0x78 = actor+0x1A8, so this is the ticks left in the range.
- FUN_08A58098 switches to **state 0xA** (FUN_08834C10(actor, 0xA, 0)) and stores max(remaining, 1.0) at `*(+0x3A8)+0x14`.
- It runs once, because the state check stops the re-entry.

**Who uses it / effect in game:** state 0xA's vtable is at 0x08A58104–0x08A58154.
- **Enter, FUN_08A57AB4:**
  - stores the unit vector to the opponent (vtable +0x130 = +0x300) in data+0..8;
  - stores gap = distance − both body radii (FUN_08837124) + 0.5 in data+0x18 (clamped to ≥ 0);
  - sets `+0x384 |= 0x1000`.
- **Velocity, FUN_08A57C70:** moves the actor along that line so the gap closes over a fixed **26 ticks**, or **58 ticks if the actor's own script is 813** (0x32D). There is special handling while the opponent is in script 822 (0x336).
- **Per tick, FUN_08A58064:** when the motion ends (`+0x188 & 1`), switches to state 0x12 (air action).
- I found no reader of the stored remaining-ticks value (+0x14) in state 0xA's methods. The dash length is the fixed 26/58.

**Data check:** all 29 fighters have it in the chase attack motions (scripts 810/820, 811, 812/815/816 over 0..12, and 813/821 over 0..44). It fits the first ticks of a chase attack, which zips to the blown-away foe.

**Confidence:** confirmed (event → state 0xA → velocity toward the opponent). The role of +0x14 is unknown.

**Test:** put `0x001D {0,12}` at the start of a normal ground attack. The character should dash to the opponent (contact distance) over about 26 ticks, regardless of distance.

### 0x044F — Forward slide on the ground {angle, speed, only-without-target, wait-for-homing}
**Kind / args:** ranged, 4 args.
- arg0 float degrees = angle the slide is rotated from the facing direction (×π/180). It is always 0 in retail.
- arg1 float = speed in units per tick (× time scale). Seen 0.01–1.0, commonly 0.1–0.5.
- arg2 int = 1: skip while the actor has a lock-on target (vtable +0x120 = FUN_08836D8C → +0x2FC). Seen 1 in most uses, 0 in Garland, Gilgamesh (fiv210) and a few others.
- arg3 int = 1: in homing state 0xF, slide only after the homing has stopped (`*(+0x314)+0x48+0x34 & 2`; see out1/F.md). Seen 1 in Bartz (fiv100) 058–061, Gilgamesh 043/045 and others.

**What the handler does (0x0882C1EC):**
1. If airborne (FUN_088374B0), it does nothing.
2. In state 0xF with arg3 set: it slides only if the homing-stopped bit is set. Otherwise (other states, or arg3 = 0): if arg2 is set and the actor has a target, nothing happens; else it slides.
3. **Slide:** `v = rotY(actor+0x90, arg0°)`, normalised. Then FUN_08835034(actor, v, arg1 × timescale) → FUN_088339FC adds `v × speed` into the extra-velocity accumulator actor+0xE0 (direction xyz + magnitude in +0xEC). FUN_08833658 clears +0xE0 every tick.

**Who uses it / effect in game:** +0xE0 is the per-tick extra velocity the movement code adds, so the fighter slides forward along its facing for the range.

**Data check:**
- Cloud sev100 043 (scripts 10000/10003) has 14..24 `{0, 0.3, 1, 0}`.
- Bartz fiv100 058–061 have 0..6–14 with arg3 = 1 (follow-up slide after the homing ends).
- Gilgamesh fiv210 046 has several overlapping phase rows (pad 271/527) `{0, 0.07–0.3, 0, 0}`.

These are short forward lunges inside ground combo hits, which fits.

**Confidence:** strong. The mechanism is clear. Not settled: whether +0x2FC is ever null in a normal 1v1. If it never is, arg2 = 1 slides would only happen with no target, as a fallback when nothing to home on.

**Test:**
1. Give a ground attack `0x044F {0, 0.5, 0, 0}` over 0..30. The fighter should slide forward about 15 units.
2. Then set arg2 = 1 and compare with and without a target (for example, after the opponent dies, or in a no-target mode).

### 0x0145 — Perfect-guard window: first 4 ticks of a block (needs ability 0x40)
**Kind / args:** ranged, 1 arg: arg0 int = 1 disables the upgrade (+0x198 |= 8). It is always 0 in retail.

**What the handler does (0x0882BD3C):** sets `+0x195 |= 0x20`, and if arg0 ≠ 0 also `+0x198 |= 8`.

**Who uses it / effect in game:**
- **Clash resolver FUN_08809F00** (0x0880A21C..0x0880A2C0, the same again for the other side at 0x0880A2D4..). When the defender's colliding volume kind is 4 (a guard volume), all of these hold:
  - the defender is blocking (FUN_08837360);
  - the defender has **ability 0x40**;
  - +0x195 bit 0x20 is set;
  - +0x198 bit 8 is clear.

  Then the guard volume's kind is upgraded to **0xC**: its flag word gets `|= 0x200`, the result is looked up in the 16×16 clash matrix at 0x089D4978 (FUN_0884A454), and FUN_08B34F9C sets `+0x1290 = 90` and `+0x5C0 |= 0x1000`. That flag drives HUD icon 0xA6 (FUN_08B15F80 at 0x08B163A8) and the body tint (FUN_08B37BCC).
- In the matrix, row 4 (a normal guard) against attack kinds 0/1/2/3 gives 3/1/4/4. Row 12 (0xC) gives 3/3/3/3. So the perfect guard turns results 1 and 4 into 3 against those attack kinds. I have not decoded what results 1/3/4 mean; my guess is that 3 = guard holds / attacker repelled and 4 = guard crushed.
- **Projectiles:** FUN_08ADA0A4 also maps a volume with flag 0x200 to kind 0xC. It tests the struck fighter's +0x195 bit 0x20 or 0x40 and passes that to the projectile object's handler (vtable +0x28).

**Data check:**
- All fighters have it in block motions 020/021 (scripts 470/471), ticks 0..4.
- A few attacks have it over 0..10 or 0..40: WoL one100 056 (10090/10098), Firion two100 052, Exdeath fiv200 056 (10020/10028). These are probably guard-type attacks.

**Confidence:** strong for "first 4 ticks of a guard + ability 0x40 → perfect-guard upgrade with a 90-tick icon". Weak for the exact clash outcome.

**Test:** with ability 0x40 equipped, block an attack timed to land in the first 4 ticks of the guard. Look for the glow and icon, and a different block outcome (no guard crush against a strong attack). Then block the same attack late, with nothing extra.

### 0x0210 — Turn the body to face its own velocity (blow-away flight)
**Kind / args:** ranged, 2 args.
- arg0 int = max turn per tick in degrees, clamped to 0..180. It is always 180 (snap) in retail.
- arg1 int = 1: face backwards (opposite to the velocity).

**What the handler does (0x0882A3A8):** calls FUN_08836468(actor, arg1 ≠ 0, arg0 × π/180) → FUN_08836120(actor, &actor+0xF0, flip, maxStep).
- **actor+0xF0 is the actor's velocity.** FUN_0883AF80 at 0x0883B380–0x0883B3B0 stores this tick's displacement divided by the time step there.
- FUN_08836120 copies it and negates it if flip is set. Then:
  - yaw: FUN_08835CF8 turns +0x9C toward atan2(x, z) by at most `maxStep × timescale`;
  - pitch: turns +0xAC toward −atan2(y, horizontal length) by the same cap. The step is scaled down when the horizontal speed is < 0.1.
- It ORs the mask at 0x089C7FA0 (0x40 in the low word) into +0x388.

Siblings: FUN_08836488 (target +0x2FC) and FUN_088364F4 (opponent +0x300) use the same core, which `face_target` and `turn_to_opponent` use.

**Who uses it / effect in game:** the body's yaw and pitch follow the flight path, nose-first or back-first.

**Data check:**
- All 29 fighters: script 700 (motion 029 etc.) `{180, 1}` and script 750 `{180, 0}`, over the whole motion. These are the two blow-away flights: one flies back-first, the other along the path.
- Cecil for100 052 (10021) has 2..14 `{180, 0}`, probably a dash-type move.

**Confidence:** strong. The math is clear; that +0xF0 = velocity is inferred from the store at 0x0883B380.

**Test:** give a jumping or dash attack `0x0210 {180, 0}`. The fighter should pitch up and down along its arc. With `{10, 0}` the turn should be smooth and lag behind.

### 0x004B — Heavy body: push mass 1000 instead of 100
**Kind / args:** ranged, no args.

**What the handler does (0x0882C3A8):** sets `+0x196 |= 1`.

**Who uses it / effect in game:** only FUN_08838E24 (the mass getter, read at 0x08838E28) returns 1000.0f if the bit is set, else 100.0f. Its only callers are 4 calls in the push-apart resolver FUN_0883E3BC (0x0883E60C..0x0883E630).
- In modes 1/2 the overlap is split by mass: other moves `pen × mSelf/(mSelf+mOther)`, self moves `pen × mOther/(…)` (see out1/B.md).
- A 1000-mass fighter against a 100-mass one moves 1/11 of the overlap, and the other moves 10/11. Two flagged fighters split it evenly again.

**Data check:** 53 uses, all over whole character moves. Examples:
- Firion two100 050 (10050) 0..116 and 055 (10093) 0..166;
- CoD thr200 054/055/060 (10090/10091/10096) 0..65535;
- Kain for110 057–059 4..100–128;
- Cloud sev100 063/064 (10093/10094).

These are rushing or big attacks that should shove the opponent rather than be stopped by it.

**Confidence:** confirmed (setter → mass getter → push split).

**Test:** give a slow forward-walking attack `0x004B` over the whole motion and walk it into a standing CPU. The CPU should be shoved away while the attacker barely slows. Without the op, both should share the push.
