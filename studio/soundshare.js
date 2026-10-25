/* Move Studio: the shared sounds a sound event can play (not in the character's own bank): where a cue lives, play it,
   and pick one of the common battle sounds (bank 1000 snd_battle, bank 100 snd_common). sharedsnd.js has the bank table
   (work/studio_sharedsnd/README.md); the sounds are read from the game ISO. Loaded after editors.js. */
const SH = { rows: new Map(), audio: null };
const SH_PICK = [[1000, 'battle sounds (snd_battle)'], [100, 'system sounds (snd_common)']];
function shGet(path) { return isoGet(path); }
function shHTML(e, mine) {
  if (typeof SharedSnd === 'undefined') return '';
  const loc = SharedSnd.cueLocation(e.sound), iso = !!ISO.index;
  let h = '<div class="lbl">Shared sounds</div>';
  if (!mine) h += loc ? `<p class="note">Cue ${e.sound} = slot ${loc.slot} of ${esc(loc.name)} (bank ${loc.id}, ${esc(loc.group)}${loc.battle ? ', loaded in battle' : ', not loaded in battle'}).</p>` : `<p class="warnline">Cue ${e.sound} is in no bank the game has: it plays nothing.</p>`;
  h += `<div class="field"><label>A shared battle sound</label><div class="btnrow"><select id="shPick"${iso && edCan() ? '' : ' disabled'}><option value="">${iso ? 'loading…' : 'set the game ISO to list them'}</option></select><button type="button" id="shPlay" class="mini"${iso && !mine && loc ? '' : ' disabled'}>▶ Play</button></div></div>`;
  return h;
}
async function shRows(id) {
  if (!SH.rows.has(id)) SH.rows.set(id, SharedSnd.loadBank(id, shGet).then((b) => SharedSnd.listCues(b)).catch((e) => { console.warn('studio: shared bank', id, e); return []; }));
  return SH.rows.get(id);
}
async function shPlay(cue) {
  try {
    const d = await SharedSnd.decodeCue(cue, shGet, { pick: 'random' });
    SH.audio = SH.audio || new (window.AudioContext || window.webkitAudioContext)();
    const buf = SH.audio.createBuffer(1, d.pcm[0].length, d.rate); buf.copyToChannel(d.pcm[0], 0);
    const src = SH.audio.createBufferSource(); src.buffer = buf; src.connect(SH.audio.destination); if (typeof psndTrack === 'function') psndTrack(src); src.start();
  } catch (e) { toast('Could not play cue ' + cue + ': ' + e.message); }
}
function shWire(p, kind, idx) {
  const sel = $('shPick'), pl = $('shPlay'); if (!sel || typeof SharedSnd === 'undefined') return;
  const cur = F(p)[kind][idx].sound;
  if (pl) pl.onclick = () => shPlay(F(p)[kind][idx].sound);
  if (!ISO.index) return;
  Promise.all(SH_PICK.map(([id]) => shRows(id))).then((all) => {
    if (!document.body.contains(sel)) return;
    sel.innerHTML = '<option value="">(pick one)</option>' + SH_PICK.map(([id, label], k) => `<optgroup label="${esc(label)}">${all[k].map((r) => `<option value="${r.cue}"${r.cue === cur ? ' selected' : ''}>${r.slot} · ${(r.seconds || 0).toFixed(2)} s${r.kind === 'random' ? ' · random of ' + r.choices.length : ''}${r.loop ? ' · loops' : ''}</option>`).join('')}</optgroup>`).join('');
  });
  sel.onchange = () => {
    const v = parseInt(sel.value, 10); if (isNaN(v)) return;
    shPlay(v);
    if (!edCan()) return;
    edit([p], () => { F(p)[kind][idx].sound = v; }); renderTimeline(); renderInspector();
  };
}
