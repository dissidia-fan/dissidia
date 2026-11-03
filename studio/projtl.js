/* Move Studio: what a projectile event sets off, drawn under it on the timeline (2026-10-31: "Can we add that to
   the timeline then? and make it more clear what it's doing?"). Read-only lanes worked out from the projectile records
   the way magi.js flies them (FUN_08AD1588 emitter, FUN_08AD2FA8 / 30CC / 3234 birth, flight, death):
     - the projectile itself: one bar per shot, from its birth to its end;
     - what it spawns at a set time (at launch, in flight, when it ends): a diamond where the child appears, then the
       child's own bars, nested again;
     - what it can spawn on contact (a foe, a wall or floor, losing the floor, being cancelled): a hatched band over the
       ticks it could happen (for a foe, while its hitbox is live).
   Times use each value's low end; a random range says "about". Ticks are 1/60 s, lane frames are ticks / 2.
   Clicking a lane selects the event and shows that projectile in the Inspector (S.projFocus). */
const PTL_DEPTH = 3, PTL_SHOTS = 24;
const PTL_LINKS = [                             // contact children: field, how it reads
  ['link_hit', 'on a foe'], ['link_terrain', 'on a wall or floor'], ['link_fall', 'if it loses the floor'], ['link_clash', 'if it is cancelled']];
function ptlRec(id) {                          // the mod's projectiles.json as edited (live), else the loaded records (retail)
  if (id == null || id < 0) return null;
  const ed = typeof edProj === 'function' && typeof edProjFile === 'function' && edProjFile() ? edProj(id) : null;
  if (ed) return ed;
  const m = S.fx && S.fx.magi; return m ? m.get(id) || null : null;
}
function ptlV(r, n) { const v = r.values && r.values[n]; return v ? v : [0, 0, 0]; }
const ptlVaries = (r, n) => { const v = ptlV(r, n); return v[0] !== v[1] || v[2] !== 0; };
const ptlFlags = (r) => (typeof r.flags === 'string' ? parseInt(r.flags, 16) : r.flags) >>> 0;
function ptlName(id) {
  const r = ptlRec(id); if (!r) return `${id} (missing)`;
  const h = S.hitById && S.hitById.get(r.atkp);
  const cf = h && typeof hitContactFx === 'function' ? hitContactFx(h) : null;   // and what it plays where it lands
  return `${id} · ${r.atkp === 0xFFFF || r.atkp < 0 ? 'looks only' : h ? (h.name || h.id) + (parseInt(h.flags, 16) & 2 ? ' (HP)' : '') + (cf ? ' → ' + fxuShort(cf.fi.name) : '') : 'hit ' + r.atkp}`;
}
const ptlSec = (ticks) => `${+(ticks / 60).toFixed(2)} s`;
/* the shots one emitter call makes: birth ticks (spawnShots: first after first_delay, then each interval) */
function ptlBirths(r, t0) {
  const n = Math.min(PTL_SHOTS, Math.trunc(ptlV(r, 'shots')[0])), [ilo, , ist] = ptlV(r, 'interval'), out = [];
  let t = t0 + Math.max(0, Math.ceil(ptlV(r, 'first_delay')[0]));
  for (let k = 0; k < n; k++) { out.push(t); t += Math.max(0, Math.ceil(ilo + (ist ? (k + 1) * ist : 0))); }
  return out;
}
/* one shot born at b: its end and the ticks of its timed children */
function ptlShot(r, b) {
  const f = ptlFlags(r), life = Math.max(0, ptlV(r, 'lifetime')[0]), end = b + 2 + Math.ceil(life), timed = [];
  const ec = Math.trunc(ptlV(r, 'emit_count')[0]);
  if (r.link_emit >= 0) {
    if (f & 0x20) timed.push({ t: end, how: 'when it ends' });
    else if (ec > 0) {                            // in flight: first after emit_delay, then every emit_interval (ages)
      const [lo, , st] = ptlV(r, 'emit_interval');
      let age = Math.max(0, Math.ceil(ptlV(r, 'emit_delay')[0])), iv = lo;
      for (let k = 0; k < Math.min(ec, PTL_SHOTS) && age <= life; k++) { timed.push({ t: b + 1 + age, how: 'in flight' }); iv = k === 0 || !st ? lo : iv + st; age += Math.max(0, Math.ceil(iv)); }
    } else timed.push({ t: b, how: 'at launch' });
  }
  const hd = Math.max(0, ptlV(r, 'hit_delay')[0]), ht = ptlV(r, 'hit_time')[0];
  const hitOn = r.atkp !== 0xFFFF && r.atkp >= 0 && ht > 0 ? [b + 1 + hd, Math.min(end, b + 1 + hd + ht)] : null;
  return { b, end, timed, hitOn };
}
/* the lanes for projectile `id` fired at tick t0 (or, with `band`, possibly any time in it) */
function ptlRows(id, t0, depth, path, how, parent, field) {
  const r = ptlRec(id), rows = [];
  if (!r) { rows.push({ depth, id, parent, field, how, missing: true, births: [], bars: [], band: null }); return rows; }
  const births = how && how.band ? [] : ptlBirths(r, t0), shots = births.map((b) => ptlShot(r, b));
  const row = { depth, id, parent, field, how, births, bars: shots.map((s) => [s.b, s.end]), band: how && how.band, rec: r };
  rows.push(row);
  if (depth >= PTL_DEPTH || path.has(id) || (how && how.band)) { if (path.has(id)) row.loop = true; return rows; }
  const p2 = new Set(path).add(id), f = ptlFlags(r);
  if (r.link_emit >= 0 && shots.length) {        // timed child: every shot's spawn ticks together on one lane
    const ts = shots.flatMap((s) => s.timed), kid = [];
    const child = { depth: depth + 1, id: r.link_emit, parent: id, field: 'link_emit', how: { text: ts[0].how }, births: [], bars: [], marks: ts.map((x) => x.t) };
    const cr = ptlRec(r.link_emit);
    if (cr) for (const x of ts) for (const b of ptlBirths(cr, x.t)) { const s = ptlShot(cr, b); child.bars.push([s.b, s.end]); child.births.push(b); }
    child.rec = cr; child.missing = !cr; rows.push(child);
    // the child's own children, from its first spawn
    if (cr && !p2.has(cr.id) && depth + 1 < PTL_DEPTH) for (const x of ptlRows(cr.id, ts[0].t, depth + 1, p2, null, id, 'link_emit').slice(1)) kid.push(x);
    rows.push(...kid);
  }
  for (const [k, text] of PTL_LINKS) {          // contact children: a band over the ticks it could happen
    let cid = r[k];
    if (k === 'link_terrain') { if (f & 0x2) continue; if (r.link_terrain_1 >= 0) cid = r.link_terrain_1; }
    if (cid == null || cid < 0 || !shots.length) continue;
    const spans = k === 'link_hit' ? shots.map((s) => s.hitOn).filter(Boolean) : shots.map((s) => [s.b, s.end]);
    rows.push({ depth: depth + 1, id: cid, parent: id, field: k === 'link_terrain' && r.link_terrain_1 >= 0 ? 'link_terrain_1' : k,
      how: { text, band: true }, births: [], bars: [], band: spans, rec: ptlRec(cid), missing: !ptlRec(cid),
      never: k === 'link_hit' && !spans.length, ends: k !== 'link_hit' || !(f & 0x1) });
  }
  return rows;
}
/* the projectile an event fires and when: [id, tick, contact?] */
function ptlOf(r) {
  const e = r.e; if (e.projectile === undefined || e.projectile === null) return null;
  if (r.kind === 'ranged') return [e.projectile, e.start, true];   // hit window + projectile: fired when it connects
  return [e.projectile, e.frame, false];
}
/* the HTML of the lanes under one event lane */
function ptlLanes(r, o) {
  const pf = ptlOf(r); if (!pf) return '';
  const [id, t0, contact] = pf, src = r.src, at = (t) => LABEL_W + (src.at + frames(t)) * o.px;
  const rows = ptlRows(id, t0, 1, new Set(), contact ? { text: 'when the window connects', band: true } : null, null, null);
  if (contact && rows[0]) rows[0].band = [[r.e.start, r.e.end === FOREVER ? r.e.start + 2 * r.src.full : r.e.end]];
  const col = `var(${evColVar(r.info)})`, foc = S.projFocus && S.sel && S.sel.kind === r.kind && S.sel.idx === r.idx ? S.projFocus : null;
  let h = '';
  for (const x of rows) {
    const rec = x.rec, about = rec && ['lifetime', 'first_delay', 'interval', 'emit_delay', 'emit_interval'].some((n) => ptlVaries(rec, n));
    const what = ptlName(x.id), when = x.depth === 1 ? (contact ? 'when the window connects' : '') : x.how.text;
    let label = when ? `${when[0].toUpperCase()}${when.slice(1)} → ${what}` : what;   // the trigger first: it is what tells the lanes apart
    if (x.loop) label += ' (spawns itself again)';
    if (x.never) label += ' (never: no live hitbox)';
    const life = rec ? ptlSec(2 + Math.ceil(ptlV(rec, 'lifetime')[0])) : '';
    const tip = x.missing ? `Projectile ${x.id} is not in this character's projectiles.` :
      `${label}${x.bars.length ? ` · lives ${about ? 'about ' : ''}${life}${x.bars.length > 1 ? ` · ${x.bars.length} shots` : ''}` : ''}` +
      (x.band ? ` · could happen in the hatched span${x.how && x.how.text === 'on a foe' ? ' (while its hitbox is live)' : ''}${x.ends === false ? '; it keeps flying (goes through foes)' : x.ends ? '; this one ends there' : ''}` : '') +
      ' · click to edit it';
    const sel = foc ? foc.id === x.id && foc.parent === (x.parent ?? null) && foc.field === (x.field ?? null) : false;
    let bars = '';
    for (const [a, b] of x.bars) bars += `<div class="ptl-life" style="left:${at(a)}px;width:${Math.max(2, frames(b - a) * o.px)}px;background:${col}"></div>`;
    for (const t of x.marks || []) bars += `<div class="ptl-key" style="left:${at(t)}px;background:${col}"></div>`;
    for (const [a, b] of x.band || []) bars += `<div class="ptl-band" style="left:${at(a)}px;width:${Math.max(3, frames(b - a) * o.px)}px;--c:${col}"></div>`;
    const ind = 6 + 12 * x.depth;
    h += `<div class="lane ptl${sel ? ' selrow' : ''}" style="width:${o.width}px"><span class="lane-label ptl-label" style="width:${LABEL_W}px;padding-left:${ind}px" data-ptl="${x.id}" data-pk="${r.kind}" data-pi="${r.idx}"${src.seg != null ? ` data-ps="${src.seg}"` : ''}${x.parent != null ? ` data-pp="${x.parent}" data-pf="${x.field}" data-ph="${esc(x.how ? x.how.text : '')}"` : ''} title="${esc(tip)}"><i class="ptl-arrow">↳</i>${esc(label)}</span>${bars}</div>`;
  }
  return h;
}
function ptlWire(inner) {
  inner.querySelectorAll('[data-ptl]').forEach((el) => el.onclick = () => {
    const d = el.dataset;
    if (d.ps !== undefined) tlPick(d.ps);
    selectEvent(d.pk, +d.pi, false);
    const e = curTL()[d.pk][+d.pi];
    S.projFocus = +d.ptl === e.projectile && d.pp === undefined ? null : { id: +d.ptl, parent: d.pp !== undefined ? +d.pp : null, field: d.pf || null, how: d.ph || '' };
    renderTimeline(); renderInspector();
  });
}
(function ptlStyle() {
  const s = document.createElement('style');
  s.textContent = `.lane.ptl{height:16px}
.lane.ptl .lane-label{height:15px;line-height:15px;font-size:10.5px;color:var(--dim)}
.lane.ptl.selrow .lane-label{color:var(--ember, var(--ink))}
.ptl-arrow{font-style:normal;opacity:.6;margin-right:4px}
.ptl-life{position:absolute;top:5px;height:6px;border-radius:3px;opacity:.55;pointer-events:none}
.ptl-key{position:absolute;top:3.5px;width:8px;height:8px;margin-left:-4px;transform:rotate(45deg);border-radius:1px;pointer-events:none;box-shadow:0 0 0 1px var(--panel)}
.ptl-band{position:absolute;top:3px;height:10px;border-radius:2px;pointer-events:none;opacity:.7;border:1px dashed var(--c);
  background:repeating-linear-gradient(135deg,color-mix(in srgb,var(--c) 45%,transparent) 0 3px,transparent 3px 7px)}`;
  document.head.appendChild(s);
})();
