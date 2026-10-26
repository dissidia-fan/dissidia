#!/usr/bin/env python3
"""Character mods: one folder per extra character, built into the game's mods dir with one command.

    charmod.py list                                   # the character mods
    charmod.py new NAME --like fiv100 [--display "Name" --series "FINAL FANTASY VI" --code abc100]
                                                      # start a new character from mods/_template
    charmod.py build [NAME ...] [--check] [--skin S]  # build mods/<NAME> (default: all) into work/mods; a folder that
                                                      # is not in the game yet is added (slot, menu ids, name text)
    charmod.py avatars NAME                           # list the avatars, draw avatars/_preview.png
    charmod.py avatars NAME --from-picture ART.png [--crop grid_tile=x0,y0,x1,y1 ...] [--names] [--logo L.png]
                                                      # make the avatar pictures from one big picture
    charmod.py avatars NAME --redo                    # make them again from avatars.generated_from (after editing crops)
    charmod.py body NAME manikin | SKIN | --from-gmo FILE --as SKIN
                                                      # choose the body; or make a skin from any model (Blender export)
    charmod.py hits NAME [MOVE]                       # every hit of every move (power, re-hits, reaction)
    charmod.py unpack NAME --code noc100 --like fiv100  # make mods/<NAME> from what is installed in work/mods
    charmod.py pull [NAME ...]                        # refresh mods/<NAME> from the install (after other tools wrote it)

Folder (mods/<name>/ at the repo root): mod.json, avatars/ (PNG pictures), moveset/ (moves as text), models/ (rig),
skins/ (bodies), files/ (everything else). See mods/README.md.
A folder whose mod.json has "kind": "retail" changes a retail character instead of adding one (the Move Studio makes
them, e.g. mods/cloud): its files are built over the retail paths (obj/p_sev100.objx ...), a file that is the same as
the retail one is left out, and no slot, roster line, avatars or move names are involved. Disable or delete the folder
and the next build of all mods takes its files out of the mods dir again, so the retail character comes back.
Options: --mods DIR (default work/mods), --disc DIR (default work/disc0).
"""
import argparse
import difflib
import filecmp
import hashlib
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
if __name__ == '__main__':
    import pydeps                    # numpy / Pillow / scipy at the pinned versions (tools/pydeps.py)
    pydeps.require()
import charmod_objx as CO        # noqa: E402
import package_extract as PE     # noqa: E402

GAME = os.path.normpath(os.path.join(HERE, '..'))
REPO = os.path.normpath(os.path.join(GAME, '..', '..'))
ROOT = os.path.normpath(os.path.join(REPO, '..'))                       # the repo root: mods/, studio/, blender/
MODS_SRC = os.environ.get('CHARMOD_SRC') or os.path.join(ROOT, 'mods')   # CHARMOD_SRC: another folder of mods (tests)
ABILITIES = os.path.join(GAME, 'extra_abilities.json')   # not in the public tree: the first build writes it
DEF_MODS = os.path.join(REPO, 'work', 'mods')
DEF_DISC = os.path.join(REPO, 'work', 'disc0')
FX_MANIFEST = 'fx_shaders/manifest.json'
STATE = 'readme_charmod_state.json'   # in the mods dir; 'readme*' files are not served to the game
RETAIL_STATE = 'readme_charmod_retail.json'   # {game path: mod folder} of the retail files "kind": "retail" mods replace
MENU_KEYS = ('tile', 'art', 'artl', 'artr', 'name', 'logo', 'text')

READ_ME = [
    'One extra character. Edit this file and the folders next to it, then build:',
    '    python3 games/dissidia/tools/charmod.py build <this folder\'s name>',
    'character: who it is. name = what menus call it; slot = its roster slot; based_on = the retail character it is built on.',
    'avatars: the menu / HUD pictures (PNG files in avatars/). Swap a picture, or make all of them from one big picture:',
    '    charmod.py avatars <folder> --from-picture my_art.png      (charmod.py avatars <folder> draws a preview sheet)',
    'body: the model\'s body. "manikin" = the based_on character\'s manikin, or a skin from skins/.',
    'move_names: the in-game name and help text of each move (cmd = the move in moveset/p_<code>/moves.json).',
    'Moves, timing and damage: moveset/p_<code>/ (damage: hits.json "power"; list them: charmod.py hits <folder>).',
    'runtime_hooks: settings for the game runtime (roster.cpp: warp=, landact= ...). menu_ids: managed by the tools.',
    'files: every other game file the character has (folder path, or {"like": ...} = the based_on character\'s file).',
    'Full guide: mods/README.md',
]


def rj(p):
    with open(p) as f:
        return json.load(f)


def rj_abilities():
    return rj(ABILITIES) if os.path.exists(ABILITIES) else {}


def wj(p, o):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w') as f:
        json.dump(o, f, indent=1, ensure_ascii=False)
        f.write('\n')


def read(p):
    with open(p, 'rb') as f:
        return f.read()


def same(p, d):
    if not os.path.exists(p):
        return False
    cur = read(p)
    if cur == d:
        return True
    if p.endswith('.json'):
        try:
            return json.loads(cur) == json.loads(d)
        except ValueError:
            return False
    if p.endswith('.txt'):
        return cur.decode('latin1').splitlines() == d.decode('latin1').splitlines()
    return False


def write(p, d):
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    if same(p, d):
        return False
    with open(p, 'wb') as f:
        f.write(d)
    return True


class Disc:
    def __init__(self, path):
        self.path = path
        self._p = None

    @property
    def pkg(self):
        if self._p is None:
            self._p = PE.Package(self.path)
        return self._p

    def read(self, game_path):
        return self.pkg.read(game_path)

    def try_read(self, game_path):
        try:
            return self.read(game_path)
        except KeyError:
            return None


def ch(m):
    return m['character']


def is_retail(m):
    """A mod that changes a retail character (mods/cloud: obj/p_sev100.objx ...) instead of adding one."""
    return m.get('kind') == 'retail'


# ---------------------------------------------------------------- roster.txt
def roster_lines(mods):
    p = os.path.join(mods, 'roster.txt')
    return open(p).read().splitlines() if os.path.exists(p) else []


def roster_tokens(m):
    c = ch(m)
    menu = ['%s=%s' % (k, v) for k, v in m.get('menu_ids', {}).items() if not k.startswith('_')]
    return menu + ['nm=' + c['name'].replace(' ', '_')] + list(m.get('runtime_hooks', []))


def roster_line(m):
    c = ch(m)
    return ' '.join([str(c['slot']), c['code'], str(c['based_on_id'])] + roster_tokens(m))


def merged_roster(mods, mod_list):
    lines = roster_lines(mods)
    for m in mod_list:
        new = roster_line(m)
        for i, l in enumerate(lines):
            f = l.split()
            if f and f[0] == str(ch(m)['slot']):
                lines[i] = new
                break
        else:
            lines.append(new)
    return '\n'.join(lines) + '\n'


def split_roster(tokens):
    """roster tokens -> (menu_ids, runtime_hooks); None if they don't rebuild the same line."""
    menu, hooks, name = {}, [], None
    for t in tokens:
        k, _, v = t.partition('=')
        if k in MENU_KEYS and not hooks and name is None:
            menu[k] = int(v)
        elif k == 'nm' and name is None and not hooks:
            name = v
        else:
            hooks.append(t)
    return menu, hooks, name


# ---------------------------------------------------------------- build
def mod_dirs(names):
    if not os.path.isdir(MODS_SRC):
        return []
    def enabled(d):
        if d.startswith(('_', '.')):                      # _template etc.: never built
            return False
        try:
            return rj(os.path.join(MODS_SRC, d, 'mod.json')).get('enabled', True) is not False
        except ValueError:
            print('warning: mods/%s/mod.json is not valid JSON; skipped' % d)
            return False
    all_ = sorted(d for d in os.listdir(MODS_SRC) if os.path.exists(os.path.join(MODS_SRC, d, 'mod.json')) and enabled(d))
    if names:
        for n in names:
            if n not in all_:
                sys.exit('no mod folder mods/%s to build (have: %s; folders starting with _ and "enabled": false are '
                         'skipped)' % (n, ', '.join(all_) or 'none'))
        return names
    return all_


def load_mod(name):
    m = rj(os.path.join(MODS_SRC, name, 'mod.json'))
    if 'character' not in m:
        sys.exit('mods/%s/mod.json is in the old layout: run  charmod.py pull %s  to update it' % (name, name))
    return m


# A mod folder is data. The build reads these kinds of file from it and never runs anything from it; a folder with a
# script or program in it is refused, so a shared mod can't carry code (docs: mods/README.md "Sharing a mod").
MOD_DATA = {'.json', '.bin', '.gmo', '.skin', '.png', '.jpg', '.jpeg', '.webp', '.gim', '.wav', '.scd', '.md', '.txt',
            '.frag', '.vert', '.glsl'}          # .frag / .vert / .glsl: fx_shaders overrides, GPU shaders the runtime compiles
MOD_CODE = {'.py', '.pyc', '.pyw', '.sh', '.bash', '.zsh', '.command', '.bat', '.cmd', '.ps1', '.exe', '.dll', '.so',
            '.dylib', '.app', '.js', '.mjs', '.jar', '.blend', '.blend1', '.lua', '.rb', '.pl', '.vbs', '.scr', '.msi',
            '.pkg', '.deb', '.rpm', '.apk', '.prx', '.elf'}
MOD_IGNORE = {'.ds_store', 'thumbs.db', 'desktop.ini'}


def check_mod_files(name):
    """[unexpected files] of a mod folder; exits on a script / program (a .blend counts: it can carry Python)."""
    root = os.path.join(MODS_SRC, name)
    code, odd = [], []
    for dp, dns, fns in os.walk(root):
        code += [os.path.relpath(os.path.join(dp, d), root) for d in dns if d.lower().endswith('.app')]   # a macOS program
        for f in fns:
            low = f.lower()
            if low in MOD_IGNORE:
                continue
            ext = os.path.splitext(low)[1]
            rel = os.path.relpath(os.path.join(dp, f), root)
            if ext in MOD_CODE or os.access(os.path.join(dp, f), os.X_OK) and ext not in MOD_DATA:
                code.append(rel)
            elif ext not in MOD_DATA:
                odd.append(rel)
    try:                                              # what mod.json installs must be data too
        files = (rj(os.path.join(root, 'mod.json')).get('files') or {})
    except (OSError, ValueError):
        files = {}
    for gp, src in files.items():
        if isinstance(src, str) and not gp.startswith('_') and os.path.isfile(os.path.join(root, src)) and \
                os.path.splitext(src.lower())[1] not in MOD_DATA:
            code.append(src)
    if code:
        sys.exit('mods/%s has scripts or programs in it: %s\nA mod folder holds data only (JSON, binary sections, models, '
                 'skins, pictures, sounds); the build never runs anything from it. Take them out (or ask whoever shared '
                 'the mod why they are there), then build again.' % (name, ', '.join(code[:8]) + (' ...' if len(code) > 8 else '')))
    return odd


def refresh_avatars(name, disc_path):
    """The first time and whenever avatars.generated_from changes (picture, crops, options; the Move Studio's
    Character tab edits them), make the cut avatars again, as `charmod.py avatars NAME --redo` does. A folder whose
    pictures were made another way keeps them until generated_from changes."""
    m = load_mod(name)
    av = m.get('avatars') or {}
    gf = av.get('generated_from') or {}
    pics = [gf.get(k) for k in ('picture', 'portrait') if gf.get(k)]
    if not pics or not all(os.path.isfile(os.path.join(MODS_SRC, name, p)) for p in pics):
        return False
    h = hashlib.md5(json.dumps({k: gf.get(k) for k in ('picture', 'portrait', 'logo', 'crops', 'options')}, sort_keys=True).encode())
    for p in pics:
        h.update(read(os.path.join(MODS_SRC, name, p)))
    sig = h.hexdigest()
    if gf.get('_made') == sig:
        return False
    import charmod_avatars as AV
    cut = [k for k in AV.CROPS if isinstance(av.get(k), dict)]
    if '_made' not in gf and any(av[k].get('picture') for k in cut):
        print('  %s: avatars kept as they are (generated_from is recorded; a change to it makes them again)' % name)
    else:
        print('  %s: avatars cut again from %s (generated_from changed)' % (name, ', '.join(pics)))
        root = os.path.join(MODS_SRC, name)
        own = {k: (e['picture'], read(os.path.join(root, e['picture']))) for k, e in av.items()    # set by hand ("_own"):
               if isinstance(e, dict) and e.get('_own') and e.get('picture') and os.path.isfile(os.path.join(root, e['picture']))}
        cmd_avatars(argparse.Namespace(name=name, disc=disc_path, redo=True, from_picture=None, portrait=None, crop=None,
                                       hud_flip=None, tile_flip=None, tile_cutout=None, hud_from=None, names=False, logo=None))
        m = load_mod(name)
        for k, (rel, data) in own.items():             # ... they stay as they were
            write(os.path.join(root, rel), data)
            m['avatars'][k]['picture'] = rel
    m['avatars'].setdefault('generated_from', {})['_made'] = sig
    wj(os.path.join(MODS_SRC, name, 'mod.json'), m)
    return True


def build_files(name, disc, skin_override=None):
    """-> (mod, {game path: bytes}, [(menu id key, id, gim bytes)])"""
    import charmod_avatars as AV
    root = os.path.join(MODS_SRC, name)
    m = load_mod(name)
    c = ch(m)
    out, cache = {}, {}
    if is_retail(m):
        for gp, src in m['files'].items():
            if gp.startswith('_') or isinstance(src, dict):
                continue
            p = os.path.join(root, src)
            data = CO.pack(p) if os.path.isdir(p) else read(p)
            if data != disc.try_read(gp):                   # only what differs from the retail file replaces it
                out[gp] = data
        return m, out, []
    body = m.get('body', {})
    skin = skin_override or body.get('skin', 'manikin')
    skins = {}
    for gp, src in m['files'].items():
        if gp.startswith('_'):
            continue
        if isinstance(src, dict) and 'like' in src:
            data = disc.read(src['like'])
        else:
            p = os.path.join(root, src)
            if src not in cache:
                cache[src] = CO.pack(p) if os.path.isdir(p) else read(p)
            data = cache[src]
        if gp in body.get('models', []):
            data = apply_body(m, root, gp, data, skin, disc, skins)
        out[gp] = data
    if m['files'].get('_auto'):                         # everything else: the based-on character's files
        import new_character as nc
        av_targets = {p for e in m.get('avatars', {}).values() if isinstance(e, dict) for p in e.get('_files', [])}
        for src in nc.alias_sources(disc.pkg, c['based_on']):
            dst = src.replace(c['based_on'], c['code'])
            if dst not in out and dst not in av_targets:
                out[dst] = disc.read(src)
    tpl = AV.Templates(disc, c['based_on'], c['based_on_id'])
    av_files, pack = AV.build(root, m, tpl, disc)
    out.update(av_files)
    entries = [(k, m['menu_ids'][k], d) for k, d in pack.items()]
    return m, out, entries


def apply_body(m, root, gp, rig, skin, disc, cache):
    import charmod_gmo as G
    if skin in (None, 'as_is'):
        return rig
    key = (skin, hashlib.md5(rig).hexdigest())
    if key not in cache:
        if skin == 'manikin':
            src = m['body'].get('manikin_from', 'obj/p_%s_zako.gmo' % ch(m)['based_on'])
            sk = G.skin_from_body(rig, disc.read(src), 'manikin: ' + src)
        else:
            stem = os.path.splitext(os.path.basename(gp))[0]
            d = os.path.join(root, 'skins', skin)
            cands = [os.path.join(d, stem + '.skin'), os.path.join(d, 'body.skin')]
            p = next((x for x in cands if os.path.exists(x)), None)
            if p is None:
                sys.exit('skin %s has no %s.skin or body.skin' % (skin, stem))
            sk = G.skin_from_bytes(read(p))
        cache[key] = G.apply_skin(rig, sk)
    return cache[key]


def merge_fx(mods, mod_list):
    p = os.path.join(mods, FX_MANIFEST)
    if not any(m.get('fx_shaders') for m in mod_list):
        return None
    man = rj(p) if os.path.exists(p) else {'version': 1, 'textures': {}, 'draws': []}
    for m in mod_list:
        fx = m.get('fx_shaders')
        if not fx:
            continue
        for k, v in fx.get('notes', {}).items():
            man[k] = v
        man.setdefault('textures', {}).update(fx.get('textures', {}))
        names = {d['name'] for d in fx.get('draws', [])}
        man['draws'] = [d for d in man.get('draws', []) if d.get('name') not in names] + fx.get('draws', [])
    return (json.dumps(man, indent=2, ensure_ascii=False) + '\n').encode()


def registered(m, mods):
    """The mods dir knows this character: its slot is in roster.txt and readme_newchars.json with this code."""
    c = ch(m)
    mf = os.path.join(mods, 'readme_newchars.json')
    man = rj(mf) if os.path.exists(mf) else {'slots': {}}
    s = man['slots'].get(str(c['slot']))
    line = next((l.split() for l in roster_lines(mods) if l.split() and l.split()[0] == str(c['slot'])), None)
    return bool(s and s.get('code') == c['code'] and line and line[1] == c['code'] and m.get('menu_ids'))


# The Quick Battle / setup screens hold a fixed number of menu pictures: the shared pack (menu_pk_free_battle_gim_3,
# 130 retail entries, entity ids 0x707F..) may grow to 161 entries (ids up to 0x711F). Past that the setup screen
# loses textures of its own (24 Oct: a 7th extra character took the Enter Battle button away; 6 x 5 = 30 of the 31
# spare entries were fine). Each extra character takes 5: grid tile, slide-in, its copy for the setup panel, name
# plate, logo plate.
MENU_PACK_MAX_ENTRIES = 161
MENU_ENTRIES_PER_CHARACTER = 5


def logo_changes(m, mods):
    """True when a character added earlier would now use the game's own logo (its series has one, and the folder has
    no logo picture of its own): registering it again frees its logo entry."""
    import new_character as nc
    av = m.get('avatars', {})
    if (av.get('generated_from') or {}).get('logo') or (av.get('logo') or {}).get('_own'):
        return False
    f2 = nc.retail_logo(ch(m).get('series'))
    if f2 is None:
        return False
    line = next((l.split() for l in roster_lines(mods) if l.split() and l.split()[0] == str(ch(m)['slot'])), [])
    return 'logo=%d' % f2 not in line                 # what this mods dir has, not what mod.json last recorded


def menu_pack_room(mods, disc):
    """(free, total) picture entries left for extra characters in the shared menu pack."""
    import new_character as nc
    _, orig, blobs = nc.shared_g3(mods, disc.pkg)
    used = sum(1 for k in range(orig, len(blobs)) if blobs[k])
    total = MENU_PACK_MAX_ENTRIES - orig
    return total - used, total


def register(name, m, mods, disc, taken):
    """Give a new character folder its place in the game (what new_character.py does): a roster slot, menu-pack ids for
    its avatars, its name in the menu text, its main.bin records. Writes the slot / ids back into mod.json."""
    import new_character as nc
    c = ch(m)
    root = os.path.join(MODS_SRC, name)
    av = m.get('avatars', {})
    gf = av.get('generated_from') or {}
    logo_own = bool(gf.get('logo')) or bool((av.get('logo') or {}).get('_own'))
    man0 = nc.load_manifest(mods)
    if not any(v.get('code') == c['code'] for v in man0['slots'].values()):    # a new slot (a re-run reuses its entries)
        need = MENU_ENTRIES_PER_CHARACTER
        if not logo_own and (nc.retail_logo(c.get('series')) is not None or nc.shared_logo(man0, c['slot'], c.get('series'))):
            need -= 1                                    # the game's own logo, or one shared with a character of its series
        free, total = menu_pack_room(mods, disc)
        if free < need:
            print('  %s: NOT added: the character select screens have no room for another character\'s pictures (%d of %d '
                  'used; this one needs %d). More would break other pictures on those screens, so the game keeps the '
                  'characters it has. Delete or disable another character (its mod.json "enabled": false) to make room.'
                  % (name, total - free, total, need))
            return None
    rmax = nc.max_slots(GAME)
    mf = os.path.join(mods, 'readme_newchars.json')
    man = nc.load_manifest(mods)
    used = {int(l.split()[0]): l.split()[1] for l in roster_lines(mods) if l.split() and l.split()[0].isdigit()}
    used.update({int(k): v['code'] for k, v in man['slots'].items()})
    for k, v in list(used.items()):
        if v == c['code']:
            del used[k]                                   # its own earlier slot (re-registering)
    slot = c['slot']
    if slot in used or slot in taken or not nc.check_slot(slot, rmax)[0]:
        free = [s for s in range(35, rmax) if s not in used and s not in taken and nc.check_slot(s, rmax)[0]]
        if not free:
            sys.exit('%s: no free roster slot (35..%d are all used)' % (name, rmax - 1))
        print('  %s: slot %d is taken (%s), using slot %d' % (name, slot, used.get(slot) or 'another mod', free[0]))
        slot = free[0]
    pic = lambda rel: os.path.join(root, rel) if rel and os.path.exists(os.path.join(root, rel)) else None
    slidein = pic(gf.get('picture')) or pic((av.get('slidein') or {}).get('picture'))
    args = {'mods': mods, 'slot': slot, 'code': c['code'], 'like': c['based_on'], 'name': c['name'],
            'full_name': c.get('full_name'), 'series': c.get('series'), 'slidein': slidein,
            'portrait': pic(gf.get('portrait')), 'logo': pic(gf.get('logo')), 'logo_own': logo_own}
    inv = {'grid_tile': 'tile_crop', 'slidein': 'art_crop', 'face_strip': 'face_crop', 'hud': 'hud_crop', 'vs_face': 'vs_crop'}
    if slidein and slidein == pic(gf.get('picture')):
        args.update({inv[k]: v for k, v in gf.get('crops', {}).items() if k in inv})
        args.update(gf.get('options', {}))

    class ModsOut(nc.Out):                               # the character's own files come from its folder, not aliases
        def write(self, rel, data):
            if rel.startswith(('obj/', 'voice/')):
                return
            nc.Out.write(self, rel, data)
    out = ModsOut(mods, False)
    r = nc.apply_slot(args, disc.pkg, out, man, lambda s_: print('    ' + s_))
    nc.save_manifest(out, man)
    st_p = os.path.join(mods, STATE)                     # what it wrote is ours: the build may overwrite it
    state = rj(st_p) if os.path.exists(st_p) else {}
    for rel in out.written + out.unchanged:
        p = os.path.join(mods, rel)
        if os.path.exists(p):
            state[rel] = hashlib.md5(read(p)).hexdigest()
    wj(st_p, state)
    menu, _, _ = split_roster(r['line'].split()[3:])
    m['character']['slot'] = slot
    m['character']['based_on_id'] = r['like_id']
    m['menu_ids'] = dict({'_info': (m.get('menu_ids') or {}).get('_info', 'entry ids in the shared menu pack; set by the '
                                    'build when the character is added; leave as they are')}, **menu)
    wj(os.path.join(root, 'mod.json'), m)
    print('  %s: added to the game as slot %d (%s)' % (name, slot, ' '.join('%s=%s' % kv for kv in menu.items())))
    return slot


def added_slots(mods):
    """{slot: code} of the extra characters this mods dir has in the game (readme_newchars.json)."""
    import new_character as nc
    try:
        return {int(s): v.get('code') for s, v in nc.load_manifest(mods)['slots'].items()}
    except (OSError, ValueError, KeyError):
        return {}


def drop_gone_characters(mods, names, disc):
    """Take out of the game the extra characters whose folder was deleted (or set "enabled": false): their roster
    slot, menu-pack entries, name text and main.bin records, as new_character.py --remove does."""
    import new_character as nc
    have = set()
    for n in names:
        m = load_mod(n)
        if not is_retail(m):
            have.add(ch(m)['code'])
    for slot, code in sorted(added_slots(mods).items()):
        if code in have:
            continue
        print('  %s (slot %d): no character folder for it any more, taking it out of the game' % (code, slot))
        out = nc.Out(mods, False)
        nc.remove_slot(slot, disc.pkg, out, nc.load_manifest(mods), mods, lambda s_: print('    ' + s_))


def cmd_build(a):
    """Build every folder in. A character that gets its roster slot in this run but whose files are then not installed
    (the stale-file stop, an error) is taken out again: with a roster line and menu pictures but no obj/ files the
    game hangs on the loading screen when it is picked (Ramza, 25 Oct 2026)."""
    added = []                                          # (folder, slot) registered by this run
    try:
        r = _build(a, added)
    except BaseException:
        unregister(a, added, 'the build stopped')
        raise
    if r == 2:
        unregister(a, added, 'the build did not install any file')
    return r


def unregister(a, added, why):
    import new_character as nc
    if not added:
        return
    disc = Disc(a.disc)
    st_p = os.path.join(a.mods, STATE)
    state = rj(st_p) if os.path.exists(st_p) else {}
    for n, slot in added:
        print('  %s: %s, so slot %d is given up again (a character in the roster without its files hangs the loading '
              'screen); the next build adds it' % (n, why, slot))
        man = nc.load_manifest(a.mods)
        for rel in (man['slots'].get(str(slot)) or {}).get('files', []):
            state.pop(rel, None)
        nc.remove_slot(slot, disc.pkg, nc.Out(a.mods, False), man, a.mods, lambda s_: print('    ' + s_))
    wj(st_p, state)


def _build(a, added):
    import charmod_avatars as AV
    names = mod_dirs(a.names)
    if not names and (a.names or not (os.path.exists(os.path.join(a.mods, RETAIL_STATE)) or added_slots(a.mods))):
        sys.exit('no character mods in %s' % MODS_SRC)
    disc = Disc(a.disc)
    if not a.names and not a.check:
        drop_gone_characters(a.mods, names, disc)
    for n in names:
        odd = check_mod_files(n)
        if odd:
            print('  %s: not used by the build (unknown kinds of file): %s' % (n, ', '.join(odd[:6]) + (' ...' if len(odd) > 6 else '')))
    taken, skipped, todo = set(), set(), []
    for n in names:
        m = load_mod(n)
        if is_retail(m):
            continue
        if registered(m, a.mods) and not (logo_changes(m, a.mods) and not a.check):
            taken.add(ch(m)['slot'])
        elif a.check:
            print('%-10s is not in the game yet (a build adds it)' % n)
        else:
            todo.append((not registered(m, a.mods), n))
    for new, n in sorted(todo):                         # characters already in first (a logo they give up makes room)
        s = register(n, load_mod(n), a.mods, disc, taken)
        if s is None:
            skipped.add(n)
        else:
            taken.add(s)
            if new:
                added.append((n, s))
    names = [n for n in names if n not in skipped]
    if not a.check:
        for n in names:
            m = load_mod(n)
            if not is_retail(m) and ch(m).get('based_on_id') is not None:
                refresh_avatars(n, a.disc)
    files, mod_list, entries, retail = {}, [], [], {}
    for n in names:
        m, f, e = build_files(n, disc, a.skin)
        c = ch(m)
        if is_retail(m):
            for gp in f:
                if gp in retail:
                    sys.exit('%s and %s both change %s: keep one of them' % (retail[gp], n, gp))
                retail[gp] = n
            files.update(f)
            print('%-10s %s (retail %s): %d changed file%s%s' % (n, c['name'], c['code'], len(f), '' if len(f) == 1 else 's',
                                                               (': ' + ', '.join(sorted(f))) if f else ', the retail files stay'))
            continue
        mod_list.append(m)
        files.update(f)
        entries += e
        print('%-10s %s (%s, slot %d): %d files, body %s' % (n, c['name'], c['code'], c['slot'], len(f),
                                                            a.skin or m.get('body', {}).get('skin', 'manikin')))
    # retail files an earlier build replaced that no mod replaces now (folder deleted / disabled / back to retail):
    # taken out of the mods dir so the game reads the retail file again. Building some mods only drops their own.
    rs_p = os.path.join(a.mods, RETAIL_STATE)
    old_retail = rj(rs_p) if os.path.exists(rs_p) else {}
    gone = sorted(gp for gp, owner in old_retail.items() if gp not in retail and (not a.names or owner in names))
    if entries:
        files[AV.G3_PATH] = AV.write_pack(a.mods, disc.pkg, entries)
    files['roster.txt'] = merged_roster(a.mods, mod_list).encode()
    fx = merge_fx(a.mods, mod_list)
    if fx is not None:
        files[FX_MANIFEST] = fx
    abil = rj_abilities()
    for m in mod_list:
        if m.get('move_names'):
            abil[ch(m)['code']] = {k: v for k, v in m['move_names'].items() if not k.startswith('_')}
    abil_bytes = (json.dumps(abil, indent=2, ensure_ascii=False) + '\n').encode()

    if a.check:
        bad = 0
        for gp, d in sorted(files.items()):
            p = os.path.join(a.mods, gp)
            cur = read(p) if os.path.exists(p) else None
            if not same(p, d):
                bad += 1
                print('  differs: %s (%s)' % (gp, 'missing' if cur is None else '%d -> %d bytes' % (len(cur), len(d))))
                if gp.endswith('.txt') and cur is not None:
                    for l in difflib.unified_diff(cur.decode().splitlines(), d.decode().splitlines(), lineterm='', n=0):
                        print('     ' + l)
        for gp in gone:
            if os.path.exists(os.path.join(a.mods, gp)):
                bad += 1
                print('  differs: %s (a build takes it out: %s is no longer changed by %s)' % (gp, gp, old_retail[gp]))
        if rj_abilities() != json.loads(abil_bytes):
            bad += 1
            print('  differs: extra_abilities.json (move names)')
        print('check: %d of %d files differ from %s' % (bad, len(files) + 1, a.mods) if bad else
              'check: all %d files match %s' % (len(files) + 1, a.mods))
        return 1 if bad else 0

    st_p = os.path.join(a.mods, STATE)
    state = rj(st_p) if os.path.exists(st_p) else {}
    stale = []
    for gp, d in sorted(files.items()):
        p = os.path.join(a.mods, gp)
        if gp in ('roster.txt', FX_MANIFEST, AV.G3_PATH) or not os.path.exists(p) or same(p, d):
            continue
        if state.get(gp) != hashlib.md5(read(p)).hexdigest():
            stale.append(gp)
    if stale and not a.force:
        print('These installed files changed since the mod folder last saw them (another tool or session wrote them):')
        for gp in stale:
            print('   ', gp)
        print('Bring them into the folder first:  charmod.py pull %s   (or build --force to overwrite them)' % ' '.join(names))
        return 2
    changed = [gp for gp, d in sorted(files.items()) if write(os.path.join(a.mods, gp), d)]
    for gp, d in files.items():
        state[gp] = hashlib.md5(d).hexdigest()
    for gp in gone:
        p = os.path.join(a.mods, gp)
        if os.path.exists(p):
            if state.get(gp) not in (None, hashlib.md5(read(p)).hexdigest()):
                print('  kept %s: %s no longer changes it, but another tool wrote it since' % (gp, old_retail[gp]))
                continue
            os.remove(p)
            print('  removed %s (the retail file is used again)' % gp)
        state.pop(gp, None)
    keep = {gp: o for gp, o in old_retail.items() if gp not in gone}
    keep.update(retail)
    if keep or old_retail:
        wj(rs_p, keep)
    wj(st_p, state)
    roster_changed = 'roster.txt' in changed
    if rj_abilities() != json.loads(abil_bytes):
        write(ABILITIES, abil_bytes)
        roster_changed = True
        changed.append('extra_abilities.json')
    for gp in changed:
        print('  wrote', gp)
    if not changed:
        print('nothing changed')
    if roster_changed:
        print('roster or move names changed: rebuilding main.bin')
        import subprocess
        r = subprocess.call([sys.executable, os.path.join(HERE, 'roster_mainbin.py'), a.disc, a.mods])
        if r:
            print('roster_mainbin.py failed (%d); run it by hand: roster_mainbin.py %s %s' % (r, a.disc, a.mods))
    return 0


# ---------------------------------------------------------------- unpack (from the current install)
def cmd_unpack(a):
    import charmod_avatars as AV
    final = os.path.join(MODS_SRC, a.name)
    if os.path.exists(final) and not a.force:
        sys.exit('mods/%s exists (use pull to refresh it from the install)' % a.name)
    prev = rj(os.path.join(final, 'mod.json')) if os.path.exists(os.path.join(final, 'mod.json')) else None
    root = os.path.join(tempfile.mkdtemp(prefix='charmod_'), a.name)   # built here, then synced into final
    for keep in ('skins', 'avatars/source'):
        if os.path.isdir(os.path.join(final, keep)):
            shutil.copytree(os.path.join(final, keep), os.path.join(root, keep))
    code = a.code
    line = next((l.split() for l in roster_lines(a.mods) if len(l.split()) > 2 and l.split()[1] == code), None)
    if not line:
        sys.exit('%s is not in %s/roster.txt' % (code, a.mods))
    slot, like_id, tokens = int(line[0]), int(line[2]), line[3:]
    mf_p = os.path.join(a.mods, 'readme_newchars.json')
    mf = rj(mf_p) if os.path.exists(mf_p) else {'slots': {}}
    slot_info = mf['slots'].get(str(slot), {})
    like = a.like or slot_info.get('like_code')
    if not like:
        sys.exit('give --like (the retail character code this one is built on)')
    disc = Disc(a.disc)
    menu, hooks, nm = split_roster(tokens)
    name = slot_info.get('name') or (nm or a.name).replace('_', ' ')
    m = {'_read_me': READ_ME,
         'character': {'name': name, 'full_name': slot_info.get('full_name') or name, 'series': slot_info.get('series'),
                       'code': code, 'slot': slot, 'based_on': like, 'based_on_id': like_id}}
    test = dict(m, menu_ids=menu, runtime_hooks=hooks)
    if roster_line(test) != ' '.join(line):
        sys.exit('cannot express the roster line in the new layout:\n  %s\n  %s' % (' '.join(line), roster_line(test)))
    abil = rj_abilities()
    notes = {}
    for k, v in (abil.get(code) or {}).items():
        if isinstance(v, dict) and 'cmd' in v:
            notes[int(v['cmd'], 16)] = '%s (ability %s)' % (v.get('name') or v.get('was', ''), k)
    # extra objects declared on the roster line (objs=new:src,...): their files belong to the character too
    objs = {}
    for t in hooks:
        if t.startswith('objs='):
            for pair in t[5:].split(','):
                if ':' in pair:
                    new_, src_ = pair.split(':', 1)
                    objs[new_] = src_
    def mine(f):
        return code in f or any(os.path.basename(f).startswith(n + '.') or os.path.basename(f).startswith(n + '_')
                                for n in objs)
    paths = sorted(set(slot_info.get('files', [])) | {
        os.path.relpath(os.path.join(dp, f), a.mods) for dp, _, fs in os.walk(a.mods) for f in fs if mine(f)})
    paths = [p for p in paths if os.path.exists(os.path.join(a.mods, p)) and not p.startswith('readme')]
    installed = {p: read(os.path.join(a.mods, p)) for p in paths}
    os.makedirs(root, exist_ok=True)
    # avatars
    av, av_paths = AV.unpack(root, a.mods, code, like, menu, installed, lambda p: disc.try_read(p.replace(code, like)),
                             disc.pkg, AV.Templates(disc, like, like_id))
    pg = (prev or {}).get('avatars', {}).get('generated_from')
    if isinstance(pg, dict) and 'crops' in pg:
        av['generated_from'] = pg
    elif slot_info.get('args'):
        g = dict(slot_info['args'])
        for k in ('slidein', 'portrait', 'logo'):
            if g.get(k) and not os.path.exists(g[k]):        # recorded on another machine: look in forge/art_sources
                alt = os.path.join(REPO, '..', '..', 'forge', 'art_sources', os.path.basename(g[k]))
                g[k] = alt if os.path.exists(alt) else None
            if g.get(k):
                rel = 'avatars/source/' + os.path.basename(g[k])
                write(os.path.join(root, rel), read(g[k]))
                g[k] = rel
        av['generated_from'] = AV.source_from_args(g)
    m['avatars'] = av
    m['body'] = None
    m['move_names'] = abil.get(code)
    m['runtime_hooks'] = hooks
    m['menu_ids'] = dict({'_info': 'entry ids in the shared menu pack, set by new_character.py; leave as they are'}, **menu)
    m['files'] = {}
    rigs = {}
    for gp in paths:
        if gp in av_paths:
            continue
        data = installed[gp]
        like_path = gp.replace(code, like)
        for n_, s_ in objs.items():
            b_ = os.path.basename(like_path)
            if b_.startswith(n_):
                like_path = os.path.join(os.path.dirname(like_path), s_ + b_[len(n_):])
        if disc.try_read(like_path) == data:
            m['files'][gp] = {'like': like_path}
            continue
        if gp.endswith('.objx') and gp.startswith('obj/'):
            stem = os.path.splitext(os.path.basename(gp))[0]
            d = 'moveset/' + stem
            parts = stem.split('_')                       # p_noc100[_a] -> obj/p_noc100_1p[_a].gmo
            model = 'obj/%s_1p%s.gmo' % ('_'.join(parts[:2]), ('_' + '_'.join(parts[2:])) if parts[2:] else '')
            bones = None
            if model in installed:
                import charmod_gmo as G
                bones = G.Model(installed[model]).bone_names()
            CO.unpack(data, os.path.join(root, d), {'commands': notes} if gp == 'obj/p_%s.objx' % code else None, bones)
            m['files'][gp] = d
        elif gp.endswith('.gmo') and gp.startswith('obj/'):
            h = hashlib.md5(data).hexdigest()
            if h not in rigs:
                rigs[h] = ('models/' + os.path.basename(gp), data)
            m['files'][gp] = rigs[h][0]
        else:
            write(os.path.join(root, 'files', gp), data)
            m['files'][gp] = 'files/' + gp
    # bodies: the rig is stored with the manikin body; a body of its own becomes a skin
    import charmod_gmo as G
    body = {'_info': 'skin = "manikin" (the based_on character\'s manikin, read from your disc) or a folder in skins/. '
                     'Change with: charmod.py body %s SKIN. models = the files the body goes on.' % a.name,
            'skin': 'manikin', 'manikin_from': 'obj/p_%s_zako.gmo' % like, 'models': []}
    made_skin = False
    pb = prev.get('body') if prev and isinstance(prev.get('body'), dict) else {}
    wspec = pb.get('weapons')                            # weapons: the rig keeps the default ones, a skin the rest
    if wspec:
        body['weapons'] = wspec
    for h, (rel, data) in rigs.items():
        stem = os.path.splitext(os.path.basename(rel))[0]
        try:
            G.Model(data).slots()
        except AssertionError as ex:
            print('  %s keeps its body (%s)' % (rel, ex))
            write(os.path.join(root, rel), data)
            continue
        try:
            manikin = G.skin_from_body(data, disc.read(body['manikin_from']))
        except Exception as ex:                          # no manikin, or a body the manikin cannot replace
            print('  %s keeps its body (no manikin skin: %s)' % (rel, ex))
            write(os.path.join(root, rel), data)
            continue
        rig = G.apply_skin(data, manikin)
        if wspec:
            rig = G.apply_weapons(rig, G.weapons_default(rig, disc.read(wspec['default_from']), wspec))
        own = G.extract_skin(data, 'the body installed on %s' % stem, weapons=wspec)
        assert G.apply_skin(rig, own) == data
        write(os.path.join(root, rel), rig)
        if rig == data:
            print('  %s has the manikin body%s installed; skins/ left as they were' % (rel, ' and default weapons' if wspec else ''))
        else:
            write(os.path.join(root, 'skins', a.skin_name, stem + '.skin'), G.skin_to_bytes(own))
            made_skin = True
        body['models'] += [gp for gp, s in m['files'].items() if s == rel]
    if prev and isinstance(prev.get('body'), dict):
        body['skin'] = prev['body'].get('skin', 'manikin')
    if a.keep_body and made_skin:
        body['skin'] = a.skin_name
    if not body['models']:                               # nothing to swap: the models are used as they are
        body['skin'] = 'as_is'
        body['_info'] = ('"as_is": the models are used as they are (this folder\'s models/, or the based-on character\'s). '
                         'To give the character another body: charmod.py body %s --from-gmo model.gmo --as NAME' % a.name)
    m['body'] = body
    fx = os.path.join(a.mods, FX_MANIFEST)
    if a.fx_prefix and os.path.exists(fx):
        man = rj(fx)
        draws = [d for d in man.get('draws', []) if d.get('name', '').startswith(a.fx_prefix)]
        if draws:
            m['fx_shaders'] = {'notes': {k: v for k, v in man.items() if k.startswith('_' + a.fx_prefix.rstrip('/'))},
                               'textures': man.get('textures', {}), 'draws': draws}
            used = {d['frag'] for d in draws} | set(man.get('textures', {}).values())
            sub = a.fx_prefix.rstrip('/')
            for dp, _, fs in os.walk(os.path.join(a.mods, 'fx_shaders')):
                for f in fs:
                    rel = os.path.relpath(os.path.join(dp, f), os.path.join(a.mods, 'fx_shaders'))
                    if rel.startswith(sub + '/') or rel in used:
                        gp = 'fx_shaders/' + rel
                        write(os.path.join(root, 'files', gp), read(os.path.join(a.mods, gp)))
                        m['files'][gp] = 'files/' + gp
    if os.path.exists(os.path.join(final, 'avatars', '_preview.png')):
        write(os.path.join(root, 'avatars', '_preview.png'), read(os.path.join(final, 'avatars', '_preview.png')))
    wj(os.path.join(root, 'mod.json'), m)
    sync_dir(root, final)
    st_p = os.path.join(a.mods, STATE)
    state = rj(st_p) if os.path.exists(st_p) else {}
    for gp, d in installed.items():
        state[gp] = hashlib.md5(d).hexdigest()
    wj(st_p, state)
    print('made mods/%s: %d files (%d borrowed from %s), %d avatars, body %s' % (
        a.name, len(m['files']), sum(isinstance(v, dict) for v in m['files'].values()), like,
        sum(1 for k, v in av.items() if isinstance(v, dict) and 'shows' in v), body['skin']))
    return 0


def sync_dir(src, dst):
    """Make dst hold exactly src's files: changed files are overwritten in place, stale ones removed."""
    keep = set()
    for dp, _, fs in os.walk(src):
        for f in fs:
            rel = os.path.relpath(os.path.join(dp, f), src)
            keep.add(rel)
            sp_, dp_ = os.path.join(src, rel), os.path.join(dst, rel)
            if not (os.path.exists(dp_) and filecmp.cmp(sp_, dp_, shallow=False)):
                os.makedirs(os.path.dirname(dp_), exist_ok=True)
                shutil.copyfile(sp_, dp_)
    left = []
    for dp, _, fs in os.walk(dst):
        for f in fs:
            rel = os.path.relpath(os.path.join(dp, f), dst)
            if rel not in keep:
                try:
                    os.remove(os.path.join(dst, rel))
                except OSError:
                    left.append(rel)
    if left:
        print('  could not remove %d stale files (delete them by hand): %s' % (len(left), ', '.join(left[:5])))


def cmd_pull(a):
    for n in mod_dirs(a.names):
        m = rj(os.path.join(MODS_SRC, n, 'mod.json'))
        if is_retail(m):
            print('%s changes a retail character: nothing to pull (the Move Studio writes it)' % n)
            continue
        c = m.get('character') or {'code': m.get('code'), 'based_on': m.get('like_code')}   # old layout too
        fx = m.get('fx_shaders', {}).get('draws', [])
        pre = fx[0]['name'].split('/')[0] + '/' if fx and '/' in fx[0]['name'] else None
        sd = os.path.join(MODS_SRC, n, 'skins')
        have = sorted(os.listdir(sd)) if os.path.isdir(sd) else []
        cur = (m.get('body') or {}).get('skin', 'manikin')
        skin_name = a.skin_name or (cur if cur != 'manikin' else (have[0] if len(have) == 1 else 'installed'))
        ns = argparse.Namespace(name=n, code=c['code'], like=c['based_on'], mods=a.mods, disc=a.disc, force=True,
                                skin_name=skin_name, keep_body=False, fx_prefix=pre)
        cmd_unpack(ns)
    return 0


# ---------------------------------------------------------------- new character from the template
def cmd_new(a):
    import re
    import charmod_avatars as AV
    import new_character as nc
    name = a.name
    if not re.match(r'^[a-z0-9][a-z0-9_-]*$', name):
        sys.exit('folder name: lower case letters, digits, - and _ (e.g. "terra-esper")')
    root = os.path.join(MODS_SRC, name)
    if os.path.exists(root):
        sys.exit('mods/%s already exists' % name)
    code = (a.code or '').lower()
    if code and not re.match(r'^[a-z][a-z0-9]{0,5}$', code):
        sys.exit('--code: 1..6 lower case letters/digits starting with a letter, e.g. ter100')
    disc = Disc(a.disc)
    like_id, like = nc.resolve_like(disc.pkg, a.like)
    # codes and slots already used: the roster, and every character folder
    used_codes, used_slots = set(), set()
    for l in roster_lines(a.mods):
        f = l.split()
        if len(f) > 1 and f[0].isdigit():
            used_slots.add(int(f[0])); used_codes.add(f[1])
    for d in os.listdir(MODS_SRC):
        p = os.path.join(MODS_SRC, d, 'mod.json')
        if os.path.exists(p) and not d.startswith('_'):
            c_ = rj(p).get('character', {})
            used_codes.add(c_.get('code')); used_slots.add(c_.get('slot'))
    taken = lambda c_: c_ in used_codes or disc.try_read('obj/p_%s.objx' % c_) is not None
    if code and taken(code):
        sys.exit('code %s is already used; pick another with --code' % code)
    if not code:                                         # first 3 letters of the name + the first free number
        base = (re.sub(r'[^a-z]', '', name) + 'xxx')[:3]
        code = next((base + str(n) for n in range(100, 1000) if not taken(base + str(n))), None)
        if code is None:
            sys.exit('no free code for %s; give one with --code' % name)
    rmax = nc.max_slots(GAME)
    slot = a.slot or next((s_ for s_ in range(35, rmax) if s_ not in used_slots), None)
    if slot is None:
        sys.exit('no free roster slot (35..%d)' % (rmax - 1))
    tpl_dir = os.path.join(MODS_SRC, '_template')
    m = rj(os.path.join(tpl_dir, 'mod.json'))
    m.pop('_template_info', None)
    m['character'].update({'name': a.display or name.replace('-', ' ').title(), 'full_name': a.full_name or a.display
                           or name.replace('-', ' ').title(), 'series': a.series or 'FINAL FANTASY', 'code': code,
                           'slot': slot, 'based_on': like, 'based_on_id': like_id})
    tpl = AV.Templates(disc, like, like_id)
    m['avatars'] = dict(AV.skeleton(code, tpl), generated_from=m['avatars'].get('generated_from'))
    os.makedirs(root)
    for f in ('README.md',):
        if os.path.exists(os.path.join(tpl_dir, f)):
            write(os.path.join(root, f), read(os.path.join(tpl_dir, f)))
    AV.placeholder_art(os.path.join(root, 'avatars', 'source', 'placeholder.png'))
    # the based-on character's moveset, unpacked so its moves can be edited
    import charmod_gmo as G
    objx = disc.read('obj/p_%s.objx' % like)
    bones = G.Model(disc.read('obj/p_%s_1p.gmo' % like)).bone_names()
    CO.unpack(objx, os.path.join(root, 'moveset', 'p_' + code), None, bones)
    m['files'] = {'_auto': True, '_info': m['files'].get('_info', ''), 'obj/p_%s.objx' % code: 'moveset/p_' + code}
    wj(os.path.join(root, 'mod.json'), m)
    print('made mods/%s: %s (code %s, slot %d), built on %s (charId %d)' % (name, m['character']['name'], code, slot,
                                                                              like, like_id))
    print('next: edit mods/%s/mod.json (name, series), add pictures (charmod.py avatars %s --from-picture art.png),'
          ' edit moves in mods/%s/moveset/p_%s/, then start the game (run.sh builds it in) or charmod.py build %s'
          % (name, name, name, code, name))
    return 0


# ---------------------------------------------------------------- avatars
def cmd_avatars(a):
    import charmod_avatars as AV
    root = os.path.join(MODS_SRC, a.name)
    m = load_mod(a.name)
    c = ch(m)
    disc = Disc(a.disc)
    tpl = AV.Templates(disc, c['based_on'], c['based_on_id'])
    made = {}
    gf = m['avatars'].get('generated_from') or {}
    if a.redo:
        if not (gf.get('picture') or gf.get('portrait')):
            sys.exit('no generated_from picture recorded: use --from-picture')
        a.from_picture = a.from_picture or (os.path.join(root, gf['picture']) if gf.get('picture') else None)
        a.portrait = a.portrait or (os.path.join(root, gf['portrait']) if gf.get('portrait') else None)
        crops = dict(gf.get('crops', {}))
        flags = dict(gf.get('options', {}))
    else:
        crops, flags = {}, {}
    if a.from_picture or a.portrait:
        crops.update(dict(x.split('=', 1) for x in (a.crop or [])))
        flags.update({k: v for k, v in (('hud_flip', a.hud_flip), ('tile_flip', a.tile_flip),
                                        ('tile_cutout', a.tile_cutout), ('hud_from', a.hud_from)) if v})
        made.update(AV.generate(root, m, tpl, a.from_picture, a.portrait, crops, flags))
        src = {}
        for k, p in (('picture', a.from_picture), ('portrait', a.portrait)):
            if p:
                rel = 'avatars/source/' + os.path.basename(p)
                if os.path.abspath(p) != os.path.abspath(os.path.join(root, rel)):
                    write(os.path.join(root, rel), read(p))
                src[k] = rel
        g = AV.source_from_args({})
        g.update({'picture': src.get('picture'), 'portrait': src.get('portrait'), 'logo': gf.get('logo'),
                  'crops': crops, 'options': flags})
        m['avatars']['generated_from'] = g
    if a.names or a.logo:
        made.update(AV.draw_names(m, tpl, a.logo))
    for k, im in made.items():
        e = m['avatars'].get(k)
        if not e:
            continue
        rel = e.get('picture') or 'avatars/%s.png' % k
        write(os.path.join(root, rel), AV.png_bytes(im))
        e['picture'] = rel
        e['size'] = '%dx%d' % im.size
        e['shows'] = e['shows'].replace(AV.BORROWED, '')
    if made:
        wj(os.path.join(root, 'mod.json'), m)
        print('made %d avatars: %s' % (len(made), ', '.join(sorted(made))))
    for k, e in m['avatars'].items():
        if isinstance(e, dict) and 'shows' in e:
            print('  %-20s %-9s %-30s %s' % (k, e.get('size', ''), e.get('picture') or "(%s's)" % c['based_on'], e['shows']))
    p = AV.preview(root, m, os.path.join(root, 'avatars', '_preview.png'))
    print('preview: %s\nbuild with: charmod.py build %s' % (p, a.name))
    return 0


# ---------------------------------------------------------------- body
def cmd_body(a):
    root = os.path.join(MODS_SRC, a.name)
    mp_ = os.path.join(root, 'mod.json')
    m = load_mod(a.name)
    if a.from_gmo:
        import charmod_gmo as G
        if not a.as_:
            sys.exit('--from-gmo needs --as SKIN_NAME')
        import re
        src = read(a.from_gmo)
        code = ch(m)['code']
        models = m['body'].get('models') or [gp for gp in m['files']
                                             if re.match(r'^obj/p_%s_(1p|2p|3p|zako)\.gmo$' % code, gp)]
        disc = Disc(a.disc)
        done = set()
        for gp in models:
            ref = m['files'][gp]
            key = json.dumps(ref)
            if key in done:
                continue
            done.add(key)
            rig = disc.read(ref['like']) if isinstance(ref, dict) else read(os.path.join(root, ref))
            stem = os.path.splitext(os.path.basename(gp))[0]
            sk = G.skin_from_body(rig, src, 'body of ' + os.path.basename(a.from_gmo))
            for g2 in models:                              # one skin file per model file the game asks for
                if json.dumps(m['files'][g2]) == key:
                    write(os.path.join(root, 'skins', a.as_, os.path.splitext(os.path.basename(g2))[0] + '.skin'),
                          G.skin_to_bytes(sk))
        m['body']['models'] = models
        a.skin = a.as_
    if not a.skin:
        sd = os.path.join(root, 'skins')
        print('body: %s; skins: %s' % (m['body'].get('skin'), ', '.join(sorted(os.listdir(sd))) if os.path.isdir(sd) else 'none'))
        return 0
    if a.skin not in ('manikin', 'as_is') and not os.path.isdir(os.path.join(root, 'skins', a.skin)):
        sys.exit('no skins/%s' % a.skin)
    m['body']['skin'] = a.skin
    wj(mp_, m)
    print('%s now uses body "%s"; run: charmod.py build %s' % (a.name, a.skin, a.name))
    return 0


# ---------------------------------------------------------------- list / hits
def cmd_list(a):
    for n in mod_dirs([]):
        m = rj(os.path.join(MODS_SRC, n, 'mod.json'))
        if 'character' not in m:
            print('%-10s (old layout: charmod.py pull %s)' % (n, n))
            continue
        c = ch(m)
        if is_retail(m):
            print('%-10s %-10s %s retail character, changes %s' % (n, c['name'], c['code'], ', '.join(
                k for k in m['files'] if not k.startswith('_'))))
            continue
        own = sum(not isinstance(v, dict) for v in m['files'].values())
        print('%-10s %-10s %s slot %d, based on %s, body %s, %d own files, %d borrowed, %d avatars' % (
            n, c['name'], c['code'], c['slot'], c['based_on'], m.get('body', {}).get('skin'), own, len(m['files']) - own,
            sum(1 for e in m.get('avatars', {}).values() if isinstance(e, dict) and 'shows' in e)))
    return 0


def cmd_hits(a):
    root = os.path.join(MODS_SRC, a.name)
    m = load_mod(a.name)
    d = os.path.join(root, 'moveset', 'p_' + ch(m)['code'])
    moves, acts = rj(os.path.join(d, 'moves.json')), rj(os.path.join(d, 'actions.json'))
    mm = rj(os.path.join(d, 'motion_map.json'))
    hits = {h['id']: h for h in rj(os.path.join(d, 'hits.json'))['hits']}
    script = {x['action']: x['script'] for x in acts['actions']}
    motion = {}
    for r in mm['rows']:
        motion.setdefault(r['script'], r['motion'])
    for c in moves['commands']:
        if a.move and a.move.lower() not in (c['id'].lower() + ' ' + c.get('_info', '')).lower():
            continue
        print('%s %s' % (c['id'], c.get('_info', '')))
        for k, s in enumerate(c['steps']):
            mo = motion.get(script.get(s['action']))
            p = os.path.join(d, 'timelines', '%03d.json' % mo) if mo is not None else None
            if not p or not os.path.exists(p):
                continue
            CO._CTX = CO.Ctx(d)
            for e in rj(p)['ranged']:
                op, vals = CO.ev_args(e)
                if op == 0x0200:
                    h = hits.get('0x%04X' % (vals[0] & 0xFFFF))
                    if h:
                        print('   press %d  timeline %03d  ticks %3d-%-3d  %-16s power %3d  rehit %2d  reaction 0x%02X' % (
                            k + 1, mo, e['start'], e['end'], h['name'], h['power'], h['rehit'], h['reaction']))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mods', default=DEF_MODS, help='game mods dir (PSPRECOMP_MODS), default work/mods')
    ap.add_argument('--disc', default=DEF_DISC, help='extracted disc (disc0), default work/disc0')
    sp = ap.add_subparsers(dest='cmd', required=True)
    b = sp.add_parser('build', help='build character mods into the game')
    b.add_argument('names', nargs='*')
    b.add_argument('--check', action='store_true', help='compare with the mods dir, write nothing')
    b.add_argument('--skin', help='use this body for this build only (e.g. to compare)')
    b.add_argument('--force', action='store_true', help='overwrite installed files changed by other tools')
    av = sp.add_parser('avatars', help='list / preview / make the menu and HUD pictures')
    av.add_argument('name')
    av.add_argument('--from-picture', help='a big full-body picture: grid tile, slide-in, HUD, face strips, VS face are cut from it')
    av.add_argument('--portrait', help='optional head-and-shoulders picture for the tile / HUD / faces')
    av.add_argument('--crop', action='append', metavar='AVATAR=x0,y0,x1,y1',
                    help='crop box in picture pixels for grid_tile, slidein, hud, face_strip or vs_face (repeatable)')
    av.add_argument('--hud-flip', action='store_true', help='mirror the picture for the HUD portrait')
    av.add_argument('--tile-flip', action='store_true', help='mirror the picture for the grid tile and VS face')
    av.add_argument('--tile-cutout', action='store_true', help='retail-style grid tile: the figure cut out, no dark box')
    av.add_argument('--hud-from', choices=('portrait', 'slidein'), help='which picture the HUD portrait is cut from')
    av.add_argument('--redo', action='store_true', help='make the picture avatars again from avatars.generated_from')
    av.add_argument('--names', action='store_true', help='redraw name plate, name text, encounter banner (and logo from series)')
    av.add_argument('--logo', help='logo artwork for the series plate')
    bo = sp.add_parser('body', help='choose or make the body')
    bo.add_argument('name')
    bo.add_argument('skin', nargs='?')
    bo.add_argument('--from-gmo', help='a model on the same skeleton (e.g. a Blender export); its part 0 becomes the body')
    bo.add_argument('--as', dest='as_', help='skin name for --from-gmo')
    sp.add_parser('list', help='the character mods')
    nw = sp.add_parser('new', help='start a new character from mods/_template')
    nw.add_argument('name', help='folder name, e.g. terra')
    nw.add_argument('--like', required=True, help='retail character to build on: code (e.g. fiv100) or charId')
    nw.add_argument('--code', help='character code, up to 6 letters/digits (default: from the name + 100)')
    nw.add_argument('--display', help='name shown in menus (default: from the folder name)')
    nw.add_argument('--full-name', help='full name (VS screen, battle banner)')
    nw.add_argument('--series', help='e.g. "FINAL FANTASY VI"')
    nw.add_argument('--slot', type=int, help='roster slot 35..42 (default: the first free one)')
    h = sp.add_parser('hits', help='every hit of every move')
    h.add_argument('name')
    h.add_argument('move', nargs='?')
    u = sp.add_parser('unpack', help='make a mod folder from what is installed')
    u.add_argument('name')
    u.add_argument('--code', required=True)
    u.add_argument('--like', help='code of the retail character it is built on (default from readme_newchars.json)')
    u.add_argument('--skin-name', default='installed', help='name of the skin made from the installed body')
    u.add_argument('--keep-body', action='store_true', help='keep the installed body as the default (not the manikin)')
    u.add_argument('--fx-prefix', help='fx_shaders draws (name prefix, e.g. alterna/) that belong to this character')
    u.add_argument('--force', action='store_true')
    pl = sp.add_parser('pull', help='refresh a mod folder from the install (after other tools changed it)')
    pl.add_argument('names', nargs='*')
    pl.add_argument('--skin-name', help='skin name for a non-manikin body found installed (default: the mod\'s skin)')
    a = ap.parse_args(argv)
    return {'build': cmd_build, 'unpack': cmd_unpack, 'pull': cmd_pull, 'body': cmd_body, 'list': cmd_list,
            'hits': cmd_hits, 'avatars': cmd_avatars, 'new': cmd_new}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
