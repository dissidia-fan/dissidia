# system.bin 'ablt': the 40-byte ability record

Research note (2026-10-24). Source: the retail USA main.bin (`work/disc0`, MPK -> system.bin ARC -> 'ablt'), header
`ABI ` / 0x20110610 / 790 records. The code addresses are EBOOT.dec.elf (loaded at its ELF vaddr) and OVL_MENU_APP.bin
(file offset 0 = 0x089DD4C0). All u16 values are little endian; 0xFFFF means none.
To reproduce: capstone (pip) plus `roster_mainbin.mpk_unpack/arc_unpack/AbilityTexts`.

## Field table

| off | type | meaning | conf. | evidence |
|---|---|---|---|---|
| +0x00 | u16 | ability id = (owner charId << 8) \| n, 0x40nn = shared support; the table is sorted on it and FUN_088B1D9C bsearches it | confirmed | roster.md |
| +0x02 | u16 | name text id (ability.bin record) | confirmed | roster_mainbin |
| +0x04 | u16 | help text id (mess_pk_customize_ability_0 entry 0) | confirmed | roster_mainbin |
| +0x06 | u16 | prerequisite: the ability this one follows up (the HP follow-ups of a brave attack) | confirmed | Rune Saber 010D -> 0100 Ascension; Extra Slice 0501 -> 0500 Multi-Hit |
| +0x08 | u16 | base ability: the variant's parent. Only Tifa (charId 0x1D, 56 records) uses it: her 1D17..1D4E variants point at 1D0F..1D16. FUN_088B9E28 resolves the id through it when it looks up the learned list (FUN_088B8414 with a2=1) | strong | 0x088B9E44 reads +8 |
| +0x0A | s16 | exclusion group: the Customize screen does not allow two equipped abilities with the same group (not -1). Examples: Speed Boost / + / ++ = 4, Concentration family = 0x20, Air Dash family = 1, Gabranth's Shadow Bringer / Luminous Shard = 1, and Tifa's variants 1..8 | strong | menu 0x08ACF8C4 reads +A, then loops over the equipped slots (save +0x3EBA / +0x148A) and compares their +A |
| +0x0C | s16 | command id (battle move, through the character's objx comb); -1 for supports | confirmed | roster.md |
| +0x0E | s16 | **CP cost** while the ability is not mastered | confirmed | menu FUN_08AF3AD8 (id) returns +0x0E, or +0x10 if the learned-list entry has flag 2; its callers (0x08ACFD8C, 0x08AF4734, ...) add it up over the 64 equip slots = the CP total |
| +0x10 | s16 | **CP cost once mastered** (always <= +0x0E; usually half of it, rounded down: 30->15, 20->10, 10->5, 5->0; supports may differ: 40->25, 90->45) | confirmed | same function |
| +0x12 | s16 | **AP to master** | confirmed | FUN_088B7F7C (AP gain): learned AP = min(AP + gain, +0x12); equal -> entry flags \|= 2 (mastered). FUN_088B8328 sets AP = +0x12 and flag 2 directly. 0x088B9B24 lists newly mastered abilities by comparing with +0x12 |
| +0x14 | s16 | **equip bonus / passive effect**: an id in system.bin 'fncs' (effect sets), -1 = none. Every support ability has one (its whole effect); an attack has one only when its help text has a bonus line (Bartz, Gabranth) | confirmed | FUN_088BE6xx (stat build) loops over the equipped ability slots (FUN_088B7A88 / FUN_088B7AB8), takes +0x14 and runs FUN_088C005C -> FUN_088BFF40 (bsearch in the fncs copy) -> FUN_088C00B0. Bartz Blade Crash 0x279 = fnct function 287 (movement speed), param 8 = Speed Boost 0x22F's function and param |
| +0x16 | u8 | type: 0 brave, 1 HP, 2 action support, 3 auto / system support, 4 battle-stat support (crit, shields, EXP...) | confirmed (0/1), strong (2-4) | roster.md; support categories by name |
| +0x17 | u8 | phase: 0 ground, 1 midair, 0xFF follow-up (and every support) | confirmed | roster.md |
| +0x18..0x1A | u8 x3 | always 00 03 00 | - | constant in all 790 records |
| +0x1B | u8 | owner charId (0xFF = shared support; 0xFE = disabled, our tool) | confirmed | menu FUN_08AD6698 |
| +0x1C | s16 | sort key of the support list (0 for attacks; supports 0x3C..0x6B, grouped by kind: 0x3C basic actions, 0x47 Speed Boost, 0x48 Jump Boost, 0x61 critical boosts...). The menu sorts on (+0x1C << 16) + id | strong | menu 0x08ACEAF4 |
| +0x1E | u8 | form / stance mask: 7 = always; Cecil 1 = Dark Knight / 2 = Paladin; Gabranth 1 / 2 (normal / EX); Lightning 1 Commando / 2 Ravager / 4 Medic (4 = Medic only; one record uses 3) | strong | read with +0x16 / +0x17 at 0x088B76B0 and 0x088B93E8 (set building) |
| +0x1F | u8 | always 0 (high byte of +0x1E as u16) | - | |
| +0x20..0x27 | u8 x8 | 8 small values 0/1/2. Attacks: 00 00 01 01 01 00 00 00 on 39 ranged / magic projectiles (Red Fang, Blizzard, Thunder, Holy, Fire...), else 0. Supports: only a few (Counterattack, Riposte, RPG Mode Boost, EXP to ...). Probably a per-preset weight or flag set (auto-equip "recommended" presets or CPU AI); not read through FUN_088B1D9C's direct callers | guess | data pattern only |

The "Additional Effect: chase / wall rush" line and the [Close]/[Mid]/[Long] tag are text only (help text +0x04); the
record has no field for them. The actual chase / wall rush behaviour comes from the move's objx / hit data. The level at
which an attack is learned is not in 'ablt' but in 'chrl' (CLEV: {u8 level, u8 flags, u16 id x3}); 'amas' lists what
mastering one ability unlocks.

Save side (for reference): a character's learned list is at save record +0x3DE, 150 entries of 6 bytes {s16 id, s16 AP,
u16 flags}; flag 2 = mastered (FUN_088B8414 searches it).

## Example rows (retail)

| id | name | +6 after | +8 base | +A grp | +C cmd | +E CP | +10 CPm | +12 AP | +14 fx | +16 | +17 | +1B | +1C | +1E |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0100 | Ascension (WoL) | FFFF | FFFF | FFFF | 01C | 30 | 15 | 120 | FFFF | 0 | 00 | 01 | 00 | 7 |
| 0109 | Shield of Light (ground) | FFFF | FFFF | FFFF | 023 | 30 | 15 | 130 | FFFF | 1 | 00 | 01 | 00 | 7 |
| 010D | Rune Saber (ground) | 0100 | FFFF | FFFF | 01D | 30 | 15 | 130 | FFFF | 1 | FF | 01 | 00 | 7 |
| 0500 | Multi-Hit (Onion Knight) | FFFF | FFFF | FFFF | 112 | 20 | 10 | 90 | FFFF | 0 | 00 | 05 | 00 | 7 |
| 0501 | Extra Slice | 0500 | FFFF | FFFF | 113 | 10 | 5 | 120 | FFFF | 0 | FF | 05 | 00 | 7 |
| 0706 | Slash (Cecil, Paladin) | FFFF | FFFF | FFFF | 12A | 20 | 10 | 90 | FFFF | 0 | 00 | 07 | 00 | 2 |
| 0710 | Shadow Bringer (Cecil) | FFFF | FFFF | 0001 | 3B4 | 30 | 15 | 130 | 04EB | 1 | 00 | 07 | 00 | 7 |
| 090C | Blade Crash (Bartz) | FFFF | FFFF | FFFF | 0A2 | 30 | 15 | 90 | 0279 | 0 | 00 | 09 | 00 | 7 |
| 0901 | Solid Ascension (Bartz) | FFFF | FFFF | FFFF | 0A3 | 30 | 15 | 90 | 027B | 0 | 00 | 09 | 00 | 7 |
| 1F11 | Cure (ground) (Lightning, Medic) | FFFF | FFFF | FFFF | 29E | 20 | 10 | 100 | FFFF | 0 | 00 | 1F | 00 | 4 |
| 402B | Speed Boost (support) | FFFF | FFFF | 0004 | FFFF | 20 | 10 | 70 | 022F | 2 | FF | FF | 47 | 7 |
| 4003 | Concentration (support) | FFFF | FFFF | 0020 | FFFF | 30 | 15 | 130 | 0255 | 4 | FF | FF | 60 | 7 |

Retail distribution: CP +0x0E is 30 in 453 records, then 10, 20, 40, 60, 50, 45, 75...; AP +0x12 is 130 / 100 / 150 / 90 /
120 / 80 / 200 / 50, a few above 255 (it is a u16).

## How a mod tool should set them

* **CP**: write +0x0E (unmastered) and +0x10 (mastered) together; keep +0x10 <= +0x0E. Retail attacks use half, rounded
  down (30/15, 20/10, 10/5). Both are s16. Raising the CP of an equipped ability can push an existing save's loadout over its CP
  limit (how the game reacts to that was not checked).
* **AP**: write +0x12 (s16, keep > 0). Saves store the AP earned per learned ability; lowering +0x12 below an existing
  save's AP does not set the mastered flag by itself (that happens only when AP is gained: FUN_088B7F7C sets the flag on
  AP == +0x12 after clamping AP to +0x12, so the next AP gain masters it).
* **Equip bonus**: point +0x14 at an existing 'fncs' effect set (reuse the like's or a support ability's id, e.g. Speed
  Boost 0x22F, Jump Boost 0x232, Physical Shield 0x25E, or Bartz's "minor" sets 0x276..0x285 / 0x5A6 / 0x5A7), or -1 for
  none, and keep the help text's bonus line (expand_help '{bonus}') in step with it: the text is only a description.
  A new effect set needs new 'fncs' records: fncs = header {'FSET', ver, u16 count (+8)} + count x {s16 id, s16 n, u32
  first sub} sorted by id + 1176 subs {s16 function (an id in 'fnct'), s16 nparams, u32 first param} + s16 params.
  The loader (FUN_088BFD3C) copies 'fncs' into a fixed 0x5000-byte heap block (retail 18928 B, so about 1.5 KB to spare)
  and 'fnct' into a block sized for 360 records (retail 355).
* Keep consistent: +0x0A groups (a new character's copies may keep the like's numbers: groups only matter within one
  loadout), +0x1E (a character without forms should use 7, otherwise the move is hidden in some forms), +0x06 / +0x08
  ids moved to the new owner (clone_ablt does this), +0x18..0x1A = 00 03 00, and +0x20..0x27 copied from the like.
