# Spring bones (hair, cloth, cape physics)

How the game swings capes, hair, skirts and weapon chains, where the data lives, and how to give a mod character
(Ramza, `mods/ramza`, built on gst120 Vaan) a physics cape. Read-only dump tool: `tools/spring_dump.py`.

**Confirmed** = traced in the recompiled code (`psprecomp/work/output/generated/batch_*.cpp`) or checked against
every retail file. **Guess** = inferred from how a value is used or from retail values; not proven.

## 1. Where the data is

- **ARC entry type 6** (tag `p_xx`, e.g. `p_gs`) of `obj/p_<code>.objx`. The assist / alt files
  (`p_<code>_a.objx`, `p_<code>_w.objx`) carry their own copy. **Confirmed**:
  - `FUN_088459E0` (character resource load) looks up ARC entries by type with `FUN_0885CA3C(arc, type)` and stores
    copies in the resource: type 5 -> +0x18, type 7 -> +0x1C, type 8 -> +0x20, **type 6 -> +0x24**.
  - The body constructor `FUN_0883975C` (0x08839B64..0x08839C3C): `actor+0x390` = that resource;
    allocates the spring object (0x40 bytes), `FUN_08810414(obj+0x18, *(res+0x24))`, stores it at **actor+0x398**,
    then `FUN_0880D82C(obj, actor)` builds the per-bone runtime entries.
- Nothing comes from the GMO except the bone transforms and the **bone length** (below).
- In the mod pipeline it is the file `moveset/p_<code>/sections/NN_p_xx_6.bin` (charmod_objx keeps unknown sections
  as raw .bin files; `arc.json` names them).
- Bone length: the GMO bone's **BlindData chunk 0x80F1** named `DISTANCE` (payload `"DISTANCE\0\0\0\0"` + f32).
  `FUN_0886DFC0` reads it per bone into the float table at `*(actor+0x394)+0` (index = bone number); `FUN_0880D82C`
  copies it to entry+0x160 (0 if the model has no table). **Confirmed** (code path + values: Garland `b_mant3`
  DISTANCE 0.5716 = `b_mant2`'s Translate x). The simulated point is the bone's tip = (length, 0, 0) in the bone's
  frame, so **game bones point along local +X**. Retail chains: each child sits at (parent DISTANCE, 0, 0).
- Vaan (gst120) and Feral Chaos (org210) have an **empty** set (36-byte header only). Ramza's
  `sections/03_p_gs_6.bin` is that empty set.

## 2. File layout (all little-endian)

Header, 0x24 bytes (**confirmed**, `FUN_08810414` and every retail file):

| off | type | field |
|---|---|---|
| 0x00 | u32 | version, always 2 |
| 0x04 / 0x08 | u32 / u32 | sphere count / offset (20-byte records) |
| 0x0C / 0x10 | u32 / u32 | parameter-set count / offset (0x1C-byte records) |
| 0x14 / 0x18 | u32 / u32 | link-parameter count / offset (8-byte records) |
| 0x1C / 0x20 | u32 / u32 | spring-bone count / offset (0x40-byte records) |

Offsets are from the start of the section; retail packs the tables back to back in this order. The counts of
the parameter sets and link params are not stored at runtime (indices are not range-checked).

Runtime object at actor+0x398: +0x18 sphere count, +0x1C spheres, +0x20 bone count, +0x24 bone records,
+0x28 parameter sets, +0x2C link params, +0x30 sphere work (16 B each), +0x34 entries (0x170 B each),
+0x38 per-group state (24 B each, +0x14 = 1.0), +0x3E group count = highest param-set index + 1 (s8).

### Spring-bone record (0x40 bytes) = `*(entry+0x158)`

| off | type | meaning | status |
|---|---|---|---|
| 0x00 | s16 | GMO bone index (the bone that swings) | confirmed (`FUN_089B0E78`; event op 0x0227 arg) |
| 0x02 | s16 | collision group A: collides with spheres whose group == this (0 = none) | confirmed (`FUN_0880D370`) |
| 0x04 | s16 | collision group B (second group, 0 = none) | confirmed |
| 0x06 | s16 | parameter-set index (also the "physics group" of event ops 0x0216 / 0x0117) | confirmed |
| 0x08 / 0x0C | f32 | angle limit 1, min / max (radians) | confirmed read in `FUN_0880C838`; axis = guess (swing out/back from the body) |
| 0x10 / 0x14 | f32 | angle limit 2, min / max (radians; sideways = guess) | same |
| 0x18 | u8 | floor clamp on: tip kept above actor+0x84 + [0x1C] (only in some modes, a global flag gates it) | read confirmed, meaning guess |
| 0x19 | u8 | gravity taken through the entry's frame matrix (entry+0xD0) instead of the plain world vector; set on almost all cloth/hair, 0 on weapon chains | read confirmed, meaning guess |
| 0x1A | u8 | alternative gravity frame (`FUN_08868128`); never set in retail | read confirmed |
| 0x1B | u8 | ignore the group force/wind (obj+0x00 vector x group strength); set only twice in retail | confirmed read, "wind" = guess |
| 0x1C | f32 | floor offset for 0x18 | guess |
| 0x20 | 4 x {s16 bone, u8 at_tip, u8 link_param} | distance links to other bones (cloth width). `bone` -1 = empty. at_tip 1 = the linked bone's tip, 0 = its joint. `link_param` indexes the 8-byte table | confirmed (`FUN_0880D82C` loop of 4, `FUN_0880C5A4`) |
| 0x30 | 4 x {-1, 0} | never read (the loop stops at 4); keep `ffff0000` x4 | confirmed |

The limits are active only if any of the four values is inside (-pi, pi) (entry+0x152 flag, set in `FUN_0880D82C`).
Retail uses +-180 deg (or +-360) to mean "no limit".

### Parameter set (0x1C bytes) = `*(entry+0x15C)`

| off | type | meaning | status |
|---|---|---|---|
| 0x00 | f32 | stiffness: pull of the tip back toward its animated rest position each tick (x dist x 0.5 x time scale; 0 = pure pendulum) | confirmed (`FUN_0880C430`) |
| 0x04 | f32 | pull clamp used with 0x00 (retail 0 except 0.03 twice) | read confirmed, exact role unclear |
| 0x08 / 0x0C / 0x10 | f32 x3 | per-axis drag on the tip's velocity in the bone frame, quadratic and clamped to 0..1 (retail 0..1, mostly 0.1) | read confirmed (0x0880FA34), meaning guess |
| 0x14 | f32 | push-out / friction factor when a collision sphere is hit (retail 0 except twice) | read in `FUN_0880D370`, meaning guess |
| 0x18 | f32 | never read; 0 in retail | confirmed unused by the physics code |

Gravity is built in: velocity += 9.8 x dt, position += 4.9 x dt^2 (constants 0x411CCCCD / 0x409CCCCD in
`FUN_0880EA20`). The tip is then renormalised to the bone length every step, so a chain
behaves like rigid rods.

### Collision sphere (20 bytes)

`s16 group, s16 bone, f32 x, y, z (offset in the bone's frame, x = along the bone), f32 radius`. **Confirmed**
(`FUN_0880D370` compares `group` to record +0x02 / +0x04 and the radius at +0x10 to the tip distance). Spring bones
whose group fields are 0 collide with nothing.

### Link parameter (8 bytes)

`f32 a, f32 b`: the spring rate of a distance link; `FUN_0880C5A4` uses `b` or `a` depending on whether the link is
stretched or compressed (which is which = guess), x 0.5 x time scale, capped. Retail: 0.03 .. 0.5.

### Order and per-frame flow (`FUN_0880EA20`, called from `FUN_08B30814` for battle actors)

For each entry in file order: pinned by an event (entry+0x150 = 0, ops 0x0227 / 0x0216) -> follow the animation;
else gravity + velocity, group force, stiffness pull, links, sphere collision, angle limits, floor, write the bone's
model-space matrix. Retail lists the back (centre) chain first, then side chains linking to it, parents before
children. Keep that order. The bone just above a chain (`*_c` "anchor" bone) is not in the list; it is animated
and carries the chain.

## 3. Retail examples

Dump any of them: `python3 tools/spring_dump.py p_for100.objx p_for100_1p.gmo` (GMO adds names and parents).

**Cecil (for100), simplest full cape.** 3 chains x 3 physics bones, each under a non-physics anchor:
`b_mant1c_c -> b_mant1c -> b_mant1b -> b_mant1a` (bones 20..23), `l_mant1c_c -> l_mant1c..a` (26..29),
`r_mant1c_c -> r_mant1c..a` (42..45). All set 0 = {stiffness 0, drag 0.1 x3}. Top bone limits [-30..120] [-180..180],
lower bones unlimited. Groups [1, 0] against two spheres: chest (17, [0.09, 0.12, 0], r 0.27) and hip
(3, [0.214, 0, 0], r 0.2). Side bones link to the same-level back bone's tip (link param 0 = [0.1, 0.0]).
Plus hair (`b_hair`, set 1) and three weapon tassels (set 2, drag 0).

**Garland (one200)** 14 bones: back `b_mant2, b_mant1`, side mantles `l/r_mant2c..a` and `l/r_mant1b..a` linked
crosswise, a stiff hip `protector` (set 1, limits [0..90] [-30..30]) and a weapon chain. Spheres chest r 0.4,
femurs r 0.3. Drag [0.1, 0.2, 0.1].

**Golbez (for200)** 10 bones, 3 x {mant3b/2b/1b, ...a}: top bones stiffness 0.05, lower set 1 stiffness 0;
the back top bone collides with groups [1, 2] (hip + right thigh), its hem bone with group 3 (right shin).

**Exdeath (fiv200)** 3 x 3 cape (`b/l/r_mant1c..a`): stiffness 0.1 / 0.05 / 0 from top to hem, hem bones have the
floor flag; 6 body spheres r 0.35..0.5.

**Bartz (fiv100)** short 2 x 2 cape `l_mant1b/a`, `r_mant1b/a` (r chain links to l chain), plus six weapon `*_acc`
tassels; 4 small spheres (chest r 0.2, hip 0.18, femurs 0.2).

**Warrior of Light (one100)** 2 x 3 cape (`l/r_mant1c..a`, floor flag on the lower bones), four skirt flaps, four
hip covers (stiffness 0.05, pull clamp 0.03, limits [0..90] [-30..30]), hair, weapon chain.

**Gabranth (gst200)** 3 x 3 cape like Cecil's but only the top bones use the gravity-frame flag; hem bones collide
with a foot sphere (group 2). **Lightning (gst130)** one 4-bone cape `mant_1d..a`, floor flag on all.
**Firion (two100)** 2 x 3 cape, side limits [0..120/140] [-70..70], link params 0.03 / 0.1. **Cloud (sev100)**
skirt panels with stiffness 0.3, drag 0.4 (springy). **Kain (for110)** hair, short cape, tail, turban: 20 spheres
in 5 groups.

Characters with no or almost no physics: Vaan gst120 (none), Feral Chaos org210 (none),
Shantotto gst100 (2 bones, no spheres), Squall eht100 (2 cloth + chain), Tidus ten100 / Ultimecia eht200 (3 bones each, no spheres).

## 4. Mod pipeline

- **Carried through as is.** `charmod_objx.unpack` writes type 6 to `sections/NN_p_xx_6.bin`, `pack` writes it back
  byte for byte. Replacing that file is all the moveset side needs. The Blender add-on (0.30+, Tools > *Add cape bones*, `cape.py` + `dfx/spring.py`) writes it when a character with cape bones is saved to its folder; `spring_dump.py` only reads.
- The based-on character's set comes along unchanged. A mod built on a caped character keeps the cape set; it
  must keep those bone indices (the add-on already never removes or renumbers bones).
- **Adding bones** (Blender add-on: appended after the existing ones, so indices of existing bones stay valid;
  `exporter._bone_chunk` writes ParentBone / Translate / Rotate to both the mesh and the animation model):
  - the new indices are `52, 53, ...` for Ramza (Vaan has 52 bones, 0..51); put them in the records' +0x00;
  - every physics bone needs a **`DISTANCE` BlindData chunk** (0x80F1, in the mesh model's bone; retail has none in
    the animation model); without it the length is 0 and the chain cannot swing. The exporter (0.30+) writes it
    from the bone property `dfx_distance` (the importer sets that from the file, Add cape bones on its new bones);
  - local +X must point down the cape (child Translate = (length, 0, 0));
  - all costume GMOs (`_1p/_2p/_3p`) of the character must have the same bones, since they share the objx;
  - motions don't need keys for the new bones (unkeyed bones stay at the rest pose and the physics takes over).
- **Limits**: none fixed. Entries are allocated per count (0x170 B each); param-set index is stored in an s8
  group count (keep < 127); 4 links per bone; `s16` bone indices. Retail goes up to 20 spheres and 16 bones.
  The skinned mesh must still fit the GE limit of 8 bones per draw subset (the exporter batches triangles by
  `MAX_SUBSET`); a 3 x 3 cape plus chest/hip adds bones to whatever subsets the cape triangles land in.
- The assist file (`p_<code>_a.objx`) is not built for mods; only `p_<code>.objx` matters for the playable body.
- Event ops that address spring bones: 0x0227 `pin_physics_bone {bone, deg/tick}`, 0x0216 (pin a whole param-set
  group), 0x0117 ranged (rest on animated pose). EX Burst / intro motions of caped retail characters use them to keep
  the cape on its keyframes; Vaan's motions have none.

## 5. Recipe: a physics cape for Ramza (not yet tested in game)

1. **Bones** (Blender, armature of `p_ram100_1p.gmo`, parents first; all game bones point along local +X):
   - `b_mant1c_c` (parent `chest`, 14), at the upper back between the shoulder blades, +X pointing down;
   - `b_mant1c` -> `b_mant1b` -> `b_mant1a` along the back centre, each about 1/3 of the cape length;
   - `l_mant1c_c` / `r_mant1c_c` (parent `chest`) at the back of each shoulder, then `l_mant1c..a`, `r_mant1c..a`.
   That is 12 new bones, indices 52..63 in export order. Check the order in the exported GMO
   (`spring_dump.py` with the GMO, or `charmod_gmo.Model(...).bone_names()`) before writing the records.
2. **DISTANCE**: each new bone gets BlindData `DISTANCE` = distance to its child; the `*_mant1a` hem bones get the
   distance from their joint to the cape hem (retail hem bones: 0.3..1.1). Add it to the exporter (see 4) or patch
   the GMO after export with `charmod_chunks`.
3. **Weights**: cape vertices weighted only to the 9 physics bones (top row mostly `*_mant1c`, middle `*_mant1b`,
   hem `*_mant1a`, blend 2 bones across each row boundary and across the l/b/r seams); the cape's top edge can share
   weight with `chest` so it stays attached. Keep the anchors (`*_c`) unweighted or lightly weighted with `chest`.
4. **Records**: copy Cecil's (for100) set with Ramza's indices, in this order: `b_mant1c, b_mant1b, b_mant1a,
   l_mant1c..a, r_mant1c..a`.
   - Param set 0: stiffness 0, clamp 0, drag [0.1, 0.1, 0.1], 0, 0. For a heavier, less floaty cape use Exdeath's
     ladder (top 0.1, middle 0.05, hem 0 in three sets).
   - Top bones: limits [-30 deg .. 120 deg] [-180 .. 180]; middle and hem: [-180 .. 180] x2. Groups [1, 0].
     Flags `0x18 = 0, 0x19 = 1, 0x1A = 0, 0x1B = 0`, floor 0.
   - Links: each `l_` / `r_` bone -> the same-level `b_` bone, at_tip 1, link param 0. Link param 0 = [0.1, 0.1]
     (Cecil uses [0.1, 0.0]).
   - Spheres (group 1): chest 14 offset [0.09, 0.12, 0] r 0.27; hip 3 [0.214, 0, 0] r 0.2; for a long cape also the
     femurs 4 and 8 at [0.17, 0, 0] r 0.2 (Bartz). Ramza's proportions are close to Vaan's; scale radii to his
     chest width if the cape clips or floats.
5. Write the 36-byte header + tables (layout in 2) to `mods/ramza/moveset/p_ram100/sections/03_p_gs_6.bin`, check it
   with `spring_dump.py`, build, and watch idle / dash / air dodge. If the cape flips forward, tighten the top limit;
   if it pops in EX Burst or intro, add 0x0216 `{0, 180}` over those motions.
