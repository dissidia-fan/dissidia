#!/usr/bin/env python3
"""steam_shortcut.py - put the native build in the Steam library as "Dissidia Trēdecim", with its artwork.

    steam_shortcut.py <launcher> [--name NAME] [--art DIR] [--match-dir DIR ...] [--launch-options OPTS]
                      [--steam-dir DIR] [--yes] [--dry-run]

Runs on the host (SteamOS' own python3, standard library only), in Desktop Mode. For every Steam account on this
machine (<steam>/userdata/<id>/config):

  * shortcuts.vdf: adds a non-Steam shortcut to <launcher> named NAME, or updates the one that is already ours. Ours
    = its target is <launcher>, or lies under a --match-dir, or its name is one this project used before
    ("play.sh", "run.sh", "run.py", "Dissidia 012 (native)", NAME). The first match keeps its app id, play time,
    collections and Launch Options (unless --launch-options is given); further matches are duplicates and removed.
    The file is backed up once, to shortcuts.vdf.before-tredecim.
  * grid/: the artwork from --art (default: ../steam next to this script) under the shortcut's app id:
        capsule.png -> <id>p.png (portrait)   wide.png -> <id>.png      hero.png -> <id>_hero.png
        logo.png    -> <id>_logo.png + <id>.json (logo pinned bottom left)    icon.png -> <id>_icon.png (+ shortcut icon)

Steam keeps its shortcut list in memory and writes it back when it exits, so it must not be running while the file
is changed: if it is, this asks to close it (steam -shutdown), waits, writes, and starts it again. Never from Game
Mode (Steam *is* the session there), so run it in Desktop Mode.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ART = os.path.normpath(os.path.join(HERE, "..", "steam"))
NAME = "Dissidia Trēdecim"
OLD_NAMES = {"play.sh", "run.sh", "run.py", "dissidia 012 (native)", "dissidia 012", "dissidia"}
ART = (("capsule.png", "{id}p.png"), ("wide.png", "{id}.png"), ("hero.png", "{id}_hero.png"),
       ("logo.png", "{id}_logo.png"), ("icon.png", "{id}_icon.png"))
LOGO_POS = {"nVersion": 1, "logoPosition": {"pinnedPosition": "BottomLeft", "nWidthPct": 42, "nHeightPct": 60}}

# ------------------------------------------------------------------ binary KeyValues (shortcuts.vdf)
# A node is a list of [type, key, value]; type 0 = nested node, 1 = string, 2 = int32, others kept as raw bytes.
SIZES = {3: 4, 4: 4, 6: 4, 7: 8, 10: 8}


def _cstr(b, i):
    j = b.index(b"\0", i)
    return b[i:j].decode("utf-8", "surrogateescape"), j + 1


def kv_parse(b, i=0):
    node = []
    while True:
        t = b[i]
        i += 1
        if t == 8:
            return node, i
        key, i = _cstr(b, i)
        if t == 0:
            val, i = kv_parse(b, i)
        elif t == 1:
            val, i = _cstr(b, i)
        elif t == 2:
            val = struct.unpack_from("<i", b, i)[0]
            i += 4
        elif t in SIZES:
            val = b[i:i + SIZES[t]]
            i += SIZES[t]
        else:
            raise ValueError("shortcuts.vdf: unknown field type %d at byte %d" % (t, i - 1))
        node.append([t, key, val])


def kv_dump(node):
    out = bytearray()
    for t, key, val in node:
        out.append(t)
        out += key.encode("utf-8", "surrogateescape") + b"\0"
        if t == 0:
            out += kv_dump(val)
        elif t == 1:
            out += val.encode("utf-8", "surrogateescape") + b"\0"
        elif t == 2:
            out += struct.pack("<i", val)
        else:
            out += val
    out.append(8)
    return bytes(out)


def kv_get(node, key, default=None):
    for t, k, v in node:
        if k.lower() == key.lower():
            return v
    return default


def kv_set(node, key, t, val):
    for e in node:
        if e[1].lower() == key.lower():
            e[0], e[2] = t, val
            return
    node.append([t, key, val])


def load_shortcuts(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return [[0, "shortcuts", []]]
    root, _ = kv_parse(open(path, "rb").read())
    if not root or root[0][1].lower() != "shortcuts" or root[0][0] != 0:
        raise ValueError("%s: no 'shortcuts' list at the top" % path)
    return root

# ------------------------------------------------------------------ our entry


def app_id(exe, name):
    """Steam's id for a non-Steam shortcut: crc32(Exe + AppName) with the top bit set (the grid files use it
    unsigned; shortcuts.vdf stores it as a signed int32)."""
    return (zlib.crc32((exe + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


def signed(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def unq(s):
    return s.strip().strip('"')


def is_ours(entry, launcher, match_dirs, name):
    exe = os.path.realpath(os.path.expanduser(unq(kv_get(entry, "Exe", "") or "")))
    if exe == launcher:
        return True
    if any(exe == d or exe.startswith(d + os.sep) for d in match_dirs):
        return True
    an = (kv_get(entry, "AppName", "") or kv_get(entry, "appname", "") or "").strip().lower()
    return an in OLD_NAMES or an == name.lower()


def new_entry():
    return [[2, "appid", 0], [1, "AppName", ""], [1, "Exe", ""], [1, "StartDir", ""], [1, "icon", ""],
            [1, "ShortcutPath", ""], [1, "LaunchOptions", ""], [2, "IsHidden", 0], [2, "AllowDesktopConfig", 1],
            [2, "AllowOverlay", 1], [2, "OpenVR", 0], [2, "Devkit", 0], [1, "DevkitGameID", ""],
            [2, "DevkitOverrideAppID", 0], [2, "LastPlayTime", 0], [1, "FlatpakAppID", ""], [0, "tags", []]]


def update_user(cfg, a, launcher, match_dirs, log):
    """Add/update the shortcut in <cfg>/shortcuts.vdf and copy the artwork into <cfg>/grid. Returns the app id."""
    path = os.path.join(cfg, "shortcuts.vdf")
    root = load_shortcuts(path)
    lst = root[0][2]
    exe = '"%s"' % launcher
    ours = [e for e in lst if e[0] == 0 and is_ours(e[2], launcher, match_dirs, a.name)]
    if ours:
        keep = ours[0]
        ent = keep[2]
        aid = kv_get(ent, "appid")
        if aid is None or aid == 0:
            aid = signed(app_id(exe, a.name))
        log("  updating shortcut '%s' -> '%s' (app id %d)" % (kv_get(ent, "AppName", "?"), a.name, aid & 0xFFFFFFFF))
        for dup in ours[1:]:
            log("  removing duplicate shortcut '%s' (%s)" % (kv_get(dup[2], "AppName", "?"), kv_get(dup[2], "Exe", "?")))
            lst.remove(dup)
    else:
        ent = new_entry()
        aid = signed(app_id(exe, a.name))
        keep = [0, "", ent]
        lst.append(keep)
        log("  adding shortcut '%s' (app id %d)" % (a.name, aid & 0xFFFFFFFF))
    for i, e in enumerate(lst):                                   # entries are keyed "0", "1", ... in order
        e[1] = str(i)
    uid = aid & 0xFFFFFFFF
    grid = os.path.join(cfg, "grid")
    icon = os.path.join(grid, "%d_icon.png" % uid)
    kv_set(ent, "appid", 2, aid)
    kv_set(ent, "AppName", 1, a.name)
    kv_set(ent, "Exe", 1, exe)
    kv_set(ent, "StartDir", 1, '"%s"' % os.path.dirname(launcher))
    kv_set(ent, "icon", 1, icon)
    if a.launch_options is not None:
        kv_set(ent, "LaunchOptions", 1, a.launch_options)
    elif kv_get(ent, "LaunchOptions") is None:
        kv_set(ent, "LaunchOptions", 1, "")
    data = kv_dump(root)
    kv_parse(data)                                                # round-trip check before touching the file
    if a.dry_run:
        log("  (dry run) would write %s and %d pictures to %s" % (path, len(ART), grid))
        return uid
    os.makedirs(grid, exist_ok=True)
    for src, dst in ART:
        s = os.path.join(a.art, src)
        if os.path.exists(s):
            shutil.copyfile(s, os.path.join(grid, dst.format(id=uid)))
        else:
            log("  warning: %s missing, skipped" % s)
    with open(os.path.join(grid, "%d.json" % uid), "w") as f:
        json.dump(LOGO_POS, f)
    bak = path + ".before-tredecim"
    if os.path.exists(path) and not os.path.exists(bak):
        shutil.copyfile(path, bak)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    log("  wrote %s, artwork in %s" % (path, grid))
    return uid

# ------------------------------------------------------------------ Steam process


def steam_running():
    return subprocess.call(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0


def game_mode():
    return (os.environ.get("XDG_CURRENT_DESKTOP", "").lower() == "gamescope" or os.environ.get("SteamGamepadUI") == "1"
            or os.environ.get("SteamDeck") == "1" and bool(os.environ.get("GAMESCOPE_WAYLAND_DISPLAY")))


def stop_steam(yes):
    if game_mode():
        sys.exit("error: this looks like Game Mode; switch to Desktop Mode and run it there (Steam has to close for a moment)")
    if not yes:
        if not sys.stdin.isatty():
            sys.exit("error: Steam is running; it has to close while its shortcut list is changed. Re-run with --yes, or quit Steam first")
        r = input("Steam is running and has to close for a moment while its shortcut list is changed. Close it now? [Y/n] ")
        if r.strip().lower() not in ("", "y", "yes"):
            sys.exit("left unchanged")
    print("closing Steam (steam -shutdown) ...")
    subprocess.call(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        if not steam_running():
            time.sleep(2)                                       # let it finish writing its own files
            return
        time.sleep(0.5)
    sys.exit("error: Steam did not close within a minute; quit it by hand and run this again")


def start_steam():
    print("starting Steam again")
    subprocess.Popen(["steam"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)

# ------------------------------------------------------------------ main


def steam_dirs(given):
    cands = [given] if given else [os.path.expanduser("~/.local/share/Steam"), os.path.expanduser("~/.steam/steam"),
                                   os.path.expanduser("~/.var/app/com.valvesoftware.Steam/data/Steam")]
    seen, out = set(), []
    for c in cands:
        r = os.path.realpath(c)
        if r not in seen and os.path.isdir(os.path.join(r, "userdata")):
            seen.add(r)
            out.append(r)
    return out


def user_configs(steam):
    ud = os.path.join(steam, "userdata")
    return [os.path.join(ud, u, "config") for u in sorted(os.listdir(ud))
            if u.isdigit() and u != "0" and os.path.isdir(os.path.join(ud, u))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("launcher")
    ap.add_argument("--name", default=NAME)
    ap.add_argument("--art", default=DEFAULT_ART)
    ap.add_argument("--match-dir", action="append", default=[])
    ap.add_argument("--launch-options", default=None)
    ap.add_argument("--steam-dir", default=None)
    ap.add_argument("--yes", action="store_true", help="close Steam without asking if it is running")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    launcher = os.path.realpath(os.path.expanduser(a.launcher))
    if not os.path.isfile(launcher):
        sys.exit("error: launcher not found: %s" % launcher)
    match_dirs = [os.path.realpath(os.path.expanduser(d)) for d in a.match_dir]
    steams = steam_dirs(a.steam_dir)
    if not steams:
        sys.exit("error: no Steam installation with a userdata folder found (log in to Steam once first)")
    cfgs = [c for s in steams for c in user_configs(s)]
    if not cfgs:
        sys.exit("error: no Steam account folders in %s (log in to Steam once first)" % ", ".join(steams))
    was_running = steam_running() and not a.dry_run
    if was_running:
        stop_steam(a.yes)
    try:
        for c in cfgs:
            print("Steam account %s:" % os.path.basename(os.path.dirname(c)))
            update_user(c, a, launcher, match_dirs, print)
    finally:
        if was_running:
            start_steam()
    print("done: '%s' is in the library (non-Steam games)." % a.name)


if __name__ == "__main__":
    main()
