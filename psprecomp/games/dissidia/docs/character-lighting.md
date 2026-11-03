# Live character lighting (native build)

The game's characters carry no vertex normals. The GE can only light them with flat ambient (`0x808080` x vertex
colour, doubled by the texture function), so all of their shading is painted into the textures. That suits the
retail cast, whose textures were painted for it, but a ripped model (Duellum, War of the Visions) looks flat and
"stuck on" next to them. `body.lighting` cel-shades a body in the native build, in the style of Dissidia Duellum:

- the body falls into a **lit tone** and a **shadow tone** with a crisp, anti-aliased edge between them (or three
  tones, or a smooth gradient);
- the shadow tone is the colour itself, darker and **richer** (more saturated), not a grey overlay;
- the **head is shaded as a rounded form** (its normals bent towards a sphere), so the face gets one clean shadow
  side instead of blotches from the nose and brows: the usual cel-shading trick;
- a small **hard highlight** on bright parts (hair, metal, skin) and a thin **rim light** round the silhouette;
- the light follows the camera, so the body reads the same from every angle.

The game's own colour stays the base (painted texture x vertex colour x stage ambient), so the stage's ambient
fades (EX Core prompts, assists), hit flashes and HP-attack cinematics still darken and tint the body as before; the
rim fades with them.

Recomp only. A stock PSP draws the body as before.

## Turning it on

In the character's `mod.json`, in `body`:

```json
"body": { "skin": "duellum", "models": [...], "lighting": true }
```

then `charmod.py build`. `true` is the defaults; an object changes some of them:

```json
"lighting": { "threshold": 0.5, "bands": 3, "rim_colour": "#ffd0a0" }
```

### Presets

`"preset"` picks a named look; other settings beside it change that look further:

```json
"lighting": { "preset": "anime", "rim": 0.5 }
```

| Preset | Look |
|---|---|
| `cel` | the defaults: Duellum's two tones with a crisp edge |
| `anime` | a harder edge, deeper and richer shadows, a brighter highlight |
| `three_tone` | lit, mid and shadow tones |
| `soft` | a smooth gradient, no hard edge |
| `dramatic` | light from the side, deep shadows, a strong warm rim |
| `subtle` | a little form, close to the retail look |

The presets are `charmod_light.PRESETS` (each lists only what it changes from the defaults below); the Move Studio
has a copy in `studio/lighting.js` (`LIT_PRESETS`).

### Settings

| Setting | Default | What |
|---|---|---|
| `key` | 1.0 | how much the light shapes the body: 0 = the game's flat look |
| `bands` | 2 | tones: 2 = lit + shadow (Duellum), 3 adds a mid tone, 0 = a smooth gradient |
| `threshold` | 0.4 | where the shadow starts: higher puts more of the body in shadow |
| `edge` | 0.04 | softness of the light/shadow edge: 0.02 crisp, 0.5 soft |
| `wrap` | 0.6 | how far the light reaches round the body |
| `key_dir` | `[-0.35, 0.55, 0.76]` | where the light comes from, relative to the camera (x right, y up, z towards the viewer): front, above, a little left |
| `light`, `shadow` | `#fffaf2`, `#8b889c` | tint of the lit tone and of the shadow tone |
| `shadow_sat` | 0.3 | how much richer (more saturated) the shadow colours are |
| `face` | 0.7 | how rounded the head's shading is (0 = as modelled); the vertices weighted to the `head` and `hair` bones. Applied when the model is built, not live in game |
| `spec`, `spec_size` | 0.25, 0.15 | the hard highlight on bright parts: strength and size |
| `rim` | 0.75 | rim light strength (0 = none) |
| `rim_width` | 0.18 | rim width |
| `rim_dir` | `[0.6, 0.3, -0.75]` | which side the rim is on, relative to the camera (right and up) |
| `rim_colour` | `#e2efff` | rim colour |

It works on any body skin, whichever tool made it, and on `manikin`; a retail character's own body (`"skin":
"retail"`) is left as it is (its textures already carry their shading). Skins made with the Blender add-on's
*Painted lighting* option have soft lighting baked in as well; it adds to the live light. If a lit body looks too
dark or too contrasty, lower `key` or `threshold`, or remake the skin with *Painted lighting* off.

## What the build does

`charmod_light.py`, called from `charmod.py build` when `body.lighting` is set:

1. **Normals.** Each body part gets s8 vertex normals (+4 bytes a vertex), smooth across the exporter's seam
   splits (UV seams, bone batches) and creased at 60°; the head's are bent towards a sphere round it (`face`). They are computed from the mesh itself, so skins made before
   this need nothing redone. They go into the built model only; the skin files are not changed.
2. **FX shader draws.** For every body texture, a draw `lit/<folder>/<n>` in `fx_shaders/manifest.json`, matched on
   the texture's texture-pack id (the one an HD `skins/<skin>/hd/` picture stands in for, so the HD texture is lit
   too), with `fx_shaders/lit/character.frag` (source: `games/dissidia/fx_shaders/lit/`) and the settings as
   uniforms. Turning lighting off and building again removes them.

The shader draws through the [FX shader system](../../../FX_SHADERS.md): same transform and skinning as the game,
same blend, depth and culling. Without normals in the vertices (an old build of the model) it draws the game's
colour unchanged.

## Tuning

In the Move Studio: Character tab > Body > **Lighting**. The viewport draws the body the way the game will (same normals, same shading); the sliders and colours change it live and save into `body.lighting`. Hold *Hold: flat* to compare with the game's flat look. Then save and build.

Live, in game: run with `PSPRECOMP_FX_SHADER_RELOAD=1` and edit the `lit/<folder>/<n>` entry's `uniforms` in
`work/mods/fx_shaders/manifest.json` (or the shader). It reloads within a second. Copy the values you like into
`body.lighting`; the next build writes the manifest again from it.

Offline (Linux / Steam Deck desktop mode): `tools/char_light_preview.sh` renders the body before (top row) and
after (bottom row) from a few angles, with the runtime's own uber-shader and FX code:

```sh
games/dissidia/tools/char_light_preview.sh ../mods/cloud-retail/models/p_sev100_1p.gmo \
    ../mods/cloud-retail/skins/duellum/p_sev100_1p.skin cloud.png
LIGHT='{"bands":3}' ANGLES="0 90 180" games/dissidia/tools/char_light_preview.sh ...
```

## Not done yet

- Stage-matched light: the key and rim are the same on every stage. The stage's ambient colour already tints
  them; a per-stage key colour and direction is the next step.
- Light from effects: fire spells, hit sparks and EX Mode don't light the body yet.
- Duellum's own normal (`_N`) and emission (`_E`) maps aren't used yet (they need tangents, and a texture slot the
  skin doesn't carry).
- Weapons and warp ghosts are not lit.
