// Dissidia Move Studio: GMO model reader, GIM textures and the three.js character (from the Forge Studio).
'use strict';
function halfToFloat(h) {
  const s = (h & 0x8000) ? -1 : 1, e = (h >> 10) & 0x1f, f = h & 0x3ff;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}
const LEAF = new Set([0x0007, 0x000C]);
function parseChunks(u8, start, end) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const out = []; let o = start;
  while (o + 8 <= end) {
    const t = dv.getUint16(o, true), hs = dv.getUint16(o + 2, true), sz = dv.getUint32(o + 4, true);
    if (sz < 8 || o + sz > end) throw new Error('Bad GMO chunk at ' + o);
    const c = { type: t, off: o, size: sz, hs, children: [] };
    if (t & 0x8000) { c.p = o + 8; c.plen = sz - 8; }
    else {
      let nm = ''; for (let i = o + 16; i < o + hs && u8[i]; i++) nm += String.fromCharCode(u8[i]);
      c.name = nm;
      if (LEAF.has(t)) { c.p = o + hs; c.plen = sz - hs; } else c.children = parseChunks(u8, o + hs, o + sz);
    }
    out.push(c); o += sz;
  }
  return out;
}
const refIndex = r => r & 0xfff;

function vertexLayout(word) {
  const tc = word & 3, col = (word >> 2) & 7, nrm = (word >> 5) & 3, pos = (word >> 7) & 3, wt = (word >> 9) & 3;
  const nw = wt ? ((word >> 14) & 7) + 1 : 0, SZ = [0, 1, 2, 4], CSZ = { 0: 0, 4: 2, 5: 2, 6: 2, 7: 4 };
  let o = 0, al = 1; const off = {};
  const place = (k, n, s) => { if (!s) return; o = Math.ceil(o / s) * s; off[k] = o; o += n * s; al = Math.max(al, s); };
  place('w', nw, SZ[wt]); place('t', 2, SZ[tc]); place('c', 1, CSZ[col]); place('n', 3, SZ[nrm]); place('p', 3, SZ[pos]);
  return { tc, col, nrm, pos, wt, nw, off, stride: word >>> 24 };
}

function parseGMO(u8) {
  if (String.fromCharCode(...u8.slice(0, 3)) !== 'OMG') throw new Error('Not a GMO model');
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const f32 = (o) => dv.getFloat32(o, true), u32 = (o) => dv.getUint32(o, true);
  const root = parseChunks(u8, 16, u8.length)[0];
  const models = root.children.filter(c => c.type === 3);
  const animM = models.find(m => m.children.some(c => c.type === 0x0B));
  const meshM = models.find(m => m.children.some(c => c.type === 0x05)) || models[models.length - 1];
  const bonesOf = (m) => m.children.filter(c => c.type === 4).map(b => {
    const bone = { name: b.name, parent: -1, t: [0, 0, 0], r: [0, 0, 0, 1], s: [1, 1, 1], drawPart: -1, blend: [], offsets: [] };
    for (const c of b.children) {
      const p = c.p;
      if (c.type === 0x8041) bone.parent = refIndex(u32(p));
      else if (c.type === 0x8048) bone.t = [f32(p), f32(p + 4), f32(p + 8)];
      else if (c.type === 0x804B) bone.r = [f32(p), f32(p + 4), f32(p + 8), f32(p + 12)];
      else if (c.type === 0x804D) bone.s = [f32(p), f32(p + 4), f32(p + 8)];
      else if (c.type === 0x804E) { const r = u32(p); bone.drawPart = r === 0xffffffff ? -1 : refIndex(r); }
      else if (c.type === 0x8044) { const n = u32(p); for (let i = 0; i < n; i++) bone.blend.push(refIndex(u32(p + 4 + i * 4))); }
      else if (c.type === 0x8045) { const n = u32(p); for (let i = 0; i < n; i++) { const a = []; for (let k = 0; k < 16; k++) a.push(f32(p + 4 + i * 64 + k * 4)); bone.offsets.push(a); } }
    }
    return bone;
  });
  const bones = bonesOf(meshM), animBones = animM ? bonesOf(animM) : bones;
  let posOff = [0, 0, 0], posScl = [1, 1, 1], uvOff = [0, 0], uvScl = [1, 1];
  for (const c of meshM.children.filter(c => c.type === 0x8015)) {
    const sel = u32(c.p);
    if (sel === 0x180) { posOff = [f32(c.p + 4), f32(c.p + 8), f32(c.p + 12)]; posScl = [f32(c.p + 16), f32(c.p + 20), f32(c.p + 24)]; }
    else if (sel === 3) { uvOff = [f32(c.p + 4), f32(c.p + 8)]; uvScl = [f32(c.p + 12), f32(c.p + 16)]; }
  }
  const parts = meshM.children.filter(c => c.type === 5).map(p => {
    const arrays = p.children.filter(c => c.type === 7).map(a => ({ word: u32(a.p), count: u32(a.p + 4), data: a.p + 16, name: a.name }));
    const meshes = p.children.filter(c => c.type === 6).map(m => {
      const mesh = { material: -1, subset: [], draws: [] };
      for (const c of m.children) {
        if (c.type === 0x8061) mesh.material = refIndex(u32(c.p));
        else if (c.type === 0x8062) { const n = u32(c.p); for (let i = 0; i < n; i++) mesh.subset.push(u32(c.p + 4 + i * 4)); }
        else if (c.type === 0x8066) {
          const ref = u32(c.p), prim = u32(c.p + 4), vpp = u32(c.p + 8), np = u32(c.p + 12), idx = new Array(vpp * np);
          for (let i = 0; i < idx.length; i++) idx[i] = dv.getUint16(c.p + 16 + i * 2, true);
          mesh.draws.push({ array: refIndex(ref), prim, vpp, np, idx });
        }
      }
      return mesh;
    });
    return { name: p.name, arrays, meshes };
  });
  const textures = meshM.children.filter(c => c.type === 0x0A).map(t => {
    const tx = { name: t.name, file: '', gim: null };
    for (const c of t.children) {
      if (c.type === 0x8012) { let s = ''; for (let i = c.p; i < c.p + c.plen && u8[i]; i++) s += String.fromCharCode(u8[i]); tx.file = s; }
      else if (c.type === 0x8013 && c.plen > 4) tx.gim = u8.subarray(c.p + 4, c.p + c.plen);
    }
    return tx;
  });
  const materials = meshM.children.filter(c => c.type === 8).map(m => {
    const tex = [];
    for (const l of m.children.filter(c => c.type === 9)) for (const c of l.children) if (c.type === 0x8091) tex.push(refIndex(u32(c.p)));
    return { name: m.name, tex };
  });
  const motions = animM ? animM.children.filter(c => c.type === 0x0B).map(m => {
    const mo = { name: m.name, start: 0, end: 0, rate: 30, binds: [], curves: [] };
    for (const c of m.children) {
      if (c.type === 0x80B1) { mo.start = f32(c.p); mo.end = f32(c.p + 4); }
      else if (c.type === 0x80B2) mo.rate = f32(c.p);
      else if (c.type === 0x80B3) mo.binds.push({ bone: refIndex(u32(c.p)), cmd: u32(c.p + 4), fc: refIndex(u32(c.p + 12)) });
      else if (c.type === 0x0C) {
        const fmt = u32(c.p), dims = u32(c.p + 4), nk = u32(c.p + 8), half = !!(fmt & 0x80), interp = fmt & 0x7f;
        const per = 1 + dims * (interp === 2 ? 3 : 1), keys = new Float32Array(nk * per);
        for (let i = 0; i < keys.length; i++) keys[i] = half ? halfToFloat(dv.getUint16(c.p + 16 + i * 2, true)) : f32(c.p + 16 + i * 4);
        mo.curves.push({ interp, dims, nk, per, keys });
      }
    }
    return mo;
  }) : [];
  return { u8, dv, bones, animBones, parts, textures, materials, motions, posOff, posScl, uvOff, uvScl };
}

function evalCurve(c, f) {
  const { keys, per, nk, dims, interp } = c, out = new Array(dims);
  const valAt = (k, d) => keys[k * per + 1 + (interp === 2 ? d * 3 : d)];
  if (nk === 1 || interp === 0) {
    let k = 0; while (k + 1 < nk && keys[(k + 1) * per] <= f) k++;
    for (let d = 0; d < dims; d++) out[d] = valAt(k, d); return out;
  }
  let i = 0; while (i < nk - 2 && keys[(i + 1) * per] <= f) i++;
  const f0 = keys[i * per], f1 = keys[(i + 1) * per];
  const t = Math.min(1, Math.max(0, (f - f0) / Math.max(f1 - f0, 1e-9)));
  if (interp === 4 && dims === 4) {
    const a = [0, 1, 2, 3].map(d => valAt(i, d)), b = [0, 1, 2, 3].map(d => valAt(i + 1, d));
    const qa = new THREE.Quaternion(a[0], a[1], a[2], a[3]), qb = new THREE.Quaternion(b[0], b[1], b[2], b[3]);
    qa.slerp(qb, t); return [qa.x, qa.y, qa.z, qa.w];
  }
  if (interp === 2) {
    const dt = f1 - f0, t2 = t * t, t3 = t2 * t;
    for (let d = 0; d < dims; d++) {
      const p0 = valAt(i, d), p1 = valAt(i + 1, d), m0 = keys[i * per + 3 + d * 3] * dt, m1 = keys[(i + 1) * per + 2 + d * 3] * dt;
      out[d] = (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
    }
    return out;
  }
  for (let d = 0; d < dims; d++) out[d] = valAt(i, d) + (valAt(i + 1, d) - valAt(i, d)) * t;
  return out;
}

/* ============================================================== GIM */

function decodeGIM(g) {
  if (!g) return null;
  const dv = new DataView(g.buffer, g.byteOffset, g.byteLength);
  const blocks = (s, e, want, acc) => {
    let o = s;
    while (o + 16 <= e) {
      const t = dv.getUint16(o, true), size = dv.getUint32(o + 4, true), dat = dv.getUint32(o + 12, true);
      if (!size) break;
      if (t === want) acc.push(o);
      if (t === 2 || t === 3) blocks(o + dat, o + size, want, acc);
      o += size;
    }
    return acc;
  };
  const readImg = (o) => {
    const h = o + dv.getUint32(o + 12, true);
    const fmt = dv.getUint16(h + 4, true), order = dv.getUint16(h + 6, true), w = dv.getUint16(h + 8, true), hh = dv.getUint16(h + 10, true);
    const bpp = dv.getUint16(h + 12, true), palign = dv.getUint16(h + 14, true), halign = dv.getUint16(h + 16, true);
    const off0 = dv.getUint32(h + 0x30, true);
    const pitch = Math.ceil(Math.ceil(w * bpp / 8) / palign) * palign, rows = Math.ceil(hh / halign) * halign;
    let raw = g.subarray(h + off0, h + off0 + pitch * rows);
    if (order === 1) {
      const out = new Uint8Array(pitch * rows), bw = pitch / 16;
      for (let by = 0; by < rows / 8; by++) for (let bx = 0; bx < bw; bx++) for (let y = 0; y < 8; y++)
        out.set(raw.subarray(((by * bw + bx) * 8 + y) * 16, ((by * bw + bx) * 8 + y) * 16 + 16), (by * 8 + y) * pitch + bx * 16);
      raw = out;
    }
    return { fmt, w, h: hh, pitch, raw };
  };
  const direct = (raw, fmt, n) => {
    const out = new Uint8Array(n * 4), d2 = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    for (let i = 0; i < n; i++) {
      if (fmt === 3) { out.set(raw.subarray(i * 4, i * 4 + 4), i * 4); continue; }
      const v = d2.getUint16(i * 2, true);
      let r, gg, b, a;
      if (fmt === 2) { r = (v & 15) * 17; gg = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17; a = ((v >> 12) & 15) * 17; }
      else if (fmt === 1) { r = (v & 31) * 255 / 31; gg = ((v >> 5) & 31) * 255 / 31; b = ((v >> 10) & 31) * 255 / 31; a = (v >> 15) ? 255 : 0; }
      else { r = (v & 31) * 255 / 31; gg = ((v >> 5) & 63) * 255 / 63; b = ((v >> 11) & 31) * 255 / 31; a = 255; }
      out[i * 4] = r; out[i * 4 + 1] = gg; out[i * 4 + 2] = b; out[i * 4 + 3] = a;
    }
    return out;
  };
  const im = readImg(blocks(16, g.length, 4, [])[0]);
  const rgba = new Uint8Array(im.w * im.h * 4);
  if (im.fmt === 4 || im.fmt === 5) {
    const pb = readImg(blocks(16, g.length, 5, [])[0]);
    const pal = direct(pb.raw, pb.fmt, pb.w);
    for (let y = 0; y < im.h; y++) for (let x = 0; x < im.w; x++) {
      let ix = im.fmt === 5 ? im.raw[y * im.pitch + x] : ((im.raw[y * im.pitch + (x >> 1)] >> ((x & 1) * 4)) & 15);
      rgba.set(pal.subarray(ix * 4, ix * 4 + 4), (y * im.w + x) * 4);
    }
  } else {
    const bpp = im.fmt === 3 ? 4 : 2;
    for (let y = 0; y < im.h; y++) rgba.set(direct(im.raw.subarray(y * im.pitch, y * im.pitch + im.w * bpp), im.fmt, im.w), y * im.w * 4);
  }
  return { w: im.w, h: im.h, rgba };
}

/* ============================================================== OBJX (moveset) */

function normalised(m) {
  const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
  m.decompose(p, q, s); return new THREE.Matrix4().compose(p, q, new THREE.Vector3(1, 1, 1));
}
function localTRS(b) { return new THREE.Matrix4().compose(new THREE.Vector3(...b.t), new THREE.Quaternion(...b.r), new THREE.Vector3(...b.s)); }

function buildCharacter(c, view) {
  if (view.root) { view.scene.remove(view.root); view.root = null; }
  if (view.bonesHelper) { view.scene.remove(view.bonesHelper); view.bonesHelper = null; }
  // bind worlds: from part blend offsets where available, else rest chain
  const bind = {};
  c.bones.forEach(b => b.blend.forEach((bi, k) => { if (!(bi in bind)) bind[bi] = new THREE.Matrix4().set(...b.offsets[k]).transpose().invert(); }));
  const worlds = [];
  c.bones.forEach((b, i) => { worlds.push(bind[i] ? bind[i] : (b.parent >= 0 ? worlds[b.parent].clone().multiply(localTRS(b)) : localTRS(b))); });
  const W = worlds.map(normalised);
  const tbones = c.bones.map(b => { const tb = new THREE.Bone(); tb.name = b.name; return tb; });
  const root = new THREE.Group();
  c.bones.forEach((b, i) => {
    const local = b.parent >= 0 ? W[b.parent].clone().invert().multiply(W[i]) : W[i].clone();
    local.decompose(tbones[i].position, tbones[i].quaternion, tbones[i].scale);
    tbones[i].userData.bindLocal = { p: tbones[i].position.clone(), q: tbones[i].quaternion.clone(), s: tbones[i].scale.clone() };
    (b.parent >= 0 ? tbones[b.parent] : root).add(tbones[i]);
  });
  root.updateMatrixWorld(true);
  const skeleton = new THREE.Skeleton(tbones, W.map(m => m.clone().invert()));
  // textures
  const texObjs = c.textures.map(t => {
    const im = (() => { try { return decodeGIM(t.gim); } catch (e) { return null; } })();
    if (!im) return null;
    const tx = new THREE.DataTexture(im.rgba, im.w, im.h, THREE.RGBAFormat);
    tx.wrapS = tx.wrapT = THREE.RepeatWrapping; tx.magFilter = THREE.LinearFilter; tx.minFilter = THREE.LinearFilter; tx.encoding = THREE.sRGBEncoding; tx.needsUpdate = true;
    return tx;
  });
  const mats = c.materials.map(m => new THREE.MeshBasicMaterial({ map: (m.tex.length && texObjs[m.tex[0]]) || null, vertexColors: true, skinning: true, alphaTest: 0.35, side: THREE.DoubleSide, transparent: false }));
  const fallbackMat = new THREE.MeshBasicMaterial({ color: 0x8890b8, vertexColors: true, skinning: true, side: THREE.DoubleSide });
  const dv = c.dv;
  view.partMeshes = [];
  c.parts.forEach((part, pi) => {
    const ownerIdx = c.bones.findIndex(b => b.drawPart === pi), owner = ownerIdx >= 0 ? c.bones[ownerIdx] : null;
    const P = [], UV = [], COL = [], SI = [], SW = [], tris = [];
    const base = {};
    part.meshes.forEach(mesh => {
      const subset = (owner && owner.blend.length && !mesh.subset.length) ? owner.blend.map((_, i) => i) : mesh.subset;
      mesh.draws.forEach(dc => {
        const arr = part.arrays[dc.array], L = vertexLayout(arr.word);
        if (!(dc.array in base)) {
          base[dc.array] = P.length / 3;
          for (let v = 0; v < arr.count; v++) {
            const o = arr.data + v * L.stride;
            const rd = (k, n, kind, signed) => { const out = []; const s = [0, 1, 2, 4][kind]; for (let i = 0; i < n; i++) { const q = o + L.off[k] + i * s; out.push(kind === 3 ? dv.getFloat32(q, true) : kind === 2 ? (signed ? dv.getInt16(q, true) / 32768 : dv.getUint16(q, true) / 32768) : (signed ? dv.getInt8(q) / 128 : dv.getUint8(q) / 128)); } return out; };
            let p = L.pos ? rd('p', 3, L.pos, true) : [0, 0, 0];
            p = p.map((x, i) => x * c.posScl[i] + c.posOff[i]);
            const uv = L.tc ? rd('t', 2, L.tc, false).map((x, i) => x * c.uvScl[i] + c.uvOff[i]) : [0, 0];
            let col = [1, 1, 1];
            if (L.col === 6) { const cv = dv.getUint16(o + L.off.c, true); col = [(cv & 15) / 15, ((cv >> 4) & 15) / 15, ((cv >> 8) & 15) / 15]; }
            else if (L.col === 7) col = [dv.getUint8(o + L.off.c) / 255, dv.getUint8(o + L.off.c + 1) / 255, dv.getUint8(o + L.off.c + 2) / 255];
            const w = L.nw ? rd('w', L.nw, L.wt, false) : [];
            // influences -> bone ids
            let inf = [];
            if (owner && w.length && subset.length) w.forEach((wt, j) => { if (wt > 0 && j < subset.length) inf.push([owner.blend[subset[j]], wt, subset[j]]); });
            if (!inf.length) inf = [[owner ? ownerIdx : 0, 1, -1]];
            // rebind into armature bind space: v_b = sum w * W_b * O_b * v
            if (owner && owner.offsets.length && inf[0][2] >= 0) {
              const acc = new THREE.Vector3(), tot = inf.reduce((a, x) => a + x[1], 0) || 1;
              inf.forEach(([bid, wt, s]) => { const M = W[bid].clone().multiply(new THREE.Matrix4().set(...owner.offsets[s]).transpose()); acc.add(new THREE.Vector3(...p).applyMatrix4(M).multiplyScalar(wt / tot)); });
              p = [acc.x, acc.y, acc.z];
            }
            inf.sort((a, b) => b[1] - a[1]); inf = inf.slice(0, 4);
            const sum = inf.reduce((a, x) => a + x[1], 0) || 1;
            P.push(...p); UV.push(...uv); COL.push(...col);
            for (let k = 0; k < 4; k++) { SI.push(inf[k] ? inf[k][0] : 0); SW.push(inf[k] ? inf[k][1] / sum : 0); }
          }
        }
        const b0 = base[dc.array];
        for (let k = 0; k < dc.np; k++) {
          const seq = dc.idx.slice(k * dc.vpp, (k + 1) * dc.vpp);
          if (dc.prim === 3) for (let i = 0; i + 2 < seq.length; i += 3) tris.push([mesh.material, b0 + seq[i], b0 + seq[i + 1], b0 + seq[i + 2]]);
          else if (dc.prim === 4) for (let i = 0; i + 2 < seq.length; i++) { const a = seq[i], b = seq[i + 1], cc = seq[i + 2]; if (a === b || b === cc || a === cc) continue; tris.push(i % 2 ? [mesh.material, b0 + b, b0 + a, b0 + cc] : [mesh.material, b0 + a, b0 + b, b0 + cc]); }
          else if (dc.prim === 5) for (let i = 1; i + 1 < seq.length; i++) tris.push([mesh.material, b0 + seq[0], b0 + seq[i], b0 + seq[i + 1]]);
        }
      });
    });
    tris.sort((a, b) => a[0] - b[0]);
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(P, 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute(UV, 2));
    g.setAttribute('color', new THREE.Float32BufferAttribute(COL, 3));
    g.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(SI, 4));
    g.setAttribute('skinWeight', new THREE.Float32BufferAttribute(SW, 4));
    g.setIndex(tris.flatMap(t => t.slice(1)));
    const used = []; let start = 0;
    for (let i = 0; i <= tris.length; i++) if (i === tris.length || tris[i][0] !== tris[start][0]) { if (i > start) { const mi = tris[start][0]; let slot = used.indexOf(mi); if (slot < 0) { used.push(mi); slot = used.length - 1; } g.addGroup(start * 3, (i - start) * 3, slot); } start = i; }
    const mesh = new THREE.SkinnedMesh(g, used.map(mi => mats[mi] || fallbackMat));
    mesh.frustumCulled = false;
    mesh.bind(skeleton, new THREE.Matrix4());
    root.add(mesh);
    view.partMeshes[pi] = { mesh, draw: ownerIdx };
  });
  view.scene.add(root);
  view.root = root; view.bones = tbones; view.texObjs = texObjs;   // facetex.js repaints face cells in these
  view.bonesHelper = new THREE.SkeletonHelper(root); view.bonesHelper.visible = false; view.scene.add(view.bonesHelper);
}

function poseBind(view) { view.bones.forEach(b => { const d = b.userData.bindLocal; b.position.copy(d.p); b.quaternion.copy(d.q); b.scale.copy(d.s); }); }
function poseMotion(c, view, mi, f) {
  const m = c && c.motions[mi]; if (!m) return poseBind(view);
  const ab = c.animBones;
  view.bones.forEach((b, i) => { const r = ab[i]; if (!r) return; b.position.set(...r.t); b.quaternion.set(...r.r); b.scale.set(...r.s); });
  for (const bd of m.binds) {
    const tb = view.bones[bd.bone], cv = m.curves[bd.fc]; if (!tb || !cv) continue;
    const v = evalCurve(cv, f);
    if (bd.cmd === 0x48) tb.position.set(v[0], v[1], v[2]);
    else if (bd.cmd === 0x4B) { tb.quaternion.set(v[0], v[1], v[2], v[3]); tb.quaternion.normalize(); }
    else if (bd.cmd === 0x4D) tb.scale.set(v[0], v[1], v[2]);
  }
}

/* ============================================================== loading */
