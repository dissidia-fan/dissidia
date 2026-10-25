# Character mods

One folder per character, holding everything it adds to the game as files you can read and edit. Drop a folder in here and it is in the game: `tools/run.sh` builds every folder before the game starts, and a new character gets its own roster slot. Nothing to run by hand.

## The short version

- **Make or edit a character** in the [Move Studio](../studio/README.md): open `studio/index.html` in Chrome, point it at this folder, and save. Then start the game.
- **Models and animations** are made in Blender with the [add-on](../blender/README.md).
- **Install someone else's character:** unzip their folder into `mods/` and start the game. Everything it borrows from the retail game is read from your own disc.

## A character folder

```
mods/<name>/
  mod.json            name, series, slot, which body, move names, runtime settings
  avatars/            every menu and HUD picture, as PNGs
  moveset/p_<code>/   the moves: commands, timelines, hits, projectiles (JSON)
  moveset/p_<code>_a/ the assist's moves
  models/             the rig: skeleton, animations, weapon and effect parts
  skins/<skin>/       bodies and weapons that replace the default ones
```

A folder with `"kind": "retail"` in its `mod.json` changes one of the game's own characters instead of adding one (the Move Studio's *Save as mod* makes those). A folder whose name starts with `_`, or whose `mod.json` has `"enabled": false`, is never built. `_template/` is the template.

## By hand

```
tools/charmod.py list                    # what's here
tools/charmod.py build noctis            # build one folder into work/mods
tools/charmod.py new terra --like six100 --display Terra --series "FINAL FANTASY VI"
tools/charmod.py avatars noctis --from-picture my_art.png     # cut all menu pictures from one render
tools/charmod.py body noctis             # which body is used, which skins exist
tools/charmod.py hits noctis             # every hit of every move
```

`--like` is the retail character a new one starts from (`fiv100` Bartz, `eht100` Squall, `sev100` Cloud, `six100` Terra, `gst130` Lightning...). Its skeleton, animations, voice and moves are the starting point. Deleting a folder (or setting `"enabled": false` in its `mod.json`) takes the character out of the game at the next start.

**How many fit.** The character select screens have room for 31 more pictures. A character takes 5 (grid tile, slide-in, its copy for the setup panel, name plate, logo), or 4 when its series is one the game already has a logo for (FINAL FANTASY I to XIII) or another added character of the same series has one to share. That is 6 to 7 characters. One that doesn't fit would push out pictures the screens need (the Enter Battle button went missing), so the build leaves it out and says so; delete or disable another to make room. A folder with a logo picture of its own (`avatars.generated_from.logo`, or its `logo` avatar marked `"_own": true`) keeps its own plate.

## Sharing

A mod folder is data only: JSON, models, skins, pictures, sounds. The build refuses a folder with a script or program in it, so a shared mod can't carry code. Zip `mods/<name>/` and send it. Don't share `.blend` files to install (Blender can run scripts saved in them), and never run a script that came with a mod; nothing here needs one.

## Everything else

Field by field: `mod.json`, the moveset files, every avatar, bodies, weapons (the model and weapons a character wants), every timeline event and how a move fits together: [docs/character-mods.md](../psprecomp/games/dissidia/docs/character-mods.md). What every event does in the game: [docs/moves-and-events.md](../psprecomp/games/dissidia/docs/moves-and-events.md).
