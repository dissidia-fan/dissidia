# Spawned objects: the wpnp table, hold_prop and summon_object

Gilgamesh's swords, Yuna's aeons and Bartz's mimicked weapons are separate objects with their own model, motions,
timeline and effect bank. Two ranged events spawn them; a row of the battle parameter table `wpnp` says which
object. Read from the recompiled EBOOT (`psprecomp/work/output/generated/batch_0024.cpp`) and the retail data
(25 Oct 2026). Move Studio draws them (`studio/spawn.js`, [move-studio.md](move-studio.md#spawned-objects)).

## The wpnp table

`general_archive/main/main.bin` (MPK) → `battle.bin` (ARC) → section `wpnp` (the battle parameter pack FUN_088135D4
loads: atkp, spec, nekp, scrb, camr, chrg, scra, wpnp). Header `{u32 version = 11, u32 count = 84}`, then 84 rows of
0x3C bytes. The game copies it to 0x09B16930 (FUN_08850AB8) and finds a row by its index (FUN_08850A40); the
event's row argument is that global index. `roster_mainbin.py` already clones rows for new objects (`objs=`).

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u32 | its own index |
| +0x04 | u16 | owner charId (25 Gilgamesh, 28 Yuna, 9 Bartz) |
| +0x06 | u16 | flags (0x0100 sets object +0x614 and is tested when a held prop changes row; 0x0020 and 0x0080 are tested at spawn; meanings not traced) |
| +0x08 | u16 | **objentry id** of the object (objentry.bin record; its file stem at +0x34 → `obj/<stem>.objx` + `.gmo`) |
| +0x0C | s16 × 8 | the prop's own atkp table (`prop_hit` 0x045B's modifier; Gilgamesh only) |
| +0x1C | s16 | **script the object plays** at spawn (its own motion_map; FUN_0882FA10). 0 = none (static weapon) |
| +0x22 | s16 | **effect record** started on the object at spawn, from the object's own bank (FUN_088A9DE8; Gilgamesh 104 = `FIV211_WEP`) |
| +0x2A / +0x2C / +0x2E | u16 | objentry id used instead of +0x08 when the owner is a manikin (`_ZAKO`), an assist (`_A`), both (`_ZAKO_A`) |
| +0x30 | s8 × 8 | hit-volume groups (`prop_hit_volume` 0x025E slot → group) |
| +0x38 | u8 × 4 | probably the weights of the random pick FUN_08850574 rolls (`rand % 101` against their running sum) |

Retail rows: 0–14 and 32–76, 83 Yuna (w_ten111 … w_ten115, scripts 8101–8103 for the EX Burst, 100xx for the
summons); 15–22 Gilgamesh (w_fiv211 … w_fiv218, no script, effect 104); 23–31 no object (charId fields are not
ids); 77–82 Bartz (w_fiv101 … w_fiv106). Dump: `python3 -c` over `roster_mainbin.mpk_unpack` / `arc_unpack`.

## The events

Handler 0x0882C570 (`hold_prop` 0x075A) and 0x0882C668 (`summon_object` 0x0C65), fighters only. They spawn at the
range's first 2 ticks only (`now − start < 2`). If an object with that id is already out, FUN_0884F1C8 keeps it
(same object: stays; another row of the same objentry: takes the new row) instead.

| Arg | hold_prop | summon_object |
|---|---|---|
| 0 | **id = the owner's bone it hangs on** (0x4000 = the owner itself). Gilgamesh 37 `l_wep`, 61 `r_wep`; Yuna 55 `summon1`, 56 `summon2` | same |
| 1 | wpnp row | same |
| 2 | ≠ 0: use the row the game picked (list +519, FUN_0884F90C / FUN_08850A64) instead of arg 1 — Gilgamesh's weapon roll | same |
| 3 | ≠ 0: **let go** — placed at the bone as it appears, then left in the world (FUN_08851078 sets +0x611 and clears the id) | same |
| 4, 5 | fade-in / fade-out ticks | same |
| 6 | start time of the object's script | x offset |
| 7, 8 | | y, z offset in the bone's frame (local matrix at object +0x550) |
| 9 | | ≠ 0: on the owner's root instead of the bone (+0x612) |
| 10, 11 | | ≠ 0: on the other object with id arg 11 (+0x613, +0x60C; FUN_088513BC) |

Spawn: FUN_08850574 → FUN_0884FA84 builds the object (FUN_08850BE0, kind 3), stores owner / id / let-go at
+0x5D4 / +0x60A / +0x610 (FUN_08851058), starts the row's script, plays the row's effect, then the fade-in. Every
tick FUN_08851078 sets its matrix from FUN_08851480: bone `id` of the owner (FUN_08836648 → FUN_0886B4CC × the
owner's world matrix) × the local offset. The child list is actor+0xD64 (16 × 0x20, [J.md](archive/event-ops/J.md)).

## What the objects do

- Their timeline is in their own objx (`motion_map` + `timelines`). Aeons use `fade_self_in` (0x0109), hit
  windows, and `summon_effect` (0x0335): an effect from the **owner's** bank on the object's bone
  ([E.md](archive/event-ops/E.md)). Gilgamesh's swords have no timeline.
- The owner's `prop_effect` (0x055F) plays a trail record from the **prop's** bank on the prop (`w_fiv211`
  record 9999 `FIV211_KISEKI`), `prop_fade` (0x0360) fades it, `prop_hit` / `prop_hit_volume` make it hit.
- `p_fiv210_w.objx` / `p_fiv210_1p_w.gmo` are not a spawned object: no wpnp row names them.

## Open

- Which row Gilgamesh's roll picks (list +516..+519, the +0x38 weights) is not traced; the Studio shows the row
  the event names (15, Excalibur-like `w_fiv211`).
- `pad` 0x400 / 0x500 on some of Yuna's events (a mode byte of 4 / 5) is not understood; the Studio skips them as
  it skips other modes it does not know.
- Whether a new row of the same object restarts its motion (FUN_0884EDE4) is a reading of the branch, not tested.
- Gilgamesh's EX Mode weapons (ids 23/49, 25/51, 27/53) are spawned by overlay code (FUN_08A4C4C8 …), not by events.
