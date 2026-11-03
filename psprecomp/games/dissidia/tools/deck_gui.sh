#!/usr/bin/env bash
# Dissidia Trēdecim on the Steam Deck without a terminal: a point-and-click front end for deck_setup.sh.
#
# In Desktop Mode, double-click it in Dolphin and choose Execute. Everything is a dialog (zenity, or kdialog):
#   * first run: if this file is on its own (downloaded from GitHub), it downloads the project into ~/dissidia/repo,
#     asking for your GitHub login in a dialog if the repository is private;
#   * Install: pick your game file (linked or copied to ~/dissidia/game.iso), build with a progress bar (the Deck is
#     kept awake while it builds), and add "Dissidia Trēdecim" to Steam with its artwork;
#   * Update: pull the latest code and mods, rebuild what changed, refresh the Steam entry;
#   * Look (Retro / Crisp), Check (deck_setup.sh --check) and the last log.
# It puts a "Dissidia Trēdecim Setup" icon on the desktop and in the application menu (Games), so later runs are one
# double-click. Underneath it is deck_setup.sh with the usual flags; its output goes to ~/dissidia/work/setup.log.
#
# Options (mainly for testing): --repo-url URL (first download), --base DIR (default ~/dissidia).
set -uo pipefail

APP="Dissidia Trēdecim"
BASE="$HOME/dissidia"
REPO_URL="https://github.com/dissidia-fan/dissidia"
SELF="$(readlink -f "${BASH_SOURCE[0]}")"
HERE="$(dirname "$SELF")"

# ------------------------------------------------------------------ dialogs
UI="${DISSIDIA_GUI_UI:-}"
if [ -z "$UI" ]; then
  if command -v zenity >/dev/null 2>&1; then UI=zenity; elif command -v kdialog >/dev/null 2>&1; then UI=kdialog; fi
fi
QDBUS=""; for q in qdbus qdbus6 qdbus-qt5; do command -v $q >/dev/null 2>&1 && { QDBUS=$q; break; }; done
esc() { local s="${1//&/&amp;}"; s="${s//</&lt;}"; printf '%s' "${s//>/&gt;}"; }   # zenity texts are Pango markup

say() {   # say info|error|warning TEXT
  local kind="$1"; shift
  case "$UI" in
    zenity) zenity --"$kind" --title="$APP" --width=460 --text="$(esc "$*")" 2>/dev/null ;;
    kdialog) case "$kind" in info) kdialog --title "$APP" --msgbox "$*" ;; error) kdialog --title "$APP" --error "$*" ;;
             *) kdialog --title "$APP" --sorry "$*" ;; esac 2>/dev/null ;;
    *) echo "$*" >&2 ;;
  esac
  return 0
}
ask() {   # ask TEXT [yes-label] [no-label]  -> status 0 for yes
  local y="${2:-Yes}" n="${3:-No}"
  case "$UI" in
    zenity) zenity --question --title="$APP" --width=460 --ok-label="$y" --cancel-label="$n" --text="$(esc "$1")" 2>/dev/null ;;
    kdialog) kdialog --title "$APP" --yes-label "$y" --no-label "$n" --yesno "$1" 2>/dev/null ;;
    *) return 1 ;;
  esac
}
entry() { # entry TEXT DEFAULT  -> stdout
  case "$UI" in
    zenity) zenity --entry --title="$APP" --width=520 --text="$(esc "$1")" --entry-text="$2" 2>/dev/null ;;
    kdialog) kdialog --title "$APP" --inputbox "$1" "$2" 2>/dev/null ;;
    *) return 1 ;;
  esac
}
secret() { # secret TEXT  -> stdout
  case "$UI" in
    zenity) zenity --entry --hide-text --title="$APP" --width=520 --text="$(esc "$1")" 2>/dev/null ;;
    kdialog) kdialog --title "$APP" --password "$1" 2>/dev/null ;;
    *) return 1 ;;
  esac
}
pick_file() { # pick_file TITLE  -> stdout
  case "$UI" in
    zenity) zenity --file-selection --title="$1" --filename="$HOME/" \
              --file-filter="Game image (.iso, .cso) | *.iso *.ISO *.cso *.CSO" --file-filter="All files | *" 2>/dev/null ;;
    kdialog) kdialog --title "$1" --getopenfilename "$HOME" "*.iso *.ISO *.cso *.CSO|Game image (.iso, .cso)" 2>/dev/null ;;
    *) return 1 ;;
  esac
}
menu() {  # menu TEXT DEFAULT key label [key label ...]  -> chosen key on stdout
  local text="$1" def="$2"; shift 2
  case "$UI" in
    zenity)
      local rows=() k
      while [ $# -ge 2 ]; do k=FALSE; [ "$1" = "$def" ] && k=TRUE; rows+=("$k" "$1" "$2"); shift 2; done
      zenity --list --radiolist --title="$APP" --text="$(esc "$text")" --width=600 --height=360 \
        --column="" --column=key --column="What to do" --hide-column=2 --print-column=2 --hide-header \
        --ok-label="Go" --cancel-label="Close" "${rows[@]}" 2>/dev/null | cut -d'|' -f1 ;;
    kdialog) kdialog --title "$APP" --default "$def" --menu "$text" "$@" 2>/dev/null ;;
    *) return 1 ;;
  esac
}
show_file() { # show_file TITLE FILE
  case "$UI" in
    zenity) zenity --text-info --title="$1" --filename="$2" --width=900 --height=600 --font=monospace 2>/dev/null ;;
    kdialog) kdialog --title "$1" --geometry 900x600 --textbox "$2" 2>/dev/null ;;
    *) cat "$2" >&2 ;;
  esac
  return 0
}
# A progress window fed on stdin with zenity's protocol: "NN" sets the bar, "# text" the label. It closes at the end
# of the input; pressing Stop closes it early (the caller tells the two apart). PULSE=1: no percentages, just motion.
progress_window() { # progress_window TEXT
  case "$UI" in
    zenity) local p=(); [ "${PULSE:-0}" = 1 ] && p=(--pulsate)
            zenity --progress "${p[@]}" --title="$APP" --width=560 --text="$(esc "$1")" --percent=0 --auto-close \
              --cancel-label="Stop" 2>/dev/null ;;
    kdialog)
      local ref line
      ref="$(kdialog --title "$APP" --progressbar "$1" 100 2>/dev/null)" || { cat >/dev/null; return 0; }
      [ -z "$QDBUS" ] || $QDBUS $ref showCancelButton true >/dev/null 2>&1
      while IFS= read -r line; do
        [ -n "$QDBUS" ] || continue
        if [ "$($QDBUS $ref wasCancelled 2>/dev/null)" = true ]; then break; fi
        case "$line" in
          "#"*) $QDBUS $ref setLabelText "${line#\# }" >/dev/null 2>&1 ;;
          [0-9]*) $QDBUS $ref Set "" value "$line" >/dev/null 2>&1 ;;
        esac
      done
      [ -z "$QDBUS" ] || $QDBUS $ref close >/dev/null 2>&1 ;;
    *) local line; while IFS= read -r line; do case "$line" in "#"*) echo "${line#\# }" >&2 ;; esac; done ;;
  esac
  return 0
}

# ------------------------------------------------------------------ GitHub login: git runs us as GIT_ASKPASS
if [ -n "${DISSIDIA_GUI_ASKPASS:-}" ]; then
  case "${1:-}" in
    Username*) entry "Signing in to GitHub to download the project.

GitHub user name:" "" ;;
    *) secret "GitHub access token for the project.

(GitHub does not take your account password here. Make a token on github.com: Settings > Developer settings > Personal access tokens, with read access to the repository.)" || exit 1
       if [ ! -e "$DISSIDIA_GUI_ASKPASS/asked" ]; then
         : > "$DISSIDIA_GUI_ASKPASS/asked"
         ask "Remember this login on the Deck, so updates do not ask again?

(It is saved in ~/.git-credentials, readable only by you.)" "Remember" "Ask every time" \
           && : > "$DISSIDIA_GUI_ASKPASS/remember"
       fi ;;
  esac
  exit 0
fi

# ------------------------------------------------------------------ jobs
# Run a command with the dialog login for git. Git's 'store' helper keeps a login only after it worked; if the person
# chose not to be remembered, it is taken out again afterwards. A credential helper they set up themselves wins.
with_login() { # with_login [--remember-in DIR] cmd...
  local repo=""; [ "${1:-}" = --remember-in ] && { repo="$2"; shift 2; }
  local d rc own
  d="$(mktemp -d)"; chmod 700 "$d"
  own="$(git ${repo:+-C "$repo"} config --get credential.helper 2>/dev/null)"
  if [ -n "$own" ]; then
    DISSIDIA_GUI_ASKPASS="$d" GIT_ASKPASS="$SELF" SSH_ASKPASS="$SELF" GIT_TERMINAL_PROMPT=0 "$@"; rc=$?
  else
    DISSIDIA_GUI_ASKPASS="$d" GIT_ASKPASS="$SELF" SSH_ASKPASS="$SELF" GIT_TERMINAL_PROMPT=0 \
      GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=credential.helper GIT_CONFIG_VALUE_0=store "$@"; rc=$?
    if [ -e "$d/asked" ]; then
      if [ -e "$d/remember" ] && [ $rc = 0 ] && [ -n "$repo" ] && [ -d "$repo" ]; then
        git -C "$repo" config credential.helper store
      else
        printf 'protocol=https\nhost=github.com\n\n' | git -c credential.helper=store credential reject 2>/dev/null
      fi
    fi
  fi
  rm -rf "$d"
  return $rc
}

# Map deck_setup.sh / pipeline.sh / setup_from_iso.py output onto a progress bar and a readable label.
to_progress() {
  local pct=0 line p n
  bar() { if [ "$1" -gt "$pct" ]; then pct=$1; echo "$pct"; fi; }
  while IFS= read -r line; do
    case "$line" in
      *"git -C"*"pull"*)                       echo "# Downloading the latest code and mods…"; bar 2 ;;
      "Checking prerequisites (nothing"*)      echo "# Checking the Deck is ready…"; bar 3 ;;
      *"distrobox/main/install"*)              echo "# Installing distrobox (one time)…"; bar 4 ;;
      "+ distrobox create"*)                   echo "# Creating the build container (one time)…"; bar 5 ;;
      *"apt-get install"*)                     echo "# Installing compilers and libraries in the container (one time)…"; bar 10 ;;
      *"sh.rustup.rs"*)                        echo "# Installing Rust (one time)…"; bar 20 ;;
      *setup_from_iso.py*)                     echo "# Extracting your game…"; bar 24 ;;
      "[2/4]"*)                                echo "# Decrypting the game's program…"; bar 28 ;;
      "[3/4]"*|"[4/4]"*)                       bar 31 ;;
      *"/pipeline.sh"*)                        echo "# Preparing the build…"; bar 33 ;;
      "=== Building the psprecomp recompiler"*) echo "# Building the recompiler…"; bar 35 ;;
      "   Compiling "*)                        n="${line#   Compiling }"; echo "# Building the recompiler… (${n%% *})" ;;
      "=== Rebuilding analysis"*)              echo "# Matching the game's code…"; bar 44 ;;
      "=== Recompiling EBOOT"*)                echo "# Translating the game's code…"; bar 47 ;;
      "=== Building overlay sources"*)         echo "# Translating the battle and menu code…"; bar 50 ;;
      "=== Recompile + overlays: up to date"*) bar 52 ;;
      "=== Configuring runtime"*)              echo "# Preparing the compile…"; bar 54 ;;
      "=== Building psprecomp_runtime"*)       echo "# Compiling the game (the long part)…"; bar 56 ;;
      "["*"%]"*)
        p="${line#[}"; p="${p%%\%]*}"; p="${p// /}"
        case "$p" in ''|*[!0-9]*) ;; *) echo "# Compiling the game… $p%"; bar $((56 + p * 38 / 100)) ;; esac ;;
      "=== Done"*)                             bar 94 ;;
      *pydeps.py*)                             echo "# Setting up the mod tools…"; bar 95 ;;
      "+ write "*play.sh)                      echo "# Writing the launcher…"; bar 97 ;;
      *steam_shortcut.py*)                     echo "# Adding $APP to Steam (Steam closes and reopens)…"; bar 98 ;;
    esac
  done
  echo 100
}

# Run a long command behind a progress window, its output in $LOG. Returns its status; 130 if the person stopped it.
run_job() { # run_job TEXT cmd...
  local text="$1"; shift
  mkdir -p "$WORK"
  [ -f "$LOG" ] && mv -f "$LOG" "$LOG.previous"
  printf '### %s  %s\n\n' "$(date '+%F %T')" "$*" > "$LOG"
  local inhibit=()
  systemd-inhibit --list >/dev/null 2>&1 && \
    inhibit=(systemd-inhibit --what=idle:sleep --who="$APP" --why="Building the game" --mode=block)
  setsid "${inhibit[@]}" "$@" </dev/null >>"$LOG" 2>&1 &
  local pid=$! rc feeder
  while :; do
    progress_window "$text" < <(tail -n +1 -F --pid=$pid "$LOG" 2>/dev/null | to_progress); feeder=$!
    pkill -P $feeder 2>/dev/null; kill $feeder 2>/dev/null
    kill -0 $pid 2>/dev/null || break                     # finished: the window closed by itself
    if ask "Stop now?

Nothing is lost: running Install or Update again continues where it stopped." "Stop" "Keep going"; then
      kill -TERM -- -$pid 2>/dev/null || kill -TERM $pid 2>/dev/null
      wait $pid 2>/dev/null; echo "### stopped by the user" >> "$LOG"
      return 130
    fi
  done
  wait $pid; rc=$?
  echo "### exit status $rc" >> "$LOG"
  return $rc
}

# A short job with a moving bar and no percentages; its output in the file named by $OUT.
pulse() { # pulse TEXT cmd...
  local text="$1"; shift
  "$@" > "$OUT" 2>&1 &
  local pid=$!
  ( while kill -0 $pid 2>/dev/null; do echo "# $text"; sleep 1; done; echo 100 ) | PULSE=1 progress_window "$text"
  wait $pid
}

failed() { # failed WHAT
  local t; t="$(mktemp)"
  { echo "$1 did not finish. The end of the log is below; the whole log is ~/dissidia/work/setup.log"
    echo "Running $1 again continues where it stopped."; echo
    grep -v '^###' "$LOG" | tail -n 40; } > "$t"
  show_file "$APP: $1 did not finish" "$t"; rm -f "$t"
}

# Put the game at ~/dissidia/game.iso (deck_setup.sh wants it under $HOME, with no spaces in the path).
ensure_iso() {
  ISO="$BASE/game.iso"
  [ -f "$ISO" ] && return 0
  say info "Choose your copy of Dissidia 012 Duodecim: an .iso or .cso file, on the Deck, an SD card or a USB stick.

It is placed at ~/dissidia/game.iso (linked if it is on the Deck already, copied otherwise; the original stays where it is)."
  local src size m0 m1
  src="$(pick_file "Choose your Dissidia 012 game file (.iso or .cso)")" || return 1
  [ -n "$src" ] && [ -f "$src" ] || return 1
  size="$(stat -c %s "$src")"
  m0="$(head -c 4 "$src" | tr -d '\0')"; m1="$(dd if="$src" bs=1 skip=32769 count=5 2>/dev/null | tr -d '\0')"
  if [ "$m1" != CD001 ] && [ "$m0" != CISO ]; then
    say error "That file is not an ISO or CSO game image:
$src

ZSO, PBP and .bin dumps are not supported."; return 1
  fi
  [ "$size" -ge 400000000 ] || ask "That image is only $((size / 1048576)) MB; the whole game is about 1 GB. Use it anyway?" "Use it" "Cancel" || return 1
  mkdir -p "$BASE"
  ln "$src" "$ISO" 2>/dev/null && return 0                 # same drive: a hard link, no extra space
  local free; free="$(df -Pk "$BASE" | awk 'NR==2{print $4 * 1024}')"
  [ "${free:-0}" -gt "$size" ] || { say error "Not enough free space on the Deck to copy the game ($((size / 1048576)) MB)."; return 1; }
  cp -- "$src" "$ISO.part" &
  local cp_pid=$!
  progress_window "Copying the game to ~/dissidia/game.iso…" < <(
    while kill -0 $cp_pid 2>/dev/null; do echo $(( $(stat -c %s "$ISO.part" 2>/dev/null || echo 0) * 99 / size )); sleep 1; done; echo 100)
  if kill -0 $cp_pid 2>/dev/null; then kill $cp_pid; wait $cp_pid; rm -f "$ISO.part"; return 1; fi
  wait $cp_pid && mv -- "$ISO.part" "$ISO" || { rm -f "$ISO.part"; say error "Copying the game failed."; return 1; }
}

# The desktop and application-menu icon that starts this script; rewritten if the checkout moves.
install_icons() {
  case "$SELF" in "$HOME"/*) ;; *) return 0 ;; esac
  local icon f content desk
  icon="$(readlink -f "$HERE/../steam/icon.png" 2>/dev/null)"; [ -f "$icon" ] || icon=applications-games
  content="[Desktop Entry]
Type=Application
Name=$APP Setup
Comment=Install, update or change the look of $APP
Exec=bash \"$SELF\"
Icon=$icon
Terminal=false
Categories=Game;
"
  desk="$(xdg-user-dir DESKTOP 2>/dev/null)"; [ -n "$desk" ] || desk="$HOME/Desktop"
  mkdir -p "$HOME/.local/share/applications"
  for f in "$HOME/.local/share/applications/dissidia-tredecim-setup.desktop" "$desk/dissidia-tredecim-setup.desktop"; do
    [ -d "$(dirname "$f")" ] || continue
    if [ ! -f "$f" ] || [ "$(cat "$f")" != "${content%$'\n'}" ]; then printf '%s' "$content" > "$f"; chmod +x "$f"; fi
  done
}

# ------------------------------------------------------------------ first run: get the project
bootstrap() {
  local clone="$BASE/repo" gui="psprecomp/games/dissidia/tools/deck_gui.sh"
  OUT="$(mktemp)"
  if [ -f "$clone/$gui" ]; then exec bash "$clone/$gui" "$@"; fi
  if [ -d "$clone/.git" ]; then
    ask "Your copy of the project in ~/dissidia/repo is older than this setup tool. Download the latest version now?" "Download" "Cancel" || exit 0
    pulse "Downloading the latest version…" with_login --remember-in "$clone" git -C "$clone" pull --ff-only \
      || { say error "Updating ~/dissidia/repo failed:

$(tail -n 15 "$OUT")"; exit 1; }
  else
    ask "$APP setup

This downloads the project into ~/dissidia/repo, then helps you install the game. You need:
 • your own copy of Dissidia 012 (.iso or .cso)
 • about 9 GB of free space
 • the Deck plugged in: the first build takes a while" "Continue" "Cancel" || exit 0
    local url; url="$(entry "Project address on GitHub:" "$REPO_URL")" || exit 0
    [ -n "$url" ] || exit 0
    mkdir -p "$BASE"
    pulse "Downloading the project…" with_login --remember-in "$clone" git clone --progress "$url" "$clone" \
      || { say error "Downloading the project failed:

$(tail -n 15 "$OUT")"; exit 1; }
  fi
  [ -f "$clone/$gui" ] || { say error "~/dissidia/repo has no setup tool ($gui). Is it the right repository and branch?"; exit 1; }
  exec bash "$clone/$gui" "$@"
}

# ------------------------------------------------------------------ main
main() {
  local a=("$@")
  while [ $# -gt 0 ]; do
    case "$1" in --repo-url) REPO_URL="$2"; shift 2 ;; --base) BASE="$2"; shift 2 ;; *) shift ;; esac
  done
  WORK="$BASE/work"; LOG="$WORK/setup.log"
  [ -n "$UI" ] || { echo "No dialog program (zenity or kdialog) found; use deck_setup.sh in a terminal (docs/steamdeck.md)." >&2; exit 1; }
  [ "$(id -u)" != 0 ] || { say error "Do not run this as root."; exit 1; }
  [ -f "$HERE/deck_setup.sh" ] || bootstrap "${a[@]}"
  local SETUP="$HERE/deck_setup.sh" REPO
  REPO="$(git -C "$HERE" rev-parse --show-toplevel 2>/dev/null || echo "$HERE")"
  install_icons

  local choice def look preset out what args rc f iso
  while :; do
    if [ -x "$WORK/play.sh" ]; then def=update; else def=install; fi
    look="$(sed -n 's/^PRESET=--//p' "$WORK/play.sh" 2>/dev/null)"
    choice="$(menu "$( [ "$def" = install ] && echo "$APP is not installed yet." || echo "$APP is installed (look: ${look:-retro}).")

What would you like to do?" "$def" \
      install "Install (or repair): build the game and add it to Steam" \
      update  "Update: get the latest code and mods, rebuild what changed" \
      look    "Change the look (Retro or Crisp)" \
      check   "Check the Deck is ready (changes nothing)" \
      log     "Show the last log")" || exit 0
    case "$choice" in
      install|update)
        if [ "$choice" = install ]; then
          what=Install
          ask "Install $APP

The first build takes a while (tens of minutes). Keep the Deck plugged in; it is kept awake until the build is done.
If it stops part-way, choose Install again: finished steps are skipped." "Start" "Cancel" || continue
        else what=Update; fi
        ensure_iso || continue
        args=("$ISO" --install-distrobox --add-to-steam)
        [ "$choice" = update ] && args+=(--update)
        if pgrep -x steam >/dev/null 2>&1; then
          ask "At the end Steam closes for a moment while $APP is added to the library, then opens again." "OK" "Cancel" || continue
          args+=(--close-steam)
        fi
        with_login --remember-in "$REPO" run_job "$what: starting…" bash "$SETUP" "${args[@]}"; rc=$?
        if [ $rc = 0 ]; then
          say info "$what finished.

Go back to Game Mode (the \"Return to Gaming Mode\" icon on the desktop) and start $APP from your library.
Your saves are in ~/dissidia/work/ms0/PSP/SAVEDATA."
          exit 0
        elif [ $rc != 130 ]; then failed "$what"; fi ;;
      look)
        preset="$(menu "How should the game look?" "${look:-retro}" \
          retro "Retro: blocky PSP textures, 3D at 2x, sharp whole-number scaling (easiest on the battery)" \
          crisp "Crisp: 3D at 3x, smooth textures, fills more of the screen")" || continue
        [ -n "$preset" ] || continue
        if out="$(bash "$SETUP" --launcher-only "$WORK" --preset "$preset" 2>&1)"; then
          say info "The look is now $preset. It applies the next time you start the game."
        else say error "Changing the look failed:

$out"; fi ;;
      check)
        f="$(mktemp)"; iso=(); [ -f "$BASE/game.iso" ] && iso=("$BASE/game.iso")
        bash "$SETUP" "${iso[@]}" "$WORK" --check --install-distrobox > "$f" 2>&1
        show_file "$APP: check" "$f"; rm -f "$f" ;;
      log)
        if [ -f "$LOG" ]; then show_file "$APP: last log" "$LOG"; else say info "There is no log yet."; fi ;;
      *) exit 0 ;;
    esac
  done
}
main "$@"
exit
