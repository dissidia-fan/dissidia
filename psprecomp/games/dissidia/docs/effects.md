# Dissidia 012: how the game draws effects

The rules the game's effect code follows, read from the recompiled EBOOT on 24 Oct 2026. Move Studio's renderer (`studio/fx.js`) implements them. The six research notes with addresses and evidence are in [archive/fx-components/](archive/fx-components/README.md). This page supersedes the early FEP notes in [formats.md](formats.md) where they disagree.

## The pieces of an effect
- **FEPA record** (260 B): where an effect plays. Offset (metres), rotation (degrees), scale, FEP name index, lifetime (+0x30, frames; the 0x0219 event's second argument replaces it), start delay (+0x74), follow time (+0x78), attach bits (+0x7C), model, bone and motion names.
  - Attached when the model name is set: the effect follows the bone every frame. Attach bit 0x1 = place once, 0x2 = rotation from the character root, 0x4 = position from the root, 0x8 = keep the bone's scale.
  - Records without a model name are placed by whoever plays them: the hit point, the projectile, the character's position.
  - Target (+0x70) picks what an attached record sits on: 0 = the named bone of the character's model; 1 = the character's own world matrix (its root, where root motion has carried it): the bone name is ignored (FUN_088A82D8 → owner vmethod +16 = FUN_0881476C → the char+1208 object, whose root and bone queries both return char+0); 4 = a global object (0x09B4D440, not traced). Vaan's spell circles name `camera2_include`, a bone his HP animations move against the root travel, but use target 1, so they sit in front of him.
  - Motion-bound records: when a fighter starts a motion (FUN_0883C2E8 stores it at char+1316, then FUN_088A98B4 → FUN_088A9E90 → FUN_088AA844), every record of its model whose motion name (+0xC8) is that motion and whose attach bits lack 0x80 plays from the motion's first tick, with its own start delay. No event is needed: Vaan's QUAKE / WATER / INFERNO / AERO squares and shots, Garland's FIRE_EISYO chants and most EX Burst pieces play this way. A record that an event also names plays twice (Bartz's WEP_APPEAR at frame 0).
- **Body**: the spawn schedule (which nodes start at which frame), a length, and how it ends.
  - `a` bit 0x2 = keeps playing until stopped (at `c`, by its lifetime, or when the projectile ends).
  - Stopping (`d`): 0 = fade out over 30 frames, 1 = over 15, 2 = vanish, 3 = loops stop and the pieces play out.
- **Node** (one piece): a length, a kind (byte 11), a loop window (h12..h14, flag 0x2) and tracks.
- **Track**: one component (the channel id), its keyframe segments, and a list of the other tracks it reads (colour, the transform, a history).
- **Segment**: a component's parameter block from one frame on. Curves run `value + rate·t + acc·t²/2` from the segment start and keep going past its length until the next segment; they don't clamp.

## Units and space
- 1 effect frame = 1 game tick (60 per second). World units are metres.
- Positions are metres; scale channels are percent (×0.01).
- Effect space has Y and Z flipped against the game: channel 14 builds the piece's matrix as `T(x,−y,−z)·R(rx,−ry,−rz)·S(s)`, rotation order XYZ by default.
- Channel 14 also picks the facing (byte 1): 0 = world, 1 = facing the camera (keeps the effect's scale, drops its rotation), 2 = upright and turned to the camera.
- Flipbooks draw at a 0.1 basis scale; meshes at their resource's scale (1.0).

## Colour and blending
- The GE draws effects lit: ambient = the node's colour (128 = 1.0), material = the vertex colour or the flipbook part's tint. FRAGMENT_2X doubles the textured colour, and the blend factors use twice the alpha. These doublings cancel the 128 = 1.0 convention.
- Each mesh material and each flipbook part carries a GE blend word: `0x000A0600` additive, `0x00070600` alpha blend, `0x000A0602` subtract. Trails and screen effects pick 0 alpha / 1 add / 2 subtract with a byte.
- No depth writes, except mesh materials without flag 0x4 (and opaque alpha-blended ones). Flag 0x8 culls back faces. Many pieces add a depth-only bias toward the camera (0.8 m is common).

## Components
| channel | what it does |
|---|---|
| 1–9 | position / rotation / scale: constant, linear, quadratic |
| 15–17 | colour RGBA, clamped to 0..255 every frame |
| 14 | builds the piece's matrix and facing |
| 10 | flipbook: the clip at time × speed; every part of that clip's sequence is drawn at once; one-shot flipbooks vanish at the end |
| 11 | mesh: every group (scale and offset, its own texture cell) and every prim with its own material |
| 20 | mesh with a scrolling texture offset |
| 21 | particle emitter: a ring buffer of up to `maxP` particles; box / cone / ring / mesh-vertex spawn shapes; speed along the shape, a second force (gravity, rising), size, spin, colour, fade; each particle is a camera-facing flipbook |
| 18, 151 | start other nodes of the body: 18 at a rate, 151 in a burst at each segment |
| 22–25, 31–34, 39 | random offsets added once to another track's start value, rate or acceleration |
| 19, 29 | gravity; turn along the velocity |
| 61 + 62 | flicker / pulse envelope multiplied into the colour |
| 106 + 105 | weapon trail: two points on the blade sampled every frame (4–15 samples), tapered to a point at the oldest, coloured newest → oldest, textured, drawn as a cubic B-spline ribbon |
| 107 | flat colour flash over the screen |
| 109 + 110 / 111 | zoom / rotation blur: the frame redrawn magnified (or turned) about the hit point |
| 101 | shockwave ring sampling the frame |
| 100, 102, 103, 95 | spark streaks, line sparks, rays, points |

Not used by any retail effect: 30, 53–57, 63, 78, 108, 119.

## Format corrections
- MDL group floats are scale ×3, offset ×3, texture scale ×2, texture offset ×2. Prim +12 is the material index (−1 = untextured). Vertex type 0x98 has colour but no UVs. Material +20 is the destination FIX colour.
- An ANM is clips (durations) → sequences (lists of parts) → parts (one quad each, with a texture, tint and blend word). Its texture list has an 8-byte stride. Header +0x40 is the quad size and offset; +0x10 bit 0x80 = loop.
- Channels 26/30/150 spawn child nodes at mesh vertices; they are not trails. The trail is 105/106.
- Event 0x042A record 9999 picks the first FEPA record with id 9999, +0x34 = 0, bit `arg2` set in +0x44 and a bone the model has; `arg3` scales it. Event 0x022E sets the blade's hit reach, not the visual trail.
