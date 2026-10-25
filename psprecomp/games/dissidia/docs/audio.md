# Audio: BGM and voices

Adding music tracks and working with character voice banks.

- [Extra BGM tracks](#extra-bgm-tracks)
- [Dissidia 012 voice banks](#dissidia-012-voice-banks)

## Extra BGM tracks

Data-only: no hooks. Works on the native build and the PSP ISO. Tool: `tools/bgm_extra.py`; list: `<mods>/bgm.txt`.

### Game side
- **Track table** `bgm_entry.bin` (MPK member of `general_archive/main/main.bin`, CRC 93307c1c): "BENT", count u32 at +8,
  then 44-byte entries: +0 s16 id (stored in settings/saves), +2 s16 name index, +4 4 x s16 unlock flags (-1 = none),
  +0xC u8 flags (bit 0/1 = BGM picker, bit 2 = DLC/edat), +0xD s8 volume %, +0xE s8 battle category (non-zero = offered
  in battle), +0x10 u64 stage/series random-pool mask (0 = never picked at random), +0x1C char[16] file in DATA/BGM.
  Retail: 131 entries. FUN_0889EE60 copies it into room for 256 (DLC slots); nothing hard-codes 131.
- **Names** `bgm_name.bin` (member of `general_archive/main/en/main_lang.bin`): plain mess_text table, 132 records;
  records 61 and 62 have start word 0 ("no text"), so the table is appended to, never rebuilt (`append_names`).
- **Playback**: FUN_0889F234 looks the id up and opens `DISC0:/PSP_GAME/USRDIR/DATA/BGM/<FILE>` (upper-cased by the
  game); `sceAtracSetDataAndGetID` picks the codec from the file, streamed through a 98000-byte ring
  (`runtime/tests/test_atrac_stream.cpp` reproduces the game's loop).
- **Lists**: BGM picker MENU FUN_08A55D54 (flags & 3, not DLC, unlocks met, max 128, retail ~67); battle list
  BATTLE FUN_08ADE4E0 (category != 0); random pools BATTLE FUN_08ADEBB4 (mask bit). The Museum player reads
  collection_param.bin table #7 instead (not extended).
- A save that selected a removed extra id finds no entry and plays silence.

### Files
Retail: ATRAC3plus 96 kbps (560-byte frames), RIFF fmt 0xFFFE + fact {samples, first-sample offset} + `smpl`
loop {start, end} in stream samples (first-sample offset included; end inclusive; the loop runs to the file end).
Extras are **ATRAC3 LP2 132 kbps** (open-source atracdenc; its ATRAC3plus is only full-band at 352 kbps), same chunk
layout, first-sample offset 1093 (sceAtrac skips it + 69 -> source sample 0). Encoder: `music/ffxv/tools/make_bgm.py`
(loop finding: `findloop.py` for pieces that repeat, `findjump.py` + a baked crossfade for ones that don't).
Volume: retail battle tracks play at ~-14.3 LUFS after their volume %; pick vol so the extras match.

### Install
- Native: copy the AT3s to `work/disc0/PSP_GAME/USRDIR/DATA/BGM/`, list them in `work/mods/bgm.txt`, run
  `bgm_extra.py work/disc0 work/mods` (idempotent). `roster_mainbin.py` calls it after rewriting main.bin, so
  roster rebuilds keep the tracks.
- ISO: run bgm_extra.py into an empty dir that has only bgm.txt (retail base), then `tools/build_iso.py SRC OUT
  --replace general_archive/main/main.bin=... --replace general_archive/main/en/main_lang.bin=...
  --add-loose /PSP_GAME/USRDIR/DATA/BGM/XV1.AT3=...` (the BGM directory grows to 5 sectors and moves to the image end).

## Dissidia 012 voice banks

`tools/voice_bank.py` dumps, rebuilds and aliases the voice files. Everything here was measured on the EU disc (ULES-01505; the US disc ULUS-10566 has identical data);
`tools/test_voice_bank.py` is a self-test on a synthetic bank (no game data).

### Files a character's voice is made of

| File (en) | What | Codec |
|---|---|---|
| `voice/en/battle/voice_p_<code>_00.scd` | battle bank: short clips (grunts, reactions), 35-70 slots, ~300 KB | SSCF, PlayStation ADPCM (VAG), mono, 14-22 kHz, played by the SAS |
| `voice/en/battle/voice_p_<code>_a_00.scd` | second set (the `_a` form), ~55 KB | same |
| `voice/en/battle/<abbr>_battle_NN.at3` | long streamed battle lines (attack shouts, quotes), 1-3 s each | RIFF ATRAC3, 66 kbps, 44.1 kHz stereo |
| `voice/en/event/<series>/NNN_<code>_NN.at3` | story/event dialogue | same |
| `voice/en/voice_load_parameter.mpk` | MPK of 32 `voice_load_p_<code>.bin` ("VOLD") cue tables (also stored as separate package entries) | - |

### SSCF container (`.scd`, and the type-14 `snd_` entry of every `obj/p_<code>.objx`)

```
0x00 "SSCF", 10 02 07 02 (version)
0x08 u32 total file size        0x0C u32 slot count
0x10 u16 1                      0x12 u16 bank id (fiv100 18100, its _a bank 18600, gst100 30100; 0 in some banks)
0x20 slot table: count x 32 bytes, then the clips back to back in slot order
```

Slot record (32 bytes):

| off | meaning |
|---|---|
| +02 | 0x20 = empty slot (everything else zero apart from the constant fields) |
| +03 | 0x10 = link: +04 holds another slot index, size field is 2, no data |
| +04 u32 / +08 u32 | offset / size of the ADPCM (16-byte aligned, offsets ascending in slot order) |
| +0C u16 | sample rate in Hz (16000, 14000, 22050 ... the SAS pitch comes from it) |
| +0E u8 | volume (0x14..0x64) |
| +10..+17 | constant `64 00 ff 00 ca 5f 00 00` |
| +18.. | per-bank parameter bytes (0xd2 in Bartz, 0x05 in Shantotto, 0xff in the `_a` bank; in objx banks a u16 that varies per clip) |

Slots may share data (two slots with the same offset and size, e.g. the same grunt at two volumes/pitches): `voice_bank.py` calls
them aliases. The ADPCM of a clip is framed: a silent lead block, the data blocks (the last carries flag 1), and a sentinel
block `00 07 77 77 ...` (flag 7 = end now in the SAS). The objx sound-effect banks additionally use loop flags (6 start, 2, 3 end).
`SSCF` banks in this game are all packed in slot order with no gaps except `sound/snd_menu.scd` (not supported).

`dump` -> `build` is byte-identical for Bartz's `_00` and `_a_00`, Shantotto's `_00` and Bartz's objx `snd_` bank (checked 17 Oct).
The decoder is the one in `runtime/src/hle/psp_hle_sas.cpp` (PPSSPP VagDecoder semantics; ffmpeg's `adpcm_psx` differs by a few LSB).

### VOLD cue table (`voice_load_p_<code>.bin`)

`"VOLD" 14 08 10 20, u32 count, u32 0`, then 24-byte records: `char name[16]`, `u32 cue` (low 16 bits sequential, bits 24+ = group 0/1/2),
`u8 kind`, `u8 0`, `u16 index`. Bartz has 80 records (`bu_battle_47` ...), Shantotto 65 (`sh_battle_41` ...); `name` is the streamed
`.at3` of the line (`bu_battle_49.at3` ...), `kind` 0/5/7/8 with a small index and kind 2 (index 1..32) are in-bank/streamed
variants, kind 6 with a large index refers to an event stream. Timeline ops 0x0303 reference cue numbers `1800000 + NN`
(0x1b7768 + ...: `1800050` is `bu_battle_50`, the attack shout of Bartz); 0x186aa-style ids (100010...) are snd_ effects.

### Tool

```
voice_bank.py dump  voice_p_fiv100_00.scd out/ [--vold voice_load_p_fiv100.bin]   # 000.wav/.vag ... manifest.json
voice_bank.py build out/ new.scd                      # edited WAVs are re-encoded, untouched clips keep their bytes
voice_bank.py build wavs/ new.scd --like retail.scd   # wavs named NNN[_name].wav on top of a retail bank
voice_bank.py alias <disc0> gst100 viv100 <mods> [--events]
voice_bank.py at3 line.wav 101_viv100_00.at3          # event line (needs atracdenc)
```

`build` without a manifest needs `--like`: slot count, header and per-slot parameter bytes come from it, slots without a WAV keep
`like`'s clip (`--fill empty` clears them). WAVs at any PCM rate are accepted (stored at their own rate, capped to 22050 by resampling;
`--rate` forces one); stereo is mixed down. ADPCM encoding is pure Python (per-block filter/shift search, ~60 dB on tones, ~20 dB on noise).
`--bank-id` sets the header id (a new character that shares a bank with its `like` probably wants its own).

#### External encoder (event lines and nothing else)

ffmpeg decodes ATRAC3 but cannot encode it. `at3` calls `atracdenc -e atrac3_lp4 --container riff` (github.com/dcherednik/atracdenc, needs
libsndfile and its `libgha` submodule: `git clone --recursive`, `cmake ../src`); its output headers match the retail event files
(fmt 0x270, 192-byte frames, fact chunk). Alternatively feed a pre-encoded `.at3` made with Sony's `at3tool -e -br 66`.

### Runtime verification (17 Oct)

- Method: `PSPRECOMP_AUDIO_DUMP` (mixed output to WAV) and `PSPRECOMP_SAS_TRACE` (`[SAS] SetVoice ... size=N`). The HLE's `SetVoice`
  size is the slot blob length minus 0x20, so a bank slot can be identified by size.
- Banks of 64 distinct tones were built for several codes and dropped in the mods directory. In a Quick Battle the tones were audible
  in the audio dump, and the `SetVoice` sizes matched the tone-bank slots exactly. A modified bank is therefore read, parsed and played
  by the unmodified runtime.
- Limitation, not fixed: an extra character (slot >= 33, e.g. `viv100`) does not request its own `voice_p_<code>_00.scd`. `roster.cpp`
  (`voiceslot_hook`, the code-string-table patch, the charinfo clone that copies `like`'s record including the +0x74 code) makes the
  extra use `like`'s voice slot and bank, and `voice_load_p_<code>` has no entry for a new code. A replacement bank only takes effect
  for an extra if it is installed under `like`'s own file name, which changes that character too. Giving extras their own bank needs a
  runtime change (redirect the scd read to the extra's file and supply a VOLD entry through a repacked `voice_load_parameter.mpk`).
  The second check on Vivi (`like` = Shantotto in the test setup) did not match retail gst100 slot sizes, so which retail bank an
  extra ends up using was not pinned down; treat this as unconfirmed.

### Slot roles (statistical, not per-action)

Slots 0-1 are the most frequently played short grunts (hit/damage); 2-9 are reaction clips; 10-37 are voiced lines, often longer
and at 14 kHz. VOLD kind-2 indices 1..32 correspond to in-bank slots, and cue `18000NN` corresponds to the streamed `<abbr>_battle_NN`.
No per-action attribution was made.
