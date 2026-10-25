# Ops B: 0x0029, 0x0032, 0x0049, 0x0059, 0x006A

**Summary**
- 0x0049 lets this fighter win body pushes, so other fighters get pushed off it (field-action motions). 0x0059 is 0x0028's "free to act" bit, but it is only set while the target is not in a 500–798 reaction script. 0x006A blocks the Assist cancel and the EX-Revenge forced cancel.
- 0x0032 makes grounded movement follow the floor plane (a slope fix used in the floor clamp). 0x0029 is read only by CPU AI. It marks a wind-up or threat phase that changes how the CPU treats the attacker.
- Confidence: 0x006A, 0x0059 and 0x0049 strong (code end to end, system names from task-name strings). 0x0029 strong for "AI only", weak for its exact meaning. 0x0032 medium.

Conventions: the dispatcher's a1/s1 is the motion player at actor+0x130 (confirmed: FUN_08AC97A0 builds `s2 = actor+0x130; s6 = s2+0x60` before reading the bit at 0x08AC9810). So `s1+0x60+n` = actor+0x190+n, the per-tick flag block that FUN_0882FBFC clears (called from FUN_08829848). The ranged events below are therefore "on" only while they run. All five handlers ignore args, `x`, and the first and last tick (s4 and a0).

Flag setters, from scanning the ranged dispatcher (useful context): 0x0028 sets +0x194|=4, the same bit as 0x0059. 0x0069 sets +0x197|=0x10, the opposite of 0x006A. 0x041E sets +0x193|=1. 0x004B sets +0x196|=1 (mass 1000).

---

### 0x0049 — Push priority: others are pushed off me (body-collision anchor)
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882C380: `actor+0x195 |= 0x80`.
- **Who uses it / effect in game:** the only reader is the body push-apart resolver FUN_0883E3BC (reads at 0x0883E454, 0x0883E4B4, 0x0883E518). The chain to it is FUN_08833360 (for each other fighter: skip self, skip kind 3) → FUN_0883E904 (for each overlapping collision capsule pair, actor+0x468 lists) → FUN_0883E3BC(self, other, accum, penetration).
  - For each side, "anchored" = (actor+0x388/0x38C & a 64-bit mask stored at 0x089D8140) OR `+0x195&0x80`. Mode is 1 if only self is anchored, 2 if both or neither are, and 3 if only the other is. Two more masks on self (0x089D8138 and 0x089D8130) override this to 3 or 0. These mask words are zero in the file image and are set at runtime; I could not find who sets them.
  - Mode 1/2: both sides move, weighted by mass. FUN_08838E24 gives mass 1000 if `+0x196&1` (op 0x004B), else 100. The other side moves by pen·mSelf/(mSelf+mOther): it is written to other+0x2D4 (0x0883E6B4) and its capsules move via FUN_0883E2F4. Self moves by −pen·mOther/(mSelf+mOther).
  - Mode 3: self takes the whole push and the other side does not move. Mode 0: nobody moves (the vector is replaced from 0x09AF84C0, assumed to be a zero vector in BSS).
  - So when a non-flagged fighter resolves against a flagged one, the non-flagged fighter is pushed out fully and the flagged one stays put. In the flagged fighter's own pass, the push is the normal mass-weighted one, so the result depends on update order (partial anchor).
- **Data check:** appears over whole motions in the shared field-action motions of all three characters: motions 8–15 (scripts 200–221: FACT_LEAP/FACT_GRIND/FACT_GRIND_TURN hit volumes, i.e. rail grind and leap) and motion 37 phases 2 and 3 (scripts 240/242/244, `pad` = phase). Examples: noctis p_noc100 008, 013, 037; viv100 010; ysh100 014. This fits: a fighter on a field action should not be shoved off its path by body contact.
- **Confidence:** strong (the code is clear). Not yet tested in game. Test: give a ground attack 0x0049 over the whole motion, walk the opponent into it, and check that the opponent slides off while the attacker does not move.

### 0x0059 — Free to act (conditional): opens 0x0028's window only if the target is not in a reaction
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B2E4 first calls actor vtable +0x130. In every fighter vtable (0x089C7C78, 0x089C76A8, 0x08A61CF8, 0x08AB71A0, 0x08B3A9A8) that entry is 0x08836D9C, which returns actor+0x300 (the current target).
  - If there is no target, nothing happens.
  - If FUN_08838074(target) is true (target+0x180 script id in 500..798), nothing happens.
  - Otherwise it sets `actor+0x194 |= 4`. This is exactly what op 0x0028 does unconditionally (0x0882B2D0).
- **Who uses it / effect in game:**
  - FUN_08B3A5BC ("can this fighter act now?") returns 1 straight away if `+0x194&4` (0x08B3A67C) or `+0x193&1` (op 0x041E). Otherwise it goes on to the normal end-of-action checks, and it returns 0 while the fighter's own 0x180 is in 500..798.
  - Callers: FUN_08B3315C and FUN_08B2D84C, both from the fighter control FUN_08B3526C. FUN_08B2D84C uses the result to drop back to neutral (0x08A54CD0 air, 0x08A588FC ground). FUN_08AB344C pairs it with input ids 0x10E/0x110 (ground) and 0x10F/0x111 (air), and the CPU uses it widely. FUN_08A72A28 is another caller.
  - CPU AI FUN_08A64400 at 0x08A646B4 skips its "set AI state 1" branch when `+0x193&1` or `+0x194&4` is set.
  - Scripts 500–798 behave like hit reactions: FUN_08B3A5BC treats them as "not actionable", and the floor clamp at 0x0883C1EC does not turn them into a normal fall. This is a guess, but a fairly safe one.
  - Net effect: from the start tick on, the attacker can cancel recovery into movement, guard and other actions, unless the target is currently reeling.
- **Data check:**
  - Vivi spells (Shantotto charge spells, motions 50–61): several 0x0059 events start at or just after each chain window (for example 96/114/116→end against chain windows 76–96, 92–112, 98–118 in viv100 050).
  - Y'shtola 049/050/056/057/061/062: from mid-chain-window to the end.
  - Noctis 045 (10003): 30→end, with the chain window 30–52.
  - This fits an early "free to act" window late in the move.
- **Confidence:** strong for the mechanism (handler, bit and consumer all traced). What "target in 500..798" means is inferred. Test: make a whiffed attack and the same attack on a stunned dummy, and press guard or move from 0x0059's start tick. You should be able to act early only when the target is not in hitstun.

### 0x006A — No Assist cancel / EX-Revenge cancel during this range
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B1B0: `actor+0x197 |= 0x20`. Its counterpart 0x0069 (0x0882B19C) sets `|= 0x10`, which forces the opposite.
- **Who uses it / effect in game:** two identical predicates read it, FUN_08AC97A0 (0x08AC9810) and FUN_08B02A88 (0x08B02AF8). Both check, in order:
  - The fighter has `+0x380&8` (FUN_088377C0) and `+0xD44 == 0`.
  - If `+0x197&0x20`, the answer is false. If `+0x197&0x10`, it is true.
  - Otherwise it checks the current attack record (FUN_08B30898: +0x18 bits 0x800/0x400, +0x1C), then the motion-event window check FUN_0882FE7C → FUN_0882E764.
  - Assist path: ASSIST_MANAGER::Update, task registered at 0x0888DB14 with name string 0x089CA470 "ASSIST_MANAGER::Update", runs FUN_08B01B74 → FUN_08B0165C → FUN_08B0203C. For each player with `+0x5B7&0x40` it sets `+0x5B7|=0x80` when the predicate is true (else clears it). Each tick, the fighter update FUN_08B37604 sees `0x5B7&0x80` and calls FUN_088303E4(motion player, 0). That forces `+0x190|=1`, `+0x193|=1|2` (the "can act" flags), i.e. an Assist cancel of the current move.
  - EX Revenge path: EX_REVENGE_MANAGER::Update, task at 0x0888D400, name string 0x089CA128, runs FUN_08AC949C → FUN_08AC9230 → FUN_08AC9D7C → FUN_08AC96A0. This calls the predicate for the fighter and for fighter+0x1108, and for both their hit-list entries (0x08AC9FCC/0x08ACA050/0x08ACA0E0). When the predicate is true it sets `+0x5B7|=1`, plus `|=4` if FUN_088301D4 finds an active chain-type event (op codes 0x01/0x67/0x78/0x79) at the current time. FUN_08B37604 then forces the can-act flags (FUN_088303E4), and with bit 4 also `+0x190|=4` (the chain window bit, FUN_08830430).
  - So 0x006A makes the move uncancellable by these two system-level cancels for as long as it runs.
- **Data check:** Vivi's spells cover most of the motion (viv100 050: 0–150 of 174; 047: 0–74). Many Y'shtola moves have it over the whole motion or a long range. Noctis has it on 066 (AIR_FINISH, 0–96) and the HP attacks 068 and 070 (0–120). The ranges cover the startup and active parts of the move and end before the late recovery, which fits "can't be assist-cancelled until here".
- **Confidence:** strong for the code path and the names (taken from the task-name strings). The exact player-facing trigger (Assist call during your own move vs. the EX Revenge freeze) is from the manager names, not tested. Test: call an Assist during Vivi's spell inside and outside the 0x006A range. Inside, the caster should not regain control early.

### 0x0032 — Ground movement follows the floor plane (slope hugging) this tick
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B724: `actor+0x194 |= 0x40`.
- **Who uses it / effect in game:** the only reader is the getter FUN_088379FC (`(+0x194 & 0x40) != 0`), and its only caller is the floor clamp FUN_0883B470 at 0x0883BA48. That code runs in the fighter path when the fighter is not in state 3/0x17 (0x0883B940..). It requires `actor+0x380 & 0x02000000`, a bit FUN_0883B470 sets at 0x0883C27C when the fighter ended the last tick snapped to the ground, so it means "grounded last tick". When both are true:
  - it queries the floor at position+1 (FUN_088976E4, type 0x22);
  - it builds a vertical line from the intended new position raised by 100 (0x0883BAFC);
  - it intersects that line with the floor triangle's plane (FUN_088737B0, a ray/plane intersect);
  - if they intersect, it replaces this tick's displacement (sp+0x70) with (hit point − position) before the normal floor probe FUN_0889CF04.
  - Result: the move vector is projected onto the floor plane under the fighter. The fighter slides along slopes instead of stepping off them into a fall or pushing into them.
- **Data check:**
  - Noctis brave and HP attacks often have it for ticks 0–2 only (045, 049, 052–064, 086): it plants the fighter on the floor plane as the move starts.
  - Vivi ground melee has it around the lunge (043: 10–30 with hit 16–24; 044: 6–26; 045: 6–24).
  - Field-action motion 37 phase 3 (script 242) has it over the whole motion.
  - This is consistent with "keep root-motion travel glued to the floor while moving".
- **Confidence:** medium. The code path is clear, but the projection geometry is read from VFPU helper calls (0x089A38C4 add, 0x089A3528 sub). The plane comes from the earlier floor query's result (sp+0x30/0x34), which I did not fully decode. Test: do a lunging ground attack on a slope with and without 0x0032 over its travel ticks. Without it, the fighter should lift off or step at slope changes.

### 0x0029 — AI hint: wind-up / threat phase (read only by CPU AI)
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B1E0: `actor+0x193 |= 8`.
- **Who uses it / effect in game:** only two CPU-AI functions read it, and both read it on the opponent of the AI fighter. No movement, hit or physics code reads it: a full scan of the EBOOT and the overlay for byte, halfword and word reads found nothing else.
  - FUN_08A71064 (0x08A710F8): if the AI's target has the bit, the AI skips its condition 0x1A check and goes straight to FUN_08A6CB5C(ai, target.pos, 1). That is a repositioning routine: it computes the direction to the target, then calls FUN_08A6C850/FUN_08A6C920 → FUN_08A877F8 "move to point". Whether it closes in or backs off is not resolved.
  - FUN_08A8310C (0x08A831F4; called from FUN_08A63C8C and FUN_08A83590) checks whether the other fighter's attack volumes (FUN_088350AC) will reach the AI fighter (FUN_08812D48). If the other fighter has the bit and AI field +0x1AC == 1 (plus two other zero conditions), it returns 0 ("no threat / skip") and does not run the volume check.
- **Data check:** always from tick 0 to around the release or first hit of big or ranged moves:
  - Noctis 050 FF02_LONG 0–46; 067 LAND_FINISH 0–110 (hit at 106); 069 HP_A_BLADE 0–34 (hit at 34); 087/088 warp-dash 0–32 (hit at 36).
  - Vivi spells 0–42…66.
  - Y'shtola 043 (whole), 048, 055, 069–074.
  - This fits "startup/telegraph phase" that the AI reacts to.
- **Confidence:** strong that it only affects CPU behaviour (no other readers). Weak on the exact AI reaction. Leaving it out should not change anything for a human player. Test: CPU vs. a modded move with and without 0x0029. Watch whether the CPU moves or evades differently during the wind-up.
