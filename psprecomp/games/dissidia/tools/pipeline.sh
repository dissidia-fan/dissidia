#!/usr/bin/env bash
# Dissidia 012 analysis + recompile + build pipeline.
#
# Usage: pipeline.sh <workdir> [--ghidra <dir>] [--ship-analysis] [--analysis-dir <dir>] [--jobs N] [--skip-analyze]
#                             [--analysis-only]
#
#   <workdir>        directory prepared by setup_from_iso.py
#
# Where the analysis (function boundaries etc.) comes from, in this order:
#   --skip-analyze          reuse existing <workdir>/analysis*.json (e.g. produced on another machine)
#   --ghidra <dir>          run Ghidra: <dir> contains support/analyzeHeadless (Homebrew: $(brew --prefix
#                           ghidra)/libexec). $GHIDRA_INSTALL_DIR is used when the flag is absent AND no
#                           shipped analysis is found.
#   --ship-analysis         use analysis*.json.gz (metadata only, no game code) instead of Ghidra; the
#                           segment bytes are rebuilt from YOUR extracted binaries and SHA-256 verified
#                           (tools/ship_analysis.py). Looked up in --analysis-dir if given, else <workdir>,
#                           the directory of this script, then games/dissidia/analysis/. This is also what
#                           happens automatically when no Ghidra was given and the files are found.
#
#   --jobs N         parallel compile jobs (default min(3, nproc); each C++ job can use ~1.5 GB RAM)
#   --analysis-only  stop after the analysis files are in place and checked (no compile)
#   --force          redo the recompile and overlay steps even when their inputs are unchanged
#
# Resumable: the recompile + overlay steps are skipped when <workdir>/.recompile.stamp matches a fingerprint
# of their inputs (analysis*.json, the recompiler binary, game.toml/overlay_*.toml, build_overlay.py) and
# output/ + overlays/ are complete; the cmake build always resumes where it stopped (ccache is used if installed).
# An interrupted run can therefore simply be started again.
#
# Requires Python 3, cmake, a C++17 compiler, and SDL2 / FreeType (+ optionally libavcodec) development
# files. The psprecomp recompiler is built with `cargo build --release` automatically if missing (set
# PSPRECOMP_BIN to use another binary). Ghidra 12.x with the ghidra-allegrex extension and JDK 21 are
# needed only for the Ghidra analysis path.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PSPRECOMP="${PSPRECOMP_BIN:-$REPO/target/release/psprecomp}"
GAME="$REPO/games/dissidia"

die()  { echo "error: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }
step() { echo; echo "=== $* ($(date +%H:%M:%S))"; }

WORK=""; GHIDRA="${GHIDRA_INSTALL_DIR:-}"; GHIDRA_EXPLICIT=0; JOBS=""; SKIP_ANALYZE=0
SHIP=0; SHIP_DIR=""; ANALYSIS_ONLY=0; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --ghidra)       [ $# -ge 2 ] || die "--ghidra needs a directory"; GHIDRA="$2"; GHIDRA_EXPLICIT=1; shift 2 ;;
    --ship-analysis) SHIP=1; shift ;;
    --analysis-dir)  [ $# -ge 2 ] || die "--analysis-dir needs a directory"; SHIP_DIR="$2"; shift 2 ;;
    --analysis-only) ANALYSIS_ONLY=1; shift ;;
    --force)        FORCE=1; shift ;;
    --jobs)         [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    --skip-analyze) SKIP_ANALYZE=1; shift ;;
    -h|--help)      sed -n '2,/^set -euo/{/^set -euo/!p}' "$0"; exit 0 ;;
    -*)             die "unknown option $1" ;;
    *)              [ -z "$WORK" ] || die "unexpected argument $1"; WORK="$1"; shift ;;
  esac
done
[ -n "$WORK" ] || die "usage: pipeline.sh <workdir> [--ghidra <dir>] [--ship-analysis] [--analysis-dir <dir>] [--jobs N] [--skip-analyze]"
[ -d "$WORK" ] || die "workdir $WORK does not exist (run setup_from_iso.py first)"
WORK="$(cd "$WORK" && pwd)"
if [ -z "$JOBS" ]; then
  JOBS="$(nproc 2>/dev/null || echo 2)"; [ "$JOBS" -le 3 ] || JOBS=3
fi
case "$JOBS" in ''|*[!0-9]*) die "--jobs must be a positive integer" ;; esac

# ---------------------------------------------------------------- analysis source
ANALYSES="analysis analysis_battle analysis_menu analysis_extra analysis_supacc"
find_shipped() {   # echo the first directory holding all five <name>.json.gz
  local d n ok
  for d in "$SHIP_DIR" "$WORK" "$HERE" "$GAME/analysis"; do
    [ -n "$d" ] && [ -d "$d" ] || continue
    ok=1; for n in $ANALYSES; do [ -f "$d/$n.json.gz" ] || ok=0; done
    if [ "$ok" = 1 ]; then ( cd "$d" && pwd ); return 0; fi
  done
  return 1
}
MODE=ghidra
if [ "$SKIP_ANALYZE" = 1 ]; then
  MODE=skip
elif [ "$SHIP" = 1 ]; then
  MODE=ship
  SHIPPED_AT="$(find_shipped)" || die "--ship-analysis: no complete set of analysis*.json.gz found (looked in ${SHIP_DIR:+$SHIP_DIR, }$WORK, $HERE, $GAME/analysis)"
elif [ "$GHIDRA_EXPLICIT" = 0 ] && [ -z "$GHIDRA" ] && SHIPPED_AT="$(find_shipped)"; then
  MODE=ship
fi

# ---------------------------------------------------------------- sanity checks
step "Checking prerequisites (analysis: $MODE${SHIPPED_AT:+, from $SHIPPED_AT})"
# Always rebuild the recompiler when it is the in-tree binary: a stale binary silently regenerates
# output/ with old decoder/emitter logic (the fingerprint is computed from the sources on disk, so
# it would not catch that). `cargo build` is a no-op when nothing changed.
if [ "$ANALYSIS_ONLY" = 0 ] && [ -z "${PSPRECOMP_BIN:-}" ]; then
  command -v cargo >/dev/null || { [ -x "$PSPRECOMP" ] || die "$PSPRECOMP not found and cargo is not installed. Install Rust (https://rustup.rs) and re-run, or set PSPRECOMP_BIN"; warn "cargo not found: using the existing $PSPRECOMP (may be stale)"; }
  if command -v cargo >/dev/null; then
    step "Building the psprecomp recompiler (cargo build --release; a no-op when up to date)"
    ( cd "$REPO" && cargo build --release ) || die "cargo build --release failed"
  fi
  [ -x "$PSPRECOMP" ] || die "$PSPRECOMP missing after cargo build"
elif [ ! -x "$PSPRECOMP" ] && [ "$ANALYSIS_ONLY" = 0 ]; then
  die "$PSPRECOMP_BIN is not executable"
fi
for c in python3 cmake; do
  if [ "$ANALYSIS_ONLY" = 1 ] && [ "$c" = cmake ]; then continue; fi
  command -v "$c" >/dev/null || die "$c not found in PATH"
done
if [ "$ANALYSIS_ONLY" = 0 ]; then
command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null || die "no C++ compiler found"
if command -v pkg-config >/dev/null; then
  pkg-config --exists sdl2 || die "SDL2 development files not found (pkg-config sdl2)"
  pkg-config --exists freetype2 || die "FreeType development files not found (pkg-config freetype2)"
  pkg-config --exists libavcodec libavutil || warn "libavcodec/libavutil not found: the build will have no ATRAC3 audio decode"
else
  die "pkg-config not found"
fi
fi

DISC="$WORK/disc0/PSP_GAME"
EBOOT="$WORK/EBOOT.dec.elf"
for f in "$EBOOT" "$DISC/USRDIR/DATA/OVL_BATTLE_APP.ELF" "$DISC/USRDIR/DATA/OVL_MENU_APP.ELF" \
         "$DISC/USRDIR/DATA/OVL_EXTRA_APP.ELF" "$DISC/USRDIR/DATA/MODULE/LIBSUPPREACC.PRX"; do
  [ -f "$f" ] || die "missing $f (run setup_from_iso.py first)"
done

if [ "$MODE" = ghidra ]; then
  [ -n "$GHIDRA" ] || die "no analysis source: pass --ghidra <dir> (or set GHIDRA_INSTALL_DIR), put the shipped analysis*.json.gz where --ship-analysis looks, or use --skip-analyze with existing analysis*.json"
  [ -d "$GHIDRA" ] || die "Ghidra directory $GHIDRA does not exist"
  if [ ! -x "$GHIDRA/support/analyzeHeadless" ] && [ -x "$GHIDRA/libexec/support/analyzeHeadless" ]; then
    GHIDRA="$GHIDRA/libexec"
  fi
  [ -x "$GHIDRA/support/analyzeHeadless" ] || die "$GHIDRA/support/analyzeHeadless not found: --ghidra must be the directory containing support/analyzeHeadless"
  command -v java >/dev/null || die "java not found (Ghidra needs JDK 21)"
  java -version 2>&1 | head -1 | grep -q '"2[1-9]\|"[3-9][0-9]' || warn "java is not 21+ ($(java -version 2>&1 | head -1)); Ghidra 12 requires JDK 21"
  # (psprecomp analyze itself also checks for the ghidra-allegrex extension and warns if it cannot find it)
  NIDDB="$REPO/data/niddb/ppsspp_niddb.xml"
  if [ ! -f "$NIDDB" ]; then
    step "Fetching NID database"
    "$REPO/scripts/fetch-niddb.sh"
  fi
  [ -f "$NIDDB" ] || die "NID database missing: $NIDDB"
fi

run_analyze() {   # run_analyze <binary> <out.json> [extra args]
  local bin="$1" out="$2"; shift 2
  # analyze locates analysis/ExtractAnalysis.java relative to the cwd, so run from the repo root.
  ( cd "$REPO" && "$PSPRECOMP" analyze --ghidra-dir "$GHIDRA" --nid-db "$NIDDB" "$@" -o "$out" "$bin" )
}

# ---------------------------------------------------------------- 1. analyze
if [ "$MODE" = ship ]; then
  step "Rebuilding analysis from $SHIPPED_AT (no Ghidra; segment bytes come from your own files)"
  SA="$HERE/ship_analysis.py"
  hyd() { python3 -I "$SA" hydrate "$SHIPPED_AT/$1.json.gz" "$2" "$WORK/$1.json" || die "shipped analysis $1 does not match your binary (see message above)"; }
  hyd analysis        "$EBOOT"
  hyd analysis_battle "$DISC/USRDIR/DATA/OVL_BATTLE_APP.ELF"
  hyd analysis_menu   "$DISC/USRDIR/DATA/OVL_MENU_APP.ELF"
  hyd analysis_extra  "$DISC/USRDIR/DATA/OVL_EXTRA_APP.ELF"
  hyd analysis_supacc "$DISC/USRDIR/DATA/MODULE/LIBSUPPREACC.PRX"
fi
if [ "$MODE" = ghidra ]; then
  step "Analyzing EBOOT.dec.elf (Ghidra; this takes a while)"
  run_analyze "$EBOOT" "$WORK/analysis.json"
  step "Analyzing OVL_BATTLE_APP.ELF"
  run_analyze "$DISC/USRDIR/DATA/OVL_BATTLE_APP.ELF" "$WORK/analysis_battle.json"
  step "Analyzing OVL_MENU_APP.ELF"
  run_analyze "$DISC/USRDIR/DATA/OVL_MENU_APP.ELF" "$WORK/analysis_menu.json"
  step "Analyzing OVL_EXTRA_APP.ELF"
  run_analyze "$DISC/USRDIR/DATA/OVL_EXTRA_APP.ELF" "$WORK/analysis_extra.json"
  step "Analyzing LIBSUPPREACC.PRX (load base 0x0B800000)"
  run_analyze "$DISC/USRDIR/DATA/MODULE/LIBSUPPREACC.PRX" "$WORK/analysis_supacc.json" --load-base 0x0B800000
fi

step "Checking analysis output"
python3 - "$WORK" <<'PY'
import json, os, sys
w = sys.argv[1]
# (file, minimum plausible function count; reference counts from the EU build: ~8.5k / 6.2k / 6.3k / 97 / 73)
want = [("analysis.json", 5000), ("analysis_battle.json", 3000), ("analysis_menu.json", 3000),
        ("analysis_extra.json", 40), ("analysis_supacc.json", 30)]
bad = False
for name, lo in want:
    p = os.path.join(w, name)
    if not os.path.isfile(p):
        print("error: missing %s" % p); bad = True; continue
    try:
        a = json.load(open(p))
        n = len(a["functions"])
    except Exception as e:
        print("error: %s unreadable: %s" % (name, e)); bad = True; continue
    flag = "" if n >= lo else "  <-- suspiciously low (expected >= %d); is the ghidra-allegrex extension installed?" % lo
    print("  %-22s %6d functions%s" % (name, n, flag))
    if n < lo: bad = True
if not json.load(open(os.path.join(w, "analysis.json"))).get("imports"):
    print("error: analysis.json has no imports"); bad = True
sys.exit(1 if bad else 0)
PY

[ "$ANALYSIS_ONLY" = 0 ] || { step "Done (--analysis-only)"; exit 0; }

# ---------------------------------------------------------------- 2+3. recompile EBOOT, overlays
STAMP="$WORK/.recompile.stamp"
FP="$( { sha256sum "$PSPRECOMP" "$WORK"/analysis*.json "$GAME/game.toml" "$GAME"/overlay_*.toml "$GAME/tools/build_overlay.py" \
           | awk '{print $1}'; } | sha256sum | awk '{print $1}')"
if [ "$FORCE" = 0 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$FP" ] && [ -f "$WORK/output/include/recomp.h" ] \
   && [ -f "$WORK/overlays/battle/dispatch.cpp" ] && [ -f "$WORK/overlays/supacc/dispatch.cpp" ]; then
  step "Recompile + overlays: up to date, skipping (--force to redo)"
else
rm -f "$STAMP"
step "Recompiling EBOOT -> $WORK/output"
rm -rf "$WORK/output"
PSPRECOMP_CROSS_MID=1 "$PSPRECOMP" recompile "$WORK/analysis.json" --config "$GAME/game.toml" -o "$WORK/output"
RH="$WORK/output/include/recomp.h"
[ -f "$RH" ] || die "$RH not generated"
if ! grep -q '^#include <cmath>' "$RH"; then
  { printf '#include <cmath>\n#include <math.h>\n'; cat "$RH"; } > "$RH.tmp" && mv "$RH.tmp" "$RH"
fi

step "Building overlay sources"
rm -rf "$WORK/overlays"
mkdir -p "$WORK/overlays"
BO="$GAME/tools/build_overlay.py"
python3 "$BO" 1 ovl_battle "$WORK/analysis_battle.json" "$WORK/analysis.json" "$WORK/overlays/battle" "$GAME/overlay_battle.toml"
python3 "$BO" 2 ovl_menu   "$WORK/analysis_menu.json"   "$WORK/analysis.json" "$WORK/overlays/menu"   "$GAME/overlay_menu.toml"
python3 "$BO" 3 ovl_extra  "$WORK/analysis_extra.json"  "$WORK/analysis.json" "$WORK/overlays/extra"  "$GAME/overlay_extra.toml"
python3 "$BO" 0 supacc     "$WORK/analysis_supacc.json" "$WORK/analysis.json" "$WORK/overlays/supacc" "$GAME/overlay_supacc.toml" --module
for d in battle menu extra supacc; do
  [ -f "$WORK/overlays/$d/dispatch.cpp" ] || die "overlay $d was not generated"
done
printf '%s\n' "$FP" > "$STAMP"
fi

# ---------------------------------------------------------------- 4. cmake build
step "Configuring runtime build ($WORK/build)"
cmake -B "$WORK/build" -S "$REPO/runtime" -DCMAKE_BUILD_TYPE=Release $(command -v ccache >/dev/null && echo -DCMAKE_CXX_COMPILER_LAUNCHER=ccache) \
  -DPSPRECOMP_GAME=dissidia -DPSPRECOMP_OUTPUT_DIR="$WORK/output" \
  "-DPSPRECOMP_OVERLAY_DIRS=$WORK/overlays/battle;$WORK/overlays/menu;$WORK/overlays/extra;$WORK/overlays/supacc"

step "Building psprecomp_runtime with -j$JOBS (long; ~1.5 GB RAM per job)"
cmake --build "$WORK/build" -j "$JOBS" --target psprecomp_runtime

BIN="$WORK/build/psprecomp_runtime"
[ -x "$BIN" ] || die "build finished but $BIN is missing"
step "Done"
echo "Runtime: $BIN"
echo "Run it:  $GAME/tools/run.sh $WORK"
