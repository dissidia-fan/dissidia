#!/usr/bin/env python3
"""Find code that Ghidra left outside any function: (a) every `jal` target in
the image, (b) every word in the image that looks like a pointer into the
text range and lands right after a function end / `jr ra` pair (function
pointer tables: comparators, callbacks, vtables). Emits force entries.

Usage: gapfuncs.py <image.bin> <base_hex> <text_end_hex> <analysis.json> <existing.toml>
"""
import json, re, struct, sys
img=open(sys.argv[1],'rb').read(); base=int(sys.argv[2],16); tend=int(sys.argv[3],16)
a=json.load(open(sys.argv[4])); existing=open(sys.argv[5]).read()
fs=sorted((int(f['address'],16), int(f['address'],16)+f['size']) for f in a['functions'])
ends=set(e for _,e in fs)
# earlier force entries are code too (extent unknown: up to the next known start)
prev_force=set(int(x,16) for x in re.findall(r'"0x([0-9A-Fa-f]{8})"', existing.split('force_entries')[1].split(']')[0]))
_starts=sorted(s for s,_ in fs)
import bisect as _b
for v in prev_force:
    if any(s<=v<e for s,e in fs): continue
    i=_b.bisect_right(_starts,v); nxt=_starts[i] if i<len(_starts) else v+4
    fs.append((v,nxt))
fs.sort()
import bisect
starts=[s for s,_ in fs]
def covered(v):
    i=bisect.bisect_right(starts,v)-1
    return i>=0 and fs[i][0]<=v<fs[i][1]
n=len(img)//4; W=struct.unpack_from('<%dI'%n,img,0)
def is_jr_ra(w): return w==0x03e00008
cand=set()
for i,w in enumerate(W):
    va=base+i*4
    if va<tend and (w>>26)==3:                       # jal
        t=((va+4)&0xF0000000)|((w&0x3ffffff)<<2)
        if base<=t<tend and not covered(t): cand.add(t)
    if va<tend and covered(va):
        op=w>>26
        if op==2:                                    # j into a gap
            t=((va+4)&0xF0000000)|((w&0x3ffffff)<<2)
            if base<=t<tend and not covered(t): cand.add(t)
        elif op in (1,4,5,6,7,0x14,0x15,0x16,0x17):  # bxx into a gap
            t=va+4+struct.unpack('<h',struct.pack('<H',w&0xffff))[0]*4
            if base<=t<tend and not covered(t): cand.add(t)
        elif op==0x0f:                               # lui/addiu pair -> code address
            rh=(w>>16)&31
            for j in range(1,9):
                if i+j>=n: break
                x=W[i+j]
                if (x>>26)==0x09 and ((x>>21)&31)==rh:
                    t=(((w&0xffff)<<16)+struct.unpack('<h',struct.pack('<H',x&0xffff))[0])&0xffffffff
                    if base<=t<tend and t%4==0 and not covered(t):
                        k=(t-base)//4
                        if (t in ends) or (k>=2 and is_jr_ra(W[k-2])): cand.add(t)
                    break
    if base<=w<tend and w%4==0 and not covered(w):   # pointer-like word
        k=(w-base)//4
        prev_end = (w in ends) or (k>=2 and is_jr_ra(W[k-2])) or (k>=1 and W[k-1]==0 and k>=3 and is_jr_ra(W[k-3]))
        if prev_end: cand.add(w)
force=set(int(x,16) for x in re.findall(r'"0x([0-9A-Fa-f]{8})"', existing.split('force_entries')[1].split(']')[0]))
new=cand-force
print(f'# new gap functions: {len(new)}', file=sys.stderr)
force|=new
head=existing.split('force_entries = [')[0]; tail=existing.split('force_entries = [')[1].split(']',1)[1]
sys.stdout.write(head+'force_entries = ['+', '.join(f'"0x{v:08X}"' for v in sorted(force))+']'+tail)
