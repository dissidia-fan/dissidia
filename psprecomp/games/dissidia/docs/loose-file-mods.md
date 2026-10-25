# Loose-file mods

How the runtime replaces files of `PACKAGE.BIN` with loose files, the model size budget, model file checks, and the package index internals. Character mod folders (the normal way to add a character) are described in [../mods/README.md](../../../../mods/README.md); this is what happens underneath.

## How overrides work

Verified in game (16 Oct): a `mods/obj/p_gst130_1p.gmo` (Lightning's 1P model, `gst130`) with scrambled textures replaced the packaged one in the first battle. Character codes seen in the binaries: `one/two/thr/for/fiv/six/sev/eht/nin/ten` + `100/110/120/200/210` (FF number + variant), `gst100..130` for the 012 newcomers (`gst130` = Lightning), `org200/210` for Chaos.

Drop files in a mods directory and they replace the matching entry of `DATA/PACKAGE.BIN`; no ISO rebuild.
The directory is `PSPRECOMP_MODS`, default `<ms0's parent>/mods` (`<workdir>/mods` with `run.sh`). Nothing
changes (and nothing is intercepted) when it is missing or empty.

```
mods/obj/p_fiv100_1p.gmo          replaces that asset (path is case-insensitive, '/' or '\\')
mods/obj/p_fiv100.objx
mods/menu/jp/battle/chara_image/p_fiv100.gim
mods/by_crc/0a1b2c3d.bin          replaces the entry with that CRC32 (hex) - for assets not in the path list
```

**Blender add-on:** `blender/` holds the Dissidia character add-on (import from the ISO, model, animate, edit move events, export); `python3 blender/build_addon.py` builds the installable zip. Guide: [blender/README.md](../../../../blender/README.md).

**Move Studio:** `studio/index.html` (open in Chrome): one place for a character's moves: the animation in 3D, its events on a timeline, each hit's damage and knock-back, saved straight into the mod folder. Guide: [studio/README.md](../../../../studio/README.md). What every event does: [docs/moves-and-events.md](moves-and-events.md).

**Character mod folders:** each extra character lives in `mods/<name>/` (moves, hits, avatars, body as editable files; guide in [mods/README.md](../../../../mods/README.md)). `tools/run.sh` builds every folder there into the mods directory before the game starts, and adds a new one to the roster (`PSPRECOMP_CHARMOD=0` skips it; by hand: `tools/charmod.py build`).

New characters: `tools/new_character.py --work <workdir> --mods <mods> --slot 35 --code viv100 --like eht110 --name "Vivi" --series "FINAL FANTASY IX"
--logo logo.png --portrait face.png --slidein render.png [--model body.gmo --objx moves.objx]` writes the roster line and every file a roster slot needs
into the mods directory (`--dry-run` to preview, re-run to update, `--remove` to take it out); slots 35..42 need `PSPRECOMP_ROSTER_EXPERIMENTAL=1`
(`docs/roster.md`, "new_character.py").

The file may be any size, with one caveat for models: the game stages each model file (`obj/*.gmo`, `*.objx`)
in a fixed 2,506,752 B (0x264000) block, so a retail-sized limit applies unless the budget is raised. The runtime
raises it by itself to the largest `obj/` override (rounded up to 4 KiB, at most 11 MiB) and logs
`[MODS] ... exceeds the retail model budget ...: staging budget raised to N B`; `PSPRECOMP_MODEL_BUDGET=<bytes>`
sets it explicitly. A file beyond the 11 MiB ceiling logs `[MODS] warning: ... exceeds the game's model budget (N B)`.
Details in `docs/roster.md` ("Model size budget"). Watch the log for `[MODS] override ...` lines; `PSPRECOMP_MODS_TRACE=<n>`
logs the first n package reads that were served from loose files.

Model files (`obj/*.gmo`): the game's loader takes the address of each vertex stream from two words in the
*chunk header* of every Arrays block, not from the array payload: chunk `+8` must be the chunk size and chunk `+12`
the offset of the first vertex from the chunk start (header size + 16, the 16 bytes being the array's own
`vtype/count/morphs/flags` header). Retail files always have them; a writer that puts the generic block header
(both words = header size) there makes every vertex stream start 16 bytes early, which renders as a blow-up of
huge triangles for 6- and 8-weight vertices (strides 18 and 20) and as subtly wrong triangles for 16-byte
vertices. The number of weights per vertex or per BlendSubset is not limited (verified in game with 4, 6 and 8, 8
bones per BlendSubset, merged parts and 1,766-index draws; see `tools/synth_weight_variants.py`).
`tools/gmo_arrays_check.py FILE.gmo [--fix OUT.gmo]` lists and repairs the headers.

Extra draw bones start visible. A modded character with extra drawn parts (the warp-dash ghost copies of the body,
a hovering sword, trail parts) shows them permanently, and ghost copies with their bright white-blue palettes
drawn over the real body make it look flat pale blue. The battle code keeps a per-character visibility table
(3-byte `{draw bone, state, arg}` entries, one slot per Part, at controller `+0x10c`, count at `+0x8e`); a draw bone
that is not in it is drawn unconditionally. It is pre-filled at battle start from the 18 bytes of the current
`atkp` record (retail lists wep0..wep15, wep17, wep18 only) and a bone is otherwise registered when an event
0x011B (show) or 0x011C (hide) first runs for it. `tools/objx_hide_bones.py IN.objx OUT.objx --gmo MODEL.gmo
[--skip-motions 16-19]` adds a whole-motion ranged 0x011C for each extra draw bone to every timeline that does not
already show or hide it (self-test: `python3 -I tools/test_objx_hide_bones.py`). Verified in game with the Noctis
GMO: dark outfit, no ghosts, no hovering sword. The warp-dash motions' own show/re-hide were not exercised.

How it works. `PACKAGE_INFO.BIN` is `{u32 0x20090218, "pack", u32 count, u32 0}` followed by `count` 12-byte
entries `{u32 crc, u32 sector, u32 size_flags}`, sorted by `crc`: the BZip2-style CRC32 (polynomial
`0x04C11DB7`, MSB first, init and final xor `0xFFFFFFFF`) of the lowercase asset path; `sector` is
the byte offset in `PACKAGE.BIN` divided by 2048; `size_flags` is `flags << 28 | byte size`. The entries tile
`PACKAGE.BIN` with no gaps. The game reads the whole index into memory at boot and then seeks to
`sector * 2048` and reads exact sizes. The game module intercepts reads of both files through the
`PspIoPolicy::classify_open` / `overlay_read` seam (`runtime/include/hle/psp_hle_io.h`): the index the game
sees has the new size patched in, and a read inside an overridden entry is served from the loose file
(zero-padded to the sector). A replacement that needs more sectors than the original is moved to virtual
sectors past the end of the real `PACKAGE.BIN`, so it never overlaps a neighbour. The `flags` nibble is kept.

A file whose path hashes to no existing entry is appended to the index as a new entry. It is only reachable if
the game asks for that path (every file `tools/new_character.py` writes is one the engine requests; verified in
game with eight scaffolded slots, see `docs/roster.md` "Package index capacity"). The game reads the index into a fixed
`0x20F8C`-byte buffer, which held about 210 new entries; `hooks_index.cpp` lifts that: once the patched index no
longer fits, the loader reads it into a host-heap block of `0x26400` bytes (13,054 entries: the 11,043 retail ones
plus up to 2,000 new ones; the boot log prints `[MODS] package index capacity ...`) and the four functions that
touch the buffer use that block. Retail and small mod sets keep the original buffer and code path. More than 2,000
new entries are dropped with a warning (first ones kept). `PSPRECOMP_INDEX_EXTENDED=0` goes back to the old
~210 ceiling, `PSPRECOMP_INDEX_RELOCATE=1` relocates even when the index fits (test aid).

`package_paths.txt` lists the 8474 asset paths (of 11043 entries, 76.7%) whose CRC matches the EU index. They
came from strings in the EBOOT, the overlays and the package contents, expanded with the game's own path
templates (`obj/%s.gmo`, `voice/en/event/...`, ...). It is documentation only: overrides hash your path, so
a path missing from the list works the same. Unnamed entries are mostly `.objx`/`.arc` containers (ARC),
`.gim` textures and a few hundred `.at3`.

Tests (host, no game run, need your extracted disc: `PSPRECOMP_DISC0`):
`games/dissidia/tests/run_package_mods_test.sh` (index parsing, CRC, ranges, relocation, and the glue against the real `PACKAGE.BIN`).
