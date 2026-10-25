# Dissidia 012 (native build) on the Steam Deck

Status: the game is playable end to end (menus, story mode with its movies, 3D battles); what is still off is
under "Known limitations". **This guide has never been run on a Deck**: it was written and checked on a headless
x86-64 Linux box (no Deck, no podman, no distrobox, no ISO). Every step that touches Deck hardware or SteamOS is
listed again under "Known unknowns" at the end; read that list before you start so a failure there does not
surprise you. If something breaks, `deck_setup.sh --check` and the log `~/dissidia/work/last_run.log` are what to
attach to a bug report.

You need your own copy of the game: an `.iso`, or a `.cso` (read directly, no conversion). ZSO, DAX,
raw 2352-byte `.bin` dumps and PSN `PBP` packages are not supported (convert with e.g.
`maxcso --decompress`). No game data is distributed with this repository. The tooling targets the
European PSN image (`ULES01505`); the US UMD (`ULUS10566`) has the same code and works too, with less testing.
The setup script tells you if your disc is a different one.

## The short version

The steps are in the top-level README: [Steam Deck](../../../../README.md#steam-deck). In short, in Desktop Mode, in
Konsole: clone the repo, put your game at `~/dissidia/game.iso`, run `deck_setup.sh ~/dissidia/game.iso --check`, then
`deck_setup.sh ~/dissidia/game.iso --install-distrobox --add-to-steam`.

The install step creates a build container, installs the compilers and libraries inside it, extracts and decrypts your
ISO, rebuilds the analysis from the shipped metadata (no Ghidra), compiles the game, writes
`~/dissidia/work/play.sh` and (with `--add-to-steam`) puts it in the Steam library as "Dissidia Trēdecim",
with its own library artwork. The compile alone takes a long time;
nobody has timed it on a Deck. For scale: on a 2-core x86-64 VM sharing its CPU with other builds, the
recompiler (cargo) plus the whole runtime took about 40 minutes from a clean work directory with a mostly cold
ccache; the Deck's 4-core/8-thread Zen 2 APU should be in the same range, and repeats take minutes. Leave the Deck plugged in, set Desktop Mode's screen
dimming/sleep to "never" (System Settings > Power Management), and keep it awake.

If it stops half-way (battery, sleep, SteamOS update, Ctrl+C): **run the same command again**. Finished steps
are skipped and the C++ build resumes where it stopped. See "Resuming" below.

Test from Desktop Mode: `~/dissidia/work/play.sh` (opens full screen; the Steam Deck's buttons act as
keyboard/mouse there, see "Controls", so use a USB/Bluetooth pad or the keyboard for this test, or go straight
to Game Mode). Then see "Add it to Steam".

Disk: about 1 GB for the ISO, 1.6 GB for the extracted data, 0.5 GB for generated sources and the build,
1.5-3 GB for the container and Rust, a few hundred MB for the Rust build of the recompiler. Budget 8 GB
free; `--check` insists on 9. Compile jobs default to 3 (about 1.5 GB of RAM each on the Deck's 16 GB;
`--jobs 2` if you leave other things open).

## What `deck_setup.sh` does, and `--check`

```
deck_setup.sh <game.iso|.cso> [workdir] [--check] [--dry-run] [--install-distrobox] [--add-to-steam]
                              [--preset retro|crisp] [--jobs N] [--container NAME] [--image IMAGE]
                              [--redo container|packages|rust|extract|all] [--launcher-only]
```

`--check` prints PASS / WARN / FAIL lines and exits non-zero on a FAIL. It tests: SteamOS and Valve hardware
(informational), not root, x86-64, RAM and free RAM, a GPU render node, that ISO / workdir / checkout are under
`$HOME` and contain no spaces, free disk (workdir and `$HOME`), that the ISO really is an ISO9660 or CSO
image, that the shipped analysis files are present, `podman` is installed and `podman info` works, `/etc/subuid`
and `/etc/subgid` have entries for you (rootless containers need them), distrobox is installed (or will be
by `--install-distrobox`) and is recent enough, network reachability of the five hosts the build uses
(docker.io, github.com, sh.rustup.rs, index.crates.io, archive.ubuntu.com), whether the host has `python3` and
a Steam account folder (for `--add-to-steam`), which steps are already done, and, if the container exists, whether the toolchain is in it. It starts
the container if one exists; otherwise it changes nothing. `--dry-run` prints the commands without running them.
A normal run starts with the same check and stops on a FAIL.

The steps, in order (each marked done in `<workdir>/.deck_setup/` when it completes):

1. **container**: `distrobox create --name dissidia --image docker.io/library/ubuntu:24.04`. The image name is
   fully qualified because SteamOS' podman is not guaranteed to have a default search registry.
2. **packages**: `apt-get install` inside the container: `build-essential cmake pkg-config git curl
   ca-certificates unzip python3 python3-venv ccache fontconfig libsdl2-dev libfreetype-dev libavcodec-dev
   libavutil-dev libgl-dev libgl1-mesa-dri libegl1 mesa-utils zlib1g-dev libssl-dev fonts-dejavu-core`. ccache is
   capped at 3 GB. `zlib1g-dev` and `libssl-dev` are for `pspdecrypt` (decrypts `EBOOT.BIN`); `libavcodec-dev` gives
   you music (ATRAC3 decode) and `mesa-utils` gives `glxinfo` for troubleshooting; `python3-venv` is for step 6.
3. **rust**: `rustup` (minimal profile) inside the container. The Ubuntu `rustc` is too old, hence rustup.
4. **extract**: `setup_from_iso.py` unpacks `PSP_GAME/USRDIR`, `SYSDIR`, decrypts the EBOOT (it clones and builds
   https://github.com/John-K/pspdecrypt on first use).
5. **pipeline.sh**: `cargo build --release` for the recompiler, rebuilds the analysis from
   `games/dissidia/analysis/*.json.gz` (SHA-256 checked against your files), recompiles the EBOOT and overlays,
   configures and builds the runtime (`<workdir>/build/psprecomp_runtime`).
6. **Python packages**: `tools/pydeps.py` puts numpy, Pillow and scipy, at the versions in `tools/requirements.txt`,
   in `games/dissidia/tools/.venv/` (one download, about 60 MB). `run.sh` builds the character mod folders with
   them at every start; setting them up here means the first start from Game Mode does not wait for the download.
7. **launcher**: writes `<workdir>/play.sh` (below).

`--redo packages` (etc.) forgets a step; `--redo all` forgets all of them. `--launcher-only --preset crisp`
rewrites just `play.sh`. Changing the package list in a later version of the script reruns step 2 automatically.

### Resuming

* Steps 1-4 are stamped; stamps live in `<workdir>/.deck_setup/`.
* `setup_from_iso.py` skips files that are already extracted.
* The Python packages step checks the environment every time and downloads only when it is missing or
  `tools/requirements.txt` changed.
* `pipeline.sh` skips the recompile + overlay generation when a fingerprint of its inputs (analysis files, the
  recompiler binary, `game.toml`, overlay manifests) matches `<workdir>/.recompile.stamp` (`--force` to redo).
* `cmake --build` continues from the objects already compiled. An interrupted compile may leave one half-written
  object; make rebuilds anything that does not match.

## Updating (new code and mods from the Mac)

The repository also tracks the installed mods (`psprecomp/work/mods`), so one command brings both:

```bash
~/dissidia/repo/psprecomp/games/dissidia/tools/deck_setup.sh ~/dissidia/game.iso --update
```

It runs `git pull --ff-only` in the checkout, then the usual steps: only what changed is recompiled, and `<workdir>/mods` is linked to the
checkout's mods folder if it does not exist yet. If `<workdir>/mods` is a folder of its own (copied by hand
earlier), the script leaves it alone and prints the two commands that swap it for the link.

## Why a container, and why the game also runs in it

* SteamOS has a read-only root filesystem and system updates replace it. `sudo steamos-readonly disable`
  plus `pacman` works until the next update wipes it and breaks the pacman keyring; not recommended, and
  this script never does it.
* Flatpak is for running apps, not for compiling against `-dev` libraries.
* A **distrobox** container (a rootless podman container sharing your home directory, `/dev` and the
  display/audio sockets) survives updates and needs nothing on the host. The C++ runtime is built in it.
* Ubuntu 24.04 rather than Arch: the dependency list was only ever built on Ubuntu 24.04 (Debian package names,
  a recent enough Mesa/SDL2/ffmpeg), and rustup works the same anywhere. An Arch image would only save you the
  glibc difference with the host, which does not matter because the game runs in the container as well.
* The built runtime is dynamically linked against the container's libraries, including ffmpeg
  (`libavcodec.so.60`, from Ubuntu 24.04) and a long tail behind it. SteamOS is Arch-based with a newer
  ffmpeg (different sonames), so copying the binary out to run on the bare host fails with
  "library not found". So the launcher runs the game inside the container too (`play.sh` does
  `distrobox enter dissidia -- run.sh ...`). Recommended and what `deck_setup.sh` does.
* Not chosen: a fully static or AppImage-style build (would need static SDL2/ffmpeg/FreeType and a
  dlopen story for audio and GL; not done), and building inside Valve's Steam Linux Runtime "sniper"
  SDK image (the Valve-blessed way to make portable binaries; plausible future route, untried).

Requirements on the Deck: `podman` (shipped with current SteamOS; verify with `podman --version`) and
`distrobox`. If `distrobox` is missing, `--install-distrobox` downloads the official install script from
`raw.githubusercontent.com/89luca89/distrobox` and installs into `~/.local/bin` (no root). If you prefer
to read the script first, install distrobox yourself (https://distrobox.it) and omit the flag. (Whether your
SteamOS version already ships distrobox is not confirmed here; `--check` tells you.) distrobox lives in `~/.local/bin`, which
is not on the default `PATH` of Game Mode launches; `play.sh` adds it itself.

## The launcher: `play.sh`

`play.sh` is a host-side script, so Steam can run it without knowing about containers. It:

1. unsets `LD_PRELOAD` / `LD_LIBRARY_PATH` (Steam fills them with its overlay and runtime libraries; the host's
   `podman` must not load them),
2. writes the display, audio, gamescope and **Steam Input** environment variables (`SDL_*`, `DISPLAY`,
   `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `STEAM*`, `PSPRECOMP_*` ...) to `<workdir>/.play_env` and sources them in
   the container, because `distrobox enter` does not forward everything. The important one is Steam Input's
   `SDL_GAMECONTROLLER_IGNORE_DEVICES`, which hides the Deck's raw controller so the game only sees Steam's
   virtual Xbox-style pad (otherwise it could open both),
3. runs `distrobox enter dissidia -- run.sh <workdir> --retro --fullscreen "$@"` (with `--no-tty` when your
   distrobox has it; Steam gives the script no terminal).

### Choosing the look: `--retro` (default) or `--crisp`

The game is a 2011 PSP title: 480x272, low-poly models, small textures. The two presets are one flag each:

| | `--retro` (default in `play.sh`) | `--crisp` |
|---|---|---|
| 3D resolution | 2x PSP = 960x544 | 3x PSP = 1440x816 |
| Textures | nearest-neighbour (blocky, as drawn); texture packs and the upscaler are switched off | what the game asks for (mostly bilinear); texture packs stay enabled |
| Fit to the 1280x800 panel | whole-number scale: 960x544 centred, 160 px bars left/right, 128 px top/bottom; the 960x544 render is shown 1:1, no resampling | aspect fit 1280x725 (37 px bars top/bottom), bilinear downscale from 1440x816 |
| Aspect | 480:272 kept, never stretched | 480:272 kept |
| Run.sh flags it stands for | `--render-scale 2 --tex-filter nearest --integer-scale` and `PSPRECOMP_TEX_REPLACE=0` | `--render-scale 3 --filter linear` |

* Per run: `play.sh --crisp` (the last of `--retro`/`--crisp` wins). In Steam: Properties > Launch Options > `--crisp`.
* Permanently: `deck_setup.sh <iso> --launcher-only --preset crisp`, or edit the `PRESET=` line at the top of
  `play.sh`.
* Mix and match: explicit flags beat presets, e.g. `play.sh --retro --render-scale 3` (retro textures at 3x), or
  `--retro --stretch` (fill the whole panel, distorts), or `--retro --tex-filter game`.
* `--retro` draws the 3D at 2x resolution, so polygon edges are cleaner than on a PSP, while textures, HUD and
  geometry stay as they were. If you want the exact PSP pixel grid use `--retro --render-scale 1` (shown at 2x).
* The battery-friendliest setting is `--retro` (about 4x the PSP's pixels); `--crisp` is about 9x.

Outside Steam, a plain `run.sh` accepts the same flags: `run.sh ~/dissidia/work --retro --fullscreen`
(inside the container on the Deck; on a normal Linux PC just run it).

## Add it to Steam (Game Mode)

In Desktop Mode (Konsole):

```
~/dissidia/repo/psprecomp/games/dissidia/tools/deck_setup.sh --launcher-only --add-to-steam
```

(or add `--add-to-steam` to a full or `--update` run). `steam_shortcut.py` then, for every Steam account on the
Deck:

* adds a non-Steam shortcut to `~/dissidia/work/play.sh` named **Dissidia Trēdecim**, or renames the entry that is
  already there for this launcher (an older `play.sh` / `run.sh` / `run.py` / "Dissidia 012 (native)" entry, or any
  entry pointing into `~/dissidia`), keeping its play time, collections and Launch Options; duplicates are removed.
  The first change backs up `shortcuts.vdf` to `shortcuts.vdf.before-tredecim` (same folder);
* copies the library artwork from `games/dissidia/steam/` into Steam's `userdata/<account>/config/grid/` under the
  shortcut's app id: `capsule.png` (600x900, library shelves), `wide.png` (920x430, recent games), `hero.png`
  (1920x620, top of the game page), `logo.png` (drawn over the hero, pinned bottom left) and `icon.png` (256x256).

Steam keeps its shortcut list in memory and writes it back when it exits, so if Steam is running the script asks to
close it (`steam -shutdown`), makes the change and starts Steam again. It refuses to run from Game Mode.
Re-running it is harmless (it updates the same entry); after new artwork is pulled, run the same command again.

Then: controller (below), return to Game Mode (the "Return to Gaming Mode" icon on the desktop) and start
Dissidia Trēdecim from the library. Do not force a Proton compatibility tool on it; this is a native Linux program.
Launch Options (`--crisp` etc.) are set in the entry's Properties as before.

By hand instead: Desktop Mode, Steam: Games > Add a Non-Steam Game to My Library > Browse, filter "All files", pick
`~/dissidia/work/play.sh`; then Properties > name it, and set the pictures with right-click > Manage > Set custom
artwork (the files in `games/dissidia/steam/`).

The artwork is the game's own: `steam_art.py` takes the PSP's XMB background (`PIC1.PNG`, the crystal sea),
paints out its "Duodecim 012" logo and sets the DISSIDIA / TRĒDECIM title over it in Cinzel (SIL OFL,
`games/dissidia/steam/fonts`). It is drawn once on the Mac and the results are committed, so the Deck needs no
Pillow. To redraw:

```
python3 games/dissidia/tools/steam_art.py --pic1 work/disc0/PSP_GAME/PIC1.PNG
```

The game window is created, then switched to borderless desktop fullscreen. Under Game Mode's gamescope it should
fill the 1280x800 screen with the presets' own black bars. Leave Steam's per-game "Scaling"/"Game
Resolution" options at their defaults: the game does its own scaling. If the picture ends up smaller than
expected or blurry, try Properties > Game Resolution = 1280x800 / Scaling = Auto.

## Controls

The runtime reads the keyboard and **SDL game controllers** (all connected pads, hot-plug aware).
In Game Mode, Steam gives a non-Steam game a virtual Xbox 360-style controller made from the Deck's buttons
through Steam Input. SDL ships a mapping for that device, so the mapping below applies without a
`gamecontrollerdb`.

| PSP | Gamepad (SDL names) | Keyboard |
|---|---|---|
| D-pad | D-pad | Arrow keys |
| Left analogue stick | Left stick (8% deadzone) | Arrow keys (full deflection) |
| (none on a PSP) camera | Right stick: free camera in battle, D-pad in menus (20% deadzone) | - |
| Cross | A (bottom) | X or Enter |
| Circle | B (right) | Z |
| Square | X (left) | A |
| Triangle | Y (top) | S |
| L / R | L1 / R1 (bumpers), or L2 / R2 (triggers) past 50% | Q / W |
| Start / Select | Menu (Start) / View (Back) | Space / Tab |

This is the same layout the Mac build uses (confirmed there with a physical controller: everything mapped
correctly). On the Deck the physical buttons are labelled the same way (A bottom, B right, X left, Y top). Back
paddles (L4/R4/L5/R5), trackpads, gyro and the Steam/QAM buttons are not read by the game; bind them in Steam
Input to any of the buttons above if you want them.

* `run.sh --swap-ab` (`PSPRECOMP_SWAP_AB=1`) swaps Cross/Circle on the pad (A becomes Circle, B Cross): for
  Nintendo-style layouts or if you want confirm on the right-hand button. In `play.sh` add it as a Launch Option.
* The right stick turns the battle camera freely (the PSP only had the D-pad for this): yaw and pitch are an
  offset on top of the game's own camera, so lock-on and auto-centring still work. `PSPRECOMP_CAM_SENS=1.5` speeds
  it up, `PSPRECOMP_CAM_INVERT_Y=1` inverts pitch, `PSPRECOMP_CAM_RECENTER=3` lets the view drift back in 3 s when the
  stick is idle, `PSPRECOMP_CAM_FREE=0` switches to the plain "right stick = D-pad" behaviour (the game's own d-pad
  camera). In menus the right stick is always a D-pad. `PSPRECOMP_RSTICK_DPAD=0` disables that conversion (the
  stick then does nothing in menus), `PSPRECOMP_RSTICK_DEADZONE=0.3` widens the dead zone.
  Environment variables set in Steam's Launch Options as `PSPRECOMP_CAM_SENS=1.5 %command%` do not work here
  (the entry is a script, `%command%` is the script); export them in `play.sh` above the `exec` line instead.
* Movement is fully analogue: the game itself only has 8 stick directions (a 78% per-axis dead zone); the port
  replaces that with a radial dead zone (`PSPRECOMP_ANALOG_DEADZONE`, default 0.20) so any angle works.
  `PSPRECOMP_ANALOG_FULL=0` brings back the original 8-way movement. The Deck's sticks have a hardware dead zone
  of their own plus Steam Input's; if drift is a problem raise the radial one.
* `PSPRECOMP_GAMECONTROLLERDB=/path/gamecontrollerdb.txt` (or `run.sh --gamecontrollerdb FILE`, or just
  drop the file in the work directory as `gamecontrollerdb.txt`) loads extra SDL mappings for pads SDL
  does not know. Community file: https://github.com/mdqinc/SDL_GameControllerDB.
* Diagnosing input: the log (`~/dissidia/work/last_run.log`) has, for every pad, `[RT] Controller opened: <name>
  (id, guid ...)` followed by its SDL mapping string. `PSPRECOMP_PAD_LOG=1` additionally logs every button as
  `[PAD] <sdl name> down -> psp 0x....` (export it in `play.sh`). On the Deck the expected pad name is "Xbox 360
  Controller" (Steam's virtual device); if it says "Steam Deck" or lists two controllers, Steam Input is not
  filtering the raw device (see Known unknowns).
* **Desktop Mode** has its own Steam Input profile: the Deck's buttons act as keyboard and mouse there, and a
  game started from a terminal gets no virtual pad. A game started *from Steam* (even in Desktop Mode) uses the
  game's controller layout instead. So test the pad via Steam, not by running `play.sh` in Konsole.
* Check the layout: in Game Mode press the Steam button > the entry's controller icon (or Desktop Mode: right-click
  the entry > Properties > Controller). Choose the template **Gamepad** (not "Keyboard/mouse" or "Desktop"). With
  it, the D-pad, sticks, A/B/X/Y, bumpers and triggers become the Xbox buttons in the table.
* Fallback if the container does not see the Steam virtual pad: in the entry's Steam Input layout bind the
  Deck's buttons to the keyboard keys of the right-hand column. There is no keyboard quit key; leave with
  the Steam button > Exit Game, or close the window.

## Window size and scaling (other options)

`play.sh` always passes `--fullscreen` (borderless desktop fullscreen). F11 / Alt+Enter leaves it. Run `run.sh`
directly for a window: it picks the largest integer scale that fits (2x = 960x544 on the Deck). Options:
`--scale N`, `--window WxH`, `--fullscreen`, `--stretch`, `--filter nearest|linear|integer`,
`--integer-scale`, `--tex-filter nearest|linear|game`, `--render-scale N|auto`
(`PSPRECOMP_SCALE`, `PSPRECOMP_WINDOW`, `PSPRECOMP_FULLSCREEN`, `PSPRECOMP_STRETCH`, `PSPRECOMP_FILTER`,
`PSPRECOMP_INTEGER_SCALE`, `PSPRECOMP_TEX_FILTER`, `PSPRECOMP_RENDER_SCALE`).

How the picture is placed (`present_layout` in `runtime/src/psp_ge_draw.cpp`, logged as `[PRESENT]` in the log):

* default: aspect fit 480:272 into the drawable (1280x725 on the Deck), nearest if the fit happens to be a whole
  multiple, bilinear otherwise;
* `--integer-scale` / `PSPRECOMP_INTEGER_SCALE=1`: the largest whole multiple of 480x272 that fits, centred
  (1280x800: 2x = 960x544); the final blit is nearest-neighbour whenever the render target is a whole multiple of it
  (or the reverse), bilinear otherwise (for example render scale 2 on a 1920x1080 TV: 3x = 1440x816 output is 1.5x
  the target, so it uses bilinear);
* `--filter integer`: the same placement, always nearest;
* `--stretch`: fill the window (distorts the aspect).

The renderer uses the batched GPU-skinning path (needs only OpenGL 3.3 core; on the Deck's Mesa radeonsi it
should get a 4.6 core context). Ring vertex buffers total 12.5 MB, render targets at scale 3 about 66 MB, both from
shared memory. `run.sh --legacy-ge` (`PSPRECOMP_GE_FAST=0`) selects the old CPU-skinning renderer as an escape hatch.
Game textures are not upscaled unless you ask (texture packs, `PSPRECOMP_TEX_UPSCALE`, see the README), so
surfaces are as detailed as the PSP's.

## Saves, and the other variables

Run-time environment (all set by `run.sh`; you normally touch none of them):

| Variable | Meaning |
|---|---|
| `PSPRECOMP_DISC0` | extracted disc, default `<workdir>/disc0` |
| `PSPRECOMP_MS0` | emulated Memory Stick, default `<workdir>/ms0`. **Saves are in `ms0/PSP/SAVEDATA/`**; back that up. Created automatically. |
| `PSPRECOMP_CROSS_MID=1` | read by the recompiler only (`pipeline.sh` sets it); the runtime ignores it. `run.sh` exports it anyway, harmlessly. |
| `PSPRECOMP_CLEANROOM` | not used; not set. |

Useful extras: `PSPRECOMP_OSK_TEXT="NAME"` (what the auto-completed on-screen keyboard types),
`PSPRECOMP_FONT` / `PSPRECOMP_FONT_SCALE` (TrueType font for in-game text; DejaVu Sans is found
automatically), `PSPRECOMP_AUDIO=0`. The runtime log goes to `<workdir>/last_run.log`; attach it to bug
reports. Most other `PSPRECOMP_*` variables in the source are developer diagnostics.

## Troubleshooting

* `deck_setup.sh --check` first. It names the missing piece.
* `podman info` fails or "cannot set up namespace / newuidmap / no subuid": rootless podman needs entries in
  `/etc/subuid` and `/etc/subgid` for your user. SteamOS normally has them; if `--check` warns about them,
  that is the place to look (fixing them needs root and a read-only-root toggle, which this guide does not
  recommend; ask on a SteamOS forum).
* `distrobox create` fails on the Ubuntu image (for example a user/UID conflict with the image's `ubuntu`
  user): re-run with `--image quay.io/toolbx/ubuntu-toolbox:24.04`, a distrobox-friendly Ubuntu 24.04 image.
  After a failed create run `distrobox rm --force dissidia` first.
* `run.sh` prints "missing shared libraries": you are running outside the container that built it. On the Deck
  use `play.sh`.
* "disc0 is incomplete" / "hash differs": re-run `setup_from_iso.py` (it resumes); for a different
  region use Ghidra analysis.
* `setup_from_iso.py` "building pspdecrypt failed": install `build-essential zlib1g-dev libssl-dev` in the
  container, or use `--decrypted-eboot` (an EBOOT that PPSSPP dumped).
* Black window or GL errors: the runtime needs OpenGL 3.3 core. In the container check `glxinfo -B`
  (installed with the packages above); it should name the AMD GPU ("RADV VANGOGH"/"radeonsi"), not "llvmpipe".
  `run.sh --software-gl` forces Mesa software rendering (slow).
* **Battle flickers / characters partly drawn, menus fine** (seen on the first Deck run, 2026-10-18; the
  fast and the legacy renderer both do it, `--ge-sync 1` fixes it but is slow). The renderer was
  developed on a software GL where GPU work completes immediately; the Deck's real driver runs it
  asynchronously. Try these in Steam (Properties > Launch Options), one at a time, cheapest first, and note
  which one makes battles clean:
  `--ge-sync 3` (wait for the GPU once per frame), `--ge-sync 2` (once per display list),
  `--ge-sync flush`, `--ge-sync 1` (after every batch: the known fix, slowest).
  These are plain flags (`play.sh` hands them to `run.sh`), so no `%command%` is needed. For a bug report add
  `--gl-debug` to the one that still flickers, play one battle, and send `~/dissidia/work/last_run.log`
  (driver messages are logged as `[GL_DEBUG]`, GL errors by GL_STATS; the `[DRAW] GE sync mode` line confirms
  the setting took, and `[RT] GL renderer:` names the driver).
* Nothing happens when started from Steam: run the same line by hand in Konsole to see the error:
  `~/dissidia/work/play.sh`. Steam swallows stdout; the log is `~/dissidia/work/last_run.log`.
* No sound: the container needs access to the host's PipeWire/Pulse socket (distrobox normally shares
  it). `PSPRECOMP_AUDIO=0` to rule audio out. Music needs libavcodec at build time.
* Out of memory during the build: `deck_setup.sh <iso> --jobs 2` (or 1); finished work is kept.
* Reclaim space after a successful build: `~/dissidia/work/output` and `overlays` (generated C++) are only
  needed to rebuild; `ccache -C` inside the container clears the compiler cache.

## On a normal Linux PC

Same thing without the container: install the packages above with your distribution's package manager
(roughly Fedora: `SDL2-devel freetype-devel ffmpeg-free-devel mesa-libGL-devel zlib-devel openssl-devel`;
Arch: `sdl2 freetype2 ffmpeg mesa zlib openssl base-devel cmake`; those two lists are from memory and
unchecked), then follow [Linux PC](../../../../README.md#linux-pc) in the top-level README from step 2 (`run.sh` takes
`--retro --fullscreen` too).

Ubuntu 24.04 is the only distribution the dependency list above has been built on.

## Manual steps (what the script does)

Everything inside the container: `distrobox create --name dissidia --image docker.io/library/ubuntu:24.04`,
`distrobox enter dissidia`, then

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config git curl unzip python3 python3-venv ccache fontconfig \
    libsdl2-dev libfreetype-dev libavcodec-dev libavutil-dev libgl-dev libgl1-mesa-dri libegl1 mesa-utils \
    zlib1g-dev libssl-dev fonts-dejavu-core
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh ; . "$HOME/.cargo/env"
cd ~/dissidia/psprecomp
python3 games/dissidia/tools/setup_from_iso.py ~/dissidia/game.iso ~/dissidia/work   # ISO or CSO
games/dissidia/tools/pipeline.sh ~/dissidia/work                                      # builds the recompiler too
python3 games/dissidia/tools/pydeps.py                                                # the tools' numpy, Pillow, scipy
games/dissidia/tools/run.sh ~/dissidia/work --retro --fullscreen
```

`pspdecrypt` alternatives: `setup_from_iso.py --pspdecrypt /path/to/pspdecrypt` for a binary you already have, or
`--decrypted-eboot FILE` with an EBOOT that PPSSPP dumped (Settings > Tools > Developer tools, "Dump decrypted
eboot"). An EBOOT that is already a plain ELF is detected and used as is.

Only the files the game uses are extracted: `PSP_GAME/USRDIR` (`DATA/PACKAGE.BIN`, the `OVL_*.ELF`
overlays, `MODULE/`, `BGM/`, `MOVIE/`), `PSP_GAME/SYSDIR`, and the metadata (`PARAM.SFO`, icons).

### Analysis: shipped metadata vs. Ghidra

`games/dissidia/analysis/analysis*.json.gz` (about 200 KB in total) hold the function boundaries and
import tables Ghidra produced for the EU image. They contain **no game code**: the executable bytes were
removed and replaced by SHA-256 hashes. `pipeline.sh` puts the bytes back from the EBOOT and overlays
you extracted from your own disc and refuses to continue if any hash differs (other region, other
revision, modified dump). Use Ghidra instead when you target a different build:
`pipeline.sh <workdir> --ghidra <dir containing support/analyzeHeadless>` (JDK 21, Ghidra 12.0.2 with
ghidra-allegrex v21.3, see the top-level README), or `--skip-analyze` to reuse existing
`<workdir>/analysis*.json`. Regenerating the shipped files after a re-analysis:
`python3 games/dissidia/tools/ship_analysis.py strip-all <workdir> games/dissidia/analysis`.

## Known limitations

* Text is rendered with a system TrueType font (DejaVu by default), not the PSP's built-in fonts, so it
  looks different; some text (e.g. the health-warning splash) may not render.
* A decoration texture sometimes draws black.
* Music is ATRAC3/3+ decoded via libavcodec; a build without `libavcodec-dev` has no music.
* The European PSN image (`ULES01505`) is the one tested in depth; the US UMD (`ULUS10566`) has been tried
  briefly (it works).
* No driver vsync: the guest's 60 Hz vblank is simulated and the window is swapped without waiting, so tearing is
  possible outside gamescope (gamescope composites whole frames). `PSPRECOMP_VSYNC=1` turns the driver's vsync on at
  the risk of stalling the render thread.

## Known unknowns (everything here needs a real Deck)

Verified on the development box (headless Xvfb 1280x800, Mesa software GL, a `distrobox` shim): `--retro` places the
picture at 960x544 centred on a 1280x800 window (`[PRESENT] drawable 1280x800 -> dst (160,128)-(1120,672) nearest
+bars`) with nearest-neighbour textures; `--crisp` gives `dst (0,37)-(1280,762) linear` at render scale 3; the
preset/flag precedence in `run.sh`; `--check`, `--dry-run`, `--launcher-only` and the text of the generated
`play.sh` including the environment hand-over; and that `pipeline.sh` builds the recompiler and the runtime from a
clean work directory and a fresh build tree and the result boots to the title and a battle. Not covered even
there: real `--fullscreen` (Xvfb has no window manager, so the borderless fullscreen request is ignored; the
tests used an explicit 1280x800 window instead), the ISO extraction on a real disc image (none available; the
existing extracted `disc0` and decrypted EBOOT were reused), apt and rustup (already installed there).
Not verified, and the first things to look at if a step fails:

1. **SteamOS has podman and distrobox.** Podman is believed to ship with SteamOS 3.5+; distrobox may or may
   not. `--check` reports both. The install route (`--install-distrobox`) goes to `~/.local/bin`.
2. **Rootless podman prerequisites** (`/etc/subuid`, `/etc/subgid`, user namespaces, `newuidmap`) and the
   `docker.io/library/ubuntu:24.04` image on distrobox: a UID-1000 `ubuntu` user exists in that image; recent
   distrobox handles it, older versions may not. Fallback image above.
3. **Build time and RAM** of the compile on the Deck's APU (never timed), and whether three jobs fit beside
   Desktop Mode. SteamOS swap is small.
4. **ISO supply**: the script expects a file under `$HOME`. An SD card (`/run/media/...`) may not be visible
   inside the container at the same path, which is why paths outside `$HOME` are refused. Copy the ISO in.
5. **`pspdecrypt`** has never been built or run by this tooling on real input (the dev box already held a
   decrypted EBOOT); the decrypt step is the likeliest first failure in step 4. Fallback: dump the decrypted EBOOT
   with PPSSPP and pass it via `setup_from_iso.py --decrypted-eboot`.
6. **Launching from Game Mode**: that Steam runs an executable shell script as a non-Steam shortcut (the file
   picker needs "All files"), that `distrobox enter` works without a terminal, that the container's `bash`
   receives the forwarded environment (`.play_env`), and that rootless podman starts at all from the Game Mode
   session (it needs the user session bus and `XDG_RUNTIME_DIR`, which Steam launches inherit).
7. **Controller**: the claim that the container sees Steam's virtual Xbox pad (it depends on `/dev/input/*`
   being shared and on SDL finding the device without a udev database in the container; SDL falls back to
   guessing the device class). Whether `SDL_GAMECONTROLLER_IGNORE_DEVICES` gets through (so the raw Deck pad
   is not opened as well), and that the default "Gamepad" template is what a new non-Steam entry gets. The
   Deck's raw pad would also open as a second controller through SDL's own Steam Deck HID driver; with both open
   the buttons are OR-ed but the sticks follow whichever device moved last. If you see two "Controller opened"
   lines in the log, set a Launch Option or `export SDL_JOYSTICK_HIDAPI_STEAMDECK=0` in `play.sh`.
8. **Display**: that SDL creates a 3.3 core GL context from the container's Mesa on the Deck GPU under gamescope
   (XWayland), that `FULLSCREEN_DESKTOP` gives exactly 1280x800 there, and that the picture is not rescaled again
   by gamescope. Check the `[PRESENT] drawable ...` line in the log: it must say `drawable 1280x800 -> dst
   (160,128)-(1120,672) nearest +bars` for `--retro`.
9. **Performance and thermals**: the batched renderer at 2x/3x on the Deck's GPU, frame pacing without driver
   vsync, battery draw. Nobody has measured any of it. The `[FPS]` lines in the log give frame rate.
10. **Audio** through PipeWire from inside the container, and sleep/resume behaviour.
11. **Steam integration details**: whether Game Mode picks up all five pictures from `grid/` (written by
    `steam_shortcut.py`, not yet tried on the Deck), controller template,
    Steam's own overlay and Quick Access menu do not work through the container (the overlay is not injected
    into the container process).
