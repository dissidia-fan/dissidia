/* Move Studio: the sound browser. Opened from a sound event's inspector (Browse sounds…): the character's own bank, the
   battle sounds every stage loads (snd_battle) and the system sounds (snd_common), each one playable, searchable and
   picked with Use. Add a sound… puts an audio file (WAV, MP3, OGG…) into the character's own bank as a new slot (slots
   0–123 stay as they are) and names it. Loaded after playsnd.js. */
'use strict';
const SNDB = { src: 'own', q: '' };
async function sndbRows(src) {                     // [{cue, slot, name, seconds, kind, added}]
  if (src === 'own') {
    const b = typeof edBank === 'function' ? edBank() : null; if (!b) return [];
    const base = SndBank.cueBase(b);
    return b.slots.filter((s) => s.kind !== 'empty').map((s) => ({ cue: base + s.index, slot: s.index, name: edSndName(s.index), seconds: s.seconds || 0, kind: s.kind, added: s.index >= 124 }));
  }
  if (!ISO.index || typeof shRows !== 'function') return null;
  return (await shRows(src)).map((r) => ({ cue: r.cue, slot: r.slot, name: '', seconds: r.seconds || 0, kind: r.kind, loop: r.loop, choices: r.choices }));
}
function sndBrowse(cur, onPick) {
  const back = document.createElement('div'); back.className = 'modal-back';
  const own = typeof edBank === 'function' && edBank(), canAdd = own && typeof edCan === 'function' && edCan();
  const SRC = [['own', 'This character', own ? '' : 'no sound bank of its own'], [1000, 'Battle', 'snd_battle, every stage'], [100, 'System', 'snd_common, always loaded']];
  if (!own && SNDB.src === 'own') SNDB.src = 1000;
  back.innerHTML = `<div class="modal sndb" role="dialog" aria-label="Sounds">
    <div class="sndb-head"><h3>Sounds</h3><input type="search" id="sndbQ" placeholder="Search by slot, cue or name" value="${esc(SNDB.q)}"><button type="button" class="mini" id="sndbX" aria-label="Close"><i class="ic ic-x"></i></button></div>
    <div class="sndb-body"><nav class="sndb-src">${SRC.map(([k, l, n]) => `<button type="button" data-src="${k}"${String(SNDB.src) === String(k) ? ' aria-pressed="true"' : ''}${k === 'own' && !own ? ' disabled' : ''}><b>${l}</b><small>${esc(n)}</small></button>`).join('')}
      ${canAdd ? `<label class="btn sndb-add"><input type="file" id="sndbFile" accept="audio/*,.wav" hidden><i class="ic ic-plus"></i> Add a sound…</label><small class="sndb-addn">Goes into this character's bank as a new slot.</small>` : ''}</nav>
      <div class="sndb-list" id="sndbList"><p class="note">Loading…</p></div></div></div>`;
  document.body.appendChild(back);
  const close = () => { back.remove(); document.removeEventListener('keydown', onKey, true); };
  const onKey = (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); } };
  document.addEventListener('keydown', onKey, true);
  back.addEventListener('pointerdown', (e) => { if (e.target === back) close(); });
  $('sndbX').onclick = close;
  const draw = async () => {
    const L = $('sndbList'); if (!L) return;
    const rows = await sndbRows(SNDB.src);
    if (rows === null) { L.innerHTML = '<p class="note">Set the game ISO to list the game\'s shared sounds.</p>'; return; }
    const q = SNDB.q.trim().toLowerCase(), shown = rows.filter((r) => !q || `${r.slot} ${r.cue} ${r.name}`.toLowerCase().includes(q));
    L.innerHTML = shown.length ? shown.map((r) => `<div class="sndb-row${r.cue === cur ? ' on' : ''}">
        <button type="button" class="mini sndb-play" data-play="${r.cue}" aria-label="Play"><i class="ic ic-play"></i></button>
        <span class="sndb-name"><b>${r.name ? esc(r.name) : 'Slot ' + r.slot}</b><small class="mono">${r.name ? 'slot ' + r.slot + ' · ' : ''}cue ${r.cue}${r.added ? ' · added' : ''}${r.kind === 'random' ? ' · random of ' + r.choices.length : ''}${r.loop ? ' · loops' : ''}</small></span>
        <span class="mono sndb-len">${r.seconds.toFixed(2)} s</span>
        ${onPick ? `<button type="button" class="mini${r.cue === cur ? '' : ' primary'}" data-use="${r.cue}"${r.cue === cur ? ' disabled' : ''}>${r.cue === cur ? 'In use' : 'Use'}</button>` : ''}</div>`).join('') : '<p class="note">Nothing matches.</p>';
    L.querySelectorAll('[data-play]').forEach((b) => b.onclick = () => psndPlay(+b.dataset.play, true));
    L.querySelectorAll('[data-use]').forEach((b) => b.onclick = () => { onPick(+b.dataset.use); close(); });
  };
  back.querySelectorAll('[data-src]').forEach((b) => b.onclick = () => { SNDB.src = b.dataset.src === 'own' ? 'own' : +b.dataset.src; back.querySelectorAll('[data-src]').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); draw(); });
  $('sndbQ').oninput = () => { SNDB.q = $('sndbQ').value; draw(); };
  const fi = $('sndbFile');
  if (fi) fi.onchange = async () => {
    const file = fi.files[0]; if (!file) return;
    try {
      const name = await uiAsk('Name the sound', 'Name', file.name.replace(/\.[^.]+$/, ''), { ok: 'Add', hint: 'Shown in this list and the event; the game only uses its slot number.' });
      if (name == null) return;
      const cue = await sndAddFile(file, name.trim() || file.name);
      SNDB.src = 'own'; SNDB.q = ''; $('sndbQ').value = '';
      back.querySelectorAll('[data-src]').forEach((x) => x.setAttribute('aria-pressed', String(x.dataset.src === 'own')));
      await draw(); psndPlay(cue, true);
      const row = $('sndbList').querySelector(`[data-play="${cue}"]`); if (row) row.closest('.sndb-row').scrollIntoView({ block: 'nearest' });
    } catch (e) { toast('Could not add ' + file.name + ': ' + e.message, 'bad'); }
    fi.value = '';
  };
  draw(); $('sndbQ').focus();
}
async function sndAddFile(file, name) {           // an audio file -> a new slot of the character's bank; returns its cue
  let wav = new Uint8Array(await file.arrayBuffer());
  const isWav = wav.length > 12 && String.fromCharCode(...wav.subarray(0, 4)) === 'RIFF' && String.fromCharCode(...wav.subarray(8, 12)) === 'WAVE';
  let pcm;
  try { if (!isWav) throw new Error('not a WAV'); pcm = SndBank.readWav(wav); }
  catch (e) {                                      // any other format (or a WAV the reader doesn't take): let the browser decode it
    const A = new (window.OfflineAudioContext || window.webkitOfflineAudioContext)(1, 1, 22050);
    const buf = await A.decodeAudioData(wav.buffer.slice(wav.byteOffset, wav.byteOffset + wav.byteLength));
    const ch = [...Array(buf.numberOfChannels)].map((_, i) => buf.getChannelData(i));
    pcm = SndBank.readWav(SndBank.writeWav({ pcm: ch, rate: buf.sampleRate }));
  }
  const sp = edSndPath(), bank = SndBank.parseBank(S.files[sp].bin);
  const slot = SndBank.addSound(bank, pcm, {}), cue = SndBank.cueOf(bank, slot);
  edit([sp, S.modPath].filter(Boolean), () => {
    S.files[sp].bin = SndBank.bankToBytes(bank);
    if (S.modPath) { S.mod.sound_names = S.mod.sound_names || {}; S.mod.sound_names[slot] = name; }
  });
  toast(`${name}: slot ${slot}, cue ${cue}. Save keeps it.`, 'ok');
  return cue;
}
