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
  const out = [], P = mvProjMap(), seen = new Set();
  m.steps.forEach((s, i) => {
    if (s.motion < 0 || seen.has(s.motion)) return;
    seen.add(s.motion);
    const p = S.tlOfMotion.get(s.motion); if (!p) return;
    const t = F(p);
    for (const kind of ['ranged', 'keyed']) t[kind].forEach((e, idx) => {
      const tick = kind === 'ranged' ? e.start : e.frame;
      if (e.hit !== undefined) { const h = hitOf(e.hit); if (h) out.push({ h, press: i, tick, path: p, kind, idx }); }
      if (opCode(e.op) === 0x0204) {
        for (const pid of mvProjChain(argsOf(e)[0], P)) { const h = hitOf(P.get(pid).atkp); if (h) out.push({ h, press: i, tick, pid }); }
      }
    });
  });
  out.sort((a, b) => a.press - b.press || a.tick - b.tick);
  const once = new Map();
  for (const x of out) { const k = x.h.id + '|' + x.press + '|' + (x.pid != null ? 'p' + x.pid : 'e'); if (!once.has(k)) once.set(k, x); }
  return [...once.values()];
}
const mvIsHP = (h) => !!(parseInt(h.flags, 16) & 2);
const mvDamaging = (h) => mvIsHP(h) || h.power > 0;

/* ====================================================== the move panel */
function mvMoveHTML() {
  if (S.moves[S.step.move].burst && typeof burstHTML === 'function') return burstHTML(S.moves[S.step.move]);
  const mi = S.step.move, m = S.moves[mi], e = mvEntry(m), can = mvCanEdit() && !!e, k = mvKind(m);
  const ro = can ? '' : ' disabled';
  const hits = mvHits(m).filter((x) => mvDamaging(x.h)), nHP = hits.filter((x) => mvIsHP(x.h)).length, nBr = hits.length - nHP;
  const seg = (name, list, val) => `<span class="mvseg" role="group">${list.map(([v, l]) => `<button type="button" data-${name}="${v}" aria-pressed="${val === v}"${ro}>${l}</button>`).join('')}</span>`;
  const ids = `<details class="edp"><summary class="lbl">Details</summary><p class="note mono">command ${esc(m.hexId)}${m.info ? ' · ability ' + esc(m.info.key) : ''}<br>${m.steps.map((x, i) => `press ${i + 1}: action ${x.action} · script ${x.script}`).join('<br>')}</p></details>`;
  let h = `<div class="sec" id="mvSec"><h3>${esc(m.name)}</h3>
    <div class="btnrow mv-play"><label class="meta">Next press takes over <select id="mvCut" title="When the next press takes over: at its chain window, as in a fight, or after each animation ends"><option value="chain"${S.mvCut !== 'end' ? ' selected' : ''}>At chain windows</option><option value="end"${S.mvCut === 'end' ? ' selected' : ''}>Full animations</option></select></label></div>`;
  if (!e) {
    h += `<p class="note">No ability in mod.json points at command ${esc(m.hexId)}, so it isn't in the character's kit (it may be played by another move's event).</p>${ids}</div>`;
    return h;
  }
  if (m.versions && m.versions.length) h += `<p class="note">The game also plays it shorter: ${m.versions.map((v) => `the first ${v.steps.length === 1 ? 'press' : v.steps.length + ' presses'} (command ${esc(v.hexId)})`).join(', ')}: the engine picks which one plays (for Shantotto's spells, likely by her Brave). They're these same presses, so editing them here changes every version.</p>`;
  if (m.branchAt != null) { const p = S.moves.find((x) => x.id === m.variantOf); h += `<p class="note">${m.branchAt > 0 ? `A branch of ${esc(p ? p.name : 'command ' + m.variantOf)}: the same first ${m.branchAt === 1 ? 'press' : m.branchAt + ' presses'}, then from press ${m.branchAt + 1} it plays its own (ending in ${esc(m.branchTag || 'other actions')})` : `A version of ${esc(p ? p.name : 'command ' + m.variantOf)} with its own presses from the first (${esc(m.branchTag || 'other actions')}), played instead of it`}${m.branchEx ? ': its actions are named _EX, so this looks like the EX Mode version' : ''}. The game picks which plays. Same ability, so the name and help are shared.</p>`; }
  else if (m.variantOf != null) { const p = S.moves.find((x) => x.id === m.variantOf); h += `<p class="note">A shorter version of ${esc(p ? p.name : 'command ' + m.variantOf)}: its first ${m.steps.length} press${m.steps.length > 1 ? 'es' : ''}, which the game plays instead of the full move. It's the same ability, so the name and help below are shared with it.</p>`; }
  if (!can) h += `<p class="note">${esc(mvWhyNot())}</p>`;
  h += `<div class="field"><label>Type</label>${seg('type', MV_KINDS, k.type)}</div>
    <div class="field"><label>Starts</label>${seg('phase', MV_PHASES, k.phase)}</div>`;
  if (k.phase === 'follow-up') {
    const opts = Object.entries(mvNames()).filter(([kk, v]) => !kk.startsWith('_') && v && kk !== m.info.key)
      .map(([kk, v]) => `<option value="${kk}"${String(e.after || '').toUpperCase().replace(/^0X/, '') === kk ? ' selected' : ''}>${esc(v.name || v.was || kk)} (${kk})</option>`).join('');
    h += `<div class="field"><label>Follows up</label><select id="mvAfter"${ro}><option value="">(as copied)</option>${opts}</select></div>`;
  }
  if (!k.type || !k.phase) h += `<p class="note">${S.abilities && Object.keys(S.abilities).length ? '' : 'Set the game ISO to see what the copied record has. '}Unset = as the copied ${esc((S.mod.character || {}).based_on || 'retail')} record has it.</p>`;
  h += `<div class="field"><label>Name</label><input type="text" id="mvName" value="${esc(e.name || '')}" placeholder="${esc(e.was || m.name)}"${ro}></div>
    <div class="field"><label>Help text</label><textarea id="mvHelp" rows="3" placeholder="(the copied record's)"${ro}>${esc(e.help || '')}</textarea></div>`;
  {                                              // the record's cost and learning (roster_mainbin; docs/archive/ablt-fields)
    const d = mvDonor(), r = d && d[e.copy !== undefined ? String(e.copy).toUpperCase().padStart(2, '0') : m.info.key];
    const ph = (k, f) => r && r[k] != null ? `placeholder="${esc(f ? f(r[k]) : r[k])}"` : 'placeholder="as copied"';
    const cpm = e.cp != null && e.cp_mastered == null ? `placeholder="${Math.floor(e.cp / 2)}"` : ph('cp_mastered');
    h += `<div class="lbl">Cost and learning <small>empty = as the copied record</small></div>
      <div class="row3">${numField('CP to equip', e.cp ?? '', `id="mvCp" min="0" max="999" ${ph('cp')}${ro}`)}${numField('CP once mastered', e.cp_mastered ?? '', `id="mvCpm" min="0" max="999" ${cpm}${ro}`)}${numField('AP to master', e.ap ?? '', `id="mvAp" min="0" max="9999" ${ph('ap')}${ro}`)}</div>
      <div class="row2">${numField('Learnt at level', e.learn ?? '', `id="mvLearn" min="1" max="100"${ro}`)}<div class="field"><label>Equip bonus</label><input type="text" id="mvBonus" value="${esc(e.bonus === null ? 'none' : e.bonus ?? '')}" ${ph('bonus', (v) => v < 0 ? 'none' : 'set ' + v)} title="A system.bin 'fncs' effect-set id (the bonus line of the help text describes it; Bartz's attacks have one), or none"${ro}></div></div>
      <p class="note">${e.copy !== undefined ? `The record is a copy of ability ${esc(e.copy)}. ` : ''}A help text's {bonus} line is the copied record's bonus: change it with the bonus. CP once mastered defaults to half.</p>`;
  }
  h += `<label class="chk"><input type="checkbox" id="mvOn"${e.disabled ? '' : ' checked'}${ro}> In the character's kit</label>`;
  if (e.disabled) h += `<p class="note">Switched off: the game keeps the record (saves stay valid) but doesn't list it, and it does nothing in battle.</p>`;
  // hits against the type
  h += `<div class="lbl">Hits</div><p class="note">${hits.length ? `${nBr} brave, ${nHP} HP` : 'No damaging hit'}: ${esc(hits.map((x) => `${x.press + 1}·${x.h.name || x.h.id}${mvIsHP(x.h) ? ' (HP)' : ''}`).join(', '))}</p>`;
  if (k.type === 'hp' && !nHP && hits.length) h += `<p class="warnline">An HP attack with no HP hit deals no HP damage.</p>${can ? '<button type="button" id="mvFixHP">Make the last hit an HP hit</button>' : ''}`;
  if (k.type === 'brave' && nHP) h += `<p class="warnline">A brave attack with ${nHP} HP hit${nHP > 1 ? 's' : ''}: they still deal HP damage.</p>${can ? '<button type="button" id="mvFixBrave">Make its hits brave</button>' : ''}`;
  if (can) {
    const first = m.steps[0] && S.actionById.get(m.steps[0].action), b = first ? mvRaw(first)[MV_START_BYTE] : null;
    const want = MV_START[k.phase];
    if (want && b !== want && b != null) h += `<p class="warnline">Its first press (action ${first.action}) is set to start ${b === 2 ? 'on the ground' : b === 1 ? 'in the air' : 'anywhere'}.</p><button type="button" id="mvFixStart">Start it ${k.phase === 'ground' ? 'on the ground' : 'in the air'} too</button>`;
  }
  if (typeof cpNotesHTML === 'function') h += cpNotesHTML(m);
  if (typeof ckRecompHTML === 'function') h += ckRecompHTML(m);
  return h + ids + '</div>';
}
function mvWire() {
  const sec = $('mvSec'); if (!sec) return;
  if ($('mvWhole')) $('mvWhole').onclick = () => mvToggleWhole();
  if ($('mvCut')) $('mvCut').onchange = () => { S.mvCut = $('mvCut').value; mvStrip(); };
  const m = S.moves[S.step.move], e = mvEntry(m);
  if (!e || !mvCanEdit()) return;
  const set = (fn) => { edit([S.modPath], () => fn(S.mod.move_names[m.info.key])); derive(); renderAll(); };
  sec.querySelectorAll('[data-type]').forEach((b) => b.onclick = () => set((x) => { x.type = b.dataset.type; }));
  sec.querySelectorAll('[data-phase]').forEach((b) => b.onclick = () => set((x) => { x.phase = b.dataset.phase; if (x.phase !== 'follow-up' && 'after' in x) x.after = null; }));
  const af = $('mvAfter'); if (af) af.onchange = () => set((x) => { if (af.value) x.after = af.value; else delete x.after; });
  $('mvName').onchange = () => set((x) => { const v = $('mvName').value.trim(); if (v) x.name = v; else delete x.name; });
  $('mvHelp').onchange = () => set((x) => { const v = $('mvHelp').value.replace(/\r/g, ''); if (v.trim()) x.help = v; else delete x.help; });
  tokenHelper($('mvName'), [['{ground}', 'the ground suffix the game adds to a ground move\'s name'], ['{midair}', 'the midair suffix']]);
  tokenHelper($('mvHelp'), [['{bonus}', 'the copied record\'s equip-bonus line'], ['{effects}', 'the copied record\'s effect lines (knock-back, wall rush…)']]);
  const ln = $('mvLearn'); if (ln) ln.onchange = () => set((x) => { const v = parseInt(ln.value, 10); if (v > 0) x.learn = Math.min(100, v); else delete x.learn; });
  for (const [id, k, max] of [['mvCp', 'cp', 999], ['mvCpm', 'cp_mastered', 999], ['mvAp', 'ap', 9999]]) { const el = $(id); if (el) el.onchange = () => set((x) => { const v = parseInt(el.value, 10); if (v >= 0) x[k] = Math.min(max, v); else delete x[k]; }); }
  const bo = $('mvBonus'); if (bo) bo.onchange = () => set((x) => { const v = bo.value.trim().toLowerCase(); if (!v) delete x.bonus; else if (v === 'none' || v === '-1') x.bonus = null; else if (/^(0x)?[0-9a-f]+$/.test(v) && !isNaN(parseInt(v, v.startsWith('0x') ? 16 : 10))) x.bonus = parseInt(v, v.startsWith('0x') ? 16 : 10); else toast('Equip bonus: an effect-set number, or none.'); });
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
function mvNewHTML() {
  const o = S.newMove, n = S.gmo ? S.gmo.motions.length : 0;
  let anims = '';
  for (let i = 0; i < n; i++) anims += `<option value="${i}"${i === o.motion ? ' selected' : ''}>${esc(motionName(i))}${S.tlOfMotion.has(i) ? '' : ' (no events)'}${(S.usedBy.get(i) || []).length ? ' · ' + esc(S.usedBy.get(i)[0]) : ''}</option>`;
  const from = S.moves.map((m, i) => m.burst ? '' : `<option value="${i}"${o.from === String(i) ? ' selected' : ''}>copy ${esc(m.name)} (${m.steps.length} press${m.steps.length > 1 ? 'es' : ''})</option>`).join('');
  const seg = (name, list, val) => `<span class="mvseg" role="group">${list.slice(0, 2).map(([v, l]) => `<button type="button" data-n${name}="${v}" aria-pressed="${val === v}">${l}</button>`).join('')}</span>`;
  const key = mvFreeKey(), cmd = mvFreeCmd();
  const src = o.from === '' ? mvTemplate(o.type, o.phase) : S.moves[+o.from];
  const presses = o.from === '' ? 1 : src.steps.length;
  return `<div class="sec" id="mvNew"><h3>New move</h3>
    <div class="field"><label>Name</label><input type="text" id="nmName" value="${esc(o.name)}" placeholder="Move name"></div>
    <div class="row2"><div class="field"><label>Type</label>${seg('type', MV_KINDS, o.type)}</div><div class="field"><label>Starts</label>${seg('phase', MV_PHASES, o.phase)}</div></div>
    <div class="field"><label>Presses</label><select id="nmFrom"><option value="">one press: pick its animation</option>${from}</select></div>
    ${o.from === '' ? `<div class="field"><label>Animation</label><select id="nmMotion">${anims}</select></div>` : ''}
    <div class="row2">${numField('Learnt at level', o.learn, 'id="nmLearn" min="1" max="100"')}<div class="field fact"><label>Ability · command</label><span class="mono">${key} · ${hex4(cmd).replace('0x00', '0x')}</span></div></div>
    <div class="field"><label>Help text</label><textarea id="nmHelp" rows="2" placeholder="[Close] What it does.">${esc(o.help)}</textarea></div>
    <p class="note">Makes ability ${key} (a copy of ${esc(src ? src.name : '?')}'s record: its CP cost and bonus line), command ${hex4(cmd).replace('0x00', '0x')} with ${presses} new press${presses > 1 ? 'es' : ''} (actions and scripts of its own)${S.files[mvBratPath()] ? ', and a CPU row like ' + esc(src ? src.name : '') + '\'s' : ''}.
      ${o.from === '' ? 'The press keeps that move\'s first-press settings (reach the CPU needs, recovery, landing).' : 'Each press plays the same animation as the copy, so they share its events; pick another animation for a press (and create its timeline) to give it its own.'}</p>
    <div class="btnrow"><button type="button" id="nmCreate" class="primary">Create move</button><button type="button" id="nmCancel">Cancel</button></div>
    <div class="lbl">Or copy one from another character</div>
    <div class="btnrow"><button type="button" id="nmCopyFrom" title="A retail move from the game ISO, or a move of another mod character: its presses, animations, events, hits, hitboxes, projectiles, effects and sounds">Copy a move from…</button></div></div>`;
}
function mvWireNew() {
  const sec = $('mvNew'); if (!sec) return;
  const o = S.newMove, keep = () => { o.name = $('nmName').value; o.help = $('nmHelp').value; o.learn = $('nmLearn').value; };
  sec.querySelectorAll('[data-ntype]').forEach((b) => b.onclick = () => { keep(); o.type = b.dataset.ntype; renderInspector(); });
  sec.querySelectorAll('[data-nphase]').forEach((b) => b.onclick = () => { keep(); o.phase = b.dataset.nphase; renderInspector(); });
  $('nmFrom').onchange = () => { keep(); o.from = $('nmFrom').value; if (o.from !== '') { const k = mvKind(S.moves[+o.from]); if (k.type) o.type = k.type; if (k.phase && k.phase !== 'follow-up') o.phase = k.phase; } renderInspector(); };
  const mo = $('nmMotion'); if (mo) mo.onchange = () => { o.motion = +mo.value; setMotion(o.motion); renderTimeline(); };
  $('nmCancel').onclick = () => { S.newMove = null; renderInspector(); };
  $('nmCreate').onclick = () => { keep(); mvCreate(o); };
  if ($('nmCopyFrom')) $('nmCopyFrom').onclick = (ev) => cpMenu(ev.currentTarget);
}
function mvStartNew() {
  if (!mvCanEdit()) { toast(mvWhyNot() || 'This character can\'t get new moves here.'); return; }
  S.sel = null; S.showChecks = false; S.whole = null;
  S.newMove = { name: '', help: '', type: 'brave', phase: 'ground', from: '', motion: Math.max(0, S.motion), learn: '1' };
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
  if (!base && typeof DM !== 'undefined' && DM.on && DM.sim && DM.sim.move === S.moves.indexOf(m) && DM.sim.extra.length) {
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
  S.whole = m.steps.length > 1 ? { move: S.step.move } : null;
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
    S.step = { move: S.whole.move, i: ni };
    S.fxEdit = null; S.motion = segs[ni].motion; S.frame = Math.min(f, segs[ni].len);
    if (typeof dummyReset === 'function' && ni === 0) dummyReset();
    pose(); renderLeft(); renderTimeline(); renderInspector();
    return true;
  }
  S.frame = f; pose(); updatePlayhead();
  return true;
}
function mvStrip() {                           // the presses of the selected move as one strip above the timeline
  const el = $('mvStrip'); if (!el) return;
  const m = S.step && !S.fxEdit && S.moves && S.moves[S.step.move];
  if (!m || m.steps.length < 1) { el.hidden = true; return; }
  const segs = mvSegments(m), total = segs.reduce((a, s) => a + s.len, 0) || 1;
  el.hidden = false;
  el.innerHTML = '<div class="mvs-row">' + segs.map((s) => `<button type="button" class="mvs${s.i === S.step.i ? (S.moveOnly ? ' cur' : ' on') : ''}" data-i="${s.i}" style="flex:${s.len} 1 0" title="${s.why ? esc(s.why) + ': action ' + s.action : 'press ' + (s.i + 1)}: ${esc(motionName(s.motion))}, plays ${Math.round(s.len)} of ${s.full} frames. ${S.moveOnly || S.step.i !== s.i ? 'Click to play only this press' : 'Click again for the whole move'}"><i>${s.why ? '↳' : s.i + 1}</i> ${esc(motionName(s.motion))}</button>`).join('')
    + `<span class="mvs-head" id="mvHead"></span></div><span class="mvs-total mono">${Math.round(total)} f · ${(total / 30).toFixed(2)} s</span>`
    + (mvMoreWithDummy(m) ? `<button type="button" class="mini mvs-more" id="mvsMore" title="Part of this move is started by the game when a hit or shot lands (${mvMoreWithDummy(m)}=): the dummy plays it, with its effects">+ what plays on hit: turn on the dummy</button>` : '');
  const more = el.querySelector('#mvsMore'); if (more) more.onclick = () => { const cb = $('dmOn'); if (cb) { cb.checked = true; cb.dispatchEvent(new Event('change')); setTimeout(() => renderAll(), 600); } };
  el.querySelectorAll('.mvs').forEach((b) => b.onclick = () => {
    const i = +b.dataset.i;
    if (!S.moveOnly && S.step.i === i) { mvSelectWhole(); return; }   // the picked press again: back to the whole move
    S.whole = null; S.step = { move: S.step.move, i }; S.moveOnly = false; S.sel = null; setMotion(segs[i].motion); renderAll();
  });
  mvStripHead();
}
function mvMoreWithDummy(m) {                  // a hook kind that adds actions after a landed hit, when the dummy is off
  if ((typeof DM !== 'undefined' && DM.on) || typeof ckMoveHooks !== 'function') return '';
  const h = ckMoveHooks(m).find((x) => ['hitact', 'chainact', 'landact'].includes(x.kind));
  return h ? h.kind : '';
}
function mvStripHead() {
  const hd = $('mvHead'), m = S.step && S.moves && S.moves[S.step.move];
  if (!hd || !m) return;
  const segs = mvSegments(m), total = segs.reduce((a, s) => a + s.len, 0) || 1;
  const at = segs.slice(0, S.step.i).reduce((a, s) => a + s.len, 0) + Math.min(S.frame, segs[S.step.i] ? segs[S.step.i].len : 0);
  hd.style.left = (100 * at / total) + '%';
}
