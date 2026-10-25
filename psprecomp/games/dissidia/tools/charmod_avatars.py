"""Character avatars: the pictures the menus and HUD show for a character, as plain PNG files.

Each avatar is one picture in the mod's avatars/ folder. At build time it is encoded into every game file that shows it,
using the like's retail file as the template (format, palette size, frame), so an unchanged picture gives exactly the
bytes the game had. An avatar whose picture is null uses the like's own picture.

Two kinds of target:
  * per-character files   menu/<lang>/.../p_<code>_*.gim
  * the shared menu pack  by_crc/<G3>.bin: grid tile, slide-in art, name plate and logo of every extra character, at the
                          entry ids in mod.json "menu_ids" (tile=, art=, artl=/artr=, name=, logo=)
"""
import hashlib
import io
import os

from PIL import Image, ImageDraw, ImageOps

import character_art as ca
import gim_encode as ge
import menu_pack as mp
import new_character as nc
import package_extract as pe

# key, what it is (shown in mod.json), game paths ({L} language folder, {c} code)
FILE_AVATARS = [
    ('hud', 'In-battle portrait at the bottom of the screen (the same picture is used for P1 and the CPU, never mirrored)',
     ['menu/{L}/battle/chara_image/p_{c}_1p.gim', 'menu/{L}/battle/chara_image/p_{c}_2p.gim',
      'menu/{L}/battle/chara_image/p_{c}_3p.gim']),
    ('face_strip', 'Face strip in name panels', ['menu/{L}/common/chara_face_in_name/p_{c}_face_in_name.gim']),
    ('face_strip_mirrored', 'Face strip, mirrored copy (the other side of the screen)',
     ['menu/{L}/common/chara_face_in_name_r/p_{c}_face_in_name_r.gim']),
    ('vs_face', 'Square face (VS screen / story mode)', ['menu/{L}/story_mode/chara2/p_{c}_1p.gim']),
    ('name_text', 'Name text in menus', ['menu/{L}/common/chara_name/p_{c}_name.gim']),
    ('encounter_text', 'Name banner when a battle starts', ['menu/{L}/battle/encount/encount_text_tim2/encount_{c}.gim']),
    ('select_portrait', 'Character-select portrait (arcade / story)', ['menu/{L}/character_select/chara2/p_{c}_1p.gim']),
    ('arcade_last', 'Arcade mode final-opponent picture', ['menu/{L}/arcademode/arcade_last_chara_tim2/arcade_last_{c}.gim']),
    ('talk_top', 'Talk-event portrait (top)', ['menu/{L}/talkevent/tim2/chara/t_talk_{c}.gim']),
    ('talk_bottom', 'Talk-event portrait (bottom)', ['menu/{L}/talkevent/tim2/chara/b_talk_{c}.gim']),
]
# key, what it is, menu id keys it fills
PACK_AVATARS = [
    ('grid_tile', 'Quick Battle character grid tile', ['tile']),
    ('slidein', 'Player Select slide-in art and the battle setup panel portrait', ['art', 'artl', 'artr']),
    ('name_plate', 'Name plate on Player Select', ['name']),
    ('logo', 'Series logo plate on Player Select', ['logo']),
]
BASES = {'tile': ca.BASE_TILE, 'art': ca.BASE_ART, 'artl': ca.BASE_ART, 'artr': ca.BASE_ART,
         'name': ca.BASE_NAME, 'logo': ca.BASE_LOGO}
G3_PATH = 'by_crc/%08x.bin' % pe.package_crc(ca.G3)
# --from-picture: avatar key -> crop option name of new_character.Art
CROPS = {'grid_tile': 'tile_crop', 'slidein': 'art_crop', 'face_strip': 'face_crop', 'hud': 'hud_crop', 'vs_face': 'vs_crop'}
OPTIONS = ('hud_flip', 'tile_flip', 'tile_cutout', 'hud_from', 'art_faces')
ORDER = ['grid_tile', 'slidein', 'name_plate', 'logo', 'hud', 'vs_face', 'face_strip', 'face_strip_mirrored', 'name_text',
         'encounter_text', 'select_portrait', 'arcade_last', 'talk_top', 'talk_bottom']
BORROWED = " (now the based-on character's picture: set a PNG to replace it)"


def file_targets(code):
    out = {}
    for key, _, pats in FILE_AVATARS:
        out[key] = [p.format(L=L, c=code) for p in pats for L in ca.LANGS]
    return out


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=True)
    return b.getvalue()


def load_png(path):
    return Image.open(path).convert('RGBA')


class Templates:
    """Retail templates, read from the disc."""

    def __init__(self, disc, like_code, like_id):
        self.disc, self.like, self.like_id = disc, like_code, like_id
        self._g = {}

    def file(self, path, code):
        for c in (self.like, 'fiv100'):
            try:
                return self.disc.read(path.replace(code, c))
            except KeyError:
                continue
        return None

    def _pack(self, name):
        if name not in self._g:
            self._g[name] = mp.parse(self.disc.read(name))
        return self._g[name]

    def pack(self, idkey):
        tpl = self.like_id if 1 <= self.like_id <= 32 else 9
        if idkey == 'tile':
            return mp.entry(self._pack('menu/menu_pk_free_battle_gim_2.bin'), 12 + tpl)
        if idkey in ('art', 'artl', 'artr'):
            return mp.entry(self._pack('menu/menu_pk_free_battle_gim_0.bin'), 12 + 9)
        if idkey == 'name':
            return mp.entry(self._pack('menu/en/menu_lpk_free_battle_gim_0.bin'), 79 + tpl - 1)
        if idkey == 'logo':
            return mp.entry(self._pack('menu/menu_pk_free_battle_gim_2.bin'), 77 + 3)
        raise KeyError(idkey)


def fit(im, size, what):
    if im.size == tuple(size):
        return im
    print('  note: %s is %dx%d, the game wants %dx%d: scaled and centre-cropped to fit' % (what, im.size[0], im.size[1], *size))
    return ImageOps.fit(im, tuple(size), Image.LANCZOS)


def encode(template, im, what):
    size = ge.decode(template).size
    return ge.encode(template, fit(im, size, what))


# ---------------------------------------------------------------- build
def build(root, m, tpl, disc):
    """-> ({game path: bytes}, {menu id key: gim bytes})"""
    code = m['character']['code']
    files, pack = {}, {}
    av = m.get('avatars', {})
    targets = file_targets(code)
    for key, entry in av.items():
        if key.startswith('_') or not isinstance(entry, dict):
            continue
        pic = entry.get('picture')
        im = load_png(os.path.join(root, pic)) if pic else None
        keep = entry.get('_keep') or {}
        if keep and pic and hashlib.md5(open(os.path.join(root, pic), 'rb').read()).hexdigest() != keep.get('png_md5'):
            keep = {}                                  # the picture was edited: encode it
        for gp in entry.get('_files', []):
            t = tpl.file(gp, code)
            if im is None:
                if t is not None:
                    files[gp] = t
            elif gp in keep.get('files', {}):
                files[gp] = open(os.path.join(root, keep['files'][gp]), 'rb').read()
            else:
                files[gp] = encode(t, im, pic)
        for idkey in entry.get('_menu_ids', []):
            if idkey in m.get('menu_ids', {}) and im is not None:
                if idkey in keep.get('menu_ids', {}):
                    pack[idkey] = open(os.path.join(root, keep['menu_ids'][idkey]), 'rb').read()
                else:
                    pack[idkey] = encode(tpl.pack(idkey), im, pic)
    return files, pack


def write_pack(mods, disc_pkg, entries):
    """entries: [(menu id key, id, gim bytes)] -> bytes of the shared pack with those entries replaced."""
    base, orig, blobs = nc.shared_g3(mods, disc_pkg)
    for idkey, val, data in entries:
        k = val + BASES[idkey] - base['base']
        if k < 0 and idkey == 'logo':
            continue                                     # the game's own logo plate (logo=0..12): nothing to write
        assert 0 <= k < len(blobs), '%s=%d is outside the menu pack' % (idkey, val)
        blobs[k] = data
    return mp.build(base, blobs)


# ---------------------------------------------------------------- unpack (pictures from what is installed)
def _keep(root, key, img, targets, encoded):
    """targets: {target: (installed bytes, template)}; keeps the installed bytes the encoder doesn't reproduce."""
    im = Image.open(io.BytesIO(img)).convert('RGBA')
    kept = {}
    for tg, (inst, tmpl) in targets.items():
        if tmpl is None or ge.encode(tmpl, im) != inst:
            rel = 'avatars/_game/%s__%s.gim' % (key, tg.replace('/', '_'))
            os.makedirs(os.path.join(root, 'avatars', '_game'), exist_ok=True)
            with open(os.path.join(root, rel), 'wb') as f:
                f.write(inst)
            kept[tg] = rel
    return kept


def unpack(root, mods, code, like_code, menu_ids, installed, like_bytes, disc_pkg, tpl=None):
    """installed: {game path: bytes} of the character's installed files; like_bytes(path) -> retail bytes or None.
    Writes avatars/*.png, returns the mod.json 'avatars' section."""
    os.makedirs(os.path.join(root, 'avatars'), exist_ok=True)
    av = {'_info': ('Pictures the menus and HUD show, in avatars/. Replace a PNG with your own picture (same size; other '
                    'sizes are scaled to fit), then build. "picture": null = the based-on character\'s picture. '
                    'Make them all from one big picture: charmod.py avatars <folder> --from-picture art.png. '
                    'See them all: charmod.py avatars <folder> (draws avatars/_preview.png). "_files" / "_menu_ids" = where '
                    'each picture goes in the game; "_keep" = the game\'s own bytes for a picture, used until you edit '
                    'the PNG. Leave those as they are.')}
    used = set()
    for key, shows, _ in FILE_AVATARS:
        paths = [p for p in file_targets(code)[key] if p in installed]
        if not paths:
            continue
        used.update(paths)
        groups = {}
        for p in paths:
            lb = like_bytes(p)
            if lb is not None and lb == installed[p]:
                groups.setdefault(None, []).append(p)
            else:
                im = ge.decode(installed[p])
                groups.setdefault(png_bytes(im), []).append(p)
        for n, (img, ps) in enumerate(sorted(groups.items(), key=lambda kv: kv[0] is not None)):
            k = key if n == 0 else '%s_%d' % (key, n + 1)
            if img is None:
                av[k] = {'picture': None, 'shows': shows + BORROWED, '_files': ps}
                continue
            rel = 'avatars/%s.png' % k
            with open(os.path.join(root, rel), 'wb') as f:
                f.write(img)
            w, h = Image.open(io.BytesIO(img)).size
            av[k] = {'picture': rel, 'size': '%dx%d' % (w, h), 'shows': shows, '_files': ps}
            kept = _keep(root, k, img, {p: (installed[p], tpl.file(p, code) if tpl else None) for p in ps}, None)
            if kept:
                av[k]['_keep'] = {'png_md5': hashlib.md5(img).hexdigest(), 'files': kept}
    base, orig, blobs = nc.shared_g3(mods, disc_pkg)
    for key, shows, ids in PACK_AVATARS:
        have = [i for i in ids if i in menu_ids and menu_ids[i] + BASES[i] - base['base'] >= 0]   # not the game's own logo
        if not have:
            continue
        groups = {}
        for i in have:
            d = blobs[menu_ids[i] + BASES[i] - base['base']]
            groups.setdefault(png_bytes(ge.decode(d)), []).append(i)
        for n, (img, ks) in enumerate(groups.items()):
            k = key if n == 0 else '%s_%s' % (key, ks[0])
            rel = 'avatars/%s.png' % k
            with open(os.path.join(root, rel), 'wb') as f:
                f.write(img)
            w, h = Image.open(io.BytesIO(img)).size
            av[k] = {'picture': rel, 'size': '%dx%d' % (w, h), 'shows': shows, '_menu_ids': ks}
            kept = _keep(root, k, img, {i: (blobs[menu_ids[i] + BASES[i] - base['base']], tpl.pack(i) if tpl else None)
                                        for i in ks}, None)
            if kept:
                av[k]['_keep'] = {'png_md5': hashlib.md5(img).hexdigest(), 'menu_ids': kept}
    rank = {k: i for i, k in enumerate(ORDER)}
    keys = sorted((k for k in av if k != '_info'), key=lambda k: (rank.get(k.split('_')[0] if k not in rank else k, 99), k))
    return dict([('_info', av['_info'])] + [(k, av[k]) for k in keys]), used


def source_from_args(args):
    """new_character.py options -> the 'generated_from' record (avatar names)."""
    inv = {v: k for k, v in CROPS.items()}
    return {'_info': 'what the avatars were made from: picture (full body), optional portrait (head and shoulders), crop '
                     'boxes x0,y0,x1,y1 in picture pixels per avatar. Change them and run: charmod.py avatars <folder> --redo',
            'picture': args.get('slidein'), 'portrait': args.get('portrait'), 'logo': args.get('logo'),
            'crops': {inv[k]: v for k, v in args.items() if k in inv and v},
            'options': {k: args[k] for k in OPTIONS if args.get(k)}}


# ---------------------------------------------------------------- make pictures
def generate(root, m, tpl, picture, portrait=None, crops=None, flags=None):
    """Cut the picture avatars from one big picture (new_character.Art). Returns {key: PIL image}."""
    args = {'slidein': picture, 'portrait': portrait}
    for k, v in (crops or {}).items():
        if k not in CROPS:
            raise SystemExit('--crop %s: crops exist for %s' % (k, ', '.join(CROPS)))
        args[CROPS[k]] = v
    args.update(flags or {})
    art = nc.Art(args)
    code = m['character']['code']
    out = {}
    av = m['avatars']

    def tfile(key):
        e = av.get(key)
        return tpl.file(e['_files'][0], code) if e and e.get('_files') else None
    if 'grid_tile' in av:
        out['grid_tile'] = art.tile(ge.decode(tpl.pack('tile')))
    if 'slidein' in av:
        out['slidein'] = art.art((256, 256))
    for key, flip in (('face_strip', False), ('face_strip_mirrored', True)):
        t = tfile(key)
        if t is not None:
            out[key] = art.strip(ge.decode(t), flip)
    t = tfile('vs_face')
    if t is not None:
        out['vs_face'] = art.square_face(ge.decode(t).size)
    if 'hud' in av:
        out['hud'] = art.hud()
    return out


def draw_names(m, tpl, logo_picture=None):
    c = m['character']
    name, full = c['name'], c.get('full_name') or c['name']
    out = {}
    av = m['avatars']
    if 'name_plate' in av:
        out['name_plate'] = ge.render_name(name, (128, 32))
    if 'name_text' in av:
        out['name_text'] = ge.render_name(full, (128, 32))
    if 'encounter_text' in av:
        out['encounter_text'] = nc.encount_image(full)
    if 'logo' in av and (logo_picture or c.get('series')):
        game, numeral = nc.split_series(c.get('series') or '')
        img = nc.load_image(logo_picture, '--logo') if logo_picture else None
        out['logo'] = ge.render_logo(numeral, (128, 64), ge.decode(tpl.pack('logo')), game=game or 'FINAL FANTASY', image=img)
    return out


def preview(root, m, path):
    """Contact sheet of every avatar at 2x on a checkerboard, labelled."""
    items = [(k, e) for k, e in m['avatars'].items()
             if not k.startswith('_') and isinstance(e, dict) and 'shows' in e and e.get('picture')]
    pics = [(k, load_png(os.path.join(root, e['picture']))) for k, e in items]
    pad, lab = 16, 18
    tiles = []
    for k, im in pics:
        big = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
        w, h = max(big.width, 200), big.height + lab
        t = Image.new('RGBA', (w, h), (40, 40, 48, 255))
        chk = Image.new('RGBA', big.size, (0, 0, 0, 0))
        dr = ImageDraw.Draw(chk)
        for y in range(0, big.height, 8):
            for x in range(0, big.width, 8):
                dr.rectangle([x, y, x + 7, y + 7], fill=(90, 90, 90, 255) if (x // 8 + y // 8) % 2 else (130, 130, 130, 255))
        chk.alpha_composite(big)
        t.paste(chk, (0, lab))
        ImageDraw.Draw(t).text((2, 2), '%s  %dx%d' % (k, im.width, im.height), fill=(235, 235, 235, 255))
        tiles.append(t)
    W = 1100
    rows, row, x, rh = [], [], 0, 0
    for t in tiles:
        if row and x + t.width > W:
            rows.append((row, rh)); row, x, rh = [], 0, 0
        row.append(t); x += t.width + pad; rh = max(rh, t.height)
    if row:
        rows.append((row, rh))
    H = sum(h + pad for _, h in rows) + pad
    sheet = Image.new('RGBA', (W + pad, max(H, 40)), (24, 24, 30, 255))
    y = pad
    for row, rh in rows:
        x = pad
        for t in row:
            sheet.paste(t, (x, y)); x += t.width + pad
        y += rh + pad
    sheet.convert('RGB').save(path)
    return path


def skeleton(code, tpl):
    """The avatars section of a new character: every avatar the based-on character has, no pictures yet (null = the
    based-on character's picture until you add one)."""
    av = {'_info': ('Pictures the menus and HUD show. "picture": null = the based-on character\'s picture. Add your own: put a '
                    'PNG in avatars/ and set "picture" to it (size as listed), or make them all from one big picture with '
                    'charmod.py avatars <folder> --from-picture art.png. charmod.py avatars <folder> draws avatars/_preview.png.')}
    for key, shows, pats in FILE_AVATARS:
        fs = [p.format(L=L, c=code) for p in pats for L in ca.LANGS if tpl.file(p.format(L=L, c=code), code) is not None]
        if fs:
            size = ge.decode(tpl.file(fs[0], code)).size
            av[key] = {'picture': None, 'size': '%dx%d' % size, 'shows': shows, '_files': fs}
    for key, shows, ids in PACK_AVATARS:
        size = ge.decode(tpl.pack(ids[0])).size
        av[key] = {'picture': None, 'size': '%dx%d' % size, 'shows': shows, '_menu_ids': ids[:2] if key == 'slidein' else ids[:1]}
    rank = {k: i for i, k in enumerate(ORDER)}
    keys = sorted((k for k in av if k != '_info'), key=lambda k: rank.get(k, 99))
    return dict([('_info', av['_info'])] + [(k, av[k]) for k in keys])


def placeholder_art(path, label='NEW'):
    """A plain full-body silhouette (transparent background) to stand in for real artwork."""
    W, H = 900, 1400
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    body = (70, 78, 104, 255)
    d.ellipse((350, 90, 550, 310), fill=body)                       # head
    d.rounded_rectangle((300, 330, 600, 820), 60, fill=body)        # torso
    d.rounded_rectangle((230, 350, 300, 780), 30, fill=body)        # arms
    d.rounded_rectangle((600, 350, 670, 780), 30, fill=body)
    d.rounded_rectangle((320, 800, 440, 1340), 40, fill=body)       # legs
    d.rounded_rectangle((460, 800, 580, 1340), 40, fill=body)
    d.text((395, 540), label, fill=(230, 230, 240, 255))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
