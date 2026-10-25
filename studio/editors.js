/* Move Studio: editors for the rest of a moveset (loaded after app.js, moves.js, dummy.js):
     - hitboxes: the objx hit-volume section (type 5), per group, drawn on the model;
     - projectiles: projectiles.json records (clone, tune, flags, children), with the runtime's hitact= hook;
     - a press's action record: CPU reach windows, start, follow-up only, homing at start, name; the CPU weights (brat);
     - the rest of a hit's fields: class, contact effect, hit sound, screen shake, EX force, knock-back bone;
     - runtime hooks per animation: warp= / warpreach= / vtrack= / landact= (mod.json runtime_hooks).
   Layouts: docs/archive/hit-contact, projectile-flight, attacker-movement, hit-reaction; tools/charmod_magi.py. */

const edCan = () => !S.readOnly && !(S.rig && S.rig.retail) && !(S.mod && S.mod.kind === 'retail');
const edSel = (name, opts, cur, attrs = '') => `<select ${attrs}>${opts.map(([v, l]) => `<option value="${esc(v)}"${String(v) === String(cur) ? ' selected' : ''}>${esc(l)}</option>`).join('')}${opts.some(([v]) => String(v) === String(cur)) ? '' : `<option selected value="${esc(cur)}">${esc(cur)}</option>`}</select>`;
const edFld = (label, inner, title = '') => `<div class="field"${title ? ` title="${esc(title)}"` : ''}><label>${esc(label)}</label>${inner}</div>`;
const edNum = (v, attrs) => `<input type="number" step="any" value="${esc(v)}" ${attrs}>`;
const edBones = () => [['16384', 'root (the character)'], ...S.boneNames.map((b, i) => [String(i), `${i} · ${b}`])];

/* ====================================================== hitboxes (objx section type 5) */
const ED_KINDS = [[4, '4 · hits'], [6, '6 · clash / guard'], [5, '5 · vs projectiles'], [7, '7 · trail point'], [1, '1 · body push'], [2, '2 · hurt'], [3, '3 · hurt (legs)'], [10, '10 · target point'], [0, '0 · body'], [11, '11'], [15, '15']];
const edVolPath = () => Object.keys(S.files).find((p) => S.files[p].vols) || '';
function edVolParse(bin) {
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength), n = dv.getUint32(8, true), recs = [];
  for (let i = 0; i < n; i++) {
    const o = 16 + 32 * i;
    recs.push({ group: dv.getUint32(o, true), kind: bin[o + 4], b5: bin[o + 5], bone: dv.getUint16(o + 6, true),
      off: [dv.getFloat32(o + 8, true), dv.getFloat32(o + 12, true), dv.getFloat32(o + 16, true)], y: dv.getFloat32(o + 20, true), r: dv.getFloat32(o + 24, true), r2: dv.getFloat32(o + 28, true) });
  }
  return { head: bin.slice(0, 16), recs };
}
function edVolBytes(v) {
  const out = new Uint8Array(16 + 32 * v.recs.length), dv = new DataView(out.buffer);
  out.set(v.head, 0); dv.setUint32(8, v.recs.length, true);
  v.recs.forEach((r, i) => {
    const o = 16 + 32 * i;
    dv.setUint32(o, r.group, true); out[o + 4] = r.kind; out[o + 5] = r.b5; dv.setUint16(o + 6, r.bone, true);
    r.off.forEach((x, k) => dv.setFloat32(o + 8 + 4 * k, x, true)); dv.setFloat32(o + 20, r.y, true); dv.setFloat32(o + 24, r.r, true); dv.setFloat32(o + 28, r.r2, true);
  });
  return out;
}
function edVolEdit(fn) {
  const p = edVolPath(); if (!p) return;
  edit([p], () => { const v = edVolParse(S.files[p].bin); fn(v); S.files[p].bin = edVolBytes(v); });
  if (typeof DM !== 'undefined') { DM.group = null; DM.key = ''; }
}
function edGroupUsers(g) {                     // the windows that arm group g
  const out = [];
  for (const p of S.tlPaths) { const t = F(p); t.ranged.forEach((e, i) => { if (e.group === g) out.push({ p, i, motion: t.motion, hit: e.hit }); }); }
  return out;
}
function edVolHTML(e) {
  const p = edVolPath(), g = e.group;
  if (!p) return '';
  const recs = edVolParse(S.files[p].bin).recs.map((r, i) => ({ ...r, i })).filter((r) => r.group === g), users = edGroupUsers(g), can = edCan();
  const ro = can ? '' : ' disabled';
  let h = `<div class="sec" id="edVol"><h3>Hitboxes · group ${g} <small>${recs.length} sphere${recs.length === 1 ? '' : 's'}</small></h3>
    <p class="note">Spheres on a bone (or the character's root), offset in the bone's own frame. Only kind 4 deals the hit; kind 6 clashes with attacks and guards. Drawn on the model while this event is selected (red = hits, blue = clash).${users.length > 1 ? ` Armed by ${users.length} windows: ${esc([...new Set(users.map((u) => motionName(u.motion)))].join(', '))}.` : ''}</p>`;
  if (users.length > 1 && can) h += `<button type="button" id="edVolOwn">Give this window its own group</button>`;
  h += `<div class="edvol">${recs.map((r) => `<div class="edvol-row" data-i="${r.i}">
      ${edSel('kind', ED_KINDS, r.kind, `data-k="kind" title="kind"${ro}`)}
      ${edSel('bone', edBones(), r.bone, `data-k="bone" title="bone"${ro}`)}
      <span class="edvol-xyz">${r.off.map((x, k) => `<input type="number" step="0.05" value="${+x.toFixed(4)}" data-k="o${k}" title="${'xyz'[k]} (m)"${ro}>`).join('')}</span>
      <input type="number" step="0.05" min="0" value="${+r.r.toFixed(4)}" data-k="r" title="radius (m)"${ro}>
      ${can ? '<button type="button" class="mini danger" data-k="del" title="Delete this sphere">×</button>' : ''}</div>`).join('')}</div>
    ${can ? '<button type="button" id="edVolAdd">+ Add sphere</button>' : ''}</div>`;
  return h;
}
function edVolWire(p, kind, idx) {
  const sec = $('edVol'); if (!sec || !edCan()) return;
  const e = F(p)[kind][idx], g = e.group;
  sec.querySelectorAll('.edvol-row').forEach((row) => {
    const i = +row.dataset.i;
    row.querySelectorAll('[data-k]').forEach((el) => {
      const k = el.dataset.k;
      if (k === 'del') { el.onclick = () => { edVolEdit((v) => v.recs.splice(i, 1)); renderInspector(); pose(); }; return; }
      el.onchange = () => {
        const val = parseFloat(el.value);
        edVolEdit((v) => {
          const r = v.recs[i];
          if (k === 'kind') r.kind = +el.value; else if (k === 'bone') r.bone = +el.value;
          else if (k === 'r') { r.r = r.r2 = Math.max(0, val || 0); } else r.off[+k[1]] = isNaN(val) ? 0 : val;
        });
        renderInspector(); pose();
      };
    });
  });
  $('edVolAdd').onclick = () => {
    edVolEdit((v) => {
      let at = -1; v.recs.forEach((r, k) => { if (r.group === g) at = k; });
      const src = at >= 0 ? v.recs[at] : null;
      const r = src ? { ...src, off: src.off.slice() } : { group: g, kind: 4, b5: 3, bone: 0x4000, off: [0, 1, 1], y: 0, r: 0.8, r2: 0.8 };
      if (at >= 0) v.recs.splice(at + 1, 0, r); else v.recs.push(r);
    });
    renderInspector(); pose();
  };
  const own = $('edVolOwn');
  if (own) own.onclick = () => {
    const v0 = edVolParse(S.files[edVolPath()].bin), ng = Math.max(...v0.recs.map((r) => r.group)) + 1;
    edVolEdit((v) => { for (const r of v.recs.filter((x) => x.group === g)) v.recs.push({ ...r, off: r.off.slice(), group: ng }); });
    edit([p], () => { F(p)[kind][idx].group = ng; });
    renderInspector(); pose(); toast(`This window now has group ${ng} (a copy of group ${g}).`);
  };
}
/* drawing: the selected window's group on the model */
const ED_VIS = { g: null };
function edVolDraw() {
  if (!ED_VIS.g) { ED_VIS.g = new THREE.Group(); view.scene.add(ED_VIS.g); }
  ED_VIS.g.clear();
  const p = edVolPath(), t = curTL(), e = S.sel && t && t[S.sel.kind] && t[S.sel.kind][S.sel.idx];
  if (!p || !e || e.group == null || !view.root || S.fxEdit) return;
  view.root.updateMatrixWorld(true);
  for (const r of edVolParse(S.files[p].bin).recs) {
    if (r.group !== e.group) continue;
    const M = r.bone === 0x4000 ? view.root.matrixWorld : view.bones[r.bone] && view.bones[r.bone].matrixWorld; if (!M) continue;
    const c = new THREE.Vector3(...r.off).applyMatrix4(M);
    const col = r.kind === 4 ? 0xff6f7f : r.kind === 6 ? 0x8fd3ff : 0xe8ac52;
    const m = new THREE.Mesh(new THREE.SphereGeometry(Math.max(0.02, r.r), 16, 10), new THREE.MeshBasicMaterial({ color: col, wireframe: true, transparent: true, opacity: r.kind === 4 ? 0.6 : 0.35 }));
    m.position.copy(c); ED_VIS.g.add(m);
  }
}

/* ====================================================== projectiles (projectiles.json) */
const ED_FLAGS = ['PIERCE', 'NO_TERRAIN', 'GROUND_HUG', 'FX_FOLLOW_DIR', 'UNK_10', 'DEATH_EMIT', 'FX_ALIGN_FLOOR', 'NO_EFFECT2', 'NO_EFFECT', 'NO_VISUALS', 'UNK_400', 'BOUNCE', 'SWEEP_ALT', 'HOMING_XZ', 'HOMING_Y',
  'CLASH_ATTACKER', 'CLASH_VICTIM', 'INHERIT_SIZE', 'CANCEL_ON_DAMAGE', 'STOP_ON_FAIL', 'CANCEL_ON_ACTION', 'LIMIT_NEWEST', 'PULL_FIELD', 'PULL_COND', 'GIVE_EFFECT', 'COLL_MASK_ALT', 'NO_IMPACT_FX', 'HOMING_STOP', 'SPAWN_LOS', 'CANCEL_COND1', 'CANCEL_COND2', 'UNK_80000000'];
const ED_FLAG_TIP = { PIERCE: 'survives hitting a character', NO_TERRAIN: 'passes through walls and floor', GROUND_HUG: 'stays on the floor', DEATH_EMIT: 'fires its child at death instead of birth', BOUNCE: 'bounces off terrain', HOMING_XZ: 'homes only horizontally', HOMING_Y: 'homes only vertically', INHERIT_SIZE: 'size from the parent shot', CANCEL_ON_DAMAGE: 'stops if the owner is hit', STOP_ON_FAIL: 'emitter stops when a shot fails to spawn', CANCEL_ON_ACTION: 'stops when the owner starts another action', PULL_FIELD: 'drags the foe (pull values)', HOMING_STOP: 'stops dead when homing reaches its stop distance', NO_EFFECT: 'no primary visual', NO_EFFECT2: 'no secondary visual' };
const ED_ORIGIN = [[0, '0 · owner'], [1, '1 · owner bone (arg)'], [2, '2 · owner bone, its own up'], [3, '3 · target'], [4, '4 · target centre'], [5, '5 · parent shot'], [6, '6 · owner effect (FEPA id arg)'], [7, '7 · world origin'], [8, '8 · second target'], [9, '9 · second target centre'], [10, '10 · summon'], [11, '11 · lock / second target'], [12, '12 · lock / second centre'], [13, '13 · opponent centre']];
const ED_DIR = [[0, '0 · keep (origin frame)'], [1, '1 · owner facing'], [2, '2 · owner root Z'], [3, '3 · parent direction'], [4, '4 · aim at the target']];
const ED_MOTION = [[0, '0 · straight'], [1, '1 · homing'], [2, '2 · parked (no motion)'], [3, '3 · glued to the origin']];
const ED_TARGET = [[0, '0 · lock-on target'], [1, '1 · owner'], [2, '2 · second target'], [3, '3 · lock, else second'], [4, '4 · opponent'], [5, '5 · none (never homes)']];
const ED_PGROUPS = [
  ['Hit', ['hit_delay', 'hit_time', 'lifetime', 'size', 'size_rate', 'size_start', 'size_min', 'size_max']],
  ['Flight', ['speed', 'accel', 'accel_start', 'speed_min', 'speed_max', 'gravity', 'gravity_start', 'move_time']],
  ['Homing', ['turn_rate', 'homing_start', 'homing_time', 'homing_stop', 'aim_dist', 'aim_yaw', 'aim_height']],
  ['Spawn', ['shots', 'first_delay', 'interval', 'fwd_dist', 'fwd_yaw', 'off_x', 'off_y', 'off_z', 'launch_pitch', 'launch_yaw', 'tgt_dist_min', 'tgt_dist_max', 'tgt_height_min', 'tgt_height_max']],
  ['Children in flight', ['emit_count', 'emit_delay', 'emit_interval']],
  ['Pull', ['pull_start', 'pull_time', 'pull_strength', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist']]];
const ED_PTIP = { speed: 'm per tick', accel: 'speed change per tick', turn_rate: 'degrees per tick', homing_time: '0 = until the stop distance', homing_stop: 'homing ends this close to the target (m)', move_time: 'moves only while younger than this (0 = always)', lifetime: 'ticks', hit_delay: 'the hitbox appears at this age', hit_time: 'the hitbox lasts this long', size: 'scales the hit radius / length', launch_pitch: 'degrees; positive points down', shots: 'shots the emitter fires', interval: 'ticks between shots', aim_height: 'm above the target point' };
const edProjFile = () => F(msPath('projectiles.json'));
const edProj = (id) => (edProjFile() ? edProjFile().records.find((r) => r.id === id) : null);
const edFmt = (x) => (Math.abs(x) >= 1e4 ? x.toExponential(2) : String(+x.toFixed(4)));
function edCompactPool(j) {                 // drop pool values no record uses (charmod_magi.compact_pool; pool[0], [1] kept)
  const used = new Set([0, 1]);
  for (const r of j.records) for (const t of Object.values(r.params)) t.forEach((k) => used.add(k));
  const order = [...used].sort((a, b) => a - b), map = new Map(order.map((k, i) => [k, i]));
  j.pool = order.map((k) => j.pool[k]);
  for (const r of j.records) for (const n of Object.keys(r.params)) r.params[n] = r.params[n].map((k) => map.get(k));
}
function edPoolIndex(j, v) {                 // the game loads pool indices as signed bytes: at most 128 values
  const f = Math.fround(v);
  let i = j.pool.findIndex((x) => Math.fround(x) === f);
  if (i < 0 && j.pool.length >= 128) { edCompactPool(j); i = j.pool.findIndex((x) => Math.fround(x) === f); }
  if (i < 0) { if (j.pool.length >= 128) throw new Error('the float pool is full (128 values in use by the records)'); j.pool.push(f); i = j.pool.length - 1; }
  return i;
}
function edSetParam(j, r, name, lo, hi, step) {   // values as numbers; the pool triple follows
  const t = r.params[name].slice();
  if (lo != null) t[0] = edPoolIndex(j, lo);
  if (hi != null) t[1] = edPoolIndex(j, hi);
  if (step != null) t[2] = edPoolIndex(j, step);
  r.params[name] = t; r.values[name] = t.map((k) => j.pool[k]);
}
function edFxOpts() {                          // the character's FEPA records (effects.json), for a projectile's look
  const out = [[65535, 'none']];
  for (const x of S.effects.values()) out.push([x.id, `${x.id} · ${x.name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '')}`]);
  return out;
}
function edProjHTML(id, label) {
  const r = edProj(id), can = edCan() && !!r, ro = can ? '' : ' disabled';
  if (!r) return `<div class="sec"><h3>Projectile ${esc(String(id))}</h3><p class="note">${edProjFile() ? 'Not in projectiles.json.' : 'Projectiles of retail characters are shown, not edited here.'}</p></div>`;
  const recs = edProjFile().records, flags = parseInt(r.flags, 16) >>> 0;
  const users = edProjUsers(id), linkOpts = [[-1, 'none'], ...recs.map((x) => [x.id, `${x.id}`])];
  const hits = [[65535, 'none (no hitbox)'], ...[...S.hitById.values()].map((h) => [parseInt(h.id, 16), `${h.name || h.id}${parseInt(h.flags, 16) & 2 ? ' (HP)' : ''}`])];
  const H = dmHooks ? dmHooks() : { hitact: null }, act = H.hitact && H.hitact.magi === id ? H.hitact.action : '';
  let h = `<div class="sec" id="edProj"><h3>Projectile ${id} <small>${esc(label || '')}</small></h3>
    <p class="note">Flown by the game's rules (docs/archive/projectile-flight). Turn on <b>Dummy</b> to see where it goes and whether it hits.${users.length > 1 ? ` Fired or chained from ${users.length} places: ${esc(users.join(', '))}.` : ''}</p>
    ${can ? `<div class="btnrow"><button type="button" id="edProjCopy">${users.length > 1 ? 'Give this event its own copy' : 'Copy to a new projectile'}</button></div>` : ''}
    <div class="row2">${edFld('Hit', edSel('atkp', hits, r.atkp, `data-f="atkp"${ro}`))}${edFld('Hit after a reflect', edSel('atkp_r', hits, r.atkp_reflected, `data-f="atkp_reflected"${ro}`))}</div>
    <div class="row2">${edFld('Looks like', edSel('eff', edGroupOpts(r.effect), r.effect, `data-f="effect"${ro}`), 'an effect group: the placements tagged with it play as the shot is born (birth) and fly with it (body)')}${edFld('Second look', edSel('eff2', edFxOpts(), r.common_effect, `data-f="common_effect"${ro}`), 'a shared-library effect; none = reuse the first')}</div>
    <div class="row2">${edFld('Reflect / guard', edSel('kind', [[0, '0 · reflectable by guard'], [1, '1 · not reflectable (destroyed)'], [2, '2 · not reflectable'], [3, '3 · not reflectable'], [4, '4 · attached object']], r.kind, `data-f="kind"${ro}`))}<div></div></div>
    <div class="row2">${edFld('Hit radius (m)', edNum(r.hit_radius, `data-f="hit_radius" min="0"${ro}`), '× size')}${edFld('Column height (m)', edNum(r.hit_length, `data-f="hit_length" min="0"${ro}`), '0 = a sphere; else an upright column this tall (× size)')}</div>
    <div class="row2">${edFld('Moves', edSel('motion', ED_MOTION, r.motion, `data-f="motion"${ro}`))}${edFld('Homes on', edSel('target', ED_TARGET, r.target, `data-f="target"${ro}`))}</div>
    <div class="row2">${edFld('Starts at', edSel('origin', ED_ORIGIN, r.origin, `data-f="origin"${ro}`))}${edFld('Origin bone / id', edNum(r.origin_arg, `data-f="origin_arg" min="0"${ro}`))}</div>
    <div class="row2">${edFld('Direction', edSel('direction', ED_DIR, r.direction, `data-f="direction"${ro}`))}${edFld('Most alive at once', edNum(r.max_alive, `data-f="max_alive" min="0"${ro}`), '0 = no limit')}</div>
    <div class="row3">${edFld('Child at birth / in flight', edSel('le', linkOpts, r.link_emit, `data-f="link_emit"${ro}`))}${edFld('Child on hit', edSel('lh', linkOpts, r.link_hit, `data-f="link_hit"${ro}`))}${edFld('Child on terrain', edSel('lt', linkOpts, r.link_terrain, `data-f="link_terrain"${ro}`))}</div>
    ${edFld('When it spawns, start action (runtime hitact=)', `<input type="number" min="0" value="${esc(act)}" placeholder="none" data-hook="hitact"${ro}>`, 'Noctis uses it for Alterna: the grip shot starts the follow-up. Needs the native build.')}`;
  for (const [title, names] of ED_PGROUPS) {
    const open = title === 'Hit' || title === 'Flight' || (title === 'Homing' && r.motion === 1) || (title === 'Pull' && flags & 0x400000);
    h += `<details class="edp"${open ? ' open' : ''}><summary class="lbl">${title}</summary><div class="edp-grid">${names.map((n) => {
      const [lo, hi, st] = r.values[n], txt = lo === hi ? edFmt(lo) : `${edFmt(lo)}..${edFmt(hi)}`;
      return `<label title="${esc(ED_PTIP[n] || '')}${st ? ` · each later shot adds ${edFmt(st)}` : ''}"><span>${n.replace(/_/g, ' ')}</span><input type="text" class="mono" value="${txt}${st ? ' +' + edFmt(st) : ''}" data-p="${n}"${ro}></label>`;
    }).join('')}</div></details>`;
  }
  h += `<details class="edp"><summary class="lbl">Flags <small class="mono">${esc(r.flags)}</small></summary><div class="edp-flags">${ED_FLAGS.map((n, b) => `<label class="chk" title="${esc(ED_FLAG_TIP[n] || '')}"><input type="checkbox" data-flag="${b}"${flags & (1 << b) ? ' checked' : ''}${ro}> ${n}</label>`).join('')}</div></details>
    <p class="note">A value is <span class="mono">3</span>, a random range <span class="mono">2..4</span>, and <span class="mono">+0.5</span> adds that to each later shot.</p></div>`;
  return h;
}
function edProjUsers(id) {
  const out = [];
  for (const p of S.tlPaths) { const t = F(p); for (const e of [...t.keyed, ...t.ranged]) if (e.projectile === id) out.push(motionName(t.motion)); }
  for (const r of (edProjFile() || { records: [] }).records) for (const k of ['link_emit', 'link_hit', 'link_terrain', 'link_terrain_1', 'link_terrain_2', 'link_terrain_4', 'link_fall', 'link_clash']) if (r[k] === id) out.push('projectile ' + r.id);
  return out;
}
function edProjWire(id, onCopy) {
  const sec = $('edProj'); if (!sec || !edCan()) return;
  const pp = msPath('projectiles.json'), r = edProj(id); if (!r) return;
  const redo = () => { derive(); if (typeof DM !== 'undefined') DM.key = ''; renderInspector(); pose(); };
  sec.querySelectorAll('[data-f]').forEach((el) => el.onchange = () => {
    const f = el.dataset.f, v = parseFloat(el.value);
    edit([pp], () => { const x = edProj(id); x[f] = ['hit_radius', 'hit_length'].includes(f) ? (isNaN(v) ? 0 : v) : (isNaN(v) ? 0 : Math.round(v)); });
    redo();
  });
  sec.querySelectorAll('[data-p]').forEach((el) => el.onchange = () => {
    const m = el.value.trim().match(/^(-?[\d.e+-]+)(?:\s*\.\.\s*(-?[\d.e+-]+))?(?:\s*\+\s*(-?[\d.e+-]+))?$/i);
    if (!m) { toast('Write a value, a range like 2..4, or add +0.5 for each later shot.'); renderInspector(); return; }
    const lo = parseFloat(m[1]), hi = m[2] != null ? parseFloat(m[2]) : lo, st = m[3] != null ? parseFloat(m[3]) : 0;
    try { edit([pp], () => edSetParam(edProjFile(), edProj(id), el.dataset.p, lo, hi, st)); } catch (e) { toast(e.message); }
    redo();
  });
  sec.querySelectorAll('[data-flag]').forEach((el) => el.onchange = () => {
    edit([pp], () => { const x = edProj(id); let f = parseInt(x.flags, 16) >>> 0; const b = 1 << +el.dataset.flag; f = el.checked ? (f | b) >>> 0 : (f & ~b) >>> 0;
      x.flags = '0x' + f.toString(16).padStart(8, '0'); x.flag_names = ED_FLAGS.filter((n, k) => f & (1 << k)); });
    redo();
  });
  const hk = sec.querySelector('[data-hook="hitact"]');
  if (hk) hk.onchange = () => { const a = parseInt(hk.value, 10); edHookSet('hitact', a >= 0 ? `hitact=${id},${a}` : null, (t) => t.startsWith('hitact=' + id + ',')); redo(); };
  const cp = $('edProjCopy');
  if (cp) cp.onclick = () => {
    const j = edProjFile(), nid = Math.max(...j.records.map((x) => x.id)) + 1, c = JSON.parse(JSON.stringify(r)); c.id = nid;
    edit([pp], () => j.records.push(c));
    if (onCopy) onCopy(nid);
    redo(); toast(`New projectile ${nid} (a copy of ${id}).`);
  };
}

/* ====================================================== a press's action record (plya) and the CPU weights (brat) */
const edF32 = (a, o) => { const r = mvRaw(a); return new DataView(r.buffer).getFloat32(o, true); };
function edSetF32(a, o, v) { const r = mvRaw(a), dv = new DataView(r.buffer); dv.setFloat32(o, v, true); a.raw = [...r].map((x) => x.toString(16).padStart(2, '0')).join(''); }
function edSetU32(a, o, v) { const r = mvRaw(a), dv = new DataView(r.buffer); dv.setUint32(o, v >>> 0, true); a.raw = [...r].map((x) => x.toString(16).padStart(2, '0')).join(''); }
const edU32 = (a, o) => new DataView(mvRaw(a).buffer).getUint32(o, true);
function edActName(a) { const r = mvRaw(a); let s = ''; for (let i = 0x64; i < 0x74 && r[i]; i++) s += String.fromCharCode(r[i]); return s; }
const ED_BRAT = [[0, '0 · 0.2'], [1, '1 · 0.4'], [2, '2 · 0.7'], [3, '3 · never'], [4, '4 · 0.01']];
const ED_SIT = ['HP state 1', 'HP state 2', 'HP state 3', 'Bravery state 1', 'Bravery state 2', 'Special / chase'];
function edPressHTML() {
  const m = S.moves[S.step.move], s = m.steps[S.step.i], a = s && S.actionById.get(s.action), can = edCan();
  if (!a) return '';
  const ro = can ? '' : ' disabled', r = mvRaw(a), fl = edU32(a, 0x18), home = !!(fl & 1);
  const shared = S.moves.filter((mm) => mm.steps.some((x) => x.action === a.action)).length > 1;
  let h = `<details class="sec" id="edPress"${S.edPressOpen ? ' open' : ''}><summary class="lbl" style="cursor:pointer">Press settings <small class="mono">action ${a.action} · ${esc(edActName(a))}</small></summary>
    ${shared ? '<p class="warnline">Other moves use this action too.</p>' : ''}
    <div class="row2">${edFld('Starts', edSel('st', [[0, 'anywhere'], [2, 'on the ground'], [1, 'in the air']], r[5], `data-b="5"${ro}`))}${edFld('Only as a later press', `<label class="chk"><input type="checkbox" data-b="3"${r[3] ? ' checked' : ''}${ro}> follow-up only</label>`)}</div>
    <div class="lbl">CPU uses it when the foe is</div>
    <div class="row2">${edFld('Distance from (m)', edNum(+edF32(a, 0x24).toFixed(3), `data-o="36"${ro}`))}${edFld('to (m)', edNum(+edF32(a, 0x28).toFixed(3), `data-o="40"${ro}`))}</div>
    <div class="row2">${edFld('Height from (m)', edNum(+edF32(a, 0x2C).toFixed(3), `data-o="44"${ro}`))}${edFld('to (m)', edNum(+edF32(a, 0x30).toFixed(3), `data-o="48"${ro}`))}</div>
    <label class="chk"><input type="checkbox" id="edHome"${home ? ' checked' : ''}${ro}> Homing dash when it starts</label>`;
  if (home) h += `<div class="row3">${[['Speed', 0x40], ['Vertical speed', 0x44], ['Track ticks', 0x48], ['Arrive radius', 0x4C], ['Max ticks', 0x50], ['Height', 0x54], ['Angle (rad)', 0x58], ['Stand-off (m)', 0x5C]].map(([l, o]) => edFld(l, edNum(+edF32(a, o).toFixed(3), `data-o="${o}"${ro}`))).join('')}</div>`;
  // CPU weights: the move's brat row (first press only)
  const bp = typeof mvBratPath === 'function' ? mvBratPath() : '';
  if (bp && S.step.i === 0) {
    const b = mvBratRows(S.files[bp].bin), row = b.rows.find((x) => x.cmd === m.id);
    if (row) h += `<div class="lbl">CPU weights (how keenly it picks this move)</div><div class="row3">${row.sit.map(([off, n], k) => edFld(ED_SIT[k], n === 1 ? edSel('w', ED_BRAT, b.w[off], `data-w="${k}"${ro}`) : `<input disabled value="${n} codes">`)).join('')}</div>`;
    else h += `<p class="note">The CPU never picks this move (no row for command ${esc(m.hexId)}).${can ? ' <button type="button" id="edBratAdd" class="mini">Add a row</button>' : ''}</p>`;
  }
  const tx = edText();
  if (tx) {                                   // the HP-move banner: plya +0x21 picks a record of the objx 'text' section
    const cur = r[0x21], recs = tx.langs[0], rec = recs.find((x) => x.index === cur);
    h += `<div class="lbl">Banner (the name shown when it lands)</div>
      <div class="row2">${edFld('Record', edSel('ban', [[255, 'none'], ...recs.map((x) => [x.index, `${x.index} · ${x.text}`]), ...(can ? [['new', '+ new record']] : [])], cur, `id="edBan"${ro}`))}
      ${edFld('Text (English)', `<input type="text" id="edBanText" value="${esc(rec ? rec.text : '')}" maxlength="24"${rec && can ? '' : ' disabled'}>`, 'Letters, digits, space and ! % & \' + , : - “ ”; other signs are not known in the game font')}</div>
      ${rec && S.moves.filter((mm) => mm.steps.some((x) => { const aa = S.actionById.get(x.action); return aa && mvRaw(aa)[0x21] === cur; })).length > 1 ? '<p class="note">Other presses show this record too.</p>' : ''}`;
  }
  h += `<details><summary class="lbl" style="cursor:pointer">Raw record</summary><textarea id="edRaw" class="mono" rows="5"${ro}>${esc(a.raw)}</textarea><p class="note">116 bytes. +0 action, +0x1C script (both from the fields above them), +0x64 name.</p></details></details>`;
  return h;
}
function edPressWire() {
  const sec = $('edPress'); if (!sec) return;
  sec.ontoggle = () => { S.edPressOpen = sec.open; };
  if (!edCan()) return;
  const m = S.moves[S.step.move], s = m.steps[S.step.i], a = S.actionById.get(s.action), pa = msPath('actions.json');
  const redo = () => { derive(); if (typeof DM !== 'undefined') DM.key = ''; renderInspector(); pose(); };
  sec.querySelectorAll('[data-b]').forEach((el) => el.onchange = () => { edit([pa], () => mvSetRaw(a, +el.dataset.b, el.type === 'checkbox' ? (el.checked ? 1 : 0) : +el.value)); redo(); });
  sec.querySelectorAll('[data-o]').forEach((el) => el.onchange = () => { const v = parseFloat(el.value); edit([pa], () => edSetF32(a, +el.dataset.o, isNaN(v) ? 0 : v)); redo(); });
  $('edHome').onchange = () => { edit([pa], () => { let f = edU32(a, 0x18); f = $('edHome').checked ? f | 1 : f & ~1; edSetU32(a, 0x18, f); if ($('edHome').checked && !edF32(a, 0x40)) { edSetF32(a, 0x40, 0.5); edSetF32(a, 0x48, 6); edSetF32(a, 0x50, 6); edSetF32(a, 0x5C, 1.5); } }); redo(); };
  const tp = edTextPath(), ban = $('edBan'), bt = $('edBanText');
  if (ban) ban.onchange = () => {
    if (ban.value === 'new') {
      const o = OBJTEXT.parseText(S.files[tp].bin), idx = Math.max(-1, ...o.langs[0].map((x) => x.index)) + 1;
      if (idx > 254) { toast('An action can only name records 0–254.'); renderInspector(); return; }
      edit([tp, pa], () => { OBJTEXT.setRecord(o, 0, idx, m.name.replace(/\s*\((ground|midair)\)/, '')); S.files[tp].bin = OBJTEXT.textToBytes(o); mvSetRaw(a, 0x21, idx); });
    } else edit([pa], () => mvSetRaw(a, 0x21, +ban.value));
    redo();
  };
  if (bt) bt.onchange = () => {
    const bad = OBJTEXT.unsupportedChars(bt.value);
    if (bad && bad.length) { toast('Not in the game font: ' + bad.join(' ')); renderInspector(); return; }
    const o = OBJTEXT.parseText(S.files[tp].bin), cur = mvRaw(a)[0x21];
    edit([tp], () => { OBJTEXT.setRecord(o, 0, cur, bt.value); S.files[tp].bin = OBJTEXT.textToBytes(o); });
    redo();
  };
  const raw = $('edRaw');
  raw.onchange = () => { const v = raw.value.replace(/\s+/g, '').toLowerCase(); if (!/^[0-9a-f]{232}$/.test(v)) { toast('The record is 116 bytes: 232 hex digits.'); renderInspector(); return; } edit([pa], () => { a.raw = v; }); redo(); };
  const bp = mvBratPath();
  sec.querySelectorAll('[data-w]').forEach((el) => el.onchange = () => {
    edit([bp], () => { const b = mvBratRows(S.files[bp].bin), row = b.rows.find((x) => x.cmd === m.id), [off] = row.sit[+el.dataset.w]; const u = S.files[bp].bin.slice(); u[20 + 28 * b.rows.length + 4 + off] = +el.value; S.files[bp].bin = u; });
    redo();
  });
  const ba = $('edBratAdd');
  if (ba) ba.onclick = () => { edit([bp], () => { const b = mvBratRows(S.files[bp].bin); S.files[bp].bin = mvBratAdd(S.files[bp].bin, m.id, b.rows.length ? b.rows[0].cmd : -1); }); redo(); };
}

const edTextPath = () => Object.keys(S.files).find((p) => S.files[p].text_) || '';
function edText() { const p = edTextPath(); if (!p || typeof OBJTEXT === 'undefined') return null; const f = S.files[p]; if (f.parsedOf !== f.bin) { try { f.parsed = OBJTEXT.parseText(f.bin); } catch (e) { f.parsed = null; } f.parsedOf = f.bin; } return f.parsed; }

/* ====================================================== the rest of a hit's fields */
function edHitMoreHTML(h) {
  if (!h || h.raw) return '';
  const can = edCan(), ro = can ? '' : ' disabled', sh = h.shake || '00000000', sb = (k) => parseInt(sh.slice(2 * k, 2 * k + 2), 16);
  const snd = parseInt(h.unk_30, 16) >>> 0;
  return `<details class="sec" id="edHit"${S.edHitOpen ? ' open' : ''}><summary class="lbl" style="cursor:pointer">More hit settings</summary>
    <div class="row2">${edFld('Class', edNum(h.class, `data-h="class" min="0" max="255"${ro}`), '4, 5, 9 and 12–15 never hurt (guard, FACT, EX, block types)')}${edFld('Contact effect bit', edNum(h.hit_effect_bit, `data-h="hit_effect_bit" min="0" max="255"${ro}`), 'effect records whose mask has this bit play on hit; 255 = none')}</div>
    <div class="row2">${edFld('Hit sound', `<input type="number" value="${snd === 0xFFFFFFFF ? -1 : snd}" data-h="unk_30"${ro}>`, '−1 = none')}${edFld('Knock-back bone', edNum(h.kb_bone, `data-h="kb_bone"${ro}`), 'attacker bone the stagger / knock target is measured from (0 = root)')}</div>
    <div class="row3">${edFld('Shake ticks', edNum(sb(1), `data-s="1" min="0" max="255"${ro}`))}${edFld('Shake ×0.01', edNum(sb(2), `data-s="2" min="0" max="255"${ro}`))}${edFld('Shake 2 ×0.01', edNum(sb(3), `data-s="3" min="0" max="255"${ro}`))}</div>
    <div class="row3">${edFld('EX force?', edNum(sb(0), `data-s="0" min="0" max="255"${ro}`), 'passed to the EX gauge manager on a fresh hit (meaning not confirmed)')}${edFld('Clash angle 1°', edNum(h.unk_0e, `data-h="unk_0e" min="0" max="255"${ro}`))}${edFld('Clash angle 2°', edNum(h.unk_0f, `data-h="unk_0f" min="0" max="255"${ro}`))}</div></details>`;
}
function edHitMoreWire(h) {
  const sec = $('edHit'); if (!sec) return;
  sec.ontoggle = () => { S.edHitOpen = sec.open; };
  if (!edCan() || !h) return;
  const hp = msPath('hits.json'), redo = () => { derive(); if (typeof DM !== 'undefined') DM.key = ''; renderTimeline(); };
  sec.querySelectorAll('[data-h]').forEach((el) => el.onchange = () => {
    const k = el.dataset.h, v = parseInt(el.value, 10) || 0;
    edit([hp], () => { h[k] = k === 'unk_30' ? '0x' + ((v < 0 ? 0xFFFFFFFF : v) >>> 0).toString(16).toUpperCase().padStart(8, '0') : Math.max(k === 'kb_bone' ? -32768 : 0, v); });
    redo();
  });
  sec.querySelectorAll('[data-s]').forEach((el) => el.onchange = () => {
    const k = +el.dataset.s, v = Math.max(0, Math.min(255, parseInt(el.value, 10) || 0));
    edit([hp], () => { const b = (h.shake || '00000000').match(/../g); b[k] = v.toString(16).padStart(2, '0'); h.shake = b.join(''); });
    redo();
  });
}

/* ====================================================== runtime hooks per animation (mod.json runtime_hooks) */
function edHookSet(kind, token, match) {      // replace the tokens `match` selects by `token` (null = remove)
  if (!S.modPath) return;
  edit([S.modPath], () => {
    const L = S.mod.runtime_hooks || (S.mod.runtime_hooks = []);
    const i = L.findIndex((t) => typeof t === 'string' && match(t));
    const rest = L.filter((t, k) => !(typeof t === 'string' && match(t) && k !== i));
    if (i >= 0) { const at = rest.indexOf(L[i]); if (token) rest[at] = token; else rest.splice(at, 1); } else if (token) rest.push(token);
    S.mod.runtime_hooks = rest;
  });
  if (typeof DM !== 'undefined') DM.key = '';
}
function edHooksHTML(mi) {
  if (!S.modPath || !S.mod || (S.rig && S.rig.retail) || S.mod.kind === 'retail') return '';
  const H = dmHooks(), can = edCan(), ro = can ? '' : ' disabled';
  const w = H.warps.find((x) => x.motions.includes(mi)), v = H.vtracks.find((x) => x.motions.includes(mi)), la = H.landact && H.landact.motions.includes(mi) ? H.landact : null;
  const shared = (x) => x && x.motions.length > 1 ? ` <small>(shared with ${x.motions.filter((m) => m !== mi).map(motionName).join(', ')})</small>` : '';
  return `<details class="sec" id="edHooks"${(w || v || la) ? ' open' : ''}><summary class="lbl" style="cursor:pointer">Runtime hooks${w || v || la ? ' · on' : ''}</summary>
    <p class="note">Extra rules the native build's runtime applies while this animation plays (mod.json runtime_hooks; runtime/roster.cpp). The dummy runs them too.</p>
    <label class="chk"><input type="checkbox" id="edWarp"${w ? ' checked' : ''}${ro}> Home its travel onto the foe (warp=)${shared(w)}</label>
    ${w ? `<div class="row3">${edFld('Travel (m)', edNum(w.travel, `data-w="travel" min="0"${ro}`), "the animation's own root travel to stretch / shrink")}${edFld('Stop short (m)', edNum(w.stop, `data-w="stop" min="0"${ro}`))}${edFld('Lift', edSel('lift', [[-1, 'flat (horizontal)'], [0, '3D'], ...(w.lift > 0 ? [[w.lift, w.lift + ' m above']] : [])], w.flat ? -1 : w.lift, `data-w="lift"${ro}`))}</div>
      ${edFld('Reach (m, empty = travel)', `<input type="number" step="any" value="${H.reach.has(mi) ? H.reach.get(mi) : ''}" data-w="reach"${ro}>`, 'warpreach=: the same travel paces a longer way')}` : ''}
    <label class="chk"><input type="checkbox" id="edVt"${v ? ' checked' : ''}${ro}> Ease to the foe's height (vtrack=)${shared(v)}</label>
    ${v ? `<div class="row2">${edFld('From tick', edNum(v.from, `data-v="from" min="0"${ro}`))}${edFld('To tick', edNum(v.to, `data-v="to" min="1"${ro}`))}${edFld('At most (m)', edNum(v.max, `data-v="max" min="0"${ro}`))}${edFld('Stay above a lower foe (m)', edNum(v.lift, `data-v="lift" min="0"${ro}`))}</div>` : ''}
    ${edFld('On touchdown, start action (landact=)', `<input type="number" min="0" value="${la ? la.action : ''}" placeholder="none" id="edLand"${ro}>`, H.landact && !la ? `landact= is used by ${H.landact.motions.map(motionName).join(', ')} (one per character)` : '')}
  </details>`;
}
function edHooksWire(mi) {
  const sec = $('edHooks'); if (!sec || !edCan()) return;
  const redo = () => { if (typeof DM !== 'undefined') DM.key = ''; renderInspector(); pose(); };
  const H = dmHooks(), w = H.warps.find((x) => x.motions.includes(mi)), v = H.vtracks.find((x) => x.motions.includes(mi));
  const isTok = (k, x) => (t) => t.startsWith(k + '=') && x && t.slice(k.length + 1).split(',')[0].split('/').map(Number).join('/') === x.motions.join('/');
  const ms = (x) => x.motions.join('/');
  const warpTok = (x) => `warp=${ms(x)},${x.travel},${x.stop}${x.flat ? ',-1' : x.lift > 0 ? ',' + x.lift : ''}`;
  $('edWarp').onchange = () => {
    if ($('edWarp').checked) edHookSet('warp', `warp=${mi},${(+motionTravel(mi).toFixed(2)) || 5},1.2,-1`, () => false);
    else if (w.motions.length > 1) edHookSet('warp', warpTok({ ...w, motions: w.motions.filter((m) => m !== mi) }), isTok('warp', w));
    else edHookSet('warp', null, isTok('warp', w));
    redo();
  };
  sec.querySelectorAll('[data-w]').forEach((el) => el.onchange = () => {
    const k = el.dataset.w, val = parseFloat(el.value);
    if (k === 'reach') { edHookSet('warpreach', isNaN(val) ? null : `warpreach=${mi},${val}`, (t) => t.startsWith('warpreach=') && t.slice(10).split(',')[0].split('/').map(Number).includes(mi)); redo(); return; }
    const x = { ...w };
    if (k === 'lift') { x.flat = val === -1; x.lift = val > 0 ? val : 0; } else x[k] = Math.max(0, isNaN(val) ? 0 : val);
    edHookSet('warp', warpTok(x), isTok('warp', w)); redo();
  });
  const vtTok = (x) => `vtrack=${ms(x)},${x.from},${x.to},${x.max}${x.lift ? ',' + x.lift : ''}`;
  $('edVt').onchange = () => {
    if ($('edVt').checked) edHookSet('vtrack', `vtrack=${mi},0,12,1.8,0.3`, () => false);
    else if (v.motions.length > 1) edHookSet('vtrack', vtTok({ ...v, motions: v.motions.filter((m) => m !== mi) }), isTok('vtrack', v));
    else edHookSet('vtrack', null, isTok('vtrack', v));
    redo();
  };
  sec.querySelectorAll('[data-v]').forEach((el) => el.onchange = () => { const x = { ...v }; x[el.dataset.v] = Math.max(0, parseFloat(el.value) || 0); edHookSet('vtrack', vtTok(x), isTok('vtrack', v)); redo(); });
  $('edLand').onchange = () => {
    const a = parseInt($('edLand').value, 10), cur = H.landact;
    const mo = cur ? cur.motions.filter((m) => m !== mi) : [];
    if (a >= 0) edHookSet('landact', `landact=${a},${[...mo, mi].join(',')}`, (t) => t.startsWith('landact='));
    else edHookSet('landact', mo.length ? `landact=${cur.action},${mo.join(',')}` : null, (t) => t.startsWith('landact='));
    redo();
  };
}
function motionTravel(mi) {                  // the animation's root travel (trans_include), for a new warp= default
  const ti = S.boneIndex.get('trans_include'); if (ti == null || !S.gmo) return 0;
  poseMotion(S.gmo, view, mi, 0); view.root.updateMatrixWorld(true); const a = new THREE.Vector3().setFromMatrixPosition(view.bones[ti].matrixWorld);
  poseMotion(S.gmo, view, mi, motionLen(mi)); view.root.updateMatrixWorld(true); const b = new THREE.Vector3().setFromMatrixPosition(view.bones[ti].matrixWorld);
  pose(); return a.distanceTo(b);
}

/* ====================================================== sounds (the character's bank: objx 'snd_', sndbank.js) */
const edSndPath = () => Object.keys(S.files).find((p) => S.files[p].snd) || '';
let edAudio = null;
function edBank() { const p = edSndPath(); if (!p) return null; const f = S.files[p]; if (f.bankOf !== f.bin) { f.bank = SndBank.parseBank(f.bin); f.bankOf = f.bin; } return f.bank; }
const edSndName = (i) => ((S.mod && S.mod.sound_names) || {})[i] || '';
function edPlay(i) {
  const b = edBank(); if (!b) return;
  try {
    const d = SndBank.decodeSlot(b, i);
    edAudio = edAudio || new (window.AudioContext || window.webkitAudioContext)();
    const buf = edAudio.createBuffer(1, d.pcm[0].length, d.rate); buf.copyToChannel(d.pcm[0], 0);
    const src = edAudio.createBufferSource(); src.buffer = buf; src.connect(edAudio.destination); if (typeof psndTrack === 'function') psndTrack(src); src.start();
  } catch (e) { toast('Could not play slot ' + i + ': ' + e.message); }
}
/* projectile looks: the effect groups (bits of the FEPA group mask) and what their birth / body placements show */
function edGroupOpts(cur) {
  const out = [[65535, 'none']];
  if (!S.fx) return out;
  for (let g = 0; g < 64; g++) {
    const rs = S.fx.fepa.filter((x) => !x.trig && fxGroupHas(x, g)); if (!rs.length && g !== cur) continue;
    const nm = (role) => { const r = rs.find((x) => x.role === role); return r ? fxuShort((S.fx.fep.names[r.effect] || {}).name) : null; };
    const b = nm(1), d = nm(2);
    out.push([g, `group ${g}: ${[b && 'birth ' + b, d && 'body ' + d].filter(Boolean).join(' · ') || (rs.length ? rs.length + ' placements, none a shot’s look' : 'empty')}`]);
  }
  return out;
}
function edSndHTML(e) {                           // a sound event: what it plays, Play, and the sound browser to change it
  const b = edBank(), cue = e.sound, base = b ? SndBank.cueBase(b) : -1, slot = cue - base, mine = b && slot >= 0 && slot < b.slots.length;
  const loc = !mine && typeof SharedSnd !== 'undefined' ? SharedSnd.cueLocation(cue) : null, s = mine ? b.slots[slot] : null;
  const what = mine ? `${edSndName(slot) ? esc(edSndName(slot)) + ' · ' : ''}slot ${slot} of the character's bank${s && s.seconds ? ' · ' + s.seconds.toFixed(2) + ' s' : ''}`
    : loc ? `slot ${loc.slot} of ${esc(loc.name)}${ISO.index ? '' : ' (set the game ISO to hear it)'}` : 'in no bank the game has: it plays nothing';
  return `<div class="sec" id="edSnd"><h3>Sound <small class="mono">cue ${cue}</small></h3>
    <p class="note">${what}.</p>
    <div class="btnrow"><button type="button" class="mini" id="edSndPlay"${mine || (loc && ISO.index) ? '' : ' disabled'}><i class="ic ic-play"></i> Play</button><button type="button" class="mini" id="edSndBrowse">Browse sounds…</button></div></div>`;
}
function edSndWire(p, kind, idx) {
  const pl = $('edSndPlay'); if (pl) pl.onclick = () => psndPlay(F(p)[kind][idx].sound, true);
}

/* ====================================================== hooks into app.js */
function edEventHTML(kind, idx, e) {
  let h = '';
  if (e.hit !== undefined) h += edHitMoreHTML(hitOf(e.hit));
  if (e.group !== undefined) h += edVolHTML(e);
  if (e.projectile !== undefined) h += edProjHTML(e.projectile, 'fired here');
  if (opCode(e.op) === 0x0303 && e.sound !== undefined) h += edSndHTML(e);
  else h += edCueHTML(e);
  h += edFxJumpHTML(e);
  return h;
}
/* a voice or conditional sound: play it on its own (the character's bank, or the game's from the ISO) */
function edCueOf(e) { if (typeof psndCue === 'function' && opCode(e.op) === 0x060F) return psndCue(e); const v = typeof e.voice === 'number' ? e.voice : typeof e.sound === 'number' ? e.sound : null; return v != null && v >= 0 && v !== 0xFFFF ? v : null; }
function edCueHTML(e) {
  const cue = edCueOf(e); if (cue == null) return '';
  const b = edBank(), base = b ? SndBank.cueBase(b) : -1, mine = b && cue - base >= 0 && cue - base < b.slots.length;
  const loc = !mine && typeof SharedSnd !== 'undefined' ? SharedSnd.cueLocation(cue) : null, can = mine || (loc && ISO.index);
  const foot = opCode(e.op) === 0x060F;
  return `<div class="sec"><h3>${e.voice !== undefined ? 'Voice' : 'Sound'} <small class="mono">cue ${cue}</small></h3>
    ${foot ? `<p class="note">In game the floor under the fighter picks the sound (each stage material has its own bank, 7 kinds × 4 footwear); the preview plays it on stone.</p>` : ''}
    <p class="note">${mine ? `Slot ${cue - base} of the character's bank.` : loc ? `Slot ${loc.slot} of ${esc(loc.name)}${ISO.index ? '' : ' (set the game ISO to play it)'}.` : 'In no bank the game has: it plays nothing.'}</p>
    <div class="btnrow"><button type="button" id="edCuePlay" class="mini"${can ? '' : ' disabled'}>▶ Play</button>${e.sound !== undefined && opCode(e.op) !== 0x060F ? '<button type="button" class="mini" id="edSndBrowse">Browse sounds…</button>' : ''}</div></div>`;
}
function edCueWire(e) {
  const btn = $('edCuePlay'), cue = edCueOf(e); if (!btn || cue == null) return;
  const b = edBank(), base = b ? SndBank.cueBase(b) : -1;
  btn.onclick = () => (b && cue - base >= 0 && cue - base < b.slots.length ? edPlay(cue - base) : shPlay(cue));
}
/* an effect event: open the effect it plays in the effect editor */
function edFxJumpHTML(e) {
  if (!S.fx) return '';
  const ids = ['effect', 'alt_effect'].map((k) => (e[k] && typeof e[k] === 'object' ? e[k].f : e[k])).filter((v) => Number.isInteger(v) && v >= 0 && v !== 0xFFFF);
  const nis = [...new Set(ids.flatMap((id) => (S.fx.byId.get(id) || []).map((r) => r.effect)))].filter((ni) => S.fx.fep.names[ni]);
  if (!nis.length) return '';
  return `<div class="btnrow" style="margin:6px 0 2px">${nis.map((ni) => `<button type="button" class="mini" data-fxjump="${ni}" title="Edit this effect: its pieces, textures and timing">Edit effect ${esc(fxuShort(S.fx.fep.names[ni].name))} ›</button>`).join('')}</div>`;
}
function edFxJumpWire() {
  document.querySelectorAll('[data-fxjump]').forEach((b) => b.onclick = () => { S.tab = 'fx'; fxuOpen(+b.dataset.fxjump); });
}
function edEventWire(p, kind, idx) {
  const e = F(p)[kind][idx];
  if (!(opCode(e.op) === 0x0303 && e.sound !== undefined)) edCueWire(e);
  edFxJumpWire();
  if (e.hit !== undefined) edHitMoreWire(hitOf(e.hit));
  if (e.group !== undefined) edVolWire(p, kind, idx);
  if (e.projectile !== undefined) edProjWire(e.projectile, (nid) => { edit([p], () => { F(p)[kind][idx].projectile = nid; }); });
  if (opCode(e.op) === 0x0303 && e.sound !== undefined) edSndWire(p, kind, idx);
  const bb = $('edSndBrowse');
  if (bb && typeof sndBrowse === 'function') bb.onclick = () => sndBrowse(F(p)[kind][idx].sound, edCan() ? (cue) => { edit([p], () => { F(p)[kind][idx].sound = cue; }); renderTimeline(); renderInspector(); } : null);
}
