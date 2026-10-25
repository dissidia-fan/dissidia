# Character mods: reference

Everything in a character folder, field by field: `mod.json`, the moveset files, avatars, bodies and skins, weapons, recipes, the move system and every timeline event, adding and sharing characters. The short guide is [../mods/README.md](../../../../mods/README.md). Paths are relative to `psprecomp/games/dissidia/`; `charmod.py` is `tools/charmod.py`.

## Changing a retail character (`"kind": "retail"`)

A folder can also change one of the game's own characters instead of adding one. The Move Studio makes them: open
the game ISO there, change a retail character, press *Save as mod*, and you get `mods/<name>/` (`mods/cloud`):

```
mods/cloud/mod.json              "kind": "retail", character {name, code}, files: game path -> folder path
mods/cloud/moveset/p_sev100/     the fighter's moveset, unpacked like any other (moves, hits, timelines ...)
mods/cloud/moveset/p_sev100_a/   the assist's
mods/cloud/models/               the 1P rigs (open them in Blender; animation changes are saved into them)
```

`charmod.py build` packs each file and installs it over the retail path (`obj/p_sev100.objx`) only when it differs
from the retail file, so an untouched assist or rig is not installed. No slot, roster line, avatars or move names are
involved, and the retail character is changed everywhere it appears. `"enabled": false` (or deleting the folder) takes
the files out of the mods dir again at the next build of all mods (`run.sh` does that at launch), which brings the
retail character back; `readme_charmod_retail.json` in the mods dir records which folder installed which file. Two
folders can't change the same file. Animation changes in `models/` reach the game for the 1P costume only (each
costume file carries its own animations).

## A character folder (`mods/<name>/`)

| Path | What | Edit it to |
|---|---|---|
| `mod.json` | the character's settings, in sections (below) | rename moves, switch body, change runtime settings |
| `avatars/` | every menu / HUD picture as a PNG, plus `source/` (the artwork they were cut from) and `_preview.png` | swap pictures |
| `moveset/p_<code>/` | the moveset (`obj/p_<code>.objx`), unpacked | change moves, timing, damage, projectiles |
| `moveset/p_<code>_a/` | the assist moveset | same, for the assist |
| `models/` | the model rig: skeleton, all animations, weapon and effect parts, with the default body and weapons in it | (binary; made in Blender) |
| `skins/<skin>/` | bodies (and weapons) that can replace the default ones | add your own model |
| `files/` | everything else the character owns, at its game path (shaders) | |

Files the character just borrows from the retail character it is built on (voice, `d_<code>`) are listed in `mod.json`
as `{"like": "<retail path>"}` and read from your disc at build time, so the folder never holds game files it didn't change.

### mod.json sections
| Section | What it holds |
|---|---|
| `_read_me` | a short how-to |
| `character` | `name` (what menus call it), `full_name`, `series`, `code`, `slot`, `based_on` / `based_on_id` (the retail character it is built on) |
| `avatars` | one entry per menu / HUD picture: `picture` (PNG in avatars/, or null = the based-on character's), `size`, `shows` (where it appears); `generated_from` = the artwork and crop boxes they were made from |
| `body` | `skin`: `"manikin"` or a folder in skins/; `models`: the model files the body goes on |
| `move_names` | in-game name and help text per move (`cmd` = the command in moveset/p_<code>/moves.json) |
| `runtime_hooks` | settings read by the game runtime (roster.cpp: `warp=`, `landact=`, `apocarms=` ...) |
| `menu_ids` | entry ids in the shared menu pack (set by new_character.py; leave as they are) |
| `files` | every other game file: folder path, or `{"like": ...}` |
| `fx_shaders` | the character's shader overrides (merged into fx_shaders/manifest.json) |

### Moveset files (`moveset/p_<code>/`)
- `moves.json`: commands. Each ability points at one by its `cmd` id (`_info` names the move). `steps` = the presses,
  each an action id.
- `actions.json`: action id -> script id (rest kept as hex).
- `motion_map.json`: script id -> motion number. The motion number is both the animation (in the model) and the timeline.
- `timelines/NNN.json`: the events of motion NNN, one per line. Times are in ticks (60 per second, 2 per animation frame).
  `ranged` events run from `start` to `end`, `keyed` ones fire at `frame`. Known events have names (`hit_window`,
  `chain_window`, `fire_projectile`, `show_part` ...; see "Making moves"); `args` are numbers, or `{"f": 1.5}` for floats.
- `bones.json`: the model's bone names, so timelines can name parts (`"part": "wep0"`). Comes from the model.
- `hits.json`: every hit profile, one per line. **`power`** = brave damage per hit (retail single hits 3-8, finishers
  10-16, keep it >= 1), **`rehit`** = 0 for one hit per window or N to hit again every N ticks, `reaction` and the
  `kb_*` / `stun` / `follow` fields = where the foe is sent and for how long. `_info` at the top explains each field.
- `projectiles.json`: projectile records (speed, homing, shots ... in `params`, as indices into `pool`; `values` shows
  what they resolve to).
- `sections/*.bin`: parts not decoded yet, kept as they are (effects `effe`, sound bank `snd_`, `brat`, `spec` ...).
- `arc.json`: the order the parts go back together in. Leave it alone.

Packing is exact: an unedited folder builds the same bytes the game had (checked on every retail moveset).
Keys starting with `_` and `what` are notes; edit them freely, they are not packed.

## Avatars

Each avatar is a PNG in `avatars/`. `charmod.py avatars noctis` lists them and draws `avatars/_preview.png` (all of them
at 2x on a checkerboard). Noctis has:

| Avatar | Size | Where it shows |
|---|---|---|
| `grid_tile` | 32x64 | Quick Battle character grid |
| `slidein` | 256x256 | Player Select slide-in and the battle setup panel |
| `name_plate` | 128x32 | name plate on Player Select |
| `logo` | 128x64 | series logo plate on Player Select |
| `hud` | 128x128 | in-battle portrait at the bottom of the screen (P1 and CPU use the same picture, never mirrored) |
| `vs_face` | 64x64 | square face (VS screen / story mode) |
| `face_strip`, `face_strip_mirrored` | 128x32 | face strips in name panels |
| `name_text` | 128x32 | name text in menus |
| `encounter_text` | 256x64 | name banner when a battle starts |
| `select_portrait`, `arcade_last`, `talk_top`, `talk_bottom` | | still Bartz's pictures (`picture: null`) |

Three ways to change them:
1. **One picture**: replace the PNG (keep the size; another size is scaled and centre-cropped to fit, with a note), then
   `charmod.py build noctis`. Transparent pixels stay transparent. To give one of the null ones a picture, set its
   `picture` to a PNG path.
2. **All of them from one big picture** (a full-body render, ideally on a transparent background):
   ```
   charmod.py avatars noctis --from-picture my_art.png --crop grid_tile=2050,15,2550,748 --crop hud=2110,0,2760,700 \
       --tile-cutout --hud-flip
   ```
   Crop boxes are `x0,y0,x1,y1` in the picture's pixels, for `grid_tile`, `slidein`, `hud`, `face_strip` and `vs_face`
   (any you leave out are found automatically). `--portrait head.png` adds a head-and-shoulders picture for the tile, HUD
   and faces. The picture and boxes are saved under `avatars.generated_from`; tweak a box there and run
   `charmod.py avatars noctis --redo`. Check `avatars/_preview.png`, then build.
3. **Names and logo**: `charmod.py avatars noctis --names` redraws the name plate, name text and encounter banner from
   `character.name` / `full_name` (and the logo plate from `series`); `--logo art.png` puts your logo artwork on the plate.
   The current Noctis name pictures were made with an earlier version of the drawing code, so `--names` changes their look
   slightly.

An unchanged PNG builds exactly the bytes the game has now (each picture is encoded with the retail file as its template).

Renaming: change `character.name` (and `full_name`), run `charmod.py avatars <name> --names`, then build. The Opponent Info
text record still comes from new_character.py.

## Body

`mod.json` → `body.skin`:
- `"manikin"` (the default): the like's manikin body (for Noctis, Bartz's crystal manikin from `obj/p_fiv100_zako.gmo`
  on your disc) goes on the models listed in `body.models`. The warp-dash ghosts are rebuilt from it.
- a skin name: the body in `skins/<name>/` (for Noctis, `noctis-ffxv` is the FFXV body he had when the folder was made).

```
charmod.py body noctis noctis-ffxv                               # use that skin
charmod.py body noctis manikin                                   # back to the manikin
charmod.py body noctis --from-gmo my_noctis.gmo --as my-noctis   # make a skin from any model on the same skeleton
charmod.py build noctis
```
`--from-gmo` takes part 0 (the body) of a model exported from the Blender add-on on the like's skeleton, with its weights
and textures; the character's animations, weapons and effects stay as they are. The body may use at most as many
materials as the rig's body slots (Noctis: 2).

### Weapons

Weapons taken from other games live in a skin too, never in the rig. `mod.json` → `body.weapons` lists them:
- `parts`: `{rig part: donor part}`; the donor is `default_from` (for Noctis, Bartz's `obj/p_fiv100_1p.gmo`, read from
  your disc). Without a skin that carries weapons, each listed part shows the donor's own weapon on its own materials
  and bones. A rig part on another bone (the warp dash's thrown sword) gets the donor mesh held on that bone.
- `ghosts`: the blade ghost stages, rebuilt from the donor blade (`of`) and tinted like the warp ghosts.
- `textures`: the weapon-only textures (no other part uses them). Blank (grey) in the rig, filled by the skin.

For Noctis the listed parts are the War of the Visions Engine Blade (`wep0`, `wep15`, `warp_sword_mesh`, the four
`blade_ghost`s), spear (`wep12`), greatsword (`wep13`) and daggers (`wep14`, `wep5`, `wep6`). They are in the
`noctis-ffxv` skin (`charmod.py body noctis noctis-ffxv`). The shield and shuriken (`wep10`, `wep18`) are original
placeholders and stay in the rig. `pull` keeps the split: the rig gets the default weapons back, and the installed
weapons go into the skin with the installed body. Effect parts (Royal Cleave, EX Burst) are not covered.

Not covered yet: the assist (`_a`) and `_w` models still carry Bartz's regular body, because there the body texture is
shared with two weapon parts (`charmod.py unpack` reports it). They are kept as they are.

### The model and weapons a character wants

Bodies and weapons taken from other games are not kept in the repo's rig; a skin made from them is your own copy.
`mod.json` → `body.model_hint` and `body.weapon_groups` say what the character should look like, and any model file
can fill each: the Move Studio's Body page (or Blender's Character mods > *Model and weapons*) has *Replace with my
model…* for the body and *Use my model…* for each weapon; Blender fits it and saves it into the skin (see
blender-addon.md).

```json
"model_hint": "Noctis Lucis Caelum: any humanoid model (the original used War of the Visions FF15ncts_00)",
"weapon_groups": [
  {"name": "Engine Blade", "parts": ["wep0", "wep15"], "hint": "held, and thrown in the warp strike",
   "length": 1.3018, "max_tex": 128}, ...]
```
- `parts`: the weapon parts that take the same model (a held blade and its thrown copy); with `body.weapons` the
  skin puts them on its `skin_materials`, gives `warp_sword_mesh` the thrown one's and rebuilds the blade ghosts.
- `length` (pommel to tip, game units), `flip`, `grip_at_origin`, `max_tex`: the defaults Blender's dialog starts
  with. Without `weapon_groups` every `wepN` part is listed alone (weapons drawn with one picture change together).

## Working with the other forge tools

Many older scripts (`forge/ffxv_tools/...`) write straight into `work/mods`. `build` remembers what it last wrote; if an
installed file has changed since, it stops and lists it instead of overwriting newer work:

```
charmod.py pull noctis     # bring the installed files back into mods/noctis (keeps the body choice and skins)
charmod.py build noctis
```
`build` also writes the character's roster line (`work/mods/roster.txt`) and ability names (`extra_abilities.json`), merges
its shader entries into `fx_shaders/manifest.json`, and reruns `roster_mainbin.py` when the roster or names changed.

## Making moves

The easiest way: the **Move Studio** (`studio/index.html` at the repo root, see [studio/README.md](../../../../studio/README.md)) shows the animation, the events on a timeline and each hit's damage in one place, and saves into these files. What follows is the same by hand.


### How a move fits together
```
mod.json move_names "0C"   the ability the player equips: name, help text, cmd
        │ cmd 0xA2
moves.json command 0xA2    steps = the presses of the combo, each an action id   (1934, 1935, 1936)
        │ action
actions.json               action -> script id                                   (1934 -> 10003)
        │ script
motion_map.json            script -> motion number                               (10003 -> 45)
        │ motion
timelines/045.json         what happens during that animation, tick by tick
models/*.gmo               the animation itself (motion 45), made in Blender
```
Inside a timeline (ticks: 60 a second, 2 per animation frame), events have names:

| Event | Fields | Links to |
|---|---|---|
| `hit_window` (start..end) | `hit`, `group` | `hit` = a name in `hits.json`: damage `power`, `rehit`, `reaction`, knock-back, `hit_effect_bit` (the spark played on contact). `group` = the set of hitboxes (reuse a number an existing move uses) |
| `chain_window` (start..end) | | when the next press of the combo is accepted |
| `fire_projectile` | `projectile` | a record in `projectiles.json`: its `atkp` = the hit it carries (shown in `_note`), `effect` = its visual |
| `effect` | `effect` | an effect from the character's effect bank |
| `sound` / `screen_shake` | `sound` / args | sound bank / shake strength |
| `show_part`, `hide_part` (start..end), `show_part_once`, `hide_part_once` | `part` | a weapon or effect part, by its bone name in the model (`bones.json`) |
| `chase_window` (start..end) | `hit` | after this hit blew the foe away, a press starts the Chase |
| `chain_followup` (start..end) | `flag`, `action`, `even_on_miss`, `mid_action` | chain window that also offers `action` as the next move (on matching input) |
| `forced_hit` | `hit` | hits the foe with that hit at once, no hitbox or range check (EX Bursts) |
| `can_act` (start..end) | | neutral: may attack, block, dodge (stand/idle/fall motions carry it whole) |
| `can_act_now` | | end of recovery: may act at once |
| `dodge_cancel` / `dodge_cancel_if_foe_free` | | may dodge out of the move (the second only if the foe isn't reeling); also when the CPU picks its next move |
| `attack_over_cpu`, `cpu_hint` | | read only by the CPU AI (when to punish; how to reposition) |
| `invincible` (start..end) | | incoming hits pass through (dodges: first 20 ticks) |
| `no_assist_cancel` | | the move can't be cut short by Assist / EX Revenge |
| `cant_be_pushed` | | other fighters are pushed off this one |
| `face_target` / `turn_to_opponent` | args | turn toward the target (arg0 = max degrees per tick) |
| `air_motion`, `ground_follow`, `follow_floor_slope`, `landing_allowed`, `keep_motion_when_falling` | | air / ground handling (see the move-system notes) |
| `face_texture` (start..end) | `face_part`, `pattern` | switches a face texture (eyes 0/1, mouth 2) for the range |
| `pin_physics_bone` | `part`, `max_deg_per_tick` | hair / cloth bone follows the animation instead of the physics (180 = snap) |
| `motion_camera` | `preset`, `camera_bone` | cinematic camera from the model's camera bone (EX Bursts) |
| `bone_effect` (start..end) | `slot`, `record`, `record_bit`, `scale` | effect on a bone while it runs: `record` 9999 = weapon trail, 8888 = other weapon/hand effects |
| `voice` | `voice` | a voice line (ids like 1810xxx) |
| `footstep_sound` | `kind` | footstep / landing / impact sound chosen by the floor surface, plus dust at a foot |
| `ground_dust` | `part`, `offset`, `size`, `variant` | dust / splash where that bone touches the floor |
| `homing_dash` | `speed`, `track_ticks`, `height`, `angle`, `distance`, `arrive_radius`, `max_ticks`, `vspeed` ... | dash toward the foe (air moves), ends before the first hit |
| `summon_effect` | `effect`, `lifetime`, `part` | summons only (Y'shtola's aeons) |
| `weapon_volume`, `summon_object`, `hold_prop`, `end_of_move` | args | |
| `0xNNNN` | args | events not identified yet: keep them as they are. What every event does: `../docs/moves-and-events.md` |

Names and numbers both work: `"hit": "NEAR_COMBO"` or `"hit": "0x04A1"`, `"part": "wep0"` or `"part": 64`,
`"op": "hit_window"` or `"op": "0x0200"`. If a name isn't found, the build says which one.

`charmod.py hits <folder> [move]` prints every hit of every move with its press and ticks.

### Common changes
- **Damage, knock-back, hit-stun:** edit the hit's line in `hits.json`. A hit used by several moves changes all of them;
  to change one move only, copy the line with a new unused `id` and a new `name`, point that move's `hit_window` at the
  new name, and edit the copy.
- **Timing:** change `start` / `end` of the events in the timeline (hit windows, chain window, effects).
- **Reorder or extend a combo:** edit the command's `steps` in `moves.json`. Each step is one press; `flags` 0x100 = needs
  a press, 0 = plays by itself once the previous hit connected (a follow-up). Steps can reuse actions of other moves.
- **A brand-new move:** add a command with a new id to `moves.json`, and an entry in `move_names` with `"copy"` (the ability
  to clone, e.g. `"0F"`), `"cmd"` (your command id), `"learn"` (level), `"name"` and `"help"`. The build adds the ability
  to the character's list (main.bin is rebuilt). The Move Studio does all of this (*+ New move*), and can copy a move,
  with its animations, hits, hitboxes, projectiles, effects and sounds, from another character (*Copy a move from…*).
- **An ability's cost:** any `move_names` entry may set `"cp"` (CP to equip; `"cp_mastered"`, default half), `"ap"` (AP
  to master), `"bonus"` (the equip bonus: a system.bin `fncs` effect set, `null` = none) and `"learn"` (the level it is
  learnt at, also for abilities the based-on character has). See docs/archive/ablt-fields.
- **New animation:** animations live in the model. Make it with the Blender add-on ([blender/README.md](../../../../blender/README.md): create a `_NNNA`
  action, *Copy events from…* to give it a timeline, *Export model + moveset*), copy the exported files into work/mods, and
  `charmod.py pull <folder>`; its timeline appears as `timelines/NNN.json`.

### Limits
- Effects themselves (the `effe` bank, `sections/*_effe_*.bin`) and hitbox shapes (`sections/*_p_fi_5.bin`) are binary
  files: edit them in the Move Studio (Effects tab; a hit window's hitboxes), not by hand.
- Unknown fields (`unk_*`, step `b` / `c`, event `x` / `pad`) are kept as they are; leave them unless you know what they do.

## Adding a character

**Drop the folder in.** A character folder put in `mods/` (at the repo root) is added to the game automatically:
`tools/run.sh` runs `charmod.py build` before the game starts (skip with `PSPRECOMP_CHARMOD=0`), and the build gives a
character the game doesn't know yet its place: its roster slot (`character.slot`, or the next free one 35-42 if that one is
taken), menu ids for its avatars, its name in the menu text, its records in main.bin. The slot and ids are written back to its
`mod.json`. A folder made on another machine works the same way: everything it borrows is read from your own disc.

**Starting one from scratch:** `mods/_template/` is the template (never built itself). Make a character from it with:
```
games/dissidia/tools/charmod.py new terra --like six100 --display Terra --full-name "Terra Branford" --series "FINAL FANTASY VI"
```
- `--like`: the retail character it starts from (code, e.g. `fiv100` Bartz, `eht100` Squall, `sev100` Cloud, `six100`
  Terra, `gst130` Lightning; or a charId). Its skeleton, animations, models, voice and moves are the starting point.
- `--code` (default: from the folder name + 100), `--slot` (default: the first free one 35-42).

It makes `mods/terra/` with `mod.json` filled in, every avatar listed (still the based-on character's pictures), a grey
placeholder picture for the menu art, and the based-on character's moveset unpacked in `moveset/p_<code>/` so its moves
can be edited straight away. Every file not in the folder is the based-on character's (`"files": {"_auto": true}`).
Then:
1. Pictures: set the picture and its crop boxes in the Move Studio (Character tab), or `charmod.py avatars terra
   --from-picture my_art.png`. The build cuts the menu pictures from `avatars.generated_from` the first time and whenever
   the picture, a box or an option changes; a picture set by hand (`"_own": true`) is never replaced.
2. Moves, move names, body: as above.
3. Start the game: it is added to the roster the first time.

The Move Studio does all of this too: character picker > *+ New character…*.

A folder whose name starts with `_`, or whose `mod.json` has `"enabled": false`, is never built.

Deleting a folder, or `"enabled": false` in its `mod.json`, takes the character out of the game at the next build (roster slot, menu-pack entries, name text, main.bin records), as `tools/new_character.py --slot N --remove` does.

**Limit: 6 or 7 extra characters.** The shared menu pack (`by_crc/8f593261.bin`, menu_pk_free_battle_gim_3: 130 retail entries, entity ids from 0x707F) can grow to 161 entries (ids up to 0x711F); each extra character takes 5 (grid tile, slide-in, its copy for the setup panel, name plate, logo plate), or 4 when it uses the game's own logo (series FINAL FANTASY I..XIII: `logo=0..12`, the grid record's f2, entity 0x706F + f2; `new_character.retail_logo`) or shares a generated plate with another slot of the same series (`shared_logo`; a shared entry is only emptied when its last user goes). A folder with its own logo picture keeps its own entry. So 31 spare entries hold 6 or 7. Measured 24 Oct: a 7th character made the setup screen lose its Enter Battle button. The build refuses a character that doesn't fit (`charmod.py` `MENU_PACK_MAX_ENTRIES`). The table in the menu engine that runs out is not found yet; raising it in the runtime would lift the limit.

`unpack` options: `--skin-name` (name for the body found installed; default `installed`), `--keep-body` (make that body
the default instead of the manikin), `--fx-prefix alterna/` (shader entries that belong to the character).

## Sharing a mod
A mod folder is **data only**: `mod.json`, the moveset's JSON and binary sections, models (`.gmo`), skins (`.skin`),
pictures, sounds and fx shaders. The build only ever reads those, and it refuses a folder with a script or program in it
(`.py`, `.sh`, `.exe`, `.app`, `.blend` …, or any file marked executable), so a shared mod can't carry code. Other unknown
files are left out with a note. So:
- Share the folder (zip `mods/<name>/`). Whoever gets it drops it into their own `mods/` and starts the game; everything
  it borrows from the retail game (`{"like": …}` and `_auto` files) is read from their own disc.
- Don't share `.blend` files to install: Blender can run scripts saved in them. Keep Blender's *Auto Run Python Scripts*
  off for files from other people.
- Never run a script that came with a mod. Nothing here needs one.

## Tools
- `tools/charmod.py`: the command above.
- `tools/charmod_objx.py`: moveset <-> folder. `tools/charmod_gmo.py`: body skins. `tools/charmod_avatars.py`: avatars. `tools/charmod_magi.py`,
  `charmod_chunks.py`, `charmod_gim.py`: copies of the forge add-on's projectile, GMO-chunk and texture codecs.
- Needs Python 3.11 or newer, nothing else to install: the first run puts numpy, Pillow and scipy, at the versions in
  `tools/requirements.txt`, in `tools/.venv/` (`tools/pydeps.py`; one download, about 60 MB). Same versions everywhere,
  so a build writes the same bytes on every machine.
