/* Move Studio: keying the game camera (2026-11-01: Noctis's new entry should show him warping up, then pan down
   as he falls). The game films an animation through its `camera_include` bone (burst.js): position = the eye, its −Z
   the view direction with +Y up, its Z scale = the field of view in degrees. Retail keys it in EX Bursts, the 051/060/
   07x/500 scenes and some entries and victories (Vaan _900A, Shantotto _910A), so the engine reads it there.
   The Game camera section (the animation's inspector) keys the viewport's view at the current frame into that bone:
   T (linear), R (slerp) and S = (1, 1, fov) (linear), full floats. A key can be a cut (a key just before it holds the
   previous shot). Clearing puts the bone back at rest (scale 1), which the game and the studio read as "no camera".
   Written into every model of this moveset that has the animation. Bones the exporter bound to a shared curve get
   curves of their own. Loaded after timing.js (acRead, acWrite, acSample, acRow). */
const CAMK = { bone: 'camera_include', cut: 0.01 };

function camkBoneIndex(gmo) { return gmo ? gmo.animBones.findIndex((b) => b.name === CAMK.bone) : -1; }
function camkTracks(gmo, mi) {                    // {T, R, S} curves of the camera bone in motion mi (parsed), or null
  const bi = camkBoneIndex(gmo), m = gmo && gmo.motions[mi]; if (bi < 0 || !m) return null;
  const o = {};
  for (const b of m.binds) if (b.bone === bi) o[{ 0x48: 'T', 0x4B: 'R', 0x4D: 'S' }[b.cmd]] = m.curves[b.fc];
  return o;
}
const camkIsCam = (c) => !!c && (() => { for (let k = 0; k < c.nk; k++) { const z = c.keys[k * c.per + (c.interp === 2 ? 7 : 3)]; if (Math.abs(z) > 1.0001) return true; } return false; })();
function camkKeys(gmo, mi) {                      // the frames the camera is keyed at (cut helpers left out), or []
  const t = camkTracks(gmo, mi); if (!t || !camkIsCam(t.S)) return [];
  const fr = new Set();
  for (const c of [t.T, t.R, t.S]) if (c) for (let k = 0; k < c.nk; k++) fr.add(Math.round(c.keys[k * c.per] * 100) / 100);
  const all = [...fr].sort((a, b) => a - b);
  return all.filter((f) => !all.some((g) => g > f && g - f < CAMK.cut * 1.5));
}
function camkFov(gmo, mi, f) { const t = camkTracks(gmo, mi); return t && camkIsCam(t.S) ? Math.abs(evalCurve(t.S, f)[2]) : null; }

/* the viewport's view as the camera bone's local transform at the current pose */
function camkFromView() {
  const bi = camkBoneIndex(S.gmo), b = view.bones && view.bones[bi]; if (!b) return null;
  view.root.updateMatrixWorld(true);
  const par = b.parent || view.root, inv = par.matrixWorld.clone().invert();
  const M = inv.multiply(new THREE.Matrix4().compose(view.camera.position, view.camera.quaternion, new THREE.Vector3(1, 1, 1)));
  const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3(); M.decompose(p, q, s);
  return { t: [p.x, p.y, p.z], r: [q.x, q.y, q.z, q.w], fov: view.camera.fov };
}

/* ---------------------------------------------------------------- writing */
function camkNewCurve(mc, dims, interp, idx) {    // an empty FCURVE chunk appended after the motion's last one
  const nm = new TextEncoder().encode('fcurve-' + idx), pad = new Uint8Array(nm.length + 1 + ((4 - (nm.length + 1) % 4) % 4)); pad.set(nm, 0);
  const header = new Uint8Array(8 + pad.length), hv = new DataView(header.buffer);
  hv.setUint32(4, 0x2C, true); header.set(pad, 8);
  const c = { type: GC.FCURVE, header, children: [], payload: new Uint8Array(16) };
  const ch = mc.children; let at = ch.length; while (at > 0 && ch[at - 1].type !== GC.FCURVE) at--;
  ch.splice(at, 0, c);
  return c;
}
/* motion mi of one model with op applied: {key: f, t, r, fov, cut} | {del: f} | {clear: true} -> new bytes, or null */
function camkApplyBytes(bytes, mi, op) {
  const gmo = parseGMO(bytes), bi = camkBoneIndex(gmo), m = gmo.motions[mi]; if (bi < 0 || !m) return null;
  const tree = cpTree(bytes), am = cpAnimModel(tree), mc = am && gcFind(am, 0x0B)[mi]; if (!mc) return null;
  const binds = gcFind(mc, 0x80B3), fcs = gcFind(mc, GC.FCURVE);
  const bdv = (c) => new DataView(c.payload.buffer, c.payload.byteOffset, c.payload.byteLength);
  const users = new Map(); for (const c of binds) { const fc = bdv(c).getUint32(12, true) & 0xfff; users.set(fc, (users.get(fc) || 0) + 1); }
  const rest = gmo.animBones[bi], real = camkIsCam(camkTracks(gmo, mi).S);
  const spec = { 0x48: { dims: 3, interp: 1, rest: rest.t }, 0x4B: { dims: 4, interp: 4, rest: rest.r }, 0x4D: { dims: 3, interp: 1, rest: rest.s } };
  let nfc = fcs.length;
  for (const cmd of [0x48, 0x4B, 0x4D]) {
    const sp = spec[cmd];
    let bc = binds.find((c) => (bdv(c).getUint32(0, true) & 0xfff) === bi && bdv(c).getUint32(4, true) === cmd), chunk, cv;
    if (!bc) {                                    // the bone isn't bound in this motion: bind it
      const tpl = binds[0]; if (!tpl) return null;
      const pl = tpl.payload.slice(), dv = new DataView(pl.buffer);
      dv.setUint32(0, (dv.getUint32(0, true) & ~0xfff) | bi, true); dv.setUint32(4, cmd, true); dv.setUint32(8, 0, true);
      bc = { type: 0x80B3, payload: pl };
      const lastBind = mc.children.lastIndexOf(binds[binds.length - 1]); mc.children.splice(lastBind + 1, 0, bc); binds.push(bc);
    }
    const fcRef = bdv(bc).getUint32(12, true), fi = fcRef & 0xfff;
    if (fcs[fi] && users.get(fi) === 1) { chunk = fcs[fi]; cv = acRead(chunk); }
    else {                                        // shared with other bones (or missing): a curve of its own
      chunk = camkNewCurve(mc, sp.dims, sp.interp, nfc);
      const old = fcs[fi] ? acRead(fcs[fi]) : null;
      cv = old || { fmt: 0, dims: sp.dims, extra: 0, half: false, interp: sp.interp, per: 1 + sp.dims, keys: [] };
      bdv(bc).setUint32(12, (fcRef & ~0xfff) | nfc, true); nfc++;
    }
    // a camera curve in the studio's layout (full floats, values only); a step curve stays a step curve
    const vals = cv.keys.map((_, k) => ({ t: cv.keys[k][0], v: acVals(cv, k) }));
    const interp = cv.interp === 0 && real ? 0 : sp.interp;
    let keys = real ? vals : [];
    const f0 = 0;
    if (op.clear) keys = [{ t: f0, v: sp.rest.slice() }];
    else if (op.del != null) {
      keys = keys.filter((k) => !(k.t > op.del - CAMK.cut * 1.5 && k.t < op.del + 1e-3));
      if (!keys.length || !camkKeepsCam(keys, cmd)) keys = [{ t: f0, v: sp.rest.slice() }];
    } else if (op.key != null) {
      const v = cmd === 0x48 ? op.t : cmd === 0x4B ? op.r : [1, 1, op.fov];
      const tmp = { dims: sp.dims, interp, keys: keys.map((k) => [k.t, ...k.v]), per: 1 + sp.dims };
      const prev = keys.length ? acSample(tmp, op.key - CAMK.cut) : null;
      keys = keys.filter((k) => Math.abs(k.t - op.key) > 1e-3 && !(op.cut && Math.abs(k.t - (op.key - CAMK.cut)) < 1e-3));
      keys.push({ t: op.key, v: v.slice() });
      if (op.cut && prev && keys.some((k) => k.t < op.key - CAMK.cut)) keys.push({ t: op.key - CAMK.cut, v: prev });
      keys.sort((a, b) => a.t - b.t);
    }
    acWrite(chunk, { fmt: 0, dims: sp.dims, extra: cv.extra || 0, half: false, interp, keys: keys.map((k) => [k.t, ...k.v]) });
  }
  return cpTreeBytes(tree);
}
function camkKeepsCam(keys, cmd) { return cmd !== 0x4D || keys.some((k) => Math.abs(k.v[2]) > 1.0001); }

async function camkApply(mi, op, msg) {
  if (S.readOnly) { readOnlyNote(); return; }
  if (!S.rig || S.rig.retail) { toast('Pick the mod character (not the retail original) to change its animations.'); return; }
  const name = motionName(mi), rigs = S.rigs.filter((r) => r.ms === S.rig.ms && !r.retail), todo = [];
  for (const r of rigs) {
    const bytes = S.files[r.gmo] && S.files[r.gmo].bin ? S.files[r.gmo].bin : new Uint8Array(await FS.read(r.gmo));
    const g = parseGMO(bytes);
    if (!g.motions[mi] || g.motions[mi].name !== name || camkBoneIndex(g) < 0) continue;
    todo.push({ r, bytes });
  }
  if (!todo.length) { toast(name + ' has no camera bone (camera_include) in this character\'s models.'); return; }
  for (const x of todo) { if (!S.files[x.r.gmo]) S.files[x.r.gmo] = { bin: x.bytes, binOrig: x.bytes, dirty: false, gmo: true }; S.files[x.r.gmo].gmo = true; }
  edit(todo.map((x) => x.r.gmo), () => { for (const x of todo) S.files[x.r.gmo].bin = camkApplyBytes(x.bytes, mi, op) || x.bytes; });
  if (typeof GCAM_OK !== 'undefined') GCAM_OK.clear();
  cpRigChanged(); runChecks(); renderAll();
  toast(`${name}: ${msg} Undo puts it back.`, 'ok');
}

/* ---------------------------------------------------------------- the inspector section */
const camkF = (f) => (Math.round(f * 100) / 100).toString();
function camkSectionHTML(mi) {
  if (!S.gmo || !S.rig || S.rig.retail || camkBoneIndex(S.gmo) < 0) return '';
  const keys = camkKeys(S.gmo, mi), f = Math.round(S.frame * 2) / 2;
  const ro = S.readOnly ? ' disabled' : '', fov = camkFov(S.gmo, mi, f);
  return `<div class="sec" id="camkSec"><h3>Game camera${keys.length ? ` <small>${keys.length} key${keys.length > 1 ? 's' : ''}</small>` : ''}${fTip(`${keys.length ? 'The game films this animation through these keys.' : 'The game uses its own camera here.'} Frame the shot in the viewport, then key it. Watch shows the animation through the keys (also Game camera in the View menu).`)}</h3>
    ${keys.length ? `<div class="btnrow camk-keys">${keys.map((k) => `<button type="button" class="mini${Math.abs(k - f) < 1e-3 ? ' on' : ''}" data-camf="${k}" title="Go to frame ${camkF(k)}">${camkF(k)}</button>`).join('')}</div>` : ''}
    <div class="row2">
      <div class="field"><label>Field of view (°)${fTip('Vertical field of view of the key. Smaller zooms in. Starts at the viewport\'s own (or this key\'s).')}</label><input type="number" id="camkFov" min="5" max="120" step="0.5" value="${(fov || view.camera.fov).toFixed(1)}"${ro}></div>
      <div class="field"><label class="chk" style="margin-top:22px"><input type="checkbox" id="camkCut"${ro}> Cut to this shot${fTip('Jump to this key instead of moving there from the previous one.')}</label></div>
    </div>
    <div class="btnrow"><button type="button" id="camkKey" class="primary" title="Key the viewport's view at the current frame (replaces a key there)"${ro}>Key the view</button>${keys.length ? `<button type="button" id="camkDel" title="Delete the key at the current frame"${ro}>Delete key</button><button type="button" id="camkClear" title="Remove every key: the game uses its own camera"${ro}>Clear</button>` : ''}<button type="button" id="camkWatch" aria-pressed="${!!(typeof GCAM !== 'undefined' && GCAM.on)}" title="${keys.length ? 'See the animation through the game camera (the same as Game camera in the View menu)' : 'Key the view first: there is no game camera to watch yet'}"${keys.length ? '' : ' disabled'}><i class="ic ic-eye" aria-hidden="true"></i> Watch</button></div></div>`;
}
function camkWire(mi) {
  const kb = $('camkKey'); if (!kb) return;
  document.querySelectorAll('[data-camf]').forEach((b) => b.onclick = () => { S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶'; setFrame(+b.dataset.camf); renderAll(); });
  kb.onclick = () => {
    if (typeof GCAM !== 'undefined' && GCAM.on) {   // the viewport is showing the game camera: give the user theirs back first
      GCAM.on = false; const cb = $('gcamOn'); if (cb) cb.checked = false; gcamRelease(); pose();
      toast('Game camera is off so you can frame the shot: move the view, then key it.'); return;
    }
    const v = camkFromView(); if (!v) return;
    const fov = Math.max(5, Math.min(120, +$('camkFov').value || view.camera.fov)), f = Math.round(S.frame * 2) / 2;
    camkApply(mi, { key: f, t: v.t, r: v.r, fov, cut: $('camkCut').checked }, `camera keyed at frame ${camkF(f)} (${fov.toFixed(1)}°).`);
  };
  const del = $('camkDel'); if (del) del.onclick = () => {
    const f = Math.round(S.frame * 2) / 2;
    if (!camkKeys(S.gmo, mi).some((k) => Math.abs(k - f) < 1e-3)) { toast(`No camera key at frame ${camkF(f)}: pick one of the keys above first.`); return; }
    camkApply(mi, { del: f }, `camera key at frame ${camkF(f)} deleted.`);
  };
  // Watch (2026-11-01: "There's no Game camera button?"): the View menu's Game camera, here beside the keys
  const w = $('camkWatch'); if (w) w.onclick = () => { const cb = $('gcamOn'); if (!cb) return; cb.checked = !cb.checked; cb.dispatchEvent(new Event('change')); w.setAttribute('aria-pressed', String(cb.checked)); };
  const cl = $('camkClear'); if (cl) cl.onclick = () => camkApply(mi, { clear: true }, 'camera cleared: the game uses its own camera.');
}
function camkSafeHTML(mi) {
  try { return camkSectionHTML(mi); }
  catch (e) { console.error('studio: Game camera', e); return `<div class="sec"><h3>Game camera</h3><p class="warnline">Could not read this animation's camera: ${esc(e && e.message || String(e))}</p></div>`; }
}
function camkSafeWire(mi) { try { camkWire(mi); } catch (e) { console.error('studio: Game camera', e); } }
