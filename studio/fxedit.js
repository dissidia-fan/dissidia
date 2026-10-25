// Dissidia Move Studio: effect package editing. A port of blender/io_scene_dissidia/dfx/fep.py's block graph:
// the FEP is cut into blocks at every pointer target (pointer_slots = the engine's relocation routine FUN_0894351C),
// edits change blocks or add new ones, and toBytes() lays them out again and rewrites every pointer, so an unedited
// package round-trips byte for byte. On top: FEPA / FXSE records, the MPK wrapper, and the edit operations the
// effect editor uses (keyframes, pieces, effects, textures, flipbooks, placements, copying between characters).
'use strict';
const FXE_PAD = 0xCD;
const FXE_KINDS = ['TEX', 'MDL', 'ANM', 'VTL'];
const FXE_ALIGN = { header: 16, 'hdr.t0': 16, 'hdr.x08': 8, 'names.arr': 8, 'x40.arr': 8, 'res0.data': 16, 'res1.data': 16, 'res2.data': 16, 'res3.data': 16,
  't0.leaf': 16, 'node.p32': 16, 'node.p36.it': 16 };
/* parameter fields that hold a resource-table index: {chan: [[kind, offset, bytes]]} (fep.py LINKS) */
const FXE_LINKS = { 10: [['ANM', 12, 4]], 11: [['MDL', 12, 4]], 20: [['MDL', 12, 4]], 21: [['ANM', 12, 4], ['MDL', 244, 2], ['VTL', 246, 2]],
  26: [['MDL', 14, 2], ['VTL', 16, 2]], 30: [['MDL', 14, 2], ['VTL', 16, 2]], 150: [['MDL', 14, 2], ['VTL', 16, 2]], 53: [['ANM', 16, 4]], 54: [['ANM', 160, 4]],
  55: [['ANM', 16, 4]], 56: [['ANM', 16, 4]], 57: [['ANM', 16, 4]], 63: [['TEX', 0, 4]], 78: [['MDL', 4, 2], ['VTL', 6, 2]], 80: [['ANM', 44, 4]],
  105: [['TEX', 44, 4]], 108: [['MDL', 16, 2], ['VTL', 18, 2]], 119: [['MDL', 12, 4]] };

const fxeDV = (u) => new DataView(u.buffer, u.byteOffset, u.byteLength);
const fxeU32 = (u, o) => fxeDV(u).getUint32(o, true);
const fxeU16 = (u, o) => fxeDV(u).getUint16(o, true);
const fxeSetU32 = (u, o, v) => fxeDV(u).setUint32(o, v >>> 0, true);
const fxeSetU16 = (u, o, v) => fxeDV(u).setUint16(o, v & 0xFFFF, true);
const fxeCat = (...parts) => { const n = parts.reduce((s, p) => s + p.length, 0), out = new Uint8Array(n); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out; };
const fxePadded = (u, align) => { const n = u.length + ((-u.length) % align + align) % align; if (n === u.length) return u; const out = new Uint8Array(n).fill(FXE_PAD); out.set(u); return out; };

/* ------------------------------------------------------------------ pointer map (fep.py pointer_slots) */
function fxePointerSlots(d) {
  const n = d.length, slots = new Map(), U = (o) => fxeU32(d, o), H = (o) => fxeU16(d, o);
  const P = (slot, kind, allow0) => {
    const v = U(slot);
    if (v === 0 && allow0) return 0;
    if (!(v > 0 && v <= n)) throw new Error('bad pointer 0x' + v.toString(16) + ' at 0x' + slot.toString(16) + ' (' + kind + ')');
    slots.set(slot, [v, kind]);
    return v;
  };
  let p = P(0x08, 'hdr.x08', true); if (p) P(p + 4, 'x08.arr');
  p = P(0x44, 'hdr.names', true);
  if (p) { const a = P(p + 4, 'names.arr'); for (let i = 0; i < U(p); i++) P(a + 12 * i + 8, 'name.str'); }
  p = P(0x40, 'hdr.x40', true);
  if (p) { const a = P(p + 4, 'x40.arr'); for (let i = 0; i < H(p); i++) { const q = P(a + 4 * i, 'x40.item'); P(q + 8, 'x40.item.p8'); } }
  for (let t = 0; t < 4; t++) {
    const a = P(0x30 + 4 * t, 'hdr.res' + t, true);
    if (a) for (let i = 0; i < H(0x20 + 2 * t); i++) { const r = P(a + 4 * i, 'res' + t + '.ref'); P(r + 12, 'res' + t + '.data'); }
  }
  let a = P(0x28, 'hdr.t0');
  for (let i = 0; i < H(0x1C); i++) {
    const e = P(a + 4 * i, 't0.ent'), la = P(e + 8, 't0.leafarr');
    for (let j = 0; j < H(e + 6); j++) { const lf = P(la + 4 * j, 't0.leaf'); P(lf + 12, 'leaf.p12', true); }
  }
  a = P(0x2C, 'hdr.t1');
  for (let i = 0; i < H(0x1E); i++) {
    const e = P(a + 4 * i, 't1.ent');
    let x = P(e + 28, 't1.spawn');
    for (let j = 0; j < H(e + 16); j++) { const it = P(x + 4 * j, 't1.spawn.it'); P(it + 4, 't1.spawn.nodes'); }
    x = P(e + 32, 't1.nodes');
    for (let j = 0; j < H(e + 18); j++) {
      const nd = P(x + 4 * j, 't1.node', true);
      if (!nd || d[nd + 8] === 0) continue;
      const y = P(nd + 28, 'node.tracks');
      for (let k = 0; k < d[nd + 8]; k++) { const z = P(y + 4 * k, 'node.track'); P(z + 8, 'node.track.keys'); }
      if (d[nd + 18]) { const z = P(nd + 32, 'node.p32'); P(z + 8, 'node.p32.p8'); }
      const y2 = P(nd + 36, 'node.segs');
      for (let k = 0; k < d[nd + 19]; k++) { const z = P(y2 + 4 * k, 'node.p36.it'); P(z + 8, 'node.p36.it.p8'); }
      for (let k = 0; k < 8; k++) P(nd + 40 + 4 * k, 'node.p' + (40 + 4 * k));
    }
    P(e + 36, 't1.chans');
    x = P(e + 40, 't1.p40');
    for (let j = 0; j < 12; j++) { const it = P(x + 4 * j, 't1.p40.it'); P(it + 4, 't1.p40.it.p4'); }
    x = P(e + 44, 't1.p44');
    for (let j = 0; j < H(e + 24); j++) { const it = P(x + 4 * j, 't1.p44.it'); P(it + 4, 't1.p44.it.p4'); }
    x = P(e + 48, 't1.p48');
    for (let j = 0; j < H(e + 26); j++) P(x + 4 * j, 't1.p48.it');
  }
  return slots;
}

/* ------------------------------------------------------------------ block graph */
class FxeBlock {
  constructor(data, kinds, align, pos) { this.data = Uint8Array.from(data); this.ptrs = new Map(); this.kinds = new Set(kinds || []); this.align = align || 4; this.pos = pos == null ? null : pos; }
  clone() { const b = new FxeBlock(this.data, this.kinds, this.align, null); b.ptrs = new Map(this.ptrs); return b; }
  has(...ks) { return ks.some((k) => this.kinds.has(k)); }
}
const fxeAlignFor = (kinds) => Math.max(4, ...[...kinds].map((k) => FXE_ALIGN[k] || 4));

class FxeFep {
  constructor(blocks) { this.blocks = blocks; }
  static fromBytes(u8) {
    const d = Uint8Array.from(u8);
    if (String.fromCharCode(d[0], d[1], d[2]) !== 'FEP' || d[3] !== 0xCD) throw new Error('not a FEP');
    const slots = fxePointerSlots(d), kinds = new Map();
    for (const [, [v, k]] of slots) { if (!kinds.has(v)) kinds.set(v, new Set()); kinds.get(v).add(k); }
    const starts = [...new Set([...kinds.keys(), 0])].sort((x, y) => x - y), blocks = [], byPos = new Map();
    starts.forEach((t, i) => {
      const end = i + 1 < starts.length ? starts[i + 1] : d.length, ks = kinds.get(t) || (t === 0 ? new Set(['header']) : new Set());
      const b = new FxeBlock(d.subarray(t, end), ks, fxeAlignFor(ks), t); blocks.push(b); byPos.set(t, b);
    });
    for (const [s, [v]] of slots) {
      let lo = 0, hi = starts.length - 1; while (lo < hi) { const m = (lo + hi + 1) >> 1; if (starts[m] <= s) lo = m; else hi = m - 1; }
      const b = blocks[lo]; b.ptrs.set(s - b.pos, byPos.get(v));
    }
    return new FxeFep(blocks);
  }
  toBytes() {
    let n = 0; const pos = new Map();
    for (const b of this.blocks) { n += ((-n) % b.align + b.align) % b.align; pos.set(b, n); n += b.data.length; }
    n += ((-n) % 16 + 16) % 16;
    const out = new Uint8Array(n).fill(FXE_PAD);
    for (const b of this.blocks) out.set(b.data, pos.get(b));
    for (const b of this.blocks) {
      const base = pos.get(b);
      for (const [rel, t] of b.ptrs) { if (!pos.has(t)) throw new Error('pointer to a block that is not in the file'); fxeSetU32(out, base + rel, pos.get(t)); }
      if (b.has('node.p32', 'node.p36.it') && b.ptrs.has(8) && pos.get(b.ptrs.get(8)) !== base + 12) throw new Error('ParamSet table must follow its header directly');
    }
    fxeSetU32(out, 0x14, n);
    return out;
  }
  get header() { return this.blocks[0]; }
  h16(o) { return fxeU16(this.header.data, o); }
  setH16(o, v) { fxeSetU16(this.header.data, o, v); }
  get idBase() { return fxeU32(this.header.data, 0x10); }
  get nEffects() { return this.h16(0x1C); }
  get nBodies() { return this.h16(0x1E); }
  count(kind) { return this.h16(0x20 + 2 * FXE_KINDS.indexOf(kind)); }
  table(hoff, n) { const t = this.header.ptrs.get(hoff); return t ? Array.from({ length: n }, (_, i) => t.ptrs.get(4 * i)) : []; }
  effectBlocks() { return this.table(0x28, this.nEffects); }
  bodyBlocks() { return this.table(0x2C, this.nBodies); }
  refBlocks(kind) { return this.table(0x30 + 4 * FXE_KINDS.indexOf(kind), this.count(kind)); }
  paramSize(ch) { return fxeU16(this.header.ptrs.get(0x08).ptrs.get(4).data, 8 * ch + 4); }
  names() {
    const ns = this.header.ptrs.get(0x44), arr = ns.ptrs.get(4), out = [];
    for (let i = 0; i < fxeU32(ns.data, 0); i++) { const id = fxeU32(arr.data, 12 * i), len = fxeU32(arr.data, 12 * i + 4), s = arr.ptrs.get(12 * i + 8); out.push({ id, name: String.fromCharCode(...s.data.subarray(0, len)) }); }
    return out;
  }
  localId(n) { return (0x98000000 + ((((this.idBase >>> 16) & 0x3FF) << 8) & ~0x3FF) + n) >>> 0; }
  insert(pred, nbs, before) {
    const idx = []; this.blocks.forEach((b, i) => { if (pred(b)) idx.push(i); });
    const at = idx.length ? (before ? idx[0] : idx[idx.length - 1] + 1) : this.blocks.length;
    this.blocks.splice(at, 0, ...nbs);
  }
  /* grow a pointer table block (in the header slot hoff) by one entry pointing at nb */
  growTable(hoff, nb) {
    const t = this.header.ptrs.get(hoff), n = t.ptrs.size;
    t.data = fxeCat(t.data.subarray(0, 4 * n), new Uint8Array(4)); t.ptrs.set(4 * n, nb);
    return n;
  }
}
/* pointer array helpers on a block holding u32 pointers (+ optional trailing data we drop) */
/* replace the pointer array at owner+rel with a fresh block (an empty array's pointer may aim at a block that is used
   for something else, so arrays are never rewritten in place) */
function fxeArrPut(fep, owner, rel, list) {
  const old = owner.ptrs.get(rel), nb = new FxeBlock(new Uint8Array(Math.max(4, 4 * list.length)), old ? old.kinds : [], old ? old.align : 4);
  list.forEach((b, i) => { if (b) nb.ptrs.set(4 * i, b); });
  const at = old ? fep.blocks.indexOf(old) : fep.blocks.indexOf(owner);
  fep.blocks.splice(at >= 0 ? at + 1 : fep.blocks.length, 0, nb); owner.ptrs.set(rel, nb); fep.dirty();
  return nb;
}
function fxeArrSet(blk, list) { blk.data = new Uint8Array(4 * list.length); blk.ptrs = new Map(); list.forEach((b, i) => { if (b) blk.ptrs.set(4 * i, b); }); }
function fxeArrGet(blk, n) { return Array.from({ length: n }, (_, i) => blk.ptrs.get(4 * i) || null); }

/* ------------------------------------------------------------------ records, MPK */
class FxeRecords {
  constructor(magic, rec, u8) {
    this.magic = magic; this.rec = rec; this.records = [];
    if (!u8 || !u8.length) { this.version = magic === 'FEPA' ? 0x20100205 : 0x20070422; return; }
    if (String.fromCharCode(...u8.subarray(0, 4)) !== magic) throw new Error('not ' + magic);
    this.version = fxeU32(u8, 4);
    const n = fxeU32(u8, 12);
    for (let i = 0; i < n; i++) this.records.push(Uint8Array.from(u8.subarray(16 + rec * i, 16 + rec * (i + 1))));
  }
  toBytes() {
    const out = new Uint8Array(16 + this.rec * this.records.length);
    for (let i = 0; i < 4; i++) out[i] = this.magic.charCodeAt(i);
    fxeSetU32(out, 4, this.version); fxeSetU32(out, 8, out.length); fxeSetU32(out, 12, this.records.length);
    this.records.forEach((r, i) => out.set(r, 16 + this.rec * i));
    return out;
  }
}
class FxeMpk {
  constructor(u8) {
    if (String.fromCharCode(...u8.subarray(0, 4)) !== 'MPK ') throw new Error('not an MPK');
    this.ver = Uint8Array.from(u8.subarray(4, 8));
    const n = fxeU32(u8, 8), ents = [];
    for (let i = 0; i < n; i++) { const o = 16 + 16 * i; ents.push([fxeU32(u8, o), fxeU32(u8, o + 4), fxeU32(u8, o + 8)]); }
    const firsts = ents.filter((e) => e[2]).map((e) => e[1]), first = firsts.length ? Math.min(...firsts) : u8.length;
    this.entries = ents.map((e) => [e[0], Uint8Array.from(u8.subarray(e[1], e[1] + e[2]))]);
    this.names = Uint8Array.from(u8.subarray(16 + 16 * n, first));
    const ends = ents.filter((e) => e[2]).map((e) => e[1] + e[2]);
    this.tail = Uint8Array.from(u8.subarray(ends.length ? Math.max(...ends) : first));
  }
  get(id) { const e = this.entries.find((x) => x[0] === id); return e ? e[1] : null; }
  set(id, data) { const e = this.entries.find((x) => x[0] === id); if (!e) throw new Error('no MPK entry ' + id); e[1] = data; }
  has(id) { return this.entries.some((x) => x[0] === id); }
  toBytes() {
    const n = this.entries.length, head = 16 + 16 * n + this.names.length, table = new Uint8Array(16 * n), parts = [];
    let o = head;
    this.entries.forEach(([id, data], i) => {
      fxeSetU32(table, 16 * i, id);
      if (!data || !data.length) return;
      const pad = (-o % 16 + 16) % 16; if (pad) { parts.push(new Uint8Array(pad)); o += pad; }
      fxeSetU32(table, 16 * i + 4, o); fxeSetU32(table, 16 * i + 8, data.length);
      parts.push(data); o += data.length;
    });
    const h = new Uint8Array(16); h.set([0x4D, 0x50, 0x4B, 0x20]); h.set(this.ver, 4); fxeSetU32(h, 8, n);
    return fxeCat(h, table, this.names, ...parts, this.tail);
  }
}
/* the objx 'effe' entry: MPK {0x40 FEP, 0x4D FEPA, 0x5A FXSE} */
class FxePackage {
  constructor(u8) {
    this.mpk = new FxeMpk(u8);
    const byMagic = (m) => { const e = this.mpk.entries.find((x) => x[1].length >= 4 && String.fromCharCode(...x[1].subarray(0, 4)) === m); return e ? e[0] : null; };
    this.idFep = byMagic('FEP\xcd'); this.idFepa = byMagic('FEPA'); this.idFxse = byMagic('FXSE');
    if (this.idFep == null) throw new Error('no FEP in the effect package');
    this.fep = FxeFep.fromBytes(this.mpk.get(this.idFep));
    this.fepa = new FxeRecords('FEPA', 260, this.idFepa != null ? this.mpk.get(this.idFepa) : null);
    this.fxse = new FxeRecords('FXSE', 320, this.idFxse != null ? this.mpk.get(this.idFxse) : null);
  }
  toBytes() {
    this.fep.gc();
    this.mpk.set(this.idFep, this.fep.toBytes());
    if (this.idFepa != null) this.mpk.set(this.idFepa, this.fepa.toBytes());
    if (this.idFxse != null) this.mpk.set(this.idFxse, this.fxse.toBytes());
    return this.mpk.toBytes();
  }
}

/* ================================================================== edit operations */
// Coordinates come from the renderer's parse of the last serialisation (fx.js parseFep: absolute offsets), so every
// operation starts by finding the block that holds an absolute offset.
const FXE_LISTS = { 1: [3, 5], 2: [0, 3, 5], 3: [0, 3, 5], 4: [3], 5: [0, 3], 6: [0, 3], 7: [3], 8: [0, 3], 9: [0, 3], 10: [0, 1], 11: [1], 14: [0, 5],
  15: [3], 16: [0, 3], 17: [0, 3], 18: [0, 3], 20: [1, 3], 21: [0, 1, 2, 3], 22: [], 23: [], 24: [], 26: [0, 3], 61: [0, 2, 3], 62: [0, 2, 3], 68: [0, 2, 3],
  79: [0, 2], 80: [0, 1, 2, 3], 95: [0, 1, 2, 3], 100: [0, 1, 2, 3], 101: [0, 1, 2, 3], 103: [0, 1, 2, 3], 105: [0, 1, 2, 3], 106: [0, 2, 3, 7], 107: [0, 1, 2, 3],
  109: [0, 2, 3], 110: [0, 1, 2, 3], 151: [0, 3] };
const fxeAl16 = (x) => (x + 15) & ~15;
Object.assign(FxeFep.prototype, {
  layoutIndex() {               // [pos, block] sorted, from the last toBytes
    if (!this._idx) { let n = 0; this._idx = []; for (const b of this.blocks) { n += ((-n) % b.align + b.align) % b.align; this._idx.push([n, b]); n += b.data.length; } }
    return this._idx;
  },
  blockAt(abs) {
    const idx = this.layoutIndex(); let lo = 0, hi = idx.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (idx[m][0] <= abs) lo = m; else hi = m - 1; }
    return { b: idx[lo][1], rel: abs - idx[lo][0] };
  },
  dirty() { this._idx = null; },
  stateSize(ch) { return fxeU16(this.header.ptrs.get(0x08).ptrs.get(4).data, 8 * ch + 6); },
  /* drop blocks nothing points at any more (after deletes / replacements), keeping file order */
  gc() {
    const live = new Set([this.header]), stack = [this.header];
    while (stack.length) { const b = stack.pop(); for (const t of b.ptrs.values()) if (!live.has(t)) { live.add(t); stack.push(t); } }
    this.blocks = this.blocks.filter((b) => live.has(b)); this.dirty();
  },
});
const fxeSetAt = (fep, abs, bytes) => { const { b, rel } = fep.blockAt(abs); b.data.set(bytes, rel); };
const fxeF32b = (v) => { const u = new Uint8Array(4); new DataView(u.buffer).setFloat32(0, v, true); return u; };
const fxeI32b = (v) => { const u = new Uint8Array(4); new DataView(u.buffer).setInt32(0, v, true); return u; };
const fxeU16b = (v) => { const u = new Uint8Array(2); new DataView(u.buffer).setUint16(0, v & 0xFFFF, true); return u; };
const fxeS16b = (v) => { const u = new Uint8Array(2); new DataView(u.buffer).setInt16(0, v, true); return u; };

/* ---- node views */
function fxeNodeTracks(fep, nd) {
  const arr = nd.ptrs.get(28), out = [];
  for (let k = 0; k < nd.data[8]; k++) {
    const t = arr.ptrs.get(4 * k), n = fxeU32(t.data, 4), keys = t.ptrs.get(8), deps = [];
    for (let q = 0; q < n; q++) deps.push(fxeU16(keys.data, 2 * q));
    out.push({ blk: t, ch: fxeU16(t.data, 0), deps });
  }
  return out;
}
function fxePsRead(fep, ps, tracks) {
  const n = fxeU32(ps.data, 4), tb = ps.ptrs.get(8), entries = [];
  for (let i = 0; i < n; i++) {
    const tr = fxeU16(tb.data, 8 * i), off = fxeU16(tb.data, 8 * i + 2), size = fep.paramSize(tracks[tr].ch);
    entries.push({ track: tr, start: fxeU16(tb.data, 8 * i + 4), len: fxeU16(tb.data, 8 * i + 6), bytes: Uint8Array.from(tb.data.subarray(off - 12, off - 12 + size)) });
  }
  return { frame0: fxeU16(ps.data, 0), frame1: fxeU16(ps.data, 2), entries };
}
function fxePsWrite(ps, frame0, frame1, entries) {
  entries.sort((a, b) => a.track - b.track);
  const n = entries.length, offs = []; let o = fxeAl16(12 + 8 * n);
  for (const e of entries) { offs.push(o); o = fxeAl16(o + e.bytes.length); }
  const end = n ? offs[n - 1] + entries[n - 1].bytes.length : 12 + 8 * n;
  const tb = ps.ptrs.get(8); tb.data = new Uint8Array(end - 12).fill(FXE_PAD);
  entries.forEach((e, i) => {
    fxeSetU16(tb.data, 8 * i, e.track); fxeSetU16(tb.data, 8 * i + 2, offs[i]); fxeSetU16(tb.data, 8 * i + 4, e.start); fxeSetU16(tb.data, 8 * i + 6, e.len);
    tb.data.set(e.bytes, offs[i] - 12);
  });
  fxeSetU16(ps.data, 0, frame0); fxeSetU16(ps.data, 2, frame1); fxeSetU32(ps.data, 4, n);
}
function fxeNodeSets(nd) { return fxeArrGet(nd.ptrs.get(36), nd.data[19]); }
/* a new ParamSet (header + table, adjacent) */
function fxeNewPs(fep, after) {
  const ps = new FxeBlock(new Uint8Array(12), ['node.p36.it'], 16), tb = new FxeBlock(new Uint8Array(0), ['node.p36.it.p8'], 4);
  ps.ptrs.set(8, tb);
  const i = after ? fep.blocks.indexOf(after) : -1;
  fep.blocks.splice(i >= 0 ? i + 1 : fep.blocks.length, 0, ps, tb);
  return ps;
}
/* rebuild a node's phase lists and state offsets from its tracks (rules measured on retail data) */
function fxeNodeRebuild(fep, nd) {
  const tr = fxeNodeTracks(fep, nd); let so = 0, end = 0;
  const offs = tr.map((t) => { so = fxeAl16(end); const s = fep.stateSize(t.ch); end = so + s; return s ? so : fxeAl16(end - s); });
  tr.forEach((t, k) => {
    fxeSetU16(t.blk.data, 2, offs[k]);
    const keys = t.blk.ptrs.get(8), n = t.deps.length;
    if (!n) return;                                    // no links: the pointer aims at whatever follows (shared), leave it
    const nk = new FxeBlock(new Uint8Array(4 * n), keys.kinds, keys.align); t.deps.forEach((d, q) => { fxeSetU16(nk.data, 2 * q, d); fxeSetU16(nk.data, 2 * (n + q), offs[d] || 0); });
    t.blk.ptrs.set(8, nk); fep.blocks.splice(fep.blocks.indexOf(t.blk) + 1, 0, nk);
    fxeSetU32(t.blk.data, 4, n);
  });
  fxeSetU16(nd.data, 2, end);
  for (let L = 0; L < 8; L++) {
    const idx = tr.map((t, k) => ((FXE_LISTS[t.ch] || [0, 3]).includes(L) ? k : -1)).filter((k) => k >= 0);
    // a fresh block: an empty list's pointer aims at whatever follows it in the file, which must not be rewritten
    const lb = new FxeBlock(new Uint8Array(Math.max(4, 2 * idx.length)), ['node.p' + (40 + 4 * L)], 4);
    idx.forEach((k, q) => fxeSetU16(lb.data, 2 * q, k));
    nd.ptrs.set(40 + 4 * L, lb); fep.blocks.splice(fep.blocks.indexOf(nd) + 1, 0, lb);
    nd.data[20 + L] = idx.length;
  }
}
/* keep the body's channel list a superset of its nodes' channels (FUN_08945BEC sizes instance state from it) */
function fxeBodyChans(fep, body) {
  const cb = body.ptrs.get(36), n = fxeU16(body.data, 20), have = []; for (let q = 0; q < n; q++) have.push(fxeU16(cb.data, 2 * q));
  const arr = body.ptrs.get(32);
  for (let j = 0; j < fxeU16(body.data, 18); j++) { const nd = arr.ptrs.get(4 * j); if (!nd || !nd.data[8]) continue; for (const t of fxeNodeTracks(fep, nd)) if (!have.includes(t.ch)) have.push(t.ch); }
  const nb = new FxeBlock(new Uint8Array(Math.max(4, 2 * have.length)), cb.kinds, cb.align); have.forEach((c, q) => fxeSetU16(nb.data, 2 * q, c));
  body.ptrs.set(36, nb); fep.blocks.splice(fep.blocks.indexOf(cb) + 1, 0, nb); fxeSetU16(body.data, 20, have.length);
}

/* ---- keys */
/* the parameter bytes a track has at frame f (the set that applies there) */
function fxeTrackBytesAt(fep, nd, ti, f) {
  const tracks = fxeNodeTracks(fep, nd), sets = [];
  if (nd.data[18]) sets.push(nd.ptrs.get(32));
  sets.push(...fxeNodeSets(nd));
  let best = null;
  for (const ps of sets) { const r = fxePsRead(fep, ps, tracks); if (r.frame0 > f && ps !== nd.ptrs.get(32)) continue; const e = r.entries.find((x) => x.track === ti); if (e) best = { e, frame0: ps === nd.ptrs.get(32) ? 0 : r.frame0 }; }
  return best;
}
/* add (or replace) the key of track ti at frame f. bytes = the parameter block; null = continue the curve there */
function fxeAddKey(fep, nd, ti, f, bytes) {
  const tracks = fxeNodeTracks(fep, nd), ch = tracks[ti].ch;
  if (!bytes) {
    const cur = fxeTrackBytesAt(fep, nd, ti, f); if (!cur) throw new Error('track has no keys');
    bytes = Uint8Array.from(cur.e.bytes);
    const isVal = (ch >= 1 && ch <= 9) || (ch >= 15 && ch <= 17);
    if (isVal) {                                     // continue: value and rate at f become the new key's
      const col = ch >= 15, q = col ? ch - 15 : (ch - 1) % 3, n = col ? 4 : 3, dv = fxeDV(bytes), tau = f - cur.e.start;
      for (let k = 0; k < n; k++) {
        const b = dv.getFloat32(4 * k, true), r = q >= 1 ? dv.getFloat32(16 + 4 * k, true) : 0, a = q === 2 ? dv.getFloat32(32 + 4 * k, true) : 0;
        dv.setFloat32(4 * k, b + r * tau + a * tau * tau / 2, true); if (q >= 1) dv.setFloat32(16 + 4 * k, r + a * tau, true);
      }
    }
  }
  const sets = fxeNodeSets(nd);
  let ps = sets.find((s) => fxeU16(s.data, 0) === f);
  if (!ps) {
    ps = fxeNewPs(fep, sets.length ? sets[sets.length - 1].ptrs.get(8) : nd);
    const all = [...sets, ps].sort((a, b) => (a === ps ? f : fxeU16(a.data, 0)) - (b === ps ? f : fxeU16(b.data, 0)));
    fxeArrPut(fep, nd, 36, all); nd.data[19] = all.length;
    fxePsWrite(ps, f, f, []);
  }
  const r = fxePsRead(fep, ps, tracks);
  r.entries = r.entries.filter((e) => e.track !== ti);
  r.entries.push({ track: ti, start: f, len: 0, bytes });
  fxePsWrite(ps, f, r.frame1, r.entries);
  if (f === 0 && nd.data[18]) {                      // the init set mirrors frame-0 keys
    const init = nd.ptrs.get(32), ri = fxePsRead(fep, init, tracks);
    if (ri.entries.some((e) => e.track === ti)) { ri.entries = ri.entries.map((e) => (e.track === ti ? { ...e, bytes: Uint8Array.from(bytes) } : e)); fxePsWrite(init, ri.frame0, ri.frame1, ri.entries); }
  }
  fxeSegLens(fep, nd);
  fep.dirty();
}
function fxeDeleteKey(fep, nd, ti, f) {
  const tracks = fxeNodeTracks(fep, nd), sets = fxeNodeSets(nd);
  const ps = sets.find((s) => fxeU16(s.data, 0) === f); if (!ps) return false;
  const r = fxePsRead(fep, ps, tracks);
  const left = r.entries.filter((e) => e.track !== ti);
  const others = sets.filter((s) => s !== ps && fxePsRead(fep, s, tracks).entries.some((e) => e.track === ti));
  if (!others.length && !(nd.data[18] && fxePsRead(fep, nd.ptrs.get(32), tracks).entries.some((e) => e.track === ti))) throw new Error('the last key of a track cannot be deleted');
  if (left.length) fxePsWrite(ps, r.frame0, r.frame1, left);
  else { const all = sets.filter((s) => s !== ps); fxeArrPut(fep, nd, 36, all); nd.data[19] = all.length; }
  fxeSegLens(fep, nd); fep.gc();
  return true;
}
/* entry lengths = frames to the track's next key (the engine never reads them; retail keeps them consistent) */
function fxeSegLens(fep, nd) {
  const tracks = fxeNodeTracks(fep, nd), sets = fxeNodeSets(nd).map((ps) => ({ ps, r: fxePsRead(fep, ps, tracks) }));
  const life = fxeU16(nd.data, 4);
  for (const s of sets) {
    for (const e of s.r.entries) { const nxt = sets.map((x) => x.r).filter((x) => x.frame0 > s.r.frame0 && x.entries.some((y) => y.track === e.track)).map((x) => x.frame0); e.len = Math.max(0, (nxt.length ? Math.min(...nxt) : life) - s.r.frame0); }
    const f1 = s.r.entries.length ? Math.min(...s.r.entries.map((e) => e.len)) : 0;
    fxePsWrite(s.ps, s.r.frame0, s.r.frame0 + f1, s.r.entries);
  }
}
/* value channels can be upgraded: constant (1/4/7/15) -> linear (2/5/8/16) -> quadratic (3/6/9/17), values kept */
function fxeSetChannel(fep, body, nd, ti, ch) {
  const tracks = fxeNodeTracks(fep, nd), old = tracks[ti].ch, size = fep.paramSize(ch), n = ch >= 15 ? 4 : 3;
  const conv = (bytes) => { const out = new Uint8Array(size).fill(0), od = fxeDV(bytes), ndv = fxeDV(out), oq = old >= 15 ? old - 15 : (old - 1) % 3, nq = ch >= 15 ? ch - 15 : (ch - 1) % 3;
    for (let k = 0; k < n; k++) { ndv.setFloat32(4 * k, od.getFloat32(4 * k, true), true); if (nq >= 1) ndv.setFloat32(16 + 4 * k, oq >= 1 ? od.getFloat32(16 + 4 * k, true) : 0, true); if (nq === 2) ndv.setFloat32(32 + 4 * k, oq === 2 ? od.getFloat32(32 + 4 * k, true) : 0, true); }
    if (n === 3) for (const o of [12, 28, 44]) if (o + 4 <= size) ndv.setUint32(o, 0xCDCDCDCD, true);
    return out; };
  const sets = [...(nd.data[18] ? [nd.ptrs.get(32)] : []), ...fxeNodeSets(nd)];
  for (const ps of sets) { const r = fxePsRead(fep, ps, tracks); r.entries = r.entries.map((e) => (e.track === ti ? { ...e, bytes: conv(e.bytes) } : e)); fxePsWrite(ps, r.frame0, r.frame1, r.entries); }
  fxeSetU16(tracks[ti].blk.data, 0, ch);
  fxeNodeRebuild(fep, nd); fxeBodyChans(fep, body); fep.gc();
}

/* ---- pieces (nodes) */
function fxeBodyNodes(body) { return fxeArrGet(body.ptrs.get(32), fxeU16(body.data, 18)); }
function fxeSpawns(body) {
  return fxeArrGet(body.ptrs.get(28), fxeU16(body.data, 16)).map((it) => { const n = fxeU16(it.data, 2), lb = it.ptrs.get(4); return { it, frame: fxeU16(it.data, 0), nodes: Array.from({ length: n }, (_, k) => fxeU16(lb.data, 2 * k)) }; });
}
function fxeSetSpawns(fep, body, list) {         // [{frame, nodes}]
  list = list.filter((s) => s.nodes.length).sort((a, b) => a.frame - b.frame);
  const items = list.map((s) => { const it = s.it || new FxeBlock(new Uint8Array(8), ['t1.spawn.it'], 4);
    const lb = new FxeBlock(new Uint8Array(Math.max(4, 2 * s.nodes.length)), ['t1.spawn.nodes'], 4); s.nodes.forEach((x, k) => fxeSetU16(lb.data, 2 * k, x));
    it.ptrs.set(4, lb); if (!s.it) fep.blocks.push(it); fep.blocks.push(lb);
    fxeSetU16(it.data, 0, s.frame); fxeSetU16(it.data, 2, s.nodes.length); return it; });
  fxeArrPut(fep, body, 28, items); fxeSetU16(body.data, 16, items.length);
  fep.gc();
}
/* all blocks of one node, cloned with their internal pointers remapped */
function fxeCloneNode(nd) {
  const seen = new Map(), order = [];
  const walk = (b) => { if (!b || seen.has(b)) return; const c = b.clone(); seen.set(b, c); order.push(b); for (const t of b.ptrs.values()) walk(t); };
  walk(nd);
  for (const c of seen.values()) c.ptrs = new Map([...c.ptrs].map(([r, t]) => [r, seen.get(t) || t]));
  return { root: seen.get(nd), blocks: order.map((b) => seen.get(b)) };
}
function fxeAddNode(fep, body, cloneOf, spawnFrame, extraBlocks) {
  const { root, blocks } = cloneOf.root ? cloneOf : fxeCloneNode(cloneOf);
  const nodes = fxeBodyNodes(body), idx = nodes.length;
  fxeSetU16(root.data, 6, idx);
  fxeArrPut(fep, body, 32, [...nodes, root]); fxeSetU16(body.data, 18, idx + 1);
  const at = fep.blocks.indexOf(body.ptrs.get(32));
  fep.blocks.splice(at + 1, 0, ...blocks, ...(extraBlocks || []));
  if (spawnFrame != null) { const sp = fxeSpawns(body), s = sp.find((x) => x.frame === spawnFrame); if (s) s.nodes.push(idx); else sp.push({ frame: spawnFrame, nodes: [idx] }); fxeSetSpawns(fep, body, sp); }
  fxeBodyChans(fep, body); fep.dirty();
  return idx;
}
function fxeDeleteNode(fep, body, ni) {
  const nodes = fxeBodyNodes(body); nodes[ni] = null;
  fxeArrPut(fep, body, 32, nodes);
  fxeSetSpawns(fep, body, fxeSpawns(body).map((s) => ({ ...s, nodes: s.nodes.filter((x) => x !== ni) })));
}
function fxeMoveNode(fep, body, ni, frame) {       // start a piece at another frame
  const sp = fxeSpawns(body).map((s) => ({ ...s, nodes: s.nodes.filter((x) => x !== ni) }));
  const s = sp.find((x) => x.frame === frame); if (s) s.nodes.push(ni); else sp.push({ frame, nodes: [ni] });
  fxeSetSpawns(fep, body, sp);
}

/* ---- resources */
function fxeRefs(fep, kind) { return fep.refBlocks(kind).map((r, i) => ({ i, blk: r, id: fxeU32(r.data, 4), size: fxeU32(r.data, 8), data: r.ptrs.get(12) })); }
function fxeNewLocalId(fep, kind) {
  const k = FXE_KINDS.indexOf(kind), lo = [0x00, 0x100, 0x80, 0x180][k];
  const used = new Set(); for (const kd of FXE_KINDS) for (const r of fxeRefs(fep, kd)) if ((r.id >>> 24) === 0x98) used.add(r.id & 0x3FF);
  for (let n = lo; n < 0x400; n++) if (!used.has(n)) return fep.localId(n);
  for (let n = 0; n < 0x400; n++) if (!used.has(n)) return fep.localId(n);
  throw new Error('no free resource id');
}
/* resource block id field: MDL keeps its id at +8, the others at +4 */
const fxeResIdOff = (kind) => (kind === 'MDL' ? 8 : 4);
function fxeAddRes(fep, kind, raw) {             // a new local resource; returns its table index
  const k = FXE_KINDS.indexOf(kind), id = fxeNewLocalId(fep, kind), bytes = Uint8Array.from(raw);
  fxeSetU32(bytes, fxeResIdOff(kind), id);
  const data = new FxeBlock(fxePadded(bytes, 16), ['res' + k + '.data'], 16);
  const ref = new FxeBlock(new Uint8Array(20), ['res' + k + '.ref'], 4);
  fxeDV(ref.data).setFloat32(0, 1, true); fxeSetU32(ref.data, 4, id); fxeSetU32(ref.data, 8, bytes.length); fxeDV(ref.data).setInt32(16, -1, true);
  ref.ptrs.set(12, data);
  const refs = fep.refBlocks(kind);
  if (refs.length) fep.blocks.splice(fep.blocks.indexOf(refs[refs.length - 1]) + 1, 0, ref); else fep.blocks.splice(1, 0, ref);
  const datas = fep.blocks.filter((b) => [...b.kinds].some((x) => /^res\d\.data$/.test(x)));
  fep.blocks.splice(datas.length ? fep.blocks.indexOf(datas[datas.length - 1]) + 1 : fep.blocks.length, 0, data);
  if (!fep.header.ptrs.get(0x30 + 4 * k)) { const t = new FxeBlock(new Uint8Array(0), ['hdr.res' + k], 4); fep.header.ptrs.set(0x30 + 4 * k, t); fep.blocks.splice(1, 0, t); }
  const ni = fep.growTable(0x30 + 4 * k, ref); fep.setH16(0x20 + 2 * k, ni + 1); fep.dirty();
  return ni;
}
/* a system (resident) resource used by this package: its ref with size 0 (the data pointer aims at any block) */
function fxeAddSystemRef(fep, kind, id) {
  const have = fxeRefs(fep, kind).find((r) => r.id === id); if (have) return have.i;
  const k = FXE_KINDS.indexOf(kind), ref = new FxeBlock(new Uint8Array(20), ['res' + k + '.ref'], 4);
  fxeDV(ref.data).setFloat32(0, 1, true); fxeSetU32(ref.data, 4, id); fxeDV(ref.data).setInt32(16, -1, true);
  const refs = fep.refBlocks(kind); ref.ptrs.set(12, refs.length ? refs[refs.length - 1].ptrs.get(12) : fep.blocks[1]);
  fep.blocks.splice(refs.length ? fep.blocks.indexOf(refs[refs.length - 1]) + 1 : 1, 0, ref);
  const ni = fep.growTable(0x30 + 4 * k, ref); fep.setH16(0x20 + 2 * k, ni + 1); fep.dirty();
  return ni;
}
function fxeSetResBytes(fep, kind, index, raw) {
  const r = fxeRefs(fep, kind)[index]; if (!r || !r.size) throw new Error('not a local resource');
  const bytes = Uint8Array.from(raw); fxeSetU32(bytes, fxeResIdOff(kind), r.id);
  r.data.data = fxePadded(bytes, 16); fxeSetU32(r.blk.data, 8, bytes.length); fep.dirty();
}
/* point every resource field of one body from index `from` to `to` (fep.py relink) */
function fxeRelink(fep, body, kind, from, to) {
  let n = 0;
  for (const nd of fxeBodyNodes(body)) {
    if (!nd || !nd.data[8]) continue;
    const tracks = fxeNodeTracks(fep, nd), sets = [...(nd.data[18] ? [nd.ptrs.get(32)] : []), ...fxeNodeSets(nd)];
    for (const ps of sets) {
      const r = fxePsRead(fep, ps, tracks); let ch = false;
      for (const e of r.entries) for (const [kd, fo, sz] of FXE_LINKS[tracks[e.track].ch] || []) {
        if (kd !== kind || fo + sz > e.bytes.length) continue;
        const dv = fxeDV(e.bytes), v = sz === 4 ? dv.getInt32(fo, true) : dv.getInt16(fo, true);
        if (v === from) { if (sz === 4) dv.setInt32(fo, to, true); else dv.setInt16(fo, to, true); n++; ch = true; }
      }
      if (ch) fxePsWrite(ps, r.frame0, r.frame1, r.entries);
    }
  }
  fep.dirty();
  return n;
}
/* rewrite texture ids inside an MDL / ANM blob */
function fxeRetex(kind, raw, map) {
  const u = Uint8Array.from(raw), dv = fxeDV(u);
  if (kind === 'MDL') { const n = dv.getUint32(12, true), mo = dv.getUint32(16, true); for (let i = 0; i < n; i++) { const t = dv.getUint32(mo + 24 * i, true); if (map.has(t)) dv.setUint32(mo + 24 * i, map.get(t), true); } }
  else if (kind === 'ANM') { const n = dv.getUint32(0x20, true), to = dv.getUint32(0x30, true); for (let i = 0; i < n; i++) { const t = dv.getUint32(to + 8 * i, true); if (map.has(t)) dv.setUint32(to + 8 * i, map.get(t), true); } }
  return u;
}
const fxeTexIdsOf = (kind, raw) => { const dv = fxeDV(raw), out = []; if (kind === 'MDL') { const n = dv.getUint32(12, true), mo = dv.getUint32(16, true); for (let i = 0; i < n; i++) out.push(dv.getUint32(mo + 24 * i, true)); } else if (kind === 'ANM') { const n = dv.getUint32(0x20, true), to = dv.getUint32(0x30, true); for (let i = 0; i < n; i++) out.push(dv.getUint32(to + 8 * i, true)); } return out; };

/* ---- effects */
function fxeEffect(fep, i) { const e = fep.effectBlocks()[i]; return { blk: e, id: fxeU32(e.data, 0), bodyIndex: fxeU16(e.data, 4), body: fep.bodyBlocks()[fxeU16(e.data, 4)] }; }
function fxeEffectOfName(fep, ni) { const nm = fep.names()[ni]; if (!nm) return -1; const g = (nm.id >>> 10) & 127; return fep.effectBlocks().findIndex((e) => ((fxeU32(e.data, 0) >>> 10) & 127) === g); }
function fxeFreeGroup(fep) { const used = new Set(fep.effectBlocks().map((e) => (fxeU32(e.data, 0) >>> 10) & 127)); for (let g = 0; g < 128; g++) if (!used.has(g)) return g; return -1; }
function fxeRename(fep, ni, name) {
  const ns = fep.header.ptrs.get(0x44), arr = ns.ptrs.get(4), s = arr.ptrs.get(12 * ni + 8);
  const b = new TextEncoder().encode(name); s.data = fxePadded(fxeCat(b, new Uint8Array(1)), 4); fxeSetU32(arr.data, 12 * ni + 4, b.length); fep.dirty();
}
/* every block an effect owns: t0 entry, leaves, body graph (fep.py effect_blocks) */
function fxeEffectGraph(fep, ei) {
  const e = fxeEffect(fep, ei), seen = new Set([e.blk]), stack = [];
  const la = e.blk.ptrs.get(8); stack.push(la, e.body);
  while (stack.length) { const b = stack.pop(); if (!b || seen.has(b)) continue; seen.add(b); for (const t of b.ptrs.values()) stack.push(t); }
  return seen;
}
/* deep copy of effect `ei` of package `src` into package `dst` (same or another character) as a new effect named
   `name`; resources the copy uses come along (local ones copied with fresh ids, system ones referenced). Returns the
   new name index (= FEPA +0x28). */
function fxeCopyEffect(dst, src, ei, name) {
  const df = dst.fep, sf = src.fep, same = df === sf;
  const g = fxeFreeGroup(df); if (g < 0) throw new Error('this character already has 128 effects, the most a package can hold: delete or reuse one first');
  const e = fxeEffect(sf, ei), owned = fxeEffectGraph(sf, ei), copies = new Map();
  for (const b of owned) copies.set(b, b.clone());
  for (const c of copies.values()) c.ptrs = new Map([...c.ptrs].map(([r, t]) => [r, copies.get(t) || t]));
  const ne = copies.get(e.blk), newId = ((df.idBase & 0xFFFC0000) | (g << 10)) >>> 0;
  fxeSetU32(ne.data, 0, newId); fxeSetU16(ne.data, 4, df.nBodies);
  for (const b of owned) if (b.has('t0.leaf')) { const c = copies.get(b); fxeSetU32(c.data, 0, (newId | (fxeU32(c.data, 0) & 0x3FF)) >>> 0); }
  const nb = copies.get(e.body);
  if (!same) fxeImportNodeRes(dst, src, fxeBodyNodes(nb));
  // node serials (body +44) are unique file-wide in every shipped file: give the copy fresh ones
  let top = 0;
  for (const b of df.bodyBlocks()) for (const it of fxeArrGet(b.ptrs.get(44), fxeU16(b.data, 24))) { const l = it && it.ptrs.get(4); if (l) for (let k = 0; k < fxeU16(it.data, 0); k++) top = Math.max(top, fxeU16(l.data, 4 * k + 2) + 1); }
  for (const it of fxeArrGet(nb.ptrs.get(44), fxeU16(nb.data, 24))) { const l = it && it.ptrs.get(4); if (l) for (let k = 0; k < fxeU16(it.data, 0); k++) fxeSetU16(l.data, 4 * k + 2, top++); }
  const blocks = [...owned].map((b) => copies.get(b)).filter((b) => b !== ne);
  df.insert((b) => b.has('t0.ent'), [ne]);
  const datas = df.blocks.findIndex((b) => [...b.kinds].some((x) => /^res\d\.data$/.test(x)));
  df.blocks.splice(datas >= 0 ? datas : df.blocks.length, 0, ...blocks);
  df.growTable(0x28, ne); df.growTable(0x2C, nb);
  df.setH16(0x1C, df.nEffects + 1); df.setH16(0x1E, df.nBodies + 1);
  const sb = new FxeBlock(fxePadded(fxeCat(new TextEncoder().encode(name), new Uint8Array(1)), 4), ['name.str'], 4);
  df.insert((b) => b.has('name.str'), [sb]);
  const ns = df.header.ptrs.get(0x44), arr = ns.ptrs.get(4), n = fxeU32(ns.data, 0);
  arr.data = fxeCat(arr.data.subarray(0, 12 * n), new Uint8Array(12)); fxeSetU32(arr.data, 12 * n, newId); fxeSetU32(arr.data, 12 * n + 4, new TextEncoder().encode(name).length); arr.ptrs.set(12 * n + 8, sb);
  fxeSetU32(ns.data, 0, n + 1);
  if (dst.idFxse != null) dst.fxse.records.push(new Uint8Array(320));
  df.dirty();
  return n;
}
/* resources used by nodes copied from package `src`: local ones copied in with fresh ids (and their textures), system
   ones referenced; the nodes' resource fields are rewritten to this package's indices */
function fxeImportNodeRes(dst, src, nodes) {
  const df = dst.fep, sf = src.fep, maps = { TEX: new Map(), MDL: new Map(), ANM: new Map(), VTL: new Map() }, texIdMap = new Map();
  const importTex = (id) => {
    if (texIdMap.has(id)) return texIdMap.get(id);
    const sref = fxeRefs(sf, 'TEX').find((r) => r.id === id); let nid = id;
    if (sref && sref.size) { const ni2 = fxeAddRes(df, 'TEX', sref.data.data.subarray(0, sref.size)); nid = fxeRefs(df, 'TEX')[ni2].id; }
    else fxeAddSystemRef(df, 'TEX', id);
    texIdMap.set(id, nid); return nid;
  };
  const importRes = (kind, si) => {
    if (maps[kind].has(si)) return maps[kind].get(si);
    const sr = fxeRefs(sf, kind)[si]; if (!sr) return si;
    let di;
    if (!sr.size) di = fxeAddSystemRef(df, kind, sr.id);
    else {
      let raw = sr.data.data.subarray(0, sr.size);
      if (kind === 'MDL' || kind === 'ANM') { const m = new Map(); for (const t of fxeTexIdsOf(kind, raw)) m.set(t, importTex(t)); raw = fxeRetex(kind, raw, m); }
      di = fxeAddRes(df, kind, raw);
    }
    maps[kind].set(si, di); return di;
  };
  for (const nd of nodes) {
    if (!nd || !nd.data[8]) continue;
    const tracks = fxeNodeTracks(sf, nd), sets = [...(nd.data[18] ? [nd.ptrs.get(32)] : []), ...fxeNodeSets(nd)];
    for (const ps of sets) {
      const r = fxePsRead(sf, ps, tracks);
      for (const en of r.entries) for (const [kd, fo, sz] of FXE_LINKS[tracks[en.track].ch] || []) {
        if (fo + sz > en.bytes.length) continue;
        const dv = fxeDV(en.bytes), v = sz === 4 ? dv.getInt32(fo, true) : dv.getInt16(fo, true); if (v < 0) continue;
        const nv = importRes(kd, v); if (sz === 4) dv.setInt32(fo, nv, true); else dv.setInt16(fo, nv, true);
      }
      fxePsWrite(ps, r.frame0, r.frame1, r.entries);
    }
  }
  df.dirty();
}
/* a piece copied from another effect (any package) into body `body`, started at `frame` */
function fxeCopyNode(dst, body, src, srcNode, frame) {
  const c = fxeCloneNode(srcNode);
  if (src.fep !== dst.fep) fxeImportNodeRes(dst, src, [c.root]);
  return fxeAddNode(dst.fep, body, c, frame);
}
/* remove effect (name index ni): its entry, name, body, sound row and placements; later indices shift down */
function fxeDeleteEffect(pkg, ni) {
  const fep = pkg.fep, ei = fxeEffectOfName(fep, ni); if (ei < 0) throw new Error('no such effect');
  const e = fxeEffect(fep, ei), effs = fep.effectBlocks(), bodies = fep.bodyBlocks();
  fxeArrPut(fep, fep.header, 0x28, effs.filter((_, i) => i !== ei)); fep.setH16(0x1C, effs.length - 1);
  const keepBodies = bodies.filter((_, i) => i !== e.bodyIndex);
  fxeArrPut(fep, fep.header, 0x2C, keepBodies); fep.setH16(0x1E, keepBodies.length);
  for (const x of fep.effectBlocks()) { const b = fxeU16(x.data, 4); if (b > e.bodyIndex) fxeSetU16(x.data, 4, b - 1); }
  const ns = fep.header.ptrs.get(0x44), arr = ns.ptrs.get(4), n = fxeU32(ns.data, 0);
  const rows = Array.from({ length: n }, (_, i) => ({ d: arr.data.slice(12 * i, 12 * i + 12), s: arr.ptrs.get(12 * i + 8) })).filter((_, i) => i !== ni);
  arr.data = new Uint8Array(12 * rows.length); arr.ptrs = new Map(); rows.forEach((r, i) => { arr.data.set(r.d, 12 * i); arr.ptrs.set(12 * i + 8, r.s); });
  fxeSetU32(ns.data, 0, rows.length);
  if (pkg.idFxse != null) pkg.fxse.records.splice(ni, 1);
  const removed = [];
  pkg.fepa.records = pkg.fepa.records.filter((r) => { const x = fxeU32(r, 0x28); if (x === ni) { removed.push(fxeU32(r, 0x24)); return false; } if (x > ni) fxeSetU32(r, 0x28, x - 1); return true; });
  fep.gc();
  return removed;
}

/* ---- placements (FEPA) */
function fxeFreeFepaId(pkg, from) { const used = new Set(pkg.fepa.records.map((r) => fxeU32(r, 0x24))); let i = from || 500; while (used.has(i)) i++; return i; }
function fxeAddFepa(pkg, ni, template) {
  const r = template ? Uint8Array.from(template) : new Uint8Array(260);
  if (!template) { const dv = fxeDV(r); for (const o of [24, 28, 32]) dv.setFloat32(o, 1, true); }
  fxeSetU32(r, 0x24, fxeFreeFepaId(pkg)); fxeSetU32(r, 0x28, ni);
  pkg.fepa.records.push(r);
  return pkg.fepa.records.length - 1;
}
const fxeSetStr = (r, o, s) => { r.fill(0, o, o + 32); const b = new TextEncoder().encode(s).subarray(0, 31); r.set(b, o); };

/* ---- textures and flipbooks */
/* RGBA (w x h, power-of-two sizes up to 512) -> 'TEX' 8-bit CLUT, swizzled; quantised to 256 colours (median cut) */
function fxeEncodeTex(rgba, w, h) {
  const px = w * h, cols = new Map();
  for (let i = 0; i < px; i++) { const k = ((rgba[4 * i] << 24) | (rgba[4 * i + 1] << 16) | (rgba[4 * i + 2] << 8) | rgba[4 * i + 3]) >>> 0; cols.set(k, (cols.get(k) || 0) + 1); }
  let pal;
  if (cols.size <= 256) pal = [...cols.keys()].map((k) => [k >>> 24, (k >>> 16) & 255, (k >>> 8) & 255, k & 255]);
  else {
    const boxes = [[...cols.entries()].map(([k, n]) => [k >>> 24, (k >>> 16) & 255, (k >>> 8) & 255, k & 255, n])];
    while (boxes.length < 256) {
      let bi = -1, best = -1, ch = 0;
      boxes.forEach((b, i) => { if (b.length < 2) return; for (let c = 0; c < 4; c++) { let lo = 255, hi = 0; for (const p of b) { lo = Math.min(lo, p[c]); hi = Math.max(hi, p[c]); } const sc = (hi - lo) * (c === 3 ? 1.5 : 1); if (sc > best) { best = sc; bi = i; ch = c; } } });
      if (bi < 0 || best <= 0) break;
      const b = boxes[bi].sort((x, y) => x[ch] - y[ch]); let tot = b.reduce((s, p) => s + p[4], 0) / 2, k = 0, acc = 0; while (k < b.length - 1 && acc + b[k][4] < tot) acc += b[k++][4]; k = Math.max(1, k);
      boxes.splice(bi, 1, b.slice(0, k), b.slice(k));
    }
    pal = boxes.map((b) => { const t = b.reduce((s, p) => s + p[4], 0); return [0, 1, 2, 3].map((c) => Math.round(b.reduce((s, p) => s + p[c] * p[4], 0) / t)); });
  }
  while (pal.length < 256) pal.push([0, 0, 0, 0]);
  const cache = new Map(), near = (r, g, b, a) => { const k = ((r << 24) | (g << 16) | (b << 8) | a) >>> 0; if (cache.has(k)) return cache.get(k); let bi = 0, bd = Infinity; pal.forEach((p, i) => { const d = (p[0] - r) ** 2 + (p[1] - g) ** 2 + (p[2] - b) ** 2 + 1.5 * (p[3] - a) ** 2; if (d < bd) { bd = d; bi = i; } }); cache.set(k, bi); return bi; };
  const idx = new Uint8Array(px); for (let i = 0; i < px; i++) idx[i] = near(rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2], rgba[4 * i + 3]);
  const sw = new Uint8Array(px), bw = w / 16; let o = 0;                                     // PSP swizzle: 16 B x 8 rows tiles
  for (let ty = 0; ty < h / 8; ty++) for (let tx = 0; tx < bw; tx++) for (let r = 0; r < 8; r++) { sw.set(idx.subarray((ty * 8 + r) * w + tx * 16, (ty * 8 + r) * w + tx * 16 + 16), o); o += 16; }
  const clutOff = 0x30 + px, out = new Uint8Array(clutOff + 16 + 1024), dv = fxeDV(out);
  out.set([0x54, 0x45, 0x58, 0]); dv.setUint32(8, 1, true); dv.setUint32(16, 4, true); dv.setUint32(20, 1, true); dv.setUint32(24, clutOff, true);
  dv.setUint32(28, w, true); dv.setUint32(32, h, true); dv.setUint32(36, 0x30, true);
  out.set(sw, 0x30); dv.setUint32(clutOff, 256, true);
  pal.forEach((p, i) => out.set(p, clutOff + 16 + 4 * i));
  return out;
}
/* a flipbook from one texture: cols x rows cells played left to right, top to bottom, `dur` frames each
   (C §1); blend: 'add' 0x000A0600, 'alpha' 0x00070600, 'sub' 0x000A0602 */
function fxeMakeAnm(texId, cols, rows, dur, loop, size, blend, frames) {
  const n = Math.max(1, Math.min(frames || cols * rows, cols * rows)), word = { add: 0x000A0600, alpha: 0x00070600, sub: 0x000A0602 }[blend] || 0x000A0600;
  const oTex = 0x50, oClip = fxeAl16(oTex + 8), oSeq = fxeAl16(oClip + 12 * n), oList = fxeAl16(oSeq + 12 * n), oPart = fxeAl16(oList + 4 * n), oVtx = fxeAl16(oPart + 28 * n);
  const out = new Uint8Array(fxeAl16(oVtx + 20 * n)).fill(FXE_PAD), dv = fxeDV(out), U = (o, v) => dv.setUint32(o, v >>> 0, true), F = (o, v) => dv.setFloat32(o, v, true);
  out.set([0x41, 0x4E, 0x4D, 0]); U(8, 1); U(12, 1); U(0x10, loop ? 0x80 : 0); U(0x14, 0); U(0x18, 0); U(0x1C, n * dur);
  U(0x20, 1); U(0x24, n); U(0x28, n); U(0x2C, n); U(0x30, oTex); U(0x34, oClip); U(0x38, oSeq); U(0x3C, oPart);
  F(0x40, size); F(0x44, size); F(0x48, 0); F(0x4C, 0);
  U(oTex, texId); U(oTex + 4, 0);
  for (let i = 0; i < n; i++) {
    U(oClip + 12 * i, loop && i === n - 1 ? 0x80 : 0); U(oClip + 12 * i + 4, i); U(oClip + 12 * i + 8, dur);
    U(oSeq + 12 * i, 0); U(oSeq + 12 * i + 4, 1); U(oSeq + 12 * i + 8, oList + 4 * i); U(oList + 4 * i, i);
    const p = oPart + 28 * i; U(p, 0); out.set([128, 128, 128, 128], p + 4); U(p + 8, 4); U(p + 12, oVtx + 20 * i); U(p + 16, word); U(p + 20, 0); U(p + 24, 0xFFFFFF);
    const c = i % cols, r = Math.floor(i / cols), u0 = Math.round(128 * c / cols), u1 = Math.round(128 * (c + 1) / cols), v0 = Math.round(128 * r / rows), v1 = Math.round(128 * (r + 1) / rows);
    [[u0, v0, -127, 127], [u1, v0, 127, 127], [u0, v1, -127, -127], [u1, v1, 127, -127]].forEach(([u, v, x, y], k) => { const o = oVtx + 20 * i + 5 * k; out[o] = u; out[o + 1] = v; dv.setInt8(o + 2, x); dv.setInt8(o + 3, y); dv.setInt8(o + 4, 127); });
  }
  return out;
}
