> Archived investigation (16 Oct). Kept for the record; the Noctis body has been rebuilt since.

# Noctis GMO vs retail Bartz GMO: static comparison

Files:
- export: /home/claude/recomp/psprecomp_work/mods_noctis/obj/p_fiv100_1p.gmo (3,451,552 B)
- retail: /home/claude/recomp/psprecomp_work/retail/p_fiv100_1p.gmo (2,102,308 B)

Method: dfx.chunks / dfx.gmo / dfx.gim parsing plus a numeric bind-pose skinning check. The analysis scripts are in /tmp/claude-0/an/.

![Noctis before/after](../images/noctis-before-after.png)

## Bottom line

I found no structural fault in the body mesh. I cannot prove the cause statically, because the guest-side loader and renderer code was not examined. The ranked suspects are in section 6.

The data the Forge wrote for the body is internally correct:
- Chunk sizes are valid and the file round-trips byte for byte.
- Every BlendSubset and BlendBones index is in range.
- Weights sum to 128 on every vertex.
- Skinning semantics are identical to retail.

What differs from anything retail ever does:
1. Four skinned meshes use 8-bone BlendSubsets (nw=8, vtype word 0x1401C31A), plus one with 6 (0x1201431A). Retail's BlendSubset path never exceeds 3.
2. The file has 135 bones (retail 83).
3. It has 35 parts (retail 18).
4. It has a 3,864-index draw call and 28.5k vertices (retail max draw is 1,218 and retail has 8.5k vertices in total).

Our GE decode handles all of this (section 7).

## 1. Skeleton

- **Bone count:** 135, against 83 retail. Both Model blocks carry all 135: `_000A` (the motion model) and `model-0` (the mesh model).
- **Retail bones:** bones 0..82 are identical to retail in order, name, parent and TRS. Nothing was reordered, so retail motion bindings that use indices 0..82 stay valid.
- **Added bones (83..134):**

| Bones | Names |
|---|---|
| 83, 84 | `warp_sword`, `warp_trail` (parent -1) |
| 85..117 | `g_root`, `g_hip`, `g_l_femur_c` ... `g_r_fing3b` (33 bones, a copy of the body rig, rooted at `g_root` with parent -1) |
| 118..121 | `warp_ghost0..3` (parent -1, draw parts 18..21) |
| 122 | `warp_sword_mesh` (parent 2, part 22) |
| 123..134 | `warp_trail0..9d` (parent -1, parts 23..34) |

  These come from the Dissidia Forge warp feature (warp.py), not from the Blender control rig. There are no CTL-* or MCH-* bones in the file.
- **Bone block layout:**
  - Retail owner bones use child order 0x8041, 0x8044 (BlendBones), 0x8045 (BlendOffsets), 0x8048, 0x804E.
  - Retail bones also carry a 0x80F1 BlindData child (a "DISTANCE" tag) on most bones.
  - The 17 new owner bones (118..134) write 0x8045 BEFORE 0x8044. That is the reverse of retail.
  - All 52 new bones lack 0x80F1.
- **Motion model:** 84 motions, same count and names as retail.
  - 77 are byte-identical in bindings and curves.
  - 7 are changed: _100A, _120A, _140A, _150A, _632A, _633A and _651A.
  - The first four bind the new bones up to index 117, with 354 bindings each (retail 177). _632A, _633A and _651A also changed, but their bindings stay on retail bone indices.
  - Retail shares fcurves between bindings (for example 128 curves for 177 bindings). The export writes one curve per binding.
  - The retail `_a` assist file is not in the working tree, so I could not check it. Bones 0..82 are untouched, so retail indices still resolve. Bones 83..134 do not exist in any retail animation file. Any animation that is not rewritten leaves them with no motion data.
- **Bind-pose consistency:**
  - For every blend bone, offset^T x World(bone) equals identity for the body's lower body, spine and waist bones, and for all `g_*` ghost bones.
  - The arm bones (19/21/22/43/45/46) show error 0.706 and chest/clavicle show 0.12..0.136. Retail shows the same errors, and the offsets are byte-identical to retail.
  - So retail's bind pose already differs from its skeleton rest pose on the arms. Noctis vertices are authored against retail's offsets, which is correct.

## 2. Parts

Body part 0 (owner bone 14 `p_fiv100`):

| | Retail | Noctis |
|---|---|---|
| Meshes / arrays | 59 / 59 | 5 / 5 |
| Owner BlendBones | 28 | 19 (all < 135, count equals BlendOffsets) |
| BlendSubset sizes | 1..3 | 8, 8, 8, 8, 6 |
| Vertex-type words | 0x0E00131A (nw1), 0x0E00531A (nw2), 0x1000931A (nw3) | 0x1401C31A (nw8, stride 20) x4, 0x1201431A (nw6, stride 18) x1 |
| Vertices per array | 24..270 | 1597, 378, 450, 253, 182 |
| Draw calls | Non-indexed; 1 TRIANGLES draw per mesh | Indexed; 3864, 954, 1326, 711, 642 indices |
| Material | 1 | 5 |

Common to both:
- Format fields: tc=u16, col=4444, no normals, pos=s16, weights=u8.
- The word's stride byte equals the computed GE stride in every array.
- No vertex array has an out-of-range index.

Weight and subset checks:
- Weight count equals subset length in every body mesh (8/8 and 6/6).
- Weights sum to exactly 128 on every vertex (u8 weights of 128 = 1.0).
- Maximum non-zero weights per vertex are 2..4.
- Skinning self-consistency:
  - The mean distance from each vertex to its dominant bone, using the subset mapping, is 0.147 for Noctis and 0.150 for retail.
  - With the wrong mapping it would be about 0.6.
  - Skinning a Noctis vertex to the bind pose reproduces its position exactly (error about 1e-7) for the ghost bones.
  - The retail-derived arm offsets show the same error as retail itself.

Other parts:
- **Parts 1..17 (wep0..wep7):** same owners and BlendBones as retail except wep0, wep12..15 and wep5, which dropped from 2 blend bones to 1.
- **Subset use:** retail weapon meshes use NO BlendSubset (the game sends the owner's whole BlendBones list; wep16/17 send 7 weights without a subset). The new weapon meshes (wep0, 10, 12, 13, 14, 15, 18, 5, 6) add `BlendSubset=[0]` with a 1-weight word (0x0E00031A).
- **Vertex counts:** wep0 and wep15 are 1625 vertices each (retail 251).
- **Parts 18..34 (new):**
  - Four ghost body copies: 5 meshes each, the same 8/6-weight words, owner `warp_ghostN`, parent -1, with 19 BlendBones into the `g_*` bones.
  - One sword mesh and twelve trail meshes (0x0E00031A, 1 weight, up to 644 vertices, draws up to 3,840 indices).
- **Max BlendBones on an owner:** 28 (retail) against 19. Nothing exceeds 8 per mesh. Every BlendSubset index is below the owner's BlendBones length. Every BlendBones index is below 135.
- **BlendOffsets presence:** present and equal in count to BlendBones for every owner bone.

## 3. 0x8015 position and UV scale

- Both Models carry the two 0x8015 chunks, byte-identical to retail:
  - pos sel 0x180: offset (0.5246, 1.3036, 0.2729), scale 1.3036 on all three axes.
  - uv sel 0x3: offset (0.00446, -1.9918), scale (1.9969, 1.4949).
- The encodable range is s16 / 32768 x 1.3036 + offset. That gives Y in [0, 2.61], X in [-0.78, 1.83] and Z in [-1.03, 1.58].
- Noctis body vertices sit in X [-0.58, 0.58], Y [0.12, 1.75], Z [-0.18, 0.11]. All are inside the range.
- A few feet vertices clip at s16 -32768, which is Y=0. Retail has 31+31+11 such vertices, so this is the same and harmless.
- Weapon part wep1 has two vertices clipped at +32767 (Z about 1.58). This is a small clamp, not an explosion.
- UV range is [0, 0.99], inside the representable window. The V maximum is 0.998, so V=1.0 cannot be encoded exactly.
- Verdict: the position and UV scale chunk is present and sane.

## 4. Materials and textures

- **Textures:** 26, all valid.
  - Every one is INDEX8 plus a 256-entry RGBA8888 palette, unswizzled (order 0), all dimensions power of two.
  - The two 263 KB textures are 512x512 (textures 7 and 9, a duplicate of the same image). That is the largest size the PSP accepts.
  - Other sizes: 256x256 (textures 0-2 and 6), 256x128, 128x128, 64x64, 64x16 and 16x16.
  - Retail uses the same format (256x256, 256x128 and 128x128, INDEX8 and unswizzled).
  - Every FileImage u32 size prefix equals the GIM length.
- **Small GIM header difference:** the new palette blocks use level_type 1 (retail 2). The Forge encoder writes 1 in `_image_block`. This is probably harmless but is a deviation from retail.
- **Texture names:** textures 20..24 are `file20..file24`, and retail textures 0..4 are also named `file20..file24`, so the names collide. Textures are referenced by index, so this should not matter.
- **Materials:** 31, each with one Layer and one SetTexture whose index is valid (0..25).
  - Material indices referenced by meshes are all valid.
  - The `Ambient?` command is identical to retail.
- **Model-level BlindData "TexAnm":** byte-identical to retail. It references no new materials, so any texture animation on retail materials is untouched.

## 5. Other structural differences

- **Chunk integrity:**
  - The parser raised no size errors.
  - All 30,924 chunks have 4-byte aligned sizes.
  - The root size equals the file length.
  - A parse-and-write round trip reproduces the file byte for byte.
  - There are no wrong size fields.
- **Block order inside Model:** VertexOffset x2, Bone, Part, Material, Texture, BlindData. This is identical to retail.
- **Retail meshes carry a 0x80F1 child on some meshes** (wep16/17 have it). The new meshes have none. This is probably irrelevant.
- **Model heap:**
  - The runtime logs "exceeds the retail model budget (2,506,752 B)" and raises the staging budget to 3,452,928 B (heap 3,655,680 B at 0x0A3ED000).
  - The guest was written for a fixed heap, so any other fixed buffer sized for retail data remains a risk.
  - The padding experiments (mods_pad_*) show size alone was already being tested.

## 6. Most likely cause and what to change

I cannot prove a single cause statically, because the body mesh data checks out. What the data does differ in:

1. **8-bone and 6-bone BlendSubsets (most likely).**
   - On the retail BlendSubset path the largest subset is 3 and the largest array nw is 3.
   - The game's per-mesh BONEMATRIX upload may assume a small maximum, or may have a fixed per-part matrix scratch area.
   - A matrix upload that writes beyond what the game has initialised, or reads stale matrices, gives enormous triangles.
   - What visible evidence supports it: the screenshot shows a few huge, smoothly shaded triangles in textureless green, blue and black. That fits vertices multiplied by non-rigid or uninitialised bone matrices.
   - The 4x ghost body copies use the same 8-weight subsets, which multiplies the effect.
   - **Change:** export the body retail-style, with at most 3 bones per BlendSubset and nw equal to the subset size:
     - Limit each vertex to 3 influences.
     - Split the body mesh into many small meshes keyed by bone triple, as retail's 59 meshes do.
   - If 3 is too restrictive, test 4 next.
2. **135 bones (83 retail).**
   - **Change:** a bisect export without the warp bones: keep only bones 0..82, with no `g_*`, `warp_*` or ghost parts (parts 18..34).
   - Per-character bone arrays in the guest may be sized for 83 (or a slightly larger power-of-two bound, such as 96). The ghost bones are root-parented with parent -1, and the new motions bind up to bone 117.
3. **Very large draw calls.**
   - Noctis has up to 3,864 (body), 3,231 (weapons) and 4,368 (max over the file) indices per draw.
   - Retail's maximum is 1,218.
   - The runtime log shows these draws with vtype 0x01C31A, count 3864, at vaddrs outside the GMO region, so the game expands indices into a work buffer first.
   - Any fixed expansion buffer sized for retail would overflow.
   - **Change:** as a test, decimate or split the draws to 1,200 indices or fewer.

Suggested bisect order, each build with the retail 83-bone skeleton:
1. Retail GMO plus only the Noctis weapon parts. This tests the weapons.
2. Body only, nw=3 split, without the warp parts (this combines fixes 1 and 2).
3. Re-add the warp bones and parts last.

Smaller issues, none expected to cause the blow-up:
- Write 0x8044 before 0x8045 on the new owner bones.
- Add the 0x80F1 BlindData child to new bones.
- Use level_type 2 for palette blocks.
- Do not duplicate texture 7 as texture 9.

## 7. Our runtime side

Checked in /home/claude/recomp/src/psprecomp-mpeg/runtime (psp_ge.cpp, psp_ge_vertex.cpp).

- **Vertex format:** the decoder handles weight count up to 8, u8/u16/float weights, 8/16-bit and float positions, and indexed draws.
- **Bone matrices:** `bone_matrix[96]` holds 8 matrices and BONEMATRIXNUMBER is masked to 0x7F, so 8-weight vertices are decoded.
- **Weight layout:** u8 weights are scaled by 1/128, which matches the data. Skinning is applied as sum w_k x (Bone_k x pos).
- **Stride:** computed from the type word and not read from the top byte; both agree for all these arrays.
- **Words involved:** 0x01C31A, 0x01431A and 0x00031A, with the stride byte removed by the logger. Retail never used nw=8 (max 7, on wep16/17).
- **Not exercised by retail:** the 8-weight skinning path and subset-based loading of 8 matrices have no retail precedent, so a latent bug there would first show with this model. I saw no defect in the code read.
- **Conclusion:** I found no runtime-side defect, and nothing the exporter does is unsupported by the GE decode.
