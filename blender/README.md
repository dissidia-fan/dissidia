# Dissidia Forge: Blender add-on

Blender 4.4+ add-on for Dissidia 012 characters: import one from your ISO or from a mod folder, model, texture and animate it, and save the result back. Moves, hits and effects are better edited in the [Move Studio](../studio/README.md); Blender is for models and animations. While Blender is open, the Studio's **Open in Blender** button (an animation's inspector) imports the character from the mods folder if needed and shows that animation here; the add-on listens for it on 127.0.0.1:47612, this computer only.

## Set up

1. Edit › Preferences › Add-ons › Install from Disk, choose `blender/io_scene_dissidia.zip`, enable **Dissidia 012 Character Tools**. Remove an older version first.
2. In the 3D view sidebar (N) open the **Dissidia** tab and, in **Character Selector**, set the game ISO. It is only ever read.
3. In the add-on's preferences set **Character mods folder** to the repo's `mods` folder (or the repo itself).

## Workflow

1. **Character Selector:** pick a character. Mod characters come first (*Import from mod folder*), then the retail cast (*Import from ISO*; the files are extracted to `forge_work/<code>/` next to the ISO).
2. **Animations:** the list shows each animation with the moves that use it. Click one to make it active.
3. **Animate** with Blender's normal tools, or with the add-on's: **Build control rig** (FK/IK controls on top of the game skeleton), **Retarget from character…**, **Retarget from FBX/glTF…** (an animation from another game, Mixamo, Rigify...), **Add base body** (a plain low-poly humanoid to sculpt your own character from). A new animation is any Action of the character without a motion slot: **New animation (from…)** (under the Animations list, and in Moves & events) makes one as a copy of an existing animation, with a copy of its events, so you start from a pose and timing that already work; name it as you like (`_NNNA` is only the game's naming). On export it is appended after the game's motions; Save moves to folder, then point a press at it in the Move Studio.
4. **Save moves to folder** writes the animations into the mod folder's rig. Then build the character (**Build into the game**, or just start the game).

For retail characters there is also **Export model + moveset** and **Build patched ISO**, which writes a new ISO with your changes; the original is never changed.

## Character mods panel

With a mod character selected: switch its **skin**, give it **your own model and weapons** (any model file for the body and for each weapon it wants), make a **new character**, and **build into the game**.

## Files

- `io_scene_dissidia.zip`: the add-on to install (committed). After changing the add-on, rebuild it with `python3 blender/build_addon.py` (from the repo root) and bump the version in `io_scene_dissidia/__init__.py`.
- `io_scene_dissidia/dfx/`: the file-format library (GMO models, objx movesets, projectiles, textures, effects).
- `tests/`: `test_retarget_ext.py` runs with plain Python; `blender -b -P tests/test_retarget_blender.py` runs the rest.

## Everything else

Moves & events in Blender, the Character mods panel in detail, the control rig, adding bones and parts, the base body, and current limits: [docs/blender-addon.md](../psprecomp/games/dissidia/docs/blender-addon.md).
