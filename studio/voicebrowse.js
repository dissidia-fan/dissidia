/* Move Studio: the voice browser and the character's own voice set. Opened from a voice event's inspector (Browse voices…).
   - Voice set: this rig's battle voice bank, voice/en/battle/voice_p_<code>_00.scd (the assist rig: _a_00). The game loads
     it for every fighter whose costume has a .cosx (tools/charmod.py voice_set gives added characters theirs; before that
     they loaded none). It starts as the based-on character's bank, read from the ISO, until the folder has its own copy.
   - Custom lines: the slots past the based-on character's, added here (Add a voice line…, or Copy in from Other fighters).
     The bank keeps the based-on character's id and its slots byte for byte, so in a mirror match the shared cues still
     play the same clips and the added cues (past that bank's count) only resolve in this one: the same rule as the sound
     bank (claude/dissidia-fep-ids-and-cpu-ai.md). The first added line makes the folder's own copy, files/voice/<bank>.scd,
     listed in mod.json files; names are kept in mod.json voice_lines {<bank name>: {slot: name}}.
   - Other fighters: every retail voice bank, to listen to and copy lines in from (a bank is only loaded when that
     fighter is in the fight, so lines are copied, never referenced).
   Timeline cues are game cues minus edCueShift (the assist's +50000, editors.js). Loaded after sndbrowse.js. */
'use strict';
const VOXB = { src: 'own', q: '', other: '' };
const VOX_LANG = 'en';                             // the disc has English battle voices only
const voxAssist = () => /_a$/.test(S.msDir || '') || !!(S.rig && S.rig.retail && /_a$/.test(S.rig.retail.cos));
const voxBankName = (gp) => gp.split('/').pop().replace(/\.scd$/, '');
function voxGamePath() {                           // this rig's voice bank, as the game asks for it
  const c = (S.mod && S.mod.character) || {}, code = S.rig && S.rig.retail ? S.rig.retail.code : c.code;
  return code ? `voice/${VOX_LANG}/battle/voice_p_${code}${voxAssist() ? '_a' : ''}_00.scd` : null;
}
function voxDonor(gp) {                            // the retail bank it was copied from: a SharedSnd BANKS record, or null
  const c = (S.mod && S.mod.character) || {}, src = ((S.mod && S.mod.files) || {})[gp];
  const like = src && src.like ? src.like : c.based_on && c.code ? gp.replace(c.code, c.based_on) : gp;
  return typeof SharedSnd === 'undefined' ? null : SharedSnd.banks({ group: 'voice' }).find((b) => b.name === voxBankName(like)) || null;
}
function voxSource(gp) {                           // {folder: path} (the folder's own copy) or {iso: game path}
  const src = ((S.mod && S.mod.files) || {})[gp];
  if (typeof src === 'string') return { folder: S.charDir + src };
  const d = voxDonor(gp);
  return d ? { iso: d.path } : null;
}
const voxCan = () => !!S.modPath && typeof edCan === 'function' && edCan() && !(S.rig && S.rig.retail);
/* load this rig's voice bank into S.files (once per rig load): -> {gp, path, donor} | {need: 'iso'} | null */
function voxLoad() {
  if (S.vox && S.vox.files === S.files && !(S.vox.need && ISO.index)) return S.vox.p;
  const v = S.vox = { files: S.files, need: false, p: null };
  v.p = (async () => {
    const gp = voxGamePath(), src = gp && voxSource(gp); if (!src) return null;
    const path = src.folder || S.charDir + 'files/voice/' + gp.split('/').pop();
    if (!S.files[path]) {
      let bin;
      try { bin = src.folder ? new Uint8Array(await FS.read(src.folder)) : ISO.index ? new Uint8Array(await isoGet(src.iso)) : null; }
      catch (e) { console.warn('studio: voice bank', gp, e); return null; }
      if (!bin) { v.need = true; return { need: 'iso' }; }
      if (v.files !== S.files) return null;       // another rig loaded meanwhile
      S.files[path] = { bin, binOrig: bin, dirty: false, voxgp: gp, created: !src.folder };
    }
    const d = voxDonor(gp);
    return { gp, path, donor: d ? d.slots : SndBank.parseBank(S.files[path].binOrig || S.files[path].bin).slots.length };
  })();
  return v.p;
}
function voxNow() {                                // the loaded voice set, synchronously: {gp, path, donor, bank, base} or null
  const p = Object.keys(S.files).find((k) => S.files[k].voxgp); if (!p) return null;
  const f = S.files[p]; if (f.bankOf !== f.bin) { f.bank = SndBank.parseBank(f.bin); f.bankOf = f.bin; }
  const d = voxDonor(f.voxgp);
  return { gp: f.voxgp, path: p, bank: f.bank, base: edTlBase(f.bank), donor: d ? d.slots : f.bank.slots.length };
}
const voxNames = (gp) => (((S.mod && S.mod.voice_lines) || {})[voxBankName(gp)] || {});
function voxSlotOf(cue) {                          // a timeline voice cue in this rig's own voice set -> {slot, name, added} or null
  const v = voxNow(); if (!v || cue == null) return null;
  const slot = cue - v.base; if (slot < 0 || slot >= v.bank.slots.length || v.bank.slots[slot].kind === 'empty') return null;
  return { slot, name: voxNames(v.gp)[slot] || '', added: slot >= v.donor };
}
function voxFighter(bank) {                       // 'voice_p_fiv100_a_00' -> 'Bartz (second set)'
  const m = /^voice_p_(\w+?)(_a)?_00$/.exec(bank.name); if (!m) return bank.name;
  const r = typeof ROSTER !== 'undefined' && ROSTER.find(([c]) => c === m[1]);
  return (r ? r[1] : m[1]) + (m[2] ? ' (second set)' : '');
}
function voxUses() {                               // cue -> how many voice events of this rig play it
  const n = new Map();
  for (const p of S.tlPaths || []) for (const e of ((F(p) || {}).keyed || [])) if (typeof e.voice === 'number') n.set(e.voice, (n.get(e.voice) || 0) + 1);
  return n;
}
async function voxRows(src) {                      // [{cue, slot, name, seconds, kind, choices, added}] or null (needs the ISO)
  if (src === 'own' || src === 'custom') {
    const r = await voxLoad(); if (!r) return []; if (r.need) return null;
    const v = voxNow(), names = voxNames(v.gp);
    return v.bank.slots.filter((s) => s.kind !== 'empty' && (src === 'own' || s.index >= v.donor))
      .map((s) => ({ cue: v.base + s.index, slot: s.index, name: names[s.index] || '', seconds: s.seconds || 0, kind: s.kind, added: s.index >= v.donor }));
  }
  if (!ISO.index || typeof shRows !== 'function') return null;
  return (await shRows(+src)).map((r) => ({ cue: r.cue, slot: r.slot, name: '', seconds: r.seconds || 0, kind: r.kind, loop: r.loop, choices: r.choices }));
}
async function voiceBrowse(cur, onPick) {
  const back = document.createElement('div'); back.className = 'modal-back';
  const gp = voxGamePath(), canAdd = voxCan() && !!gp;
  const ownD = gp ? voxDonor(gp) : null, ch = (S.mod && S.mod.character) || {};
  const others = typeof SharedSnd === 'undefined' ? [] : SharedSnd.banks({ group: 'voice' });
  if (!VOXB.other || !others.some((b) => String(b.id) === VOXB.other)) VOXB.other = others.length ? String(others[0].id) : '';
  const SRC = [['own', 'Voice set', gp ? `${esc(voxBankName(gp))}${ownD && !(S.rig && S.rig.retail) && ownD.name !== voxBankName(gp) ? ', from ' + esc(voxFighter(ownD)) + "'s" : ''}` : 'no voice bank'],
    ['custom', 'Custom lines', `added to ${esc(ch.name || 'this character')}`], ['other', 'Other fighters', 'listen, then copy one in']];
  if (!gp && VOXB.src !== 'other') VOXB.src = 'other';
  back.innerHTML = `<div class="modal sndb" role="dialog" aria-label="Voice lines">
    <div class="sndb-head"><h3>Voice lines</h3><input type="search" id="voxQ" placeholder="Search by slot, cue or name" value="${esc(VOXB.q)}"><button type="button" class="mini" id="voxX" aria-label="Close"><i class="ic ic-x"></i></button></div>
    <div class="sndb-body"><nav class="sndb-src">${SRC.map(([k, l, n]) => `<button type="button" data-src="${k}"${VOXB.src === k ? ' aria-pressed="true"' : ''}${k !== 'other' && !gp ? ' disabled' : ''}><b>${l}</b><small>${n}</small></button>`).join('')}
      <div id="voxOtherW"${VOXB.src === 'other' ? '' : ' hidden'}><select id="voxOther" aria-label="Fighter">${others.map((b) => `<option value="${b.id}"${String(b.id) === VOXB.other ? ' selected' : ''}>${esc(voxFighter(b))}</option>`).join('')}</select></div>
      ${canAdd ? `<label class="btn sndb-add"><input type="file" id="voxFile" accept="audio/*,.wav" hidden><i class="ic ic-plus"></i> Add a voice line…</label><small class="sndb-addn">Added to the end of ${esc(ch.name || 'this character')}'s voice bank, so it never collides with the based-on character's.</small>` : ''}</nav>
      <div class="sndb-list" id="voxList"><p class="note">Loading…</p></div></div></div>`;
  document.body.appendChild(back);
  const close = () => { back.remove(); document.removeEventListener('keydown', onKey, true); };
  const onKey = (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); } };
  document.addEventListener('keydown', onKey, true);
  back.addEventListener('pointerdown', (e) => { if (e.target === back) close(); });
  $('voxX').onclick = close;
  const setSrc = (k) => { VOXB.src = k; back.querySelectorAll('[data-src]').forEach((x) => x.setAttribute('aria-pressed', String(x.dataset.src === k))); $('voxOtherW').hidden = k !== 'other'; };
  const draw = async (flash) => {
    const L = $('voxList'); if (!L) return;
    const other = VOXB.src === 'other', custom = VOXB.src === 'custom';
    const rows = await voxRows(other ? VOXB.other : VOXB.src); if (!document.body.contains(L)) return;
    if (rows === null) { L.innerHTML = '<p class="note">Set the game ISO to list the game\'s voice banks.</p>'; return; }
    const uses = voxUses(), q = VOXB.q.trim().toLowerCase();
    const shown = rows.filter((r) => !q || `${r.slot} ${r.cue} ${r.name}`.toLowerCase().includes(q));
    const empty = custom && !rows.length ? `<p class="note">No custom voice lines yet.${canAdd ? ' Add one from an audio file, or copy one in from Other fighters.' : ''}</p>` : '<p class="note">Nothing matches.</p>';
    L.innerHTML = shown.length ? shown.map((r) => {
      const u = other ? 0 : uses.get(r.cue) || 0, on = !other && r.cue === cur;
      const act = other ? (canAdd ? `<button type="button" class="mini primary" data-copy="${r.cue}" data-slot="${r.slot}" title="Copy this line to the end of this character's voice bank">Copy in</button>` : '')
        : onPick ? `<button type="button" class="mini${on ? '' : ' primary'}" data-use="${r.cue}"${on ? ' disabled' : ''}>${on ? 'In use' : 'Use'}</button>` : '';
      return `<div class="sndb-row${on ? ' on' : ''}">
        <button type="button" class="mini sndb-play" data-play="${r.cue}" aria-label="Play"><i class="ic ic-play"></i></button>
        <span class="sndb-name"><b>${r.name ? esc(r.name) : 'Slot ' + r.slot}</b><small class="mono">${r.name ? 'slot ' + r.slot + ' · ' : ''}cue ${r.cue}${r.added && !custom ? ' · added' : ''}${r.kind === 'random' ? ' · random of ' + r.choices.length : ''}${u ? ` · used ${u}×` : ''}</small></span>
        <span class="mono sndb-len">${r.seconds.toFixed(2)} s</span>${act}</div>`;
    }).join('') : empty;
    L.querySelectorAll('[data-play]').forEach((b) => b.onclick = () => (other ? shPlay(+b.dataset.play) : psndPlay(+b.dataset.play, true)));
    L.querySelectorAll('[data-use]').forEach((b) => b.onclick = () => { onPick(+b.dataset.use); close(); });
    L.querySelectorAll('[data-copy]').forEach((b) => b.onclick = async () => {
      const bank = SharedSnd.bank(+VOXB.other), def = `${voxFighter(bank)} ${b.dataset.slot}`;
      const name = await uiAsk('Copy the voice line in', 'Name', def, { ok: 'Copy in', hint: 'Shown in Custom lines and on the event; the game only uses its cue.' });
      if (name == null) return;
      try {
        const d = await SharedSnd.decodeCue(+b.dataset.copy, isoGet);
        const cue = await voxAddPcm({ rate: d.rate, pcm: d.pcm }, name.trim() || def);
        setSrc('custom'); await draw(cue);
      } catch (e) { toast('Could not copy that line: ' + e.message, 'bad'); }
    });
    if (flash != null) { psndPlay(flash, true); const row = L.querySelector(`[data-play="${flash}"]`); if (row) row.closest('.sndb-row').scrollIntoView({ block: 'nearest' }); }
  };
  back.querySelectorAll('[data-src]').forEach((b) => b.onclick = () => { setSrc(b.dataset.src); draw(); });
  $('voxOther').onchange = () => { VOXB.other = $('voxOther').value; draw(); };
  $('voxQ').oninput = () => { VOXB.q = $('voxQ').value; draw(); };
  const fi = $('voxFile');
  if (fi) fi.onchange = async () => {
    const file = fi.files[0]; if (!file) return;
    try {
      const name = await uiAsk('Name the voice line', 'Name', file.name.replace(/\.[^.]+$/, ''), { ok: 'Add', hint: 'Shown in Custom lines and on the event; the game only uses its cue.' });
      if (name == null) { fi.value = ''; return; }
      const cue = await voxAddPcm(await sndDecodeFile(file), name.trim() || file.name);
      VOXB.q = ''; $('voxQ').value = ''; setSrc('custom'); await draw(cue);
    } catch (e) { toast('Could not add ' + file.name + ': ' + e.message, 'bad'); }
    fi.value = '';
  };
  draw(); $('voxQ').focus();
}
/* a clip -> a new slot at the end of this rig's voice bank (full volume, like the retail voice clips); returns its timeline cue */
async function voxAddPcm(pcm, name) {
  const r = await voxLoad();
  if (!r || r.need) throw new Error(r && r.need ? 'set the game ISO first (the voice set starts as the based-on character\'s bank)' : 'this character has no voice bank');
  const v = voxNow(), f = S.files[v.path], bank = SndBank.parseBank(f.bin);
  const slot = SndBank.addSound(bank, pcm, { volume: 100 }), cue = edTlBase(bank) + slot;
  const bn = voxBankName(v.gp), rel = v.path.slice(S.charDir.length);
  edit([v.path, S.modPath], () => {
    f.bin = SndBank.bankToBytes(bank);
    const m = S.mod; m.voice_lines = m.voice_lines || {}; (m.voice_lines[bn] = m.voice_lines[bn] || {})[slot] = name;
    m.files = m.files || {}; m.files[v.gp] = rel;     // the folder's own copy of the bank from now on
  });
  toast(`${name}: slot ${slot} of ${bn}, cue ${cue}. Save keeps it.`, 'ok');
  return cue;
}
