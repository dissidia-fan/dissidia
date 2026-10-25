/* Move Studio: the sounds of the playing animation, with Sound on (the 3D view's options): sound (0x0303), voice
   (0x0315), sound-if-part-visible (0x0311), sound-if-form (0x0312) and footsteps (0x060F) play as the playhead passes
   them. The character's own bank plays without the ISO; shared banks (battle, voices, floors) need it.
   Footsteps: in game the floor under the fighter picks the bank (map_sound_param.bin: material x 7 kinds x 4 footwear
   classes, docs/archive/event-ops/E.md); the preview stands on stone (snd_stone, bank 50000, 28 slots) with footwear 0.
   Also here: folding the 3D view's option row. Loaded after soundshare.js. */
'use strict';
const PSND = { last: null, audio: null, live: new Set(), tabKey: '' };
function psndTrack(src, fromPlayback) {          // every sound the Studio starts, so a tab change / pause can stop it
  src._pb = !!fromPlayback; PSND.live.add(src); src.addEventListener('ended', () => PSND.live.delete(src));
}
function psndStop(onlyPlayback) { for (const s of [...PSND.live]) if (!onlyPlayback || s._pb) { try { s.stop(); } catch (e) { /* already ended */ } PSND.live.delete(s); } }
setInterval(() => {                               // another tab or the effect editor: all of them; paused or Sound off: the playback ones
  const key = S.tab + '|' + (S.fxEdit ? 'fx' : '');
  if (key !== PSND.tabKey) { if (PSND.tabKey) psndStop(false); PSND.tabKey = key; }
  if (!S.playing || !($('sndOn') || {}).checked) psndStop(true);
}, 150);
const PSND_FLOOR = 5000000;                       // snd_stone: slot = kind * 4 + footwear (GUESS: the table's own order)
function psndCue(e) {                              // the cue an event plays, or null
  const c = opCode(e.op), a = argsOf(e), n = (v) => (typeof v === 'number' && v >= 0 && v !== 0xFFFF && v !== 0xFFFFFFFF ? v : null);
  if (c === 0x0303) return n(e.sound ?? a[0]);
  if (c === 0x0315) return n(e.voice ?? a[0]);
  if (c === 0x0311 || c === 0x0312) return n(e.sound ?? a[1]);
  if (c === 0x060F) { const k = e.kind ?? a[0]; return typeof k === 'number' && k >= 0 && k < 7 ? PSND_FLOOR + k * 4 : null; }
  return null;
}
async function psndDecode(cue) {                  // {pcm, rate} from the character's bank, else a shared one (needs the ISO)
  const b = typeof edBank === 'function' ? edBank() : null, base = b ? SndBank.cueBase(b) : -1;
  if (b && cue - base >= 0 && cue - base < b.slots.length) return SndBank.decodeSlot(b, cue - base);
  if (!ISO.index || typeof SharedSnd === 'undefined' || !SharedSnd.cueLocation(cue)) return null;
  return SharedSnd.decodeCue(cue, isoGet, { pick: 'random' });
}
async function psndPlay(cue, preview) {          // preview: a one-off (the sound browser), not stopped when playback pauses
  if (cue == null) return;
  try {
    const d = await psndDecode(cue); if (!d || !d.pcm || !d.pcm[0] || !d.pcm[0].length) return;
    const A = PSND.audio = PSND.audio || new (window.AudioContext || window.webkitAudioContext)();
    if (A.state === 'suspended') A.resume();
    const buf = A.createBuffer(1, d.pcm[0].length, d.rate); buf.copyToChannel(d.pcm[0], 0);
    const src = A.createBufferSource(); src.buffer = buf; src.connect(A.destination); psndTrack(src, !preview); src.start();
  } catch (e) { console.warn('studio: sound', cue, e.message); }
}
/* called after every pose: the keyed sound events the playhead passed since the last one (while playing) */
function psndAfterPose() {
  const cb = $('sndOn'), now = { m: S.motion, t: ticks(S.frame), k: S.step ? S.step.move + ':' + S.step.i : '', on: !!S.playing };
  const L = PSND.last; PSND.last = now;
  if (!cb || !cb.checked || !S.playing || S.fxEdit || S.motion < 0 || !L) return;
  const t = curTL(); if (!t) return;
  let spans;
  if (L.m !== now.m || L.k !== now.k || (!L.on && L.t === 0)) spans = [[-1, now.t]];   // a new animation / press, or play from frame 0: from its start                         // a new animation / press: from its start
  else if (now.t > L.t) spans = now.t - L.t > 40 ? [] : [[L.t, now.t]];              // a big jump forward is a scrub, not play
  else if (now.t < L.t) spans = [[L.t, Infinity], [-1, now.t]];                       // looped round
  else return;
  const ph = typeof stepPhase === 'function' ? stepPhase() : null;
  for (const e of t.keyed) {
    if (typeof evMode === 'function' && !evMode(e)) continue;
    const p = (e.x || 0) & 0xFF; if (p && ph != null && p !== ph) continue;           // filtered to another press's phase
    if (!spans.some(([a, b]) => e.frame > a && e.frame <= b)) continue;
    psndPlay(psndCue(e));
  }
}

/* the 3D view's options: Sound remembered, and the row folds away behind its toggle */
(function viewOptions() {
  const cb = $('sndOn'), hud = $('hud'), tog = $('hudTog');
  const get = (k) => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const set = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* per-viewer only */ } };
  if (cb) { cb.checked = get('studio.sound') !== '0'; cb.onchange = () => set('studio.sound', cb.checked ? '1' : '0'); }
  if (!hud || !tog) return;
  const apply = (folded) => {
    hud.classList.toggle('folded', folded); tog.setAttribute('aria-expanded', String(!folded));
    tog.title = folded ? 'Show the view options' : 'Hide the view options';
  };
  apply(get('studio.hudFold') === '1');
  tog.onclick = () => { const f = !hud.classList.contains('folded'); set('studio.hudFold', f ? '1' : '0'); apply(f); };
})();
