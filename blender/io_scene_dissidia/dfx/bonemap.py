"""Bone-name mapping between skeletons from different games / tools (pure python, no Blender).

Every humanoid is described by canonical slots:

  hips spine1 spine2 spine3 neck head                      (centre line)
  {l,r}_clavicle {l,r}_upperarm {l,r}_forearm {l,r}_hand   (arms)
  {l,r}_thigh {l,r}_shin {l,r}_foot {l,r}_toe              (legs)

Each naming family (Luminous/FFXV, Mixamo + Unity humanoid, Rigify, Unreal mannequin, 3ds Max Biped,
Bartz's own game bones) lists which normalised bone names fill a slot.  `auto_map(src, dst)` resolves both
skeletons into slots, pairs them up (reducing or skipping spine bones when the counts differ) and adds every
bone that has exactly the same name in both as an "extra" pair.  The result is a `BoneMap` that is saved as
`<file>.bonemap.json` so it can be edited by hand.

Name normalisation: a bone name is split into lower-case tokens (CamelCase, `_`, `.`, spaces, digits), tool
prefixes (`mixamorig:`, `Armature|`, `Bip001`, `DEF-`) and centre markers (`C_`) are dropped, the side
(`L_`, `Left`, `.R`, ` R `...) is taken out, and what is left is joined: `C_Spine1` -> spine1,
`mixamorig:LeftForeArm` -> (L, forearm), `upper_arm.R` -> (R, upperarm).
"""
import json
import re

SPINE_SLOTS = ('spine1', 'spine2', 'spine3')
CENTRE_SLOTS = ('hips',) + SPINE_SLOTS + ('neck', 'head')
LIMB_SLOTS = ('clavicle', 'upperarm', 'forearm', 'hand', 'thigh', 'shin', 'foot', 'toe')
SLOTS = CENTRE_SLOTS + tuple(f'{s}_{k}' for s in ('l', 'r') for k in LIMB_SLOTS)

# Slots that must be filled for a usable retarget (the rest only refine it).
REQUIRED = ('hips', 'spine1', 'head') + tuple(f'{s}_{k}' for s in ('l', 'r')
                                              for k in ('upperarm', 'forearm', 'hand', 'thigh', 'shin', 'foot'))

# Chains used for limb directions (rest alignment) and for the hierarchy fallback.
CHAINS = (
    ('hips', 'spine1', 'spine2', 'spine3', 'neck', 'head'),
    ('l_clavicle', 'l_upperarm', 'l_forearm', 'l_hand'),
    ('r_clavicle', 'r_upperarm', 'r_forearm', 'r_hand'),
    ('l_thigh', 'l_shin', 'l_foot', 'l_toe'),
    ('r_thigh', 'r_shin', 'r_foot', 'r_toe'),
)

# Normalised names per family, best candidate first.  A bone is claimed by the first slot that wants it
# (slots are resolved in SLOTS order, 'foot' before 'toe'), so "ankle, foot" works for rigs where
# "Foot" is the ball of the foot.
FAMILIES = {
    # Final Fantasy XV (Luminous Studio): C_Hip, C_Spine1..3, C_Neck1, C_Head, L_Shoulder, L_UpperArm, L_Forearm,
    # L_Hand, L_UpperLeg, L_Foreleg (shin), L_Foot, L_Toe.  The game's nh00 skeleton also has L_Knee (a knee
    # helper parented to L_UpperLeg, beside L_Foreleg) and L_ankle (a helper under L_Foreleg, beside L_Foot):
    # they only turn part of the way, so they must not take the shin/foot slots (it made the lower legs flail).
    # Several spellings are accepted because rips differ.
    'luminous': {
        'hips': ['hip', 'hips', 'pelvis'],
        'spine1': ['spine1', 'spine'], 'spine2': ['spine2'], 'spine3': ['spine3'],
        'neck': ['neck1', 'neck'], 'head': ['head'],
        'clavicle': ['shoulder', 'clavicle', 'collar'],
        'upperarm': ['upperarm', 'uparm', 'arm'],
        'forearm': ['forearm', 'lowerarm', 'elbow'],
        'hand': ['hand', 'wrist'],
        'thigh': ['thigh', 'upperleg', 'upleg', 'femur'],
        'shin': ['foreleg', 'shin', 'lowerleg', 'calf', 'tibia', 'knee', 'leg'],
        'foot': ['foot', 'ankle'],
        'toe': ['toe', 'toes', 'toebase', 'ball', 'toe1', 'foot'],
    },
    # Mixamo (Hips, Spine, Spine1, Spine2, LeftShoulder, LeftArm, LeftForeArm, LeftHand, LeftUpLeg, LeftLeg,
    # LeftFoot, LeftToeBase) and the Unity humanoid names (Spine, Chest, UpperChest, LeftShoulder, LeftUpperArm,
    # LeftLowerArm, LeftHand, LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes).
    'mixamo': {
        'hips': ['hips', 'pelvis'],
        'spine1': ['spine'], 'spine2': ['spine1', 'chest'], 'spine3': ['spine2', 'upperchest'],
        'neck': ['neck'], 'head': ['head'],
        'clavicle': ['shoulder', 'clavicle'],
        'upperarm': ['arm', 'upperarm'],
        'forearm': ['forearm', 'lowerarm'],
        'hand': ['hand'],
        'thigh': ['upleg', 'upperleg', 'thigh'],
        'shin': ['leg', 'lowerleg', 'calf'],
        'foot': ['foot'],
        'toe': ['toebase', 'toes', 'toe'],
    },
    # Blender Rigify (metarig and DEF- bones): spine = pelvis, spine.001-.003 body, .004 neck, .006 head.
    'rigify': {
        'hips': ['spine', 'pelvis'],
        'spine1': ['spine1'], 'spine2': ['spine2'], 'spine3': ['spine3'],
        'neck': ['spine4', 'neck'], 'head': ['spine6', 'head'],
        'clavicle': ['shoulder'],
        'upperarm': ['upperarm'],
        'forearm': ['forearm'],
        'hand': ['hand'],
        'thigh': ['thigh'],
        'shin': ['shin'],
        'foot': ['foot'],
        'toe': ['toe'],
    },
    # Unreal mannequin (pelvis, spine_01.., neck_01, clavicle_l, upperarm_l, lowerarm_l, thigh_l, calf_l, ball_l).
    'unreal': {
        'hips': ['pelvis'],
        'spine1': ['spine1'], 'spine2': ['spine2'], 'spine3': ['spine3'],
        'neck': ['neck1', 'neck'], 'head': ['head'],
        'clavicle': ['clavicle'],
        'upperarm': ['upperarm'], 'forearm': ['lowerarm'], 'hand': ['hand'],
        'thigh': ['thigh'], 'shin': ['calf'], 'foot': ['foot'], 'toe': ['ball'],
    },
    # 3ds Max Biped / CAT (Bip001 Pelvis, Spine, Spine1, Neck, Head, L Clavicle, L UpperArm, L Forearm, L Hand,
    # L Thigh, L Calf, L Foot, L Toe0).
    'biped': {
        'hips': ['pelvis'],
        'spine1': ['spine'], 'spine2': ['spine1'], 'spine3': ['spine2'],
        'neck': ['neck'], 'head': ['head'],
        'clavicle': ['clavicle'],
        'upperarm': ['upperarm'], 'forearm': ['forearm'], 'hand': ['hand'],
        'thigh': ['thigh'], 'shin': ['calf'], 'foot': ['foot'], 'toe': ['toe0', 'toe'],
    },
    # Bartz / Dissidia 012 game bones.  `root` carries the body's translation, `hip` the pelvis, `waist` and
    # `chest` are the only spine bones.
    'bartz': {
        'hips': ['hip'],
        'spine1': ['waist'], 'spine2': ['chest'], 'spine3': [],
        'neck': ['neck'], 'head': ['head'],
        'clavicle': ['clavicle'],
        'upperarm': ['uparm'], 'forearm': ['foarm'], 'hand': ['hand'],
        'thigh': ['femur'], 'shin': ['tibia'], 'foot': ['foot'], 'toe': ['toe'],
    },
}
FAMILY_ORDER = ('bartz', 'mixamo', 'luminous', 'rigify', 'unreal', 'biped')

_PREFIX_TOKENS = {'mixamorig', 'armature', 'def', 'org', 'mch', 'tweak', 'jnt', 'bn', 'cc', 'base', 'skel'}
_CENTRE_TOKENS = {'c', 'center', 'centre', 'mid', 'm'}
_LEFT = {'l', 'left', 'lt'}
_RIGHT = {'r', 'right', 'rt'}


def tokens(name):
    """Lower-case tokens of a bone name, without namespaces."""
    name = re.split(r'[|:]', name)[-1]
    s = re.sub(r'([a-z])([A-Z])', r'\1 \2', name)
    s = re.sub(r'([A-Za-z])(\d)', r'\1 \2', s)
    s = re.sub(r'(\d)([A-Za-z])', r'\1 \2', s)
    toks = [t.lower() for t in re.split(r'[^A-Za-z0-9]+', s) if t]
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == 'bip' and i + 1 < len(toks) and toks[i + 1].isdigit():     # Bip001 / Bip01
            i += 2
            continue
        out.append(str(int(t)) if t.isdigit() else t)
        i += 1
    return out


def split_name(name):
    """-> (side, core): side is 'l', 'r' or None; core is the remaining tokens joined."""
    toks = tokens(name)
    side = None
    core = []
    for k, t in enumerate(toks):
        if t in _PREFIX_TOKENS:
            continue
        if t in _CENTRE_TOKENS and k == 0 and len(toks) > 1:
            continue
        if t in _LEFT and side is None:
            side = 'l'
        elif t in _RIGHT and side is None:
            side = 'r'
        else:
            core.append(t)
    return side, ''.join(core)


def resolve(names, family):
    """Fill slots from one family's table.  -> {slot: bone name}.  A bone fills at most one slot."""
    table = FAMILIES[family]
    keyed = {}
    for n in names:
        side, core = split_name(n)
        if core:
            keyed.setdefault((side, core), n)       # first bone wins on duplicates
    used, out = set(), {}
    for slot in SLOTS:
        if '_' in slot and slot[:2] in ('l_', 'r_'):
            side, base = slot[0], slot[2:]
        else:
            side, base = None, slot
        for core in table.get(base, []):
            n = keyed.get((side, core))
            if n is not None and n not in used:
                out[slot] = n
                used.add(n)
                break
    return out


def _by_hierarchy(found, names, parents):
    """Fill gaps in limb / spine chains from the bone hierarchy (names -> parent index list)."""
    if parents is None:
        return found
    idx = {n: i for i, n in enumerate(names)}
    used = set(found.values())

    def par(n):
        p = parents[idx[n]]
        return names[p] if p >= 0 else None

    if 'head' in found and 'hips' in found and 'spine1' not in found:
        chain, n = [], par(found['head'])
        while n is not None and n != found['hips'] and len(chain) < 12:
            chain.append(n)
            n = par(n)
        if n == found['hips']:
            chain = [c for c in chain if c not in used]          # head -> hips order
            if 'neck' not in found and len(chain) > 3:
                found['neck'] = chain.pop(0)
            body = chain[::-1][:3] if len(chain) <= 3 else [chain[::-1][0], chain[::-1][len(chain) // 2], chain[0]]
            for slot, n in zip(SPINE_SLOTS, body):
                found[slot] = n
    used = set(found.values())
    for s in ('l', 'r'):
        for hi, lo in (('hand', 'forearm'), ('forearm', 'upperarm'), ('foot', 'shin'), ('shin', 'thigh')):
            a, b = f'{s}_{hi}', f'{s}_{lo}'
            if a in found and b not in found:
                p = par(found[a])
                if p is not None and p not in used and p not in found.values():
                    found[b] = p
                    used.add(p)
        if f'{s}_upperarm' in found and f'{s}_clavicle' not in found:
            p = par(found[f'{s}_upperarm'])
            if p is not None and p not in used and p != found.get('hips') and not any(
                    p == found.get(k) for k in CENTRE_SLOTS):
                found[f'{s}_clavicle'] = p
                used.add(p)
    return found


def resolve_best(names, parents=None):
    """Pick the family that explains most bones and fill remaining gaps from the others.
    -> (slots, family, filled_from_other_families)"""
    results = {f: resolve(names, f) for f in FAMILY_ORDER}
    ranked = sorted(FAMILY_ORDER, key=lambda f: (-len(results[f]), FAMILY_ORDER.index(f)))
    best = ranked[0]
    found = dict(results[best])
    used = set(found.values())
    mixed = []
    for f in ranked[1:]:
        for slot, n in results[f].items():
            if slot not in found and n not in used:
                found[slot] = n
                used.add(n)
                mixed.append(slot)
    n0 = len(found)
    found = _by_hierarchy(found, list(names), parents)
    mixed += [s for s in found if s not in results[best] and s not in mixed]
    return found, best, mixed


def _reduce_spine(src, dst):
    """Pair the spine slots when the skeletons have different numbers of them.  `src`/`dst` are slot->bone.
    -> {dst_slot: src bone}"""
    ss = [src[s] for s in SPINE_SLOTS if s in src]
    ds = [s for s in SPINE_SLOTS if s in dst]
    out = {}
    if not ss or not ds:
        return out
    m, k = len(ss), len(ds)
    if k >= m:
        for j in range(m):
            out[ds[j]] = ss[j]
    elif k == 1:
        out[ds[0]] = ss[-1]
    else:
        for j in range(k):
            out[ds[j]] = ss[round(j * (m - 1) / (k - 1))]
    return out


class BoneMap:
    """slots: {slot: (src bone, dst bone)}; extras: [(src, dst)] with no limb direction handling."""

    def __init__(self, slots=None, extras=None, family_src='', family_dst='', confidence=0.0, notes=None):
        self.slots = dict(slots or {})
        self.extras = [tuple(e) for e in (extras or [])]
        self.family_src = family_src
        self.family_dst = family_dst
        self.confidence = confidence
        self.notes = list(notes or [])

    @property
    def unmapped(self):
        return [s for s in SLOTS if s not in self.slots]

    @property
    def missing_required(self):
        return [s for s in REQUIRED if s not in self.slots]

    def pairs(self):
        """[(slot or None, src, dst)] in slot order, then extras (a bone is only used once on each side)."""
        out, su, du = [], set(), set()
        for s in SLOTS:
            if s in self.slots:
                a, b = self.slots[s]
                if a in su or b in du:
                    continue
                out.append((s, a, b)); su.add(a); du.add(b)
        for a, b in self.extras:
            if a in su or b in du:
                continue
            out.append((None, a, b)); su.add(a); du.add(b)
        return out

    def describe(self):
        lines = [f'Source naming: {self.family_src or "?"}   destination naming: {self.family_dst or "?"}   '
                 f'confidence {self.confidence:.0%}']
        for s in SLOTS:
            if s in self.slots:
                lines.append(f'  {s:12s} {self.slots[s][0]}  ->  {self.slots[s][1]}')
            else:
                lines.append(f'  {s:12s} (unmapped)')
        if self.extras:
            lines.append(f'  + {len(self.extras)} bones with identical names')
        lines += [f'  note: {n}' for n in self.notes]
        return '\n'.join(lines)

    def to_json(self):
        return json.dumps({
            'version': 1,
            'comment': 'slot: [source bone, destination bone]. Edit a name, or set a slot to null to leave it unmapped. '
                       '"extra" pairs copy world-space rotation with no limb alignment.',
            'family_src': self.family_src, 'family_dst': self.family_dst, 'confidence': round(self.confidence, 3),
            'slots': {s: (list(self.slots[s]) if s in self.slots else None) for s in SLOTS},
            'extra': [list(e) for e in self.extras],
        }, indent=1)

    @classmethod
    def from_json(cls, text, src_names=None, dst_names=None):
        """Read a map; names that do not exist in the given skeletons are dropped (and noted)."""
        d = json.loads(text)
        sset = set(src_names) if src_names is not None else None
        dset = set(dst_names) if dst_names is not None else None
        notes = []

        def ok(a, b, what):
            if sset is not None and a not in sset:
                notes.append(f'{what}: source bone "{a}" not found'); return False
            if dset is not None and b not in dset:
                notes.append(f'{what}: destination bone "{b}" not found'); return False
            return True
        slots = {}
        for s, v in (d.get('slots') or {}).items():
            if s in SLOTS and v and len(v) == 2 and ok(v[0], v[1], s):
                slots[s] = (v[0], v[1])
        extras = [(a, b) for a, b in (d.get('extra') or []) if ok(a, b, 'extra')]
        bm = cls(slots, extras, d.get('family_src', ''), d.get('family_dst', ''), d.get('confidence', 0.0), notes)
        return bm


def _confidence(bm, src_found, dst_found):
    req = [s for s in REQUIRED if s in bm.slots]
    opt = [s for s in SLOTS if s not in REQUIRED and s in bm.slots]
    opt_possible = [s for s in SLOTS if s not in REQUIRED and s in dst_found and s in src_found]
    c = 0.85 * len(req) / len(REQUIRED)
    c += 0.15 * (len(opt) / len(opt_possible) if opt_possible else 1.0) * (len(req) / len(REQUIRED))
    return c


def auto_map(src_names, dst_names, src_parents=None, dst_parents=None):
    """Guess the bone map from the bone names (and, where the names fail, the hierarchies)."""
    src_names, dst_names = list(src_names), list(dst_names)
    sf, fam_s, mix_s = resolve_best(src_names, src_parents)
    df, fam_d, mix_d = resolve_best(dst_names, dst_parents)
    slots = {}
    spine = _reduce_spine(sf, df)
    for s in SLOTS:
        if s in SPINE_SLOTS:
            if s in spine:
                slots[s] = (spine[s], df[s])
        elif s in sf and s in df:
            slots[s] = (sf[s], df[s])
    notes = []
    if mix_s:
        notes.append('source slots guessed from mixed naming or hierarchy: ' + ', '.join(mix_s))
    if mix_d:
        notes.append('destination slots guessed from mixed naming or hierarchy: ' + ', '.join(mix_d))
    ns = sum(1 for s in SPINE_SLOTS if s in sf)
    nd = sum(1 for s in SPINE_SLOTS if s in df)
    if ns != nd:
        notes.append(f'source has {ns} spine bones, destination {nd}: spine bones paired by height')
    claimed_s = {a for a, _ in slots.values()}
    claimed_d = {b for _, b in slots.values()}
    dset = set(dst_names)
    extras = [(n, n) for n in src_names if n in dset and n not in claimed_s and n not in claimed_d]
    bm = BoneMap(slots, extras, fam_s, fam_d, 0.0, notes)
    bm.confidence = _confidence(bm, sf, df)
    miss = bm.missing_required
    if miss:
        bm.notes.append('required slots without a match: ' + ', '.join(miss))
    return bm


def load_or_create(path, src_names, dst_names, src_parents=None, dst_parents=None):
    """Read `<file>.bonemap.json` if it exists, else guess a map and write it there.
    -> (BoneMap, created)"""
    import os
    if os.path.exists(path):
        with open(path, 'r', encoding='utf-8') as f:
            return BoneMap.from_json(f.read(), src_names, dst_names), False
    bm = auto_map(src_names, dst_names, src_parents, dst_parents)
    try:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(bm.to_json())
    except OSError as e:
        bm.notes.append(f'could not write {path}: {e}')
    return bm, True
