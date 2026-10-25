#!/usr/bin/env python3
"""Find compiler-generated switch jump tables in a flat MIPS code image and
emit [recompile] force entries for every case target.

Ghidra's data xrefs for these tables don't reach the recompiler, so cases
surface one at a time as LOOKUP_MISS at runtime. GCC's Allegrex idiom:

    sltiu rC, rI, N          ; bounds check, N cases
    beq   rC, zero, default
    ...
    sll   rT, rI, 2
    lui   rH, hi(table)
    addiu rB, rH, lo(table)  ; or lw rZ, lo(rH) directly
    addu  rA, rT, rB
    lw    rZ, 0(rA)
    jr    rZ

Usage: jumptables.py <code.bin> <base_vaddr_hex> <analysis.json> [existing.toml]
Prints a TOML [recompile] section (merged with existing.toml's entries if given).
"""
import json, re, struct, sys

def s16(v): return struct.unpack('<h', struct.pack('<H', v & 0xffff))[0]

def main():
    code = open(sys.argv[1], 'rb').read(); base = int(sys.argv[2], 16)
    a = json.load(open(sys.argv[3]))
    fs = sorted((int(f['address'], 16), int(f['address'], 16) + f['size']) for f in a['functions'])
    existing = open(sys.argv[4]).read() if len(sys.argv) > 4 else ''
    n = len(code) // 4
    W = [struct.unpack_from('<I', code, i * 4)[0] for i in range(n)]
    lo_code, hi_code = base, base + n * 4
    tables = {}
    for i in range(n):
        w = W[i]
        if (w >> 26) != 0 or (w & 0x3f) != 8: continue           # jr
        rz = (w >> 21) & 31
        if rz == 31: continue
        # find lw rz, off(ra)
        lw = None
        for k in range(i - 1, max(i - 10, -1), -1):
            x = W[k]
            if (x >> 26) == 0x23 and ((x >> 16) & 31) == rz:
                lw = (k, (x >> 21) & 31, s16(x)); break
        if not lw: continue
        k, ra, off = lw
        # resolve table base: addiu rb, rh, lo + lui rh, hi (rb feeds addu -> ra), or lui ra directly
        hi = lo = None
        for m in range(k - 1, max(k - 12, -1), -1):
            x = W[m]
            if (x >> 26) == 0x09 and lo is None:        # addiu
                lo = (s16(x), (x >> 21) & 31)
            if (x >> 26) == 0x0f:                        # lui
                rh = (x >> 16) & 31
                if lo is not None and rh == lo[1]:
                    hi = (x & 0xffff) << 16; break
                if ra == rh and lo is None:
                    hi = (x & 0xffff) << 16; lo = (off, rh); off = 0; break
        if hi is None or lo is None: continue
        table = (hi + lo[0] + off) & 0xffffffff
        # case count from sltiu
        # case count: sltiu (unsigned bound) or slti (signed range checks,
        # GCC emits several for a sparse switch) — take the largest within
        # reach; every entry is validated against the code range anyway.
        N = 0
        for m in range(k, max(k - 24, -1), -1):
            x = W[m]
            if (x >> 26) in (0x0a, 0x0b): N = max(N, x & 0xffff)
        if not N: continue
        N = min(N, 1024)
        if not (lo_code <= table < hi_code): continue
        toff = (table - base) // 4
        cases = set()
        # Walk the whole bound: tables sit back to back in .rodata, and an
        # over-read into the next table (whose own jr idiom may not match)
        # still yields real case targets. Non-code words are skipped.
        for c in range(N):
            if toff + c >= n: break
            t = W[toff + c]
            if lo_code <= t < hi_code and t % 4 == 0: cases.add(t)
        # Extend past the bound while words keep pointing into the jr's own
        # function: GCC lays several switches' tables back to back and the
        # tail of a table can be reached by a sibling jr whose idiom this
        # scanner doesn't match (pause menu Retry/Forfeit cases, #sas).
        jr_va = base + i * 4
        owner = next((f for f in fs if f[0] <= jr_va < f[1]), None)
        if owner:
            c = N
            while toff + c < n:
                t = W[toff + c]
                if not (owner[0] <= t < owner[1] and t % 4 == 0): break
                cases.add(t); c += 1
        if len(cases) >= 2: tables[table] = (base + i * 4, N, cases)

    def parent(va):
        for f in fs:
            if f[0] <= va < f[1]: return f[0]
        return None
    force = set(int(x, 16) for x in re.findall(r'"0x([0-9A-Fa-f]{8})"', existing.split('force_entries')[1].split(']')[0])) if 'force_entries' in existing else set()
    mids = set((int(e, 16), int(p, 16)) for e, p in re.findall(r'entry\s*=\s*"0x([0-9A-Fa-f]+)"\s*parent\s*=\s*"0x([0-9A-Fa-f]+)"', existing))
    for table, (jr, N, cases) in sorted(tables.items()):
        for c in cases:
            p = parent(c)
            if p and p != c: mids.add((c, p))
            elif not p: force.add(c)
    head = existing.split('[recompile]')[0] if '[recompile]' in existing else existing
    # keep every other key of [recompile] (exclude_ranges, force_entries_cross_mid, ...)
    body = existing.split('[recompile]', 1)[1] if '[recompile]' in existing else ''
    body = body.split('[[recompile.force_mid_entries]]', 1)[0]
    body = re.sub(r'force_entries\s*=\s*\[[^\]]*\]\s*', '', body).strip()
    others = (body + '\n') if body else ''
    out = head + '[recompile]\n' + others + 'force_entries = [' + ', '.join(f'"0x{v:08X}"' for v in sorted(force)) + ']\n'
    for e, p in sorted(mids): out += f'\n[[recompile.force_mid_entries]]\nentry  = "0x{e:08X}"\nparent = "0x{p:08X}"\n'
    sys.stdout.write(out)
    print(f'# jump tables: {len(tables)}, cases: {sum(len(c) for _,_,c in tables.values())}, force={len(force)} mid={len(mids)}', file=sys.stderr)

if __name__ == '__main__':
    main()
