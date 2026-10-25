#!/usr/bin/env bash
# One-shot Steam Deck / immutable-distro setup: build container + dependencies + recompiler + your game data
# + runtime build, then a host-side launcher. Run it on the Deck in Desktop Mode (Konsole), NOT as root.
#
# Usage: deck_setup.sh <game.iso|game.cso> [workdir] [options]
#   workdir              default ~/dissidia/work (must be under $HOME)
#   --check              validate prerequisites and print PASS/WARN/FAIL lines; changes nothing, builds nothing
#                        (the ISO argument is optional with --check). Exit status 1 if anything FAILs.
#   --dry-run            print the commands that would run, without running them
#   --install-distrobox  if distrobox is missing, install it into ~/.local (downloads the official install
#                        script from https://raw.githubusercontent.com/89luca89/distrobox/main/install)
#   --add-to-steam       when finished, put the launcher in the Steam library as "Dissidia Trēdecim" with its artwork
#                        (steam_shortcut.py: capsule, banner, hero, logo, icon; an earlier entry for this launcher is
#                        renamed in place, keeping play time and Launch Options). Steam is closed for a moment and
#                        started again if it is running. Desktop Mode only.
#   --preset retro|crisp look baked into the launcher (default retro, see docs/steamdeck.md; --retro/--crisp work too)
#   --jobs N             parallel compile jobs (default 3, about 1.5 GB RAM each)
#   --container NAME     default dissidia
#   --image IMAGE        default docker.io/library/ubuntu:24.04 (fully qualified: SteamOS' podman has no
#                        default search registry). Alternative: quay.io/toolbx/ubuntu-toolbox:24.04
#   --redo STEP          forget that STEP (container | packages | rust | extract | all) is done and run it again
#   --launcher-only      just (re)write <workdir>/play.sh, e.g. to change --preset; builds nothing
#                        (deck_setup.sh --launcher-only [workdir] --preset crisp; no ISO needed). With
#                        --add-to-steam it also (re)does the Steam entry and artwork, e.g. --launcher-only --add-to-steam
#   --update             first pull the latest code and mods from GitHub (git pull --ff-only in this checkout),
#                        then run as usual: the runtime is rebuilt incrementally and the launcher rewritten
#   --allow-outside-home allow an ISO/workdir/checkout outside $HOME (the container may not see it)
#
# Mods: the checkout tracks the installed mods in psprecomp/work/mods. If <workdir>/mods does not exist, it is
# created as a link to that folder, so a later --update brings new mods along with the code.
#
# Resumable: each slow step leaves a stamp in <workdir>/.deck_setup/; run the same command again after an
# interruption and finished steps are skipped (the analysis/recompile steps skip themselves via
# pipeline.sh stamps and the C++ build resumes where it stopped, ccache makes repeats cheap).
#
# SteamOS' root filesystem is read-only and replaced by system updates, so nothing is installed on the host:
# everything (compilers, SDL2, Rust, the build, and later the game itself) lives in a distrobox container
# whose home directory is your real home. Writes: ~/.local/bin (distrobox, optional), the container's
# storage under ~/.local/share/containers, ~/.cargo + ~/.rustup, ~/.cache/ccache, <workdir>, and
# <workdir>/play.sh (the launcher to add to Steam).
set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

ISO=""; WORK=""; DRY=0; CHECK=0; INSTALL_DB=0; ADD_STEAM=0; JOBS=3; NAME=dissidia
IMAGE=docker.io/library/ubuntu:24.04; PRESET=retro; OUTSIDE_HOME=0; REDO=""; LAUNCHER_ONLY=0; UPDATE=0
ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --check) CHECK=1; shift ;;
    --dry-run) DRY=1; shift ;;
    --install-distrobox) INSTALL_DB=1; shift ;;
    --add-to-steam) ADD_STEAM=1; shift ;;
    --launcher-only) LAUNCHER_ONLY=1; shift ;;
    --update) UPDATE=1; shift ;;
    --allow-outside-home) OUTSIDE_HOME=1; shift ;;
    --retro) PRESET=retro; shift ;;
    --crisp) PRESET=crisp; shift ;;
    --preset) [ $# -ge 2 ] || die "--preset needs retro or crisp"; PRESET="$2"; shift 2 ;;
    --redo) [ $# -ge 2 ] || die "--redo needs a step"; REDO="$REDO $2"; shift 2 ;;
    --jobs) [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    --container) [ $# -ge 2 ] || die "--container needs a name"; NAME="$2"; shift 2 ;;
    --image) [ $# -ge 2 ] || die "--image needs an image"; IMAGE="$2"; shift 2 ;;
    -h|--help) sed -n '2,/^set -euo/{/^set -euo/!p}' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) if [ -z "$ISO" ]; then ISO="$1"; elif [ -z "$WORK" ]; then WORK="$1"; else die "unexpected argument $1"; fi; shift ;;
  esac
done
if [ "$UPDATE" = 1 ]; then
  # Pull, then re-run the freshly pulled copy of this script (bash reads a running script lazily, so it must not
  # continue in a file that git just replaced).
  REST=(); for a in "${ARGS[@]}"; do [ "$a" = --update ] || REST+=("$a"); done
  echo "+ git -C $REPO pull --ff-only"
  if [ "$DRY" = 0 ]; then
    git -C "$REPO" pull --ff-only || die "git pull failed (local changes in $REPO? see 'git -C $REPO status')"
  fi
  exec bash "$HERE/deck_setup.sh" "${REST[@]}"
fi
if [ "$LAUNCHER_ONLY" = 1 ] && [ -n "$ISO" ] && [ -z "$WORK" ]; then WORK="$ISO"; ISO=""; fi   # one argument = the workdir
case "$PRESET" in retro|crisp) ;; *) die "--preset must be retro or crisp" ;; esac
case "$JOBS" in ''|*[!0-9]*|0) die "--jobs must be a positive integer" ;; esac
[ -n "$ISO" ] || [ "$CHECK" = 1 ] || [ "$LAUNCHER_ONLY" = 1 ] || die "usage: deck_setup.sh <game.iso|game.cso> [workdir] [--check] [--install-distrobox] [--dry-run]"
[ "$DRY" = 1 ] || [ "$CHECK" = 1 ] || [ "$LAUNCHER_ONLY" = 1 ] || [ "$(id -u)" != 0 ] || die "do not run this as root (containers are per-user)"
if [ -n "$ISO" ]; then
  [ -f "$ISO" ] || [ "$CHECK" = 1 ] || [ "$LAUNCHER_ONLY" = 1 ] || die "ISO not found: $ISO"
  [ ! -f "$ISO" ] || ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
fi
WORK="${WORK:-$HOME/dissidia/work}"
case "$WORK" in /*) ;; *) WORK="$PWD/$WORK" ;; esac
case "$ISO$WORK$REPO" in *[[:space:]]*) die "paths containing spaces are not supported" ;; esac

export PATH="$HOME/.local/bin:$PATH"
STATE="$WORK/.deck_setup"

# The package list is part of the 'packages' stamp: change it and the step runs again.
PKGS="build-essential cmake pkg-config git curl ca-certificates unzip python3 python3-venv ccache fontconfig
  libsdl2-dev libfreetype-dev libavcodec-dev libavutil-dev libgl-dev libgl1-mesa-dri libegl1 mesa-utils
  zlib1g-dev libssl-dev fonts-dejavu-core"
PKGS="$(echo $PKGS)"

# ------------------------------------------------------------------ steps state
for r in $REDO; do
  case "$r" in container|packages|rust|extract) rm -f "$STATE/$r" ;; all) rm -rf "$STATE" ;;
    *) die "--redo: unknown step '$r' (container | packages | rust | extract | all)" ;; esac
done
done_step() { [ -f "$STATE/$1" ] && { [ -z "${2:-}" ] || [ "$(cat "$STATE/$1")" = "$2" ]; }; }
mark_step() { [ "$DRY" = 1 ] || { mkdir -p "$STATE"; printf '%s\n' "${2:-ok}" > "$STATE/$1"; }; }

# ------------------------------------------------------------------ --check
FAILS=0; WARNS=0
pass() { echo "  [PASS] $*"; }
warn() { echo "  [WARN] $*"; WARNS=$((WARNS + 1)); }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }
info() { echo "  [info] $*"; }
http() { local c; c="$(curl -sS -o /dev/null -m 15 -w '%{http_code}' "$1" 2>/dev/null)" || true; echo "${c:-000}"; }
free_gb() { df -Pk "$1" 2>/dev/null | awk 'NR==2{printf "%d", $4/1048576}'; }
nearest_dir() { local d="$1"; while [ ! -d "$d" ] && [ "$d" != / ]; do d="$(dirname "$d")"; done; echo "$d"; }

do_check() {
  echo "Checking prerequisites (nothing is changed or built)"
  echo "System"
  local id="" ver="" vendor=""
  if [ -r /etc/os-release ]; then id="$(. /etc/os-release; echo "${ID:-}")"; ver="$(. /etc/os-release; echo "${VERSION_ID:-}${BUILD_ID:+ build $BUILD_ID}")"; fi
  vendor="$(cat /sys/devices/virtual/dmi/id/board_vendor 2>/dev/null || true)"
  if [ "$id" = steamos ]; then pass "SteamOS $ver"; else warn "not SteamOS (os-release ID='$id'): fine for a generic Linux test, but untested there"; fi
  [ "$vendor" = Valve ] && info "Valve hardware (Steam Deck) detected" || info "not Valve hardware (board vendor '${vendor:-unknown}')"
  [ "$(id -u)" != 0 ] && pass "running as $(id -un), not root" || fail "running as root; run as your normal user (containers are per-user)"
  [ "$(uname -m)" = x86_64 ] && pass "x86_64" || fail "architecture $(uname -m): only x86_64 is supported"
  if command -v steamos-readonly >/dev/null; then info "root filesystem: $(steamos-readonly status 2>&1 | head -1) (this script never changes it)"; fi
  local mem_kb avail_kb
  mem_kb="$(awk '/^MemTotal/{print $2}' /proc/meminfo)"; avail_kb="$(awk '/^MemAvailable/{print $2}' /proc/meminfo)"
  if [ "${mem_kb:-0}" -ge 7000000 ]; then pass "RAM $((mem_kb / 1048576)) GB total"; else warn "RAM $((mem_kb / 1048576)) GB total: use --jobs 1 or 2"; fi
  if [ "${avail_kb:-0}" -lt $((JOBS * 1600000)) ]; then
    warn "only $((avail_kb / 1048576)) GB RAM available now; $JOBS compile jobs want ~$((JOBS * 3 / 2)) GB (close apps/games, or use --jobs $(( avail_kb / 1600000 > 0 ? avail_kb / 1600000 : 1 )))"
  else pass "$((avail_kb / 1048576)) GB RAM available for $JOBS compile jobs"; fi
  if ls /dev/dri/renderD* >/dev/null 2>&1; then
    [ -r "$(ls /dev/dri/renderD* | head -1)" ] && pass "GPU render node present and readable" || warn "/dev/dri/renderD* not readable by $(id -un)"
  else warn "no /dev/dri/renderD*: the game would fall back to software GL (very slow)"; fi

  echo "Paths and disk"
  case "$WORK" in "$HOME"/*) pass "workdir $WORK is under \$HOME" ;;
    *) if [ "$OUTSIDE_HOME" = 1 ]; then warn "workdir $WORK is outside \$HOME (allowed by flag; the container may not see it)"; else fail "workdir $WORK is outside \$HOME; the container shares \$HOME only (--allow-outside-home to override)"; fi ;; esac
  case "$REPO" in "$HOME"/*) pass "this checkout ($REPO) is under \$HOME" ;;
    *) if [ "$OUTSIDE_HOME" = 1 ]; then warn "checkout $REPO is outside \$HOME"; else fail "checkout $REPO is outside \$HOME; clone it into your home directory"; fi ;; esac
  local g; g="$(free_gb "$(nearest_dir "$WORK")")"
  if [ "${g:-0}" -ge 9 ]; then pass "${g} GB free for the workdir (need about 8; 9 required)"; else fail "only ${g:-?} GB free for the workdir; about 8 GB are needed (ISO 1 + extracted 1.6 + build 0.5 + container/Rust 1.5-3)"; fi
  if [ "$(df -Pk "$HOME" | awk 'NR==2{print $1}')" != "$(df -Pk "$(nearest_dir "$WORK")" | awk 'NR==2{print $1}')" ]; then
    g="$(free_gb "$HOME")"; [ "${g:-0}" -ge 5 ] && pass "${g} GB free in \$HOME for the container and Rust" || fail "only ${g:-?} GB free in \$HOME for the container and Rust (need 5)"
  fi
  case "$(df -Pk "$(nearest_dir "$WORK")" | awk 'NR==2{print $1}')" in /dev/mmcblk*) warn "workdir is on an SD card: builds will be slow (many small files); prefer the internal drive" ;; esac
  if [ -z "$ISO" ]; then info "no ISO given (pass one to check it)";
  elif [ ! -f "$ISO" ]; then fail "ISO not found: $ISO";
  else
    local sz magic1 magic0; sz="$(stat -c %s "$ISO")"
    magic0="$(head -c 4 "$ISO" | tr -d '\0')"; magic1="$(dd if="$ISO" bs=1 skip=32769 count=5 2>/dev/null | tr -d '\0')"
    if [ "$magic1" = CD001 ]; then pass "ISO9660 image, $((sz / 1048576)) MB"
    elif [ "$magic0" = CISO ]; then pass "CSO image, $((sz / 1048576)) MB (read directly)"
    else fail "$ISO is neither an ISO9660 image nor a CSO (ZSO/DAX/PBP/raw .bin are not supported; convert e.g. with maxcso --decompress)"; fi
    [ "$sz" -ge 400000000 ] || warn "ISO is only $((sz / 1048576)) MB; a full Dissidia 012 image is ~1 GB"
  fi
  local n=0 f; for f in analysis analysis_battle analysis_menu analysis_extra analysis_supacc; do [ -f "$REPO/games/dissidia/analysis/$f.json.gz" ] && n=$((n + 1)); done
  [ "$n" = 5 ] && pass "shipped analysis files present (no Ghidra needed)" || fail "games/dissidia/analysis/*.json.gz incomplete ($n/5): wrong branch? (the Deck cannot run Ghidra)"

  echo "Container tooling"
  if command -v podman >/dev/null; then
    pass "podman $(podman --version | awk '{print $NF}')"
    if podman info >/dev/null 2>&1; then pass "rootless podman works (podman info)"; else fail "podman info fails for $(id -un): $(podman info 2>&1 | head -2 | tr '\n' ' ')"; fi
    for f in /etc/subuid /etc/subgid; do
      if grep -qE "^($(id -un)|$(id -u)):" "$f" 2>/dev/null; then pass "$f has an entry for $(id -un)"; else warn "$f has no entry for $(id -un): rootless podman may fail to create the container (see steamdeck.md, Troubleshooting)"; fi
    done
  elif command -v docker >/dev/null; then warn "no podman, but docker found: distrobox can use it (untested)";
  else fail "neither podman nor docker found; on SteamOS podman ships with the OS (3.5+); update SteamOS"; fi
  if command -v distrobox >/dev/null; then
    pass "distrobox $(distrobox version 2>/dev/null | head -1 | awk '{print $NF}')"
    distrobox enter --help 2>&1 | grep -q -- '--no-tty' && info "distrobox enter supports --no-tty (used by play.sh)" || warn "this distrobox is old (no --no-tty); update it (--install-distrobox installs a current one into ~/.local/bin)"
  elif [ "$INSTALL_DB" = 1 ]; then
    command -v curl >/dev/null && pass "distrobox will be installed into ~/.local (--install-distrobox)" || fail "curl not found, cannot install distrobox"
  else fail "distrobox not found: re-run with --install-distrobox, or install it yourself (https://distrobox.it)"; fi
  if ! command -v curl >/dev/null; then fail "curl not found on the host"; else
    local c
    c="$(http https://registry-1.docker.io/v2/)"; case "$c" in 200|401) pass "container registry reachable (docker.io)" ;; *) done_step container && info "container registry unreachable (HTTP $c), not needed: container exists" || fail "cannot reach docker.io (HTTP $c): the image cannot be pulled" ;; esac
    c="$(http https://github.com/John-K/pspdecrypt)"; [ "$c" = 200 ] && pass "github.com reachable (pspdecrypt source)" || warn "github.com HTTP $c: needed once to fetch pspdecrypt (or pass --decrypted-eboot to setup_from_iso.py)"
    c="$(http https://sh.rustup.rs)"; [ "$c" = 200 ] && pass "sh.rustup.rs reachable" || { done_step rust && info "sh.rustup.rs unreachable, not needed: Rust installed" || fail "cannot reach sh.rustup.rs (HTTP $c)"; }
    c="$(http https://index.crates.io/config.json)"; [ "$c" = 200 ] && pass "crates.io index reachable" || warn "crates.io index HTTP $c: cargo needs it to fetch the recompiler's dependencies"
    c="$(http http://archive.ubuntu.com/ubuntu/dists/noble/Release)"; [ "$c" = 200 ] && pass "archive.ubuntu.com reachable (apt)" || { done_step packages && info "archive.ubuntu.com unreachable, not needed: packages installed" || warn "archive.ubuntu.com HTTP $c: apt inside the container needs it (mirrors may still work)"; }
  fi

  echo "Launching from Steam"
  command -v python3 >/dev/null && pass "host python3 present (--add-to-steam)" || warn "no python3 on the host: --add-to-steam cannot write the Steam entry; add play.sh by hand (steamdeck.md)"
  local sd; for sd in "$HOME/.local/share/Steam" "$HOME/.steam/steam"; do [ -d "$sd/userdata" ] && break; sd=""; done
  [ -n "$sd" ] && info "Steam account folders in $sd/userdata" || info "no Steam userdata folder yet (log in to Steam once before --add-to-steam)"
  pgrep -x steam >/dev/null 2>&1 && info "Steam is running" || info "Steam is not running (--add-to-steam needs it)"

  echo "State"
  local s; for s in container packages rust extract; do done_step "$s" && info "step '$s': done (will be skipped)" || info "step '$s': pending"; done
  if command -v distrobox >/dev/null && distrobox list 2>/dev/null | awk -F'|' '{gsub(/ /,"",$2); print $2}' | grep -qx "$NAME"; then
    pass "container '$NAME' exists"
    if distrobox enter "$NAME" -- bash -c 'for c in cmake g++ pkg-config python3 git; do command -v $c >/dev/null || echo "missing:$c"; done; python3 -c "import ensurepip" 2>/dev/null || echo missing:python3-venv; pkg-config --exists sdl2 freetype2 || echo missing:sdl2/freetype; [ -x "$HOME/.cargo/bin/cargo" ] || echo missing:cargo' 2>/dev/null | grep -q missing; then
      warn "container '$NAME' is missing build tools (the 'packages'/'rust' steps will install them)"
    else pass "container '$NAME' has the toolchain"; fi
  else info "container '$NAME' does not exist yet"; fi
  [ -f "$WORK/play.sh" ] && info "launcher exists: $WORK/play.sh"

  echo
  echo "Result: $FAILS failed, $WARNS warnings"
  [ "$FAILS" = 0 ]
}
if [ "$CHECK" = 1 ]; then do_check; exit $?; fi

# ------------------------------------------------------------------ real run
write_launcher() {
LAUNCH="$WORK/play.sh"
NOTTY=""
if command -v distrobox >/dev/null && distrobox enter --help 2>&1 | grep -q -- '--no-tty'; then NOTTY="--no-tty"; fi
echo "+ write $LAUNCH"
if [ "$DRY" = 0 ]; then
  cat > "$LAUNCH" <<EOF
#!/usr/bin/env bash
# Dissidia Trēdecim (the native Dissidia 012 build) launcher: runs the game inside the '$NAME' container. This file
# is what the Steam entry starts (deck_setup.sh --add-to-steam, see games/dissidia/docs/steamdeck.md).
# Written by deck_setup.sh.
#
# Look: PRESET below is the default. Per run (or in Steam: Properties > Launch Options) pass --retro or --crisp,
# the last one wins; any other run.sh flag works too, e.g. "--swap-ab", "--render-scale 3", "--stretch".
#   --retro  nearest-neighbour textures, 3D at 2x, whole-number scale (960x544 centred on the 1280x800 screen)
#   --crisp  3D at 3x, bilinear fit (1280x725)
PRESET=--$PRESET
export PATH="\$HOME/.local/bin:\$PATH"

# Steam injects its overlay/runtime libraries into what it launches; the host's podman must not see them.
unset LD_PRELOAD LD_LIBRARY_PATH

# distrobox does not forward the whole environment. Hand the container what the game needs: display and audio
# sockets, gamescope, and above all Steam Input's SDL_GAMECONTROLLER_* variables (they hide the Deck's raw pad so
# only Steam's virtual Xbox-style controller is seen).
ENVF="$WORK/.play_env"
: > "\$ENVF"
for v in \$(compgen -e | grep -E '^(SDL_|SteamDeck\$|SteamGameId\$|SteamAppId\$|STEAM_|GAMESCOPE|DISPLAY\$|WAYLAND_DISPLAY\$|XAUTHORITY\$|XDG_RUNTIME_DIR\$|XDG_SESSION_TYPE\$|PULSE_|PIPEWIRE_|DBUS_SESSION_BUS_ADDRESS\$|LIBGL_|MESA_|PSPRECOMP_|ENABLE_GAMESCOPE_WSI\$)'); do
  declare -px "\$v" >> "\$ENVF"
done

exec distrobox enter $NOTTY $NAME -- bash -c '. "\$1"; shift; exec "\$@"' _ "\$ENVF" "$HERE/run.sh" "$WORK" "\$PRESET" --fullscreen "\$@"
EOF
  chmod +x "$LAUNCH"
fi
}
add_to_steam() {
  # The Steam entry ("Dissidia Trēdecim") and its library artwork; host python3, standard library only.
  local args=("$WORK/play.sh" --art "$HERE/../steam" --match-dir "$WORK" --match-dir "$(dirname "$REPO")")
  [ "$DRY" = 0 ] || args+=(--dry-run)
  echo "+ python3 $HERE/steam_shortcut.py ${args[*]}"
  command -v python3 >/dev/null || { echo "python3 not found on the host; add $WORK/play.sh by hand, see docs/steamdeck.md." >&2; return 0; }
  python3 "$HERE/steam_shortcut.py" "${args[@]}" || echo "adding to Steam failed (above); see \"Add it to Steam\" in docs/steamdeck.md." >&2
}
if [ "$LAUNCHER_ONLY" = 1 ]; then
  [ "$DRY" = 1 ] || mkdir -p "$WORK"
  write_launcher
  echo "wrote $WORK/play.sh (look: $PRESET)"
  [ "$ADD_STEAM" = 0 ] || add_to_steam
  exit 0
fi
run() { echo "+ $*"; [ "$DRY" = 1 ] || "$@"; }
if [ -n "$ISO" ]; then
  case "$ISO" in "$HOME"/*) ;; *) [ "$OUTSIDE_HOME" = 1 ] || die "ISO is outside \$HOME ($ISO); copy it into your home directory (or --allow-outside-home)" ;; esac
fi
case "$WORK" in "$HOME"/*) ;; *) [ "$OUTSIDE_HOME" = 1 ] || die "workdir is outside \$HOME ($WORK); use one under \$HOME (or --allow-outside-home)" ;; esac
case "$REPO" in "$HOME"/*) ;; *) [ "$OUTSIDE_HOME" = 1 ] || die "this checkout is outside \$HOME ($REPO); clone it under your home directory (or --allow-outside-home)" ;; esac

# Cheap safety net before the hours-long part: fail fast on the things --check calls FAIL.
if [ "$DRY" = 0 ]; then
  echo "Running the prerequisite check first (re-run with --check to see it alone)"
  do_check || die "prerequisite check failed (above). Fix the FAIL lines and re-run; finished steps are kept."
  echo
fi
[ "$DRY" = 1 ] || mkdir -p "$WORK"

if ! command -v distrobox >/dev/null; then
  if [ "$INSTALL_DB" = 1 ]; then
    command -v curl >/dev/null || die "curl not found"
    echo "+ curl -fsSL https://raw.githubusercontent.com/89luca89/distrobox/main/install | sh -s -- --prefix \$HOME/.local"
    [ "$DRY" = 1 ] || curl -fsSL https://raw.githubusercontent.com/89luca89/distrobox/main/install | sh -s -- --prefix "$HOME/.local"
  elif [ "$DRY" = 0 ]; then
    die "distrobox not found. Re-run with --install-distrobox, or install it yourself (https://distrobox.it)"
  fi
fi
if [ "$DRY" = 0 ]; then
  command -v distrobox >/dev/null || die "distrobox still not found after install"
  command -v podman >/dev/null || command -v docker >/dev/null || die "distrobox needs podman (or docker); neither found on this system"
fi

# 1. container
if [ "$DRY" = 1 ] || ! distrobox list 2>/dev/null | awk -F'|' '{gsub(/ /,"",$2); print $2}' | grep -qx "$NAME"; then
  run distrobox create --yes --name "$NAME" --image "$IMAGE"
  mark_step container "$NAME $IMAGE"
else
  echo "container '$NAME' exists, skipping create"
fi
inbox() { run distrobox enter "$NAME" -- "$@"; }

# 2. packages (apt inside the container; sudo is passwordless there)
if done_step packages "$PKGS"; then echo "packages: done, skipping (--redo packages)"; else
  inbox bash -c "sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y $PKGS"
  inbox bash -c 'ccache -M 3G >/dev/null 2>&1 || true'
  mark_step packages "$PKGS"
fi

# 3. Rust (rustup: the distro rustc is too old for the recompiler)
if done_step rust; then echo "rust: done, skipping (--redo rust)"; else
  inbox bash -c 'command -v cargo >/dev/null || [ -x "$HOME/.cargo/bin/cargo" ] || \
    curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal'
  mark_step rust
fi

# 4. extract the ISO (setup_from_iso.py resumes by itself; the stamp only avoids re-reading the ISO)
if [ -n "$ISO" ]; then ISO_KEY="$(stat -c '%s:%Y' "$ISO" 2>/dev/null || echo unknown)"; else ISO_KEY="none"; fi
if done_step extract "$ISO_KEY" && [ -f "$WORK/disc0/PSP_GAME/SYSDIR/EBOOT.BIN" ]; then
  echo "extract: done for this ISO, skipping (--redo extract)"
else
  [ -n "$ISO" ] || [ "$DRY" = 1 ] || die "the game ISO is needed for the extract step: deck_setup.sh <game.iso> [workdir]"
  inbox python3 "$HERE/setup_from_iso.py" "$ISO" "$WORK"
  mark_step extract "$ISO_KEY"
fi

# 5. analysis (shipped) + recompile + overlays + C++ build; every part resumes by itself
inbox bash -c ". \"\$HOME/.cargo/env\" 2>/dev/null; exec \"$HERE/pipeline.sh\" \"$WORK\" --jobs $JOBS"

# 6. mods: link <workdir>/mods to the checkout's tracked mods unless the workdir already has its own
if [ -d "$REPO/work/mods" ]; then
  if [ -L "$WORK/mods" ] || [ ! -e "$WORK/mods" ]; then
    run ln -sfn "$REPO/work/mods" "$WORK/mods"
  else
    echo "mods: $WORK/mods is a folder of its own, left as it is. To use the repo's mods instead:" >&2
    echo "      mv $WORK/mods $WORK/mods_old && ln -s $REPO/work/mods $WORK/mods" >&2
  fi
fi

# 7. the tools' Python packages (numpy, Pillow, scipy at the versions in tools/requirements.txt, in tools/.venv):
#    run.sh builds the character mod folders with them at every start, and Game Mode shows no download progress
inbox python3 "$HERE/pydeps.py" \
  || echo "warning: the tools' Python packages were not set up (above); run.sh tries again at the next start" >&2

# 8. host-side launcher
write_launcher
LAUNCH="$WORK/play.sh"

[ "$ADD_STEAM" = 0 ] || add_to_steam
cat <<EOF

Done. Play from Desktop Mode with:   $LAUNCH      (look: $PRESET; "$LAUNCH --crisp" for the other)
Add it to Steam (Game Mode):         $0 --launcher-only --add-to-steam   (as "Dissidia Trēdecim", with artwork)
Saves:                               $WORK/ms0/PSP/SAVEDATA   (back this folder up)
EOF
