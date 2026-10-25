# Moves and timeline events

How a move is put together (menu → command → action → script → motion → timeline) and what every timeline event does. The raw per-op research behind the event table is in [archive/event-ops/](archive/event-ops/).

- [Timeline event ops](#timeline-event-ops)
- [Dissidia 012: how a move is defined](#dissidia-012-how-a-move-is-defined)

## Timeline event ops

What each event in a move's timeline does. Traced in the game code on 2026-10-23 in two rounds over the EBOOT and battle overlay: round 1 covered the events in the mod folders ([`A.md`](archive/event-ops/A.md) … `F.md`), round 2 every other event used by the 30 retail fighters (`G.md` … `M.md`) plus the open questions (`N.md`). Each write-up has the addresses and an in-game test per event.

The names in the left column are the ones used in the mod folders' `timelines/*.json` files (`tools/charmod_objx.py` NAMED) and in the Blender Moves & events panel.

### How events are dispatched
- **Op encoding:** op = (number of u32 args << 8) | code. The handler is picked by the low byte (code).
- **Ranged events:** dispatcher `FUN_08829930` (table 0x089C7910, codes < 0x7D).
  - Owner = the motion player at actor+0x130, so its +0x60 is actor+0x190, the per-tick flag block.
  - Per tick: keyed events run first, then the flag block is cleared, then ranged events run. So a ranged flag is on for exactly the ticks of its range.
- **Keyed events:** `FUN_0882CEC0` (table 0x089C7B08, codes < 0x38), called from the walker `FUN_08829104`. The event's `x` byte 0 must match `*(*(owner+0x4C)+4)`, which looks like a phase/owner filter.
- **Confidence levels:** **confirmed** = the code shows the behaviour end to end. **strong** = the mechanism is clear, but one link is inferred. Neither has been tested in the game yet.

### Moves, combos, cancels
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `hit_window` | 0x0200 | Attack is active `{hit, group}` | confirmed |
| `weapon_volume` | 0x0112 | Second hit volume over the window | confirmed |
| `chain_window` | 0x0101 | Next press of the combo accepted | confirmed |
| `chain_followup` | 0x0467 | Chain window that also offers action `arg1` as a follow-up when the input matches; arg2 = 1: even on a miss; arg3 = 1: input accepted mid-action. Not automatic. | confirmed |
| `chase_window` | 0x0120 | If this hit (`arg0` atkp) blew the foe away (blow-away scripts 700/701/750/751), a press starts the Chase | confirmed |
| `forced_hit` | 0x0108 | Hits the foe with atkp `arg0` at once, with no hitbox or range check (EX Bursts) | confirmed |
| `can_act` | 0x0005 | Neutral: may attack, block, dodge (+0x190 bit 1; attack gate vtable +0x150, block gate +0x158, dodge, R+△ dash). Stand, idle and fall motions carry it for their whole length. | confirmed |
| `can_act_now` | 0x041E | End of recovery: may act at once, skipping the idle-state checks. Non-zero args set more bits (unknown; retail uses all 0). | strong |
| `dodge_cancel` | 0x0028 | May dodge out of the move; the CPU picks its next move from here | strong |
| `dodge_cancel_if_foe_free` | 0x0059 | Same as `dodge_cancel`, but only while the foe isn't in a hit reaction | strong |
| `attack_over_cpu` | 0x0022 | The dangerous part is over; read only by the CPU AI (punish timing) | strong |
| `cpu_hint` | 0x0029 | Read only by the CPU AI (repositioning / threat check) | strong (CPU-only), weak (exact use) |
| `invincible` | 0x0006 | Incoming hits pass through; own hits still land (dodges: ticks 0–20) | confirmed |
| `no_assist_cancel` | 0x006A | Can't be cut short by Assist / EX Revenge (0x0069 forces the cut) | strong |
| `cant_be_pushed` | 0x0049 | Other fighters are pushed off this one | strong |

### Movement
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `face_target` | 0x0402 | Turn toward the current target, arg0 = max degrees per tick | strong |
| `turn_to_opponent` | 0x040B | Turn toward the opponent (+0x300), arg0 = max degrees per tick; arg2 / arg3 skip turning while the foe evades or reels | confirmed |
| `homing_dash` | 0x0D10 | Dash toward the foe (state 0xF): speed, tracking ticks, height, angle, stand-off distance, arrive radius, max ticks, vertical speed, stop flags | confirmed |
| `air_motion` | 0x000A | No ground snapping; counts as airborne | high (earlier notes) |
| `ground_follow` | 0x0015 | Keep the feet on the floor | medium-high |
| `follow_floor_slope` | 0x0032 | Movement is projected onto the floor's slope | medium |
| `landing_allowed` | 0x002D | Lands when the height reaches 0 | high |
| `keep_motion_when_falling` | 0x0023 | Keep this motion when converted to a fall | high |

### Look and sound
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `show_part` / `hide_part` | 0x011B / 0x011C | Draw / hide a part for the range (it flips back when the range ends) | confirmed |
| `show_part_once` / `hide_part_once` | 0x010B / 0x010C | Draw / hide a part from this frame on | confirmed |
| `bone_effect` | 0x042A | Effect on a bone while the range runs. `{slot 0–15, record (9999 = weapon trail, 8888 = other weapon/hand effects), record bit, scale}`. Spawns only if the bone's part is visible. | confirmed |
| `effect` | 0x0219 | Plays an effect (FEPA record id) | confirmed |
| `face_texture` | 0x0214 | Face texture pattern `{group, pattern}` for the range (groups 0/1 probably the eyes, 2 the mouth); goes back to default 1–2 ticks after | confirmed (group meaning strong) |
| `pin_physics_bone` | 0x0227 | Hair/cloth spring bone follows the animation, at most arg1 degrees per tick (180 = snap) | strong |
| `motion_camera` | 0x030C | Cinematic camera from the model's `camera_include` (arg1 = 0) / `camera2_include` (1) bone; arg0 = preset | confirmed |
| `sound` | 0x0303 | Sound effect | confirmed |
| `voice` | 0x0315 | Voice line `{voice id, only if alt off, only if alt on}`; ids like 1810xxx | confirmed |
| `footstep_sound` | 0x060F | Footstep / landing / impact sound chosen by floor surface and footwear, plus a ground effect at a foot | confirmed (kind names weak) |
| `ground_dust` | 0x0422 | Dust / splash where that bone meets the floor `{bone, offset, size, variant}` | confirmed |
| `screen_shake` | 0x0306 | Camera shake | |
| `summon_effect` | 0x0335 | Summons only: the owner's effect attached to the summon `{effect, lifetime, bone}` | strong |

### Round 2: everything else the retail fighters use

#### Combat and cancels
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `hit_window_fire` | 0x0743 | Hit window `{hit, group}` that fires projectile arg2 on contact; arg4 = 1 shows caption arg5 | confirmed |
| `guard_counter_hit` | 0x027B | Hit window that is live only after your guard blocked something (Exdeath) | confirmed |
| `chain_window_if_hit` | 0x0278 | Chain window that opens only if hit `arg1` landed (Firion: always); arg0 = CPU flag | strong |
| `chain_followup_if_hit` | 0x0579 | `chain_followup` gated on a hit you landed (args shifted by one) | confirmed |
| `can_act_now_if` | 0x047A | `can_act_now`, only if the move hit (arg2 = 0) or only on a whiff (arg2 = 1) | strong |
| `dodge_attack_cancel` | 0x003A | End of dodges: Brave, HP and EX inputs may start a move | confirmed (buttons strong) |
| `brave_attack_allowed` / 0x0037 (ranged) | 0x0036 / 0x0037 | Only the Brave / only the HP input may start a move now | strong |
| `act_after_guard` | 0x0158 | After a successful guard, may act at once (Jecht Block) | strong |
| `can_guard` | 0x0072 | May block or start a guard-type move as if neutral (Exdeath) | strong |
| `dash_cancel` | 0x016C | Dash-cancel window (arg = only after a hit) (Kain) | strong |
| `no_jump` | 0x004A | Jumping blocked (Kain's dash) | confirmed |
| `timed_input_latch` | 0x004C | A press during the range voids later timed-input windows (Jecht's timed combos) | strong |
| `force_assist_cancel` | 0x0069 | Assist / EX Revenge cancel always allowed (opposite of `no_assist_cancel`) | confirmed |
| `invincible_with_ability` | 0x0047 | Invincible, only with ability 0x136 (dodges 0–40: doubles dodge invincibility) | confirmed |
| `wakeup_invincible` | 0x017C | After a blow-away down, invincible for arg0 ticks when you get up (ends if you attack) | confirmed |
| `perfect_dodge_window` / `perfect_guard_window` | 0x0074 / 0x0145 | First 4 ticks of dodge / block: with ability 0x41 / 0x40 a well-timed dodge/guard gets a bonus (glow + HUD icon) | strong |
| `counts_as_evading` | 0x002F | Treated as evading by the foe and the AI (not invincible) | confirmed |
| `shot_by_presses` (keyed) | 0x0633 | Fires a projectile depending on how many presses the move got (Laguna) | strong |
| `button_shot` | 0x0524 | ○ / □ during the range fires extra projectiles `{projectile, arg, cooldown, slot, button}` | confirmed |
| `projectile_variant` / `effect_variant` / `rare_variant_roll` (keyed) | 0x042D / 0x042F / 0x032E | A 1-in-N roll picks alternate projectile / effect (Tidus) | strong / confirmed |
| `timed_buff` (keyed) | 0x0B36 | Timed buff with a pulsing glow (Onion Knight, 600 ticks); which stats is open | strong (stats weak) |
| `switch_form` (keyed) | 0x011E | Switch form: model, moves, parameters (Cecil Dark Knight ↔ Paladin) | strong |

#### Movement and physics
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `ground_homing_dash` (keyed) | 0x0925 | Ground homing lunge `{speed, track ticks, angle, distance, arrive radius, max ticks, …}` (was mislabelled `weapon_trail`; the old name still packs) | confirmed |
| `lunge` | 0x0548 | Straight lunge: speed arg2 along the facing pitched arg0°; arg3 = only without a target, arg4 = after the homing dash | confirmed |
| `ground_slide` | 0x044F | Short slide on the ground `{angle, speed, skip if target, after homing}` | strong |
| `chase_dash` | 0x001D | Chase dash toward the foe (state 0xA) | confirmed |
| `take_off` (keyed) | 0x0123 | Switch to air action where ground-follow hands over to air motion | confirmed |
| `enter_state` (keyed) | 0x0000 | Enter the movement state the move armed (jump fall, dodge, knock-back, homing …) | confirmed |
| `steer_during_move` | 0x0518 | The stick steers during the move `{turn ×, speed ×}` (Kuja, Ultimecia) | strong |
| `turn_or_swivel` | 0x0811 | arg2 = 0: turn to the target; arg2 = 1: twist the drawn body toward it within limits | confirmed |
| `turn_to_target` / `turn_to_target_or_aim` | 0x046D / 0x086E | Turn to the lock-on target (fallback the foe), with reeling / guarding conditions | confirmed |
| `face_flight_direction` | 0x0210 | Face along your own velocity (blow-away reactions) | strong |
| `stop_turning` (keyed) | 0x0024 | End any turn in progress | strong |
| `aim_bones_at_target` / `aim_bones_at_foe` | 0x0D64 / 0x0D63 | Up to two bones aim at the target / foe within limits (guns, bows) | confirmed |
| `no_body_collision` | 0x0013 | Pass through fighters (dashes, EX Bursts) | confirmed |
| `heavy_body` | 0x004B | Push mass ×10 | confirmed |
| `stop_momentum` | 0x0038 | Zero velocity each tick (floating fighters) | confirmed |
| `floating_physics` | 0x0030 | Air-drag momentum; landing keeps velocity | strong |
| `gravity` | 0x013B | Gravity multiplier (Emperor 0.01, Kuja 0.25) | confirmed |
| `root_motion_3d` | 0x0062 | Root motion rotated by the full body orientation | strong |
| `pull_foe` | 0x0539 | Pull the foe (and their assist) toward you | confirmed |
| `ground_contact_switch` | 0x0070 | On ground contact, switch to the action's alternate script (Kain) | strong |
| `pin_physics_group` | 0x0216 | Whole hair/cloth physics group follows the animation (642 uses) | confirmed |
| 0x0117 (ranged) | 0x0117 | A physics group's rest pose follows the animated pose | strong |

#### Look, sound, EX Bursts
| Name | Op | What it does | Confidence |
|---|---|---|---|
| `fade_self` / `fade_self_in` (keyed) | 0x0340 / 0x0109 | Fade yourself out and back (Golbez teleports) / in | confirmed |
| `fade_foe_out` / `fade_foe_in` / `set_foe_speed` (keyed) | 0x0220 / 0x011F / 0x0121 | EX Bursts: fade the foe out / in; freeze or restore the foe | confirmed / strong |
| `slow_foe_exburst` | 0x0126 | EX Burst: the foe runs at arg0 × speed (0 = frozen) | confirmed |
| `slow_motion_exburst` | 0x0134 | EX Burst: the whole game at arg0 × speed | confirmed |
| `exburst_free_victim` / `exburst_release` | 0x003E / 0x0031 | EX Burst: stop holding the victim | strong |
| `screen_flash` (keyed) / `white_flash` | 0x051B / 0x012B | Full-screen flash (white / black / red) | confirmed |
| `dim_stage` | 0x0133 | Darken the stage (arg0 × colour) | confirmed |
| `screen_shake_range` | 0x0353 | Screen shake that lasts the range | confirmed |
| `caption` (keyed) | 0x021D | 2D caption from the character's pack (EX Burst names, move branches) | strong |
| `pulse_tint` / `pulse_tint_partner` | 0x0475 / 0x0476 | Pulsing colour tint on you / your partner | confirmed |
| `no_shadow` | 0x003D | No drop shadow | strong |
| `feathers` | 0x072C | Falling feathers from random bones | confirmed |
| `timed_effect` | 0x0466 | Effect once at a total motion time | confirmed |
| `trail_length` | 0x022E | How far the weapon trail reaches along a bone | confirmed (trail strong) |
| `sound_if_part_visible` / `sound_if_form` (keyed) | 0x0311 / 0x0312 | Sound only if a draw part is visible / in a form | confirmed / strong |
| 0x0117 (keyed) | 0x0117 | Eject a gun cartridge from a bone (Laguna, Squall) | confirmed |
| `prop_hit_volume` / `prop_hit` / `prop_effect` / `prop_fade` | 0x025E / 0x045B / 0x055F / 0x0360 | Separate weapon props (Gilgamesh): their hit volumes, hits, trails, fades | confirmed |

#### Event filters (keyed `x`, ranged `pad`)
- Low byte = **phase**: 0 = always; otherwise the event runs only when the motion_map phase of the script playing the shared animation matches. That is how one animation serves several moves.
- Next byte = **mode**: 1 = normal, 2 = EX Mode only (4/5/6/3: EX variants and actor kinds).
- Keyed bytes 2/3: actor+0xD60 (form) + 1 and setup byte 7 + 1.

#### Timeline layout (corrected)
- The header is 8 bytes: u16 motion, u8 ranged count, u8 keyed count, u16 keyed offset, u16 size.
- Ranged events start at +8 (what the tools call the "header slot" is just ranged event 0).
- A timeline may have no ranged events: `charmod_objx` handles that since round 2, and all 2,748 retail timelines decode and re-encode byte-exact.
- The game finds a timeline by motion id, first match.

### Settled in round 2 ([N.md](archive/event-ops/N.md))
- **`can_act_now` args:** any of arg0/2/3 blocks jump, dash and steering (attack, block, dodge still work); arg2 = next move must differ, arg3 = must be the same move, arg1 = no hit-confirm cancel. 22 retail uses are non-zero.
- **`motion_camera` presets:** 0x38-byte records from the runtime-loaded `camr` section (spring constants, blend rates, look distance, flags: smooth follow, look at foe / self, keep up vector, freeze foe …). The preset values themselves are in `camr`, not yet dumped.
- **`face_texture` groups:** numbered per model (TexAnm block names); 0/1 = eyes, 2 = mouth for the fighters; Shantotto's group 3 needs her model.
- **Push-apart masks:** at 0x089C8130/38/40 (not 0x089D8130). They are the battle-start "anchored" bit and world-map piece bits; in battle, `cant_be_pushed` is the only push setting a move has.

### Still open
- Ability names for 0x136 / 0x41 / 0x40.
- Which stats `timed_buff` changes.
- Keyed 0x0037 (releases a hold state, weak), 0x0071, 0x0131's listener.
- 0x0051 / 0x0077 (no reader found).
- The `camr` preset values.

### Engine facts found on the way
- **Actor fields:**
  - +0x180 = current script id.
  - +0x2FC = current target; +0x300 = the 1v1 opponent.
  - +0x380 bit 8 = in an action; bits 0x800/0x1000 are set by the on-hit handler `FUN_08838734`.
  - +0x542 = actor kind (0/1 = fighters).
  - +0x534 = two 6-entry hit histories.
  - +0x5D4 = a summon's owner.
  - +0x1028/+0x1068 = the per-tick follow-up list (not a hit list).
- **Script id ranges:** 400–469 = evade/guard (400/410/440/460 dodges, 470/471 block, state 0x15); 500–798 = hit reactions; 700/701/750/751 = blow-away.
- **vtable:** base 0x089C7C78; slot +0x150 = "can start attack", +0x158 = "can block".
- **Manager modules:** 0x08AC9xxx = EX_REVENGE_MANAGER; `FUN_08B01B74` = ASSIST_MANAGER::Update.
- **Effects:** an effect's lifetime is at fx+0xE4. Ground-effect queue at actor+0xD08 (2 per tick), processed by `FUN_0885436C`.

## Dissidia 012: how a move is defined

> Early notes (15–16 Oct, from the first look at the EU disc). Later sections and docs supersede them where they disagree. The event list below was guesswork from timing; the table above is the traced version.

Worked out from Cloud's `obj/p_sev100.objx` and model `obj/p_sev100_1p.gmo`, cross-checked against Yuna (`ten110`), Lightning (`gst130`) and Bartz (`fiv100`).

### Chain from menu to animation
1. **command_list_param.bin** (`ACMD`, 23,268 B): the menu layout. Records are 40 bytes: a u16 group header, then up to 18 u16 command IDs.
2. **`comb`** (in the objx): commands. Header is `u16 6, u16 count`, followed by a u32 offset table. Each command record is `u16 commandId, u16 stepCount, u16 prereqA, u16 prereqB`, then steps of `{u16 actionId, u16 0, u16 0xFFFF, u16 flags}`. The prereq fields chain combo branches.
3. **`plya`**: actions. Header is `u32 0x16, u32 count`, then 116-byte records. Fields: +0 actionId, +0x1C scriptId (10000+ = character moves).
4. **`p_se` type 7**: the script→motion map. Rows are `{u16 scriptId, u16 motionIndex, u32 phase}`.
5. **GMO motions**: motionIndex is the position in the model's motion list.
   - Animations live inside each model GMO. Player models have about 83–86 motions; assist models have 14–18.
   - About 60 IDs are shared by every character. Attacks use 6xx–8xx; the A/B/C suffix marks phases of one move.
6. **`p_se` type 8**: per-motion event timelines, one per motion index.
   - Header u16[8]: motion, (keyedCount<<8 | rangedCount+1), offKeyed, size.
   - Ranged events: `{u16 start, u16 end, u16 op, u16, u32, u32 × (op>>8)}`, after a variable 4–20 byte preamble.
   - Keyed events: `{u16 frame, u16 op, u32, u32 × (op>>8)}`.
   - 184 of 189 timelines decode and re-encode identically.
7. **`atkp`**: 68-byte hit profiles, name at +52.
8. **`magi`** projectiles, **`brat`** brave-attack data, **`effe`** MPK effects, **`snd_`** SSCF sound bank, **`text`/`spec`** mlng text and name banners.

### Event opcodes
- **0x011B = show weapon (confirmed on Bartz).** A ranged event with 1 arg: the bone index of a weapon part's draw bone (wep0…wepN, the bone carrying DrawPart 0x804E). The weapon is visible for that frame range. Bartz has 17 weapon parts, each skinned to its own weapon bone (r_wep1..6, l_wep..l_wep5, star). His HP attack _802A shows most of them. Weapon bones stay at scale 1, so hiding is done by this event, not by scaling.
- Guesses from timing (Cloud's Rush):
  - 0x042A hitbox (×8 per move, active frames)
  - 0x0200 attack active
  - 0x0101 recovery window
  - 0x0402 movement
  - 0x0315 effect, 0x0925 weapon trail, 0x060F effect end
  - 0x0311/0x0303 sound
  - 0x0422 face/blink
- Other Bartz-only ops: 0x072C (7 args, in HP attack), 0x0134 (float 0.3), 0x0108 (arg 0x6b3… likely an effect/sound ID).

### Adding moves
- Easiest: overwrite a donor character's existing animations in place (same `_NNNA` names). The move, its event timeline and hitboxes keep working; then adjust timing in the studio.
- New motion slots (new Actions) are appended to the motion list. Blender's *New animation (from…)* makes one as a copy of an existing Action with a copy of its events; *Save moves to folder* gives any new Action without events an empty timeline (type-8 entry). The Studio's *Duplicate this animation* does the same on the data side (motion chunk + timeline, into every model of the moveset) and can re-point the selected press's motion_map row at the copy.
- Brand-new commands also need the ACMD menu entry, plus CP cost and learn level (location unknown; collection_param.bin also contains 0x139/0x13A).
