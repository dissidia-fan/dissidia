# New character

Made from `mods/_template` by `charmod.py new`. Everything you don't replace is still the based-on
character's, so it already plays.

1. **Name**: `mod.json` → `character` (`name`, `full_name`, `series`).
2. **Pictures**: `charmod.py avatars <folder> --from-picture my_art.png` (a full-body render on a transparent
   background works best), then look at `avatars/_preview.png`. Or drop single PNGs in `avatars/` and set their
   `"picture"` in `mod.json`.
3. **Moves**: `moveset/p_<code>/`: combos in `moves.json`, timing in `timelines/NNN.json`, damage in `hits.json`.
   `charmod.py hits <folder>` lists every hit of every move.
4. **Move names**: `mod.json` → `move_names` (see `_example`).
5. **Body**: `charmod.py body <folder> --from-gmo my_model.gmo --as mine` (a model on the same skeleton).
6. **Play**: start the game with `tools/run.sh`; it adds the character to the roster the first time.

Full guide: `mods/README.md`.
