/* Move Studio: effects made of layers ("+ New effect" on the Effects tab). Loaded after fxui.js and fxextra.js.

   An effect here is what a player sees at one moment of a move: a glow on the hand, a sphere that grows, a flash, a
   shockwave, each a layer with its own start and length, the bone it sits on and its offset, turn and size. It plays
   from one frame of one animation. The game has no such thing: it plays separate effects, each started by an event.
   So the layers are stored as the game wants them, and this file only remembers how they belong together:
     - each layer is one effect of the package (named P0051_<EFFECT>_<LAYER>_LEAF_0) with one placement (FEPA
       record: bone, offset, turn, size); its look is edited in the effect editor ("Edit how it looks");
     - Build writes one Effect event per layer into the animation's timeline (frame + the layer's start, its length
       as the event's lifetime), tagged "_comp": <file name> so building again replaces exactly those.
   The file: mods/<character>/effects/<name>.json  {name, plays: {motion, frame}, layers: [{name, part, id, start,
   life}]}, frames at 30 per second. */
'use strict';
const COMP = { busy: false };
const compDir = () => (S.charDir && S.rig && !S.rig.retail ? S.charDir + 'effects/' : null);
const compObj = () => (S.comp && S.files[S.comp.path] ? S.files[S.comp.path].obj : null);
const compSlug = (p) => p.split('/').pop().replace(/\.json$/, '');
const compCode = (s) => (s || '').toUpperCase().normalize('NFD').replace(/[^A-Z0-9]+/g, '_').replace(/^_|_$/g, '').slice(0, 20) || 'EFFECT';
function compPartNi(l) { return S.fx ? S.fx.fep.names.findIndex((n) => n.name === l.part) : -1; }
function compRec(l) { return S.fx ? S.fx.fepa.find((r) => r.id === l.id) : null; }
function compMotion(o) { if (!S.gmo) return -1; const i = S.gmo.motions.findIndex((m) => m.name === o.plays.motion); return i; }

/* ------------------------------------------------------------------ loading */
async function compLoad() {
  S.comps = []; S.comp = null;
  const d = compDir(); if (!d) return;
  let items = [];
  try { items = await FS.list(d.replace(/\/$/, '')); } catch (e) { return; }
  for (const it of items) {
    if (it.kind !== 'file' || !it.name.endsWith('.json')) continue;
    const p = d + it.name;
    try { const text = await FS.text(p); S.files[p] = { obj: JSON.parse(text), text, dirty: false, comp: true }; S.comps.push(p); }
    catch (e) { console.warn('studio: effect file', p, e); }
  }
}

/* ------------------------------------------------------------------ left pane (above the stored effects) */
function compLeftHTML() {
  if (!compDir()) return '';
  let h = `<div class="fxu-top"><span class="lbl">Effects</span><span class="btnrow"><button type="button" id="compNew" class="newmove" title="An effect made of layers that plays at one frame of a move">+ New effect</button></span></div>`;
  if (!S.comps.length) h += '<p class="note">None yet. An effect is a set of layers (glows, meshes, flashes, shaders) timed together on one frame of a move.</p>';
  for (const p of S.comps) {
    const o = S.files[p].obj, sel = S.comp && S.comp.path === p;
    h += `<div class="anim-row comp-row${sel ? ' sel' : ''}" data-comp="${esc(p)}"><span>${esc(o.name || compSlug(p))}</span><small>${esc(o.plays.motion)} · frame ${o.plays.frame}</small><small>${o.layers.length} layer${o.layers.length === 1 ? '' : 's'}</small></div>`;
  }
  return h + '<div class="fxu-parts lbl" title="What the game stores: every effect of the package. The layers of your effects are among them.">Parts</div>';
}
function compLeftWire(L) {
  const nb = L.querySelector('#compNew'); if (nb) nb.onclick = compNew;
  L.querySelectorAll('[data-comp]').forEach((el) => el.onclick = () => compOpen(el.dataset.comp));
}
function compOpen(p) {
  S.comp = { path: p, sel: null }; S.fxEdit = null; S.step = null; S.sel = null; S.newMove = null;
  const o = compObj(), mi = compMotion(o);
  if (mi >= 0) { S.motion = mi; S.frame = Math.min(o.plays.frame, motionLen(mi)); playAuto(); if (S.playing) S.frame = Math.max(0, S.frame - 2); }   // with play on: from just before it starts
  pose(); renderAll();
}
function compClose() { S.comp = null; pose(); renderAll(); }
async function compNew() {
  if (S.readOnly) { readOnlyNote(); return; }
  const name = await uiAsk('New effect', 'Name', '', { placeholder: 'e.g. Alterna', ok: 'Create', hint: 'What you call it: the layers you add are timed together on one frame of the move.', check: (v) => (v.trim() ? '' : 'Give it a name.') });
  if (!name || !name.trim()) return;
  let slug = slugOf(name.trim()), p = compDir() + slug + '.json', n = 2;
  while (S.files[p]) { p = compDir() + slug + '-' + n + '.json'; n++; }
  const mi = S.motion >= 0 ? S.motion : 0;
  const obj = { _info: 'An effect made in the Move Studio (Effects tab): layers timed together on one frame of a move. Each layer is one effect of the package (part) with one placement (id). Build writes their Effect events into the animation.',
    name: name.trim(), plays: { motion: motionName(mi), frame: Math.round(S.frame || 0) }, layers: [] };
  S.files[p] = { obj, text: '', dirty: true, created: true, comp: true };
  S.comps.push(p); markDirty([p]); refreshStatus();
  compOpen(p);
  toast('Add layers with "+ Layer": a template part, a copy of any effect in the game, or a shader.');
}

/* ------------------------------------------------------------------ preview (fx.js plays these with the timeline) */
function compInsts() {
  const o = compObj(); if (!o || S.fxEdit || compMotion(o) !== S.motion) return [];
  const out = [];
  o.layers.forEach((l, i) => {
    if (l.hidden) return;
    const rec = compRec(l); if (!rec) return;
    out.push({ type: 'fx', tick: Math.round((o.plays.frame + l.start) * 2), recs: [rec], life: l.life > 0 ? Math.round(l.life * 2) : null, kind: 'comp', idx: i });
  });
  return out;
}

/* ------------------------------------------------------------------ timeline */
function compTimeline() {
  const inner = $('lanesInner'), o = compObj(), px = PX();
  const L = S.motion >= 0 ? motionLen(S.motion) : 60;
  const end = Math.max(L, ...o.layers.map((l) => o.plays.frame + l.start + l.life));
  $('tlTitle').textContent = `${o.name} — plays at ${o.plays.motion} frame ${o.plays.frame}`;
  const width = LABEL_W + (end + 10) * px, X = (f) => LABEL_W + f * px;
  let h = `<div class="ruler" id="ruler" style="width:${width}px">`;
  const stepF = px >= 14 ? 5 : px >= 7 ? 10 : 20;
  for (let f = 0; f <= end + 10; f += stepF) h += `<span style="left:${X(f)}px">${f}</span>`;
  h += '</div>';
  h += `<div class="lane cat" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">Animation</span></div>`;
  h += `<div class="lane" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">${esc(o.plays.motion)}</span>
    <div class="ev comp-anim" style="left:${X(0)}px;width:${L * px}px"></div>
    <div class="ev key comp-at" data-compbar="at" style="left:${X(o.plays.frame)}px;background:var(--ember)" title="The effect starts here (drag)"></div></div>`;
  h += `<div class="lane cat" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">Layers</span></div>`;
  o.layers.forEach((l, i) => {
    const ok = compPartNi(l) >= 0 && compRec(l), sel = S.comp.sel === i;
    h += `<div class="lane${sel ? ' selrow' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-layer="${i}" title="${esc(l.part)}">${l.hidden ? '<s>' : ''}${esc(l.name)}${l.hidden ? '</s>' : ''}${l.shader ? ' <small>shader</small>' : ''}${ok ? '' : ' <small class="bad">part missing</small>'}</span>
      <div class="ev${sel ? ' sel' : ''}" data-compbar="layer" data-i="${i}" style="left:${X(o.plays.frame + l.start)}px;width:${Math.max(3, l.life * px)}px;background:var(--c-fx);${l.hidden ? 'opacity:.3' : ''}" title="${esc(l.name)}: frames ${l.start}–${l.start + l.life} after the start (drag to move, drag the end for its length)"><span class="h r"></span></div></div>`;
  });
  if (!o.layers.length) h += `<div class="lane" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">No layers yet: + Layer in the inspector</span></div>`;
  h += `<div class="endline" style="left:${X(L)}px"></div><div class="playhead" id="playhead"></div>`;
  inner.innerHTML = h; inner.style.width = width + 'px';
  if ($('mvStrip')) $('mvStrip').hidden = true;
  updatePlayhead();
  inner.querySelectorAll('[data-layer]').forEach((el) => el.onclick = () => { S.comp.sel = +el.dataset.layer; renderTimeline(); renderInspector(); });
  inner.querySelectorAll('[data-compbar]').forEach((el) => el.addEventListener('pointerdown', (ev) => compDrag(ev, el)));
  const ruler = $('ruler');
  const scrub = (ev) => { const r = ruler.getBoundingClientRect(); setFrame(Math.round(((ev.clientX - r.left) - LABEL_W) / px * 2) / 2); };
  ruler.addEventListener('pointerdown', (ev) => { ruler.setPointerCapture(ev.pointerId); scrub(ev); ruler.onpointermove = scrub; ruler.onpointerup = () => { ruler.onpointermove = null; }; });
}
function compDrag(ev, el) {
  ev.preventDefault();
  const o = compObj(), kind = el.dataset.compbar, i = el.dataset.i != null ? +el.dataset.i : null, x0 = ev.clientX, px = PX();
  const resize = ev.target.classList.contains('r'), l = i != null ? o.layers[i] : null;
  if (i != null && S.comp.sel !== i) { S.comp.sel = i; renderInspector(); }
  el.setPointerCapture(ev.pointerId);
  let d = 0;
  el.onpointermove = (m) => {
    d = Math.round((m.clientX - x0) / px * 2) / 2;
    if (kind === 'at') el.style.left = (LABEL_W + Math.max(0, o.plays.frame + d) * px) + 'px';
    else if (resize) el.style.width = Math.max(3, (l.life + d) * px) + 'px';
    else el.style.left = (LABEL_W + Math.max(0, o.plays.frame + l.start + d) * px) + 'px';
  };
  el.onpointerup = () => {
    el.onpointermove = null; el.onpointerup = null;
    if (!d) { renderTimeline(); return; }
    compEdit(() => {
      if (kind === 'at') o.plays.frame = Math.max(0, o.plays.frame + d);
      else if (resize) l.life = Math.max(0.5, l.life + d);
      else l.start = Math.max(-o.plays.frame, l.start + d);
    });
  };
}
function compEdit(fn) { edit([S.comp.path], fn); pose(); renderAll(); }

/* ------------------------------------------------------------------ inspector */
function compInspector(I) {
  const o = compObj(), lay = S.comp.sel != null ? o.layers[S.comp.sel] : null;
  const motions = S.gmo ? S.gmo.motions.map((m) => m.name) : [o.plays.motion];
  const built = compBuilt(o);
  let h = `<div class="sec"><h3>${esc(o.name)} <span class="btnrow"><button type="button" id="compBack" title="Back to the effects list">✕</button></span></h3>
    <div class="field"><label>Name</label><input type="text" id="compName" value="${esc(o.name)}"></div>
    <div class="row2"><div class="field"><label>Plays in</label><select id="compMotion">${motions.map((n) => `<option${n === o.plays.motion ? ' selected' : ''}>${esc(n)}</option>`).join('')}</select></div>
      <div class="field"><label>At frame</label><input type="number" step="0.5" min="0" id="compFrame" value="${o.plays.frame}"></div></div>
    <p class="note">${compUsedBy(o)}</p>
    <div class="btnrow"><button type="button" id="compBuild" class="primary" title="Write one Effect event per layer into ${esc(o.plays.motion)} (replaces what an earlier build wrote)">${built === 'yes' ? 'Built ✓ (build again)' : built === 'stale' ? 'Build (changed since)' : 'Build into ' + esc(o.plays.motion)}</button>
      <button type="button" id="compPlay">▶ Play from the start</button></div></div>
    <div class="sec"><h3>Layers <small>${o.layers.length}</small> <span class="btnrow"><button type="button" id="compAdd">+ Layer</button></span></h3>
    <div class="comp-layers">${o.layers.map((l, i) => `<button type="button" class="comp-layer${S.comp.sel === i ? ' sel' : ''}" data-layer="${i}"><span>${esc(l.name)}</span><small>${l.start}–${l.start + l.life}${l.shader ? ' · shader' : ''}</small></button>`).join('') || '<p class="note">No layers yet.</p>'}</div></div>`;
  if (lay) h += compLayerHTML(lay);
  I.innerHTML = h;
  $('compBack').onclick = compClose;
  $('compName').onchange = (e) => compEdit(() => { o.name = e.target.value.trim() || o.name; });
  $('compMotion').onchange = (e) => { compEdit(() => { o.plays.motion = e.target.value; }); compOpen(S.comp.path); };
  $('compFrame').onchange = (e) => compEdit(() => { o.plays.frame = Math.max(0, Math.round(+e.target.value * 2) / 2); });
  $('compBuild').onclick = compBuild;
  $('compPlay').onclick = () => { setFrame(Math.max(0, o.plays.frame - 2)); S.playing = true; $('playBtn').textContent = '❚❚'; };
  $('compAdd').onclick = (e) => compAddMenu(e.currentTarget);
  I.querySelectorAll('.comp-layer').forEach((b) => b.onclick = () => { S.comp.sel = +b.dataset.layer; renderTimeline(); renderInspector(); });
  if (lay) compLayerWire(lay);
}
function compUsedBy(o) {
  return `In game it starts at frame ${o.plays.frame} of <span class="mono">${esc(o.plays.motion)}</span>${(S.usedBy && S.usedBy.get(compMotion(o)) || []).length ? ' (' + esc(S.usedBy.get(compMotion(o)).join(', ')) + ')' : ''}. The preview plays it with the animation: press play or scrub.`;
}
function compLayerHTML(l) {
  const r = compRec(l), ni = compPartNi(l), bones = ['root', ...S.boneNames.filter((b) => b !== 'root')];
  let h = `<div class="sec" id="compLayerSec"><h3>${esc(l.name)} <small class="mono">${ni >= 0 ? 'effect ' + ni : 'missing'} · id ${l.id}</small></h3>
    <div class="field"><label>Layer name</label><input type="text" id="clName" value="${esc(l.name)}"></div>
    <div class="row2"><div class="field"><label>Starts (frames after the effect)</label><input type="number" step="0.5" id="clStart" value="${l.start}"></div>
      <div class="field"><label>Length (frames)</label><input type="number" step="0.5" min="0.5" id="clLife" value="${l.life}"></div></div>`;
  if (r) h += `<div class="field"><label>On the bone</label><select id="clBone">${bones.map((b) => `<option${b === (r.bone || 'root') ? ' selected' : ''}>${esc(b)}</option>`).join('')}</select></div>
    ${fxuVecField('Offset (m)', 'clPos', r.pos)}${fxuVecField('Turn (°)', 'clRot', r.rot)}${fxuVecField('Size', 'clScl', r.scale)}
    <label class="chk"><input type="checkbox" id="clOnce"${r.attach & 1 ? ' checked' : ''}> Placed once (stays where it starts, doesn't follow the bone)</label>`;
  else h += '<p class="warnline">Its part is not in the effect package any more.</p>';
  h += `<div class="btnrow" style="margin-top:8px">${ni >= 0 ? '<button type="button" id="clLook">Edit how it looks ›</button>' : ''}<button type="button" id="clHide">${l.hidden ? 'Show' : 'Hide'}</button><button type="button" id="clDup">Duplicate</button><button type="button" id="clDel" class="danger">Delete layer</button></div>`;
  if (typeof compShaderHTML === 'function') h += compShaderHTML(l);
  return h + '</div>';
}
function compLayerWire(l) {
  const o = compObj(), i = o.layers.indexOf(l), r = compRec(l), ni = compPartNi(l);
  $('clName').onchange = (e) => {
    const nm = e.target.value.trim(); if (!nm) return;
    const part = compPartName(o, nm, l);
    if (ni >= 0) fxuEdit((pkg) => fxeRename(pkg.fep, ni, part));
    compEdit(() => { l.name = nm; l.part = part; });
  };
  $('clStart').onchange = (e) => compEdit(() => { l.start = Math.round(+e.target.value * 2) / 2; });
  $('clLife').onchange = (e) => compEdit(() => { l.life = Math.max(0.5, Math.round(+e.target.value * 2) / 2); });
  if (r) {
    const recEdit = (fn) => fxuEdit((pkg) => { const rr = pkg.fepa.records[r.index]; fn(rr, fxeDV(rr)); }, true);
    $('clBone').onchange = (e) => recEdit((rr) => { fxeSetStr(rr, 0xA8, e.target.value); fxeSetStr(rr, 0x88, compModel()); });
    [['clPos', 0], ['clRot', 12], ['clScl', 24]].forEach(([id, off]) => [0, 1, 2].forEach((k) => { $(id + k).onchange = (e) => recEdit((rr, dv) => dv.setFloat32(off + 4 * k, +e.target.value, true)); }));
    $('clOnce').onchange = (e) => recEdit((rr, dv) => { const v = dv.getUint32(0x7C, true); dv.setUint32(0x7C, e.target.checked ? v | 1 : v & ~1, true); });
  }
  if ($('clLook')) $('clLook').onclick = () => { S.tab = 'fx'; fxuOpen(ni); };
  $('clHide').onclick = () => compEdit(() => { l.hidden = !l.hidden; if (!l.hidden) delete l.hidden; });
  $('clDup').onclick = () => compAddLayer({ name: l.name + ' 2', srcPkg: () => fxuPkg(), srcNi: ni, rec: r ? fxuPkg().fepa.records[r.index] : null, start: l.start, life: l.life, shader: l.shader ? JSON.parse(JSON.stringify(l.shader)) : null });
  $('clDel').onclick = async () => {
    if (!(await uiConfirm(`Delete the layer ${l.name}?`, `Its part (effect ${ni}) is deleted from the package too. Undo brings it back.`, { ok: 'Delete', danger: true }))) return;
    if (ni >= 0) fxuEdit((pkg) => fxeDeleteEffect(pkg, ni));
    compEdit(() => { o.layers.splice(i, 1); S.comp.sel = null; });
  };
  if (typeof compShaderWire === 'function') compShaderWire(l);
}
const compModel = () => (S.rig && S.rig.label ? S.rig.label.replace(/_a$/, '').replace(/_(2p|3p|zako)/, '_1p') : 'p');
function compPartName(o, layerName, self) {
  const base = 'P0051_' + compCode(o.name) + '_' + compCode(layerName);
  const taken = new Set((S.fx ? S.fx.fep.names : []).map((n) => n.name));
  if (self) taken.delete(self.part);
  let nm = base + '_LEAF_0', k = 2;
  while (taken.has(nm)) { nm = base + k + '_LEAF_0'; k++; }
  return nm;
}

/* ------------------------------------------------------------------ adding layers */
function compAddMenu(anchor) {
  if (S.readOnly) { readOnlyNote(); return; }
  const items = [];
  for (const [k, lab] of FXT_KINDS) items.push({ label: lab, cat: 'A template part', tpl: k });
  items.push({ label: 'Copy an effect from…', cat: 'From the game or a mod', copy: true, note: 'any character’s effect, with its textures' });
  if (typeof compShaderTemplates === 'function') for (const t of compShaderTemplates()) items.push({ label: t.label, cat: 'A shader layer (needs the recomp)', shader: t, note: t.note });
  fxuMenu(anchor, 'Add a layer…', items, async (it) => {
    if (it.copy) {
      fxuMenu(anchor, 'Copy an effect from…', fxuCharItems(), async (ch) => {
        let list; try { list = await fxuEffectItems(ch.src); } catch (e) { toast('Could not read that character’s effects: ' + e.message, 'bad'); return; }
        fxuMenu(anchor, 'Effect to copy', list, (x) => {
          const t = x.p.pkg.fepa.records.find((r) => fxeU32(r, 0x28) === x.ni);
          compAddLayer({ name: fxuShort(x.p.fx.fep.names[x.ni].name).toLowerCase().replace(/_/g, ' '), srcPkg: () => x.p.pkg, srcNi: x.ni, rec: t || null });
        });
      });
      return;
    }
    toast('Finding the simplest effect of that kind…');
    let tpls; try { tpls = await fxtTemplates(); } catch (e) { tpls = []; }
    const want = it.tpl || (it.shader && it.shader.carrier) || 'flipbook', t = tpls.find((x) => x.kind === want) || tpls.find((x) => x.kind === 'flipbook');
    if (!t) { toast('No template of that kind found (set the game ISO for more).'); return; }
    const rec = t.p.pkg.fepa.records.find((r) => fxeU32(r, 0x28) === t.ni);
    compAddLayer({ name: it.shader ? it.shader.name : it.tpl, srcPkg: () => t.p.pkg, srcNi: t.ni, rec: rec || null, shader: it.shader ? it.shader.make() : null });
  });
}
/* one layer: the part copied into the package (with its textures) and one placement on the root, at the playhead */
function compAddLayer(x) {
  const o = compObj(), part = compPartName(o, x.name);
  const res = fxuEdit((pkg) => {
    const src = x.srcPkg();
    const ni2 = fxeCopyEffect(pkg, src, fxeEffectOfName(src.fep, x.srcNi), part);
    const ri = fxeAddFepa(pkg, ni2, x.rec), rr = pkg.fepa.records[ri], dv = fxeDV(rr);
    const bone = new TextDecoder().decode(rr.subarray(0xA8, 0xC8)).replace(/\0.*$/, '');
    if (!bone || !S.boneNames.includes(bone)) fxeSetStr(rr, 0xA8, 'root');
    fxeSetStr(rr, 0x88, compModel()); fxeSetStr(rr, 0xC8, '');      // on this character, in any animation
    dv.setInt32(0x30, 0, true); dv.setInt32(0x74, 0, true);           // the event gives the lifetime; no delay
    dv.setUint32(0x34, 0, true); dv.setUint32(0x3C, 0, true);         // not played by the engine, not a hit's contact effect
    return { id: fxeU32(rr, 0x24) };
  });
  if (!res) return;
  const now = Math.max(0, Math.round((S.frame - o.plays.frame) * 2) / 2);
  const ni = S.fx.fep.names.findIndex((n) => n.name === part), b = ni >= 0 ? fxuBody(ni) : null;
  const life = x.life || (b ? Math.max(5, Math.min(60, b.duration / 2)) : 15);
  compEdit(() => { o.layers.push({ name: x.name, part, id: res.id, start: x.start != null ? x.start : now, life, ...(x.shader ? { shader: x.shader } : {}) }); S.comp.sel = o.layers.length - 1; });
  if (x.shader && typeof compShaderAdded === 'function') compShaderAdded(o.layers[o.layers.length - 1]);
}

/* ------------------------------------------------------------------ build: the layers' events in the animation */
function compEvents(o, slug) {
  return o.layers.filter((l) => !l.hidden && compRec(l)).map((l) => ({ frame: Math.max(0, Math.round((o.plays.frame + l.start) * 2)), op: 'effect', effect: l.id, args: [Math.round(l.life * 2)], _comp: slug }));
}
function compBuilt(o) {
  const mi = compMotion(o), p = mi >= 0 && S.tlOfMotion && S.tlOfMotion.get(mi), slug = compSlug(S.comp.path);
  const mine = (t) => t.keyed.filter((e) => e._comp === slug).map((e) => [e.frame, e.effect, (e.args || [])[0]].join(':')).sort().join('|');
  let any = false;
  for (const q of S.tlPaths || []) { const t = F(q); if (t && t.keyed.some((e) => e._comp === slug)) any = true; }
  if (!any) return 'no';
  const want = compEvents(o, slug).map((e) => [e.frame, e.effect, e.args[0]].join(':')).sort().join('|');
  return p && mine(F(p)) === want ? 'yes' : 'stale';
}
function compBuild() {
  const o = compObj(), slug = compSlug(S.comp.path), mi = compMotion(o);
  const p = mi >= 0 && S.tlOfMotion.get(mi);
  if (!p) { toast(`${o.plays.motion} has no timeline in this moveset, so nothing can start the effect there.`); return; }
  const evs = compEvents(o, slug);
  const paths = [...new Set([p, ...(S.tlPaths || []).filter((q) => F(q) && F(q).keyed.some((e) => e._comp === slug))])];
  if (typeof compShaderBuild === 'function' && o.layers.some((l) => l.shader) && !compShaderBuild(o)) return;
  edit(paths, () => {
    for (const q of paths) F(q).keyed = F(q).keyed.filter((e) => e._comp !== slug);
    F(p).keyed.push(...evs);
    F(p).keyed.sort((a, b) => a.frame - b.frame);
  });
  renderAll();
  toast(`${evs.length} Effect event${evs.length === 1 ? '' : 's'} in ${o.plays.motion} from frame ${o.plays.frame}. Save, then start the game.`);
}

/* ------------------------------------------------------------------ hooks */
(function compWire() {
  for (const id of ['tabMoves', 'tabAnims', 'tabChar']) { const b = $(id); if (b) b.addEventListener('click', () => { if (S.comp) { S.comp = null; pose(); renderAll(); } }); }
})();
