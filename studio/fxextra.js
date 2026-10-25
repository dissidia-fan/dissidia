/* Move Studio: new effects from templates, and key curves. Loaded after fxui.js.
   Templates: the smallest retail effect of each kind (one flipbook, a mesh, sparks, a screen flash, a shockwave, a
   weapon trail, particles …) from this character's package and, with the game ISO, from the retail characters; a new
   effect is a copy (with its textures) under a new name, ready to be changed: swap its flipbook with "Flipbook from an
   image", recolour it, retime it. Curves: under each value track's keys, its value over the piece's life. */

const FXT_KINDS = [['flipbook', 'One flipbook (a picture or animation)'], ['mesh', 'A mesh'], ['scrolling mesh', 'A mesh with scrolling texture'],
  ['particles', 'Particles'], ['sparks', 'Spark streaks'], ['line sparks', 'Line sparks (converging)'], ['rays', 'Light rays'], ['points', 'Points'],
  ['screen flash', 'Screen flash'], ['shockwave', 'Shockwave ring'], ['zoom blur', 'Zoom blur'], ['trail', 'Weapon trail']];
function fxtShapeOf(fep, ni) {                     // {kinds, nodes, size} of an effect's body
  const bi = fepBodyOfName(fep, ni); if (bi == null) return null;
  const b = fepBody(fep, bi); if (!b) return null;
  const kinds = new Set(); let nodes = 0;
  for (const n of b.nodes) { if (!n) continue; nodes++; for (const t of n.tracks) if (FXU_DRAW[t.chan] && t.chan !== 18 && t.chan !== 151) kinds.add(FXU_DRAW[t.chan]); }
  return { kinds, nodes, dur: b.duration };
}
async function fxtTemplates() {                    // [{kind, label, p (package), ni, from}]
  const sources = [{ label: 'this character', src: 'cur' }];
  for (const c of (S.retailChars || []).slice(0, 40)) sources.push({ label: c.label, src: { retail: c.code } });
  const best = new Map();
  for (const s of sources) {
    let pk; try { pk = await fxuPackagesOf(s.src); } catch (e) { continue; }
    for (const p of pk) {
      const fep = p.fx.fep;
      fep.names.forEach((nm, ni) => {
        const sh = fxtShapeOf(fep, ni); if (!sh || sh.kinds.size !== 1) return;
        const kind = [...sh.kinds][0], score = sh.nodes * 1000 + sh.dur + (s.src === 'cur' ? 0 : 500);   // simplest first, own package first
        const cur = best.get(kind);
        if (!cur || score < cur.score) best.set(kind, { kind, score, p, ni, from: s.label, name: nm.name });
      });
    }
    if (FXT_KINDS.every(([k]) => best.has(k) && best.get(k).score < 3000)) break;   // simple ones of every kind found
  }
  return FXT_KINDS.filter(([k]) => best.has(k)).map(([k, label]) => ({ ...best.get(k), label }));
}
function fxtMenu(anchor) {
  if (S.readOnly) { readOnlyNote(); return; }
  toast('Looking for the simplest effect of each kind…');
  fxtTemplates().then((items) => {
    if (!items.length) { toast('No template found.'); return; }
    fxuMenu(anchor, 'New effect: what kind?', items.map((x) => ({ ...x, note: `from ${x.from} · ${fxuShort(x.name)}` })), async (it) => {
      const name = await uiAsk('New effect', 'Name', 'NEW_' + it.kind.toUpperCase().replace(/[^A-Z]+/g, '_'), { ok: 'Create', hint: 'Letters, digits and _ (written in capitals).', body: `A copy of the simplest ${it.kind} effect, from ${it.from}.` });
      if (name == null) return;
      const nm = 'P0051_' + (name.trim().toUpperCase().replace(/[^A-Z0-9_]+/g, '_') || 'NEW_EFFECT') + '_LEAF_0';
      const ni = fxuEdit((pkg) => {
        const ni2 = fxeCopyEffect(pkg, it.p.pkg, fxeEffectOfName(it.p.pkg.fep, it.ni), nm);
        const t = it.p.pkg.fepa.records.find((r) => fxeU32(r, 0x28) === it.ni);
        const ri = fxeAddFepa(pkg, ni2, t), r = pkg.fepa.records[ri];
        const bone = new TextDecoder().decode(r.subarray(0xA8, 0xC8)).replace(/\0.*$/, '');
        if (bone && !S.boneNames.includes(bone) && bone !== 'root') fxeSetStr(r, 0xA8, 'root');
        fxeSetStr(r, 0xC8, '');
        if (new TextDecoder().decode(r.subarray(0x88, 0xA8)).replace(/\0.*$/, '')) fxeSetStr(r, 0x88, S.rig.label.replace(/_(2p|3p|zako)/, '_1p'));
        return ni2;
      });
      if (ni != null) { S.tab = 'fx'; fxuOpen(ni); toast(`New effect ${ni} (event id ${fxuRecs(ni).map((r) => r.id).join(', ')}), a copy of ${it.from}'s ${fxuShort(it.name)}: change its pieces, or swap its picture with “Flipbook from an image” under Textures.`); }
    });
  });
}

/* ---------------------------------------------------------------- key curves */
const FXG_COL = ['#ff6f7f', '#6fd39b', '#8fd3ff', '#e7e9f6'];
function fxgCurveSVG(n, tr, st) {                   // a value track over the piece's life, with its keys
  const fep = fxuFep(), dur = Math.max(2, n.duration || 60), nv = tr.chan >= 15 ? 4 : 3, fl = FXU_FLIP(tr.chan);
  const N = Math.min(120, dur), vals = [];
  for (let i = 0; i <= N; i++) { const g = i / N * dur, r = chanEval(fep, tr, g, null); vals.push(r ? r.v.map((x, k) => x * (fl[k] || 1)) : new Array(nv).fill(0)); }
  let lo = Infinity, hi = -Infinity; for (const v of vals) for (const x of v) { if (x < lo) lo = x; if (x > hi) hi = x; }
  if (!(hi > lo)) { hi = lo + 1; lo -= 1; }
  const W = 300, H = 64, X = (i) => (i / N * W).toFixed(1), Y = (x) => (H - 4 - (x - lo) / (hi - lo) * (H - 8)).toFixed(1);
  let p = '';
  for (let k = 0; k < nv; k++) p += `<polyline fill="none" stroke="${FXG_COL[k]}" stroke-width="1.4" points="${vals.map((v, i) => X(i) + ',' + Y(v[k])).join(' ')}"/>`;
  const keys = tr.segs.filter((s) => !s.init).map((s) => `<line x1="${(s.start / dur * W).toFixed(1)}" x2="${(s.start / dur * W).toFixed(1)}" y1="0" y2="${H}" stroke="#e8ac52" stroke-dasharray="2 2"/>`).join('');
  const t = S.fxEdit ? (S.fxEdit.t - st) : -1, ph = t >= 0 && t <= dur ? `<line x1="${(t / dur * W).toFixed(1)}" x2="${(t / dur * W).toFixed(1)}" y1="0" y2="${H}" stroke="#ffffff" stroke-opacity=".6"/>` : '';
  const zero = lo < 0 && hi > 0 ? `<line x1="0" x2="${W}" y1="${Y(0)}" y2="${Y(0)}" stroke="#363c66"/>` : '';
  return `<svg class="fxg-curve" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" data-st="${st}" data-dur="${dur}" role="img" aria-label="curve"><rect width="${W}" height="${H}" fill="rgba(255,255,255,.03)"/>${zero}${keys}${p}${ph}</svg>
    <div class="fxg-scale"><span>${fxuNum(lo)}</span><span>frame ${(st / 2)}–${((st + dur) / 2)}</span><span>${fxuNum(hi)}</span></div>`;
}
function fxgWire(I) {                              // click a curve: the playhead goes there
  I.querySelectorAll('.fxg-curve').forEach((svg) => svg.onclick = (e) => {
    const r = svg.getBoundingClientRect(), u = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
    S.fxEdit.t = +svg.dataset.st + u * +svg.dataset.dur; fxRefresh(); renderTimeline && renderTimeline(); renderInspector();
  });
}

/* ---------------------------------------------------------------- effects focus: the viewport as an effect editor */
// In the Effects tab the character, its dummy, bones and hitboxes go (switch "Show character" to bring it back), the
// character-only switches hide, and the camera frames the open effect where it plays.
const FXF = { on: false, keep: null, framed: null };
function fxfActive() { return S.tab === 'fx' && !!S.fx && !(S.rig == null) && !(S.comp && !S.fxEdit); }   // a layered effect plays on the character
function fxfApply() {
  const on = fxfActive(), showChar = on && $('fxShowChar') && $('fxShowChar').checked;
  document.body.classList.toggle('fxfocus', on);
  if (on && !FXF.on) FXF.keep = { pos: view.camera.position.clone(), target: view.controls.target.clone() };
  if (!on && FXF.on && FXF.keep) { view.camera.position.copy(FXF.keep.pos); view.controls.target.copy(FXF.keep.target); view.controls.update(); FXF.keep = null; FXF.framed = null; }
  FXF.on = on;
  const hide = on && !showChar;
  if (view.root) view.root.visible = !hide;
  if (view.bonesHelper) view.bonesHelper.visible = !hide && $('showBones').checked;
  if (typeof DM !== 'undefined' && DM.mesh && on) { DM.mesh.visible = DM.path.visible = DM.marks.visible = DM.vols.visible = false; }
  if (typeof DM_BODY !== 'undefined' && DM_BODY.root && on) DM_BODY.root.visible = false;
  if (on && S.fxEdit && FXF.framed !== S.fxEdit.ni) fxfFrame();
}
function fxfFrame() {                              // look at where the open effect plays (its placement's bone), from 5 m
  const E = S.fxEdit; if (!E) return;
  FXF.framed = E.ni;
  const r = (fxuRecs(E.ni) || [])[E.rec || 0], bi = r && r.bone && r.bone !== 'root' && S.boneIndex ? S.boneIndex.get(r.bone) : null;
  if (view.root) view.root.updateMatrixWorld(true);
  const p = bi != null && view.bones[bi] ? new THREE.Vector3().setFromMatrixPosition(view.bones[bi].matrixWorld) : new THREE.Vector3(0, 1, 0);
  view.controls.target.copy(p);
  view.camera.position.set(p.x + 3.4, p.y + 1.4, p.z + 4.0);
  view.controls.update();
}
(function fxfWire() { const cb = $('fxShowChar'); if (cb) cb.onchange = () => { fxfApply(); pose(); }; })();
