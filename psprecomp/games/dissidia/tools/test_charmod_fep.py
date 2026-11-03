#!/usr/bin/env python3
"""Self-test for charmod_fep.py (own effect-package ids for extra characters) on Cecil's files from the game:
a Cecil clone's main + assist packages move to a free low byte together (assist = main + 2048), sizes, effect names
and effect records stay the same, a second run changes nothing, and two clones of one donor get different ids.
Needs the game set up (work/disc0); skipped without it.
Run: python3 -I test_charmod_fep.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import charmod_objx as CO  # noqa: E402
import charmod_fep as FX  # noqa: E402
import package_extract as PE  # noqa: E402

DISC = os.path.join(HERE, '..', '..', '..', 'work', 'disc0')


def effe(objx):
    fep = FX._fep()
    m = fep.Mpk.from_bytes([b for _, t, b in CO.arc_read(objx)[1] if t == b'effe'][0])
    return [n for _, n in fep.Fep.from_bytes(m.get(0x40)).names()], m.get(0x4D)


def main():
    if not os.path.isdir(DISC):
        print('skipped: no game files in', DISC)
        return 0
    pkg = PE.Package(DISC)
    retail = FX.retail_ids(DISC)
    donor = {'obj/p_tst100.objx': pkg.read('obj/p_for100.objx'), 'obj/p_tst100_a.objx': pkg.read('obj/p_for100_a.objx'),
             'obj/p_tst100_w.objx': pkg.read('obj/p_for100_w.objx')}
    old = {g: FX.package_id(d) for g, d in donor.items()}
    assert old['obj/p_tst100.objx'] in retail and old['obj/p_tst100_w.objx'] is None, old

    a = dict(donor)
    done = FX.give_own_ids([('a', a)], retail, log=lambda *x: None)['a']
    main_new, ast_new = done['obj/p_tst100.objx'][1], done['obj/p_tst100_a.objx'][1]
    assert ast_new == main_new + FX.ASSIST, done
    assert main_new not in retail and (main_new & 0xFF) not in {x & 0xFF for x in retail}, main_new
    assert main_new not in FX.RESERVED and (main_new & 0xFF) not in {x & 0xFF for x in FX.RESERVED}, main_new
    assert a['obj/p_tst100_w.objx'] == donor['obj/p_tst100_w.objx']
    for g in ('obj/p_tst100.objx', 'obj/p_tst100_a.objx'):
        assert len(a[g]) == len(donor[g]) and a[g] != donor[g], g
        assert FX.package_id(a[g]) == done[g][1]
        assert effe(a[g]) == effe(donor[g]), g          # same effect names, same FEPA records

    again = dict(a)
    assert FX.give_own_ids([('a', again)], retail, log=lambda *x: None) == {} and again == a

    x, y = dict(donor), dict(donor)
    two = FX.give_own_ids([('x', x), ('y', y)], retail, log=lambda *x: None)
    assert two['x']['obj/p_tst100.objx'][1] != two['y']['obj/p_tst100.objx'][1], two
    assert x['obj/p_tst100.objx'] == a['obj/p_tst100.objx']   # the first character in slot order: same ids every build
    print('ok: Cecil clone %d -> %d, assist %d -> %d' % (old['obj/p_tst100.objx'], main_new,
                                                          old['obj/p_tst100_a.objx'], ast_new))
    return 0


if __name__ == '__main__':
    sys.exit(main())
