/* Move Studio: objects a move spawns — Gilgamesh's swords, Yuna's aeons, Bartz's mimicked weapons — drawn in the 3D view
   with their own model, motion, fades and effects. Loaded after app.js, fx.js and dummy.js; needs the game ISO (the
   table and the object files are only there). What is read from the game and what is a guess:
   psprecomp/games/dissidia/docs/spawned-objects.md.

   The events (ranged; argsOf order):
     hold_prop      0x075A {id, row, pick, release, fade_in, fade_out, start}
     summon_object  0x0C65 {id, row, pick, release, fade_in, fade_out, x, y, z, on_root, follow, follow_id}
   row = a global row of battle.bin 'wpnp' (main.bin): +8 the objentry id of the object (objentry.bin record, file stem at
   +0x34 -> obj/<stem>.objx + .gmo), +0x1C the script the object plays (its own motion_map), +0x22 the effect record
   it starts at once (its own effect bank). id = the owner's bone it hangs on (0x4000 = the owner itself). release = 1:
   placed at that bone when it appears, then left in the world; 0: follows the bone. x, y, z: an offset in that frame.
   on_root: on the owner instead of the bone; follow + follow_id: on the other object with that id. pick = 1: the game
   swaps the row for one it picks (Gilgamesh's weapon roll) — shown as the row the event names. */
'use strict';
const SP = { tables: null, tablesFor: null, objs: new Map(), bodies: new Map(), loading: new Set(), live: [], fxInsts: [], poseKey: '', poseCache: new Map(), gmo: null };
const SP_HOLD = 0x075A, SP_SUMMON = 0x0C65, SP_PROP_FX = 0x055F, SP_PROP_FADE = 0x0360, SP_SUMMON_FX = 0x0335, SP_FADE_IN = 0x0109;

/* ---------------------------------------------------------------- the game's tables (main.bin) */
function spParseTables(mainBin) {
  const main = mpkNamed(mainBin), battle = arcRead(main['battle.bin']);
  const wp = battle.find((e) => e.tag === 'wpnp').data, wdv = DV(wp), rows = [];
  for (let i = 0, n = wdv.getUint32(4, true); i < n; i++) {
    const o = 8 + 0x3C * i, s16 = (k) => wdv.getInt16(o + k, true), u16 = (k) => wdv.getUint16(o + k, true);
    rows.push({ index: wdv.getUint32(o, true), char: u16(4), flags: u16(6), obj: u16(8), objZako: u16(0x2A), objA: u16(0x2C), objZakoA: u16(0x2E),
      script: s16(0x1C), effect: s16(0x22) });
  }
  const oe = main['objentry.bin'], odv = DV(oe), stems = new Map();
  for (let i = 0, n = odv.getUint32(4, true); i < n; i++) {
    const o = 8 + 0x44 * i; let s = '';
    for (let k = o + 0x34; k < o + 0x44 && oe[k]; k++) s += String.fromCharCode(oe[k]);
    stems.set(odv.getUint16(o, true), s.toLowerCase());
  }
  return { rows, stems };
}
async function spTables() {
  if (!(typeof ISO !== 'undefined' && ISO.index && ISO.index.size)) return null;
  if (SP.tablesFor === ISO.index) return SP.tables;
  SP.tablesFor = ISO.index; SP.tables = null;
  try { SP.tables = spParseTables(await isoGet('general_archive/main/main.bin')); } catch (e) { console.warn('studio: no wpnp table', e); }
  return SP.tables;
}
function spRowInfo(row) {                        // {row, stem} of a wpnp row, or null (no ISO, no such row)
  const T = SP.tables, r = T && T.rows[row]; if (!r || r.obj === 0xFFFF) return null;
  const stem = T.stems.get(r.obj); return stem ? { row: r, stem } : null;
}

/* ---------------------------------------------------------------- an object's files (cached per file stem) */
function spObj(stem) {                          // the loaded object, or null while it loads (pose() runs again when it is in)
  const o = SP.objs.get(stem); if (o) return o.failed ? null : o;
  if (!SP.loading.has(stem)) {
    SP.loading.add(stem);
    (async () => {
      try {
        const gmo = parseGMO(await isoGet(`obj/${stem}.gmo`));
        let dec = null, fx = null;
        try { dec = decodeMoveset(await isoGet(`obj/${stem}.objx`), []); } catch (e) { console.warn('studio: object moves', stem, e); }
        if (dec) {
          const ent = dec['arc.json'].entries.find((x) => x.tag === '65666665');
          try { if (ent) fx = loadEffects(dec[ent.file]); } catch (e) { console.warn('studio: object effects', stem, e); }
        }
        const rows = dec && dec['motion_map.json'] ? dec['motion_map.json'].rows : [], tls = new Map();
        for (const [p, v] of Object.entries(dec || {})) if (p.startsWith('timelines/') && v && v.motion != null && !tls.has(v.motion)) tls.set(v.motion, v);
        SP.objs.set(stem, { stem, gmo, fx, rows, tls, names: gmo.bones.map((b) => b.name) });
      } catch (e) { console.warn('studio: no object', stem, e); SP.objs.set(stem, { failed: true }); }
      SP.loading.delete(stem); SP.poseKey = '';
      if (S.gmo) pose();
    })();
  }
  return null;
}
const spMotionOf = (o, script) => { const r = script > 0 ? o.rows.find((x) => x.script === script) : null; return r && o.gmo.motions[r.motion] ? r.motion : -1; };

/* ---------------------------------------------------------------- which objects are out at a tick of the owner */
function spPass(e) {                            // the phase / mode filter fx.js applies to effects (pad low byte, next byte)
  const f = e.pad || 0, ph = f & 0xFF, md = (f >> 8) & 0xFF, cur = stepPhase();
  return (!ph || cur == null || ph === cur) && (!md || md === (S.exMode ? 2 : 1));
}
function spEventsOf(tl) {
  if (!tl) return [];
  return tl.ranged.map((e, idx) => ({ e, idx, c: opCode(e.op) })).filter((x) => (x.c === SP_HOLD || x.c === SP_SUMMON) && spPass(x.e)).map((x) => {
    const a = argsOf(x.e), n = (i) => (typeof a[i] === 'number' ? a[i] : 0);
    return { idx: x.idx, summon: x.c === SP_SUMMON, start: x.e.start, end: x.e.end === FOREVER ? Infinity : x.e.end,
      id: n(0), row: n(1), pick: n(2), release: n(3) > 0, fadeIn: n(4), fadeOut: n(5),
      offset: x.c === SP_HOLD ? n(6) : 0, local: x.c === SP_SUMMON ? [n(6), n(7), n(8)] : [0, 0, 0],
      onRoot: x.c === SP_SUMMON && n(9) !== 0, follow: x.c === SP_SUMMON && n(10) !== 0 ? n(11) : null };
  });
}
/* An object the previous press left out (same id and object, its range running to that press's end) is the same object:
   it stays where it is, and with the same row its motion carries on (the game keeps a prop alive while an event with
   its id refreshes it, FUN_0884F1C8; a new row of the same object is taken as a new script for it — a guess). */
function spCarried(ev) {                        // -> ticks its motion has run already; ev.from = where it appeared
  ev.from = null;
  if (ev.start !== 0 || !S.step || !S.moves) return 0;
  const m = S.moves[S.step.move]; if (!m || typeof mvSegments !== 'function') return 0;
  const segs = mvSegments(m), stem = (r) => { const x = spRowInfo(r); return x && x.stem; };
  let age = 0, counting = true, cur = ev;
  for (let i = Math.min(S.step.i, segs.length) - 1; i >= 0; i--) {
    const p = S.tlOfMotion.get(segs[i].motion), len = ticks(segs[i].len);
    const prev = p ? spEventsOf(F(p)).find((x) => x.id === cur.id && x.start < len && x.end >= len && stem(x.row) && stem(x.row) === stem(cur.row)) : null;
    if (!prev) break;
    ev.from = { motion: segs[i].motion, ev: prev };
    if (prev.row !== cur.row) counting = false;     // another row of the same object: it stays, its new script starts over
    if (counting) age += len - prev.start;
    if (prev.start > 0) break;
    cur = prev;
  }
  return age;
}
function spFadeOf(ev, T, tl) {                  // 0..1: the event's fade in / out and prop_fade (0x0360) on its id
  let a = 1;
  const age = T - ev.start + ev.carried;
  if (ev.fadeIn > 0 && !ev.from) a = Math.min(a, age / ev.fadeIn);
  if (T >= ev.end) a = Math.min(a, ev.fadeOut > 0 ? 1 - (T - ev.end) / ev.fadeOut : 0);
  for (const e of tl ? tl.ranged : []) {
    if (opCode(e.op) !== SP_PROP_FADE || !spPass(e)) continue;
    const x = argsOf(e); if (x[0] !== ev.id) continue;
    const end = e.end === FOREVER ? Infinity : e.end, fin = x[1] || 0, fout = x[2] || 0;
    if (T >= e.start && T < end) a = Math.min(a, fout > 0 ? Math.max(0, 1 - (T - e.start) / fout) : 0);
    else if (T >= end) a = Math.min(a, fin > 0 ? Math.min(1, (T - end) / fin) : 1);
  }
  return Math.max(0, Math.min(1, a));
}

/* ---------------------------------------------------------------- posing */
function spBody(key, o) {                        // a posed copy of the object's model (like the dummy's DM_BODY)
  let B = SP.bodies.get(key);
  if (B && B.gmo === o.gmo) return B;
  if (B) spDrop(key);
  B = { scene: view.scene, root: null, bones: [], partMeshes: [], gmo: o.gmo };
  buildCharacter(o.gmo, B);
  B.root.matrixAutoUpdate = false;
  B.mats = []; B.partMeshes.forEach((pm) => pm && [].concat(pm.mesh.material).forEach((m) => B.mats.push(m)));
  SP.bodies.set(key, B);
  return B;
}
function spDrop(key) {
  const B = SP.bodies.get(key); if (!B) return;
  view.scene.remove(B.root); if (B.bonesHelper) view.scene.remove(B.bonesHelper);
  SP.bodies.delete(key);
}
function spOwnerPose(T, tNow) {                  // the owner's root and bone matrices at owner tick T
  if (Math.abs(T - tNow) < 1e-6 || typeof fxPoseAt !== 'function') return { root: view.root.matrixWorld, bones: view.bones.map((b) => b.matrixWorld) };
  return fxPoseAt(T);
}
/* where an object stands at owner tick T: its attach frame x its local offset (FUN_08851480 / FUN_08851078) */
function spPlace(ev, T, tNow, placed) {
  const at = ev.release ? Math.min(T, ev.start + 1) : T;          // released: where it was as it appeared, then left there
  let M;
  if (ev.follow != null && placed.has(ev.follow)) M = placed.get(ev.follow).clone();
  else if (ev.from && ev.from.ev.release) {       // let go in an earlier press: it stays where it was placed then
    const f = ev.from.ev;
    poseMotion(S.gmo, view, ev.from.motion, frames(f.start + 1)); view.root.updateMatrixWorld(true);
    const bones = view.bones.map((b) => b.matrixWorld);
    M = normalised(f.onRoot || (f.id & 0x4000) || !bones[f.id] ? view.root.matrixWorld : bones[f.id]).multiply(new THREE.Matrix4().makeTranslation(...f.local));
    poseMotion(S.gmo, view, S.motion, S.frame); view.root.updateMatrixWorld(true);
    return M;
  } else {
    const P = spOwnerPose(at, tNow);
    M = normalised(ev.onRoot || (ev.id & 0x4000) || !P.bones[ev.id] ? P.root : P.bones[ev.id]);
  }
  return M.multiply(new THREE.Matrix4().makeTranslation(...ev.local));
}
function spPoseBody(B, o, motion, mt) {
  const m = motion >= 0 && o.gmo.motions[motion];
  poseMotion(o.gmo, B, m ? motion : -1, m ? Math.min(mt / 2, Math.max(0, m.end || 0)) : 0);
}
function spApply(B, o, ev, M, mt, alpha) {
  B.root.matrix.copy(M); B.root.matrixWorldNeedsUpdate = true;
  spPoseBody(B, o, ev.motion, mt);
  B.root.updateMatrixWorld(true);
  const tl = o.tls.get(ev.motion), st = typeof partStates === 'function' ? partStates(tl, mt) : new Map();
  let a = alpha;
  for (const e of tl ? tl.keyed : []) if (opCode(e.op) === SP_FADE_IN && e.frame <= mt) { const n = argsOf(e)[0] || 0; a = Math.min(a, n > 0 ? (mt - e.frame) / n : 1); }
  a = Math.max(0, Math.min(1, a));
  B.root.visible = a > 0.01;
  B.partMeshes.forEach((pm) => { if (pm) pm.mesh.visible = st.has(pm.draw) ? st.get(pm.draw) : true; });
  for (const m of B.mats) {
    const want = a < 0.999;
    if (m.transparent !== want) { m.transparent = want; m.needsUpdate = true; }
    m.opacity = a; m.depthWrite = !want; m.alphaTest = 0.35 * a;
  }
  B.alpha = a;
}

/* ---------------------------------------------------------------- per frame (app.js pose() calls it before fxRefresh) */
function spOn() { const cb = $('showSpawn'); return !cb || cb.checked; }
function spOwnsProp(id) { return SP.live.some((x) => x.ev.id === id || id < 0); }   // fx.js: a prop_effect the objects play
function spRefresh() {
  SP.live = []; SP.fxInsts = [];
  if (SP.gmo !== S.gmo) { for (const k of [...SP.bodies.keys()]) spDrop(k); SP.gmo = S.gmo; }
  const tl = !S.fxEdit && S.gmo && view.root && spOn() ? curTL() : null;
  const evs = spEventsOf(tl);
  if (!evs.length || !SP.tables) { if (evs.length) spTables().then((t) => t && pose()); for (const B of SP.bodies.values()) B.root.visible = false; return; }
  view.root.updateMatrixWorld(true);
  const tNow = ticks(S.frame), used = new Set(), placed = new Map(), nth = new Map();
  const key = [S.gmo && S.rig && S.rig.gmo, S.motion, S.step ? S.step.move + ':' + S.step.i : '', !!S.exMode, typeof DM !== 'undefined' && DM.on ? DM.key : ''].join('|');
  if (key !== SP.poseKey) { SP.poseKey = key; SP.poseCache.clear(); }
  // summons that follow another object come after it
  evs.sort((p, q) => (p.follow != null) - (q.follow != null) || p.start - q.start);
  for (const ev of evs) {
    ev.carried = spCarried(ev);
    if (tNow < ev.start || tNow >= ev.end + Math.max(0, ev.fadeOut)) continue;
    if (tNow >= ev.end && ev.fadeOut <= 0) continue;
    const info = spRowInfo(ev.row); if (!info) continue;
    const o = spObj(info.stem); if (!o) continue;
    ev.info = info; ev.motion = spMotionOf(o, info.row.script);
    const k = nth.get(info.stem) || 0; nth.set(info.stem, k + 1);
    const bkey = `${info.stem}#${k}`, B = spBody(bkey, o); used.add(bkey);             // models are reused from motion to motion
    const mt = tNow - ev.start + ev.carried + ev.offset;                 // the object's own motion tick
    const M = spPlace(ev, tNow, tNow, placed); placed.set(ev.id, M);
    spApply(B, o, ev, M, mt, spFadeOf(ev, tNow, tl));
    const live = { ev, o, B, key: `${ev.idx}:${bkey}`, mt };
    SP.live.push(live);
    spCollectFx(live, tl, tNow);
  }
  for (const k of [...SP.bodies.keys()]) if (!used.has(k)) SP.bodies.get(k).root.visible = false;
  if (typeof FXP !== 'undefined' && FXP.dirty) { FXP.dirty = false; poseMotion(S.gmo, view, S.motion, S.frame); view.root.updateMatrixWorld(true); }
}

/* ---------------------------------------------------------------- the objects' effects (fx.js opts.extra) */
/* An object's pose at its motion tick t: the owner posed at the matching tick for the attach frame, the object's own
   motion for its bones (cached; the live pose is put back after). */
function spScratch(o) {                         // an unseen copy of the object's model for its pose at other ticks
  if (!o.scratch) { o.scratch = { scene: view.scene, root: null, bones: [], partMeshes: [] }; buildCharacter(o.gmo, o.scratch); view.scene.remove(o.scratch.root); view.scene.remove(o.scratch.bonesHelper); o.scratch.root.matrixAutoUpdate = false; }
  return o.scratch;
}
function spPoseAt(live, t) {
  const { ev, o, B } = live;
  if (Math.abs(t - live.mt) < 1e-6) return live.now;
  const T = Math.round(t), k = live.key + '@' + T;
  if (!SP.poseCache.has(k)) {
    const X = spScratch(o), Towner = Math.max(ev.start, ev.start + T - ev.offset - ev.carried);
    X.root.matrix.copy(spPlace(ev, Towner, ticks(S.frame), new Map()));
    spPoseBody(X, o, ev.motion, Math.max(0, T)); X.root.updateMatrixWorld(true);
    SP.poseCache.set(k, { root: X.root.matrixWorld.clone(), bones: X.bones.map((b) => b.matrixWorld.clone()) });
  }
  return SP.poseCache.get(k);
}
function spCtx(live, fx) {
  const names = live.o.names;
  return { fx, tick: live.mt, poseAt: (T) => spPoseAt(live, T), boneIndex: (n) => (n === 'root' ? FX_ROOT_BONE : names.indexOf(n)),
    boneOk: (i) => i >= 0 && i < names.length };
}
function spCollectFx(live, tl, tNow) {
  const { ev, o, B } = live;
  live.now = { root: B.root.matrixWorld.clone(), bones: B.bones.map((b) => b.matrixWorld.clone()) };
  if (!$('showFx') || !$('showFx').checked) return;
  const own = o.fx ? spCtx(live, o.fx) : null, out = SP.fxInsts;
  // 1. the row's effect, started with the object (FUN_0884FA84: row +0x22 from the object's own bank)
  if (own && ev.info.row.effect >= 0 && o.fx.byId.has(ev.info.row.effect))
    out.push({ type: 'fx', tick: ev.offset, recs: o.fx.byId.get(ev.info.row.effect), kind: 'spawn', idx: 0, ctx: own });
  // 2. the object's own timeline: its effect events (its bank), summon_effect (the owner's bank, on its bone)
  const otl = o.tls.get(ev.motion);
  if (own && otl) for (const x of fxInstances(o.fx, otl, { ex: !!S.exMode })) if (x.type === 'fx') out.push({ ...x, ctx: own });
  if (otl && S.fx) for (const e of otl.keyed) {
    if (opCode(e.op) !== SP_SUMMON_FX) continue;
    const a = argsOf(e), recs = S.fx.byId.get(a[0]); if (!recs) continue;
    out.push({ type: 'fx', tick: e.frame, recs: recs.slice(0, 1), life: a[1] > 0 ? a[1] : null, bone: a[2] || 0, kind: 'keyed', idx: 0, ctx: spCtx(live, S.fx) });
  }
  // 3. the owner's prop_effect (0x055F {slot, record, bit, scale, prop}) on this object: a trail from its bank, on its bones
  if (own && tl) for (const e of tl.ranged) {
    if (opCode(e.op) !== SP_PROP_FX || !spPass(e)) continue;
    const a = argsOf(e); if (!(a[4] === ev.id || a[4] < 0)) continue;
    const bit = a[2] || 0, recs = (o.fx.byId.get(a[1]) || []).filter((r) => r.trig === 0 && ((r.keys >>> bit) & 1));
    const mine = recs.filter((r) => o.names.includes(r.bone)), pick = (mine.length ? mine : recs).slice(0, 1);
    const shift = ev.offset + ev.carried - ev.start;                       // owner tick -> object motion tick
    if (pick.length) out.push({ type: 'fx', tick: e.start + shift, end: e.end === FOREVER ? null : e.end + shift, recs: pick, kind: 'ranged', idx: 0,
      slot: (a[0] || 0) & 15, scale: typeof a[3] === 'number' && a[3] > 0 ? a[3] : 1, trail: true, ctx: own });
  }
}
function spFxInsts() { return SP.fxInsts; }    // app.js fxRefresh: the objects' effect instances
/* the inspector's line under a hold_prop / summon_object: what its row spawns, and where */
function spEventNote(e) {
  const c = opCode(e.op); if (c !== SP_HOLD && c !== SP_SUMMON) return '';
  const a = argsOf(e), row = a[1], id = a[0];
  if (!SP.tables) { spTables().then((t) => t && renderInspector()); return '<p class="note">The object it spawns shows with the game ISO set.</p>'; }
  const where = id & 0x4000 ? 'on the character' : `on bone ${id} (${esc(S.boneNames[id] || '?')})`;
  return `<p class="note">wpnp row ${row}: <b class="mono">${esc(spRowLabel(row) || 'nothing')}</b>, ${where}${a[3] > 0 ? ', then left in place' : ', following it'}${a[2] ? '; the game may swap the row for one it picks' : ''}.</p>`;
}
function spRowLabel(row) {
  const i = spRowInfo(row); if (!i) return SP.tables ? `row ${row}: no object` : '';
  return `${i.stem}` + (i.row.script > 0 ? ` · script ${i.row.script}` : '') + (i.row.effect >= 0 ? ` · effect ${i.row.effect}` : '');
}
(function spWire() { const cb = $('showSpawn'); if (cb) cb.onchange = () => pose(); })();
