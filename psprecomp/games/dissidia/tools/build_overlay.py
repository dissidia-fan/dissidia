#!/usr/bin/env python3
"""Recompile a Dissidia 012 overlay (OVL_*_APP.ELF) as a separately named
function set that the runtime's overlay resolver can switch between.

Dissidia loads one of three flat overlay blobs at the fixed address
0x089DD4C0 (battle=1, menu=2, extra=3 — the id is the blob's first word).
The game copies the blob into guest memory itself, so only the *code* needs
recompiling; its data is already where the code expects it.

Usage: build_overlay.py <id> <prefix> <analysis.json> <eboot_analysis.json> <out_dir> [recompile.toml]
  e.g. build_overlay.py 2 ovl_menu analysis_menu.json analysis.json overlays/menu

Steps: graft the EBOOT's imports/module into the overlay analysis (recompile
refuses an import-free analysis), run `psprecomp recompile`, then rename every
symbol whose address lies in the overlay window with the given prefix and turn
dispatch.cpp into `<prefix>_lookup()` / `<prefix>_init_dispatch_table()`.
"""
import json, os, re, subprocess, sys, shutil

OVL_LO, OVL_HI = 0x089DD4C0, 0x08B63170
PSPRECOMP = os.environ.get('PSPRECOMP_BIN') or os.path.join(os.path.dirname(__file__), '..', '..', '..', 'target', 'release', 'psprecomp')

def main():
    ovl_id, prefix, analysis, eboot_analysis, out = sys.argv[1:6]
    config = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6] != '--module' else None
    # --module: a relocatable PRX the runtime must load itself (keep its data
    # sections and import-stub table, both prefixed) rather than an overlay
    # whose bytes the game copies into memory.
    module = '--module' in sys.argv
    a = json.load(open(analysis)); e = json.load(open(eboot_analysis))
    if not a.get('imports'):
        a['imports'] = e['imports']; a['module'] = e['module']
        grafted = analysis + '.grafted.json'
        json.dump(a, open(grafted, 'w')); analysis = grafted
    env = dict(os.environ, PSPRECOMP_CROSS_MID='1')
    cmd = [PSPRECOMP, 'recompile', analysis, '-o', out] + (['--config', config] if config else [])
    subprocess.run(cmd, check=True, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def in_window(hexstr):
        v = int(hexstr, 16); return OVL_LO <= v < OVL_HI
    sym = re.compile(r'\b(FUN_([0-9a-fA-F]{8})|func_([0-9A-Fa-f]{8})_entry)\b')
    def ren(m):
        # Prefix every symbol: an overlay only references its own functions
        # directly (EBOOT calls are emitted as RECOMP_LOOKUP), and gap-fill
        # can mint junk functions outside the window that would otherwise
        # clash across overlays and with the EBOOT set.
        return prefix + '_' + m.group(1)

    files = [os.path.join(out, 'generated', f) for f in os.listdir(os.path.join(out, 'generated')) if f.endswith('.cpp')]
    files += [os.path.join(out, 'funcs.h'), os.path.join(out, 'mid_entries.cpp')]
    for f in files:
        s = open(f).read(); open(f, 'w').write(sym.sub(ren, s))

    d = os.path.join(out, 'dispatch.cpp'); s = sym.sub(ren, open(d).read())
    s = s.replace('void psp_init_dispatch_table()', f'void {prefix}_init_dispatch_table()')
    s = re.sub(r'FuncPtr RECOMP_LOOKUP\(uint32_t vaddr\) \{.*?\n\}\n', f'''FuncPtr {prefix}_lookup(uint32_t vaddr) {{
    auto it = s_dispatch.find(vaddr);
    if (it == s_dispatch.end()) return nullptr;
    return it->second;
}}
''', s, flags=re.S)
    # drop the duplicate runtime-facing helpers (the EBOOT's dispatch.cpp owns them)
    s = re.sub(r'void psp_dispatch_register\(.*?\n\}\n', '', s, flags=re.S)
    s = re.sub(r'FuncPtr psp_dispatch_probe_lookup\(.*?\n\}\n', '', s, flags=re.S)
    s = re.sub(r'\n\s*s_dispatch\[0x00000000U\] = noop_func;', '', s)
    s = s.replace('static void noop_func(uint8_t*, recomp_context*) {}\n', '')
    open(d, 'w').write(s)
    # The game's own entry symbol must not be duplicated by an overlay.
    for f in files + [d]:
        s = open(f).read()
        s = re.sub(r'\bentry\b(?=\s*[(;])', f'{prefix}_entry', s)
        open(f, 'w').write(s)
    if module:
        ds = os.path.join(out, 'data_sections.cpp'); t = sym.sub(ren, open(ds).read())
        t = t.replace('void psp_init_data_sections(', f'void {prefix}_init_data_sections(')
        open(ds, 'w').write(t)
        st = os.path.join(out, 'syscall_table.cpp'); t = open(st).read()
        t = t.replace('recomp_nid_stubs', f'{prefix}_nid_stubs').replace('recomp_nid_stub_count', f'{prefix}_nid_stub_count')
        open(st, 'w').write(t)
    open(os.path.join(out, 'overlay.txt'), 'w').write(f'{ovl_id} {prefix}\n')
    n = len(re.findall(r's_dispatch\[0x', open(d).read()))
    print(f'{prefix}: id={ovl_id} functions={n} -> {out}')

if __name__ == '__main__':
    main()
