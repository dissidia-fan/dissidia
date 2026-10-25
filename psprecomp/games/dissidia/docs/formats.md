# File formats

Disc layout, the package archive, per-character files, effects and weapons. Model/GMO details for new characters are in [roster.md](roster.md); timelines and events in [moves-and-events.md](moves-and-events.md).

- [Dissidia 012 (EU PSN ISO): archive format and character notes](#dissidia-012-eu-psn-iso-archive-format-and-character-notes)
- [Dissidia 012: character effects (effe) format notes](#dissidia-012-character-effects-effe-format-notes)
- [Dissidia 012: weapon export for Blender](#dissidia-012-weapon-export-for-blender)

## Dissidia 012 (EU PSN ISO): archive format and character notes

> Early notes (15–16 Oct, from the first look at the EU disc). Later sections and docs supersede them where they disagree.

Source: "Dissidia 012 - Duodecim Final Fantasy (Europe) (PSN).iso" (1,754,529,792 bytes). Tool: `tools/dissidia_pkg.py` in the local dissidia folder.

### ISO layout
- `PSP_GAME/SYSDIR/EBOOT.BIN`: encrypted (~PSP header). BOOT.BIN is blank.
- `PSP_GAME/USRDIR/DATA/OVL_BATTLE_APP.ELF`, `OVL_MENU_APP.ELF`, `OVL_EXTRA_APP.ELF`: plain MIPS ELF overlays, load vaddr 0x089DD4C0, file offset 0x54.
- `USRDIR/DATA/PACKAGE.BIN` (ISO LBA 186624, 1,034,358,784 bytes): all game data.
- `USRDIR/DATA/PACKAGE_INFO.BIN` (LBA 26000, 132,532 bytes): the index.

### PACKAGE_INFO.BIN
- Header: 16 bytes. 'pack' at 0x04, u32 count (11043) at 0x08.
- Entries: count × 12 bytes, sorted by hash: `u32 hash, u32 sector (2048-byte units into PACKAGE.BIN), u32 (size & 0x0FFFFFFF) | (flag << 28)`.
- Hash: BZip2 CRC-32 (poly 0x04C11DB7, non-reflected, init/xorout 0xFFFFFFFF) of the lowercase path. The EU build uses `/EN/` where the strings in the code say `/JP/`.
- Files are stored raw, with no compression or encryption.

### File formats seen
GMO (`OMG.00.1PSP`), GIM (`MIG.00.1`), ATRAC3 RIFF, `ARC\x01` containers (16-byte entries: u32 type, 4-char tag, u32 offset, u32 size), `MPK ` (effects), `SSCF` (sound), `mlng` (text), `menu pak`, `mess pak`, `MAGI`.

### Per-character files
- `obj/p_<code>.objx`: an ARC bundle. It holds anim/skeleton (p_xx types 5–9), `effe` MPK, `snd_` SSCF, `plya`/`comb`/`atkp`/`magi`/`brat` (moveset/params), `text`, `wind` GIM, `spec` mlng, and `d_xx`.
- `obj/p_<code>_a.objx`: the assist bundle. `obj/p_<code>_w.objx`: weapons.
- Models: `obj/p_<code>_{1p,2p,3p,zako}.gmo` (normal/alt1/alt2/manikin), with `_a` for assist. Each has a matching `.cosx` (68 B) and `.exex` (EX aura).
- Codes: one100 WoL, one200 Garland, two100 Firion, two200 Emperor, thr100 OK, thr200 CoD, for100 Cecil, for110 Kain, for200 Golbez, fiv100 Bartz, fiv200 Exdeath, fiv210 Gilgamesh, six100 Terra, six200 Kefka, sev100 Cloud, sev110 Tifa, sev120 Aerith, sev200 Sephiroth, eht100 Squall, eht110 Laguna, eht200 Ultimecia, nin100 Zidane, nin200 Kuja, ten100 Tidus, ten110 Yuna, ten200 Jecht, gst100 Shantotto, gst110 Prishe, gst120 Vaan, gst130 Lightning, gst200 Gabranth, org200 Chaos, org210 Feral Chaos.

### Roster tables (hardcoded)
A 33-entry pointer array of code strings in the order one100…org210, sev120. Locations:
- OVL_BATTLE_APP.ELF file offset 0x11E800
- OVL_MENU_APP.ELF offsets 0xCF58, 0xF100, and 0x17AB50 (the last has 32 entries, without sev120)

Each array is followed directly by other data, so there's no room to append in place.

### Object table
- Located in the PACKAGE file at sector 4 (hash 0x93307c1c, 1,218,448 B), at file offset 0xA0A8. It has 1172 entries of 0x44 bytes, in the format of DissDlcToolkit's ObjectEntry: u16 id, u16, u16, u8 charId, u8 costume, u8 type (0 player / 8 assist / 5 manikin / 13 manikin assist), u8 slot, 18 B model data, 4×u16 attachment ids, char[16] modelName, char[16] objxName.
- charId = roster index + 1 (Chaos = 23).
- **charId 33 = Aerith.** Player entries `P_SEV120_1P/2P/3P → p_sev120` exist, but `obj/p_sev120.objx` and the player GMOs are missing. The cosx files exist. This is a cut, partly wired playable slot.
- charId 34 = `P_ORG800_1P`: a GMO and cosx exist, but there's no objx.

### DLC route
DissDlcToolkit (github.com/adriangl/DissDlcToolkit) builds costume DLC: 9 DLC costume slots per character and 255 player/assist object slots. Its files are `dlc/obj/dlc_NNNoe.bin` plus the GMO/GIM, packaged as .edat.

## Dissidia 012: character effects (effe) format notes

> Early notes (15–16 Oct, from the first look at the EU disc). Later sections and docs supersede them where they disagree.

Source: Bartz `obj/p_fiv100.objx` → ARC entry `effe` (type 4, 1,093,520 B).

### Container
- `MPK ` + `06 06 07 20`, u32 count (3), then 16-byte entries `{u32 id, u32 offset, u32 size, 0}`:
  - id 0x40 **FEP** (1,031,296 B): the effect package.
  - id 0x4D **FEPA** (33 KB): effect attachment points (contains the model name `p_fiv100_1p` and bone names such as `root`).
  - id 0x5A **FXSE** (29 KB): per-effect table with 90 entries (likely sound links).

### FEP
- Header: `FEP\xcd`, u16 1, u16 3, u32 0x48, … u16 counts[6] at 0x1C = (90, 90, 30, 119, 38, 1).
- Six u32 offset tables run back to back from 0x50, one per count.
  - Tables 0/1 (90 each) are the effects themselves. 90 matches the effect index used by event 0x0219 (e.g. #0x36, #0x3C, #0x3D).
  - Tables 2–4 are 20-byte resource refs `{f32 1.0, u32 tag (…f0 / …98 ids), u32, u32 offset, -1}`.
- Resource blocks are tagged by ASCII magic:
  - **ANM** ×30: effect animation curves (colour/scale over time, RGBA keys such as 80808080).
  - **TEX** ×16: textures.
  - **MDL** ×112: particle models/emitters.
- **TEX** layout:
  - 48-byte header `TEX\0, u32 id (0x9800ccNN), 1, 0, u32 fmt (4), 1, u32 blockSize (header+pixels), u32 w, u32 h, u32 0x30`.
  - Then w×h 8-bit indices, **swizzled** (16-byte × 8-row tiles).
  - At offset blockSize: `u32 palCount (256), 0, 8 pad bytes`, then palCount × RGBA8888.
  - Palettes are mostly grey/white with an alpha ramp (tinted at runtime by ANM colours); some are coloured.
  - Bartz has 16 textures: magic circle, flame, pink lightning, smoke, explosion flipbook, flares, slash arcs, a blue crystal diamond (#11, id 0x19), sparkle spray.

### Events that use effects
- **0x0219** `{effectIndex, frames}`: plays effect #N from the character's own FEP. Bartz fires it on weapon swaps, e.g. (0x3C, 30).
- **0x0315** `{globalId, 0, 0}`: game-wide effect id (e.g. 0x1B9E6B).
- **0x011B / 0x011C**: show / hide-or-stow weapon N for a frame range (arg = draw-bone index).

### Custom effect plan (Noctis Armiger)
1. Prototype: recolour or replace the texture of Bartz's weapon-swap effect in place. Same size texture, so this is a pure byte patch. Then recolour its ANM colour keys.
2. Then clone an effect entry into a new index (table rebuild) so Noctis keeps the original effects too.
3. Event side: show weapon (0x011B) plus play effect (0x0219, new index) on the summon frame.
- Still unknown: the effect → MDL → TEX/ANM linkage inside table entries, and the MDL emitter parameters.

## Dissidia 012: weapon export for Blender

> Early notes (15–16 Oct, from the first look at the EU disc). Later sections and docs supersede them where they disagree.

- Tool: `tools/export_weapons.py <dfx dir> <gmo> <outdir>` (local dissidia folder). Writes Y-up .glb with embedded textures.
- Bartz export: `work/bartz_weapons/` (from `obj/p_fiv100_1p.gmo`).
  - `weapons/wepN_<bone>.glb`: origin = holding bone (grip), in that bone's axes. Second-bone pieces (`*_acc`) are child objects with their origin on their own bone.
  - `Bartz_weapons_reference.glb`: body in bind pose plus every weapon on its bone.
  - `weapons.json`: bone, hand, tris, size, textures.
- Bartz weapon parts: wep0–7 right hand (r_wep…r_wep6; wep5 and wep6 share r_wep5), wep10–15 left hand (l_wep…l_wep5; wep10 is a shield), wep18 = throwing stars on the top-level `star` bone. wep16/17 are cape variants. There is no wep8/9.
- Facts:
  - Vertex positions in the GMO are already in bind-world space. Bone-local = BlendOffset^T @ v.
  - Weapon BlendOffsets have no scale.
  - Weapons point along +Z in bind pose; swords are about 1–2.5 units long against a body height of 1.79.
  - Originals are 34–122 tris.
- `p_fiv100_1p_w.gmo` holds alternative textures (`p_fiv100_as_*`). Only wep3/12/15 and the cape are real meshes there; the rest are stubs.
