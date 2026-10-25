# studio_sharedsnd: shared sound banks for the Move Studio

`studio/sharedsnd.js` (copied from work/studio_sharedsnd) finds and decodes the sounds a timeline `sound` event (op 0x0303) can play that are **not** in the
character's own `snd_` bank, e.g. cue 100010, the most used cue in retail timelines. Plain browser JS (`window.SharedSnd`;
load `sndbank.js` first, it provides `window.SndBank`) or node (`require('./sharedsnd.js')`, which requires
`../studio_snd/sndbank.js`). `node test.js` reads the real disc (`work/disc0`) and writes `out/*.wav`.

## API

| call | what |
|---|---|
| `bankFiles(filter?)` | PACKAGE.BIN paths to read. Default `['general_archive/main/main.bin', 'map/maparc_one00_2.mpk']` (snd_common + snd_battle). `{all:true}`, or a `banks()` filter such as `{group:['stage','floor']}` or `{battle:true, shared:true}` |
| `cueLocation(cue)` | `{cue, id, base, slot, name, group, path, member, battle, variants}` or `null`. Covers every bank in the table, character banks too |
| `banks(filter)`, `bank(id)`, `BANKS`, `GROUPS` | the table (233 banks): `{id, base, slots, name, group, path, member, battle, alsoIn?, variants?}` |
| `loadBank(id \| record, getFile)` | Promise of a sndbank bank (`parseBank` result plus `.shared` = the record). Files and banks are cached per `getFile` |
| `decodeCue(cue, getFile, {pick}?)` | Promise of `SndBank.decodeSlot` output plus `location`, `slotInfo`, `played`, `randomGroup`; rejects for an empty slot. `pick`: member of a random group (index or `'random'`) |
| `cueWav(cue, getFile)` | Promise of WAV bytes |
| `listCues(bank)` | picker rows `{cue, slot, kind ('data' / 'alias' / 'random'), owner, rate, seconds, volume, loop, choices}` (empty slots left out) |
| `extract(fileU8, member)`, `parse(scdU8)`, `members(u8)` | MPK / ARC walking; the returned SSCF copy has its size field fixed (see below) |
| `headerBase(scdU8)`, `headerId`, `cueOf(id, slot)`, `pathHash(path)` | engine base, header id, `100*id+slot`, PACKAGE.BIN hash |

`getFile(path)` is any `path -> Promise<Uint8Array>`; in the studio it is `isoGet` from `retail.js`. Studio hookup:
```js
const loc = SharedSnd.cueLocation(ev.args[0]);              // ev.args[0] = the cue of a 0x0303 event
if (loc && loc.group !== 'character') playPcm(await SharedSnd.decodeCue(loc.cue, isoGet));
const rows = SharedSnd.listCues(await SharedSnd.loadBank(1000, isoGet));   // the picker list for snd_battle
```

## Cue -> bank + slot (engine)

`FUN_0899075C` registers a bank (20 slots, table at `*(0x089DA420)`, 16 bytes each: +52 bank, +56 used, +60 base,
+64 count). base = `100 * u16@0x12` when that is not 0 (plus `10000 * (u16@0x10 - 1)`; 0x10 is 1 in every disc bank),
otherwise `100 * u32@0x14` (if that is >= 600). `FUN_08991928` plays slot `cue - base` of the first registered bank with
`base <= cue < base + count`. So **cue = 100 x bank id + slot**, where the id is u16@0x12 or, for the 012 additions
(id > 65535), u32@0x14. Slot word +0x00: byte 2 bit 0x20 empty (returns 0x100, no sound), byte 2 bit 0x10 link
(play slot +04; unused on the disc), byte 3 bit 0x10 random group (play one of the +08 slots from slot +04, at random),
byte 2 bit 0x01 loop. 125 slots are random groups (snd_common 16, fighters' and voice banks; none in snd_battle); sndbank
calls them kind `link` and decodes the first member; `decodeCue(cue, getFile, {pick})` picks one, `listCues` gives `choices`.

Ranges never overlap (checked by the test), so the id is unambiguous; only banks that are never loaded together share an
id (`variants`: stage two00 / two05, an assist's `_a` / `_w` objx, Chaos's three forms).

## Bank ids

| id (cue base) | slots | file in PACKAGE.BIN -> member | what | battle |
|---|---|---|---|---|
| 100 (10000) | 33 | `general_archive/main/main.bin` -> `snd_common.scd` | resident system SE | always |
| **1000 (100000)** | 146 (96 playable) | `map/maparc_<map>_2.mpk` -> `map_se_<map>-msnd.mpk` -> `snd_battle.scd` | **common battle SE**, byte-identical on all 20 battle stages (wld01-07: `_1.mpk`) | yes |
| 4000 (400000) | 56 | `main.bin` -> `snd_menu.scd` | menu SE | no |
| 5000 / 5100 / 6000 | 68 / 77 / 2 | `sound/snd_region.scd`, `worldmap.bin` (ARC `snd_`), `sound/snd_region_enc.scd` | world map / region | no |
| 9000, 60300-66000 | 0-7 | `general_archive/party_command/<cmd>.bin` -> `snd_ptc_<CMD>.scd` | party-command SE (`snd_ptc_%s.scd` in OVL_BATTLE) | with the command |
| 10000-35000 step 1000 / 500 | | `obj/p_<code>.objx` / `_a.objx` (ARC `snd_`) | fighter / assist bank (sndbank.js) | yes |
| 10100-35100 (+100), 10600.. (+600) | 24-71 | `voice/en/battle/voice_p_<code>_00.scd`, `_a_00.scd` | battle voices (DFF cast; org200 normal/hard/s_hard/s_hard2 = 32100-35100) | yes |
| 40000-41300 | 2-42 | `map/maparc_<map>_2.mpk` -> `map_se_<map>-msnd.mpk` -> `snd_<map>.scd` | stage SE, DFF stages (one00 40000 ... org50 41300) | that stage |
| 50000-52900 | 28 | same msnd mpk (and `_3.mpk` -> `map_se_<map>_rt.mpk`) -> `snd_<material>.scd` | floor materials: stone, bright_stone, rock, sand, wood, grass, steel, wire_net, cloth, magic_floor, orgamic, petrifaction, tiling_red, clear_magic_floor, black_cloud, glass, floating_stone, black_stone, white_stone, water | that stage |
| 70000 (7000000) | 50 | `sound/snd_rt.scd` | unknown use (loaded near `map_se_%s_rt.mpk`) | not traced |
| 210000-219500 / 210100-219600 | | `obj/p_<code>.objx` / `voice/en/battle/voice_p_<code>_00.scd` | 012 cast banks / voices (u32@0x14 ids) | yes |
| 240000-241300 | 3-12 | msnd mpk -> `snd_<map>.scd` | stage SE, 012 stages (ele00, twe00, thn00, vsi00, vni00, vth00, vtw00, wld01-07) | that stage |

Not in the table (no Move Studio use): event-scene banks 100300-139900 (one-slot banks inside unnamed talkevent ARCs and
`obj/z_*.objx`), field-object banks 70100-97000 / 130000-139900 / 230100-277100 (`obj/z_*.objx`), 150000/150100 and the
voice twins at +200 / +700 (e.g. 10200, 10700: same slot counts as the en banks, unnamed, probably the second voice language).

## In battle

The stage files are `map/maparc_%s_{1,2,3}.mpk` (names in EBOOT / OVL_BATTLE; `_1` effects, `_2` battle sound, `_3` `map_se_<map>_rt.mpk`). `FUN_0888E4D4` takes the `snd_battle.scd` member of
`map_se_<map>-msnd.mpk` (inside `_2`) and registers it; the same mpk carries the stage bank, the floor banks (per `map_sound_param.bin`,
"MAPS"), `snd_sebattle_ex.at3` and `snd_sebattle_finish.at3`. `snd_common` is resident (main.bin). Then the fighters' objx
banks and voice banks. So in a normal battle a 0x0303 cue resolves against: snd_common (100), snd_battle (1000), the stage
bank, its floor banks, both fighters' and assists' banks, their voice banks. The EBOOT also names `sound/se_override.bin`
(not on the disc) and `sound/snd_chokobo.scd` (not on the disc).

## Retail use (0x0303 events in the 60 named p_*.objx)

Shared cues are only bank 1000: 100010 x124, 100022 x60, 100012 x45, 100023 x43, 100011 x29, 100078 x23,
100013 x17, 100024 x17, 100096 x1 (359 of the events); everything else is the fighter's own bank. Slot 10 (15 kHz, 0.38 s),
11 (14 kHz, 0.30 s), 12/13/24/25 aliases of 10/11 at other pitches: the short whoosh family.

## Names

None per cue: SSCF has no names, no table in the EBOOT/overlays maps cue -> name, and the VOLD tables only cover voices.
Recoverable: bank names (MPK member names above, the `party_command` file names), and usage counts. `package_paths.txt`
has some wrong names on hash collisions (b8c7a4f4 is `voice/en/battle/voice_p_for100_00.scd`, listed as a png;
a6ead691 "bgm/ids6.at3" is the voice twin 215200); the stage files `map/maparc_<map>_{1,2,3}.mpk` are not in it.

## Format notes / quirks

- Same SSCF as sndbank.js; every one of the 233 banks parses and round-trips with `SndBank.parseBank`/`bankToBytes`.
- `snd_battle.scd` (436580 vs 321888 B) and `snd_menu.scd` (362615 vs 339440) have a stale u32@0x08 size; all clips lie
  inside the real data. `extract` fixes the field on its copy (sndbank rejects the mismatch).
- Chaos's `p_org200_0N.objx` have two `snd_` entries (32000/33000/34000 and 35000); member `snd_#1` = the second.
- sndbank.js issues found (not changed here): `cueBase` returns u32@0x14 raw instead of `100 * u32@0x14`, so cues of the
  012 cast's own banks (Kain 210000 -> 21000000+N) come out wrong; the export `encodeWav` points at `readWav`
  (`writeWav` is the encoder; sharedsnd uses it). Its README's "+03 0x10 link" is the engine's random-group flag (+04 first slot, +08 count).
