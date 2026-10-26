#!/usr/bin/env bash
# Launch the Dissidia 012 native build.
#
# Usage: run.sh <workdir> [--retro | --crisp] [--software-gl] [--swap-ab] [--gamecontrollerdb FILE] [--log FILE]
#                         [--scale N] [--window WxH] [--fullscreen] [--stretch] [--filter nearest|linear|integer]
#                         [--integer-scale] [--tex-filter nearest|linear|game] [--render-scale N|auto]
#   --retro                  PRESET: keep the PSP's low-poly look. Nearest-neighbour texture filtering, 3D at 2x the PSP
#                            resolution (960x544), shown at a whole-number scale (2x on a 1280x800 Deck: 960x544 centred,
#                            black borders), 480:272 aspect kept, no texture-pack replacement or upscaling.
#                            = --render-scale 2 --tex-filter nearest --integer-scale, PSPRECOMP_TEX_REPLACE=0.
#   --crisp                  PRESET: smoother alternative. 3D at 3x (1440x816), game-chosen (bilinear) texture filtering,
#                            bilinear fit to the screen (1280x725 on the Deck, 37 px bars), 480:272 aspect kept.
#                            = --render-scale 3 --filter linear. Texture packs stay enabled.
#                            Presets only fill in what you did not set: explicit flags win; the last of --retro/--crisp wins.
#                            Same via the environment: PSPRECOMP_PRESET=retro|crisp (a flag overrides it).
#   --integer-scale          PSPRECOMP_INTEGER_SCALE=1: show the picture at the largest whole multiple of 480x272 that
#                            fits (centred, black borders); crisp when the render scale divides evenly into it
#   --tex-filter MODE        PSPRECOMP_TEX_FILTER: nearest | linear | game (default game = what the game requests)
#   --software-gl            LIBGL_ALWAYS_SOFTWARE=1 (Mesa software rendering; slow, for GPUs/VMs without GL 3.3)
#   --swap-ab                PSPRECOMP_SWAP_AB=1: swap confirm/cancel on the gamepad (A=Circle, B=Cross); for
#                            Nintendo-style pads. Default is A=Cross (Xbox/Steam layout).
#   --gamecontrollerdb FILE  extra SDL controller mappings (SDL_GameControllerDB format). Also picked up
#                            automatically from <workdir>/gamecontrollerdb.txt if that file exists.
#   --scale N                PSPRECOMP_SCALE: window = N x 480x272 (default: largest integer scale that fits the desktop)
#   --window WxH             PSPRECOMP_WINDOW: explicit window size (the picture is letterboxed into it)
#   --fullscreen             PSPRECOMP_FULLSCREEN=1 (borderless desktop fullscreen). F11 / Alt+Enter toggle at runtime.
#   --stretch                PSPRECOMP_STRETCH=1: fill the window instead of keeping the 480:272 aspect (no bars)
#   --filter MODE            PSPRECOMP_FILTER: nearest | linear | integer (largest crisp integer multiple, centred).
#                            Default: crisp when the window is an exact multiple, bilinear otherwise.
#   --render-scale N|auto    PSPRECOMP_RENDER_SCALE (1..8 or auto, default 1): internal render resolution = N x 480x272
#                            (sharper 3D edges, HUD unchanged; cost grows with N^2). auto = drawable height / 272,
#                            capped at 3. Steam Deck (1280x800): 3 (or 2 to save battery).
#   --legacy-ge              PSPRECOMP_GE_FAST=0: use the legacy per-PRIM renderer (CPU skinning, one draw per PRIM)
#                            instead of the default batched GPU-skinning path. Escape hatch only; it is slower.
#   --ge-sync 1|2|3|flush|off  PSPRECOMP_GE_SYNC: wait for the GPU (1 per batch, 2 per display list, 3 per frame,
#                            flush = glFlush per batch, off = none). Default on a Steam Deck: flush (it fixes the
#                            flicker there, see docs/steamdeck.md); elsewhere off.
#   --gl-debug               PSPRECOMP_GL_DEBUG=1 PSPRECOMP_GL_STATS=2: driver messages and GL errors in the log
#   --log FILE               also write the runtime's stderr log to FILE (default: <workdir>/last_run.log)
#
# What this sets for the runtime (everything else is a developer diagnostic and is left alone):
#   PSPRECOMP_DISC0=<workdir>/disc0   extracted game data (the runtime's default is ./disc0)
#   PSPRECOMP_MS0=<workdir>/ms0       emulated Memory Stick: saves go to ms0/PSP/SAVEDATA (back this up)
#   PSPRECOMP_CROSS_MID=1             not read by the runtime; it only matters when recompiling (pipeline.sh
#                                     sets it there). Exported anyway so a shell that reuses this
#                                     environment for a re-recompile cannot forget it.
# PSPRECOMP_CLEANROOM is not set: nothing in the runtime or the Dissidia module reads it.
#
# Optional variables you can set before calling this script:
#   PSPRECOMP_GAMECONTROLLERDB, PSPRECOMP_SWAP_AB   (same as the flags above)
#   PSPRECOMP_PAD_L2=R+CROSS      left trigger (L2) = R+X, the dodge (default set here; L1 stays L)
#   PSPRECOMP_PAD_R2=R+TRIANGLE   right trigger (R2) = R+Triangle, the dash, held while the trigger is (R1 stays R)
#                                 Set either to L / R for the plain PSP shoulder, or none. Names: L R CROSS CIRCLE
#                                 SQUARE TRIANGLE UP DOWN LEFT RIGHT START SELECT, joined by '+'.
#   PSPRECOMP_CAM_FREE=0          right stick is a plain D-pad (default: free camera in battle, D-pad in menus)
#   PSPRECOMP_CAM_SENS=1.0, PSPRECOMP_CAM_INVERT_Y=1, PSPRECOMP_CAM_RECENTER=<s>, PSPRECOMP_RSTICK_DEADZONE=0.2
#   PSPRECOMP_RSTICK_DPAD=0       right stick never acts as the D-pad
#   PSPRECOMP_ANALOG_FULL=0       original 8-way stick movement (default: continuous angle); PSPRECOMP_ANALOG_DEADZONE=0.2
#   PSPRECOMP_ARROWS_DPAD_ONLY=1  keyboard arrows do not drive the analogue stick
#   PSPRECOMP_OSK_TEXT   text the headless on-screen keyboard auto-enters (e.g. the Data Config name)
#   PSPRECOMP_FONT       TrueType font for sceFont (default: DejaVu Sans, located automatically)
#   PSPRECOMP_FONT_SCALE em scale applied to the font (default 0.8)
#   PSPRECOMP_AUDIO=0    disable audio output
#   PSPRECOMP_CHARMOD=0  don't build the character mods (mods/ at the repo root) before starting
#   PSPRECOMP_PAD_LOG=1  log every gamepad button event (SDL name -> PSP button); useful on new hardware
#   PSPRECOMP_GL_STATS=1|2, PSPRECOMP_GE_AB=1   renderer diagnostics (GL call counters / GL errors; fast-vs-legacy comparison), see DEBUGGING.md
set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }

PRESET="${PSPRECOMP_PRESET:-}"; INTSCALE=0; TEXF=""
GE_SYNC=""; WORK=""; LEGACY_GE=0; SOFT=0; RSCALE=""; SCALE=""; WINSZ=""; FULL=0; STRETCH=0; FILTER=""; SWAP=0; PAD_DB=""; LOG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --retro)       PRESET=retro; shift ;;
    --crisp)       PRESET=crisp; shift ;;
    --integer-scale) INTSCALE=1; shift ;;
    --tex-filter)  [ $# -ge 2 ] || die "--tex-filter needs nearest|linear|game"; TEXF="$2"; shift 2 ;;
    --software-gl) SOFT=1; shift ;;
    --legacy-ge)   LEGACY_GE=1; shift ;;
    --swap-ab)     SWAP=1; shift ;;
    --gamecontrollerdb) [ $# -ge 2 ] || die "--gamecontrollerdb needs a file"; PAD_DB="$2"; shift 2 ;;
    --log)         [ $# -ge 2 ] || die "--log needs a file"; LOG="$2"; shift 2 ;;
    --render-scale) [ $# -ge 2 ] || die "--render-scale needs N (1..8)"; RSCALE="$2"; shift 2 ;;
    --scale)       [ $# -ge 2 ] || die "--scale needs N"; SCALE="$2"; shift 2 ;;
    --window)      [ $# -ge 2 ] || die "--window needs WxH"; WINSZ="$2"; shift 2 ;;
    --fullscreen)  FULL=1; shift ;;
    --stretch)     STRETCH=1; shift ;;
    --filter)      [ $# -ge 2 ] || die "--filter needs nearest|linear|integer"; FILTER="$2"; shift 2 ;;
    --texpack)     [ $# -ge 2 ] || die "--texpack needs a directory"; [ -d "$2" ] || die "--texpack: no directory $2"
                   export PSPRECOMP_TEXPACK="$(cd "$2" && pwd)"; shift 2 ;;   # absolute: the game runs from inside the work dir
    --tex-dump)    [ $# -ge 2 ] || die "--tex-dump needs a directory"; export PSPRECOMP_TEX_DUMP=1 PSPRECOMP_TEX_DUMP_DIR="$2"; shift 2 ;;
    --ge-sync)     [ $# -ge 2 ] || die "--ge-sync needs 1|2|3|flush|off"; case "$2" in 1|2|3|flush|off) ;; *) die "--ge-sync must be 1, 2, 3, flush or off" ;; esac
                   GE_SYNC="$2"; shift 2 ;;
    --gl-debug)    export PSPRECOMP_GL_DEBUG=1 PSPRECOMP_GL_STATS=2; shift ;;
    -h|--help)     sed -n '2,/^set -euo/{/^set -euo/!p}' "$0"; exit 0 ;;
    -*)            die "unknown option $1" ;;
    *)             [ -z "$WORK" ] || die "unexpected argument $1"; WORK="$1"; shift ;;
  esac
done
[ -n "$WORK" ] || die "usage: run.sh <workdir> [--retro|--crisp] [--software-gl] [--swap-ab] [--gamecontrollerdb FILE] [--log FILE] [--scale N] [--window WxH] [--fullscreen] [--stretch] [--filter MODE] [--texpack DIR] [--tex-dump DIR] [--ge-sync 1|2|3|flush|off] [--gl-debug]"
[ -d "$WORK" ] || die "workdir $WORK does not exist"
case "$PRESET" in ""|none|retro|crisp) ;; *) die "unknown preset '$PRESET' (retro | crisp | none)" ;; esac
case "$TEXF" in ""|nearest|linear|game) ;; *) die "--tex-filter must be nearest, linear or game" ;; esac
WORK="$(cd "$WORK" && pwd)"
# Paths given relative to where run.sh was started (the game runs inside $WORK): make them absolute.
case "$LOG" in ""|/*) ;; *) LOG="$PWD/$LOG" ;; esac
case "${PSPRECOMP_MODS:-}" in ""|/*) ;; *) export PSPRECOMP_MODS="$PWD/$PSPRECOMP_MODS" ;; esac
[ -z "${PSPRECOMP_MODS:-}" ] || [ -d "$PSPRECOMP_MODS" ] || die "PSPRECOMP_MODS=$PSPRECOMP_MODS is not a folder"

BIN="$WORK/build/psprecomp_runtime"
[ -x "$BIN" ] || die "$BIN not found; run pipeline.sh first"
[ -f "$WORK/disc0/PSP_GAME/SYSDIR/EBOOT.BIN" ] || die "disc0 not set up; run setup_from_iso.py first"
[ -f "$WORK/disc0/PSP_GAME/USRDIR/DATA/PACKAGE.BIN" ] || die "disc0 is incomplete (no PACKAGE.BIN); re-run setup_from_iso.py"

# Character mods: every folder in mods/ at the repo root (with a mod.json; not _template etc.) is built into the mods dir before the game
# starts; a new folder is added to the roster (tools/charmod.py, mods/README.md). PSPRECOMP_CHARMOD=0 skips it.
CHARMOD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "${PSPRECOMP_CHARMOD:-1}" != 0 ] && ls "$CHARMOD_DIR"/../../../../mods/[!_.]*/mod.json >/dev/null 2>&1; then
  if command -v python3 >/dev/null; then
    mkdir -p "${PSPRECOMP_MODS:-$WORK/mods}"
    python3 "$CHARMOD_DIR/charmod.py" --mods "${PSPRECOMP_MODS:-$WORK/mods}" --disc "$WORK/disc0" build \
      || die "character mods were not built (see above). Fix that, or start without building: PSPRECOMP_CHARMOD=0 run.sh ..."
  else
    warn "python3 not found: character mods in mods/ were not built"
  fi
fi

# A binary built in one container/distro and run in another fails with an unhelpful loader error.
if command -v ldd >/dev/null && ldd "$BIN" 2>/dev/null | grep -q 'not found'; then
  ldd "$BIN" | grep 'not found' >&2
  die "missing shared libraries (above). Run this inside the environment the runtime was built in (on the Steam Deck: via distrobox, see docs/steamdeck.md)"
fi

# Presets fill in only what was not set explicitly (flags above, or PSPRECOMP_* already in the environment).
case "$PRESET" in
  retro)
    [ -n "$RSCALE" ] || [ -n "${PSPRECOMP_RENDER_SCALE:-}" ] || RSCALE=2
    [ -n "$TEXF" ] || [ -n "${PSPRECOMP_TEX_FILTER:-}" ] || TEXF=nearest
    [ -n "$FILTER" ] || [ "$STRETCH" = 1 ] || [ -n "${PSPRECOMP_FILTER:-}" ] || INTSCALE=1
    # no replacement/upscaled textures: the original art at the PSP's own detail
    export PSPRECOMP_TEX_REPLACE="${PSPRECOMP_TEX_REPLACE:-0}"
    unset PSPRECOMP_TEX_UPSCALE ;;
  crisp)
    [ -n "$RSCALE" ] || [ -n "${PSPRECOMP_RENDER_SCALE:-}" ] || RSCALE=3
    [ -n "$FILTER" ] || [ "$STRETCH" = 1 ] || [ -n "${PSPRECOMP_FILTER:-}" ] || FILTER=linear ;;
esac
[ -z "$PRESET" ] || [ "$PRESET" = none ] || echo "preset: $PRESET" >&2

export PSPRECOMP_DISC0="${PSPRECOMP_DISC0:-$WORK/disc0}"
export PSPRECOMP_MS0="${PSPRECOMP_MS0:-$WORK/ms0}"
export PSPRECOMP_CROSS_MID=1
mkdir -p "$PSPRECOMP_MS0/PSP/SAVEDATA" "$PSPRECOMP_MS0/PSP/GAME"
[ "$SOFT" = 0 ] || export LIBGL_ALWAYS_SOFTWARE=1
[ "$SWAP" = 0 ] || export PSPRECOMP_SWAP_AB=1
export PSPRECOMP_PAD_L2="${PSPRECOMP_PAD_L2:-R+CROSS}"      # L2 = dodge
export PSPRECOMP_PAD_R2="${PSPRECOMP_PAD_R2:-R+TRIANGLE}"   # R2 = dash
[ "$LEGACY_GE" = 0 ] || export PSPRECOMP_GE_FAST=0
# GPU sync: the Deck's driver runs GL work asynchronously and battles flicker without it; flush fixes that at no
# visible cost. An explicit --ge-sync or PSPRECOMP_GE_SYNC wins; "off" = no sync.
if [ -z "$GE_SYNC" ] && [ -z "${PSPRECOMP_GE_SYNC:-}" ] && [ "$(cat /sys/devices/virtual/dmi/id/sys_vendor 2>/dev/null)" = Valve ]; then
  GE_SYNC=flush
fi
case "$GE_SYNC" in "") ;; off) unset PSPRECOMP_GE_SYNC ;; *) export PSPRECOMP_GE_SYNC="$GE_SYNC" ;; esac
[ -z "$SCALE" ] || export PSPRECOMP_SCALE="$SCALE"
[ -z "$WINSZ" ] || export PSPRECOMP_WINDOW="$WINSZ"
[ "$FULL" = 0 ] || export PSPRECOMP_FULLSCREEN=1
[ "$STRETCH" = 0 ] || export PSPRECOMP_STRETCH=1
[ -z "$FILTER" ] || export PSPRECOMP_FILTER="$FILTER"
[ "$INTSCALE" = 0 ] || export PSPRECOMP_INTEGER_SCALE=1
[ -z "$TEXF" ] || [ "$TEXF" = game ] || export PSPRECOMP_TEX_FILTER="$TEXF"
[ -z "$RSCALE" ] || export PSPRECOMP_RENDER_SCALE="$RSCALE"
[ -n "$PAD_DB" ] || PAD_DB="${PSPRECOMP_GAMECONTROLLERDB:-}"
[ -n "$PAD_DB" ] || { [ ! -f "$WORK/gamecontrollerdb.txt" ] || PAD_DB="$WORK/gamecontrollerdb.txt"; }
if [ -n "$PAD_DB" ]; then
  [ -f "$PAD_DB" ] || die "controller mapping file $PAD_DB not found"
  export PSPRECOMP_GAMECONTROLLERDB="$(cd "$(dirname "$PAD_DB")" && pwd)/$(basename "$PAD_DB")"
fi

# Text rendering needs a TrueType font. The runtime looks for DejaVuSans.ttf in the usual Debian/Arch
# directories; if it is elsewhere, ask fontconfig.
if [ -z "${PSPRECOMP_FONT:-}" ]; then
  have=0
  for d in /usr/share/fonts/truetype/dejavu /usr/share/fonts/TTF /usr/share/fonts/dejavu /usr/share/fonts/truetype; do
    [ ! -f "$d/DejaVuSans.ttf" ] || have=1
  done
  if [ "$have" = 0 ]; then
    f=""
    command -v fc-match >/dev/null && f="$(fc-match -f '%{file}' 'DejaVu Sans:fontformat=TrueType' 2>/dev/null || true)"
    case "$f" in *.ttf) export PSPRECOMP_FONT="$f" ;; *)
      warn "no DejaVuSans.ttf found: in-game text may be missing. Install fonts-dejavu-core / ttf-dejavu or set PSPRECOMP_FONT=/path/to/font.ttf" ;; esac
  fi
fi

[ -n "$LOG" ] || LOG="$WORK/last_run.log"
echo "Dissidia 012 (native)  saves: $PSPRECOMP_MS0/PSP/SAVEDATA   log: $LOG" >&2
cd "$WORK"
"$BIN" 2> >(tee "$LOG" >&2) &
pid=$!
trap 'kill -TERM "$pid" 2>/dev/null || true' INT TERM
rc=0; wait "$pid" || rc=$?
wait 2>/dev/null || true
exit "$rc"
