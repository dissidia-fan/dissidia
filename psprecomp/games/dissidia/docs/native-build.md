# The native build in detail

How the three setup steps work, the work folder, and every runtime option (window, scaling, render resolution, input, camera). The short install steps are in the [top-level README](../../../../README.md#install-and-play); the Steam Deck walkthrough is [steamdeck.md](steamdeck.md). Paths are relative to `psprecomp/games/dissidia/`.

## What each step does

You need your own copy of the game, a 64-bit Linux PC or a Steam Deck, about 8 GB of free disk space, and patience:
the first build takes a long time (tens of minutes to hours). The steps, for the Deck and for a Linux PC, are in the
top-level README: [Install and play](../../../../README.md#install-and-play). Steam Deck details (Steam, controls,
display, troubleshooting): [`docs/steamdeck.md`](steamdeck.md).

On a Linux PC or a Mac, `tools/install.sh <game.iso> [workdir]` does all of it in one go: the build tools (apt or Homebrew,
only what is missing), Rust (rustup), then the steps below, the tools' Python packages, and `<workdir>/play.sh`, a
launcher that runs `run.sh` with any flags you give it. `--jobs N`, `--update` (git pull first), `--no-packages`.
The Steam Deck has its own, `deck_setup.sh`. The steps by hand:

1. `setup_from_iso.py` reads the ISO/CSO itself (no tools needed), copies the files the game uses to
   `<workdir>/disc0`, decrypts `EBOOT.BIN` (it builds the small `pspdecrypt` tool for this; see the
   script's `--help` for alternatives), and creates the emulated Memory Stick `<workdir>/ms0`.
2. `pipeline.sh` builds the recompiler (Rust), rebuilds the analysis from the shipped metadata in
   `analysis/` and your own files, translates the game to C++ and compiles it with the runtime into
   `<workdir>/build/psprecomp_runtime`. Re-running it is safe. Add `--jobs 2` if it runs out of memory.
3. `run.sh` starts the game with the right environment. Saves are in `<workdir>/ms0/PSP/SAVEDATA/`.
   Gamepads work out of the box (A = Cross); `--swap-ab` swaps confirm/cancel. Keyboard: arrows, X/Z/A/S,
   Q/W, Space = Start, Tab = Select.

If something fails, the scripts say what is missing. This is a work-in-progress preview: see
[Status](#status) for what does and does not work yet.

Developers: to re-analyse with Ghidra instead (needed for a different region or build), install JDK 21,
Ghidra 12.x and the ghidra-allegrex extension (versions pinned in the top-level README) and run
`pipeline.sh <workdir> --ghidra /path/to/ghidra`. The shipped `analysis/*.json.gz` files contain no game
code (function tables only; the code bytes are re-read from your disc and hash-checked), and are
regenerated with `tools/ship_analysis.py strip-all`.

## Work directory layout

```
<workdir>/
  disc0/PSP_GAME/SYSDIR/{EBOOT,BOOT}.BIN   decrypted ELF (the runtime boots this)
  disc0/PSP_GAME/USRDIR/DATA/...            PACKAGE.BIN, PACKAGE_INFO.BIN, OVL_*_APP.ELF, MODULE/, BGM/
  disc0/PSP_GAME/USRDIR/DATA/MOVIE/         movies
  ms0/PSP/{SAVEDATA,GAME}                   Memory Stick root (saves)
  EBOOT.enc.bin, EBOOT.dec.elf              encrypted / decrypted EBOOT
  OVL_{BATTLE,MENU,EXTRA}_APP.bin           flat overlay blobs (ELF offset 0x54 onward)
  eboot_image.bin                           flat EBOOT image 0x08804000..0x089DD4C0
  analysis*.json                            analysis results (analysis, _battle, _menu, _extra, _supacc)
  last_run.log                              runtime log of the last run.sh
  output/  overlays/{battle,menu,extra,supacc}/   generated C++
  build/psprecomp_runtime                   the runtime
  tools/pspdecrypt                          built decryptor
```

The runtime reads `PSPRECOMP_DISC0=<workdir>/disc0` and `PSPRECOMP_MS0=<workdir>/ms0` (both set by
`run.sh`). `PSPRECOMP_CROSS_MID=1` is needed when recompiling (`pipeline.sh` sets it); the runtime does not
read it. `PSPRECOMP_CLEANROOM` is not used. Optional: `PSPRECOMP_OSK_TEXT`,
`PSPRECOMP_FONT`, `PSPRECOMP_FONT_SCALE`, `PSPRECOMP_AUDIO=0`.

Look presets (`run.sh --retro`, `--crisp`; `PSPRECOMP_PRESET=retro|crisp`): `--retro` keeps the PSP's low-poly look
(nearest-neighbour textures `PSPRECOMP_TEX_FILTER=nearest`, render scale 2, whole-number window scaling
`PSPRECOMP_INTEGER_SCALE=1` = 960x544 centred on a 1280x800 Deck panel, 480:272 kept, texture packs/upscaler off);
`--crisp` is render scale 3 with a bilinear aspect fit. The Deck's `play.sh` uses `--retro --fullscreen`. Explicit
flags beat presets. Details and the table in `docs/steamdeck.md`.

Window and scaling (generic runtime options; `run.sh` flags in brackets): the window is resizable and
defaults to the largest integer multiple of 480x272 that fits the desktop (1280x800 Deck: 2x = 960x544;
a 1440x900 Mac: 3x). `PSPRECOMP_SCALE=<n>` [`--scale n`], `PSPRECOMP_WINDOW=<w>x<h>` [`--window WxH`],
`PSPRECOMP_FULLSCREEN=1` [`--fullscreen`, borderless desktop fullscreen]; F11 or Alt+Enter toggles
fullscreen at runtime. The picture keeps its 480:272 aspect with black bars; `PSPRECOMP_STRETCH=1`
[`--stretch`] fills the window instead. `PSPRECOMP_FILTER=nearest|linear|integer` [`--filter`]: `integer` =
largest whole multiple, centred (crisp); `linear` = bilinear to the aspect-fit rectangle; the default is
nearest when the fit is an exact multiple and linear otherwise. `PSPRECOMP_INTEGER_SCALE=1` [`--integer-scale`]
uses the `integer` placement but blits nearest only when the render target is a whole multiple of the output (or
the reverse) and bilinear otherwise. `PSPRECOMP_TEX_FILTER=nearest|linear` [`--tex-filter`] forces the sampling
filter of game textures (default: what the game requests; render-target reads are untouched). High-DPI (Retina) uses the drawable size.
Debug-socket `S path.tga` still captures the native 480x272 frame; `V path.tga` captures the whole window.

Internal render resolution (generic runtime option): `PSPRECOMP_RENDER_SCALE=<n|auto>` [`--render-scale n`], 1..8 or `auto` (drawable height / 272, capped at 3),
default 1 (the Deck's `play.sh`: 2 with `--retro`, 3 with `--crisp`). Every GE render target (colour + depth) is allocated at 480n x 272n and the GL viewport is
multiplied by n, so 3D geometry is rasterised at n times the PSP resolution while 2D/HUD vertices (which are
PSP-pixel coordinates mapped to NDC) land in the same place. Game textures are not upscaled and keep the
filter the game sets. Framebuffer-as-texture effects (the battle's render-to-texture chain, blur) keep
working because their UVs are normalised; the window blit uses bilinear unless the window is exactly
480n x 272n (or `PSPRECOMP_FILTER=nearest|integer`). `S` box-averages the n x n blocks back to 480x272
(`PSPRECOMP_SHOT_FULL=1` writes the full-size frame instead). Block transfers only move guest memory and are
not scaled. Wide lines/points are requested at n pixels (best effort; core profiles may clamp). Scale 1 is
bit-identical to the previous output. Cost is roughly proportional to n^2 in fill rate.

Input: keyboard and SDL game controllers (`PSPRECOMP_SWAP_AB`, `PSPRECOMP_GAMECONTROLLERDB`, `PSPRECOMP_PAD_LOG=1`; key/button
tables and Steam Input setup in `docs/steamdeck.md`).

Controls, right stick (the PSP has none): in battle it is a free camera. `games/dissidia/runtime/camera.cpp`
wraps the battle camera update (`ovl_battle FUN_08B3CA24`, once per frame) and rotates the final eye position
around the camera target by an accumulated yaw/pitch offset, so the game's own d-pad camera, lock-on framing,
auto-centring and collision pull-in keep working underneath (the stick is an offset on top, held until you move
it again; `PSPRECOMP_CAM_RECENTER=<s>` makes it drift back). Outside battle (menus) the right stick acts as the
D-pad. Options: `PSPRECOMP_CAM_FREE=0` (stick is a plain D-pad everywhere, the game's own camera), `PSPRECOMP_CAM_SENS`
(default 1.0 = ~150 deg/s at full deflection), `PSPRECOMP_CAM_INVERT_Y=1`, `PSPRECOMP_CAM_PITCH_MIN/MAX` (eye elevation
limits in degrees, default -5..80), `PSPRECOMP_RSTICK_DEADZONE` (default 0.20), `PSPRECOMP_RSTICK_DPAD=0` (right stick
never becomes a D-pad). Headless test: debug socket `A2 <x> <y> <ms>` holds the right stick (0..255, 128 centre).
`PSPRECOMP_CAMX_LOG=<n>` logs n camera updates (game yaw, stick offset, eye, target).

Analogue stick direction: the game's own stick processing (`FUN_08872138`, pad object `+0x258` = 100.0) applies a
per-axis dead zone of 100/127.5 and then normalises, so a stick at 20 degrees reads as straight up and 60 degrees
as straight right: 8 directions on a PSP nub, only 4 on a circular-gate stick. `games/dissidia/runtime/input.cpp`
wraps it and replaces the stick vector (unit direction + magnitude, same layout) with a radial dead zone and the
true angle; the virtual D-pad bits the original returns are untouched. `PSPRECOMP_ANALOG_FULL=0` restores the
original 8-way behaviour, `PSPRECOMP_ANALOG_DEADZONE` (default 0.20 of travel) sets the radial dead zone,
`PSPRECOMP_ANALOG_LOG=<n>` logs raw / game / replacement vectors.
