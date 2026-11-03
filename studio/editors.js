/* Move Studio: editors for the rest of a moveset (loaded after app.js, moves.js, dummy.js):
     - hitboxes: the objx hit-volume section (type 5), per group, drawn on the model;
     - projectiles: projectiles.json records (clone, tune, flags, children), with the runtime's hitact= hook;
     - a press's action record: CPU reach windows, start, follow-up only, homing at start, name; the CPU weights (brat);
     - the rest of a hit's fields: class, contact effect, hit sound, screen shake, EX force, knock-back bone;
     - runtime hooks per animation: warp= / warpreach= / vtrack= / landact= (mod.json runtime_hooks).
   Layouts: docs/archive/hit-contact, projectile-flight, attacker-movement, hit-reaction; tools/charmod_magi.py. */

const edCan = () => !S.readOnly && !(S.rig && S.rig.retail) && !(S.mod && S.mod.kind === 'retail');
const edSel = (name, opts, cur, attrs = '') => `<select ${attrs}>${opts.map(([v, l, note]) => `<option value="${esc(v)}"${String(v) === String(cur) ? ' selected' : ''}${note ? ` data-note="${esc(note)}"` : ''}>${esc(l)}</option>`).join('')}${opts.some(([v]) => String(v) === String(cur)) ? '' : `<option selected value="${esc(cur)}">${esc(cur)}</option>`}</select>`;
// a tip is a Lucide info icon (2026-11-01), not a tab stop: in a label its text describes the field (edLinkLabels)
const edTip = (t) => (t ? `<span class="tip" data-tip="${esc(t)}" role="img" aria-label="${esc(t)}"><i class="ic ic-info" aria-hidden="true"></i></span>` : '');
const edFld = (label, inner, title = '', hint = '') => `<div class="field"><label>${esc(label)}${edTip(title)}</label>${inner}${hint ? `<small class="fhint">${esc(hint)}</small>` : ''}</div>`;
const edOptTip = (tips, cur) => (tips && tips[cur] != null ? tips[cur] : '');
const ED_EV_TIP = {
  start: 'Frame the event switches on (half frames allowed: 1 frame = 2 game ticks). A ranged flag is on for exactly the ticks of its range.',
  end: 'Frame the event switches off.',
  open: 'Stays on until the animation ends, however long that is.',
  frame: 'Frame this one-off event runs on (half frames allowed: 1 frame = 2 game ticks).',
  other: 'Values this event carries that have no name yet. Their meaning is unknown: copy them from a retail move that uses the same event. 1.5f = a float.',
  phase: 'When one animation is shared by several moves, only the move whose motion-map phase matches runs this event. 0 = every move.',
  mode: 'Runs always, only outside EX Mode, or only in EX Mode.' };
const ED_HIT_TIP = {
  name: 'The hit\'s name (15 characters at most). Renaming it updates every event that uses it.',
  power: 'Brave damage per hit (× the attack / defence factor). Retail single hits 3–8, finishers 10–16. Keep it at least 1.',
  rehit: '0 = one hit per window. N = the window can hit the same foe again every N ticks (60 ticks = 1 s), for multi-hits.',
  reaction: 'How the foe reacts: stagger (held in front of you at Forward / Up), knock, blow, blow away, launch, flinch…',
  hp: 'An HP attack: damage = your Bravery, and Power is ignored.',
  rand: 'Damage is random between 1 and Power.',
  kb_forward: 'Stagger / knock / blow: where the foe is moved, in metres in front of you (negative = behind). For flinch-type reactions it is a height instead.',
  kb_up: 'Stagger / knock / blow: the foe\'s target height in metres. For launch / blow-away reactions it is a speed instead.',
  stun: 'Ticks the foe takes to reach that point: the hit-stun (60 ticks = 1 s).',
  follow: 'Ticks the foe keeps re-aiming at your current position, so they travel with you when you dash on.',
  kb_yaw: 'Turns the knock-back point around you (stagger / knock / blow). For launch / blow-away reactions the game reads it as a distance instead.',
  hitstop: 'Orbs scattered on hit (count × value / 10), probably EX Force. Not hit-stop: that is under Impact.' };
const ED_VOL_TIP = {
  kind: 'What the sphere does. 4 hits: the only kind that deals the hit. 6 clash / guard: an attack touching it starts a clash (guards are only this). 5 meets projectiles. 7 a weapon-trail point (looks only). 2 / 3 hurt spheres (upper body / legs). 1 body push. 10 the lock-on point. 0, 11, 15 not understood yet.',
  bone: 'The bone the sphere rides on, or the root (the whole character).',
  off: 'Offset from the bone in metres, in the bone\'s own frame (x along the bone for most limbs).',
  r: 'Radius in metres. Fast swings are swept: extra spheres fill the gap from last tick\'s position.' };
const ED_ARG_TIP = {
  hit: 'The hit profile (hits.json): damage, reaction and knock-back. Its settings are below.',
  group: 'Which hitbox group switches on: its spheres are listed under Hitboxes below.',
  projectile: 'The projectile record (projectiles.json) to fire. Its settings are below.',
  alt_projectile: 'The projectile fired instead after a winning rare-variant roll.',
  effect: 'The effect (FEPA record) to play.', alt_effect: 'The effect played instead after a winning rare-variant roll.',
  lifetime: 'How long the effect lasts, in ticks (60 = 1 s).', alt_lifetime: 'Lifetime of the alternate effect, in ticks.',
  part: 'The model part / bone it acts on.', draw_part: 'The part that must be visible for the sound to play.',
  ticks: 'Ticks (60 = 1 s).', form: 'Form number (Cecil: Dark Knight / Paladin).',
  sound: 'Sound cue to play. Browse sounds below to pick one.', voice: 'Voice cue (ids like 1810xxx). Browse voices below to pick one.',
  positional: '1 = the sound comes from the character\'s position.',
  speed: 'Speed: for a slow-down, 0 = frozen and 1 = normal; for a dash or lunge, metres per tick.',
  percent: 'Foe speed in percent: 0 = frozen, 100 = normal.',
  deg_per_tick: 'Most degrees it turns per tick (60 ticks = 1 s).', max_deg_per_tick: 'Most degrees per tick (180 = snaps at once).',
  backwards: '1 = face away from the direction of travel.',
  only_without_target: '1 = only when you have no lock-on target.',
  only_after_hit: '1 = the window opens only once the move has hit.',
  value: 'The value posted to the battle manager (listener not traced).',
  brightness: 'How dark the stage goes (× its colour).', multiplier: 'Gravity × this (1 = normal; Emperor uses 0.01, Kuja 0.25).',
  off: 'Not understood yet (retail values are copied as-is).',
  face_part: 'Face texture group: 0 / 1 the eyes, 2 the mouth.', pattern: 'Which pattern of that texture group to show.',
  image: 'Caption image from the character\'s pack.', floor_percent: 'How far the foe fades, in percent.',
  extent: 'How far along the bone the weapon trail reaches.',
  slot: 'Slot number: bone effects use 0–15, props and button shots their own slot.', prop: 'Which separate weapon prop (Gilgamesh-style).',
  cpu_flag: 'A flag for the CPU AI.', preset: 'Camera preset (motion-camera records, values not dumped yet).',
  camera_bone: '0 = the camera_include bone, 1 = camera2_include.',
  only_if_alt_off: '1 = only when the alternate costume / voice is off.', only_if_alt_on: '1 = only when it is on.',
  min: 'Rare-variant roll: minimum (not fully traced).', unlock_bit: 'Unlock needed for the rare variant.', one_in: 'The rare variant happens 1 time in N.',
  fade_out: 'Ticks to fade out.', fade_in: 'Ticks to fade in.', fade_back: 'Ticks to fade back.', hold: 'Ticks the flash holds at full.',
  opacity: 'How far it fades (opacity).', duration: 'Ticks the shake lasts.', x_amp: 'Sideways shake strength.', y_amp: 'Vertical shake strength.',
  offset: 'Offset from where the bone meets the floor.', size: 'Size of the dust / splash.', variant: 'Which dust / splash look.',
  record: 'Effect record to put on the bone (9999 = weapon trail, 8888 = other weapon / hand effects).', record_bit: 'Which bit of that record.',
  scale: 'Size of the effect.', arg: 'Passed to the projectile as its event argument (origin 6 uses it as the effect id).', alt_arg: 'Argument for the alternate projectile.',
  angle: 'Angle in degrees.', skip_if_target: '1 = skipped when you have a lock-on target.', after_homing: '1 = only right after the homing dash.',
  modifier: 'Not understood yet.', tick: 'The total motion time (ticks) it plays at.',
  flag: 'Which input / chain flag opens it.', action: 'Action offered as the follow-up when the input matches.',
  even_on_miss: '1 = offered even if the move missed.', mid_action: '1 = the input is accepted mid-action.',
  only_if_reeling: '1 = only while the foe is reeling from a hit.', only_if_not_reeling: '1 = only while the foe is not reeling.',
  skip_if_reeling: '1 = don\'t turn while the foe reels.', even_if_evading: '1 = still turns while the foe evades.',
  r: 'Red amount of the tint.', g: 'Green amount of the tint.', b: 'Blue amount of the tint.', period: 'Ticks per pulse.',
  turn_mult: 'How fast the stick turns you (×).', speed_mult: 'How fast the stick moves you (×).',
  colour: 'Flash colour: white, black or red.', cooldown: 'Ticks before another button shot can fire.', button: 'Which button fires it: ○ or □.',
  outer_radius: 'The foe is pulled from within this distance (m).', inner_radius: 'Pull stops this close (m).',
  pitch: 'Lunge angle in degrees (positive = up / down as the facing is pitched).', yaw: 'Lunge turn in degrees.',
  kind: 'Footstep / landing / impact kind; the floor material picks the bank.',
  count: 'Press count it checks.', if_le: 'Fires when presses ≤ count.', if_eq: 'Fires when presses = count.', if_ge: 'Fires when presses ≥ count.',
  rate: 'How often feathers drop.', chance: 'Chance a feather drops.',
  away: '1 = turn away from the target instead.', swivel: '0 = turn the whole body; 1 = only twist the drawn body toward the target, within limits.',
  mode: 'Turn toward the target, or aim bones.',
  track_ticks: 'Ticks the dash keeps steering toward the foe.', distance: 'Stand-off: the dash aims to stop this far from the foe (m).',
  arrive_radius: 'The dash ends once this close (m).', max_ticks: 'The dash gives up after this many ticks.',
  height: 'Height offset of the dash\'s target point (m).', vspeed: 'Vertical speed of the dash.' };
const ED_ARG_TIP_OP = {
  'forced_hit.hit': 'Hits the foe with this at once: no hitbox, no range check (EX Bursts).',
  'chase_window.hit': 'If this hit blew the foe away, a press during the range starts the Chase.',
  'chain_window_if_hit.hit': 'The chain window opens only if this hit landed.',
  'hit_window_fire.projectile': 'Fired when the hit connects.',
  'pin_physics_group.group': 'The hair / cloth physics group that follows the animation.',
  'face_flight_direction.deg_per_tick': 'Most degrees per tick it turns to face its own velocity.',
  'slow_foe_exburst.speed': 'The foe runs at this × speed (0 = frozen).', 'slow_motion_exburst.speed': 'The whole game runs at this × speed.',
  'lunge.speed': 'Lunge speed in metres per tick, along the facing.', 'ground_slide.speed': 'Slide speed, metres per tick.',
  'pull_foe.speed': 'How fast the foe is pulled in, metres per tick.',
  'ground_homing_dash.speed': 'Dash speed, metres per tick.', 'homing_dash.speed': 'Dash speed, metres per tick.',
  'homing_dash.angle': 'Approach angle in degrees.', 'ground_homing_dash.angle': 'Approach angle in degrees.',
  'button_shot.arg': 'Passed to the projectile as its event argument.', 'summon_effect.part': 'The summon\'s bone it attaches to.',
  'ground_dust.part': 'The bone whose contact with the floor raises the dust.', 'trail_length.part': 'The bone the trail runs along.',
  'wakeup_invincible.ticks': 'Invincible for this many ticks when getting up after a blow-away (ends if you attack).',
  'fade_self_in.ticks': 'Ticks to fade yourself in.', 'fade_foe_in.ticks': 'Ticks to fade the foe back in.', 'fade_foe_out.ticks': 'Ticks to fade the foe out.',
  'dash_cancel.only_after_hit': '1 = the dash-cancel window opens only after a hit.' };
const edArgTip = (op, n) => ED_ARG_TIP_OP[op + '.' + n] || ED_ARG_TIP[n] || '';
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
  let h = `<div class="sec" id="edVol"><h3>Hitboxes · group ${g} <small>${recs.length} sphere${recs.length === 1 ? '' : 's'}</small>${edTip('Spheres on a bone (or the character\'s root), offset in the bone\'s own frame. Only kind 4 deals the hit; kind 6 clashes with attacks and guards. Drawn on the model while this event is selected (red = hits, blue = clash).')}</h3>
    ${users.length > 1 ? `<p class="note">Armed by ${users.length} windows: ${esc([...new Set(users.map((u) => motionName(u.motion)))].join(', '))}.</p>` : ''}`;
  if (users.length > 1 && can) h += scopeSeg('data-dscope-group', S.dscope.group, users.length, `The first change copies group ${g} for this window (a new group); the others keep theirs`, `Group ${g} itself, so every window that arms it`);
  h += `<div class="edvol">${recs.length ? `<div class="edvol-row edvol-head"><span>Kind${edTip(ED_VOL_TIP.kind)}</span><span>Bone${edTip(ED_VOL_TIP.bone)}</span><span class="edvol-xyz">x · y · z (m)${edTip(ED_VOL_TIP.off)}</span><span>Radius${edTip(ED_VOL_TIP.r)}</span>${can ? '<span class="edvol-del"></span>' : ''}</div>` : ''}${recs.map((r) => `<div class="edvol-row" data-i="${r.i}">
      ${edSel('kind', edNoNum(ED_KINDS), r.kind, `data-k="kind" title="kind ${r.kind}"${ro}`)}
      ${edSel('bone', edNoNum(edBones()), r.bone, `data-k="bone" title="bone ${r.bone}"${ro}`)}
      <span class="edvol-xyz">${r.off.map((x, k) => `<input type="number" step="0.05" value="${fmt3(x)}" data-k="o${k}" title="${'xyz'[k]} (m)"${ro}>`).join('')}</span>
      <input type="number" step="0.05" min="0" value="${fmt3(r.r)}" data-k="r" title="radius (m)"${ro}>
      ${can ? '<button type="button" class="mini danger edvol-del" data-k="del" title="Delete this sphere">×</button>' : ''}</div>`).join('')}</div>
    ${can ? '<button type="button" id="edVolAdd">+ Add sphere</button>' : ''}</div>`;
  return h;
}
function edVolWire(p, kind, idx) {
  const sec = $('edVol'); if (!sec || !edCan()) return;
  const e = F(p)[kind][idx], g = e.group;
  dscopeWire('group');
  // fn(v, at, grp): change the spheres of the group this window arms; at(i) = where sphere i of group g is now. With
  // "This event" and a group other windows arm too, the group is first copied to a new one (the highest + 1) and this
  // window pointed at it, in one undo step
  const volEdit = (fn) => {
    if (!dscopeShared('group', edGroupUsers(g).length)) { edVolEdit((v) => fn(v, (i) => i, g)); return; }
    const vp = edVolPath(), ng = Math.max(...edVolParse(S.files[vp].bin).recs.map((r) => r.group)) + 1;
    edit([vp, p], () => {
      const v = edVolParse(S.files[vp].bin), mine = v.recs.map((r, k) => (r.group === g ? k : -1)).filter((k) => k >= 0), at0 = v.recs.length;
      for (const k of mine) v.recs.push({ ...v.recs[k], off: v.recs[k].off.slice(), group: ng });
      fn(v, (i) => at0 + mine.indexOf(i), ng);
      S.files[vp].bin = edVolBytes(v); F(p)[kind][idx].group = ng;
    });
    if (typeof DM !== 'undefined') { DM.group = null; DM.key = ''; }
    toast(`This window now has group ${ng} (a copy of group ${g}); the other windows keep group ${g}.`);
  };
  sec.querySelectorAll('.edvol-row').forEach((row) => {
    const i = +row.dataset.i;
    row.querySelectorAll('[data-k]').forEach((el) => {
      const k = el.dataset.k;
      if (k === 'del') { el.onclick = () => { volEdit((v, at) => v.recs.splice(at(i), 1)); renderTimeline(); renderInspector(); pose(); }; return; }
      el.onchange = () => {
        const val = parseFloat(el.value);
        volEdit((v, at) => {
          const r = v.recs[at(i)];
          if (k === 'kind') r.kind = +el.value; else if (k === 'bone') r.bone = +el.value;
          else if (k === 'r') { r.r = r.r2 = Math.max(0, val || 0); } else r.off[+k[1]] = isNaN(val) ? 0 : val;
        });
        renderTimeline(); renderInspector(); pose();
      };
    });
  });
  $('edVolAdd').onclick = () => {
    volEdit((v, at_, grp) => {
      let at = -1; v.recs.forEach((r, k) => { if (r.group === grp) at = k; });
      const src = at >= 0 ? v.recs[at] : null;
      const r = src ? { ...src, off: src.off.slice() } : { group: grp, kind: 4, b5: 3, bone: 0x4000, off: [0, 1, 1], y: 0, r: 0.8, r2: 0.8 };
      if (at >= 0) v.recs.splice(at + 1, 0, r); else v.recs.push(r);
    });
    renderTimeline(); renderInspector(); pose();
  };
}
/* drawing, with View → Hitboxes on: the hit volumes on the model at the playhead. The groups of the hit windows live
   now (attack spheres red, clash / guard blue, the rest amber), the selected window's group even when it isn't live
   (fainter), and the character's own hurt spheres (group 0, kinds 2 and 3, green). With Dummy on the simulation
   draws the live attack spheres itself (swept back to last tick, where it has him), so here only the selected group
   and the hurt spheres. Retail characters have no edited section: their volumes are read once, as the dummy reads them. */
const ED_VIS = { g: null, tried: null };
const hbOn = () => { const cb = $('hbOn'); return !cb || cb.checked; };
function edVolRecs() {                         // [{group, kind, bone, off, r}]
  const p = edVolPath(); if (p) return edVolParse(S.files[p].bin).recs;
  if (typeof DM === 'undefined') return [];
  if (!DM.group) {
    if (S.rig && ED_VIS.tried !== S.rig) { ED_VIS.tried = S.rig; dmLoadVolumes().then(() => { if (DM.group) pose(); }); }
    return [];
  }
  const out = []; for (const [g, vs] of DM.group) for (const v of vs) out.push({ group: g, ...v }); return out;
}
function edVolDraw() {
  if (!ED_VIS.g) { ED_VIS.g = new THREE.Group(); view.scene.add(ED_VIS.g); }
  ED_VIS.g.children.forEach((m) => { m.geometry.dispose(); m.material.dispose(); });
  ED_VIS.g.clear();
  if (!hbOn() || !S.gmo || !view.root || S.fxEdit) return;
  const t = curTL(), sel = S.sel && t && t[S.sel.kind] && t[S.sel.kind][S.sel.idx];
  const selG = sel && sel.group != null ? sel.group : null, tick = ticks(S.frame), live = new Set();
  if (t && !(typeof DM !== 'undefined' && DM.on))
    for (const e of t.ranged) if (e.group != null && e.start <= tick && (e.end === FOREVER || tick < e.end) && (typeof evMode !== 'function' || evMode(e))) live.add(e.group);
  const recs = edVolRecs(); if (!recs.length) return;
  view.root.updateMatrixWorld(true);
  for (const r of recs) {
    const hurt = r.group === 0 && (r.kind === 2 || r.kind === 3), on = live.has(r.group), picked = r.group === selG;
    if (!hurt && !on && !picked) continue;
    const M = r.bone === 0x4000 ? view.root.matrixWorld : view.bones[r.bone] && view.bones[r.bone].matrixWorld; if (!M) continue;
    const col = hurt ? 0x6fd39b : r.kind === 4 ? 0xff6f7f : r.kind === 6 ? 0x8fd3ff : 0xe8ac52;
    const op = hurt ? 0.3 : r.kind === 4 ? (on ? 0.7 : 0.4) : (on ? 0.45 : 0.3);
    // clash / guard and the other kinds: an outline ring facing the camera, so a big one doesn't hide the character
    // (2026-11-01: BR_L_RUSH3's clash sphere covered everything); attack and hurt spheres stay wire spheres
    const R = Math.max(0.02, r.r), ring = !hurt && r.kind !== 4;
    const m = ring ? new THREE.Mesh(new THREE.RingGeometry(R - Math.min(0.02, R * 0.1), R, 48), new THREE.MeshBasicMaterial({ color: col, transparent: true, opacity: Math.min(1, op + 0.3), depthWrite: false, side: THREE.DoubleSide }))
      : new THREE.Mesh(new THREE.SphereGeometry(R, 16, 10), new THREE.MeshBasicMaterial({ color: col, wireframe: true, transparent: true, opacity: op, depthWrite: false }));
    if (ring) m.onBeforeRender = (rr, sc, cam) => { m.quaternion.copy(cam.quaternion); m.updateMatrixWorld(); };
    m.position.copy(new THREE.Vector3(...r.off).applyMatrix4(M)); ED_VIS.g.add(m);
  }
}
{ // View → Hitboxes, remembered per browser
  const cb = $('hbOn');
  if (cb) {
    try { if (localStorage.getItem('studio.hitboxes') === '0') cb.checked = false; } catch (e) { /* defaults */ }
    cb.onchange = () => { try { localStorage.setItem('studio.hitboxes', cb.checked ? '1' : '0'); } catch (e) { /* per-viewer only */ } pose(); };
  }
}

/* ====================================================== projectiles (projectiles.json) */
const ED_FLAGS = ['PIERCE', 'NO_TERRAIN', 'GROUND_HUG', 'FX_FOLLOW_DIR', 'UNK_10', 'DEATH_EMIT', 'FX_ALIGN_FLOOR', 'NO_EFFECT2', 'NO_EFFECT', 'NO_VISUALS', 'UNK_400', 'BOUNCE', 'SWEEP_ALT', 'HOMING_XZ', 'HOMING_Y',
  'CLASH_ATTACKER', 'CLASH_VICTIM', 'INHERIT_SIZE', 'CANCEL_ON_DAMAGE', 'STOP_ON_FAIL', 'CANCEL_ON_ACTION', 'LIMIT_NEWEST', 'PULL_FIELD', 'PULL_COND', 'GIVE_EFFECT', 'COLL_MASK_ALT', 'NO_IMPACT_FX', 'HOMING_STOP', 'SPAWN_LOS', 'CANCEL_COND1', 'CANCEL_COND2', 'UNK_80000000'];
const ED_FLAG_TIP = { PIERCE: 'survives hitting a character', NO_TERRAIN: 'passes through walls and floor', GROUND_HUG: 'stays on the floor', DEATH_EMIT: 'spawns its Spawn projectile when it ends instead of as it launches', BOUNCE: 'bounces off terrain', HOMING_XZ: 'homes only horizontally', HOMING_Y: 'homes only vertically', INHERIT_SIZE: 'size from the parent shot', CANCEL_ON_DAMAGE: 'stops if the owner is hit', STOP_ON_FAIL: 'emitter stops when a shot fails to spawn', CANCEL_ON_ACTION: 'stops when the owner starts another action', PULL_FIELD: 'drags the foe (pull values)', HOMING_STOP: 'stops dead when homing reaches its stop distance', NO_EFFECT: 'no primary visual', NO_EFFECT2: 'no secondary visual' };
// (short: the option texts fit a half-width field; the Origin / Direction tips say the rest)
const ED_ORIGIN = [[0, '0 · owner'], [1, '1 · owner bone'], [2, '2 · bone, tilted'], [3, '3 · target'], [4, '4 · target centre'], [5, '5 · parent shot'], [6, '6 · owner effect'], [7, '7 · world origin'], [8, '8 · 2nd target'], [9, '9 · 2nd target centre'], [10, '10 · summon'], [11, '11 · lock / 2nd target'], [12, '12 · lock / 2nd centre'], [13, '13 · foe centre']];
const ED_DIR = [[0, '0 · as the origin'], [1, '1 · owner facing'], [2, '2 · owner root Z'], [3, '3 · parent direction'], [4, '4 · at the target']];
const ED_MOTION = [[0, '0 · straight'], [1, '1 · homing'], [2, '2 · parked (no motion)'], [3, '3 · glued to the origin']];
const ED_TARGET = [[0, '0 · lock-on target'], [1, '1 · owner'], [2, '2 · second target'], [3, '3 · lock, else second'], [4, '4 · opponent'], [5, '5 · none (never homes)']];
const ED_PTIP = { speed: 'm per tick', accel: 'speed change per tick', turn_rate: 'degrees per tick', homing_time: '0 = until the stop distance', homing_stop: 'homing ends this close to the target (m)', move_time: 'moves only while younger than this (0 = always)', lifetime: 'ticks', hit_delay: 'the hitbox appears at this age', hit_time: 'the hitbox lasts this long', size: 'scales the hit radius / length', launch_pitch: 'degrees; positive points down', shots: 'shots the emitter fires', interval: 'ticks between shots', aim_height: 'm above the target point' };
Object.assign(ED_PTIP, {
  hit_delay: 'the hitbox appears at this age (ticks; birth = 0)', hit_time: 'the hitbox lasts this many ticks, then is gone for good',
  lifetime: 'the shot dies at this age (ticks, 60 = 1 s)', size: 'multiplies the hit radius and column height (and the look)',
  size_rate: 'added to size every tick once size start is reached', size_start: 'age (ticks) when size starts changing',
  size_min: 'size never shrinks below this', size_max: 'size never grows past this',
  speed: 'm per tick at birth (60 ticks = 1 s)', accel: 'added to speed each tick (after the move)', accel_start: 'age when accel kicks in',
  speed_min: 'speed is clamped to at least this', speed_max: 'speed is clamped to at most this; 0 stops the shot after one tick',
  gravity: 'downward pull; the drop per tick grows by half this each tick', gravity_start: 'age when gravity kicks in',
  move_time: 'moves only while younger than this (0 = always)',
  turn_rate: 'homing: at most this many degrees per tick toward the target (3D)', homing_start: 'age when homing begins',
  homing_time: 'homing lasts this many ticks (0 = until the stop distance)', homing_stop: 'homing ends for good this close to the target (m); 0 / −1 = never',
  aim_dist: 'target point moved this far along the target\'s facing (m)', aim_yaw: 'turns that aim offset around the target (degrees)',
  aim_height: 'target point raised this much (m)',
  shots: 'how many shots this emitter fires', first_delay: 'ticks before the first shot', interval: 'ticks between shots (0 = all at once)',
  fwd_dist: 'spawn this far out from the origin (m), in the fwd yaw direction', fwd_yaw: 'direction of the forward offset (degrees; + turns toward the origin\'s local X)',
  off_x: 'spawn offset sideways from the origin (m)', off_y: 'spawn offset up from the origin (m)', off_z: 'spawn offset ahead of the origin (m)',
  launch_pitch: 'degrees; positive points down, negative rises (ignored with direction 4)', launch_yaw: 'turns the launch direction left / right (degrees; ignored with direction 4)',
  tgt_dist_min: 'for target origins: spawn point at least this far from the owner horizontally (m)', tgt_dist_max: 'for target origins: at most this far horizontally (m)',
  tgt_height_min: 'for target origins: at least this much height difference from the owner (m)', tgt_height_max: 'for target origins: at most this much height difference (m)',
  emit_count: 'how many it spawns while it flies', emit_delay: 'age of the first one spawned in flight', emit_interval: 'time between the ones spawned in flight',
  pull_start: 'age the pull begins (start and time both 0 = whole life)', pull_time: 'ticks the pull lasts',
  pull_strength: 'm per tick the foe is dragged toward the shot (needs PULL_FIELD)', pull_full_dist: 'full strength from this distance out',
  pull_min_dist: 'no pull closer than this (m); fades to 0 here', pull_max_dist: 'no pull farther than this (m)' });
const ED_PROJ_TIP = {
  atkp: 'The hit this shot deals on contact (damage, reaction, Bravery or HP). none = no hitbox at all: the shot is only a visual.',
  atkp_reflected: 'The hit used after a foe\'s guard reflects the shot back at you. none = a reflected shot can\'t hurt anyone.',
  effect: 'An effect group: the placements tagged with it play as the shot is born (birth) and fly with it (body).',
  common_effect: 'A shared-library effect drawn as well. none = reuse the first look.',
  kind: 'What a guarding foe does to the shot.',
  hit_radius: 'Radius of the hitbox in metres, multiplied by the shot\'s size every tick. Contact is swept from last tick\'s position, so fast shots don\'t skip through.',
  hit_length: '0 = a sphere centred on the shot. Otherwise an upright column this tall standing on the shot\'s point (not along its flight), × size.',
  motion: 'How the shot moves each tick.',
  target: 'Whose body the shot aims at: homing steers toward it, Direction 4 launches at it, and target origins spawn near it. The Homing group\'s aim values shift the point.',
  origin: 'Where each shot spawns. The Spawn group\'s offsets are added in this point\'s frame (off z = ahead, off y = up, off x = sideways).',
  origin_arg: 'What Starts at points to: a bone number for 1 / 2, an effect (FEPA) id for 6, a summon slot for 10. Ignored by the other origins.',
  direction: 'The way the shot launches. Launch pitch / yaw (Spawn group) then turn it, except with 4.',
  max_alive: 'Most of this projectile alive at once; firing another removes the oldest (with LIMIT_NEWEST, the newest). 0 = no limit.',
  link_emit: 'Another projectile this one spawns: once as it launches, a number of times while it flies, or once when it ends. One projectile and one timing only (the game has a single slot). It starts from this shot\'s position, direction and size.',
  link_hit: 'Fired where the shot touches a character (also with PIERCE). Without PIERCE the shot then dies.',
  link_terrain: 'Fired when the shot hits a wall or floor; the shot then dies (BOUNCE: bounces instead). Never with NO_TERRAIN.',
  hitact: 'When this projectile spawns, the owner starts this action (runtime hitact=). Noctis uses it for Alterna: the grip shot starts the follow-up. Needs the native build.' };
const ED_OPT_TIPS = {
  kind: { 0: 'A guard bounces it back at the shooter; it then uses Hit after a reflect.', 1: 'A guard can\'t reflect it; the guard destroys it.', 2: 'A guard can\'t reflect it.', 3: 'A guard can\'t reflect it.', 4: 'An attached object: not reflectable and ignores walls and floor.' },
  motion: { 0: 'Flies along its launch direction; speed, accel and gravity still apply.', 1: 'Steers toward Homes on by up to turn rate° per tick (Homing group), then flies on.', 2: 'Never moves: for fields, pulls and effect holders.', 3: 'Stays glued to its origin (bone or point) every tick.' },
  target: { 0: 'The foe you\'re locked on to.', 1: 'Yourself.', 2: 'The second target.', 3: 'The lock-on target, else the second target.', 4: 'Your opponent.', 5: 'Never aims or homes; dies if its stored target dies.' },
  origin: { 0: 'Your feet, facing your way.', 1: 'A bone (Origin bone / id), aimed along the bone; up stays world up.', 2: 'A bone, and the frame tilts with it.', 3: 'At the target\'s feet (clamped by tgt dist / height); no lock = in front of you.', 4: 'At the target\'s body centre (clamped the same way).', 5: 'Where the parent shot was when it fired this one.', 6: 'On one of your playing effects (id in Origin bone / id); effect not playing = no shot.', 7: 'The arena\'s world origin.', 8: 'At the second target\'s feet.', 9: 'At the second target\'s centre.', 10: 'At your summon (slot in Origin bone / id); no summon = no shot.', 11: 'At the lock-on (else second) target\'s feet.', 12: 'At the lock-on (else second) target\'s centre.', 13: 'At the opponent\'s centre (not clamped).' },
  direction: { 0: 'Keeps the origin\'s forward.', 1: 'Your facing.', 2: 'Your root\'s Z axis (follows a tilted body).', 3: 'The parent shot\'s direction.', 4: 'Straight at the target point at spawn; launch pitch / yaw are ignored.' } };
const edProjFile = () => F(msPath('projectiles.json'));
const edProj = (id) => (edProjFile() ? edProjFile().records.find((r) => r.id === id) : null);
const edFmt = (x) => (Math.abs(x) >= 1e4 ? x.toExponential(2) : String(fmt3(x)));   // 0.9, not 0.899999976
// "forever" values (9999 ticks / m): shown as words, an empty box puts the value back
const ED_PSENT = { hit_time: 'until it ends', lifetime: 'no limit', tgt_dist_max: 'no limit', tgt_height_max: 'no limit' };
const edNoNum = (opts) => opts.map(([v, l]) => [v, String(l).replace(/^\d+ · /, '')]);   // option text without its number (the select's title has it)
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
  for (const x of S.effects.values()) out.push([x.id, x.name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, ''), 'effect ' + x.id]);
  return out;
}
/* ---- the projectile as compact data rows (2026-10-31: "Redo all of this to use simple inputs that I can tweak
   data for"): every field of the record, one row each (label, input(s), unit), in folding groups; the label's tooltip
   says what it does. A parameter takes a value, a random range 2..4, and +0.5 added for each later shot. Times show in
   seconds and speeds per second; the game counts ticks (60 = 1 s). */
const ED_TPS = 60;
const edPV = (r, n) => r.values[n][0];                                  // a parameter's (first) value
const edPFixed = (r, n) => r.values[n][0] === r.values[n][1] && !r.values[n][2];
const edPFlags = (r) => parseInt(r.flags, 16) >>> 0;
const ED_PF = { PIERCE: 0x1, NO_TERRAIN: 0x2, DEATH_EMIT: 0x20, PULL_FIELD: 0x400000 };
// how a parameter is shown: [unit, scale] (the game's value × scale = the value in the box)
const ED_PU = {};
for (const n of ['lifetime', 'hit_delay', 'hit_time', 'accel_start', 'gravity_start', 'move_time', 'size_start', 'homing_start', 'homing_time',
  'first_delay', 'interval', 'emit_delay', 'emit_interval', 'pull_start', 'pull_time']) ED_PU[n] = ['s', 1 / ED_TPS];
for (const n of ['speed', 'speed_min', 'speed_max', 'pull_strength']) ED_PU[n] = ['m/s', ED_TPS];
ED_PU.turn_rate = ['°/s', ED_TPS]; ED_PU.accel = ['m/tick²', 1]; ED_PU.gravity = ['/tick²', 1]; ED_PU.size_rate = ['/tick', 1];
for (const n of ['off_x', 'off_y', 'off_z', 'fwd_dist', 'aim_dist', 'aim_height', 'homing_stop', 'tgt_dist_min', 'tgt_dist_max',
  'tgt_height_min', 'tgt_height_max', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist']) ED_PU[n] = ['m', 1];
for (const n of ['launch_pitch', 'launch_yaw', 'fwd_yaw', 'aim_yaw']) ED_PU[n] = ['°', 1];
for (const n of ['size', 'size_min', 'size_max']) ED_PU[n] = ['×', 1];
const edPTip = (n) => {
  const [u, k] = ED_PU[n] || ['', 1];
  return (ED_PTIP[n] || n.replace(/_/g, ' ')) + (k === 1 / ED_TPS ? ' · shown in seconds' : k === ED_TPS ? ' · shown per second' : '');
};
const ED_KIND = [[0, '0 · reflectable'], [1, '1 · guard breaks it'], [2, '2 · not reflectable'], [3, '3 · not reflectable'], [4, '4 · attached object']];
const ED_PG_OPEN = { start: 1, aim: 1, flight: 1, hit: 1, spawns: 1 };  // open until you fold them (remembered per browser)
const ED_SPAWN_WHEN = [['launch', 'as it launches'], ['flight', 'while it flies'], ['end', 'when it ends']];
const edProjSpawnWhen = (r) => (edPFlags(r) & ED_PF.DEATH_EMIT ? 'end' : edPV(r, 'emit_count') > 0 ? 'flight' : 'launch');
function edProjRange(r) {                                                // metres flown in its life, by speed / accel / clamps
  let s = edPV(r, 'speed'), d = 0;
  const life = edPV(r, 'lifetime'), mt = edPV(r, 'move_time'), a = edPV(r, 'accel'), a0 = edPV(r, 'accel_start');
  const lo = edPV(r, 'speed_min'), hi = edPV(r, 'speed_max');
  for (let t = 0; t < Math.min(life, 9999); t++) {
    if (mt > 0 && t > mt) break;
    d += s;
    if (t >= a0) s += a;
    if (hi > 0 || lo > 0) s = Math.min(Math.max(s, lo), Math.max(hi, lo));
  }
  return d;
}
function edProjChildName(id, noId) {
  const c = edProj(id); if (!c) return `${id}`;
  const h = S.hitById && S.hitById.get(c.atkp);
  return `${noId ? '' : id + ' · '}${c.atkp === 65535 || !h ? 'looks only, no hit' : (h.name || h.id) + (parseInt(h.flags, 16) & 2 ? ' (HP)' : '')}`;
}
function edProjHTML(id, label) {
  const r = edProj(id), can = edCan() && !!r, ro = can ? '' : ' disabled';
  if (!r) return `<div class="sec"><h3>Projectile ${esc(String(id))}</h3><p class="note">${edProjFile() ? 'Not in projectiles.json.' : 'Projectiles of retail characters are shown, not edited here.'}</p></div>`;
  const recs = edProjFile().records, f = edPFlags(r), users = edProjUsers(id), seen = new Set();
  const kids = [[-1, 'none'], ...recs.filter((x) => x.id !== r.id).map((x) => [x.id, edProjChildName(x.id, true), 'projectile ' + x.id])];
  const hits = [[65535, 'none (no hitbox)'], ...[...S.hitById.values()].map((h) => [parseInt(h.id, 16), `${h.name || h.id}${parseInt(h.flags, 16) & 2 ? ' (HP)' : ''}`])];
  const H = typeof dmHooks === 'function' ? dmHooks() : { hitact: null }, act = H.hitact && H.hitact.magi === id ? H.hitact.action : '';
  let open = {}; try { open = JSON.parse(localStorage.getItem('studio.projOpen') || '{}'); } catch (e) { /* defaults */ }
  const isOpen = (k, dflt) => (k in open ? !!open[k] : !!dflt);
  // a row: label (its tooltip says what it does), the input(s), the unit
  // a field as the event's own fields are (2026-11-01: "use the exact same styling for these inputs than we do for
  // the others"): the label above with its unit and a ? tip, the input(s) full width below
  // two fields to a row, as the hit section (2026-11-01: the values were cut off); a field with two or three
  // inputs (offset x · y · z, min · max) takes the whole row
  const row = (text, tip, inputs, unit = '', wide = false) => `<div class="field pfield${wide ? ' wide' : ''}"><label>${esc(text)}${unit ? ` (${esc(unit)})` : ''}${edTip(tip)}</label><div class="pin">${inputs}</div></div>`;
  const ph = (n) => ({ off_x: 'x', off_y: 'y', off_z: 'z', launch_pitch: 'pitch', launch_yaw: 'yaw' })[n] || (/_min$/.test(n) ? 'min' : /_max$/.test(n) ? 'max' : '');
  const pIn = (n) => {
    seen.add(n);
    const [, k] = ED_PU[n] || ['', 1], [lo, hi, st] = r.values[n], v = (x) => edFmt(x * k);
    if (ED_PSENT[n] && lo === hi && !st && lo >= 9999) return `<input type="text" inputmode="decimal" value="" placeholder="${esc(ED_PSENT[n])}" data-p="${n}" data-sent="1" title="${esc(n)}: ${lo}"${ro}>`;
    return `<input type="text" inputmode="decimal" value="${lo === hi ? v(lo) : `${v(lo)}..${v(hi)}`}${st ? ' +' + v(st) : ''}" placeholder="${esc(ph(n))}" data-p="${n}" title="${esc(n)}"${ro}>`;
  };
  const p = (text, ...names) => {
    names = names.filter((n) => r.values[n]); if (!names.length) return '';
    return row(text, names.map((n) => (names.length > 1 ? `${n.replace(/_/g, ' ')}: ` : '') + edPTip(n)).join(' · '), names.map(pIn).join(''), (ED_PU[names[0]] || [''])[0], names.length > 1);
  };
  const tipOf = (fld, tips) => (ED_PROJ_TIP[fld] || '') + (tips && tips[r[fld]] ? ' ' + tips[r[fld]] : '');
  const sel = (text, fld, opts, tips, wide) => row(text, tipOf(fld, tips), edSel(fld, opts, r[fld], `data-f="${fld}" title="${esc(fld.replace(/_/g, ' ') + ' ' + r[fld])}"${ro}`), '', wide);
  const num = (text, fld, unit, extra = '') => row(text, ED_PROJ_TIP[fld], edNum(fmt3(+r[fld]), `data-f="${fld}"${extra}${ro}`), unit);   // (0.9, not 0.8999999761581421)
  const flag = (name, text) => { const b = ED_FLAGS.indexOf(name); return `<label class="chk" title="${esc(name + (ED_FLAG_TIP[name] ? ': ' + ED_FLAG_TIP[name] : ''))}"><input type="checkbox" data-flag="${b}"${f & (1 << b) ? ' checked' : ''}${ro}> ${esc(text)}</label>`; };
  const grp = (k, title, body, dflt, titleHTML) => body ? `<details class="pgrp" data-pg="${k}"${isOpen(k, dflt) ? ' open' : ''}><summary>${titleHTML || esc(title)}</summary><div class="pgrid">${body}</div></details>` : '';

  let h = `<div class="sec" id="edProj"><h3>Projectile ${id} <small>${esc(label || '')}</small></h3>
    ${users.length > 1 ? `<p class="note">Fired or chained from ${users.length} places: ${esc(users.join(', '))}.</p>` : ''}
    ${can && users.length > 1 ? scopeSeg('data-dscope-proj', S.dscope.proj, users.length, `The first change copies projectile ${id} for this event (a new projectile); the others keep theirs`, `Projectile ${id} itself, so everything that fires it`, users.some((u) => /^projectile /.test(u)) ? 'uses' : 'events') : ''}
    ${can && users.length <= 1 ? `<div class="btnrow"><button type="button" id="edProjCopy">Copy to a new projectile</button></div>` : ''}`;
  let start = sel('Origin', 'origin', edNoNum(ED_ORIGIN), ED_OPT_TIPS.origin);
  if ([1, 2].includes(r.origin)) start += sel('Bone', 'origin_arg', S.boneNames.map((n, i) => [i, n, 'bone ' + i]));
  else if (r.origin === 6 && S.effects.size) start += row('Effect', ED_PROJ_TIP.origin_arg, effectSelect(r.origin_arg, `data-f="origin_arg"${ro}`));
  else if ([6, 10].includes(r.origin)) start += num(r.origin === 6 ? 'Effect id' : 'Summon slot', 'origin_arg', '', ' min="0"');
  start += p('Offset', 'off_x', 'off_y', 'off_z');
  h += grp('start', 'Start', start, ED_PG_OPEN.start);
  h += grp('aim', 'Direction', sel('Direction', 'direction', edNoNum(ED_DIR), ED_OPT_TIPS.direction) + p('Pitch · yaw', 'launch_pitch', 'launch_yaw'), ED_PG_OPEN.aim);
  h += grp('flight', 'Flight', sel('Motion', 'motion', edNoNum(ED_MOTION), ED_OPT_TIPS.motion) + p('Speed', 'speed') + p('Speed min · max', 'speed_min', 'speed_max')
    + p('Accel', 'accel') + p('Accel from', 'accel_start') + p('Gravity', 'gravity') + p('Gravity from', 'gravity_start') + p('Moves for', 'move_time')
    + row('Lifetime', `${edPTip('lifetime')} · reaches about ${+edProjRange(r).toFixed(1)} m`, pIn('lifetime'), 's'), ED_PG_OPEN.flight);
  h += grp('homing', 'Homing', sel('Target', 'target', edNoNum(ED_TARGET), ED_OPT_TIPS.target) + p('Turn rate', 'turn_rate') + p('Starts at', 'homing_start')
    + p('Lasts', 'homing_time') + p('Stops within', 'homing_stop') + p('Aim ahead', 'aim_dist') + p('Aim turn', 'aim_yaw') + p('Aim up', 'aim_height'), r.motion === 1);
  // the effect the hit plays where it lands (2026-11-02: the Apocalypsis arms' leftover Holy Saber shatter came
  // from their hit's contact effect bit and showed nowhere): the hit windows' picker, None strips it
  const hh = S.hitById && S.hitById.get(r.atkp), cf = hh && typeof hitContactFx === 'function' ? hitContactFx(hh) : null;
  const landRow = hh && S.fx ? row('Where it lands', `The effect ${hh.name || hh.id} plays where it hits: the effect placements with its contact effect bit${cf ? ' ' + cf.bit : ''}. Shared by every user of the hit.`,
    `<button type="button" class="apbtn" id="edProjCfx"${ro} aria-haspopup="listbox"><span class="mono">${esc(cf ? fxuShort(cf.fi.name) : 'None')}</span><small>${cf ? 'bit ' + cf.bit : 'no contact effect'}</small><span class="ap-ch" aria-hidden="true">▾</span></button>`, '', true) : '';
  h += grp('hit', 'Hit', sel('Hit', 'atkp', hits, null, true) + landRow + sel('After a reflect', 'atkp_reflected', hits, null, true) + sel('Against a guard', 'kind', edNoNum(ED_KIND), ED_OPT_TIPS.kind)
    + num('Radius', 'hit_radius', 'm', ' min="0"') + num('Column height', 'hit_length', 'm', ' min="0"') + p('Hitbox from', 'hit_delay') + p('Hitbox lasts', 'hit_time')
    + `<div class="pchk">${flag('PIERCE', 'pierce')}${flag('NO_TERRAIN', 'through walls')}${flag('BOUNCE', 'bounce')}</div>`, ED_PG_OPEN.hit);
  h += grp('size', 'Size', p('Size', 'size') + p('Grows', 'size_rate') + p('Grows from', 'size_start') + p('Size min · max', 'size_min', 'size_max'));
  // what it spawns (2026-10-31: "Rather than having children perhaps a 'Spawn on X'"): the one timed slot (link_emit)
  // with its timing as a choice (DEATH_EMIT and emit count underneath), then one slot per kind of contact
  const when = edProjSpawnWhen(r);
  let spawns = row('Spawn', ED_PROJ_TIP.link_emit, edSel('link_emit', kids, r.link_emit, `data-f="link_emit"${ro}`), '', true);
  if (r.link_emit >= 0) spawns += row('… when', 'When it spawns it: once as it launches, a number of times while it flies, or once when it ends.', edSel('spawnwhen', ED_SPAWN_WHEN, when, `data-spawnwhen="1"${ro}`));
  if (r.link_emit >= 0 && when === 'flight') spawns += p('How many', 'emit_count') + p('First after', 'emit_delay') + p('Then every', 'emit_interval');
  spawns += sel('On a foe', 'link_hit', kids, null, true);
  spawns += f & ED_PF.NO_TERRAIN ? row('On a wall or floor', ED_PROJ_TIP.link_terrain, '<span class="pfact">never: it goes through walls (Hit → through walls)</span>', '', true)
    : sel('On a wall or floor', 'link_terrain', kids, null, true);
  for (const [fld, text] of [['link_fall', 'If it loses the floor'], ['link_clash', 'If it is cancelled']]) if (r[fld] >= 0) spawns += sel(text, fld, kids, null, true);
  const odd = ['link_terrain_1', 'link_terrain_2', 'link_terrain_4'].filter((k) => r[k] >= 0);
  if (odd.length) spawns += `<p class="note">Also set: ${odd.map((k) => `${k.replace(/_/g, ' ')} → ${r[k]}`).join(', ')} (per kind of terrain, not understood yet; only in projectiles.json).</p>`;
  h += grp('spawns', 'Spawns', spawns, ED_PG_OPEN.spawns);
  h += grp('spawn', 'Shots', p('Shots', 'shots') + p('First after', 'first_delay') + p('Every', 'interval') + num('Most alive', 'max_alive', '', ' min="0"')
    + p('Forward', 'fwd_dist') + p('Forward turn', 'fwd_yaw') + p('From target min · max', 'tgt_dist_min', 'tgt_dist_max') + p('Height min · max', 'tgt_height_min', 'tgt_height_max'));
  h += grp('pull', 'Pull', `<div class="pchk">${flag('PULL_FIELD', 'drags the foe along')}</div>` + p('Starts at', 'pull_start') + p('Lasts', 'pull_time')
    + p('Strength', 'pull_strength') + p('Full within', 'pull_full_dist') + p('Distance min · max', 'pull_min_dist', 'pull_max_dist'), f & ED_PF.PULL_FIELD);
  h += grp('look', 'Look', sel('Effect group', 'effect', edGroupOpts(r.effect), null, true) + sel('Second look', 'common_effect', edFxOpts(), null, true));
  const rest = Object.keys(r.values).filter((n) => !seen.has(n));             // anything the groups above don't name
  if (rest.length) h += grp('other', 'Other', rest.map((n) => p(n.replace(/_/g, ' '), n)).join(''));
  h += grp('flags', `Flags ${r.flags}`, `<div class="edp-flags">${ED_FLAGS.map((n, b) => `<label class="chk" title="${esc(ED_FLAG_TIP[n] || '')}"><input type="checkbox" data-flag="${b}"${f & (1 << b) ? ' checked' : ''}${ro}> ${n}</label>`).join('')}</div>`, false, `Flags <span class="mono" style="text-transform:none">${esc(r.flags)}</span>`);
  h += grp('native', 'Native build', row('Start action', ED_PROJ_TIP.hitact, `<input type="number" min="0" value="${esc(act)}" placeholder="none" data-hook="hitact"${ro}>`));
  return h + '</div>';
}
function edProjUsers(id) {
  const out = [];
  for (const p of S.tlPaths) { const t = F(p); for (const e of [...t.keyed, ...t.ranged]) if (e.projectile === id) out.push(motionName(t.motion)); }
  for (const r of (edProjFile() || { records: [] }).records) for (const k of ['link_emit', 'link_hit', 'link_terrain', 'link_terrain_1', 'link_terrain_2', 'link_terrain_4', 'link_fall', 'link_clash']) if (r[k] === id) out.push('projectile ' + r.id);
  return out;
}
/* relink: {paths, set(nid), after(nid)}: how this event (or the parent projectile) is pointed at a copy of the projectile */
function edProjWire(id, relink) {
  const sec = $('edProj'); if (!sec) return;
  dscopeWire('proj');
  sec.querySelectorAll('details[data-pg]').forEach((d) => d.addEventListener('toggle', () => {   // remember each group's fold
    let o = {}; try { o = JSON.parse(localStorage.getItem('studio.projOpen') || '{}'); } catch (e) { /* fresh */ }
    o[d.dataset.pg] = d.open; try { localStorage.setItem('studio.projOpen', JSON.stringify(o)); } catch (e) { /* per viewer */ }
  }));
  if (!edCan()) return;
  const pp = msPath('projectiles.json'), r = edProj(id); if (!r) return;
  { const b = $('edProjCfx'), hh = S.hitById && S.hitById.get(r.atkp); if (b && hh && typeof cfxOpen === 'function') b.onclick = () => cfxOpen(b, hh, null, null); }
  const redo = () => { derive(); if (typeof DM !== 'undefined') DM.key = ''; renderTimeline(); renderInspector(); pose(); };   // (the timeline's projectile lanes)
  const copyOf = (j) => { const c = JSON.parse(JSON.stringify(edProj(id))); c.id = Math.max(...j.records.map((x) => x.id)) + 1; return c; };
  // fn(x, j): change the projectile this event fires. With "This event" and a projectile fired or chained from other places
  // too, it is first copied (a new id, the highest + 1) and this event (or the parent shot) pointed at the copy, in one undo step
  const pedit = (fn) => {
    if (relink && dscopeShared('proj', edProjUsers(id).length)) {
      const j = edProjFile(), c = copyOf(j);
      edit([pp, ...relink.paths], () => { j.records.push(c); fn(c, j); relink.set(c.id); });
      if (relink.after) relink.after(c.id);
      toast(`This event now has its own copy, projectile ${c.id}; the others keep projectile ${id}.`);
      return;
    }
    edit([pp], () => fn(edProj(id), edProjFile()));
  };
  sec.querySelectorAll('[data-f]').forEach((el) => el.onchange = () => {
    const fld = el.dataset.f, v = parseFloat(el.value);
    const nv = ['hit_radius', 'hit_length'].includes(fld) ? (isNaN(v) ? 0 : Math.max(0, v)) : (isNaN(v) ? 0 : Math.round(v));
    if (nv === r[fld]) return;
    pedit((x) => { x[fld] = nv; });
    redo();
  });
  sec.querySelectorAll('[data-p]').forEach((el) => el.onchange = () => {
    const n = el.dataset.p, k = (ED_PU[n] || ['', 1])[1], un = (x) => +(x / k).toFixed(6);
    if (!el.value.trim() && ED_PSENT[n]) {          // empty: back to "forever" (unchanged: no edit)
      const x0 = edProj(id), [a0, b0, c0] = x0.values[n];
      if (!(a0 === b0 && !c0 && a0 >= 9999)) { pedit((x, j) => edSetParam(j, x, n, 9999, 9999, 0)); redo(); } else renderInspector();
      return;
    }
    const m = el.value.trim().match(/^(-?[\d.e+-]+)(?:\s*\.\.\s*(-?[\d.e+-]+))?(?:\s*\+\s*(-?[\d.e+-]+))?$/i);
    if (!m) { toast('Write a value, a range like 2..4, or add +0.5 for each later shot.'); renderInspector(); return; }
    const lo = un(parseFloat(m[1])), hi = m[2] != null ? un(parseFloat(m[2])) : lo, st = m[3] != null ? un(parseFloat(m[3])) : 0;
    const [a0, b0, c0] = edProj(id).values[n];
    if (Math.fround(lo) === Math.fround(a0) && Math.fround(hi) === Math.fround(b0) && Math.fround(st) === Math.fround(c0)) { renderInspector(); return; }   // unchanged: no edit, no copy
    try {
      pedit((x, j) => {
        const was = edPV(x, 'speed');
        // speed with no accel: clamps that pinned the old speed move with it (or they'd cut the new one)
        if (n === 'speed' && !edPV(x, 'accel')) for (const c of ['speed_min', 'speed_max']) if (edPFixed(x, c) && edPV(x, c) === was) edSetParam(j, x, c, lo, lo, 0);
        edSetParam(j, x, n, lo, hi, st);
      });
    } catch (e) { toast(e.message); }
    redo();
  });
  sec.querySelectorAll('[data-flag]').forEach((el) => el.onchange = () => {
    pedit((x, j) => {
      let fl = edPFlags(x); const b = (1 << +el.dataset.flag) >>> 0;
      fl = el.checked ? (fl | b) >>> 0 : (fl & ~b) >>> 0;
      x.flags = '0x' + fl.toString(16).padStart(8, '0'); x.flag_names = ED_FLAGS.filter((nm, k) => fl & (1 << k));
      if (b === ED_PF.PULL_FIELD && el.checked && !edPV(x, 'pull_strength')) {   // a drag that keeps a touched foe on the shot
        const P = (nm, v) => edSetParam(j, x, nm, v, v, 0);
        P('pull_start', 0); P('pull_time', edPV(x, 'lifetime')); P('pull_strength', Math.max(0.2, edPV(x, 'speed') * 1.3));
        P('pull_full_dist', 1.5); P('pull_min_dist', 0); P('pull_max_dist', Math.max(1.5, x.hit_radius * 2.4));
      }
    });
    redo();
  });
  const sw = sec.querySelector('[data-spawnwhen]');                      // Spawn's timing: the flag and the in-flight count
  if (sw) sw.onchange = () => {
    pedit((x, j) => {
      const w = sw.value; let fl = edPFlags(x);
      fl = w === 'end' ? (fl | ED_PF.DEATH_EMIT) >>> 0 : (fl & ~ED_PF.DEATH_EMIT) >>> 0;
      x.flags = '0x' + fl.toString(16).padStart(8, '0'); x.flag_names = ED_FLAGS.filter((nm, k) => fl & (1 << k));
      const n = w === 'flight' ? Math.max(1, Math.trunc(edPV(x, 'emit_count'))) : 0;
      edSetParam(j, x, 'emit_count', n, n, 0);
    });
    redo();
  };
  const hk = sec.querySelector('[data-hook="hitact"]');
  if (hk) hk.onchange = () => { const a = parseInt(hk.value, 10); edHookSet('hitact', a >= 0 ? `hitact=${id},${a}` : null, (t) => t.startsWith('hitact=' + id + ',')); redo(); };
  const cp = $('edProjCopy');
  if (cp) cp.onclick = () => {
    const j = edProjFile(), c = copyOf(j), nid = c.id;
    edit([pp, ...(relink ? relink.paths : [])], () => { j.records.push(c); if (relink) relink.set(nid); });
    if (relink && relink.after) relink.after(nid);
    redo(); toast(`New projectile ${nid} (a copy of ${id}).`);
  };
}
/* ====================================================== a press's action record (plya) and the CPU weights (brat) */
const edF32 = (a, o) => { const r = mvRaw(a); return new DataView(r.buffer).getFloat32(o, true); };
function edSetF32(a, o, v) { const r = mvRaw(a), dv = new DataView(r.buffer); dv.setFloat32(o, v, true); a.raw = [...r].map((x) => x.toString(16).padStart(2, '0')).join(''); }
function edSetU32(a, o, v) { const r = mvRaw(a), dv = new DataView(r.buffer); dv.setUint32(o, v >>> 0, true); a.raw = [...r].map((x) => x.toString(16).padStart(2, '0')).join(''); }
const edU32 = (a, o) => new DataView(mvRaw(a).buffer).getUint32(o, true);
function edActName(a) { const r = mvRaw(a); let s = ''; for (let i = 0x64; i < 0x74 && r[i]; i++) s += String.fromCharCode(r[i]); return s; }
const ED_BRAT = [[2, 'Often (0.7)'], [1, 'Sometimes (0.4)'], [0, 'Rarely (0.2)'], [4, 'Almost never (0.01)'], [3, 'Never']];   // brat weight codes (seco table 0x15c)
const ED_SIT = ['HP 1', 'HP 2', 'HP 3', 'Bravery 1', 'Bravery 2', 'When chasing'];
const ED_SIT_TIP = ['One of the three situations the CPU judges by HP. Which HP level each one is has not been decoded yet.', 'One of the three situations the CPU judges by HP. Which HP level each one is has not been decoded yet.', 'One of the three situations the CPU judges by HP. Which HP level each one is has not been decoded yet.', 'One of the two situations the CPU judges by Bravery. Which Bravery level each one is has not been decoded yet.', 'One of the two situations the CPU judges by Bravery. Which Bravery level each one is has not been decoded yet.', 'When the CPU is chasing the foe (after a knock-back), and other special cases.'];
function edPressHTML() {
  // grouped by what each part does (request: "can we lay it out in a better way"): how the press starts, its name banner,
  // then what only the computer opponent reads, then the raw bytes
  const m = S.moves[S.step.move], s = m.steps[S.step.i], a = s && S.actionById.get(s.action), can = edCan();
  if (!a) return '';
  const ro = can ? '' : ' disabled', r = mvRaw(a), fl = edU32(a, 0x18), home = !!(fl & 1);
  const others = S.moves.filter((mm) => mm !== m && mm.steps.some((x) => x.action === a.action));
  const kv = (label, inner, tip) => `<div class="kv"><label>${esc(label)}${edTip(tip || '')}</label>${inner}</div>`;
  const grp = (title, tip, body) => `<div class="pgrp"><h4>${esc(title)}${edTip(tip || '')}</h4>${body}</div>`;
  const f = (o) => edNum(+edF32(a, o).toFixed(3), `data-o="${o}"${ro}`);
  let h = `<div class="sec" id="edPress"><h3>Press settings</h3>
    ${others.length ? `<p class="warnline">${esc(others.map((mm) => mm.name).join(', '))} ${others.length > 1 ? 'use' : 'uses'} this press too: changes here apply there as well.</p>` : ''}`;
  // how it starts
  let start = kv('Can start', edSel('st', [[0, 'anywhere'], [2, 'on the ground'], [1, 'in the air']], r[5], `data-b="5"${ro}`), 'Where the character has to be for the move to begin with this press.')
    + `<label class="chk"><input type="checkbox" data-b="3"${r[3] ? ' checked' : ''}${ro}> Later press only${edTip('Plays only as press 2 or later, never opens a move. A first press must have it off.')}</label>`
    + `<label class="chk"><input type="checkbox" id="edHome"${home ? ' checked' : ''}${ro}> Dashes at the foe as it starts${edTip('For players and the CPU alike.')}</label>`;
  if (home) start += `<div class="row3 pdash">${[['Speed', 0x40], ['Vertical speed', 0x44], ['Track ticks', 0x48], ['Arrive radius', 0x4C], ['Max ticks', 0x50], ['Height', 0x54], ['Angle (rad)', 0x58], ['Stand-off (m)', 0x5C]].map(([l, o]) => edFld(l, f(o))).join('')}</div>`;
  h += grp('Starting', '', start);
  // the name banner
  const tx = edText();
  if (tx) {                                   // plya +0x21 picks a record of the objx 'text' section
    const cur = r[0x21], recs = tx.langs[0], rec = recs.find((x) => x.index === cur);
    const e = mvEntry(m), hpMove = !!e && mvKind(m).type === 'hp' && typeof mvBannerAuto === 'function';
    if (hpMove && e.banner !== 'manual') {       // HP attacks: automatic (moves.js mvSyncBanners)
      const bp = mvBannerPress(m), mine = bp && bp.action === a.action;
      h += grp('Name banner', 'HP attacks show their name when they land: the press that lands the HP hit gets the move\'s name as its banner, kept in step with the name.',
        kv('Banner', `<span class="fact">${mine ? `Automatic: “${esc(rec ? rec.text : mvBannerText(m))}”` : `On press ${bp ? bp.i + 1 : '?'} (the one that lands the HP hit)`}</span>`)
        + (can ? `<label class="chk"><input type="checkbox" id="edBanManual"> Set by hand${edTip('Pick the banner of each press yourself.')}</label>` : ''));
    } else
    h += grp('Name banner', 'The move name that pops up on screen when the press lands, as HP attacks usually have (a record of the moveset\'s text).',
      kv('Banner', edSel('ban', [[255, 'no banner'], ...recs.map((x) => [x.index, `${x.index} · ${x.text}`]), ...(can ? [['new', '+ new record']] : [])], cur, `id="edBan"${ro}`))
      + (rec ? kv('Text (English)', `<input type="text" id="edBanText" value="${esc(rec.text)}" maxlength="24"${can ? '' : ' disabled'}>`, 'Letters, digits, space and ! % & \' + , : - “ ”; other signs are not known in the game font') : '<input type="hidden" id="edBanText">')
      + (rec && S.moves.filter((mm) => mm.steps.some((x) => { const aa = S.actionById.get(x.action); return aa && mvRaw(aa)[0x21] === cur; })).length > 1 ? '<p class="note">Other presses show this banner too.</p>' : '')
      + (hpMove && can ? `<label class="chk"><input type="checkbox" id="edBanManual" checked> Set by hand${edTip('Untick to name it automatically from the move.')}</label>` : ''));
  }
  // the computer opponent: only a move's first press is read (the CPU picks a move by its first action's reach and the
  // command's brat row; retail later presses have zeros there). request: "it doesn't really make sense to have cpu data
  // on a second press right?"
  const first = S.moves.some((mm) => mm.steps[0] && mm.steps[0].action === a.action);
  const rng = (lab, o1, o2, tip) => kv(lab, `<div class="rng">${f(o1)}<span>to</span>${f(o2)}<span>m</span></div>`, tip);
  let cpu = rng('Foe distance', 0x24, 0x28, 'The computer only chooses this move when the foe is this far away.')
    + rng('Foe height', 0x2C, 0x30, 'How far below (negative) and above the character the foe can be.');
  const bp = typeof mvBratPath === 'function' ? mvBratPath() : '';
  if (bp && S.step.i === 0) {                 // the move's brat row: for the whole move, so on its first press only
    const b = mvBratRows(S.files[bp].bin), row = b.rows.find((x) => x.cmd === m.id);
    if (row) cpu += `<div class="psub">How often it picks the move${edTip('For the whole move (the brat row of its command). In each situation the CPU weighs its moves against each other.')}</div>`
      + row.sit.map(([off, n], k) => kv(ED_SIT[k], n === 1 ? edSel('w', ED_BRAT, b.w[off], `data-w="${k}"${ro}`) : `<input disabled value="${n} codes">`, ED_SIT_TIP[k])).join('');
    else cpu += `<p class="note">The CPU never picks this move (no row for command ${esc(m.hexId)}).${can ? ' <button type="button" id="edBratAdd" class="mini">Add a row</button>' : ''}</p>`;
  }
  if (first) h += grp('Computer opponent', 'What the CPU reads to decide when to use this move. Your own presses ignore all of this.', cpu);
  // the record's ids and offsets live here, out of the labels (request: no hex in the inspector)
  h += `<details class="pgrp"><summary class="lbl" style="cursor:pointer">Raw record (116 bytes)</summary><p class="note mono">action ${a.action} · ${esc(edActName(a))}</p><textarea id="edRaw" class="mono" rows="5"${ro}>${esc(a.raw)}</textarea><p class="note">+0 action, +3 later press only, +5 can start, +0x18 bit 0 dashes at the foe (its values +0x40–0x5C), +0x1C script, +0x21 name banner, +0x24 / +0x28 foe distance, +0x2C / +0x30 foe height, +0x64 name.</p></details></div>`;
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
  const bm = $('edBanManual');
  if (bm) bm.onchange = () => { edit([S.modPath], () => { const e = S.mod.move_names[m.info.key]; if (bm.checked) e.banner = 'manual'; else delete e.banner; }); if (!bm.checked) mvSyncBanners(); redo(); };
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
/* the clash classes we know (FUN_08809F00, the 16 x 16 table at 0x089D4978); other values show as "Class N" */
const ED_HIT_CLASS = { 0: 'Melee (a guard blocks it)', 2: 'Guard crush', 3: 'Guard crush (strong)', 4: 'Guard', 7: 'Projectile (a guard stops it)', 11: 'Never clashes' };
/* the rest of a hit's fields, as subgroups of the Hit section (2026-11-01: no "More hit settings" fold): Impact,
   Clashes, Extras. The knock-back bone / launch speed sits under Knock-back (app.js hitHTML calls edHit2CRow). */
function edHitExtraHTML(h) {
  if (!h || h.raw) return '';
  const can = edCan(), ro = can ? '' : ' disabled', sh = h.shake || '00000000', sb = (k) => parseInt(sh.slice(2 * k, 2 * k + 2), 16);
  const snd = parseInt(h.unk_30, 16) >>> 0;
  const cls = Object.entries(ED_HIT_CLASS).map(([v, l]) => [v, l]);
  if (!(h.class in ED_HIT_CLASS)) cls.push([h.class, 'Class ' + h.class]);
  return `<div id="edHit">
    <div class="lbl subhd">Impact</div>
    <div class="row2">${edFld('Hit-stop (ticks)', edNum(sb(0), `data-s="0" min="0" max="255"${ro}`), 'The freeze when the hit lands: the whole game runs at 15 % speed for this many ticks, which gives the hit its weight. 0 = none; retail hits use 6–14.')}${edFld('Hit sound', `<input type="number" value="${snd === 0xFFFFFFFF ? '' : snd}" placeholder="none" data-h="unk_30" data-none="1"${ro}>`, 'A sound cue played on contact, on top of the contact effect\'s own. Empty = none (most hits have none).')}</div>
    <div class="row3">${edFld('Shake (ticks)', edNum(sb(1), `data-s="1" min="0" max="255"${ro}`), 'Screen shake on contact: how long it lasts. 0 = no shake.')}${edFld('Strength', edNum(sb(2), `data-s="2" min="0" max="255"${ro}`), 'How hard the screen shakes, in percent: 100 = full strength.')}${edFld('Strength 2', edNum(sb(3), `data-s="3" min="0" max="255"${ro}`), 'How hard it shakes on the other axis, in percent: 100 = full strength.')}</div>
    <div class="lbl subhd">Clashes</div>
    <div class="field"><label>Clash class${fTip('What happens when this hit meets another attack or a guard. Melee: a guard blocks it, and it clashes with other melee. Guard crush: breaks through a guard and wins against weaker attacks. Projectile: a guard stops it (and can reflect it). Never clashes: passes through other attacks.')}</label>${edSel('class', cls, h.class, `data-h="class"${ro}`)}</div>
    <div class="row2">${edFld('Clash angle from (°)', edNum(h.unk_0e, `data-h="unk_0e" min="0" max="255"${ro}`), 'It only clashes with attacks coming from this angle range: from this angle, over Clash angle width. 0 and 180 = any direction; 0 and 90 = from the front only.')}${edFld('Clash angle width (°)', edNum(h.unk_0f, `data-h="unk_0f" min="0" max="255"${ro}`), 'How wide that angle range is, in degrees. 180 = any direction.')}</div>
    <div class="lbl subhd">Extras</div>
    <div class="row2">${edFld('Orbs on hit', edNum(h.hitstop, `id="h_hitstop" min="0" max="255"${ro}`), 'Orbs scattered when it lands (count × value / 10), most likely EX Force.')}${edFld('Crit bonus', edNum(h.unk_07, `data-h="unk_07" min="0" max="127"${ro}`), 'Added to this hit\'s chance of a critical hit. Retail hits all use 0.')}</div>
    ${edFld('Second contact effect', `<input type="number" min="0" max="254" value="${h.unk_06 === 255 ? '' : h.unk_06}" placeholder="none" data-h="unk_06" data-none="255"${ro}>`, 'Another effect bit played where the hit lands, after the contact effect above: the effect records whose mask has this bit. Empty = none (retail: Tifa, the Emperor).')}
    </div>`;
}
/* +0x2C / +0x2E are one 32-bit field (docs/archive/open-questions/atkp.md §8): an s16 bone for reactions 11–15
   (FUN_08813484), a float for 2, 3, 17 (FUN_0881341C); hits.json keeps them as kb_bone (s16) + unk_2e (u16) */
const edHitU2C = (h) => (((h.unk_2e || 0) & 0xFFFF) << 16 | ((h.kb_bone || 0) & 0xFFFF)) >>> 0;
function edHit2C(h, ro) {
  const R = h.reaction;
  if ([2, 3, 17].includes(R)) {
    const f = new DataView(new ArrayBuffer(4)); f.setUint32(0, edHitU2C(h), true);
    return edFld('Launch speed', `<input type="number" step="0.01" value="${+f.getFloat32(0, true).toFixed(6)}" data-hf="1"${ro}>`,
      'How fast this reaction launches the foe (reactions 2, 3 and 17; retail 0.01–0.6). It goes straight up, at Follow, with no direction of its own.');
  }
  if (R >= 11 && R <= 15) return edFld('Knock-back bone', edNum(h.kb_bone, `data-h="kb_bone"${ro}`), 'Which of your bones the foe is held or dragged from, for this reaction (0 = your root, the floor under you).');
  return '';                                    // this reaction doesn't read +0x2C
}
function edHit2CRow(h) { if (!h || h.raw) return ''; const f = edHit2C(h, edCan() ? '' : ' disabled'); return f ? `<div class="row2" id="edHit2c">${f}</div>` : ''; }
function edHitMoreWire(h, apply) {               // apply(fn): app.js hitEdit (a hit other events use is copied first)
  const sec = $('edHit'), sec2 = $('edHit2c'); if (!sec && !sec2) return;
  if (!edCan() || !h) return;
  const all = (q) => [...(sec ? sec.querySelectorAll(q) : []), ...(sec2 ? sec2.querySelectorAll(q) : [])];
  const hp = msPath('hits.json');
  const set = (fn) => { const x = apply ? apply(fn) : (edit([hp], () => fn(h)), h); derive(); if (typeof DM !== 'undefined') DM.key = ''; if (x && x !== h) renderAll(); else renderTimeline(); };
  all('[data-h]').forEach((el) => el.onchange = () => {
    const k = el.dataset.h, nv = el.dataset.none, v = el.value.trim() === '' && nv ? (nv === '1' ? -1 : +nv) : parseInt(el.value, 10) || 0;   // empty = none
    set((x) => { x[k] = k === 'unk_30' ? '0x' + ((v < 0 ? 0xFFFFFFFF : v) >>> 0).toString(16).toUpperCase().padStart(8, '0') : Math.max(k === 'kb_bone' ? -32768 : 0, v); });
  });
  all('[data-hf]').forEach((el) => el.onchange = () => {   // the float view of +0x2C / +0x2E
    const v = parseFloat(el.value) || 0, dv = new DataView(new ArrayBuffer(4)); dv.setFloat32(0, v, true);
    const u = dv.getUint32(0, true);
    set((x) => { x.kb_bone = (u & 0xFFFF) << 16 >> 16; x.unk_2e = u >>> 16; });
  });
  all('[data-s]').forEach((el) => el.onchange = () => {
    const k = +el.dataset.s, v = Math.max(0, Math.min(255, parseInt(el.value, 10) || 0));
    set((x) => { const b = (x.shake || '00000000').match(/../g); b[k] = v.toString(16).padStart(2, '0'); x.shake = b.join(''); });
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
/* one sub-section per hook, each with a switch (2026-10-31: "This should have a main header", "Each hook should
   have a subsection here"); a hook's values only show while it is on */
function edHooksHTML(mi) {
  if (!S.modPath || !S.mod || (S.rig && S.rig.retail) || S.mod.kind === 'retail') return '';
  const H = dmHooks(), can = edCan(), ro = can ? '' : ' disabled';
  const w = H.warps.find((x) => x.motions.includes(mi)), v = H.vtracks.find((x) => x.motions.includes(mi)), la = H.landact && H.landact.motions.includes(mi) ? H.landact : null;
  const laOn = !!la || S.edLandOpen === mi, on = [w, v, laOn].filter(Boolean).length;
  const shared = (x) => x && x.motions.length > 1 ? `<p class="note">Shared with ${esc(x.motions.filter((m) => m !== mi).map(motionName).join(', '))}.</p>` : '';
  // what each hook does is its title's tip (request: no explanation paragraphs in the inspector)
  const sub = (title, key, id, isOn, what, body) => `<div class="hook"><label class="hook-h"><span>${title}${edTip(what.replace(/<[^>]+>/g, ''))}</span><small class="mono">${key}</small><input type="checkbox" role="switch" class="switch" id="${id}"${isOn ? ' checked' : ''}${ro}></label>
    ${isOn ? body : ''}</div>`;
  return `<div class="sec" id="edHooks"><h3>Runtime hooks <small>native build${on ? ' · ' + on + ' on' : ''}</small>${edTip('Extra rules the native build applies while this animation plays. The dummy runs them too. mod.json runtime_hooks, read by runtime/roster.cpp. A stock game (PSP, PPSSPP) ignores them.')}</h3>
    ${sub('Home onto the foe', 'warp=', 'edWarp', w, "Stretches or shrinks the animation's travel so it ends at the foe.", w ? `${shared(w)}<div class="row3">${edFld('Travel (m)', edNum(w.travel, `data-w="travel" min="0"${ro}`), "the animation's own root travel to stretch / shrink")}${edFld('Stop short (m)', edNum(w.stop, `data-w="stop" min="0"${ro}`), 'ends this far before the foe')}${edFld('Lift', edSel('lift', [[-1, 'flat'], [0, '3D'], ...(w.lift > 0 ? [[w.lift, w.lift + ' m above']] : [])], w.flat ? -1 : w.lift, `data-w="lift"${ro}`), 'flat = home on the foe horizontally only; 3D = also up / down to them; N m above = aim that high over them')}</div>
      ${edFld('Reach (m)', `<input type="number" step="any" value="${H.reach.has(mi) ? H.reach.get(mi) : ''}" placeholder="${w.travel}" data-w="reach"${ro}>`, 'warpreach=: the same travel paces a longer way. Empty = the travel.')}` : '')}
    ${sub("Match the foe's height", 'vtrack=', 'edVt', v, 'Eases him up or down to the foe\'s height over a range of ticks.', v ? `${shared(v)}<div class="row2">${edFld('From tick', edNum(v.from, `data-v="from" min="0"${ro}`), 'tick of the animation the easing starts')}${edFld('To tick', edNum(v.to, `data-v="to" min="1"${ro}`), 'tick it has reached the foe\'s height')}${edFld('At most (m)', edNum(v.max, `data-v="max" min="0"${ro}`), 'the most it moves you up or down')}${edFld('Stay above a lower foe (m)', edNum(v.lift, `data-v="lift" min="0"${ro}`), 'when the foe is below you, stop this far above them')}</div>` : '')}
    ${sub('Action on landing', 'landact=', 'edLandOn', laOn, `Starts another action when he touches down. One per character${H.landact && !la ? ` (now used by ${esc(H.landact.motions.map(motionName).join(', '))})` : ''}.`, edFld('Action', `<input type="number" min="0" value="${la ? la.action : ''}" placeholder="action number" id="edLand"${ro}>`, 'the plya action to start at touchdown'))}
  </div>`;
}
function edHooksWire(mi) {
  const sec = $('edHooks'); if (!sec) return;
  sec.querySelectorAll('.hook-h .tip').forEach((t) => t.addEventListener('click', (e) => e.preventDefault()));   // (a tip in the switch's label must not flip it)
  if (!edCan()) return;
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
  $('edLandOn').onchange = () => {
    if ($('edLandOn').checked) { S.edLandOpen = mi; renderInspector(); return; }
    S.edLandOpen = null;
    const cur = H.landact, mo = cur ? cur.motions.filter((m) => m !== mi) : [];
    if (cur && cur.motions.includes(mi)) edHookSet('landact', mo.length ? `landact=${cur.action},${mo.join(',')}` : null, (t) => t.startsWith('landact='));
    redo();
  };
  if ($('edLand')) $('edLand').onchange = () => {
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
/* the assist rig: the game adds +50000 to its sound and voice ids (FUN_08842828, chardata+8 bit 8; docs/archive/event-ops/E.md),
   so its timelines name its bank (18500, voices 18600) with the main bank's numbers (18000xx, 18100xx). Bank ids: a fighter's at
   10000 + 1000k, its assist's +500, their voices +100. A timeline cue is "game cue - edCueShift". */
const edCueShift = (b) => { const id = b ? Math.floor(SndBank.cueBase(b) / 100) : 0; return id >= 10000 && id % 1000 >= 500 ? 50000 : 0; };
const edTlBase = (b) => SndBank.cueBase(b) - edCueShift(b);       // the timeline cue of slot 0
function edGameCue(cue) {                          // a timeline cue -> the cue the game plays for this rig (character and voice banks only)
  const sh = edCueShift(typeof edBank === 'function' ? edBank() : null); if (!sh || cue == null) return cue;
  const loc = typeof SharedSnd !== 'undefined' && SharedSnd.cueLocation(cue + sh);
  return loc && (loc.group === 'character' || loc.group === 'voice') ? cue + sh : cue;
}
const edSndName = (i) => ((S.mod && S.mod.sound_names) || {})[i] || '';
/* names for the timeline and the inspector: a sound cue -> its sound_names name, else "slot N" (the character's bank or
   a shared one), else "cue N"; a voice cue -> its voice_lines name, else "slot N" (the voice set's slots follow the
   character bank's at +10000 when the set isn't loaded yet) */
function edCueName(cue) {
  if (typeof cue !== 'number') return String(cue);
  const b = edBank(), slot = b ? cue - edTlBase(b) : -1;
  if (b && slot >= 0 && slot < b.slots.length) return edSndName(slot) || 'slot ' + slot;
  const loc = typeof SharedSnd !== 'undefined' ? SharedSnd.cueLocation(edGameCue(cue)) : null;
  return loc ? 'slot ' + loc.slot : 'cue ' + cue;
}
function edVoiceSlot(cue) {                       // {slot, name} of a voice cue in this rig's voice set, or null
  if (typeof cue !== 'number') return null;
  const vx = typeof voxSlotOf === 'function' ? voxSlotOf(cue) : null; if (vx) return vx;
  const b = edBank(), gp = typeof voxGamePath === 'function' ? voxGamePath() : null; if (!b || !gp) return null;
  const slot = cue - (edTlBase(b) + 10000);
  return slot >= 0 && slot < 1000 ? { slot, name: voxNames(gp)[slot] || '' } : null;
}
function edVoiceName(cue) {
  const v = edVoiceSlot(cue); if (v) return v.name || 'slot ' + v.slot;
  return edCueName(cue);
}
const ED_FOOT_KIND = ['step', 'landing', 'impact', 'second impact'];   // 0x060F kinds (playsnd.js PSND_FLOOR)
const ED_FACE_GROUP = ['left eye', 'right eye', 'mouth'];              // 0x0214 face groups when the model doesn't name them
function edFaceGroup(g) {                          // the model's TexAnm group label ("lip"), else the usual layout
  const fg = typeof FT !== 'undefined' && FT.groups ? FT.groups.find((x) => x.id === g) : null;
  return fg && fg.label ? fg.label.replace(/_/g, ' ') : ED_FACE_GROUP[g] || 'group ' + g;
}
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
    out.push([g, [b && 'birth ' + b, d && 'body ' + d].filter(Boolean).join(' · ') || (rs.length ? rs.length + ' placements, none a shot’s look' : 'empty'), 'group ' + g]);
  }
  return out;
}
function edSndHTML(e) {                           // a sound event: what it plays, Play, and the sound browser to change it
  const b = edBank(), cue = e.sound, base = b ? edTlBase(b) : -1, slot = cue - base, mine = b && slot >= 0 && slot < b.slots.length;
  const loc = !mine && typeof SharedSnd !== 'undefined' ? SharedSnd.cueLocation(edGameCue(cue)) : null, s = mine ? b.slots[slot] : null;
  const what = mine ? `${edSndName(slot) ? esc(edSndName(slot)) + ' · ' : ''}slot ${slot} of the character's bank${s && s.seconds ? ' · ' + s.seconds.toFixed(2) + ' s' : ''}`
    : loc ? `slot ${loc.slot} of ${esc(loc.name)}${ISO.index ? '' : ' (set the game ISO to hear it)'}` : 'in no bank the game has: it plays nothing';
  return `<div class="sec" id="edSnd"><h3>Sound <small class="mono">cue ${cue}</small></h3>
    <p class="note">${what}.</p>
    <div class="btnrow"><button type="button" class="mini" id="edSndPlay"${mine || (loc && ISO.index) ? '' : ' disabled'}><i class="ic ic-play"></i> Play</button><button type="button" class="mini" id="edSndBrowse">Browse sounds…</button></div></div>`;
}
function edSndWire(p, kind, idx) {
  const pl = $('edSndPlay'); if (pl) pl.onclick = () => psndPlay(F(p)[kind][idx].sound, true);
}

const ED_PROJ_SPAWN_HOW = { link_emit: '(at launch, in flight or when it ends)', link_hit: 'on a foe', link_terrain: 'on a wall or floor',
  link_terrain_1: 'on a wall or floor', link_fall: 'if it loses the floor', link_clash: 'if it is cancelled' };
/* ====================================================== hooks into app.js */
function edEventHTML(kind, idx, e) {
  let h = '';
  if (e.group !== undefined) h += edVolHTML(e);
  // a projectile lane under the event (projtl.js) shows that projectile here instead of the one the event fires
  if (e.projectile !== undefined) { const pf = S.projFocus; h += pf ? edProjHTML(pf.id, pf.parent != null ? `spawned by ${pf.parent} ${pf.how || ED_PROJ_SPAWN_HOW[pf.field] || ''}` : 'fired here') : edProjHTML(e.projectile, 'fired here'); }
  if (opCode(e.op) === 0x0303 && e.sound !== undefined) h += edSndHTML(e);
  else h += edCueHTML(e);
  h += edFxJumpHTML(e);
  return h;
}
/* a voice or conditional sound: play it on its own (the character's bank, or the game's from the ISO) */
function edCueOf(e) { if (typeof psndCue === 'function' && opCode(e.op) === 0x060F) return psndCue(e); const v = typeof e.voice === 'number' ? e.voice : typeof e.sound === 'number' ? e.sound : null; return v != null && v >= 0 && v !== 0xFFFF ? v : null; }
function edCueHTML(e) {
  const cue = edCueOf(e); if (cue == null) return '';
  const b = edBank(), base = b ? edTlBase(b) : -1, mine = b && cue - base >= 0 && cue - base < b.slots.length;
  const vx = e.voice !== undefined && typeof voxSlotOf === 'function' ? voxSlotOf(cue) : null;   // this character's own voice set
  const loc = !mine && !vx && typeof SharedSnd !== 'undefined' ? SharedSnd.cueLocation(edGameCue(cue)) : null, can = mine || vx || (loc && ISO.index);
  const foot = opCode(e.op) === 0x060F, vn = !vx && e.voice !== undefined ? edVoiceSlot(cue) : null;   // (the voice set not loaded yet: its name still)
  return `<div class="sec"><h3>${e.voice !== undefined ? 'Voice' : 'Sound'} <small class="mono">cue ${cue}</small>${foot ? edTip('In game the floor under the fighter picks the sound (each stage material has its own bank, 7 kinds × 4 footwear); the preview plays it on stone.') : ''}</h3>
    <p class="note">${vx ? `${vx.name ? esc(vx.name) + ' · ' : ''}${vx.name ? 'slot' : 'Slot'} ${vx.slot} of this character's voice bank${vx.added ? ' (a custom line)' : ''}.` : mine ? `${edSndName(cue - base) ? esc(edSndName(cue - base)) + ' · ' : ''}${edSndName(cue - base) ? 'slot' : 'Slot'} ${cue - base} of the character's bank.` : vn && vn.name ? `${esc(vn.name)} · slot ${vn.slot} of this character's voice bank.` : loc ? `Slot ${loc.slot} of ${esc(loc.name)}${ISO.index ? '' : ' (set the game ISO to play it)'}.` : 'In no bank the game has: it plays nothing.'}</p>
    <div class="btnrow"><button type="button" id="edCuePlay" class="mini"${can ? '' : ' disabled'}>▶ Play</button>${e.sound !== undefined && opCode(e.op) !== 0x060F ? '<button type="button" class="mini" id="edSndBrowse">Browse sounds…</button>' : ''}${e.voice !== undefined && typeof voiceBrowse === 'function' ? '<button type="button" class="mini" id="edVoxBrowse">Browse voices…</button>' : ''}</div></div>`;
}
function edCueWire(e) {
  const btn = $('edCuePlay'), cue = edCueOf(e); if (!btn || cue == null) return;
  if (e.voice !== undefined && typeof voxLoad === 'function' && !(typeof voxNow === 'function' && voxNow())) voxLoad().then((r) => { if (r && !r.need && voxNow()) renderInspector(); });
  btn.onclick = () => psndPlay(cue, true);
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
  if (e.hit !== undefined) edHitMoreWire(hitOf(e.hit), typeof hitEdit === 'function' ? (fn) => hitEdit(p, kind, idx, fn) : null);
  if (e.group !== undefined) edVolWire(p, kind, idx);
  if (e.projectile !== undefined) {
    const pf = S.projFocus;
    if (pf && pf.parent != null) edProjWire(pf.id, { paths: [], set: (nid) => { edProj(pf.parent)[pf.field] = nid; }, after: (nid) => { S.projFocus = { ...pf, id: nid }; } });
    else edProjWire(pf ? pf.id : e.projectile, { paths: [p], set: (nid) => { F(p)[kind][idx].projectile = nid; } });
  }
  if (opCode(e.op) === 0x0303 && e.sound !== undefined) edSndWire(p, kind, idx);
  const bb = $('edSndBrowse');
  const vb = $('edVoxBrowse');
  if (vb) vb.onclick = () => voiceBrowse(F(p)[kind][idx].voice, edCan() ? (cue) => { edit([p], () => { F(p)[kind][idx].voice = cue; }); renderTimeline(); renderInspector(); } : null);
  if (bb && typeof sndBrowse === 'function') bb.onclick = () => sndBrowse(F(p)[kind][idx].sound, edCan() ? (cue) => { edit([p], () => { F(p)[kind][idx].sound = cue; }); renderTimeline(); renderInspector(); } : null);
}

/* ====================================================== field help: one floating bubble for every .tip */
(() => {
  let box = null;
  const show = (el) => {
    if (!box) { box = document.createElement('div'); box.className = 'tipbox'; document.body.appendChild(box); }
    box.textContent = el.dataset.tip; box.style.display = 'block';
    const r = el.getBoundingClientRect(), w = box.offsetWidth, h = box.offsetHeight;
    let x = Math.min(Math.max(8, r.left + r.width / 2 - w / 2), innerWidth - w - 8), y = r.top - h - 6;
    if (y < 8) y = r.bottom + 6;
    box.style.left = x + 'px'; box.style.top = y + 'px';
  };
  const hide = () => { if (box) box.style.display = 'none'; };
  document.addEventListener('mouseover', (e) => { const t = e.target.closest && e.target.closest('.tip,.ptip'); if (t) show(t); });
  // click / tap: the bubble (a tap on the Deck), not the label's field
  document.addEventListener('click', (e) => { const t = e.target.closest && e.target.closest('.tip'); if (!t) return; e.preventDefault(); show(t); });
  document.addEventListener('mouseout', (e) => { if (e.target.closest && e.target.closest('.tip,.ptip')) hide(); });
  document.addEventListener('focusin', (e) => { if (e.target.classList && e.target.classList.contains('tip')) show(e.target); });
  document.addEventListener('focusout', hide);
  document.addEventListener('scroll', hide, true);
})();
/* labels linked to their fields: every rendered .field / .kv label gets for= its control (an id made up when it has
   none), and a tip inside the label describes that control (aria-describedby) instead of being read as part of the name */
let edFldN = 0;
function edLinkLabels(root) {
  if (!root.querySelectorAll) return;
  for (const lab of [...(root.matches('label') ? [root] : []), ...root.querySelectorAll('label:not([for])')]) {
    if (lab.htmlFor || lab.querySelector('input,select,textarea')) continue;            // already linked, or wraps its control
    const fld = lab.closest('.field, .kv'); if (!fld) continue;
    const ctl = [...fld.querySelectorAll('input:not([type=hidden]),select,textarea')].find((c) => c.closest('.field, .kv') === fld);
    if (!ctl) continue;
    if (!ctl.id) ctl.id = 'fld-' + (++edFldN);
    lab.htmlFor = ctl.id;
  }
  for (const tip of root.querySelectorAll('label .tip:not([id])')) {
    const ctl = tip.closest('label').control; if (!ctl) continue;
    tip.id = 'tip-' + (++edFldN); tip.setAttribute('aria-hidden', 'true');
    ctl.setAttribute('aria-describedby', [ctl.getAttribute('aria-describedby'), tip.id].filter(Boolean).join(' '));
  }
}
new MutationObserver((ms) => { for (const m of ms) for (const n of m.addedNodes) if (n.nodeType === 1) edLinkLabels(n); }).observe(document.body, { childList: true, subtree: true });
edLinkLabels(document.body);
