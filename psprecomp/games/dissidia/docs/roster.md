# Roster and new characters

Where the character count and code strings live, how extra charIds are added, and the extra characters' ability names. The step-by-step guide to making a character is [../mods/README.md](../../../../mods/README.md).

- [Dissidia 012 (EU) roster: where the character count and code strings live](#dissidia-012-eu-roster-where-the-character-count-and-code-strings-live)
- [The extra characters' ability names and help texts (19 Oct)](#the-extra-characters-ability-names-and-help-texts-19-oct)

## Dissidia 012 (EU) roster: where the character count and code strings live

Status: static map plus a data-patch route (`runtime/roster.cpp`). charId 34 ("noc100", Noctis) is a selectable
tile in the Quick Battle grid with its own face tile, slide-in, name plate and game logo, and a battle starts
with it (Bartz's model files served under the new code), including the VS screen (see "VS screen: solved, 17 Oct"). ROSTER_MAX (43 = 35 + 8) is one constant and drives
the grid bounds, the relocated menu/battle tables and the main.bin tool; ids above 34 are accepted by default (opt-out `PSPRECOMP_ROSTER_RETAIL_ONLY=1`) since the third pass, with the
residual risks listed under "Third pass" at the end.

### Key facts

* charId space is 35 slots, 0..34. 0 = none, **charId = table index + 1** (not the order a roster
  list suggests). 1..33 are retail characters. 34 is already used by the unfinished ORG800
  (object-table entries carry charId 34), so it is "spare but half used".
* Code strings are 8-byte NUL-padded ids (`fiv100` = Bartz) spliced into asset paths:
  `obj/p_%s_1p.gmo`, `menu/JP/.../p_%s_1p.gim`, `voice/.../voice_p_%s_00.scd`.
* The code is statically recompiled: code immediates (bounds such as `slti 0x21`) cannot be patched
  at runtime. Only data can. Code bounds need either a same-size ELF patch plus re-recompile of the
  touched batches, or a host reimplementation registered with `psp_dispatch_register`. Accessor
  functions that return a table address can be replaced by a host hook.
* Overlays (battle/menu/extra) load into the window at 0x089DD4C0. File offset = vaddr - 0x089DD4C0 + 0x54.

### Consumer map

| Address | Module | What it indexes | Elem | Bound / free slot 34 |
|---|---|---|---|---|
| 0x089D6CA8 | EBOOT | voice bank loader (FUN_088A3A94) code strings | 4 (ptr) | 35 slots, slot 34 free. Patched |
| 0x08AFBC68 | battle | story/encounter face-in names (FUN_08AF6B70) | 4 (ptr) | 35 slots, slot 34 free. Patched |
| 0x089EA3C0 | menu | arcade last-chara gim (FUN_089E9EB4) | 4 (ptr) | 35 slots, slot 34 free. Patched |
| 0x089EC568 | menu | arcade select `p_%s_1p.gim` (FUN_089EA4D0), charId comes from caller, unbounded | 4 (ptr) | 35 slots, slot 34 free. Patched |
| FUN_08A85ECC -> 0x08A864D0 | menu | Quick Battle select grid: per-charId record, halfword at +8 is the tile id | 12 | 33 records. Callers bound charId `slti 0x21` at 0x08A7F30C and 0x08A8019C |
| FUN_08A85ED8 -> 0x08A8665C | menu | second grid table | ? | follows the first table directly |
| 0x08A87C82 | menu | 34 tile sequence ids for the grid | 2 | fixed 34-tile layout |
| `menu_pk_upper_chara_select_seq_0.bin` | data file | cell positions and layout | | 34 cells, in a package file |
| 0x089CBC98 / `*(0x89D712C)+0xF58` | EBOOT | per-character save records: 31-entry id list, records of 0x778 bytes, unknown id gets a dummy record | 0x778 | unlock flags live here |
| PACKAGE sector 4 (crc 0x93307C1C), entries at 0xA0A8 | package | object table, 1172 entries of 0x44 bytes, charId at byte 6 | 0x44 | needs a new entry per file set |

Callers of FUN_08A85ECC: 0x8A7F2B4, 0x8A7FB54, 0x8A80130, 0x8A807EC, 0x8A81188, 0x8A8255C,
0x8A83304, 0x8A84458, 0x8A84FD4. Callers of FUN_08A85ED8: 0x8A81BB8, 0x8A81FB8, 0x8A8255C, 0x8A82F94.
Grid cells are 8 bytes at `*(obj+0x28)+0x678+8*i` (40 max): flags halfword (bit0 = active) and charId
halfword at +4. FUN_08A7F2B4 finds the cell index whose table tile matches an id.

Not mapped yet (no table address found): names, portraits beyond the gim paths above, stats, PP
catalog, assist, EX, story per-charId tables. The population function for grid cells and the
unlock source were not found.

### Route decision

Chosen: (a) game-module data patching, extended by hooks only where a bound blocks it.

* Data tables (all four code-string tables) take slot 34 with no code change. Done in `roster.cpp`.
  EBOOT tables are patched in `on_boot_context`. Overlay tables are patched in flight when
  `OVL_*_APP.ELF` is streamed (`hooks_io.cpp` tags the read, `dissidia_roster_patch_overlay`).
* Route (b), reusing the sev120 slot and DLC costume mechanism, was not chosen: it would replace
  a character and the DLC path was not examined this session.
* The Quick Battle grid needs more: accessor hook for FUN_08A85ECC returning a relocated 35-record
  table (scratch RAM 0x08700000+), host reimplementation (or `slti` patch + overlay re-recompile) of
  FUN_08A7F2B4 / FUN_08A80130, plus a tile and a cell in the layout data file.
* New asset files come from the loose-file mods dir, aliasing Bartz: `tools/package_extract.py alias`.

### Config

* `<mods>/roster.txt`: lines `34 noc100` (or env `PSPRECOMP_ROSTER="34=noc100"`). The string is up to 7 chars.
* `<mods>/by_crc/93307c1c.bin`: patched object table (two records cloned from Bartz, charId=34).
* `package_mods` NEW-entry path: verified that the index accepts 3 new `noc100` entries and the game
  boots to the menu (run005). Not verified that the game loads them, because nothing requests
  charId 34 yet. `roster.txt` is skipped as a non-asset file.

### Reaching the roster screen (button script, debug socket 9999)

Env: `DISPLAY=:99 PSPRECOMP_DISC0=$PWD/disc0 PSPRECOMP_MS0=$PWD/ms0 LIBGL_ALWAYS_SOFTWARE=1 PSPRECOMP_CROSS_MID=1 PSPRECOMP_NO_MPEG=1 SDL_AUDIODRIVER=dummy`
(run from psprecomp_work, 11-25 fps). Commands: `B <mask> <ms>`, `S path.tga`, `R addr len`.
Masks: START 8, CROSS 4000, CIRCLE 2000, UP 10, DOWN 40.
The first-run prologue takes ~25-30 min of input. The main menu was dead after the prologue until two
menu callbacks (0x089E0A88, 0x089E0AA4, LOOKUP_MISS) were hooked (`dissidia_menu_flag_cb` in hooks_main.cpp).
Then: Main Menu -> Quick Battle -> 1-on-1 -> Player Select grid. Baseline screenshot:
`psprecomp_work/shots/roster_grid_baseline.png`.

### ROSTER_MAX (one constant)

`runtime/roster_max.h` defines `DISSIDIA_RETAIL_SLOTS 35`, `DISSIDIA_ROSTER_MAX 43` and the scratch RAM layout
of the relocated tables. Everything else is derived from it:

| Consumer | How |
|---|---|
| `roster.cpp` | `kRosterMax` bounds `roster.txt` ids, sizes the grid record table (`kGridRows` = 33 + 8 = 41 rows of 12 bytes at `DISSIDIA_GRID_DST`, unused rows 0xFF) and the relocated tables |
| `tools/roster_patches.py [--write\|--check]` | writes the `# BEGIN/END ROSTER_MAX patches` block of `overlay_menu.toml` / `overlay_battle.toml` from the header: three grid `slti` bounds (-> 41), the name-table guard `slti 0x23` at 0x08A54194 (-> 43) and the `lui`/`addiu` pair of each relocated table |
| `tools/roster_mainbin.py --max` (default 43) | rejects charIds >= ROSTER_MAX when it builds chrp/objentry records |

Relocated tables (patched overlay code reads a 43-slot copy in guest scratch RAM; `dissidia_roster_patch_overlay`
copies the 35 retail entries into it while the overlay streams in, and writes the roster entries on top, with or
without a roster, because the patched code always reads the copy):

| Table | Overlay | Original | Copy | Reference patched |
|---|---|---|---|---|
| arcade last-chara gim names | menu | 0x089EA3C0 | 0x08700400 | `lui` 0x089E9EC4, `addiu` 0x089E9ECC |
| arcade select `p_%s_1p.gim` names | menu | 0x089EC568 | 0x087004C0 | 0x089EA4D4 / 0x089EA4DC |
| charId -> name text record (u32) | menu | 0x08A544E0 | 0x08700580 | 0x08A541A0 / 0x08A541A8 (+ guard 0x08A54194) |
| story/encounter face-in names | battle | 0x08AFBC68 | 0x08700640 | 0x08AF6CAC / 0x08AF6CC4 |
| Opponent Info name text ids (8 bytes/charId: id, alt id) | battle | 0x08AFB6E8 | 0x08701A00 | two references: 0x08AF90F0 / 0x08AF90F4 and 0x08AF9DB0 / 0x08AF9DB8 |

Each table has exactly one `lui`+`addiu` reference in its overlay (found by scanning the overlay code for the
hi/lo halves), which is why a two-instruction patch relocates it. After changing `roster_max.h` run
`roster_patches.py --write`, regenerate both overlays (`tools/build_overlay.py`, see `pipeline.sh`) and rebuild.
Changing the patch values regenerates only the batches that contain the patched functions (menu: 5 of 131).

NOT relocatable from here (EBOOT code, recompiled as one piece; a patch means regenerating and compiling the
whole EBOOT output): the voice bank loader FUN_088A3A94 (reads the 35 code strings at 0x089D6CA8 and fills 35 x
12-byte voice slot records in a caller buffer, loop bounds `slti 0x23` at 0x088A3BBC / 0x088A3C08 / 0x088A3C78),
the selectable-list builder FUN_088D5AC8 (capacity 35, caller buffers) and the save record table. A charId >= 35
would index past these, so `roster.cpp` skips such lines unless `PSPRECOMP_ROSTER_EXPERIMENTAL=1`. The menu overlay
also has 36 `slti reg,reg,0x23` sites, not audited (only 0x08A54194 is patched; the ones near the grid, 0x08A802D8 /
0x08A803E4 / 0x08A80474 / 0x08A8169C / 0x08A8170C, are the first to look at): 0x089F4E88 0x089FAC14 0x089FC410 0x089FC458 0x089FE908 0x08A05094 0x08A1BE20 0x08A4CABC 0x08A53FD4 0x08A54194 0x08A6A074 0x08A6A8DC 0x08A6AA14 0x08A6AD88 0x08A6BEE8 0x08A6C044 0x08A6C3FC 0x08A802D8 0x08A803E4 0x08A80474 0x08A8169C 0x08A8170C 0x08AB1C94 0x08AE4474 0x08AF35AC 0x08B16B64 0x08B17360 0x08B3294C 0x08B35E48 0x08B36214 0x08B386CC 0x08B38868 0x08B3A704 0x08B3AAB4 0x08B40954 0x08B40FD0.

### Quick Battle grid geometry (decoded, 16 Oct)

* The grid is a fixed 10 x 4 matrix, cell = row * 10 + col (0-based). Cell records live at
  `*(obj+0x28) + 0x678 + 8 * cell`: `{u16 flags (bit0 = active), u16 slot, u16 charId, u16 pad}`;
  d-pad navigation helper (0x08A7FC68) tests the active bit of cell `row * 10 + col`.
* Slot -> cell (FUN_08A7FB00..): `cell = slot + 2`, `+2` more when `slot >= 6`, `+2` more when `slot >= 26`. So slots 0..5 are row 1
  cols 3..8 (cells 2..7), 6..15 row 2, 16..25 row 3, 26..31 row 4 cols 3..8 (cells 32..37), slot 32/33 fall on cells
  38/39 (row 4 cols 9/10), which is where the random "?" tile goes when all 32 characters are listed (seen with
  `PSPRECOMP_ROSTER_UNLOCK_ALL=1`: row 4 shows 6 characters, Noctis at col 8 and "?" at col 9).
  The list is compacted: only unlocked characters take a slot, in list order.
* Each slot has a pre-existing tile sprite (`*(obj+0x28)+0x404`, handle `[(base + slot) * 4 + 8]`), whose frame is set to
  `lhu 0x36(*(obj+0x1c)) + record.f0`. The 34 slot sprites are the only anchors: the free cells 0, 1, 8, 9, 30, 31
  (row 1 cols 1,2,9,10 and row 4 cols 1,2) have NO sprite today. Placing an extra there needs a new sprite per cell
  (a new entry in the upper chara-select layout, `menu_pk_upper_chara_select_seq_0.bin`, SEQ files = 0x20-byte
  quad records `{u16 x0,y0,x1,y1; 8 zero bytes; 4 x RGBA}` after a 0x24 byte header, plus the creation tables at
  0x08A85EE8 / 0x08A86218) and a host reimplementation of the slot->cell function. Not done; extras currently
  take the next compacted slot (and only fit while fewer than 32 characters are listed, or at cells 38/39).

### Quick Battle grid, slot 34 (418ac66, extended 16 Oct)

Route: accessor hook + generated `[[patches]]` (see ROSTER_MAX) + overlay regeneration + list hook.

* `roster.cpp`: `grid_table_hook` replaces FUN_08A85ECC (12-byte record table, 33 rows at 0x08A864D0) with a copy at
  `DISSIDIA_GRID_DST` and appends one row per configured extra character (cloned from its `like`, charId replaced,
  tile/art/name/logo fields overridden from the `roster.txt` tokens). It is installed whether or not a roster exists
  because the overlay bounds were raised. `list_build_hook` wraps FUN_088D5AC8 (selectable list, capacity 35) and
  appends a clone of `like`'s entry per extra character (that clone is the unlock; no save record is needed).
  Debug env `PSPRECOMP_ROSTER_UNLOCK_ALL=1` lists charIds 1..32 as well (screenshot-tested).
* Data side (`tools/roster_mainbin.py`, writes `mods/by_crc/93307c1c.bin`, see below).
* Art side (`tools/character_art.py`, `tools/gim_encode.py`, writes `mods/by_crc/8f593261.bin` + text packs).

#### Art (done, 16 Oct)

* Slide-in (256x256): framed like Cloud's original (`menu_pk_free_battle_gim_1` entry 1): hair top at ~12% of the square,
  head ~27% tall, body cut at the waist at the bottom edge, head at ~62% from the left so the figure fills the right
  part. All numbers are fractions of the picture's bounding box (`frame_art`), `--art-crop` overrides.
* Face tile (32x64): hairline to chin, centred (`frame_tile`: crop of 14.5% of the figure height starting 1.2% below the top,
  unsharp + contrast for the 32 px width). Alpha: the GIM is I8 + RGBA8888 palette like the originals; `quantize` now
  reserves palette entry 255 as (255,255,255,0), the value the original tiles use for their background, and maps every
  texel with alpha < 10 to it (before, transparent texels shared an entry with dark opaque colours: the dark box).
  The tile keeps the template's silhouette (alpha is the minimum of the face alpha and the original tile's alpha) and its
  bottom marker block (3x3 gray texels at (1..3, 60..62), copied from the template).
* Name plate (128x32) and game logo plate (128x64, `--logo XV`: "FINAL FANTASY" + large numeral on the white ground like the
  FFIV/FFV plates; the picture sits in the top-left 89x62 block). The grid record's f2 selects the logo entity
  `0x706F + f2` (FFII = 1 ... XIII = 12; texture entries 77..91 of `menu_pk_free_battle_gim_2`), so a new logo is a
  new appended texture entry of `menu_pk_free_battle_gim_3` and `logo=<entity id - 0x706F>` on the roster line
  (149 for the first appended logo; `character_art.py` prints the whole line).

### main.bin: adding chrp / objentry records (`tools/roster_mainbin.py`)

`tools/roster_mainbin.py <disc0> <mods>` builds `mods/by_crc/93307c1c.bin` from the roster file (every line with charId >= 34) or from
`--char/--like/--code`. The archive is rebuilt, not patched: `--check` proves the layout rules with a byte identical round trip.

* MPK: `"MPK "` + 4 stamp bytes, u32 count, u32 0, then `count x {name_off, data_off, size, 0}`, the NUL separated names,
  zero padding to 16, entries on 16 byte boundaries (zero padding), end padded to 16.
* ARC (inside `system.bin`): `"ARC\x01"`, u32 count (43), 8 zero bytes, `count x {u32 1, tag[4], off (from the ARC start), size}`,
  entries contiguous with 4 byte alignment.
* `chrp` (0x90-byte records, u32 33 + u32 count header; sorted by charId = byte 0, found by bsearch): a record is replaced
  (charId 34, the ORG800 stub) or inserted in sorted position (charId > 34) with a clone of `like`'s, `+0x74` = `P_<CODE>`
  (8 bytes, so at most 6 characters of code).
* `objentry` (0x44-byte records sorted by their u16 id, count at +4): ORG800's two records (ids 1067, 1068) are rewritten
  in place as clones of `like`'s `_1P` and `_1P_A`, and clones of the other `P_<LIKECODE>_*` records (2P, 3P, ZAKO, W, A, PUB) are
  appended with ids above the maximum (1457..), names renamed to `P_<CODE>_*`, charId byte 6 = the new id.
  New ids >= 35 get all twelve.
* The first u32 of the chrp header (33) and of the objentry header (12) were left unchanged; their meaning is unknown.
* Other ARC sections (`chrl`, `plyp`, `mogp`, ...) may hold per-character tables too; none was needed for a Quick Battle
  with an alias character.

### `tools/new_character.py`: one command per roster slot (17 Oct)

```
new_character.py --work <psprecomp work dir> --mods <mods dir> --slot 35 --code viv100 --like eht110 --name "Vivi" \
    --series "FINAL FANTASY IX" --logo ff9_logo.png --portrait vivi_portrait.png --slidein vivi_render.png \
    [--model body.gmo --objx moves.objx] [--tile-crop x0,y0,x1,y1] [--art-crop ...] [--face-crop ...] [--full-name "..."]
new_character.py --work W --mods M --slot 35 --remove          # take the slot out again
add --dry-run to print the plan and write nothing
```

What it does (reusing `character_art`/`gim_encode`/`menu_pack`/`mess_text`/`package_extract`/`roster_mainbin` as libraries):

* Validates the slot against `runtime/roster_max.h`: 1..32 are retail (refused), 33 = retail SEV120 slot (see the caveat below), 34 = Noctis's slot,
  35..42 work only with `PSPRECOMP_ROSTER_EXPERIMENTAL=1` at run time (the tool says so). `--like` is a retail code (`eht110` = Laguna, not Shantotto) or charId;
  `--code` is up to 6 lower case characters (chrp holds `P_` + 6).
* roster.txt: the slot's line is added or replaced in place (other lines and comments stay); a different code in the slot needs `--force`.
* **Files of a slot** (28..30 for a like with the usual set): every file the engine requests for the code. Everything the tool does not draw is a byte copy of
  `like`'s retail file; the list is `package_paths.txt` entries containing the like code plus path templates (the arcade gims and face strips are missing from
  `package_paths.txt`), kept only if they exist in PACKAGE.BIN:
  * `obj/p_<c>_{1p,2p,3p,zako}.gmo` and `_a.gmo`, `_1p_w.gmo`, `obj/p_<c>.objx`, `_a.objx`, `_w.objx`, `obj/d_<c>.{gmo,objx}` (aliases; `--model` replaces
    the four body gmos, `--objx` replaces `p_<c>.objx`)
  * `voice/en/battle/voice_p_<c>_00.scd`, `voice_p_<c>_a_00.scd`, `voice/en/event/ffst/*_<c>_*.at3` (aliases, if the like has them)
  * `menu/jp/battle/chara_image/p_<c>_{1p,2p,3p}.gim` (in-battle HUD portrait), `menu/jp/talkevent/tim2/chara/{b,t}_talk_<c>.gim`,
    `menu/jp/character_select/chara2/p_<c>_1p.gim` and `menu/jp/arcademode/arcade_last_chara_tim2/arcade_last_<c>.gim` (arcade select/last, aliases)
  * generated from your pictures: `menu/en/common/chara_name/p_<c>_name.gim` (VS name plate), `menu/jp/story_mode/chara2/p_<c>_1p.gim` (VS face, 64x64),
    `menu/jp/common/chara_face_in_name{,_r}/p_<c>_face_in_name{,_r}.gim` (strips), `menu/en/battle/encount/encount_text_tim2/encount_<c>.gim` (256x64 name text)
  * shared files that hold every scaffolded slot: `by_crc/8f593261.bin` (menu_pk_free_battle_gim_3 with tile, slide-in, name plate and logo plate entries
    appended; the slot keeps its entries on re-runs, `--remove` empties them and trims the tail), `text/<lang>/mess_pk_chara_select_0.bin` (name record),
    `by_crc/93307c1c.bin` (main.bin chrp + objentry, rebuilt from the whole roster.txt by `roster_mainbin.build`, which now takes `min_id` and drops
    leftover objentry records of a taken-over retail id: slot 33 loses SEV120's 2P/3P/PUB records).
* Pictures: `--portrait` is a head-and-shoulders picture (grid tile = a cover crop filling the opaque box, VS face, face strips; `--tile-crop` /
  `--face-crop` are source-pixel rectangles), `--slidein` a full body render framed like Cloud's (head detection, `--art-crop`). Either can stand in for the
  other. `--logo` is logo artwork (89x62 block); without it `--series` makes a text plate ("FINAL FANTASY" + numeral, or the title alone).
* `<mods>/readme_newchars.json` (name starts with `readme`, so `package_mods` ignores it) records each slot's files, entries, text record and arguments. A re-run
  of the same slot updates in place (options you leave out are inherited) and is byte-idempotent; `--remove` deletes exactly those files and shared entries
  and the mods dir ends up empty (verified for slots 33/34/35, and for two slots at once).
* It prints the roster line, a summary and the placeholder checklist: body models, moves, voice banks, in-battle HUD portraits, arcade gims and the `d_`/`_a`/`_w`
  files stay `like`'s unless supplied (no tool yet for voice banks, HUD portraits or arcade pictures).

Caveats found while testing it (17 Oct, main build, private ms0, save with 16 characters unlocked):

* The slot is listed only when `like`'s charId is in the player's selectable list (`list_build_hook` clones that entry as the unlock). `--like eht110`
  (Laguna) is locked in an early save, so nothing appears: test with `PSPRECOMP_ROSTER_UNLOCK_ALL=1`, or pick an unlocked like (Bartz).
* Slot 33 is not an "extra" for `roster.cpp` (`kFirstNew = 34`): the runtime only overrides its code string and adds no grid row/list entry, so a slot-33
  scaffold shows nothing in the grid (screens `slot33_grid_no_extra.png`). Making 33 work means lowering `kFirstNew` and auditing the id-minus-`kFirstNew`
  indexing (save record copies, entity ids at `roster.cpp` 397/491). The tool builds the data side for 33 (chrp + objentry replaced) already.
* Screens of the end-to-end runs: `psprecomp_work/shots_newchar/` (`slot34_{grid,play_as,setup,vs,battle}.png` like Laguna with `PSPRECOMP_ROSTER_UNLOCK_ALL=1`;
  `slot35_{grid_small,setup,vs,battle}.png` like Bartz with `PSPRECOMP_ROSTER_EXPERIMENTAL=1`): grid tile, name, logo and slide-in, the setup panel, the VS screen
  (face and name) and the battle all work for the scaffolded slot (the model is the like's, `LOOKUP_MISS 0xDEADBEEF` appears once in the log, not looked into).

### Package index capacity: the ~210 new-entry ceiling is gone (17 Oct)

A scaffolded slot adds about 30 new package entries (`tools/new_character.py`: 28 to 31 files, of which a few are
overrides of shared files), and the game's in-RAM copy of `PACKAGE_INFO.BIN` held only 210 more than retail, so six
slots was the end. The index is now relocatable (`runtime/hooks_index.cpp`).

* Game side. The index is read by `FUN_08871938` into a fixed BSS buffer, `0x09B26BC0`, size `0x20F8C` (the next global is at
  `0x09B47B50`). Only four functions reference that address (a `lui 0x09B2` / `addiu 0x6BC0` pair in each; found by
  scanning every EBOOT and overlay batch plus the data of all four images; there are no pointers to it): the loader
  `FUN_08871938`, the entry count `FUN_08871674` (`*(u32*)(base + 8)`), `FUN_08871684(idx)` (`&entry[idx]` or 0) and the by-name
  lookup `FUN_088713E4(obj, path)` (CRC via `FUN_08871288`, a binary search whose `hi` starts at `count`, then a one-entry cache
  in `obj+8` / `obj+88`). Everyone else gets entries as pointers from the last two, so nothing else needs to know the base. The
  count is a u32 in the file header: no widening needed.
* Hooks. All four are `psp_dispatch_register`ed. The loader hook runs first: it builds the patched index (`hooks_io.cpp`, once),
  and when it exceeds `kIndexBufferBytes` it takes `kIndexExtendedBytes` = `0x26400` (13,054 entries = 11,043 retail + 2,000 new + header, rounded to
  256) from the host heap with `psp_kmem_bump_alloc_named(.., "PACKAGE_INDEX")`, zero-fills it (the lookup may read `entry[count]`), and reads
  the index there (same two vtable calls as the original, with another destination and size). The other three then use the new base. Until
  then, and whenever the index fits (retail, empty mods dir, a few overrides), every hook calls the original function: retail
  is unchanged by construction. The boot log shows `[MODS] package index capacity: retail buffer 135052 B (0x09B26BC0), extended 156672 B = 13054 entries ...` and,
  when used, `relocated to host heap 0x...` plus a `PACKAGE_INDEX` line in the `[MEM]` layout (153 KiB, at the start of region 1 in the test run).
* New limit: 2,000 new entries (about 60 scaffolded slots by file count; `kIndexMaxNewEntries` in `package_mods.h`, the host block is 153 KiB
  so raising it is cheap). The `textures/` tree is never indexed, so a 2,000-file texture dump costs nothing. The roster itself is still
  capped by `kRosterMax` (slots up to 42).
* **Still limiting: the Quick Battle select list holds 34 entries in total** (`[ROSTER] selectable list is full (34 entries)`; the EBOOT list builder's
  buffers are 35 wide and `FUN_088D5AC8` cannot be relocated, see "NOT relocatable"). Unlocked retail characters and extras share it, so
  the number of extras that show up is `34 - unlocked retail`: eight extras need a save with at most 26 characters unlocked (the test save has 21); with
  `PSPRECOMP_ROSTER_UNLOCK_ALL=1` (32 retail) only two of them are listed. Extras that do not fit are skipped in list order with that log line.
* Second limit met on the way: with eight scaffolded slots the menu pack (`by_crc/8f593261.bin`: 725,056 B grows to 1,406,272 B) and the main.bin
  (`by_crc/93307c1c.bin`, 1,218,448 to 1,226,064 B) are much bigger than retail, and the battle load then fails in the game's main 12.9 MB heap
  (`[MODELS] heap 0x08C15E80 alloc(1048576) FAILED`, `alloc(2506752) FAILED`, "Now Loading" for good). `PSPRECOMP_MODEL_BUDGET=2600000` (any value above
  `0x264000`) moves the 0x275800 file heap and the 0x780000 model pool to the host heap and the battle loads. Not automated yet.
* Test (17 Oct, build_sas, private ms0, `PSPRECOMP_MODS` = eight scaffolds + 2,000 empty `textures/*.png`): slots 35..42 (`tst100`..`tst800`, likes
  fiv100 one100 thr100 for100 six100 sev100 nin100 ten100, placeholder gradient PNGs as portrait and slide-in) give `256 override(s) active`, 253 of them NEW
  (the old build kept about 210 and dropped the rest); `package index: patched index 135568 B ... relocated to host heap 0x09B90000`. The third grid row shows all eight tiles
  plus `?`, with the generated names, logos and slide-in art in Player Select; slot 40 (as player) against slot 37 reaches a running battle (like models), and a
  second run with slot 42 got as far as the setup panel. Empty mods: `0 override(s)`, buffer not relocated. Retail through the relocated lookup
  (`PSPRECOMP_INDEX_RELOCATE=1`, empty mods): boot, menus, Quick Battle grid and a Lightning mirror battle all work. Host test: `tests/run_package_mods_test.sh` (400 new entries: first 210 kept
  under the retail capacity, all kept under the extended one; 2,000 textures ignored).
### In-battle HUD portraits (`chara_image`), 17 Oct

Layout of `menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim` (decoded from Bartz and Laguna, 17696 bytes each, I8 + RGBA8888 palette, the same
container as the other menu gims, so `gim_encode.decode/encode` apply unchanged):

* 128x128, but the picture is a small **free-form cut-out**, not a framed square: the star frame is drawn by the HUD itself underneath, the texture has
  no background and no frame. Everything outside the cut-out is alpha 0 on palette entry 255 (255,255,255,0).
* Box of the cut-out: x 41..85, y 50..101 (any-alpha bbox 40..84 x 48..101 over the six files). Head and shoulders, the hair top at y 52..53, the head
  about 55-65% of the height, the rest chest. Hair may spill left of the box and up to y 48 (Bartz 3p, Laguna 2p), the right edge is 83..84 in every file.
* Mask: the upper half is the figure's own outline (alpha of the art, 1 px soft edge); **below y ~ 80 the picture is clipped by an ellipse**, centre
  (66.5, 79.6), half axes 18 x 21.4, last row y 100 (rows 88..100 measured: right edge 83, 83, 82, 82, 81, 81, 80, 79, 78, 77, 75, 73, 70, left edge
  mirrors it about x 66.5). The clip has anti-aliased edge texels (alpha 1..254), which is why the star ring is not cut by a hard staircase.
* 1p / 2p / 3p share the box and the ellipse; they differ only in the art (2p = alternative palette of the same costume, 3p = second costume), and the
  3p silhouette differs slightly at the top left. The HUD draws the texture 1:1 (a 42x48 face in a ~50 px star) and does not mirror it for the CPU side
  (Lightning's cut-out looks left on the right-hand frame too).
* `b_talk_<code>.gim` / `t_talk_<code>.gim` do **not** share it: they are 128x128 pictures with the ornate silver frame and a dark star panel baked in
  (opaque box 2..121 x 1..120, 5..126 x 8..126). Used by story talk events only, not by Quick Battle; left as aliases of `like` (no compositor).

Compositor (`gim_encode.frame_hud`, `prepare(..., "hud"|"hud_figure")`): scales a crop of the source into the box (41,52)-(85,101), keeps the picture's alpha,
multiplies it with the anti-aliased clip (lower ellipse only, so a cut-out keeps its own outline above; a crop with no transparency, e.g. a portrait with a
painted background, gets the whole oval as a cameo), light unsharp + contrast for the small size. The default crop is `cover_crop` of the portrait (top
aligned) or, from a full body render, hair top to 27% of the figure height centred on the head (same head detection as the tile). The same picture is written
for 1p, 2p and 3p.

* Facing: the HUD never mirrors a portrait. The same texture is drawn on the P1 frame (bottom left) and on the CPU frame (bottom right), and retail
  portraits face either way (Lightning looks left, Laguna right). A figure looking right therefore looks towards the arena as P1 and outward as the CPU;
  nothing in the game flips it. `--hud-flip` mirrors the source before cutting (`--hud-crop` stays in the unmirrored picture's pixels; `--no-hud-flip`
  undoes it). Vivi (slot 33) and Y'shtola (slot 35) use it so that they face right as P1.
* `new_character.py ... --hud-crop x0,y0,x1,y1` (source pixels) and `--hud-from portrait|slidein` (default portrait; a transparent render gives the retail
  style cut-out, which is what Vivi uses because his portrait has a painted gradient). Recorded in `readme_newchars.json`, so a re-run inherits them.
* `character_art.py ... --code <c> [--hud-crop ...] [--hud-figure 1]` writes the three files too (template `fiv100`).
* Pictures on a plain white ground (an opaque render with white corners, e.g. `chara/yshtola_render_v2.png`) get an alpha channel on load
  (`gim_encode.knockout_white`: border-connected near-white removed, enclosed pure-white gaps of >= 600 px too, edge eroded 1 px and feathered).
* Test data (`psprecomp_work/mods_chars`, 18 Oct): Vivi `--hud-from slidein --hud-crop 190,34,520,404 --hud-flip`, Y'shtola from `yshtola_render_v2.png` as portrait and slide-in (`--tile-crop 360,70,620,330 --face-crop 300,110,700,210 --hud-crop 385,75,600,305 --hud-flip`), Noctis default
  (render, auto head). Screens: `psprecomp_work/shots_models/hud/` (HUD as P1 and as the CPU, Quick Battle).

### What each extra id needs (checklist)

1. A `roster.txt` line `<id> <code> [like] [tile=N art=N artl=N artr=N name=N text=N logo=N nm=Name]` (`nm` = display name with `_` for spaces, used for the Opponent Info text; code <= 7 chars; `like` = the retail charId cloned for
   grid row, list entry and voice bank).
   `artl` / `artr`: portrait entry for the player's (left) / the opponent's (right) side of the battle setup panel, which
   draws one texture on both sides and never mirrors it; `new_character.py` makes a mirrored copy for the side the slide-in
   would look away on (`--art-faces`, default left), so the two portraits face each other.
2. `tools/roster_mainbin.py` (chrp + objentry records, above).
(`tools/new_character.py` does all of 1-4 in one command, see above.)
3. Loose files under `<mods>/` named after the code: `obj/p_<code>_{1p,2p,3p,zako}[_a|_w].gmo`, `obj/p_<code>[_a|_w].objx`,
   `obj/d_<code>.{gmo,objx}`, `menu/jp/battle/chara_image/p_<code>_{1p,2p,3p}.gim`, `menu/en/battle/encount/.../encount_<code>.gim`,
   `menu/jp/common/chara_face_in_name/p_<code>_face_in_name.gim`, `menu/jp/talkevent/tim2/chara/{b,t}_talk_<code>.gim`,
   `voice/en/battle/voice_p_<code>[_a]_00.scd` (`tools/package_extract.py alias <like path> <code path> <mods>` makes alias copies).
4. Menu art: `tools/character_art.py <disc0> <mods> <image> <Name> [--logo XV]` (tile, slide-in, plate, logo, name text) and the
   line it prints.
5. EBOOT side for ids >= 35 (not done): voice slot array, list buffers, `slti 0x23` sites (see ROSTER_MAX).

Verified with `PSPRECOMP_PKG_TRACE=1` on a battle with slot 34 and chrp code `P_NOC100`: the package read of the new
entry `obj/p_noc100_1p.gmo` (sector 506280, 2102308 bytes, virtual sector past the real package) is served from
`<mods>/obj/p_noc100_1p.gmo`, the encount gim and the chara_image gim likewise; no `p_fiv100*` entry is read at all. The game did not request
`p_noc100.objx`, `voice_p_noc100_00.scd` or `p_noc100_1p_a.gmo` for the player in that battle (the voice slot reuses
`like`'s bank by design, the other two were not requested; animation still works), so those alias copies are unused so far.

### What breaks first when charId > 34

1. chrp is bsearched by byte 0 (`lbu`) and objentry stores charId in byte 6: ids stay below 256. The archive is now
   rebuilt by `roster_mainbin.py`, so records can be added (done; ids > 34 only built, not run).
2. EBOOT table 0x089D7BE0 (20-byte entries, -1 terminated) has no free room; voice table and voice slot array are 35 slots.
3. Selectable list builder capacity 0x23 (35) and `slti 0x23` charId bounds in the menu overlay (36 sites, unaudited).
4. Grid: 34 tile sprites in the layout asset (no sprite for the free corner cells), 33-row record table (now 41),
   per-character 35-sized arrays elsewhere.

### What a real Noctis needs

chrp record (voice/object code 'P_NOC100'), objentry records (1P and 1P_A, charId set), obj/p_noc100_*,
voice/* for the code, menu portrait/logo gims (p_noc100_1p.gim and the small arcade-select gim), text
ids for the name and FF logo, and (separately) the per-charId tables listed in the consumer map.

### Run script

The scripts are in the shared scratchpad `mdl/` (not in repo): start.sh, goqb.sh (title -> Load game ->
Story -> back -> Battle Mode -> Quick Battle 1-on-1 -> grid, then DOWN x n), qb34.sh.

### Model size budget (large replacement models)

Symptom: a replacement `p_<code>_1p.gmo` of 3.45 MB (retail Bartz: 2.10 MB) crashed the battle load (white screen,
`[LOOKUP_MISS] addr=0x14040506`, GE display lists running away). Cause, found by tracing the load
(`PSPRECOMP_PKG_TRACE=1` now logs the guest destination of each PACKAGE.BIN read):

* the loader reads every model file into a temporary staging block taken from the "FILE" heap: a 0x275800 block
  (staging + 0x11800) carved out of the main BSS heap by `FUN_0888204C` and made a heap by `FUN_0885C40C`
  (`DAT_089D4960` is the model manager, `FUN_0884417C(mgr, id, heap, size, 0)` registers a slot and
  `FUN_08843014` allocates `size` bytes top-down, reads the file there and frees it);
* the slot size is the constant `0x264000` = 2,506,752 B (`lui 0x26 / addiu 0x4000` in `overlay_battle`
  `FUN_08B45310`, ids 0x54E, 0x134, 0x408, 0x548). It is per file, not per character: the files are staged one
  after the other (gmo, objx, `_a`, `_w`, ...), so two modded fighters do not add up;
* the read size comes from the (patched) index, so a bigger file is read past the end of the block: the
  allocation returns null (read to guest address 0) or the file overruns the heap behind it;
* the largest retail character GMO is `p_thr200_1p.gmo` at 2,280,840 B, so retail never needed more. The package
  mod layer (index patch, relocated virtual sectors) was correct: a zero-padded retail Bartz GMO of 3,451,552 B
  loads and plays once the budget is raised.

`hooks_models.cpp` raises it without touching the recompiled code (EBOOT/overlay immediates cannot be patched at
runtime): `FUN_0888204C(0x275800)` is served from the host heap (`psp_kmem_bump_alloc`, once, reused by every
battle) with size `0x275800 + (budget - 0x264000) + 0x20000`, `FUN_0885C40C` gets that size, `FUN_0884417C` swaps
`0x264000` for the budget and `FUN_0888208C` keeps the host block. Budget = `PSPRECOMP_MODEL_BUDGET` (bytes) or,
automatically, the largest `obj/*` override size rounded up to 4 KiB; ceiling 18 MiB (GMOs up to 16 MiB; see "Host
heap layout and the 16 MiB ceiling" below for what actually fits). With retail files nothing changes (the hooks pass
through). The `.objx` is a separate, much smaller file (retail 1.56 MB) and obeys the same limit.

#### Battle model pool (17 Oct regression: both fighters invisible, Ifrit drawn in place of both)

With the Noctis files (`p_fiv100_1p.gmo` 3.72 MB) Quick Battle against Vaan (not Lightning, not Cloud) drew neither
fighter from the first frame. It is not the effect allocator (`sceKernelAllocMemoryBlock("EFFECT_ALLOCATOR")`,
0x0A010000-0x0A3ED000, which ends exactly where the raised model heap used to start): a canary-filled 4 MB gap
between them stayed untouched, retail never writes behind the block, and moving the model heap changed nothing.
Bisecting the mod (objx only: fine; retail GMO padded to 3.45 MB: fine; GMO only: broken) pointed at the GMO content,
and a diagnostic on the game's heap allocator (FUN_0885C6CC, `[MODELS] heap ... FAILED`) showed the cause:

* `FUN_0888204C(0x780000)` at game start returns the **battle model pool** (heap object 0x08C15F80, used bytes at
  heap+8): the built model data of both fighters. Retail models fill it to ~0x77Fxxx of 0x780000; Noctis + Vaan do not
  fit, so ~13 allocations (1 KB - 74 KB) return null and the model objects are never built (no skinned PRIMs, no
  GE_XFER into RAM). The Hellfire summon then loads Ifrit through the same null/stale slots.
* `hooks_models.cpp` now serves that request from the host heap with `kPoolExtra` (default 0x80000, env
  `PSPRECOMP_MODEL_POOL_EXTRA`) more bytes whenever the model budget is raised. The pool is allocated before the package
  index is opened, so the budget is taken from a prescan of `<mods>/obj/`. With Noctis + Vaan the pool ends at
  `used` 0x786DF0 of the pool. `[MODELS] heap 0x... alloc(n) FAILED (heap end, used)` is logged for every null
  allocation of the game's allocator, so an exhausted heap is visible next time. (`heap 0x04110000 alloc(1419280)
  FAILED` is a VRAM request seen with the Noctis files; it does not stop either fighter being drawn.)

#### Host heap layout and the 16 MiB ceiling (raised from ~3.7 MB)

The host heap serves `sceKernelAllocMemoryBlock` / partition allocations (bump allocator, `psp_hle_kernel_memory.cpp`).
Its limits are now per-game config: `game.toml` `[runtime] host_heap_start`, `host_heap_end`, `host_heap2_base`,
`host_heap2_end` become `RECOMP_HOST_HEAP_START` / `RECOMP_HOST_HEAP_END` / `RECOMP_HOST_HEAP2_BASE` /
`RECOMP_HOST_HEAP2_END` in the generated `recomp_game_config.h`. Defaults (no keys) are the old behaviour: one
region ending at 0x0B000000, no second region, heap start aligned up to 16 MB.

Dissidia:

| Region | Range | Size | Holds |
|---|---|---|---|
| 1 | `0x09B80000-0x0B800000` | 28.5 MiB | UserSbrk (64 KB), model pool, model file heap, effect allocator (first fit) |
| (PRX) | `0x0B800000-0x0B810000` | 64 KB | LIBSUPPREACC (`SUPACC_BASE`), not heap |
| 2 | `0x0B810000-0x0BE00000` | 5.9 MiB | overflow: used when a request does not fit what is left of region 1 |
| stacks | `0x0BE00000-0x0BFFF000` | 2 MiB reserve | guest thread stacks grow down from `0x0BFFF000` (Dissidia uses ~130 KB) |

* `host_heap_start` is set to the analysis heap base `0x09B80000` (end of the loaded image) instead of the aligned-up
  `0x0A000000`: the 4.5 MiB in between was unused, and a 16 MiB model needs one contiguous block. This moves UserSbrk
  from `0x0A000000` to `0x09B80000`; nothing in the runtime or the Dissidia hooks refers to the old address.
* Every block is a bump allocation, so the pool, the file heap and the effect allocator cannot overlap. `psp_kmem_log_layout`
  prints the final layout as `[MEM]` lines (regions, each block with address and size, free per region, stack top) at
  boot and again when the model budget is fixed, and flags `OVERLAP` should one ever occur. `psp_alloc_stack` warns if a
  stack would reach the host heap.
* Addresses above `0x0C000000` are deliberately NOT used: with the 0x07FFFFFF mask `0x0C000000` aliases VRAM
  (`0x04000000`), and the HLE's user-RAM checks (`psp_hle_utility.cpp`, `psp_hle_mpeg.cpp`) reject them.
* The pool (`FUN_0888204C(0x780000)`) and the file heap are reserved together when the pool is requested at game start
  (budget from the prescan of `<mods>/obj/`), after `psp_kmem_plan_fits` simulated pool + file heap + the later effect
  allocator (4,050,944 B + 1 MiB margin) against the real free space. A budget that cannot fit is lowered 1 MiB at a time
  (`[MODELS] host heap too small ... lowered to`) instead of failing an allocation; once reserved, later budget requests
  cannot grow it (`... exceeds the reserved budget ... (ignored)`).
* Pool extra (22 Oct): `0x280000` up to a ~4.25 MB model (staging `0x440000`; Noctis 4.21 MB with the EX Burst), plus all
  of the staging growth beyond that; `PSPRECOMP_MODEL_POOL_EXTRA` forces a fixed value. (Was 0x180000 + 1/4 of the growth
  beyond `0x390000`, which needed the env override for the 4.21 MB model.) Older note: The 1/4 factor is an estimate (Noctis needed only
  ~28 KB over retail); watch for `[MODELS] heap ... FAILED` with very large models and raise the extra if it appears.
* Ceiling: `kMaxStaging` = 0x1200000 (18 MiB). A 16 MiB GMO needs ~16.9 MB file heap + ~11.7 MB pool (8.4 MB at minimum).


### Status 17 Oct (VS screen, corners, ids > 34)

Verified in game (Quick Battle, build_models, private save): grid shows Noctis with the FFXV logo plate, the Enter Battle panel
shows Noctis (portrait + name plate via the setup binder hooks), and the battle starts. The VS screen (the one before battle) had
an empty left box with no name for slot 34 at that point; the cause and fix are in "VS screen: solved" below.

What was found and done for the VS screen:

* The per-character save record list (31 ids at 0x089CBC98, records of 0x778 bytes, FUN_088CB3B4 = id -> index, FUN_088CB164 =
  record) gates the party setters in the menu overlay (0x08A48DB0 / 0x08A491E0): a charId without a record never gets a team
  struct. Before: team struct byte 0 (charId) of side 0 stayed 0xFF, so the VS builder (battle overlay FUN_08AF6B70 and its
  record builder at 0x08AF8870) skipped the side. `roster.cpp` now hooks FUN_088CB3B4 (extra id -> record index of `like`) and
  FUN_088CB164 (private 0x778-byte copy of `like`'s record at DISSIDIA_SAVEREC with its own charId byte). With only the first
  hook the VS screen showed Bartz (record charId 9); with both, team byte 0 is 34 (checked by memory dump 0x09AFB8A0).
* (Superseded, see "VS screen: solved".) The VS builder texture loads (face strip `p_<code>_face_in_name[_r].gim` and the `encount_<code>.gim` name text) are still not
  requested for side 0: a trace of FUN_088D0810 shows only the CPU paths. Open: find why the side 0 branch of FUN_08AF6B70 does
  not issue the load although its record has charId 34 (record `34 1 0 1 -1 0 3 0`, same shape as Lightning's). The relocated
  table (0x08700640) and the patched `lui`/`addiu` at 0x08AF6D00/0x08AF6D18 were verified in the generated code and in memory.
* `tools/roster_patches.py` addresses were off by 21 instructions (file offset without the 0x54 header); fixed and `--verify <DATA dir>`
  added. Face-in strips (`character_art.py --code`) and a table for FUN_08B558F0 were added.
* Debug aids (env only): `PSPRECOMP_ROSTER_DEBUG=1`, `PSPRECOMP_ROSTER_TRACE=<hex,...>` (arguments + 0x60 bytes behind pointers of
  overlay/EBOOT functions; do not trace FUN_088CB164/FUN_088CB3B4, it crashed), `PSPRECOMP_ROSTER_WATCHCPY`.

Corner cells (cells 0, 1, 8, 9, 30, 31): not done. The grid UI creates 98 entity sprites from the table at 0x08A85EE8
(`slti 0x62` at 0x08A82060 / 0x08A8212C), handles stored at `*(obj+0x28)+0x408+4*i`, each slot needs two sprites (tile and backdrop)
and two quads of the layout SEQ. The handle array is followed by other fields at +0x590, so it cannot grow in place: it needs
relocation (all `0x408` users) plus new SEQ quads. Estimated as its own task.

Ids > 34: the EBOOT arrays are handled by hooks instead of patches where the code only indexes by charId: FUN_088CB3B4/164 (save
records, above) and FUN_088A3C98 (voice slot accessor, id -> `like`'s slot). The `PSPRECOMP_ROSTER_EXPERIMENTAL` gate stays because slot 35
was not run (a second roster line plus a mods copy with noc101 aliases is prepared in the scratchpad `mods_models35`). Remaining
limits: the selectable list buffer holds 35 entries (at most 34 listed), the 36 `slti 0x23` sites in the menu overlay are unaudited.

Harness notes: another agent regenerates `psprecomp_work/output` (recomp.h mtime), which makes `build_models` want a full rebuild;
touch the `.o` files first, then only roster.cpp rebuilds (6 s). Fixed sleeps in the button scripts break under load: use screen-diff polling.

### VS screen: solved, 17 Oct (second pass)

Cause: not a table or a hook, a missing asset. The Quick Battle VS screen is built by battle overlay FUN_08AF6B70, which for each of
the two sides issues (through FUN_088D0810, always, there is no skip on the charId) these loads, the code string coming from the
patched name table (0x08AFBC68 -> relocated):

| Request (side's code `<c>`) | Handle slot | Content |
|---|---|---|
| `menu/JP/common/chara_name/p_<c>_name.gim` | obj+4+4*side (a2 0x1A04) | 128x32 name text ("Lightning") |
| `menu/JP/story_mode/chara2/p_<c>_1p.gim` | obj+0xC+4*side (a2 0x1A08) | 64x64 opaque face tile |
| `menu/JP/common/chara_face_in_name_r/p_<c>_face_in_name_r.gim` | obj+0x18+4*side (a2 0x1A02), only for the assist character (record halfword +8 >= 0) | 128x32 assist strip |
| `menu/JP/battle/encount/encount_text_tim2/encount_<c>.gim` | later (a2 0x226), logged for the opposing side only | encounter text |

The earlier agent looked at the wrong strings: `p_<c>_face_in_name.gim` (what `character_art.py --code` produced, via FUN_08B558F0) is
the battle-prep/story strip and is not used here. The loader did request the Noctis files; they simply did not exist in the mods dir (a
missing mods file falls through to PACKAGE.BIN, which has no `p_noc100_*`), so the box stayed empty. A one-line-per-request log
(env `PSPRECOMP_ROSTER_LOADLOG=1`, logs the path string of every FUN_088D0810 call, does not stall) showed exactly that.
Language folder: the game spells it `JP` in the string, the package keeps the name text only under `menu/en/...` (EU disc) and the face
tile under `menu/jp/story_mode/chara2/`; the loose-file layer maps it the same way as the other gims. Fix: `character_art.py --code <c>`
now also writes `menu/<lang>/common/chara_name/p_<c>_name.gim` (name plate rendered like the grid plate, template `fiv100`) and
`menu/jp/story_mode/chara2/p_<c>_1p.gim` (face crop; `--vs-only 1` writes just these two for an existing roster entry, `--tile-crop`
adjusts the face). No runtime change is needed. Screenshots: `psprecomp_work/shots_models/vs/slot34_*.png`.

### Slot 35 (first id > 34), 17 Oct

Run with `PSPRECOMP_ROSTER_EXPERIMENTAL=1`, mods copy with `35 noc101` (aliases of the noc100 assets, incl. the two VS gims), private
save, `build_models` against a regenerated output (shared output copied at HEAD 4477fb6, overlays regenerated with the models recompiler):

* Quick Battle grid: both extra cells appear (34 at the third cell of row 3, 35 next to it, `?` after), cursor, face, name plate and
  logo of slot 35 correct.
* Player Select as P1, CPU select, setup screen (portrait, name plate), VS screen (left tile with face and name), Opponent Info, and a
  running battle all work with slot 35 as player. As CPU opponent (P1 Lightning) the VS right tile shows the Noctis face and name and the
  battle starts and runs (screens `slot35_*.png`).
* Nothing broke up to there: no LOOKUP_MISS, no crash, no missing text in the flow. The hooks already cover the id-indexed EBOOT arrays
  (save records FUN_088CB3B4/164, voice slot FUN_088A3C98, char info FUN_088B3B20) and the relocated overlay tables have 43 slots.
* Former gaps, both closed: the Opponent Info name (see "Opponent Info name, solved") and the in-battle HUD portrait (see "In-battle HUD portraits").
* Not exercised, so the gate stays: Arcade/Story/Practice entry points, the result and unlock screens, the selectable-list buffer limit of
  35 entries, the 36 `slti 0x23` sites of the menu overlay, and ids 36..42 (more extras).

### Third pass, 17 Oct (slot 33, ids > 34 default-on, slti audit, corner plan)

Status of what was run versus only read. **The `slti 0x23` audit below is static only** (disassembly of the menu overlay); the
Arcade/Story/Practice/Customization/result screens were NOT exercised with an id above 34. Quick Battle through battle with slot 35
was exercised in the previous pass (see "Slot 35").

#### Changes
* `kFirstNew` is now 33 (`DISSIDIA_FIRST_NEW` in `roster_max.h`): a `33 <code>` roster line (the unused SEV120 slot) is an extra like 34/35
  (grid row, list entry, voice alias, save record, charinfo). The grid record table has 33 + (ROSTER_MAX - 33) = 43 rows and moved to
  `DISSIDIA_GRID_DST` 0x08702000 (43 rows overran the table at 0x08700400). `roster_patches.py` derives the grid bounds from it.
* The selectable-list hook clones `like`'s entry; when `like` is not in the player's list (locked on the save) it clones the first entry,
  so a new character always appears. It also never adds a charId twice.
* `PSPRECOMP_ROSTER_EXPERIMENTAL` is gone. ids >= 35 are default-on (a roster.txt line is already an opt-in);
  `PSPRECOMP_ROSTER_RETAIL_ONLY=1` skips every id >= 35 and restores the old behaviour.
* Debug aids kept: `PSPRECOMP_ROSTER_TRACE_RING=1` (caller ring in `PSPRECOMP_ROSTER_TRACE` output) and
  `PSPRECOMP_ROSTER_TRACE_GATE=1` (trace only while the guest byte at 0x08701F00 is non-zero: `W 0x08701F00 01` on the debug socket).

#### `slti 0x23` audit (menu overlay, 36 sites, STATIC ONLY)
Correction: the addresses listed earlier in this file are **0x54 too low** (they came from file offsets). The real instruction addresses:
089F4EDC 089FAC68 089FC464 089FC4AC 089FE95C 08A050E8 08A1BE74 08A4CB10 08A54028 08A541E8 08A6A0C8 08A6A930 08A6AA68 08A6ADDC 08A6BF3C
08A6C098 08A6C450 08A8032C 08A80438 08A804C8 08A816F0 08A81760 08AB1CE8 08AE44C8 08AF3600 08B16BB8 08B173B4 08B329A0 08B35E9C 08B36268
08B38720 08B388BC 08B3A758 08B3AB08 08B409A8 08B41024. (Also `sltiu 0x22` / `slti 0x22,0x24` sites exist elsewhere; not audited.)

* **(a) harmless, left alone** - loops over fixed 35-element arrays or 35-bit sets; widening them would overrun the neighbouring fields,
  and an id >= 35 simply is not visited: 089F4EDC 089FAC68 089FC464 089FC4AC 089FE95C 08A050E8 08AE44C8 08AF3600 08B16BB8 08B173B4 08B329A0.
* **(b) patched to ROSTER_MAX (43)** - `slti` charId validity checks in front of code that only calls the hooked save-record accessors
  (FUN_088CB164 / FUN_088CB3B4) or stores/compares the id; an id >= 35 used to fall to the "invalid/empty" branch. Generated by
  `roster_patches.py` (`CHARID_BOUND_SITES`, immediate only, verified against the ELF): 08A1BE74 (settings validation, reset id to 1)
  08A4CB10 08A6A0C8 08A6A930 08A6AA68 08A6ADDC (party panel) 08A8032C 08A80438 08A804C8 08A816F0 08A81760 08AB1CE8 08B36268 08B38720
  08B388BC 08B3A758 08B3AB08 08B409A8 08B41024. Not exercised in game.
* **(c) open, NOT patched** - the guarded code indexes a per-charId table that is not relocated, so widening the bound would read past it:
  08A54028 (info panel; copies a 36-word table from 0x08A54638 to the stack and indexes it by charId: an id >= 35 shows no portrait),
  08A6BF3C / 08A6C098 / 08A6C450 (party panel portraits: 6-byte-per-charId table at about 0x08A6CAD0). Fix = relocate those tables the way
  `roster_patches.py` relocates the name table, then widen. Until then an extra shows no portrait in those two panels.
* **unclassified**: 08B35E9C (compares a value returned by FUN_08912AE8, probably not a charId).
* Open: list capacity. The Quick Battle list buffer is a field of the screen struct (`this->0x14C + 0x974`, capacity `ori a1,0x23` at
  0x08A87AA0, followed by other fields at +0xA8C), so at most 34 characters are listed (32 retail + 2 extras when everything is unlocked).
  More extras are not listed (`[ROSTER] selectable list is full`). Raising it needs the fields behind the buffer moved.

#### Opponent Info name, solved (18 Oct)
The "Target" name of Opponent Info is made by the battle overlay (FUN_08AF8F58, and FUN_08AF9C6C for a second layout), not by the menu
panel that reads 0x08A544E0. After building "Lv%d" with sprintf (FUN_089347E8) it reads `*(0x08AFB6E8 + charId*8 + alt*4)` (two u32 text ids per
charId; 35 entries, no bound check, the alt column is chosen by a flag of the record) and, unless that is -1, creates the text with
`FUN_0887B3EC(obj, 1, 0, id, 0, 0)`, kind 1 = a record of the battle text bank (ids 0..0x3E = names and alternative names). For charId >= 35 the table
read ran into the string data behind it, and an extra has no record in that bank anyway, so the name stayed empty.

Found by diffing two gated traces (`PSPRECOMP_ROSTER_TRACE=0887B3EC,089347E8`, gate on from the VS screen to Opponent Info) of a retail CPU (Cecil)
and an extra CPU (Noctis): the only structural difference was one missing `0887B3EC kind 1 id 6` call straight after the "Lv%d" sprintf.
(The BGM title, id 0x6F vs 0x67/0x74, and the stat numbers differ between any two runs and are not the name.)

Fix (all in this module): the table is relocated to `DISSIDIA_TBL_OPPINFO` 0x08701A00 (43 x 8 bytes, retail rows copied when the overlay is read, extras
-1 unless labelled; `roster_patches.py` patches both references) and an extra with a `nm=<Name>` token in its roster.txt line gets the marker id
0x7F00 + charId. `oppinfo_name_hook` wraps FUN_0887B3EC: for kind 1 and a marker id it writes the label as UTF-16 into `DISSIDIA_OPPINFO_STR`
and calls FUN_0887D868 with kind 0 and that buffer, exactly the way the "Lv%d" text is made, so size, colour and position come from the layout. `_` in
the label is a space. `new_character.py` writes `nm=<--name>` itself. An extra without `nm=` stays blank (id -1).
Verified: Lightning as P1, Noctis as the CPU shows "Noctis" after "Target" (`shots_models/hud/opp_info_noctis_after.png`, before:
`opp_info_noctis_blank_before.png`). The overlay sources must be regenerated for the two new patch pairs (`build_overlay.py 1 ovl_battle ...`).

Debug aids added: `PSPRECOMP_ROSTER_TRACE_LEN=<hex words>` (words dumped behind each pointer argument, default 0x18) and `PSPRECOMP_ROSTER_TRACE_POST=1`
(after the call: v0, v1 and the words behind a0, v0 and *a0). Tracing an EBOOT-range function now takes the main dispatch entry as the original; the
menu overlay's grafted copy (`ovl_menu_func_0887B3EC_entry`) used to be picked and left the menu without text (white screen, hang).
Slow alternative that was tried and dropped: scanning all guest RAM over the debug socket for the UTF-16 name (20 minutes).

#### Plan: corner cells 0, 1, 8, 9, 30, 31 (not implemented)
Findings from the disassembly (menu overlay):
* Screen struct S = `*(obj+0x28)`, 0x7DC bytes from `FUN_0885C6CC` in the init function 0x08A80D8C (`ori a1,zero,0x7DC`). Fields: +0x404
  pointer A to the layout's sheet array, +0x408..+0x58F handles H[98], +0x590 text base, +0x594 name text handles (20), +0x5F4 sheet base
  index, +0x678 cells (40 x 8 bytes, to 0x7B8).
* The tile of slot s is NOT in H. Its image is bound on the sheet object `A[S[0x5F4] + 2 + s]` (`FUN_08A7FB54` -> `FUN_088D0984(heap, sheet, byte, frame)`);
  H[20+s] is only a node (`FUN_088DAA40(sheet, quad 0)`) of that sheet, positioned from layout quads through the table at 0x08A6218
  (`FUN_08A85E74`) and `FUN_08A7F398`. The 98-entry creation table is 0x08A85EE8 (`FUN_08A85E68`; entries {quad/frame, sheet idx, quad idx,
  flag}; 20..53 slot tiles, 59..90 backdrops on one shared sheet). The image is per sheet, so a new tile needs a new sheet with its own
  image, not just a new handle.
* Sheets come from the layout (SEQ pack `menu_pk_upper_chara_select_seq_0.bin`, sheet array A). Adding six tiles therefore means six more
  sheets (SEQ entries + quads) in that pack, or cloning a sheet object and registering it in the draw lists (not understood).
* H relocation sites (variable index): 0x08A7EC34/EC50, 0x08A7F410/F438/F454, 0x08A7F628, 0x08A80ADC/B10/B30 (`addiu a0,a1,0x408`),
  0x08A80B94, 0x08A81C18, 0x08A82058/20CC/20F0/2104/2120, 0x08A84E94/EA8/ED4/EE8 (H[0], H[1]); fixed-index uses 0x410..0x4F4 (~60 sites in
  0x08A7E000-0x08A86000, e.g. 0x08A7EF00-F038, 0x08A83BCC-C2C, 0x08A84564-84F5C). Static route: enlarge the allocation (`ori a1,zero,0x7DC`),
  append a bigger H' at the end of S, shift every immediate in 0x408..0x58F whose base is S, raise `slti 0x62` (0x08A82060, 0x08A8212C,
  0x08A81C24) and relocate the creation table (hook `FUN_08A85E68`).
* Slot -> cell: reimplement `FUN_08A7FB54` on the host (it is short) with a corner remap for slots >= 34, writing the cell record at
  S+0x678+8*cell. Cell geometry is read from existing neighbours (pitch of cells 2/3, 10/11).
* Visual note: cells 0/1 sit under the hand cursor, 8/9 under the big art; 30/31 (row 4, cols 1-2) are free.
* The corner test needs more than 34 tiles, which the list capacity above forbids; extras would have to be placed outside the list buffer
  by the host placer.

### Extra characters frozen at battle start: solved, 18 Oct

Symptom: an extra (charId >= 33) never responds to buttons or acts as a CPU, its reticle floats away, it cannot be hit, and Noctis's extra draw parts stay visible.

Cause: not a charId table. `roster_mainbin.py` `renamed()` wrote the clone's name over `r[0x24:0x44]`, which also zeroed the 16-byte **file stem** at +0x34 of every cloned objentry record (retail: `p_fiv100`, `p_fiv100_w`, `p_fiv100_a`). The engine builds `obj/<stem>.objx` (the move / motion script of the body) from the stem, and FUN_0883975c (the body object constructor) copies it to `this+0x4CC`. The body GMO is found by the name at +0x24, so the model, HUD and animation clips loaded, but `p_<code>.objx` was never requested (see the 17 Oct note above that it "was not requested") and the actor had no script to run.

How it was found: `PSPRECOMP_FIRSTCALL=<gate addr hex>:<file>` (new, `psp_dispatch.cpp`) logs the first call and the call count of every function while a guest byte is non-zero (`W 08701F00 01`, value = phase 1..15; the counts are written when the phase changes). Retail Bartz as P1 against an extra (Noctis, same like) showed the per-frame actor functions (FUN_08A79550, FUN_08A5B3D0, FUN_08A50AF4, FUN_08B2F5E0...) called ~450 times in retail and never for the extra. Dumping both actors (found with `PSPRECOMP_ARG_WATCH=088373C8`) showed the extra's name copy at +0x4CC empty, and `PSPRECOMP_FN_WATCH` on FUN_0883975c / FUN_088444D4 / FUN_08843F4C plus a read of the objentry record (id 1338, stem empty) closed the loop.

Fix: `renamed()` now rewrites the name (+0x24) and the stem (+0x34) separately, replacing the like's code in the stem. Rebuild main.bin with `roster_mainbin.py <disc0> <mods> --first 33` (the default first id is 34, which silently skips a slot 33 line). Verified (build_models, private save, Lightning CPU): Noctis casts Razor Gale / Ravager on circle / square, Y'shtola casts spells, the per-frame player functions are called every frame.
Not yet re-run: Vivi, extra vs extra, retail vs retail regression, the 10-minute soak.

### Abilities screen empty for the extras: solved, 19 Oct (runtime rebuild needed)

Symptom: Noctis's Abilities screen listed no attacks (every Land/Air slot of SET A empty, "CP 85 / 350") and could not be left
("You don't have any HP attacks enabled"). Vivi and Y'shtola had the same problem. In battle Noctis still played Bartz's moves.

Cause: an ability id is `(owner charId << 8) | n` (0x40nn: shared supports, owner byte 0xFF). The list builder (menu overlay
FUN_08AD6698) takes only the system.bin `ablt` records whose owner byte (+0x1B) is the edited charId (`*(P+0)`,
`P = *(*(0x08B01CF8)+0x140)`) or 0xFF and that the learned list (`P+0xC80+0x3DE`, FUN_088B8414) holds. The equipped slots compose
`(set+0xB6 << 8) | n` (FUN_088B7A88 / FUN_088B7AB8), and the HP check (FUN_08AD11BC) uses only the listed entries. charId 34 owned only
the unfinished ORG800 record 0x2200, and its save record (a copy of Bartz's) held Bartz's ids and `set+0xB6 = 9`. The screen commits
through FUN_08AF3ECC into the record FUN_088CB164 returns, and saverec_hook used to overwrite that record with a new copy of Bartz's
record on every call, so edits were lost.

Fix:
* `tools/roster_mainbin.py` (every roster line, also through new_character.py):
  * Gives every extra its own `ablt` records, copies of its like's under `(charId << 8) | n` with owner charId. The +6 / +8
    ability references are moved along, and the command id, CP and the names stay the like's. It drops charId 33's leftover
    0x2100..0x2103 and 0x2200. Totals: 790 -> 824.
  * Adds `chrl` (learned by level) and `amas` (unlocked by mastering) rows for the extras, copied from the like with the
    ability ids moved along.
  * Self-test: `python3 -I tools/test_roster_mainbin.py`.
* Loader limits:
  * `chrl` and `amas` are copied into heap blocks of their own size (FUN_088B6E60, FUN_088B1B20), so they can grow.
  * The `ablt` loader (FUN_088B1CC0) allocates `size + (808 - count) * 40` bytes: over 808 records it overflows.
    roster.cpp `ablt_load_hook` copies such a table into a host block (`[ROSTER] ablt: 824 records ... moved to`).
    **main.bin built with the extras' abilities needs this runtime.**
* roster.cpp `saverec_hook`: the private record is derived from the like's as before, then moved to the extra's charId:
  * record byte 0;
  * ability block (record + 0x10) +0x762 and the +0xB6 of each of the 5 sets (0xC6 bytes each);
  * learned ids at +0x3DE (150 x {s16 id, s16 AP, u16 flags}).
  It is derived again on every call until the game writes to it; from then on it is the character's own record
  (`[ROSTER] charId 34: save record changed by the game, kept as its own`). The records live in a host block (`ROSTER_SAVEREC`),
  not in the volatile scratch RAM.

### Extra characters' records saved with the game, 19 Oct

The memory card save (`ms0/PSP/SAVEDATA/ULES01505GameData00/DISSIDIA.BIN`) holds the save object `*(0x089D712C)` from +0xF08:
0x3ECC8 bytes, with the 31 retail records at +0xF58. There is no room for more records, and the save is left exactly as the game
writes it. The extras' own records are kept in one sidecar file per save:

    ms0/PSP/ROSTER/<gameName><saveName>_<fileName>.roster      e.g. ms0/PSP/ROSTER/ULES01505GameData00_DISSIDIA.BIN.roster

Back it up together with `ms0/PSP/SAVEDATA`.

* **When it is written and read.** roster.cpp wraps the sceUtilitySavedata InitStart / GetStatus / ShutdownStart import stubs
  (found with `psp_hle_stub_addr_for_nid`, then `psp_dispatch_register`). The first GetStatus that reports FINISHED (or the
  ShutdownStart) handles the finished call.
* **What counts.** Only successful loads (modes 0/2/4/15/16) and saves (1/3/5/13/14/17/18) of the game's own data file count:
  the game name at `*(0x089D6010)` and a data size of 0x3ECC8. The import of an earlier DISSIDIA save uses another game name
  and size, and listings are ignored.
* **Which save slot.** It is the slot in saveName (the list dialog writes the chosen slot back there). Otherwise it is the
  saveNameList slot whose data file exists; for a save, the newest one.
* **Save.** The sidecar gets the records of the extras that have one of their own (the others are left out and keep following
  their like). It also stores the FNV-1a hash and size of the data file as written. It is written to `.tmp` and then renamed.
  Log: `[ROSTER] save ULES01505GameData00/DISSIDIA.BIN written: records of charId 34 (Lv 12) -> .../PSP/ROSTER/...roster`.
* **Load.** The sidecar is used only if its checksum, layout and stored hash / size match the data file on the memory stick.
  Extras in it get their saved record as their own; every other extra follows its like. A missing, stale (save written
  without it) or damaged sidecar means every extra follows its like.
  Log: `[ROSTER] save ULES01505GameData00/DISSIDIA.BIN loaded: restored the records of charId 34 (Lv 12) (...)`,
  or `loaded: no sidecar / written for another version of this save / damaged (...): the extra characters follow their like`.
* **New Game.** FUN_088CB210 (creates the 31 records; FUN_088AB800 calls it for New Game and to reset the save data) on the
  live object (`*(0x089D712C)+0xF58`) makes every extra follow its like
  (`[ROSTER] save data created anew (New Game): ...`).
* **File layout** (little endian):
  * header: `"DSRSTR01"`, u32 0x60, u32 0x778, u64 data file hash, u32 data file size, u32 n, char dir[32], char file[16], 8 zero bytes;
  * n x {u8 charId, u8 like, 6 zero bytes, the 0x778-byte record};
  * u64 FNV-1a of everything before it.
* **Checked offline.** `tests/run_roster_hooks_test.sh` includes roster.cpp with fake stubs and a temporary ms0. It covers the
  ablt hook, the private records, and these sidecar cases: save then restart then load; stale and damaged sidecars; two slots;
  New Game; and the ignored cases (cancel, import, listing, failed save). Not yet run in game.

### The extras' own ability names and help texts, 19 Oct

The copied `ablt` records used to keep the like's text ids, so an extra's Abilities screen showed the like's move names.

* `roster_mainbin.py` (and `new_character.py`) now apply `games/dissidia/extra_abilities.json`. They append the new names to
  `general_archive/main/<lang>/main_lang.bin` -> `ability.bin` and the help texts to entry 0 of
  `text/<lang>/mess_pk_customize_ability_0.bin`.
* They point the records' +2 / +4 at the new texts and write both files next to main.bin. `--names none` = the old output.
* The help table no longer fits the plain mess layout (u16 starts), so it is written as an OFF4 entry. The game's text code
  reads that layout too: entry 1 of the same pack is OFF4 in retail.
* No runtime change. Name table, layouts, loaders and limits: [ability names](#the-extra-characters-ability-names-and-help-texts-19-oct).

### EX Mode per character: `exranged=` and `armiger=` (20 Oct, runtime rebuild needed)
Noctis's EX Mode "Armiger Unleashed" (crystal royal arms orbit him; immune to ranged attacks). Tokens on the roster line,
e.g. `34 noc100 9 ... exranged=2 armiger=157,158,159,73,74,14`; nothing is hooked without them. Code: `roster.cpp`
(`exmode_install`, `can_hit_hook`, `bone_post_hook`), test `tests/test_exmode.cpp` (`run_exmode_test.sh`).
- **How EX Mode works (RE):** EX flag = actor +0x5B2 bit 2, set by `FUN_08ACAFAC` (activation; also applies the chrp
  +0xC trait flags 2/0x10/0x800, plays script 3000 = `_170A` phase 3 only when activated while being hit, i.e. EX
  Revenge), cleared by `FUN_08ACA77C`. `FUN_08ACB208` (per-frame EX manager) calls `FUN_08ACA4D8(on)` once the
  activation flash is over and at the end: it swaps the EX parts (`FUN_08ACAC60`) from the system.bin **chrp** row
  (actor +0x5A4): +0x18 = 18 x s16 draw bones shown outside EX (hidden in EX), +0x3C = 18 x s16 draw bones shown only
  in EX (+0xC bit 0x100000: pairwise swap, Vaan). Retail: Squall [48 Revolver] / [49 Lionheart], Cloud [51]/[52],
  Gabranth [54]/[52,53], Tidus [40]/[41], Lightning [46]/[53], Tifa []/[55], Prishe []/[40], Bartz [72 short cape] /
  [73 long cape, 74 star over his head on bone `star`, animated in all 84 motions]. It also plays a common effect
  (id at actor +0x390 -> +0xC -> +0x38) and instantiates the character's FEPA records with +0x34 == 2 and +0x64 == 0
  for the EX duration (no retail character has such a record). The EX trait getters `FUN_08B343D0`..`FUN_08B3476C`
  test chrp +0xC bits while in EX. No retail EX trait gives projectile immunity.
- **`exranged=N`:** wraps EBOOT `FUN_08807D4C(hit object, defender)` (the "may this hit object hit this defender"
  check of the contact pass `FUN_08809CEC` and of the melee hitbox callback). While the defender (charId match,
  chrp record agrees, not a summoned object) is in EX Mode: N >= 1 makes hit objects of kind 2 (u16 +0x2C: magi
  projectile hitboxes, `FUN_0880B05C`) pass through; N >= 2 also hits whose attacker (+0x18 / +0x20) is a kind-3
  object (u8 +0x542: wpnp summoned / thrown objects). Melee (kind 1), stage hazards (3), EX Burst direct damage
  (`FUN_08808AE8`) are unchanged.
- **`armiger=ORBIT,FLOOR,FLASH,RINGDRAW,FXDRAW[,BODYDRAW]`:** wraps EBOOT `FUN_0880EA20(spring data, gmo model,
  actor)`, the last pass that writes model-space bone matrices (cape physics) before drawing. When RINGDRAW's
  visibility entry is shown (the engine shows the EX parts), it turns ORBIT about the vertical at 40 deg/s in world
  space (facing cancelled with actor +0x330), following the root bone, with an intro (rise 0.35 m, grow from 15 %,
  +300 deg spin-up, fade-in) and sets RINGDRAW's alpha to 205 (translucent pass); for 0.7 s it animates FLOOR (floor
  ring spreading 0.3 -> 1.6 m) and FLASH (light pillar) with FXDRAW's alpha fading out, and dims BODYDRAW to alpha 96
  and back (adds its visibility entry if missing; always restored to 0xFF). Time step = the actor's own
  (`FUN_08838300`), so slow motion / pauses slow it. Visibility table: *(actor+0x394)+0x10C, {bone, shown, alpha},
  alpha 0xFF opaque pass, 1..0xFE translucent pass (`FUN_0886ADBC`), 0 not drawn.
- **Old runtime:** the GMO parts still work as plain EX parts: the ring stands still around him (opaque) during EX
  Mode only; the floor ring / pillar bones have rest scale 0 and stay invisible; no ranged immunity.

### 40-cell Quick Battle grid (22 Oct)

With more than 34 tiles (characters + the "?" tile) the grid now uses all 40 cells of its 10 x 4 matrix instead of
dropping characters (before: with every retail character unlocked, only two extras were listed). Nothing changes
with 34 or fewer tiles: the game's own layout runs.

* Layout: row 1 moves two columns left (cells 0..5, Lightning first) and gains cells 6..9; row 4 gains cells 30/31.
  Characters fill cells 0..5, 10..29, 30.. (the special tiles keep the last cells of row 4), then 6..9. At most 39
  characters are handed to the grid (`kMaxChars`); with two special tiles one fewer.
* Geometry (read from the retail tile nodes): tile top-left x = 28 + 32 * col, y = 113 + 39 * row.
* Sprites: the layout has 34 tile sheets (`A[2..35]` of the screen's sheet array, node `H[20 + slot]`, all sharing the
  tile layout definition of `A[2]`). Entries 34..39 get sheets made at run time: `FUN_088D107C(heap, 0, 0)` (pool
  sheet), `FUN_088DA8E8(sheet, def)` (the tile definition, as `FUN_088D0700` does for layout sheets), moved in the
  heap's draw list (head `heap+0x24`, tail `+0x28`; sheet `+0x24` next, `+0x28` prev) behind the last tile sheet
  (the pool puts it at the head, under the background), node `FUN_088DAA40(sheet, 0, 0)`, image bound like the
  placer (`FUN_088D0984`). Destroyed with the screen (`FUN_088DA060` node, `FUN_088DAB44` sheet).
* Sprite structs: sheet 0x2C bytes `{node head, tail, count, def +0xC, image +0x10, flags +0x14, priority +0x15,
  +0x18/+0x1A from def, callback +0x1C/+0x20, next +0x24, prev +0x28}`; node `{+0x10 x, +0x14 y, +0x18/+0x1C scale,
  +0x20 flags (8 = shown), +0x24 owner sheet, +0x28 colour, +0x3C/+0x40 links}`. `FUN_088D9B84(node, m)` takes a
  4x4 (translation at +0x30/+0x34).
* Hooks (`roster.cpp`, `grid40_install`): `FUN_088D5AC8` keeps every entry host-side (at most 34 go into the Quick
  Battle caller's buffer at screen struct +0x974: live fields follow at +0xA8C); `FUN_08A80D8C` (grid init, list in
  t0/t1) gets the whole list; `FUN_08A84FD4` (layout) is replaced by `layout40` for more than 34 tiles (same record
  scan, `FUN_08A80A4C` check and special tiles per mode as the game; the game's placer still binds and positions
  slots 0..33, then their cell records move to the new cells); `FUN_08A7F398` with an explicit reference `14 + slot`
  (cursor hand and highlight) is moved to that slot's cell; `FUN_08A810A4` (selected animation) animates the host
  node; `FUN_08A7EB80` (teardown) frees the host sprites.
* Default cursor (23 Oct fix): the game hard-codes column 2 / row 0 (slot 0's retail cell) in the grid init
  (`FUN_08A81BB8`, with S+0x600 = slot 0 under the hand) and in `FUN_08A812FC` (a side's first visit, e.g. the
  opponent with nobody picked). On the 40-cell grid cell 2 holds slot 2, so the hand showed Lightning while confirm
  took Laguna. `layout40` now puts column/row on the cell of the slot at S+0x600 after the layout tail, and
  `FUN_08A812FC` is wrapped to use slot 0's cell (record S+0x5FC and slot S+0x600 read from it).
* Env: `PSPRECOMP_GRID40_LOG=1` logs the layout, `PSPRECOMP_GRID40_OFF=1` keeps the retail layout (and its limit).
* Checked in the cloud build: P1 and CPU Select with 35, 37, 38 and 39 characters (all cells), cursor moves and
  wraps over the new cells, the "?" tile on a new sprite picks at random, Noctis on a new tile (cell 8) through the
  confirm dialog, setup screen and VS screen, leaving and re-entering the grid, and the retail layout on a save with
  fewer characters.

## The extra characters' ability names and help texts (19 Oct)

Each extra character's Abilities screen used to show the names of the character it was cloned from. The `ablt` records
that `tools/roster_mainbin.py` copies for an extra kept the like's text ids. Now they point at text records of their own.

* **Source:** `games/dissidia/extra_abilities.json`, a name map per roster code. Edit it there; the format is in its `_doc`.
* **Applied by:** `tools/roster_mainbin.py` (and `tools/new_character.py`, which calls it) whenever they rebuild main.bin.
  `--names PATH` uses another map; `--names none` builds main.bin as before (byte-identical to the 19 Oct
  `db12385b...` file).
* **Self-test:** `python3 -I tools/test_roster_mainbin.py` (synthetic tables, no game files).
* **No runtime rebuild is needed for the texts.** main.bin itself still needs the 19 Oct runtime with the `ablt` hook
  (824 records, see above).

### Files

A rebuild writes these three files into the mods directory. Install them together: main.bin's new text ids point into the
other two.

| File | What changes |
|---|---|
| `by_crc/93307c1c.bin` (main.bin) | `ablt` +2 (name id) / +4 (help id) of 29 extra records; nothing else. |
| `general_archive/main/en/main_lang.bin` | Member `ability.bin` (the name table): 26 names appended after the 1,307 retail ones. The other 20 members are byte-identical. |
| `text/en/mess_pk_customize_ability_0.bin` | Entry 0 (the help texts): 25 texts appended after the 1,221 retail ones, written as an OFF4 entry. Entry 1 is byte-identical. |

Only `en` exists in this package. The tool handles every language folder the package has (`TEXT_LANGS`), using the same ids
in each.

### Where the game reads them

* **Names.** An `ablt` record's +2 is a record of the `ability.bin` member of `general_archive/main/<lang>/main_lang.bin`.
  * The boot loader FUN_0893354C reads main.bin (at most 0x180000 bytes) and main_lang.bin (at most **0x80000** bytes) into
    one 2 MiB block. A larger main_lang.bin fails the size check, so the tool refuses it. Ours is 398,080 B.
  * FUN_088BA88C -> FUN_088B1CC0 (the `ablt` loader) finds the member by name (`"ability.bin"` at 0x089CB778).
    FUN_0887AC70 copies it into a heap block of its own size (text object 0x09B49108), so the table can grow.
* **Help texts.** An `ablt` record's +4 is a record of entry 0 of `text/<lang>/mess_pk_customize_ability_0.bin`.
  * The menu overlay loads the pack by path (0x08B0145C, in `ovl_menu_FUN_08AF7AEC`).
  * FUN_0887B260 allocates its file size from the screen's heap and decodes every entry with FUN_0887A6A0.
  * Entry 1 is an OFF4 table of move-detail texts that `ablt` does not reference. Those texts are unchanged, and so are the
    extras' detail texts.
* **Text layouts** (`tools/mess_text.py`):
  * **Plain:** u16 count, u16 record starts in words. Records must start below 64 Ki words, about 128 KiB.
  * **OFF4:** `"OFF4"`, u32 count, u32 offsets in 4-byte units, then records of {u32 byte length, units}, padded with NULs to an
    even number of units.
  * The game's text code reads both layouts:
    * FUN_0887A8F0 tests the magic.
    * FUN_0887A800 returns a record's offset.
    * The accessors FUN_0887B3EC / B500 / AD78 / AEAC / AFBC all go through FUN_0887A800.
  * Every layout uses the same XOR key, `KEY[(record + position) % 16]`.
  * Retail entry 0 of the help pack is 128,302 B. Only about 2.7 KB fit after it in the plain layout, and the 25 new texts
    need about 3.6 KB. So the tool writes that entry as OFF4 (`mess_text.build_fit`): the same thing retail does for the
    153 KB entry 1. Every retail record keeps its index, its text and its encoded units.
  * The name table still fits the plain layout and stays plain.
* **Memory.** The help pack grows from 281,840 to 291,888 B: about 3.6 KB of text and about 6 KB of OFF4 framing. It is
  allocated from the Abilities screen's heap, which could not be measured offline. If that heap ran short, the runtime's
  allocator diagnostic would print `[MODELS] heap 0x... alloc(291888) FAILED` when the screen opens.
  Fallback: `--names none` and remove the two text overrides.

### How the names were chosen

For each extra ability, the record's command id (+12) was resolved to the character's objx `comb`, then to `plya` actions,
then to motions. The installed objx and GMO were compared with the like's retail files. The effects (FEPA names of 0x0219
events, `atkp` hit-effect bits, `magi` chains) show what each move now looks like.

* **Noctis:** the re-made moves get FFXV names (Blitz Attack, Royal Cleave, Star of the Rogue, Alterna ...).
  * Moves that are still Bartz's **keep Bartz's names**. No FFXV placeholder names were invented for them.
  * 0x2201 (Solid Ascension's record) now plays Bartz's Blade Crash chain (`_658A/_656A/_657A`), left on command 0xA3 by the
    Blitz comb swap. So it is named **Blade Crash**, after what it plays.
  * The old hand-keyed sword combo (v1) on Slide Shooter's air chain is **Blitz Attack (midair)**.
* **Vivi:** each Shantotto spell becomes the FF9 black magic spell with the same effect: Stun -> Stop, Bind -> Sleep.
  * Spirit Magic Earth / Fire / Thunder / Water / Air / Ice become Comet / Flare / Thundaga / Water / Doomsday / Blizzaga. FF9
    has no wind spell, so the design doc's Doomsday takes Air.
  * **Kept:** Bio, which is an FF9 spell already.
  * **Kept:** A Couple Attacks and Retribution. They are re-animated as staff casts, but they are still physical hits with
    Shantotto's normal / Retribution hit flash, and no FF9 spell fits them. They get spell names once they have spell effects.
  * Help texts no longer say "strength depends on bravery": Vivi's HP tier records are equalised.
* **Y'shtola:** her aeons are invisible, so every summon reads as a spell. The FF14 black mage names follow the design doc
  where it names a spell: Fire, Thunder, Scathe, Paradox, Blizzard, Foul; HP Fire IV, Flare, Thunder III, Freeze, Flare Star.
  * Impulse's cast motion is still Yuna's `_023A`; its aeon is invisible like the others.
* **Help texts** are new one-to-three-line descriptions, at most 35 characters a line, the retail limit. Bonus lines (equip
  bonuses) and orange effect lines are taken from the record's original help text, because the record keeps the like's
  values. Moves that kept their name keep the like's help text too.

Help column: the map's own lines. *(like's bonus line)* / *(like's effect line)* are the `{bonus}` / `{effects}` lines
copied from the original help text. *(effect: X)* is an orange `Additional Effect: X` line.

#### Noctis (charId 34, `noc100`, from Bartz)

Record kind: an entry may set `type` (brave / hp, `ablt` +0x16), `phase` (ground / midair / follow-up, +0x17) and
`after` (+6, the ability it follows up; null = none), and its cost: `cp` (CP to equip, +0x0E; `cp_mastered` +0x10,
default half), `ap` (AP to master, +0x12) and `bonus` (equip bonus, +0x14: a system.bin `fncs` effect set, null = none;
docs/archive/ablt-fields). `learn` on an ability the like has moves it to that level of the character's `chrl` row
(2026-10-24, `set_learn_levels`). Bartz's Flare (0x0907, command 0xAC) is an HP follow-up of Holy
(+6 = 0x0906, phase 0xFF; comb 0xAC prereq_b = 0xAB, step 1 = Holy's action 1949 with flag 0x100): Noctis's 0x2207
(Greatsword Down Strike) is made a midair brave move of its own this way (2026-10-20), with comb 0xAC and its
landing hit 0xCDA changed in the objx (forge/ffxv_tools/greatsword/gs_brave.py). Since then it is one press (comb
0xAC = action 1949 = `_640A` only): the chop turns into a plunge to the ground from any height, one brave hit, on the
engine recipe of Cloud's Braver `_704B` (forge/ffxv_tools/greatsword/install_gs_dive.py).

Disabled abilities: an entry with `"disabled": true` takes the move out of the kit but keeps its `ablt` record, because
the game reads the record of every learned and equipped id without a null check (EBOOT FUN_088B5254, FUN_0884921C).
The tool sets the owner byte (+0x1B) to 0xFE, so the Abilities screen (menu FUN_08AD6698) no longer lists it, a set slot
that still holds it shows as empty, and roster.cpp `learn_all_own` stops teaching it. It also sets the command (+12) to
-2, which matches no comb record, so pressing an old equipped slot does nothing in battle (FUN_0884921C) and the CPU
never picks it (FUN_08AB7F40). The character's `chrl` entries drop it, and an `amas` entry that unlocks it is dropped.
Star of the Rogue (0x220D) is disabled this way since 22 Oct 2026, until its animation exists; Reel Impulse (0x2203, still
Bartz's) and the HP Warp-Strike (0x220F, replaced by the warp dash) since 23 Oct. To bring it back, delete
the flag and rebuild main.bin (`roster_mainbin.py <disc0> work/mods --first 33`).

| Ability | Cmd | Type | Motions | Old name (Bartz) | New name | Status | Text ids (name / help) | Help text (en) |
|---|---|---|---|---|---|---|---|---|
| 0x220C | 0x0A2 | Brave | `_603A` `_604A` `_605A` | Blade Crash | Blitz Attack | re-made | 1330 / 1243 | [Close] Leaping seven-hit combo. / (like's bonus line) / (effect: wall rush) |
| 0x2201 | 0x0A3 | Brave | `_658A` `_656A` `_657A` | Solid Ascension | Blade Crash | renamed | 1327 / 1240 | [Close] Two-sword slashing chain. / (like's bonus line) / (effect: chase) |
| 0x2202 | 0x0A4 | Brave | `_610A` `_652A` `_653A` `_654A` `_655A` | Hazard Raid | *Hazard Raid* (kept) | kept | 511 / 544 | the like's, unchanged |
| 0x2203 | 0x0A5 | Brave | `_620A` `_631A` | Reel Impulse | *Reel Impulse* | **disabled** (23 Oct; still Bartz's, unchanged) | 512 / 545 | the like's |
| 0x220D | 0x0A9 | Brave | `_650A` (_659A still Bartz's) | Rush Impact | Star of the Rogue | **disabled** (22 Oct) | 1331 / 1244 | [Close] Slash and fling a star. / (like's bonus line) / (like's effect line) |
| 0x2205 | 0x0AA | Brave | `_632A` `_633A` `_651A` | Slide Shooter | Blitz Attack (midair) | re-made | 1328 / 1241 | [Mid] Engine Blade combo in midair. / (like's bonus line) / (like's effect line) |
| 0x2206 | 0x0AB | Brave (midair) | `_654A` `_655A` | Holy | Dagger Strike | re-made | (see extra_abilities.json) | [Close] Dagger flurry, then a dive. / (like's bonus line) / (like's effect line) |
| 0x220E | 0x0A6 | HP | `_753A` | Hellfire | *Hellfire* (kept) | kept | 530 / 563 | the like's, unchanged |
| 0x2208 | 0x0A7 | HP | `_750A` | Dark Flame | Royal Cleave | re-made | 1329 / 1242 | [Long] Slam unleashes a blue flame. / (like's bonus line) / (like's effect line) |
| 0x2207 | 0x0AC | Brave (midair; was HP, a follow-up of 0x2206) | `_640A` | Flare | Greatsword Down Strike | re-made | (see extra_abilities.json) | [Close] Chop that slams to earth. / (like's bonus line) / (like's effect line) |
| 0x220F | 0x0AD | HP | `_752A` | Ragnarok Blade | Warp-Strike | **disabled** (23 Oct; the HP Warp-Strike no longer exists, the R+Triangle warp dash replaced it; the record stays because Alterna 0x2204 is copied from it) | 1332 / 1245 | |
| 0x220B | 0x0AE | HP | `_751A` | Luminous Shard | *Luminous Shard* (kept) | kept | 533 / 566 | the like's, unchanged |

#### Vivi (charId 33, `viv100`, from Shantotto)

| Ability | Cmd | Type | Motions | Old name (Shantotto) | New name | Status | Text ids (name / help) | Help text (en) |
|---|---|---|---|---|---|---|---|---|
| 0x2100 | 0x000 | Brave | `_600A` `_601A` `_602A` | A Couple Attacks | *A Couple Attacks* (kept) | kept | 761 / 764 | the like's, unchanged |
| 0x2102 | 0x001 | Brave | `_611A` | Stun (ground) | Stop (ground) | recast | 1307 / 1221 | [Special] Stop time on the foe and / cancel their attack. Instant, no / damage; a new attack frees them. |
| 0x2103 | 0x002 | Brave | `_612A` | Bind (ground) | Sleep (ground) | recast | 1308 / 1222 | [Special] Put the foe to sleep. / They can't move until another / attack wakes them. |
| 0x2104 | 0x003 | Brave | `_613A` | Bio (ground) | *Bio (ground)* (kept) | kept | 765 / 768 | the like's, unchanged |
| 0x2101 | 0x004 | Brave | `_620A` | Retribution | *Retribution* (kept) | kept | 762 / 765 | the like's, unchanged |
| 0x2105 | 0x005 | Brave | `_611A` | Stun (midair) | Stop (midair) | recast | 1309 / 1221 | [Special] Stop time on the foe and / cancel their attack. Instant, no / damage; a new attack frees them. |
| 0x2106 | 0x006 | Brave | `_612A` | Bind (midair) | Sleep (midair) | recast | 1310 / 1222 | [Special] Put the foe to sleep. / They can't move until another / attack wakes them. |
| 0x2107 | 0x007 | Brave | `_613A` | Bio (midair) | *Bio (midair)* (kept) | kept | 768 / 768 | the like's, unchanged |
| 0x2108 | 0x008 | HP | `_700A` `_707A` `_708A` | Spirit Magic: Earth | Comet | recast | 1311 / 1223 | [Long] Boulders hurtle at the foe. |
| 0x210E | 0x00B | HP | `_700A` | Spirit Magic: Earth | Comet | recast | 1311 / 1223 | [Long] Boulders hurtle at the foe. |
| 0x2109 | 0x00C | HP | `_701A` `_706A` `_708A` | Spirit Magic: Fire | Flare | recast | 1312 / 1224 | [Long] Launch a blazing fireball. |
| 0x210A | 0x00F | HP | `_702A` `_706A` `_707A` | Spirit Magic: Thunder | Thundaga | recast | 1313 / 1225 | [Mid] Lightning strikes from above. |
| 0x210B | 0x012 | HP | `_704A` `_711A` `_709A` | Spirit Magic: Water | Water | recast | 1314 / 1226 | [Long] Drown the foe in a torrent. |
| 0x210C | 0x015 | HP | `_703A` `_711A` `_710A` | Spirit Magic: Air | Doomsday | recast | 1315 / 1227 | [Long] Engulf the foe in a tempest. |
| 0x210D | 0x018 | HP | `_705A` `_710A` `_709A` | Spirit Magic: Ice | Blizzaga | recast | 1316 / 1228 | [Long] Fire a volley of ice shards. |
| 0x210F | 0x01B | HP | `_705A` | Spirit Magic: Ice | Blizzaga | recast | 1316 / 1228 | [Long] Fire a volley of ice shards. |

#### Y'shtola (charId 35, `ysh100`, from Yuna)

| Ability | Cmd | Type | Motions | Old name (Yuna) | New name | Status | Text ids (name / help) | Help text (en) |
|---|---|---|---|---|---|---|---|---|
| 0x2302 | 0x2A5 | Brave | `_610A` `_611A` `_612A` | Meteor Strike | Fire | recast | 1317 / 1229 | [Close] Point-blank burst of flame. / (like's effect line) |
| 0x2303 | 0x2A7 | Brave | `_620A` `_621A` `_622A` | Aerospark | Thunder | recast | 1318 / 1230 | [Mid] Lightning strikes the foe. / Wait between presses to hit more. / (like's effect line) |
| 0x2304 | 0x2AD | Brave | `_630A` `_631A` | Energy Blast | Scathe | recast | 1319 / 1231 | [Long] Sweep five beams of aether. |
| 0x2305 | 0x2AE | Brave | `_640A` `_641A` `_642A` | Sonic Wings | Paradox | recast | 1320 / 1232 | [Mid] A blast of aether that rises. / Best used from below the foe. / (like's effect line) |
| 0x2306 | 0x2B0 | Brave | `_650A` `_651A` `_652A` `_653A` | Heavenly Strike | Blizzard | recast | 1321 / 1233 | [Close] Quick burst of ice. / Short reach, fast to come out. / (like's effect line) |
| 0x230D | 0x2B2 | Brave | `_023A` | Impulse | Foul | recast | 1326 / 1239 | [Long] Gather aether, then release. / Charging alters its power. |
| 0x2308 | 0x2B8 | HP | `_700A` | Hellfire | Fire IV | recast | 1322 / 1234 | [Close] Engulf the foe in flame. / Covers a tall area up close. |
| 0x2309 | 0x2B9 | HP | `_704A` | Energy Ray | Flare | recast | 1312 / 1235 | [Long] Fire pillars burst forth. |
| 0x230A | 0x2BA | HP | `_701A` | Thor's Hammer | Thunder III | recast | 1323 / 1236 | [Mid] Call down a mighty bolt. / Reaches foes at any height. / (like's effect line) |
| 0x230B | 0x2BB | HP | `_702A` `_702B` | Diamond Dust | Freeze | recast | 1324 / 1237 | [Close] Lock the foe in ice. / Fast, but only up close. |
| 0x230C | 0x2BC | HP | `_703A` | Mega Flare | Flare Star | recast | 1325 / 1238 | [Long] Unleash a star of flame. / Hard to dodge, hits very hard. / (like's effect line) |

### Open

* Not yet seen in game: the names, the help panel, and whether the screen heap holds the larger help pack.
* Vivi's A Couple Attacks / Retribution and Noctis's six Bartz moves keep their like's names until they are re-made. Rename
  them with one JSON entry each.
* The move-detail texts (help pack entry 1) and any battle-side ability text are unchanged.
