/* Move Studio: copy and paste events (Ctrl+C / Ctrl+V, between animations and characters), and the EX Mode preview.
   Loaded after checks.js. */

/* ---------------------------------------------------------------- the event filter's mode byte */
// ranged events keep their filter in `pad`, keyed ones in `x`: low byte = motion phase, next byte = mode
// (0 always, 1 outside EX Mode, 2 EX Mode only)
S.exMode = false;
function evMode(e) { const md = ((('pad' in e ? e.pad : e.x) || 0) >> 8) & 0xFF; return !md || md === (S.exMode ? 2 : 1); }
function exSet(on) {
  S.exMode = !!on;
  const cb = $('exOn'); if (cb) cb.checked = S.exMode;
  if (typeof DM !== 'undefined') DM.key = '';
  pose(); renderTimeline();
}
(function exWire() { const cb = $('exOn'); if (cb) cb.onchange = () => exSet(cb.checked); })();

/* ---------------------------------------------------------------- clipboard */
const CLIP_KEY = 'studio.clip';
function clipGet() { try { return S.clip || JSON.parse(localStorage.getItem(CLIP_KEY) || 'null'); } catch (e) { return S.clip || null; } }
function clipPut(c) { S.clip = c; try { localStorage.setItem(CLIP_KEY, JSON.stringify(c)); } catch (e) { /* private window */ } }
function clipCopy() {
  const t = curTL(); if (!t) { toast('This animation has no events to copy.'); return; }
  const from = `${S.char} ${motionName(S.motion)}`;
  if (S.sel && t[S.sel.kind] && t[S.sel.kind][S.sel.idx]) {
    const e = t[S.sel.kind][S.sel.idx];
    clipPut({ from, events: [{ kind: S.sel.kind, e: JSON.parse(JSON.stringify(e)) }], at: S.sel.kind === 'ranged' ? e.start : e.frame });
    toast(`Copied ${opInfo(e, S.sel.kind).label || e.op}. Ctrl+V pastes it at the playhead.`);
  } else {
    const evs = [...t.ranged.map((e) => ({ kind: 'ranged', e })), ...t.keyed.map((e) => ({ kind: 'keyed', e }))]
      .filter((x) => !(x.kind === 'keyed' && opCode(x.e.op) === 0x010C && x.e.frame === 0));   // the parts hidden at start stay per animation
    clipPut({ from, events: JSON.parse(JSON.stringify(evs)), at: 0 });
    toast(`Copied all ${evs.length} events of ${motionName(S.motion)} (not its parts hidden at the start). Ctrl+V pastes them at the same times; select an event first to copy just that one.`);
  }
}
function clipPaste(keepTime) {
  const c = clipGet(); if (!c || !c.events || !c.events.length) { toast('Nothing copied yet: select an event and press Ctrl+C.'); return; }
  if (S.readOnly) { readOnlyNote(); return; }
  const p = curTLPath(); if (!p) { toast('This animation has no timeline yet: create one in the inspector first.'); return; }
  const shift = keepTime || c.events.length > 1 ? 0 : ticks(S.frame) - c.at, t = F(p), added = [];
  edit([p], () => {
    for (const { kind, e } of c.events) {
      const n = JSON.parse(JSON.stringify(e));
      if (kind === 'ranged') { n.start = Math.max(0, n.start + shift); if (n.end !== FOREVER) n.end = Math.max(n.start + 1, n.end + shift); }
      else n.frame = Math.max(0, n.frame + shift);
      t[kind].push(n); added.push([kind, t[kind].length - 1]);
    }
  });
  derive(); runChecks();
  if (added.length === 1) selectEvent(added[0][0], added[0][1]); else { S.sel = null; renderAll(); }
  const bad = S.checks.filter((x) => x.motion === S.motion && x.level === 'err').length;
  toast(`Pasted ${added.length} event${added.length > 1 ? 's' : ''} from ${c.from}${bad ? `: ${bad} problem${bad > 1 ? 's' : ''} (hits, parts or projectiles this character doesn't have) are in the checks` : ''}.`);
}
document.addEventListener('keydown', (e) => {
  if (isTyping() || !S.moves) return;
  if (!(e.ctrlKey || e.metaKey) || e.altKey) return;
  const k = e.key.toLowerCase();
  if (k === 'c' && !(window.getSelection && String(window.getSelection()))) { e.preventDefault(); clipCopy(); }
  else if (k === 'v') { e.preventDefault(); clipPaste(e.shiftKey); }
});
