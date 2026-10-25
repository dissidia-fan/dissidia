#!/usr/bin/env python3
"""texpack.py - build a texture-replacement pack from a texture dump.

The runtime (PSPRECOMP_TEX_DUMP=1) writes every decoded texture it sees to
<mods>/textures/dump/<id>.png plus an index <mods>/textures/textures.txt.
This tool runs an external upscaler over those files and writes the results to
<mods>/textures/<id>.png, which the runtime loads in place of the original
(any size; UVs are normalised).  Nothing is downloaded or installed here: the
upscaler is whatever command you give it.

  texpack.py <dump-dir> --list [--min-size 32]
  texpack.py <dump-dir> --cmd "realesrgan-ncnn-vulkan -i {in} -o {out} -s 4" --min-size 32
  texpack.py <dump-dir> --cmd "waifu2x-ncnn-vulkan -i {in} -o {out} -n 1 -s 2"
  texpack.py <dump-dir> --cmd "convert {in} -filter Lanczos -resize 400% {out}"   # no AI needed

{in} / {out} are substituted per argument (paths with spaces are safe; the
command is split with shlex and run without a shell).  Ids that already have
an output file are skipped, so a run can be interrupted and resumed; outputs
are written to a temporary name and renamed when the command succeeded.

Upscalers that drop the alpha channel (several do) are repaired afterwards when
Pillow is installed: the original alpha is resized (Lanczos) to the output size
and re-attached (--no-alpha-fix to disable).  --scale-check N verifies the
output is N times the input size and rejects it otherwise.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

ID_RE = re.compile(r'^[0-9a-fA-F]{16}$')


def read_index(textures_dir):
    """textures.txt -> {id: dict(size=(w,h), fmt, clut, addr, seen)}"""
    idx = {}
    p = os.path.join(textures_dir, 'textures.txt')
    if not os.path.exists(p):
        return idx
    for line in open(p):
        if line.startswith('#') or not line.strip():
            continue
        f = line.split()
        if len(f) < 6 or not ID_RE.match(f[0]):
            continue
        try:
            w, h = (int(x) for x in f[1].split('x'))
        except ValueError:
            continue
        idx[f[0].lower()] = dict(size=(w, h), fmt=f[2], clut=f[3], addr=f[4], seen=int(f[5]))
    return idx


def png_size(path):
    with open(path, 'rb') as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    return int.from_bytes(head[16:20], 'big'), int.from_bytes(head[20:24], 'big')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump_dir', help='<mods>/textures/dump (as written by PSPRECOMP_TEX_DUMP=1)')
    ap.add_argument('--out', help='pack directory (default: parent of the dump dir)')
    ap.add_argument('--cmd', help='upscaler command template with {in} and {out}')
    ap.add_argument('--list', action='store_true', help='print the index (id, size, format, clut, seen, status) and exit')
    ap.add_argument('--min-size', type=int, default=0, metavar='N', help='only textures with width>=N and height>=N')
    ap.add_argument('--max-size', type=int, default=0, metavar='N', help='only textures with width<=N and height<=N')
    ap.add_argument('--filter', metavar='WxH', help='shorthand for --min-size: e.g. ">=32x32" (width and height at least)')
    ap.add_argument('--format', help='only this texture format (e.g. CLUT8, 8888); see --list')
    ap.add_argument('--limit', type=int, default=0, help='stop after N textures were processed')
    ap.add_argument('--scale-check', type=int, default=0, metavar='N', help='require output == N x input size')
    ap.add_argument('--no-alpha-fix', action='store_true')
    ap.add_argument('--force', action='store_true', help='redo ids that already have an output')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    dump = os.path.abspath(a.dump_dir)
    if not os.path.isdir(dump):
        sys.exit('not a directory: ' + dump)
    out = os.path.abspath(a.out) if a.out else os.path.dirname(dump)
    if a.filter:
        m = re.match(r'^\s*>=?\s*(\d+)\s*x\s*(\d+)\s*$', a.filter)
        if not m:
            sys.exit('--filter expects ">=WxH", e.g. ">=32x32"')
        a.min_size = max(int(m.group(1)), int(m.group(2)))

    idx = read_index(os.path.dirname(dump))
    ids = sorted(f[:-4].lower() for f in os.listdir(dump) if f.lower().endswith('.png') and ID_RE.match(f[:-4]))

    rows = []
    for i in ids:
        src = os.path.join(dump, i + '.png')
        meta = idx.get(i)
        size = meta['size'] if meta else png_size(src)
        if not size:
            continue
        w, h = size
        if w < a.min_size or h < a.min_size:
            continue
        if a.max_size and (w > a.max_size or h > a.max_size):
            continue
        if a.format and (not meta or meta['fmt'].lower() != a.format.lower()):
            continue
        done = os.path.exists(os.path.join(out, i + '.png'))
        rows.append((i, size, meta, done))

    if a.list:
        print('# id size format clut seen status')
        for i, (w, h), meta, done in rows:
            print('%s %dx%d %s %s %s %s' % (i, w, h, meta['fmt'] if meta else '?', meta['clut'] if meta else '?',
                                           meta['seen'] if meta else '?', 'done' if done else 'todo'))
        print('# %d texture(s), %d done, %d todo' % (len(rows), sum(1 for r in rows if r[3]), sum(1 for r in rows if not r[3])))
        return
    if not a.cmd:
        sys.exit('--cmd is required (or use --list)')
    if '{in}' not in a.cmd or '{out}' not in a.cmd:
        sys.exit('--cmd must contain {in} and {out}')

    try:
        from PIL import Image
    except ImportError:
        Image = None
    base = shlex.split(a.cmd)
    os.makedirs(out, exist_ok=True)
    todo = [r for r in rows if a.force or not r[3]]
    print('%d candidate(s), %d to do -> %s' % (len(rows), len(todo), out))
    n_ok = n_fail = 0
    for k, (i, (w, h), meta, done) in enumerate(todo, 1):
        if a.limit and k > a.limit:
            break
        src = os.path.join(dump, i + '.png')
        dst = os.path.join(out, i + '.png')
        tmp = os.path.join(out, i + '.tmp.png')
        argv = [t.replace('{in}', src).replace('{out}', tmp) for t in base]
        print('[%d/%d] %s %dx%d' % (k, len(todo), i, w, h), flush=True)
        if a.dry_run:
            print('   ', ' '.join(shlex.quote(t) for t in argv))
            continue
        try:
            r = subprocess.run(argv)
        except OSError as e:
            sys.exit('cannot run %s: %s' % (argv[0], e))
        if r.returncode != 0 or not os.path.exists(tmp):
            print('   FAILED (exit %d)' % r.returncode)
            n_fail += 1
            if os.path.exists(tmp):
                os.remove(tmp)
            continue
        ow, oh = png_size(tmp) or (0, 0)
        if a.scale_check and (ow != w * a.scale_check or oh != h * a.scale_check):
            print('   rejected: %dx%d is not %dx the input' % (ow, oh, a.scale_check))
            os.remove(tmp)
            n_fail += 1
            continue
        if Image and not a.no_alpha_fix:
            try:
                s = Image.open(src).convert('RGBA')
                alpha = s.getchannel('A')
                if alpha.getextrema() != (255, 255):
                    o = Image.open(tmp).convert('RGBA')
                    o.putalpha(alpha.resize(o.size, Image.LANCZOS))
                    o.save(tmp)
            except Exception as e:   # leave the upscaler's output untouched
                print('   alpha fix skipped:', e)
        os.replace(tmp, dst)
        n_ok += 1
    print('done: %d written, %d failed' % (n_ok, n_fail))


if __name__ == '__main__':
    main()
