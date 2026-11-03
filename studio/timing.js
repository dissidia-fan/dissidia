/* Move Studio: an animation's timing (2026-11-01, after "Is there anything else that we did to make noctis that you
   can't yet do in the studio alone?": Blitz press 2 was resampled 1.4× by a script, the entry and victory were spliced
   by build_ev.py). The Timing section (the animation's inspector) plays a range of frames faster or slower: the range
   gets a new length, the frames after it move with its end, and the events on the animation's timeline move with the
   frames they sit on.
   How it is written, per curve of the motion (every model of this moveset that has the animation):
   - every key moves in time (a key is added at each end of the range where it cuts between two keys), so the curves
     keep their exact shape: a pose keyed every frame lasts 1/speed frames, a part switched on by its scale stays a
     switch; hermite tangents are scaled to the new speed; half-float curves stay half unless two keys would collide;
   - keys before the range are untouched, keys after it move by the change in length;
   - the motion's frame range (0x80B1 {loop start, end}) moves with it. The game has no other length field
     (docs: Blitz v2 press 3 notes).
   Event ticks (60 a second, 2 a frame) are mapped the same way and rounded to whole ticks; "until the end" stays.
   Runtime hooks keep their own numbers, so the result names any hook on this animation to check.
   Also holds the curve read / write / sample helpers camkey.js uses.
   Loaded after travel.js (cpTree, cpAnimModel, cpTreeBytes, cpRigChanged, gcFind, gcFirst). */

/* ---------------------------------------------------------------- curves (FCURVE 0x000C payloads) */
function acHalf(f) {                              // float -> IEEE half bits (round to nearest)
  const b = new DataView(new ArrayBuffer(4)); b.setFloat32(0, f); const x = b.getUint32(0);
  const s = (x >>> 16) & 0x8000, ex = (x >>> 23) & 0xff; let e = ex - 127 + 15, m = x & 0x7fffff;
  if (ex === 0xff) return s | 0x7c00 | (m ? 0x200 : 0);
  if (e >= 31) return s | 0x7c00;
  if (e <= 0) { if (e < -10) return s; m = (m | 0x800000) >> (1 - e); if (m & 0x1000) m += 0x2000; return s | (m >> 13); }
  if (m & 0x1000) { m += 0x2000; if (m & 0x800000) { m = 0; e++; if (e >= 31) return s | 0x7c00; } }
  return s | (e << 10) | (m >> 13);
}
function acRead(c) {                              // {fmt, dims, extra, half, interp, per, keys: [[frame, ...]]}
  const p = c.payload, dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
  const fmt = dv.getUint32(0, true), dims = dv.getUint32(4, true), nk = dv.getUint32(8, true), extra = dv.getUint32(12, true);
  const half = !!(fmt & 0x80), interp = fmt & 0x7f, per = 1 + dims * (interp === 2 ? 3 : 1), keys = [];
  for (let k = 0; k < nk; k++) {
    const r = [];
    for (let j = 0; j < per; j++) { const i = k * per + j; r.push(half ? halfToFloat(dv.getUint16(16 + i * 2, true)) : dv.getFloat32(16 + i * 4, true)); }
    keys.push(r);
  }
  return { fmt, dims, extra, half, interp, per, keys };
}
function acWrite(c, cv) {                         // cv back into the chunk (its block size field too)
  const per = 1 + cv.dims * (cv.interp === 2 ? 3 : 1), w = cv.half ? 2 : 4, n = cv.keys.length, raw = 16 + n * per * w;
  const out = new Uint8Array(raw + ((4 - raw % 4) % 4)), dv = new DataView(out.buffer);
  dv.setUint32(0, ((cv.fmt >>> 0) & ~0xff) | (cv.half ? 0x80 : 0) | cv.interp, true);
  dv.setUint32(4, cv.dims, true); dv.setUint32(8, n, true); dv.setUint32(12, cv.extra >>> 0, true);
  cv.keys.forEach((r, k) => { for (let j = 0; j < per; j++) { const i = k * per + j, v = r[j] || 0; if (cv.half) dv.setUint16(16 + i * 2, acHalf(v), true); else dv.setFloat32(16 + i * 4, v, true); } });
  c.payload = out; cv.per = per;
  if (c.header && c.header.length >= 8) new DataView(c.header.buffer, c.header.byteOffset, c.header.byteLength).setUint32(0, 8 + c.header.length + out.length, true);
}
const acVals = (cv, k) => { const r = cv.keys[k], o = []; for (let d = 0; d < cv.dims; d++) o.push(r[cv.interp === 2 ? 1 + 3 * d : 1 + d]); return o; };
function acRow(cv, t, vals, tin, tout) {          // a key row in the curve's layout
  const r = [t];
  for (let d = 0; d < cv.dims; d++) { if (cv.interp === 2) r.push(vals[d], tin ? tin[d] : 0, tout ? tout[d] : 0); else r.push(vals[d]); }
  return r;
}
function acNorm(q) { const l = Math.hypot(...q) || 1; return q.map((x) => x / l); }
function acSlerp(a, b, t) {
  let d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) { b = b.map((x) => -x); d = -d; }
  if (d > 0.9995) return acNorm(a.map((x, i) => x + (b[i] - x) * t));
  const th = Math.acos(Math.min(1, d)), s = Math.sin(th), wa = Math.sin((1 - t) * th) / s, wb = Math.sin(t * th) / s;
  return a.map((x, i) => wa * x + wb * b[i]);
}
/* the curve's value at frame x (as evalCurve, without three.js). A step curve (interp 0) holds until the next key, as
   the game plays it: it holds until the next key, as the game plays it. */
function acSample(cv, x) {
  const K = cv.keys, n = K.length;
  if (n === 1 || x <= K[0][0]) return acVals(cv, 0);
  if (x >= K[n - 1][0]) return acVals(cv, n - 1);
  let i = 0; while (i < n - 2 && K[i + 1][0] <= x) i++;
  const t0 = K[i][0], t1 = K[i + 1][0], dt = Math.max(t1 - t0, 1e-9), t = (x - t0) / dt, a = acVals(cv, i), b = acVals(cv, i + 1);
  if (cv.interp === 0) return a;
  if (cv.interp === 2) {
    const t2 = t * t, t3 = t2 * t;
    return a.map((p0, d) => { const m0 = K[i][3 + 3 * d] * dt, m1 = K[i + 1][2 + 3 * d] * dt, p1 = b[d]; return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1; });
  }
  if (cv.dims === 4) return cv.interp === 4 ? acSlerp(a, b, t) : acNorm(a.map((v, d) => v + (b[d] - v) * t));
  return a.map((v, d) => v + (b[d] - v) * t);
}
function acSlope(cv, x) {                         // d value / d frame, for hermite keys the studio adds
  const h = 0.01, p = acSample(cv, x + h), m = acSample(cv, x - h); return p.map((v, d) => (v - m[d]) / (2 * h));
}

/* ---------------------------------------------------------------- the retime */
// frame x of the old animation -> its frame now, with frames [a, b] played over n2 frames
const tmgMap = (a, b, n2) => (x) => (x <= a ? x : x <= b ? a + (x - a) * n2 / (b - a) : x + n2 - (b - a));
function acRetime(cv, a, b, n2) {                 // -> true when the curve changed
  const K = cv.keys, s = n2 / (b - a), map = tmgMap(a, b, n2), eps = 1e-6;
  if (K.length <= 1 || K[K.length - 1][0] <= a + eps || Math.abs(n2 - (b - a)) < eps) return false;
  // a key at each end of the range where it cuts between two keys, so the curve keeps its shape exactly; then every key
  // moves in time (a pose keyed every frame now lasts 1/s frames, a switch stays a switch)
  const at = (t) => K.some((k) => Math.abs(k[0] - t) < eps), add = [];
  for (const t of [a, b]) if (t > K[0][0] + eps && t < K[K.length - 1][0] - eps && !at(t)) { const sl = cv.interp === 2 ? acSlope(cv, t) : null; add.push(acRow(cv, t, acSample(cv, t), sl, sl)); }
  const all = [...K.map((k) => k.slice()), ...add].sort((x, y) => x[0] - y[0]);
  for (const r of all) {
    const t = r[0];
    if (cv.interp === 2) for (let d = 0; d < cv.dims; d++) {     // tangents are per frame: scaled to the new speed inside the range
      if (t > a - eps && t < b - eps) r[3 + 3 * d] /= s;
      if (t > a + eps && t < b + eps) r[2 + 3 * d] /= s;
    }
    r[0] = map(t);
  }
  // half-float times are exact to 1/32 frame below 64, 1/8 below 512: if two keys would land on the same time the
  // curve is written in full floats instead
  if (cv.half) for (let i = 1; i < all.length; i++) if (halfToFloat(acHalf(all[i][0])) <= halfToFloat(acHalf(all[i - 1][0]))) { cv.half = false; break; }
  cv.keys = all;
  return true;
}
/* motion mi of one model with frames [a, b] played over n2 frames: -> new GMO bytes, or null */
function tmgApplyBytes(bytes, mi, a, b, n2) {
  const tree = cpTree(bytes), am = cpAnimModel(tree), mc = am && gcFind(am, 0x0B)[mi]; if (!mc) return null;
  let n = 0;
  for (const fc of gcFind(mc, GC.FCURVE)) { const cv = acRead(fc); if (acRetime(cv, a, b, n2)) { acWrite(fc, cv); n++; } }
  const lp = gcFirst(mc, 0x80B1);
  if (lp && lp.payload.length >= 8) {
    // {loop start, end}: the motion plays from frame 0 and loops back to `loop start` (a victory's idle tail)
    const dv = new DataView(lp.payload.buffer, lp.payload.byteOffset, lp.payload.byteLength), map = tmgMap(a, b, n2);
    const out = lp.payload.slice(), ov = new DataView(out.buffer);
    ov.setFloat32(0, map(dv.getFloat32(0, true)), true); ov.setFloat32(4, map(dv.getFloat32(4, true)), true); lp.payload = out;
  }
  return n || lp ? cpTreeBytes(tree) : null;
}
function tmgTimeline(t, a, b, n2) {               // the events of a timeline object, moved in place
  const map = tmgMap(a, b, n2), tk = (v) => (v === FOREVER || typeof v !== 'number' ? v : Math.max(0, Math.min(FOREVER - 1, Math.round(map(v / 2) * 2))));
  for (const e of t.ranged || []) {
    const d0 = e.end - e.start;
    e.start = tk(e.start); e.end = tk(e.end);
    if (e.end !== FOREVER && e.end < e.start) e.end = e.start;
    if (typeof e.duration === 'number' && e.duration === d0 && e.end !== FOREVER) e.duration = e.end - e.start;   // screen_shake_range keeps its length = its range
  }
  for (const e of t.keyed || []) e.frame = tk(e.frame);
}
function tmgInfo(gmo, mi) {
  const m = gmo && gmo.motions[mi]; if (!m) return null;
  return { first: 0, last: Math.ceil(m.end) };   // (m.start is where it loops back to, not where it starts)
}
async function tmgApply(mi, a, b, n2) {
  if (S.readOnly) { readOnlyNote(); return; }
  if (!S.rig || S.rig.retail) { toast('Pick the mod character (not the retail original) to change its animations.'); return; }
  const name = motionName(mi), rigs = S.rigs.filter((r) => r.ms === S.rig.ms && !r.retail), todo = [];
  for (const r of rigs) {
    const bytes = S.files[r.gmo] && S.files[r.gmo].bin ? S.files[r.gmo].bin : new Uint8Array(await FS.read(r.gmo));
    const g = parseGMO(bytes);
    if (!g.motions[mi] || g.motions[mi].name !== name) continue;
    todo.push({ r, bytes });
  }
  if (!todo.length) { toast(name + ' is not in this character\'s models.'); return; }
  for (const x of todo) { if (!S.files[x.r.gmo]) S.files[x.r.gmo] = { bin: x.bytes, binOrig: x.bytes, dirty: false, gmo: true }; S.files[x.r.gmo].gmo = true; }
  const tls = S.tlPaths.filter((p) => F(p) && F(p).motion === mi);
  edit([...todo.map((x) => x.r.gmo), ...tls], () => {
    for (const x of todo) S.files[x.r.gmo].bin = tmgApplyBytes(x.bytes, mi, a, b, n2) || x.bytes;
    for (const p of tls) tmgTimeline(F(p), a, b, n2);
  });
  cpRigChanged(); derive(); runChecks(); renderAll();
  const hooks = typeof ckHooks === 'function' ? [...new Set(ckHooks().filter((h) => h.motions.includes(mi)).map((h) => h.kind))] : [];
  const pct = Math.round(100 * (b - a) / n2);
  toast(`${name}: frames ${a}–${b} now take ${n2} frames (${pct} % speed)${tls.length ? ', events moved with them' : ''}.${hooks.length ? ` Runtime hooks on this animation keep their own numbers: check ${hooks.join(', ')}.` : ''} Undo puts it back.`, 'ok');
}

/* ---------------------------------------------------------------- the inspector section */
function tmgSectionHTML(mi) {
  if (!S.gmo || !S.rig || S.rig.retail) return '';
  const i = tmgInfo(S.gmo, mi); if (!i || i.last - i.first < 2) return '';
  const ro = S.readOnly ? ' disabled' : '';
  return `<div class="sec" id="tmgSec"><h3>Timing${fTip('Play part of the animation faster or slower. Its events move with it.')}</h3>
    <div class="row2">
      <div class="field"><label>From frame${fTip('The part to speed up or slow down. Frames before it stay as they are; frames after it carry on from where it now ends.')}</label><input type="number" id="tmgFrom" min="${i.first}" max="${i.last}" step="1" placeholder="${i.first}"${ro}></div>
      <div class="field"><label>To frame</label><input type="number" id="tmgTo" min="${i.first}" max="${i.last}" step="1" placeholder="${i.last}"${ro}></div>
    </div>
    <div class="row2">
      <div class="field"><label>Length (frames)${fTip('How many frames the range takes now. Fewer is faster. Written into every model of this moveset that has the animation; event ticks move with the frames they sit on.')}</label><input type="number" id="tmgLen" min="1" step="1" placeholder="${i.last - i.first}"${ro}></div>
      <div class="field"><label>Speed (%)${fTip('The same as Length, as a speed: 140 plays the range 1.4× as fast.')}</label><input type="number" id="tmgPct" min="5" max="2000" step="5" placeholder="100"${ro}></div>
    </div>
    <p class="note" id="tmgNow" data-rest="${esc(`${i.last - i.first} frames (${i.first}–${i.last}).`)}"></p>
    <div class="btnrow" id="tmgBtns" hidden><button type="button" id="tmgGo" class="primary"${ro}>Apply</button><button type="button" id="tmgReset">Reset</button></div></div>`;
}
function tmgWire(mi) {
  const go = $('tmgGo'); if (!go) return;
  const v = (id) => { const el = $(id); return el.value === '' ? +el.placeholder : Math.round(+el.value); };
  const read = () => { const a = v('tmgFrom'), b = v('tmgTo'); return { a, b, n2: $('tmgLen').value === '' ? b - a : Math.max(1, Math.round(+$('tmgLen').value)) }; };
  const typed = () => ['tmgFrom', 'tmgTo', 'tmgLen', 'tmgPct'].some((id) => $(id).value !== '');
  const show = () => {
    const { a, b, n2 } = read(), n = $('tmgNow'), i = tmgInfo(S.gmo, mi);
    const bad = b <= a ? 'To must come after From.' : a < i.first || b > i.last ? `The animation runs from ${i.first} to ${i.last}.` : '';
    const same = n2 === b - a;
    n.textContent = bad || (same ? n.dataset.rest : `${b - a} → ${n2} frames (${Math.round(100 * (b - a) / n2)} % speed). The animation becomes ${i.last - i.first} → ${i.last - i.first + n2 - (b - a)} frames.`);
    $('tmgBtns').hidden = !typed();
    go.disabled = !!bad || same || S.readOnly || S.rig.retail;
  };
  const fromPct = () => { const { a, b } = read(), p = +$('tmgPct').value; $('tmgLen').value = $('tmgPct').value === '' || !(p > 0) ? '' : Math.max(1, Math.round((b - a) * 100 / p)); };
  ['tmgFrom', 'tmgTo'].forEach((id) => $(id).oninput = () => { const { a, b } = read(); $('tmgLen').placeholder = Math.max(0, b - a); if ($('tmgPct').value !== '') fromPct(); show(); });
  $('tmgLen').oninput = () => { $('tmgPct').value = ''; show(); };
  $('tmgPct').oninput = () => { fromPct(); show(); };
  show();
  $('tmgReset').onclick = () => { ['tmgFrom', 'tmgTo', 'tmgLen', 'tmgPct'].forEach((id) => { $(id).value = ''; }); $('tmgFrom').oninput(); };
  go.onclick = () => { const { a, b, n2 } = read(); if (b > a && n2 !== b - a) tmgApply(mi, a, b, n2); };
}
function tmgSafeHTML(mi) {
  try { return tmgSectionHTML(mi); }
  catch (e) { console.error('studio: Timing', e); return `<div class="sec"><h3>Timing</h3><p class="warnline">Could not read this animation's timing: ${esc(e && e.message || String(e))}</p></div>`; }
}
function tmgSafeWire(mi) { try { tmgWire(mi); } catch (e) { console.error('studio: Timing', e); } }
