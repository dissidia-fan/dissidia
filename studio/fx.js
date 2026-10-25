// Dissidia Move Studio: effect preview. This is the game's own effect renderer, re-implemented from the recompiled
// EBOOT (docs/archive/fx-components/A.md … F.md: element core, meshes, flipbooks, particles, trails, other channels).
// It reads the character's effect package (the objx 'effe' section: FEP effects, FEPA placements, the magi projectile
// records) and the shared library in main.bin (resident.frr), and draws what a motion's events start.
//
//   placement   FEPA record: bone / root / caller point, attach bits, delay, lifetime, follow time (A §1)
//   timing      1 effect frame = 1 tick; spawn schedule, segments that extrapolate, loops h12..h14, sustained
//               bodies and their stop / fade modes (A §3)
//   transform   channel 14 builds T(x,−y,−z)·R(rx,−ry,−rz)·S(0.01·s); billboard modes 0/1/2; basis 0.1 for
//               flipbooks, the MDL reference scale for meshes (A §2)
//   colour      GE lighting with ambient = node colour, FRAGMENT_2X, 2·alpha blend factors, the blend word of each
//               material / part, depth writes and culling per material (A §4, B §3, C §3)
//   drawn       ch10 flipbooks, ch11 meshes, ch20 meshes with scrolling UVs, ch21 particles (a deterministic replay
//               of the game's emitter), ch105/106 weapon trails (B-spline ribbon), ch107 screen flash, ch110 zoom
//               blur, ch101 shockwave ring, ch100/102 line sparks; ch18/151 child nodes; ch22–25/31–34/39 randomisers,
//               ch19 gravity, ch29 face-velocity, ch61/62 flicker
//   not exact   the game's random numbers are one global sequence, so the preview seeds its own per element; ch101's
//               field split, ch18/151 child placement and ch100/102 spark layouts are partly guessed (F.md)
'use strict';
const FXR = { resident: null };        // the resident library: {id: {kind, u8}} from main.bin
const FX_D2R = Math.PI / 180;
const FX_ORDERS = ['XYZ', 'YXZ', 'ZXY', 'XZY', 'YZX', 'ZYX'];

/* ------------------------------------------------------------------ containers */
const fxDV = (u) => new DataView(u.buffer, u.byteOffset, u.byteLength);
function fxMpk(u8) {                   // 'MPK ' ver, u32 n, 0, n x {u32 name offset, u32 offset, u32 size, 0}
  const dv = fxDV(u8), n = dv.getUint32(8, true), out = [];
  if (String.fromCharCode(...u8.subarray(0, 4)) !== 'MPK ') return out;
  for (let i = 0; i < n; i++) {
    const o = 16 + 16 * i, no = dv.getUint32(o, true), off = dv.getUint32(o + 4, true), size = dv.getUint32(o + 8, true);
    let name = ''; for (let k = no; k < u8.length && u8[k]; k++) name += String.fromCharCode(u8[k]);
    out.push({ name, data: u8.subarray(off, off + size) });
  }
  return out;
}
const magicOf = (u8) => String.fromCharCode(...u8.subarray(0, 4));
const cstrAt = (u8, o, max) => { let s = ''; for (let i = o; i < o + max && u8[i]; i++) s += String.fromCharCode(u8[i]); return s; };

/* ------------------------------------------------------------------ FEP */
// Pointers are file offsets (the engine relocates them in place), so the parser reads the file directly.
function parseFep(u8) {
  const dv = fxDV(u8), U = (o) => dv.getUint32(o, true), H = (o) => dv.getUint16(o, true);
  if (magicOf(u8) !== 'FEP\xcd') throw new Error('not a FEP');
  const ct = U(U(0x08) + 4), paramSize = (ch) => H(ct + 8 * ch + 4);
  const fep = { u8, dv, idBase: U(0x10), names: [], effects: [], res: { TEX: [], MDL: [], ANM: [], VTL: [] }, bodies: new Map(), paramSize, p21: new Map() };
  const np = U(0x44);
  if (np) {
    const n = U(np), arr = U(np + 4);
    for (let i = 0; i < n; i++) { const id = U(arr + 12 * i), len = U(arr + 12 * i + 4), sp = U(arr + 12 * i + 8); fep.names.push({ id, name: cstrAt(u8, sp, len) }); }
  }
  const t0 = U(0x28);
  for (let i = 0; i < H(0x1C); i++) { const e = U(t0 + 4 * i); fep.effects.push({ id: U(e), body: H(e + 4) }); }
  ['TEX', 'MDL', 'ANM', 'VTL'].forEach((k, ki) => {   // resource refs {f32 scale, u32 id, u32 size, ptr data, handle}
    const tab = U(0x30 + 4 * ki);
    if (!tab) return;
    for (let i = 0; i < H(0x20 + 2 * ki); i++) { const r = U(tab + 4 * i); fep.res[k].push({ scale: dv.getFloat32(r, true) || 1, id: U(r + 4), size: U(r + 8), off: U(r + 12) }); }
  });
  fep.nBodies = H(0x1E); fep.bodyTab = U(0x2C);
  return fep;
}
/* body (52 B) -> spawn schedule + nodes (72 B) -> tracks (12 B) -> ParamSets. A segment applies from its ParamSet's
   frame0; its curve runs from the entry's own start (A §3.5). */
function fepBody(fep, i) {
  if (fep.bodies.has(i)) return fep.bodies.get(i);
  const { dv } = fep, U = (o) => dv.getUint32(o, true), H = (o) => dv.getUint16(o, true), B = (o) => fep.u8[o];
  const e = U(fep.bodyTab + 4 * i);
  const body = { index: i, off: e, a: U(e), duration: U(e + 4), c: U(e + 8), d: U(e + 12), spawns: [], nodes: [] };
  const sp = U(e + 28);
  for (let j = 0; j < H(e + 16); j++) {
    const it = U(sp + 4 * j), n = H(it + 2), lst = U(it + 4), nodes = [];
    for (let k = 0; k < n; k++) nodes.push(H(lst + 2 * k));
    body.spawns.push({ frame: H(it), nodes });
  }
  body.spawns.sort((x, y) => x.frame - y.frame);
  const na = U(e + 32);
  for (let j = 0; j < H(e + 18); j++) {
    const nd = U(na + 4 * j);
    if (!nd || !B(nd + 8)) { body.nodes.push(null); continue; }
    const node = { index: j, off: nd, flags: H(nd), duration: H(nd + 4), kind: B(nd + 11), loop0: H(nd + 12), loop1: H(nd + 14), tracks: [] };
    const ta = U(nd + 28);
    for (let k = 0; k < B(nd + 8); k++) {
      const tr = U(ta + 4 * k), n = U(tr + 4), dp = U(tr + 8), deps = [];
      for (let q = 0; q < n; q++) deps.push(H(dp + 2 * q));
      node.tracks.push({ index: k, off: tr, chan: H(tr), deps, segs: [] });
    }
    const sets = [];
    if (B(nd + 18)) sets.push({ z: U(nd + 32), init: true });
    const sa = U(nd + 36);
    for (let k = 0; k < B(nd + 19); k++) sets.push({ z: U(sa + 4 * k), init: false });
    for (const { z, init } of sets) {
      const n = U(z + 4), tab = U(z + 8), f0 = init ? -1 : H(z);
      for (let q = 0; q < n; q++) {
        const ti = H(tab + 8 * q), off = H(tab + 8 * q + 2), a = H(tab + 8 * q + 4), b = H(tab + 8 * q + 6), tr = node.tracks[ti];
        if (tr) tr.segs.push({ start: f0, es: init ? 0 : a, len: init ? 0 : b, o: z + off, init });
      }
    }
    for (const tr of node.tracks) tr.segs.sort((x, y) => x.start - y.start);
    node.byChan = new Map(); for (const tr of node.tracks) if (!node.byChan.has(tr.chan)) node.byChan.set(tr.chan, tr);
    body.nodes.push(node);
  }
  fep.bodies.set(i, body);
  return body;
}
/* effect id of a FEPA record -> body index (FUN_089444B0: the t0 entry of the same group) */
function fepBodyOfName(fep, nameIndex) {
  const nm = fep.names[nameIndex]; if (!nm) return null;
  const g = (nm.id & 0x1FC00) >> 10;
  const e = fep.effects.find((x) => ((x.id & 0x1FC00) >> 10) === g);
  return e ? e.body : null;
}
/* FEPA placement records (260 B, A §1.2) */
function parseFepa(u8) {
  if (!u8 || magicOf(u8) !== 'FEPA') return [];
  const dv = fxDV(u8), n = dv.getUint32(12, true), out = [];
  for (let i = 0; i < n; i++) {
    const r = 16 + 260 * i, f = (o) => dv.getFloat32(r + o, true), U = (o) => dv.getUint32(r + o, true), I = (o) => dv.getInt32(r + o, true);
    out.push({ index: i, pos: [f(0), f(4), f(8)], rot: [f(12), f(16), f(20)], scale: [f(24), f(28), f(32)], id: U(0x24), effect: U(0x28),
      life: I(0x30), trig: U(0x34), kinds: U(0x38), mask: U(0x3C), maskHi: U(0x40), keys: U(0x44), role: U(0x48), r50: U(0x50), target: U(0x70), delay: I(0x74), follow: I(0x78), attach: U(0x7C),
      model: cstrAt(u8, r + 0x88, 32), bone: cstrAt(u8, r + 0xA8, 32), motion: cstrAt(u8, r + 0xC8, 32) });
  }
  return out;
}
/* the character's effect package from the objx 'effe' section */
function loadEffects(effe) {
  const ents = fxMpk(effe);
  const fepE = ents.find((e) => magicOf(e.data) === 'FEP\xcd'), fepaE = ents.find((e) => magicOf(e.data) === 'FEPA');
  if (!fepE) return null;
  const fx = { fep: parseFep(fepE.data), fepa: parseFepa(fepaE && fepaE.data), cache: new Map() };
  fx.byId = new Map();
  for (const r of fx.fepa) { if (!fx.byId.has(r.id)) fx.byId.set(r.id, []); fx.byId.get(r.id).push(r); }
  return fx;
}
/* resident library: main.bin member resident.frr holds the 0xF0xxxxxx TEX / MDL / ANM blocks the FEPs share */
function loadResident(mainBin) {
  const m = fxMpk(mainBin).find((e) => e.name === 'resident.frr'); if (!m) return null;
  const u8 = m.data, dv = fxDV(u8), lib = new Map();
  for (let o = 0; o + 12 <= u8.length; o += 4) {
    const mg = u8[o + 3] === 0 && String.fromCharCode(u8[o], u8[o + 1], u8[o + 2]);
    if (mg !== 'TEX' && mg !== 'MDL' && mg !== 'ANM' && mg !== 'VTL') continue;
    const id = dv.getUint32(o + (mg === 'MDL' ? 8 : 4), true);
    if ((id >>> 24) === 0xF0 && !lib.has(id)) lib.set(id, { kind: mg, u8: u8.subarray(o) });
  }
  return lib;
}
/* resource blob: a table entry of this FEP (local, or a system id resolved in the resident library) */
function fxResource(fep, kind, index) {
  const r = fep.res[kind][index]; if (!r) return null;
  if (r.size) return fep.u8.subarray(r.off, r.off + r.size);
  const lib = FXR.resident && FXR.resident.get(r.id);
  return lib ? lib.u8 : null;
}
function fxResourceById(fep, kind, id) {
  const i = fep.res[kind].findIndex((r) => r.id === id);
  if (i >= 0) return fxResource(fep, kind, i);
  const lib = FXR.resident && FXR.resident.get(id);
  return lib && lib.kind === kind ? lib.u8 : null;
}

/* ------------------------------------------------------------------ resources */
function decodeTex(u8) {               // 'TEX\0' id, 1, 0, fmt (4 = 8-bit CLUT, 5 = 4-bit), swizzled, clut_off, w, h, pix_off
  const dv = fxDV(u8), fmt = dv.getUint32(16, true), sw = dv.getUint32(20, true), co = dv.getUint32(24, true);
  const w = dv.getUint32(28, true), h = dv.getUint32(32, true), po = dv.getUint32(36, true);
  const bpp = fmt === 4 ? 8 : 4, wb = w * bpp / 8, n = dv.getUint32(co, true), pal = u8.subarray(co + 16, co + 16 + 4 * n);
  let px = u8.subarray(po, po + wb * h);
  if (sw) {
    const out = new Uint8Array(wb * h), bw = wb / 16; let i = 0;
    for (let ty = 0; ty < h / 8; ty++) for (let tx = 0; tx < bw; tx++) for (let r = 0; r < 8; r++) { out.set(px.subarray(i, i + 16), (ty * 8 + r) * wb + tx * 16); i += 16; }
    px = out;
  }
  const rgba = new Uint8Array(w * h * 4);
  for (let p = 0; p < w * h; p++) {
    const ci = bpp === 8 ? px[p] : (p & 1 ? px[p >> 1] >> 4 : px[p >> 1] & 15);
    rgba.set(pal.subarray(4 * ci, 4 * ci + 4), 4 * p);
  }
  return { w, h, rgba };
}
function geLayout(vtype) {             // PSP GE vertex type -> {size, uv, c, pos}
  const tcs = [0, 1, 2, 4][vtype & 3], col = (vtype >> 2) & 7, nrm = [0, 1, 2, 4][(vtype >> 5) & 3], pos = [0, 1, 2, 4][(vtype >> 7) & 3];
  const cs = { 4: 2, 5: 2, 6: 2, 7: 4 }[col] || 0;
  let off = 0, al = 1; const L = { col };
  for (const [name, cnt, sz] of [['uv', 2, tcs], ['c', 1, cs], ['n', 3, nrm], ['pos', 3, pos]]) {
    if (!sz) continue;
    off = Math.ceil(off / sz) * sz; L[name] = { off, sz }; off += cnt * sz; al = Math.max(al, sz);
  }
  L.size = Math.ceil(off / al) * al;
  return L;
}
/* GE vertex fields in transform mode: s8 / 128, s16 / 32768, u8 uv / 128, u16 uv / 32768 */
function geVert(u8, dv, o, L) {
  const P = (k) => L.pos.sz === 1 ? dv.getInt8(o + L.pos.off + k) / 128 : L.pos.sz === 2 ? dv.getInt16(o + L.pos.off + 2 * k, true) / 32768 : dv.getFloat32(o + L.pos.off + 4 * k, true);
  const T = (k) => !L.uv ? 0 : L.uv.sz === 1 ? u8[o + L.uv.off + k] / 128 : L.uv.sz === 2 ? dv.getUint16(o + L.uv.off + 2 * k, true) / 32768 : dv.getFloat32(o + L.uv.off + 4 * k, true);
  let c = [1, 1, 1, 1];
  if (L.c) {
    if (L.col === 6) { const x = dv.getUint16(o + L.c.off, true); c = [x & 15, (x >> 4) & 15, (x >> 8) & 15, x >> 12].map((n) => n * 17 / 255); }
    else if (L.col === 7) { const x = dv.getUint32(o + L.c.off, true); c = [x & 255, (x >> 8) & 255, (x >> 16) & 255, x >>> 24].map((n) => n / 255); }
    else if (L.col === 5) { const x = dv.getUint16(o + L.c.off, true); c = [(x & 31) / 31, ((x >> 5) & 31) / 31, ((x >> 10) & 31) / 31, x >> 15]; }
    else if (L.col === 4) { const x = dv.getUint16(o + L.c.off, true); c = [(x & 31) / 31, ((x >> 5) & 63) / 63, (x >> 11) / 31, 1]; }
  }
  return { p: [P(0), P(1), P(2)], uv: [T(0), T(1)], c };
}
const stripTris = (n) => { const t = []; for (let i = 0; i + 2 < n; i++) t.push(...(i & 1 ? [i + 1, i, i + 2] : [i, i + 1, i + 2])); return t; };
/* MDL (B §1): every group, every prim; group MODEL = T(t)·S(s), texture scale / offset per group; each prim has its own
   material (blend word, fix colours, flags 0x4 no depth write / 0x8 cull) or none (-1: untextured, GE state carries) */
function decodeMdl(u8) {
  const dv = fxDV(u8), U = (o) => dv.getUint32(o, true), F = (o) => dv.getFloat32(o, true);
  const nMat = U(12), matOff = U(16), nGrp = U(20), grpOff = U(24);
  const mats = []; for (let i = 0; i < nMat; i++) { const o = matOff + 24 * i; mats.push({ tex: U(o), flags: U(o + 8), word: U(o + 12), fixA: U(o + 16), fixB: U(o + 20) }); }
  const parts = []; let last = { word: 0x000A0600, fixB: 0xFFFFFFFF, flags: 7 };
  for (let g = 0; g < nGrp; g++) {
    const go = grpOff + 52 * g, nPrim = U(go), primOff = U(go + 4), ccw = U(go + 8) !== 0;
    const [sx, sy, sz, tx, ty, tz, su, sv, ou, ov] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((k) => F(go + 12 + 4 * k));
    for (let k = 0; k < nPrim; k++) {
      const po = primOff + 24 * k; if (po + 24 > u8.length) break;
      const prim = U(po), vtype = U(po + 4), mi = dv.getInt32(po + 12, true), nv = U(po + 16), vo = U(po + 20), L = geLayout(vtype);
      const P = [], UV = [], C = [];
      for (let i = 0; i < nv; i++) {
        const o = vo + i * L.size; if (o + L.size > u8.length) break;
        const v = geVert(u8, dv, o, L);
        P.push(tx + sx * v.p[0], ty + sy * v.p[1], tz + sz * v.p[2]); UV.push(v.uv[0] * su + ou, v.uv[1] * sv + ov); C.push(...v.c);
      }
      const n = P.length / 3; if (n < 3) continue;
      const I = prim === 4 ? stripTris(n) : Array.from({ length: n - n % 3 }, (_, i) => i);
      const mat = mi >= 0 ? mats[mi] || null : null;
      if (mat) last = mat;
      parts.push({ P, UV, C, I, tex: mat ? mat.tex : null, word: last.word, fixB: last.fixB, flags: mat ? mat.flags : last.flags, ccw });
    }
  }
  return { mats, parts };
}
/* ANM 'TexAnm' (C §1): clips {flags, seq, dur} -> sequences {flags, n, list} -> parts (one quad each: texture index,
   u8 tint, blend word, fix colours); vertex {u8 u, u8 v, s8 x, y, z}, position x/128·size + offset, z/128 */
function decodeAnm(u8) {
  const dv = fxDV(u8), U = (o) => dv.getUint32(o, true), F = (o) => dv.getFloat32(o, true);
  const [nTex, nClip, nSeq, nPart] = [0x20, 0x24, 0x28, 0x2C].map(U), [oTex, oClip, oSeq, oPart] = [0x30, 0x34, 0x38, 0x3C].map(U);
  const a = { id: U(4), loop: !!(U(0x10) & 0x80), total: U(0x1C), size: [F(0x40), F(0x44)], offset: [F(0x48), F(0x4C)], tex: [], clips: [], seqs: [], parts: [] };
  for (let i = 0; i < nTex; i++) a.tex.push(U(oTex + 8 * i));
  for (let i = 0; i < nClip; i++) a.clips.push(U(oClip + 12 * i + 8));
  for (let i = 0; i < nSeq; i++) { const n = U(oSeq + 12 * i + 4), lo = U(oSeq + 12 * i + 8); a.seqs.push(Array.from({ length: n }, (_, k) => U(lo + 4 * k))); }
  for (let i = 0; i < nPart; i++) {
    const o = oPart + 28 * i; if (o + 28 > u8.length) break;
    const vo = U(o + 12), nv = Math.min(U(o + 8), 64), P = [], UV = [];
    for (let k = 0; k < nv; k++) {
      const p = vo + 5 * k; if (p + 5 > u8.length) break;
      UV.push(u8[p] / 128, u8[p + 1] / 128);
      P.push(dv.getInt8(p + 2) / 128 * a.size[0] + a.offset[0], dv.getInt8(p + 3) / 128 * a.size[1] + a.offset[1], dv.getInt8(p + 4) / 128);
    }
    a.parts.push({ tex: U(o), tint: [u8[o + 4], u8[o + 5], u8[o + 6], u8[o + 7]], word: U(o + 16), fixA: U(o + 20), fixB: U(o + 24), P, UV, I: stripTris(P.length / 3) });
  }
  if (!a.total) a.total = a.clips.reduce((s, x) => s + x, 0);
  return a;
}
/* the clip an ANM shows at time T (C §2.3), -1 once a one-shot flipbook has ended */
function anmClip(a, T) {
  if (!a.clips.length) return -1;
  if (T < 0) T = 0;
  if (T >= a.total) { if (!a.loop || !a.total) return -1; T = Math.trunc(T) % a.total; }
  let k = 0, end = a.clips[0];
  while (T >= end && k < a.clips.length - 1) end += a.clips[++k];
  return k;
}
/* VTL vertex lists (E §2) */
function decodeVtl(u8) {
  const dv = fxDV(u8), U = (o) => dv.getUint32(o, true), F = (o) => dv.getFloat32(o, true);
  const nl = U(0x18), nv = U(0x1C), lo = U(0x20), vo = U(0x24), lists = [], verts = [];
  for (let i = 0; i < nl; i++) { const n = U(lo + 8 * i), o = U(lo + 8 * i + 4); lists.push(Array.from({ length: n }, (_, k) => U(o + 4 * k))); }
  for (let i = 0; i < nv; i++) verts.push([F(vo + 12 * i), F(vo + 12 * i + 4), F(vo + 12 * i + 8)]);
  return { lists, verts };
}

/* ------------------------------------------------------------------ channel values (A §5) */
function fxRng(seed) { let s = seed >>> 0; return () => { s = (Math.imul(s, 0x343FD) + 0x269EC3) >>> 0; return ((s >>> 16) & 0x7FFF) / 32767; }; }
const fxR = (rnd, x) => (2 * rnd() - 1) * x;
const fxHash = (...xs) => { let h = 2166136261; for (const x of xs) { h ^= x & 0xFFFF; h = Math.imul(h, 16777619); h ^= (x >>> 16); h = Math.imul(h, 16777619); } return h >>> 0; };
function fxSeg(tr, f) { let s = null; for (const x of tr.segs) { if (x.start <= f) s = x; else break; } return s || tr.segs[0] || null; }
const fxF = (fep, o) => fep.dv.getFloat32(o, true);
const fxB = (fep, o) => fep.u8[o];
const fxI = (fep, o) => fep.dv.getInt32(o, true);
const fxS = (fep, o) => fep.dv.getInt16(o, true);
const fxH = (fep, o) => fep.dv.getUint16(o, true);
const isValueChan = (ch) => (ch >= 1 && ch <= 9) || (ch >= 15 && ch <= 17);
/* value and rate of a value channel at element time g: base + rate·τ + acc·τ²/2 with τ = g − entry start, no clamp
   (a curve extrapolates until the next segment); scale ×0.01; colour clamped to 0..255. off = randomiser offsets. */
function chanEval(fep, tr, g, off) {
  const ch = tr.chan, s = fxSeg(tr, Math.floor(g)); if (!s) return null;
  const col = ch >= 15, q = col ? ch - 15 : (ch - 1) % 3, n = col ? 4 : 3, tau = g - s.es, v = new Array(n), vel = new Array(n);
  for (let k = 0; k < n; k++) {
    let b = fxF(fep, s.o + 4 * k), r = q >= 1 ? fxF(fep, s.o + 16 + 4 * k) : 0, a = q === 2 ? fxF(fep, s.o + 32 + 4 * k) : 0;
    if (off) { b += off.b[k]; r += off.r[k]; a += off.a[k]; }
    v[k] = b + r * tau + a * tau * tau / 2; vel[k] = r + a * tau;
  }
  if (ch >= 7 && ch <= 9) for (let k = 0; k < 3; k++) { v[k] *= 0.01; vel[k] *= 0.01; }
  if (col) for (let k = 0; k < 4; k++) v[k] = Math.min(255, Math.max(0, v[k]));
  return { v, vel };
}
/* element time with node loops: flag 0x2 on a sustained instance loops h12..h14 until the effect stops; after the
   stop the element plays out from where it was (A §3.4) */
function nodeTime(node, g0, sustained, stopAt) {
  const lp = (node.flags & 2) && sustained && node.loop1 > node.loop0;
  if (!lp) return { g: g0, looping: false };
  const wrap = (x) => x < node.loop1 ? x : node.loop0 + ((Math.floor(x) - node.loop1) % (node.loop1 - node.loop0)) + (x - Math.floor(x));
  if (g0 < stopAt) return { g: wrap(g0), looping: true };
  const s = Math.max(0, stopAt);
  return { g: wrap(s) + (g0 - s), looping: false };
}
/* randomisers (D §6, F): offsets they add once to another track's base / rate / acceleration */
function fxRandomOffsets(fep, node, rnd) {
  const off = new Map();
  const get = (ti) => { if (!off.has(ti)) off.set(ti, { b: [0, 0, 0, 0], r: [0, 0, 0, 0], a: [0, 0, 0, 0] }); return off.get(ti); };
  const pick = (mode, x) => mode === 1 ? rnd() * x : mode === 2 ? -rnd() * x : fxR(rnd, x);
  const slot = (k) => ['b', 'r', 'a'][k];
  for (const tr of node.tracks) {
    const ch = tr.chan; if (![19, 22, 23, 24, 25, 31, 32, 33, 34, 39].includes(ch) || !tr.deps.length) continue;
    const s = fxSeg(tr, 0); if (!s) continue;
    const o = s.o, tgt = get(tr.deps[0]);
    if (ch === 22 || ch === 23 || ch === 24) {
      const lay = ch === 24 ? [[13, 14], [15, 28], [29, 30]] : [[12, 13], [14, 15], [28, 29]];
      for (let k = 0; k < 3; k++) {
        if (!fxB(fep, o + lay[k][0])) continue;
        const mode = fxB(fep, o + lay[k][1]), R = [0, 1, 2].map((c) => fxF(fep, o + 16 * k + 4 * c));
        let d = R.map((x) => pick(mode, x));
        if (ch === 24 && !fxB(fep, o + 12)) d = [d[0], d[0], d[0]];
        for (let c = 0; c < 3; c++) tgt[slot(k)][c] += d[c];
      }
    } else if (ch === 25) {
      for (let k = 0; k < 3; k++) {
        if (!fxB(fep, o + 48 + 2 * k)) continue;
        const mode = fxB(fep, o + 49 + 2 * k);
        for (let c = 0; c < 4; c++) tgt[slot(k)][c] += pick(mode, fxF(fep, o + 16 * k + 4 * c));
      }
    } else if (ch >= 31 && ch <= 33) {
      const k = Math.min(2, fxB(fep, o + 12));
      for (let c = 0; c < 3; c++) tgt[slot(k)][c] += pick(fxB(fep, o + 13 + c), fxF(fep, o + 4 * c));
    } else if (ch === 34 || ch === 39) {
      const k = Math.min(2, fxB(fep, o + 16));
      for (let c = 0; c < 4; c++) tgt[slot(k)][c] += pick(fxB(fep, o + 17 + c), fxF(fep, o + 4 * c));
    } else if (ch === 19) tgt.a[1] += fxS(fep, o) / 100 * 0.098;      // gravity: acceleration y (effect space)
  }
  return off;
}
/* ch61 flicker / pulse envelope (F §61), replayed from the element's start; ch62 multiplies its colour by it */
function fxEnvelope(fep, tr, steps, rnd) {
  const s = { mult: [1, 1, 1, 1], rate: [0, 0, 0, 0], timer: 0, stage: -1 };
  for (let f = 0; f < steps; f++) {
    const seg = fxSeg(tr, f); if (!seg) break;
    const o = seg.o, V = (x) => [0, 1, 2, 3].map((c) => fxF(fep, o + x + 4 * c));
    const p = { A: V(0), jitA: V(16), B: V(32), jitB: V(48), holdA: fxH(fep, o + 64), holdB: fxH(fep, o + 66), ramp: fxH(fep, o + 68), jitAOn: fxB(fep, o + 70), jitBOn: fxB(fep, o + 72) };
    const th = [p.holdA, p.holdA + p.ramp, p.holdA + p.ramp + p.holdB, p.holdA + 2 * p.ramp + p.holdB];
    const st = s.timer < th[0] ? 0 : s.timer < th[1] ? 1 : s.timer < th[2] ? 2 : 3;
    if (st !== s.stage) {
      const toB = st === 1 || (st === 0 && s.stage === 2), toA = st === 3 || (st === 2 && s.stage === 0);
      if (toB || toA) {
        const T = toB ? p.B : p.A, J = toB ? p.jitB : p.jitA, on = toB ? p.jitBOn : p.jitAOn;
        for (let i = 0; i < 4; i++) { const tgt = Math.min(1, Math.max(0, T[i] + (on ? fxR(rnd, J[i]) : 0))); s.rate[i] = (tgt - s.mult[i]) / (p.ramp > 0 ? p.ramp : 1); }
        if (st === 0 || st === 2) { for (let i = 0; i < 4; i++) s.mult[i] += s.rate[i]; s.rate.fill(0); }
      }
      s.stage = st;
    }
    for (let i = 0; i < 4; i++) s.mult[i] += s.rate[i];
    s.timer += 0.5; if (s.timer >= th[3]) s.timer = 0;
  }
  return s.mult;
}
/* ch21 parameter block (277 B, D §2) */
function fxP21(fep, o) {
  if (fep.p21.has(o)) return fep.p21.get(o);
  const f = (x) => fxF(fep, o + x), b = (x) => fxB(fep, o + x), V3 = (x) => [f(x), f(x + 4), f(x + 8)], V4 = (x) => [f(x), f(x + 4), f(x + 8), f(x + 12)], V2 = (x) => [f(x), f(x + 4)];
  const P = { box: V3(0), anm: fxI(fep, o + 12), axis: V3(16), anmSpeed: f(28), forceDir: V3(32), rate: f(44), forceDirRand: V3(48), rateAcc: f(60),
    col: V4(64), colRate: V4(80), colRand: V4(96), colRateRand: V4(112), scale: V2(128), scaleVel: V2(136), scaleAcc: V2(144), scaleRand: V2(152), scaleVelRand: V2(160), scaleAccRand: V2(168),
    spread: f(176), speed: f(180), accel: f(184), speedRand: f(188), accelRand: f(192), fSpeed: f(196), fAccel: f(200), fSpeedRand: f(204), fAccelRand: f(208), rot: V3(212), rotRand: V3(224), zbias: f(236),
    maxP: fxH(fep, o + 240), life: fxH(fep, o + 242), mdl: fxS(fep, o + 244), vtl: fxS(fep, o + 246), vtlSet: fxS(fep, o + 248), flip: b(250), space: b(251), interval: b(252), prob: b(253), shape: b(254),
    vtxOrder: b(255), useDist: b(256), distMode: b(257), distPct: b(258), useSpeed: b(259), rndSpeed: b(260), useForce: b(261), rndForce: b(262), useScale: b(263), rndScale: b(264), scaleRandMode: b(265),
    useRot: b(266), rndRot: b(267), useCol: b(268), rndCol: b(269), useZbias: b(270), fade: b(272), fadeIn: b(273), fadeOut: b(274) };
  fep.p21.set(o, P);
  return P;
}

/* ------------------------------------------------------------------ projectiles (objx 'magi', tools/charmod_magi.py) */
// 204-byte records after a 16-byte header, 46 parameters as (lo, hi, step) byte triples at +0x40 into a float pool;
// a shot plays FEPA id `effect` as its visual
const MAGI_PARAMS = ['shots', 'first_delay', 'interval', 'fwd_dist', 'fwd_yaw', 'off_x', 'off_y', 'off_z', 'launch_pitch', 'launch_yaw',
  'aim_dist', 'aim_yaw', 'aim_height', 'move_time', 'speed', 'speed_min', 'speed_max', 'accel', 'accel_start', 'turn_rate', 'homing_start',
  'homing_time', 'homing_stop', 'gravity', 'gravity_start', 'size', 'size_min', 'size_max', 'size_rate', 'size_start', 'emit_count',
  'emit_delay', 'emit_interval', 'tgt_dist_min', 'tgt_dist_max', 'tgt_height_min', 'tgt_height_max', 'pull_start', 'pull_time',
  'pull_strength', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist', 'lifetime', 'hit_time', 'hit_delay'];
function parseMagi(u8) {
  const dv = fxDV(u8); if (magicOf(u8) !== 'MAGI') return new Map();
  const n = dv.getUint32(8, true), nf = dv.getUint32(12, true), pool = [], out = new Map();
  for (let i = 0; i < nf; i++) pool.push(dv.getFloat32(16 + 204 * n + 4 * i, true));
  for (let i = 0; i < n; i++) {
    const o = 16 + 204 * i, values = {};
    MAGI_PARAMS.forEach((nm, k) => { const q = o + 0x40 + 3 * k; values[nm] = [pool[dv.getInt8(q)], pool[dv.getInt8(q + 1)], pool[dv.getInt8(q + 2)]]; });
    out.set(dv.getUint16(o, true), { id: dv.getUint16(o, true), effect: dv.getUint16(o + 4, true), origin: dv.getUint16(o + 0x1E, true), origin_arg: dv.getUint16(o + 0x20, true),
      motion: dv.getUint16(o + 0x28, true), flags: dv.getUint32(o + 0x38, true), atkp: dv.getUint16(o + 0x0A, true),
      link_emit: dv.getInt16(o + 0x0E, true), link_hit: dv.getInt16(o + 0x18, true), values,
      // the rest of the record (charmod_magi.FIELDS), for the Move Studio dummy's projectile flight (magi.js)
      kind: dv.getUint16(o + 0x02, true), atkp_reflected: dv.getUint16(o + 0x0C, true), link_terrain: dv.getInt16(o + 0x10, true),
      link_terrain_1: dv.getInt16(o + 0x12, true), link_terrain_2: dv.getInt16(o + 0x14, true), link_terrain_4: dv.getInt16(o + 0x16, true),
      link_fall: dv.getInt16(o + 0x1A, true), link_clash: dv.getInt16(o + 0x1C, true), direction: dv.getUint16(o + 0x22, true),
      orient: dv.getInt16(o + 0x24, true), target: dv.getUint16(o + 0x26, true), hit_radius: dv.getFloat32(o + 0x2C, true),
      hit_length: dv.getFloat32(o + 0x30, true), bounce_speed: dv.getFloat32(o + 0x34, true) });
  }
  return out;
}
function magiFromJson(j) {               // projectiles.json of a mod folder (same records, values already resolved)
  const out = new Map();
  for (const r of (j && j.records) || []) out.set(r.id, { ...r, flags: typeof r.flags === 'string' ? parseInt(r.flags, 16) : r.flags });
  return out;
}
/* what a projectile shows (FUN_08AD96F0 / FUN_08AD32A8 / FUN_08AD3350 -> FUN_08AD4284 -> FUN_088AB140): its effect field
   is a bit of the FEPA records' 64-bit group mask (+0x3C | +0x40 << 32), not an id. The game plays a record with that
   bit, +0x34 (engine trigger) 0 and +0x50 0: role +0x48 = 1 is the birth visual, placed once where and as the shot is
   born; role 2 is the body that flies with the shot (B+112). Flags 0x100 hide the birth visual, 0x80 the body, 0x200
   both; a shared-bank effect (common_effect) replaces the body. Several matches: the game picks one at random. */
function fxGroupHas(r, bit) { return bit < 32 ? ((r.mask >>> bit) & 1) === 1 : ((r.maskHi >>> (bit - 32)) & 1) === 1; }
function projVisuals(fx, pr) {
  const out = [];
  if (!pr || pr.effect == null || pr.effect === 0xFFFF || pr.effect >= 64) return out;
  const f = pr.flags >>> 0, of = (role) => fx.fepa.find((r) => !r.trig && !r.r50 && r.role === role && fxGroupHas(r, pr.effect));
  if (!(f & 0x300)) { const a = of(1); if (a) out.push({ ...a, _once: true }); }
  if (!(f & 0x280) && (pr.common_effect == null || pr.common_effect === 0xFFFF || pr.common_effect < 0)) { const b = of(2); if (b) out.push(b); }
  return out;
}
const pv = (r, n, d) => { const v = r.values && r.values[n]; if (!v) return d; const x = (v[0] + v[1]) / 2; return isFinite(x) ? x : d; };

/* ================================================================== drawing (three.js) */
const FX_VS = `attribute vec4 fxcol; uniform float bias; uniform float screen; uniform vec2 uvOff; uniform float psize; varying vec2 vUv; varying vec4 vCol;
void main() {
  vUv = uv + uvOff; vCol = fxcol; gl_PointSize = psize;
  if (screen > 0.5) gl_Position = vec4(position.xy, 0.0, 1.0);
  else { gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); gl_Position.z += bias * gl_Position.w; }
}`;
// GE: lighting with ambient = light-0 ambient = node colour C and the material = vertex colour V (or the flipbook part
// tint): lit = min(1, 2·C·V); MODULATE + FRAGMENT_2X: rgb = min(1, 2·T·lit); alpha = T.a · A2 · V.a, used as 2·alpha
// by the blend factors. Output is premultiplied, so the blend's source factor is ONE.
const FX_FS = `uniform sampler2D map; uniform float useMap; uniform vec3 C; uniform float A2; uniform float lit2; uniform float aMul;
varying vec2 vUv; varying vec4 vCol;
void main() {
  vec3 lit = min(vec3(1.0), lit2 * C * vCol.rgb);
  vec4 T = useMap > 0.5 ? texture2D(map, vUv) : vec4(1.0);
  vec3 rgb = min(vec3(1.0), 2.0 * T.rgb * lit);
  float f = min(1.0, aMul * T.a * A2 * vCol.a);
  gl_FragColor = vec4(rgb * f, f);
}`;
const FXV = { group: null, pool: new Map(), used: new Set(), labels: [], post: [], order: 0, rt: null };
const fxTexCache = new Map(), fxGeoCache = new Map();
function fxNewMat() {
  return new THREE.ShaderMaterial({
    uniforms: { map: { value: null }, useMap: { value: 0 }, C: { value: new THREE.Vector3(1, 1, 1) }, A2: { value: 1 }, lit2: { value: 2 }, aMul: { value: 2 },
      bias: { value: 0 }, screen: { value: 0 }, uvOff: { value: new THREE.Vector2() }, psize: { value: 1 } },
    vertexShader: FX_VS, fragmentShader: FX_FS, transparent: true, depthTest: true, depthWrite: false, side: THREE.DoubleSide, blending: THREE.CustomBlending,
    depthFunc: THREE.LessEqualDepth });
}
/* GE blend word {u8 op, u8 src, u8 dst} + FIX colours (A §4.2): factor 6 / 7 = 2·As / 1−2·As, 10 = FIX */
function fxBlend(m, word, fixB) {
  const op = word & 15, src = (word >> 8) & 15, dst = (word >> 16) & 15;
  m.blending = THREE.CustomBlending;
  m.blendEquation = op === 2 ? THREE.ReverseSubtractEquation : op === 1 ? THREE.SubtractEquation : THREE.AddEquation;
  m.blendSrc = THREE.OneFactor;
  m.blendDst = (dst === 7 || dst === 3) ? THREE.OneMinusSrcAlphaFactor : dst === 10 ? (((fixB >>> 0) & 0xFFFFFF) === 0 ? THREE.ZeroFactor : THREE.OneFactor) : THREE.OneFactor;
  m.blendEquationAlpha = THREE.AddEquation; m.blendSrcAlpha = THREE.ZeroFactor; m.blendDstAlpha = THREE.OneFactor;
  m.uniforms.aMul.value = (src === 6 || src === 7) ? 2 : 1;
}
/* the 0/1/2 blend switch of the trail, flash and screen components: alpha, additive, subtractive */
const fxBlendMode = (mode) => [0x00070600, 0x000A0600, 0x000A0602][mode] || 0x00070600;
function fxMesh(key, geo, kind) {
  let m = FXV.pool.get(key);
  if (m && m.fxKind !== (kind || 'mesh')) { FXV.group.remove(m); m.material.dispose(); FXV.pool.delete(key); m = null; }
  if (!m) {
    m = kind === 'lines' ? new THREE.LineSegments(geo, fxNewMat()) : kind === 'points' ? new THREE.Points(geo, fxNewMat()) : new THREE.Mesh(geo, fxNewMat());
    m.fxKind = kind || 'mesh'; m.matrixAutoUpdate = false; m.frustumCulled = false; FXV.group.add(m); FXV.pool.set(key, m);
  }
  if (m.fxBase && m.material !== m.fxBase) m.material = m.fxBase;         // a shader layer's draw last frame (compshader.js)
  if (m.geometry !== geo) m.geometry = geo;
  m.visible = true; m.renderOrder = 10 + (FXV.order++); FXV.used.add(key);
  const u = m.material.uniforms; u.bias.value = 0; u.screen.value = 0; u.uvOff.value.set(0, 0); u.lit2.value = 2;
  m.material.depthWrite = false; m.material.side = THREE.DoubleSide; m.material.depthTest = true;
  return m;
}
function fxColour(m, C, lit2) {               // C: game colour bytes 0..255 (128 = 1.0)
  const u = m.material.uniforms;
  u.C.value.set((Math.trunc(C[0]) & 255) / 255, (Math.trunc(C[1]) & 255) / 255, (Math.trunc(C[2]) & 255) / 255);
  u.A2.value = Math.min(255, 2 * Math.trunc(C[3])) / 255; u.lit2.value = lit2 == null ? 2 : lit2;
}
function fxTexture(fep, id) {
  const key = id + '|' + (fep.res.TEX.find((r) => r.id === id && r.size) ? fep.idBase : 'lib');
  if (fxTexCache.has(key)) return fxTexCache.get(key);
  let tx = null;
  try {
    const u8 = fxResourceById(fep, 'TEX', id);
    if (u8) { const t = decodeTex(u8); tx = new THREE.DataTexture(t.rgba, t.w, t.h, THREE.RGBAFormat); tx.wrapS = tx.wrapT = THREE.RepeatWrapping; tx.magFilter = tx.minFilter = THREE.LinearFilter; tx.needsUpdate = true; tx.fxImg = t; }
  } catch (e) { tx = null; }
  fxTexCache.set(key, tx);
  return tx;
}
function fxGeo(P, UV, C, I) {
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(P, 3));
  g.setAttribute('uv', new THREE.Float32BufferAttribute(UV, 2));
  g.setAttribute('fxcol', new THREE.Float32BufferAttribute(C, 4));
  if (I) g.setIndex(I);
  return g;
}
function fxCached(fep, kind, index, build) {
  const key = fep.idBase + '|' + kind + '|' + index;
  if (fxGeoCache.has(key)) return fxGeoCache.get(key);
  let out = null;
  try { const u8 = fxResource(fep, kind, index); if (u8) out = build(u8); } catch (e) { console.warn('studio: effect resource', kind, index, e); out = null; }
  fxGeoCache.set(key, out);
  return out;
}
const fxMdl = (fep, i) => fxCached(fep, 'MDL', i, (u8) => {
  const m = decodeMdl(u8);
  return { scale: (fep.res.MDL[i] || {}).scale || 1, parts: m.parts.map((p) => { const tx = p.tex != null ? fxTexture(fep, p.tex) : null; return { ...p, geo: fxGeo(p.P, p.UV, p.C, p.I), tx, texMissing: p.tex != null && !tx }; }) };
});
const fxAnm = (fep, i) => fxCached(fep, 'ANM', i, (u8) => {
  const a = decodeAnm(u8);
  a.parts = a.parts.map((p) => { const tx = a.tex[p.tex] != null ? fxTexture(fep, a.tex[p.tex]) : null; return { ...p, geo: fxGeo(p.P, p.UV, Array.from({ length: p.P.length / 3 }, () => p.tint.map((x) => x / 255)).flat(), p.I), tx, texMissing: a.tex[p.tex] != null && !tx }; });
  return a;
});
const fxVtl = (fep, i) => fxCached(fep, 'VTL', i, decodeVtl);

/* ------------------------------------------------------------------ matrices */
const fxM = () => new THREE.Matrix4();
const fxEuler = new THREE.Euler();
function fxRot(deg, order) { return fxM().makeRotationFromEuler(fxEuler.set(deg[0] * FX_D2R, deg[1] * FX_D2R, deg[2] * FX_D2R, order || 'XYZ')); }
function fxScale3(m, k) { const e = m.elements; for (const i of [0, 1, 2, 4, 5, 6, 8, 9, 10]) e[i] *= k; return m; }
function fxNormAxes(m) {
  const e = m.clone().elements;
  for (const c of [0, 4, 8]) { const l = Math.hypot(e[c], e[c + 1], e[c + 2]) || 1; e[c] /= l; e[c + 1] /= l; e[c + 2] /= l; }
  return fxM().fromArray(e);
}
const fxRot3 = (m) => { const r = m.clone(); r.setPosition(0, 0, 0); return r; };
const fxPos = (m) => new THREE.Vector3().setFromMatrixPosition(m);
const FX_FLIP = [null, fxM().makeScale(-1, 1, -1), fxM().makeScale(1, -1, -1), fxM().makeScale(-1, -1, 1)];   // ANM flip: Ry(π), Rx(π), both
/* channel 14: element matrix T(px,−py,−pz)·R(rx,−ry,−rz)·S(s) (s already ×0.01) */
function fxElMatrix(p, r, s, order) {
  const ord = FX_ORDERS[order];
  const m = ord ? fxRot([r[0], -r[1], -r[2]], ord) : fxM();
  m.scale(new THREE.Vector3(s[0], s[1], s[2]));
  m.setPosition(p[0], -p[1], -p[2]);
  return m;
}
/* draw matrix (A §2.5): mode 0 world B·[k·M3 | M.t]; 1 camera-facing CamRot·diag(instScale)·k·M3 at B·M.t; 2 locked to the
   element's Y axis, turned to the camera */
function fxDrawMat(ctx, B, M, k, mode, instScale) {
  const M3 = fxScale3(fxRot3(M), k), t = fxPos(M);
  if (mode === 1) {
    const W = ctx.camRot.clone().multiply(fxM().makeScale(instScale[0], instScale[1], instScale[2])).multiply(M3);
    return W.setPosition(t.applyMatrix4(B));
  }
  const W = B.clone().multiply(M3.clone().setPosition(t));
  if (mode === 2) {
    const e = W.elements, pos = fxPos(W);
    const lx = Math.hypot(e[0], e[1], e[2]), ly = Math.hypot(e[4], e[5], e[6]), lz = Math.hypot(e[8], e[9], e[10]);
    const up = new THREE.Vector3(e[4], e[5], e[6]).normalize(), toCam = ctx.camPos.clone().sub(pos).normalize();
    const right = new THREE.Vector3().crossVectors(up, toCam); if (right.lengthSq() < 1e-10) return W;
    right.normalize(); const fwd = new THREE.Vector3().crossVectors(right, up);
    return fxM().makeBasis(right.multiplyScalar(lx), up.multiplyScalar(ly), fwd.multiplyScalar(lz)).setPosition(pos);
  }
  return W;
}
/* depth-only bias (bias mode 3): NDC depth of the point moved by a view-space offset, minus that of the point */
function fxBias(ctx, wpos, off) {
  const o = wpos.clone().applyMatrix4(ctx.cam.matrixWorldInverse), b = o.clone().add(off);
  const near = ctx.cam.near || 0.01;
  if (o.z > -near) return 0;
  if (b.z > -near * 1.05) b.z = -near * 1.05;
  return b.clone().applyMatrix4(ctx.cam.projectionMatrix).z - o.clone().applyMatrix4(ctx.cam.projectionMatrix).z;
}

/* ------------------------------------------------------------------ resource draws */
function fxDrawMdl(ctx, key, fep, idx, W, C, bias, uv) {
  const M = fxMdl(fep, idx); if (!M) { ctx.missing++; return; }
  M.parts.forEach((p, pi) => {
    if (p.texMissing) { ctx.missing++; return; }      // its texture is the game's shared one (needs the ISO): not a white slab
    const m = fxMesh(key + '|' + pi, p.geo), u = m.material.uniforms;
    u.map.value = p.tx; u.useMap.value = p.tx ? 1 : 0; fxColour(m, C); fxBlend(m.material, p.word, p.fixB);
    u.bias.value = bias || 0;
    if (uv && p.tx && p.tx.fxImg) u.uvOff.value.set(uv[0] / p.tx.fxImg.w, uv[1] / p.tx.fxImg.h);
    m.material.depthWrite = !(p.flags & 4) || (C[3] > 128 && p.word === 0x00070600);
    m.material.side = p.flags & 8 ? (p.ccw ? THREE.FrontSide : THREE.BackSide) : THREE.DoubleSide;
    m.matrix.copy(W);
  });
}
function fxDrawAnm(ctx, key, fep, idx, clip, W, C, bias) {
  const A = fxAnm(fep, idx); if (!A) { ctx.missing++; return; }
  const seq = clip >= 0 ? A.seqs[clip] : null; if (!seq) return;
  for (const pi of seq) {
    const p = A.parts[pi]; if (!p || p.P.length < 9) continue;
    if (p.texMissing) { ctx.missing++; continue; }
    const m = fxMesh(key + '|' + pi, p.geo), u = m.material.uniforms;
    u.map.value = p.tx; u.useMap.value = p.tx ? 1 : 0; fxColour(m, C); fxBlend(m.material, p.word, p.fixB);
    u.bias.value = bias || 0;
    m.matrix.copy(W);
  }
}

/* ------------------------------------------------------------------ elements */
/* One element = a node started by the spawn schedule (or by a ch18 / ch151 parent), `age` effect frames ago. */
function fxElement(I, node, born, age, extra) {
  const fep = I.fep, nt = nodeTime(node, age, I.sustained, I.stop - born);
  const el = { I, fep, node, born, age, g: nt.g, looping: nt.looping, seed: fxHash(I.seed, node.index, born, extra ? extra.n : 0), posOff: extra ? extra.posOff : null, frozen: extra ? extra.frozen : null, depth: extra ? extra.depth : 0 };
  el.expired = !el.looping && Math.floor(el.g) >= node.duration;
  el.off = fxRandomOffsets(fep, node, fxRng(el.seed ^ 0x5bd1e995));
  return el;
}
const fxElTick = (el, r) => el.I.t0 + el.born + r;               // absolute tick of the element at age r
function fxElTime(el, r) { return nodeTime(el.node, r, el.I.sustained, el.I.stop - el.born).g; }
function fxVal(el, ti, g) { const tr = el.node.tracks[ti]; return tr && isValueChan(tr.chan) ? chanEval(el.fep, tr, g == null ? el.g : g, el.off.get(ti)) : null; }
/* colour (0..255) of colour track ti, with any ch62 flicker that multiplies it */
function fxElColour(el, ti) {
  if (ti == null) return [128, 128, 128, 128];
  const v = fxVal(el, ti); if (!v) return [128, 128, 128, 128];
  const c = v.v.slice();
  for (const tr of el.node.tracks) {
    if (tr.chan !== 62 || tr.deps[0] !== ti) continue;
    const t61 = el.node.tracks[tr.deps[1]]; if (!t61 || t61.chan !== 61) continue;
    const mult = fxEnvelope(el.fep, t61, Math.floor(el.age) + 1, fxRng(el.seed ^ 0x61));
    for (let i = 0; i < 4; i++) c[i] = Math.min(255, Math.max(0, c[i] * mult[i]));
  }
  return c;
}
/* element matrix (+80) at element time g, from the ch14 track `t14` (its links: position, scale, rotation) */
function fxElMat(el, t14, g) {
  if (g == null) g = el.g;
  let p = [0, 0, 0], r = [0, 0, 0], s = [1, 1, 1], order = 0;
  const tr = t14 != null ? el.node.tracks[t14] : null;
  const deps = tr ? tr.deps : el.node.tracks.map((x) => x.index);
  for (const d of deps) {
    const dt = el.node.tracks[d]; if (!dt) continue; const v = fxVal(el, d, g); if (!v) continue;
    if (dt.chan >= 1 && dt.chan <= 3) p = v.v; else if (dt.chan >= 4 && dt.chan <= 6) r = v.v.slice(); else if (dt.chan >= 7 && dt.chan <= 9) s = v.v;
  }
  if (tr) { const sg = fxSeg(tr, Math.floor(g)); if (sg) order = fxB(el.fep, sg.o + 3); }
  const t29 = el.node.byChan.get(29);                              // face along velocity (F §29)
  if (t29 && t29.deps.length >= 1) {
    const vv = fxVal(el, t29.deps[0], g);
    if (vv) { const [vx, vy, vz] = vv.vel; r = r.slice(); r[0] += Math.atan2(-vz, -vy) / FX_D2R; r[2] += Math.atan2(vx, Math.hypot(vy, vz)) / FX_D2R; }
  }
  if (el.posOff) p = [p[0] + el.posOff[0], p[1] + el.posOff[1], p[2] + el.posOff[2]];
  return fxElMatrix(p, r, s, order);
}
const fxCh14 = (el, tr) => { for (const d of tr.deps) { const x = el.node.tracks[d]; if (x && x.chan === 14) return d; } return el.node.byChan.has(14) ? el.node.byChan.get(14).index : null; };
const fxColDep = (el, tr) => { for (const d of tr.deps) { const x = el.node.tracks[d]; if (x && x.chan >= 15 && x.chan <= 17) return d; } return null; };
function fxCh14Bytes(el, t14) { const tr = t14 != null ? el.node.tracks[t14] : null, sg = tr && fxSeg(tr, Math.floor(el.g)); return sg ? [0, 1, 2, 3, 4, 5].map((k) => fxB(el.fep, sg.o + k)) : [0, 0, 0, 0, 0, 0]; }
/* base matrix B of the element at absolute tick T (the instance's, or frozen where a world-space child was spawned) */
function fxElBase(el, T) { return el.frozen ? el.frozen : el.I.place(T); }
function fxElWorld(el, t14, r) { return fxElBase(el, fxElTick(el, r)).clone().multiply(fxElMat(el, t14, fxElTime(el, r))); }

/* draw one element: every drawing component of its node */
function fxDrawElement(ctx, el, key) {
  const { fep, node } = el, I = el.I, B = fxElBase(el, ctx.tick);
  for (const tr of node.tracks) {
    const ch = tr.chan;
    if (ch === 18 || ch === 151) { fxChildren(ctx, el, tr, key); continue; }
    if (ch === 21) { fxParticles(ctx, el, tr, key + '|t' + tr.index); continue; }
    if (el.expired || node.kind === 255) continue;
    const sg = fxSeg(tr, Math.floor(el.g)); if (!sg) continue;
    const o = sg.o, k = key + '|t' + tr.index;
    if (ch === 10 || ch === 11 || ch === 20) {
      const t14 = fxCh14(el, tr), b14 = fxCh14Bytes(el, t14), C = fxElColour(el, fxColDep(el, tr)).map((c, i) => c * I.colour[i]);
      let M = fxElMat(el, t14);
      if (ch === 10) {                                               // flipbook (C §2)
        const idx = fxI(fep, o + 12); if (idx < 0) continue;
        const A = fxAnm(fep, idx); if (!A) { ctx.missing++; continue; }
        const rnd = fxRng(el.seed ^ 0x10);
        const T = (Math.floor(el.age) + (fxH(fep, o + 24) ? rnd() * fxH(fep, o + 24) : 0)) * fxF(fep, o + 16);
        const fm = fxB(fep, o + 26), flip = [0, rnd() < 0.5 ? 0 : 1, rnd() < 0.5 ? 0 : 2, Math.trunc(rnd() * 255) >> 6][fm] || 0;
        if (flip) M = M.clone().multiply(FX_FLIP[flip]);
        const W = fxDrawMat(ctx, B, M, 0.1, b14[1], I.iscale);
        const bias = fxB(fep, o + 29) ? fxBias(ctx, fxPos(W), new THREE.Vector3(fxF(fep, o), -fxF(fep, o + 4), -fxF(fep, o + 8))) : 0;
        fxDrawAnm(ctx, k, fep, idx, anmClip(A, T), W, C, bias);
      } else {                                                       // mesh (B §2) / mesh with scrolling UVs (E §1i)
        const idx = fxI(fep, o + 12); if (idx < 0) continue;
        const ref = fep.res.MDL[idx]; const W = fxDrawMat(ctx, B, M, ref ? ref.scale : 1, b14[1], I.iscale);
        const biasOn = ch === 11 ? fxB(fep, o + 24) : fxB(fep, o + 72);
        const bias = biasOn ? fxBias(ctx, fxPos(W), new THREE.Vector3(fxF(fep, o), -fxF(fep, o + 4), -fxF(fep, o + 8))) : 0;
        let uv = null;
        if (ch === 20) {
          const tau = el.g - sg.es, rnd = fxRng(el.seed ^ 0x20), rz = fxB(fep, o + 74);
          const ru = rz ? fxR(rnd, fxF(fep, o + 48)) : 0, rv = rz ? fxR(rnd, fxF(fep, o + 52)) : 0;
          uv = [fxF(fep, o + 16) + ru + fxF(fep, o + 24) * tau + fxF(fep, o + 32) * tau * tau / 2, fxF(fep, o + 20) + rv + fxF(fep, o + 28) * tau + fxF(fep, o + 36) * tau * tau / 2];
        }
        fxDrawMdl(ctx, k, fep, idx, W, C, bias, uv);
      }
    } else if (ch === 105) fxTrail(ctx, el, tr, k);
    else if (ch === 107) fxFlash(ctx, el, tr, sg, k);
    else if (ch === 110) fxZoomBlur(ctx, el, tr, sg);
    else if (ch === 101) fxShockwave(ctx, el, tr, sg, k);
    else if (ch === 111) fxRotBlur(ctx, el, tr, sg);
    else if (ch === 100) fxStreaks100(ctx, el, tr, k);
    else if (ch === 102 || ch === 103) fxLines(ctx, el, tr, k);
    else if (ch === 95) fxPoints95(ctx, el, tr, k);
  }
}

/* ------------------------------------------------------------------ child nodes: ch18 (rate) and ch151 (burst) */
function fxChildren(ctx, el, tr, key) {
  if (el.depth >= 2) return;
  const { fep, node } = el, body = el.I.body, rnd = fxRng(el.seed ^ (0x18 + tr.index)), spawns = [];
  let acc = 0, lastSeg = null;
  const steps = Math.floor(el.age);
  for (let r = 0; r <= steps; r++) {
    const g = fxElTime(el, r); if (!(el.node.flags & 2) && Math.floor(g) >= node.duration) break;
    const sg = fxSeg(tr, Math.floor(g)); if (!sg) break;
    const o = sg.o, at = () => { const pv = tr.deps.length ? fxVal(el, tr.deps[0], g) : null; return pv ? pv.v : [0, 0, 0]; };
    if (tr.chan === 18) {
      const rate = fxF(fep, o), ni = fxS(fep, o + 16); if (ni < 0 || fxS(fep, o + 20) < 0) continue;
      acc += rate * 0.5;
      if (acc >= 1) {
        const n = Math.trunc(acc); acc = acc - n - rate * fxH(fep, o + 18);
        for (let i = 0; i < n; i++) {
          if (fxB(fep, o + 23) < 100 && Math.trunc(rnd() * 100) >= fxB(fep, o + 23)) continue;
          const j = fxF(fep, o + 4), p = at();
          spawns.push({ r, ni, off: [p[0] + fxR(rnd, j), p[1] + fxR(rnd, j), p[2] + fxR(rnd, j)] });
        }
      }
    } else {                                                         // 151: at the start and at every new segment
      if (sg === lastSeg) continue; lastSeg = sg;
      const ni = fxS(fep, o + 16); if (ni < 0 || (ni & 0x1000) || fxS(fep, o + 18) < 0) continue;
      const n = Math.floor(fxF(fep, o)), j = fxF(fep, o + 4), p = at();
      for (let i = 0; i < n; i++) spawns.push({ r, ni, off: [p[0] + fxR(rnd, j), p[1] + fxR(rnd, j), p[2] + fxR(rnd, j)] });
    }
  }
  spawns.slice(-48).forEach((s, si) => {
    const cn = body.nodes[s.ni]; if (!cn || cn === node) return;
    const world = [2, 6, 7, 8, 9].includes(cn.kind);
    const born = el.born + s.r, age = el.age - s.r;
    const ch = fxElement(el.I, cn, born, age, { n: si + 1, posOff: s.off, frozen: world ? el.I.place(el.I.t0 + born) : null, depth: el.depth + 1 });
    if (ch.expired && !(cn.flags & 1) && !cn.byChan.has(21)) return;
    fxDrawElement(ctx, ch, `${key}|c${tr.index}.${si}`);
  });
  ctx.approx.add('child nodes');
}

/* ------------------------------------------------------------------ ch21 particles (D), replayed from the element's start */
const fxV = (x, y, z) => new THREE.Vector3(x, y, z);
const fxFv = (v) => [v[0], -v[1], -v[2]];
function fxLookAt(axis) {
  const a = axis.slice(); if (a[0] === 0 && a[1] === 0) a[0] = 1e-6;
  const Z = fxV(...a).normalize(), up = (a[0] === 0 && a[2] === 0) ? fxV(0, 0, 1) : fxV(0, 1, 0);
  const X = fxV(...a).cross(up).normalize(), Y = Z.clone().cross(X);
  return fxM().makeBasis(X, Y, Z);
}
function fxParticleSpawn(P, age0, rnd, el, W, kind, nextSeq) {
  const world = (P.space === 7 && [0, 3, 4].includes(kind)) || [2, 6, 7, 8, 9].includes(kind);
  const p = { age: age0, anmTime: age0, anmStep: 0, flip: 0 };
  if (P.flip === 1) p.flip = rnd() * 256 < 128 ? 0 : 1;
  else if (P.flip === 2) p.flip = rnd() * 256 < 128 ? 0 : 2;
  else if (P.flip === 3) p.flip = Math.trunc(rnd() * 255) >> 6;
  const axis = P.space === 7 ? fxFv(P.axis) : P.axis.slice();
  const L = fxLookAt(axis);
  const cone = (ring) => { const ax = ring ? 0 : fxR(rnd, P.spread), ay = fxR(rnd, P.spread), az = ring ? 0 : fxR(rnd, P.spread); return L.clone().multiply(fxM().makeRotationX(ax * FX_D2R)).multiply(fxM().makeRotationY(ay * FX_D2R)).multiply(fxM().makeRotationZ(az * FX_D2R)); };
  const dist = () => !P.useDist ? rnd() * P.box[0] : P.distMode === 0 ? P.box[0] - P.box[0] * (P.distPct / 100) * rnd() : P.distMode === 1 ? P.box[0] * (P.distPct / 100) * rnd() : P.spread;
  let pos = null;
  switch (P.shape) {
    case 0: case 2: pos = fxV(fxR(rnd, P.box[0]), fxR(rnd, P.box[1]), fxR(rnd, P.box[2])); break;
    case 1: case 3: { const M = cone(false); pos = fxV(0, 0, dist()).applyMatrix4(M); break; }
    case 6: case 7: { const M = cone(true); pos = fxV(0, 0, dist()).applyMatrix4(M); break; }
    case 8: case 9: {
      const vt = P.vtl >= 0 ? fxVtl(el.fep, P.vtl) : null, set = vt && vt.lists[P.vtlSet];
      if (set && set.length) { const k = P.vtxOrder === 2 ? set[nextSeq() % set.length] : set[Math.trunc(rnd() * (set.length - 1))]; const v = vt.verts[k] || [0, 0, 0]; const sc = (el.fep.res.MDL[P.mdl] || {}).scale || 1; pos = fxV(v[0] * sc, v[1] * sc, v[2] * sc); }
      else pos = fxV(0, 0, 0);
      break;
    }
    default: pos = fxV(0, 0, 0);
  }
  let radial;
  if (world) { const wp = pos.clone().applyMatrix4(W); radial = wp.clone().sub(fxPos(W)).normalize(); pos = wp; }
  else radial = pos.clone().normalize();
  let spd = P.useSpeed ? P.speed : 0, acc = P.useSpeed ? P.accel : 0;
  if (P.rndSpeed) { spd += fxR(rnd, P.speedRand); acc += fxR(rnd, P.accelRand); }
  let dir;
  if ([0, 1, 6, 8].includes(P.shape)) {
    let M = cone(false);
    if (P.space === 7 && [0, 3, 4].includes(kind)) M = fxRot3(W).multiply(M);
    dir = fxV(1e-5, 0, 1).applyMatrix4(fxRot3(M));
  } else if ([2, 3, 7, 9].includes(P.shape)) dir = radial; else dir = fxV(0, 0, 0);
  p.p0 = pos; p.v = dir.clone().multiplyScalar(spd); p.a = dir.clone().multiplyScalar(acc);
  let fd = P.space === 7 ? fxFv(P.forceDir) : P.forceDir.slice();
  let fs = P.useForce ? P.fSpeed : 0, fa = P.useForce ? P.fAccel : 0;
  if (P.rndForce) { fd = fd.map((c, i) => c + fxR(rnd, P.forceDirRand[i])); fs += fxR(rnd, P.fSpeedRand); fa += fxR(rnd, P.fAccelRand); }
  const fn = fxV(...fd); if (fn.lengthSq() > 0) fn.normalize();
  p.v.addScaledVector(fn, fs); p.a.addScaledVector(fn, fa);
  p.s = P.useScale ? P.scale.slice() : [100, 100]; p.sv = P.useScale ? P.scaleVel.slice() : [0, 0]; p.sa = P.useScale ? P.scaleAcc.slice() : [0, 0];
  if (P.rndScale) {
    if (P.scaleRandMode === 0) { const d0 = fxR(rnd, P.scaleRand[0]), d1 = fxR(rnd, P.scaleVelRand[0]), d2 = fxR(rnd, P.scaleAccRand[0]); for (const i of [0, 1]) { p.s[i] += d0; p.sv[i] += d1; p.sa[i] += d2; } }
    else for (const i of [0, 1]) {
      p.s[i] += p.s[i] * fxR(rnd, P.scaleRand[i]) * 0.01; p.sv[i] += p.sv[i] * fxR(rnd, P.scaleVelRand[i]) * 0.01; p.sa[i] += p.sa[i] * fxR(rnd, P.scaleAccRand[i]) * 0.01;
      p.s[i] += fxR(rnd, P.scaleRand[i]); p.sv[i] += fxR(rnd, P.scaleVelRand[i]); p.sa[i] += fxR(rnd, P.scaleAccRand[i]);
    }
  }
  p.r = P.useRot ? P.rot.slice() : [0, 0, 0];
  if (P.rndRot) for (const k of [0, 1, 2]) p.r[k] += fxR(rnd, P.rotRand[k]);
  p.c = P.useCol ? P.col.slice() : [128, 128, 128, 128]; p.cr = P.useCol ? P.colRate.slice() : [0, 0, 0, 0];
  if (P.rndCol) for (const k of [0, 1, 2, 3]) { p.c[k] += fxR(rnd, P.colRand[k]); p.cr[k] += fxR(rnd, P.colRateRand[k]); }
  return p;
}
function fxParticles(ctx, el, tr, key) {
  const { fep, node } = el, rnd = fxRng(el.seed ^ 0x21), parts = [];
  const t14 = fxCh14(el, tr);
  let st = null, acc = 0, seq = 0, lastSeg = null, emitEnd = Infinity;
  if (el.expired && !(node.flags & 1)) return;
  const steps = Math.floor(el.age);
  for (let f = 0; f <= steps; f++) {
    const g = fxElTime(el, f), alive = el.looping || Math.floor(g) < node.duration || f === 0;
    if (!alive && emitEnd === Infinity) emitEnd = f;
    const sg = fxSeg(tr, Math.floor(g)); if (!sg) continue;
    const P = fxP21(fep, sg.o);
    if (sg !== lastSeg && !(lastSeg && sg.start < lastSeg.start)) {            // segment start (not a loop re-seek)
      st = { P, age: 0, life: P.life, endT: P.rate <= 0 ? 0 : (P.rateAcc < 0 ? P.rate / -P.rateAcc : -1), fIn: 1, fOutStart: 0, fOut: 0 };
      if (P.fade) { const n = P.life, a = P.fadeIn, b = P.fadeOut;
        if (a + b < n) { st.fOutStart = n - b; st.fOut = b > 0 ? 1 / b : 0; st.fIn = a > 0 ? 1 / a : 1; }
        else if (b < n) { st.fOutStart = n - b; st.fIn = 1 / (n - b); st.fOut = b > 0 ? 1 / b : 0; }
        else { st.fOutStart = 0; st.fIn = 0; st.fOut = b ? 1 / b : 0; } }
    } else if (st) st.P = P;
    lastSeg = sg;
    if (!st) continue;
    const Pc = st.P;
    st.age += 1;
    const emitting = f < emitEnd && !(st.endT === 0 || (st.endT > 0 && st.endT < st.age));
    if (emitting) {
      const amount = Pc.rate + Pc.rateAcc / 2;
      if (amount > 0) {
        acc += amount;
        if (acc >= 1) {
          const n = Math.trunc(acc), step = 1 / acc;
          acc -= n; acc -= amount * Pc.interval + Pc.rateAcc / 2 * Pc.interval * Pc.interval;
          let W = null;
          for (let i = n; i >= 1; i--) {
            if (Pc.prob < 100 && Math.trunc(rnd() * 100) >= Pc.prob) continue;
            if (parts.length >= Pc.maxP) break;
            if (!W) W = fxElWorld(el, t14, f);
            parts.push(fxParticleSpawn(Pc, i * step - 1, rnd, el, W, node.kind, () => seq++));
          }
        }
      }
    }
    if (Pc.anm < 0) continue;
    const A = fxAnm(fep, Pc.anm); if (!A) { ctx.missing++; return; }
    let dead = 0;
    for (const p of parts) {
      if (st.life <= p.age) { dead++; continue; }
      let t = p.anmTime * Pc.anmSpeed;
      if (t >= A.total) { if (!A.loop || !A.total) { dead++; continue; } t -= Math.trunc(Math.trunc(t) / Math.trunc(A.total)) * A.total; p.anmTime = t; }
      let s = 0, cum = A.clips[0] || 1;
      while (!(t < cum) && s < A.clips.length - 1) { s++; cum += A.clips[s]; }
      p.anmStep = s; p.age += 1; p.anmTime += 1;
    }
    parts.splice(0, dead);
  }
  if (!st || !parts.length) return;
  const P = st.P, Wnow = fxElWorld(el, t14, el.age), frac = el.age - steps, I = el.I;
  parts.forEach((p, pi) => {
    const t = p.age + frac; if (t >= st.life || t < 0) return;
    let pos = p.p0.clone().addScaledVector(p.v, t).addScaledVector(p.a, t * t / 2);
    if (!(P.space === 2 || P.space === 6 || P.space === 7)) pos = ((P.shape === 8 || P.shape === 9) ? pos : fxV(pos.x, -pos.y, -pos.z)).applyMatrix4(Wnow);
    const sx = (p.s[0] + p.sv[0] * t + p.sa[0] * t * t / 2) * 0.001 * I.iscale[0], sy = (p.s[1] + p.sv[1] * t + p.sa[1] * t * t / 2) * 0.001 * I.iscale[1], sz = 0.1 * I.iscale[2];
    const ang = -(p.r[0] + p.r[1] * t + p.r[2] * t * t / 2) * FX_D2R;
    const M3 = ctx.camRot.clone().multiply(fxM().makeRotationZ(ang));
    if (p.flip) M3.multiply(FX_FLIP[p.flip]);
    M3.multiply(fxM().makeScale(sx, sy, sz)).setPosition(pos);
    const c = p.c.map((v, k) => v + p.cr[k] * t);
    const fa = t < st.fOutStart ? st.fIn * t : 1 - st.fOut * (t - st.fOutStart);
    c[3] = fa < 0 ? 0 : fa < 1 ? c[3] * fa : c[3];
    const rgba = c.map((v, k) => Math.min(255, Math.max(0, v * I.colour[k])));
    const bias = P.useZbias && P.zbias ? fxBias(ctx, pos, fxV(0, 0, -P.zbias)) : 0;
    fxDrawAnm(ctx, key + '|p' + pi, fep, P.anm, p.anmStep, M3, rgba, bias);
  });
}

/* ------------------------------------------------------------------ ch106 + ch105 weapon trail (E §1a/1b) */
function fxBasis(knots, i, k, x) {                // Cox–de Boor
  if (k === 0) return (knots[i] <= x && x < knots[i + 1]) ? 1 : 0;
  const d1 = knots[i + k] - knots[i], d2 = knots[i + k + 1] - knots[i + 1];
  return (d1 ? (x - knots[i]) / d1 * fxBasis(knots, i, k - 1, x) : 0) + (d2 ? (knots[i + k + 1] - x) / d2 * fxBasis(knots, i + 1, k - 1, x) : 0);
}
function fxTrail(ctx, el, tr, key) {
  const { fep, node } = el, I = el.I;
  const t106 = tr.deps.map((d) => node.tracks[d]).find((x) => x && x.chan === 106); if (!t106) return;
  const s106 = fxSeg(t106, Math.floor(el.g)), s105 = fxSeg(tr, Math.floor(el.g)); if (!s106 || !s105) return;
  const o6 = s106.o, o5 = s105.o, n = Math.max(2, fxS(fep, o6 + 12));
  const A = fxV(fxF(fep, o6), -fxF(fep, o6 + 4), -fxF(fep, o6 + 8)), Bp = fxV(fxF(fep, o6 + 16), -fxF(fep, o6 + 20), -fxF(fep, o6 + 24));
  const t14 = fxCh14(el, t106);
  const S = [], fl = Math.floor(el.age);
  for (let i = 0; i < n; i++) {
    const r = i === 0 ? el.age : Math.max(0, fl - i);
    const W = fxElWorld(el, t14, r);
    S.push([A.clone().applyMatrix4(W), Bp.clone().applyMatrix4(W)]);
  }
  const c0 = [0, 1, 2, 3].map((k) => fxF(fep, o5 + 4 * k)), c1 = [0, 1, 2, 3].map((k) => fxF(fep, o5 + 16 + 4 * k));
  const texIdx = fxI(fep, o5 + 44), textured = fxB(fep, o5 + 72), ref = fep.res.TEX[texIdx];
  const tx = textured && ref ? fxTexture(fep, ref.id) : null, TW = tx && tx.fxImg ? tx.fxImg.w : 128, TH = tx && tx.fxImg ? tx.fxImg.h : 128;
  const uw = fxH(fep, o5 + 68), vh = fxH(fep, o5 + 70), alongU = fxB(fep, o5 + 76), dA = fxB(fep, o5 + 77), dC = fxB(fep, o5 + 78);
  const tau = el.g - s105.es, wrap = (x, m) => ((x % m) + m) % m;
  const su = wrap(fxH(fep, o5 + 64) - fxF(fep, o5 + 48) * tau, TW), sv = wrap(fxH(fep, o5 + 66) - fxF(fep, o5 + 52) * tau, TH);
  const fadeLen = fxF(fep, o5 + 56) * 0.01 * (n - 1);
  let fade = fadeLen === 0 ? 1 : 0;
  const rows = [];
  for (let i = 0; i < n; i++) {
    const k = i / (n - 1), tw = 1 - k, m = S[i][0].clone().lerp(S[i][1], 0.5);
    const Pp = m.clone().lerp(S[i][0], tw), Qp = m.clone().lerp(S[i][1], tw);
    const c = [0, 1, 2, 3].map((j) => Math.min(255, Math.max(0, (c0[j] + (c1[j] - c0[j]) * k) * I.colour[j])));
    c[3] *= Math.min(1, fade); fade = Math.min(1, fade) + (fadeLen ? 1 / fadeLen : 0);
    const along = i / n; let uvP, uvQ;
    if (alongU) { const u = dA ? su + along * uw : su + uw - along * uw; uvP = [u / TW, dC ? sv / TH : (sv + vh) / TH]; uvQ = [u / TW, dC ? (sv + vh) / TH : sv / TH]; }
    else { const v = dC ? sv + along * vh : sv + vh - along * vh; uvP = [dA ? (su + uw) / TW : su / TW, v / TH]; uvQ = [dA ? su / TW : (su + uw) / TW, v / TH]; }
    rows.push({ P: Pp, Q: Qp, uvP, uvQ, c: c.map((x) => x / 255) });
  }
  // clamped cubic B-spline along the rows (GE spline, edges open/open), `div` steps per span
  const div = Math.max(1, fxB(fep, o5 + 84)), spans = Math.max(1, n - 3), deg = Math.min(3, n - 1), out = [];
  const knots = []; for (let i = 0; i <= deg; i++) knots.push(0); for (let i = 1; i < n - deg; i++) knots.push(i); for (let i = 0; i <= deg; i++) knots.push(n - deg);
  const kmax = n - deg;
  for (let s = 0; s <= spans * div; s++) {
    const x = Math.min(s / (spans * div) * kmax, kmax - 1e-6), w = rows.map((_, j) => fxBasis(knots, j, deg, x));
    const mixV = (get) => rows.reduce((a, r, j) => a.addScaledVector(get(r), w[j]), fxV(0, 0, 0));
    const mixA = (get, len) => rows.reduce((a, r, j) => { const v = get(r); for (let q = 0; q < len; q++) a[q] += v[q] * w[j]; return a; }, Array(len).fill(0));
    out.push({ P: mixV((r) => r.P), Q: mixV((r) => r.Q), uvP: mixA((r) => r.uvP, 2), uvQ: mixA((r) => r.uvQ, 2), c: mixA((r) => r.c, 4) });
  }
  const Pa = [], UV = [], Ca = [], Ix = [];
  out.forEach((r, i) => {
    Pa.push(r.P.x, r.P.y, r.P.z, r.Q.x, r.Q.y, r.Q.z); UV.push(...r.uvP, ...r.uvQ); Ca.push(...r.c, ...r.c);
    if (i) { const b = 2 * i; Ix.push(b - 2, b - 1, b, b, b - 1, b + 1); }
  });
  const old = FXV.pool.get(key); if (old && old.geometry) old.geometry.dispose();
  const m = fxMesh(key, fxGeo(Pa, UV, Ca, Ix)), u = m.material.uniforms;
  u.map.value = tx; u.useMap.value = tx ? 1 : 0; u.C.value.set(1, 1, 1); u.A2.value = 1; u.lit2.value = 1;
  const mode = fxB(fep, o5 + 81); fxBlend(m.material, fxBlendMode(mode), 0xFFFFFF); u.aMul.value = 1;
  m.matrix.identity();
}

/* ------------------------------------------------------------------ screen components (F) */
let fxQuadGeo = null;
function fxFlash(ctx, el, tr, sg, key) {                     // ch107: flat colour over the screen (or a rect of it)
  const { fep } = el, o = sg.o, C = fxElColour(el, fxColDep(el, tr)).map((c, i) => Math.min(255, c * el.I.colour[i]));
  if (C[3] <= 0) return;
  let x0 = 0, y0 = 0, x1 = 480, y1 = 272;
  if (fxB(fep, o + 25)) { x0 = fxI(fep, o); y0 = fxI(fep, o + 4); x1 = Math.min(x0 + fxI(fep, o + 8), 480); y1 = Math.min(y0 + fxI(fep, o + 12), 272); }
  const X = (x) => x / 480 * 2 - 1, Y = (y) => 1 - y / 272 * 2;
  const geo = fxGeo([X(x0), Y(y0), 0, X(x1), Y(y0), 0, X(x0), Y(y1), 0, X(x1), Y(y1), 0], [0, 0, 1, 0, 0, 1, 1, 1], Array(16).fill(1), [0, 1, 2, 2, 1, 3]);
  const old = FXV.pool.get(key); if (old && old.geometry) old.geometry.dispose();
  const m = fxMesh(key, geo), u = m.material.uniforms;
  u.screen.value = 1; u.map.value = null; u.useMap.value = 0;
  u.C.value.set(C[0] / 510, C[1] / 510, C[2] / 510); u.lit2.value = 1; u.A2.value = Math.min(255, 2 * C[3]) / 255;   // flat colour, not doubled
  fxBlend(m.material, fxBlendMode(fxB(fep, o + 24)), 0xFFFFFF); u.aMul.value = 1;
  m.material.depthTest = false; m.renderOrder = 100000 + FXV.order;
}
function fxZoomBlur(ctx, el, tr, sg) {                      // ch110: the frame redrawn magnified about a point
  const { fep, node } = el, o = sg.o;
  const t109 = tr.deps.map((d) => node.tracks[d]).find((x) => x && x.chan === 109); if (!t109) return;
  const s9 = fxSeg(t109, Math.floor(el.g)); if (!s9) return;
  const s = fxF(fep, s9.o) + fxF(fep, s9.o + 4) * (el.g - s9.es);
  const C = fxElColour(el, fxColDep(el, tr)).map((c, i) => Math.min(255, c * el.I.colour[i])); if (C[3] <= 0) return;
  let cx, cy;
  if (fxB(fep, o + 12)) { cx = fxS(fep, o) / 480; cy = 1 - fxS(fep, o + 2) / 272; }
  else {
    const W = fxElWorld(el, fxCh14(el, tr), el.age), p = fxPos(W).project(ctx.cam);
    if (p.z > 1) return; cx = (p.x + 1) / 2; cy = (p.y + 1) / 2;
  }
  FXV.post.push({ kind: 'zoom', c: [Math.min(1, Math.max(0, cx)), Math.min(1, Math.max(0, cy))], k: 2 * s / 480, rgba: C, blend: fxB(fep, o + 17), passes: Math.max(1, Math.min(4, fxB(fep, o + 19))) });
}
function fxShockwave(ctx, el, tr, sg, key) {                // ch101: refraction ring sampling the frame (field split guessed, F §101)
  const { fep } = el, o = sg.o, tau = el.g - sg.es;
  const R = fxF(fep, o + 12) + fxF(fep, o + 28) * tau + fxF(fep, o + 44) * tau * tau / 2;
  if (!(R > 0)) return;
  const C = fxElColour(el, fxColDep(el, tr)).map((c, i) => Math.min(255, c * el.I.colour[i])); if (C[3] <= 0) return;
  const W = fxElWorld(el, fxCh14(el, tr), el.age), pos = fxPos(W);
  const e = W.elements, sc = Math.max(Math.hypot(e[0], e[1], e[2]), 1e-4);
  FXV.post.push({ kind: 'ring', pos, r: R * sc * el.I.iscale[0], inner: Math.max(0, Math.min(0.99, fxF(fep, o + 68))), aIn: fxF(fep, o + 60) / 128, aOut: fxF(fep, o + 64) / 128,
    seg: Math.max(8, fxB(fep, o + 83) * 2), rgba: C, blend: fxB(fep, o + 79) });
  ctx.approx.add('shockwave');
}
/* ------------------------------------------------------------------ ch111 screen shift, line / point emitters (docs/archive/fx-components/G.md) */
function fxRotBlur(ctx, el, tr, sg) {                      // ch111: the frame redrawn shifted by V pixels (G §2)
  const { fep, node } = el, o = sg.o, dep = (c0, c1) => tr.deps.find((d) => { const x = node.tracks[d]; return x && x.chan >= c0 && x.chan <= c1; });
  const d109 = dep(109, 109); if (d109 == null) return;
  const s9 = fxSeg(node.tracks[d109], Math.floor(el.g)); if (!s9) return;
  const s = fxF(fep, s9.o) + fxF(fep, s9.o + 4) * (el.g - s9.es);          // ch109 value = pixels
  const dr = dep(4, 6), rz = dr != null ? ((fxVal(el, dr) || { v: [0, 0, 0] }).v[2] || 0) * FX_D2R : 0;
  const C = fxElColour(el, fxColDep(el, tr)).map((c, i) => Math.min(255, Math.max(0, c * el.I.colour[i]))); if (C[3] <= 0) return;
  const x = fxF(fep, o), y = fxF(fep, o + 4), vx = Math.cos(rz) * x - Math.sin(rz) * y, vy = Math.sin(rz) * x + Math.cos(rz) * y;
  const L = Math.hypot(vx, vy); if (!(L > 0) || !s) return;
  FXV.post.push({ kind: 'shift', d: [vx / L * s, vy / L * s], rgba: C, blend: fxB(fep, o + 12),
    passes: Math.max(1, Math.min(4, fxB(fep, o + 14))), feedback: fxB(fep, o + 13) === 1 });
}

/* line / point components: element-local vertices through the element matrix (08946AC4, scale 1, ch14 billboard), like MDL */
function fxPrimWorld(ctx, el, tr) {
  const t14 = fxCh14(el, tr), b14 = fxCh14Bytes(el, t14);
  return fxDrawMat(ctx, fxElBase(el, ctx.tick), fxElMat(el, t14), 1, b14[1], el.I.iscale);
}
const fxPrimK = (el, tr) => fxElColour(el, fxColDep(el, tr)).map((c, i) => c * el.I.colour[i]);   // node colour × instance (0..255)
const fxSat = (x) => Math.min(1, Math.max(0, x));
const fxV3 = { add: (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]], sub: (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
  mul: (a, k) => [a[0] * k, a[1] * k, a[2] * k], len: (a) => Math.hypot(a[0], a[1], a[2]),
  norm: (a) => { const l = Math.hypot(a[0], a[1], a[2]); return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0]; } };
/* acc += rate·dt; at >= 1: n = trunc(acc), acc -= n + pause·rate (95/100/102/103) */
function fxEmitN(st, rate, pause) { st.acc += rate; if (st.acc < 1) return 0; const n = Math.trunc(st.acc); st.acc -= n + pause * rate; return n; }
function fxSlot(pool, max) { const s = pool.find((x) => !x.alive); if (s) return s; if (pool.length >= max) return null; pool.push({}); return pool[pool.length - 1]; }
/* the parameter block of track tr at element age f (the emitters read the current segment every tick) */
const fxSegAt = (el, tr, f) => fxSeg(tr, Math.floor(fxElTime(el, f)));
function fxPrimDraw(ctx, key, W, P, Ca, kind, blend) {
  if (!P.length) return;
  const old = FXV.pool.get(key); if (old && old.geometry) old.geometry.dispose();
  const m = fxMesh(key, fxGeo(P, Array(P.length / 3 * 2).fill(0), Ca, null), kind), u = m.material.uniforms;
  u.map.value = null; u.useMap.value = 0; u.C.value.set(0.5, 0.5, 0.5); u.lit2.value = 1; u.A2.value = 1;   // vertex colour as is
  fxBlend(m.material, fxBlendMode(blend), 0xFFFFFF);                // aMul = 2: the GE's 2·alpha
  if (kind === 'points') u.psize.value = Math.max(1, ((ctx.view && ctx.view.renderer && ctx.view.renderer.domElement.height) || 272) / 272);
  m.matrix.copy(W);
}

function fxStreaks100(ctx, el, tr, key) {                  // G §3.1
  const { fep } = el, rnd = fxRng(el.seed ^ 100), st = { acc: 0, time: 0 }, sp = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + 60)), n = Math.max(1, fxB(fep, i0.o + 66)); if (!max || !fxH(fep, i0.o + 62)) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    st.time += 1;
    for (let k = fxEmitN(st, fxF(fep, o + 52), fxB(fep, o + 67)); k > 0; k--) {
      const s = fxSlot(sp, max); if (!s) break;
      let h = fxF(fep, o + 56) * Math.PI / 360; while (h < -Math.PI) h += 2 * Math.PI; while (h > Math.PI) h -= 2 * Math.PI;
      const a1 = fxR(rnd, h), a2 = fxR(rnd, h), d = [-Math.sin(a1), Math.cos(a1) * Math.cos(a2), Math.cos(a1) * Math.sin(a2)];
      const acc = fxF(fep, o + 40) + fxR(rnd, fxF(fep, o + 44)), v0 = fxF(fep, o + 32) + fxR(rnd, fxF(fep, o + 36));
      const start = fxV3.mul(d, v0 * rnd()), L = fxH(fep, o + 62);
      Object.assign(s, { alive: true, d, v0, acc, start, born: st.time, life: L + Math.trunc(fxR(rnd, fxH(fep, o + 64))), half: L >> 1, a: 1,
        pts: Array.from({ length: n }, () => start.slice()),
        c0: [0, 1, 2, 3].map((j) => fxF(fep, o + 4 * j) / 128), c1: [0, 1, 2, 3].map((j) => fxF(fep, o + 16 + 4 * j) / 128) });
    }
    const G = fxF(fep, o + 48);
    for (const s of sp) {
      if (!s.alive) continue;
      if (--s.life <= 0) { s.alive = false; continue; }
      for (let j = n - 1; j > 0; j--) s.pts[j] = s.pts[j - 1];
      const t = st.time - s.born, T = t * (t + 1) / 2;
      s.pts[0] = [0, 1, 2].map((j) => s.start[j] + s.d[j] * s.v0 * t + (s.d[j] * s.acc - (j === 1 ? G : 0)) * T);
      if (s.life < s.half && s.half > 0) s.a = Math.max(0, s.a - 1 / s.half);
    }
  }
  const K = fxPrimK(el, tr).map((c) => Math.min(255, Math.max(0, c)) / 255), P = [], Ca = [];
  for (const s of sp) if (s.alive) for (let j = 0; j + 1 < n; j++) for (const q of [j, j + 1]) {   // strip -> segments
    P.push(...s.pts[q]);
    Ca.push(...[0, 1, 2, 3].map((c) => fxSat(K[c] * (c === 3 ? s.a : 1) * (s.c0[c] + (s.c1[c] - s.c0[c]) * q / n))));
  }
  const sg = fxSeg(tr, Math.floor(el.g)); fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'lines', sg ? fxB(fep, sg.o + 68) : 1);
}

function fxLines(ctx, el, tr, key) {                       // ch102 (G §3.2) and ch103 (G §3.3)
  const { fep } = el, is103 = tr.chan === 103, rnd = fxRng(el.seed ^ tr.chan), st = { acc: 0 }, Ls = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + (is103 ? 44 : 64))); if (!max || !fxH(fep, i0.o + (is103 ? 46 : 66))) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    const rate = fxF(fep, o + (is103 ? 28 : 60)), pause = fxB(fep, o + (is103 ? 62 : 70));
    for (let k = fxEmitN(st, rate, pause); k > 0; k--) {
      const ln = fxSlot(Ls, max); if (!ln) break;
      let A, vel, acc, D, L, Lr;
      if (is103) {
        A = [0, 1, 2].map((j) => fxR(rnd, fxF(fep, o + 64 + 4 * j) / 2));
        const sgn = [1, -1, -1];
        vel = [0, 1, 2].map((j) => sgn[j] * fxF(fep, o + 4 * j) + fxR(rnd, fxF(fep, o + 16 + 4 * j)));
        acc = [0, 1, 2].map((j) => sgn[j] * fxF(fep, o + 32 + 4 * j) + fxR(rnd, fxF(fep, o + 48 + 4 * j)));
        D = fxF(fep, o + 12); L = fxH(fep, o + 46); Lr = fxH(fep, o + 60);
      } else {
        const u = fxV3.norm([fxR(rnd, 1), fxR(rnd, 1), fxR(rnd, 1)]), r0 = fxF(fep, o + 52), r1 = fxF(fep, o + 56);
        A = fxV3.mul(u, r0 + rnd() * (r1 - r0)); const inw = fxV3.norm(fxV3.mul(A, -1));
        vel = fxV3.mul(inw, fxF(fep, o + 36) + fxR(rnd, fxF(fep, o + 40)));
        acc = fxV3.mul(inw, fxF(fep, o + 44) + fxR(rnd, fxF(fep, o + 48)));
        D = fxF(fep, o + 32); L = fxH(fep, o + 66); Lr = fxH(fep, o + 68);
      }
      Object.assign(ln, { alive: true, A, B: A.slice(), vel, acc, D, life: L + Math.trunc(fxR(rnd, Lr)), half: L >> 1, fade: 2 / L, a: 1 });
    }
    for (const ln of Ls) {
      if (!ln.alive) continue;
      if (--ln.life <= 0) { ln.alive = false; continue; }
      if (ln.life < ln.half) ln.a = Math.max(0, ln.a - ln.fade);
      if (!is103 && fxV3.len(ln.A) <= fxV3.len(ln.vel)) {    // 102: the head reached the centre
        ln.D -= fxV3.len(ln.vel) - fxV3.len(ln.A); ln.A = [0, 0, 0];
        if (ln.D <= 0) { ln.alive = false; continue; }
      } else ln.A = fxV3.add(ln.A, ln.vel);
      const BA = fxV3.sub(ln.B, ln.A), l = fxV3.len(BA);
      if (ln.D < l) ln.B = fxV3.add(ln.A, fxV3.mul(BA, ln.D / l));
      ln.vel = fxV3.add(ln.vel, ln.acc);
    }
  }
  const sg = fxSeg(tr, Math.floor(el.g)); if (!sg) return; const o = sg.o, K = fxPrimK(el, tr);
  const cols = [is103 ? 80 : 0, is103 ? 96 : 16].map((b) => [0, 1, 2, 3].map((j) => Math.min(255, Math.max(0, K[j] * fxF(fep, o + b + 4 * j) / 128)) / 255));
  const P = [], Ca = [];
  for (const ln of Ls) if (ln.alive) { P.push(...ln.A, ...ln.B); for (const c of cols) Ca.push(c[0], c[1], c[2], fxSat(c[3] * ln.a)); }
  fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'lines', fxB(fep, o + (is103 ? 63 : 71)));
}

function fxPoints95(ctx, el, tr, key) {                    // G §3.4
  const { fep } = el, rnd = fxRng(el.seed ^ 95), st = { acc: 0 }, ps = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + 32)); if (!max || !fxH(fep, i0.o + 34)) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    const hs = [0, 1, 2].map((j) => fxF(fep, o + 4 * j) / 2), lo = hs.map((h) => -h - 1e-4), hi = hs.map((h) => h + 1e-4);
    for (let k = fxEmitN(st, fxF(fep, o + 28), fxB(fep, o + 38)); k > 0; k--) {
      const p = fxSlot(ps, max); if (!p) break; const L = fxH(fep, o + 34);
      Object.assign(p, { alive: true, pos: hs.map((h) => fxR(rnd, h)), dir: fxV3.norm([fxR(rnd, 1), fxR(rnd, 1), fxR(rnd, 1)]),
        v: fxF(fep, o + 12) + fxR(rnd, fxF(fep, o + 16)), dv: fxF(fep, o + 20) + fxR(rnd, fxF(fep, o + 24)),
        life: L + Math.trunc(fxR(rnd, fxH(fep, o + 36))), half: L >> 1, fade: 2 / L, a: 1 });
    }
    for (const p of ps) {
      if (!p.alive) continue;
      if (--p.life <= 0) { p.alive = false; continue; }
      if (p.life < p.half) p.a = Math.max(0, p.a - p.fade);
      let rem = p.v, at = p.pos;
      for (let g = 0; g < 8 && rem > 0; g++) {               // bounce off the box walls (several per tick if needed)
        const q = fxV3.add(at, fxV3.mul(p.dir, rem)); let hit = -1, wall = 0;
        for (const [side, w] of [[0, lo], [1, hi]]) for (let j = 0; j < 3; j++) if (side ? q[j] > w[j] : q[j] < w[j]) { hit = j; wall = w[j]; }
        if (hit < 0) { at = q; break; }
        const r = Math.abs(wall - at[hit]) / Math.max(1e-9, Math.abs(q[hit] - at[hit]));
        const h = fxV3.add(at, fxV3.mul(p.dir, rem * r)); h[hit] = wall;
        rem -= fxV3.len(fxV3.sub(h, at)); at = h; p.dir = p.dir.slice(); p.dir[hit] = -p.dir[hit];
      }
      p.pos = at; p.v += p.dv;
    }
  }
  const K = fxPrimK(el, tr).map((c) => Math.min(255, Math.max(0, c)) / 255), P = [], Ca = [];
  for (const p of ps) if (p.alive) { P.push(...p.pos); Ca.push(K[0], K[1], K[2], fxSat(K[3] * p.a)); }
  const sg = fxSeg(tr, Math.floor(el.g)); fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'points', sg ? fxB(fep, sg.o + 39) : 1);
}


/* the earlier spark approximation (kept for reference; G §3.6 lists what it gets wrong) */
function fxSparks(ctx, el, tr, sg, key) {                   // ch100 streaks / ch102, 103 lines (layouts partly guessed, F)
  const { fep, node } = el, o = sg.o, rnd = fxRng(el.seed ^ tr.chan), I = el.I;
  const is100 = tr.chan === 100;
  const head = [0, 1, 2, 3].map((k) => fxF(fep, o + 4 * k)), tail = is100 ? head.slice(0, 3).concat([0]) : [0, 1, 2, 3].map((k) => fxF(fep, o + 16 + 4 * k));
  const speed = is100 ? fxF(fep, o + 32) : fxF(fep, o + 44), speedR = is100 ? fxF(fep, o + 36) : fxF(fep, o + 48), grav = is100 ? fxF(fep, o + 48) : 0;
  const rate = is100 ? fxF(fep, o + 52) : fxF(fep, o + 60), cone = is100 ? fxF(fep, o + 56) : 360;
  const maxN = Math.min(64, is100 ? fxH(fep, o + 60) : fxH(fep, o + 64)), life = is100 ? fxH(fep, o + 62) : fxH(fep, o + 66), lifeR = is100 ? fxH(fep, o + 64) : fxH(fep, o + 68);
  const pts = is100 ? Math.max(2, fxB(fep, o + 66)) : 2, len = is100 ? 1 : fxF(fep, o + 36);
  const blend = is100 ? fxB(fep, o + 68) : 1;
  if (!maxN || !life) return;
  const sparks = []; let acc = rate > 0 ? 0 : maxN;
  const steps = Math.floor(el.age);
  for (let f = 0; f <= steps && f < node.duration; f++) {
    acc += rate; let n = Math.trunc(acc); acc -= n; if (f === 0 && rate <= 0) n = maxN;
    for (let i = 0; i < n && sparks.length < maxN; i++) {
      const yaw = fxR(rnd, cone / 2) * FX_D2R, pitch = fxR(rnd, cone / 2) * FX_D2R;
      const d = fxV(Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch));
      sparks.push({ born: f, d, v: speed + fxR(rnd, speedR), life: life + fxR(rnd, lifeR) });
    }
    if (rate <= 0 && f === 0) break;
  }
  const W = fxElWorld(el, fxCh14(el, tr), el.age), P = [], Ca = [];
  const ec = fxElColour(el, fxColDep(el, tr));
  for (const s of sparks) {
    const t = el.age - s.born; if (t < 0 || t >= s.life) continue;
    const a = t < s.life / 2 ? 1 : 2 * (1 - t / s.life);
    const at = (tt) => { tt = Math.max(0, tt); return s.d.clone().multiplyScalar(s.v * tt).add(fxV(0, grav * tt * tt / 2, 0)); };
    const seg = []; for (let j = 0; j < pts; j++) seg.push(is100 ? at(t - j) : at(t).addScaledVector(s.d, -len * j / (pts - 1)));
    for (let j = 0; j + 1 < pts; j++) for (const q of [j, j + 1]) {
      const p = fxV(seg[q].x, -seg[q].y, -seg[q].z).applyMatrix4(W), k = q / (pts - 1);
      P.push(p.x, p.y, p.z);
      Ca.push(...[0, 1, 2, 3].map((c) => Math.min(255, Math.max(0, (head[c] + (tail[c] - head[c]) * k) * ec[c] / 128 * I.colour[c])) / 255 * (c === 3 ? a : 1)));
    }
  }
  if (!P.length) return;
  const old = FXV.pool.get(key); if (old && old.geometry) old.geometry.dispose();
  const m = fxMesh(key, fxGeo(P, Array(P.length / 3 * 2).fill(0), Ca, null), 'lines'), u = m.material.uniforms;
  u.map.value = null; u.useMap.value = 0; u.C.value.set(0.5, 0.5, 0.5); u.lit2.value = 1; u.A2.value = 1;
  fxBlend(m.material, fxBlendMode(blend), 0xFFFFFF);
  m.matrix.identity();
  ctx.approx.add('sparks');
}

/* ------------------------------------------------------------------ what a timeline starts */
// [{type: 'fx' | 'trail' | 'proj' | 'hit', tick, recs, end?, life?, bone?, scale?}] (tick and end in timeline ticks)
const FX_FOREVER = 0xFFFF;
function fxInstances(fx, t, ctx) {
  const out = []; if (!fx || !t) return out;
  const add = (tick, id, kind, idx, extra) => { const recs = fx.byId.get(id) || []; if (recs.length) out.push({ type: 'fx', tick, recs, kind, idx, ...extra }); };
  // the event filter: phase (low byte; the press's motion-map phase) and mode (next byte; 2 = EX Mode only)
  const pass = (f) => { f = f || 0; const ph = f & 0xFF, md = (f >> 8) & 0xFF; return (!ph || !ctx || ctx.phase == null || ph === ctx.phase) && (!md || md === (ctx && ctx.ex ? 2 : 1)); };
  const life = (v) => typeof v === 'number' && v > 0 ? v : null;
  // motion-bound records: when a fighter starts a motion (FUN_0883C2E8 sets char+1316, then FUN_088A98B4 ->
  // FUN_088A9E90 -> FUN_088AA844) the game plays every FEPA record of its model whose motion name (+0xC8) is that
  // motion and whose attach bits lack 0x80, from the motion's first tick (their start delay +0x74 still applies).
  // Vaan's spell circles (QUAKE / WATER / INFERNO / AERO _SQUARE, _SHOT, INFERNO_SNC) have no event at all.
  if (ctx && ctx.motionName) for (const r of fx.fepa) if (r.model && r.motion === ctx.motionName && !(r.attach & 0x80)) out.push({ type: 'fx', tick: 0, recs: [r], kind: 'motion', idx: r.index });
  t.keyed.forEach((e, i) => {
    if (!pass(e.x) || (ctx && ctx.skipComp && e._comp === ctx.skipComp)) return;   // an effect being edited plays from its layers
    const c = opCode(e.op), a = argsOf(e);
    if (c === 0x0219) add(e.frame, a[0], 'keyed', i, { life: life(a[1]) });                         // arg1 overwrites the FEPA lifetime
    else if (c === 0x042F) add(e.frame, a[0], 'keyed', i, { life: life(a[1]) });
    else if (c === 0x0204 && fx.magi) {                  // fire_projectile: its visual (and its children's) travel with the shot
      const pr = fx.magi.get(a[0]); if (!pr) return;
      const recs = projVisuals(fx, pr);
      const ch = pr.link_emit >= 0 && !(pr.flags & 0x20) ? fx.magi.get(pr.link_emit) : null;   // child fired at birth
      if (ch) recs.push(...projVisuals(fx, ch));
      if (recs.length) out.push({ type: 'proj', tick: e.frame, proj: pr, recs, kind: 'keyed', idx: i, arg2: typeof a[1] === 'number' ? a[1] : 0 });
    }
  });
  t.ranged.forEach((e, i) => {
    if (!pass(e.pad)) return;
    const c = opCode(e.op), a = argsOf(e), end = e.end === FX_FOREVER ? null : e.end;
    if (c === 0x0219) add(e.start, a[0], 'ranged', i, { end });
    else if (c === 0x0466) add(e.start + (a[2] || 0), a[0], 'ranged', i, { life: life(a[1]), bone: typeof a[3] === 'number' ? a[3] : null });
    else if (c === 0x055F && typeof spOwnsProp === 'function' && spOwnsProp(a[4])) { /* a trail on a spawned prop: spawn.js plays it on the prop */ }
    else if (c === 0x042A || c === 0x055F) {
      // bone_effect {slot, id, bit, scale}: id 9999 = LOCUS trails, 8888 = weapon glows, the same code (E §3, G §1): the
      // first FEPA record of that id with +0x34 == 0, bit arg2 in +0x44 and a bone this model has; one effect per slot
      const id = a[1], bit = a[2] || 0, recs = (fx.byId.get(id) || []).filter((r) => r.trig === 0 && ((r.keys >>> bit) & 1));
      const mine = recs.filter((r) => !ctx || !ctx.hasBone || ctx.hasBone(r.bone));
      const pick = (mine.length ? mine : recs).slice(0, 1);
      if (pick.length) out.push({ type: 'fx', tick: e.start, end, recs: pick, kind: 'ranged', idx: i, slot: (a[0] || 0) & 15, scale: typeof a[3] === 'number' && a[3] > 0 ? a[3] : 1, trail: true });
    } else if (c === 0x0743 && fx.magi && e.projectile !== undefined) {   // hit window that fires a projectile (Garland's Flare): its look from the window's start
      const pr = fx.magi.get(typeof e.projectile === 'object' ? e.projectile.f : e.projectile);
      const recs = pr ? projVisuals(fx, pr) : [];
      if (recs.length) out.push({ type: 'proj', tick: e.start, proj: pr, recs, kind: 'ranged', idx: i, arg2: 0 });
    }
    if (c === 0x0200 && ctx && ctx.hitOf) {       // hit_window: the hit's contact effect, where the foe would be (once per hit and tick)
      const h = ctx.hitOf(e.hit !== undefined ? e.hit : a[0]);
      const bit = h ? h.hit_effect_bit : 0;
      if (bit) {
        const grp = fx.fepa.filter((r) => fxGroupHas(r, bit) && !r.trig);
        const contact = grp.filter((r) => r.kinds & 1), recs = contact.length ? contact : grp;   // the *_HIT records carry +0x38 bit 0
        // with the dummy on, the contact effect plays where and when the hit landed on it (and not at all for a miss)
        const at = ctx.foeAt ? ctx.foeAt(h.name, e.start, e.end) : null;
        if (at !== false && recs.length && !out.some((x) => x.type === 'hit' && x.hit === h.name && x.tick === (at ? at.T : e.start)))
          out.push({ type: 'hit', tick: at ? at.T : e.start, recs: recs.slice(0, 2), kind: 'ranged', idx: i, hit: h.name, foe: at ? at.pos : null });
      }
    }
  });
  // one bone effect per slot while any range on it runs: overlapping or touching ranges continue the first one (G §1.4)
  const slots = new Map();
  for (const x of out.filter((x) => x.slot != null).sort((p, q) => p.tick - q.tick || p.idx - q.idx)) {
    const prev = slots.get(x.slot);
    if (prev && (prev.end == null || x.tick <= prev.end)) { prev.end = prev.end == null || x.end == null ? null : Math.max(prev.end, x.end); x.merged = true; }
    else slots.set(x.slot, x);
  }
  return out.filter((x) => !x.merged);
}

/* ------------------------------------------------------------------ placement (A §1.5) */
const FOE_AT = [0, 1.0, 1.6];
// FEPA records on the bone named 'root' sit on the floor in the game (Cloud's FIGA_START is 1.36 m up, 0.9 m ahead of
// it); the GMO's 'root' bone is the animated pelvis, so the character's own root matrix stands in for it
const FX_ROOT_BONE = -2;          // where a hit's contact effect is drawn (in front of the character, chest high)
/* the bone an attached record follows. +0x70 (target) picks the record's skeleton (FUN_088A82D8 -> owner vmethod +16 =
   FUN_0881476C: char+1200 / +1208 / +1216). Target 1 (char+1208, vtable 0x089C7C48) answers both the root and the
   bone query with the character's own world matrix (FUN_08830FB8 / 08830FE4 -> FUN_088143B8 = char+0), and the bone
   index is forced to 0: the record's bone name is ignored. Vaan's spell circles name camera2_include, which his HP
   animations move against the root travel; in the game they sit on the character, not on that bone. */
function fxRecBone(ctx, rec) { return rec.model && rec.target === 1 ? FX_ROOT_BONE : ctx.boneIndex(rec.bone); }
function fxL(rec, hs) {                 // Rx·Ry·Rz(rot°)·S(scale ⊙ handle scale)
  return fxRot(rec.rot, 'XYZ').scale(fxV(rec.scale[0] * hs, rec.scale[1] * hs, rec.scale[2] * hs));
}
/* attached record: B3 = Rsrc·L3, B.t = (root | bone)·pos; bone axes normalised unless attach 0x8 */
function fxAttached(pose, bi, rec, hs) {
  const mode = rec.attach, root = pose.root, bone = bi === FX_ROOT_BONE ? root : pose.bones[bi];
  const boneN = (mode & 8) ? bone : fxNormAxes(bone);
  const B = fxRot3((mode & 2) ? root : boneN).multiply(fxL(rec, hs));
  return B.setPosition(fxV(...rec.pos).applyMatrix4((mode & 4) ? root : boneN));
}
/* caller-placed record: B3 = P3·L3, B.t = point + pos (not rotated) */
function fxCallerPlaced(P, point, rec, hs) {
  return fxRot3(P).multiply(fxL(rec, hs)).setPosition(point.clone().add(fxV(...rec.pos)));
}
/* a function tick -> B for one record of an instance */
function fxPlacer(ctx, inst, rec, t0, shot) {
  const hs = inst.scale || 1, cache = new Map();
  const pose = (T) => ctx.poseAt(T);
  let fn;
  if (inst.type === 'proj') fn = (T) => { const M = shot.at(T); return fxCallerPlaced(M, fxPos(M), rec, hs); };   // the record's own scale / rotation too (BIND_GENE is 0.13)
  else if (inst.type === 'hit') { const p0 = pose(t0); const root = p0.root; const foe = inst.foe ? fxV(...inst.foe) : fxV(...FOE_AT).applyMatrix4(root); fn = () => fxCallerPlaced(root, foe, rec, hs); }
  else {
    const bi = inst.bone != null && ctx.boneOk(inst.bone) ? inst.bone : (rec.model || inst.trail) ? fxRecBone(ctx, rec) : -1;
    if (bi >= 0 || bi === FX_ROOT_BONE) {
      const followTo = (rec.attach & 1) ? t0 : rec.follow > 0 ? t0 + rec.follow : Infinity;
      fn = (T) => fxAttached(pose(Math.min(T, followTo)), bi, rec, hs);
    } else { const r0 = pose(t0).root; fn = () => fxCallerPlaced(r0, fxPos(r0), rec, hs); }
  }
  return (T) => {
    const k = T >= ctx.tick - 1e-6 ? 'now' : Math.round(T);
    if (!cache.has(k)) cache.set(k, fn(k === 'now' ? ctx.tick : k));
    return cache.get(k);
  };
}
/* a projectile shot's path: from its origin (owner, or a bone) straight ahead at its speed, with acceleration and
   gravity; homing and target-relative spawns are not simulated */
function fxShot(ctx, pr, fireTick, arg2) {
  const p0pose = ctx.poseAt(fireTick), rootM = p0pose.root;
  // origin 1/2: an owner bone; origin 6: where the owner's effect with FEPA id origin_arg is playing (FUN_08AD3C24 ->
  // FUN_088AA350 finds that effect's handle, FUN_088A82D8 gives its matrix), e.g. Cloud's Fire balls start at FIGA_START
  let org = rootM;
  const FOE_ORG = { 3: 128, 4: 256, 8: 128, 9: 256, 11: 128, 12: 256, 13: 256 };   // FUN_08AD5D70: on the target (128 its feet, 256 its centre), clamped to tgt_dist / tgt_height
  if ((pr.origin === 1 || pr.origin === 2) && ctx.boneOk(pr.origin_arg)) org = p0pose.bones[pr.origin_arg];
  else if (FOE_ORG[pr.origin]) {
    const me = fxPos(rootM), foe = (ctx.opts.foePosAt && ctx.opts.foePosAt(fireTick)) || fxV(FOE_AT[0], 0, FOE_AT[2]).applyMatrix4(rootM);
    const pt = foe.clone(); if (FOE_ORG[pr.origin] === 256) pt.y += 1.0;          // magi.js MAGI_LOCK_H (a guess)
    const h = fxV(pt.x - me.x, 0, pt.z - me.z), dy = pt.y - me.y, cl = (x, lo, hi) => (x < lo ? lo : x <= hi ? x : hi);
    const hd = cl(h.length(), pv(pr, 'tgt_dist_min', 0), pv(pr, 'tgt_dist_max', 9999)), vd = cl(Math.abs(dy), pv(pr, 'tgt_height_min', 0), pv(pr, 'tgt_height_max', 9999));
    const at = me.clone().add(h.lengthSq() > 1e-9 ? h.normalize().multiplyScalar(hd) : fxV(0, 0, 0)).add(fxV(0, Math.sign(dy) * vd, 0));
    org = rootM.clone().setPosition(at);
  }
  else if (pr.origin === 6 && (arg2 || pr.origin_arg) > 0) {        // the fire event's value names the effect (else origin_arg)
    const rec = (ctx.fx.byId.get(arg2 || pr.origin_arg) || [])[0];
    if (rec) { const bi = fxRecBone(ctx, rec); org = bi >= 0 || bi === FX_ROOT_BONE ? fxAttached(p0pose, bi, rec, 1) : fxCallerPlaced(rootM, fxPos(rootM), rec, 1); }
  }
  const p0 = fxV(pv(pr, 'off_x', 0), pv(pr, 'off_y', 0), pv(pr, 'off_z', 0)).applyMatrix4(org);
  const fwd = fxV(0, 0, 1).transformDirection(rootM);
  const yaw = pv(pr, 'launch_yaw', 0) * FX_D2R; fwd.applyAxisAngle(fxV(0, 1, 0), yaw);
  p0.addScaledVector(fwd, pv(pr, 'fwd_dist', 0));
  const speed = pv(pr, 'speed', 0), accel = pv(pr, 'accel', 0), accelAt = pv(pr, 'accel_start', 0), vmax = pv(pr, 'speed_max', 0);
  const grav = pv(pr, 'gravity', 0), gravAt = pv(pr, 'gravity_start', 0), moveT = pv(pr, 'move_time', 0);
  const R = fxM().makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(fxV(0, 0, 1), fwd));
  const life = pv(pr, 'lifetime', 0) || 90;
  return { life, at: (T) => {
    const age = Math.min(life, Math.max(0, T - fireTick));          // the effect stays where the shot ended
    let dist = 0;
    if (pr.motion !== 2) { const tm = moveT ? Math.min(age, moveT) : age, ta = Math.max(0, tm - accelAt); dist = speed * tm + accel * ta * ta / 2; if (vmax && accel) dist = Math.min(dist, vmax * tm); }
    const pos = p0.clone().addScaledVector(fwd, dist);
    if (grav && age > gravAt) pos.y -= grav * (age - gravAt) * (age - gravAt) / 2;
    return R.clone().setPosition(pos);
  } };
}

/* ------------------------------------------------------------------ one effect instance (A §3) */
function fxRunInstance(ctx, inst, rec, key, t0, extra) {
  const fep = ctx.fx.fep, bi = fepBodyOfName(fep, rec.effect); if (bi == null) return;
  const body = fepBody(fep, bi);
  t0 += rec.delay > 0 ? rec.delay : 0;
  const t = ctx.tick - t0; if (t < 0) return;
  const sustained = !!(body.a & 2);
  let stop = Infinity;
  const life = inst.life != null ? inst.life : rec.life; if (life > 0) stop = life;
  if (inst.end != null) stop = Math.min(stop, inst.end - t0);
  if (extra && extra.stop != null) stop = Math.min(stop, extra.stop);
  if (sustained && body.c > 0) stop = Math.min(stop, body.c);
  if (!sustained && !(body.a & 1) && t >= body.duration) return;
  let fade = 1;
  if (t >= stop) {
    const d = body.d, ts = t - stop;
    if (d === 0 || d === 1) { fade = 1 - ts / (d ? 15 : 30); if (fade <= 0) return; }
    else if (d === 2) return;
  }
  const hs = inst.scale || 1;
  const I = { fep, body, t0, stop, sustained, seed: fxHash(inst.tick, rec.index, ctx.fx.fep.idBase & 0xFFFF, extra ? extra.n : 0), colour: [1, 1, 1, fade],
    iscale: [rec.scale[0] * hs, rec.scale[1] * hs, rec.scale[2] * hs], place: fxPlacer(ctx, inst, rec, t0, extra && extra.shot) };
  FXV.labels.push({ m: I.place(ctx.tick), text: (fep.names[rec.effect] || {}).name });
  for (const sp of body.spawns) {
    if (sp.frame > t || sp.frame >= Math.max(1, body.duration)) continue;
    for (const ni of sp.nodes) {
      const node = body.nodes[ni]; if (!node || (ctx.opts.mute && ctx.opts.mute.has(ni))) continue;
      const el = fxElement(I, node, sp.frame, t - sp.frame, null);
      if (el.expired && !(node.flags & 1) && !node.byChan.has(21) && !node.byChan.has(18) && !node.byChan.has(151)) continue;
      fxDrawElement(ctx, el, `${key}|${sp.frame}|${ni}`);
    }
  }
}

/* ------------------------------------------------------------------ per frame */
function fxUpdate(view, fx, tl, tick, opts) {
  if (!FXV.group) { FXV.group = new THREE.Group(); view.scene.add(FXV.group); }
  FXV.used.clear(); FXV.order = 0; FXV.labels = []; FXV.post = [];
  const finish = () => {
    for (const [k, m] of FXV.pool) if (!FXV.used.has(k)) m.visible = false;
    if (FXV.pool.size > 4000) for (const [k, m] of [...FXV.pool]) if (!m.visible) { FXV.group.remove(m); if (m.fxBase) { m.material = m.fxBase; if (m.fxOver) m.fxOver.dispose(); } m.material.dispose(); FXV.pool.delete(k); }
  };
  if (!fx || (!tl && !opts.solo && !(opts.extra && opts.extra.length)) || !opts.on || !opts.poseAt) return finish();
  view.camera.updateMatrixWorld();
  const ctx = { view, fx, opts, tick, cam: view.camera, camRot: fxRot3(fxM().extractRotation(view.camera.matrixWorld)), camPos: fxPos(view.camera.matrixWorld),
    poseAt: opts.poseAt, boneIndex: opts.boneIndex, boneOk: opts.boneOk, missing: 0, approx: new Set() };
  try {
    // solo: the effect editor plays one effect by itself (opts.solo = {rec, stop, hit}) at effect tick `tick`
    const insts = (opts.solo ? [{ type: opts.solo.hit ? 'hit' : 'fx', tick: 0, recs: [opts.solo.rec], end: opts.solo.stop, kind: 'solo', idx: 0 }] : fxInstances(fx, tl, opts)).concat(opts.extra || []);
    insts.forEach((inst, ii) => {
      const base = ctx, ctx2 = inst.ctx ? Object.assign(Object.create(base), inst.ctx) : base;   // spawn.js: an object's own clock, pose and bank
      ((ctx, tick) => {
      if (tick < inst.tick) return;
      if (inst.type === 'proj') {
        const pr = inst.proj, shots = Math.max(1, Math.min(8, Math.round(pv(pr, 'shots', 1)))), delay = pv(pr, 'first_delay', 0), every = pv(pr, 'interval', 0);
        for (let s = 0; s < shots; s++) {
          const fire = inst.tick + delay + s * every; if (tick < fire) continue;
          const shot = fxShot(ctx, pr, fire, inst.arg2), born = { life: shot.life, at: () => shot.at(fire) };
          inst.recs.forEach((rec, ri) => fxRunInstance(ctx, inst, rec, `${ii}|s${s}|${ri}`, fire, rec._once ? { shot: born, n: s + 1 } : { shot, stop: shot.life, n: s + 1 }));
        }
      } else inst.recs.forEach((rec, ri) => fxRunInstance(ctx, inst, rec, `${ii}|${ri}`, inst.tick, null));
      })(ctx2, ctx2.tick);
    });
    if (typeof compShaderPass === 'function') compShaderPass(ctx, tick);       // shader layers take their draws (compshader.js)
  } finally { finish(); }
  FXV.missing = ctx.missing; FXV.approx = [...ctx.approx];
}

/* ------------------------------------------------------------------ rendering with the screen passes */
// Zoom blur (ch110) and the shockwave (ch101) redraw a copy of the frame. Without them this is a plain render.
const FX_POST_VS = 'varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0.0, 1.0); }';
function fxPostMat(fs, uniforms) { return new THREE.ShaderMaterial({ uniforms, vertexShader: FX_POST_VS, fragmentShader: fs, depthTest: false, depthWrite: false, transparent: true }); }
function fxRender(view) {
  const R = view.renderer;
  if (!FXV.post || !FXV.post.length) { R.render(view.scene, view.camera); return; }
  const size = R.getDrawingBufferSize(new THREE.Vector2());
  if (!FXV.rt || FXV.rt.width !== size.x || FXV.rt.height !== size.y) {
    if (FXV.rt) FXV.rt.dispose();
    FXV.rt = new THREE.WebGLRenderTarget(size.x, size.y, { minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, format: THREE.RGBAFormat });
    FXV.rt2 = new THREE.WebGLRenderTarget(size.x, size.y, { minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, format: THREE.RGBAFormat });
    FXV.rt.texture.encoding = FXV.rt2.texture.encoding = R.outputEncoding;      // the copy holds what the screen would
  }
  if (!FXV.postScene) {
    FXV.postScene = new THREE.Scene(); FXV.postCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    FXV.copyMat = fxPostMat('uniform sampler2D src; varying vec2 vUv; void main() { gl_FragColor = texture2D(src, vUv); }', { src: { value: null } });
    FXV.copyMat.transparent = false; FXV.copyMat.blending = THREE.NoBlending;
    FXV.zoomMat = fxPostMat(`uniform sampler2D src; uniform vec2 c; uniform float k; uniform vec4 col; uniform float aMul; varying vec2 vUv;
      void main() { vec2 uv = c + (vUv - c) * (1.0 - k); vec3 rgb = texture2D(src, uv).rgb * min(vec3(1.0), 2.0 * col.rgb); float f = min(1.0, aMul * col.a); gl_FragColor = vec4(rgb * f, f); }`,
      { src: { value: null }, c: { value: new THREE.Vector2() }, k: { value: 0 }, col: { value: new THREE.Vector4() }, aMul: { value: 2 } });
    FXV.ringMat = fxPostMat(`varying float vA; uniform sampler2D src; uniform vec2 c; uniform vec4 col; uniform float aMul; varying vec2 vUv;
      void main() { vec2 uv = c + (vUv - c) * 0.9; vec3 rgb = texture2D(src, uv).rgb * min(vec3(1.0), 2.0 * col.rgb); float f = min(1.0, aMul * col.a * vA); gl_FragColor = vec4(rgb * f, f); }`,
      { src: { value: null }, c: { value: new THREE.Vector2() }, col: { value: new THREE.Vector4() }, aMul: { value: 2 } });
    FXV.ringMat.vertexShader = 'attribute float fxa; varying vec2 vUv; varying float vA; void main() { vUv = uv; vA = fxa; gl_Position = vec4(position.xy, 0.0, 1.0); }';
    FXV.ringMat.needsUpdate = true;
    FXV.shiftMat = fxPostMat(`uniform sampler2D src; uniform vec2 off; uniform vec4 col; uniform float aMul; varying vec2 vUv;
      void main() { vec2 uv = vUv - off; if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;
        vec3 rgb = texture2D(src, uv).rgb * min(vec3(1.0), 2.0 * col.rgb); float f = min(1.0, aMul * col.a); gl_FragColor = vec4(rgb * f, f); }`,
      { src: { value: null }, off: { value: new THREE.Vector2() }, col: { value: new THREE.Vector4() }, aMul: { value: 2 } });
    FXV.quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), FXV.copyMat); FXV.quad.frustumCulled = false; FXV.postScene.add(FXV.quad);
  }
  const blendOf = (m, mode) => fxBlend(m, fxBlendMode(mode), 0xFFFFFF);
  R.setRenderTarget(FXV.rt); R.render(view.scene, view.camera);
  let src = FXV.rt;
  const auto = R.autoClear; R.autoClear = false;
  for (const p of FXV.post) {
    if (p.kind === 'zoom') {
      for (let i = 0; i < p.passes; i++) {
        // draw the copy, then the magnified copy over it, into the other buffer
        const dst = src === FXV.rt ? FXV.rt2 : FXV.rt;
        R.setRenderTarget(dst); FXV.quad.geometry = FXV.quad.geometry; FXV.quad.material = FXV.copyMat; FXV.copyMat.uniforms.src.value = src.texture; R.render(FXV.postScene, FXV.postCam);
        const z = FXV.zoomMat; z.uniforms.src.value = src.texture; z.uniforms.c.value.set(p.c[0], p.c[1]); z.uniforms.k.value = Math.min(0.5, p.k);
        z.uniforms.col.value.set(p.rgba[0] / 255, p.rgba[1] / 255, p.rgba[2] / 255, p.rgba[3] / 255); blendOf(z, p.blend);
        FXV.quad.material = z; R.render(FXV.postScene, FXV.postCam); src = dst;
      }
    } else if (p.kind === 'shift') {                 // ch111: the frame redrawn shifted by V PSP pixels (G §2)
      const base = src;
      for (let i = 0; i < p.passes; i++) {
        const dst = src === FXV.rt ? FXV.rt2 : FXV.rt;
        R.setRenderTarget(dst); FXV.quad.material = FXV.copyMat; FXV.copyMat.uniforms.src.value = src.texture; R.render(FXV.postScene, FXV.postCam);
        const m = FXV.shiftMat; m.uniforms.src.value = (p.feedback ? src : base).texture;
        m.uniforms.off.value.set(p.d[0] / 480, -p.d[1] / 272);
        m.uniforms.col.value.set(p.rgba[0] / 255, p.rgba[1] / 255, p.rgba[2] / 255, p.rgba[3] / 255); blendOf(m, p.blend);
        FXV.quad.material = m; R.render(FXV.postScene, FXV.postCam); src = dst;
      }
    } else if (p.kind === 'ring') {
      const ctr = p.pos.clone().project(view.camera); if (ctr.z > 1) continue;
      const camRight = fxV(1, 0, 0).applyQuaternion(view.camera.quaternion), edge = p.pos.clone().addScaledVector(camRight, p.r).project(view.camera);
      const rx = Math.abs(edge.x - ctr.x), ry = rx * size.x / size.y;
      const P = [], UV = [], Aa = [], Ix = [];
      for (let i = 0; i <= p.seg; i++) {
        const a = i / p.seg * Math.PI * 2, cs = Math.cos(a), sn = Math.sin(a);
        for (const [rr, al] of [[p.inner, p.aIn], [1, p.aOut]]) { const x = ctr.x + cs * rx * rr, y = ctr.y + sn * ry * rr; P.push(x, y, 0); UV.push((x + 1) / 2, (y + 1) / 2); Aa.push(al); }
        if (i) { const b = 2 * i; Ix.push(b - 2, b - 1, b, b, b - 1, b + 1); }
      }
      const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(P, 3)); g.setAttribute('uv', new THREE.Float32BufferAttribute(UV, 2)); g.setAttribute('fxa', new THREE.Float32BufferAttribute(Aa, 1)); g.setIndex(Ix);
      const dst = src === FXV.rt ? FXV.rt2 : FXV.rt;
      R.setRenderTarget(dst); FXV.quad.material = FXV.copyMat; FXV.copyMat.uniforms.src.value = src.texture; R.render(FXV.postScene, FXV.postCam);
      const rm = FXV.ringMat; rm.uniforms.src.value = src.texture; rm.uniforms.c.value.set((ctr.x + 1) / 2, (ctr.y + 1) / 2);
      rm.uniforms.col.value.set(p.rgba[0] / 255, p.rgba[1] / 255, p.rgba[2] / 255, p.rgba[3] / 255); blendOf(rm, p.blend);
      const mesh = new THREE.Mesh(g, rm); mesh.frustumCulled = false; FXV.postScene.add(mesh); FXV.quad.visible = false;
      R.render(FXV.postScene, FXV.postCam); FXV.postScene.remove(mesh); g.dispose(); FXV.quad.visible = true; src = dst;
    }
  }
  R.setRenderTarget(null); FXV.quad.material = FXV.copyMat; FXV.copyMat.uniforms.src.value = src.texture; R.render(FXV.postScene, FXV.postCam);
  R.autoClear = auto;
}

/* ------------------------------------------------------------------ inspector */
const FX_CHAN_KIND = { 10: 'flipbooks', 11: 'meshes', 20: 'scrolling meshes', 21: 'particles', 105: 'a weapon trail', 107: 'a screen flash', 110: 'a zoom blur', 111: 'a screen shift',
  101: 'a shockwave', 100: 'spark streaks', 102: 'line sparks', 103: 'light rays', 95: 'points', 18: 'child pieces', 151: 'child pieces' };
const FX_NOT_DRAWN = new Set([82, 58, 80, 26, 30, 150]);
function fxInfo(fx, id) {
  const recs = fx && fx.byId.get(id); if (!recs || !recs.length) return null;
  const fep = fx.fep, bi = fepBodyOfName(fep, recs[0].effect), body = bi != null ? fepBody(fep, bi) : null, nm = fep.names[recs[0].effect];
  const kinds = new Set(), tex = new Set(), notes = new Set(); let missing = 0;
  const mdlTex = (i) => { const u8 = i >= 0 && fxResource(fep, 'MDL', i); if (i >= 0 && !u8) missing++; else if (u8) try { decodeMdl(u8).mats.forEach((m) => tex.add(m.tex)); } catch (e) { /* skip */ } };
  const anmTex = (i) => { const u8 = i >= 0 && fxResource(fep, 'ANM', i); if (i >= 0 && !u8) missing++; else if (u8) try { decodeAnm(u8).tex.forEach((x) => tex.add(x)); } catch (e) { /* skip */ } };
  if (body) for (const n of body.nodes) {
    if (!n) continue;
    for (const tr of n.tracks) {
      const sg = tr.segs[tr.segs.length - 1]; if (!sg) continue;
      if (FX_CHAN_KIND[tr.chan]) kinds.add(FX_CHAN_KIND[tr.chan]);
      if (FX_NOT_DRAWN.has(tr.chan)) notes.add('channel ' + tr.chan + ' is not drawn');
      if (tr.chan === 11 || tr.chan === 20) mdlTex(fxI(fep, sg.o + 12));
      else if (tr.chan === 10) anmTex(fxI(fep, sg.o + 12));
      else if (tr.chan === 21) anmTex(fxI(fep, sg.o + 12));
      else if (tr.chan === 105) { const r = fep.res.TEX[fxI(fep, sg.o + 44)]; if (r) tex.add(r.id); }
      if (tr.chan === 101) notes.add('the shockwave is approximate');
      if (tr.chan === 100 || tr.chan === 102 || tr.chan === 103) notes.add('spark lines are approximate');
    }
  }
  return { name: nm ? nm.name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '') : '#' + recs[0].effect, full: nm && nm.name, recs, body, duration: body ? body.duration : 0,
    sustained: body ? !!(body.a & 2) : false, nodes: body ? body.nodes.filter(Boolean).length : 0, kinds: [...kinds], tex: [...tex], notes: [...notes], approx: notes.size > 0, missing };
}
/* little canvas of a texture, for the inspector */
function fxTexCanvas(fep, id, size) {
  const tx = fxTexture(fep, id); if (!tx) return null;
  const img = tx.fxImg, cv = document.createElement('canvas');
  cv.width = img.w; cv.height = img.h;
  const ctx = cv.getContext('2d'), id_ = ctx.createImageData(img.w, img.h);
  for (let i = 0; i < img.w * img.h; i++) { const a = img.rgba[4 * i + 3]; id_.data[4 * i] = img.rgba[4 * i] * a / 255; id_.data[4 * i + 1] = img.rgba[4 * i + 1] * a / 255; id_.data[4 * i + 2] = img.rgba[4 * i + 2] * a / 255; id_.data[4 * i + 3] = 255; }
  ctx.putImageData(id_, 0, 0);
  cv.style.width = (size || 64) + 'px'; cv.style.height = Math.round((size || 64) * img.h / img.w) + 'px';
  return cv;
}
