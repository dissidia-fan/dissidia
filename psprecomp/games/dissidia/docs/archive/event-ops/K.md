# Ops K (round 2): 0x016C 0x004C 0x0579 0x003D 0x0072 0x022E 0x0069 0x0173 0x013B 0x027B 0x0225 0x0031 0x004A 0x0051 0x0158 0x0652 0x0070 0x0071 0x0475 0x0476 0x012B 0x086E 0x0077

**Summary**
- Confirmed end to end: 0x0069 (forces the Assist / EX-Revenge cancel check to pass), 0x027B (a hit window that only works after this move's guard blocked something), 0x0579 (chain follow-up gated on a given atkp having landed), 0x004A (no jumping), 0x013B (gravity multiplier), 0x012B (white full-screen flash), 0x022E (weapon-trail length on a bone), 0x0475/0x0476 (pulsing colour tint on self / on partner or owner), 0x086E (turn toward target, or body aim).
- Strong: 0x016C (dash-cancel window, optionally only after a hit landed), 0x0158 (after a successful guard you may act at once: Jecht Block), 0x0072 (may block or start a guard-type move, as if neutral), 0x004C (an early press locks out the later timed-input windows: Jecht's timed combos), 0x003D (no drop shadow), 0x0173 (an assist turns toward its main fighter), 0x0652 (player-steered body lean), 0x0225 (turn-rate / move-speed multipliers), 0x0031 (EX Burst: release the held victim), 0x0070 (ground-contact branch to the action's +0x12 script).
- Weak or no effect found: 0x0071 (flag on self and partner that skips hit handling; the gameplay meaning is unclear), 0x0051 / 0x0077 and the flag bit of 0x0225 (the bit is set, but nothing in the EBOOT or the battle overlay reads it).

Conventions: actor = s2 (= *(owner+0x30)), and owner+0x60 = actor+0x190 (the per-tick flag block, cleared every tick). args are the u32s after `x`. "Fighter" means FUN_0883739C(actor) is true, i.e. actor+0x542 ∈ {0,1}. **Actor kinds** (new): 0 = main fighter, 1 = assist character (created by FUN_08A61C3C from FUN_08888354, which links main and assist through **+0x1108** both ways), 3 = summon (+0x5D4 = its owner).

New engine facts found on the way:
- **+0x384 bit 0x40000 = "my guard volume blocked an attack."** FUN_0884A53C sets it, together with 0x10000000, on the owner of hit object A when the hit-type interaction matrix at 0x089D4978 (16×16, read by FUN_0884A454 from the hit task FUN_08809F00) gives result 3 for (A, B). Rows 4/5/12/13/14 (guard-type volumes) give 3 against attacks. The getter is FUN_08837388.
- **+0x3D0 / +0x3D4 / +0x3D8 / +0x3DC** are multipliers, reset to 1.0 every frame by FUN_08833658 (from the per-frame pre-update FUN_08833910):
  - +0x3D0 = turn rate (FUN_08831360).
  - +0x3D4 = movement speed (FUN_08B2D2CC).
  - +0x3DC = gravity (air update FUN_08A5B724).
- **actor+0x76C = the weapon-trail system.** It holds a list of emitters, 0xE0 bytes each: +0x14 bone, +4 tip extent along the bone's X axis (reset each tick from +8), +0xC base extent. It also holds a ring of 0x140-byte segments that FUN_0884B908 draws. FUN_0884D658 builds the segments.
- **actor+0x1294..+0x12A0 = per-tick pulse tint** {period, R, G, B}. FUN_08B37604 at 0x08B37D6C turns it into model colour `0x80 + RGB·(1+sin(t/period))/2` (t = +0x318). +0x1294 is zeroed after use, so the tint lasts only while the event refreshes it.
- **FUN_08B2FBCC(actor, kind) = "may start a dash now."** Its kind-1 path → FUN_08A53290(actor, 3) = script 244 in state 0x13. FUN_08A53290 plays script 240/242/244 (0xF0/0xF2/0xF4). FUN_08A52FE4 picks the dash type from action ids 0x38/0x39/0x3A (ground) or 0x11B/0x11C/0x118 (air), with the air-dash count at +0x1075.
- **Base vtable 0x089C7C78**, 8-byte entries {s16 this-adjust, fn}:
  - +0x158 = FUN_08837B9C ("can block").
  - +0x150 = FUN_08837B20.
  - +0x120 → +0x2FC (current target); +0x130 → +0x300 (opponent).
  - +0x48 = FUN_08839D84 (drop shadow).
  - +0xB8 = FUN_08833B2C (create or refresh an attack object).
- **EX Burst controller:** global `*0x08A405E0`, module 0x08A3xxxx. Attacker at +0x26C, victim at +0x270; per-tick FUN_08A3FE10 → FUN_08A3D904.

---

### 0x0069 — Assist / EX-Revenge can cut this move short (forced)
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B19C: `actor+0x197 |= 0x10`.
- **Who uses it / effect in game:** this confirms round 1 (B.md). The two predicates FUN_08AC97A0 (EX_REVENGE_MANAGER) and FUN_08B02A88 (ASSIST_MANAGER) read it the same way:
  - First: the actor is in an action (FUN_088377C0) and +0xD44 == 0.
  - Then they test owner+0x60+7 = actor+0x197:
    - bit 0x20 (0x006A) → return false (0x08AC9814 / 0x08B02AFC);
    - otherwise bit 0x10 → **return true at once** (0x08AC9834 / 0x08B02B10), skipping the attack-record flags (+0x18 bits 0x800/0x400, +0x1C) and the event-window check (FUN_0882FE7C).
  - A true result makes ASSIST_MANAGER set +0x5B7|=0x80 (Assist cancel). EX_REVENGE_MANAGER sets +0x5B7|=1 (and |=4 inside a chain window). The fighter update then forces the can-act flags (FUN_088303E4).
  - 0x006A wins if both are set.
- **Data check:** 5 uses, always from mid-move to the end:
  - Cloud sev100 064 (script 10094): ticks 10–end.
  - Squall eht100 064 (10083/20002): 16–end; 069 (20012): 18–end; 073 (10093/20004): 16–end.
  - So the move's late part is always assist-cancellable, whatever its record says.
- **Confidence:** confirmed (code, both predicates). **Test:** in Cloud's 10094, call an Assist at tick 12. It should cut the move; with the event removed it might not.

### 0x027B — Counter hit window: attack active only after this move's guard blocked something
- **Kind / args:** ranged, `{atkpId, hitGroup}`, the same layout as hit_window 0x0200.
- **What the handler does (0x08829AE4):**
  - Skip if `(+0x388/+0x38C) & mask(0x089C7908) = bit 54` (the same test as 0x0200 at 0x08829A80).
  - Then, only if **+0x384 & 0x40000** (guard succeeded), it does exactly what 0x0200 does:
    - FUN_08833B80(actor, arg0, arg1): +0x448 |= 1<<arg1 (64-bit, FUN_089985B0), then vtable+0xB8 → FUN_0883E108(*(actor+0x3AC), slot arg1, atkp arg0, actor), which creates or keeps the 0x220-byte attack object.
    - FUN_0882FD08(owner, start, end): records the window in owner+0x170/+0x174.
- **Who uses it:** the hit task, as for 0x0200. The gate bit is set by the clash/guard resolver FUN_0884A53C (result 3), see the engine facts.
- **Data check:** only Exdeath (fiv200), on his guard motions, over the whole motion:
  - 056 (scripts 10020/10028) `{1223,10}`;
  - 057 (10021/10040) `{1224,11}`;
  - 062 (10026/10044) `{1226,13}`.
  - A counter hit that comes out only when the guard caught an attack fits Exdeath's guard-counter moves.
- **Confidence:** confirmed for the mechanism. The meaning of "result 3 = guard success" is strong, from the matrix rows and the users. **Test:** Exdeath's guard 056, once blocking an attack and once whiffing. Atkp 1223 should only hit after a block.

### 0x0158 — After a successful guard: may act at once (Jecht Block)
- **Kind / args:** ranged, `{alsoDashAndJump}`. Retail uses 0.
- **What the handler does (0x0882B020):** only if **+0x384 & 0x40000** (guard succeeded):
  - `+0x193 |= 1` (the "act now" bit, as 0x041E) and `+0x190 |= 1` (can_act).
  - If arg0 != 0, it also sets `+0x193 |= 2` and `+0x199 |= 4`.
- **Who uses it:**
  - +0x190&1 / +0x193&1: the can-attack / can-block gates (FUN_08837B9C etc., see EVENT_OPS).
  - +0x199&4 together with +0x193&1:
    - makes FUN_08B2FBCC (may dash) return true at once (0x08B2FC90);
    - in the fighter update at 0x08B3691C, sets +0x5B9|=8 before FUN_08A5B2B4 (jump path);
    - is read by the CPU AI at 0x08A7C558.
  - No reader of +0x193&2 was found.
- **Data check:** only Jecht (ten200), over whole motions:
  - 054 (scripts 10020–10022);
  - 055 (10070–10072).
  - These fit his "Jecht Block" stances (ground/air), which can be followed up right after blocking.
- **Confidence:** strong. **Test:** Jecht Block a projectile, then press ○ at once. You should attack immediately; without the event you should not.

### 0x016C — Dash-cancel window (optionally only after a hit landed)
- **Kind / args:** ranged, `{onlyAfterHit}`. 0 or 1.
- **What the handler does (0x0882B350):** `actor+0x197 |= 0x40` if arg0 != 1, or if `actor+0x380 & 0x100`. FUN_08838734, the "my hit landed" handler, sets that bit (0x100|0x200|0x800).
- **Who uses it:**
  - FUN_08B2FBCC(actor, 1) (0x08B2FCBC) returns true when +0x197&0x40.
  - The fighter update at 0x08B367DC–0x08B3682C: if FUN_08B2FBCC(actor,1) is true, and the dash input check FUN_08B30D68 is true (controller command mask 0x00800000 at 0x08B3AC20, or CPU command 0x15), and +0x5B6&0x80 is not set → **FUN_08A53290(actor, 3) → script 244 (0xF4), state 0x13**, i.e. a dash out of the move.
- **Data check:** only Kain (for110 / for110_a):
  - arg 1 from late in his attacks to the end, e.g. 043 (10000) 52–end, 045 (10006) 38–end, 047 (10011) 82–end, 052–054 (10025–10027) 48–end;
  - arg 0 on 069 (10075) from 100.
  - Script 244 is in Kain's dash motion 037 (scripts 240/242/244/800).
- **Confidence:** strong. The button behind mask 0x00800000 is inferred. **Test:** Kain's 10006: hit, then press dash after tick 38 → he dashes out. Whiff and try again → he can't (arg 1).

### 0x0579 — Chain follow-up if a given attack has landed
- **Kind / args:** ranged, `{chainFlag, followupActionId, evenOnMiss, inputMidAction, atkpId}`.
- **What the handler does (0x08829D6C):**
  - Runs only if FUN_08839120(actor, arg4) is true. That checks the actor's **own** hit history `*(actor+0x534)`, first 6×0x40 list (the "hits I landed" list, which FUN_08838734 → FUN_088393F4 pushes to), for an entry whose record (entry+0x20) has u32 +0 == arg4 (atkp id).
  - If so, it does exactly what 0x0467 does:
    - `+0x190 |= 4` (chain window), `|= 8` if arg0;
    - for fighters: FUN_08B34DD4, FUN_0884808C(action table, arg1) → FUN_08B308D8(actor, rec, arg2!=0, arg3!=0), which adds the follow-up candidate (D.md).
- **Who uses it:** the same consumers as 0x0101 / 0x0467 (chain check and follow-up list in the move picker).
- **Data check:** only gst110, motion 073 (scripts 10117…10140), whole motion, 10 events with `{0, action, 0, 1, atkp}`. Pairs: 2734→2802, 2735→3311, 2813/2816/2819/2822→3067, 2814/2817/2820/2823→3312. Each follow-up is offered only if its matching atkp hit.
- **Confidence:** confirmed (mechanism). **Test:** in gst110's 073, land only atkp 3067 → only actions 2813/2816/… are offered.

### 0x0072 — May block / start guard-type moves (counts as neutral for the block gate)
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B00C: `actor+0x198 |= 2`.
- **Who uses it:**
  - **FUN_08837B9C** (base vtable +0x158, "can block"; via the player wrapper FUN_08B2FB6C): treats +0x198&2 like can_act (0x08837C04). It returns true unless FUN_08837310 (+0x384&0x20000, or script 0x1D6/0x1D7).
  - **FUN_08B2F9F4** (the player's attack-start gate): +0x198&2 → true (0x08B2FAAC), unless the 0x004C latch +0x5B4&2 is set.
  - **Move picker FUN_08849B50** (0x08849C38): sets context+0x81 = 1. FUN_08849700 then uses the fresh-start slot logic FUN_0884921C instead of the mid-combo chain logic. FUN_088483E4 then only allows records with +0x18 bit 0x4000.
  - CPU AI at 0x08A9C4C8.
- **Data check:** only Exdeath (fiv200 / _a), from tick 0:
  - 046 (10010/10060) 0–56;
  - 047 (10011/10061) 0–68;
  - 048 (10012) 0–40;
  - 049 (10013) 0–90;
  - 050 (10014/10064) 0–70;
  - 067 (10093/10096) 0–end.
  - Exdeath's moves are guard-button moves, so letting him start another guard-type move during the stance fits.
- **Confidence:** strong for the code. Record bit 0x4000 = "guard-type command" is inferred. **Test:** during Exdeath 048 before tick 40, press block plus a direction. He should switch guards; after tick 40 he should not.

### 0x004C — Early-press lockout for timed inputs (Jecht)
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882C3BC: `actor+0x196 |= 4`.
- **Who uses it:** the fighter update FUN_08B3526C, at 0x08B357EC.
  - s1 = "a command input was registered this tick". The input checks FUN_08B30E10, FUN_08B309C8, … set s1 and the matching bits in +0x388/+0x38C.
  - If s1 and +0x196&4, it **latches `+0x5B4 |= 2`**.
  - While the latch is set:
    - FUN_08B334D0 returns 0 instead of +0x195&1 (op 0x37);
    - FUN_08B33508 returns 0 instead of +0x194&0x80 (ops 0x36/0x3A);
    - FUN_08B33544 returns 0 instead of +0x195&2 (op 0x3A);
    - the attack gate FUN_08B2F9F4 returns 0.
  - The first three gate the input-triggered follow-ups in the fighter update (0x08B363AC, 0x08B364F0, 0x08B365A0 → FUN_08B2D664).
  - So pressing during this range voids the later input windows. I did not find where +0x5B4 bit 2 is cleared; probably when a new action starts.
- **Data check:**
  - Jecht ten200: 048 (10006) 0–42, 049 (10007) 0–56, 053 (10015) 0–34. Jecht's attacks have timed button presses, so mashing early fails.
  - gst130: many scripts on motions 048/049/055/056, 0–18.
- **Confidence:** strong for the code; "timed input" is from the usage. **Test:** Jecht 10007: press ○ at tick 30 (inside) vs tick 60. The follow-up should fail in the first case.

### 0x004A — No jumping
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882C394: `actor+0x196 |= 2`.
- **Who uses it:**
  - FUN_08B2F5E0 (ground "can jump", 0x08B2F644) and FUN_08B2F7E0 (air jump, 0x08B2F810) return false when it is set.
  - The fighter update at 0x08B35E44 / 0x08B35E98 uses them to start the jump: script 100, then FUN_08834934(actor,101,102); or the air jump.
  - Many CPU callers (0x08A68980, …) use them too.
- **Data check:** Kain for110 037 (dash motion, scripts 240/242/244/800) 0–40, also on _a. You can't jump out of the first 40 ticks of his dash.
- **Confidence:** confirmed. **Test:** jump during Kain's dash before and after tick 40.

### 0x013B — Gravity multiplier
- **Kind / args:** ranged, `{float scale}`.
- **What the handler does:** 0x0882BAC4: `actor+0x3DC = arg0`. FUN_08833658 resets it to 1.0 every frame, so it applies only during the range.
- **Who uses it:** the air movement update FUN_08A5B724 (called from FUN_08A54A6C / FUN_08A5BC58):
  - the gravity term is multiplied by +0x3DC (0x08A5B918–0x08A5B930);
  - the fall-speed clamp is multiplied by it when < 1 (0x08A5B950–0x08A5B97C).
- **Data check:**
  - Emperor two200 051 (10090/10093) `{0.01}` from 40 → hangs in the air.
  - Kuja nin200 056 (10094) `{0.25}` whole motion (_a too).
- **Confidence:** confirmed (field → physics). **Test:** set 0.01 on any air attack; the fighter should float.

### 0x022E — Weapon-trail length on a bone
- **Kind / args:** ranged, `{bone, float extent}`. Fighters only.
- **What the handler does (0x0882AC74):** FUN_0884BB00(actor+0x76C, actor, arg0, arg1). For every trail emitter (0xE0 records) whose +0x14 (bone) == arg0, it sets +4 = arg1.
- **Who uses it:** FUN_0884D658 builds each tick's trail segment from that bone's matrix (FUN_088369A4). The tip is at +4 along the bone's X axis, the base at −(+0xC). Then +4 is reset from +8 (0x0884D8E0), so the override lasts only for the range. FUN_0884B908 draws the ring (additive/sub sprites).
- **Data check:**
  - WoL one100 059 (10093) `{55, 5.0}` 52–54;
  - Cloud sev100 044 (10007) `{50, 1.65}` 12–18 and 061 (10091) `{50, 5.0}` 36–end;
  - Squall eht100 065 (10094) `{46, 9.5}` 36–44.
  - These are short spans on big swings (a longer trail).
- **Confidence:** confirmed (code). That the emitter list is the weapon trail is strong (sprite ribbon). **Test:** change Cloud's 044 to 5.0; the trail should reach much further past the blade.

### 0x0475 / 0x0476 — Pulsing colour tint (self / partner or owner)
- **Kind / args:** ranged, `{R, G, B, period}` ints.
- **What the handlers do:**
  - 0x0475 (0x0882CDA4): fighters only, FUN_08B34FE4(actor, R, G, B, period). That stores floats +0x1298 = R, +0x129C = G, +0x12A0 = B, +0x1294 = period.
  - 0x0476 (0x0882CDD8): the same, applied to the **summon's owner** (+0x5D4) if kind 3, or the **partner** (+0x1108) if kind 1 (assist). Main fighters: nothing.
- **Who uses it:** FUN_08B37604 at 0x08B37D6C. If period > 0:
  - colour = 0x80 + RGB·(1+sin(t/period))/2, with t = +0x318 (running timer), mode 3;
  - +0x1294 is cleared.
  - The colour then goes to +0x107A..C (clamped 0..255) and fades with +0x5C8 = 30.
- **Data check:** Aerith (sev120_a, assist-only) 010 (10070) whole motion, both ops `{255, 218, 0, 2}`: a golden pulse on Aerith and on her main fighter.
- **Confidence:** confirmed (path); the exact visual is strong. **Test:** set 0x0475 to `{0,0,255,2}` on any motion → a blue pulse.

### 0x0173 — Assist turns toward its main fighter
- **Kind / args:** ranged, `{maxDegPerTick}`.
- **What the handler does (0x0882A33C):** only if actor+0x542 == 1 (assist) and +0x1108 (partner) exists: FUN_08835F60(actor, partner, arg0·π/180). That is the same turn routine as 0x040B: a flat turn toward the target, clamped per tick, and it zeroes +0x3D0.
- **Data check:** Aerith (sev120_a):
  - 008 (10000) `{180}` 0–2 then `{10}` 2–22;
  - 010 (10070) `{180}` 0–2 then `{10}` 2–end.
  - She snaps, then tracks her partner.
- **Confidence:** strong (turn routine traced in D.md). **Test:** Aerith's assist 10070 should keep facing the caller as they move.

### 0x0071 — Flag on self and partner that skips hit handling (meaning unclear)
- **Kind / args:** ranged, 0 args.
- **What the handler does (0x0882CD4C):** `actor+0x197 |= 0x80`. If the actor is an assist (kind 1), also partner(+0x1108)+0x197 |= 0x80.
- **Who uses it:**
  - FUN_08B3526C (0x08B35278/0x08B352C0) copies "self or partner has it" into `+0x188 |= 0x20` each tick.
  - Readers of (0x197&0x80 || 0x188&0x20):
    - FUN_0883A6FC returns at once (0x0883A764; the hit/reaction processing called from FUN_0883A150 ← FUN_08B2DE00);
    - FUN_0883A150 (0x0883A4A8);
    - the attacker-side on-hit extras in FUN_08B33938 are skipped when the **other** fighter has it (0x08B339BC): FUN_08842D18 on *(attacker+0x52C)+8, two FUN_088AA0C0 calls, FUN_08858CB8;
    - several CPU AI routines (0x08AE1A58, 0x08AE2688, 0x08AE2958, 0x08AE47F4/48F0/5A74, 0x08AE5C14).
- **Data check:** Aerith 010 (10070) whole motion only, together with the golden tint on her partner. That suggests a support move whose contact with the partner shouldn't count as a normal hit.
- **Confidence:** strong for the flag flow; weak for the gameplay meaning. **Test:** Aerith 10070: does the main fighter flinch or take damage from it, with and without the event?

### 0x003D — No drop shadow
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882BC04: `actor+0x195 |= 4`.
- **Who uses it:** FUN_08839D84 (vtable +0x48, in six vtables). It positions the shadow object +0x4A8 on the floor and draws it through FUN_088768D0. It returns early when +0x195&4 is set (0x08839E28).
- **Data check:**
  - Golbez for200 002 (script 100) / 005 (103) 0–6, 045/049/057 0–12;
  - CoD thr200 061 (10097) 4–20;
  - Exdeath fiv200 074 (1100) 0–48;
  - Shantotto gst100 069 (1210) 120–end.
  - These are short starts of warp / teleport-like motions.
- **Confidence:** strong. That the routine draws the drop shadow is inferred from the shadow object, floor height and draw call. **Test:** add it to a stand motion; the shadow under the fighter should disappear.

### 0x0225 — Turn-rate / move-speed multipliers (plus an unread flag)
- **Kind / args:** ranged, `{float turnMul, float speedMul}`.
- **What the handler does (0x0882ADE4):** `+0x192 |= 0x20`, `+0x3D0 = arg0`, `+0x3D4 = arg1`. Both are reset to 1.0 every frame.
- **Who uses it:**
  - +0x3D0 scales the yaw turn toward +0x320 in FUN_08831360 (0x08831460).
  - +0x3D4 scales the speed +0xBC in FUN_08B2D2CC (0x08B2D354/0x08B2D370).
  - No reader of +0x192&0x20 found.
  - A sibling case at 0x0882AD08 sets +0x192|=8 with the same two floats (another op).
- **Data check:** Squall eht100 068/070/071 (scripts 20011/20014/20013), whole motion, `{1.0, 1.0}`. With 1.0/1.0 this has no visible effect unless the 0x20 bit is read somewhere I didn't find.
- **Confidence:** strong for the multipliers; unknown for the bit. **Test:** set `{0, 0.2}` on a run/move motion → no turning, slow movement.

### 0x0031 — EX Burst: release the held victim
- **Kind / args:** ranged, 0 args.
- **What the handler does:** 0x0882B294: `actor+0x194 |= 1`.
- **Who uses it:** the EX-Burst controller FUN_08A3D904 (called from FUN_08A3FE10, global *0x08A405E0; attacker = +0x26C, victim = +0x270):
  - attacker +0x194&2 (op 0x3E): clear the victim's +0x384 bit 0x1000 and stop.
  - attacker **+0x194&1** (this op): clear the victim's 0x1000 "held" bit, call FUN_088359A8(victim, victimPos + (0,0,0.9), 1), then victim vtable+0xE8.
  - neither: hold the victim (+0x384|=0x1000) and keep placing it relative to the attacker.
- **Data check:**
  - Jecht ten200 066 (EX script 8101) 32–58 and 78–106;
  - Squall eht100 076 (8102) 226–end.
  - These are the points where the victim is let go or knocked away in the burst.
- **Confidence:** strong (mechanism). The exact placement offset is literal and its purpose unclear. **Test:** remove it from Jecht 8101 → the opponent should stay pinned through 32–58.

### 0x012B — White screen flash
- **Kind / args:** ranged, `{unused}`.
- **What the handler does:** 0x0882B2BC: FUN_08A3EAB4(*0x08A405E0) sets the EX-Burst controller's +0x280 = 1.
- **Who uses it:** FUN_08A3E268 (from the draw task FUN_088881BC). If +0x280 is set, it clears it and draws a full-screen 480×272 sprite in 0x00FFFFFF with depth test, blend and texture disabled, i.e. **solid white**, for that frame.
- **Data check:** Squall eht100 077 (EX Burst 8500) 168–198. The screen is white for those 30 ticks.
- **Confidence:** confirmed. **Test:** add it to any motion for 10 ticks.

### 0x0652 — Player-steered body lean (tilt with the d-pad)
- **Kind / args:** ranged, floats `{maxPitchDeg, maxRollDeg, pitchRateDeg, rollRateDeg, followSmoothing, returnFactor}`.
- **What the handler does (0x0882C41C):** fighters only.
  - `+0x196 |= 0x80`.
  - FUN_08B347CC(actor, rad(a0), rad(a1), rad(a2), rad(a3), a4, a5): the four directional checks (FUN_08B32AC4/2B6C/2CBC/2C14; CPU commands 0x17…) change the target lean +0x10E0 (pitch) and +0x10E8 (roll) by rate·dt.
  - With no input, they decay by pow(a5, dt/2). They are clamped to ±max.
  - +0x10F0 = a4. It also sets a +0x388 status bit (mask 0x08B3AC08).
- **Who uses it:** FUN_08B2E864 smooths the current lean (+0x10E4/+0x10EC) toward the target with +0x10F0 and rotates the model matrix by it. Without +0x196&0x80 (0x08B2EA64) it also pulls the target back toward 0 each tick. CPU AI at 0x08A75FBC.
- **Data check:** Emperor two200 047 (10013), 40–end, `{30, 45, 1, 1, 0.8, 0.9}`.
- **Confidence:** strong. Whether the lean also steers the move's travel is not traced. **Test:** during Emperor 10013 after tick 40, hold the d-pad; he should tilt up to 30°/45°.

### 0x0070 — On ground contact, branch to the action's landing script
- **Kind / args:** ranged, 0 args.
- **What the handler does (0x0882B398):** fighters only.
  - Requires +0x380 bit 0x2000 and |pos.y − floor y (+0x2E4)| < 0.001.
  - Builds a collision query at the body centre (FUN_08837030) with 2 × body radius (FUN_08837014, +0x3A0→+0x18) and calls FUN_0889BEB0(world, …, mask 0x36, …).
  - On a hit: FUN_08B2F054(actor) plays the current attack record's **+0x12 script** (if not 0/0xFFFF) via FUN_0882FA10, stores it in +0x540 and sets +0x380 |= 8|0x10.
- **Data check:** Kain for110 064 (10066), 16–end. A dive that switches to its impact part on touching down fits.
- **Confidence:** strong for the code. The meaning of bit 0x2000 and of mask 0x36 is not traced. **Test:** Kain 10066 from low vs high altitude; the follow-up script should start on touchdown.

### 0x086E — Turn toward target (or body aim)
- **Kind / args:** ranged, 8 args `{deg, away, mode, f3..f7}`.
- **What the handler does (0x0882A580):**
  - **mode (arg2) == 0:** if a target exists (vtable+0x120 → +0x2FC), FUN_08836488(actor, clamp(arg0,0,180)·π/180, arg1!=0); else, if an opponent exists (vtable+0x130 → +0x300), FUN_088364F4 with the same args. Both call FUN_08836120. arg1 = 1 negates the direction, i.e. turn **away**.
  - **mode != 0:** body-aim slot on the bone-aim manager actor+0x1120 (see J.md 0x0D63/0x0D64): FUN_08A62C58 (target, slot type 2) or FUN_08A62B04 (opponent), with f12..f17 = arg3..arg7 and speed = float(arg0).
  - This is 0x0811's handler (0x0882A418) plus a fallback to the opponent when there is no target.
- **Data check:** Ultimecia eht200 064 (10071) `{180,0,0,0,…}` whole motion: snaps to face the target every tick.
- **Confidence:** confirmed (mode 0); strong (mode ≠ 0, via J.md). **Test:** set arg0 = 5 → slow tracking.

### 0x0051 — Flag with no reader
- **Kind / args:** ranged, 0 args. **Handler:** 0x0882C408: `actor+0x197 |= 1`.
- **Who uses it:** none found. Scanned all.s (EBOOT + battle overlay) for byte/half/word loads from actor+0x190 or owner+0x60 with any mask, and for direct `0x197(` loads.
- **Data check:** Kain for110 059 (10053) 4–92, also on _a.
- **Confidence:** weak / probably dead in this binary (could be read by a story-mode overlay). **Test:** remove it from Kain 10053 and compare.

### 0x0077 — Flag with no reader
- **Kind / args:** ranged, 0 args. **Handler:** 0x0882CE70: `actor+0x198 |= 0x20`.
- **Who uses it:** none found (same scan).
- **Data check:** gst120 067 (10060), whole motion.
- **Confidence:** weak / probably dead.
