# Open questions: event args, unknown events, ability ids (31 Oct 2026)

Static reading of the EU EBOOT (`work/EBOOT.dec.elf`) and the battle / menu / extra overlays (loaded at 0x089DD4C0),
disassembled with capstone; retail data from the 31 objx in `work/exmode_re/all/` and system.bin in main.bin.
Nothing was run or changed. Labels: **confirmed** (code end to end), **strong** (one link inferred), **guess**.
"already in docs" = the meaning is in moves-and-events.md or archive/event-ops/*.md; only the name is new here.
`tools/charmod_objx.py` NAMED currently names few of these args; the names below are proposals for it.

## 1. Argument names

| Op | Arg | Proposed name | Meaning | Conf. | Source |
|---|---|---|---|---|---|
| 0x0D63 / 0x0D64 aim_bones | 0 | bone_a | model bone (−1 none) | confirmed | already in docs (J.md) |
| | 1 | weight_a | float share of the aim bone A takes | confirmed | J.md |
| | 2 / 3 | bone_b / weight_b | second bone | confirmed | J.md |
| | 4 / 5 | yaw_left / yaw_right | yaw limits, degrees | confirmed | J.md |
| | 6 / 7 | pitch_up / pitch_down | pitch limits, degrees (0 = no vertical aim) | confirmed | J.md |
| | 8 | deg_per_tick | aim speed (180 = snap) | confirmed | J.md |
| | 9 | hold_ticks | hold after the range ends | confirmed | J.md |
| | 10 | smoothing | follow factor 0–1 | confirmed | J.md |
| | 11 | relax | decay factor back to rest | confirmed | J.md |
| | 12 | player_steered | 1 = d-pad steers the aim | strong | J.md |
| 0x0B36 timed_buff | 0 | ticks | float duration | confirmed | M.md |
| | 1 | phys_attack | added to the holder's attack total when it hits with a physical attack (atkp +0x08 bit 0x80 clear) | strong | FUN_08AE45B8 0x08AE5350 (into the sum that also gets Cecil-form fn 0x43 param1, Hazard Raid 0x9A) |
| | 2 | phys_defense | added to the holder's defence total when it is hit by a physical attack | strong | 0x08AE5624 (sum with Physical Shield fn 0x13A) |
| | 3 | phys_crit | added to the critical-hit chance (physical) | strong | FUN_08AE70E4 0x08AE7938 (sum with Spite 0x30, Fragile Pride 0x32, Cat Nip 0x141, Anti-EX 0x145, Assist Critical Boost 0x5F) |
| | 4 / 5 / 6 | magic_attack / magic_defense / magic_crit | the same three for attacks with atkp +0x08 bit 0x80 (magic: the branch that applies Magic Shield fn 0x138) | strong | 0x08AE535C, 0x08AE5630, 0x08AE7944 |
| | 7 / 8 / 9 | r / g / b | pulse tint | confirmed | M.md |
| | 10 | period | pulse period | confirmed | M.md |
| 0x060F footstep_sound | 1 | line_probe | surface found by a downward line probe | strong | already in docs (E.md) |
| | 2 | volume_probe | surface found by a volume probe (also: dust not snapped to the ground) | strong | E.md |
| | 3 | dust_foot_a | ground dust at bone +0xA4+0x54 (kind 0/6 only) | strong | E.md |
| | 4 | dust_foot_b | ground dust at bone +0xA4+0x50 | strong | E.md |
| | 5 | fixed_position | sound placed once at the body bone instead of following the fighter | strong | E.md |
| 0x072C feathers | 2–5 | bone0..bone3 | bones to shed from, list ends at −1 | confirmed | already in docs (I.md) |
| | 6 | ex_mode_only | only in EX Mode | confirmed | I.md |
| 0x0D10 homing_dash | 8 | no_rise | clamp upward speed; stop if below target | confirmed | already in docs (F.md) |
| | 9 | no_fall | clamp downward speed | confirmed | F.md |
| | 10 | ignore_target_reaction | skip the per-tick target-state checks | confirmed | F.md |
| | 11 | stop_if_target_busy | stop homing when the target is busy (script 400–469 or +0x194 bit 0x10) | confirmed | F.md |
| | 12 | alt_target | home on the assist's partner / summon's owner | confirmed | F.md |
| 0x0925 ground_homing_dash | 6 / 7 / 8 | ignore_target_reaction / stop_if_target_busy / alt_target | as 0x0D10 args 10–12 | confirmed | already in docs (L.md) |
| 0x0306 screen_shake | 0 | duration | ticks (≤0 → 30) | confirmed | handler 0x0882D244 → FUN_088749A0, same as 0x0353 (I.md) |
| | 1 / 2 | x_amp / y_amp | amplitude in % (×0.01; ≤0 → 1.0) | confirmed | 0x0882D284–0x0882D2C4 |
| 0x0101 chain_window | 0 | cpu_flag | also sets +0x190 bit 0x08 (0x08829C28); the only reader is CPU AI 0x08A77840 | strong | same bit as 0x0467/0x0278 arg0 (D.md, I.md) |
| 0x0112 weapon_volume | 0 | group | hit-volume group turned on (+0x448 bit arg0, 0x088299E8 → FUN_089985B0) | confirmed | already in docs (hit-contact) |
| 0x0204 fire_projectile | 1 | origin_effect | passed as `arg` to FUN_08AD0978; used as the FEPA effect id of an origin-6 projectile (emitter +56), when ≠ 0 | confirmed | already in docs (projectile-flight); retail: 0 ×809, 9/19/25–31 on Gilgamesh / Kuja |
| 0x0303 sound | 1 | no_doppler | only with arg2: arg1 = 0 sets sound flag 0x10 (FUN_088A1F6C), which turns on the speed-of-sound (340) pitch step at 0x088A3734 | strong | FUN_0883834C 0x08838468; retail (1,0) ×6 |
| | 2 | at_body | play at the body-centre bone position (FUN_08838D14) instead of following the fighter (+0x510) | confirmed | FUN_0883834C 0x088383E0; retail (0,1) ×8 |
| 0x0402 face_target | 1 | only_if_target_reeling | turn only while the target is in a hit reaction 500–798 (FUN_088360BC) | strong | already in docs (attacker-movement) |
| | 2 | even_if_target_busy | keep turning while the target is busy | strong | attacker-movement |
| | 3 | skip_if_target_reeling | no turn while the target reels | strong | attacker-movement |
| 0x0518 steer_during_move | 2 | move_along_facing | +0x192 bit 0x40: velocity from the facing vector (+0x90), not the stick (FUN_08804674) | guess | already in docs (H.md, weak) |
| | 3 | face_move_direction | +0x192 bit 0x10: turn to the move direction even in an action (FUN_08B2DD60) | strong | H.md |
| | 4 | stick_relative_to_facing | +0x198 bit 0x01: stick read in the fighter's frame | guess | H.md |
| 0x055F prop_effect | 2 | record_bit | which record (bit of record +0x44) | confirmed | already in docs (J.md) |
| | 3 | scale | float, 0 = 1.0 | confirmed | J.md |
| | 4 | prop | prop id, −1 = any | confirmed | J.md |
| 0x0743 hit_window_fire | 3 | projectile_arg | second arg of the fire call (as 0x0204 arg1) | confirmed | already in docs (G.md) |
| | 4 | show_caption | 1 = show caption on contact | confirmed | G.md |
| | 5 / 6 | caption / caption_flag | as 0x021D args | confirmed | G.md |
| 0x0811 turn_or_swivel | 3 / 4 | limit_left / limit_right | swivel limits, degrees | confirmed | already in docs (G.md) |
| | 5 | hold_ticks | hold after the range | confirmed | G.md |
| | 6 | smoothing | display follow factor | confirmed | G.md |
| | 7 | decay | relax factor | confirmed | G.md |
| 0x086E turn_to_target_or_aim | 0 | deg_per_tick | (NAMED has 'mode' here: wrong) | confirmed | already in docs (K.md) |
| | 1 | away | turn away (mode 0 only) | confirmed | K.md |
| | 2 | swivel | 0 = turn, ≠0 = body swivel | confirmed | K.md |
| | 3–7 | limit_left, limit_right, hold_ticks, smoothing, decay | as 0x0811 | strong | K.md (same slot writer FUN_08A62C58 / FUN_08A62B04) |
| 0x0579 chain_followup_if_hit | 0–4 | cpu_flag, action, even_on_miss, mid_action, hit | 0x0467 args + atkp id that must have landed | confirmed | already in docs (K.md) |
| 0x047A can_act_now_if | 0 | no_jump_dash | +0x193 bit 2, +0x199 bit 4 (as 0x041E arg0) | strong | already in docs (J.md, N.md) |
| | 1 | not_same_move | +0x198 bit 0x40 | strong | J.md |
| | 2 | only_on_whiff | 0 = only if the move hit, 1 = only if it missed | confirmed | J.md |
| | 3 | only_same_move | +0x198 bit 0x80 | strong | J.md |
| 0x0652 steer_lean | 0–5 | max_pitch, max_roll, pitch_rate, roll_rate, follow_smoothing, return_factor | degrees / factors | strong | already in docs (K.md) |
| 0x051B screen_flash | 1–4 | hold, fade_out, colour (0 white, 1 black, 2 red), unused | as named; arg0 = fade_in | confirmed | already in docs (L.md) |

## 2. Unknown events

- **0x0051 (+0x197 bit 0x01) and 0x0077 (+0x198 bit 0x20): no reader.** Rescanned EBOOT, battle, menu and extra overlays
  (the supacc overlay has no such offsets) for byte/half/word loads at actor+0x190..0x19B, owner+0x60..0x6B and via
  `addiu base,0x190` with any mask, plus whole-word reads (`lw 0x194/0x198`): every hit is another struct. The flag
  block is never copied elsewhere. Same result for the aim-bones bit +0x197 0x04 (0x0D63/0x0D64). **strong: dead flags**
  in this build (Kain 10053, gst120 10060). The scanner is a pattern match; a computed-offset read would escape it.
- **0x0225's +0x192 bit 0x20 is read** (corrects K.md): FUN_08B30544 at 0x08B3059C, the "stick may move you" test,
  accepts it like can_act or 0x0518's bit 0x08 (H.md). So 0x0225 = **stick movement allowed with turn/speed
  multipliers, without 0x0518's air state and airborne flag**. Name: `steer_on_ground`. **confirmed** (reader), retail
  {1,1} on Squall 20011/13/14.
- **0x0071 (self + partner +0x197 bit 0x80, copied to +0x188 bit 0x20): protection.** Readers: the hit-reaction
  applicator FUN_0883A6FC returns at once (0x0883A764: no flinch / knock-back); FUN_0883A150 skips the hit-by-assist
  branch (0x0883A4A4); the battle calc in FUN_08AE45B8 skips the Brave subtraction for a flagged fighter (0x08AE47F0,
  0x08AE48EC, before `brave obj(+0x52C)+0x14`); FUN_08B33938 skips the attacker's on-hit extras against it. So while
  Aerith's 10070 runs, **she and her partner take hits without reaction or Brave loss**. Name: `protect_self_and_partner`.
  **strong** (the 0x08AE1A58..0x08AE5C14 readers are this per-side battle-calc module, not only CPU AI as K.md said).
- **Keyed 0x0037: ends the foe's "suspended" hold.** It clears opp motion-player +0x58 bit 0x10 (anim dt = 0). The
  setter that matters is FUN_08A56E10, the hang timer of hit reaction 19 (`FUN_088325D0(1)` → FUN_08A56F98): it keeps
  the bit on until a time that depends on the attacker's charId at `*(+0x5A4)` — 0x10 (Ultimecia) or 0x21 get special
  values (0x08A56E8C–0x08A56EB8). Ultimecia's 10070 at tick 0 releases a foe she left hanging. Name: `release_foe_hold`.
  **strong** (mechanism), purpose strong.
- **Keyed 0x0131 battle_message: changes Brave.** Listener = per-side object with vtable 0x08AE7B50 (ctor FUN_08AE05A4,
  registered with FUN_08AFCD40 by FUN_08887068 at scene+0x18); its handler 0x08AE17FC jump table entry 0x1F → FUN_08AE223C:
  if FUN_08AE9AE0(obj+0x30, 0xE) passes, it takes the fighter (assist → partner +0x1108), computes a fixed-point amount
  from `value` (÷100, const 0x64 at 0x08AE7B88) and vtable+0x198 (= actor+0x1094, meaning unknown), adds it to the
  **current Brave** (`*(+0x52C)+0x14`), clamps 0..9999, and posts FUN_08AE3710(obj, 4, delta) (HUD/stat). Values 1–6,
  23, 60 per weapon variant on gst130. Name: `brave_bonus`. **strong** that it changes Brave; **guess** for the exact
  formula and for what actor+0x1094 is.

## 3. Ability ids 0x136 / 0x41 / 0x40

These are **fnct function ids**, not ablt ids: FUN_088BF4CC(stats = actor+0x310, id) tests bit `id` of the bitset at
stats+8 that the equipped abilities' 'fncs' effect sets fill (ablt +0x14). Matching system.bin (main.bin) effect sets:

| fn id | ablt record(s) | Name | Effect | Conf. |
|---|---|---|---|---|
| 0x136 | 0x4042, 0x4066 (fncs set 589) | **Evasion Boost** | dodge invincibility 20 → 40 ticks (0x0047) | strong |
| 0x41 | 0x4002 (set 1376) | **Precision Evasion** | perfect-dodge window (0x0074) | strong |
| 0x40 | 0x4001 (set 1375) | **Precision Block** | perfect-guard window (0x0145) | strong |

## 4. atkp leads from the US-build notes (VCKing101 moveset-format.md), checked against the EU code

| Claim | Verdict | Evidence |
|---|---|---|
| +0x00 is a u32 id (+0x02 its high half) | **confirmed** | FUN_08837F14 bsearch, record size 0x44, compare 0x08830FAC `lw 0(a1)`; +0x02 = 0 in all 2,060 records |
| +0x04 class indexes the 16×16 clash table 0x089D4978, read from 0x08809F00 | **confirmed** (same addresses in EU) | FUN_08809F00 `lbu 4(atkp)` at 0x0880A090/0x0880A09C → FUN_0884A454 (`table + a·64 + b·4`); already in guard-dodge-walls |
| +0x0D = hit-spark kind (not hit-stop) | **half right**: it is a spark *amount*, not a kind | FUN_08808C98 → 0x08ABB60C (Brave) / 0x08ABB690 (HP) → FUN_08ABD6F4 / D810 → FUN_08ABDBB4: particle count = param(0xB)+0x48 × (+0x0D / 10). Also runs for main fighters (skipped only for assists), so hit-reaction's "not read for fighter attackers" is wrong |
| +0x06 almost always −1 | **confirmed**, and it is read: a **second contact-effect bit** | FUN_0880966C 0x088096B0/0x088096B8: pass 0 uses +0x0C, pass 1 uses +0x06; 0xFF = none. Data: 0xFF ×2055, 16–32 on 5 (Tifa sev110, Firion) |
| +0x07 always 0 | confirmed (data) | no reader looked for |
| +0x14 (0/6/8/10/12) unknown | **it is hit-stop**: ticks of global slow-down on a fresh hit by a non-assist fighter | FUN_08807E6C 0x0880800C → FUN_08ACEEF4: global speed slot 9 = 0.15 (0.0 if attacker +0x384 bit 0x2000) for +0x14 ticks, restored by FUN_08ACEF94. Data: 0 ×1646, 6 ×175, 8/10 ×108, 12 ×21, 14 ×2 |
| +0x2E = high half of a 32-bit +0x2C, 0xFFFF or small float | **confirmed** as a union | reactions 11–15: s16 bone at +0x2C (0xFFFF = root, +0x2E = 0); reactions 2/3/17 (and a few others): f32 at +0x2C (0.01–0.6). charmod_objx HIT_FIELDS splits it as h + H; round-trips, but the name `kb_bone` is wrong for floats |

Tool follow-ups (not done): HIT_FIELDS calls +0x0D `hitstop` (it is spark amount) and +0x14 part of `shake`
(+0x14 = hit-stop, +0x15..0x17 = shake duration / x / y).
