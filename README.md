# Dissidia 012 Duodecim: native port and modding

Dissidia 012 Duodecim (PSP) running natively on the Steam Deck and Linux, no emulator, plus tools for adding new characters. The native build is called **Dissidia Trēdecim**.

You need your own copy of the game as an `.iso` or `.cso`. The European PSN release (`ULES01505`) is the one it was built against; the US UMD (`ULUS10566`) works too.

**Put it at `~/dissidia/game.iso`**: make a `dissidia` folder in your home folder and copy the game into it, named `game.iso`. Every command below uses that path. A `.cso` can be named `game.iso` too; it is recognised by its contents.

## Install and play

### Steam Deck

In Desktop Mode (Steam button > Power > Switch to Desktop), first put your game at `~/dissidia/game.iso` (a folder called `dissidia` in your home folder, the file renamed `game.iso`; from a USB stick or SD card, copy it across in Dolphin). Then open Konsole and run:

```bash
cd ~/dissidia
git clone https://github.com/dissidia-fan/dissidia repo
repo/psprecomp/games/dissidia/tools/deck_setup.sh ~/dissidia/game.iso --install-distrobox --add-to-steam
```

The last command does everything: build container, extract and decrypt the game, compile it, add **Dissidia Trēdecim** to Steam. It takes a while (tens of minutes). Keep the Deck plugged in and set screen dimming and sleep to "never" (System Settings > Power Management). If it stops part-way, run it again; finished steps are skipped.

Then switch back to Game Mode and start **Dissidia Trēdecim** from your library.

To update later: `~/dissidia/repo/psprecomp/games/dissidia/tools/deck_setup.sh ~/dissidia/game.iso --update`

Controls, display options, troubleshooting: [docs/steamdeck.md](psprecomp/games/dissidia/docs/steamdeck.md).

### Linux PC

Ubuntu/Debian; about 8 GB of disk needed. First put your game at `~/dissidia/game.iso` (a folder called `dissidia` in your home folder, the file renamed `game.iso`). Then, in a terminal:

```bash
git clone https://github.com/dissidia-fan/dissidia ~/dissidia/repo
~/dissidia/repo/psprecomp/games/dissidia/tools/install.sh ~/dissidia/game.iso
```

The last command does everything: installs the build tools (it asks for your password for `apt`) and Rust, extracts and decrypts the game, and compiles it. It takes a while (tens of minutes; add `--jobs 2` if it runs out of memory). If it stops part-way, run it again; finished steps are skipped. Then play with `~/dissidia/work/play.sh`.

To update later: `~/dissidia/repo/psprecomp/games/dissidia/tools/install.sh ~/dissidia/game.iso --update`

Gamepads work out of the box. Keyboard: arrows, X/Z/A/S for the face buttons, Q/W shoulders, Space = Start, Tab = Select. Saves are in `~/dissidia/work/ms0/PSP/SAVEDATA/`.

### Mac

Needs [Homebrew](https://brew.sh); about 8 GB of disk. First put your game at `~/dissidia/game.iso` (a folder called `dissidia` in your home folder, the file renamed `game.iso`). Then, in Terminal:

```bash
git clone https://github.com/dissidia-fan/dissidia ~/dissidia/repo
~/dissidia/repo/psprecomp/games/dissidia/tools/install.sh ~/dissidia/game.iso
```

The last command does everything: installs the build tools with Homebrew and Rust, extracts and decrypts the game, and compiles it. If Apple's command line tools are missing it opens their installer first; run the command again when that finishes. It takes a while (tens of minutes). If it stops part-way, run it again; finished steps are skipped. Then play with `~/dissidia/work/play.sh`.

To update later: `~/dissidia/repo/psprecomp/games/dissidia/tools/install.sh ~/dissidia/game.iso --update`

Gamepads work out of the box. Keyboard: arrows, X/Z/A/S for the face buttons, Q/W shoulders, Space = Start, Tab = Select. Saves are in `~/dissidia/work/ms0/PSP/SAVEDATA/`.

Character mod folders in `mods/` are built and loaded every time the game starts. What each step does and every runtime option: [docs/native-build.md](psprecomp/games/dissidia/docs/native-build.md).

## What is not in this repository
No game data (bring your own ISO), no character mods (`mods/_template/` shows the layout), no texture packs (the loader works with the community packs credited below), no third-party mod ports, no game artwork.

## Making characters

A character is one folder in `mods/`: its menu pictures, moves and models as files you can read and edit. You need the native build working first (above), since added characters only run there; on a PSP or in PPSSPP only changes to the game's own characters work.

1. **Start in the Dissidia Move Studio.** Open `studio/index.html`, set the mods folder (the repo's `mods`) and your game ISO, then pick **+ New character…** at the top of the character list. Give them a name, a series and the retail character it is built on (its skeleton, animations, voice and moves are the starting point), and optionally a full-body picture for the menus.
2. **Make their moves** in the Studio: copy moves from any retail or mod character, place events on the timeline, tune each hit's damage and knock-back, edit the effects, and press **Save**. The **Character** tab sets the name, slot and how the menu pictures are cropped.
3. **Give them a body and animations** in Blender with the add-on: import the character from its mod folder, model or retarget, then **Save moves to folder**. Back in the Studio, **Reload** and point moves at the new animations.
4. **Play.** Start the game: every folder in `mods/` is built in before it runs, and the new character is on the Quick Battle grid. Deleting a folder (or `"enabled": false` in its `mod.json`) takes the character out again.
5. **Share** by zipping the folder. It holds data only, no scripts, and whoever gets it unzips it into their own `mods/`; anything it borrows from the retail game is read from their own disc.

To change one of the game's own characters instead, pick them in the Studio (with the ISO set), edit, and press **Save as mod**.

Up to 6 characters can be added: the character select screens have no room for a 7th for now.

More detail:
- [mods/README.md](mods/README.md): what is in a character folder, the command-line tools, sharing.
- [studio/README.md](studio/README.md): the Move Studio, panel by panel.
- [blender/README.md](blender/README.md): installing and using the Blender add-on.
- [docs/](psprecomp/games/dissidia/docs/README.md): everything else (roster, formats, events, audio, how the native build was made).

## Layout
`psprecomp/` is a fork of [PSPRecomp](https://github.com/wizardengineer/psprecomp) (static recompiler + runtime). Everything Dissidia-specific is in `psprecomp/games/dissidia/`.

Findings in the docs carry a confidence (confirmed / inferred / medium / low); inferred ones are educated guesses from disassembly, not tested facts. Corrections welcome. The project was developed with heavy use of an AI assistant (Claude, by Anthropic), which did most of the reverse engineering and wrote most of the code, under my direction and testing.

## Credits
- [PSPRecomp](https://github.com/wizardengineer/psprecomp): the recompiler and runtime this is forked from (GPL-2.0).
- [PPSSPP](https://github.com/hrydgard/ppsspp): VFPU maths (`psprecomp/runtime/src/vfpu_ppsspp`, GPL-2.0+), and the emulator used for every comparison and trace.
- [DissDlcToolkit](https://github.com/adriangl/DissDlcToolkit) (adriangl): the object-table format and the DLC route.
- [dissidia-012-sora](https://github.com/MatNyaga/dissidia-012-sora) (MatNyaga): notes on effect-package ids and CPU AI.
- D012FFRT and [LiinkPK/Dissidia-012-HD-Textures](https://github.com/LiinkPK/Dissidia-012-HD-Textures) (LiinkPK, ajb3art): HD texture packs. Not included.
- [atracdenc](https://github.com/dcherednik/atracdenc): ATRAC3 encoding for new BGM.
- Fonts: Cinzel and Crimson Text (SIL Open Font License).
- Everyone in the Dissidia modding community without whom this work could never have happened.

## Licence
`psprecomp/` is GPL-2.0 (see `psprecomp/LICENSE`), and so are the changes made to it here. Dissidia 012 Duodecim and Final Fantasy are trademarks of Square Enix; this project is not affiliated with or endorsed by Square Enix.
