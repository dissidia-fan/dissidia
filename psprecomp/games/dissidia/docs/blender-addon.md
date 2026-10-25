# Blender add-on: reference

The Dissidia Forge add-on's panels in detail: Moves & events, Character mods (skins, your own model and weapons, new characters), the control rig, adding bones and parts, the base body, and current limits. The short guide is [../blender/README.md](../../../../blender/README.md).

## Moves & events (sidebar > Dissidia > Moves & events)

For placing events and tuning hits, the **Move Studio** (`studio/` at the repo root, the film-strip button) is the main tool; Blender is for the animations. *Save moves to folder* writes the animations and leaves the events as the studio left them unless you edited events here (the arrow next to it: *Keep the folder's* / *Save from Blender*).

For a character imported from its mod folder (Character Selector):
- **Save moves to folder** writes the animations into the folder's rig (`models/<rig>.gmo`; only the animation part is
  replaced, the body, weapons and textures in the folder stay as they are) and the events and hits into `moveset/`
  as the usual readable files. Saving with nothing changed leaves every file as it was. Then build the character.
  New bones aren't saved this way.
- **Events** of the active animation, in time order, with names (Hit window, Chain window, Effect, Sound, Show part …,
  hits and parts by name). Each event is also an `e:` marker in the timeline: drag it, then **Keep marker timing**.
  **Show events** redraws the markers; **Copy events from…** gives a new animation another one's events; **Weapon
  preview** shows only the weapons the game would show on each frame.
- The sections (mod folder, animation, the selected event, its hit, parts, the event list) fold away.
- **Parts hidden at start**: one tick box per weapon / ghost / effect part. Ticked = the animation hides it at frame 0
  (a "hide part once" event); a *Show part* event can still show it later. These events are left out of the list.
- Click an event to edit it: start / end (in frames; **To the end** = until the move ends), the hit, the part, the
  effect / sound id, the hit group, the phase, and any other values. **Add** puts a new event at the current frame,
  shaped like the moveset's own events of that kind; **Duplicate** copies the selected one to the current frame.
- A hit window's **hit** (damage and knock-back): name, power, re-hit, HP attack, reaction, knock-back forward / up,
  stun, follow. A hit used by several windows changes all of them; **Own copy of this hit** gives this window its own.

## Character mods (in the Character Selector)

Pick a character mod in the Character Selector to see these (the + next to the list makes a new one). Lists the character folders in the repo's `mods/` (found by itself when the add-on runs from the repo;
otherwise set *Character mods folder* in the add-on preferences). For the chosen character:
- **Skin**: the skins it has (body and weapons) and which one the game build uses; *Use* switches it (`mod.json`
  `body.skin`). The default is the based-on character's manikin and own weapons, read from your disc.
- **Replace body with my model…** (after *Import from mod folder*): pick a model file (FBX, glTF/GLB, VRM, OBJ or .blend). A
  rigged model is matched bone by bone (Mixamo, Unity, Unreal, VRM, Rigify and similar names, or found by the
  hierarchy from the hands and feet), turned to face the same way, scaled to the character's height, and each limb
  carried onto the game's (`own_model.py`, with `fit_body.py`'s fitting); fingers, twist bones, hair and the like go
  with their limb. A model without a skeleton is scaled to the character and weighted automatically (it must stand
  in a pose like the character's). One picture is kept as it is; several are put together into one. Options: skin
  name, *Bulk* (thickness), triangles (2,500), texture size, painted lighting. It is saved as
  `skins/<name>/<model>.skin` for every model of the character (the fighter, the assists `_a`, `_w`; files read from the ISO too), so none keeps the based-on character's body. The costumes (2P, 3P, zako, and their assists) become the same model: `mod.json` points them at the first one's files and the based-on character's costume models are removed from `models/`, and set for the game (`mod.json`
  `body.skin`, `body.models`). A rig whose body shares a texture with other parts (many retail models put the body
  and weapons on one picture) first gets a copy of its own, with the same pixels.
  Ripped game models: corrective helper bones (`j_addKnee_l` style) are skipped and numbered chains (`leg_01/02/03`,
  `arm_01/02`, `neck_01`) read as limbs; a mesh stored twice is kept once; eye layers, the inside of the mouth,
  stubble and lashes are left out; textures whose path points elsewhere are found next to the model by name (a
  missing one is a flat colour); hair made of strips with no colour picture becomes one solid mass in *Hair colour*,
  without the strands over the face. A highly detailed model (a modern game's, 100k+ triangles, layered clothing)
  still comes out rough at 2,500 triangles: a hand-made low-poly version looks far better.
- **Your own weapon** (the Move Studio's Body page > Weapons > *Use my model…*, add-on 0.29+; `own_model.py`
  `DFX_OT_own_weapon`): pick an FBX, glTF/GLB or OBJ for one `wepN` part. It is lined up with the old weapon (hilt on
  the grip, its length, *Flip*, *Grip at the model's origin* for game rips), reduced to the triangle budget (600), its
  pictures put into one (256 px), and saved into the skin the game uses (`skins/<skin>/<model>.skin`, its body kept),
  or a new `own-weapons` skin on the body the game shows now. Weapons drawn with the same picture (a weapon held and
  the same one carried) take the model together; a weapon sharing its picture with an effect can't take one. The
  game stores positions inside the model's range, so a weapon reaching past it is made a little smaller about the
  hand until every fighter model takes it; an assist model (`_a`, `_w`) whose range it still doesn't fit keeps its old
  weapon (the message says which). Replacing the body later keeps the skin's weapons.
- **Model and weapons**: the body the character wants (`body.model_hint`) with *Replace body with my model…*, and
  each weapon (`body.weapon_groups`, else every `wepN`) with *Use my model…* (the same as the Studio's buttons).
- **New character…**: folder name, name, full name, series, the retail character it's built on, and optionally a big picture to cut the menu / HUD pictures from. Runs `charmod.py new` (and
  `charmod.py avatars --from-picture`), then selects it. The code (first 3 letters of the name + the first free number,
  e.g. `ter100`) and the roster slot (the first free one from 35) are picked automatically.
- **Build into the game** runs `tools/charmod.py build <name>` (run.sh also does this at every launch).
- The repo's tools are started with Blender's own Python. They bring their own numpy, Pillow and scipy: the first run
  sets them up in `tools/.venv/` (`tools/pydeps.py`; one download, about 60 MB) and the tool restarts itself there.

## Control rig
- **Rig › Build control rig** adds standard controls on top of the game skeleton:
  - master and torso;
  - hips and spine;
  - FK/IK arms and legs with pole targets;
  - fingers, toes and cape.
- The game bones follow the controls. Their own rest orientations never change.
- **Animate with controls** fits the controls to the current action's animation (every frame) and switches that action to the rig. **Back to game keys** bakes the controls' result onto the game bones and switches the rig off for that action.
- FK/IK sliders per limb are in the Rig box. Arms start in FK and legs in IK.
- Export (and Copy animations to costume) bakes every rig action onto the game bones first. Rig bones are never exported.
- Tools that write animation (Warp dash, Noctis move) put that action back on game keys. Press **Animate with controls** again to take it over.

## Adding to a character
- **New bones** are appended after the existing ones, so the moveset's bone indices stay valid. Don't delete existing bones; hide them or unweight them.
- **New mesh objects** parented to the armature (with an Armature modifier and bone-named vertex groups) become new parts with their own draw bone. That draw bone can be the target of show-weapon events.
- **Non-game materials** become new GMO materials. Their Image Texture node becomes a new GIM texture.

## Cape bones (physics cape)
For a character whose based-on model has no cape physics (e.g. Ramza on Vaan). The game swings the cape itself
(spring bones, `docs/spring-bones.md`); Blender only sets it up. Add-on 0.30+.
1. **Import from mod folder**, then **Tools › Add cape bones**: length (0 = to mid-shin), width (0 = the shoulders'),
   chains (3 = back, left, right), bones per chain (3), hang back, hem spread, stiffness (*Loose* = Cecil's,
   *Graded* = Exdeath's, heavier), leg spheres. It adds `b/l/r_mant1c_c` on the chest and under each
   `*_mant1c → *_mant1b → *_mant1a`, after the existing bones (no index changes), fitted to the character's own chest,
   shoulders, back and legs. Each physics bone's length is its `dfx_distance` property (the GMO's DISTANCE). Run it
   again to redo the bones with other settings.
2. Model the cape **in the body mesh** (part 0; join a separate cape object into it, same materials as the body).
   **Weight to cape** weights the selected mesh, or in Edit Mode the selected vertices: each row to its bone, blended
   between rows and between chains, the top row blended into the chest. Touch it up by hand as needed.
3. Save the body the usual way (*Replace body…* / *Make skin* writes `skins/`), and **Save moves to folder**. Saving
   puts the new bones into every rig in `models/` (all costumes share one moveset, so they all get them) and writes
   the cape's physics records (Cecil-style: limits, links between chains, collision spheres on chest / hip / thighs)
   into `moveset/<p_code>/sections/*_6.bin`, after any the character had. Then build.
Notes: a cape that is a separate object (a new part) is not saved by *Save moves*; join it into the body. Skins made
before the cape still work (without a cape). Assist models (`_a`) get no physics. Check the result with
`tools/spring_dump.py`. Untested in game so far: tune with *Hang back* / stiffness if it flips or floats.

## Base body (for modelling your own character)
- **Tools › Add base body** builds a plain, low-poly humanoid (about 1,240 triangles, Bartz's body is 1,636) fitted to the character's bind pose:
  - one closed mesh with edge loops at every joint, weighted to the 25 body bones (at most 4 weights per vertex);
  - UV islands per body part (seams down the back and at the joints), and a 128 × 128 flat-colour texture (skin / top / legs / feet) to paint over;
  - **Use as the body** (on by default): it exports in place of the character's body (part 0). The original body is kept, hidden, in the "Original body (not exported)" collection as a reference. Running it again swaps out the previous base body.
- Shape it in Edit/Sculpt mode in the bind pose; add hair, clothes and accessories as extra geometry in the same object (weight them with the existing vertex groups) or as new objects parented to the armature (each becomes a new part).
- After changing the body, re-apply **Warp dash** so its ghost copies use the new body.

## Other tools
- `tools/build_iso.py`, `tools/moveset.py`, `tools/warpdash.py`, `tools/keymove.py` (in the dissidia folder next to `forge/`, outside the repo): command-line equivalents.
- `forge/forge_studio_local.html` (outside the repo): browser studio. Command → animation browser, event timeline editor, warp dash tool, and **Save all**.

## Current limits
- Re-encoded geometry uses triangle lists, about 10% bigger than the game's strips.
- Re-encoded textures use a 256-colour palette.
- Event meanings marked "?" are inferred from timing.
- Event timelines count at 60 fps (two ticks per animation frame). Blender markers and the studio show them in animation frames and convert when saving.
- Aerith (sev120) has no playable model on the disc, and Chaos (org200) uses a different file name, so neither imports from the ISO list yet.
