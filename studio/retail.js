// Dissidia Move Studio: retail characters straight from the game ISO (read-only).
// The ISO reader comes from the Forge Studio; the moveset decoding mirrors tools/charmod_objx.py (unpack +
// friendly names), so a retail character looks exactly like a mod folder does.
'use strict';
const ROSTER = [['one100', 'Warrior of Light'], ['one200', 'Garland'], ['two100', 'Firion'], ['two200', 'The Emperor'], ['thr100', 'Onion Knight'], ['thr200', 'Cloud of Darkness'],
  ['for100', 'Cecil'], ['for110', 'Kain'], ['for200', 'Golbez'], ['fiv100', 'Bartz'], ['fiv200', 'Exdeath'], ['fiv210', 'Gilgamesh'], ['six100', 'Terra'], ['six200', 'Kefka'],
  ['sev100', 'Cloud'], ['sev110', 'Tifa'], ['sev120', 'Aerith'], ['sev200', 'Sephiroth'], ['eht100', 'Squall'], ['eht110', 'Laguna'], ['eht200', 'Ultimecia'],
  ['nin100', 'Zidane'], ['nin200', 'Kuja'], ['ten100', 'Tidus'], ['ten110', 'Yuna'], ['ten200', 'Jecht'], ['gst100', 'Shantotto'], ['gst110', 'Prishe'],
  ['gst120', 'Vaan'], ['gst130', 'Lightning'], ['gst200', 'Gabranth'], ['org200', 'Chaos'], ['org210', 'Feral Chaos']];
const COSTUMES = [['1p', 'Normal'], ['2p', 'Alternate 1'], ['3p', 'Alternate 2'], ['zako', 'Manikin'], ['1p_a', 'Assist'], ['2p_a', 'Assist (alt 1)']];

/* ---------------------------------------------------------------- ISO (PACKAGE.BIN by path hash) */
const CRC_T = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[i] = c >>> 0; } return t; })();
function crc32(u8) { let c = 0xffffffff; for (let i = 0; i < u8.length; i++) c = CRC_T[(c ^ u8[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
const REV8 = (() => { const t = new Uint8Array(256); for (let i = 0; i < 256; i++) { let r = 0; for (let k = 0; k < 8; k++) if (i & (1 << k)) r |= 1 << (7 - k); t[i] = r; } return t; })();
function dhash(path) {
  const b = new TextEncoder().encode(path.toLowerCase()).map((x) => REV8[x]);
  const c = crc32(b); let r = 0;
  for (let k = 0; k < 32; k++) if (c & (1 << k)) r |= 1 << (31 - k);
  return r >>> 0;
}
const ISO = { file: null, pkgLba: 0, index: null, name: '' };
async function isoRead(off, len) { return new Uint8Array(await ISO.file.slice(off, off + len).arrayBuffer()); }
async function isoFind(path) {
  const pvd = await isoRead(16 * 2048, 2048);
  if (String.fromCharCode(...pvd.slice(1, 6)) !== 'CD001') throw new Error('not an ISO 9660 image');
  let rec = pvd.slice(156, 190);
  for (const part of path.split('/').filter(Boolean)) {
    const dv = new DataView(rec.buffer, rec.byteOffset), lba = dv.getUint32(2, true), size = dv.getUint32(10, true);
    const d = await isoRead(lba * 2048, size); let i = 0, found = null;
    while (i < d.length) {
      const l = d[i]; if (!l) { i = (Math.floor(i / 2048) + 1) * 2048; continue; }
      const r = d.slice(i, i + l), nm = String.fromCharCode(...r.slice(33, 33 + r[32])).split(';')[0];
      if (nm.toUpperCase() === part.toUpperCase()) { found = r; break; }
      i += l;
    }
    if (!found) throw new Error(path + ' not found: is this Dissidia 012?');
    rec = found;
  }
  const dv = new DataView(rec.buffer, rec.byteOffset);
  return { lba: dv.getUint32(2, true), size: dv.getUint32(10, true) };
}
async function isoOpen(file) {
  ISO.file = file; ISO.name = file.name;
  const pkg = await isoFind('PSP_GAME/USRDIR/DATA/PACKAGE.BIN'), inf = await isoFind('PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN');
  const info = await isoRead(inf.lba * 2048, inf.size), dv = new DataView(info.buffer);
  if (String.fromCharCode(...info.slice(4, 8)) !== 'pack') throw new Error('PACKAGE_INFO.BIN is not in the expected format');
  const n = dv.getUint32(8, true), index = new Map();
  for (let i = 0; i < n; i++) { const o = 16 + i * 12; index.set(dv.getUint32(o, true), [dv.getUint32(o + 4, true), dv.getUint32(o + 8, true) & 0x0FFFFFFF]); }
  ISO.pkgLba = pkg.lba; ISO.index = index;
}
const isoHas = (name) => !!(ISO.index && ISO.index.has(dhash(name)));
async function isoGet(name) { const [sec, size] = ISO.index.get(dhash(name)); return isoRead((ISO.pkgLba + sec) * 2048, size); }
const costumesOf = (code) => COSTUMES.filter(([c]) => isoHas(`obj/p_${code}_${c}.gmo`) && isoHas(`obj/p_${code}${c.endsWith('_a') ? '_a' : ''}.objx`));

/* remember the picked ISO (Chrome keeps file handles in IndexedDB; the browser asks again before reading) */
const IDB = {
  db() { return new Promise((res, rej) => { const r = indexedDB.open('dissidia-move-studio', 1); r.onupgradeneeded = () => r.result.createObjectStore('kv'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); }); },
  async get(k) { try { const db = await this.db(); return await new Promise((res) => { const r = db.transaction('kv').objectStore('kv').get(k); r.onsuccess = () => res(r.result); r.onerror = () => res(null); }); } catch (e) { return null; } },
  async set(k, v) { try { const db = await this.db(); db.transaction('kv', 'readwrite').objectStore('kv').put(v, k); } catch (e) { /* private window */ } },
};

/* ---------------------------------------------------------------- objx -> the mod-folder JSON (charmod_objx.unpack) */
const f32Short = (f) => { for (let p = 1; p < 10; p++) { const x = Number(f.toPrecision(p)); if (Math.fround(x) === f) return x; } return f; };
const _fb = new DataView(new ArrayBuffer(4));
function vOut(v) {
  if (v < 0x10000 || v >= 0xFFFF0000) return v;
  _fb.setUint32(0, v, true); const f = _fb.getFloat32(0, true);
  return (isFinite(f) && Math.abs(f) > 1e-6 && Math.abs(f) < 1e7) ? { f: f32Short(f) } : v;
}
const hexU = (v, w) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(w, '0');
const bhex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');
function arcRead(u8) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if (String.fromCharCode(...u8.slice(0, 3)) !== 'ARC') throw new Error('not an objx');
  const n = dv.getUint32(4, true), out = [];
  for (let i = 0; i < n; i++) {
    const o = 16 + i * 16, t = dv.getUint32(o, true), tag = String.fromCharCode(...u8.slice(o + 4, o + 8)).replace(/\0+$/, '');
    const off = dv.getUint32(o + 8, true), size = dv.getUint32(o + 12, true);
    out.push({ type: t, tag, rawTag: u8.subarray(o + 4, o + 8), data: u8.subarray(off, off + size) });
  }
  return out;
}
const DV = (d) => new DataView(d.buffer, d.byteOffset, d.byteLength);
function evDecode(b) {
  const dv = DV(b), L = b.length;
  const keyedAt = (p, n, out) => {
    for (let k = 0; k < n; k++) {
      if (p + 8 > L) return -1;
      const fr = dv.getUint16(p, true), op = dv.getUint16(p + 2, true), x = dv.getUint32(p + 4, true), na = op >> 8;
      if (p + 8 + 4 * na > L) return -1;
      const e = { frame: fr, op: hexU(op, 4), args: [] };
      for (let a = 0; a < na; a++) e.args.push(vOut(dv.getUint32(p + 8 + 4 * a, true)));
      if (x) e.x = x;
      out.push(e); p += 8 + 4 * na;
    }
    return p;
  };
  if (L >= 8 && !(dv.getUint16(2, true) & 0xFF)) {
    const cnt = dv.getUint16(2, true), offK = dv.getUint16(4, true), keyed = [];
    if (offK !== 8 || dv.getUint16(6, true) !== L) return null;
    return keyedAt(8, cnt >> 8, keyed) === L ? { motion: dv.getUint16(0, true), ranged: [], keyed } : null;
  }
  if (L < 20) return null;
  const h = []; for (let i = 0; i < 8; i++) h.push(dv.getUint16(i * 2, true));
  const nR = (h[1] & 0xFF) - 1, nK = h[1] >> 8, offK = h[2];
  if (h[3] !== L || nR < 0 || offK > L) return null;
  const keyed = [];
  if (keyedAt(offK, nK, keyed) !== L) return null;
  const n0 = h[6] >> 8;
  if (20 + 4 * n0 > offK) return null;
  const s0 = { start: h[4], end: h[5], op: hexU(h[6], 4), args: [] };
  for (let a = 0; a < n0; a++) s0.args.push(vOut(dv.getUint32(20 + 4 * a, true)));
  if (h[7]) s0.pad = h[7];
  const x0 = dv.getUint32(16, true); if (x0) s0.x = x0;
  const ranged = [s0]; let p = 20 + 4 * n0;
  for (let k = 0; k < nR; k++) {
    if (p + 12 > offK) return null;
    const s = dv.getUint16(p, true), en = dv.getUint16(p + 2, true), op = dv.getUint16(p + 4, true), pad = dv.getUint16(p + 6, true), x = dv.getUint32(p + 8, true), na = op >> 8;
    if (p + 12 + 4 * na > offK) return null;
    const e = { start: s, end: en, op: hexU(op, 4), args: [] };
    for (let a = 0; a < na; a++) e.args.push(vOut(dv.getUint32(p + 12 + 4 * a, true)));
    if (pad) e.pad = pad;
    if (x) e.x = x;
    ranged.push(e); p += 12 + 4 * na;
  }
  return p === offK ? { motion: h[0], ranged, keyed } : null;
}
const HIT_FIELDS = [[0x00, 'H', 'id'], [0x02, 'H', 'unk_02'], [0x04, 'B', 'class'], [0x05, 'B', 'reaction'], [0x06, 'B', 'unk_06'],
  [0x07, 'b', 'unk_07'], [0x08, 'I', 'flags'], [0x0C, 'B', 'hit_effect_bit'], [0x0D, 'B', 'hitstop'], [0x0E, 'B', 'unk_0e'], [0x0F, 'B', 'unk_0f'],
  [0x10, 'h', 'power'], [0x12, 'H', 'rehit'], [0x14, '4s', 'shake'], [0x18, 'f', 'kb_yaw'], [0x1C, 'f', 'kb_forward'], [0x20, 'f', 'kb_up'],
  [0x24, 'f', 'stun'], [0x28, 'f', 'follow'], [0x2C, 'h', 'kb_bone'], [0x2E, 'H', 'unk_2e'], [0x30, 'I', 'unk_30']];
function hitDecode(r) {
  const dv = DV(r), d = {};
  for (const [o, f, n] of HIT_FIELDS) {
    let v;
    if (f === 'H') v = dv.getUint16(o, true); else if (f === 'h') v = dv.getInt16(o, true); else if (f === 'B') v = dv.getUint8(o);
    else if (f === 'b') v = dv.getInt8(o); else if (f === 'I') v = hexU(dv.getUint32(o, true), 8); else if (f === '4s') v = bhex(r.subarray(o, o + 4));
    else { const x = dv.getFloat32(o, true); v = isFinite(x) ? f32Short(x) : x; }
    if (n === 'id') v = hexU(v, 4);
    d[n] = v;
  }
  let name = ''; for (let i = 52; i < 68 && r[i]; i++) name += String.fromCharCode(r[i]);
  const { id, ...rest } = d;
  return { id, name, ...rest };
}
function effectsOf(effe) {
  try {
    const dv = DV(effe);
    if (String.fromCharCode(...effe.slice(0, 4)) !== 'MPK ') return [];
    const n = dv.getUint32(8, true), ents = {};
    for (let i = 0; i < n; i++) { const o = 16 + 16 * i; ents[dv.getUint32(o, true)] = effe.subarray(dv.getUint32(o + 4, true), dv.getUint32(o + 4, true) + dv.getUint32(o + 8, true)); }
    const fep = ents[0x40], fepa = ents[0x4D];
    const names = [];
    if (fep) {
      const fd = DV(fep), p = fd.getUint32(0x44, true);
      if (p) {
        const cnt = fd.getUint32(p, true), arr = fd.getUint32(p + 4, true);
        for (let i = 0; i < cnt; i++) {
          const ln = fd.getUint32(arr + 12 * i + 4, true), sp = fd.getUint32(arr + 12 * i + 8, true);
          names.push(String.fromCharCode(...fep.slice(sp, sp + ln)).replace(/\0.*$/, ''));
        }
      }
    }
    const by = new Map();
    if (fepa) {
      const ad = DV(fepa), cnt = ad.getUint32(12, true);
      const cstr = (o) => { let s = ''; for (let i = o; i < o + 32 && fepa[i]; i++) s += String.fromCharCode(fepa[i]); return s; };
      for (let i = 0; i < cnt; i++) {
        const r = 16 + 260 * i, id = ad.getUint32(r + 0x24, true), ei = ad.getUint32(r + 0x28, true);
        if (!by.has(id)) by.set(id, { id, name: '', bones: [], motions: [] });
        const e = by.get(id), bone = cstr(r + 0xA8), mo = cstr(r + 0xC8);
        if (ei < names.length && !e.name) e.name = names[ei];
        if (bone && !e.bones.includes(bone)) e.bones.push(bone);
        if (mo && !e.motions.includes(mo)) e.motions.push(mo);
      }
    }
    return [...by.values()].sort((a, b) => a.id - b.id);
  } catch (e) { return []; }
}
function friendlyEvents(t, hitName, boneName) {
  const conv = (e, kind) => {
    const code = parseInt(e.op, 16), info = OPS.ops[hexU(code, 4)];
    if (!info || !info.name) return e;
    const names = info.args.slice(0, code >> 8), out = {};
    for (const k of ['start', 'end', 'frame']) if (k in e) out[k] = e[k];
    out.op = info.name;
    names.forEach((n, i) => {
      let v = e.args[i];
      if (n === 'hit' && typeof v === 'number') v = hitName.get(v) || (v < 0x10000 ? hexU(v, 4) : v);
      else if (n === 'part' && typeof v === 'number') v = boneName.get(v) ?? v;
      out[n] = v;
    });
    if (e.args.length > names.length) out.args = e.args.slice(names.length);
    if ('x' in e) out.x = e.x;
    if ('pad' in e) out.pad = e.pad;
    return out;
  };
  t.ranged = t.ranged.map((e) => conv(e, 'ranged'));
  t.keyed = t.keyed.map((e) => conv(e, 'keyed'));
  return t;
}
function hitEncode(d) {
  const r = new Uint8Array(68), dv = DV(r);
  for (const [o, f, n] of HIT_FIELDS) {
    let v = d[n];
    if (f === '4s') { for (let i = 0; i < 4; i++) r[o + i] = parseInt(v.slice(2 * i, 2 * i + 2), 16); continue; }
    if (typeof v === 'string') v = parseInt(v, 16);
    if (f === 'H') dv.setUint16(o, v, true); else if (f === 'h') dv.setInt16(o, v, true); else if (f === 'B') dv.setUint8(o, v);
    else if (f === 'b') dv.setInt8(o, v); else if (f === 'I') dv.setUint32(o, v >>> 0, true); else dv.setFloat32(o, v, true);
  }
  if (d.name_raw) for (let i = 0; i < 16; i++) r[52 + i] = parseInt(d.name_raw.slice(2 * i, 2 * i + 2), 16);
  else for (let i = 0; i < Math.min(15, d.name.length); i++) r[52 + i] = d.name.charCodeAt(i);
  return r;
}
const sameBytes = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const NOTES = {
  moves: 'Commands: an ability record points at one by its cmd id. Steps = presses; action ids are in actions.json. Step flags: 0x100 = needs a press, 0 = plays by itself once the previous hit connected.',
  actions: 'Action records (116 bytes). "action" = id used by moves.json steps, "script" -> motion_map.json; both override the matching bytes of raw.',
  motion_map: 'script id -> motion index (= timelines/NNN.json and the animation in the model)',
  timelines: 'Timeline order in the file. Events: ticks (2 per animation frame); "what" is a note.',
  arc: 'ARC entry order; each entry is packed from the named file.',
  bones: 'bone names of the model (index = bone number); timelines name parts by these. Comes from the model; not packed.',
};
/* objx -> the files of a moveset folder, as charmod_objx.unpack writes them: {path: object (JSON) | Uint8Array (binary)}.
   cmdNotes: {cmd id: "Name (ability NN)"} for moves.json. The projectile section stays binary (sections/NN_magi_1.bin). */
function decodeMoveset(u8, boneNames, cmdNotes = {}) {
  const files = {}, ents = arcRead(u8), done = new Set();
  const arc = { _info: NOTES.arc, header_extra: bhex(u8.subarray(8, 16)), entries: [] };
  const sec = {};
  ents.forEach((e, i) => {
    const key = e.type === 7 ? 'motion_map' : e.type === 8 ? 'timelines' : (e.type === 1 && ['comb', 'plya', 'atkp'].includes(e.tag) ? e.tag : null);
    const ent = { type: e.type, tag: bhex(e.rawTag) };
    if (key && !done.has(key)) { done.add(key); sec[key] = e.data; ent.file = { comb: 'moves.json', plya: 'actions.json', atkp: 'hits.json', motion_map: 'motion_map.json', timelines: 'timelines/index.json' }[key]; }
    else {
      const safe = Array.from(e.tag, (c) => /[\p{L}\p{N}]/u.test(c) ? c : '_').join('') || 'x';
      ent.file = `sections/${String(i).padStart(2, '0')}_${safe}_${e.type}.bin`;
      files[ent.file] = e.data;
    }
    arc.entries.push(ent);
  });
  files['arc.json'] = arc;
  if (sec.comb) {
    const d = sec.comb, dv = DV(d), n = dv.getUint16(2, true), cmds = [];
    for (let i = 0; i < n; i++) {
      const o = dv.getUint32(4 + 4 * i, true), cid = dv.getUint16(o, true), ns = dv.getUint16(o + 2, true), steps = [];
      for (let k = 0; k < ns; k++) { const so = o + 8 + 8 * k; steps.push({ action: dv.getUint16(so, true), b: dv.getUint16(so + 2, true), c: dv.getUint16(so + 4, true), flags: dv.getUint16(so + 6, true) }); }
      const c = { id: hexU(cid, 2), prereq: [dv.getUint16(o + 4, true), dv.getUint16(o + 6, true)], steps };
      if (cmdNotes[cid]) c._info = cmdNotes[cid];
      cmds.push(c);
    }
    files['moves.json'] = { _info: NOTES.moves, version: dv.getUint16(0, true), commands: cmds };
  }
  if (sec.plya) {
    const d = sec.plya, dv = DV(d), n = dv.getUint32(4, true), acts = [];
    for (let i = 0; i < n; i++) { const o = 8 + 116 * i; acts.push({ action: dv.getUint16(o, true), script: dv.getUint16(o + 0x1C, true), raw: bhex(d.subarray(o, o + 116)) }); }
    files['actions.json'] = { _info: NOTES.actions, header: dv.getUint32(0, true), actions: acts, tail: bhex(d.subarray(8 + 116 * n)) };
  }
  if (sec.motion_map) {
    const d = sec.motion_map, dv = DV(d), n = dv.getUint32(4, true), rows = [];
    for (let i = 0; i < n; i++) rows.push({ script: dv.getUint16(8 + 8 * i, true), motion: dv.getUint16(10 + 8 * i, true), phase: dv.getUint32(12 + 8 * i, true) });
    files['motion_map.json'] = { _info: NOTES.motion_map, version: dv.getUint32(0, true), rows, tail: bhex(d.subarray(8 + 8 * n)) };
  }
  const hits = [];
  if (sec.atkp) {
    const d = sec.atkp, dv = DV(d), n = dv.getUint32(4, true);
    for (let i = 0; i < n; i++) {
      const r = d.subarray(8 + 68 * i, 8 + 68 * (i + 1)), h = hitDecode(r);
      const nb = new Uint8Array(16); for (let k = 0; k < h.name.length; k++) nb[k] = h.name.charCodeAt(k);
      if (!sameBytes(nb, r.subarray(52, 68))) h.name_raw = bhex(r.subarray(52, 68));
      if (!sameBytes(hitEncode(h), r)) h.raw = bhex(r);
      hits.push(h);
    }
    files['hits.json'] = { _info: (typeof OPS !== 'undefined' && OPS.hit_info) || {}, header: dv.getUint32(0, true), hits, tail: bhex(d.subarray(8 + 68 * n)) };
  }
  const cnt = {}; hits.forEach((h) => { cnt[h.name] = (cnt[h.name] || 0) + 1; });
  const hitName = new Map(hits.filter((h) => cnt[h.name] === 1 && h.name && !h.name.startsWith('0x')).map((h) => [parseInt(h.id, 16), h.name]));
  const bcnt = {}; boneNames.forEach((b) => { bcnt[b] = (bcnt[b] || 0) + 1; });
  const boneName = new Map(boneNames.map((b, i) => [i, b]).filter(([i, b]) => bcnt[b] === 1 && b && !b.startsWith('0x')));
  if (boneNames.length) files['bones.json'] = { _info: NOTES.bones, bones: boneNames };
  if (sec.timelines) {
    const d = sec.timelines, dv = DV(d), n = dv.getUint32(4, true), offs = [], list = [];
    for (let i = 0; i < n; i++) offs.push(dv.getUint32(8 + 4 * i, true));
    offs.push(d.length);
    for (let k = 0; k < n; k++) {
      const b = d.subarray(offs[k], offs[k + 1]), t = evDecode(b);
      if (!t) { const fn = `timelines/${String(k).padStart(3, '0')}_raw.bin`; files[fn] = b; list.push(fn); continue; }
      let fn = `timelines/${String(t.motion).padStart(3, '0')}.json`;
      if (list.includes(fn)) fn = `timelines/${String(t.motion).padStart(3, '0')}_${k}.json`;
      list.push(fn);
      files[fn] = friendlyEvents(t, hitName, boneName);
    }
    files['timelines/index.json'] = { _info: NOTES.timelines, version: dv.getUint32(0, true), files: list };
  }
  const effe = ents.find((e) => e.tag === 'effe');
  if (effe) files['effects.json'] = { _info: 'Effect records of this character (the `effect` event takes the id). 9999 = weapon-trail records, 8888 = other weapon / hand effects (used by bone_effect). A note: not packed.', effects: effectsOf(effe.data) };
  return files;
}

/* ---------------------------------------------------------------- retail ability names (tools/roster_mainbin.py) */
// main.bin (MPK) -> system.bin (ARC) -> 'chrp' (charId -> code at +0x74) and 'ablt' (40-byte records: +0 id, +2 name text,
// +12 command id, +0x16 type, +0x17 phase, +0x1B owner charId); names: general_archive/main/en/main_lang.bin -> ability.bin.
const MESS_KEY = [0xED98, 0x90AB, 0x5E28, 0x8442, 0x32B1, 0x2DDA, 0xA657, 0xC8BF, 0x0033, 0x579A, 0xF227, 0x4B4C, 0x36A7, 0x927D, 0x739D, 0x84CE];
function mpkNamed(d) {
  const dv = DV(d), n = dv.getUint32(8, true), out = {};
  for (let i = 0; i < n; i++) {
    const o = 16 + 16 * i, no = dv.getUint32(o, true), off = dv.getUint32(o + 4, true), size = dv.getUint32(o + 8, true);
    let nm = ''; for (let k = no; d[k]; k++) nm += String.fromCharCode(d[k]);
    out[nm] = d.subarray(off, off + size);
  }
  return out;
}
function messRecords(e) {
  const dv = DV(e), out = [];
  const dec = (i, a, nbytes) => { let s = ''; for (let j = 0; j < nbytes / 2; j++) s += String.fromCharCode(dv.getUint16(a + 2 * j, true) ^ MESS_KEY[(i + j) % 16]); return s.replace(/\0+$/, ''); };
  if (String.fromCharCode(...e.slice(0, 4)) === 'OFF4') {
    const n = dv.getUint32(4, true);
    for (let i = 0; i < n; i++) { const o = 4 * dv.getUint32(8 + 4 * i, true); out.push(dec(i, o + 4, dv.getUint32(o, true))); }
    return out;
  }
  const n = dv.getUint16(0, true);
  for (let i = 0; i < n; i++) { const a = dv.getUint16(2 + 2 * i, true) * 2; out.push(dec(i, a + 2, dv.getUint16(a, true))); }
  return out;
}
const shownText = (s) => s.replace(/\x10../g, '').replace(/\x11./g, '').replace(/\x1b/g, '').trim();
const PHASES = { 0: 'ground', 1: 'midair', 0xFF: 'follow-up' }, TYPES = { 0: 'brave', 1: 'hp' };
/* -> {code: {move_names like a mod.json: {'<n hex>': {cmd, name, type, phase}}}} */
function retailAbilities(mainBin, langArc) {
  const main = mpkNamed(mainBin), sys = arcRead(main['system.bin']);
  const names = messRecords(mpkNamed(langArc)['ability.bin']);
  const chrp = sys.find((e) => e.tag === 'chrp').data, cdv = DV(chrp), codeOf = {};
  for (let i = 0, n = cdv.getUint32(4, true); i < n; i++) {
    const r = 8 + 0x90 * i; let c = '';
    for (let k = 0; k < 8 && chrp[r + 0x74 + k]; k++) c += String.fromCharCode(chrp[r + 0x74 + k]);
    codeOf[chrp[r]] = c.toLowerCase().replace(/^p_/, '');
  }
  const ablt = sys.find((e) => e.tag === 'ablt').data, adv = DV(ablt), out = {};
  for (let i = 0, n = adv.getUint32(8, true); i < n; i++) {
    const r = 16 + 40 * i, id = adv.getUint16(r, true), owner = ablt[r + 0x1B], code = codeOf[owner];
    if (!code) continue;
    const cmd = adv.getUint16(r + 12, true), nm = shownText(names[adv.getUint16(r + 2, true)] || '');
    const set = (out[code] = out[code] || {}), key = (id & 0xFF).toString(16).toUpperCase().padStart(2, '0');
    set[key] = { cmd: hexU(cmd, 2), name: nm, type: TYPES[ablt[r + 0x16]] || 'other', phase: PHASES[ablt[r + 0x17]] || '',
      cp: adv.getInt16(r + 0x0E, true), cp_mastered: adv.getInt16(r + 0x10, true), ap: adv.getInt16(r + 0x12, true), bonus: adv.getInt16(r + 0x14, true) };   // docs/archive/ablt-fields
  }
  return out;
}
