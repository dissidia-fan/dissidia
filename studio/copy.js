/* Move Studio: copy a move from another character (a retail one from the ISO, or another mod folder) into this mod.
   Loaded after fxui.js. What comes along:
     the command (new id), its presses' actions (new action and script ids; follow-ups their chain windows offer too),
     the motion_map rows, the animations (appended to every model of this moveset; bones matched by name),
     the timelines (events renumbered), the hits, the hitbox groups (bones matched by name), the projectiles
     (with their child chain and float pool), the effects (copied into this package with their textures),
     the character's own sounds (appended to this bank, never over slots 0–123), the HP banner and the CPU row.
   What does not: parts this character doesn't have (weapon show / hide on bones it lacks), face textures and voices,
   which are per character. */

const CP = { notes: {} };
const cpHex = (v, w) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(w, '0');
const cpU8 = (h) => Uint8Array.from((h || '').match(/../g) || [], (x) => parseInt(x, 16));
const cpHexOf = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');

/* ---------------------------------------------------------------- the source character */
async function cpSource(src, assist) {         // {label, code, files: {rel: obj | Uint8Array}, gmoBytes, names: move_names}
  if (src.retail) {
    const code = src.retail, c = S.retailChars.find((x) => x.code === code);
    const cos = c && c.costumes.find(([x]) => x.endsWith('_a') === !!assist) || (c && c.costumes[0]);
    if (!cos) throw new Error('no model of ' + code + ' in the ISO');
    const ms = `p_${code}${cos[0].endsWith('_a') ? '_a' : ''}`, gmo = `obj/p_${code}_${cos[0]}.gmo`;
    const gmoBytes = await isoGet(gmo), files = await retailDecoded(`obj/${ms}.objx`, gmo);
    return { label: c.label, code, files, gmoBytes, names: S.abilities[code] || {} };
  }
  const dir = src.mod + '/', mod = await FS.json(dir + 'mod.json'), rigs = [];
  for (const it of await FS.list(dir + 'models').catch(() => [])) if (it.name.endsWith('.gmo')) rigs.push(it.name.slice(0, -4));
  const stem = rigs.find((r) => assist ? /_a$/.test(r) : /_1p$/.test(r)) || rigs.find((r) => !/_a$|_w$/.test(r)) || rigs[0];
  if (!stem) throw new Error(src.mod + ' has no model');
  const ms = dir + 'moveset/' + MOVESET_OF(stem) + '/', files = {};
  const arc = await FS.json(ms + 'arc.json'); files['arc.json'] = arc;
  for (const e of arc.entries) {
    const bytes = await FS.read(ms + e.file).catch(() => null); if (!bytes) continue;
    files[e.file] = e.file.endsWith('.json') ? JSON.parse(new TextDecoder().decode(bytes)) : bytes;
  }
  for (const fn of (files['timelines/index.json'] || { files: [] }).files) if (fn.endsWith('.json')) files[fn] = await FS.json(ms + fn).catch(() => null);
  return { label: (mod.character && mod.character.name) || src.mod, code: (mod.character && mod.character.code) || src.mod, files, gmoBytes: await FS.read(dir + 'models/' + stem + '.gmo'), names: mod.move_names || {} };
}
function cpSection(src, tag, type) {
  const e = src.files['arc.json'].entries.find((x) => (tag ? x.tag === tag : true) && (type == null || x.type === type));
  return e ? { file: e.file, data: src.files[e.file] } : null;
}
function cpMoves(src) {                          // [{cmd, label, note, named}]
  const byCmd = {};
  for (const [k, v] of Object.entries(src.names)) if (!k.startsWith('_') && v && v.cmd) byCmd[parseInt(v.cmd, 16)] = { ...v, key: k };
  const cmds = src.files['moves.json'].commands;
  return cmds.map((c) => {
    const id = parseInt(c.id, 16), own = byCmd[id], par = !own && c.prereq && c.prereq[0] !== 0xFFFF ? byCmd[c.prereq[0]] : null, nm = own || par;   // a shorter version: its move's name
    const name = nm ? (nm.name || nm.was || '').replace(/\{(ground|midair)\}/g, '($1)') : (c._info || '').replace(/\s*\(ability \w+\)$/, '');
    return { cmd: id, label: (name || 'Command ' + c.id) + (par ? ' (shorter)' : ''), note: `${c.steps.length} press${c.steps.length > 1 ? 'es' : ''}${nm ? ` · ${nm.type || ''} ${nm.phase || ''}` : ''}`, named: !!own, info: own };
  });
}

/* ---------------------------------------------------------------- GMO motions */
function cpAnimModel(tree) { return [...gcWalk(tree.root)].find((c) => c.type === GC.MODEL && gcFind(c, 0x0B).length) || null; }
function cpTree(bytes) { const roots = gcParse(bytes, 16, bytes.length); return { magic: bytes.subarray(0, 16), root: roots[0] }; }
function cpTreeBytes(t) { const out = new Uint8Array(16 + gcSize(t.root)); out.set(t.magic, 0); gcWrite(t.root, out, 16); return out; }
/* a block header holding `name`: two u32 (header size, header size) + the name, NUL padded to 4, like dfx/chunks.py */
function cpSetChunkName(c, name) {
  const nb = new TextEncoder().encode(name), raw = new Uint8Array(nb.length + 1 + ((-(nb.length + 1)) % 4 + 4) % 4);
  raw.set(nb, 0);
  const hs = 8 + 8 + raw.length, h = new Uint8Array(8 + raw.length), dv = new DataView(h.buffer);
  dv.setUint32(0, hs, true); dv.setUint32(4, hs, true); h.set(raw, 8);
  c.header = h;
}
function cpChunkClone(c) {
  if (c.type & 0x8000) return { type: c.type, payload: c.payload.slice() };
  return { type: c.type, header: c.header.slice(), payload: c.payload ? c.payload.slice() : null, children: (c.children || []).map(cpChunkClone) };
}
/* motion `mi` of the source as a chunk for this rig: bones by name; on another skeleton translation keys of bones
   whose rest offset differs are dropped (the bone keeps this rig's length), the root-motion bone keeps its travel */
function cpMotionChunk(srcGmo, srcTree, mi, dstGmo, newName, stat) {
  const m = cpChunkClone(gcFind(cpAnimModel(srcTree), 0x0B)[mi]);
  const sb = srcGmo.animBones, db = dstGmo.animBones, dIdx = new Map();
  db.forEach((b, i) => { if (!dIdx.has(b.name)) dIdx.set(b.name, i); });
  const same = sb.length === db.length && sb.every((b, i) => b.name === db[i].name);
  m.children = m.children.filter((c) => {
    if (c.type !== 0x80B3) return true;
    const dv = new DataView(c.payload.buffer, c.payload.byteOffset, 16), ref = dv.getUint32(0, true), bi = ref & 0xFFF, cmd = dv.getUint32(4, true);
    if (same) return true;
    const s = sb[bi], di = s ? dIdx.get(s.name) : undefined;
    if (di === undefined) { stat.noBone.add(s ? s.name : '#' + bi); return false; }
    if (cmd === 0x48 && !/include$/.test(s.name) && di !== 0) {
      const a = s.t, b = db[di].t, d = Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]), L = Math.max(0.05, Math.hypot(...b));
      if (d > 0.1 * L) { stat.keptLength.add(s.name); return false; }
    }
    dv.setUint32(0, ((ref & ~0xFFF) | di) >>> 0, true);
    return true;
  });
  stat.same = same;
  if (m.header.length > 8) {                     // the name (header +8, NUL padded)
    const room = m.header.length - 9, nb = new TextEncoder().encode(newName).subarray(0, room);
    m.header.fill(0, 8); m.header.set(nb, 8);
  }
  return m;
}

/* ---------------------------------------------------------------- projectiles (magi) */
const CP_PARAMS = ['shots', 'first_delay', 'interval', 'fwd_dist', 'fwd_yaw', 'off_x', 'off_y', 'off_z', 'launch_pitch', 'launch_yaw', 'aim_dist', 'aim_yaw', 'aim_height', 'move_time', 'speed', 'speed_min', 'speed_max', 'accel', 'accel_start', 'turn_rate', 'homing_start', 'homing_time', 'homing_stop', 'gravity', 'gravity_start', 'size', 'size_min', 'size_max', 'size_rate', 'size_start', 'emit_count', 'emit_delay', 'emit_interval', 'tgt_dist_min', 'tgt_dist_max', 'tgt_height_min', 'tgt_height_max', 'pull_start', 'pull_time', 'pull_strength', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist', 'lifetime', 'hit_time', 'hit_delay'];
function cpMagiJson(u8) {                        // a retail 'magi' section as projectiles.json (charmod_magi's unpack)
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = dv.getUint32(8, true), nf = dv.getUint32(12, true), pool = [], records = [];
  for (let i = 0; i < nf; i++) pool.push(dv.getFloat32(16 + 204 * n + 4 * i, true));
  const H = (o) => dv.getUint16(o, true), h = (o) => dv.getInt16(o, true);
  for (let i = 0; i < n; i++) {
    const o = 16 + 204 * i, params = {};
    CP_PARAMS.forEach((nm, k) => { const q = o + 0x40 + 3 * k; params[nm] = [dv.getInt8(q), dv.getInt8(q + 1), dv.getInt8(q + 2)]; });
    const flags = dv.getUint32(o + 0x38, true);
    records.push({ id: H(o), kind: H(o + 2), effect: H(o + 4), common_effect: H(o + 6), max_alive: H(o + 8), atkp: H(o + 0x0A), atkp_reflected: H(o + 0x0C),
      link_emit: h(o + 0x0E), link_terrain: h(o + 0x10), link_terrain_1: h(o + 0x12), link_terrain_2: h(o + 0x14), link_terrain_4: h(o + 0x16), link_hit: h(o + 0x18), link_fall: h(o + 0x1A), link_clash: h(o + 0x1C),
      origin: H(o + 0x1E), origin_arg: H(o + 0x20), direction: H(o + 0x22), orient: h(o + 0x24), target: H(o + 0x26), motion: H(o + 0x28), unk_2a: H(o + 0x2A),
      hit_radius: dv.getFloat32(o + 0x2C, true), hit_length: dv.getFloat32(o + 0x30, true), bounce_speed: dv.getFloat32(o + 0x34, true),
      flags: '0x' + flags.toString(16).padStart(8, '0'), ai_tags: cpHexOf(u8.subarray(o + 0x3C, o + 0x40)), flag_names: ED_FLAGS.filter((_, b) => flags & (1 << b)),
      tail: cpHexOf(u8.subarray(o + 0x40 + 3 * 46, o + 204)), params });
  }
  return { pool, records };
}
const CP_LINKS = ['link_emit', 'link_terrain', 'link_terrain_1', 'link_terrain_2', 'link_terrain_4', 'link_hit', 'link_fall', 'link_clash'];

/* ---------------------------------------------------------------- the copy */
async function cpCopyMove(srcSpec, cmdId) {
  if (!mvCanEdit()) { toast(mvWhyNot() || 'This character can\'t get new moves here.'); return; }
  const assist = /_a$/.test(S.msDir);
  let src; try { src = await cpSource(srcSpec, assist); } catch (e) { toast('Could not read that character: ' + e.message, 'bad'); return; }
  const T = src.files, rep = { copied: [], left: [], approx: [] };
  const srcCmd = T['moves.json'].commands.find((c) => parseInt(c.id, 16) === cmdId); if (!srcCmd) return;
  const mvInfo = cpMoves(src).find((x) => x.cmd === cmdId), moveName = mvInfo.label;
  const srcGmo = parseGMO(src.gmoBytes), srcTree = cpTree(src.gmoBytes);
  const sBones = srcGmo.bones.map((b) => b.name), sParent = srcGmo.bones.map((b) => b.parent);
  const sActs = new Map(T['actions.json'].actions.map((a) => [a.action, a])), sRows = T['motion_map.json'].rows;
  const sTL = new Map(); for (const fn of T['timelines/index.json'].files) { const t = T[fn]; if (t && t.motion != null && !sTL.has(t.motion)) sTL.set(t.motion, t); }
  const sHits = T['hits.json'].hits, sHitById = new Map(sHits.map((x) => [parseInt(x.id, 16), x]));
  const cnt = {}; sHits.forEach((x) => { cnt[x.name] = (cnt[x.name] || 0) + 1; });
  const sHitOf = (v) => typeof v === 'number' ? sHitById.get(v) : (/^0x/i.test(v) ? sHitById.get(parseInt(v, 16)) : sHits.find((x) => x.name === v && cnt[v] === 1));
  const sVolSec = cpSection(src, null, 5), sVols = sVolSec ? edVolParse(sVolSec.data) : { recs: [] };
  const sMagiSec = cpSection(src, '6d616769'), sMagi = !sMagiSec ? { pool: [], records: [] } : sMagiSec.data instanceof Uint8Array ? cpMagiJson(sMagiSec.data) : sMagiSec.data;
  const sMagiById = new Map(sMagi.records.map((r) => [r.id, r]));
  const sSndSec = cpSection(src, null, 14); let sBank = null; try { if (sSndSec) sBank = SndBank.parseBank(sSndSec.data); } catch (e) { /* no bank */ }
  const sTextSec = cpSection(src, '74657874'); let sText = null; try { if (sTextSec && typeof OBJTEXT !== 'undefined') sText = OBJTEXT.parseText(sTextSec.data); } catch (e) { /* no text */ }
  const sEffSec = cpSection(src, '65666665'); let sPkg = null; try { if (sEffSec) sPkg = new FxePackage(sEffSec.data); } catch (e) { /* no package */ }
  const sBratSec = cpSection(src, '62726174');

  /* 1. what the move uses: actions (follow-ups too), motions, hits, groups, projectiles, effects, sounds */
  const actList = [], actSet = new Set(), motions = [], motionSet = new Set();
  const want = (a) => { if (a == null || actSet.has(a) || !sActs.has(a)) return; actSet.add(a); actList.push(a); };
  srcCmd.steps.forEach((s) => want(s.action));
  const ARGS = {}; for (const [code, o] of Object.entries(OPS.ops)) if (o.name) ARGS[o.name] = { code: parseInt(code, 16), args: o.args };
  for (let k = 0; k < actList.length && k < 40; k++) {
    const a = sActs.get(actList[k]);
    for (const r of sRows.filter((x) => x.script === a.script)) if (!motionSet.has(r.motion)) { motionSet.add(r.motion); motions.push(r.motion); }
    for (const mo of motions) { const t = sTL.get(mo); if (t) for (const e of [...t.ranged, ...t.keyed]) if (e.op === 'chain_followup') want(e.action); }
  }
  const hits = new Set(), groups = new Set(), projs = new Set(), fepas = new Set(), sounds = new Set(), pgroups = new Set();
  const wantProj = (id) => { if (id == null || id < 0 || id === 0xFFFF || projs.has(id) || !sMagiById.has(id)) return; projs.add(id); const r = sMagiById.get(id);
    for (const l of CP_LINKS) wantProj(r[l]);
    for (const x of [r.atkp, r.atkp_reflected]) if (sHitById.has(x)) hits.add(sHitById.get(x));
    if (r.effect !== 0xFFFF && r.effect < 64) pgroups.add(r.effect); if (r.origin === 6 && r.origin_arg > 0) fepas.add(r.origin_arg); };   // effect = a FEPA group bit (fx.js projVisuals)
  for (const mo of motions) {
    const t = sTL.get(mo); if (!t) continue;
    for (const e of [...t.ranged, ...t.keyed]) {
      const A = ARGS[e.op];
      if (e.hit !== undefined) { const h = sHitOf(e.hit); if (h) hits.add(h); }
      if (e.group !== undefined && A && [0x0200, 0x027B, 0x0743].includes(A.code)) groups.add(e.group);
      if (e.op === 'weapon_volume' && e.args) groups.add(e.args[0]);
      for (const k of ['projectile', 'alt_projectile']) if (e[k] !== undefined) { wantProj(e[k]); const a2 = e.args && e.args[0]; const pr = sMagiById.get(e[k]); if (pr && pr.origin === 6 && typeof a2 === 'number' && a2 > 0) fepas.add(a2); }
      for (const k of ['effect', 'alt_effect']) if (e[k] !== undefined) fepas.add(e[k]);
      if (e.sound !== undefined && sBank && SndBank.slotOfCue(sBank, e.sound) >= 0) sounds.add(e.sound);
    }
  }

  /* 2. this character's files */
  const pm = msPath('moves.json'), pa = msPath('actions.json'), pmm = msPath('motion_map.json'), ph = msPath('hits.json'), pidx = msPath('timelines/index.json');
  const pp = F(msPath('projectiles.json')) ? msPath('projectiles.json') : null, pv = edVolPath ? edVolPath() : null, pb = mvBratPath();
  const ps = Object.keys(S.files).find((p) => S.files[p].snd) || null, pt = Object.keys(S.files).find((p) => S.files[p].text_) || null;
  if (projs.size && !pp) { toast('This character has no projectiles.json, so a move that fires projectiles can\'t come along.'); return; }
  if ((fepas.size || pgroups.size) && !S.effe) rep.left.push('its effects (this character has no effect package)');
  const rigs = S.rigs.filter((r) => r.ms === S.rig.ms && !r.retail);
  const ch = S.mod.character || {}, like = S.mod.kind === 'retail' ? ch.code : ch.based_on, own = ch.code;
  const lc = like && S.retailChars.find((x) => x.code === like);
  const newRigs = [];
  if (lc && own) for (const [cos] of lc.costumes.filter(([x]) => x.endsWith('_a') === assist)) {   // every costume shares the moveset: each needs the animations
    const stem = `p_${own}_${cos}`, gmo = S.charDir + 'models/' + stem + '.gmo';
    if (rigs.some((r) => r.gmo === gmo) || !isoHas(`obj/p_${like}_${cos}.gmo`)) continue;
    newRigs.push({ gmo, ms: S.rig.ms, label: stem, bytes: await isoGet(`obj/p_${like}_${cos}.gmo`) });
  }
  if (!lc && S.mod.files && S.mod.files._auto) rep.approx.push('set the game ISO and copy again so the other costumes get the animations too: without it only the models in models/ get them');
  for (const r of newRigs) { rigs.push(r); S.rigs.push({ gmo: r.gmo, ms: r.ms, label: r.label }); }
  const rigBytes = [];
  for (const r of rigs) rigBytes.push(r.bytes || (S.files[r.gmo] && S.files[r.gmo].bin ? S.files[r.gmo].bin : new Uint8Array(await FS.read(r.gmo))));
  const rigGmos = rigBytes.map((b) => parseGMO(b));
  const nMot = rigGmos[0].motions.length;
  if (rigGmos.some((g) => g.motions.length !== nMot)) { toast(`The models of this moveset have different motion counts (${rigGmos.map((g) => g.motions.length).join(', ')}): they must match to add animations.`); return; }

  /* 3. effects first (their own undo snapshot, merged with the rest below) */
  const fepaMap = new Map(), niMap = new Map(), pgMap = new Map(); let effeUndo = null;
  if ((fepas.size || pgroups.size) && S.effe && sPkg && fxeFreeGroup(fxuPkg().fep) < 0) rep.left.push(`its effects: this character already has 128 effects, the most a package holds; delete or reuse one, then copy the effect itself in the Effects tab`);
  else if ((fepas.size || pgroups.size) && S.effe && sPkg) {
    const before = S.undo.length;
    fxuEdit((pkg) => {
      // a projectile's look: the records of its group bit, copied under a group bit this character doesn't use yet
      const bitOf = (r, b) => b < 32 ? (fxeU32(r, 0x3C) >>> b) & 1 : (fxeU32(r, 0x40) >>> (b - 32)) & 1;
      const usedBits = new Set(); for (const r of pkg.fepa.records) for (let b = 0; b < 64; b++) if (bitOf(r, b)) usedBits.add(b);
      for (const g of [...pgroups].sort((a, b) => a - b)) {
        const srcRecs = sPkg.fepa.records.filter((r) => !fxeU32(r, 0x34) && bitOf(r, g)); if (!srcRecs.length) continue;
        let nb = 0; while (nb < 64 && usedBits.has(nb)) nb++;
        if (nb >= 64) { (rep.fxErr = rep.fxErr || {})['no free effect group left'] = ['projectile look ' + g]; continue; }
        usedBits.add(nb); pgMap.set(g, nb);
        for (const rec of srcRecs) {
          const sni = fxeU32(rec, 0x28);
          try {
            if (!niMap.has(sni)) { const nm = (sPkg.fep.names()[sni] || {}).name || 'EFFECT'; niMap.set(sni, fxeCopyEffect(pkg, sPkg, fxeEffectOfName(sPkg.fep, sni), nm.replace(/_LEAF_\d+$/, '').slice(0, 40) + '_' + src.code.toUpperCase() + '_LEAF_0')); }
            const ri = fxeAddFepa(pkg, niMap.get(sni), rec), r = pkg.fepa.records[ri];
            fxeSetU32(r, 0x3C, nb < 32 ? (1 << nb) >>> 0 : 0); fxeSetU32(r, 0x40, nb >= 32 ? (1 << (nb - 32)) >>> 0 : 0);
            fxeSetStr(r, 0xC8, '');
          } catch (e) { (rep.fxErr = rep.fxErr || {})[e.message] = [...((rep.fxErr || {})[e.message] || []), 'projectile look ' + g]; }
        }
      }
      for (const id of fepas) {
        const rec = sPkg.fepa.records.find((r) => fxeU32(r, 0x24) === id); if (!rec) continue;
        const sni = fxeU32(rec, 0x28);
        try {
          if (!niMap.has(sni)) {
            const nm = (sPkg.fep.names()[sni] || {}).name || 'EFFECT';
            niMap.set(sni, fxeCopyEffect(pkg, sPkg, fxeEffectOfName(sPkg.fep, sni), nm.replace(/_LEAF_\d+$/, '').slice(0, 40) + '_' + src.code.toUpperCase() + '_LEAF_0'));
          }
          const ri = fxeAddFepa(pkg, niMap.get(sni), rec), r = pkg.fepa.records[ri];
          const bone = new TextDecoder().decode(r.subarray(0xA8, 0xC8)).replace(/\0.*$/, '');
          if (bone && bone !== 'root' && !S.boneNames.includes(bone)) { fxeSetStr(r, 0xA8, 'root'); rep.approx.push(`effect ${id} sits on ${bone}, which this rig lacks: put on the root`); }
          fxeSetStr(r, 0xC8, '');
          if (new TextDecoder().decode(r.subarray(0x88, 0xA8)).replace(/\0.*$/, '')) fxeSetStr(r, 0x88, S.rig.label.replace(/_(2p|3p|zako)/, '_1p'));
          fepaMap.set(id, fxeU32(r, 0x24));
        } catch (e) { (rep.fxErr = rep.fxErr || {})[e.message] = [...((rep.fxErr || {})[e.message] || []), id]; }
      }
      return true;
    });
    if (S.undo.length > before) effeUndo = S.undo.pop();
  }

  for (const [msg, ids] of Object.entries(rep.fxErr || {})) rep.left.push(`effect${ids.length > 1 ? 's' : ''} ${ids.join(', ')}: ${msg}`);
  delete rep.fxErr;

  /* 4. everything else, as one edit */
  const paths = [S.modPath, pm, pa, pmm, ph, pidx, ...[pp, pv, pb, ps, pt].filter(Boolean), ...rigs.map((r) => r.gmo)];
  for (const [i, r] of rigs.entries()) if (!S.files[r.gmo]) S.files[r.gmo] = { bin: rigBytes[i], binOrig: r.bytes ? null : rigBytes[i], dirty: false, gmo: true };
  const newTL = [];
  let cmdNew;
  edit(paths, () => {
    // hits
    const dHits = F(ph).hits, names = new Set(dHits.map((x) => x.name)), hitMap = new Map();
    let hid = Math.max(...dHits.map((x) => parseInt(x.id, 16))) + 1;
    const sameHit = (a, b) => { const x = { ...a }, y = { ...b }; delete x.id; delete y.id; delete x.raw; delete y.raw; return JSON.stringify(x) === JSON.stringify(y) && !a.raw === !b.raw; };
    let reused = 0;
    for (const h of hits) {
      const twin = h.name && !/^0x/i.test(h.name) && dHits.filter((x) => x.name === h.name);
      if (twin && twin.length === 1 && sameHit(twin[0], h)) { hitMap.set(h, { id: parseInt(twin[0].id, 16), ref: h.name }); reused++; continue; }
      const c = JSON.parse(JSON.stringify(h)), id = hid++;
      c.id = cpHex(id, 4);
      let nm = c.name && !/^0x/i.test(c.name) ? c.name : '';
      if (nm) { let k = 2; const base = nm.slice(0, 13); while (names.has(nm)) nm = base + '_' + k++; names.add(nm); c.name = nm; delete c.name_raw; }
      if (c.raw) { const r = cpU8(c.raw); r[0] = id & 255; r[1] = id >> 8; if (nm) { r.fill(0, 52, 68); for (let k = 0; k < nm.length; k++) r[52 + k] = nm.charCodeAt(k); } c.raw = cpHexOf(r); }
      dHits.push(c); hitMap.set(h, { id, ref: nm || cpHex(id, 4) });
    }
    if (hits.size - reused) rep.copied.push(`${hits.size - reused} hit${hits.size - reused > 1 ? 's' : ''}`);
    if (reused) rep.copied.push(`${reused} hit${reused > 1 ? 's' : ''} this character already has (the same record, shared)`);
    // hitbox groups
    const groupMap = new Map(), boneTo = (bi) => {
      if (bi === 0x4000) return bi;
      for (let b = bi; b >= 0 && b < sBones.length; b = sParent[b]) { const d = S.boneIndex.get(sBones[b]); if (d !== undefined) { if (b !== bi) rep.approx.push(`hitbox on ${sBones[bi]} moved to ${sBones[b]}`); return d; } }
      rep.approx.push(`hitbox on ${sBones[bi]} moved to the body centre`); return 0x4000;
    };
    if (groups.size && pv) {
      const v = edVolParse(S.files[pv].bin); let g = Math.max(0, ...v.recs.map((r) => r.group)) + 1;
      const key = (rs) => JSON.stringify(rs.map((r) => [r.kind, r.b5, r.bone, r.off, r.y, r.r, r.r2]));
      let shared = 0;
      for (const sg of groups) {
        const recs = sVols.recs.filter((r) => r.group === sg); if (!recs.length) continue;
        const mine = v.recs.filter((r) => r.group === sg);
        if (mine.length && key(mine) === key(recs.map((r) => ({ ...r, bone: boneTo(r.bone) })))) { groupMap.set(sg, sg); shared++; continue; }
        const ng = g++; groupMap.set(sg, ng);
        for (const r of recs) v.recs.push({ ...r, off: r.off.slice(), group: ng, bone: boneTo(r.bone) });
      }
      S.files[pv].bin = edVolBytes(v);
      if (groupMap.size - shared) rep.copied.push(`${groupMap.size - shared} hitbox group${groupMap.size - shared > 1 ? 's' : ''}`);
      if (shared) rep.copied.push(`${shared} hitbox group${shared > 1 ? 's' : ''} this character already has`);
    } else if (groups.size) rep.left.push('its hitboxes (this character has no hit volume section)');
    // projectiles
    const projMap = new Map();
    if (projs.size) {
      const j = F(pp), have = new Set(j.pool.map((x) => Math.fround(x))), need = new Set();
      for (const id of projs) for (const n of CP_PARAMS) for (const k of sMagiById.get(id).params[n]) { const v = Math.fround(sMagi.pool[k]); if (!have.has(v)) need.add(v); }
      if (j.pool.length + need.size > 128) edCompactPool(j);
      let pid = Math.max(0, ...j.records.map((r) => r.id)) + 1;
      for (const id of [...projs].sort((a, b) => a - b)) projMap.set(id, pid++);
      for (const id of [...projs].sort((a, b) => a - b)) {
        const s = sMagiById.get(id), c = JSON.parse(JSON.stringify(s));
        c.id = projMap.get(id);
        for (const l of CP_LINKS) if (c[l] >= 0 && c[l] !== 0xFFFF) c[l] = projMap.has(c[l]) ? projMap.get(c[l]) : -1;
        for (const k of ['atkp', 'atkp_reflected']) if (sHitById.has(c[k]) && hitMap.has(sHitById.get(c[k]))) c[k] = hitMap.get(sHitById.get(c[k])).id;
        if (c.effect !== 0xFFFF) c.effect = pgMap.has(c.effect) ? pgMap.get(c.effect) : 0xFFFF;
        if (c.origin === 6) c.origin_arg = fepaMap.get(c.origin_arg) ?? c.origin_arg;
        if ((c.origin === 1 || c.origin === 2) && sBones[c.origin_arg] !== undefined) { const d = S.boneIndex.get(sBones[c.origin_arg]); if (d !== undefined) c.origin_arg = d; else { rep.approx.push(`projectile ${id} starts at ${sBones[c.origin_arg]}, which this rig lacks: from the owner instead`); c.origin = 0; c.origin_arg = 0; } }
        c.params = {}; c.values = {};
        for (const n of CP_PARAMS) { const t = s.params[n].map((k) => edPoolIndex(j, sMagi.pool[k])); c.params[n] = t; }
        j.records.push(c);
      }
      for (const r of j.records) r.values = r.values || {};
      for (const r of j.records) for (const n of Object.keys(r.params)) r.values[n] = r.params[n].map((k) => j.pool[k]);
      j.records.sort((a, b) => a.id - b.id);
      rep.copied.push(`${projs.size} projectile${projs.size > 1 ? 's' : ''}`);
    }
    // sounds
    const sndMap = new Map();
    if (sounds.size && ps) {
      const bank = SndBank.parseBank(S.files[ps].bin), slotMap = new Map();
      const copySlot = (si) => {
        if (slotMap.has(si)) return slotMap.get(si);
        let s = sBank.slots[si]; if (s.kind === 'link') return copySlot(s.owner);
        let di;
        if (s.kind === 'alias') { const o = copySlot(s.owner); bank.slots.push({ raw: s.raw.slice(), kind: 'alias', owner: o, blob: null }); di = bank.slots.length - 1; }
        else if (s.kind === 'data') { bank.slots.push({ raw: s.raw.slice(), kind: 'data', owner: null, blob: s.blob.slice() }); di = bank.slots.length - 1; }
        else return -1;
        SndBank.refresh(bank, di); slotMap.set(si, di); return di;
      };
      for (const cue of sounds) { const di = copySlot(SndBank.slotOfCue(sBank, cue)); if (di >= 0) sndMap.set(cue, SndBank.cueOf(bank, di)); }
      S.files[ps].bin = SndBank.bankToBytes(bank);
      if (S.mod) { S.mod.sound_names = S.mod.sound_names || {}; for (const [a, b] of sndMap) S.mod.sound_names[String(b)] = `${moveName} (${src.label} ${a})`; }
      rep.copied.push(`${sndMap.size} sound${sndMap.size > 1 ? 's' : ''}`);
    } else if (sounds.size) rep.left.push('its sounds (this character has no sound bank)');
    // animations into every model of this moveset
    const motMap = new Map(), stat = { noBone: new Set(), keptLength: new Set(), same: true };
    const trees = rigBytes.map((b) => cpTree(b));
    motions.forEach((mo, k) => {
      const nm = ((srcGmo.motions[mo] || {}).name || '_' + mo) + '_' + src.code.slice(0, 3);
      trees.forEach((t, ri) => { const am = cpAnimModel(t), last = gcFind(am, 0x0B).pop(); am.children.splice(am.children.indexOf(last) + 1, 0, cpMotionChunk(srcGmo, srcTree, mo, rigGmos[ri], nm, stat)); });
      motMap.set(mo, nMot + k);
    });
    rigs.forEach((r, ri) => { S.files[r.gmo].bin = cpTreeBytes(trees[ri]); });
    if (S.mod.files) for (const r of rigs) { const stem = r.gmo.split('/').pop(), gp = 'obj/' + stem; if (!(gp in S.mod.files) || typeof S.mod.files[gp] !== 'string') S.mod.files[gp] = 'models/' + stem; }   // the build installs them
    if (newRigs.length) rep.copied.push(`the ${newRigs.map((r) => r.label.split('_').pop()).join(', ')} costume model${newRigs.length > 1 ? 's' : ''} into models/ (they share the moveset)`);
    rep.copied.push(`${motions.length} animation${motions.length > 1 ? 's' : ''} (into ${rigs.length} model${rigs.length > 1 ? 's' : ''}${stat.same ? ', same skeleton' : ', bones matched by name'})`);
    if (!stat.same && (stat.noBone.size || stat.keptLength.size)) rep.approx.push(`another skeleton: bones matched by name${stat.noBone.size ? `; ${stat.noBone.size} bones it doesn't have (${[...stat.noBone].slice(0, 6).join(', ')}${stat.noBone.size > 6 ? '…' : ''}) don't move` : ''}${stat.keptLength.size ? `; ${stat.keptLength.size} bones keep this rig's length` : ''}. Refine it in Blender (Retarget from character)`);
    // actions, scripts, motion_map
    const acts = F(pa).actions, rows = F(pmm).rows, actMap = new Map();
    let act = Math.max(...acts.map((a) => a.action)) + 1, scr = Math.max(...acts.map((a) => a.script), ...rows.map((r) => r.script)) + 1;
    for (const id of actList) actMap.set(id, act++);
    const dText = pt && sText ? OBJTEXT.parseText(S.files[pt].bin) : null; let textDirty = false;
    for (const id of actList) {
      const a = sActs.get(id), raw = cpU8(a.raw), na = { action: actMap.get(id), script: scr };
      new DataView(raw.buffer).setUint16(0, na.action, true); new DataView(raw.buffer).setUint16(0x1C, scr, true);
      if (raw[0x21] !== 255) {                    // the HP banner: a record of its own here
        const r = sText && sText.langs[0].find((x) => x.index === raw[0x21]);
        if (r && dText) { const idx = Math.max(-1, ...dText.langs[0].map((x) => x.index)) + 1; if (idx <= 254) { OBJTEXT.setRecord(dText, 0, idx, r.text); raw[0x21] = idx; textDirty = true; } else raw[0x21] = 255; }
        else raw[0x21] = 255;
      }
      na.raw = cpHexOf(raw);
      acts.push(na);
      for (const r of sRows.filter((x) => x.script === a.script)) rows.push({ script: scr, motion: motMap.get(r.motion), phase: r.phase });
      scr++;
    }
    if (textDirty) S.files[pt].bin = OBJTEXT.textToBytes(dText);
    // timelines
    const hidden = cpHiddenAtStart();
    const dropped = {};
    for (const mo of motions) {
      const t = sTL.get(mo); if (!t) continue;
      const nt = JSON.parse(JSON.stringify(t)); nt.motion = motMap.get(mo);
      const fix = (e) => {
        const A = ARGS[e.op];
        if (e.hit !== undefined) { const h = sHitOf(e.hit); if (h && hitMap.has(h)) e.hit = hitMap.get(h).ref; }
        if (e.group !== undefined && A && [0x0200, 0x027B, 0x0743].includes(A.code) && groupMap.has(e.group)) e.group = groupMap.get(e.group);
        if (e.op === 'weapon_volume' && e.args && groupMap.has(e.args[0])) e.args[0] = groupMap.get(e.args[0]);
        for (const k of ['projectile', 'alt_projectile']) if (e[k] !== undefined && projMap.has(e[k])) e[k] = projMap.get(e[k]);
        for (const k of ['effect', 'alt_effect']) if (e[k] !== undefined) { if (fepaMap.has(e[k])) e[k] = fepaMap.get(e[k]); else if (fepas.has(e[k])) return 'effects it could not bring'; }
        if (e.op === 'chain_followup' && actMap.has(e.action)) e.action = actMap.get(e.action);
        if (e.sound !== undefined && sndMap.has(e.sound)) e.sound = sndMap.get(e.sound);
        for (const k of ['part', 'draw_part', 'camera_bone']) if (e[k] !== undefined) {
          const nm = typeof e[k] === 'number' ? sBones[e[k]] : e[k];
          if (nm === undefined || !S.boneIndex.has(nm)) return `parts this rig lacks (${nm})`;
          e[k] = typeof e[k] === 'number' ? S.boneIndex.get(nm) : nm;
        }
        if (['voice', 'face_texture'].includes(e.op)) return e.op === 'voice' ? 'voices' : 'face textures';
        return null;
      };
      for (const kind of ['ranged', 'keyed']) nt[kind] = nt[kind].filter((e) => { const why = fix(e); if (why) dropped[why] = (dropped[why] || 0) + 1; return !why; });
      for (const ev of hidden) if (!nt.keyed.some((e) => e.op === ev.op && e.part === ev.part && e.frame === 0)) nt.keyed.unshift(JSON.parse(JSON.stringify(ev)));
      let fn = 'timelines/' + String(nt.motion).padStart(3, '0') + '.json';
      while (S.files[msPath(fn)]) fn = fn.replace('.json', '_n.json');
      S.files[msPath(fn)] = { obj: nt, text: '', dirty: true, created: true };
      F(pidx).files.push(fn); newTL.push(msPath(fn));
    }
    for (const [why, n] of Object.entries(dropped)) rep.left.push(`${n} event${n > 1 ? 's' : ''} for ${why}`);
    if (hidden.length) rep.copied.push(`this character's ${hidden.length} parts hidden at the start of each animation`);
    // the command, the ability, the CPU row
    const key = mvFreeKey(); cmdNew = mvFreeCmd(); const cmdHex = '0x' + cmdNew.toString(16).toUpperCase();
    const steps = srcCmd.steps.map((st) => ({ action: actMap.get(st.action), b: st.b, c: st.c, flags: st.flags }));
    F(pm).commands.push({ id: cmdHex, prereq: [65535, 65535], steps, _info: `${moveName} (ability ${key})` });
    const info = mvInfo.info || {}, type = info.type === 'hp' ? 'hp' : 'brave', phase = ['ground', 'midair', 'follow-up'].includes(info.phase) ? info.phase : 'ground';
    const tmpl = mvTemplate(type, phase), te = tmpl && mvEntry(tmpl);
    const ent = { copy: te ? (te.copy !== undefined ? te.copy : tmpl.info.key) : undefined, cmd: cmdHex, type, phase, learn: 1, status: 'new', name: moveName };
    if (ent.copy === undefined) delete ent.copy;
    if (info.help) ent.help = info.help;
    ent._note = `Made in the Move Studio: copied from ${src.label}'s ${moveName} (command ${srcCmd.id})`;
    S.mod.move_names[key] = ent;
    if (pb) {
      if (sBratSec && sBratSec.data instanceof Uint8Array) S.files[pb].bin = cpBratAdd(S.files[pb].bin, cmdNew, sBratSec.data, cmdId);
      else S.files[pb].bin = mvBratAdd(S.files[pb].bin, cmdNew, tmpl ? tmpl.id : -1);
    }
    rep.copied.unshift(`${srcCmd.steps.length} press${srcCmd.steps.length > 1 ? 'es' : ''}${actList.length > srcCmd.steps.length ? ` (+${actList.length - srcCmd.steps.length} follow-up action${actList.length - srcCmd.steps.length > 1 ? 's' : ''})` : ''}`);
  });
  if (effeUndo) S.undo[S.undo.length - 1] = [...effeUndo, ...S.undo[S.undo.length - 1]];
  if (fepaMap.size) rep.copied.push(`${niMap.size} effect${niMap.size > 1 ? 's' : ''}`);
  S.tlPaths.push(...newTL);
  markDirty(newTL);
  cpRigChanged();
  const mi = S.moves.findIndex((m) => m.id === cmdNew);
  rep.approx = [...new Set(rep.approx)];
  CP.notes[cmdNew] = { from: `${src.label}'s ${moveName}`, ...rep };
  S.newMove = null; S.step = { move: mi, i: 0 }; S.moveOnly = true; setMotion(S.moves[mi].steps[0].motion);
  runChecks(); renderAll();
  toast(`${moveName} copied from ${src.label}: ${rep.copied.join(', ')}.${rep.left.length ? ' Left out: ' + rep.left.join('; ') + '.' : ''}`);
}

/* the frame-0 part hides most of this character's timelines carry (its weapons, ghosts …), for the new ones */
function cpHiddenAtStart() {
  const count = new Map(); let n = 0;
  for (const p of S.tlPaths) {
    const t = F(p); if (!t) continue; n++;
    for (const e of t.keyed) if (e.op === 'hide_part_once' && e.frame === 0) { const k = String(e.part); if (!count.has(k)) count.set(k, { e, n: 0 }); count.get(k).n++; }
  }
  return [...count.values()].filter((x) => x.n > n / 2).map((x) => x.e);
}
function cpBratAdd(bin, cmd, srcBin, srcCmd) {   // a CPU row with the source character's weights
  const b = mvBratRows(bin), s = mvBratRows(srcBin), row = s.rows.find((r) => r.cmd === srcCmd);
  if (!row || b.rows.some((r) => r.cmd === cmd)) return mvBratAdd(bin, cmd, -1);
  const sit = row.sit.map(([off, n]) => { const o = b.w.length; b.w.push(...s.w.slice(off, off + n)); return [o, n]; });
  b.rows.push({ cmd, sit });
  const size = 20 + 28 * b.rows.length + 4 + b.w.length, out = new Uint8Array((size + 3) & ~3), dv = new DataView(out.buffer);
  out.set(b.head, 0); dv.setUint32(16, b.rows.length, true);
  b.rows.forEach((r, i) => { const o = 20 + 28 * i; dv.setUint32(o, r.cmd, true); r.sit.forEach(([off, n], k) => { dv.setUint16(o + 4 + 4 * k, off, true); dv.setUint16(o + 6 + 4 * k, n, true); }); });
  const mo = 20 + 28 * b.rows.length; dv.setUint32(mo, b.w.length, true); out.set(b.w, mo + 4);
  return out;
}

/* the model in the viewport after its file changed (a copy, or undo) */
function cpRigChanged() {
  const f = S.rig && S.files[S.rig.gmo];
  if (f && f.bin) {
    if (S.rigBytes) { S.rigBytes = f.bin; if (S.skinShown) { skinShow(S.skinShown, true).then(() => { derive(); renderAll(); }); } else { S.gmo = parseGMO(f.bin); buildCharacter(S.gmo, view); } }
    else { S.gmo = parseGMO(f.bin); buildCharacter(S.gmo, view); }
    view.bonesHelper.visible = $('showBones').checked;
    if (typeof FXP !== 'undefined') { FXP.gmo = null; FXP.map.clear(); }
    if (typeof DM_BODY !== 'undefined') DM_BODY.gmo = null;
  }
  S.tlPaths = F(msPath('timelines/index.json')).files.filter((fn) => fn.endsWith('.json') && S.files[msPath(fn)]).map(msPath);
  derive();
}
function cpAfterUndo(u) {
  const idx = F(msPath('timelines/index.json')), listed = new Set(idx ? idx.files.map(msPath) : []);
  for (const [p, f] of Object.entries(S.files)) if (f.created && p.startsWith(msPath('timelines/')) && !listed.has(p)) delete S.files[p];   // (only timelines: effects/ and shader files have their own life)
  if (u.some((x) => x.p && S.files[x.p] && S.files[x.p].gmo) || u.some((x) => x.p === msPath('timelines/index.json'))) cpRigChanged();
}

/* ---------------------------------------------------------------- duplicate an animation of this character
   A copy of motion `mi` is appended to every model of this moveset (same skeleton: the chunk is cloned as it is), with a
   copy of its event timeline (or an empty one), and optionally the selected press is pointed at the copy. Undo-able as
   one edit. -> the new motion index, or -1 */
async function dupMotion(mi, newName, withEvents, pointStep) {
  if (S.readOnly) { readOnlyNote(); return -1; }
  if (!S.rig || S.rig.retail) { toast('Pick the mod character (not the retail original) to add animations to it.'); return -1; }
  const rigs = S.rigs.filter((r) => r.ms === S.rig.ms && !r.retail);
  const rigBytes = [];
  for (const r of rigs) rigBytes.push(S.files[r.gmo] && S.files[r.gmo].bin ? S.files[r.gmo].bin : new Uint8Array(await FS.read(r.gmo)));
  const rigGmos = rigBytes.map((b) => parseGMO(b));
  const nMot = rigGmos[0].motions.length;
  if (rigGmos.some((g) => g.motions.length !== nMot)) { toast(`The models of this moveset have different motion counts (${rigGmos.map((g) => g.motions.length).join(', ')}): they must match to add animations.`); return -1; }
  if (mi < 0 || mi >= nMot) return -1;
  const name = (newName || (motionName(mi) + '_2')).replace(/[^\x20-\x7E]/g, '').slice(0, 31) || ('_' + nMot);
  const pidx = msPath('timelines/index.json'), pmm = msPath('motion_map.json');
  const srcPath = withEvents ? (S.tlOfMotion.get(mi) || null) : null;
  for (const [i, r] of rigs.entries()) if (!S.files[r.gmo]) S.files[r.gmo] = { bin: rigBytes[i], binOrig: rigBytes[i], dirty: false, gmo: true };
  const paths = [pidx, ...(pointStep ? [pmm] : []), ...rigs.map((r) => r.gmo)];
  let fn = 'timelines/' + String(nMot).padStart(3, '0') + '.json';
  while (S.files[msPath(fn)]) fn = fn.replace('.json', '_n.json');
  const newIdx = nMot;
  edit(paths, () => {
    const trees = rigBytes.map((b) => cpTree(b)), stat = { noBone: new Set(), keptLength: new Set(), same: true };
    trees.forEach((t, ri) => {
      const am = cpAnimModel(t), last = gcFind(am, 0x0B).pop();
      const m = cpMotionChunk(rigGmos[ri], t, mi, rigGmos[ri], name, stat);
      cpSetChunkName(m, name);                   // a header sized for the name (as the Blender exporter writes new motions)
      am.children.splice(am.children.indexOf(last) + 1, 0, m);
    });
    rigs.forEach((r, ri) => { S.files[r.gmo].bin = cpTreeBytes(trees[ri]); });
    if (S.mod.files) for (const r of rigs) { const stem = r.gmo.split('/').pop(), gp = 'obj/' + stem; if (!(gp in S.mod.files) || typeof S.mod.files[gp] !== 'string') S.mod.files[gp] = 'models/' + stem; }
    const nt = srcPath ? JSON.parse(JSON.stringify(F(srcPath))) : { motion: newIdx, ranged: [], keyed: [] };
    nt.motion = newIdx;
    S.files[msPath(fn)] = { obj: nt, text: '', dirty: true, created: true };
    F(pidx).files.push(fn);
    if (pointStep) { const s = S.moves[pointStep.move].steps[pointStep.i]; for (const r of F(pmm).rows) if (r.script === s.script && r.motion === mi) r.motion = newIdx; }
  });
  S.tlPaths.push(msPath(fn)); markDirty([msPath(fn)]);
  cpRigChanged();
  setMotion(newIdx); runChecks(); renderAll();
  toast(`${name} added as a copy of ${motionName(mi)}${srcPath ? ' with its events' : ' (empty timeline)'}${pointStep ? '; this press now plays it' : ''}. Edit the poses in Blender (Open in Blender), events here.`);
  return newIdx;
}
/* ---------------------------------------------------------------- UI: the New move panel's "Copy from…" */
function cpNotesHTML(m) {
  const n = CP.notes[m.id]; if (!n) return '';
  return `<details class="edp" open><summary class="lbl">Copied from ${esc(n.from)}</summary>
    <p class="note">Came along: ${esc(n.copied.join(', '))}.</p>
    ${n.approx.length ? `<p class="note">Approximate: ${esc(n.approx.join('; '))}.</p>` : ''}
    ${n.left.length ? `<p class="warnline">Left out: ${esc(n.left.join('; '))}.</p>` : ''}
    <p class="note">New animations are added to every model of this moveset; the game's handling of extra motions is not tested in a fight yet.</p></details>`;
}
function cpMenu(anchor) {
  const items = fxuCharItems().filter((it) => it.src !== 'cur' && !(it.src.mod && it.src.mod === S.char));
  if (!items.length) { toast('Set the game ISO (or open a mods folder with other characters) to copy their moves.'); return; }
  fxuMenu(anchor, 'Copy a move from…', items, async (ch) => {
    let src; try { src = await cpSource(ch.src, /_a$/.test(S.msDir)); } catch (e) { toast('Could not read that character: ' + e.message, 'bad'); return; }
    const mv = cpMoves(src).sort((a, b) => b.named - a.named).map((x) => ({ ...x, cat: x.named ? `${src.label}: abilities` : `${src.label}: other commands` }));
    fxuMenu(anchor, 'Move to copy', mv, (it) => cpCopyMove(ch.src, it.cmd));
  });
}
