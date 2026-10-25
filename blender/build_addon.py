#!/usr/bin/env python3
"""Package the Blender add-on (io_scene_dissidia/) as an installable zip.

    python3 blender/build_addon.py            # -> blender/io_scene_dissidia.zip (in git)
    python3 blender/build_addon.py --versioned  # also a copy in dist/io_scene_dissidia_v<version>.zip
    python3 blender/build_addon.py --out FILE.zip

The zip in the repo is the one to install; rebuild it after changing the add-on so the repo copy stays current.

Install in Blender 4.4+: Edit > Preferences > Add-ons > Install from Disk, pick the zip, enable
"Dissidia 012 Character Tools". The version comes from bl_info in io_scene_dissidia/__init__.py.
"""
import argparse
import ast
import os
import py_compile
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'io_scene_dissidia')


def version():
    tree = ast.parse(open(os.path.join(SRC, '__init__.py')).read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', '') == 'bl_info' for t in node.targets):
            return '.'.join(str(x) for x in ast.literal_eval(node.value)['version'][:2])
    sys.exit('no bl_info version in io_scene_dissidia/__init__.py')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', default=os.path.join(HERE, 'io_scene_dissidia.zip'), help='zip to write')
    ap.add_argument('--versioned', action='store_true', help='also copy it to dist/io_scene_dissidia_v<version>.zip')
    a = ap.parse_args(argv)
    files = []
    for dp, dns, fns in os.walk(SRC):
        dns[:] = sorted(d for d in dns if d != '__pycache__' and not d.startswith('.'))
        for f in sorted(fns):
            if f.endswith(('.pyc', '.pyo')) or f.startswith('.'):
                continue
            p = os.path.join(dp, f)
            if f.endswith('.py'):
                py_compile.compile(p, cfile=os.path.join(tempfile.gettempdir(), 'addon_check.pyc'), doraise=True)
            files.append(p)
    out = a.out
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in files:
            # fixed timestamps: an unchanged add-on gives a byte-identical zip (no churn in git)
            zi = zipfile.ZipInfo(os.path.join('io_scene_dissidia', os.path.relpath(p, SRC)), (2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(p, 'rb').read())
    print('%s: version %s (%d files, %d bytes)' % (out, version(), len(files), os.path.getsize(out)))
    if a.versioned:
        import shutil
        os.makedirs(os.path.join(HERE, 'dist'), exist_ok=True)
        v = os.path.join(HERE, 'dist', 'io_scene_dissidia_v%s.zip' % version())
        shutil.copyfile(out, v)
        print('copy:', v)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
