/* Move Studio: an animation's travel (2026-10-30: "there's no way to edit the data in the air motion to make him
   launch higher"). How far a move carries the fighter is root motion: the animated trans_include bone, its per-tick change
   moves him (docs/archive/attacker-movement). Air motion only lets him leave the floor (no ground snapping); the height
   itself is this travel. The Travel section (the animation's inspector, and an Air motion / Ground follow / Landing
   allowed event's) scales it over a range of frames: height and forward separately, the part after the range carried
   along, so the rest of the animation follows on from where the range now ends. A uniform height scale keeps the frames
   where he crosses the floor, so a dive still lands where it did.
   Written per frame (linear keys, full floats) into every model of this moveset that has the animation. Ghost skeletons
   kept still in the world by cancelling his travel (g_root, the warp ghost) get the same change taken off, so they stay
   where they were left. Hits that place the foe relative to him (stagger Up / Forward) don't change with it.
   Loaded after copy.js (cpTree, cpAnimModel, cpTreeBytes, cpRigChanged). */
const TRV = { bone: 'trans_include', ghost: 'g_root', ops: new Set([0x000A, 0x0015, 0x002D]) };

function trvSample(gmo, mi, name) {               // a bone's translation keyed in motion mi, at every whole frame
  const m = gmo && gmo.motions[mi]; if (!m) return null;
  const bi = gmo.animBones.findIndex((b) => b.name === name); if (bi < 0) return null;
  const bd = m.binds.find((b) => b.bone === bi && b.cmd === 0x48); if (!bd || !m.curves[bd.fc]) return null;
  const f0 = 0, f1 = Math.ceil(m.end), fr = [];   // from frame 0: m.start is the loop start (a victory loops its tail), and the curve is written back from these frames
  for (let f = f0; f <= f1; f++) fr.push(f);
  return { fc: bd.fc, frames: fr, vals: fr.map((f) => evalCurve(m.curves[bd.fc], f)) };
}
function trvInfo(gmo, mi) {                       // {peak, peakF, fwd, side, lands} of the whole animation, or null
  const s = trvSample(gmo, mi, TRV.bone); if (!s) return null;
  const v = s.vals, y0 = v[0][1];
  let k = 0; v.forEach((x, i) => { if (x[1] > v[k][1]) k = i; });
  let land = null;
  if (v[k][1] - y0 > 0.05) for (let i = k + 1; i < v.length; i++) if (v[i][1] <= y0 + 1e-4) { land = s.frames[i]; break; }
  const e = v[v.length - 1];
  return { peak: v[k][1] - y0, peakF: s.frames[k], fwd: e[2] - v[0][2], side: e[0] - v[0][0], lands: land, first: s.frames[0], last: s.frames[s.frames.length - 1], moves: v.some((x) => Math.abs(x[0] - v[0][0]) + Math.abs(x[1] - y0) + Math.abs(x[2] - v[0][2]) > 1e-3) };
}
/* the scale factors that turn the range's highest point into `height` m (above the range's first frame) and its forward
   travel into `fwd` m */
function trvFactors(gmo, mi, from, to, height, fwd) {
  const s = trvSample(gmo, mi, TRV.bone); if (!s) return null;
  const a = Math.max(0, s.frames.indexOf(Math.round(from))), b = Math.min(s.vals.length - 1, Math.max(a, s.frames.indexOf(Math.round(to)) < 0 ? s.vals.length - 1 : s.frames.indexOf(Math.round(to))));
  const base = s.vals[a]; let top = -Infinity;
  for (let i = a; i <= b; i++) top = Math.max(top, s.vals[i][1] - base[1]);
  const dz = s.vals[b][2] - base[2];
  return { a, b, top, dz, kh: top > 0.01 && height != null ? height / top : 1, kf: Math.abs(dz) > 0.01 && fwd != null ? fwd / dz : 1 };
}
function trvCurve(c, frames, vals) {              // an FCURVE chunk's payload: linear, full floats, one key a frame
  const dv0 = new DataView(c.payload.buffer, c.payload.byteOffset, c.payload.byteLength), extra = dv0.getUint32(12, true);
  const n = frames.length, out = new Uint8Array(16 + n * 16), dv = new DataView(out.buffer);
  dv.setUint32(0, 1, true); dv.setUint32(4, 3, true); dv.setUint32(8, n, true); dv.setUint32(12, extra, true);
  frames.forEach((f, i) => { const o = 16 + i * 16; dv.setFloat32(o, f, true); for (let d = 0; d < 3; d++) dv.setFloat32(o + 4 + 4 * d, vals[i][d], true); });
  c.payload = out;
  const h = new DataView(c.header.buffer, c.header.byteOffset, c.header.byteLength);    // the block's own size field
  if (c.header.length >= 8) h.setUint32(0, 8 + c.header.length + out.length, true);
}
/* motion mi of one model, its travel scaled over frames [from, to]: -> new GMO bytes, or null if it has none */
function trvApplyBytes(bytes, mi, from, to, kh, kf) {
  const gmo = parseGMO(bytes), s = trvSample(gmo, mi, TRV.bone); if (!s) return null;
  const tree = cpTree(bytes), am = cpAnimModel(tree), mc = am && gcFind(am, 0x0B)[mi]; if (!mc) return null;
  const fcs = gcFind(mc, GC.FCURVE);
  const a = Math.max(0, s.frames.indexOf(Math.round(from))), bi = s.frames.indexOf(Math.round(to)), b = bi < 0 ? s.vals.length - 1 : Math.max(a, bi);
  const base = s.vals[a], nv = s.vals.map((v, i) => {
    if (i < a) return v.slice();
    const j = Math.min(i, b), w = s.vals[j];
    const sc = [base[0] + kf * (w[0] - base[0]), base[1] + kh * (w[1] - base[1]), base[2] + kf * (w[2] - base[2])];
    return i <= b ? sc : [v[0] + sc[0] - w[0], v[1] + sc[1] - w[1], v[2] + sc[2] - w[2]];
  });
  trvCurve(fcs[s.fc], s.frames, nv);
  const g = trvSample(gmo, mi, TRV.ghost);
  if (g && g.frames.length === s.frames.length) {
    const gv = g.vals.map((v, i) => [v[0] - (nv[i][0] - s.vals[i][0]), v[1] - (nv[i][1] - s.vals[i][1]), v[2] - (nv[i][2] - s.vals[i][2])]);
    if (g.fc !== s.fc) trvCurve(fcs[g.fc], g.frames, gv);
  }
  return cpTreeBytes(tree);
}
async function trvApply(mi, from, to, kh, kf) {
  if (S.readOnly) { readOnlyNote(); return; }
  if (!S.rig || S.rig.retail) { toast('Pick the mod character (not the retail original) to change its animations.'); return; }
  const name = motionName(mi), rigs = S.rigs.filter((r) => r.ms === S.rig.ms && !r.retail), todo = [];
  for (const r of rigs) {
    const bytes = S.files[r.gmo] && S.files[r.gmo].bin ? S.files[r.gmo].bin : new Uint8Array(await FS.read(r.gmo));
    const g = parseGMO(bytes);
    if (!g.motions[mi] || g.motions[mi].name !== name || !trvSample(g, mi, TRV.bone)) continue;
    todo.push({ r, bytes });
  }
  if (!todo.length) { toast(name + ' has no root travel to change.'); return; }
  for (const x of todo) { if (!S.files[x.r.gmo]) S.files[x.r.gmo] = { bin: x.bytes, binOrig: x.bytes, dirty: false, gmo: true }; S.files[x.r.gmo].gmo = true; }   // (undo re-reads the rig)
  edit(todo.map((x) => x.r.gmo), () => { for (const x of todo) S.files[x.r.gmo].bin = trvApplyBytes(x.bytes, mi, from, to, kh, kf) || x.bytes; });
  cpRigChanged(); runChecks(); renderAll();
  const i = trvInfo(parseGMO(S.files[todo[0].r.gmo].bin), mi);   // (S.gmo is re-read asynchronously when a skin is shown)
  toast(`${name}: travel ×${kh.toFixed(2)} up, ×${kf.toFixed(2)} forward over frames ${from}–${to}${i ? ` → highest ${i.peak.toFixed(2)} m, ${i.fwd.toFixed(2)} m forward` : ''}. Undo puts it back.`, 'ok');
}

/* ---------------------------------------------------------------- the inspector section */
const trvFmt = (x) => (Math.round(x * 100) / 100).toFixed(2);
function trvSectionHTML(mi, fromEvent) {
  if (!S.gmo || !S.rig) return '';
  const i = trvInfo(S.gmo, mi); if (!i) return fromEvent ? `<div class="sec"><h3>Travel</h3><p class="note">${esc(motionName(mi))} has no movement baked in: he stays where he is unless a dash, lunge or homing event moves him.</p></div>` : '';
  const ro = S.rig.retail || S.readOnly ? ' disabled' : '';
  // the inputs start empty with the animation's own values as placeholders (2026-10-31: "make them the input
  // placeholder"); Apply and Reset show once something is typed
  const tipH = 'His highest point in the range, above where he is at its first frame. The whole climb and fall are scaled, so a drop back to the floor still lands on the same frame. Hits that hold the foe at a height from him (stagger Up) don\'t change with it.';
  const tipF = 'How far forward he goes over the range (sideways travel scales with it). Written into the animation (every model of the moveset that has it); the warp ghost stays where he left it.';
  return `<div class="sec" id="trvSec"><h3>Travel${fTip(`${fromEvent ? 'This event only says whether he snaps to the floor. ' : ''}Movement baked into the animation. Edit to tweak the distance and height the motion covers.`)}</h3>
    <div class="row2">
      <div class="field"><label>Height (m)${fTip(tipH)}</label><input type="number" id="trvH" step="0.1" placeholder="${trvFmt(i.peak)}"${ro}></div>
      <div class="field"><label>Distance (m)${fTip(tipF)}</label><input type="number" id="trvF" step="0.1" placeholder="${trvFmt(i.fwd)}"${ro}></div>
    </div>
    <div class="row2">
      <div class="field"><label>From frame${fTip('The part of the animation to scale. Frames after it carry on from where it now ends.')}</label><input type="number" id="trvFrom" min="${i.first}" max="${i.last}" step="1" placeholder="${i.first}"${ro}></div>
      <div class="field"><label>To frame</label><input type="number" id="trvTo" min="${i.first}" max="${i.last}" step="1" placeholder="${i.last}"${ro}></div>
    </div>
    <p class="note" id="trvNow" data-rest="${esc(`Peaks at frame ${i.peakF}${i.lands != null ? `, lands at frame ${i.lands}` : ''}${Math.abs(i.side) > 0.05 ? `, ${trvFmt(i.side)} m to the side` : ''}.`)}"></p>
    <div class="btnrow" id="trvBtns" hidden><button type="button" id="trvGo" class="primary"${ro}>Apply</button><button type="button" id="trvReset">Reset</button></div></div>`;
}
function trvWire(mi) {
  const go = $('trvGo'); if (!go) return;
  const v = (id) => { const el = $(id); return el.value === '' ? +el.placeholder : +el.value; };
  const read = () => {
    const from = v('trvFrom'), to = v('trvTo'), k0 = trvFactors(S.gmo, mi, from, to, null, null);
    const n = (id) => ($(id).value === '' ? null : +$(id).value), f = trvFactors(S.gmo, mi, from, to, n('trvH'), n('trvF'));   // empty = as it is
    return { from, to, k0, f };
  };
  const typed = () => ['trvH', 'trvF', 'trvFrom', 'trvTo'].some((id) => $(id).value !== '');
  const show = () => {
    const { from, to, f } = read(), n = $('trvNow'); if (!n) return;
    const same = !f || (Math.abs(f.kh - 1) < 1e-3 && Math.abs(f.kf - 1) < 1e-3);
    n.textContent = to < from ? 'To must come after From.' : same ? n.dataset.rest
      : [Math.abs(f.kh - 1) >= 1e-3 ? `Height ×${f.kh.toFixed(2)}` : '', Math.abs(f.kf - 1) >= 1e-3 ? `distance ×${f.kf.toFixed(2)}` : ''].filter(Boolean).join(', ').replace(/^d/, 'D') + ` over frames ${from}–${to}.`;
    $('trvBtns').hidden = !typed();
    go.disabled = same || to < from || S.readOnly || S.rig.retail;
  };
  ['trvFrom', 'trvTo'].forEach((id) => $(id).oninput = () => {     // a new range: its current height / distance as the placeholders
    const { k0 } = read(); if (k0) { $('trvH').placeholder = trvFmt(k0.top); $('trvF').placeholder = trvFmt(k0.dz); } show();
  });
  ['trvH', 'trvF'].forEach((id) => $(id).oninput = show);
  show();
  $('trvReset').onclick = () => { ['trvH', 'trvF', 'trvFrom', 'trvTo'].forEach((id) => { $(id).value = ''; }); $('trvFrom').oninput(); };
  go.onclick = () => { const { from, to, f } = read(); if (f) trvApply(mi, from, to, f.kh, f.kf); };
}

/* the section never takes the inspector down with it (2026-10-30: "selecting air motion doesnt show anything"):
   an error shows in the section, with its message, and in the console */
function trvSafeHTML(mi, fromEvent) {
  try { return trvSectionHTML(mi, fromEvent); }
  catch (e) { console.error('studio: Travel', e); return `<div class="sec"><h3>Travel</h3><p class="warnline">Could not read this animation's travel: ${esc(e && e.message || String(e))}</p></div>`; }
}
function trvSafeWire(mi) { try { trvWire(mi); } catch (e) { console.error('studio: Travel', e); } }
