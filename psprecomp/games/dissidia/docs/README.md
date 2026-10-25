# Dissidia 012 docs

Everything about the game itself, the native build and the modding tools, in one place. Paths in these docs are relative to `psprecomp/games/dissidia/` unless they say otherwise.

## Start here
- [The top-level README](../../../../README.md#install-and-play): install and play (Steam Deck, Linux PC). The install steps are written there only.
- [steamdeck.md](steamdeck.md): the Steam Deck in detail (`deck_setup.sh`, Steam entry, controls, troubleshooting).
- [native-build.md](native-build.md): what each setup step does, the work folder, every runtime option (window, scaling, input, camera).
- [loose-file-mods.md](loose-file-mods.md): how loose files replace `PACKAGE.BIN` entries, the model size budget, package index internals.
- [texture-packs.md](texture-packs.md): HD texture replacement: dumping, upscaling, the built-in upscaler.

## Making characters
Short guides next to the tools, and the full reference here:
- [../mods/README.md](../../../../mods/README.md): character mod folders. Reference: [character-mods.md](character-mods.md) (`mod.json`, moveset files, avatars, bodies, weapons, recipes, every event).
- [../studio/README.md](../../../../studio/README.md): Move Studio. Reference: [move-studio.md](move-studio.md) (every panel, the dummy, the effect editor).
- [../blender/README.md](../../../../blender/README.md): the Blender add-on. Reference: [blender-addon.md](blender-addon.md) (Moves & events, Character mods panel, control rig, base body).

## Reference
- [roster.md](roster.md): roster tables, extra charIds, the Quick Battle grid, main.bin records, `new_character.py`, model size budget, ability names.
- [moves-and-events.md](moves-and-events.md): how a move is defined, and what every timeline event op does.
- [formats.md](formats.md): disc layout, PACKAGE.BIN / PACKAGE_INFO.BIN, per-character files, effects (FEP), weapons.
- [spawned-objects.md](spawned-objects.md): the `wpnp` table and the objects `hold_prop` / `summon_object` spawn (Gilgamesh's swords, Yuna's aeons).
- [effects.md](effects.md): how the game draws effects (placement, timing, colour and blending, every component), and corrections to the FEP notes.
- [audio.md](audio.md): extra BGM tracks, voice banks.
- [recomp-status.md](recomp-status.md): how the native build came together, and soak-test findings.
- Custom effect shaders are an engine feature: [psprecomp/FX_SHADERS.md](../../../FX_SHADERS.md).
- Engine docs: [ARCHITECTURE](../../../ARCHITECTURE.md), [DEBUGGING](../../../DEBUGGING.md), [ENV_FLAGS](../../../docs/ENV_FLAGS.md), [GRAPHICS](../../../docs/GRAPHICS.md), [TROUBLESHOOTING](../../../docs/TROUBLESHOOTING.md).

## Archive
Dated investigations kept for the record:
- [archive/event-ops/](archive/event-ops/README.md): the per-op research behind moves-and-events.md (14 notes, 23 Oct).
- [archive/fx-components/](archive/fx-components/README.md): the research behind effects.md (7 notes, 24 Oct; G.md: 8888 glows, channel 111, the line / point emitters).
- [archive/hit-reaction/](archive/hit-reaction/README.md): how a hit moves the defender (reactions, knock-back, gravity, hit-stun, juggles; 24 Oct). Move Studio's dummy follows it.
- [archive/hit-contact/](archive/hit-contact/README.md): the hit contact test (spheres, kinds, sweep, conditions; 24 Oct).
- [archive/projectile-flight/](archive/projectile-flight/README.md): how projectiles spawn, fly, home and hit (24 Oct).
- [archive/attacker-movement/](archive/attacker-movement/README.md): how a move moves the fighter doing it (root motion, homing dashes, lunges, air physics, landing; 24 Oct).
- [archive/guard-dodge-walls/](archive/guard-dodge-walls/README.md): guard, the clash table and its outcomes, dodges, wall rushes (24 Oct). Move Studio's dummy follows it.
- [archive/ablt-fields/](archive/ablt-fields/README.md): the ability records' CP, AP, equip bonus and other fields (24 Oct).
- [archive/shared-sounds/](archive/shared-sounds/README.md): every sound bank on the disc, cue numbers, and which banks battle loads (24 Oct).
- [archive/noctis-gmo-analysis-2026-10-16.md](archive/noctis-gmo-analysis-2026-10-16.md): Noctis GMO vs Bartz GMO.
- [images/](images/): screenshots (first 3D battle, Noctis before/after, warp dash in game).

![Noctis warp dash in game](images/noctis-warp-dash-ingame.png)
