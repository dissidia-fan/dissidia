#!/usr/bin/env bash
# One-command install for a Linux PC or a Mac: the build tools, Rust, your game data, the build, and a launcher.
# (The Steam Deck has its own: deck_setup.sh.) Run it as your normal user; it asks for your password only to
# install missing system packages (apt on Linux; on a Mac, Homebrew needs none).
#
# Usage: install.sh <game.iso|game.cso> [workdir] [options]
#   workdir     default ~/dissidia/work
#   --jobs N    parallel compile jobs (default: pipeline.sh's, min(3, CPUs); each needs about 1.5 GB RAM)
#   --update    first pull the latest code and mods (git pull --ff-only in this checkout), then run as usual
#   --no-packages  don't install system packages (you installed them yourself)
#
# Resumable: run the same command again after an interruption; finished steps are skipped (setup_from_iso.py and
# pipeline.sh resume by themselves). When it is done, play with <workdir>/play.sh.
set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
step() { echo; echo "== $*"; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

ISO=""; WORK=""; JOBS=""; UPDATE=0; PACKAGES=1; ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    --update) UPDATE=1; shift ;;
    --no-packages) PACKAGES=0; shift ;;
    -h|--help) sed -n '2,/^set -euo/{/^set -euo/!p;}' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) if [ -z "$ISO" ]; then ISO="$1"; elif [ -z "$WORK" ]; then WORK="$1"; else die "unexpected argument $1"; fi; shift ;;
  esac
done
if [ "$UPDATE" = 1 ]; then
  REST=(); for a in "${ARGS[@]}"; do [ "$a" = --update ] || REST+=("$a"); done
  echo "+ git -C $REPO pull --ff-only"
  git -C "$REPO" pull --ff-only || die "git pull failed (local changes in $REPO? see 'git -C $REPO status')"
  exec bash "$HERE/install.sh" ${REST[@]+"${REST[@]}"}
fi
[ -n "$ISO" ] || die "usage: install.sh <game.iso|game.cso> [workdir] [--jobs N]   (put your game at ~/dissidia/game.iso)"
[ -f "$ISO" ] || die "game not found: $ISO (put your copy of the game there, named game.iso)"
[ "$(id -u)" != 0 ] || die "run this as your normal user, not with sudo"
case "$JOBS" in ''|*[!0-9]*) [ -z "$JOBS" ] || die "--jobs must be a positive integer" ;; esac
ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
WORK="${WORK:-$HOME/dissidia/work}"
case "$WORK" in /*) ;; *) WORK="$PWD/$WORK" ;; esac
mkdir -p "$WORK"
OS="$(uname -s)"

# ------------------------------------------------------------------ 1. system packages
if [ "$OS" = Darwin ]; then
  step "Build tools (Homebrew)"
  if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "Apple's command line tools are being installed (a window opened). When that finishes, run this command again."
  fi
  BREW="$(command -v brew || true)"
  [ -n "$BREW" ] || for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && BREW="$b" && break; done
  [ -n "$BREW" ] || die "Homebrew not found. Install it from https://brew.sh, then run this command again."
  eval "$("$BREW" shellenv)"
  FORMULAS="cmake pkg-config sdl2 freetype ffmpeg openssl@3 python"
  MISSING=""; for f in $FORMULAS; do "$BREW" list --formula "$f" >/dev/null 2>&1 || MISSING="$MISSING $f"; done
  if [ -z "$MISSING" ]; then echo "all installed: $FORMULAS"
  elif [ "$PACKAGES" = 1 ]; then echo "+ brew install$MISSING"; "$BREW" install $MISSING
  else die "missing Homebrew packages:$MISSING (brew install$MISSING)"; fi
elif [ "$OS" = Linux ]; then
  step "Build tools"
  PKGS="build-essential cmake pkg-config git curl python3 python3-venv libsdl2-dev libfreetype-dev libavcodec-dev
    libavutil-dev libgl-dev zlib1g-dev libssl-dev fonts-dejavu-core"
  if command -v dpkg >/dev/null && command -v apt-get >/dev/null; then
    MISSING=""; for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || MISSING="$MISSING $p"; done
    if [ -z "$MISSING" ]; then echo "all installed"
    elif [ "$PACKAGES" = 1 ]; then echo "+ sudo apt-get install -y$MISSING"; sudo apt-get update && sudo apt-get install -y $MISSING
    else die "missing packages:$MISSING"; fi
  else
    echo "Not an apt-based system: install the equivalents of these yourself if the build fails:"
    echo "  $(echo $PKGS)"
    for c in cmake pkg-config git curl python3; do command -v "$c" >/dev/null || die "$c not found; install the packages above first"; done
  fi
else
  die "unsupported system $OS (Linux and macOS only; on a Steam Deck use deck_setup.sh)"
fi

# ------------------------------------------------------------------ 2. Rust
step "Rust"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
if command -v cargo >/dev/null; then echo "found $(cargo --version)"; else
  echo "+ rustup (https://sh.rustup.rs), into ~/.cargo"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
  . "$HOME/.cargo/env"
fi

# ------------------------------------------------------------------ 3. Python 3.11+ for the tools
PY=""
for c in python3.13 python3.12 python3.11 python3; do
  p="$(command -v "$c" || true)"; [ -n "$p" ] || continue
  "$p" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null && PY="$p" && break
done
[ -n "$PY" ] || die "Python 3.11 or newer not found (on a Mac: brew install python)"

# ------------------------------------------------------------------ 4. your game
step "Unpacking and decrypting your game"
if [ -f "$WORK/disc0/PSP_GAME/SYSDIR/EBOOT.BIN" ] && [ -f "$WORK/.install_extract" ] && [ "$(cat "$WORK/.install_extract")" = "$ISO" ]; then
  echo "done before, skipping"
else
  "$PY" "$HERE/setup_from_iso.py" "$ISO" "$WORK"
  echo "$ISO" > "$WORK/.install_extract"
fi

# ------------------------------------------------------------------ 5. the build
step "Building (long the first time; later runs only rebuild what changed)"
"$HERE/pipeline.sh" "$WORK" ${JOBS:+--jobs "$JOBS"}

# ------------------------------------------------------------------ 6. the tools' Python packages
step "Python packages for the character mods"
"$PY" "$HERE/pydeps.py" || echo "warning: not set up (above); run.sh tries again at the next start" >&2

# ------------------------------------------------------------------ 7. launcher
LAUNCH="$WORK/play.sh"
cat > "$LAUNCH" <<EOS
#!/usr/bin/env bash
# Dissidia Trēdecim launcher, written by install.sh. Any run.sh flag works: --crisp, --fullscreen, --swap-ab ...
exec "$HERE/run.sh" "$WORK" "\$@"
EOS
chmod +x "$LAUNCH"

cat <<EOS

Done. Play with:   $LAUNCH            ($LAUNCH --crisp for the smoother look, --fullscreen for full screen)
Update later:      $HERE/install.sh $ISO --update
Saves:             $WORK/ms0/PSP/SAVEDATA   (back this folder up)
EOS
