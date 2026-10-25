/* Move Studio: more checks (added to runChecks), and the Changes panel: what saving and building would change, file by
   file, as records added / removed / changed. Loaded after copy.js. */

/* ---------------------------------------------------------------- checks */
function ckMore() {
  const out = [], P = F(msPath('projectiles.json')), recs = P ? P.records : [];
  const num = (v) => (v && typeof v === 'object' && 'f' in v ? v.f : v);    // timeline floats are stored as {"f": x}
  const projIds = new Set(recs.map((r) => r.id)), fxIds = S.fx && S.fx.byId ? new Set(S.fx.byId.keys()) : null;
  const hitIds = S.hitById || new Map();
  // hitbox groups: which groups exist, which can hurt (kind 4)
  let vols = null; const vp = typeof edVolPath === 'function' ? edVolPath() : null;
  try { if (vp) vols = edVolParse(S.files[vp].bin); } catch (e) { vols = null; }
  const groups = new Map(); if (vols) for (const r of vols.recs) { if (!groups.has(r.group)) groups.set(r.group, new Set()); groups.get(r.group).add(r.kind); }
  const seenGroup = new Set();
  for (const p of S.tlPaths) {
    const t = F(p); if (!t) continue;
    const mi = t.motion, nm = motionName(mi);
    for (const kind of ['ranged', 'keyed']) t[kind].forEach((e, idx) => {
      const code = opCode(e.op);
      for (const k of ['projectile', 'alt_projectile']) { const v = num(e[k]); if (Number.isInteger(v) && v !== 0xFFFF && v >= 0 && P && !projIds.has(v))
        out.push({ level: 'err', motion: mi, kind, idx, text: `${nm}: projectile ${v} is not in projectiles.json, so nothing is fired.` }); }
      if (fxIds && fxIds.size) for (const k of ['effect', 'alt_effect']) { const v = num(e[k]); if (Number.isInteger(v) && v !== 0xFFFF && v >= 0 && !fxIds.has(v))   // a float there (0.05) is another kind of value, not an effect id
        out.push({ level: 'warn', motion: mi, kind, idx, text: `${nm}: effect ${v} is not one of this character's effect records, so nothing shows.` }); }
      const g = [0x0200, 0x027B, 0x0743].includes(code) ? e.group : code === 0x0112 && e.args ? e.args[0] : undefined;
      if (g !== undefined && vols && !seenGroup.has(g + ':' + mi)) {
        seenGroup.add(g + ':' + mi);
        if (!groups.has(g)) out.push({ level: 'err', motion: mi, kind, idx, text: `${nm}: hitbox group ${g} has no spheres, so this window can't touch anyone.` });
        else if (code !== 0x0112 && !groups.get(g).has(4) && !groups.get(g).has(6)) out.push({ level: 'warn', motion: mi, kind, idx, text: `${nm}: hitbox group ${g} has no attack sphere (kind 4), so the hit never lands.` });
      }
    });
  }
  // projectiles: their links, hits and pool
  for (const r of recs) {
    for (const l of CP_LINKS) if (r[l] >= 0 && r[l] !== 0xFFFF && !projIds.has(r[l])) out.push({ level: 'err', text: `Projectile ${r.id}: ${l} points at ${r[l]}, which isn't in projectiles.json.` });
    for (const k of ['atkp', 'atkp_reflected']) if (r[k] !== 0xFFFF && hitIds.size && !hitIds.has(r[k])) out.push({ level: 'err', text: `Projectile ${r.id}: ${k} ${r[k]} is not in hits.json.` });
    if (S.fx && r.effect !== 0xFFFF && r.effect < 64 && !S.fx.fepa.some((x) => !x.trig && fxGroupHas(x, r.effect))) out.push({ level: 'warn', text: `Projectile ${r.id}: no effect placement is in its look group ${r.effect}, so it flies unseen.` });
  }
  if (P && P.pool) {
    const n = P.pool.length;
    if (n > 128) out.push({ level: 'err', text: `The projectile value pool has ${n} values; the game reads at most 128 (signed bytes).` });
    else if (n > 120) out.push({ level: 'warn', text: `The projectile value pool is nearly full (${n} of 128 values). New distinct values may not fit.` });
  }
  // effects package
  if (S.effe && typeof fxuPkg === 'function') {
    try { const pkg = fxuPkg(); if (pkg && fxeFreeGroup(pkg.fep) < 0) out.push({ level: 'info', text: 'The effect package is full (128 effects): copying or adding an effect needs room first (Effects tab > Remove unused…).' }); } catch (e) { /* unreadable */ }
  }
  // abilities: a character's ability ids are (charId << 8) | key, and a save keeps 150 learned abilities
  const keys = Object.keys((S.mod && S.mod.move_names) || {}).filter((k) => /^[0-9A-F]+$/i.test(k));
  const bad = keys.filter((k) => parseInt(k, 16) > 0xFF);
  if (bad.length) out.push({ level: 'err', text: `Ability key${bad.length > 1 ? 's' : ''} ${bad.join(', ')} ${bad.length > 1 ? 'are' : 'is'} above FF: a key is one byte of the ability id.` });
  if (keys.length > 150) out.push({ level: 'err', text: `${keys.length} abilities: a save remembers at most 150 learned abilities per character.` });
  for (const t of ckCharRecomp()) out.push({ level: 'info', text: 'Recomp only: ' + t });
  for (const m of S.moves || []) { const hs = ckMoveHooks(m); if (hs.length) out.push({ level: 'info', text: `Recomp only: ${m.name} uses ${[...new Set(hs.map((h) => h.kind + '='))].join(', ')}; a stock game plays it without.` }); }
  if (S.gmo && S.gmo.motions.length > 255) out.push({ level: 'warn', text: `${S.gmo.motions.length} animations in the model: retail ones have at most about 90; many more is untested.` });
  return out;
}

/* ---------------------------------------------------------------- the Changes panel */
const ckJSON = (t) => { try { return JSON.parse(t); } catch (e) { return null; } };
function ckDiffList(a, b, keyOf, label) {     // records by key: added, removed, changed
  const A = new Map((a || []).map((x) => [keyOf(x), x])), B = new Map((b || []).map((x) => [keyOf(x), x])), out = [];
  for (const [k, x] of B) if (!A.has(k)) out.push(`+ ${label(x)}`);
  for (const [k, x] of A) if (!B.has(k)) out.push(`− ${label(x)}`);
  for (const [k, x] of B) if (A.has(k) && JSON.stringify(A.get(k)) !== JSON.stringify(x)) {
    const y = A.get(k), f = Object.keys({ ...x, ...y }).filter((n) => JSON.stringify(x[n]) !== JSON.stringify(y[n]) && !['values', 'flag_names', '_info'].includes(n));
    if (f.length) out.push(`~ ${label(x)}: ${f.slice(0, 6).map((n) => typeof x[n] !== 'object' && typeof y[n] !== 'object' ? `${n} ${y[n]} → ${x[n]}` : n).join(', ')}${f.length > 6 ? ' …' : ''}`);
  }
  return out;
}
function ckEvLabel(e) { const info = OPS.ops[hex4(opCode(e.op))]; return `${info ? info.label : e.op} ${'start' in e ? `${fmtF(e.start)}–${e.end === FOREVER ? 'end' : fmtF(e.end)}` : '@' + fmtF(e.frame)}`; }
function ckFileDiff(p, f) {
  const name = p.slice(S.charDir.length);
  if (f.bin) {
    const o = f.binOrig;
    if (!o) return { name, lines: [`new file, ${f.bin.length.toLocaleString()} bytes`] };
    const lines = [`${o.length.toLocaleString()} → ${f.bin.length.toLocaleString()} bytes`];
    try {
      if (f.vols) lines.push(...ckDiffList(edVolParse(o).recs.map((r, i) => ({ ...r, i })), edVolParse(f.bin).recs.map((r, i) => ({ ...r, i })), (r) => r.group + ':' + r.i, (r) => `group ${r.group} sphere (kind ${r.kind}, r ${+r.r.toFixed(2)})`));
      else if (f.brat) lines.push(...ckDiffList(mvBratRows(o).rows, mvBratRows(f.bin).rows, (r) => r.cmd, (r) => `CPU row for command ${cpHex(r.cmd, 2)}`));
      else if (f.snd) { const a = SndBank.parseBank(o), b = SndBank.parseBank(f.bin); if (b.slots.length !== a.slots.length) lines.push(`${b.slots.length - a.slots.length > 0 ? '+' : ''}${b.slots.length - a.slots.length} sound slots (${a.slots.length} → ${b.slots.length})`); }
      else if (f.text_ && typeof OBJTEXT !== 'undefined') lines.push(...ckDiffList(OBJTEXT.parseText(o).langs[0], OBJTEXT.parseText(f.bin).langs[0], (r) => r.index, (r) => `banner ${r.index} “${r.text}”`));
      else if (f.gmo) { const a = parseGMO(o), b = parseGMO(f.bin); if (b.motions.length !== a.motions.length) lines.push(`+${b.motions.length - a.motions.length} animations: ${b.motions.slice(a.motions.length).map((m) => m.name).join(', ')}`); }
    } catch (e) { lines.push('(could not read: ' + e.message + ')'); }
    return { name, lines };
  }
  const a = ckJSON(f.text), b = f.obj;
  if (!a) return { name, lines: ['new file' + (b && b.motion != null ? ` (events of ${motionName(b.motion)}: ${b.ranged.length + b.keyed.length})` : '')] };
  const lines = [];
  if (f.mod) {
    const A = a.move_names || {}, B = b.move_names || {};
    for (const k of Object.keys(B)) if (!k.startsWith('_') && !(k in A)) lines.push(`+ ability ${k}: ${B[k].name || B[k].was || ''}`);
    for (const k of Object.keys(A)) if (!k.startsWith('_') && !(k in B)) lines.push(`− ability ${k}`);
    for (const k of Object.keys(B)) if (k in A && JSON.stringify(A[k]) !== JSON.stringify(B[k])) { const fs = Object.keys({ ...A[k], ...B[k] }).filter((n) => JSON.stringify(A[k][n]) !== JSON.stringify(B[k][n])); lines.push(`~ ability ${k} (${B[k].name || B[k].was || ''}): ${fs.join(', ')}`); }
    for (const top of Object.keys({ ...a, ...b })) if (top !== 'move_names' && JSON.stringify(a[top]) !== JSON.stringify(b[top])) lines.push(`~ ${top}`);
  } else if (b.commands) lines.push(...ckDiffList(a.commands, b.commands, (c) => c.id, (c) => `command ${c.id}${c._info ? ' ' + c._info : ''} (${c.steps.length} press${c.steps.length > 1 ? 'es' : ''})`));
  else if (b.actions) lines.push(...ckDiffList(a.actions, b.actions, (x) => x.action, (x) => `action ${x.action} (script ${x.script})`));
  else if (b.rows) lines.push(...ckDiffList(a.rows, b.rows, (r) => r.script + ':' + r.phase, (r) => `script ${r.script} → ${motionName(r.motion)}`));
  else if (b.hits) lines.push(...ckDiffList(a.hits, b.hits, (h) => h.id, (h) => `hit ${h.name || h.id}`));
  else if (b.records && b.pool) { lines.push(...ckDiffList(a.records, b.records, (r) => r.id, (r) => `projectile ${r.id}`)); if (a.pool.length !== b.pool.length) lines.push(`pool ${a.pool.length} → ${b.pool.length} values`); }
  else if (b.files) { const A = new Set(a.files); for (const x of b.files) if (!A.has(x)) lines.push('+ ' + x); for (const x of a.files) if (!b.files.includes(x)) lines.push('− ' + x); }
  else if (b.ranged) {
    const sig = (e) => JSON.stringify(e), A = [...a.ranged, ...a.keyed].map(sig), B = [...b.ranged, ...b.keyed].map(sig);
    const added = B.filter((x) => !A.includes(x)), removed = A.filter((x) => !B.includes(x));
    added.forEach((x) => lines.push('+ ' + ckEvLabel(JSON.parse(x)))); removed.forEach((x) => lines.push('− ' + ckEvLabel(JSON.parse(x))));
    lines.unshift(motionName(b.motion));
  }
  if (!lines.length) lines.push('reformatted only');
  return { name, lines };
}
function renderChangesPanel(I) {
  const ch = Object.entries(S.files).filter(([, f]) => fileChanged(f)).map(([p, f]) => ckFileDiff(p, f));
  if (S.effe && S.effe.dirty) ch.push({ name: S.effe.path ? S.effe.path.slice(S.charDir.length) : 'effects', lines: [`effect package ${(S.effe.orig || S.effe.bytes).length.toLocaleString()} → ${S.effe.bytes.length.toLocaleString()} bytes${S.fx ? `, ${S.fx.fep.names.length} effects` : ''}`] });
  I.innerHTML = `<div class="sec"><h3>Changes <small>not saved yet</small> <button type="button" id="chgClose">Close</button></h3>
    ${ch.length ? ch.map((c) => `<details class="edp" open><summary class="lbl mono">${esc(c.name)}</summary><div class="chg">${c.lines.map((l) => `<div class="${l[0] === '+' ? 'add' : l[0] === '−' ? 'del' : l[0] === '~' ? 'mod' : ''}">${esc(l)}</div>`).join('')}</div></details>`).join('') : '<p class="note">Nothing changed since the last save.</p>'}
    <p class="note">Save writes these into the mod folder; the next build (or run.sh at game start) packs them into the character's objx and models. Nothing else in the game changes.</p></div>`;
  $('chgClose').onclick = () => { S.showChanges = false; renderInspector(); };
}
(function ckWire() {
  const st = $('status'); if (!st) return;
  st.style.cursor = 'pointer';
  st.title = 'Click to see what changed';
  st.addEventListener('click', () => { if (!dirtyCount()) return; S.showChanges = true; S.showChecks = false; renderInspector(); });
})();

/* ---------------------------------------------------------------- what only the recomp does */
// runtime_hooks are read by the native build's runtime (runtime/roster.cpp); a stock game (PSP, PPSSPP, a patched ISO)
// ignores them, and an extra character needs the runtime's relocated tables (docs/roster.md).
const CK_HOOK_INFO = { warp: 'homes its travel onto the foe', warpreach: 'homing reach', vtrack: 'eases to the foe\'s height', warpfx: 'thrown sword / warp wisps',
  landact: 'starts an action at touchdown', hitact: 'starts an action when its projectile spawns', chainact: 'a chain window starts an action', pinspin: 'pinned spin',
  apocarms: 'EX weapon parts and their shots', projblock: 'blocks projectiles', wdopen: 'warp dash links into press 1', hitcam: 'camera on the follow-up',
  dash: 'dash speed', exranged: 'EX Mode projectile immunity', armiger: 'EX-only weapon parts', exarms: 'EX arms', armtrail: 'trails on the EX weapons', warpdash: 'R+△ warp dash' };
function ckHooks() {                              // [{kind, tok, motions, actions, magi, cmds}] of mod.json runtime_hooks
  if (!S.mod || S.mod.kind === 'retail' || (S.rig && S.rig.retail)) return [];
  return (S.mod.runtime_hooks || []).filter((t) => typeof t === 'string' && t.includes('=')).map((tok) => {
    const kind = tok.slice(0, tok.indexOf('=')), f = tok.slice(kind.length + 1).split(','), n = (s) => String(s || '').split('/').map(Number).filter((x) => !isNaN(x));
    const h = { kind, tok, motions: [], actions: [], magi: [], cmds: [] };
    if (['warp', 'warpreach', 'vtrack', 'warpfx', 'pinspin', 'apocarms', 'projblock'].includes(kind)) h.motions = n(f[0]);
    else if (kind === 'landact') { h.actions = n(f[0]); h.motions = f.slice(1).map(Number); }
    else if (kind === 'hitact') { h.magi = n(f[0]); h.actions = n(f[1]); }
    else if (kind === 'chainact') h.actions = f.map(Number);
    else if (kind === 'wdopen') h.cmds = f[0].split('/').map((x) => parseInt(x, 16));
    return h;
  });
}
function ckMoveHooks(m) {                         // the hooks a move relies on in game
  if (!m || m.burst) return [];
  const H = ckHooks(); if (!H.length) return [];
  const mot = new Set(m.steps.map((s) => s.motion)), act = new Set(m.steps.map((s) => s.action)), P = mvProjMap(), magi = new Set();
  for (const mo of mot) { const p = S.tlOfMotion.get(mo); if (!p) continue; for (const e of [...F(p).ranged, ...F(p).keyed]) if (e.projectile !== undefined) for (const x of mvProjChain(e.projectile, P)) magi.add(x); }
  return H.filter((h) => h.motions.some((x) => mot.has(x)) || h.actions.some((x) => act.has(x)) || h.magi.some((x) => magi.has(x)) || h.cmds.includes(m.id) ||
    (h.kind === 'landact' && h.motions.some((x) => mot.has(x))));
}
function ckRecompHTML(m) {                        // the move panel's note
  const hs = ckMoveHooks(m); if (!hs.length) return '';
  const kinds = [...new Set(hs.map((h) => h.kind))];
  return `<p class="note rt-note"><b>Needs the recomp:</b> in game this move uses ${kinds.map((k) => `<span class="mono">${k}=</span> (${esc(CK_HOOK_INFO[k] || 'runtime hook')})`).join(', ')}. A stock game (PSP, PPSSPP, a patched ISO) ignores runtime hooks, so there it plays the plain animation and events.</p>`;
}
function ckCharRecomp() {                         // why the character as a whole needs the recomp: [text]
  const out = [], m = S.mod || {};
  if (!S.modPath || m.kind === 'retail' || (S.rig && S.rig.retail)) return out;
  const c = m.character || {};
  out.push(`A new roster slot (${c.slot ?? '?'}): extra characters need the recomp's runtime (it moves the character, object and ability tables into bigger blocks and gives the character a save record of its own). A stock game can't add characters; change a retail one instead (open it from the ISO, Save as mod).`);
  const own = ckHooks().filter((h) => !h.motions.length && !h.actions.length && !h.magi.length && !h.cmds.length);
  if (own.length) out.push(`Character-wide runtime hooks: ${[...new Set(own.map((h) => h.kind))].map((k) => `${k}= (${CK_HOOK_INFO[k] || 'runtime hook'})`).join(', ')}.`);
  return out;
}

/* ---------------------------------------------------------------- unused effects */
// An effect is used when one of its placements (FEPA ids) is played by a timeline event, a projectile, a hit's contact
// bit, the engine (trig records, dodges, 8888 / 9999 bone effects), is bound to one of the model's animations, or may be
// played by the runtime (any number in a hook), an unnamed event value, or a timeline the studio can't decode. Only effects none of that reaches are offered for removal.
function ckEffectRefs() {
  const ids = new Set(), add = (v) => { if (v && typeof v === 'object' && 'f' in v) v = v.f; if (typeof v === 'number') ids.add(v); };
  const EFX = new Set(['effect', 'alt_effect', 'record']);
  for (const p of S.tlPaths || []) {
    const t = F(p); if (!t) continue;
    for (const e of [...t.ranged, ...t.keyed]) {
      for (const [k, v] of Object.entries(e)) if (EFX.has(k)) add(v);
      const c = opCode(e.op); if ([0x0219, 0x042F, 0x0466, 0x0335].includes(c) || c === 0x042A || c === 0x055F) argsOf(e).forEach(add);
      if (Array.isArray(e.args)) e.args.forEach(add);                            // unnamed values: maybe an effect id
      if (typeof e.op === 'string' && e.op.startsWith('0x')) Object.values(e).forEach(add);
    }
  }
  const grp = (g) => { if (S.fx && g !== 0xFFFF && g < 64) for (const x of S.fx.fepa) if (fxGroupHas(x, g)) add(x.id); };   // a projectile's look is a group bit
  for (const pr of ((S.fx && S.fx.magi) ? S.fx.magi.values() : [])) { grp(pr.effect); if (pr.origin === 6) add(pr.origin_arg); }
  for (const r of ((F(msPath('projectiles.json')) || {}).records || [])) { grp(r.effect); if (r.origin === 6) add(r.origin_arg); }
  for (const h of (S.mod && S.mod.runtime_hooks) || []) if (typeof h === 'string') for (const x of h.split(/[=,/:]/)) { const v = parseInt(x, 10); if (!isNaN(v)) ids.add(v); }
  return ids;
}
async function ckRawTimelineIds() {               // u32 values in timelines the studio couldn't decode (kept, to be safe)
  const out = new Set(), idx = F(msPath('timelines/index.json'));
  for (const fn of (idx ? idx.files : []).filter((x) => !x.endsWith('.json'))) {
    try { const b = await FS.read(msPath(fn)), dv = new DataView(b.buffer, b.byteOffset, b.byteLength); for (let o = 0; o + 4 <= b.length; o += 2) out.add(dv.getUint32(o, true)); } catch (e) { out.add(-1); }
  }
  return out;
}
async function ckUnusedEffects() {                // [ni] nothing reaches
  if (!S.fx) return [];
  const refs = ckEffectRefs(), raw = await ckRawTimelineIds();
  if (raw.has(-1)) return [];                     // a timeline we couldn't even read: offer nothing
  const out = [], motions = new Set(((S.gmo && S.gmo.motions) || []).map((m) => m.name));
  S.fx.fep.names.forEach((nm, ni) => {
    const recs = fxuRecs(ni);
    const bound = (n) => motions.has(n) || (n.includes('X') && [...motions].some((m) => new RegExp('^' + n.replace(/X/g, '.') + '$').test(m)));   // _8X8A: any variant
    if (recs.some((r) => r.motion && bound(r.motion))) return;   // bound to one of this model's animations: the game may play it with it (EX Burst effects are)
    if (/_(DOGE|UKEMI)_/.test(nm.name || '')) return;
    if (recs.some((r) => r.mask || r.trig || r.id >= 8000 || refs.has(r.id) || raw.has(r.id))) return;   // 8888 / 9999 / 9002 …: ids the engine looks up
    out.push(ni);
  });
  return out;
}
async function ckRemoveUnused() {
  if (S.readOnly) { readOnlyNote(); return; }
  const list = await ckUnusedEffects();
  if (!list.length) { toast('Every effect is used by something (or could be), so none can go.'); return; }
  const org = typeof fxuOrigins === 'function' ? fxuOrigins() : null, nm = (ni) => `${ni} ${fxuShort(S.fx.fep.names[ni].name)}`;
  const tag = (((S.mod && S.mod.character) || {}).code || '').slice(0, 3).toUpperCase();     // without the ISO: the character's own effects carry its code
  const own = org ? list.filter((ni) => org[ni] && org[ni].kind !== 'same') : list.filter((ni) => tag && (S.fx.fep.names[ni].name || '').includes(tag + '_')), retail = list.filter((ni) => !own.includes(ni));
  const show = (l) => l.slice(0, 30).map(nm).join('\n') + (l.length > 30 ? `\n… and ${l.length - 30} more` : '');
  let pick = [];
  if (retail.length && await uiConfirm(`Remove ${retail.length} unused effect${retail.length > 1 ? 's' : ''}?`, `They came with the based-on character and nothing plays them: no event, projectile, hit, runtime hook, animation or engine rule points at them. Undo brings them back.\n\n${show(retail)}`, { ok: 'Remove' })) pick = pick.concat(retail);
  if (own.length && await uiConfirm(`Remove ${own.length} of your own effect${own.length > 1 ? 's' : ''} too?`, `${own.length > 1 ? 'These new or changed effects aren\'t' : 'This new or changed effect isn\'t'} played by anything either (maybe earlier versions):\n\n${show(own)}`, { ok: 'Remove', cancel: 'Keep' })) pick = pick.concat(own);
  if (!pick.length) return;
  list.length = 0; list.push(...pick);
  const n0 = S.fx.fep.names.length;
  const done = fxuEdit((pkg) => { for (const ni of [...list].sort((a, b) => b - a)) fxeDeleteEffect(pkg, ni); return true; });
  if (done) { S.fxEdit = null; S.tab = 'fx'; renderAll(); runChecks(); toast(`Removed ${list.length} unused effect${list.length > 1 ? 's' : ''}: ${n0 - list.length} of 128 in use now.`); }
}
