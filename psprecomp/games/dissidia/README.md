# Dissidia 012 Duodecim: the native build

The Dissidia-specific part of the PSPRecomp fork: manifests, runtime hooks, the setup and build scripts, the modding tools, the Blender add-on and the Move Studio. No game data is included.

**Installing and playing** is in the [top-level README](../../../README.md#install-and-play). Everything else is in [docs/](docs/README.md).

## What's here

| Path | What |
|---|---|
| `tools/setup_from_iso.py`, `tools/pipeline.sh`, `tools/run.sh` | unpack your ISO, build the game, run it |
| `tools/deck_setup.sh` | the Steam Deck one-shot (container + everything above + a Steam entry) |
| `tools/charmod.py`, `tools/new_character.py` | build character mod folders; add or remove a roster slot |
| `mods/` | character mod folders ([README](../../../mods/README.md)) |
| `studio/` | Move Studio: moves, hits, effects in the browser ([README](../../../studio/README.md)) |
| `blender/` | the Blender add-on: models and animations ([README](../../../blender/README.md)) |
| `runtime/` | game-specific runtime hooks (overlays, loose-file mods, roster, camera, input, save dialog) |
| `*.toml`, `analysis/`, `package_paths.txt` | recompiler manifests and the shipped analysis (no game code) |
| `docs/` | all the documentation, indexed in [docs/README.md](docs/README.md) |

The tools need Python 3.11+ and nothing else: the first run sets up numpy, Pillow and scipy at pinned versions in `tools/.venv/` (one 60 MB download).

## How it works, briefly

1. `setup_from_iso.py` reads your ISO, copies what the game uses to `<workdir>/disc0`, decrypts `EBOOT.BIN` and makes an emulated Memory Stick in `<workdir>/ms0`.
2. `pipeline.sh` translates the game to C++ and compiles it with the runtime into `<workdir>/build/psprecomp_runtime`. Re-running is safe.
3. `run.sh` starts it. First it builds every folder in the repo's `mods/` into `<workdir>/mods` (`PSPRECOMP_CHARMOD=0` skips that). Files there replace the matching entries of the game's `PACKAGE.BIN` at load time, so there is never an ISO rebuild.

Details: [the native build](docs/native-build.md) (each step, the work folder, window and scaling, input, camera), [loose-file mods](docs/loose-file-mods.md) (how overrides work, model budget), [texture packs](docs/texture-packs.md).

## Status

Playable end to end on Linux and the Steam Deck: menus, story mode with its movies, and battles. Built against the European PSN image; the US UMD works with less testing. Limitations and what is untested: [docs/steamdeck.md](docs/steamdeck.md).
