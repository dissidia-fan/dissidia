#!/usr/bin/env python3
"""Merge addresses into a manifest's [recompile] force_entries.
Usage: addforce.py <toml> <addr|file>...  (files: one hex address per line)"""
import re, sys, os
p = sys.argv[1]; s = open(p).read()
new = set()
for a in sys.argv[2:]:
    if os.path.isfile(a): new |= {int(x, 16) for x in open(a).read().split() if x}
    else: new.add(int(a, 16))
head, rest = s.split('force_entries = [', 1); body, tail = rest.split(']', 1)
cur = {int(x, 16) for x in re.findall(r'"0x([0-9A-Fa-f]{8})"', body)}
added = new - cur
open(p, 'w').write(head + 'force_entries = [' + ', '.join(f'"0x{v:08X}"' for v in sorted(cur | new)) + ']' + tail)
print(f'{p}: +{len(added)} ({len(cur | new)} total)')
