#!/usr/bin/env python3
"""Write requirements.txt's --hash lines: the sha256 of every wheel PyPI has for each pinned version.

    python3 games/dissidia/tools/pin_hashes.py         # after changing a version in requirements.txt

pip then installs a file only if its hash is one of these (pydeps.py passes --require-hashes), so a changed or
swapped download is refused. Standard library only; needs the internet (pypi.org)."""
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REQ = os.path.join(HERE, "requirements.txt")


def main():
    head, pins = [], []
    for line in open(REQ):
        s = line.strip()
        if s.startswith('#') or not s:
            if not pins:
                head.append(line.rstrip('\n'))
            continue
        m = re.match(r'([A-Za-z0-9_.\-]+)==([^\s\;]+)', s)
        if m:
            pins.append((m.group(1), m.group(2)))
    out = head[:]
    for name, ver in pins:
        with urllib.request.urlopen('https://pypi.org/pypi/%s/%s/json' % (name, ver), timeout=60) as r:
            files = json.load(r)['urls']
        hs = sorted({f['digests']['sha256'] for f in files if f['packagetype'] == 'bdist_wheel'})
        if not hs:
            sys.exit('%s %s: no wheels on PyPI' % (name, ver))
        out.append('%s==%s \\' % (name, ver))
        out += ['    --hash=sha256:%s%s' % (h, ' \\' if i < len(hs) - 1 else '') for i, h in enumerate(hs)]
        print('%s %s: %d wheels' % (name, ver, len(hs)))
    with open(REQ, 'w') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
