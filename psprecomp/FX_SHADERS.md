# FX_SHADERS.md — custom GLSL shaders for effects

The GE emulation draws everything with one uber-shader (`runtime/src/psp_ge_shader.cpp`).
**FX shaders** replace it, for chosen draws only, with your own fragment shader. That gives effects
looks the PSP's fixed-function GE cannot produce (fresnel rims, flowing masks, reveal fronts,
refraction). The game's FEP effect still decides when and where the effect draws, its mesh, and
its colour and alpha keys. The shader only decides how each pixel of that draw looks. Remove the
manifest entry and the FEP version draws as before, so the FEP effect is always the fallback.

Code: `runtime/include/psp_fx_shader.h`, `runtime/src/psp_fx_manifest.cpp` (manifest, matching,
ages; no GL), `runtime/src/psp_fx_shader.cpp` (GL), hook in `psp_ge_draw.cpp` (`fx_batch`).
Data: `work/mods/fx_shaders/` (`<mods>/fx_shaders`, from `games/dissidia/runtime/hooks_io.cpp`).
Tests: `test_fx_shader` (ctest `fx_shader_tests`, which also checks the shipped manifest), and
`games/dissidia/tools/fx_shader_preview.sh` (Linux / Steam Deck: real GL compile, contact sheet, GPU bench).

## How a draw is matched

* Every texture the GE binds gets its **texture-pack id**: the 64-bit xxHash of its decoded texels
  plus the palette entries it uses (`psp_texpack_make_id`, the same id as `mods/textures/textures.txt`,
  the `PSPRECOMP_TEX_DUMP=1` PNG names and texture-pack replacements). It is content based, so it
  does not depend on the guest address and survives re-installs. Ids are computed on a texture-cache
  miss only, and only when the texture pack or the FX shaders are on.
* When a new GL batch starts (`ensure_batch`), after the uber pipeline has been applied, the bound
  texture's id is looked up in the manifest. On a hit:
  * the override program is bound. Its vertex stage is the uber vertex shader compiled with
    `PSP_FX` (same GE transform, skinning, lighting and UV transform), so the geometry is the game's;
  * blend, depth, stencil, cull and colour mask stay the game's, unless the entry sets `blend` / `depth`;
  * alpha and colour tests are **not** applied (use `discard`).
* So that one effect's draws can be told apart, every effect layer that needs a shader needs **its own texture**.
* Render-target textures, movies and untextured draws never match.

## manifest.json

```json
{
  "textures": { "crack": "tex/crack_01.png" },            // named PNGs (paths relative to the folder)
  "params":   { "common": { "u_hue": 0.7 } },             // optional shared uniform sets
  "draws": [
    { "name": "alterna/sphere",
      "hash": "43d72f192865ba6a",                          // texture-pack id, or a list of ids
      "frag": "alterna/sphere.frag",
      "include": "alterna/common.glsl",                    // or a list; prepended to frag
      "uniforms": { "u_rim": 4.0, "u_tint": "#ff3048", "u_v": [1, 2] },  // float/vec2-4/"#rrggbb"/mat4
      "params": "common",                                  // merged before "uniforms"
      "textures": { "u_crack": "crack", "u_raw": { "file": "tex/raw.png", "clamp": true } },
      "blend": "game",       // game | add (SRC_ALPHA,ONE) | add_one (ONE,ONE) | alpha | premul | screen | off
      "depth": "game",       // game | nowrite | write | off
      "screen": false,       // true: u_fx_screen = copy of the render target (refraction)
      "life_ticks": 156,     // u_fx_life_ticks; u_fx_progress = age / life
      "age_offset_ticks": 63,// added to the age (e.g. the lab tick the part is born)
      "reset_gap_ticks": 8,  // unseen this long -> the next draw starts a new instance (age 0)
      "enabled": true }
  ]
}
```

`//` and `/* */` comments and trailing commas are accepted. Entries with no valid hash, no `frag`,
or `enabled: false` are skipped (logged). A hash listed twice keeps its first entry. Extra
textures are PNG (RGBA, RGB, grey or palette), mipmapped, and repeat unless `clamp` is set.

## Writing a shader

The file defines `vec4 fx_shade()` and returns the fragment colour. The runtime puts a preamble in
front of it (`psp_fx::fragment_preamble()`) and a `main()` after it. Declare your own uniforms and
samplers. The preamble provides:

| Name | What |
|---|---|
| `v_color` | GE vertex colour after lighting: the **FEP colour/alpha keys** |
| `fx_tint()`, `fx_env()` | the same in display scale: colour doubled when the GE doubles (`128` = 1.0), alpha doubled when the game blends with 2×src alpha but the entry overrides the blend |
| `v_texcoord` | GE texture coordinates (after the GE UV scale/offset) |
| `v_fx_uv`, `fx_uv()` | raw mesh UV; `fx_uv()` = mesh UV on 3D draws, `v_texcoord` on sprites |
| `v_fx_opos`, `v_fx_wpos`, `v_fx_wnormal` | model / world position, world normal (3D draws; normal 0 if the vertices have none) |
| `fx_normal()`, `fx_view_dir()`, `fx_rim()` | world normal (from the mesh origin when there are no normals: right for spheres), eye direction, `1 - abs(dot(N, V))` |
| `u_texture`, `fx_ge_color()` | the draw's own texture (unit 0) and the colour the uber-shader would have produced |
| `u_fx_time`, `u_fx_ticks` | seconds / ticks (60/s) since the system started |
| `u_fx_age`, `u_fx_age_ticks` | seconds / ticks since this effect instance first drew (+ `age_offset_ticks`) |
| `u_fx_life_ticks`, `u_fx_progress` | from `life_ticks` |
| `u_fx_world`, `u_fx_view`, `u_fx_proj`, `u_fx_camera`, `u_fx_origin` | GE matrices (mat4), eye position, mesh origin, world space |
| `u_fx_screen_size`, `u_fx_viewport`, `fx_screen_uv()` | render-target size, draw viewport, `gl_FragCoord.xy / size` |
| `u_fx_screen` | render-target copy (entries with `"screen": true`) |
| `u_fx_has_world`, `u_fx_has_normal` | 1 on GPU-transform draws / when the vertices carry normals |
| `fx_ease()`, `fx_saturate()`, `texture2D` | helpers (`texture2D` is defined as `texture`, for WebGL ports) |

GLSL is `#version 330 core`, so the same source runs on macOS (4.1 core) and Mesa (Steam Deck).
The preamble declares `in`/`out`, so a WebGL port changes `gl_FragColor = x;` into `return x;`
and `varying vUv` into `fx_uv()`/`v_texcoord`; three.js `vN`/`vV` become `fx_normal()`/`fx_view_dir()`.
Avoid GLSL keywords as names (`flat`, `sample`, `smooth`, `patch`). Errors are reported with lines
relative to your file (`#line 1`). **The Mac's compiler is stricter than Mesa:** every non-void
function needs a `return`, even after `discard`. `ctest -R fx_shader` lints for that, and when
`glslangValidator` is installed (`brew install glslang`) it compiles every shipped shader with the
Khronos reference compiler, which is as strict as the Mac.

**Timing in data.** Use `fx_env()` and `fx_tint()` as the envelope and colour, so fades and colour
ramps stay in the FEP keys and the fallback FEP effect and the shader agree. Drive anything else that
moves over time (reveal progress, collapse, twist) from `u_fx_age_ticks`. With `age_offset_ticks`
set to the tick the part is born in your timeline, `u_fx_age_ticks` is that timeline's tick.

**Additive output.** For `add_one`, return `vec4(rgb * env, 0.0)`: GL clamps the output to 0..1
before blending, like the lab's WebGL canvas, and alpha 0 leaves the destination alpha alone (the GE
keeps the stencil there).

## Adding a shader for another move

1. Give each layer that needs a shader its own texture in the move's FEP data, and install it.
2. Get its texture-pack id. Either compute it offline from the FEP TEX
   (`games/dissidia/tools/texpack_id.py`, as `forge/noctis/alterna_lab/fep_layers.json` does), or run
   with `PSPRECOMP_TEX_DUMP=1` and look up the PNG in `mods/textures/dump/<id>.png`.
3. Write `work/mods/fx_shaders/<move>/<layer>.frag` with `vec4 fx_shade()`, and add a `draws` entry.
4. Check it: `ctest -R fx_shader` (parse, files exist). On Linux / the Deck,
   `games/dissidia/tools/fx_shader_preview.sh` compiles every shader with the real GL code and renders
   a contact sheet (extend `fx_shader_preview_samples.py` for the new layers).
5. In game, run with `PSPRECOMP_FX_SHADER_LOG=1 PSPRECOMP_FX_SHADER_RELOAD=1`, then edit and save.
   The shaders reload within a second. A compile error is logged and the FEP look stays.

No rebuild is needed for manifest or shader changes, only for runtime code.

## Env vars and log

| Flag | Effect |
|---|---|
| (always) | `first match <hash> <name>` once per override per run, so any log shows whether the ids matched |
| `PSPRECOMP_FX_SHADERS=0` | System off (no ids, no lookups) |
| `PSPRECOMP_FX_SHADER_LOG=1` | Load / compile / per-entry hash list; one `match <hash> <name>` line each time an effect instance starts; a stats line per second while overrides draw |
| `PSPRECOMP_FX_SHADER_LOG=2` | + GPU time of the override draws (`GL_TIME_ELAPSED` queries): `GPU x ms/frame (y us/batch)` |
| `PSPRECOMP_FX_SHADER_RELOAD=1` | Re-read the folder when manifest.json or a shader file changes (checked once a second) |
| `PSPRECOMP_FX_SHADERS_DIR=<dir>` | Use another folder |

```
[FXSHADER] <dir>/manifest.json: 12 override(s) listed (texture ids on)
[FXSHADER] <dir>/manifest.json: 12 of 12 override(s) ready, 10 program(s), compiled in 40.2 ms
[FXSHADER] match 43d72f192865ba6a alterna/sphere (energy sphere) (alterna/sphere.frag): new instance at t=812.4s, gpu-transform draw
[FXSHADER] stats: 60.0 frames/s, 9.2 override batches/frame (7 textures), 0.00 screen copies/frame, GPU 0.412 ms/frame (44.8 us/batch)
[FXSHADER] compile failed: alterna/x.frag (...), using the uber-shader:
fragment: 0:12(5): error: ...
```

If there is no manifest, or it has no enabled entry with a hash, the system is off: one log line at
start, no ids computed, and draws never call in.
**`PSPRECOMP_GE_AB=1`** compares the fast path with the legacy path, and the legacy path has no
overrides, so run it with `PSPRECOMP_FX_SHADERS=0`.

## Cost

* **No manifest:** a cached flag test per batch.
* **Manifest on:** one xxHash per texture-cache miss (as with the texture pack), and one hash-map
  lookup per GL batch.
* **Each override batch:** a program switch and about 30 uniform uploads, uncached (overrides are
  a handful of batches a frame). It also stops the batch from merging with uber-shader neighbours.
* **Screen copy:** one `glBlitFramebuffer` of the target, at most once per frame per render target,
  and only for entries with `"screen": true`. Later overrides in the same frame see the copy taken
  at the first one.
* **GPU:** the fragment cost of the override's own pixels. The Alterna sphere shader is the heaviest
  (13 texture reads). Measure with `PSPRECOMP_FX_SHADER_LOG=2` in game, or
  `FX_PREVIEW_BENCH=1280x800 games/dissidia/tools/fx_shader_preview.sh` on the Deck, which times
  a full-screen pass of each shader next to the uber-shader.

## Alterna (Noctis)

`work/mods/fx_shaders/alterna/` holds a three.js lab ported to the game
(`forge/noctis/alterna_lab/alterna_fx_lab.html`, values from `alterna_lab_spec.json`). It is matched
to the v8 FEP layers from `forge/noctis/alterna_lab/fep_layers.json`. Contact sheet:
`forge/noctis/alterna_lab/fx_shader_contact_sheet.png`.

| Lab layer / FEP part | Shader | Notes |
|---|---|---|
| bubble disc (`0cc790f34c19a020`, v8c camera-facing quad) | `bubble.frag` (F_BUBBLE) | the quad is the sphere face-on: fake sphere normal from the radius (fresnel), lat-long UV of the front and back hemispheres; crack_01 opens patch by patch over 40 ticks, with a bright front |
| glassbreak shards (`aa47040e80bcd4b3`) | `glassbreak.frag` (F_GLASSB) | crack_01 + rim |
| sphere spectrum (`43d72f192865ba6a`) | `sphere.frag` (F_SPHERE) | glass facets, spectrum shades, fire flow, holes from tick 73, collapse to a hard rim 181→215 |
| sphere rim billboard (`43e7ef0959d5ee3e`) | `smear.frag` (F_SMEAR) | sideways edge bleed; `u_edge` = outline radius on the quad (0.6667: the rim billboard is the lab's 1.5 R smear quad, `forge/ffxv_tools/alterna/v8/patch_rim_lab_size.py`) |
| flare star (`394a20508fa9b2b4`) | `star.frag` (F_STAR) | rays, streak, brightens, twists, white fill swelling to the sphere |
| flare white glow (`62febe46d4b9fc0a`) | `none.frag` | hidden: `star.frag` does the fill |
| flare line / afterglow line | `line.frag` (F_LINE) | |
| nova swirl (`4a1dca449075a139`) | `nova.frag` (F_NOVA ×3) | twist for 45% of the life, then erupt, lens bulge, heat peaking at the HP hit |
| nova arc (`b2aeeb3c58a67229`) | `arc.frag` (F_ARC) | flickers every 3 ticks |
| afterglow star (`5578440d00fe9315`) | `star.frag` (no fill) | |
| smoke patches (`73d9eeb9c4ff8939`) | `fire.frag` (F_FIRE) | torn smoke, alpha blend; in the second half of its life the FEP fade becomes growing gaps (patch fade) |

The FEP keys use 128 = 1.0, and the alpha carries min(op, 1). v8 keeps the colour keys white for
fixed-colour layers (their colour is baked into the FEP texture palette), so those entries set
`u_color` (rgb, a = 1) to the lab colour; ramp layers (sphere, rim, glassbreak) take it from the keys. `u_gain` = lab op / FEP peak alpha
restores ops above 1. `age_offset_ticks` is the lab tick each bullet is born, so the shaders run on
lab ticks.
