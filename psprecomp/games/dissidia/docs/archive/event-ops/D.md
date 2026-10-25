# Agent D: ops 0x042A, 0x040B, 0x041E, 0x0120, 0x0467

**Summary**
- 0x042A attaches a **looping effect from the character's own effect list to a bone, for as long as the range lasts** (confirmed). arg0 = slot 0..15, arg1 = effect-record id (9999 = the weapon-trail records, 8888 = other weapon/hand effects), arg2 = which record (bit in record +0x44), arg3 = float scale (0 = 1.0). 0x040B **turns the fighter toward its opponent (actor+0x300)** by up to arg0 degrees per tick (confirmed). 0x0120 **opens a Chase window** when the opponent was last hit by atkp arg0 and is now in a blow-away reaction (confirmed).
- 0x041E means **"can act now, from any state"**: it sets the can-act bit (+0x190 bit 0x01) plus an override bit (+0x193 bit 0x01) that skips the usual idle-state checks. It sits at the end of recovery motions (strong). 0x0467 is a **chain window that also adds action arg1 to a per-tick list of follow-up candidates**. It is NOT automatic: the action is picked only when the player's input matches that action's input bits (confirmed). arg2 lets the follow-up start even when the attack missed; arg3 lets the input through mid-action.
- Conventions used below: owner (s1) = motion player at actor+0x130, so owner+0x60 = actor+0x190. +0x542 = actor kind; FUN_0883739C returns true for kinds 0 and 1 (battle fighters). +0x180 (s16) = current **script id** (the same ids as motion_map.json). +0x2FC = current target. **+0x300 = opponent**, set both ways in FUN_0888BA0C (0x0888BC34/BC40 via setter 0x08836DA4). Delay slots were re-checked with the fixed psd.py.

---

### 0x042A — Weapon trail / bone-attached effect for the range ("weapon_trail")
**Kind / args:** ranged, 4 args `{slot, effectId, recordBit, scale}`.
- arg0 slot, 0..15 (values above 15 are ignored, 0x08B2E1A8). Retail uses 0..3. Use a different slot for each effect that runs at the same time; for example, Noctis 051 uses slots 1 and 2 for the right and left blade.
- arg1: the effect record id compared with record +0x24. **9999** = weapon-trail records: all share emitter resource #7 in Noctis, and attach to `r_wep*`/`l_wep*`. **8888** = other effects (resources 9/27/34/38 in Noctis; Vivi `r_hand` resource 49/50; Y'shtola resource 9).
- arg2: a bit index (0..31) into record +0x44. Each 9999 record has exactly one bit set, so arg2 picks **one specific record**.
- arg3: raw **float** scale (the handler loads it with `lwc1`). 0 or a missing value means 1.0 (0x0882A8C0–A8F4). Retail always passes 0.

**What the handler does (0x0882A8B0):** runs only if FUN_0883739C(actor) (kind 0 or 1). It calls FUN_08B33CD8, which is FUN_08B2E1A8(actor, arg0, arg1, arg2, f12 = scale, t0 = 0). That sets bit arg0 in **actor+0x6D8** and writes slot actor+0x5D8+arg0*16 = {arg1, arg2, scale, t0}. The mask actor+0x6D8 is cleared every tick in FUN_08829848 (0x088298C4 → FUN_08B33CD0, `sw $zero,0x6d8`) before the events run. So a bit stays set exactly while some 0x042A range on that slot is active.
- Op 0x055F (code 0x5F, 0x0882A918) is the same with a 5th arg passed as t0. FUN_0884E660 reads t0 as an **alternate actor pointer**: it supplies the attach position and the effect set (0x0884E758–E784). Not seen in our data.

**Who uses it (FUN_0884E660, called from FUN_088413D0 each frame):** for each actor (FUN_08841F84) it keeps a 0x48-byte state at 0x09B166F0+i*0x48: {active mask, script id when spawned, 16 effect handles}. For each slot:
- **Bit requested, not active:** call FUN_088A9E24 → FUN_088AA6F8 on the actor's effect set `*(*(actor+0x390)+0x34)+0x28`. That walks the 0x104-byte effect records and calls the filter 0x0884E888. The filter accepts a record only if all of these hold:
  - record+0x24 == arg1
  - record+0x34 == 0
  - record+0x44 has bit arg2
  - the record's attach bone, named by the string at record+0xA8 (e.g. "r_wep", "l_wep4") and resolved through actor+0x4C8 vtable+0x28, is a **visible draw part** (FUN_0886CC38, table at +0x10C)

  The first matching record is instanced (FUN_088AA4E0, 0x280-byte instance). It is positioned from actor+0x4C8 and scaled by FUN_088A72E8(h, s, s, s). The slot is then marked active and the current script id (+0x180) is stored.
- **Active, and the bit was dropped or the script id changed:** the effect is stopped (`h+0xDC |= 0x100`, FUN_088A6AA4) and the slot is freed (0x0884E700–E748, 0x0884E7F8–E830).
- Two ranges on the same slot that overlap or touch keep **one continuous** effect, as in Noctis 052 and 053.

**Data check:** I cross-checked every retail 0x042A against the moveset's effe section (scratch_D/chk42a.py). Every 9999/arg2 pair resolves to exactly one record. That record's own motion-name field (+0xC8) names the attack using it. Examples:
- Noctis 045 `{0,9999,0}` → r_wep `_603A`.
- Noctis 052 `{0,9999,21}` → r_wep `_632A`.
- Noctis 049 `{0,9999,9}` → l_wep2 `_610A`.
- Y'shtola: all 9999 uses → r_wep `_700A`; `{1,8888,1}` → r_wep `_641A`.

The ranges wrap the hit windows: they start a few ticks before and end at or just after the hit. **One mismatch:** Noctis motion 042 has `{1,8888,1,0}` (18–54), but Noctis's effe has no 8888 record with bit 1, so nothing spawns. It looks copied from Y'shtola 042 and is harmless.

**Confidence:** confirmed for the mechanism. "Trail" for 9999 is strong: weapon bones, one shared emitter resource, placed over the swings. The visual type of resource #7 itself is not decoded. **Test:** set arg3 to 2.0f (0x40000000) on one Noctis swing; the trail should double in size. Hide the weapon part during the range; the trail should not spawn.

---

### 0x040B — Turn toward opponent (max arg0°/tick)
**Kind / args:** ranged, `{maxDegPerTick, unused, allowVsDodging, skipIfOpponentHurt}`. Retail is always `{90,0,0,0}`.

**What the handler does (0x0882A08C):** `opp = vtable+0x130(actor)`. In vtable 0x08B3A9A8, slot +0x130 = 0x08836D9C, which returns **actor+0x300 = the opponent**; 0x0402 uses slot +0x120 = actor+0x2FC instead. The handler returns early in three cases:
1. `opp == 0`.
2. **arg3 != 0** and FUN_08838074(opp): the opponent's script id is 500..798 (hit reactions).
3. **arg2 == 0** and FUN_08837458(opp): the opponent's script id is 400..469, or opp+0x194 bit 0x10 is set. This block is lifted when the actor is in an action (+0x380 bit 8), is a fighter, and the current action record (FUN_08B30898 → +0x1020/+0x101C) has +0x18 bit 0x10.

Otherwise it calls FUN_08835F60(actor, opp, arg0·π/180). That takes the flat (y = 0) direction from actor+0x80 to opp+0x80; if it is longer than 0.01, FUN_08835CF8 rotates the actor toward it, clamped to that many radians. It also zeroes +0x3D0. arg1 is never read. 0x0402 uses arg1 to choose FUN_088360BC instead.

**Who uses it:** direct effect (actor yaw). Script range meanings are guesses: 400–469 = evade/guard family, 500–798 = hurt/knock/blow reactions.

**Data check:** used only in the Chase motions (scripts 810–813: Noctis/Vivi/Y'shtola 039–042) at ticks 0–16 (0–86 on 042), and on Y'shtola's summon objects w_ysh102–104. 90°/tick for 16 ticks snaps the attacker to face the blown-away opponent. That fits, because the target pointer can differ from the opponent during a chase.

**Confidence:** confirmed. **Test:** change arg0 to 1 on a chase motion; the chase strike should stop facing the opponent.

---

### 0x041E — Recovery cancel: can act now (bypasses state checks)
**Kind / args:** ranged, `{a, b, c, d}`. Retail is always `{0,0,0,0}` (60 uses).

**What the handler does (0x0882AF34),** with all offsets on the actor:
- always: +0x193 |= 0x01 and **+0x190 |= 0x01** (the same "can act" bit as 0x0005).
- arg0 != 0: +0x193 |= 0x02, +0x199 |= 0x04.
- arg1 != 0: +0x198 |= 0x04.
- arg2 != 0: +0x193 |= 0x02, +0x199 |= 0x04, +0x198 |= 0x40.
- arg3 != 0: +0x193 |= 0x02, +0x199 |= 0x04, +0x198 |= 0x80.

Variant 0x0058 (0x0882B020) does the same, but only if +0x384 bit 0x40000 is set.

**Who uses it:**
- **FUN_08B2F5E0** decides whether a grounded fighter may start an action. Callers: player input FUN_08B3526C at 0x08B35E44, and CPU AI 0x08A68980, 0x08A7082C and others. It returns 0 when airborne. When +0x193 bit 0 is set and +0x199 bit 4 is not, it **returns 1 at once** (0x08B2F658–F680). That skips the normal path at 0x08B2F698, which needs +0x190 bit 0, no hurt script, not in an action, and state (+0x3A4)→0 ∈ {1,6,8,0xC} or +0x384 bit 0x200.
- FUN_08B2FBCC (0x08B2FC58) has the same +0x193/+0x199 pattern; it is probably the airborne or other-state counterpart (guess).
- Other readers: FUN_08B2FF2C and FUN_08B2FD90 (+0x193 bit 0 **blocks** their "in-action cancel" path, 0x08B2FF7C and 0x08B2FDE0); FUN_08B3526C 0x08B368FC (sets or clears +0x5B9 bit 8 from +0x193 bit 0 and +0x199 bit 4); CPU AI 0x08AA0248… and 0x08A7C538.
- The args (all zero in retail) turn the override off again through +0x199 bit 4 and set extra bits. +0x198 bit 4 (from arg1) disables the hit-confirm cancel path in FUN_08B2FD90 (0x08B2FE6C). +0x198 bit 0x40 is read by FUN_08B382B8 at 0x08B3837C and by AI at 0x08A7D1CC. +0x198 bit 0x80 is read by AI at 0x08A7D298. I found no reader for +0x193 bit 0x02. **Exact arg semantics are unknown.**

**Data check:** always open-ended (to 65535) near the end of recovery-type motions:
- fall/land script 102 from tick 14
- 901/911 from 80
- 400–471 from 56/50
- 300 from 36/46
- 702–754 (blow-away recoveries) from 70
- 801 from 10

That fits "from here on the fighter may act out of this recovery".

**Confidence:** strong for the arg-0 form ("can act, state checks skipped"); weak or unknown for nonzero args. **Test:** move 0x041E earlier in a get-up motion (e.g. 031 from 70 to 30); you should be able to act or attack earlier out of the get-up.

---

### 0x0120 — Chase window (opponent blown away by this hit)
**Kind / args:** ranged, `{atkpId}`. The values (1121, 1122, 1123, 1183, 1396–1398, 1471, 1577, 1585, 2417–3243) are **hit-profile (atkp) ids**: Noctis 1121 SE_ATTACK_UPPER, 1123 CHASE_ATTACK, 1183 NEAR_COMBO2. Y'shtola's ids are not in her p_ysh hits.json; they probably live in the summon files.

**What the handler does (0x08829E0C):** only for fighters. It sets **+0x190 |= 0x10** when all of these hold:
- `opp = actor+0x300` exists.
- `opp+0x534` (a 0x310-byte struct with two 6×0x40 histories, built in FUN_088371C4) exists.
- **actor+0x380 bit 0x800** is set. This is set by FUN_08838734, the attacker's "my hit landed" handler, which sets 0x100|0x200|0x800.
- `*(opp+0x534+0x1A0)` (first entry of the second history, +0x20) is non-null and points to a record whose u32 +0 == arg0. Read as "the last hit the opponent took was atkp arg0" (strong guess on the history meaning).
- FUN_08837584(opp) is true: opp state (+0x3A4)→+4 bit 4, or the opponent's script ∈ {700, 701, 750, 751}, the blow-away reactions.

**Who uses it:** +0x190 bit 0x10 is read by:
- **FUN_08B33580** (0x08B3360C): true if the actor is not kind 1, the opponent is valid, the opponent is blown away, and the bit is set. Callers:
  - FUN_08B3526C at 0x08B35D8C and 0x08B360B0: when true, it calls FUN_08AB6348(…, actor, opponent, 0) and FUN_08B35018 or FUN_08B333F0. This **starts the chase**.
  - FUN_08B372D8 at 0x08B37524: sets actor+0x5C0 bit 0x200 while it is true (a likely chase-prompt flag).
  - CPU AI at 0x08A7F240 and elsewhere.
- FUN_08B3362C (0x08B33768) checks the same bit on actor+0x1108 (a partner object).

**Data check:** in the Chase motions 039–041 from tick 40 with the motion's own chase atkp (Noctis 039 → 1123 CHASE_ATTACK), so a chase can continue into a further chase. Also on combo finishers that blow the opponent away: Noctis 044 `{1183}` NEAR_COMBO2, reaction 12; Vivi 049/050/055; Y'shtola 069/072. That fits "after blow-away".

**Confidence:** confirmed (flag → chase start). **Test:** remove 0x0120 from Noctis 044; after the second combo hit blows the opponent away, the chase prompt or follow-up should no longer be offered.

---

### 0x0467 — Chain window + offer follow-up action (re-check)
**Kind / args:** ranged, `{chainFlag, actionId, noHitNeeded, letInputThrough}`. arg0 ∈ {0,1}; arg1 = action id in actions.json (all retail values resolve); arg2/arg3 ∈ {0,1}.

**What the handler does (0x08829C38):**
- +0x190 |= 0x04 (chain window, as 0x0101). arg0 != 0 adds +0x190 |= 0x08.
- For fighters only:
  - FUN_08B34DD4 selects the character's action table (FUN_08848078(*0x089D4974, *(actor+0x390)+0x88)).
  - FUN_0884808C finds the 0x74-byte action record whose u16 id == arg1.
  - **FUN_08B308D8 appends** {record ptr @+0x1028+8n, arg2!=0 @+0x102C+8n, arg3!=0 @+0x102D+8n} and increments s16 count +0x1068. There is no bounds check; the list holds 8 entries.
- The list is cleared every tick: FUN_08829848 → FUN_08B30904 (0x088298CC). So the entry exists only while the range is active.
- The notes' "hit list" at +0x1028/+0x1068 is actually this **follow-up candidate list**.

**Who uses it:**
- **FUN_08848984** (mid-combo move pick, reached only through FUN_0884A294 ← FUN_08B35218 / FUN_08B382B8, i.e. on an input). For each listed action it:
  - finds its table index;
  - requires the record's input bits (+0xF bits 1/2/4/8, plus +0x18 bit 0x80) to match the current input kind ctx+0x7C (0..6);
  - requires the stick-direction bits (+0x18 bits 0x20000–0x400000 vs ctx+0x78) to match;
  - requires air/ground (FUN_08847F10) to match;
  - requires **FUN_08B2FD90(actor, arg2)** to pass;
  - requires the chain check FUN_08AB81E0 to pass.

  If all pass, it marks the action as a candidate (ctx+0x60 bitset). **So the follow-up is offered for the matching button, not played automatically.**
- **arg2** goes to FUN_08B2FD90:
  - flag = 1: can-act (vtable+0x150 → FUN_08837B20, which accepts the +0x190 bit 0x04 chain window) and in an action (+0x380 bit 8) and not +0x193 bit 0.
  - flag = 0: additionally needs +0x380 bit 0x1000 (set in FUN_08838734 on a landed hit, strong guess) or +0x384 bit 0x08000000, and not +0x198 bit 4.

  So **arg2 = 1 lets the follow-up start even if the attack missed**. It acts like action-record flag +0x18 bit 0x100 (FUN_08B30930, used in FUN_08B309C8 / FUN_08B30E10 and AI at 0x08A7760C).
- **arg3** makes FUN_08B3097C true. In FUN_08B309C8 / FUN_08B30E10 (the "let the Brave/HP input through mid-action" checks) that returns 1, acting like record flag +0x18 bit 0x200. Read: **the input is accepted (buffered) during this window**.

**Data check:**
- Y'shtola 043/048/055/060: two entries over the same range, `{0,A,1,1}` + `{0,B,1,1}`. These are alternative follow-ups, presumably for different inputs or directions.
- Y'shtola 049/050: three consecutive 12-tick windows (20–32, 30–42, 40–52), each offering a different action, `{1|0, id, 0, 0}`, so they need a landed hit. This reads as timing-dependent follow-ups.
- Noctis 063 `{0,3026,1,1}` from 196 and Noctis_a 011 `{0,3017,0,0}` at 48–72 (Armiger Assault steps).

**Confidence:** confirmed for list mechanics and input-gated selection; strong for arg2 (no-hit-needed); medium for arg3 and for +0x190 bit 0x08 (meaning not traced here). **Correction to the notes:** this is not an "automatic follow-up". **Test:** in Y'shtola 049, do not press anything during 20–52; the follow-up should not play. Then set arg2 = 1 on a window and whiff; the follow-up should become available.

---
Scratch: `/tmp/claude-0/re/scratch_D/` (ovl.s full overlay disassembly, scan.py flag-reader scanner, tab.py / u120.py / chk42a.py data tabulation).
