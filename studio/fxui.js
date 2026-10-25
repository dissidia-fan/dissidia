// Dissidia Move Studio: the effect editor. Pick an effect in the left pane's Effects tab: it plays by itself in the
// view, its pieces sit on the timeline, and the inspector edits it (effect settings, where it plays, each piece's
// keys and components, particles, trails, textures and flipbooks). Edits go through fxedit.js (the FEP block graph),
// so the saved effect package is the game's own format. Undo and Save work as for events.
'use strict';
const FXU = { others: new Map(), menu: null };
const fxuShort = (n) => (n || '').replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '');
const fxuNum = (v, d) => { const x = Number(v); return isFinite(x) ? +x.toFixed(d == null ? 3 : d) : 0; };

/* ------------------------------------------------------------------ state and edits */
function fxuPkg() {
  if (!S.effe) return null;
  if (!S.effe.pkg) S.effe.pkg = new FxePackage(S.effe.bytes);
  return S.effe.pkg;
}
function fxuApply(bytes) {
  const magi = S.fx && S.fx.magi;
  S.effe.bytes = bytes;
  S.fx = loadEffects(bytes); if (S.fx) S.fx.magi = magi;
  fxTexCache.clear(); fxGeoCache.clear();
  fxuDeriveEffects();
}
function fxuDeriveEffects() {                    // the effect pickers list what the package holds now
  if (!S.fx) return;
  S.effects = new Map();
  for (const r of S.fx.fepa) {
    if (!S.effects.has(r.id)) S.effects.set(r.id, { id: r.id, name: (S.fx.fep.names[r.effect] || {}).name || '#' + r.effect, bones: [] });
    if (r.bone && !S.effects.get(r.id).bones.includes(r.bone)) S.effects.get(r.id).bones.push(r.bone);
  }
}
/* one undoable edit of the effect package: fn(pkg) changes the block graph, then it is written and re-read */
function fxuEdit(fn, keepSel) {
  if (S.readOnly) { readOnlyNote(); return; }
  if (!S.effe) { toast('This character has no effect package.'); return; }
  const before = S.effe.bytes, wasDirty = S.effe.dirty, pkg = fxuPkg();
  let res;
  try { res = fn(pkg); } catch (e) { S.effe.pkg = null; console.warn('studio: effect edit', e); toast(e.message); return; }
  let bytes;
  try { bytes = pkg.toBytes(); } catch (e) { S.effe.pkg = null; console.warn(e); toast('That edit would break the effect package: ' + e.message); return; }
  S.undo.push([{ effe: before, dirty: wasDirty }]);
  const effeUndos = S.undo.filter((u) => u[0] && u[0].effe);
  if (effeUndos.length > 40) S.undo.splice(S.undo.indexOf(effeUndos[0]), 1);    // snapshots are whole packages (1–2 MB)
  $('undoBtn').disabled = false;
  S.effe.dirty = true;
  fxuApply(bytes);
  refreshStatus();
  if (!keepSel) fxuClampSel();
  fxRefresh(); renderAll();
  return res;
}
function fxuUndoEntry(x) { S.effe.bytes = x.effe; S.effe.pkg = null; S.effe.dirty = x.dirty; fxuApply(x.effe); fxuClampSel(); }
function fxuClampSel() {
  const E = S.fxEdit; if (!E || !S.fx) return;
  if (E.ni >= S.fx.fep.names.length) E.ni = S.fx.fep.names.length - 1;
  const b = fxuBody(); if (!b || E.node == null || !b.nodes[E.node]) E.node = null;
}

/* ------------------------------------------------------------------ views on the parsed package */
const fxuFep = () => S.fx && S.fx.fep;
function fxuBody(ni) { const fep = fxuFep(); if (!fep) return null; const bi = fepBodyOfName(fep, ni == null ? S.fxEdit.ni : ni); return bi == null ? null : fepBody(fep, bi); }
const fxuRecs = (ni) => (S.fx ? S.fx.fepa.filter((r) => r.effect === ni) : []);
const FXU_DRAW = { 10: 'flipbook', 11: 'mesh', 20: 'scrolling mesh', 21: 'particles', 105: 'trail', 107: 'screen flash', 110: 'zoom blur', 111: 'screen shift', 101: 'shockwave',
  100: 'sparks', 102: 'line sparks', 103: 'rays', 95: 'points', 18: 'spawns pieces', 151: 'spawns pieces' };
function fxuNodeWhat(n) { const k = n.tracks.map((t) => FXU_DRAW[t.chan]).filter(Boolean); return k.length ? [...new Set(k)].join(' + ') : 'no drawing'; }
const FXU_CHAN = { 1: 'Position', 2: 'Position', 3: 'Position', 4: 'Rotation', 5: 'Rotation', 6: 'Rotation', 7: 'Size', 8: 'Size', 9: 'Size', 15: 'Colour', 16: 'Colour', 17: 'Colour',
  10: 'Flipbook', 11: 'Mesh', 14: 'Facing', 18: 'Spawns pieces', 19: 'Gravity', 20: 'Scrolling mesh', 21: 'Particles', 22: 'Random start position', 23: 'Random start rotation', 24: 'Random start size',
  25: 'Random colour', 29: 'Turns along its movement', 31: 'Random position', 32: 'Random rotation', 33: 'Random size', 34: 'Random colour', 39: 'Random colour per key', 61: 'Flicker', 62: 'Flicker on colour',
  79: '(trail helper)', 100: 'Spark streaks', 101: 'Shockwave ring', 102: 'Line sparks', 103: 'Light rays', 105: 'Weapon trail', 106: 'Trail points', 107: 'Screen flash', 109: 'Blur strength', 110: 'Zoom blur', 111: 'Screen shift', 151: 'Spawns pieces' };
/* where each FEPA id is used: timelines, projectiles, trails, hits, or the game itself */
function fxuUses() {
  const u = new Map(), add = (id, what) => { if (!u.has(id)) u.set(id, new Set()); u.get(id).add(what); };
  for (const p of S.tlPaths || []) {
    const t = F(p); if (!t) continue; const nm = motionName(t.motion);
    for (const e of t.keyed) { const c = opCode(e.op); if (c === 0x0219 || c === 0x042F) { const a = argsOf(e); add(a[0], nm); if (c === 0x042F && a[2]) add(a[2], nm); } }
    for (const e of t.ranged) { const c = opCode(e.op), a = argsOf(e); if (c === 0x0219 || c === 0x0466) add(a[0], nm); if (c === 0x042A) add(a[1], nm); }
  }
  if (S.fx && S.fx.magi) for (const pr of S.fx.magi.values()) { if (pr.effect !== 0xFFFF && pr.effect < 64) for (const x of S.fx.fepa) if (fxGroupHas(x, pr.effect)) add(x.id, 'projectile ' + pr.id); if (pr.origin === 6) add(pr.origin_arg, 'projectile ' + pr.id); }
  return u;
}
function fxuEffectUse(ni, uses) {
  const out = new Set();
  for (const r of fxuRecs(ni)) {
    for (const x of uses.get(r.id) || []) out.add(x);
    if (r.mask) out.add('hits');
    if (r.trig) out.add('the game');                 // played by the engine itself (dodges, guards, landings …)
    if (/_(DOGE|UKEMI)_/.test((S.fx.fep.names[ni] || {}).name || '')) out.add('the game (dodges)');
    if (r.id === 9999 || r.id === 8888) out.add('weapon trails');
  }
  return out;
}

/* ------------------------------------------------------------------ left pane */
function fxuLeft(L) {
  if (!S.fx) { L.innerHTML = '<p class="note">This character has no effect package.</p>'; return; }
  const fep = S.fx.fep, uses = fxuUses(), n = fep.names.length;
  const isMod = !(S.rig && S.rig.retail), org = isMod ? fxuOrigins() : null;
  if (isMod && !org && S.retailChars && S.retailChars.length && !FXU.retailP) fxuRetailIndex().then(() => { if (S.tab === 'fx') renderLeft(); if (S.fxEdit) renderInspector(); });
  const OL = { same: 'retail, unchanged', changed: 'retail, changed', new: 'new' };
  let h = `<div class="fxu-top"><span class="note">${n} of 128 effects</span><span class="btnrow"><button type="button" id="fxuUnused" title="Remove the effects nothing plays (no event, projectile, hit, runtime hook or engine rule points at them), to make room">Remove unused…</button><button type="button" id="fxuNew" title="A new effect from a template: the simplest retail effect of the kind you pick, copied with its textures">+ New…</button><button type="button" id="fxuFrom" title="Copy an effect (with its textures, meshes and flipbooks) from this or another character">+ Copy…</button></span></div>`;
  if (isMod && org) { const c = { same: 0, changed: 0, new: 0 }; org.forEach((o) => c[o.kind]++); h += `<p class="note fxu-legend"><span class="fxu-org same">${c.same} from ${esc((org.donor || 'retail').replace(/ \(assist\)$/, ''))} and others, unchanged</span> <span class="fxu-org changed">${c.changed} changed</span> <span class="fxu-org new">${c.new} new</span></p>`; }
  else if (isMod && S.retailChars && S.retailChars.length) h += '<p class="note">Comparing with the game\u2019s characters…</p>';
  else if (isMod) h += '<p class="note">Set the game ISO to see which effects come from retail characters.</p>';
  fep.names.forEach((nm, ni) => {
    const recs = fxuRecs(ni), use = fxuEffectUse(ni, uses), sel = S.fxEdit && S.fxEdit.ni === ni;
    const body = fxuBody(ni);
    h += `<div class="anim-row fxu-row${sel ? ' sel' : ''}" data-ni="${ni}" title="${esc(nm.name)}${recs.length ? ' · event ids ' + recs.map((r) => r.id).join(', ') : ''}">
      <span class="mono">${ni}</span><span>${esc(fxuShort(nm.name))}</span>
      <small>${org ? `<span class="fxu-org ${org[ni].kind}" title="${org[ni].kind === 'new' ? 'Not in any retail character' : 'From ' + esc(org[ni].who) + (org[ni].kind === 'same' ? ', exactly as in the game' : ', edited')}">${org[ni].kind === 'new' ? 'new' : esc(org[ni].who.replace(/ \(assist\)$/, '')) + (org[ni].kind === 'same' ? '' : ' · changed')}</span> ` : ''}${body && body.a & 2 ? 'keeps playing · ' : ''}${use.size ? esc([...use].slice(0, 2).join(', ')) + (use.size > 2 ? ' …' : '') : recs.length ? 'not used' : 'no placement'}</small></div>`;
  });
  L.innerHTML = (typeof compLeftHTML === 'function' ? compLeftHTML() : '') + h;
  if (typeof compLeftWire === 'function') compLeftWire(L);
  L.querySelectorAll('.fxu-row').forEach((el) => el.onclick = () => fxuOpen(+el.dataset.ni));
  $('fxuFrom').onclick = (e) => fxuCopyMenu(e.currentTarget);
  if ($('fxuNew')) $('fxuNew').onclick = (e) => fxtMenu(e.currentTarget);
  if ($('fxuUnused')) { $('fxuUnused').hidden = !isMod; $('fxuUnused').onclick = () => ckRemoveUnused(); }
}
function fxuOpen(ni) {
  const body = fxuBody(ni);
  S.fxEdit = { ni, node: null, t: 0, stop: body ? (body.a & 2 ? (body.c || Math.max(60, body.duration)) : null) : null, rec: 0 };
  playAuto();                                   // plays at once while the play button is on
  S.sel = null;
  fxRefresh(); renderAll();
}
function fxuClose() { S.fxEdit = null; pose(); renderAll(); }

/* ------------------------------------------------------------------ preview */
function fxuSoloOpts() {
  const E = S.fxEdit, recs = fxuRecs(E.ni);
  let rec = recs[Math.min(E.rec || 0, recs.length - 1)];
  if (!rec) rec = { index: -1, pos: [0, 0, 0], rot: [0, 0, 0], scale: [1, 1, 1], id: -1, effect: E.ni, life: 0, delay: 0, follow: 0, attach: 0, model: '', bone: '', motion: '', trig: 0, mask: 0, keys: 0 };
  const hit = !rec.model;                         // caller-placed effects (hits, shots) play where a foe would be
  return { rec: { ...rec, delay: 0 }, stop: E.stop, hit };
}
function fxuLength() {                           // preview length in ticks
  const b = fxuBody(); if (!b) return 120;
  let end = b.duration;
  for (const sp of b.spawns) { if (sp.frame >= Math.max(1, b.duration)) continue;   // a spawn past the effect's length never happens (fxRunInstance)
    for (const ni of sp.nodes) { const n = b.nodes[ni]; if (n) end = Math.max(end, sp.frame + n.duration); } }
  if (b.a & 2) end = Math.max(end, (S.fxEdit.stop || 0) + 40);
  return Math.max(30, end + 10);
}
function fxuTick(dt) {
  const E = S.fxEdit, L = fxuLength();
  E.t += dt * 60 * parseFloat($('speed').value);
  if (E.t >= L) { if ($('loop').checked) E.t = E.t % L; else { E.t = L; S.playing = false; $('playBtn').textContent = '▶'; } }
  fxRefresh(); fxuPlayhead();
}

/* ------------------------------------------------------------------ timeline */
function fxuTimeline() {
  const inner = $('lanesInner'), px = PX(), E = S.fxEdit, b = fxuBody(), fep = fxuFep(), nm = fep.names[E.ni];
  const L = fxuLength() / 2;
  $('tlTitle').textContent = 'Effect ' + E.ni + ' — ' + fxuShort(nm && nm.name);
  const width = LABEL_W + (L + 10) * px, X = (tick) => LABEL_W + tick / 2 * px;
  let h = `<div class="ruler" id="ruler" style="width:${width}px">`;
  const stepF = px >= 14 ? 5 : px >= 7 ? 10 : 20;
  for (let f = 0; f <= L + 10; f += stepF) h += `<span style="left:${LABEL_W + f * px}px">${f}</span>`;
  h += '</div>';
  if (!b) { inner.innerHTML = h + '<p class="note">This effect has no body.</p>'; return; }
  const col = 'var(--c-fx)';
  h += `<div class="lane cat" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">Effect</span></div>`;
  h += `<div class="lane" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-fxe="1" title="Click for the effect's settings">${b.a & 2 ? 'Keeps playing' : 'Plays'} ${b.duration / 2} frames</span>
    <div class="ev${b.a & 2 ? ' open' : ''}" data-fxbar="body" style="left:${X(0)}px;width:${Math.max(3, b.duration / 2 * px)}px;background:${col}" title="The effect's length (drag the end)"><span class="h r"></span></div>
    ${b.a & 2 && E.stop != null ? `<div class="ev key fxu-stop" data-fxbar="stop" style="left:${X(E.stop)}px;background:var(--ember)" title="The preview stops the effect here (drag); in the game it stops when its event, shot or lifetime ends"></div>` : ''}</div>`;
  h += `<div class="lane cat" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">Pieces</span></div>`;
  const starts = new Map(); for (const sp of b.spawns) for (const ni of sp.nodes) if (!starts.has(ni)) starts.set(ni, sp.frame);
  b.nodes.forEach((n, ni) => {
    if (!n) return;
    const st = starts.has(ni) ? starts.get(ni) : null, sel = E.node === ni, mute = E.mute && E.mute.has(ni);
    const keys = new Set(); for (const t of n.tracks) for (const s of t.segs) if (!s.init && s.start > 0) keys.add(s.start);
    const lab = `#${ni} ${fxuNodeWhat(n)}`;
    let bar = '';
    if (st != null) {
      bar = `<div class="ev${sel ? ' sel' : ''}" data-fxbar="node" data-ni="${ni}" style="left:${X(st)}px;width:${Math.max(3, n.duration / 2 * px)}px;background:${col};${mute ? 'opacity:.3' : ''}" title="${esc(lab)}: frames ${st / 2}–${(st + n.duration) / 2} (drag to move, drag the end for its life)"><span class="h r"></span></div>`;
      if ((n.flags & 2) && n.loop1 > n.loop0) bar += `<div class="fxu-loop" style="left:${X(st + n.loop0)}px;width:${(n.loop1 - n.loop0) / 2 * px}px" title="Loops frames ${n.loop0 / 2}–${n.loop1 / 2} while the effect keeps playing"></div>`;
      for (const k of keys) bar += `<div class="fxu-key" style="left:${X(st + k)}px" title="key at frame ${(st + k) / 2}"></div>`;
    }
    h += `<div class="lane${sel ? ' selrow' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-ni="${ni}" title="${esc(lab)}${st == null ? ' (started by another piece)' : ''}">${mute ? '<s>' : ''}${esc(lab)}${mute ? '</s>' : ''}${st == null ? ' <small>(spawned)</small>' : ''}</span>${bar}</div>`;
  });
  h += `<div class="endline" style="left:${X(b.duration)}px"></div><div class="playhead" id="playhead"></div>`;
  inner.innerHTML = h; inner.style.width = width + 'px';
  fxuPlayhead();
  inner.querySelectorAll('.lane-label[data-ni]').forEach((el) => el.onclick = () => { E.node = +el.dataset.ni; renderTimeline(); renderInspector(); });
  inner.querySelectorAll('.lane-label[data-fxe]').forEach((el) => el.onclick = () => { E.node = null; renderTimeline(); renderInspector(); });
  inner.querySelectorAll('[data-fxbar]').forEach((el) => el.addEventListener('pointerdown', (ev) => fxuDrag(ev, el)));
  const ruler = $('ruler');
  const scrub = (ev) => { const r = ruler.getBoundingClientRect(); E.t = Math.max(0, Math.round(((ev.clientX - r.left) - LABEL_W) / px * 2)); fxRefresh(); fxuPlayhead(); };
  ruler.addEventListener('pointerdown', (ev) => { ruler.setPointerCapture(ev.pointerId); scrub(ev); ruler.onpointermove = scrub; ruler.onpointerup = () => { ruler.onpointermove = null; }; });
}
function fxuPlayhead() {
  const ph = $('playhead'), E = S.fxEdit; if (!E) return;
  if (ph) ph.style.left = (LABEL_W + E.t / 2 * PX()) + 'px';
  $('tlFrame').textContent = `frame ${(E.t / 2).toFixed(1)}`;
}
function fxuDrag(ev, el) {
  ev.preventDefault();
  const E = S.fxEdit, b = fxuBody(), kind = el.dataset.fxbar, ni = el.dataset.ni != null ? +el.dataset.ni : null, x0 = ev.clientX, px = PX();
  const resize = ev.target.classList.contains('r');
  const n = ni != null ? b.nodes[ni] : null, st0 = n ? b.spawns.find((s) => s.nodes.includes(ni)).frame : 0;
  if (ni != null && E.node !== ni) { E.node = ni; renderInspector(); }
  el.setPointerCapture(ev.pointerId);
  let d = 0;
  el.onpointermove = (m) => {
    d = Math.round((m.clientX - x0) / px * 2);
    if (kind === 'stop') el.style.left = (LABEL_W + Math.max(0, E.stop + d) / 2 * px) + 'px';
    else if (resize) el.style.width = Math.max(3, ((kind === 'body' ? b.duration : n.duration) + d) / 2 * px) + 'px';
    else el.style.left = (LABEL_W + Math.max(0, st0 + d) / 2 * px) + 'px';
  };
  el.onpointerup = () => {
    el.onpointermove = null; el.onpointerup = null;
    if (!d) { renderTimeline(); return; }
    if (kind === 'stop') { E.stop = Math.max(0, E.stop + d); fxRefresh(); renderTimeline(); return; }
    if (kind === 'body') fxuEdit((pkg) => fxeSetAt(pkg.fep, b.off + 4, fxeI32b(Math.max(1, b.duration + d))));
    else if (resize) fxuEdit((pkg) => { const nd = pkg.fep.blockAt(n.off).b; fxeSetU16(nd.data, 4, Math.max(1, n.duration + d)); fxeSegLens(pkg.fep, nd); });
    else fxuEdit((pkg) => fxeMoveNode(pkg.fep, pkg.fep.blockAt(b.off).b, ni, Math.max(0, st0 + d)));
  };
}

/* ------------------------------------------------------------------ inspector */
function fxuInspector(I) {
  const E = S.fxEdit, fep = fxuFep(), b = fxuBody(), nm = fep.names[E.ni];
  let h = `<div class="sec"><h3>Effect ${E.ni} <span class="btnrow"><button type="button" id="fxuBack" title="Back to the animation">✕</button></span></h3>
    <div class="field"><label>Name</label><input type="text" id="fxuName" value="${esc(nm ? nm.name : '')}"></div>`;
  const org = !(S.rig && S.rig.retail) && fxuOrigins();
  if (org && org[E.ni]) h += `<p class="note"><span class="fxu-org ${org[E.ni].kind}">${org[E.ni].kind === 'new' ? 'New: not in any retail character' : (org[E.ni].kind === 'same' ? 'Unchanged from ' : 'Changed from ') + esc(org[E.ni].who)}</span></p>`;
  if (b) {
    h += `<div class="row2"><div class="field"><label>Length (frames)</label><input type="number" step="0.5" id="fxuDur" value="${b.duration / 2}"></div>
      <div class="field"><label>Ends</label><select id="fxuSus"><option value="0"${b.a & 2 ? '' : ' selected'}>after its length</option><option value="2"${b.a & 2 ? ' selected' : ''}>when it is stopped</option></select></div></div>`;
    if (b.a & 2) h += `<div class="row2"><div class="field"><label>When stopped</label><select id="fxuStopMode">${['fades out (½ s)', 'fades out (¼ s)', 'vanishes', 'pieces finish'].map((x, i) => `<option value="${i}"${b.d === i ? ' selected' : ''}>${x}</option>`).join('')}</select></div>
      <div class="field"><label>Stops by itself at (frame, 0 = never)</label><input type="number" step="0.5" id="fxuC" value="${b.c / 2}"></div></div>`;
  }
  h += `<div class="btnrow"><button type="button" id="fxuDup">Duplicate</button><button type="button" id="fxuDel" class="danger">Delete</button></div></div>`;
  const piece = b && E.node != null && b.nodes[E.node];
  if (b) h += fxuPiecesHTML(b);
  if (piece) h += fxuNodeHTML(b, E.node);
  h += `<details class="sec fxu-det"${piece ? '' : ' open'}><summary>Where it plays</summary>${fxuPlacementsHTML()}</details>`;
  h += `<details class="sec fxu-det"${piece ? '' : ' open'}><summary>Textures</summary>${fxuResourcesHTML()}</details>`;
  I.innerHTML = h;
  $('fxuBack').onclick = fxuClose;
  $('fxuName').onchange = (e) => fxuEdit((pkg) => fxeRename(pkg.fep, E.ni, e.target.value.trim() || nm.name));
  if (b) {
    $('fxuDur').onchange = (e) => fxuEdit((pkg) => fxeSetAt(pkg.fep, b.off + 4, fxeI32b(Math.max(1, Math.round(+e.target.value * 2)))));
    $('fxuSus').onchange = (e) => fxuEdit((pkg) => fxeSetAt(pkg.fep, b.off, fxeI32b((b.a & ~2) | +e.target.value)));
    if ($('fxuStopMode')) $('fxuStopMode').onchange = (e) => fxuEdit((pkg) => fxeSetAt(pkg.fep, b.off + 12, fxeI32b(+e.target.value)));
    if ($('fxuC')) $('fxuC').onchange = (e) => fxuEdit((pkg) => fxeSetAt(pkg.fep, b.off + 8, fxeI32b(Math.max(0, Math.round(+e.target.value * 2)))));
  }
  $('fxuDup').onclick = () => {
    const ni = fxuEdit((pkg) => { const ni2 = fxeCopyEffect(pkg, pkg, fxeEffectOfName(pkg.fep, E.ni), (nm.name || 'EFFECT').replace(/_LEAF_\d+$/, '') + '_COPY_LEAF_0'); const t = pkg.fepa.records.find((r) => fxeU32(r, 0x28) === E.ni); fxeAddFepa(pkg, ni2, t); return ni2; });
    if (ni != null) { fxuOpen(ni); toast('Copied as effect ' + ni + ' (event id ' + fxuRecs(ni).map((r) => r.id).join(', ') + '): play it with an Effect event.'); }
  };
  $('fxuDel').onclick = async () => {
    const uses = fxuEffectUse(E.ni, fxuUses());
    if (!(await uiConfirm(`Delete effect ${E.ni} (${fxuShort(nm.name)})?`, `${uses.size ? 'It is used by ' + [...uses].join(', ') + ': those events will play nothing. ' : ''}Later effects move down one number (their event ids stay the same).`, { ok: 'Delete', danger: true }))) return;
    const removed = fxuEdit((pkg) => fxeDeleteEffect(pkg, E.ni));
    if (removed) { S.fxEdit = null; S.tab = 'fx'; renderAll(); toast('Deleted.' + (removed.length ? ' Event id' + (new Set(removed).size > 1 ? 's ' : ' ') + [...new Set(removed)].join(', ') + ' play' + (new Set(removed).size > 1 ? '' : 's') + ' nothing now.' : '')); }
  };
  fxuWirePlacements(); if (b) fxuWirePieces(b);
  if (b && E.node != null && b.nodes[E.node]) fxuWireNode(b, E.node);
  fxuWireResources();
  if (typeof fxgWire === 'function') fxgWire(I);
  if (S.readOnly) I.querySelectorAll('input, select, textarea, button:not(#fxuBack)').forEach((el) => { el.disabled = true; });
}

/* ---- placements (FEPA records) */
const FXU_ATT = [[1, 'Placed once (does not follow the bone)'], [2, 'Rotation from the character, not the bone'], [4, 'Position from the character, not the bone'], [8, 'Keeps the bone’s scale']];
function fxuPlacementsHTML() {
  const E = S.fxEdit, recs = fxuRecs(E.ni);
  let h = `<div class="fxu-inner"><p class="note">${recs.length} placement${recs.length === 1 ? '' : 's'}</p>`;
  if (!recs.length) h += '<p class="note">No placement: no event can play it yet.</p>';
  if (recs.length > 1) h += `<div class="field"><label>Placement</label><select id="fxuRec">${recs.map((r, i) => `<option value="${i}"${i === (E.rec || 0) ? ' selected' : ''}>event id ${r.id}${r.bone ? ' on ' + esc(r.bone) : ''}${r.motion ? ' (' + esc(r.motion) + ')' : ''}</option>`).join('')}</select></div>`;
  const r = recs[Math.min(E.rec || 0, recs.length - 1)];
  if (r) {
    const bones = ['', ...S.boneNames];
    h += `<p class="note">Played by events with effect id <b class="mono">${r.id}</b>.${r.mask ? ' Also a hit’s contact effect.' : ''}${r.trig ? ' The game also plays it by itself.' : ''}</p>
      <div class="field"><label>Bone</label><select id="fxuBone">${bones.map((x) => `<option value="${esc(x)}"${x === r.bone && (x ? r.model : true) ? ' selected' : ''}>${x ? esc(x) : '(where the event puts it: hit point, shot)'}</option>`).join('')}</select></div>
      ${fxuVecField('Offset (m)', 'fxuPos', r.pos)}${fxuVecField('Rotation (°)', 'fxuRot', r.rot)}${fxuVecField('Size', 'fxuScl', r.scale)}
      <div class="row2"><div class="field"><label>Lifetime (frames, 0 = its own)</label><input type="number" step="0.5" id="fxuLife" value="${Math.max(0, r.life) / 2}"></div>
      <div class="field"><label>Delay (frames)</label><input type="number" step="0.5" id="fxuDelay" value="${Math.max(0, r.delay) / 2}"></div></div>
      ${r.model ? FXU_ATT.map(([bit, lab]) => `<label class="chk"><input type="checkbox" data-att="${bit}"${r.attach & bit ? ' checked' : ''}> ${lab}</label>`).join('') : ''}`;
  }
  h += `<div class="btnrow"><button type="button" id="fxuRecAdd">+ Placement</button>${r ? '<button type="button" id="fxuRecDel" class="danger">Delete placement</button>' : ''}</div></div>`;
  return h;
}
const fxuVecField = (lab, id, v, n) => `<div class="field"><label>${lab}</label><div class="row3">${(v || []).slice(0, n || 3).map((x, k) => `<input type="number" step="any" id="${id}${k}" value="${fxuNum(x)}">`).join('')}</div></div>`;
function fxuWirePlacements() {
  const E = S.fxEdit, recs = fxuRecs(E.ni), r = recs[Math.min(E.rec || 0, recs.length - 1)];
  if ($('fxuRec')) $('fxuRec').onchange = (e) => { E.rec = +e.target.value; fxRefresh(); renderInspector(); };
  const recEdit = (fn) => fxuEdit((pkg) => { const rr = pkg.fepa.records[r.index]; fn(rr, fxeDV(rr)); }, true);
  $('fxuRecAdd').onclick = () => { if (fxuEdit((pkg) => { fxeAddFepa(pkg, E.ni, r ? pkg.fepa.records[r.index] : null); return true; })) { E.rec = recs.length; fxRefresh(); renderInspector(); } };
  if (!r) return;
  $('fxuRecDel').onclick = async () => { if (await uiConfirm(`Delete placement ${r.id}?`, `Events with event id ${r.id} will play nothing.`, { ok: 'Delete', danger: true })) { fxuEdit((pkg) => { pkg.fepa.records.splice(r.index, 1); }); E.rec = 0; } };
  $('fxuBone').onchange = (e) => recEdit((rr) => { fxeSetStr(rr, 0xA8, e.target.value); fxeSetStr(rr, 0x88, e.target.value ? (S.rig.label.replace(/_a$/, '').replace(/_(2p|3p|zako)/, '_1p') || 'p') : ''); });
  [['fxuPos', 0], ['fxuRot', 12], ['fxuScl', 24]].forEach(([id, o]) => [0, 1, 2].forEach((k) => { $(id + k).onchange = (e) => recEdit((rr, dv) => dv.setFloat32(o + 4 * k, +e.target.value, true)); }));
  $('fxuLife').onchange = (e) => recEdit((rr, dv) => dv.setInt32(0x30, Math.round(+e.target.value * 2), true));
  $('fxuDelay').onchange = (e) => recEdit((rr, dv) => dv.setInt32(0x74, Math.round(+e.target.value * 2), true));
  document.querySelectorAll('[data-att]').forEach((c) => c.onchange = () => recEdit((rr, dv) => { const v = dv.getUint32(0x7C, true), bit = +c.dataset.att; dv.setUint32(0x7C, c.checked ? v | bit : v & ~bit, true); }));
}

/* ---- pieces */
function fxuPiecesHTML(b) {
  const E = S.fxEdit;
  return `<div class="sec"><h3>Pieces <small>${b.nodes.filter(Boolean).length}</small></h3>
    <div class="fxu-list">${b.nodes.map((n, ni) => n ? `<button type="button" class="fxu-piece${E.node === ni ? ' sel' : ''}" data-ni="${ni}"><span class="mono">#${ni}</span> ${esc(fxuNodeWhat(n))}</button>` : '').join('')}</div>
    <div class="btnrow"><button type="button" id="fxuAddPiece" title="Copy a piece from any effect (this character or another)">+ Piece…</button></div></div>`;
}
function fxuWirePieces(b) {
  const E = S.fxEdit;
  document.querySelectorAll('.fxu-piece').forEach((el) => el.onclick = () => { E.node = +el.dataset.ni; renderTimeline(); renderInspector(); });
  $('fxuAddPiece').onclick = (e) => fxuPieceMenu(e.currentTarget);
}
/* the piece editor: start, life, loop, then every component of the node */
function fxuNodeHTML(b, ni) {
  const n = b.nodes[ni], E = S.fxEdit, sp = b.spawns.find((s) => s.nodes.includes(ni));
  let h = `<div class="sec fxu-node"><h3>Piece #${ni} <small>${esc(fxuNodeWhat(n))}</small></h3>
    <div class="row2"><div class="field"><label>Starts at (frame)</label><input type="number" step="0.5" id="fxuStart" value="${sp ? sp.frame / 2 : ''}"${sp ? '' : ' disabled title="Started by another piece"'}></div>
    <div class="field"><label>Life (frames)</label><input type="number" step="0.5" id="fxuLifeN" value="${n.duration / 2}"></div></div>
    <label class="chk"><input type="checkbox" id="fxuLoop"${n.flags & 2 ? ' checked' : ''}> Loops while the effect keeps playing${b.a & 2 ? '' : ' (only for effects that end when stopped)'}</label>
    ${n.flags & 2 ? `<div class="row2"><div class="field"><label>Loop from (frame)</label><input type="number" step="0.5" id="fxuL0" value="${n.loop0 / 2}"></div><div class="field"><label>Loop to</label><input type="number" step="0.5" id="fxuL1" value="${n.loop1 / 2}"></div></div>` : ''}
    <div class="btnrow"><button type="button" id="fxuMute">${E.mute && E.mute.has(ni) ? 'Show in preview' : 'Hide in preview'}</button><button type="button" id="fxuDupN">Duplicate</button><button type="button" id="fxuDelN" class="danger">Delete</button></div>`;
  for (const tr of n.tracks) h += fxuTrackHTML(n, tr, sp ? sp.frame : 0);
  return h + '</div>';
}
function fxuWireNode(b, ni) {
  const n = b.nodes[ni], E = S.fxEdit;
  const nodeEdit = (fn) => fxuEdit((pkg) => fn(pkg.fep.blockAt(n.off).b, pkg));
  $('fxuStart').onchange = (e) => fxuEdit((pkg) => fxeMoveNode(pkg.fep, pkg.fep.blockAt(b.off).b, ni, Math.max(0, Math.round(+e.target.value * 2))));
  $('fxuLifeN').onchange = (e) => nodeEdit((nd, pkg) => { fxeSetU16(nd.data, 4, Math.max(1, Math.round(+e.target.value * 2))); fxeSegLens(pkg.fep, nd); });
  $('fxuLoop').onchange = (e) => nodeEdit((nd) => { const f = fxeU16(nd.data, 0); fxeSetU16(nd.data, 0, e.target.checked ? f | 2 : f & ~2); if (e.target.checked && fxeU16(nd.data, 14) <= fxeU16(nd.data, 12)) { fxeSetU16(nd.data, 12, 0); fxeSetU16(nd.data, 14, Math.max(2, fxeU16(nd.data, 4) - 1)); } });
  if ($('fxuL0')) { $('fxuL0').onchange = (e) => nodeEdit((nd) => fxeSetU16(nd.data, 12, Math.round(+e.target.value * 2))); $('fxuL1').onchange = (e) => nodeEdit((nd) => fxeSetU16(nd.data, 14, Math.round(+e.target.value * 2))); }
  $('fxuMute').onclick = () => { E.mute = E.mute || new Set(); if (E.mute.has(ni)) E.mute.delete(ni); else E.mute.add(ni); fxRefresh(); renderAll(); };
  $('fxuDupN').onclick = () => { const sp = b.spawns.find((s) => s.nodes.includes(ni)); const k = fxuEdit((pkg) => fxeAddNode(pkg.fep, pkg.fep.blockAt(b.off).b, pkg.fep.blockAt(n.off).b, sp ? sp.frame : 0)); if (k != null) { E.node = k; renderAll(); } };
  $('fxuDelN').onclick = async () => {
    const spawners = b.nodes.some((m) => m && m !== n && m.tracks.some((t) => (t.chan === 18 || t.chan === 151) && t.segs.some((s) => fxS(fxuFep(), s.o + 16) === ni)));
    if (spawners && !(await uiConfirm('Delete this piece?', 'Another piece of this effect starts copies of this one.', { ok: 'Delete anyway', danger: true }))) return;
    fxuEdit((pkg) => fxeDeleteNode(pkg.fep, pkg.fep.blockAt(b.off).b, ni)); E.node = null; renderAll();
  };
  for (const tr of n.tracks) fxuWireTrack(b, n, tr);
}

/* ---- one component (track) */
const FXU_KIND = (ch) => (ch >= 15 && ch <= 17 ? ch - 15 : (ch - 1) % 3);              // 0 constant, 1 linear, 2 curve
const FXU_BASE = (ch) => (ch >= 15 ? 15 : ch - FXU_KIND(ch));
const FXU_FLIP = (ch) => (ch >= 1 && ch <= 6 ? [1, -1, -1] : [1, 1, 1]);            // effect space has y and z flipped
function fxuSegsOf(tr) {                          // the keys a user sees: every segment, and the init set only if no key sits at 0
  const segs = tr.segs.filter((s) => !s.init);
  if (!segs.some((s) => s.start === 0)) { const init = tr.segs.find((s) => s.init); if (init) segs.unshift(init); }
  return segs;
}
function fxuTrackHTML(n, tr, st) {
  const ch = tr.chan, fep = fxuFep(), name = FXU_CHAN[ch] || 'Channel ' + ch, id = `fxuT${tr.index}`;
  if (isValueChan(ch)) {
    const kind = FXU_KIND(ch), nv = ch >= 15 ? 4 : 3, fl = FXU_FLIP(ch), segs = fxuSegsOf(tr), unit = ch <= 3 ? 'm' : ch <= 6 ? '°' : ch <= 9 ? '%' : '0–255, 128 = normal';
    const comp = ch >= 15 ? ['R', 'G', 'B', 'A'] : ['X', 'Y (up)', 'Z (fwd)'];
    let h = `<div class="fxu-track"><div class="fxu-th"><b>${name}</b> <small>${unit}</small>
      <select data-tk="${tr.index}" title="How the value moves between keys">${['steady', 'changes at a rate', 'speeds up / slows down'].map((x, k) => `<option value="${k}"${k === kind ? ' selected' : ''}${k < kind ? ' disabled' : ''}>${x}</option>`).join('')}</select></div>
      <table class="fxu-keys"><tr><th>frame</th>${comp.map((c) => `<th>${c}</th>`).join('')}<th></th></tr>`;
    for (const s of segs) {
      const rows = [['', 0]]; if (kind >= 1) rows.push(['rate', 16]); if (kind === 2) rows.push(['accel', 32]);
      rows.forEach(([lab, base], ri) => {
        h += `<tr>${ri === 0 ? `<td class="mono">${(st + Math.max(0, s.start)) / 2}</td>` : `<td class="fxu-rl">${lab}</td>`}`;
        for (let k = 0; k < nv; k++) {
          const raw = fxF(fep, s.o + base + 4 * k) * (fl[k] || 1) * (base ? 2 : 1) * (base === 32 ? 2 : 1);
          h += `<td><input type="number" step="any" data-v="${tr.index}" data-o="${s.o + base + 4 * k}" data-f="${(fl[k] || 1) * (base === 16 ? 0.5 : base === 32 ? 0.25 : 1)}" data-init="${s.init ? 1 : 0}" data-st="${s.start}" value="${fxuNum(raw)}" title="${lab || 'value'}${base ? ' (per motion frame' + (base === 32 ? ' squared' : '') + ')' : ''}"></td>`;
        }
        h += ri === 0 ? `<td>${ch >= 15 ? `<input type="color" data-c="${tr.index}" data-o="${s.o}" data-init="${s.init ? 1 : 0}" data-st="${s.start}" value="${fxuHex(fep, s.o)}" title="colour (shown at full brightness = 128)">` : ''}${s.init || segs.length < 2 ? '' : `<button type="button" class="fxu-x" data-dk="${tr.index}" data-st="${s.start}" title="Delete this key">✕</button>`}</td>` : '<td></td>';
        h += '</tr>';
      });
    }
    h += `</table>${typeof fxgCurveSVG === 'function' ? fxgCurveSVG(n, tr, st) : ''}<button type="button" data-ak="${tr.index}" title="A key at the playhead that continues the curve from there">+ Key at the playhead</button></div>`;
    return h;
  }
  const fields = fxuFields(ch);
  if (!fields) return `<div class="fxu-track"><div class="fxu-th"><b>${name}</b></div></div>`;
  const segs = fxuSegsOf(tr), s = fxuCurSeg(tr, st), multi = segs.length > 1;
  let h = `<div class="fxu-track"><div class="fxu-th"><b>${name}</b>${multi ? ` <small>${segs.length} keys; showing the one at frame ${(st + Math.max(0, s.start)) / 2} (move the playhead for another). A change goes to every key with the same value.</small>` : ''}</div><div class="fxu-fields">`;
  for (const f of fields) h += fxuFieldHTML(tr, s, f);
  return h + '</div></div>';
}
const fxuHex = (fep, o) => '#' + [0, 1, 2].map((k) => Math.max(0, Math.min(255, Math.round(fxF(fep, o + 4 * k) * 2))).toString(16).padStart(2, '0')).join('');
function fxuCurSeg(tr, st) { const t = Math.max(0, S.fxEdit.t - st); return fxSeg(tr, Math.floor(t)) || tr.segs[0]; }
/* field specs: [label, offset, type, extra]; types f32, u8, u16, s16, i32, v2, v3, v3f (y/z flipped), v4c (colour), sel, chk, anm, mdl, tex */
function fxuFields(ch) {
  const B = [['Pull toward the camera (m)', 8, 'neg']];
  const sel = (o) => Object.entries(o).map(([v, l]) => [+v, l]);
  return {
    10: [['Flipbook', 12, 'anm'], ['Speed', 16, 'f32'], ['Depth bias on', 29, 'chk'], ...B],
    11: [['Mesh', 12, 'mdl'], ['Depth bias on', 24, 'chk'], ...B],
    20: [['Mesh', 12, 'mdl'], ['Texture offset U, V (texels)', 16, 'v2'], ['Scroll per frame U, V', 24, 'v2x2'], ['Random start', 74, 'chk'], ['Depth bias on', 72, 'chk'], ...B],
    14: [['Facing', 1, 'sel', sel({ 0: 'as placed (3D)', 1: 'the camera', 2: 'the camera, upright' })], ['Rotation order', 3, 'sel', sel({ 0: 'X Y Z', 1: 'Y X Z', 2: 'Z X Y', 3: 'X Z Y', 4: 'Y Z X', 5: 'Z Y X' })]],
    21: [['— Emission', 0, 'h'], ['Particles per frame', 44, 'f32x2'], ['Wait between bursts (frames)', 252, 'u8h'], ['Chance per particle (%)', 253, 'u8'], ['At most alive', 240, 'u16'], ['Particle life (frames)', 242, 'u16h'],
      ['— Shape', 0, 'h'], ['Shape', 254, 'sel', sel({ 0: 'box, aimed', 2: 'box, outward', 1: 'sphere, aimed', 3: 'sphere, outward', 6: 'ring, aimed', 7: 'ring, outward', 8: 'mesh points, aimed', 9: 'mesh points, outward' })],
      ['Box size / radius (m)', 0, 'v3'], ['Spread (°)', 176, 'f32'], ['Aim axis', 16, 'v3f'],
      ['— Motion', 0, 'h'], ['Use speed', 259, 'chk'], ['Speed (m/frame)', 180, 'f32x2'], ['Acceleration', 184, 'f32x4'], ['Random speed', 260, 'chk'], ['± speed', 188, 'f32x2'],
      ['Use force', 261, 'chk'], ['Force direction', 32, 'v3f'], ['Force speed', 196, 'f32x2'], ['Force acceleration (gravity)', 200, 'f32x4'],
      ['— Size', 0, 'h'], ['Size (%)', 128, 'v2'], ['Grows per frame (%)', 136, 'v2x2'], ['Growth acceleration', 144, 'v2x4'], ['Random size', 264, 'chk'], ['± size', 152, 'v2'],
      ['— Spin', 0, 'h'], ['Use spin', 266, 'chk'], ['Angle, spin /frame, spin accel', 212, 'v3s'], ['Random spin', 267, 'chk'], ['± angle, spin, accel', 224, 'v3s'],
      ['— Colour', 0, 'h'], ['Use colour', 268, 'chk'], ['Colour (128 = normal)', 64, 'v4c'], ['Change per frame', 80, 'v4x2'], ['Random colour', 269, 'chk'], ['± colour', 96, 'v4'],
      ['Fade in / out', 272, 'chk'], ['Fade in (frames)', 273, 'u8h'], ['Fade out (frames)', 274, 'u8h'],
      ['— Look', 0, 'h'], ['Flipbook', 12, 'anm'], ['Flipbook speed', 28, 'f32'], ['Mirror', 250, 'sel', sel({ 0: 'never', 1: 'random left/right', 2: 'random up/down', 3: 'random' })],
      ['Space', 251, 'sel', sel({ 255: 'follow the piece', 7: 'stay where emitted' })], ['Depth bias on', 270, 'chk'], ['Pull toward the camera (m)', 236, 'neg1']],
    105: [['Colour, newest (0–255)', 0, 'v4c255'], ['Colour, oldest', 16, 'v4c255'], ['Texture', 44, 'tex'], ['Textured', 72, 'chk'], ['Scroll per frame U, V (texels)', 48, 'v2x2'],
      ['Fade in at the head (%)', 56, 'f32'], ['Blend', 81, 'sel', sel({ 0: 'normal', 1: 'add (glow)', 2: 'subtract (dark)' })], ['Smoothing steps', 84, 'u8']],
    106: [['Blade point A (m)', 0, 'v3f'], ['Blade point B (m)', 16, 'v3f'], ['Samples (trail length)', 12, 's16']],
    107: [['Blend', 24, 'sel', sel({ 0: 'normal', 1: 'add', 2: 'subtract' })]],
    109: [['Strength', 0, 'f32'], ['Change per frame', 4, 'f32x2']],
    110: [['Blend', 17, 'sel', sel({ 0: 'normal', 1: 'add', 2: 'subtract' })], ['Passes', 19, 'u8']],
    22: [['Position range (m)', 0, 'v3']], 23: [['Rotation range (°)', 0, 'v3']], 24: [['Size range (%)', 0, 'v3']],
  }[ch] || null;
}
function fxuFieldHTML(tr, s, f) {
  const [lab, o, ty, extra] = f, fep = fxuFep(), at = s.o + o, k = `data-p="${tr.index}" data-o="${o}" data-ty="${ty}"`;
  if (ty === 'h') return `<div class="fxu-h">${esc(lab.replace(/^— /, ''))}</div>`;
  const fl = (x) => fxF(fep, at + 4 * x);
  const scale = /x2$/.test(ty) ? 2 : /x4$/.test(ty) ? 4 : 1;
  const inp = (v, i, attrs) => `<input type="number" step="any" ${k} data-i="${i || 0}" value="${fxuNum(v, 4)}" ${attrs || ''}>`;
  let ctl;
  if (ty === 'f32' || ty === 'f32x2' || ty === 'f32x4') ctl = inp(fl(0) * scale);
  else if (ty === 'neg') ctl = inp(-fl(0)); else if (ty === 'neg1') ctl = inp(-fl(0));
  else if (ty === 'u8' || ty === 'u8h') ctl = inp(fxB(fep, at) / (ty === 'u8h' ? 2 : 1));
  else if (ty === 'u16' || ty === 'u16h') ctl = inp(fxH(fep, at) / (ty === 'u16h' ? 2 : 1));
  else if (ty === 's16') ctl = inp(fxS(fep, at));
  else if (ty === 'chk') return `<label class="chk"><input type="checkbox" ${k}${fxB(fep, at) ? ' checked' : ''}> ${esc(lab)}</label>`;
  else if (ty === 'sel') ctl = `<select ${k}>${extra.map(([v, l]) => `<option value="${v}"${v === fxB(fep, at) ? ' selected' : ''}>${esc(l)}</option>`).join('')}${extra.some(([v]) => v === fxB(fep, at)) ? '' : `<option selected value="${fxB(fep, at)}">${fxB(fep, at)}</option>`}</select>`;
  else if (/^v[234]/.test(ty)) {
    const n = +ty[1], f3 = ty === 'v3f' ? [1, -1, -1] : [1, 1, 1, 1];
    ctl = `<div class="row${Math.min(n, 3)}${n === 4 ? ' row4' : ''}">${Array.from({ length: n }, (_, i) => inp(fl(i) * f3[i] * scale, i)).join('')}</div>`;
    if (ty.startsWith('v4c')) ctl += `<input type="color" ${k} data-col="1" value="#${[0, 1, 2].map((i) => Math.max(0, Math.min(255, Math.round(fl(i) * (ty === 'v4c255' ? 1 : 2)))).toString(16).padStart(2, '0')).join('')}">`;
  } else if (ty === 'anm' || ty === 'mdl' || ty === 'tex') {
    const kind = ty.toUpperCase(), cur = fxI(fep, at), list = fep.res[kind];
    ctl = `<select ${k}><option value="-1"${cur < 0 ? ' selected' : ''}>(none)</option>${list.map((r, i) => `<option value="${i}"${i === cur ? ' selected' : ''}>${kind} ${i}${r.size ? '' : ' (shared library)'} · ${(r.id >>> 0).toString(16)}</option>`).join('')}</select>`;
    if (ty === 'anm' && cur >= 0) ctl += fxuAnmThumbHTML(cur);
  }
  return `<div class="field"><label>${esc(lab)}</label>${ctl}</div>`;
}
function fxuAnmThumbHTML(i) {
  try { const u8 = fxResource(fxuFep(), 'ANM', i); if (!u8) return '<p class="note">Needs the game ISO (shared library).</p>'; const a = decodeAnm(u8); return `<p class="note">${a.clips.length} frame${a.clips.length === 1 ? '' : 's'}, ${a.total / 2} frames long${a.loop ? ', loops' : ''}; texture ${a.tex.map((x) => (x >>> 0).toString(16)).join(', ')}</p>`; } catch (e) { return ''; }
}
/* write one field of a component: to every key holding the same old value, else to the current key only */
function fxuWriteField(tr, segO, o, size, bytesFor) {
  const fep = fxuFep(), segs = tr.segs, old = fep.u8.slice(segO + o, segO + o + size);
  const same = segs.filter((s) => { const v = fep.u8.subarray(s.o + o, s.o + o + size); return v.every((x, i) => x === old[i]); });
  fxuEdit((pkg) => { for (const s of (same.length ? same : segs.filter((x) => x.o === segO))) fxeSetAt(pkg.fep, s.o + o, bytesFor(s)); }, true);
}
function fxuWireTrack(b, n, tr) {
  const fep = fxuFep(), q = (sel) => document.querySelectorAll(sel), st = (b.spawns.find((s) => s.nodes.includes(n.index)) || { frame: 0 }).frame;
  const ndOf = (pkg) => pkg.fep.blockAt(n.off).b, bodyOf = (pkg) => pkg.fep.blockAt(b.off).b;
  if (isValueChan(tr.chan)) {
    q(`[data-tk="${tr.index}"]`).forEach((el) => el.onchange = () => fxuEdit((pkg) => fxeSetChannel(pkg.fep, bodyOf(pkg), ndOf(pkg), tr.index, FXU_BASE(tr.chan) + +el.value)));
    q(`[data-v="${tr.index}"]`).forEach((el) => el.onchange = () => {
      const abs = +el.dataset.o, v = +el.value * +el.dataset.f, start = +el.dataset.st, mirror = !el.dataset.init && start === 0 ? tr.segs.find((s) => s.init) : null;
      fxuEdit((pkg) => { fxeSetAt(pkg.fep, abs, fxeF32b(v)); if (mirror) { const seg0 = tr.segs.find((s) => !s.init && s.start === 0); fxeSetAt(pkg.fep, mirror.o + (abs - seg0.o), fxeF32b(v)); } }, true);
    });
    q(`[data-c="${tr.index}"]`).forEach((el) => el.onchange = () => {
      const hex = el.value, rgb = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 2), abs = +el.dataset.o, start = +el.dataset.st;
      const init = !+el.dataset.init && start === 0 ? tr.segs.find((s) => s.init) : null;
      fxuEdit((pkg) => { rgb.forEach((c, k) => { fxeSetAt(pkg.fep, abs + 4 * k, fxeF32b(c)); if (init) fxeSetAt(pkg.fep, init.o + 4 * k, fxeF32b(c)); }); }, true);
    });
    q(`[data-dk="${tr.index}"]`).forEach((el) => el.onclick = () => fxuEdit((pkg) => fxeDeleteKey(pkg.fep, ndOf(pkg), tr.index, +el.dataset.st)));
    q(`[data-ak="${tr.index}"]`).forEach((el) => el.onclick = () => {
      const f = Math.round(S.fxEdit.t - st);
      if (f <= 0 || f >= n.duration) { toast('Move the playhead inside this piece’s life first (after its first frame).'); return; }
      fxuEdit((pkg) => fxeAddKey(pkg.fep, ndOf(pkg), tr.index, f, null));
    });
    return;
  }
  const s = fxuCurSeg(tr, st);
  q(`[data-p="${tr.index}"]`).forEach((el) => {
    const o = +el.dataset.o, ty = el.dataset.ty, i = +(el.dataset.i || 0);
    el.onchange = () => {
      const scale = /x2$/.test(ty) ? 2 : /x4$/.test(ty) ? 4 : 1;
      if (ty === 'chk') return fxuWriteField(tr, s.o, o, 1, () => Uint8Array.of(el.checked ? 1 : 0));
      if (ty === 'sel') return fxuWriteField(tr, s.o, o, 1, () => Uint8Array.of(+el.value & 255));
      if (ty === 'u8' || ty === 'u8h') return fxuWriteField(tr, s.o, o, 1, () => Uint8Array.of(Math.max(0, Math.min(255, Math.round(+el.value * (ty === 'u8h' ? 2 : 1))))));
      if (ty === 'u16' || ty === 'u16h') return fxuWriteField(tr, s.o, o, 2, () => fxeU16b(Math.max(0, Math.round(+el.value * (ty === 'u16h' ? 2 : 1)))));
      if (ty === 's16') return fxuWriteField(tr, s.o, o, 2, () => fxeS16b(Math.round(+el.value)));
      if (ty === 'anm' || ty === 'mdl' || ty === 'tex') return fxuWriteField(tr, s.o, o, 4, () => fxeI32b(+el.value));
      if (ty === 'neg' || ty === 'neg1') return fxuWriteField(tr, s.o, o, 4, () => fxeF32b(-el.value));
      if (el.dataset.col) {                       // colour picker of a v4c field
        const hex = el.value, k255 = ty === 'v4c255' ? 1 : 0.5, rgb = [1, 3, 5].map((j) => parseInt(hex.slice(j, j + 2), 16) * k255);
        return fxuEdit((pkg) => rgb.forEach((c, j) => fxeSetAt(pkg.fep, s.o + o + 4 * j, fxeF32b(c))), true);
      }
      if (/^v/.test(ty)) { const f3 = ty === 'v3f' ? [1, -1, -1] : [1, 1, 1, 1]; return fxuWriteField(tr, s.o, o + 4 * i, 4, () => fxeF32b(+el.value * f3[i] / scale)); }
      return fxuWriteField(tr, s.o, o, 4, () => fxeF32b(+el.value / scale));
    };
  });
}

/* ---- textures and flipbooks */
function fxuResourcesHTML() {
  const fi = fxInfo(S.fx, (fxuRecs(S.fxEdit.ni)[0] || {}).id);
  const ids = fxuEffectTexIds();
  return `<div class="fxu-inner">
    <div class="fxu-texes">${ids.map((id) => { const loc = S.fx.fep.res.TEX.find((r) => r.id === id && r.size); return `<div class="fxu-tex" data-id="${id}"><span class="fxu-cv" data-id="${id}"></span><small class="mono">${(id >>> 0).toString(16)}${loc ? '' : ' · shared'}</small>
      <span class="btnrow"><button type="button" data-rep="${id}" title="Replace this texture with an image (PNG, JPG…)">Replace…</button><button type="button" data-exp="${id}" title="Save as PNG">PNG</button></span></div>`; }).join('')}</div>
    <p class="note">${fi && fi.missing ? 'Some shared pieces need the game ISO. ' : ''}Replacing a shared-library texture gives this effect its own copy; replacing one of the character's own changes every effect that uses it.</p>
    <div class="btnrow"><button type="button" id="fxuNewAnm" title="An image becomes a texture and a flipbook (a grid of frames) that flipbook and particle pieces can use">+ Flipbook from an image…</button></div>
    <input type="file" id="fxuImg" accept="image/*" hidden></div>`;
}
function fxuEffectTexIds() {
  const fep = fxuFep(), b = fxuBody(), ids = new Set(); if (!b) return [];
  for (const n of b.nodes) {
    if (!n) continue;
    for (const tr of n.tracks) for (const s of tr.segs) {
      for (const [kd, fo, sz] of FXE_LINKS[tr.chan] || []) {
        const v = sz === 4 ? fxI(fep, s.o + fo) : fxS(fep, s.o + fo); if (v < 0) continue;
        if (kd === 'TEX') { const r = fep.res.TEX[v]; if (r) ids.add(r.id); continue; }
        if (kd === 'VTL') continue;
        const u8 = fxResource(fep, kd, v); if (!u8) continue;
        try { for (const t of fxeTexIdsOf(kd, u8)) ids.add(t); } catch (e) { /* skip */ }
      }
    }
  }
  return [...ids];
}
function fxuWireResources() {
  document.querySelectorAll('.fxu-cv').forEach((el) => { const cv = fxTexCanvas(fxuFep(), +el.dataset.id, 64); if (cv) el.appendChild(cv); });
  document.querySelectorAll('[data-exp]').forEach((el) => el.onclick = () => fxuExportTex(+el.dataset.exp));
  document.querySelectorAll('[data-rep]').forEach((el) => el.onclick = () => fxuPickImage((img) => fxuReplaceTex(+el.dataset.rep, img)));
  $('fxuNewAnm').onclick = () => fxuPickImage((img) => fxuNewFlipbook(img));
}
function fxuPickImage(cb) {
  const inp = $('fxuImg'); inp.value = '';
  inp.onchange = async () => {
    const f = inp.files[0]; if (!f) return;
    try { cb(await createImageBitmap(f), f.name); } catch (e) { toast('Could not read that image: ' + e.message, 'bad'); }
  };
  inp.click();
}
const fxuPow2 = (x, max) => { let p = 8; while (p < x && p < max) p *= 2; return p; };
/* image -> RGBA at PSP-friendly sizes (powers of two, 16..256 wide, 8..256 high) */
function fxuImageRGBA(img, w, h) {
  w = w || fxuPow2(Math.min(img.width, 256), 256); h = h || fxuPow2(Math.min(img.height, 256), 256); w = Math.max(16, w);
  const cv = document.createElement('canvas'); cv.width = w; cv.height = h;
  const c = cv.getContext('2d'); c.imageSmoothingQuality = 'high'; c.drawImage(img, 0, 0, w, h);
  return { w, h, rgba: new Uint8Array(c.getImageData(0, 0, w, h).data.buffer) };
}
function fxuExportTex(id) {
  const tx = fxTexture(fxuFep(), id); if (!tx) return toast('That texture needs the game ISO.');
  const t = tx.fxImg, cv = document.createElement('canvas'); cv.width = t.w; cv.height = t.h;
  const c = cv.getContext('2d'), d = c.createImageData(t.w, t.h); d.data.set(t.rgba); c.putImageData(d, 0, 0);
  cv.toBlob((bl) => { const a = document.createElement('a'); a.href = URL.createObjectURL(bl); a.download = 'tex_' + (id >>> 0).toString(16) + '.png'; a.click(); });
}
async function fxuReplaceTex(id, img) {
  const E = S.fxEdit, fep = fxuFep(), local = fep.res.TEX.findIndex((r) => r.id === id && r.size);
  const old = fxTexture(fep, id), size = old && old.fxImg ? [old.fxImg.w, old.fxImg.h] : [];
  const { w, h, rgba } = fxuImageRGBA(img, size[0], size[1]), raw = fxeEncodeTex(rgba, w, h);
  if (local >= 0) {
    const users = S.fx.fep.names.filter((_, ni) => { const prev = S.fxEdit.ni; S.fxEdit.ni = ni; const has = fxuEffectTexIds().includes(id); S.fxEdit.ni = prev; return has; }).length;
    if (users > 1 && !(await uiConfirm('Replace it everywhere?', `This texture is used by ${users} effects. Replacing it changes all of them.`, { ok: 'Replace in all' }))) return;
    fxuEdit((pkg) => fxeSetResBytes(pkg.fep, 'TEX', local, raw));
    return;
  }
  // a shared-library texture: a local copy, and local copies of this effect's flipbooks / meshes that use it
  fxuEdit((pkg) => {
    const ti = fxeAddRes(pkg.fep, 'TEX', raw), nid = fxeRefs(pkg.fep, 'TEX')[ti].id, body = pkg.fep.blockAt(fxuBody().off).b, map = new Map([[id, nid]]);
    for (const kind of ['ANM', 'MDL']) fxeRefs(pkg.fep, kind).forEach((r, ri) => {
      const u8 = r.size ? r.data.data.subarray(0, r.size) : fxResource(fep, kind, ri); if (!u8) return;
      if (!fxeTexIdsOf(kind, u8).includes(id)) return;
      const ni = fxeAddRes(pkg.fep, kind, fxeRetex(kind, u8, map));
      fxeRelink(pkg.fep, body, kind, ri, ni);
    });
  });
  void E;
}
async function fxuNewFlipbook(img) {
  const pos = (x) => (parseFloat(x) > 0 ? '' : 'Use a number above 0.');
  const r = await uiDialog({ title: 'New flipbook', body: `From a ${img.width || '?'}×${img.height || '?'} picture: a grid of frames, left to right, top to bottom.`, ok: 'Add flipbook', fields: [
    { label: 'Frames across', value: '1', type: 'number', min: 1, step: 1, half: true, check: pos },
    { label: 'Frames down', value: '1', type: 'number', min: 1, step: 1, half: true, check: pos },
    { label: 'Each frame shows for (frames)', value: '1', type: 'number', min: 0.5, step: 0.5, half: true, check: pos },
    { label: 'Blend', value: 'add', options: [['add', 'Add (glow)'], ['alpha', 'Alpha (normal)'], ['sub', 'Subtract (dark)']], half: true }] });
  if (!r) return;
  const cols = Math.max(1, parseInt(r[0], 10) || 1), rows = Math.max(1, parseInt(r[1], 10) || 1);
  const dur = Math.max(1, Math.round((parseFloat(r[2]) || 1) * 2)), blend = r[3];
  const { w, h, rgba } = fxuImageRGBA(img);
  const idx = fxuEdit((pkg) => {
    const ti = fxeAddRes(pkg.fep, 'TEX', fxeEncodeTex(rgba, w, h)), tid = fxeRefs(pkg.fep, 'TEX')[ti].id;
    return fxeAddRes(pkg.fep, 'ANM', fxeMakeAnm(tid, cols, rows, dur, true, 30, blend.startsWith('a') ? 'alpha' : blend.startsWith('s') ? 'sub' : 'add'));
  });
  if (idx != null) toast(`Added flipbook ANM ${idx} (${cols * rows} frame${cols * rows > 1 ? 's' : ''}). Pick it in a flipbook or particle piece.`);
}

/* ------------------------------------------------------------------ copying from other characters */
async function fxuPackagesOf(src) {             // [{label, pkg, fx}] for a character: 'cur', {retail: code} or {mod: name}
  const key = JSON.stringify(src);
  if (FXU.others.has(key)) return FXU.others.get(key);
  const out = [], add = (label, bytes) => { try { out.push({ label, pkg: new FxePackage(bytes), fx: loadEffects(bytes) }); } catch (e) { console.warn(e); } };
  if (src === 'cur') { const p = fxuPkg(); out.push({ label: 'this character', pkg: p, fx: S.fx }); return out; }
  if (src.retail) {
    for (const ms of [`p_${src.retail}`, `p_${src.retail}_a`]) {
      if (!isoHas(`obj/${ms}.objx`)) continue;
      const c = S.retailChars.find((x) => x.code === src.retail), cos = c && c.costumes.find(([x]) => x.endsWith('_a') === ms.endsWith('_a')); if (!cos) continue;
      const dec = await retailDecoded(`obj/${ms}.objx`, `obj/p_${src.retail}_${cos[0]}.gmo`), ent = dec['arc.json'].entries.find((x) => x.tag === '65666665');
      if (ent) add(ms, dec[ent.file]);
    }
  } else if (src.mod) {
    for (const it of await FS.list(src.mod + '/moveset').catch(() => [])) {
      if (it.kind !== 'directory') continue;
      try { const arc = await FS.json(`${src.mod}/moveset/${it.name}/arc.json`), ent = arc.entries.find((x) => x.tag === '65666665'); if (ent) add(it.name, await FS.read(`${src.mod}/moveset/${it.name}/${ent.file}`)); } catch (e) { /* skip */ }
    }
  }
  FXU.others.set(key, out);
  return out;
}
function fxuMenu(anchor, title, items, onPick) {   // a searchable list menu (the add-event menu's style)
  fxuMenuClose();
  const m = document.createElement('div'); m.className = 'menu';
  m.innerHTML = `<input type="search" placeholder="${esc(title)}"><div class="list"></div>`;
  document.body.appendChild(m); FXU.menu = m;
  menuPlace(m, anchor, 360);
  const list = m.querySelector('.list'), inp = m.querySelector('input');
  const draw = () => {
    const f = inp.value.toLowerCase(); let h = '', cat = null;
    items.forEach((it, i) => { if (f && !(it.label + ' ' + (it.cat || '')).toLowerCase().includes(f)) return; if (it.cat !== cat) { cat = it.cat; if (cat) h += `<div class="cat">${esc(cat)}</div>`; } h += `<button type="button" data-i="${i}">${esc(it.label)}${it.note ? `<small>${esc(it.note)}</small>` : ''}</button>`; });
    list.innerHTML = h || '<p class="note" style="padding:8px">Nothing matches.</p>';
    list.querySelectorAll('button').forEach((b) => b.onclick = () => { fxuMenuClose(); onPick(items[+b.dataset.i]); });
  };
  inp.oninput = draw; draw(); inp.focus();
  setTimeout(() => document.addEventListener('pointerdown', FXU.away = (e) => { if (!m.contains(e.target)) fxuMenuClose(); }), 0);
}
function fxuMenuClose() { if (FXU.menu) { FXU.menu.remove(); FXU.menu = null; } if (FXU.away) { document.removeEventListener('pointerdown', FXU.away); FXU.away = null; } }
function fxuCharItems() {
  const items = [{ label: 'This character', src: 'cur', cat: 'Characters' }];
  for (const c of S.chars || []) items.push({ label: c.label, src: { mod: c.name }, cat: 'Mod characters' });
  for (const c of S.retailChars || []) items.push({ label: c.label, src: { retail: c.code }, cat: 'From the game ISO' });
  return items;
}
async function fxuEffectItems(src) {
  const pk = await fxuPackagesOf(src), items = [];
  pk.forEach((p, pi) => p.fx.fep.names.forEach((nm, ni) => items.push({ label: fxuShort(nm.name), note: `${p.label} · effect ${ni}`, cat: p.label, p, pi, ni })));
  return items;
}
function fxuCopyMenu(anchor) {
  fxuMenu(anchor, 'Copy an effect from…', fxuCharItems(), async (ch) => {
    let items; try { items = await fxuEffectItems(ch.src); } catch (e) { toast('Could not read that character’s effects: ' + e.message, 'bad'); return; }
    fxuMenu(anchor, 'Effect to copy', items, (it) => {
      const name = (it.p.fx.fep.names[it.ni].name || 'EFFECT').replace(/_LEAF_\d+$/, '') + '_COPY_LEAF_0';
      const ni = fxuEdit((pkg) => {
        const ni2 = fxeCopyEffect(pkg, it.p.pkg, fxeEffectOfName(it.p.pkg.fep, it.ni), name);
        const t = it.p.pkg.fepa.records.find((r) => fxeU32(r, 0x28) === it.ni);
        const ri = fxeAddFepa(pkg, ni2, t);
        const r = pkg.fepa.records[ri], bone = new TextDecoder().decode(r.subarray(0xA8, 0xC8)).replace(/\0.*$/, '');
        if (bone && !S.boneNames.includes(bone) && bone !== 'root') fxeSetStr(r, 0xA8, 'root');
        fxeSetStr(r, 0xC8, '');                    // it may be played by any motion of this character
        if (new TextDecoder().decode(r.subarray(0x88, 0xA8)).replace(/\0.*$/, '')) fxeSetStr(r, 0x88, S.rig.label.replace(/_(2p|3p|zako)/, '_1p'));
        return ni2;
      });
      if (ni != null) { S.tab = 'fx'; fxuOpen(ni); toast(`Copied as effect ${ni}, event id ${fxuRecs(ni).map((r) => r.id).join(', ')}: add an Effect event with that id to play it.`); }
    });
  });
}
function fxuPieceMenu(anchor) {
  const E = S.fxEdit;
  fxuMenu(anchor, 'Copy a piece from…', fxuCharItems(), async (ch) => {
    let items; try { items = await fxuEffectItems(ch.src); } catch (e) { toast(e.message); return; }
    fxuMenu(anchor, 'Effect', items, (it) => {
      const sf = it.p.fx.fep, bi = fepBodyOfName(sf, it.ni), body = bi != null ? fepBody(sf, bi) : null;
      if (!body) return;
      const pieces = body.nodes.map((n, k) => n ? { label: `#${k} ${fxuNodeWhat(n)}`, note: n.tracks.some((t) => t.chan === 18 || t.chan === 151) ? 'starts other pieces of its effect: they are not copied' : `life ${n.duration / 2} frames`, n } : null).filter(Boolean);
      fxuMenu(anchor, 'Piece', pieces, (pc) => {
        const k = fxuEdit((pkg) => {
          const dstBody = pkg.fep.blockAt(fxuBody().off).b;
          const srcPkg = it.p.pkg === pkg ? pkg : it.p.pkg;
          return fxeCopyNode(pkg, dstBody, srcPkg, srcPkg.fep.blockAt(pc.n.off).b, Math.round(E.t));
        });
        if (k != null) { E.node = k; renderAll(); }
      });
    });
  });
}

/* ------------------------------------------------------------------ which effects came from a retail character */
// A content signature of an effect: its body, nodes, tracks and parameter blocks, with resource fields replaced by
// the resource's own content hash (ids differ between packages: copies are rebased). Names are left out, so a renamed
// copy still counts as unchanged.
function fxuHashBytes(h, u8) { for (let i = 0; i < u8.length; i++) { h ^= u8[i]; h = Math.imul(h, 16777619); } return h >>> 0; }
function fxuHashNum(h, x) { for (let k = 0; k < 4; k++) { h ^= (x >>> (8 * k)) & 255; h = Math.imul(h, 16777619); } return h >>> 0; }
function fxuResSig(fep, kind, idx, memo) {
  const key = kind + idx; if (memo.has(key)) return memo.get(key);
  const r = fep.res[kind][idx]; let h = 2166136261;
  if (!r) h = 1;
  else if (!r.size) h = fxuHashNum(h, r.id);                 // shared library: the id is the identity
  else {
    const u = Uint8Array.from(fep.u8.subarray(r.off, r.off + r.size)), dv = fxDV(u), io = kind === 'MDL' ? 8 : 4;
    dv.setUint32(io, 0, true);
    if (kind === 'MDL' || kind === 'ANM') {
      try {
        const ids = fxeTexIdsOf(kind, u), map = new Map();
        for (const t of ids) { const ti = fep.res.TEX.findIndex((x) => x.id === t); map.set(t, ti >= 0 ? fxuResSig(fep, 'TEX', ti, memo) : t); }
        const v = fxeRetex(kind, u, map); u.set(v);
      } catch (e) { /* hash as is */ }
    }
    h = fxuHashBytes(h, u);
  }
  memo.set(key, h);
  return h;
}
function fxuEffectSig(fep, ni, memo) {
  const bi = fepBodyOfName(fep, ni); if (bi == null) return 0;
  const b = fepBody(fep, bi); let h = 2166136261;
  for (const x of [b.a, b.duration, b.c, b.d]) h = fxuHashNum(h, x);
  for (const sp of b.spawns) { h = fxuHashNum(h, sp.frame); for (const n of sp.nodes) h = fxuHashNum(h, n); }
  for (const n of b.nodes) {
    if (!n) { h = fxuHashNum(h, 0xFFFFFFFF); continue; }
    for (const x of [n.flags, n.duration, n.kind, n.loop0, n.loop1]) h = fxuHashNum(h, x);
    for (const tr of n.tracks) {
      h = fxuHashNum(h, tr.chan); for (const d of tr.deps) h = fxuHashNum(h, d);
      const size = fep.paramSize(tr.chan), links = FXE_LINKS[tr.chan] || [];
      for (const s of tr.segs) {
        h = fxuHashNum(h, s.start); h = fxuHashNum(h, s.es);
        const u = Uint8Array.from(fep.u8.subarray(s.o, s.o + size)), dv = fxDV(u);
        for (const [kd, fo, sz] of links) {
          if (fo + sz > size) continue;
          const v = sz === 4 ? dv.getInt32(fo, true) : dv.getInt16(fo, true), rs = v < 0 ? 0 : fxuResSig(fep, kd, v, memo);
          if (sz === 4) dv.setUint32(fo, rs, true); else dv.setUint16(fo, rs & 0xFFFF, true);
        }
        h = fxuHashBytes(h, u);
      }
    }
  }
  return h;
}
/* signatures and names of every retail character's effects (from the ISO), built once */
async function fxuRetailIndex() {
  if (FXU.retail) return FXU.retail;
  if (FXU.retailP) return FXU.retailP;
  FXU.retailP = (async () => {
    const bySig = new Map(), byName = new Map();
    for (const c of S.retailChars || []) {
      for (const ms of [`p_${c.code}`, `p_${c.code}_a`]) {
        if (!isoHas(`obj/${ms}.objx`)) continue;
        try {
          const ent = arcRead(await isoGet(`obj/${ms}.objx`)).find((e) => e.tag === 'effe'); if (!ent) continue;
          const fx = loadEffects(ent.data); if (!fx) continue;
          const memo = new Map(), who = c.label + (ms.endsWith('_a') ? ' (assist)' : '');
          fx.fep.names.forEach((nm, ni) => {
            const sig = fxuEffectSig(fx.fep, ni, memo);
            if (sig) { if (!bySig.has(sig)) bySig.set(sig, []); if (!bySig.get(sig).includes(who)) bySig.get(sig).push(who); }
            if (!byName.has(nm.name)) byName.set(nm.name, []); if (!byName.get(nm.name).includes(who)) byName.get(nm.name).push(who);
          });
        } catch (e) { console.warn('studio: retail effects of', ms, e); }
        await new Promise((r) => setTimeout(r, 0));
      }
    }
    FXU.retail = { bySig, byName };
    return FXU.retail;
  })();
  return FXU.retailP;
}
/* per effect of the open package: {kind: 'same' | 'changed' | 'new', who} */
function fxuOrigins() {
  if (!S.fx || !FXU.retail) return null;
  if (FXU.originsOf === S.fx) return FXU.origins;
  const memo = new Map(), sigs = S.fx.fep.names.map((nm, ni) => fxuEffectSig(S.fx.fep, ni, memo));
  // the donor: the retail character most of the unchanged effects come from (shared ones prefer it)
  const count = new Map(); for (const g of sigs) for (const w of FXU.retail.bySig.get(g) || []) count.set(w, (count.get(w) || 0) + 1);
  const donor = [...count.entries()].sort((a, b) => b[1] - a[1]).map((x) => x[0])[0];
  const pick = (ws) => (ws.includes(donor) ? donor : ws[0]);
  const out = S.fx.fep.names.map((nm, ni) => {
    const same = FXU.retail.bySig.get(sigs[ni]);
    if (same) return { kind: 'same', who: pick(same) };
    const nmatch = FXU.retail.byName.get(nm.name);
    return nmatch ? { kind: 'changed', who: pick(nmatch) } : { kind: 'new' };
  });
  out.donor = donor;
  FXU.origins = out; FXU.originsOf = S.fx;
  return out;
}
