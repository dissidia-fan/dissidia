/* Move Studio: the move itself (its ability: brave or HP, ground or midair, name, help, in the kit or not), new moves,
   and playing a whole move press after press. Loaded after app.js.

   Where a move lives:
     mod.json move_names[KEY]    the ability (KEY = its id's low byte): cmd, name, help, type, phase, after, disabled;
                                 "copy": K adds a new ability (a copy of ability K's record: its CP cost and bonus)
                                 and "learn": N lists it at level N (tools/roster_mainbin.py add_new_abilities)
     moveset moves.json          command cmd -> its presses (action ids)
     moveset actions.json        each action -> a script id (raw: the rest of the 116-byte record)
     moveset motion_map.json     each script id -> the animation (and its event timeline)
     moveset sections/*brat*     which commands the CPU picks, and how keenly in each situation
   Brave or HP is the ability's type (what the Abilities screen offers it as); the damage each hit does is its own
   hits.json flag (0x02 = HP). The move panel shows both and lines them up. */

const MV_CHAIN = [0x0101, 0x0467, 0x0278, 0x0579];
const MV_START_BYTE = 5;                          // plya +5 of a first press: 2 = starts on the ground, 1 = in the air
const MV_START = { ground: 2, midair: 1 };
const MV_FOLLOW_BYTE = 3;                         // plya +3: 1 = only as a later press (a first press must have 0)
const MV_KINDS = [['brave', 'Brave'], ['hp', 'HP']];
const MV_PHASES = [['ground', 'Ground'], ['midair', 'Midair'], ['follow-up', 'Follow-up']];

const mvNames = () => (S.mod && S.mod.move_names) || {};
const mvEntry = (m) => m && m.info ? mvNames()[m.info.key] : null;
function mvCanEdit() {
  return !S.readOnly && !!S.modPath && S.mod && S.mod.kind !== 'retail' && !!S.mod.move_names;
}
function mvWhyNot() {
  if (S.rig && S.rig.retail) return 'Retail moves can\'t be renamed.';
  if (S.mod && S.mod.kind === 'retail') return 'This mod changes a retail character: its abilities stay the retail ones.';
  if (!S.modPath) return 'mod.json could not be read, so the ability can\'t be changed here.';
  return '';
}
function mvDonor() {                              // the retail ability table the character's records were copied from
  const c = (S.mod && S.mod.character) || {};
  if (S.rig && S.rig.retail) return S.abilities[S.rig.retail.code] || null;
  return S.abilities[c.based_on] || null;
}
function mvKind(m) {                              // {type, phase} as the build will set them (null = unknown without the ISO)
  const e = mvEntry(m); if (!e) return { type: null, phase: null };
  const d = mvDonor(), r = d && d[e.copy || m.info.key];
  return { type: e.type || (r && r.type) || null, phase: e.phase || (r && r.phase) || null };
}
/* for the list and its groups: mvKind, with what it leaves unset read from the move's first own press (its button:
   plya +0x0F; where it starts: plya +5), so a remade move without type / phase in mod.json isn't under "Other" (request
   2026-11-01). guessed: something came from the press; type or phase still null = it can't be worked out */
function mvKindGuess(m) {
  const k = mvKind(m); if ((k.type && k.phase) || m.burst) return k;
  const s = m.steps[mvOwnFrom(m)], a = s && S.actionById && S.actionById.get(s.action); if (!a || !a.raw) return k;
  const b = mvRaw(a)[MV_START_BYTE];
  const phase = k.phase || (m.chainOf != null ? 'follow-up' : b === MV_START.ground ? 'ground' : b === MV_START.midair ? 'midair' : null);
  return { type: k.type || (mvPressHP(a) ? 'hp' : 'brave'), phase, guessed: true };
}

/* the hits a move deals: its presses' hit events, and the projectiles they fire (with the bullets those spawn) */
function mvProjMap() { return new Map(((F(msPath('projectiles.json')) || {}).records || []).map((r) => [r.id, r])); }
function mvProjChain(pid, P, out = new Set()) {
  if (out.has(pid) || !P.has(pid)) return out;
  out.add(pid);
  const r = P.get(pid);
  for (const [k, v] of Object.entries(r)) if (k.startsWith('link_') && typeof v === 'number' && v >= 0) mvProjChain(v, P, out);
  return out;
}
function mvMotions(m) { return [...new Set(m.steps.map((s) => s.motion).filter((x) => x >= 0))]; }
function mvHits(m) {
  const out = [], P = mvProjMap(), seen = new Set(), picks = hkPicks();
  // a charge's releases (charge=) count as the press after the charge; every member of a magipick= set can be the shot
  const steps = [...m.steps];
  for (const c of hkCharges()) if (m.steps.some((x) => x.motion === c.motion)) for (const a of c.acts) steps.push({ motion: hkActionMotion(a) });
  steps.forEach((s, i) => {
    if (s.motion < 0 || seen.has(s.motion)) return;
    seen.add(s.motion);
    const p = S.tlOfMotion.get(s.motion); if (!p) return;
    const t = F(p);
    for (const kind of ['ranged', 'keyed']) t[kind].forEach((e, idx) => {
      const tick = kind === 'ranged' ? e.start : e.frame;
      if (e.hit !== undefined) { const h = hitOf(e.hit); if (h) out.push({ h, press: i, tick, path: p, kind, idx }); }
      if (opCode(e.op) === 0x0204) {
        const set = picks.get(argsOf(e)[0]);
        for (const id of set || [argsOf(e)[0]]) for (const pid of P.has(id) ? mvProjChain(id, P) : []) { const h = hitOf(P.get(pid).atkp); if (h) out.push({ h, press: Math.min(i, m.steps.length), tick, pid, pick: !!set }); }
      }
    });
  });
  out.sort((a, b) => a.press - b.press || a.tick - b.tick);
  const once = new Map();
  for (const x of out) { const k = x.h.id + '|' + x.press + '|' + (x.pick ? 'pick' : x.pid != null ? 'p' + x.pid : 'e'); if (!once.has(k)) once.set(k, x); }
  return [...once.values()];
}
const mvIsHP = (h) => !!(parseInt(h.flags, 16) & 2);
const mvDamaging = (h) => mvIsHP(h) || h.power > 0;

/* ====================================================== the move panel */
/* the move's inspector (canvas review 2026-10-31): the preview switches on top, out of any section; the move's name
   with its type, start and chain attacks; Type; Details (name, help, cost); the Ability Builder; the Danger zone
   (Enabled, Delete); Metadata (ids, where it was copied from) in a fold at the very bottom. All inside #mvSec. */
function mvMoveHTML() {
  if (S.moves[S.step.move].burst && typeof burstHTML === 'function') return burstHTML(S.moves[S.step.move]);
  const mi = S.step.move, m = S.moves[mi], e = mvEntry(m), can = mvCanEdit() && !!e, k = mvKind(m);
  const ro = can ? '' : ' disabled';
  const hits = mvHits(m).filter((x) => mvDamaging(x.h)), nHP = hits.filter((x) => mvIsHP(x.h)).length;
  const seg = (name, list, val) => `<span class="mvseg" role="group">${list.map(([v, l]) => `<button type="button" data-${name}="${v}" aria-pressed="${val === v}"${ro}>${l}</button>`).join('')}</span>`;
  const KIND = Object.fromEntries(MV_KINDS), PHASE = { ground: 'Ground', midair: 'Air', 'follow-up': 'Follow-up' };
  const sum = [KIND[k.type], PHASE[k.phase], `${m.steps.length} chain attack${m.steps.length === 1 ? '' : 's'}`].filter(Boolean).join(' · ');
  const d = e ? mvDonor() : null, r = d && d[e.copy !== undefined ? String(e.copy).toUpperCase().padStart(2, '0') : m.info.key];
  const meta = `<div class="sec"><details class="acc" id="mvMeta"><summary>Metadata</summary>
    <p class="note mono meta-ids">command ${esc(m.hexId)}${m.info ? ' · ability ' + esc(m.info.key) : ''}<br>${m.steps.map((x, i) => `attack ${i + 1}: action ${x.action} · script ${x.script}`).join('<br>')}</p>
    ${typeof cpNotesHTML === 'function' ? cpNotesHTML(m).replace(/<details class="edp" open><summary class="lbl">([^<]*)<\/summary>/, '<div class="pgrp"><h4>$1</h4>').replace(/<\/details>$/, '</div>') : ''}
    ${e ? `<p class="note">${e.copy !== undefined ? `The record is a copy of ability ${esc(e.copy)}. ` : ''}Empty cost fields use the copied record's values; CP once mastered defaults to half. The help text's {bonus} line names the equip bonus.</p>` : ''}</details></div>`;
  // the preview switches (request: "Move this to the top and take it out of any section")
  let h = `<div id="mvSec" class="mvwrap"><div class="mv-preview">
    <div class="field"><label for="mvCut">Preview: Chain Attack</label><select id="mvCut" title="When the next chain attack takes over in the preview: at its first chain window, as in a fight, or after each attack plays in full"><option value="chain"${S.mvCut !== 'end' ? ' selected' : ''}>At first window</option><option value="end"${S.mvCut === 'end' ? ' selected' : ''}>Play full attack</option></select></div>
    ${hkPrevHTML(m)}</div>`;
  // (no header section of its own: 2026-10-31 "The section with the ability name here is empty, remove it"; its
  // notes, when there are any, open the Type section)
  if (!e) return h + `<div class="sec"><p class="note">No ability in mod.json points at command ${esc(m.hexId)}, so it isn't in the character's kit (it may be played by another move's event).</p></div>${meta}</div>`;
  let notes = '';
  if (m.versions && m.versions.length) notes += `<p class="note">The game also plays it shorter: ${m.versions.map((v) => `the first ${v.steps.length === 1 ? 'chain attack' : v.steps.length + ' chain attacks'} (command ${esc(v.hexId)})`).join(', ')}: the engine picks which one plays (for Shantotto's spells, likely by her Brave). They're these same attacks, so editing them here changes every version.</p>`;
  if (m.branchAt != null) { const p = S.moves.find((x) => x.id === m.variantOf); notes += `<p class="note">${m.branchAt > 0 ? `A branch of ${esc(p ? p.name : 'command ' + m.variantOf)}: the same first ${m.branchAt === 1 ? 'chain attack' : m.branchAt + ' chain attacks'}, then from attack ${m.branchAt + 1} it plays its own (ending in ${esc(m.branchTag || 'other actions')})` : `A version of ${esc(p ? p.name : 'command ' + m.variantOf)} with its own chain attacks from the first (${esc(m.branchTag || 'other actions')}), played instead of it`}${m.branchEx ? ': its actions are named _EX, so this looks like the EX Mode version' : ''}. The game picks which plays. Same ability, so the name and help are shared.</p>`; }
  else if (m.variantOf != null) { const p = S.moves.find((x) => x.id === m.variantOf); notes += `<p class="note">A shorter version of ${esc(p ? p.name : 'command ' + m.variantOf)}: its first ${m.steps.length} chain attack${m.steps.length > 1 ? 's' : ''}, which the game plays instead of the full move. It's the same ability, so the name and help below are shared with it.</p>`; }
  if (!can) notes += `<p class="note">${esc(mvWhyNot())}</p>`;
  if (typeof ckRecompHTML === 'function') notes += ckRecompHTML(m);
  // Type: what it is and where it starts, with the fixes when the presses disagree
  h += `<div class="sec" id="mvType"><h3>Type</h3>${notes}
    <div class="field"><label>Attack</label>${seg('type', MV_KINDS, k.type)}</div>
    <div class="field"><label>Starts on</label>${seg('phase', MV_PHASES.map(([v, l]) => [v, PHASE[v] || l]), k.phase)}</div>`;
  if (k.phase === 'follow-up') {
    const opts = Object.entries(mvNames()).filter(([kk, v]) => !kk.startsWith('_') && v && kk !== m.info.key)
      .map(([kk, v]) => `<option value="${kk}"${String(e.after || '').toUpperCase().replace(/^0X/, '') === kk ? ' selected' : ''}>${esc(v.name || v.was || kk)} (${kk})</option>`).join('');
    h += `<div class="field"><label>Follows up</label><select id="mvAfter"${ro}><option value="">(as copied)</option>${opts}</select></div>`;
  }
  if (!k.type || !k.phase) h += `<p class="note">${S.abilities && Object.keys(S.abilities).length ? '' : 'Set the game ISO to see what the copied record has. '}Unset = as the copied ${esc((S.mod.character || {}).based_on || 'retail')} record has it.</p>`;
  if (k.type === 'hp' && !nHP && hits.length) h += `<p class="warnline">An HP attack with no HP hit deals no HP damage.</p>${can ? '<div class="btnrow"><button type="button" id="mvFixHP">Make the last hit an HP hit</button></div>' : ''}`;
  if (k.type === 'brave' && nHP) h += `<p class="warnline">A brave attack with ${nHP} HP hit${nHP > 1 ? 's' : ''}: they still deal HP damage.</p>${can ? '<div class="btnrow"><button type="button" id="mvFixBrave">Make its hits brave</button></div>' : ''}`;
  if (can) {
    const first = m.steps[0] && S.actionById.get(m.steps[0].action), b = first ? mvRaw(first)[MV_START_BYTE] : null;
    const want = MV_START[k.phase];
    if (want && b !== want && b != null) h += `<p class="warnline">Its first chain attack (action ${first.action}) is set to start ${b === 2 ? 'on the ground' : b === 1 ? 'in the air' : 'anywhere'}.</p><div class="btnrow"><button type="button" id="mvFixStart">Start it ${k.phase === 'ground' ? 'on the ground' : 'in the air'} too</button></div>`;
  }
  const wrong = mvWrongButton(m);
  if (wrong.length) h += `<p class="warnline">Chain attack${wrong.length > 1 ? 's' : ''} ${wrong.map((s) => s.i + 1).join(', ')} answer${wrong.length > 1 ? '' : 's'} ${k.type === 'hp' ? '○ (brave)' : '□ (HP)'}, so ${k.type === 'hp' ? '□' : '○'} never starts ${wrong.length > 1 ? 'them' : 'it'}.</p>${can ? `<div class="btnrow"><button type="button" id="mvFixButton">Make ${wrong.length > 1 ? 'them' : 'it'} answer ${k.type === 'hp' ? '□' : '○'}</button></div>` : ''}`;
  h += '</div>';
  // Details: the ability record's name, help and cost (request: "Details which has name and cost")
  {
    const ph = (kk, f) => r && r[kk] != null ? `placeholder="${esc(f ? f(r[kk]) : r[kk])}"` : 'placeholder="as copied"';
    const cpm = e.cp != null && e.cp_mastered == null ? `placeholder="${Math.floor(e.cp / 2)}"` : ph('cp_mastered');
    h += `<div class="sec" id="mvDetails"><h3>Details</h3>
      <div class="field"><label>Name</label><input type="text" id="mvName" value="${esc(e.name || '')}" placeholder="${esc(e.was || m.name)}"${ro}></div>
      <div class="field"><label>Help text</label><textarea id="mvHelp" rows="3" placeholder="(the copied record's)"${ro}>${esc(e.help || '')}</textarea></div>
      <div class="row3">${numField('CP to equip', e.cp ?? '', `id="mvCp" min="0" max="999" ${ph('cp')}${ro}`)}${numField('CP mastered', e.cp_mastered ?? '', `id="mvCpm" min="0" max="999" ${cpm}${ro}`)}${numField('AP to master', e.ap ?? '', `id="mvAp" min="0" max="9999" ${ph('ap')}${ro}`)}</div>
      <div class="row2">${numField('Learnt at level', e.learn ?? '', `id="mvLearn" min="1" max="100"${ro}`)}${mvBonusHTML(e, r ? r.bonus : undefined, ro)}</div></div>`;
  }
  h += mvBuilderHTML(m, can);
  // Danger zone: switching it off (kept, hidden) and deleting it (request: "Change this to Enabled in a section next to the delete")
  const del = !m.burst && can;
  h += meta;                                     // Metadata, then the Danger zone last (request: "Danger zone should be on the bottom")
  h += `<div class="sec danger" id="mvDanger"><h3>Danger zone</h3>
    <label class="toggle-row"><span>Enabled</span><input type="checkbox" role="switch" class="switch" id="mvOn"${e.disabled ? '' : ' checked'}${ro}></label>
    <p class="note">Off: hidden from the game's ability list and does nothing in battle. Saves stay valid.</p>
    ${del ? `<div class="sep"></div><p class="note">Removes ${esc(m.name)}'s command, chain attacks and CPU row. The ability record stays, switched off, so saves stay valid. Undo brings it back until you save.</p>
    <div class="btnrow"><button type="button" id="mvDelete" class="danger">Delete this move…</button></div>` : ''}</div>`;
  return h + '</div>';
}
function mvWire() {
  const sec = $('mvSec'); if (!sec) return;
  if ($('mvWhole')) $('mvWhole').onclick = () => mvToggleWhole();
  if ($('mvCut')) $('mvCut').onchange = () => { S.mvCut = $('mvCut').value; renderTimeline(); };
  hkWire();
  const m = S.moves[S.step.move], e = mvEntry(m);
  mvPressWire(m);
  if (!e || !mvCanEdit()) return;
  const set = (fn) => { edit([S.modPath], () => fn(S.mod.move_names[m.info.key])); derive(); mvSyncBanners(); renderAll(); };
  sec.querySelectorAll('[data-type]').forEach((b) => b.onclick = () => set((x) => { x.type = b.dataset.type; }));
  sec.querySelectorAll('[data-phase]').forEach((b) => b.onclick = () => set((x) => { x.phase = b.dataset.phase; if (x.phase !== 'follow-up' && 'after' in x) x.after = null; }));
  const af = $('mvAfter'); if (af) af.onchange = () => set((x) => { if (af.value) x.after = af.value; else delete x.after; });
  $('mvName').onchange = () => set((x) => { const v = $('mvName').value.trim(); if (v) x.name = v; else delete x.name; });
  $('mvHelp').onchange = () => set((x) => { const v = $('mvHelp').value.replace(/\r/g, ''); if (v.trim()) x.help = v; else delete x.help; });
  tokenHelper($('mvName'), [['{ground}', 'the ground suffix the game adds to a ground move\'s name'], ['{midair}', 'the midair suffix']]);
  tokenHelper($('mvHelp'), [['{bonus}', 'the copied record\'s equip-bonus line'], ['{effects}', 'the copied record\'s effect lines (knock-back, wall rush…)']]);
  const ln = $('mvLearn'); if (ln) ln.onchange = () => set((x) => { const v = parseInt(ln.value, 10); if (v > 0) x.learn = Math.min(100, v); else delete x.learn; });
  for (const [id, k, max] of [['mvCp', 'cp', 999], ['mvCpm', 'cp_mastered', 999], ['mvAp', 'ap', 9999]]) { const el = $(id); if (el) el.onchange = () => set((x) => { const v = parseInt(el.value, 10); if (v >= 0) x[k] = Math.min(max, v); else delete x[k]; }); }
  mvBonusWire(set);
  const dl = $('mvDelete'); if (dl) dl.onclick = () => mvDeleteMove(m);
  $('mvOn').onchange = () => set((x) => { if ($('mvOn').checked) { delete x.disabled; delete x._disabled; } else x.disabled = true; });
  const fh = $('mvFixHP'); if (fh) fh.onclick = () => { const hs = mvHits(m).filter((x) => mvDamaging(x.h)); mvSetHP(m, [hs[hs.length - 1]], true); };
  const fb = $('mvFixBrave'); if (fb) fb.onclick = () => mvSetHP(m, mvHits(m).filter((x) => mvIsHP(x.h)), false);
  const fs = $('mvFixStart'); if (fs) fs.onclick = async () => {
    const a = S.actionById.get(m.steps[0].action), want = MV_START[mvKind(m).phase];
    const others = S.moves.filter((mm) => mm !== m && mm.steps.some((s) => s.action === a.action));
    if (others.length && !(await uiConfirm('Change a shared action?', `Action ${a.action} is also ${others.map((x) => x.name).join(', ')}'s: the change applies there too.`, { ok: 'Change it' }))) return;
    edit([msPath('actions.json')], () => mvSetRaw(a, MV_START_BYTE, want));
    derive(); renderAll();
  };
}

/* plya raw bytes */
const mvRaw = (a) => Uint8Array.from(a.raw.match(/../g).map((x) => parseInt(x, 16)));
function mvSetRaw(a, off, v) { const r = mvRaw(a); r[off] = v; a.raw = [...r].map((x) => x.toString(16).padStart(2, '0')).join(''); }

/* brave / HP on a move's hits: a hit other moves use too gets a copy of its own first */
function mvCloneHit(h) {
  const hits = F(msPath('hits.json')).hits, ids = new Set(hits.map((x) => parseInt(x.id, 16)));
  let nid = Math.max(...ids) + 1; while (ids.has(nid)) nid++;
  const base = (h.name || 'HIT').slice(0, 11), names = new Set(hits.map((x) => x.name)); let k = 2;
  while (names.has(base + '_' + k)) k++;
  const c = JSON.parse(JSON.stringify(h)); c.id = hex4(nid); c.name = base + '_' + k; delete c.name_raw;
  hits.push(c);
  return c;
}
function mvSetHP(m, list, on) {
  list = list.filter(Boolean);
  if (!list.length) return;
  const mine = new Set(mvMotions(m).map(motionName)), P = mvProjMap(), myPids = new Set(), tls = mvMotions(m).map((x) => S.tlOfMotion.get(x)).filter(Boolean);
  for (const x of mvHits(m)) if (x.pid != null) myPids.add(x.pid);
  const paths = [msPath('hits.json'), msPath('projectiles.json'), ...tls].filter((p) => S.files[p]);
  let copies = 0;
  edit(paths, () => {
    const done = new Set();
    for (const x of list) {
      if (done.has(x.h)) continue;
      let h = x.h;
      const users = hitUsers(h), outside = users.filter((u) => !mine.has(u) && !(u.startsWith('projectile ') && myPids.has(+u.slice(11))));
      if (outside.length && h.raw === undefined) {
        const old = h, oldKeys = new Set([hitKey(old), old.id, parseInt(old.id, 16)]);
        h = mvCloneHit(old); copies++;
        for (const p of tls) for (const ev of [...F(p).ranged, ...F(p).keyed]) if (ev.hit !== undefined && oldKeys.has(ev.hit)) ev.hit = h.name;
        for (const pid of myPids) { const r = P.get(pid); if (r && r.atkp === parseInt(old.id, 16)) { r.atkp = parseInt(h.id, 16); if (r.atkp_reflected === parseInt(old.id, 16)) r.atkp_reflected = r.atkp; } }
        derive();
      }
      let f = parseInt(h.flags, 16); f = on ? f | 2 : f & ~2;
      h.flags = '0x' + (f >>> 0).toString(16).toUpperCase().padStart(8, '0');
      done.add(x.h);
    }
  });
  derive(); renderAll();
  toast(`${list.length} hit${list.length > 1 ? 's' : ''} now ${on ? 'HP' : 'brave'}${copies ? ` (${copies} copied first: other moves use ${copies > 1 ? 'them' : 'it'})` : ''}.`);
}

/* ====================================================== new move */
function mvFreeKey() {
  const used = new Set(Object.keys(mvNames()).filter((k) => /^[0-9A-F]{2}$/i.test(k)).map((k) => parseInt(k, 16)));
  const d = mvDonor(); if (d) for (const k of Object.keys(d)) used.add(parseInt(k, 16));
  let k = Math.max(0x0F, ...used) + 1;
  while (used.has(k)) k++;
  return k.toString(16).toUpperCase().padStart(2, '0');
}
function mvFreeCmd() {
  const ids = F(msPath('moves.json')).commands.map((c) => parseInt(c.id, 16));
  for (const v of Object.values(mvNames())) if (v && v.cmd) ids.push(parseInt(v.cmd, 16));
  return Math.max(...ids) + 1;
}
function mvTemplate(type, phase) {            // a move to borrow the ability record and first-press action from
  const ok = S.moves.filter((m) => mvEntry(m) && m.variantOf == null && m.steps.length && S.actionById.get(m.steps[0].action));
  const score = (m) => { const k = mvKind(m); return (k.type === type ? 2 : 0) + (k.phase === phase ? 1 : 0) + (mvEntry(m).copy === undefined ? 0.5 : 0); };
  return ok.sort((a, b) => score(b) - score(a))[0];
}
/* + New move (canvas review 2026-10-31): two tabs. "New move" builds the chain with the Ability Builder's rows (request:
   "This should reuse the ability builder"); "Duplicate existing move" copies a move of this character (its own chain
   attacks, the same animations) or of another one (copy.js, with its animations, hits, effects and sounds) (request:
   "we should change import to also work for the current character. Call it Duplicate Existing move"). */
function mvNewHTML() {
  const o = S.newMove, tab = o.tab || 'new';
  const tabs = `<div class="ntabs" role="tablist"><button type="button" role="tab" data-ntab="new" aria-selected="${tab === 'new'}">New move</button><button type="button" role="tab" data-ntab="dup" aria-selected="${tab === 'dup'}">Duplicate existing move</button></div>`;
  if (tab === 'dup') return `<div id="mvNew" class="mvwrap">${tabs}${mvDupHTML(o)}</div>`;
  const seg = (name, list, val) => `<span class="mvseg" role="group">${list.map(([v, l]) => `<button type="button" data-n${name}="${v}" aria-pressed="${val === v}">${l}</button>`).join('')}</span>`;
  const mos = o.motions && o.motions.length ? o.motions : [-1];   // starts empty: "Pick an animation…" (2026-11-01)
  const rows = mos.map((mo, k) => `<div class="mvpress"><i class="st-n ${o.type === 'hp' ? 'hp' : 'brave'}">${k + 1}</i>${apButtonHTML('n:' + k, mo, `Animation of chain attack ${k + 1}`, false)}${mos.length > 1 ? `<button type="button" class="mini mvx" data-nrm="${k}" title="Remove this chain attack" aria-label="Remove chain attack ${k + 1}">✕</button>` : '<span></span>'}</div>`).join('');
  return `<div id="mvNew" class="mvwrap">${tabs}
    <div class="sec"><h3>New move</h3>
      <div class="field"><label>Name</label><input type="text" id="nmName" value="${esc(o.name)}" placeholder="Move name"></div>
      <div class="field"><label>Attack</label>${seg('type', MV_KINDS, o.type)}</div>
      <div class="field"><label>Starts on</label>${seg('phase', [['ground', 'Ground'], ['midair', 'Air']], o.phase)}</div></div>
    <div class="sec" id="nmBuild"><h3>Ability Builder</h3>
      <div class="mvpresses">${rows}</div>
      ${mos[0] >= 0 ? `<button type="button" class="mvadd" data-ap="nadd" data-motion="${mos[mos.length - 1]}" title="Pick the animation of chain attack ${mos.length + 1}">+ Add chain attack</button>` : ''}</div>
    <div class="sec"><h3>Details</h3>
      <div class="field"><label>Help text</label><textarea id="nmHelp" rows="2" placeholder="[Close] What it does.">${esc(o.help)}</textarea></div>
      ${numField('Learnt at level', o.learn, 'id="nmLearn" min="1" max="100"')}</div>
    <div class="btnrow"><button type="button" id="nmCreate" class="primary"${o.name.trim() && mos[0] >= 0 ? '' : ' disabled'} title="Needs a name and the first chain attack's animation">Create move</button><button type="button" id="nmCancel">Cancel</button></div></div>`;
}
/* the Duplicate tab: a character, then one of its moves */
function mvDupSources() {
  const cur = (S.chars || []).find((c) => c.name === S.char), me = (S.mod && S.mod.character && S.mod.character.name) || (cur && cur.label) || 'This character';
  const out = [{ v: 'cur', label: me + ' (this character)', cat: '' }];
  for (const c of S.chars || []) if (c.name !== S.char) out.push({ v: 'mod:' + c.name, label: c.label, cat: 'Mod characters', src: { mod: c.name } });
  for (const c of S.retailChars || []) out.push({ v: 'iso:' + c.code, label: c.label, cat: 'From the game ISO', src: { retail: c.code } });
  return out;
}
function mvDupHTML(o) {
  const srcs = mvDupSources(), sv = o.dupSrc || 'cur';
  let opts = '', cat = '';
  for (const x of srcs) { if (x.cat !== cat) { if (cat) opts += '</optgroup>'; cat = x.cat; if (cat) opts += `<optgroup label="${esc(cat)}">`; } opts += `<option value="${esc(x.v)}"${x.v === sv ? ' selected' : ''}>${esc(x.label)}</option>`; }
  if (cat) opts += '</optgroup>';
  let list = null, err = '';
  const KIND = Object.fromEntries(MV_KINDS), PH = { ground: 'Ground', midair: 'Air', 'follow-up': 'Follow-up' };
  if (sv === 'cur') list = S.moves.map((m, i) => ({ v: 'i' + i, label: m.name, note: ((k) => [KIND[k.type] || '?', PH[k.phase] || '?', m.steps.length].join(' · '))(mvKindGuess(m)), skip: m.burst || m.variantOf != null || m.chainOf != null || !mvEntry(m) })).filter((x) => !x.skip);
  else { const c = (o.dupCache || {})[sv]; if (c && c.err) err = c.err; else if (c) list = c.list; }
  const pick = list && list.find((x) => x.v === o.dupPick);
  const body = err ? `<p class="warnline">${esc(err)}</p>` : !list ? '<p class="note">Reading its moves…</p>'
    : `<div class="dmlist" role="listbox" aria-label="Moves">${list.map((x) => `<button type="button" role="option" data-dm="${esc(x.v)}" aria-selected="${x.v === o.dupPick}"><span>${esc(x.label)}</span><small>${esc(x.note || '')}</small></button>`).join('') || '<p class="note" style="padding:6px 9px">No moves.</p>'}</div>`;
  return `<div class="sec">
      <div class="field"><label for="dmChar">Character</label><select id="dmChar">${opts}</select></div>
      <div class="field"><label for="dmQ">Move</label><input type="search" id="dmQ" placeholder="Search moves…" value="${esc(o.dupQ || '')}"></div>
      ${body}
      ${sv === 'cur' ? `<label class="chk"><input type="checkbox" id="dmOwn"${o.dupOwn !== false ? ' checked' : ''}> Give it its own animations${fTip('Each chain attack plays a copy of the animation, with a copy of its events, so editing the copy leaves the original alone. Off: the copy plays the same animations and shares their events.')}</label>` : '<p class="note">Brings its chain attacks, animations, events, hits, hitboxes, projectiles, effects and sounds.</p>'}</div>
    <div class="btnrow"><button type="button" id="dmGo" class="primary"${pick ? '' : ' disabled'}>${pick ? 'Duplicate ' + esc(pick.label) : 'Duplicate'}</button><button type="button" id="nmCancel">Cancel</button></div>`;
}
function mvWireNew() {
  const sec = $('mvNew'); if (!sec) return;
  const o = S.newMove, keep = () => { if ($('nmName')) { o.name = $('nmName').value; o.help = $('nmHelp').value; o.learn = $('nmLearn').value; } };
  sec.querySelectorAll('[data-ntab]').forEach((b) => b.onclick = () => { keep(); o.tab = b.dataset.ntab; renderInspector(); });
  $('nmCancel').onclick = () => { S.newMove = null; renderInspector(); };
  if ((o.tab || 'new') === 'dup') return mvWireDup(o);
  if (!o.motions || !o.motions.length) o.motions = [-1];
  const can = () => { const b = $('nmCreate'); if (b) b.disabled = !($('nmName').value.trim() && o.motions[0] >= 0); };
  $('nmName').oninput = can;
  sec.querySelectorAll('[data-ntype]').forEach((b) => b.onclick = () => { keep(); o.type = b.dataset.ntype; renderInspector(); });
  sec.querySelectorAll('[data-nphase]').forEach((b) => b.onclick = () => { keep(); o.phase = b.dataset.nphase; renderInspector(); });
  sec.querySelectorAll('[data-ap]').forEach((b) => b.onclick = () => apOpen(b, +b.dataset.motion, (mo) => {
    keep();
    if (b.dataset.ap === 'nadd') o.motions.push(mo); else o.motions[+b.dataset.ap.split(':')[1]] = mo;
    setMotion(mo); renderAll();
  }));
  sec.querySelectorAll('[data-nrm]').forEach((b) => b.onclick = () => { keep(); o.motions.splice(+b.dataset.nrm, 1); renderInspector(); });
  $('nmCreate').onclick = () => {
    keep();
    const mos = o.motions.filter((x) => x >= 0), name = o.name.trim();
    if (!name || !mos.length) return;
    mvCreate({ ...o, from: '', motion: mos[0] });
    const m = !S.newMove && S.step && S.moves[S.step.move];    // (mvCreate picks the new move)
    if (m && m.name === name) for (const mo of mos.slice(1)) mvAddPress(S.moves.find((x) => x.id === m.id), mo);
  };
}
function mvWireDup(o) {
  const sv = o.dupSrc || 'cur';
  $('dmChar').onchange = () => { o.dupSrc = $('dmChar').value; o.dupPick = null; renderInspector(); };
  if (sv !== 'cur' && !(o.dupCache && o.dupCache[sv])) {     // read the other character's moves once
    const x = mvDupSources().find((y) => y.v === sv);
    o.dupCache = o.dupCache || {};
    o.dupCache[sv] = null;
    cpSource(x.src, /_a$/.test(S.msDir)).then((src) => { o.dupCache[sv] = { src: x.src, list: cpMoves(src).filter((m) => m.named).map((m) => ({ v: 'c' + m.cmd, cmd: m.cmd, label: m.label, note: m.note })) }; })
      .catch((e) => { o.dupCache[sv] = { err: 'Could not read that character: ' + e.message }; })
      .finally(() => { if (S.newMove === o && (o.tab === 'dup') && (o.dupSrc || 'cur') === sv) renderInspector(); });
  }
  const q = $('dmQ'), filt = () => { const t = q.value.trim().toLowerCase(); o.dupQ = q.value; document.querySelectorAll('[data-dm]').forEach((b) => { b.hidden = !!t && !b.textContent.toLowerCase().includes(t); }); };
  q.oninput = filt; filt();
  document.querySelectorAll('[data-dm]').forEach((b) => b.onclick = () => { o.dupPick = b.dataset.dm; renderInspector(); });
  const own = $('dmOwn'); if (own) own.onchange = () => { o.dupOwn = own.checked; };
  $('dmGo').onclick = () => {
    const v = o.dupPick; if (!v) return;
    if (v[0] === 'i') {
      const src = S.moves[+v.slice(1)], k = mvKindGuess(src), se = mvEntry(src);
      const name = src.name + ' (copy)', u0 = S.undo.length;
      mvCreate({ name, help: (se && se.help) || '', type: k.type || 'brave', phase: k.phase && k.phase !== 'follow-up' ? k.phase : 'ground', from: v.slice(1), motion: src.steps[0].motion, learn: '1' });
      if (o.dupOwn !== false && S.undo.length > u0) mvDupOwnAnims(name, u0);
    } else { const c = o.dupCache[sv], it = c.list.find((x) => x.v === v); S.newMove = null; cpCopyMove(c.src, it.cmd); }
  };
}
/* Duplicate › Give it its own animations (2026-11-01, on by default): after mvCreate made the move (its presses
   play the source's animations), each press gets a copy of its animation with a copy of its events (dupMotion, copy.js),
   so editing the copy leaves the source alone. The steps are folded into the one undo step mvCreate started at u0. */
async function mvDupOwnAnims(name, u0) {
  const id = S.step && S.moves[S.step.move] && S.moves[S.step.move].id; if (id == null) return;
  let n = 0, failed = false;
  {
    const m0 = S.moves.find((x) => x.id === id);
    for (let i = 0; i < m0.steps.length; i++) {
      const m = S.moves.find((x) => x.id === id), s = m.steps[i]; if (!s || s.motion < 0) continue;
      const names = new Set(S.gmo.motions.map((x) => x.name)), base = motionName(s.motion).slice(0, 28); let k = 2;
      while (names.has(base + '_' + k)) k++;
      const before = S.undo.length;
      const r = await dupMotion(s.motion, base + '_' + k, true, { move: S.moves.indexOf(m), i }, true);   // (quiet: one summary below)
      if (r < 0 || S.undo.length === before) { failed = true; break; }
      n++;
    }
  }
  if (S.undo.length > u0 + 1) S.undo.push(S.undo.splice(u0).reverse().flat());   // one Undo puts it all back (earliest snapshot last)
  const mi = S.moves.findIndex((x) => x.id === id);
  if (mi >= 0) { S.step = { move: mi, i: 0 }; S.moveOnly = true; S.whole = null; setMotion(S.moves[mi].steps[0].motion); }
  runChecks(); renderAll();
  toast(failed ? `${name}: made, but ${n ? 'only ' + n + ' of its' : 'none of its'} animations could be copied, so it shares the rest with the original.` : `${name}: a new ability with ${n} animation${n === 1 ? '' : 's'} of its own (copies with their events). Save, build, then equip it in the game's Abilities screen.`);
}
function mvStartNew() {
  if (!mvCanEdit()) { toast(mvWhyNot() || 'This character can\'t get new moves here.'); return; }
  S.sel = null; S.showChecks = false; S.whole = null;
  S.newMove = { tab: 'new', name: '', help: '', type: 'brave', phase: 'ground', from: '', motion: -1, motions: [-1], learn: '1', dupOwn: true };
  renderAll();
}
const mvBratPath = () => Object.keys(S.files).find((p) => S.files[p].brat) || '';
function mvCreate(o) {
  const name = o.name.trim();
  if (!name) { toast('Give the move a name.'); $('nmName').focus(); return; }
  const pm = msPath('moves.json'), pa = msPath('actions.json'), pmm = msPath('motion_map.json'), pb = mvBratPath();
  const copyOf = o.from === '' ? null : S.moves[+o.from], tmpl = copyOf || mvTemplate(o.type, o.phase);
  if (!tmpl) { toast('No move to copy the ability record from.'); return; }
  const te = mvEntry(tmpl);
  if (!te) { toast(tmpl.name + ' has no ability to copy the record from: pick another move.'); return; }
  const key = mvFreeKey(), cmd = mvFreeCmd(), cmdHex = '0x' + cmd.toString(16).toUpperCase();
  const acts = F(pa).actions, rows = F(pmm).rows;
  let act = Math.max(...acts.map((a) => a.action)) + 1, scr = Math.max(...acts.map((a) => a.script), ...rows.map((r) => r.script)) + 1;
  const srcCmd = F(pm).commands.find((c) => parseInt(c.id, 16) === tmpl.id);
  const srcSteps = copyOf ? srcCmd.steps : srcCmd.steps.slice(0, 1);
  const learn = parseInt(o.learn, 10);
  edit([S.modPath, pm, pa, pmm, ...(pb ? [pb] : [])], () => {
    const steps = srcSteps.map((st, i) => {
      const a = S.actionById.get(st.action), na = { action: act, script: scr, raw: a.raw };
      if (i === 0) { mvSetRaw(na, MV_FOLLOW_BYTE, 0); if (MV_START[o.phase]) mvSetRaw(na, MV_START_BYTE, MV_START[o.phase]); }
      acts.push(na);
      const row = rows.find((r) => r.script === a.script);
      rows.push({ script: scr, motion: copyOf ? (row ? row.motion : 0) : o.motion, phase: row ? row.phase : 0 });
      const s = { action: act, b: st.b, c: st.c, flags: st.flags };
      act++; scr++;
      return s;
    });
    F(pm).commands.push({ id: cmdHex, prereq: [65535, 65535], steps, _info: `${name} (ability ${key})` });
    const ent = { copy: te.copy !== undefined ? te.copy : tmpl.info.key, cmd: cmdHex, type: o.type, phase: o.phase };
    if (learn > 0) ent.learn = Math.min(100, learn);
    ent.status = 'new'; ent.name = name;
    if (o.help.trim()) ent.help = o.help.replace(/\r/g, '');
    ent._note = 'Made in the Move Studio' + (copyOf ? ` from ${copyOf.name}` : '');
    S.mod.move_names[key] = ent;
    if (pb) S.files[pb].bin = mvBratAdd(S.files[pb].bin, cmd, tmpl.id);
  });
  S.newMove = null;
  derive();
  const mi = S.moves.findIndex((m) => m.id === cmd);
  S.step = { move: mi, i: 0 }; S.moveOnly = true; setMotion(S.moves[mi].steps[0].motion);
  runChecks(); renderAll();
  toast(`${name}: ability ${key}, command ${cmdHex}. Save, build, then equip it in the game's Abilities screen${learn > 0 ? ` (learnt at level ${learn})` : ''}.`);
}

/* brat: 'atta', f32, u32, u32, u32 N, N x {u32 command, 6 x (u16 weight offset, u16 count)}, u32 M, M weight codes */
function mvBratRows(bin) {
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength), n = dv.getUint32(16, true), rows = [];
  for (let i = 0; i < n; i++) { const o = 20 + 28 * i, sit = []; for (let k = 0; k < 6; k++) sit.push([dv.getUint16(o + 4 + 4 * k, true), dv.getUint16(o + 6 + 4 * k, true)]); rows.push({ cmd: dv.getUint32(o, true), sit }); }
  const mo = 20 + 28 * n, M = dv.getUint32(mo, true);
  return { head: bin.slice(0, 16), rows, w: Array.from(bin.slice(mo + 4, mo + 4 + M)) };
}
function mvBratAdd(bin, cmd, fromCmd) {
  const b = mvBratRows(bin);
  if (b.rows.some((r) => r.cmd === cmd)) return bin;
  const src = b.rows.find((r) => r.cmd === fromCmd);
  const sit = (src ? src.sit : [[0, 0], [0, 0], [0, 0], [0, 0], [0, 0], [0, 0]]).map(([off, n]) => { const o = b.w.length; b.w.push(...b.w.slice(off, off + n)); return [o, n]; });
  b.rows.push({ cmd, sit });
  const size = 20 + 28 * b.rows.length + 4 + b.w.length, out = new Uint8Array((size + 3) & ~3), dv = new DataView(out.buffer);
  out.set(b.head, 0); dv.setUint32(16, b.rows.length, true);
  b.rows.forEach((r, i) => { const o = 20 + 28 * i; dv.setUint32(o, r.cmd, true); r.sit.forEach(([off, n], k) => { dv.setUint16(o + 4 + 4 * k, off, true); dv.setUint16(o + 6 + 4 * k, n, true); }); });
  const mo = 20 + 28 * b.rows.length; dv.setUint32(mo, b.w.length, true); out.set(b.w, mo + 4);
  return out;
}

/* ====================================================== the whole move, press after press */
/* ====================================================== runtime-hook previews: charge= levels, magipick= picks
   charge=MOTION,MIN,T2,T3,MAX,ACT1,ACT2,ACT3 (roster.cpp charge_tick): the game holds MOTION while the HP button is held
   and starts ACT1 / ACT2 / ACT3 by the hold time; magipick=BASE,r|c,ID1.. fires one of a set in place of BASE. The
   studio can't hold a button or roll dice, so the move panel picks what the preview shows: a charge level (released at
   MIN, T2 or T3 ticks) and which member of each set fires. */
const HK_PREV = { level: 3, pick: 0 };
function hkTokens(kind) {
  if (!S.mod || S.mod.kind === 'retail' || (S.rig && S.rig.retail)) return [];
  return (S.mod.runtime_hooks || []).filter((t) => typeof t === 'string' && t.startsWith(kind + '=')).map((t) => t.slice(kind.length + 1).split(','));
}
function hkCharges() {
  return hkTokens('charge').filter((f) => f.length === 8).map((f) => ({ motion: +f[0], min: +f[1], t2: +f[2], t3: +f[3], max: +f[4], acts: f.slice(5).map(Number) }));
}
function hkPicks() {                              // BASE -> [ID1, ...]
  const m = new Map();
  for (const f of hkTokens('magipick')) if (f.length >= 3) m.set(+f[0], f.slice(2).map(Number));
  return m;
}
function hkPickMagi(id) {                         // the projectile the preview fires for a fire_projectile of `id`
  const ids = hkPicks().get(id);
  return ids && ids.length ? ids[Math.min(HK_PREV.pick, ids.length - 1)] : id;
}
const hkChargeOf = (motion) => hkCharges().find((c) => c.motion === motion) || null;
const hkHold = (c, lv) => (lv <= 1 ? c.min : Math.max(c.min, lv === 2 ? c.t2 : c.t3));   // ticks held before the release
function hkActionMotion(action) {
  const a = S.actionById && S.actionById.get(action); if (!a) return -1;
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === a.script);
  return row ? row.motion : -1;
}
function hkPickBases(m) {                         // magipick= bases this move fires (its presses and a charge's releases)
  const P = hkPicks(); if (!P.size) return [];
  const mots = new Set(mvMotions(m));
  for (const c of hkCharges()) if (mots.has(c.motion)) for (const a of c.acts) mots.add(hkActionMotion(a));
  const out = new Set();
  for (const mo of mots) { const p = S.tlOfMotion.get(mo); if (!p) continue; for (const e of F(p).keyed) if (opCode(e.op) === 0x0204 && P.has(argsOf(e)[0])) out.add(argsOf(e)[0]); }
  return [...out];
}
function hkPrevHTML(m) {                       // the charge level / shot the preview plays: fields of the preview row
  const ch = hkCharges().find((c) => mvMotions(m).includes(c.motion)), bases = hkPickBases(m);
  if (!ch && !bases.length) return '';
  const seg = (name, list, val) => `<span class="mvseg" role="group">${list.map(([v, l, t]) => `<button type="button" data-${name}="${v}" aria-pressed="${val === v}" title="${esc(t || '')}">${l}</button>`).join('')}</span>`;
  let h = '';
  if (ch) h += `<div class="field"><label title="In a fight the charge is released when the button is let go (charge=): this picks the level the preview plays">Preview: charge level</label>${seg('hklv', [1, 2, 3].map((lv) => [lv, String(lv), `held ${hkHold(ch, lv)} ticks (${(hkHold(ch, lv) / 60).toFixed(2)} s): action ${ch.acts[lv - 1]}`]), HK_PREV.level)}</div>`;
  const ids = bases.length ? hkPicks().get(bases[0]) : [];
  if (ids.length > 1) {
    const lab = (id) => { const a = S.projAtkp && S.projAtkp.get(id), hh = a != null ? hitOf(a) : null; return hh ? hh.name.replace(/^[A-Z]+_[A-Z]+_/, '') : '#' + id; };
    h += `<div class="field" style="grid-column:1/-1"><label title="In a fight the shot is picked at random (magipick=): this picks the one the preview fires">Preview: shot</label>${seg('hkpk', ids.map((id, k) => [k, esc(lab(id)), `projectile ${id}${bases.length > 1 ? ' (and the same place in ' + bases.slice(1).map((b) => hkPicks().get(b)[Math.min(k, hkPicks().get(b).length - 1)]).join(', ') + ')' : ''}`]), HK_PREV.pick)}</div>`;
  }
  return h;
}
function hkWire() {
  const sec = $('mvSec'); if (!sec) return;
  const redo = () => { if (typeof dummyReset === 'function') dummyReset(); if (typeof FXP !== 'undefined') FXP.map.clear(); renderAll(); if (typeof dmRefresh === 'function') dmRefresh(); };
  sec.querySelectorAll('[data-hklv]').forEach((b) => b.onclick = () => { HK_PREV.level = +b.dataset.hklv; redo(); });
  sec.querySelectorAll('[data-hkpk]').forEach((b) => b.onclick = () => { HK_PREV.pick = +b.dataset.hkpk; redo(); });
}
function mvSegments(m, base) {                 // base: the move's own presses only (no actions a hook starts in the dummy)                       // [{i, motion, len (frames it plays), full}]
  const segs = m.steps.map((s, i) => {
    const full = s.motion >= 0 ? motionLen(s.motion) : 0;
    let len = full;
    if (!m.burst && S.mvCut !== 'end' && i < m.steps.length - 1 && S.tlOfMotion.has(s.motion)) {
      const w = F(S.tlOfMotion.get(s.motion)).ranged.filter((e) => MV_CHAIN.includes(opCode(e.op))).map((e) => frames(e.start));
      if (w.length) len = Math.max(1, Math.min(full, ...w));
    }
    return { i, motion: s.motion, len, full, action: s.action };
  });
  const dm = typeof DM !== 'undefined' && DM.on;
  if (!base && !dm) {                              // a charge (charge=): its press ends at the release, then the level's release
    const k = segs.findIndex((x) => hkChargeOf(x.motion));
    if (k >= 0) {
      const c = hkChargeOf(segs[k].motion), hold = hkHold(c, HK_PREV.level), act = c.acts[HK_PREV.level - 1], mo = hkActionMotion(act);
      if (mo >= 0) {
        segs.length = k + 1; segs[k].len = Math.min(segs[k].full, hold / 2);
        segs.push({ i: k + 1, motion: mo, len: motionLen(mo), full: motionLen(mo), why: `charge= release, level ${HK_PREV.level} (held ${hold} ticks)`, action: act });
      }
    }
  }
  if (!base && dm && DM.sim && DM.sim.move === S.moves.indexOf(m) && DM.sim.extra.length) {
    const nb = DM.sim.segs.filter((x) => !x.why).length;           // presses that played before the hook took over
    segs.length = Math.min(segs.length, nb);
    const last = segs[segs.length - 1], cut = DM.sim.segs[nb - 1];
    if (last && cut) last.len = Math.min(last.len, cut.len / 2);   // the press the hook cut short
    for (const x of DM.sim.extra) segs.push({ i: segs.length, motion: x.motion, len: x.len / 2, full: motionLen(x.motion), why: x.why, action: x.action });
  }
  return segs;
}
/* the whole move selected (its name in the left pane, or a picked press clicked again): play goes press after press */
function mvSelectWhole() {
  if (!S.step) return;
  const m = S.moves[S.step.move]; if (!m) return;
  S.moveOnly = true; S.sel = null;
  S.whole = mvSegments(m).length > 1 ? { move: S.step.move } : null;
  playAuto(); renderAll();
}
function mvToggleWhole() {
  if (!S.step) return;
  if (S.whole) { S.whole = null; S.playing = false; }
  else {
    S.whole = { move: S.step.move };
    if (S.step.i !== 0 || S.frame >= motionLen(S.motion) - 0.5) { S.step.i = 0; setMotion(S.moves[S.step.move].steps[0].motion); }
    S.playing = true;
  }
  S.autoplay = S.playing; $('playBtn').textContent = S.playing ? '❚❚' : '▶';
  renderAll();
}
function mvTick(dt) {                          // called by app.js tick while a whole move plays
  const m = S.moves[S.whole.move];
  if (!m || !S.step || S.step.move !== S.whole.move) { S.whole = null; return false; }
  const segs = mvSegments(m), seg = segs[S.step.i];
  let f = S.frame + dt * 30 * parseFloat($('speed').value);
  if (f >= seg.len) {
    let ni = S.step.i + 1;
    if (ni >= segs.length) {
      if (!$('loop').checked) { S.frame = seg.len; S.playing = false; $('playBtn').textContent = '▶'; pose(); updatePlayhead(); return true; }
      ni = 0; f = 0;
    } else f -= seg.len;
    S.step = { move: S.whole.move, i: ni }; S.sel = null; S.mfxSel = null;   // a selected event belongs to the press before
    S.fxEdit = null; S.motion = segs[ni].motion; S.frame = Math.min(f, segs[ni].len);
    if (typeof dummyReset === 'function' && ni === 0) dummyReset();
    pose(); renderLeft(); renderTimeline(); renderInspector();
    return true;
  }
  S.frame = f; pose(); updatePlayhead();
  return true;
}
function mvStrip() {                           // the presses of the selected move as the first row of the timeline
  // A sticky row inside the scrolling lanes (request: "The selectable header should be in the scrollable section rather
  // than above it"), on the timeline's own frame scale: the press on show starts over frame 0 and the presses after it
  // follow at the same frames-per-pixel, so its playhead and bars line up with the events below. Presses before it sit
  // in the row's sticky label cell; the move's length goes in the bar above.
  const inner = $('lanesInner'); if (!inner) return;
  let el = $('mvStrip');
  const tot = $('mvTotal');
  const m = S.step && !S.fxEdit && !(S.comp && typeof compObj === 'function' && compObj()) && S.moves && S.moves[S.step.move];
  if (!m || m.steps.length < 1) { if (el) el.remove(); if (tot) tot.textContent = ''; S.mvPos = null; return; }
  if (!el || el.parentNode !== inner) { if (el) el.remove(); el = document.createElement('div'); el.className = 'mvstrip'; el.id = 'mvStrip'; inner.insertBefore(el, inner.firstChild); }
  const segs = mvSegments(m), total = segs.reduce((a, s) => a + s.len, 0) || 1, px = PX();
  const cur = typeof tlWhole === 'function' && tlWhole() ? 0 : Math.min(S.step.i, segs.length - 1);   // the whole move: every press from frame 0
  const btn = (s, style, short) => `<button type="button" class="mvs${s.i === S.step.i ? (S.moveOnly ? ' cur' : ' on') : ''}" data-i="${s.i}" style="${style}" title="${s.why ? esc(s.why) + ': action ' + s.action : 'press ' + (s.i + 1)}: ${esc(motionName(s.motion))}, plays ${Math.round(s.len)} of ${s.full} frames. ${S.moveOnly || S.step.i !== s.i ? 'Click to play only this press' : 'Click again for the whole move'}">${s.why ? '<i>↳</i>' : `<i class="pk ${mvPressCls(s.action)}">${s.i + 1}</i>`}${short ? '' : ' ' + esc(motionName(s.motion))}</button>`;
  let x = 0, track = '';
  for (const s of segs.slice(cur)) { track += btn(s, `left:${LABEL_W + x * px}px;width:${Math.max(4, s.len * px - 2)}px`); x += s.len; }
  const before = segs.slice(0, cur), short = before.length * 90 > LABEL_W - 8;   // only numbers when the names would not fit
  const w = Math.max(parseFloat(inner.style.width) || 0, LABEL_W + (x + 10) * px);
  inner.style.width = w + 'px';
  el.style.width = w + 'px';
  el.innerHTML = `<div class="mvs-lab" style="width:${LABEL_W}px">${before.map((s) => btn(s, '', short)).join('')}${before.length ? '' : '<span class="mvs-cap">Presses</span>'}</div>`
    + track + `<span class="mvs-head" id="mvHead"></span>`;
  const whole = typeof tlWhole === 'function' && tlWhole();   // on the whole-move timeline the frame readout already has its length
  if (tot) tot.innerHTML = whole ? `<span title="the whole move: ${Math.round(total)} frames, ${(total / 30).toFixed(2)} s">· ${(total / 30).toFixed(2)} s</span>` : ''
    + (mvMoreWithDummy(m) ? ` <button type="button" class="mini mvs-more" id="mvsMore" title="Part of this move only plays when a hit or shot lands (${mvMoreWithDummy(m)}): turn on the dummy to see it, with its effects">+ on hit</button>` : '');
  const more = $('mvsMore'); if (more) more.onclick = () => { const cb = $('dmOn'); if (cb) { cb.checked = true; cb.dispatchEvent(new Event('change')); setTimeout(() => renderAll(), 600); } };
  el.querySelectorAll('.mvs').forEach((b) => b.onclick = () => {
    const i = +b.dataset.i;
    if (!S.moveOnly && S.step.i === i) { mvSelectWhole(); return; }   // the picked press again: back to the whole move
    S.whole = null; S.step = { move: S.step.move, i }; S.moveOnly = false; S.sel = null; setMotion(segs[i].motion); renderAll();
  });
  mvStripHead();
}
function mvMoreWithDummy(m) {                  // what only plays after a landed hit, when the dummy is off ('' = nothing)
  if ((typeof DM !== 'undefined' && DM.on) || typeof ckMoveHooks !== 'function') return '';
  const h = ckMoveHooks(m).find((x) => ['hitact', 'chainact', 'landact'].includes(x.kind));   // (charge= shows without the dummy)
  if (h) return h.kind + '=';
  // an on-hit press (plya +3 = 1) after a press whose projectile spawns children on a hit (link_hit, e.g. Alterna's
  // void): without the dummy nothing is hit, so those children never play
  if (!m || m.burst || !m.steps.some((s, i) => i > 0 && (S.actionById.get(s.action) ? mvRaw(S.actionById.get(s.action))[3] === 1 : false))) return '';
  const P = typeof mvProjMap === 'function' ? mvProjMap() : null; if (!P) return '';
  for (const s of m.steps) {
    const p = S.tlOfMotion.get(s.motion); if (!p) continue;
    for (const e of [...F(p).ranged, ...F(p).keyed]) if (e.projectile !== undefined && [...mvProjChain(e.projectile, P)].some((id) => { const r = P.get(id); return r && r.link_hit >= 0; })) return 'shot hit';
  }
  return '';
}
function mvStripHead() {                       // on the timeline's scale: the press on show starts at 0
  const hd = $('mvHead'); if (!hd) return;
  hd.style.left = (LABEL_W + ((typeof tlAt === 'function' ? tlAt() : 0) + S.frame) * PX()) + 'px';
  if (typeof phClip === 'function') phClip();
}

/* ====================================================== presses and □ HP finishers, built here (2026-10-29: "Can
   we make it possible to build this in studio only, so in a move I select the animation and how to chain them
   together?")
   A press is a comb step: an action (plya record) whose script plays an animation (motion_map). A press can follow the
   one before it only inside that press's chain window (0x0101 and kin on its timeline), so adding a press makes sure
   the press before it has one. An HP finisher on □ is a command of its own whose prereq names the brave move (retail
   pattern: Cloud's 0x13B after 0x13A): the brave move's first N presses again with flag 0x100, then the HP press. Its
   ability (mod.json move_names) is HP / follow-up / after the brave move's ability; it is equipped like any other.
   Which button a press answers is in its plya record: +0x0F kind bits (1 = brave ○, 2 = HP □), +0x18 bit 0x10 (HP),
   +0x0E 3..5 on HP presses. A brave finisher turned into an HP one without those never answers □ (Noctis's Katana of
   the Warrior, 29 Oct). */
const MV_IN_KIND = 0x0F, MV_IN_FLAGS = 0x18, MV_IN_HPCLASS = 0x0E;
const mvRawHex = (r) => [...r].map((x) => x.toString(16).padStart(2, '0')).join('');
const mvPressHP = (a) => !!(mvRaw(a)[MV_IN_KIND] & 2);
function mvSetInput(a, hp) {
  const r = mvRaw(a);
  r[MV_IN_KIND] = (r[MV_IN_KIND] & ~3) | (hp ? 2 : 1);
  r[MV_IN_FLAGS] = hp ? r[MV_IN_FLAGS] | 0x10 : r[MV_IN_FLAGS] & ~0x10;
  if (hp && !r[MV_IN_HPCLASS]) r[MV_IN_HPCLASS] = 3;
  if (!hp && r[MV_IN_HPCLASS] >= 3 && r[MV_IN_HPCLASS] <= 5) r[MV_IN_HPCLASS] = 0;
  a.raw = mvRawHex(r);
}
const mvOwnFrom = (m) => (m.chainOf != null ? m.chainAt : 0);          // a chain's first presses are its brave move's
function mvWrongButton(m) {                       // own presses whose record answers the other button: [{i, action}]
  const k = mvKind(m); if (!k.type || m.burst) return [];
  return m.steps.slice(mvOwnFrom(m)).filter((s) => { const a = S.actionById.get(s.action); return a && mvPressHP(a) !== (k.type === 'hp'); });
}
const mvCmdOf = (m) => F(msPath('moves.json')).commands.find((c) => parseInt(c.id, 16) === m.id);
function mvAnimOpts(sel) {
  const n = S.gmo ? S.gmo.motions.length : 0; let o = '';
  for (let i = 0; i < n; i++) o += `<option value="${i}"${i === sel ? ' selected' : ''}>${esc(motionName(i))}${S.tlOfMotion.has(i) ? '' : ' (no events)'}</option>`;
  return o;
}
const mvCanBranch = (m) => m.chainOf == null && m.variantOf == null && !m.burst && mvKind(m).type !== 'hp';   // (unset = the copied brave record's, without the ISO)

/* a chain window at the end of `motion`'s timeline (after its last hit), unless it has one; returns what it did */
function mvChainPlan(motion) {                    // {path, idx (new timeline: the index file)} or null when it has one
  const p = S.tlOfMotion.get(motion);
  if (p && F(p).ranged.some((e) => MV_CHAIN.includes(opCode(e.op)))) return null;
  return { motion, path: p || null };
}
function mvChainPaths(plans) {
  const out = [];
  for (const x of plans) if (x) out.push(x.path || msPath('timelines/index.json'));
  return [...new Set(out)];
}
function mvChainApply(x) {                        // inside edit(): add the window (a new timeline if the motion has none)
  if (!x) return;
  let p = x.path;
  if (!p) {
    let fn = 'timelines/' + String(x.motion).padStart(3, '0') + '.json';
    while (S.files[msPath(fn)]) fn = fn.replace('.json', '_n.json');
    p = msPath(fn);
    S.files[p] = { obj: { motion: x.motion, ranged: [], keyed: [] }, text: '', dirty: true };
    F(msPath('timelines/index.json')).files.push(fn);
    S.tlPaths.push(p);
    markDirty([p]);
  }
  const t = F(p), len = Math.round(motionLen(x.motion) * 2);
  const hitEnd = Math.max(0, ...t.ranged.filter((e) => e.hit !== undefined && e.end !== FOREVER).map((e) => e.end));
  const start = Math.min(len - 2, hitEnd || Math.round(len * 0.5));
  t.ranged.push({ start: Math.max(0, start), end: len, op: 'chain_window', args: [0] });
}

function mvBuilderHTML(m, can) {              // its own section (request: "This should be its own section, and be called Ability Builder")
  // an HP finisher shows its combo's builder: the whole tree, its own presses among them (request: "Why do I only see the
  // hp attack in the ability builder here?")
  if (m.chainOf != null) { const p = S.moves.find((x) => x.id === m.chainOf); if (p) { m = p; can = mvCanEdit() && !!mvEntry(p); } }
  if (m.burst || m.variantOf != null) return '';
  const own = m.steps.slice(mvOwnFrom(m));
  if (!own.length) return '';
  const par = m.chainOf != null ? S.moves.find((x) => x.id === m.chainOf) : null, mi = S.moves.indexOf(m);
  // the presses in the order they can play: the combo's, with each HP finisher's presses after the press they branch in
  // (request: "The HP finisher should be done in the press, not here. It should just be Add press")
  const rows = [], chains = par ? [] : (m.chains || []).filter((c) => !c.disabled);
  const branch = (c) => { for (const cs of c.steps.slice(c.chainAt)) rows.push({ mv: c, mi: S.moves.indexOf(c), s: cs, hp: mvKind(c).type === 'hp', branch: true }); };
  for (const s of own) {                         // a finisher in press N sits under the press it plays instead of
    rows.push({ mv: m, mi, s, hp: mvKind(m).type === 'hp' });
    for (const c of chains) if (c.chainAt === s.i && s.i > 0) branch(c);
  }
  for (const c of chains) if (c.chainAt >= m.steps.length) branch(c);
  const row = (r) => {
    const g = r.hp ? '□' : '○', canX = can && (r.branch || (m.steps.length > 1 && !(par && own.length < 2)));
    return `<div class="mvpress${r.branch ? ' branch' : ''}"><i class="st-n ${r.hp ? 'hp' : 'brave'}" title="${r.branch ? `${esc(r.mv.name)}: □ in press ${r.s.i}` : `${g} press`}">${r.s.i + 1}</i>${apButtonHTML(r.mi + ':' + r.s.i, r.s.motion, `Animation of press ${r.s.i + 1}${r.branch ? ' (' + r.mv.name + ')' : ''}`, !can)}${canX ? `<button type="button" class="mini mvx" data-prm="${r.mi}:${r.s.i}" title="Remove this chain attack" aria-label="Remove chain attack ${r.s.i + 1}${r.branch ? ' of ' + esc(r.mv.name) : ''}">✕</button>` : '<span></span>'}</div>`;
  };
  let h = `<div class="sec" id="mvBuild"><h3>Ability Builder</h3>`;
  if (par) h += `<p class="note">Chain attacks 1–${m.chainAt} are ${esc(par.name)}'s: change them there. Then □ plays:</p>`;
  h += `<div class="mvpresses">${rows.map(row).join('')}</div>`;
  if (can) h += `<button type="button" class="mvadd" data-ap="add" data-motion="${own[own.length - 1].motion}" title="Pick the animation of chain attack ${m.steps.length + 1}">+ Add chain attack</button>`;
  return h + '</div>';
}
function mvPressesHTML() { return ''; }
/* the button a press plays on, for its marker everywhere (request: "I like the O and [] glyphs. Can we use them everywhere
   for the presses?"): round = ○ brave, square = □ HP */
function mvPressCls(action) { const a = S.actionById && S.actionById.get(action); return a ? (mvPressHP(a) ? 'hp' : 'brave') : ''; }
/* the animation picker: a searchable list that plays each animation in the viewport as you go through it (request: "The
   animation is a difficult thing to know by id, so we need some kind of preview as I'm selecting it"). Hover or ↑ ↓
   previews, a click or Enter picks, Esc or a click outside goes back to what was showing. */
function apButtonHTML(key, motion, label, disabled) {
  const used = (S.usedBy.get(motion) || []).slice(0, 2).join(', ');
  return `<button type="button" class="apbtn" data-ap="${key}" data-motion="${motion}" aria-haspopup="listbox" aria-label="${esc(label || 'Animation')}: ${esc(motionName(motion))}"${disabled ? ' disabled' : ''}><span class="${motion >= 0 ? 'mono' : 'ap-none'}">${esc(motion >= 0 ? motionName(motion) : 'Pick an animation…')}</span><small>${motion >= 0 ? motionLen(motion) + ' f' + (used ? ' · ' + esc(used) : S.tlOfMotion.has(motion) ? '' : ' · no events') : ''}</small><span class="ap-ch" aria-hidden="true">▾</span></button>`;
}
let AP = null;
function apClose(pick) {
  if (!AP) return;
  const a = AP; AP = null;
  cancelAnimationFrame(a.raf); document.removeEventListener('mousedown', a.out, true); window.removeEventListener('resize', a.out); a.el.remove();
  Object.assign(S, a.save); S.frame = a.save.frame;
  if ($('playBtn')) $('playBtn').textContent = S.playing ? '❚❚' : '▶';
  pose(); renderTimeline();
  if (pick != null) a.onPick(pick); else if (a.anchor.isConnected) a.anchor.focus();
}
function apOpen(anchor, cur, onPick) {
  apClose();
  const n = S.gmo ? S.gmo.motions.length : 0; if (!n) return;
  const el = document.createElement('div'); el.className = 'appop'; el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Pick an animation');
  el.innerHTML = `<input type="search" class="ap-q" placeholder="Search by name, or by the move using it" aria-label="Search animations"><div class="ap-list" role="listbox"></div><p class="ap-hint">Hover or ↑ ↓ to preview · click or Enter to pick · Esc to cancel</p>`;
  document.body.appendChild(el);
  const r = anchor.getBoundingClientRect(), W = Math.min(innerWidth - 16, Math.max(r.width, 320));
  el.style.left = Math.max(8, Math.min(r.left, innerWidth - W - 8)) + 'px'; el.style.width = W + 'px';
  const below = innerHeight - r.bottom - 12, above = r.top - 12;
  if (below >= 260 || below >= above) { el.style.top = (r.bottom + 4) + 'px'; el.style.maxHeight = Math.min(420, below) + 'px'; }
  else { el.style.bottom = (innerHeight - r.top + 4) + 'px'; el.style.maxHeight = Math.min(420, above) + 'px'; }
  AP = { el, anchor, onPick, hi: cur, vis: [], save: { motion: S.motion, frame: S.frame, whole: S.whole, playing: S.playing, fxEdit: S.fxEdit } };
  const rows = [];
  for (let i = 0; i < n; i++) rows.push({ i, name: motionName(i), used: (S.usedBy.get(i) || []).join(', '), len: motionLen(i), ev: S.tlOfMotion.has(i) });
  const list = el.querySelector('.ap-list'), q = el.querySelector('.ap-q');
  const draw = () => {
    const t = q.value.trim().toLowerCase();
    const vis = rows.filter((x) => !t || x.name.toLowerCase().includes(t) || x.used.toLowerCase().includes(t));
    AP.vis = vis.map((x) => x.i);
    list.innerHTML = vis.map((x) => `<div class="ap-row${x.i === AP.hi ? ' hi' : ''}${x.i === cur ? ' cur' : ''}" role="option" id="ap-${x.i}" data-i="${x.i}" aria-selected="${x.i === AP.hi}"><span class="mono">${esc(x.name)}</span><small>${x.len} f</small><small class="ap-used">${esc(x.used || (x.ev ? 'not used yet' : 'not used yet · no events'))}</small></div>`).join('') || '<p class="note">No animation matches.</p>';
  };
  const preview = (i, scroll) => {
    AP.hi = i; S.fxEdit = null; S.whole = null; S.motion = i; S.frame = 0; S.playing = true;
    if ($('playBtn')) $('playBtn').textContent = '❚❚';
    pose(); renderTimeline();
    list.querySelectorAll('.ap-row').forEach((d) => { const on = +d.dataset.i === i; d.classList.toggle('hi', on); d.setAttribute('aria-selected', on); if (on && scroll) d.scrollIntoView({ block: 'nearest' }); });
    q.setAttribute('aria-activedescendant', 'ap-' + i);
  };
  draw();
  list.onmousemove = (e) => { const d = e.target.closest('.ap-row'); if (d && +d.dataset.i !== AP.hi) preview(+d.dataset.i); };
  list.onclick = (e) => { const d = e.target.closest('.ap-row'); if (d) apClose(+d.dataset.i); };
  q.oninput = () => { draw(); if (AP.vis.length && !AP.vis.includes(AP.hi)) preview(AP.vis[0], true); };
  q.onkeydown = (e) => {
    e.stopPropagation();
    const v = AP.vis, k = v.indexOf(AP.hi);
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); if (v.length) preview(v[k < 0 ? 0 : Math.max(0, Math.min(v.length - 1, k + (e.key === 'ArrowDown' ? 1 : -1)))], true); }
    else if (e.key === 'Enter') { e.preventDefault(); if (v.includes(AP.hi)) apClose(AP.hi); }
    else if (e.key === 'Escape') { e.preventDefault(); apClose(); }
  };
  AP.out = (e) => { if (AP && !(e && e.target && el.contains(e.target))) apClose(); };
  document.addEventListener('mousedown', AP.out, true); window.addEventListener('resize', AP.out);
  const loop = () => {                          // the preview loops whatever the Loop switch says
    if (!AP) return;
    if (S.motion === AP.hi && (!S.playing || S.frame >= motionLen(S.motion) - 0.5)) { S.frame = 0; S.playing = true; }
    AP.raf = requestAnimationFrame(loop);
  };
  AP.raf = requestAnimationFrame(loop);
  preview(cur >= 0 ? cur : 0, true);
  q.focus();
}
function mvPressWire(m) {
  const fw = $('mvFixButton'); if (fw && mvCanEdit()) fw.onclick = () => {
    const hp = mvKind(m).type === 'hp', list = mvWrongButton(m);
    edit([msPath('actions.json')], () => { for (const s of list) mvSetInput(S.actionById.get(s.action), hp); });
    derive(); renderAll(); toast(`${list.length} press${list.length > 1 ? 'es' : ''} now answer${list.length > 1 ? '' : 's'} ${hp ? '□' : '○'}.`);
  };
  if (m.chainOf != null) { const p = S.moves.find((x) => x.id === m.chainOf); if (p) m = p; }   // (the builder shows the combo)
  const sec = $('mvBuild'); if (!sec || !mvCanEdit() || !mvEntry(m)) return;
  sec.querySelectorAll('[data-ap]').forEach((b) => b.onclick = () => apOpen(b, +b.dataset.motion, (mo) => {
    if (b.dataset.ap === 'add') { mvAddPress(m, mo); return; }
    const [mi, i] = b.dataset.ap.split(':').map(Number);
    mvSetPressMotion(S.moves[mi], i, mo);
  }));
  sec.querySelectorAll('[data-prm]').forEach((b) => b.onclick = () => {
    const [mi, i] = b.dataset.prm.split(':').map(Number), x = S.moves[mi];
    if (x.chainOf != null && x !== m && x.steps.length - x.chainAt < 2) mvDropChain(x); else mvRemovePress(x, i);
  });
}
function mvSetPressMotion(m, i, motion) {
  const s = m.steps[i], p = msPath('motion_map.json');
  const shared = S.moves.filter((x) => x !== m && x.steps.some((y) => y.script === s.script) && !(x.chainOf === m.id && i < x.chainAt) && !(m.chainOf === x.id));
  edit([p], () => { for (const r of F(p).rows) if (r.script === s.script) r.motion = motion; });
  derive(); S.whole = null; S.step = { move: S.moves.findIndex((x) => x.id === m.id), i }; S.moveOnly = true; S.sel = null; setMotion(motion); renderAll();
  if (shared.length) toast(`Press ${i + 1} shares its script with ${shared.map((x) => x.name).join(', ')}: it changed there too.`);
  else if (!S.tlOfMotion.has(motion)) toast(motionName(motion) + ' has no events yet: add its hits on the timeline.');
}
function mvNewAction(fromAction, motion, hp) {    // inside edit(): a new action + script playing `motion`, raw copied
  const acts = F(msPath('actions.json')).actions, rows = F(msPath('motion_map.json')).rows;
  const act = Math.max(...acts.map((a) => a.action)) + 1, scr = Math.max(...acts.map((a) => a.script), ...rows.map((r) => r.script)) + 1;
  const src = S.actionById.get(fromAction), na = { action: act, script: scr, raw: src.raw };
  mvSetRaw(na, MV_FOLLOW_BYTE, 1); mvSetRaw(na, MV_START_BYTE, 0);   // a later press: never a first one
  mvSetInput(na, hp);
  acts.push(na);
  const row = rows.find((r) => r.script === src.script);
  rows.push({ script: scr, motion, phase: row ? row.phase : 0 });
  return act;
}
function mvAddPress(m, motion) {
  const last = m.steps[m.steps.length - 1], plan = mvChainPlan(last.motion);
  edit([msPath('moves.json'), msPath('actions.json'), msPath('motion_map.json'), ...mvChainPaths([plan])], () => {
    const act = mvNewAction(last.action, motion, mvKind(m).type === 'hp');
    mvCmdOf(m).steps.push({ action: act, b: 0, c: 65535, flags: 0 });
    mvChainApply(plan);
  });
  derive(); runChecks();
  const mi = S.moves.findIndex((x) => x.id === m.id);
  S.whole = null; S.step = { move: mi, i: S.moves[mi].steps.length - 1 }; S.moveOnly = true; S.sel = null; setMotion(motion); renderAll();
  toast(`Press ${m.steps.length + 1}: ${motionName(motion)}${plan ? `. Press ${m.steps.length} had no chain window: it now has one from its last hit to its end (adjust it on its timeline)` : ''}.`);
}
function mvRemovePress(m, i) {
  const chains = (m.chains || []).filter((c) => i < c.chainAt);
  if (chains.some((c) => c.chainAt - 1 < 1)) { toast(`${chains[0].name} branches off press ${i + 1}: remove or move that HP finisher first.`); return; }
  if (m.chainOf != null && m.steps.length - m.chainAt < 2) { toast('An HP finisher needs at least one press of its own.'); return; }
  const cmds = [m, ...chains];
  edit([msPath('moves.json'), msPath('actions.json')], () => {
    for (const x of cmds) mvCmdOf(x).steps.splice(i, 1);
    if (i === 0) {                                 // the new first press: a first press (+3 = 0) that starts where the move does
      const a = S.actionById.get(mvCmdOf(m).steps[0].action), want = MV_START[mvKind(m).phase];
      mvSetRaw(a, MV_FOLLOW_BYTE, 0); if (want) mvSetRaw(a, MV_START_BYTE, want);
      for (const c of chains) mvCmdOf(c).steps[0].flags = 0x100;
    }
  });
  derive(); runChecks();
  const mi = S.moves.findIndex((x) => x.id === m.id);
  S.whole = null; S.step = { move: mi, i: Math.min(i, S.moves[mi].steps.length - 1) }; S.moveOnly = true; setMotion(S.moves[mi].steps[S.step.i].motion); renderAll();
  toast(`Press ${i + 1} removed${chains.length ? ` (also from ${chains.map((c) => c.name).join(', ')}, which repeat${chains.length > 1 ? '' : 's'} it)` : ''}. Its action stays in actions.json, unused.`);
}
/* a press's button, in its own panel (request: "The HP finisher should be done in the press"). □ on a press of a brave
   combo makes it, and the presses after it, an HP finisher: a follow-up command (the combo's presses before it again
   with flag 0x100, then these; retail pattern Cloud 0x13B after 0x13A) with an ability of its own (HP, follow-up of the
   combo). ○ on a finisher's press that comes after the combo's last press puts its presses back at the end of the combo
   (the finisher's ability is switched off: saves that learned it stay valid). */
function mvPressBtnHTML() {
  if (!S.step || !mvCanEdit()) return '';
  const m = S.moves[S.step.move], s = m && m.steps[S.step.i]; if (!s || m.burst || m.variantOf != null || !mvEntry(m)) return '';
  const par = m.chainOf != null ? S.moves.find((x) => x.id === m.chainOf) : null;
  let hp, can, note;
  if (par) {                                       // a finisher's press
    if (s.i < m.chainAt) return '';                // (one of the combo's presses it repeats)
    hp = mvKind(m).type === 'hp';
    const end = m.chainAt === par.steps.length;
    can = hp && end; note = hp ? (end ? `□ after ${esc(par.name)}'s last press plays ${esc(m.name)}. ○ makes ${m.steps.length - m.chainAt > 1 ? 'its presses' : 'it'} the combo's press ${par.steps.length + 1}${m.steps.length - m.chainAt > 1 ? ' onward' : ''} again.` : `□ in ${esc(par.name)}'s press ${m.chainAt} plays it instead of press ${m.chainAt + 1}. It can't be ○: press ${m.chainAt + 1} already plays on ○ there.`) : '';
  } else {
    if (!mvCanBranch(m)) return '';
    hp = false; can = s.i > 0;
    note = s.i > 0 ? `□ makes this press${s.i < m.steps.length - 1 ? ' and the ones after it' : ''} an HP finisher: pressing □ during press ${s.i} plays it. It's an ability of its own: equip it in the game's Abilities screen.` : 'The first press starts the move.';
  }
  return `<div class="field" id="mvPressBtn"><label>Button</label><div><span class="mvseg" role="group" aria-label="Button of press ${s.i + 1}" title="${note.replace(/"/g, '&quot;')}"><button type="button" data-pbtn="brave" aria-pressed="${!hp}"${can || !hp ? '' : ' disabled'}>○ Brave</button><button type="button" data-pbtn="hp" aria-pressed="${hp}"${can || hp ? '' : ' disabled'}>□ HP</button></span></div></div>`;   // what the buttons do is their tooltip (the labels already say ○ / □)
}
function mvPressBtnWire() {
  const sec = $('mvPressBtn'); if (!sec) return;
  sec.querySelectorAll('[data-pbtn]').forEach((b) => b.onclick = () => {
    const m = S.moves[S.step.move], i = S.step.i, want = b.dataset.pbtn;
    if (b.getAttribute('aria-pressed') === 'true') return;
    if (want === 'hp') mvToHP(m, i); else mvToBrave(m);
  });
}
function mvToHP(m, i) {
  if (i < 1) return;
  const later = (m.chains || []).filter((c) => !c.disabled && c.chainAt > i);
  if (later.length) { toast(`${later.map((c) => c.name).join(', ')} branch${later.length > 1 ? '' : 'es'} later in the combo: set ${later.length > 1 ? 'their presses' : 'its press'} to ○ or remove ${later.length > 1 ? 'them' : 'it'} first.`); return; }
  const tmpl = S.moves.filter((x) => mvEntry(x) && x.variantOf == null && !x.burst && mvKind(x).type === 'hp')
    .sort((a, b) => (mvKind(b).phase === 'follow-up') - (mvKind(a).phase === 'follow-up'))[0];
  if (!tmpl) { toast('The character has no HP attack to copy an ability record from.'); return; }
  // a finisher of this move switched off earlier (its press set back to ○) comes back with its ability and name
  const back = Object.entries(S.mod.move_names).find(([kk, e]) => e && e.disabled && e.type === 'hp' && e.after === m.info.key && /^Made in the Move Studio/.test(e._note || '') && !F(msPath('moves.json')).commands.some((c) => c.id === e.cmd));
  const te = mvEntry(tmpl), key = back ? back[0] : mvFreeKey(), cmd = back ? parseInt(back[1].cmd, 16) : mvFreeCmd(), cmdHex = '0x' + cmd.toString(16).toUpperCase();
  const names = new Set(S.moves.map((x) => x.name)); let name = back ? back[1].name : m.name + ' (HP)', k = 2; while (!back && names.has(name)) name = `${m.name} (HP ${k++})`;
  const plan = mvChainPlan(m.steps[i - 1].motion), pb = mvBratPath(), pe = mvEntry(m);
  edit([S.modPath, msPath('moves.json'), msPath('actions.json'), ...(pb ? [pb] : []), ...mvChainPaths([plan])], () => {
    const pc = mvCmdOf(m), moving = pc.steps.splice(i);
    for (const s of moving) mvSetInput(S.actionById.get(s.action), true);
    const steps = [...pc.steps.map((s) => ({ ...s, flags: 0x100 })), ...moving.map((s) => ({ ...s, flags: 0 }))];
    F(msPath('moves.json')).commands.push({ id: cmdHex, prereq: [65535, m.id], steps, _info: `${name} (ability ${key}): HP finisher, square in ${m.name}'s press ${i}` });
    if (back) { delete back[1].disabled; back[1]._note = `Made in the Move Studio: square in ${m.name}'s press ${i}`; }
    else {
      const ent = { copy: te.copy !== undefined ? te.copy : tmpl.info.key, cmd: cmdHex, type: 'hp', phase: 'follow-up', after: m.info.key };
      ent.learn = pe && pe.learn ? pe.learn : 1;
      ent.status = 'new'; ent.name = name; ent.help = `[Close] After ${m.name}.\n{bonus}`;
      ent._note = `Made in the Move Studio: square in ${m.name}'s press ${i}`;
      S.mod.move_names[key] = ent;
    }
    if (pb) S.files[pb].bin = mvBratAdd(S.files[pb].bin, cmd, m.id);
    mvChainApply(plan);
  });
  derive(); mvSyncBanners(); runChecks();
  const ci = S.moves.findIndex((x) => x.id === cmd);
  S.finisher = S.finisher || {}; S.finisher[m.id] = ci;
  S.whole = null; S.step = { move: ci, i }; S.moveOnly = false; S.sel = null; setMotion(S.moves[ci].steps[i].motion); renderAll();
  toast(`Press ${i + 1} is now □: the HP finisher "${name}" (ability ${key}). Rename it in its move panel, give its last hit HP damage, then equip it in the game's Abilities screen.`);
}
function mvToBrave(c) {
  const p = S.moves.find((x) => x.id === c.chainOf);
  if (!p || c.chainAt !== p.steps.length) return;
  const n = c.steps.length - c.chainAt;
  edit([S.modPath, msPath('moves.json'), msPath('actions.json')], () => {
    const own = mvCmdOf(c).steps.slice(c.chainAt);
    for (const s of own) mvSetInput(S.actionById.get(s.action), false);
    mvCmdOf(p).steps.push(...own.map((s) => ({ ...s, flags: 0 })));
    mvDropChainIn(c);
  });
  derive(); runChecks();
  const pi = S.moves.findIndex((x) => x.id === p.id);
  S.whole = null; S.step = { move: pi, i: p.steps.length }; S.moveOnly = false; S.sel = null; setMotion(S.moves[pi].steps[p.steps.length].motion); renderAll();
  toast(`${n > 1 ? 'Its presses are' : 'It\'s'} ${p.name}'s press ${p.steps.length + 1}${n > 1 ? ' onward' : ''} again (○). ${c.name} is switched off.`);
}
function mvDropChainIn(c) {                       // inside edit(): the command goes, the ability stays as a switched-off record
  const cm = F(msPath('moves.json')).commands, k = cm.findIndex((x) => parseInt(x.id, 16) === c.id);
  if (k >= 0) cm.splice(k, 1);
  const e = S.mod.move_names[c.info.key]; if (e) e.disabled = true;
}
function mvDropChain(c) {
  edit([S.modPath, msPath('moves.json')], () => mvDropChainIn(c));
  derive(); runChecks(); renderAll();
  toast(`${c.name} removed (its ability is switched off, so saves that learned it stay valid).`);
}

/* the equip bonus as a combobox of bonus names (request: "Can we use the bonus Name instead of the id?" / "And make it a
   combobox"): type to filter, ↑ ↓ / click to pick; As copied (empty), None (null) or a set id. Names come from the
   game ISO's help texts (retail.js retailBonusNames); a set without a known name shows as "set N". */
const mvBonusName = (v) => (v == null || v < 0 ? 'None' : (S.bonusNames && S.bonusNames.get(v)) || 'set ' + v);
function mvBonusHTML(e, copied, ro) {
  const cur = e.bonus === undefined ? '' : mvBonusName(e.bonus);
  const ph = copied === undefined ? 'As copied' : 'As copied: ' + mvBonusName(copied);
  return `<div class="field"><label>Equip bonus${typeof edTip === 'function' ? edTip('A support ability the character gets for the whole fight while this attack is equipped (Bartz\'s attacks all have one). Its help text\'s {bonus} line names it. Empty = the copied record\'s.') : ''}</label>
    <div class="bcombo"><input type="text" id="mvBonus" role="combobox" aria-expanded="false" aria-autocomplete="list" aria-controls="mvBonusList" autocomplete="off" value="${esc(cur)}" placeholder="${esc(ph)}"${ro}><ul class="bc-list" id="mvBonusList" role="listbox" hidden></ul></div>
    ${S.bonusNames && S.bonusNames.size ? '' : '<small class="fhint">Set the game ISO to pick bonuses by name.</small>'}</div>`;
}
function mvBonusWire(set) {
  const inp = $('mvBonus'), ul = $('mvBonusList'); if (!inp || inp.disabled) return;
  const all = () => [{ v: undefined, l: 'As copied', s: inp.placeholder.replace(/^As copied:?\s*/, '') }, { v: null, l: 'None', s: 'no bonus' },
    ...[...(S.bonusNames || new Map())].map(([v, l]) => ({ v, l, s: 'set ' + v })).sort((a, b) => a.l.localeCompare(b.l))];
  let vis = [], hi = -1, open = false;
  const show = (filter) => {
    const t = filter ? inp.value.trim().toLowerCase() : '';
    vis = all().filter((o) => !t || (o.l + ' ' + o.s).toLowerCase().includes(t));
    hi = Math.min(Math.max(hi, 0), vis.length - 1);
    ul.innerHTML = vis.map((o, k) => `<li role="option" id="bc-${k}" data-k="${k}" aria-selected="${k === hi}" class="${k === hi ? 'hi' : ''}"><span>${esc(o.l)}</span><small>${esc(o.s)}</small></li>`).join('') || '<li class="none">No bonus matches: type a set number</li>';
    ul.hidden = false; open = true; inp.setAttribute('aria-expanded', 'true');
    if (hi >= 0) { inp.setAttribute('aria-activedescendant', 'bc-' + hi); const li = ul.querySelector(`[data-k="${hi}"]`); if (li) li.scrollIntoView({ block: 'nearest' }); }
  };
  const close = () => { ul.hidden = true; open = false; inp.setAttribute('aria-expanded', 'false'); inp.removeAttribute('aria-activedescendant'); };
  const pick = (o) => { close(); set((x) => { if (o.v === undefined) delete x.bonus; else x.bonus = o.v; }); };
  const commitTyped = () => {                     // a typed name, set number or none; anything else is put back
    const t = inp.value.trim(), tl = t.toLowerCase();
    if (!t) return pick({ v: undefined });
    if (tl === 'none' || tl === '-1') return pick({ v: null });
    const num = tl.replace(/^set\s*/, '');
    if (/^(0x)?[0-9a-f]+$/.test(num) && !isNaN(parseInt(num, num.startsWith('0x') ? 16 : 10))) return pick({ v: parseInt(num, num.startsWith('0x') ? 16 : 10) });
    const o = all().find((x) => x.l.toLowerCase() === tl); if (o) return pick(o);
    close(); renderInspector(); toast('Equip bonus: pick one from the list, or type a set number or none.');
  };
  inp.onfocus = () => { hi = -1; show(false); inp.select(); };
  inp.oninput = () => { hi = 0; show(true); };
  inp.onkeydown = (ev) => {
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') { ev.preventDefault(); if (!open) show(false); hi = Math.max(0, Math.min(vis.length - 1, hi + (ev.key === 'ArrowDown' ? 1 : -1))); show(!!inp.value && inp.dataset.typed === '1'); }
    else if (ev.key === 'Enter') { ev.preventDefault(); if (open && vis[hi]) pick(vis[hi]); else commitTyped(); }
    else if (ev.key === 'Escape') { ev.preventDefault(); close(); renderInspector(); }
    else inp.dataset.typed = '1';
    ev.stopPropagation();
  };
  ul.onmousedown = (ev) => { const li = ev.target.closest('li[data-k]'); if (li) { ev.preventDefault(); pick(vis[+li.dataset.k]); } };
  inp.onblur = () => { if (open) { close(); if (inp.dataset.typed === '1') commitTyped(); else renderInspector(); } };
}

/* ====================================================== deleting a move (request: "Let's add a way to delete attacks so I
   can delete star of the rogue. It should come with a very big warning popover")
   Gone: its command and every command that follows it up or is a version of it (moves.json), their CPU rows (brat),
   and the actions + motion_map rows nothing else plays (another command's press, a chain_followup event, a runtime
   hook). Kept: the ability record, switched off (mod.json "disabled" + "deleted"): the game reads the record of every
   learned and equipped ability without checking it exists, so removing it would crash saves that learned it
   (roster_mainbin.py disable_abilities). Kept too: animations, their timelines, hits, projectiles, effects (other moves
   can use them; an unused one costs nothing). */
function mvDeleteHTML() {                        // the inspector's last section on a move's root (request: "Delete this move should be at the bottom of the inspector")
  const m = S.step && S.moves[S.step.move];
  if (!m || m.burst || !mvCanEdit() || !mvEntry(m)) return '';
  return `<div class="sec mv-del-sec"><button type="button" id="mvDelete" class="danger">Delete this move…</button></div>`;
}
function mvDeletePlan(m) {
  const cmds = F(msPath('moves.json')).commands, ids = new Set([m.id]);
  for (let grew = true; grew;) {                  // follow-ups and versions of it, and theirs
    grew = false;
    for (const c of cmds) { const id = parseInt(c.id, 16); if (ids.has(id)) continue; const p = c.prereq || []; if (ids.has(p[0]) || ids.has(p[1])) { ids.add(id); grew = true; } }
  }
  const gone = cmds.filter((c) => ids.has(parseInt(c.id, 16))), kept = cmds.filter((c) => !ids.has(parseInt(c.id, 16)));
  const used = new Set(kept.flatMap((c) => c.steps.map((s) => s.action)));
  for (const p of S.tlPaths) for (const kind of ['ranged', 'keyed']) for (const e of F(p)[kind]) if (e.action !== undefined) used.add(+e.action);
  const hookNums = new Set(((S.mod && S.mod.runtime_hooks) || []).flatMap((t) => String(t).split(/[=,/]/).map(Number).filter((n) => !isNaN(n))));
  const acts = [...new Set(gone.flatMap((c) => c.steps.map((s) => s.action)))];
  const drop = acts.filter((a) => !used.has(a) && !hookNums.has(a));
  const dropScripts = new Set(drop.map((a) => S.actionById.get(a)).filter(Boolean).map((a) => a.script));
  for (const a of F(msPath('actions.json')).actions) if (!drop.includes(a.action)) dropScripts.delete(a.script);   // a script another action plays stays
  const names = Object.entries(mvNames()).filter(([k, v]) => v && v.cmd && ids.has(parseInt(v.cmd, 16)) && !k.startsWith('_'));
  const motions = [...new Set(gone.flatMap((c) => c.steps.map((s) => { const a = S.actionById.get(s.action), r = a && F(msPath('motion_map.json')).rows.find((x) => x.script === a.script); return r ? r.motion : -1; })))].filter((x) => x >= 0);
  return { ids, gone, drop, dropScripts, names, motions, keptActs: acts.filter((a) => !drop.includes(a)) };
}
function mvDeleteDialog(m) {
  const P = mvDeletePlan(m), others = P.gone.filter((c) => parseInt(c.id, 16) !== m.id);
  const otherNames = others.map((c) => { const mm = S.moves.find((x) => x.id === parseInt(c.id, 16)); return mm ? mm.name : 'command ' + c.id; });
  return new Promise((resolve) => {
    const back = document.createElement('div'); back.className = 'modal-back danger-back';
    back.innerHTML = `<form class="modal ui-dlg del-dlg" role="alertdialog" aria-modal="true" aria-labelledby="delT" aria-describedby="delW" novalidate>
      <div class="del-head"><span class="del-ic" aria-hidden="true">!</span><div><h3 id="delT">Delete ${esc(m.name)}?</h3><p id="delW" class="del-sub">This takes the move out of ${esc((S.mod.character || {}).name || 'the character')}'s kit for good. Undo works until you save; after that it can't be brought back from here.</p></div></div>
      <div class="del-cols"><div><div class="lbl">Removed</div><ul>
        <li>The move: command ${esc(m.hexId)}, ${m.steps.length} press${m.steps.length > 1 ? 'es' : ''}${m.info ? `, ability ${esc(m.info.key)}` : ''}</li>
        ${others.length ? `<li class="del-more">Also ${others.length} move${others.length > 1 ? 's' : ''} that follow${others.length > 1 ? '' : 's'} it up or ${others.length > 1 ? 'are versions' : 'is a version'} of it: <b>${esc(otherNames.join(', '))}</b></li>` : ''}
        <li>Its CPU row: the computer won't use it</li>
        <li>${P.drop.length} press record${P.drop.length === 1 ? '' : 's'} only it plays${P.dropScripts.size ? ` and ${P.dropScripts.size} animation link${P.dropScripts.size > 1 ? 's' : ''}` : ''}</li></ul></div>
      <div><div class="lbl">Kept</div><ul>
        <li>The ability record, switched off: saves that learned or equipped it stay valid, and its slot shows empty</li>
        <li>Its animations (${esc(P.motions.map(motionName).join(', ') || 'none')}), their events, hits, projectiles and effects</li>
        ${P.keptActs.length ? `<li>${P.keptActs.length} press record${P.keptActs.length > 1 ? 's' : ''} other moves or runtime hooks still play</li>` : ''}</ul></div></div>
      <div class="field"><label for="delName">Type <b>${esc(m.name)}</b> to confirm</label><input id="delName" type="text" autocomplete="off" spellcheck="false" placeholder="${esc(m.name)}"></div>
      <div class="btnrow ui-dlg-btns"><button type="button" class="ui-cancel">Keep it</button><button type="submit" class="danger del-go" disabled>Delete ${esc(m.name)}</button></div></form>`;
    document.body.appendChild(back);
    const form = back.firstElementChild, inp = form.querySelector('#delName'), go = form.querySelector('.del-go'), prev = document.activeElement;
    const done = (v) => { back.remove(); document.removeEventListener('keydown', onKey, true); if (prev && prev.focus && prev.isConnected) prev.focus(); resolve(v); };
    const onKey = (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); done(null); } };
    document.addEventListener('keydown', onKey, true);
    form.querySelector('.ui-cancel').onclick = () => done(null);
    const ok = () => inp.value.trim().toLowerCase() === m.name.trim().toLowerCase();
    inp.oninput = () => { go.disabled = !ok(); };
    form.onsubmit = (e) => { e.preventDefault(); if (ok()) done(P); };
    form.querySelector('.ui-cancel').focus();
  });
}
async function mvDeleteMove(m) {
  if (!mvCanEdit()) { toast(mvWhyNot() || 'This character\'s moves can\'t be deleted here.'); return; }
  const P = await mvDeleteDialog(m); if (!P) return;
  const pb = mvBratPath(), name = m.name;
  edit([S.modPath, msPath('moves.json'), msPath('actions.json'), msPath('motion_map.json'), ...(pb ? [pb] : [])], () => {
    const mj = F(msPath('moves.json')); mj.commands = mj.commands.filter((c) => !P.ids.has(parseInt(c.id, 16)));
    const aj = F(msPath('actions.json')); aj.actions = aj.actions.filter((a) => !P.drop.includes(a.action));
    const mm = F(msPath('motion_map.json')); mm.rows = mm.rows.filter((r) => !P.dropScripts.has(r.script));
    for (const [, v] of P.names) { v.disabled = true; v.deleted = true; v._note = (v._note ? v._note + '; ' : '') + 'deleted in the Move Studio (record kept, switched off, so saves stay valid)'; }
    if (pb) S.files[pb].bin = mvBratDrop(S.files[pb].bin, P.ids);
  });
  S.step = null; S.sel = null; S.whole = null; S.moveOnly = false; S.newMove = null;
  derive(); runChecks(); renderAll();
  toast(`${name} deleted${P.gone.length > 1 ? ` (with ${P.gone.length - 1} follow-up${P.gone.length > 2 ? 's' : ''})` : ''}. Undo brings it back until you save.`);
}
function mvBratDrop(bin, ids) {                   // brat without the rows of these commands (their weight codes stay: unused)
  const b = mvBratRows(bin), rows = b.rows.filter((r) => !ids.has(r.cmd));
  if (rows.length === b.rows.length) return bin;
  const size = 20 + 28 * rows.length + 4 + b.w.length, out = new Uint8Array((size + 3) & ~3), dv = new DataView(out.buffer);
  out.set(b.head, 0); dv.setUint32(16, rows.length, true);
  rows.forEach((r, i) => { const o = 20 + 28 * i; dv.setUint32(o, r.cmd, true); r.sit.forEach(([off, n], k) => { dv.setUint16(o + 4 + 4 * k, off, true); dv.setUint16(o + 6 + 4 * k, n, true); }); });
  const mo = 20 + 28 * rows.length; dv.setUint32(mo, b.w.length, true); out.set(b.w, mo + 4);
  return out;
}

/* ====================================================== HP attacks name themselves (request: "This should be automated for
   HP attacks"): the press of an HP move that lands its HP hit (else its first own press) shows the move's name as its
   banner (plya +0x21 -> a record of the moveset's text). A record with that text is reused; the press's own record is
   renamed when nothing else shows it; otherwise a new record is added. mod.json move_names[k].banner = "manual" turns it
   off for that move (the press panel's "Set by hand"). Runs when a move is opened, renamed, retyped or made HP. */
const mvBannerText = (m) => { const e = mvEntry(m); return ((e && (e.name || e.was)) || m.name).replace(/\s*\{(ground|midair)\}/g, '').replace(/\s*\((ground|midair)\)/g, '').trim(); };
const mvBannerAuto = (m) => { const e = mvEntry(m); return !!e && !e.disabled && !m.burst && m.variantOf == null && mvKind(m).type === 'hp' && e.banner !== 'manual'; };
function mvBannerPress(m) {
  const from = mvOwnFrom(m), hp = mvHits(m).filter((x) => mvIsHP(x.h) && x.press >= from);
  return (hp.length && m.steps[hp[0].press]) || m.steps[from] || null;
}
function mvSyncBanners() {                        // returns how many presses changed (one undo step)
  if (!mvCanEdit() || typeof OBJTEXT === 'undefined' || typeof edTextPath !== 'function') return 0;
  const tp = edTextPath(), pa = msPath('actions.json'); if (!tp) return 0;
  const want = [];
  for (const m of S.moves) {
    if (!mvBannerAuto(m)) continue;
    const s = mvBannerPress(m), a = s && S.actionById.get(s.action), t = mvBannerText(m);
    if (!a || !t || t.length > 24 || (OBJTEXT.unsupportedChars(t) || []).length) continue;
    want.push({ a, t });
  }
  const o0 = OBJTEXT.parseText(S.files[tp].bin);
  const todo = want.filter(({ a, t }) => { const r = o0.langs[0].find((x) => x.index === mvRaw(a)[0x21]); return !r || r.text !== t; });
  if (!todo.length) return 0;
  edit([tp, pa], () => {
    const o = OBJTEXT.parseText(S.files[tp].bin), recs = o.langs[0], acts = F(pa).actions;
    for (const { a, t } of todo) {
      const cur = mvRaw(a)[0x21], same = recs.find((x) => x.text === t);
      if (same) { mvSetRaw(a, 0x21, same.index); continue; }
      const mine = cur !== 0xFF && recs.some((x) => x.index === cur) && !acts.some((b) => b !== a && mvRaw(b)[0x21] === cur);
      if (mine) { OBJTEXT.setRecord(o, 0, cur, t); continue; }
      const idx = Math.max(-1, ...recs.map((x) => x.index)) + 1; if (idx > 254) continue;
      OBJTEXT.setRecord(o, 0, idx, t); mvSetRaw(a, 0x21, idx);
    }
    S.files[tp].bin = OBJTEXT.textToBytes(o);
  });
  return todo.length;
}
