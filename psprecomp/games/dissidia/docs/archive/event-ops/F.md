# F: keyed ops 0x0422 and 0x0D10

**Summary**
- **0x0422 is not face/blink.** It spawns a ground-contact effect (footstep, skid or body-fall dust, or a water splash) under a bone. The effect it picks depends on the floor surface the actor stands on. Confirmed in code. Retail data places it on toe/foot/hand/hip bones at footfalls, in dash trails and in knockdowns.
- **0x0D10 is homing.** It starts a tracking dash toward the lock-on target: the actor enters state 0xF and flies to a point set relative to the target, at a set speed, for N ticks. It takes the same parameter block as the homing block in a plya action record (+0x40..+0x5C). Confirmed in code.
- Sibling keyed codes 0x25, 0x27 and 0x28 (handlers 0x0882D9E8, 0x0882DAA0, 0x0882DBA4) call the same homing setup with fixed flag variations (ground homing, no-target straight dash). They are noted at the end.

---

### 0x0422 — Ground-contact effect at a bone (footstep dust / water splash)

**Kind / args:** keyed, 4 args. Handler: keyed table 0x089C7B08[0x22] = 0x0882D87C.
| arg | type | meaning | seen |
|---|---|---|---|
| 0 | int | GMO bone index (raw index into model bone array, FUN_089B0E78: `model+0xC + idx*0x100`, bounds `model+0x20`) | toe/foot bones (7,8,9,12,13,15,18), hands (22,46), chest 16, hip 3, Vivi bone 2 `m_wep` |
| 1 | float | offset along the bone's local X axis. The game multiplies it by a per-bone float from table `**(actor+0x394)`, and uses 0 if that table is null | 0 almost always. Vivi `m_wep` (staff) uses 0.4–0.6 |
| 2 | float | effect size/intensity, passed as f12 to the spawners | 0 (Y'shtola idle), 0.05 (Vivi steps), 0.1 (steps), 0.2, 0.4–0.6 (body falls), 1.0 (dash) |
| 3 | int (byte) | variant flag, passed as `t0` to the water spawner FUN_0885AD38 | 1 on the dash and on heavier impacts, 0 on normal steps |

**What the handler does (0x0882D87C):**
1. `FUN_0883739C(actor)` returns 1 only if `actor+0x542` is 0 or 1. Otherwise it does nothing. I don't know what +0x542 is: some actor kind. Values 2 and 3 are handled differently elsewhere, and 3 looks like a summon/object.
2. `FUN_08853E80(actor+0xD08, (s8)actor[0x3C6], arg0, 0, f12=arg1, f13=arg2, f14=0.5, t0=arg3)` pushes a 24-byte request into a per-actor queue at `actor+0xD08`. The queue holds at most 2 entries per frame (`+0x30` count < 2) and drops any further request. Entry layout:
   - +0 = surface type
   - +4 = bone
   - +6 = 0 (path select)
   - +7 = 0 (snap to ground)
   - +8 = arg3
   - +0xC = arg1
   - +0x10 = arg2
   - +0x14 = 0.5

   **Surface type** is `actor+0x3C6`. The floor clamp FUN_0883B470 writes it at 0x0883BFC4 from bits 16..20 of the attribute word of the collision triangle under the actor (raycast FUN_088976E4 with mask 0x22).
3. Keyed 0x060F (handler 0x0882D6D0) pushes into the same queue through FUN_08853E24, with +6 = 1 and different fields.

**Who uses it / effect in game:** `FUN_0885436C(actor+0xD08, actor)` consumes the queue. It is called once per frame from the actor update FUN_08B2D524 (0x08B2D5B0) and clears the count when it finishes. For each entry:
- It gets the bone matrix (bone+0x80), transforms `(arg1*boneScale, 0, 0, 1)` into world space (actor+0x330), and then, because +7 is 0, raycasts down to the floor (FUN_088976E4, mask 0x22, at 0x08854570) so the effect sits on the ground under the bone.
- If the stage descriptor `*(*(0x089D6AC8)+0x710)` is null, it does nothing.
- If the global `**(0x089D85E8)==1` and `FUN_08A50140()==1`, it only calls FUN_08853D3C(effectMgr+0x188, pos, f12=arg2). This is an alternate mode; I couldn't identify which.
- Otherwise it takes the +6 = 0 path:
  - For surface 0x1E or 0x1F, it calls `FUN_0885AD38(effectMgr+0x160, pos, …, surface, f12=arg2, f13=0.5, f14=120.0, t0=arg3!=0)`. The same routine runs when debris particles land on surfaces 0x1E/0x1F (FUN_08852AAC at 0x08853004), so this is most likely a water ripple/splash. "Water" is my reading, not proven.
  - Unless the actor's `+0x180` is 1 or 0xB, or it is (+0x542==2 and in state 0xC):
    - It calls `FUN_08856374(effectMgr+0x174, pos, f12=arg2, -1, -1, 1)`. This is probably the generic dust puff. It is skipped when the stage-name prefix matches "ele" (strncmp-like FUN_08935B50 with n=3, string at 0x089C8D40).
    - It calls `FUN_0885BA18(effectMgr+0x7180, actor, surface, pos, …)`. That spawns a surface-specific stage effect through FUN_088A9DE8: surface 0x1E → effect 0x0A (raised 0.02 m, plus an oriented second part), surface 0x0A → effect 0x0C, surface 0x04 → effect 0x0B. It is skipped when bit 0x04 of `actor+0x5A4 → +0x69` is set.
  - effectMgr = `*(0x089D4DC8)`.

**Data check (177 uses, all three mods):**
- Stand/idle motion 000 (scripts 1/10): alternating l_toe/r_toe every 40 ticks (Noctis 0/2/40/80; Vivi every 40 up to 440, arg2 0.05). These are idle weight-shift steps.
- Walk/run 023/024 (scripts 500/501): one r_toe or l_toe at tick 8–10. Footfalls.
- Dash 037 (script 240, R+△): l_toe every 4 ticks from 24 to 64, `{toe, 0, 1.0, 1}`, x=3, in all three characters. This is the ground skid trail during the dash.
- Knockdown 019 (script 460, Noctis): r_hand @30 (0.1), chest @40 (0.6, flag 1), hip @46 (0.4). Body parts hitting the floor.
- Vivi 069 (script 1210, intro/victory): bone 2 `m_wep` with arg1 0.4–0.6. The staff tip taps the ground (the offset is along the staff).
- On attacks, the timing is the landing or step of the foot (e.g. Noctis 043 r_toe @22 between hits 0–10 and 18–26), never the hit itself.

None of these places fits "face/blink". All of them fit ground contact.

**Confidence:** **confirmed** for "queue a ground effect at bone N, snapped to the floor, chosen by floor surface". The exact effect for each surface (water = 0x1E/0x1F) and the roles of FUN_08856374 and FUN_08853D3C are **strong / unnamed**.

To settle it: do a Noctis dash (or Vivi idle) on a water or sand stage with and without the 0x0422 events. The dust or splash under the feet should disappear when they are removed. A larger arg2 should give a bigger puff.

---

### 0x0D10 — Homing dash toward target (air), state 0xF

**Kind / args:** keyed, 13 args. Handler: keyed table 0x089C7B08[0x10] = 0x0882D8C4. The setup function is FUN_08A60BC8, which writes the homing block J = `*(actor+0x314) + 0x48`.

| arg | type | stored at | meaning (from the per-tick code) | seen |
|---|---|---|---|---|
| 0 | float | J+0x14 | horizontal speed, metres per tick (× actor time scale FUN_08838300) | 0–0.75 |
| 1 | float | J+0x1C | tracking time in ticks. Each tick: re-aim at the target and move, then decrement by the time step. At ≤ 0, movement stops (FUN_08A60838 returns a zero vector) | 6–62 |
| 2 | float | J+0x20 | height offset of the destination above the target (m) | -1.5–2.0 |
| 3 | float (deg) | J+0x24 | angle of the destination around the target, converted deg→rad (×π/180 at 0x0882D90C–0x0882D93C). Applied by FUN_089A4124 (probably a Y rotation) in FUN_08A604AC | always 0 |
| 4 | float | J+0x28 | stand-off distance from the target (m), along the target→actor horizontal direction | 0–14 |
| 5 | float | J+0x2C | arrive radius: if the distance to the destination is less than this, set J+0x34 bit 1 and end the state. -1 disables it | always -1 |
| 6 | float | J+0x30 | maximum state duration (ticks). When elapsed time (state data +0, counted at 0x08A61168) ≥ arg6, the state ends | usually = arg1 |
| 7 | float | J+0x18 | vertical speed (m/tick). If ≤ 0, it uses arg0 instead (0x0882D8E8) | 0–0.7 |
| 8 | int | J+0x34 \|= 0x10 (FUN_08A60E4C) | no rising: clamps upward velocity to 0. Stops homing (bit 2) if actor.y < target.y | 0 |
| 9 | int | J+0x34 \|= 0x20 (FUN_08A60E64) | no falling: clamps downward velocity to 0. Stops if target.y < actor.y | 0 |
| 10 | int | J+0x34 \|= 0x40 (FUN_08A60E7C) | skip the per-tick target-state checks (no stop/freeze when the target reacts) | 1 on several Noctis/Y'shtola moves |
| 11 | int | J+0x34 \|= 0x80 (FUN_08A60E94) | stop homing as soon as the target is "busy" (see FUN_08837458 below) | 0 |
| 12 | int | J+0x34 bit 0x100 (FUN_08A60BC8 a3) | use the alternate target: FUN_0885142C if +0x542==3, `actor+0x1108` if +0x542==1 | 0 |

**What the handler does (0x0882D8C4):**
1. It calls the actor vtable at `*(actor+0x544)+0x120`, which gets the current (lock-on) target. If there is no target, nothing happens.
2. It calls `FUN_08A60BC8(actor, a1=0, a2=0, a3=arg12!=0, f12=arg0, f13=arg7>0?arg7:arg0, f14=arg1, f15=arg2, f16=rad(arg3), f17=arg4, f18=arg5, f19=arg6)`. That function:
   - computes the horizontal unit vector `-(norm(FUN_08836DAC(actor,target)))` (y zeroed) and stores x,z at J+0xC/J+0x10;
   - stores the floats above;
   - sets J+0x34: clears bits 1/2/0x10–0x80, sets bit 4 = a1 (0), bit 8 = a2 (0), bit 0x100 = a3;
   - because bit 4 is clear, sets `*(actor+0x314)+0xF4 = 6`, the controller's "pending mode 6".
3. If the actor is airborne (`FUN_088374B0`), it switches the actor to **state 0xF** immediately with `FUN_08834C10(actor, 0xF, 0)`. Mode 6 maps to state 0xF in keyed op 0x0000's table at 0x089C7BE8.
4. It calls the arg8–arg11 flag setters.

**Who uses it / effect in game:** state 0xF's methods (vtable entries at 0x08A611A8..0x08A61200):
- **Enter, FUN_08A60794:** zeroes the state data and sets `actor+0x384 |= 0x1000` (bit 4 is clear). The exit counterpart, FUN_08A607F0, clears it again.
- **Per-tick update, FUN_08A60EAC:**
  - Looks up the target.
  - `FUN_08837458(target)` is true when the target's `+0x180` is in [400, 470) (probably the hit-reaction scripts, my guess) or `target+0x194` bit 0x10 is set. If true, bit 0x80 → stop, and the destination freezes (state data +4 = 1) unless bit 8 is set.
  - Applies the 0x10/0x20 height checks.
  - While arg1 > 0, recomputes the destination with FUN_08A604AC: `target.pos(+0x110) + rotY(arg3)·dir·arg4 + (0, arg2, 0)`. It stores the destination in J+0..8 (FUN_08A60730).
  - Decrements arg1.
  - Ends the state on any of: arrived (bit 1), elapsed ≥ arg6, or `FUN_0882FCE8(actor+0x130,-1)==0` with `actor+0x188` bit 0 set (probably "motion ended"). On ending it switches to **state 0x12 (air action)**, or to state 0 if bit 4 is set.
- **Velocity, FUN_08A60838 (vtable slot 0x08A611FC):**
  - Returns zero if bit 2 (stopped) is set or arg1 ≤ 0.
  - Otherwise takes `d = dest - actor.pos(+0x100)`. If |d| is less than 2×max(speed), it returns d (snap onto the point), or zero if |d| ≤ 0.02.
  - Otherwise it returns normalize(d) with x,z × arg0×timescale and y × arg7×timescale, then applies the 0x10/0x20 clamps.
- Ranged ops with codes 0x48 (0x0882BDC4) and 0x4F (0x0882C1EC) check "state == 0xF and J+0x34 bit 2 (homing stopped)". These are follow-ups that wait for homing to end.

**Cross-check:** the action starter FUN_08B382B8 at 0x08B385BC–0x08B38654 calls the same FUN_08A60BC8 when plya action flag `+0x18 & 1` is set and the actor has a target (`+0x2FC`). It maps the record fields like this:

| plya field | event arg |
|---|---|
| +0x40 | arg0 (speed) |
| +0x44 | arg7 (vertical speed) |
| +0x48 | arg1 (tracking time) |
| +0x4C | arg5 (arrive radius) |
| +0x50 | arg6 (max duration) |
| +0x54 | arg2 (height) |
| +0x58 | arg3 (angle, already radians) |
| +0x5C | arg4 (stand-off distance) |
| flags 0x08000000 / 0x10000000 / 0x20000000 | arg8 / arg9 / arg10 |

The one retail action with flag bit 1 set (Vivi) has `{0.1, 0.01, 38, -1, 38, 0, 0, 3.0}`, which has the same shape as the event data.

**Data check (22 uses):** every timeline that uses it is an air motion (ranged 0x000A covers the whole motion), so step 3 always enters state 0xF at once. The homing time ends near or at the first hit:
- Noctis p_noc100 044 (script 10002): @2 `{0.15, 18, -0.15, 0, 0.1, -1, 18, 0.01, …, a10=1}`. Homing ends at tick 20, and the hit window is 24–36.
- Y'shtola w_ysh104 004 (10052): @0 `{0.75, 6, 0, 0, 0.5, -1, 6, 0}`. Ends at 6, hit 2–6. w_ysh104 003 (10050): @2, 14 ticks, ends at 16, hit 14–18.
- Noctis p_noc100_a 012 (10057): @4, 10 ticks, the hit is 4–12. Stand-off 1.14 m, 0.6 m below the target.
- Long version: Y'shtola w_ysh105 006 (10075): @60 `{0.07, 62, 0, 0, 14.0, -1, 62, 0.07}`. A slow 62-tick drift that holds 14 m from the target during a 60–120 hit window (a ranged/aeon move).
- Duplicate events with `x=16` and without x at the same frame (w_ysh101 001, w_ysh102 001) are alternatives the keyed walker FUN_08829104 chooses between. At 0x0882921C, x byte 0 must equal `*(*(owner+0x4C)+4)`, an owner id/variant whose meaning I don't know. The handler itself never reads x.

**Confidence:** **confirmed** (the code shows the behaviour end to end, and it matches the plya homing block). Still uncertain:
- the sign convention of FUN_08836DAC. I think the destination is on the actor's side of the target; that is a guess.
- that arg3 is a rotation about the vertical axis (a guess, because it is always 0).
- what FUN_08837458 means.

In-game test: on Noctis 044, change arg0 to 0 or arg1 to 0. The move should stop closing distance before the hit. Raising arg4 should make him stop further from the opponent.

---

### Related keyed codes (same system, not assigned, for reference)
All of these call the homing setup FUN_08A60BC8.

| keyed code (handler) | a1 / a2 | what differs from 0x0D10 |
|---|---|---|
| 0x25 (0x0882D9E8) | a1=1, a2=0 | Always forces state 0xF. Vertical speed and height are fixed at 0, so the args shift: args[0..5] = {speed, track, angle°, dist, arrive, maxdur}, args[6] = flag 0x40, args[7] = flag 0x80, args[8] = alternate target. Bit 4 set means the mode is not armed, `actor+0x384` bit 0x1000 is not set, and the state ends in state 0. Probably ground homing. |
| 0x27 (0x0882DAA0) | a1=0, a2=1 | 0x0D10's layout, but bit 8 is set: the destination is not taken from the target (fixed direction from the constant at 0x089B08AB0) and there is no target guard. A straight-line dash. |
| 0x28 (0x0882DBA4) | a1=1, a2=1 | Forced state 0xF, fixed direction. |

These readings are **strong** on the code; their arg layouts would need data to confirm.
