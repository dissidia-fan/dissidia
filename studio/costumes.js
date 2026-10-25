/* Move Studio: a mod character's costumes (Character tab > Costumes). Loaded after skins.js.

   The game asks for every character's costume models: obj/p_<code>_{1p,2p,3p,zako}.gmo (+ _a, the assist). Until
   you add one, a costume is the main model (mod.json files point its paths at the 1P files). "Add costume…" gives
   that costume model files of its own (copies of the main ones) and has Blender fit your model into them
   (own_model.py bake_costume: the body is in the files, so the costume isn't in body.models and no skin applies).
   Remove points it back at the main model; Save deletes its files. */
'use strict';
const CST_SLOTS = [['2p', 'Costume 2'], ['3p', 'Costume 3'], ['zako', 'Manikin']];
const CST = { job: null, c: null, state: '', message: '', timer: null };
const cstCode = () => ((S.mod && S.mod.character) || {}).code || '';
const cstGp = (c, a) => `obj/p_${cstCode()}_${c}${a ? '_a' : ''}.gmo`;
const cstSame = (x, y) => JSON.stringify(x) === JSON.stringify(y);
function cstOwn(c) {                                  // does the costume have model files of its own?
  const F = (S.mod && S.mod.files) || {};
  return F[cstGp(c)] !== undefined && !cstSame(F[cstGp(c)], F[cstGp('1p')]);
}
const cstSummary = () => { const own = CST_SLOTS.filter(([c]) => cstOwn(c)); return own.length ? own.map(([, l]) => l).join(', ') : 'none: every costume is the main model'; };

function costumePanel(I) {
  const busy = CST.job && (CST.state === 'picking' || CST.state === 'working' || CST.state === 'starting');
  const row = ([c, label]) => {
    const own = cstOwn(c), mine = CST.c === c && CST.state;
    return `<div class="cst-row"><div><b>${label}</b><small>${own ? 'its own model' : 'the main model'}</small></div>
      <span class="btnrow">${own ? `<button type="button" class="mini" data-cst-replace="${c}"${busy ? ' disabled' : ''}>Replace model…</button><button type="button" class="mini danger" data-cst-remove="${c}"${busy ? ' disabled' : ''}>Remove</button>`
        : `<button type="button" class="mini" data-cst-add="${c}"${busy ? ' disabled' : ''}>Add…</button>`}</span>
      ${mine ? `<p class="note cst-status${CST.state === 'error' ? ' bad' : ''}">${esc(cstText())}</p>` : ''}</div>`;
  };
  I.innerHTML = `<div class="sec"><h3>Costumes</h3>
    <p class="note">The game has three more costume slots. Until you add one it shows the main model; added, it gets a model of its own, fitted in Blender from your file.</p>
    <div class="cst-row cst-main"><div><b>Main</b><small>${esc(skinLabel(skinGameChoice()))}</small></div></div>
    ${CST_SLOTS.map(row).join('')}</div>`;
  I.querySelectorAll('[data-cst-add]').forEach((b) => b.onclick = () => cstStart(b.dataset.cstAdd, true));
  I.querySelectorAll('[data-cst-replace]').forEach((b) => b.onclick = () => cstStart(b.dataset.cstReplace, false));
  I.querySelectorAll('[data-cst-remove]').forEach((b) => b.onclick = () => cstRemove(b.dataset.cstRemove));
}
function cstText() {
  return { starting: 'Asking Blender…', picking: 'Blender is waiting: pick the model file there.', working: 'Blender: ' + CST.message,
    done: 'Done: ' + CST.message, error: CST.message, cancelled: 'No model picked: nothing changed.' }[CST.state] || '';
}
const cstRefresh = () => { if (S.tab === 'char' && S.ncSel === 'costumes') renderInspector(); };

/* point the costume back at the main model (Save removes its files) */
function cstPointAtMain(c) {
  const F = S.mod.files, b = S.mod.body = S.mod.body || {};
  for (const a of [false, true]) {
    const gp = cstGp(c, a), one = cstGp('1p', a);
    if (F[one] !== undefined) F[gp] = JSON.parse(JSON.stringify(F[one]));
    if ((b.models || []).includes(one) && !(b.models || []).includes(gp)) b.models = [...(b.models || []), gp];
  }
}
function cstRemove(c) {
  if (S.readOnly) { readOnlyNote(); return; }
  edit([S.modPath], () => cstPointAtMain(c));
  renderLeft(); renderInspector();
  toast(`${CST_SLOTS.find((x) => x[0] === c)[1]} is the main model again. Save removes its files.`, 'ok');
}

async function cstStart(c, add) {
  if (S.readOnly) { readOnlyNote(); return; }
  if (dirtyCount()) { toast('Save first: Blender works on the files in the folder.', 'bad'); return; }
  const F = S.mod.files || {}, rigRel = `models/p_${cstCode()}_${c}.gmo`, aRel = `models/p_${cstCode()}_${c}_a.gmo`;
  const main = F[cstGp('1p')], mainA = F[cstGp('1p', true)];
  if (typeof main !== 'string') { toast('The main model isn\'t a file in the folder, so a costume can\'t copy it.', 'bad'); return; }
  Object.assign(CST, { c, job: null, state: 'starting', message: '' }); cstRefresh();
  let wroteA = false;
  if (add) {                                         // the costume's own files: copies of the main ones, then mod.json
    try {
      await FS.write(S.charDir + rigRel, await FS.read(S.charDir + main));
      if (typeof mainA === 'string') { await FS.write(S.charDir + aRel, await FS.read(S.charDir + mainA)); wroteA = true; }
    } catch (e) { Object.assign(CST, { state: 'error', message: 'Could not copy the main model: ' + e.message }); cstRefresh(); return; }
    edit([S.modPath], () => {
      F[cstGp(c)] = rigRel; if (wroteA) F[cstGp(c, true)] = aRel;
      const b = S.mod.body = S.mod.body || {};
      b.models = (b.models || []).filter((g) => g !== cstGp(c) && g !== cstGp(c, true));   // its body is in its files
    });
    await save();
  }
  const q = new URLSearchParams({ mod: studioModName(), rig: rigRel, costume: c, assist: typeof F[cstGp(c, true)] === 'string' ? F[cstGp(c, true)] : '' });
  const undoAdd = async () => { if (!add) return; edit([S.modPath], () => cstPointAtMain(c)); await save(); };
  let j;
  try { j = await (await fetch('http://127.0.0.1:47612/own_model?' + q, { cache: 'no-store' })).json(); }
  catch (e) { await undoAdd(); Object.assign(CST, { state: 'error', message: 'Blender isn\'t answering. Open it with the Dissidia add-on (0.27+) and try again.' }); cstRefresh(); toast(CST.message, 'bad'); return; }
  if (!j.ok || !j.job) { await undoAdd(); Object.assign(CST, { state: 'error', message: 'Blender: ' + (j.message || 'could not start') }); cstRefresh(); return; }
  Object.assign(CST, { job: j.job, state: 'picking' }); cstRefresh();
  const t0 = Date.now(), char = S.charDir;
  clearInterval(CST.timer);
  CST.timer = setInterval(async () => {
    if (S.charDir !== char || Date.now() - t0 > 3600e3) { clearInterval(CST.timer); return; }
    let s; try { s = await (await fetch('http://127.0.0.1:47612/job?id=' + CST.job, { cache: 'no-store' })).json(); } catch (e) { return; }
    const was = CST.state; Object.assign(CST, { state: s.state, message: s.message || '' });
    if (s.state === 'done') {
      clearInterval(CST.timer);
      await pickChar(S.char === 'character' ? '' : S.charDir.replace(/\/$/, ''), S.charDir + rigRel);   // the costume's model, as written
      S.tab = 'char'; S.ncSel = 'costumes'; renderAll();
      toast(`${CST_SLOTS.find((x) => x[0] === c)[1]} has its own model now. Build to see it in the game.`, 'ok');
      return;
    }
    if (['error', 'cancelled', 'unknown'].includes(s.state)) { clearInterval(CST.timer); await undoAdd(); cstRefresh(); return; }
    if (was !== s.state) cstRefresh(); else { const st = document.querySelector('.cst-status'); if (st) st.textContent = cstText(); }
  }, 1000);
}
