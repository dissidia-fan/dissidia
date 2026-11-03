// Dissidia Move Studio: one place to see a move's animation, place its events and tune its hits, working straight on a
// character's mod folder (mods/<name>/moveset/p_<code>/). Files are read and written in the exact
// format tools/charmod_objx.py writes, so `charmod.py build` (and run.sh) pick the changes up.
'use strict';
const $ = (id) => document.getElementById(id);
const OPS = window.STUDIO_OPS;
const FOREVER = 0xFFFF;
const CAT_VAR = { 'Hits': '--c-hits', 'Combo & cancels': '--c-combo', 'Defence': '--c-defence', 'Movement': '--c-move',
  'Weapons & parts': '--c-parts', 'Effects & sound': '--c-fx', 'Camera & screen': '--c-cam', 'EX Burst': '--c-ex',
  'CPU': '--c-cpu', 'Other': '--c-other' };
// a category's colour; the sounds of Effects & sound get their own (request: "Red for hits, purple for effects, yellow for objects, blue for sounds")
const SOUND_OPS = new Set(['sound', 'voice', 'sound_if_part_visible', 'sound_if_form', 'footstep_sound']);
const evColVar = (info) => (info && info.cat === 'Effects & sound' && SOUND_OPS.has(info.name) ? '--c-sound' : CAT_VAR[info && info.cat] || '--c-other');
const hex4 = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
const BY_NAME = {};
for (const [k, v] of Object.entries(OPS.ops)) if (v.name) BY_NAME[v.name] = parseInt(k, 16);
const opCode = (op) => typeof op === 'number' ? op : op.startsWith('0x') ? parseInt(op, 16) : (BY_NAME[op] ?? NaN);
function opInfo(e, kind) {
  const c = opCode(e.op), h = hex4(c);
  return OPS.ops[h + ':' + kind] || OPS.ops[h] ||
    { op: h, name: null, label: isNaN(c) ? String(e.op) : h, cat: 'Other', args: [], nargs: c >> 8, desc: 'Not identified yet.', kinds: [kind], shape: {} };
}
const frames = (t) => t / 2;
const ticks = (f) => Math.round(f * 2);
const fmtF = (t) => t === FOREVER ? 'end' : (t % 2 ? (t / 2).toFixed(1) : String(t / 2));
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function toast(msg, kind) {                         // kind: 'bad' (an alert: red, stays longer) or 'ok'
  const t = $('toast');
  t.className = 'toast' + (kind ? ' ' + kind : '');
  t.innerHTML = (kind === 'bad' ? '<i class="ic ic-circle-alert" aria-hidden="true"></i>' : kind === 'ok' ? '<i class="ic ic-circle-check" aria-hidden="true"></i>' : '') + '<span></span>';
  t.lastChild.textContent = msg; t.hidden = false; t.setAttribute('role', kind === 'bad' ? 'alert' : 'status');
  clearTimeout(toast.h); toast.h = setTimeout(() => t.hidden = true, kind === 'bad' ? 7000 : 4200);
}

/* in-page dialogs instead of the browser's prompt() / confirm(): they look like the studio, take Enter / Esc, and can
   ask several things at once. uiDialog resolves to the field values (or true with no fields), or null when cancelled. */
function uiDialog({ title, body = '', fields = [], ok = 'OK', cancel = 'Cancel', danger = false }) {
  return new Promise((resolve) => {
    const back = document.createElement('div'); back.className = 'modal-back';
    const field = (f, i) => `<div class="field"><label for="uiF${i}">${esc(f.label)}</label>${f.options
      ? `<select id="uiF${i}">${f.options.map(([v, l]) => `<option value="${esc(v)}"${String(v) === String(f.value) ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>`
      : `<input id="uiF${i}" type="${f.type || 'text'}" value="${esc(f.value ?? '')}" autocomplete="off"${f.placeholder ? ` placeholder="${esc(f.placeholder)}"` : ''}${f.min != null ? ` min="${f.min}"` : ''}${f.step != null ? ` step="${f.step}"` : ''}>`}${f.hint ? `<small class="ui-hint">${esc(f.hint)}</small>` : ''}</div>`;
    back.innerHTML = `<form class="modal ui-dlg" role="dialog" aria-modal="true" aria-label="${esc(title)}" novalidate><h3>${esc(title)}</h3>
      ${body ? `<p class="note ui-dlg-body">${esc(body)}</p>` : ''}${fields.length > 1 && fields.every((f) => f.half) ? `<div class="row2">${fields.map(field).join('')}</div>` : fields.map(field).join('')}
      <p class="warnline ui-dlg-err" role="alert" hidden></p>
      <div class="btnrow ui-dlg-btns"><button type="button" class="ui-cancel">${esc(cancel)}</button><button type="submit" class="${danger ? 'danger' : 'primary'}">${esc(ok)}</button></div></form>`;
    document.body.appendChild(back);
    const form = back.firstElementChild, prev = document.activeElement;
    const done = (v) => { back.remove(); document.removeEventListener('keydown', onKey, true); if (prev && prev.focus) prev.focus(); resolve(v); };
    const onKey = (e) => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); done(null); } };
    document.addEventListener('keydown', onKey, true);
    back.addEventListener('pointerdown', (e) => { if (e.target === back) done(null); });
    form.querySelector('.ui-cancel').onclick = () => done(null);
    form.onsubmit = (e) => {
      e.preventDefault();
      const vals = fields.map((f, i) => form.querySelector('#uiF' + i).value);
      for (let i = 0; i < fields.length; i++) {
        const msg = fields[i].check && fields[i].check(vals[i], vals);
        if (msg) { const er = form.querySelector('.ui-dlg-err'); er.textContent = msg; er.hidden = false; form.querySelector('#uiF' + i).focus(); return; }
      }
      done(fields.length ? vals : true);
    };
    const first = form.querySelector('input,select');
    if (first) { first.focus(); if (first.select) first.select(); } else form.querySelector('[type=submit]').focus();
  });
}
async function uiAsk(title, label, value = '', o = {}) {             // one text answer, or null
  const r = await uiDialog({ title, body: o.body, ok: o.ok || 'OK', fields: [{ label, value, placeholder: o.placeholder, hint: o.hint, check: o.check }] });
  return r ? r[0] : null;
}
const uiConfirm = async (title, body, o = {}) => !!(await uiDialog({ title, body, ok: o.ok || 'OK', cancel: o.cancel || 'Cancel', danger: !!o.danger }));
const uiDiscard = (what) => uiConfirm('Discard unsaved changes?', `${dirtyCount()} file${dirtyCount() > 1 ? 's have' : ' has'} changes that aren't saved. ${what || 'They'} will be lost.`, { ok: 'Discard', cancel: 'Keep editing', danger: true });

/* ====================================================== JSON in the exact format charmod_objx.wj writes */
const FLOAT_KEYS = new Set(['kb_yaw', 'kb_forward', 'kb_up', 'stun', 'follow']);
function pyFloat(x) {
  if (!isFinite(x)) return isNaN(x) ? 'NaN' : (x > 0 ? 'Infinity' : '-Infinity');
  if (Object.is(x, -0)) return '-0.0';
  const m = x.toExponential().match(/^(-?)(\d)(?:\.(\d+))?e([+-]\d+)$/);
  const sign = m[1], d = m[2] + (m[3] || ''), e = parseInt(m[4], 10);
  if (e < -4 || e >= 16) return sign + d[0] + (d.length > 1 ? '.' + d.slice(1) : '') + 'e' + (e < 0 ? '-' : '+') + String(Math.abs(e)).padStart(2, '0');
  if (e >= 0) return sign + (d.length <= e + 1 ? d + '0'.repeat(e + 1 - d.length) + '.0' : d.slice(0, e + 1) + '.' + d.slice(e + 1));
  return sign + '0.' + '0'.repeat(-e - 1) + d;
}
function pyDumps(v, key) {
  if (v === null || v === undefined) return 'null';
  if (v === true) return 'true';
  if (v === false) return 'false';
  if (typeof v === 'number') return (key === 'f' || FLOAT_KEYS.has(key) || !Number.isInteger(v)) ? pyFloat(v) : String(v);
  if (typeof v === 'string') return JSON.stringify(v);
  if (Array.isArray(v)) return '[' + v.map((x) => pyDumps(x)).join(', ') + ']';
  return '{' + Object.entries(v).map(([k, x]) => JSON.stringify(k) + ': ' + pyDumps(x, k)).join(', ') + '}';
}
/* mod.json (charmod.py writes it with json.dump(indent=1)): only the top-level values that changed are rewritten, so
   the rest keeps its exact text (Python's 1.0 floats included) */
function jsonTopSpans(t) {                       // {key: [value start, value end]} of a JSON object's top-level members
  const out = {}; let depth = 0, key = null, vs = -1, expectKey = false;
  const end = (i) => { while (i > vs && /\s/.test(t[i - 1])) i--; return i; };
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (c === '"') {
      let j = i + 1; while (t[j] !== '"') j += t[j] === '\\' ? 2 : 1;
      if (depth === 1 && expectKey) { key = JSON.parse(t.slice(i, j + 1)); expectKey = false; }
      i = j; continue;
    }
    if (c === '{' || c === '[') { if (depth === 0) expectKey = true; depth++; continue; }
    if (c === '}' || c === ']') { depth--; if (depth === 0 && key !== null && vs >= 0) out[key] = [vs, end(i)]; continue; }
    if (depth !== 1) continue;
    if (c === ':' && key !== null && vs < 0) { vs = i + 1; while (t[vs] === ' ') vs++; }
    else if (c === ',') { out[key] = [vs, end(i)]; key = null; vs = -1; expectKey = true; }
  }
  return out;
}
function modJsonText(f) {
  const spans = jsonTopSpans(f.text), edits = [], adds = [];
  for (const [k, v] of Object.entries(f.obj)) {
    if (JSON.stringify(v) === JSON.stringify(f.orig[k])) continue;
    const txt = JSON.stringify(v, null, 1).replace(/\n/g, '\n ');
    if (spans[k]) edits.push([spans[k][0], spans[k][1], txt]); else adds.push(`,\n ${JSON.stringify(k)}: ${txt}`);
  }
  let t = f.text;
  for (const [a, b, txt] of edits.sort((x, y) => y[0] - x[0])) t = t.slice(0, a) + txt + t.slice(b);
  if (adds.length) { const z = t.lastIndexOf('}'); let y = z; while (/\s/.test(t[y - 1])) y--; t = t.slice(0, y) + adds.join('') + t.slice(y); }
  return t;
}
const fileText = (f) => f.mod ? modJsonText(f) : wjText(f.obj);
const sameU8 = (a, b) => { if (a === b) return true; if (!a || !b || a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; };
const fileChanged = (f) => f.dirty && (f.bin ? !(f.binOrig && sameU8(f.bin, f.binOrig)) : fileText(f) !== f.text);
function wjText(obj) {
  const isRec = (x) => x && typeof x === 'object' && !Array.isArray(x);
  const parts = Object.entries(obj).map(([k, v]) => (Array.isArray(v) && v.length && v.every(isRec))
    ? ' ' + JSON.stringify(k) + ': [\n' + v.map((x) => '  ' + pyDumps(x)).join(',\n') + '\n ]'
    : ' ' + JSON.stringify(k) + ': ' + pyDumps(v, k));
  return '{\n' + parts.join(',\n') + '\n}\n';
}

/* ====================================================== files: a picked folder (Chrome) or files in memory */
const FS = {
  kind: null, root: null, mem: null, base: '',
  join(p) { return (this.base ? this.base + '/' : '') + p; },
  async _dir(parts, create = false) { let d = this.root; for (const p of parts) if (p) d = await d.getDirectoryHandle(p, { create }); return d; },
  async read(path) {
    path = this.join(path);
    if (this.kind === 'mem') {
      const v = this.mem.get(path);
      if (v == null) throw new Error('missing ' + path);
      if (typeof v === 'string') return new TextEncoder().encode(v);
      if (v instanceof Uint8Array) return v;
      return new Uint8Array(await v.arrayBuffer());
    }
    const ps = path.split('/');
    const d = await this._dir(ps.slice(0, -1));
    return new Uint8Array(await (await (await d.getFileHandle(ps[ps.length - 1])).getFile()).arrayBuffer());
  },
  async text(path) { return new TextDecoder().decode(await this.read(path)); },
  async json(path) { return JSON.parse(await this.text(path)); },
  async exists(path) {
    const full = this.join(path);
    if (this.kind === 'mem') return this.mem.has(full) || [...this.mem.keys()].some((k) => k.startsWith(full + '/'));
    const ps = full.split('/');
    try {
      const d = await this._dir(ps.slice(0, -1));
      try { await d.getFileHandle(ps[ps.length - 1]); return true; } catch (e) { await d.getDirectoryHandle(ps[ps.length - 1]); return true; }
    } catch (e) { return false; }
  },
  async list(dir) {
    const full = this.join(dir);
    if (this.kind === 'mem') {
      const pre = full ? full + '/' : '', out = new Map();
      for (const k of this.mem.keys()) if (k.startsWith(pre)) { const rest = k.slice(pre.length), i = rest.indexOf('/'); out.set(i < 0 ? rest : rest.slice(0, i), i < 0 ? 'file' : 'directory'); }
      return [...out].map(([name, kind]) => ({ name, kind })).sort((a, b) => a.name.localeCompare(b.name));
    }
    const d = await this._dir(full.split('/'));
    const out = [];
    for await (const [name, h] of d.entries()) out.push({ name, kind: h.kind });
    return out.sort((a, b) => a.name.localeCompare(b.name));
  },
  async remove(path) {
    const full = this.join(path);
    if (this.kind === 'mem') { this.mem.delete(full); return; }
    const ps = full.split('/');
    await (await this._dir(ps.slice(0, -1))).removeEntry(ps[ps.length - 1]);
  },
  async write(path, text) {
    const full = this.join(path);
    if (this.kind === 'mem') { this.mem.set(full, text); this.written.set(full, text); return; }
    const ps = full.split('/');
    const d = await this._dir(ps.slice(0, -1), true);
    const w = await (await d.getFileHandle(ps[ps.length - 1], { create: true })).createWritable();
    await w.write(text); await w.close();
  },
  written: new Map(),
};

/* ====================================================== state */
const S = {
  chars: [], char: null, mod: null, rigs: [], rig: null, msDir: '', files: {}, tlPaths: [], gmo: null,
  motion: -1, frame: 0, playing: false, sel: null, step: null, tab: 'moves', undo: [], checks: [], showChecks: false,
};
const F = (p) => S.files[p] && S.files[p].obj;
const msPath = (p) => S.msDir + '/' + p;

function markDirty(paths) { for (const p of paths) if (S.files[p]) S.files[p].dirty = true; refreshStatus(); }
/* Undo / redo. An entry is the state of the files an edit touched, from before it: [{p, text | bin, dirty, f}] (f: the
   file's record, to put back a file that was dropped since), or [{effe, dirty}] for the effect package. Undoing an
   entry pushes the state it replaces onto the other stack, so redo is the same step the other way. A new edit clears
   the redo stack. The save marker: a save sets dirty on every entry of the files it wrote, so stepping back past a
   save leaves the file changed (fileChanged then compares with what was saved). */
const undoRec = (p, f) => (f ? { p, text: f.bin ? null : JSON.stringify(f.obj), bin: f.bin ? f.bin.slice() : null, dirty: f.dirty, f } : { p, gone: true });
function pushUndo(paths) {
  S.undo.push(paths.map((p) => undoRec(p, S.files[p])));
  if (S.undo.length > 200) S.undo.shift();
  undoPushed();
}
function undoPushed() { S.redo = []; undoBtns(); }   // a new edit: nothing to redo
function undoBtns() {
  $('undoBtn').disabled = !S.undo.length; $('redoBtn').disabled = !(S.redo && S.redo.length);
  $('undoBtn').title = 'Undo (Ctrl+Z)'; $('redoBtn').title = 'Redo (Ctrl+Shift+Z)';   // what it does: on hover (undoTitle)
}
function undoStep(from, to) {
  const u = from.pop(); if (!u) return;
  const inv = u.map((x) => (x.effe ? { effe: S.effe.bytes, dirty: S.effe.dirty } : undoRec(x.p, S.files[x.p]))), before = { ...S.files };
  S.gen = (S.gen || 0) + 1;
  for (const x of u) {
    if (x.effe) { fxuUndoEntry(x); continue; }
    if (x.gone) { delete S.files[x.p]; continue; }
    if (!S.files[x.p] && x.f) S.files[x.p] = x.f;
    if (x.bin) S.files[x.p].bin = x.bin; else S.files[x.p].obj = JSON.parse(x.text);
    S.files[x.p].dirty = x.dirty;
  }
  if (S.modPath && S.files[S.modPath]) S.mod = S.files[S.modPath].obj;
  if (typeof cpAfterUndo === 'function') cpAfterUndo(u);
  for (const [p, f] of Object.entries(before)) if (!S.files[p] && !inv.some((y) => y.p === p)) inv.push(undoRec(p, f));   // dropped on the way (a copied timeline): back with the step back
  to.push(inv); if (to.length > 200) to.shift();
  undoBtns();
  derive(); if (S.fxEdit) fxRefresh(); renderAll(); refreshStatus();
}
function undo() { undoStep(S.undo, S.redo || (S.redo = [])); }
function redo() { if (S.redo && S.redo.length) undoStep(S.redo, S.undo); }
function undoSaved(paths, effe) {               // the save marker (above)
  const ps = new Set(paths);
  for (const u of [...S.undo, ...(S.redo || [])]) for (const x of u) if (x.effe ? effe : ps.has(x.p)) x.dirty = true;
}
/* the Undo / Redo buttons' tooltips say what they would do, in the Changes panel's words (checks.js ckFileDiff):
   "Undo: Power 8 → 12 (BR_L_RUSH2)" */
function undoDesc(u, isRedo) {
  const out = [];
  for (const x of u) {
    if (x.effe) { out.push('Effect package edit'); continue; }
    const f = S.files[x.p] || x.f, name = x.p.slice((S.charDir || '').length);
    if (x.gone || !f) { out.push((isRedo ? 'Remove ' : 'Add ') + name); continue; }
    if (typeof ckFileDiff !== 'function') continue;
    let d;
    try {
      const fl = { vols: f.vols, brat: f.brat, snd: f.snd, text_: f.text_, gmo: f.gmo };
      d = f.bin ? ckFileDiff(x.p, isRedo ? { ...fl, bin: x.bin, binOrig: f.bin } : { ...fl, bin: f.bin, binOrig: x.bin })
        : ckFileDiff(x.p, isRedo ? { mod: f.mod, text: JSON.stringify(f.obj), obj: JSON.parse(x.text) } : { mod: f.mod, text: x.text, obj: f.obj });
    } catch (e) { continue; }
    let lines = d.lines.filter((l) => l !== 'reformatted only');
    if (f.bin && lines.length > 1) lines = lines.slice(1);           // "n → m bytes" only when it is all there is
    const ev = !f.bin && f.obj && f.obj.ranged ? lines.shift() : null;     // a timeline: its animation first
    const add = lines.filter((l) => l.startsWith('+ ')).map((l) => l.slice(2)), del = lines.filter((l) => l.startsWith('− ')).map((l) => l.slice(2));
    const at = (l) => l.replace(/ (@?[\d.]+(–([\d.]+|end))?)$/, ''), when = (l) => (l.match(/ (@?[\d.]+(–([\d.]+|end))?)$/) || [, ''])[1];
    if (ev && add.length === 1 && del.length === 1 && at(add[0]) === at(del[0])) out.push(when(add[0]) === when(del[0]) ? `${at(add[0])} ${when(add[0])} (${ev})` : `${at(add[0])} ${when(del[0])} → ${when(add[0])} (${ev})`);
    else if (ev) out.push(...add.map((l) => `Add ${l} (${ev})`), ...del.map((l) => `Remove ${l} (${ev})`));
    else for (const l of lines) {
      const m = l.match(/^~ (.+?): (.+)$/);
      out.push(m ? `${m[2]} (${m[1].replace(/^hit /, '')})` : l.startsWith('+ ') ? 'Add ' + l.slice(2) : l.startsWith('− ') ? 'Remove ' + l.slice(2) : l.replace(/^~ /, ''));
    }
  }
  const s = out.slice(0, 2).join('; ') + (out.length > 2 ? ' …' : '');
  return s ? s[0].toUpperCase() + s.slice(1) : '';
}
function undoTitle(isRedo) {
  const st = isRedo ? S.redo : S.undo, u = st && st[st.length - 1], key = isRedo ? 'Ctrl+Shift+Z' : 'Ctrl+Z';
  const d = u ? undoDesc(u, isRedo) : '';
  return `${isRedo ? 'Redo' : 'Undo'}${d ? ': ' + d : ''} (${key})`;
}
function edit(paths, fn) {
  if (S.readOnly) { readOnlyNote(); return; }
  pushUndo(paths); fn(); S.gen = (S.gen || 0) + 1; markDirty(paths); runChecks();
}
for (const [id, r] of [['undoBtn', false], ['redoBtn', true]]) for (const ev of ['pointerenter', 'focus']) $(id).addEventListener(ev, () => { $(id).title = undoTitle(r); });
function readOnlyNote() {
  const m = retailModOf(S.rig.retail.code);
  toast(`This is the original ${S.mod.character.name} from the ISO, for comparing. Your changed ${S.mod.character.name} is mods/${m.name}: pick it under Mod characters.`);
}
const retailModOf = (code) => S.chars.find((c) => c.kind === 'retail' && c.code === code);
const slugOf = (name) => name.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'character';
function dirtyCount() { return Object.values(S.files).filter(fileChanged).length + (S.effe && S.effe.dirty ? 1 : 0); }
function refreshStatus() {
  const n = dirtyCount(), st = $('status');
  const retail = S.rig && S.rig.retail;
  // Only what needs attention: unsaved changes, or that this is the read-only original.
  st.textContent = !S.char ? '' : S.readOnly ? 'Original (read-only)' : n ? n + ' file' + (n > 1 ? 's' : '') + ' changed' : '';
  st.title = S.readOnly ? `The retail ${S.mod.character.name} from ${ISO.name}; your changed one is under Mod characters` : n && retail ? `Save makes mods/${slugOf(S.mod.character.name)}` : n ? 'See what Save will write' : '';
  st.classList.toggle('dirty', n > 0);
  st.hidden = !st.textContent; st.disabled = !n;   // a button (opens the Changes panel, checks.js), shown as a badge
  $('saveBtn').disabled = !n && !(retail && !S.readOnly);   // an unchanged retail character can still become a mod (to swap its body)
  $('saveBtn').textContent = retail && !S.readOnly ? 'Save as mod' : 'Save';
  $('saveBtn').title = retail && !S.readOnly ? `Save into mods/${slugOf(S.mod.character.name)}: built into the game, it replaces the retail ${S.mod.character.name} (Ctrl+S)` : 'Save into the mod folder (Ctrl+S)';
}

/* ====================================================== opening */
const MOVESET_OF = (stem) => stem.replace(/_(\d+p|zako)(_a|_w)?$/, '$2');
async function openFolder() {
  if (window.showDirectoryPicker) {
    let h;
    try { h = await window.showDirectoryPicker({ id: 'dissidia-mods', mode: 'readwrite' }); }
    catch (e) { return; }
    return openModsHandle(h);
  } else $('dirInput').click();
}
/* the mods folder picked last time comes back after a reload: by itself when Chrome still allows it (it can offer
   "Allow on every visit"), otherwise with one click on "Use <folder> again" (a page can't read a folder it wasn't given) */
async function openModsHandle(h) {
  FS.root = h; FS.kind = 'handle'; FS.base = ''; FS.readonly = false;
  IDB.set('mods', h);
  $('modsReopen').hidden = true;
  await findChars();
}
async function reopenMods() {
  const h = await IDB.get('mods');
  if (!h) return openFolder();
  try { if ((await h.requestPermission({ mode: 'readwrite' })) !== 'granted') return; return openModsHandle(h); }
  catch (e) { return openFolder(); }
}
IDB.get('mods').then(async (h) => {
  if (!h || FS.root) return;
  try { if ((await h.queryPermission({ mode: 'readwrite' })) === 'granted') return openModsHandle(h); } catch (e) { return; }
  $('modsReopen').hidden = false; $('modsReopen').textContent = 'Use ' + h.name + ' again';
});
async function openFromInput(fileList) {
  const mem = new Map();
  for (const f of fileList) mem.set(f.webkitRelativePath.split('/').slice(1).join('/'), f);
  FS.kind = 'mem'; FS.mem = mem; FS.base = ''; FS.readonly = true;
  toast('This browser can only read folders: Save will download the changed files instead. Chrome or Edge can save in place.');
  await findChars();
}
window.studioOpenMem = async (obj) => {          // tests: {relative path: text | Uint8Array}
  FS.kind = 'mem'; FS.mem = new Map(Object.entries(obj)); FS.base = ''; FS.readonly = false; FS.written = new Map();
  await findChars();
};
const MODS_CANDS = ['', 'mods', 'dissidia-recomp/mods', 'games/dissidia/mods', 'psprecomp/games/dissidia/mods', 'dissidia-recomp/psprecomp/games/dissidia/mods'];   // the old places still work
async function scanMods(cand) {                   // the character folders of a mods folder, or null
  FS.base = '';
  if (cand && !(await FS.exists(cand))) return null;
  let list; try { list = await FS.list(cand); } catch (e) { return null; }
  const chars = [];
  for (const it of list) {
    if (it.kind !== 'directory' || it.name.startsWith('_') || it.name.startsWith('.')) continue;
    const mj = (cand ? cand + '/' : '') + it.name + '/mod.json';
    if (!(await FS.exists(mj))) continue;
    let m = {}; try { m = await FS.json(mj); } catch (e) { /* keep */ }
    if (m.enabled === false) continue;
    const c = m.character || {};
    chars.push({ name: it.name, label: (c.name || it.name) + (m.kind === 'retail' ? ' (Modded)' : ''), kind: m.kind || 'extra', code: c.code });
  }
  const isMods = chars.length || list.some((it) => it.name === '_template');
  if (!isMods) return null;
  FS.base = cand; S.modsBase = cand; S.chars = chars;
  return chars;
}
function markMods() {                            // header button: a tick once a mods folder is open
  const on = !!FS.kind;
  setLoc(['openBtn', 'openBtn2'], on, 'mods folder', on ? (FS.root && FS.root.name ? FS.root.name : 'mods folder') + ': ' + S.chars.length + ' character' + (S.chars.length === 1 ? '' : 's') + '. Click to set another folder.' : null);
}
// The two locations (mods folder, game ISO) are settings: "Set … …" until given, then the name with a tick.
const LOCS = { mods: null, iso: null };            // what each location holds (a line for the popover), null = not set
function setLoc(ids, on, what, title) {
  for (const id of ids) {
    const b = $(id); if (!b) continue;
    if (id === 'openBtn' || id === 'isoBtn') { b.textContent = on ? 'Change…' : 'Set…'; continue; }   // in the Locations popover
    b.classList.toggle('set', on);
    b.innerHTML = on ? `${what[0].toUpperCase() + what.slice(1)} <b>✓</b>` : `Set ${what}…`;
    if (title) b.title = title;
  }
  LOCS[what === 'game ISO' ? 'iso' : 'mods'] = on ? (title || '').replace(/ Click to set another (folder|ISO)\.$/, '') : null;
  locRefresh();
}
function locRefresh() {                            // the Locations button and its popover
  const btn = $('locBtn'); if (!btn) return;
  const m = !!LOCS.mods, i = !!LOCS.iso;
  btn.innerHTML = !m && !i ? 'Set locations…' : `<span class="ld${m ? ' on' : ''}">Mods</span><span class="ld${i ? ' on' : ''}">ISO</span>`;
  btn.title = [m ? 'Mods folder: ' + LOCS.mods : 'No mods folder', i ? 'Game ISO: ' + LOCS.iso : 'No game ISO'].join('\n');
  const rows = document.querySelectorAll('#locPop .loc-row');
  if (rows[0]) rows[0].classList.toggle('on', m); if (rows[1]) rows[1].classList.toggle('on', i);
  $('locModsInfo').textContent = m ? LOCS.mods : 'Your characters (the repo\'s mods folder). Save writes into them.';
  $('locIsoInfo').textContent = i ? LOCS.iso : 'Every retail fighter and assist, read in place, never changed.';
  $('locMsg').innerHTML = m && i ? 'All set. Edit your characters and <b>Save</b>; change a retail one and <b>Save as mod</b> to add it to your mods folder. Copying moves and effects can take from either.'
    : m ? 'Editing your characters. Add the ISO to list the retail cast, copy moves and effects from it, and check pictures and logos against the game.'
    : i ? 'Looking at the retail cast, read-only. Set the mods folder to keep changes: <b>Save as mod</b> puts a changed character there.'
    : 'Set at least one. With both, a changed retail character can be saved as a mod.';
}
(function locWire() {
  const btn = $('locBtn'), pop = $('locPop'); if (!btn || !pop) return;
  let away = null;
  const esc = (e) => { if (e.key === 'Escape') { close(); btn.focus(); } };
  const close = () => { pop.hidden = true; btn.setAttribute('aria-expanded', 'false'); document.removeEventListener('keydown', esc); if (away) { document.removeEventListener('pointerdown', away); away = null; } };
  btn.onclick = () => {
    if (!pop.hidden) { close(); return; }
    pop.hidden = false; btn.setAttribute('aria-expanded', 'true'); document.addEventListener('keydown', esc);
    setTimeout(() => document.addEventListener('pointerdown', away = (e) => { if (!pop.contains(e.target) && !btn.contains(e.target)) close(); }), 0);
  };
  pop.addEventListener('click', (e) => { if (e.target.closest('#openBtn, #isoBtn')) close(); });
  locRefresh();
})();
async function findChars() {
  await findChars0();
  markMods();
}
// Opening a folder lists its characters; it only switches the view when nothing is open yet.
async function findChars0() {
  const viewing = !!S.moves;
  const keep = () => { fillCharPick(); $('charPick').value = charPickValue(); if (S.rig && S.rig.retail && !dirtyCount() && retailModOf(S.rig.retail.code)) { S.readOnly = true; document.body.classList.add('ro'); refreshStatus(); renderInspector(); } };
  S.modsBase = undefined;
  let found = null;
  for (const cand of MODS_CANDS) {
    FS.base = '';
    if (!(await FS.exists(cand || '.')) && cand) continue;
    if (await FS.exists((cand ? cand + '/' : '') + 'mod.json')) {           // one character's folder
      FS.base = cand; S.chars = [{ name: '', label: 'this folder' }];
      if (viewing) return keep();
      fillCharPick();
      return pickChar('');
    }
    const chars = await scanMods(cand);
    if (chars && !chars.length && found === null) found = cand;
    if (chars && chars.length) {
      if (viewing) return keep();
      fillCharPick();
      const last = (() => { try { return localStorage.getItem('studio.char'); } catch (e) { return null; } })();
      const start = S.chars.find((c) => c.name === last) ? last : S.chars[0].name;
      $('charPick').value = start;
      return pickChar(start);
    }
  }
  if (found !== null) { FS.base = S.modsBase = found; S.chars = []; if (viewing) return keep(); fillCharPick(); toast('No characters in this mods folder yet. Set the game ISO, change a retail character and Save to make one.'); return; }
  toast('No character mod folders here. Pick the repo\'s mods folder.');
}
function fillCharPick() {
  const mods = S.chars.map((c) => `<option value="${esc(c.name)}">${esc(c.label)}</option>`).join('');
  const retail = S.retailChars.map((c) => `<option value="iso:${c.code}">${esc(c.label)}${retailModOf(c.code) ? ' (original)' : ''}</option>`).join('');
  $('charPick').innerHTML = (S.chars.length ? (retail ? `<optgroup label="Mod characters">${mods}</optgroup>` : mods) : '') +
    (retail ? `<optgroup label="From the game ISO">${retail}</optgroup>` : '') +
    (retail && S.modsBase !== undefined ? '<option value="new:">+ New character…</option>' : '');
  $('charSel').hidden = !(S.chars.length || S.retailChars.length);
}
const charPickValue = () => S.rig && S.rig.retail ? 'iso:' + S.rig.retail.code : S.char;
async function onCharPick(v) {
  if (dirtyCount() && !(await uiDiscard())) { $('charPick').value = charPickValue(); return; }
  for (const f of Object.values(S.files)) f.dirty = false;
  if (v === 'new:') { $('charPick').value = charPickValue(); return ncStart(); }
  return v.startsWith('iso:') ? pickRetail(v.slice(4)) : pickChar(v);
}
async function pickChar(name, rigPath) {
  if (dirtyCount() && !(await uiDiscard())) { $('charPick').value = charPickValue(); return; }
  const dir = name ? name + '/' : '';
  S.char = name || 'character'; S.charDir = dir;
  try { localStorage.setItem('studio.char', name); } catch (e) { /* private window */ }
  S.mod = await FS.json(dir + 'mod.json');
  S.rigs = [];
  for (const it of await FS.list(dir + 'models').catch(() => [])) {
    if (!it.name.endsWith('.gmo')) continue;
    const ms = MOVESET_OF(it.name.slice(0, -4));
    if (await FS.exists(dir + 'moveset/' + ms + '/arc.json')) S.rigs.push({ gmo: dir + 'models/' + it.name, ms: dir + 'moveset/' + ms, label: it.name.slice(0, -4) });
  }
  if (!S.rigs.length) { toast(name + ' has no rig with a moveset (models/*.gmo + moveset/).'); return; }
  const rp = $('rigPick');
  rp.innerHTML = S.rigs.map((r, i) => `<option value="${i}">${esc(r.label)}${/_a$/.test(r.label) ? ' (assist)' : ''}</option>`).join('');
  const first = Math.max(0, S.rigs.findIndex((r) => /_1p$/.test(r.label))), at = rigPath ? S.rigs.findIndex((r) => r.gmo === rigPath) : -1;
  const ri = at >= 0 ? at : first;                // (a rig that is gone: the fighter's)
  rp.value = String(ri);
  await loadRig(S.rigs[ri]);
}
async function loadRig(rig) {
  if (rig.retail) return loadRetailRig(rig);
  S.readOnly = false; document.body.classList.remove('ro');
  S.rig = rig; S.msDir = rig.ms; S.files = {}; S.undo = []; S.redo = []; S.sel = null; S.step = null; S.newMove = null; S.whole = null; S.comp = null; S.comps = []; undoBtns();
  S.modPath = null;
  try {                                             // mod.json: the move names, brave / HP, ground / midair, new abilities
    const p = S.charDir + 'mod.json', text = await FS.text(p);
    S.files[p] = { obj: JSON.parse(text), orig: JSON.parse(text), text, dirty: false, mod: true };
    S.modPath = p; S.mod = S.files[p].obj;
  } catch (e) { console.warn('studio: mod.json not editable', e); }
  const load = async (p, optional) => {
    try { const text = await FS.text(msPath(p)); S.files[msPath(p)] = { obj: JSON.parse(text), text, dirty: false }; }
    catch (e) { if (!optional) throw e; }
  };
  for (const p of ['moves.json', 'actions.json', 'motion_map.json', 'hits.json', 'timelines/index.json']) await load(p);
  for (const p of ['bones.json', 'effects.json', 'projectiles.json']) await load(p, true);
  S.tlPaths = [];
  for (const fn of F(msPath('timelines/index.json')).files) if (fn.endsWith('.json')) { await load(fn); S.tlPaths.push(msPath(fn)); }
  const bad = Object.entries(S.files).filter(([p, f]) => !f.mod && !f.bin && !/(bones|projectiles|effects)\.json$/.test(p) && wjText(f.obj) !== f.text).map(([p]) => p.split('/').pop());
  if (bad.length) console.warn('studio: these files will be reformatted on save:', bad);
  try { S.gmo = parseGMO(await FS.read(rig.gmo)); } catch (e) { S.gmo = null; toast('Could not read the model: ' + e.message, 'bad'); }
  S.fx = null; S.effe = null; S.fxEdit = null;
  try {
    const arc = await FS.json(msPath('arc.json')), ent = arc.entries.find((x) => x.tag === '65666665');
    const tx = arc.entries.find((x) => x.tag === '74657874');         // the 'text' section (HP-move banners)
    if (tx) S.files[msPath(tx.file)] = { bin: new Uint8Array(await FS.read(msPath(tx.file))), dirty: false, text_: true };
    const sd = arc.entries.find((x) => x.type === 14);                // the sound bank
    if (sd) S.files[msPath(sd.file)] = { bin: new Uint8Array(await FS.read(msPath(sd.file))), dirty: false, snd: true };
    const hv = arc.entries.find((x) => x.type === 5);                 // the hit volumes (hitboxes)
    if (hv) S.files[msPath(hv.file)] = { bin: new Uint8Array(await FS.read(msPath(hv.file))), dirty: false, vols: true };
    const br = arc.entries.find((x) => x.tag === '62726174');          // brat: which commands the CPU uses, and when
    if (br) S.files[msPath(br.file)] = { bin: new Uint8Array(await FS.read(msPath(br.file))), dirty: false, brat: true };
    if (ent) { fxTexCache.clear(); FXHD.map = null; fxGeoCache.clear();          // decoded textures / flipbooks are cached by resource number: drop them, the file may have changed outside
      const bytes = await FS.read(msPath(ent.file)); S.effe = { path: msPath(ent.file), rel: ent.file, bytes, orig: bytes, dirty: false, pkg: null }; S.fx = loadEffects(bytes); }
    for (const f of Object.values(S.files)) if (f.bin) f.binOrig = f.bin;
    // projectiles: projectiles.json, or (when the unpack kept the section raw, as for Ramza on Vaan) the magi section itself
    const pj = F(msPath('projectiles.json')), mg = arc.entries.find((x) => x.tag === '6d616769' && /\.bin$/.test(x.file));
    if (S.fx) S.fx.magi = pj || !mg ? magiFromJson(pj) : parseMagi(new Uint8Array(await FS.read(msPath(mg.file))));
  } catch (e) { console.warn('studio: no effects', e); }
  showRig();
  await skinSetup();                                // the body and weapons the game build puts on (skins.js)
  if (typeof compLoad === 'function') await compLoad();   // the effects made of layers (comp.js)
}
function showRig() {
  derive();
  if (typeof dmRigChanged === 'function') dmRigChanged();
  if (S.gmo) { buildCharacter(S.gmo, view); view.bonesHelper.visible = $('showBones').checked; }
  $('empty').hidden = true; $('hud').hidden = false; $('reloadBtn').disabled = S.readOnly; $('addBtn').disabled = S.readOnly; $('playBtn').disabled = false;
  const first = S.moves.find((m) => m.steps.length);
  setMotion(first ? first.steps[0].motion : 0);
  if (!S.readOnly && typeof mvSyncBanners === 'function') { const n = mvSyncBanners(); if (n) { derive(); setTimeout(() => toast(`${n} HP attack${n > 1 ? 's' : ''} now show${n > 1 ? '' : 's'} ${n > 1 ? 'their' : 'its'} name when ${n > 1 ? 'they land' : 'it lands'}: Save to keep it (Undo to skip).`), 400); } }
  runChecks(); renderAll(); refreshStatus();
}

/* ====================================================== retail characters from the game ISO (retail.js) */
S.retailChars = []; S.abilities = {};
async function openIsoFile(file, handle) {
  try { await isoOpen(file); } catch (e) { toast('Could not read the ISO: ' + e.message, 'bad'); return false; }
  if (handle) IDB.set('iso', handle);
  try {
    const mainBin = await isoGet('general_archive/main/main.bin');
    try { FXR.resident = loadResident(mainBin); } catch (e) { console.warn('studio: no resident effect library', e); }
    fxTexCache.clear(); FXHD.map = null; fxGeoCache.clear();
    S.abilities = retailAbilities(mainBin, await isoGet('general_archive/main/en/main_lang.bin'));
    try { S.bonusNames = retailBonusNames(mainBin, await isoGet('text/en/mess_pk_customize_ability_0.bin')); } catch (e) { S.bonusNames = new Map(); console.warn('studio: no bonus names', e); }
  } catch (e) { S.abilities = {}; console.warn('studio: no ability names', e); }
  if (S.fx) { S.fx.cache.clear(); fxRefresh(); }
  S.retailChars = ROSTER.map(([code, label]) => ({ code, label, costumes: costumesOf(code) })).filter((c) => c.costumes.length);
  setLoc(['isoBtn', 'isoBtn2'], true, 'game ISO', ISO.name + ': ' + S.retailChars.length + ' retail characters. Click to set another ISO.');
  $('isoReopen').hidden = true;
  fillCharPick();
  if (!S.char) { $('charPick').value = 'iso:' + S.retailChars[0].code; await pickRetail(S.retailChars[0].code); }
  else toast(S.retailChars.length + ' retail characters from ' + ISO.name + ' are in the character list. Change one and Save makes it a mod.');
  return true;
}
async function openIso() {
  if (window.showOpenFilePicker) {
    let h;
    try { [h] = await window.showOpenFilePicker({ id: 'dissidia-iso', types: [{ description: 'Dissidia 012 ISO', accept: { 'application/octet-stream': ['.iso'] } }] }); }
    catch (e) { return; }
    return openIsoFile(await h.getFile(), h);
  }
  $('isoInput').click();
}
async function reopenIso() {                       // the ISO picked last time (Chrome asks once per visit)
  const h = await IDB.get('iso');
  if (!h) return openIso();
  try { if ((await h.requestPermission({ mode: 'read' })) !== 'granted') return; return openIsoFile(await h.getFile(), h); }
  catch (e) { return openIso(); }
}
async function pickRetail(code) {
  const c = S.retailChars.find((x) => x.code === code); if (!c) return;
  const name = c.label;
  S.char = name; S.charDir = '';
  S.mod = { character: { name }, move_names: S.abilities[code] || {} };
  S.rigs = c.costumes.map(([cos, label]) => ({ retail: { code, cos }, label: `p_${code}_${cos}`, desc: label, gmo: `obj/p_${code}_${cos}.gmo`, ms: `retail/p_${code}${cos.endsWith('_a') ? '_a' : ''}` }));
  const rp = $('rigPick');
  rp.innerHTML = S.rigs.map((r, i) => `<option value="${i}" title="${esc(r.label)}">${esc(r.desc)}</option>`).join('');
  rp.value = '0';
  await loadRetailRig(S.rigs[0]);
}
const retailCache = new Map();
const cmdNotesOf = (code) => Object.fromEntries(Object.entries(S.abilities[code] || {}).map(([k, v]) => [parseInt(v.cmd, 16), `${v.name} (ability ${k})`]));
async function retailDecoded(objx, gmo, bones) {          // the moveset folder's files (JSON objects and binary sections)
  const key = objx + '|' + gmo;
  if (!retailCache.has(key)) retailCache.set(key, decodeMoveset(await isoGet(objx), bones || parseGMO(await isoGet(gmo)).bones.map((b) => b.name), cmdNotesOf(objx.match(/p_([a-z]+\d+)/)[1])));
  return retailCache.get(key);
}
async function loadRetailRig(rig) {
  S.readOnly = !!retailModOf(rig.retail.code); document.body.classList.toggle('ro', S.readOnly);
  S.rig = rig; S.msDir = rig.ms; S.files = {}; S.undo = []; S.redo = []; S.sel = null; S.step = null; S.newMove = null; S.whole = null; S.comp = null; S.comps = []; S.modPath = null; undoBtns();
  try {
    S.gmo = parseGMO(await isoGet(rig.gmo));
    const dec = await retailDecoded(rig.ms.replace('retail/', 'obj/') + '.objx', rig.gmo, S.gmo.bones.map((b) => b.name));
    for (const [p, obj] of Object.entries(dec)) if (!(obj instanceof Uint8Array)) S.files[msPath(p)] = { obj: JSON.parse(JSON.stringify(obj)), text: '', dirty: false };
    S.fx = null; S.effe = null; S.fxEdit = null;
    const ent = dec['arc.json'].entries.find((x) => x.tag === '65666665');
    try {
      if (ent) { S.effe = { path: null, rel: ent.file, bytes: dec[ent.file], dirty: false, pkg: null }; S.fx = loadEffects(dec[ent.file]); }
      const mg = dec['arc.json'].entries.find((x) => x.tag === '6d616769');
      if (S.fx && mg) S.fx.magi = parseMagi(dec[mg.file]);
    } catch (e) { console.warn('studio: no effects', e); }
  } catch (e) { toast('Could not read ' + rig.label + ' from the ISO: ' + e.message); return; }
  S.tlPaths = F(msPath('timelines/index.json')).files.filter((fn) => fn.endsWith('.json') && S.files[msPath(fn)]).map(msPath);
  for (const f of Object.values(S.files)) f.text = wjText(f.obj);
  S.skins = []; S.skinShown = null; skinHud();
  showRig();
}
$('isoBtn').onclick = openIso; $('isoBtn2').onclick = openIso;
$('isoReopen').onclick = reopenIso;
$('isoInput').onchange = (e) => e.target.files[0] && openIsoFile(e.target.files[0], null);
IDB.get('iso').then(async (h) => {
  if (!h) return;
  try { if ((await h.queryPermission({ mode: 'read' })) === 'granted') return openIsoFile(await h.getFile(), h); } catch (e) { return; }
  $('isoReopen').hidden = false; $('isoReopen').textContent = 'Use ' + h.name + ' again';
});

/* what the engine's own scripts (states, ids below 10000) play, where the reverse-engineering notes name them
   (docs/archive: hit-reaction, attacker-movement, event-ops); the rest stay "state N" */
const STATE_NAMES = { 1: 'stand', 100: 'jump', 101: 'fall', 102: 'landing', 103: 'air dash (hold ✕)', 240: 'dash (R+△)',
  400: 'dodge', 410: 'dodge', 440: 'dodge', 460: 'dodge', 470: 'block', 471: 'block',
  500: 'flinch (front)', 501: 'flinch (back)', 510: 'flinch', 511: 'flinch', 550: 'launched (front)', 551: 'launched (back)',
  560: 'stagger (front)', 561: 'stagger (back)', 600: 'knocked back', 601: 'floor slide', 700: 'blown away (front)', 750: 'blown away (back)',
  701: 'blown away, dropping', 751: 'blown away, dropping', 702: 'down', 752: 'down', 704: 'wall rush', 754: 'wall rush', 1210: 'intro / victory', 3000: 'EX Revenge' };
const stateLabel = (n) => (STATE_NAMES[n] ? `${STATE_NAMES[n]} · ${n}` : 'state ' + n);
/* an action's own name (the last 16 bytes of its record, e.g. BR_L_AERO2_EX) */
function actionTag(id) {
  const a = S.actionById && S.actionById.get(id); if (!a || !a.raw) return '';
  const b = a.raw.slice(-32); let t = '';
  for (let i = 0; i < b.length; i += 2) { const c = parseInt(b.slice(i, i + 2), 16); if (!c) break; t += String.fromCharCode(c); }
  return t;
}
/* ====================================================== derived tables */
function derive() {
  const mm = F(msPath('motion_map.json')).rows, acts = F(msPath('actions.json')).actions;
  S.actionById = new Map(acts.map((a) => [a.action, a]));
  S.motionsOfScript = new Map();
  for (const r of mm) { if (!S.motionsOfScript.has(r.script)) S.motionsOfScript.set(r.script, []); S.motionsOfScript.get(r.script).push(r.motion); }
  S.tlOfMotion = new Map(); S.weaponBones = null;
  for (const p of S.tlPaths) { const m = F(p).motion; if (!S.tlOfMotion.has(m)) S.tlOfMotion.set(m, p); }
  const names = {};
  for (const [k, v] of Object.entries(S.mod.move_names || {})) if (!k.startsWith('_') && v && v.cmd) names[parseInt(v.cmd, 16)] = { ...v, key: k };
  S.moves = F(msPath('moves.json')).commands.map((c) => {
    const id = parseInt(c.id, 16), nm = names[id];
    const steps = c.steps.map((s, i) => {
      const a = S.actionById.get(s.action), script = a ? a.script : null, ms = script != null ? (S.motionsOfScript.get(script) || []) : [];
      const tr = !ms.length && typeof tracedFor === 'function' ? tracedFor(s.action) : null;   // a shared script: the traced one (traced.js)
      if (tr && S.motionsOfScript.has(tr.start)) return { i, action: s.action, flags: s.flags, script: tr.start, recordScript: script, traced: tr, motion: S.motionsOfScript.get(tr.start)[0] };
      return { i, action: s.action, flags: s.flags, script, motion: ms.length ? ms[0] : -1 };
    });
    const name = (nm && (nm.name || nm.was || '').replace(/\{(ground|midair)\}/g, '($1)')) || (c._info ? c._info.replace(/\s*\(ability \w+\)$/, '') : 'Command ' + c.id);
    return { id, hexId: c.id, name, info: nm, disabled: nm && nm.disabled, steps };
  });
  // a command with no ability of its own whose prereq names another command is a version of that move the game picks
  // instead of it (Shantotto's spells: 0x09 / 0x0A are Comet with fewer casts): it shows, and is named as, that move
  const cmds = F(msPath('moves.json')).commands, byCmd = new Map(S.moves.map((m) => [m.id, m]));
  S.moves.forEach((m, i) => {
    const pre = (cmds[i] && cmds[i].prereq) || [], p = !m.info && pre[0] != null && pre[0] !== 0xFFFF ? byCmd.get(pre[0]) : null;
    if (!(p && p.info && p.variantOf == null)) return;
    m.name = p.name; m.info = p.info; m.disabled = p.disabled; m.variantOf = p.id;
    // its presses the first ones of that move: the same animations and events, so it isn't listed on its own
    if (m.steps.length <= p.steps.length && m.steps.every((s, k) => s.action === p.steps[k].action)) { m.folded = true; (p.versions = p.versions || []).push(m); return; }
    // otherwise a branch: the same first press(es), then other actions (Yuna's _EX and AERO3_2 / 3_3 follow-ups)
    const at = m.steps.findIndex((s, k) => !p.steps[k] || s.action !== p.steps[k].action);
    m.branchAt = at < 0 ? m.steps.length - 1 : at; m.branchTag = actionTag(m.steps[m.steps.length - 1].action);   // its last press names it best (AERO3_2_EX)
    m.branchEx = m.steps.slice(m.branchAt).some((s) => /_EX|_E$/.test(actionTag(s.action)));   // its own actions named _EX: the EX Mode version
  });
  // an HP chain: a move with its own ability whose prereq's second entry is another command and whose first presses are
  // that move's (all but its last, or all of them): Noctis's Katana of the Warrior after Armiger Assault. It is that
  // move's HP finisher, listed under it with the brave one (2026-10-28)
  S.moves.forEach((m, i) => {
    const pre = (cmds[i] && cmds[i].prereq) || [], p = m.info && m.variantOf == null && pre[1] != null && pre[1] !== 0xFFFF ? byCmd.get(pre[1]) : null;
    if (!p || p === m || p.variantOf != null) return;
    let k = 0; while (k < p.steps.length && k < m.steps.length && m.steps[k].action === p.steps[k].action) k++;
    if (k < 1 || k >= m.steps.length) return;   // (any press: studio-made finishers branch where the user picks)
    m.chainOf = p.id; m.chainAt = k; (p.chains = p.chains || []).push(m);
  });
  if (typeof burstMove === 'function') { const b = burstMove(); if (b) S.moves.push(b); }   // the EX Burst (burst.js)
  S.usedBy = new Map();
  for (const m of S.moves) m.steps.forEach((s) => { if (s.motion < 0) return; if (!S.usedBy.has(s.motion)) S.usedBy.set(s.motion, []); S.usedBy.get(s.motion).push(`${m.name} ${s.i + 1}/${m.steps.length}`); });
  for (const r of mm) if (r.script < 10000) { if (!S.usedBy.has(r.motion)) S.usedBy.set(r.motion, []); S.usedBy.get(r.motion).push(stateLabel(r.script)); }
  const hits = F(msPath('hits.json')).hits;
  S.hitByName = new Map(); const seen = {};
  hits.forEach((h) => { seen[h.name] = (seen[h.name] || 0) + 1; });
  hits.forEach((h) => S.hitByName.set(seen[h.name] === 1 && h.name && !h.name.startsWith('0x') ? h.name : h.id, h));
  S.hitById = new Map(hits.map((h) => [parseInt(h.id, 16), h]));
  S.boneNames = S.gmo ? S.gmo.bones.map((b) => b.name) : (F(msPath('bones.json')) || { bones: [] }).bones;
  S.boneIndex = new Map(); S.boneNames.forEach((n, i) => { if (!S.boneIndex.has(n)) S.boneIndex.set(n, i); });
  S.effects = new Map(((F(msPath('effects.json')) || {}).effects || []).map((e) => [e.id, e]));
  if (S.fx) fxuDeriveEffects();               // the package itself (it may have been edited)
  if (S.fx && !(S.rig && S.rig.retail) && F(msPath('projectiles.json'))) S.fx.magi = magiFromJson(F(msPath('projectiles.json')));   // edits show at once
  S.projAtkp = new Map();
  for (const r of ((F(msPath('projectiles.json')) || {}).records || [])) S.projAtkp.set(r.id, r.atkp);
  S.exOnly = new Set();
  for (const h of S.mod.runtime_hooks || []) if (typeof h === 'string' && h.startsWith('armiger=')) h.slice(8).split(',').slice(3, 5).forEach((x) => S.exOnly.add(parseInt(x, 10)));
  S.gameVis = null; gameVisLoad();
}
const motionName = (i) => (S.gmo && S.gmo.motions[i]) ? S.gmo.motions[i].name : 'motion ' + i;
function motionLen(i) {
  const m = S.gmo && S.gmo.motions[i];
  if (m && m.end) return Math.max(1, Math.ceil(m.end));
  const t = S.tlOfMotion.get(i) && F(S.tlOfMotion.get(i));
  if (!t) return 60;
  return Math.max(30, ...t.ranged.map((e) => frames(e.end === FOREVER ? e.start + 20 : e.end)), ...t.keyed.map((e) => frames(e.frame) + 5));
}
const hitKey = (h) => [...S.hitByName].find(([k, v]) => v === h)[0];
function hitOf(v) { return S.hitByName.get(v) || S.hitByName.get(typeof v === 'number' ? hex4(v) : v) || (S.hitById && S.hitById.get(typeof v === 'number' ? v : parseInt(v, 16))); }
const isSetupHide = (e, kind) => kind === 'keyed' && e.frame === 0 && opCode(e.op) === 0x010C;
function partIndex(v) { return typeof v === 'number' ? v : (S.boneIndex.has(v) ? S.boneIndex.get(v) : -1); }

/* ====================================================== 3D view */
const view = (() => {
  const host = $('viewport');
  // Opaque, with the page's backdrop drawn by the scene: on a transparent canvas an effect's glow over an empty spot has
  // no alpha (the GE blends keep the destination's), and the browser drops it there; only the grid showed it.
  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, preserveDrawingBuffer: true });
  renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
  renderer.outputEncoding = THREE.sRGBEncoding;
  host.insertBefore(renderer.domElement, host.firstChild);
  const scene = new THREE.Scene();
  const themeVar = (k) => getComputedStyle(document.documentElement).getPropertyValue(k).trim();
  const backdrop = () => {                        // the .viewport CSS gradient: ellipse at 50% 85%, --vp-glow -> --void at 70%
    const c = document.createElement('canvas'); c.width = c.height = 512; const g = c.getContext('2d'), bg = themeVar('--void') || '#18181b', glow = themeVar('--vp-glow') || '#2f2f35';
    g.fillStyle = bg; g.fillRect(0, 0, 512, 512);
    g.save(); g.translate(256, 0.85 * 512); g.scale(1, 1.2 / 0.707);
    const r = 0.707 * 512, gr = g.createRadialGradient(0, 0, 0, 0, 0, r);
    gr.addColorStop(0, glow); gr.addColorStop(0.7, bg); gr.addColorStop(1, bg);
    g.fillStyle = gr; g.fillRect(-512, -1024, 1024, 2048); g.restore();
    const tx = new THREE.CanvasTexture(c); tx.encoding = THREE.sRGBEncoding; return tx;
  };
  scene.background = backdrop();
  const camera = new THREE.PerspectiveCamera(35, 1, 0.01, 200);
  camera.position.set(2.4, 1.5, 4.6);
  const controls = new THREE.OrbitControls(camera, renderer.domElement);
  controls.target.set(0, 0.85, 0); controls.update();
  // Blender-style camera: drag (left or middle button) orbits, Shift+drag pans, Ctrl+middle-drag and the wheel zoom;
  // right-drag still pans. OrbitControls already turns a Shift (or Ctrl / Cmd) rotate drag into a pan; the middle
  // button orbits instead of zooming, except with Ctrl held, chosen just before OrbitControls sees the press
  // (a capture listener on the canvas runs before its own).
  controls.mouseButtons = { LEFT: THREE.MOUSE.ROTATE, MIDDLE: THREE.MOUSE.ROTATE, RIGHT: THREE.MOUSE.PAN };
  renderer.domElement.addEventListener('pointerdown', (e) => {
    controls.mouseButtons.MIDDLE = e.button === 1 && e.ctrlKey && !e.shiftKey ? THREE.MOUSE.DOLLY : THREE.MOUSE.ROTATE;
  }, true);
  renderer.domElement.title = 'Drag to orbit · Shift+drag to pan · wheel or Ctrl+middle-drag to zoom';
  // the floor: 40 m of quarter-metre cells fading out from 7 m, moved with the camera's target in whole metres (tick), so
  // a move that travels 10 m still has a floor under it (2026-11-01); colours from the theme variables
  const gridMake = () => {
    const g = new THREE.GridHelper(40, 160, new THREE.Color(themeVar('--grid-a') || '#4a4a52'), new THREE.Color(themeVar('--grid-b') || '#2f2f35'));
    g.material.transparent = true; g.material.depthWrite = false;
    g.material.onBeforeCompile = (sh) => {
      sh.vertexShader = sh.vertexShader.replace('#include <common>', '#include <common>\nvarying vec2 vGridP;').replace('#include <begin_vertex>', '#include <begin_vertex>\nvGridP = position.xz;');
      sh.fragmentShader = sh.fragmentShader.replace('#include <common>', '#include <common>\nvarying vec2 vGridP;').replace('#include <dithering_fragment>', '#include <dithering_fragment>\ngl_FragColor.a *= 1.0 - smoothstep(7.0, 18.0, length(vGridP));');
    };
    return g;
  };
  let grid = gridMake(); scene.add(grid);
  const ring = new THREE.Mesh(new THREE.RingGeometry(0.55, 0.57, 64), new THREE.MeshBasicMaterial({ color: 0xb0b4ba, transparent: true, opacity: 0.35, side: THREE.DoubleSide }));
  ring.rotation.x = -Math.PI / 2; ring.position.y = 0.002; scene.add(ring);
  const resize = () => { const r = host.getBoundingClientRect(); renderer.setSize(r.width, r.height, false); camera.aspect = r.width / Math.max(1, r.height); camera.updateProjectionMatrix(); };
  new ResizeObserver(resize).observe(host); resize();
  return { renderer, scene, camera, controls, grid, root: null, bones: [], partMeshes: [] };
})();
function partStates(t, tick) {
  const st = new Map(); if (!t) return st;
  const run = (e) => typeof evMode !== 'function' || evMode(e);
  for (const e of [...t.keyed].filter(run).sort((a, b) => a.frame - b.frame)) {
    const c = opCode(e.op);
    if ((c === 0x010B || c === 0x010C) && e.frame <= tick) st.set(partIndex(e.part ?? (e.args || [])[0]), c === 0x010B);
  }
  for (const e of t.ranged.filter(run)) {
    const c = opCode(e.op);
    if ((c === 0x011B || c === 0x011C) && e.start <= tick && (e.end === FOREVER || tick < e.end)) st.set(partIndex(e.part ?? (e.args || [])[0]), c === 0x011B);
  }
  return st;
}
/* Weapon parts: every draw bone a ranged "show part" (0x011B) switches anywhere in the moveset. The game starts them
   hidden (the weapon list fills their visibility entries as {bone, 0, 0} at load) and a ranged show hides its bone
   again when it ends, so outside their own show ranges they are not drawn (retail Bartz's 13 weapons, seen in game
   2026-10-20). Parts no event switches stay drawn (Bartz's cape wep16). Cached until the next edit. */
function weaponBones() {
  if (S.weaponBones && S.weaponBones.gen === S.gen) return S.weaponBones.set;
  const set = new Set();
  for (const p of S.tlOfMotion.values()) {
    const t = F(p); if (!t) continue;
    for (const e of t.ranged || []) if (opCode(e.op) === 0x011B) set.add(partIndex(e.part ?? (e.args || [])[0]));
  }
  S.weaponBones = { gen: S.gen, set };
  return set;
}
/* The game's own part lists for this character (needs the ISO; a mod uses its based_on character's, which the roster
   tool clones for it), from main.bin:
   - objentry.bin, the record whose file stem (+0x34) is p_<code>: +0x0A up to 26 draw bones, 0xFF-ended. At battle
     start FUN_08833534 hides each of them (FUN_088372D0): retail Bartz lists wep0-wep15, wep17, wep18, not the cape wep16.
   - system.bin 'chrp' (0x90-byte records, code at +0x74): +0x18 draw bones drawn only outside EX Mode, +0x3C only in
     EX Mode (u16 lists, 0xFFFF-ended; FUN_08ACAC60). Bartz: 72 / 73 + 74. */
const GAMEVIS = { for: null, byCode: null };
async function gameVisLoad() {
  const code = S.rig && S.rig.retail ? S.rig.retail.code : (S.mod && S.mod.character && S.mod.character.based_on) || null;
  if (!code || !(typeof ISO !== 'undefined' && ISO.index && ISO.index.size)) return;
  try {
    if (GAMEVIS.for !== ISO.index) {
      const main = mpkNamed(await isoGet('general_archive/main/main.bin')), by = new Map();
      const oe = main['objentry.bin'], odv = DV(oe);
      for (let i = 0, n = odv.getUint32(4, true); i < n; i++) {
        const o = 8 + 0x44 * i; let st = '';
        for (let k = o + 0x34; k < o + 0x44 && oe[k]; k++) st += String.fromCharCode(oe[k]);
        const c = st.toLowerCase().replace(/^p_/, ''); if (by.has(c) || !/^[a-z]+\d+$/.test(c)) continue;
        const hidden = new Set(); for (let k = o + 0x0A; k < o + 0x24 && oe[k] !== 0xFF; k++) hidden.add(oe[k]);
        by.set(c, { hidden, exOff: new Set(), exOn: new Set() });
      }
      const chrp = arcRead(main['system.bin']).find((e) => e.tag === 'chrp').data, cdv = DV(chrp);
      const list = (o) => { const out = new Set(); for (let k = 0; k < 18; k++) { const v = cdv.getUint16(o + 2 * k, true); if (v === 0xFFFF) break; out.add(v); } return out; };
      for (let r = 8; r + 0x90 <= chrp.length; r += 0x90) {
        let c = ''; for (let k = 0; k < 8 && chrp[r + 0x74 + k]; k++) c += String.fromCharCode(chrp[r + 0x74 + k]);
        const e = by.get(c.toLowerCase().replace(/^p_/, '')); if (e) { e.exOff = list(r + 0x18); e.exOn = list(r + 0x3C); }
      }
      GAMEVIS.for = ISO.index; GAMEVIS.byCode = by;
    }
    const want = S.rig && S.rig.retail ? S.rig.retail.code : (S.mod && S.mod.character && S.mod.character.based_on) || null;
    if (want !== code) return;                                    // the character changed while this loaded
    S.gameVis = GAMEVIS.byCode.get(code) || null;
    if (S.gameVis) { S.gameVis.exOn.forEach((b) => S.exOnly.add(b)); pose(); renderInspector(); }
  } catch (e) { console.warn('studio: no part lists from main.bin', e); }
}
/* Drawn when no event of this animation sets it: hidden if the game hides it at battle start, or (no ISO) if a ranged
   show switches it somewhere; EX-only and outside-EX-only parts follow the EX Mode switch. */
function partDefault(b) {
  const g = S.gameVis;
  if (g && g.exOff.has(b)) return !S.exMode;
  if (g && g.exOn.has(b)) return !!S.exMode;
  return !(g ? g.hidden.has(b) : false) && !weaponBones().has(b);
}
/* mod.json body.ex_parts: weapons the game draws in EX Mode only, in place of the others (retail Cloud's wep1, the
   Ultima Weapon). The game swaps them in its own code, not the moveset, so the preview is told. -> false: hide it */
function exWeaponShown(name) {
  const ex = new Set((S.mod && S.mod.body && S.mod.body.ex_parts) || []);
  if (!ex.size) return true;
  return ex.has(name) ? !!S.exMode : !(S.exMode && /^wep\d+$/.test(name));
}
function updateParts() {
  if (!view.partMeshes) return;
  const game = $('partsGame').checked;
  if (S.modelPose && game) {                        // Body / Costumes: the body and its weapons, not the effect parts
    const gs = (S.mod && S.mod.body && S.mod.body.weapon_groups) || [], shown = new Set(gs.flatMap((g) => g.parts || []));
    view.partMeshes.forEach((pm, pi) => { if (!pm) return; const n = (S.gmo.parts[pi] || {}).name || ''; pm.mesh.visible = pi === 0 || (shown.size ? shown.has(n) : /^wep\d+$/.test(n)) && exWeaponShown(n); });
    return;
  }
  const st = partStates(curTL(), ticks(S.frame));
  // exaway= (roster.cpp ex_summons_out): in EX Mode the ring gives way while he holds one of these royal arms
  // (it stays away until none has been out for 20 ticks, kRingAwayHold)
  const ad = S.exMode ? exAwayDraws() : [], T = ticks(S.frame), held = (st2) => ad.some((d) => st2.get(d) === true);
  const away = ad.length && (held(st) || [5, 10, 15, 20].some((k) => T - k >= 0 && held(partStates(curTL(), T - k)))) ? exRingDraw() : -1;
  view.partMeshes.forEach((pm, pi) => { if (!pm) return; pm.mesh.visible = !game || pi === 0 || (st.has(pm.draw) ? st.get(pm.draw) : partDefault(pm.draw)) && (S.exMode || !S.exOnly.has(pm.draw)) && pm.draw !== away && exWeaponShown((S.gmo.parts[pi] || {}).name || ''); });
}
const exHook = (k) => ((S.mod && !(S.rig && S.rig.retail) && S.mod.runtime_hooks) || []).find((x) => typeof x === 'string' && x.startsWith(k + '='));
function exAwayDraws() { const h = exHook('exaway'); return h ? h.slice(7).split(/[,/]/).map(Number).filter((n) => n >= 0) : []; }
function exRingDraw() { const h = exHook('armiger'); return h ? parseInt(h.slice(8).split(',')[3], 10) : -1; }
/* mod.json ghostpin=M/M..,FIRST,LAST (roster.cpp ghostpin_bone_post): while one of those motions plays the warp-ghost
   skeleton stays in the world where the motion started (the dodges the game moves him through itself). With the dummy
   on the preview moves him too, so g_root is put back where it was on the motion's first tick. */
function ghostPinOf() {
  const h = ((S.mod && S.mod.runtime_hooks) || []).find((x) => typeof x === 'string' && x.startsWith('ghostpin='));
  if (!h) return null;
  const [ms, first] = h.slice(9).split(',');
  return { motions: new Set(ms.split('/').map(Number)), first: +first };
}
function ghostPinApply() {
  const gp = ghostPinOf(); if (!gp || !gp.motions.has(S.motion) || !view.root || !view.bones) return;
  if (!(typeof DM !== 'undefined' && DM.on && DM.sim)) return;                    // he doesn't travel: nothing to undo
  const b = view.bones[gp.first]; if (!b || b.parent !== view.root) return;
  view.root.updateMatrixWorld(true);
  const W0 = fxPoseAt(0).root.clone(); b.updateMatrix();
  const L = new THREE.Matrix4().copy(view.root.matrixWorld).invert().multiply(W0).multiply(b.matrix);
  L.decompose(b.position, b.quaternion, b.scale); b.updateMatrixWorld(true);
}
function pose() { if (S.gmo && S.modelPose) { poseMotion(S.gmo, view, -1, 0); updateParts(); return; } if (S.gmo) { poseMotion(S.gmo, view, S.motion, S.frame); updateParts(); if (typeof psndAfterPose === 'function') psndAfterPose(); if (typeof dmRefresh === 'function') dmRefresh(); ghostPinApply(); if (typeof spRefresh === 'function') spRefresh(); fxRefresh(); if (typeof edVolDraw === 'function') edVolDraw(); if (typeof gcamRefresh === 'function') gcamRefresh(); } if (typeof fxfApply === 'function') fxfApply(); if (typeof ftApply === 'function') ftApply(); }
/* an event's values in order (named ones first), with parts as bone numbers */
function argsOf(e) {
  if (isHexOp(e)) return (e.args || []).map((v) => typeof v === 'object' ? v.f : v);
  const info = opInfo(e, 'frame' in e ? 'keyed' : 'ranged'), out = [];
  for (const n of (info.args || []).slice(0, info.nargs)) { const v = e[n]; out.push(n === 'part' ? partIndex(v) : n === 'hit' ? v : (v && typeof v === 'object') ? v.f : v); }
  for (const v of e.args || []) out.push(typeof v === 'object' ? v.f : v);
  return out;
}
/* the effect preview poses the skeleton at earlier ticks too (trails, world-space particles, spawn-once placement) */
const FXP = { gmo: null, motion: -1, map: new Map() };
function fxPoseAt(tick, mo = S.motion, pi = null) {   // mo, pi: an earlier press's animation and index (fxEarlierInsts)
  const dmk = typeof DM !== 'undefined' && DM.on && DM.sim && !S.fxEdit ? DM.key + '|' + (S.step ? S.step.move + ':' + S.step.i : '') : '';
  if (FXP.gmo !== S.gmo || FXP.motion !== S.motion || FXP.dmk !== dmk) { FXP.gmo = S.gmo; FXP.motion = S.motion; FXP.dmk = dmk; FXP.map.clear(); }
  const T = Math.max(0, Math.round(tick)), key = pi == null ? T : `${mo}|${pi}|${T}`;
  if (!FXP.map.has(key)) {
    if (!S.gmo || !view.root) return { root: new THREE.Matrix4(), bones: (view.bones || []).map(() => new THREE.Matrix4()) };
    // with the dummy on the attacker travels: pose him where he was at that tick, so what he left behind stays there
    const at = dmk && typeof dmAttackerAt === 'function' ? dmAttackerAt(T, pi) : null;
    const keep = at ? { p: view.root.position.clone(), r: view.root.rotation.y } : null;
    if (at) { view.root.position.set(at.x, at.y, at.z); view.root.rotation.y = at.yaw; }
    poseMotion(S.gmo, view, mo, frames(T)); view.root.updateMatrixWorld(true);
    FXP.map.set(key, { root: view.root.matrixWorld.clone(), bones: view.bones.map((b) => b.matrixWorld.clone()) });
    if (keep) { view.root.position.copy(keep.p); view.root.rotation.y = keep.r; }
    FXP.dirty = true;
  }
  return FXP.map.get(key);
}
/* 2026-11-01: "Why do effects all vanish when a new animation starts? They should persist". In the game an effect
   lives out its own lifetime (or its range) after the action that started it hands over; the preview only ran the
   press on show. With the whole move playing, the effects the earlier presses started run on, each on its own press's
   clock: attached ones follow the bones as they are now, the rest stay where they were left. Weapon trails (bone effect
   slots) are the action's and stop with it, so they are left out, as are projectiles and (with the dummy on) contact
   effects, which the simulation places for the press on show. */
function fxEarlierInsts(tickNow, now, base) {
  const W = tlWhole(); if (!W || !S.fx || !S.step) return [];
  const cur = W.segs[S.step.i]; if (!cur) return [];
  const out = [];
  for (let j = 0; j < S.step.i; j++) {
    const g = W.segs[j]; if (!g || !g.t) continue;
    const T0 = tickNow + (cur.at - g.at) * 2;          // this tick on press j's clock
    const opts = { ...base, motionName: motionName(g.motion), phase: stepPhase(j, g.motion), foeAt: null, foePosAt: null, shotAt: null };
    const ctx = { tick: T0, opts, poseAt: (T) => (T >= T0 - 1e-6 ? now : fxPoseAt(T, g.motion, j)) };
    for (const x of fxInstances(S.fx, g.t, opts)) if (x.slot == null && !x.trail && x.type !== 'proj' && !(x.type === 'hit' && base.foeAt) && x.tick <= T0) out.push({ ...x, ctx });
  }
  return out;
}
function fxRefresh() {
  try {
    if (view.root) view.root.updateMatrixWorld(true);
    const now = { root: view.root ? view.root.matrixWorld.clone() : new THREE.Matrix4(), bones: (view.bones || []).map((b) => b.matrixWorld.clone()) };
    const tickNow = S.frame * 2, names = (view.bones || []).map((b) => b.name);
    const solo = S.fxEdit && S.fx ? fxuSoloOpts() : null;
    const fo = { on: $('showFx').checked || !!solo, hitOf, motionName: motionName(S.motion), phase: stepPhase(), ex: !!S.exMode, solo, mute: solo && S.fxEdit.mute,
      poseAt: (T) => (solo || T >= tickNow - 1e-6 ? now : fxPoseAt(T)), extra: !solo ? [...(typeof compInsts === 'function' ? compInsts() || [] : []), ...(typeof spFxInsts === 'function' ? spFxInsts() : []), ...(typeof dmSimFxInsts === 'function' ? dmSimFxInsts() : []), ...(typeof dmArmTrailInsts === 'function' ? dmArmTrailInsts() : [])] : null, skipComp: S.comp ? compSlug(S.comp.path) : null, foeAt: !solo && typeof dmFoeAt === 'function' ? dmFoeAt : null, foePosAt: !solo && typeof dmFoePosAt === 'function' ? dmFoePosAt : null, shotAt: !solo && typeof dmShotAt === 'function' ? dmShotAt : null, boneIndex: (n) => (n === 'root' ? FX_ROOT_BONE : names.indexOf(n)), boneOk: (i) => i >= 0 && i < names.length, hasBone: (n) => names.includes(n) };
    if (FXEV && !solo) {                 // the effect picker's preview: only the hovered effect, on its own clock (fxEvPreview)
      const one = fxInstances(S.fx, curTL(), fo).filter((x) => x.kind === FXEV.kind && x.idx === FXEV.idx)
        .map((x) => ({ ...x, ctx: { tick: x.tick + FXEV.t, poseAt: () => now } }));
      fxUpdate(view, S.fx, null, tickNow, { ...fo, extra: one });
    } else {
      if (fo.extra && !solo) fo.extra.push(...fxEarlierInsts(tickNow, now, fo));
      fxUpdate(view, S.fx, curTL(), solo ? S.fxEdit.t : tickNow, fo);
    }
  } catch (e) { console.warn('studio: effect preview', e); }
  if (FXP.dirty) { FXP.dirty = false; if (S.gmo) { poseMotion(S.gmo, view, S.motion, S.frame); if (view.root) view.root.updateMatrixWorld(true); if (typeof dmApocNow === 'function') dmApocNow(); ghostPinApply(); } }
}
function stepPhase(i, mo) {             // motion-map phase of the selected press (events filtered on another phase don't run); i, mo: another press of the move
  if (!S.step) return null;
  if (i == null) i = S.step.i;
  if (mo == null) mo = S.motion;
  let s = S.moves[S.step.move] && S.moves[S.step.move].steps[i];
  if (!s && S.moves[S.step.move]) {               // an action a runtime hook starts (a charge's release): its own script
    const g = mvSegments(S.moves[S.step.move])[i], a = g && g.action != null && S.actionById.get(g.action);
    if (a) s = { script: a.script };
  }
  if (!s) return null;
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === s.script && r.motion === mo);
  return row ? row.phase & 0xFF : null;
}
function fxInfoCached(id) {
  if (!S.fx) return null;
  if (!S.fx.cache.has(id)) S.fx.cache.set(id, fxInfo(S.fx, id));
  return S.fx.cache.get(id);
}
function fxLabels() {                    // name tags: effects at their attach points, weapon/effect parts at their middle
  const L = $('fxLabels'); if (!L) return;
  const on = $('fxLabelsOn').checked, fxOn = on && $('showFx').checked && FXV.labels.length, parts = on ? partLabelSpots() : [], hits = hitLabelSpots();
  if (!fxOn && !parts.length && !hits.length) { if (L.innerHTML) L.innerHTML = ''; return; }
  const r = view.renderer.domElement.getBoundingClientRect(), seen = new Set(), items = []; let h = '';
  const at = (v, cls, text, attrs = '') => {
    const p = v.clone().project(view.camera);
    if (p.z > 1) return;
    items.push({ x: (p.x + 1) / 2 * r.width + 8, w: text.length * 6.2 + 12, y0: (1 - p.y) / 2 * r.height, cls, text, attrs });
  };
  // only the effects of events live at the playhead (a ranged event that has ended, or an effect fading out, has no tag)
  const tlNow = curTL(), tkNow = ticks(S.frame);
  const live = (ev) => { if (!ev || ev.kind !== 'ranged' || !tlNow) return true; const e = tlNow.ranged[ev.idx]; return !e || (e.start <= tkNow && (e.end === FOREVER || tkNow < e.end)); };
  if (fxOn) for (const lb of FXV.labels) {
    if (lb.fading || !live(lb.ev)) continue;
    const v = new THREE.Vector3().setFromMatrixPosition(lb.m), p = v.clone().project(view.camera);
    const key = lb.text + '|' + Math.round(p.x * 20) + '|' + Math.round(p.y * 20);   // the same effect in two places gets two labels
    if (seen.has(key)) continue; seen.add(key);
    const nm = (lb.text || '').replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '');
    const ev = lb.ev, link = ev || lb.ni != null;
    at(v, 'fxlabel' + (link ? ' link' : ''), nm, ev && ev.kind === 'sim' ? ` data-sk="${ev.type}" data-si="${ev.idx}" title="${esc(nm)}: ${ev.type === 'hit' ? 'played where a projectile hit the dummy' : 'a shot born of a hit'}; click to select the shot that causes it on the timeline"`
      : ev ? ` data-ek="${ev.kind}" data-ei="${ev.idx}" title="${esc(nm)}: click to select its event on the timeline"`
      : lb.ni != null ? ` data-ni="${lb.ni}" title="${esc(nm)}: no event on this timeline plays it; click to open it in the Effects tab"` : '');
  }
  for (const hl of hits) at(hl.v, 'hitlabel' + (hl.other ? ' other' : ''), hl.text, hl.attrs + ` title="${esc(hl.tip)}"`);
  for (const pl of parts) at(pl.v, 'partlabel' + (pl.flat ? ' flat' : ''), pl.name, ` data-b="${pl.b}" title="${esc(pl.name)}${pl.flat ? ': drawn, but its bones are scaled to nothing in this frame' : ''}. Click: show it in Parts › Hidden at start"`);
  // tags that would overlap stack in a column (top to bottom by where they point), and each stack is centred on the
  // points its tags belong to, so they stay next to them (2026-11-01: NOC_ARM_SUMMON / GLOW / DISMISS piled up)
  items.sort((a, b) => a.y0 - b.y0 || a.x - b.x);
  const LH = 17, grp = items.map((_, i) => i), root = (i) => (grp[i] === i ? i : (grp[i] = root(grp[i])));
  items.forEach((it, i) => {
    it.y = it.y0;
    for (let n = 0; n < 40; n++) {
      const k = items.findIndex((q, j) => j < i && it.x < q.x + q.w && q.x < it.x + it.w && Math.abs(it.y - q.y) < LH); if (k < 0) break;
      it.y = items[k].y + LH; grp[root(i)] = root(k);
    }
  });
  const sums = new Map();
  items.forEach((it, i) => { const g = root(i), a = sums.get(g) || { d: 0, n: 0 }; a.d += it.y0 - it.y; a.n++; sums.set(g, a); });
  for (const [i, it] of items.entries()) { const a = sums.get(root(i)); it.y += a.d / a.n; }
  for (const it of items) h += `<span class="${it.cls}" style="left:${(it.x - 8).toFixed(0)}px;top:${it.y.toFixed(0)}px"${it.attrs}>${esc(it.text)}</span>`;
  if (L.innerHTML !== h) L.innerHTML = h;
}
/* every drawn part except the body, at the middle of its skinned vertices (a sample of them, so it follows the pose),
   named as in the inspector's Parts list */
function partLabelSpots() {
  if (!S.gmo || !view.partMeshes || S.modelPose || (typeof PH !== 'undefined' && PH.on)) return [];
  const out = [], t = new THREE.Vector3();
  view.partMeshes.forEach((pm, pi) => {
    if (!pm || pi === 0 || !pm.mesh.visible) return;
    const m = pm.mesh, pos = m.geometry.attributes.position; if (!pos || !pos.count) return;
    if (!m.userData.labelIdx) {                     // up to 24 vertices spread through the part
      const step = Math.max(1, Math.floor(pos.count / 24)), idx = [];
      for (let i = 0; i < pos.count && idx.length < 24; i += step) idx.push(i);
      m.userData.labelIdx = idx;
    }
    const c = new THREE.Vector3(), bb = new THREE.Box3();
    for (const i of m.userData.labelIdx) { m.boneTransform(i, t); c.add(t); bb.expandByPoint(t); }
    c.divideScalar(m.userData.labelIdx.length).applyMatrix4(m.matrixWorld);
    const b = S.gmo.bones[pm.draw];             // flat: still drawn, but its bones are scaled to zero, so nothing shows
    out.push({ v: c, b: pm.draw, flat: bb.getSize(t).length() < 1e-3, name: (b && b.name) || (S.gmo.parts[pi] || {}).name || 'part ' + pi });
  });
  return out;
}
/* View → Hits (2026-10-30: "shows a label on the hit and lets you jump to it on the timeline and inspector"): a tag
   on every hit. With Dummy on, where each hit landed on the dummy (the dots on its path), named with its press (the
   other presses' hits dimmed); without it, the hit windows live at the playhead, at the middle of their attack spheres.
   Clicking one puts the press and the playhead on that hit and selects its event (timeline + inspector). */
const hitsOn = () => { const cb = $('hitLabelsOn'); return !!cb && cb.checked; };
function hitEvName(e) { const h = hitOf(e.hit); return h ? h.name || h.id : String(e.hit); }
function hitLabelSpots() {
  if (!hitsOn() || !S.gmo || !view.root || S.fxEdit || S.comp || S.modelPose || (typeof PH !== 'undefined' && PH.on)) return [];
  const out = [];
  if (typeof DM !== 'undefined' && DM.on && DM.sim && DM.sim.hits) {
    const H = DM.sim.H, cur = S.step ? S.step.i : -1;
    DM.sim.hits.forEach((h, k) => {
      const fr = DM.sim.frames[Math.min(DM.sim.frames.length - 1, h.T)]; if (!fr) return;
      const what = `${h.hp ? 'HP' : h.power + ' brave'}${h.guarded ? ', guarded' : ''}`;
      out.push({ v: new THREE.Vector3(fr.pos.x, fr.pos.y - H + 0.85, fr.pos.z), text: `${h.press + 1} · ${h.name}${h.guarded ? ' (guarded)' : ''}`,
        attrs: ` data-hk="${k}"`, other: cur >= 0 && h.press !== cur, tip: `${h.name}, press ${h.press + 1} (${what}): click to go to it on the timeline` });
    });
    return out;
  }
  const t = curTL(); if (!t || typeof DM_HIT_OPS === 'undefined' || typeof edVolRecs !== 'function') return out;
  const tick = ticks(S.frame), recs = edVolRecs(); if (!recs.length) return out;
  view.root.updateMatrixWorld(true);
  t.ranged.forEach((e, idx) => {
    if (!DM_HIT_OPS.has(opCode(e.op)) || e.group == null || !(e.start <= tick && (e.end === FOREVER || tick < e.end)) || (typeof evMode === 'function' && !evMode(e))) return;
    const c = new THREE.Vector3(); let n = 0;
    for (const r of recs) {
      if (r.group !== e.group || r.kind !== 4) continue;
      const M = r.bone === 0x4000 ? view.root.matrixWorld : view.bones[r.bone] && view.bones[r.bone].matrixWorld; if (!M) continue;
      c.add(new THREE.Vector3(...r.off).applyMatrix4(M)); n++;
    }
    if (!n) return;
    const nm = hitEvName(e);
    out.push({ v: c.divideScalar(n), text: nm, attrs: ` data-ek="ranged" data-ei="${idx}"`, other: false, tip: `${nm} (group ${e.group}, ${frames(e.start)}–${e.end === FOREVER ? 'end' : frames(e.end)}): click to select its window` });
  });
  return out;
}
function hitJump(k) {
  const h = DM.sim && DM.sim.hits[k]; if (!h) return;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(), seg = segs[h.press]; if (!seg) return;
  S.showChecks = false; S.showChanges = false; S.newMove = null; S.mfxSel = null; S.sel = null;
  if (S.step && (S.step.i !== h.press || S.whole || S.moveOnly || S.motion !== seg.motion)) { S.whole = null; S.step = { move: S.step.move, i: h.press }; S.moveOnly = false; setMotion(seg.motion); }
  else if (!S.step && S.motion !== seg.motion) setMotion(seg.motion);
  S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶';
  const local = h.T - seg.start; setFrame(local / 2);
  const t = curTL(); let kind = 'ranged', idx = -1;
  if (t) {
    idx = t.ranged.findIndex((e) => DM_HIT_OPS.has(opCode(e.op)) && hitEvName(e) === h.name && e.start - 1 <= local && local <= (e.end === FOREVER ? Infinity : e.end) + 1);
    if (idx < 0) idx = t.ranged.findIndex((e) => DM_HIT_OPS.has(opCode(e.op)) && hitEvName(e) === h.name);
    if (idx < 0 && h.proj != null) {             // a projectile's hit: the shot that fired it (the last fire_projectile before)
      let best = -1; t.keyed.forEach((e, i) => { if (opCode(e.op) === 0x0204 && e.frame <= local && (best < 0 || e.frame >= t.keyed[best].frame)) best = i; });
      if (best >= 0) { kind = 'keyed'; idx = best; }
    }
  }
  if (idx >= 0) fxLabelSelect(kind, idx); else { renderAll(); toast(`${h.name}: no event on this timeline names it (started by a runtime hook?)`); }
}
/* a simulated shot's tag (2026-10-30: "Clicking on the label should open it in the timeline, not in the effect
   editor"): its press and the playhead at its birth, and the fire_projectile that started it (the last one before) */
function shotJump(k) {
  const L = DM.sim && DM.sim.shotLog && DM.sim.shotLog[k]; if (!L) return;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments();
  let pi = segs.findIndex((g) => L.T < g.start + g.len); if (pi < 0) pi = segs.length - 1;
  const seg = segs[pi]; if (!seg) return;
  S.showChecks = false; S.showChanges = false; S.newMove = null; S.mfxSel = null; S.sel = null;
  if (S.step && (S.step.i !== pi || S.whole || S.moveOnly || S.motion !== seg.motion)) { S.whole = null; S.step = { move: S.step.move, i: pi }; S.moveOnly = false; setMotion(seg.motion); }
  else if (!S.step && S.motion !== seg.motion) setMotion(seg.motion);
  S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶';
  const local = L.T - seg.start; setFrame(local / 2);
  const t = curTL(); let best = -1;
  if (t) t.keyed.forEach((e, i) => { if (opCode(e.op) === 0x0204 && e.frame <= local && (best < 0 || e.frame >= t.keyed[best].frame)) best = i; });
  if (best >= 0) fxLabelSelect('keyed', best); else { renderAll(); toast('No fire-projectile event on this timeline starts that shot (started by a runtime hook?)'); }
}
/* clicking an effect's tag selects the timeline event that plays it (the inspector shows it), or opens it in the Effects tab
   when no event does; clicking a part's tag opens Parts › Hidden at start in the inspector at that part's row */
// on press, not click (2026-11-02: "I can't click on the labels"): the tags are rebuilt whenever the view moves (the
// camera settling, Follow, playback), so the tag under the mouse could be replaced between press and release and the
// click never fired
$('fxLabels').addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || !e.target.closest('.hitlabel, .partlabel, .fxlabel.link')) return;
  e.preventDefault(); e.stopPropagation();
  const ht = e.target.closest('.hitlabel');
  if (ht) { if (ht.dataset.hk != null) hitJump(+ht.dataset.hk); else fxLabelSelect(ht.dataset.ek, +ht.dataset.ei); return; }
  const tag = e.target.closest('.partlabel'); if (tag) partReveal(+tag.dataset.b);
  const fx = e.target.closest('.fxlabel.link'); if (!fx) return;
  if (fx.dataset.sk) { if (fx.dataset.sk === 'hit') hitJump(+fx.dataset.si); else shotJump(+fx.dataset.si); }
  else if (fx.dataset.ek) fxLabelSelect(fx.dataset.ek, +fx.dataset.ei);
  else if (fx.dataset.ni != null && typeof fxuOpen === 'function') { S.tab = 'fx'; fxuOpen(+fx.dataset.ni); }
});
function fxLabelSelect(kind, idx) {
  S.showChecks = false; S.showChanges = false; S.newMove = null;
  if (kind === 'comp') { if (!S.comp) return; S.comp.sel = idx; renderTimeline(); renderInspector(); }
  else if (S.comp) { S.comp = null; pose(); renderAll(); return fxLabelSelect(kind, idx); }   // an event of the move itself: leave the layered effect for the move's timeline
  else if (kind === 'motion') { S.sel = null; S.mfxSel = idx; renderTimeline(); renderInspector(); }
  else { const t = curTL(); if (!t || !t[kind] || !t[kind][idx]) return; S.mfxSel = null; selectEvent(kind, idx); }
  const row = $('lanesInner').querySelector('.selrow, .sel'); if (row) row.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}
function partReveal(bi) {
  if (S.sel || S.showChecks || S.showChanges) { S.sel = null; S.showChecks = false; S.showChanges = false; renderTimeline(); }
  renderInspector();
  const cb = $('insp').querySelector(`.parts input[data-b="${bi}"]`);
  if (!cb) { toast('This animation has no event timeline yet, so its parts can\'t be hidden here: create one in the inspector first.'); return; }
  const d = cb.closest('details'); if (d && !d.open) d.open = true;
  const row = cb.closest('label');
  row.scrollIntoView({ block: 'center', behavior: 'smooth' });
  row.classList.remove('flash'); void row.offsetWidth; row.classList.add('flash');
  cb.focus({ preventScroll: true });
}
let lastT = 0;
function tick(t) {
  const dt = Math.min(0.1, (t - lastT) / 1000); lastT = t;
  if (typeof PH !== 'undefined' && PH.on) phTick(dt);           // Character tab > Jump, gravity, dash (newchar.js)
  else if (S.playing && S.fxEdit) fxuTick(dt);
  else if (S.playing && S.whole && psndMark(true) && mvTick(dt)) { /* a whole move, press after press (moves.js) */ }
  else if (S.playing && S.motion >= 0) {
    let f = S.frame + dt * 30 * parseFloat($('speed').value);
    const L = motionLen(S.motion);
    if (f >= L) { if ($('loop').checked) f = f % L; else { f = L; S.playing = false; $('playBtn').textContent = '▶'; } }
    psndMark(true); S.frame = f; pose(); updatePlayhead();
  }
  psndMark(false);                                                // only playback plays sounds, not scrubbing or stepping
  camFollowTick(dt);
  if (!(typeof GCAM !== 'undefined' && GCAM.keep)) view.controls.update();   // the game camera drives it instead
  view.grid.position.set(Math.round(view.controls.target.x), 0, Math.round(view.controls.target.z));
  fxRender(view); fxLabels();
  requestAnimationFrame(tick);
}
/* Follow: the camera moves with the character (its hip bone, so root motion, jumps and the dummy's travel all count),
   keeping the orbit and zoom you set; Centre (or F) puts the orbit's target on the character now */
const CAMF = { last: null };
const d2 = (p) => p.distanceToSquared(CAMF.last);
function camFocus() {
  if (!S.gmo || !view.root) return null;
  const b = (view.bones || []).find((x) => x.name === 'hip') || (view.bones || []).find((x) => x.name === 'root');
  return (b || view.root).getWorldPosition(new THREE.Vector3());
}
function camFree() {                       // nothing else is steering the camera
  return !(typeof GCAM !== 'undefined' && GCAM.keep) && !(typeof PH !== 'undefined' && PH.on) && !(typeof FXF !== 'undefined' && FXF.on) && !S.fxEdit;
}
function camCentre() {
  const p = camFocus(); if (!p || !camFree()) return;
  const c = view.camera, t = view.controls.target, d = p.clone().sub(t); d.y = p.y + 0.15 - t.y;   // a little above the hip, near the chest
  c.position.add(d); t.add(d); view.controls.update(); CAMF.last = p;
}
function camFollowTick(dt) {
  if (!$('camFollow').checked || !camFree() || (typeof DM !== 'undefined' && DM.on && DM.sim)) { CAMF.last = null; return; }   // with the dummy, dummy.js frames both fighters   // with the dummy on too: the attacker's travel (warps) is where he really moves
  const p = camFocus(); if (!p) { CAMF.last = null; return; }
  if (!CAMF.last) { CAMF.last = p; return; }
  const d = p.clone().sub(CAMF.last).multiplyScalar(d2(p) > 4 ? 1 : 1 - Math.exp(-dt * 10)); CAMF.last.add(d);   // eased, so the body's bob doesn't shake the view; a loop's jump back is instant
  if (d.lengthSq() < 1e-10) return;
  view.camera.position.add(d); view.controls.target.add(d);
}
$('camCentre').onclick = () => camCentre();
$('camFollow').onchange = () => { try { localStorage.setItem('studio.camFollow', $('camFollow').checked ? '1' : '0'); } catch (e) { /* per-viewer only */ } if ($('camFollow').checked) { if (typeof DM !== 'undefined' && DM.on && DM.sim) { DM.camSet = null; dmRefresh(); } else camCentre(); } };
try { if (localStorage.getItem('studio.camFollow') === '1') $('camFollow').checked = true; } catch (e) { /* defaults */ }
function psndMark(on) { if (typeof PSND !== 'undefined') PSND.play = on; return true; }
requestAnimationFrame(tick);

/* ====================================================== left pane */
// The move list grouped like the in-game kit: Brave attacks, then HP attacks, then the rest (EX Burst, unknown),
// ground before midair inside each; the file's order otherwise.
function mvGrouped() {
  const TYPES = ['brave', 'hp'], PH = ['ground', 'midair', 'follow-up'];
  const rows = S.moves.map((m, mi) => ({ m, mi })).filter(({ m }) => !m.folded).map(({ m, mi }) => { const k = typeof mvKindGuess === 'function' ? mvKindGuess(m) : {}; return { m, mi, t: TYPES.indexOf(k.type), p: PH.indexOf(k.phase), k }; });
  const rank = (x) => (x.t < 0 ? 9 : x.t) * 10 + (x.p < 0 ? 9 : x.p);
  rows.sort((a, b) => rank(a) - rank(b) || a.mi - b.mi);
  // a branch sits under the move it is a version of (Vaan's weapon switches: 0x25B under Sword & Shield), not where the
  // file has it
  const ids = new Set(rows.map((r) => r.m.id)), under = new Map();
  const isBranch = (r) => r.m.branchAt != null && r.m.variantOf != null && ids.has(r.m.variantOf) && r.m.variantOf !== r.m.id;
  for (const r of rows) if (isBranch(r)) { if (!under.has(r.m.variantOf)) under.set(r.m.variantOf, []); under.get(r.m.variantOf).push(r); }
  const flat = [];
  const chainsOf = new Map();
  for (const r of rows) if (r.m.chainOf != null && ids.has(r.m.chainOf)) { if (!chainsOf.has(r.m.chainOf)) chainsOf.set(r.m.chainOf, []); chainsOf.get(r.m.chainOf).push(r); }
  for (const r of rows) { if (isBranch(r) || (r.m.chainOf != null && ids.has(r.m.chainOf))) continue; flat.push(r);   // (HP chains show under their move)
    for (const b of under.get(r.m.id) || []) flat.push({ ...b, t: r.t, p: r.p });
    for (const c of chainsOf.get(r.m.id) || []) flat.push({ ...c, t: r.t, p: r.p, chainRow: true }); }   // its follow-ups, a row each (↳ Katana of the Warrior · HP)
  const out = []; let last = null;
  for (const r of flat) {
    const g = r.t < 0 ? 'Other' : (r.t === 0 ? 'Brave attacks' : 'HP attacks') + (r.p < 0 ? '' : ' · ' + PH[r.p]);
    if (g !== last && !(g === 'Other' && last === null && rows.every((x) => x.t < 0))) out.push({ head: g });
    last = g; out.push(r);
  }
  return out;
}
/* the finisher picked for move mi: -1 = its own last (brave) press, else the index of the HP chain move shown */
function mvFinisher(mi) {
  const m = S.moves[mi], chains = (m && m.chains || []).map((c) => S.moves.indexOf(c)).filter((x) => x >= 0);
  if (!chains.length) return -1;
  const v = S.finisher && S.finisher[m.id];
  if (v != null) return chains.includes(v) ? v : -1;
  return S.step && chains.includes(S.step.move) ? S.step.move : -1;   // showing a chain's press: that finisher
}
/* the moves list's search: every word must be in the move's name or one of its animation ids; a group with nothing left hides */
function mvFilter(L) {
  const words = (S.mvQ || '').toLowerCase().split(/\s+/).filter(Boolean);
  let head = null, any = false;
  const close = () => { if (head) head.hidden = !any; };
  for (const el of L.children) {
    if (el.classList.contains('mv-group')) { close(); head = el; any = false; continue; }
    if (!el.classList.contains('move')) continue;
    const ok = words.every((w) => el.dataset.q.includes(w)) && !(el.classList.contains('in-open') && !words.length); el.hidden = !ok; if (ok) any = true;   // a follow-up of an open combo only shows in a search
  }
  close();
  let none = L.querySelector('.mv-none');
  if (words.length && !L.querySelector('.move:not([hidden])')) { if (!none) { none = document.createElement('p'); none.className = 'note mv-none'; L.appendChild(none); } none.textContent = `No move matches “${(S.mvQ || '').trim()}”.`; }
  else if (none) none.remove();
}
function renderLeft() {
  const L = $('leftList');
  if ($('tabChar')) $('tabChar').setAttribute('aria-pressed', S.tab === 'char');
  $('tabMoves').setAttribute('aria-pressed', S.tab === 'moves'); $('tabAnims').setAttribute('aria-pressed', S.tab === 'anims'); $('tabFx').setAttribute('aria-pressed', S.tab === 'fx');
  if (typeof ncArtSync === 'function') ncArtSync();          // the Character tab's menu art takes the middle
  if (!S.moves) return;
  if (typeof fxfApply === 'function') fxfApply();
  if (S.tab === 'fx') return fxuLeft(L);
  if (S.tab === 'char' && typeof ncLeft === 'function') return ncLeft(L);
  if (S.tab === 'moves') {
    const kindTag = (m, mi) => { const k = mvKindGuess(m), unk = !m.burst && m.variantOf == null && mvEntry(m) && (!k.type || !k.phase); return (k.type === 'brave' || k.type === 'hp' ? `<i class="mvg ${k.type}" title="${k.type === 'hp' ? 'HP attack (□)' : 'Brave attack (○)'}" aria-label="${k.type === 'hp' ? 'HP attack' : 'Brave attack'}"></i>` : '') + (k.type ? `<em class="mvk ${k.type}">${k.type === 'hp' ? 'HP' : 'Brave'}</em>` : '') + (unk ? `<button type="button" class="mvk unk" data-settype="${mi}" title="Brave or HP, ground or air can't be worked out from its first press: Set type">?</button>` : ''); };
    const rtTag = (m) => (typeof ckMoveHooks === 'function' && ckMoveHooks(m).length ? '<em class="mvk rt" title="Uses runtime hooks: in game it needs the recomp (see the move panel)">recomp</em>' : '');   // after the name (2026-11-01)
    // what a press plays, by name (2026-11-01): the move that owns the animation when another one does, else its
    // first hit, else its own "Move 1/3"; the raw animation id stays in the tooltip
    const fam = (m) => { const p = m.chainOf != null ? S.moves.find((x) => x.id === m.chainOf) : null; return [m, ...(m.chains || []), ...(p ? [p, ...(p.chains || [])] : [])].map((x) => x.name); };
    const pressName = (m, s) => {
      if (s.motion < 0) return '?';
      const names = fam(m), other = (S.usedBy.get(s.motion) || []).find((u) => /\s\d+\/\d+$/.test(u) && !names.some((n) => u.startsWith(n + ' ') && /^\d+\/\d+$/.test(u.slice(n.length + 1))));
      if (other) return other;
      const p = S.tlOfMotion.get(s.motion), t = p && F(p), hs = t ? t.ranged.filter((e) => e.hit !== undefined).sort((a, b) => a.start - b.start) : [];
      const hn = hs.map((e) => hitEvName(e)).find((n) => n && !/^0x/i.test(n));
      return hn || `${m.name} ${s.i + 1}/${m.steps.length}`;
    };
    const typing = document.activeElement && document.activeElement.id === 'mvQ';
    L.innerHTML = `<div class="fxu-find"><label class="tl-search" title="A move's name or one of its animations (_603A)"><i class="ic ic-search" aria-hidden="true"></i><input type="search" id="mvQ" placeholder="Search moves" aria-label="Search moves" autocomplete="off" spellcheck="false" value="${esc(S.mvQ || '')}"></label></div>`
      + (mvCanEdit() ? '<button type="button" id="newMoveBtn" class="newmove" title="A new ability with its own command, presses and animation">+ New move</button>' : '')
      + mvGrouped().map(({ m, mi, head, chainRow }) => {
        if (head) return `<div class="mv-group">${esc(head)}</div>`;
        const mq = esc([m.name, ...m.steps.map((s) => (s.motion >= 0 ? motionName(s.motion) : ''))].join(' ').toLowerCase());
        if (chainRow) {                                  // an HP follow-up, under its combo: picks it as the finisher and plays the whole chain
          // only while its combo is closed (2026-11-01: "when open this is awkward"): an open combo shows it as its finisher
          // (it stays in the list, hidden, so a search still finds it: mvFilter)
          const pi = S.moves.findIndex((x) => x.id === m.chainOf), pm = S.moves[pi];
          const pch = pm ? (pm.chains || []).map((c) => S.moves.indexOf(c)) : [], pcur = S.step && (S.step.move === pi || pch.includes(S.step.move));
          const inOpen = !!pm && (pcur ? S.openMove !== -1 : S.openMove === pi);
          const k = mvKindGuess(m), on = S.step && S.step.move === mi;
          return `<div class="move chain${inOpen ? ' in-open' : ''}${m.disabled ? ' off' : ''}${on ? ' sel' : ''}${on && S.moveOnly ? ' whole' : ''}" data-q="${mq}"${inOpen ? ' hidden' : ''}>
      <div class="mv-head" data-m="${mi}" data-chain="1" title="${esc(`${m.name}: the follow-up of ${(S.moves.find((x) => x.id === m.chainOf) || {}).name || 'its combo'} (from press ${m.chainAt + 1}) · command ${m.hexId}${m.info ? ' · ability ' + m.info.key : ''}`)}" role="button" tabindex="0"><span class="mv-ar" aria-hidden="true">↳</span><i class="mvg ${k.type === 'hp' ? 'hp' : 'brave'}" title="${k.type === 'hp' ? 'Plays on □' : 'Plays on ○'}" aria-label="${k.type === 'hp' ? 'On square' : 'On circle'}"></i><b>${esc(m.name)} <small class="mv-var">· ${k.type === 'hp' ? 'HP' : 'Brave'}</small>${m.disabled ? ' <small>off</small>' : ''}</b><span class="mv-n">${m.steps.length - m.chainAt}</span></div></div>`;
        }
        const chains = (m.chains || []).map((c) => S.moves.indexOf(c)).filter((x) => x >= 0);
        const onChain = S.step && chains.includes(S.step.move);
        const cur = S.step && (S.step.move === mi || onChain), open = cur ? S.openMove !== -1 : S.openMove === mi;
        const fin = mvFinisher(mi);                      // -1 brave, else the chain move's index
        const tip = `${m.name}${m.branchAt != null ? ` (a branch: ${m.branchAt > 0 ? `from press ${m.branchAt + 1}` : 'instead of it'} it plays ${m.branchTag || 'other actions'})` : m.variantOf != null ? ' (a shorter version)' : ''} · command ${m.hexId}${m.info ? ' · ability ' + m.info.key : ''}${m.info && m.info.type ? ' · ' + m.info.type + (m.info.phase ? ', ' + m.info.phase : '') : ''}${m.disabled ? ' · switched off in mod.json' : ''}`;
        const stepBtn = (s, owner, label, sel) => `<button type="button" class="step${sel ? ' sel' : ''}" data-m="${owner}" data-s="${s.i}" title="${esc(s.motion >= 0 ? motionName(s.motion) + ' · ' : '')}press ${s.i + 1}: action ${s.action}, script ${s.script}${s.flags & 0x100 ? ', needs a press' : ', follows on hit'}"><i class="st-n ${typeof mvPressCls === 'function' ? mvPressCls(s.action) : ''}">${s.i + 1}</i><span class="st-trig">${label}</span><small class="st-m st-name">${esc(pressName(S.moves[owner], s))}</small><small class="st-len">${s.motion >= 0 ? motionLen(s.motion) + 'f' : ''}</small></button>`;
        const trig = (s) => s.i === 0 ? 'Start' : s.flags & 0x100 ? 'Press again' : 'On hit';
        let steps = '';
        if (open) {
          const here = (s) => cur && !S.moveOnly && S.step.i === s.i;
          const at = chains.length ? Math.min(...chains.map((ci) => S.moves[ci].chainAt)) : m.steps.length;   // the presses both share
          steps = m.steps.slice(0, at).map((s) => stepBtn(s, mi, trig(s), here(s) && S.step.move === mi || (onChain && !S.moveOnly && S.step.i === s.i))).join('');
          if (chains.length) {
            const opts = [`<option value="-1"${fin < 0 ? ' selected' : ''}>The combo's own${at >= m.steps.length ? ' · ends on press ' + m.steps.length : ''}</option>`, ...chains.map((ci) => `<option value="${ci}"${fin === ci ? ' selected' : ''}>${mvKind(S.moves[ci]).type === 'hp' ? 'HP' : 'Brave'} · ${esc(S.moves[ci].name)}</option>`)].join('');
            steps += `<label class="step fin" title="The combo can end on its last brave press or chain into an HP attack: pick which finisher the list, the timeline and Play show"><i class="st-n fin-ar">↳</i><span class="st-trig">Finisher</span><select class="fin-pick" data-m="${mi}" aria-label="Finisher of ${esc(m.name)}">${opts}</select></label>`;
            if (fin >= 0) { const c = S.moves[fin]; steps += c.steps.slice(c.chainAt).map((s) => stepBtn(s, fin, (mvKind(c).type === 'hp' ? 'HP · ' : 'Brave · ') + esc(c.name), cur && !S.moveOnly && S.step.move === fin && S.step.i === s.i)).join(''); }
            else steps += m.steps.slice(at).map((s) => stepBtn(s, mi, 'Brave · ' + trig(s), here(s) && S.step.move === mi)).join('');
          }
          steps = `<div class="steps">${steps}</div>`;
        }
        return `<div class="move${m.disabled ? ' off' : ''}${cur ? ' sel' : ''}${cur && S.moveOnly && !onChain ? ' whole' : ''}${open ? ' open' : ''}" data-q="${mq}">
      <div class="mv-head" data-m="${mi}" title="${esc(tip)}" role="button" tabindex="0" aria-expanded="${open}">${kindTag(m, mi)}<b>${m.branchAt != null ? `<span class="mv-branch">↳ ${m.branchEx ? 'EX Mode' : 'Variant'}${m.branchAt > 0 ? ` · from press ${m.branchAt + 1}` : ''}</span> <small class="mv-var mono">${esc(m.branchTag || '')}</small>` : esc(m.name)}${m.variantOf != null && m.branchAt == null ? ` <small class="mv-var">· ${m.steps.length} press${m.steps.length > 1 ? 'es' : ''}</small>` : ''}${m.disabled ? ' <small>off</small>' : ''}</b>${rtTag(m)}<span class="mv-n">${m.steps.length}</span><span class="mv-chev" data-m="${mi}" aria-hidden="true">▸</span></div>${steps}</div>`;
      }).join('');
    L.querySelectorAll('.fin-pick').forEach((sel) => { sel.onclick = (e) => e.stopPropagation(); sel.onchange = () => {
      const pm = +sel.dataset.m, v = +sel.value; S.finisher = S.finisher || {}; S.finisher[S.moves[pm].id] = v;
      const tgt = v >= 0 ? { move: v, i: S.moves[v].chainAt } : { move: pm, i: Math.min(S.moves[pm].steps.length - 1, Math.min(...S.moves[pm].chains.map((c) => c.chainAt))) };
      S.whole = null; S.step = tgt; S.moveOnly = false; S.sel = null; S.newMove = null; setMotion(S.moves[tgt.move].steps[tgt.i].motion); renderAll();
    }; });
    L.querySelectorAll('button.step').forEach((b) => b.onclick = () => { const m = +b.dataset.m, s = +b.dataset.s; S.whole = null; S.step = { move: m, i: s }; S.moveOnly = false; S.sel = null; S.newMove = null; setMotion(S.moves[m].steps[s].motion); renderAll(); });
    const openMove = (m0, chev) => {
      const fin = mvFinisher(m0), m = fin >= 0 ? fin : m0;   // with the HP finisher picked, the whole move is the HP chain
      const cur = S.step && (S.step.move === m0 || S.step.move === m), open = cur ? S.openMove !== -1 : S.openMove === m0;
      if (chev && open) { S.openMove = cur ? -1 : null; renderLeft(); return; }   // the chevron folds it again
      if (!S.moves[m].steps.length) return;
      if (S.whole && S.whole.move !== m) S.whole = null;
      if (!cur || !S.moveOnly) { S.step = { move: m, i: 0 }; S.whole = null; setMotion(S.moves[m].steps[0].motion); }
      S.openMove = null; S.newMove = null;
      mvSelectWhole();                                 // the whole move: play goes press after press (a press is picked below)
    };
    const headGo = (h, chev) => {
      S.finisher = S.finisher || {};
      if (h.dataset.chain) { const c = S.moves[+h.dataset.m]; S.finisher[c.chainOf] = +h.dataset.m; }   // a follow-up row: the finisher its combo shows
      else if (S.moves[+h.dataset.m] && (S.moves[+h.dataset.m].chains || []).length) S.finisher[S.moves[+h.dataset.m].id] = -1;   // the combo's name: the combo itself (its Details), not the picked finisher (2026-11-02: Armiger Assault showed Katana of the Warrior's Details)
      openMove(+h.dataset.m, chev);
    };
    L.querySelectorAll('.mv-head').forEach((h) => {
      h.onclick = (e) => { if (e.target.closest('[data-settype]')) return; headGo(h, !!e.target.closest('.mv-chev')); };
      h.onkeydown = (e) => { if (e.target !== h) return; if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); headGo(h, false); } };
    });
    L.querySelectorAll('[data-settype]').forEach((b) => b.onclick = (e) => {   // the move's panel, at its Type section
      e.stopPropagation(); const mi = +b.dataset.settype;
      if (!(S.step && S.step.move === mi && S.moveOnly)) openMove(mi, false);
      const t = $('mvType'); if (t) { t.scrollIntoView({ block: 'start' }); t.classList.remove('flash'); void t.offsetWidth; t.classList.add('flash'); }
    });
    const nb = $('newMoveBtn'); if (nb) nb.onclick = mvStartNew;
    const mq = $('mvQ'); if (mq) { mq.oninput = () => { S.mvQ = mq.value; mvFilter(L); }; mq.onkeydown = (e) => { if (e.key === 'Escape') { e.stopPropagation(); if (mq.value) { mq.value = ''; mq.oninput(); } else mq.blur(); } }; }
    mvFilter(L);
    if (typing && mq) { mq.focus(); mq.setSelectionRange(mq.value.length, mq.value.length); }
  } else {
    const n = S.gmo ? S.gmo.motions.length : Math.max(...S.tlOfMotion.keys()) + 1;
    let h = '';
    for (let i = 0; i < n; i++) {
      const u = S.usedBy.get(i) || [];
      h += `<div class="anim-row${i === S.motion ? ' sel' : ''}" data-i="${i}" tabindex="0"><span class="mono">${esc(motionName(i))}</span><small>${esc(u.join(', ') || (S.tlOfMotion.has(i) ? '' : 'no events'))}</small><small class="mono">${motionLen(i)}f</small></div>`;
    }
    L.innerHTML = h;
    L.querySelectorAll('.anim-row').forEach((r) => r.onclick = () => { S.step = null; S.sel = null; setMotion(+r.dataset.i); renderAll(); });
  }
}

/* ====================================================== timeline */
const LABEL_W = 200;
const PX = () => parseFloat($('zoom').value);
const curTLPath = () => S.tlOfMotion && S.tlOfMotion.get(S.motion);
const curTL = () => { const p = curTLPath(); return p ? F(p) : null; };
function setMotion(mi) { S.fxEdit = null; S.projOnly = null; S.motion = mi; S.frame = 0; if (!S.whole) playAuto(); pose(); }
/* the play button is a mode: while it's on, whatever you pick next (an animation, a press, an effect) plays from the start;
   paused, it stays paused. An animation that ends without Loop doesn't turn it off. */
function playAuto() { S.playing = !!S.autoplay; if ($('playBtn')) $('playBtn').textContent = S.playing ? '❚❚' : '▶'; }
function setFrame(f) { S.frame = Math.max(0, Math.min(f, motionLen(S.motion))); pose(); updatePlayhead(); }
const fxuShortName = (r) => ((S.fx && S.fx.fep.names[r.effect]) || {}).name ? S.fx.fep.names[r.effect].name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '') : '#' + r.effect;
/* effects an event plays without naming them (2026-10-28: "This GS120 Katana hit isn't in the timeline"): a hit
   window's contact effect (the hit's hit_effect_bit group) and a shot's visuals, by row, for the lane labels */
function impliedFx(t, mo = S.motion, ph = stepPhase()) {
  const out = new Map(); if (!S.fx || !t || typeof fxInstances !== 'function') return out;
  let insts = [];
  try { insts = fxInstances(S.fx, t, { hitOf, motionName: motionName(mo), phase: ph, ex: !!S.exMode }); } catch (e) { return out; }
  for (const x of insts) {
    if (x.type !== 'hit' && x.type !== 'proj') continue;
    const k = x.kind + ':' + x.idx, names = [...new Set(x.recs.map((r) => fxuShortName(r)))];
    out.set(k, [...new Set([...(out.has(k) ? out.get(k).split(', ') : []), ...names])].join(', '));
  }
  return out;
}
/* the effect a bone-effect / trail event plays, by name (request: "Can we show the name of the effect instead of this in
   the timeline"): the records fx.js picks for the row (a weapon trail's are found by its bone, record 9999), else the
   event's own effect id */
function boneFxNames(t, mo = S.motion, ph = stepPhase()) {
  const out = new Map(); if (!S.fx || !t || typeof fxInstances !== 'function') return out;
  let insts = [];
  try { insts = fxInstances(S.fx, t, { hitOf, motionName: motionName(mo), phase: ph, ex: !!S.exMode }); } catch (e) { return out; }
  for (const x of insts) if (x.type === 'fx' && x.recs && x.recs.length) out.set(x.kind + ':' + x.idx, [...new Set(x.recs.map((r) => fxuShortName(r)))].join(', '));
  return out;
}
function evFxLabel(r, bfx, tl = curTL()) {      // a lane label naming the effect, or null to keep the generic one
  const oc = opCode(r.e.op), a = argsOf(r.e);
  if (oc === 0x042A) {
    let nm = bfx.get(r.kind + ':' + r.idx) || (a[1] !== 9999 && fxInfoCached(a[1]) ? fxuShort(fxInfoCached(a[1]).name) : ''), more = '';
    if (!nm && r.kind === 'ranged') {            // a range on a slot that is already running: the game continues that one
      const t = tl, slot = (a[0] || 0) & 15;
      const prev = t ? t.ranged.map((e, i) => ({ e, i })).filter((x) => x.i !== r.idx && opCode(x.e.op) === 0x042A && ((argsOf(x.e)[0] || 0) & 15) === slot && x.e.start <= r.e.start && bfx.has('ranged:' + x.i)).sort((p, q) => q.e.start - p.e.start)[0] : null;
      if (prev) { nm = bfx.get('ranged:' + prev.i); more = ' (continued)'; }
    }
    return nm ? `${a[1] === 9999 ? 'Trail' : 'Bone effect'} · ${nm}${more}` : null;
  }
  if ([0x0219, 0x042F, 0x0466].includes(oc) && !(oc === 0x0219 && r.kind === 'ranged')) { const fi = fxInfoCached(a[0]); return fi ? `${r.info.label} · ${fxuShort(fi.name)}` : null; }
  return null;
}
function motionBoundFx(mo = S.motion) { // as fx.js fxInstances: model records whose motion name is this animation, attach bit 0x80 clear
  if (!S.fx || !S.fx.fepa || mo < 0) return [];
  const mn = motionName(mo);
  return S.fx.fepa.filter((r) => r.model && r.motion === mn && !(r.attach & 0x80));
}
function mfxInspector(I, r) {
  const nm = fxuShortName(r), mn = motionName(S.motion);
  I.innerHTML = `<div class="sec"><h3>Effect · ${esc(nm)} <small>with the animation</small></h3>
    <p class="note">No event plays this. When ${esc(mn)} starts, the game plays every effect placement in the character's effect package whose animation name is <b class="mono">${esc(mn)}</b>, so this one always plays${r.delay > 0 ? ` (after ${r.delay / 2} frames)` : ''}, on <b class="mono">${esc(r.bone || 'the character')}</b>.</p>
    <p class="note">Placement <b class="mono">${r.id}</b>${r.id >= 500 ? '' : ', inherited from the effect package this character was built from'}.</p>
    <div class="btnrow"><button type="button" id="mfxUnbind" class="danger">Stop it playing with this animation</button><button type="button" id="mfxEdit">Edit effect ${esc(nm)} ›</button></div>
    <p class="note">Stopping clears the placement's animation name, so it only plays where an event asks for effect ${r.id} (no event does now). Undo puts it back.</p></div>
    <div class="sec"><h3>Placement <small class="mono">${r.id}</small></h3>
    ${fxBoneField({ recs: [r] })}${fxuVecField('Rotation (°)', 'evfxRot', r.rot.map(fmt3))}${fxuVecField('Size', 'evfxScl', r.scale.map(fmt3))}${fxuVecField('Offset (m)', 'evfxPos', r.pos.map(fmt3))}
    <p class="note">Relative to <b class="mono">${esc(r.bone || 'the character')}</b>.</p></div>`;
  [['evfxPos', 0], ['evfxRot', 12], ['evfxScl', 24]].forEach(([nm, o]) => [0, 1, 2].forEach((k) => { const el = $(nm + k); if (el) el.onchange = () => { const v = +el.value; if (!isFinite(v)) return;
    fxuEdit((pkg) => { fxeDV(pkg.fepa.records[r.index]).setFloat32(o + 4 * k, v, true); }, true);
    const nr = S.fx.fepa.find((x) => x.index === r.index); if (nr) mfxInspector(I, nr); }; }));
  if ($('evfxBone')) $('evfxBone').onchange = () => { const b = $('evfxBone').value; if (!b || b === r.bone) return;
    fxuEdit((pkg) => { fxeSetStr(pkg.fepa.records[r.index], 0xA8, b); }, true);
    const nr = S.fx.fepa.find((x) => x.index === r.index); if (nr) mfxInspector(I, nr); };
  $('mfxUnbind').onclick = () => { fxuEdit((pkg) => { fxeSetStr(pkg.fepa.records[r.index], 0xC8, ''); }, true); S.mfxSel = null; renderTimeline(); renderInspector(); };
  $('mfxEdit').onclick = () => { S.mfxSel = null; fxuOpen(r.effect); };
}
function timelineRows(t) {
  const rows = [];
  if (!t) return rows;
  for (const kind of ['ranged', 'keyed']) t[kind].forEach((e, idx) => { if (!isSetupHide(e, kind)) rows.push({ kind, idx, e, info: opInfo(e, kind) }); });
  const ci = (c) => { const i = OPS.cats.indexOf(c); return i < 0 ? 99 : i; };
  rows.sort((a, b) => ci(a.info.cat) - ci(b.info.cat) || (a.e.start ?? a.e.frame) - (b.e.start ?? b.e.frame) || a.idx - b.idx);
  return rows;
}
const isHexOp = (e) => typeof e.op === 'string' && e.op.startsWith('0x');
function argSummary(e, info) {
  const n = isHexOp(e) ? null : info.args[0];
  if (!n || e[n] === undefined) return '';
  const v = e[n];
  if (n === 'effect' && S.effects.has(v)) return S.effects.get(v).name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, '');
  if (n === 'projectile') { const a = S.projAtkp.get(v); const h = a != null ? hitOf(a) : null; return '#' + v + (h ? ' → ' + h.name : ''); }
  // names, not numbers: sounds and voices by name (else their slot), face groups and footstep kinds by what they are
  if (n === 'sound' && typeof edCueName === 'function') return edCueName(v);
  if (n === 'voice' && typeof edVoiceName === 'function') return edVoiceName(v);
  if (n === 'face_part' && typeof edFaceGroup === 'function') {
    const fg = typeof FT !== 'undefined' && FT.groups ? FT.groups.find((x) => x.id === v) : null, pt = fg && fg.pats.get(e.pattern);
    return edFaceGroup(v) + (e.pattern !== undefined ? ' · ' + (pt && pt.name ? pt.name.toLowerCase().replace(/_/g, ' ') : 'pattern ' + e.pattern) : '');
  }
  if (n === 'kind' && opCode(e.op) === 0x060F && typeof ED_FOOT_KIND !== 'undefined') return ED_FOOT_KIND[v] || 'kind ' + v;
  if (typeof v === 'object') return fmt3(v.f);
  return String(v);
}
/* placeholders a text field understands, shown under it while it has focus: click one to put it at the cursor */
function tokenHelper(el, tokens) {
  if (!el || el.disabled || el.readOnly) return;
  const row = document.createElement('div');
  row.className = 'tok-row'; row.hidden = true;
  row.innerHTML = '<span>Insert</span>' + tokens.map(([t, d]) => `<button type="button" class="tok" data-tok="${esc(t)}" title="${esc(d)}">${esc(t)}</button>`).join('');
  el.after(row);
  let dirty = false;
  el.addEventListener('focus', () => { row.hidden = false; });
  el.addEventListener('blur', () => { row.hidden = true; if (dirty) { dirty = false; el.dispatchEvent(new Event('change')); } });
  el.addEventListener('change', () => { dirty = false; });         // typed too: the browser's own change came first
  row.addEventListener('mousedown', (e) => e.preventDefault());    // keep the focus (and the cursor) in the field
  row.querySelectorAll('[data-tok]').forEach((b) => b.onclick = () => {
    const t = b.dataset.tok, a = el.selectionStart ?? el.value.length, z = el.selectionEnd ?? a;
    el.setRangeText(t, a, z, 'end'); dirty = true; el.focus();
  });
}
/* folded timeline categories and inspector sections, remembered per browser */
const foldLoad = (k) => { try { return new Set(JSON.parse(localStorage.getItem(k) || '[]')); } catch (e) { return new Set(); } };
const foldSave = (k, set) => { try { localStorage.setItem(k, JSON.stringify([...set])); } catch (e) { /* per-viewer only */ } };
const TL_FOLD = foldLoad('studio.tlFold'), SEC_FOLD = foldLoad('studio.secFold');
/* the timeline's Show filter (every kind, one category, or the effects that play with the animation) and its Group
   switch (by category, foldable; off = every event in time order), remembered per browser */
const TL_SHOW_OPTS = () => [['all', 'Everything'], ...OPS.cats.map((c) => [c, c]), ['mfx', 'With the animation']];
// the kinds ticked (2026-10-30: "Can this be multiselect"), or null for everything
function tlShowGet() { const el = $('tlShow'); const v = el ? [...el.selectedOptions].map((o) => o.value).filter((x) => x !== 'all') : []; return v.length ? new Set(v) : null; }
const tlShowName = (v) => (v === 'mfx' ? 'With the animation' : v);
function tlGroupGet() { return !$('tlGroup') || $('tlGroup').checked; }
function tlQuery() {                           // the search's words; "press 2" and "quoted words" stay one phrase
  const el = $('tlSearch'); if (!el) return [];
  return (el.value.toLowerCase().match(/press\s*\d+|"[^"]*"|\S+/g) || []).map((w) => w.replace(/"/g, '').replace(/^press\s*/, 'press ')).filter(Boolean);
}
(function tlFilterWire() {
  const sel = $('tlShow'), grp = $('tlGroup'); if (!sel || !grp) return;
  const get = (k) => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const put = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* per-viewer only */ } };
  let want = []; try { const raw = get('studio.tlShow'); want = raw ? (raw.startsWith('[') ? JSON.parse(raw) : [raw]) : []; } catch (e) { /* defaults */ }   // (a single kind before)
  sel.innerHTML = TL_SHOW_OPTS().map(([v, l]) => `<option value="${esc(v)}"${v === 'all' ? ' data-all' : ''}${v !== 'all' && want.includes(v) ? ' selected' : ''}>${esc(l)}</option>`).join('');
  if (get('studio.tlGroup') === '0') grp.checked = false;
  sel.onchange = () => { const s = tlShowGet(); put('studio.tlShow', JSON.stringify(s ? [...s] : [])); renderTimeline(); };
  grp.onchange = () => { put('studio.tlGroup', grp.checked ? '1' : '0'); renderTimeline(); };
  const comfy = $('tlComfy');                       // Comfortable rows: taller lanes for the Deck's touchscreen
  if (comfy) { comfy.checked = get('studio.tlComfy') === '1'; document.body.classList.toggle('tl-comfy', comfy.checked); comfy.onchange = () => { put('studio.tlComfy', comfy.checked ? '1' : '0'); document.body.classList.toggle('tl-comfy', comfy.checked); renderTimeline(); }; }
  const q = $('tlSearch');
  if (q) {
    q.closest('.tl-search').onpointerdown = (e) => { if (e.target === e.currentTarget || e.target.classList.contains('ic-search')) { e.preventDefault(); q.focus(); } };   // (not a label any more: it holds buttons)
    q.oninput = () => { q.closest('.tl-search').classList.toggle('on', !!q.value.trim()); renderTimeline(); };
    q.onkeydown = (e) => { if (e.key === 'Escape') { e.stopPropagation(); if (q.value) { q.value = ''; q.oninput(); } else q.blur(); } };
  }
})();
/* The whole move selected (2026-10-30: "When I have a full move selected the timeline just vanishes at a certain
   point. I want to be able to see the full timeline so I can check blends between presses"): every press end to end on
   one frame scale, each starting where the one before hands over (its chain window, or its end with "Full animations",
   as the preview plays them). A press's events past its hand-over are drawn faint: they only run if the next press
   comes later than the earliest it can. The playhead, ruler and arrow keys run over the whole move; picking an event
   (or scrubbing into another press) shows that press. */
function tlWhole() {                           // null, or { m, total, segs: [{ ...mvSegments, at (its first frame here), t (its timeline) }] }
  if (!S.moveOnly || !S.step || S.fxEdit || !S.moves || typeof mvSegments !== 'function') return null;
  const m = S.moves[S.step.move]; if (!m || (S.comp && typeof compObj === 'function' && compObj())) return null;
  let at = 0;
  const segs = mvSegments(m).map((x) => { const p = S.tlOfMotion.get(x.motion), r = { ...x, at, t: p ? F(p) : null }; at += x.len; return r; });
  return segs.length > 1 ? { m, segs, total: at } : null;
}
const tlAt = (W = tlWhole()) => (W && S.step && W.segs[S.step.i] ? W.segs[S.step.i].at : 0);   // where the press on show starts
function tlPick(i) {                           // the whole move: show press i (true when it changed)
  if (i == null || i === '') return false;
  i = +i;
  const W = tlWhole(), g = W && W.segs[i]; if (!g || (i === S.step.i && S.motion === g.motion)) return false;
  S.step = { move: S.step.move, i }; S.fxEdit = null; S.motion = g.motion; S.frame = Math.min(S.frame, g.len); S.sel = null; S.mfxSel = null;
  return true;
}
function tlGoto(G) {                           // the whole move: the playhead to frame G of the move
  const W = tlWhole(); if (!W) return setFrame(G);
  G = Math.max(0, Math.min(G, W.total));
  let i = W.segs.findIndex((x) => G < x.at + x.len); if (i < 0) i = W.segs.length - 1;
  const g = W.segs[i], had = !!S.sel || S.mfxSel != null;
  if (!tlPick(i)) return setFrame(G - g.at);
  S.frame = Math.max(0, Math.min(G - g.at, g.len));
  pose(); updatePlayhead(); mvStrip(); $('tlTitle').textContent = tlTitle();
  $('lanesInner').querySelectorAll('.sel, .selrow').forEach((x) => x.classList.remove('sel', 'selrow'));   // (no redraw: the ruler keeps its drag)
  if (had) renderInspector();
}
const tlTitle = () => (S.motion >= 0 ? (S.step ? S.moves[S.step.move].name + ' · ' : '') + motionName(S.motion) : 'No animation');   // the move first: a narrow bar cuts the animation id (the press number is in the frame readout)
function renderTimeline() {
  S._tlDrawn = tlSelSig();
  document.body.classList.toggle('fxmode', !!(S.fxEdit && S.fx));
  if (S.fxEdit && S.fx) { fxuTimeline(); mvStrip(); return; }
  if (S.comp && typeof compObj === 'function' && compObj()) return compTimeline();
  const inner = $('lanesInner'), t = curTL(), px = PX(), W = tlWhole();
  const L = W ? W.total : S.motion >= 0 ? motionLen(S.motion) : 60;
  $('tlTitle').textContent = tlTitle();
  // the timelines on show: the press on show, or every press of the move
  const srcs = W ? W.segs.map((x) => ({ seg: x.i, at: x.at, cut: x.len, full: x.full, t: x.t, motion: x.motion, action: x.action, why: x.why }))
    : [{ seg: null, at: 0, cut: L, full: L, t, motion: S.motion }];
  const width = LABEL_W + (L + 10) * px;
  let h = `<div class="ruler" id="ruler" style="width:${width}px">`;
  const stepF = px >= 14 ? 5 : px >= 7 ? 10 : 20;
  for (let f = 0; f <= L + 10; f += stepF) h += `<span style="left:${LABEL_W + f * px}px">${f}</span>`;
  h += '</div>';
  if (W) W.segs.forEach((x, k) => {               // every other press shaded, a line where each takes over
    if (k % 2) h += `<div class="tl-band" style="left:${LABEL_W + x.at * px}px;width:${x.len * px}px"></div>`;
    if (k) h += `<div class="tl-press" style="left:${LABEL_W + x.at * px}px" title="Press ${k + 1} takes over at frame ${+x.at.toFixed(1)}"></div>`;
  });
  if (!srcs.some((x) => x.t)) h += `<div class="lane" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">No events yet</span></div>`;
  const ci = (c) => { const i = OPS.cats.indexOf(c); return i < 0 ? 99 : i; }, rowsAll = [];
  for (const src of srcs) {
    if (!src.t) continue;
    const ph = W ? stepPhase(src.seg, src.motion) : stepPhase(), im = impliedFx(src.t, src.motion, ph), bf = boneFxNames(src.t, src.motion, ph);
    for (const r of timelineRows(src.t)) { r.src = src; r.implied = im.get(r.kind + ':' + r.idx); r.fxl = evFxLabel(r, bf, src.t); rowsAll.push(r); }
  }
  const at0 = (r) => r.src.at + frames(r.e.start ?? r.e.frame);
  if (W) rowsAll.sort((a, b) => ci(a.info.cat) - ci(b.info.cat) || at0(a) - at0(b) || a.src.seg - b.src.seg || a.idx - b.idx);
  const isSel = (r) => S.sel && S.sel.kind === r.kind && S.sel.idx === r.idx && (!W || r.src.seg === S.step.i);
  const dS = (src) => (W ? ` data-s="${src.seg}"` : '');
  const span = (r) => { const a = frames(r.e.start); return [a, r.e.end === FOREVER ? r.src.full : Math.max(frames(r.e.end), a + 0.25)]; };
  const cuts = (src) => W && src.cut < src.full - 0.01;              // this press is handed over before it ends
  const pn = (src) => (W ? `<i class="pk tl-pn ${src.why ? '' : (typeof mvPressCls === 'function' ? mvPressCls(src.action) : '')}" title="${src.why ? esc(src.why) : 'press ' + (src.seg + 1)}">${src.why ? '↳' : src.seg + 1}</i>` : '');
  const evLane = (r) => {                          // one event's lane: its label and its bar
    const sel = isSel(r), src = r.src, off = typeof evMode === 'function' && !evMode(r.e) ? ' off' : '';
    const col = `var(${evColVar(r.info)})`, sum = argSummary(r.e, r.info);
    const label = (r.fxl || r.info.label + (sum !== '' ? ' · ' + sum : '')) + (r.implied ? ' · ✦ ' + r.implied : '');
    // inside a category group a lane that names its hit or effect drops the kind in front ("BR_L_RUSH2 · ✦ GST120_KATANA_HIT"),
    // so the 200 px label shows the useful part; the tooltip keeps the whole label
    const own = group ? (r.info.args[0] === 'hit' && sum !== '' ? sum : FX_EV_OPS.includes(opCode(r.e.op)) && r.fxl ? r.fxl.replace(/^[^·]*· /, '') : null) : null;   // ungrouped: the whole label (it said "false")
    const shown = own != null ? own + (r.implied ? ' · ✦ ' + r.implied : '') : label;
    const pre = W ? `Press ${src.seg + 1} · ` : '', after = 'after the next press takes over: runs only if it comes later than the earliest it can';
    let bar;
    if (r.kind === 'ranged') {
      const [a, en] = span(r), past = cuts(src) && a >= src.cut - 0.001, vis = cuts(src) && !past && en > src.cut ? src.cut : en;
      bar = `<div class="ev${r.e.end === FOREVER ? ' open' : ''}${sel ? ' sel' : ''}${off}${past ? ' past' : ''}" data-k="r" data-i="${r.idx}"${dS(src)} style="left:${LABEL_W + (src.at + a) * px}px;width:${Math.max(3, (vis - a) * px)}px;background:${col}" title="${esc(pre + label)}: ${fmtF(r.e.start)}–${fmtF(r.e.end)}${past ? ' (' + after + ')' : ''}"><span class="h l"></span><span class="h r"></span></div>`;
      if (vis < en) bar += `<div class="evtail" style="left:${LABEL_W + (src.at + vis) * px}px;width:${(en - vis) * px}px;background:${col}" title="${esc(label)}: the part ${after}"></div>`;
    } else {
      const past = cuts(src) && frames(r.e.frame) >= src.cut - 0.001;
      bar = `<div class="ev key${sel ? ' sel' : ''}${off}${past ? ' past' : ''}" data-k="k" data-i="${r.idx}"${dS(src)} style="left:${LABEL_W + (src.at + frames(r.e.frame)) * px}px;background:${col}" title="${esc(pre + label)} @ ${fmtF(r.e.frame)}${past ? ' (' + after + ')' : ''}"></div>`;
      const c = opCode(r.e.op);
      if (c === 0x0219 || c === 0x042F) {                // how long the effect plays
        const fi = fxInfoCached(argsOf(r.e)[0]);
        if (fi && fi.duration) bar = `<div class="fxtail" style="left:${LABEL_W + (src.at + frames(r.e.frame)) * px}px;width:${(fi.duration / 2) * px}px;background:${col}" title="${esc(fi.name)}: plays ${fi.duration / 2} frames"></div>` + bar;
      }
    }
    // ungrouped, a dot in the category's colour says which group the lane belongs to
    const dot = group ? '' : `<i class="tl-cat" style="background:${col}" title="${esc(r.info.cat)}"></i>`;
    return `<div class="lane${sel ? ' selrow' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-k="${r.kind[0]}" data-i="${r.idx}"${dS(src)} title="${esc(pre + (own != null ? label + ': ' : '') + (r.info.desc || ''))}">${dot}${pn(src)}${esc(shown)}</span>${bar}</div>`;
  };
  // effect placements the game plays by themselves when this animation starts (no event): Bartz's package has some
  // (2026-10-28: Blitz Attack showed two sparks with one event)
  const mfxSt = (r) => Math.max(0, r.delay) / 2;
  const mfxLane = ({ r, src }) => {
    const nm = fxuShortName(r), fi = fxInfoCached(r.id), st = mfxSt(r), len = fi && fi.duration ? fi.duration / 2 : 10;
    const sel = S.mfxSel === r.index && !S.sel && (!W || src.seg === S.step.i);
    const dot = group ? '' : '<i class="tl-cat" style="background:var(--c-fx, var(--c-other))" title="With the animation"></i>';
    return `<div class="lane${sel ? ' selrow' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-mfx="${r.index}"${dS(src)} title="Plays with the animation, no event: placement ${r.id} on ${esc(r.bone || 'the character')}">${dot}${pn(src)}Effect · ${esc(nm)} · ${esc(r.bone || '')}</span><div class="ev mfx${sel ? ' sel' : ''}" data-mfx="${r.index}"${dS(src)} style="left:${LABEL_W + (src.at + st) * px}px;width:${Math.max(3, len * px)}px;background:var(--c-fx, var(--c-other))" title="${esc(nm)}: plays from frame ${st} with the animation (no event)"></div></div>`;
  };
  // the timeline's Show filter and Group switch (2026-10-30: "you can choose to only show hits for example and all
  // the other groups disappear. You can disable grouping to see everything side by side")
  // and the search (2026-10-30: "Lets add a search here"): every word must be found in the lane's text (its label,
  // effect and hit names, kind, event code, the press number "press 2")
  const show = tlShowGet(), group = tlGroupGet(), q = tlQuery();
  // what a projectile event sets off, in lanes under it (projtl.js)
  const ptlSub = (r) => (typeof ptlLanes === 'function' ? ptlLanes(r, { px, width }) : '');
  const hit = (txt) => !q.length || q.every((w) => txt.includes(w));
  // the category name only counts when the search is one (request: "sound" found every effect, under "Effects & sound"):
  // the whole query is a category's name or its first word
  const qs = q.join(' '), catQ = !!qs && OPS.cats.some((c) => { const lc = c.toLowerCase(); return qs === lc || qs === lc.split(/\s/)[0]; });
  const rowText = (r) => [r.fxl, r.info.label, argSummary(r.e, r.info), r.implied, catQ ? r.info.cat : '', r.info.name, typeof r.e.op === 'string' ? r.e.op : '', '0x' + opCode(r.e.op).toString(16).padStart(4, '0'), W ? 'press ' + (r.src.seg + 1) : ''].filter(Boolean).join(' ').toLowerCase();
  const rowsShown = rowsAll.filter((r) => (!show || show.has(r.info.cat)) && hit(rowText(r)));
  const mbs = (!show || show.has('mfx') ? srcs.flatMap((src) => motionBoundFx(src.motion).map((r) => ({ r, src }))) : [])
    .filter(({ r, src }) => hit(['effect', fxuShortName(r), r.bone || '', 'with the animation', W ? 'press ' + (src.seg + 1) : ''].join(' ').toLowerCase()));
  if (srcs.some((x) => x.t) && !rowsShown.length && !mbs.length) h += `<div class="lane" style="width:${width}px"><span class="lane-label tl-none" style="width:${LABEL_W}px">${q.length ? `No events match “${esc($('tlSearch').value.trim())}”${show ? ' under ' + esc([...show].map(tlShowName).join(', ')) : ''}` : !show ? 'No events yet' : `Nothing under ${esc([...show].map(tlShowName).join(', '))} here`}</span></div>`;
  if (group) {
    const cats = [];
    for (const r of rowsShown) { if (!cats.length || cats[cats.length - 1].cat !== r.info.cat) cats.push({ cat: r.info.cat, rows: [] }); cats[cats.length - 1].rows.push(r); }
    for (const g of cats) {
      const folded = TL_FOLD.has(g.cat) && !g.rows.some(isSel) && !q.length;   // the selected event's stays open, and every group while searching
      // a folded category keeps one lane with its events drawn small, so their timing still shows
      const marks = folded ? g.rows.map((r) => r.kind === 'ranged'
        ? (([a, en]) => `<div class="ev mini" style="left:${LABEL_W + (r.src.at + a) * px}px;width:${Math.max(3, (en - a) * px)}px;background:var(${evColVar(r.info)})"></div>`)(span(r))
        : `<div class="ev key mini" style="left:${LABEL_W + (r.src.at + frames(r.e.frame)) * px}px;background:var(${evColVar(r.info)})"></div>`).join('') : '';
      h += `<div class="lane cat${folded ? ' folded' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-fold="${esc(g.cat)}" title="${folded ? 'Show' : 'Fold'} ${esc(g.cat)}"><i class="ic ic-chevron-down fold-ch" aria-hidden="true"></i>${esc(g.cat)}<small>${g.rows.length}</small></span>${marks}</div>`;
      if (!folded) for (const r of g.rows) h += evLane(r) + ptlSub(r);
    }
    if (mbs.length) {
      h += `<div class="lane cat" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" title="Effect placements in the character's effect package whose animation name is this animation: the game plays them when it starts, with no event">With the animation<small>${mbs.length}</small></span></div>`;
      for (const m of mbs) h += mfxLane(m);
    }
  } else {                                       // one list in time order, every kind side by side
    const items = [...rowsShown.map((r) => ({ t: at0(r), c: ci(r.info.cat), html: () => evLane(r) + ptlSub(r) })), ...mbs.map((m) => ({ t: m.src.at + mfxSt(m.r), c: 99, html: () => mfxLane(m) }))];
    items.sort((p, q) => p.t - q.t || p.c - q.c);
    for (const it of items) h += it.html();
  }
  h += `<div class="endline" style="left:${LABEL_W + L * px}px"></div><div class="playhead" id="playhead"></div>`;
  inner.innerHTML = h; inner.style.width = width + 'px';
  inner.querySelectorAll('[data-mfx]').forEach((el) => el.onclick = () => { if (tlPick(el.dataset.s)) pose(); S.sel = null; S.mfxSel = +el.dataset.mfx; renderTimeline(); renderInspector(); });
  mvStrip();
  updatePlayhead();
  inner.querySelectorAll('.lane-label[data-k]').forEach((el) => el.onclick = () => { tlPick(el.dataset.s); selectEvent(el.dataset.k === 'r' ? 'ranged' : 'keyed', +el.dataset.i, true); });
  { const labs = tlLabels(), cur = inner.querySelector('.lane.selrow .lane-label[data-k]') || labs[0]; labs.forEach((el) => { el.tabIndex = el === cur ? 0 : -1; }); }   // one tab stop (tlRowStep)
  if (typeof ptlWire === 'function') ptlWire(inner);
  inner.querySelectorAll('[data-fold]').forEach((el) => el.onclick = () => { const c = el.dataset.fold; TL_FOLD.has(c) ? TL_FOLD.delete(c) : TL_FOLD.add(c); foldSave('studio.tlFold', TL_FOLD); renderTimeline(); });
  inner.querySelectorAll('.ev:not(.mini):not(.mfx)').forEach((el) => el.addEventListener('pointerdown', (ev) => startDrag(ev, el)));
  const ruler = $('ruler'), lanes = $('lanes');
  const scrub = (ev) => {                         // the pointer past an edge counts as at it, so the playhead stays in view
    const r = ruler.getBoundingClientRect(), b = lanes ? lanes.getBoundingClientRect() : r;
    const x = lanes ? Math.max(b.left + LABEL_W, Math.min(b.left + lanes.clientWidth - Math.max(6, px * 0.6), ev.clientX)) : ev.clientX;
    const G = Math.round(((x - r.left) - LABEL_W) / px * 2) / 2; if (tlWhole()) tlGoto(G); else setFrame(G);
  };
  // scrubbing past either edge (2026-10-30: "When I scrub out of view it should scroll"): the lanes scroll along
  // while the pointer is held there, at an animation pace (request: "way too fast"): 8 frames a second just past the edge,
  // up to 45 when far out, whatever the zoom. Nothing on the page is text-selected while scrubbing.
  let last = null, raf = 0, prevT = 0;
  const edge = (now) => {
    raf = 0; if (!last || !lanes) return;
    const dt = prevT ? Math.min(0.05, (now - prevT) / 1000) : 1 / 60; prevT = now;
    const b = lanes.getBoundingClientRect(), lo = b.left + LABEL_W, hi = b.left + lanes.clientWidth;
    const d = last.clientX > hi - 12 ? last.clientX - (hi - 12) : last.clientX < lo + 12 ? last.clientX - (lo + 12) : 0;
    if (!d) { prevT = 0; return; }
    const fps = Math.min(45, 8 + Math.abs(d) * 0.4), before = lanes.scrollLeft;
    S.tlEdgeAcc = (S.tlEdgeAcc || 0) + Math.sign(d) * fps * PX() * dt;   // (sub-pixel steps add up)
    const step = Math.trunc(S.tlEdgeAcc); S.tlEdgeAcc -= step;
    if (step) lanes.scrollLeft += step;
    if (lanes.scrollLeft !== before) scrub(last);
    raf = requestAnimationFrame(edge);
  };
  ruler.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();                          // no text selection starting on the ruler
    ruler.setPointerCapture(ev.pointerId); last = ev; prevT = 0; S.tlEdgeAcc = 0; scrub(ev);
    document.body.classList.add('tl-scrubbing'); const ws = window.getSelection && window.getSelection(); if (ws) ws.removeAllRanges();
    ruler.onpointermove = (m) => { last = m; scrub(m); if (!raf) raf = requestAnimationFrame(edge); };
    ruler.onpointerup = ruler.onpointercancel = () => { ruler.onpointermove = null; last = null; if (raf) cancelAnimationFrame(raf); raf = 0; document.body.classList.remove('tl-scrubbing'); };
  });
}
/* inspector sections fold by their title (click it): the title's own text is the key, so "Effects & sound", "Voice",
   "Hit" ... stay folded from one event to the next */
const secKey = (h3) => { for (const n of h3.childNodes) if (n.nodeType === 3 && n.textContent.trim()) return n.textContent.trim().toLowerCase(); return ''; };
/* a section title's tooltip says what the section is for (2026-10-31: "Rather than show or fold, we should have a
   tooltip explaining what this section is for"); matched on the title's key, first match wins; no match, no tooltip */
const SEC_HELP = [
  [/^type$/, 'What kind of attack this is (brave or HP) and where it can start'],
  [/^details$/, 'The move\'s name, help text and what it costs to learn and equip'],
  [/^ability builder$/, 'The chain attacks this move plays, one press each, in order'],
  [/^danger zone$/, 'Switch the move off, or delete it'],
  [/^new move$/, 'Name the new move and pick its attack type and where it starts'],
  [/^ex burst$/, 'The scripts the game runs for this character\'s EX Burst'],
  [/^changes$/, 'Everything Save will write, file by file'],
  [/^checks$/, 'Problems found in this character\'s files'],
  [/^layers$/, 'The effects this one stacks, each with its own timing'],
  [/^effect ·/, 'An effect that plays whenever this animation does, with no event'],
  [/^effect \d/, 'Edit this effect: its name and the pieces it is made of'],
  [/^effect$/, 'The effect this event plays: what it is and where'],
  [/^contact effect$/, 'The effect played where this hit lands'],
  [/^placement$/, 'Where the effect sits on its bone: offset, rotation and size'],
  [/^animation$/, 'The animation playing: where it is used, and open it in Blender'],
  [/^show \/ hide parts$/, 'Which model parts are drawn when this animation starts'],
  [/^hit\b/, 'The hit this window deals and what it does to the foe'],
  [/^hitboxes\b/, 'The spheres on the bones that deal this hit or clash with attacks'],
  [/^rare-variant roll$/, 'The roll that decides when the alternate effect plays'],
  [/^projectile\b/, 'The projectile this fires: how it flies and what it hits with'],
  [/^press settings$/, 'The action record behind this press: the numbers the game reads for it'],
  [/^runtime hooks$/, 'Extra rules the native build applies while this animation plays'],
  [/^sound$/, 'The sound this event plays, and the bank it comes from'],
  [/^voice$/, 'The voice line this event plays'],
  [/^pieces$/, 'The pieces this effect is made of: pick one to edit it'],
  [/^piece #/, 'One piece of the effect: when it starts, how long it lives, how it looks'],
  [/^lighting$/, 'Cel shading for this character (the native build only)'],
  [/^costumes$/, 'The three extra costume slots, each with a model of its own'],
  [/^body$/, 'Which body the model shows, and the one the game uses'],
  [/^your own model$/, 'Swap the body for a model of your own, fitted in Blender'],
  [/^weapons$/, 'Swap a weapon for a model of your own'],
  [/^travel$/, 'The movement baked into the animation: how far and how high it goes'],
  [/^menu art$/, 'The menu and HUD pictures, cut from the character\'s art'],
  [/^jump, gravity, dash$/, 'How high the character jumps, how fast they fall and how fast they dash'],
  [/^preview$/, 'What the 3D view plays: the jump, the dash or both'],
];
const SEC_HELP_ACC = { metadata: 'The move\'s ids, and where it was copied from', details: 'Where this animation is used, and how many events it has' };
function secHelp(h3, k) {
  const sm = (h3.querySelector(':scope > small') || {}).textContent || '';
  if (h3.hasAttribute('data-ev') || /one frame|start → end/.test(sm)) return 'This event: what it does, and its settings';   // an event's section: titled by its category
  if (/^press \d/.test(sm)) return 'This press of the move: the animation it plays';
  const hit = SEC_HELP.find(([re]) => re.test(k));
  return hit ? hit[1] : '';
}
function secFoldApply(root) {
  for (const sec of root.querySelectorAll('.sec')) {
    const h3 = sec.querySelector(':scope > h3'); if (!h3) continue;
    const k = secKey(h3); if (!k) continue;
    sec.classList.add('foldable'); sec.classList.toggle('folded', SEC_FOLD.has(k));
    const help = secHelp(h3, k);
    if (help) h3.title = help; else h3.removeAttribute('title');
  }
  for (const s of root.querySelectorAll('.sec details.acc > summary')) {
    const help = SEC_HELP_ACC[s.textContent.trim().toLowerCase()];
    if (help) s.title = help;
  }
}
(function secFoldWire() {
  const insp = $('insp'); if (!insp) return;
  new MutationObserver(() => secFoldApply(insp)).observe(insp, { childList: true, subtree: true });
  insp.addEventListener('click', (e) => {
    const h3 = e.target.closest('.sec > h3'); if (!h3 || e.target.closest('button, input, select, label, a, textarea, .tip')) return;
    const k = secKey(h3); if (!k) return;
    SEC_FOLD.has(k) ? SEC_FOLD.delete(k) : SEC_FOLD.add(k); foldSave('studio.secFold', SEC_FOLD);
    secFoldApply(insp);
  });
})();
function updatePlayhead() {
  const W = tlWhole(), at = tlAt(W);              // the whole move: on the move's own frame scale
  const ph = $('playhead'); if (ph) ph.style.left = (LABEL_W + (at + S.frame) * PX()) + 'px';
  $('tlFrame').textContent = S.motion < 0 ? '' : W ? `${(at + S.frame).toFixed(1)} / ${+W.total.toFixed(1)}` : `${S.frame.toFixed(1)} / ${motionLen(S.motion)}`;   // (the press is on the strip below)
  $('tlFrame').title = S.motion < 0 ? '' : tlTitle() + (W ? ` · press ${S.step.i + 1} at frame ${S.frame.toFixed(1)}` : '');
  mvStripHead();
  phClip();
}
/* the playhead never draws over the sticky label column (or past the right edge): when it is scrolled out of view a
   small arrow at that edge of the lanes points to it instead (2026-10-28); click the arrow to scroll to it */
function phClip() {
  const lanes = $('lanes'), ph = $('playhead'); if (!lanes) return;
  let a = $('phArrow');
  if (!a) {
    a = document.createElement('button'); a.type = 'button'; a.id = 'phArrow'; a.className = 'ph-arrow'; a.hidden = true;
    a.onclick = () => { const p = $('playhead'); if (p) lanes.scrollLeft = Math.max(0, parseFloat(p.style.left) - LABEL_W - (lanes.clientWidth - LABEL_W) / 3); };
    lanes.parentNode.appendChild(a);
    lanes.addEventListener('scroll', phClip); window.addEventListener('resize', phClip);
  }
  const x = ph ? parseFloat(ph.style.left) : NaN;
  const sl = lanes.scrollLeft, vw = lanes.clientWidth;
  const left = x < sl + LABEL_W, right = x > sl + vw - 1, out = ph && !isNaN(x) && (left || right);
  if (ph) ph.style.visibility = out ? 'hidden' : '';
  const hd = $('mvHead'); if (hd) hd.style.visibility = out ? 'hidden' : '';
  a.hidden = !out; if (!out) return;
  a.textContent = left ? '◀' : '▶'; a.title = 'The playhead is ' + (left ? 'to the left' : 'to the right') + ': click to scroll to it';
  const strip = $('mvStrip') ? 24 : 0;
  a.style.top = (lanes.offsetTop + strip + 2) + 'px';
  a.style.left = (left ? lanes.offsetLeft + LABEL_W + 2 : lanes.offsetLeft + vw - 18) + 'px';
}
function selectEvent(kind, idx, jump) {
  const was = S.sel;
  S.sel = { kind, idx }; S.projFocus = null;
  const e = curTL()[kind][idx];
  if (jump) setFrame(frames(kind === 'ranged' ? e.start : e.frame));
  if (!tlSelectFast(kind, idx, was)) renderTimeline();
  renderInspector(); selReveal();
}
/* the selected event's lane scrolled into view (after add, paste, duplicate, an alert, the keyboard) */
function selReveal() { const row = $('lanesInner') && $('lanesInner').querySelector('.lane.selrow'); if (row) row.scrollIntoView({ block: 'nearest' }); }
/* the timeline from the keyboard: the lane labels are one tab stop (the selected one, else the first), ↑/↓ go from row to
   row, Enter selects (and moves the playhead), Alt+←/→ nudges the selected event half a frame, Shift+Alt+←/→ its end.
   Nudges in a row (the same event and edge, within 1.5 s) are one undo step. */
const tlLabels = () => [...$('lanesInner').querySelectorAll('.lane-label[data-k]')];
const tlLabelFocused = () => { const a = document.activeElement; return !!(a && a.matches && a.matches('#lanesInner .lane-label[data-k]')); };
function tlFocusSel() { const el = $('lanesInner').querySelector('.lane.selrow .lane-label[data-k]'); if (el) { el.tabIndex = 0; el.focus({ preventScroll: true }); } }
function tlRowStep(dir) {
  const labs = tlLabels(); if (!labs.length) return;
  let i = labs.indexOf(document.activeElement); if (i < 0) i = labs.findIndex((el) => el.closest('.lane').classList.contains('selrow'));
  const n = labs[i < 0 ? 0 : Math.max(0, Math.min(labs.length - 1, i + dir))];
  if (tlPick(n.dataset.s)) pose();
  selectEvent(n.dataset.k === 'r' ? 'ranged' : 'keyed', +n.dataset.i, false); tlFocusSel();
}
function tlNudge(dir, end) {
  if (!S.sel || S.fxEdit || S.comp) return;
  if (S.readOnly) { readOnlyNote(); return; }
  const p = curTLPath(), t = p && F(p), { kind, idx } = S.sel, e = t && t[kind][idx]; if (!e) return;
  const n = {};
  if (kind === 'keyed') { if (end) return; n.frame = Math.max(0, Math.min(0xFFFE, e.frame + dir)); }
  else if (end) n.end = Math.max(e.start, Math.min(0xFFFE, (e.end === FOREVER ? ticks(motionLen(S.motion)) : e.end) + dir));
  else { const len = e.end === FOREVER ? null : e.end - e.start; n.start = Math.max(0, Math.min(0xFFFE - (len || 0), e.start + dir)); if (len != null) n.end = n.start + len; }
  if (Object.keys(n).every((k) => n[k] === e[k])) return;
  const key = [p, kind, idx, end ? 'e' : 'm'].join('|'), nu = S.nudge, again = nu && nu.key === key && nu.u === S.undo[S.undo.length - 1] && Date.now() - nu.t < 1500;
  if (again) { Object.assign(e, n); S.gen = (S.gen || 0) + 1; markDirty([p]); runChecks(); }
  else edit([p], () => { Object.assign(t[kind][idx], n); });
  S.nudge = { key, u: S.undo[S.undo.length - 1], t: Date.now() };
  const f = kind === 'keyed' ? e.frame : end ? (e.end === FOREVER ? e.start : e.end) : e.start;
  setFrame(frames(f)); renderTimeline(); renderInspector(); selReveal(); tlFocusSel();
}
/* what the drawn timeline depends on besides the selection (renderTimeline notes it as it starts) */
const tlSelSig = () => JSON.stringify([S.motion, S.step && S.step.move, S.step && S.step.i, !!S.whole, !!S.moveOnly, S.gen || 0, S.mfxSel, S.projFocus, !!S.fxEdit, S.comp && S.comp.path]);
/* only the selection moved (review 2026-11-01: every click redrew the whole timeline): move the selected marks to the
   new lane instead of redrawing. Anything else (another press, a folded category to open or close, a projectile focus)
   -> false, and the caller redraws */
function tlSelectFast(kind, idx, was) {
  const inner = $('lanesInner'); if (!inner || S._tlDrawn !== tlSelSig()) return false;
  const W = tlWhole(), seg = W ? `[data-s="${S.step.i}"]` : '';
  const lab = inner.querySelector(`.lane-label[data-k="${kind[0]}"][data-i="${idx}"]${seg}`), lane = lab && lab.closest('.lane');
  if (!lane || !lane.querySelector(`.ev[data-k="${kind[0]}"][data-i="${idx}"]`)) return false;
  const catOf = (l) => { for (let x = l && l.previousElementSibling; x; x = x.previousElementSibling) if (x.classList.contains('cat')) { const f = x.querySelector('[data-fold]'); return f && f.dataset.fold; } return null; };
  const old = inner.querySelector('.lane.selrow:not(.ptl)');
  if (was && old && TL_FOLD.has(catOf(old))) return false;   // its category stayed open only for the selection
  inner.querySelectorAll('.sel, .selrow').forEach((x) => x.classList.remove('sel', 'selrow'));
  lane.classList.add('selrow'); lane.querySelector(`.ev[data-k="${kind[0]}"][data-i="${idx}"]`).classList.add('sel');
  tlLabels().forEach((el) => { el.tabIndex = el === lab ? 0 : -1; });   // the one tab stop follows the selection
  return true;
}
function startDrag(ev, el) {
  ev.preventDefault();
  if (tlPick(el.dataset.s)) pose();               // the whole move: the event's press
  const at = tlAt(), kind = el.dataset.k === 'r' ? 'ranged' : 'keyed', idx = +el.dataset.i, p = curTLPath(), e = F(p)[kind][idx];
  const mode = ev.target.classList.contains('l') ? 'l' : ev.target.classList.contains('r') ? 'r' : 'm';
  const x0 = ev.clientX, s0 = kind === 'ranged' ? e.start : e.frame, e0 = kind === 'ranged' ? e.end : 0;
  let moved = false;
  if (S.readOnly) { selectEvent(kind, idx, true); return; }
  if (!(S.sel && S.sel.kind === kind && S.sel.idx === idx)) { S.sel = { kind, idx }; renderInspector(); }
  el.setPointerCapture(ev.pointerId);
  el.onpointermove = (m) => {
    let d = (m.clientX - x0) / PX() * 2;
    d = m.altKey ? Math.round(d) : Math.round(d / 2) * 2;
    if (!d && !moved) return;
    if (!moved) { pushUndo([p]); moved = true; }
    const ev2 = F(p)[kind][idx];
    if (kind === 'keyed') ev2.frame = Math.max(0, Math.min(0xFFFE, s0 + d));
    else if (mode === 'm') { const len = e0 === FOREVER ? null : e0 - s0; ev2.start = Math.max(0, s0 + d); if (len != null) ev2.end = Math.min(0xFFFE, ev2.start + len); }
    else if (mode === 'l') ev2.start = Math.max(0, Math.min(s0 + d, (e0 === FOREVER ? 0xFFFE : e0)));
    else { const base = e0 === FOREVER ? ticks(motionLen(S.motion)) : e0; ev2.end = Math.max(ev2.start, Math.min(0xFFFE, base + d)); }
    markDirty([p]);
    const px = PX();                                // move the bar itself; the timeline is redrawn on release
    if (kind === 'keyed') el.style.left = (LABEL_W + (at + frames(ev2.frame)) * px) + 'px';
    else {
      const en = ev2.end === FOREVER ? motionLen(S.motion) : Math.max(frames(ev2.end), frames(ev2.start) + 0.25);
      el.style.left = (LABEL_W + (at + frames(ev2.start)) * px) + 'px';
      el.style.width = Math.max(3, (en - frames(ev2.start)) * px) + 'px';
    }
    setFrame(frames(kind === 'ranged' ? (mode === 'r' ? (ev2.end === FOREVER ? ev2.start : ev2.end) : ev2.start) : ev2.frame));
  };
  el.onpointerup = () => { el.onpointermove = null; if (moved) { runChecks(); renderTimeline(); renderInspector(); } else selectEvent(kind, idx, true); };
}

/* ====================================================== inspector */
const fTip = (t) => (t && typeof edTip === 'function' ? edTip(t) : '');   // the "?" help bubble (editors.js)
/* Number fields step by what they hold (2026-10-30: "When editing a float like this, can the default be incrementing
   by 0.1 instead of 1? Can we determine which makes sense depending on the value?"). A field whose author set a step
   (frames 0.5, radii 0.05 ...) keeps it. The rest (no step, or step="any", which Chrome steps by 1): a decimal field
   (data-num="float", a float value data-f, or a value with decimals; once seen, remembered) steps 0.1, 0.01 below 0.1
   and 1 from 50; a whole number steps 1. Shift + ↑/↓ steps ×10, Alt + ↑/↓ ÷10. The step is set before each spin or key,
   so the browser's own arrows and spinner use it. */
function numStep(el) {
  if (!('step0' in el.dataset)) el.dataset.step0 = el.getAttribute('step') || '';
  const s0 = el.dataset.step0;
  if (s0 && s0 !== 'any') return parseFloat(s0) || 1;
  const v = parseFloat(el.value), dec = /[.,]\d/.test(el.value);
  if (dec || s0 === 'any' && 'f' in el.dataset) el.dataset.num = 'float';
  if (el.dataset.num !== 'float') return 1;
  const a = Math.abs(v || 0);
  return a >= 50 ? 1 : a > 0 && a < 0.1 ? 0.01 : 0.1;
}
(function numStepWire() {
  const isNum = (el) => el && el.tagName === 'INPUT' && el.type === 'number' && !el.readOnly && !el.disabled;
  const apply = (el) => { if (!isNum(el)) return; const st = numStep(el); el.step = String(st); };
  document.addEventListener('focusin', (e) => apply(e.target));
  document.addEventListener('pointerdown', (e) => apply(e.target), true);     // before a spinner click
  document.addEventListener('input', (e) => apply(e.target));
  document.addEventListener('keydown', (e) => {
    const el = e.target; if (!isNum(el) || (e.key !== 'ArrowUp' && e.key !== 'ArrowDown')) return;
    apply(el);
    if (!e.shiftKey && !e.altKey) return;                                      // the browser's own step
    e.preventDefault();
    const st = parseFloat(el.step) || 1, k = e.shiftKey ? st * 10 : st / 10, dir = e.key === 'ArrowUp' ? 1 : -1;
    const dp = Math.max(0, -Math.floor(Math.log10(k) + 1e-9));
    let v = (parseFloat(el.value) || 0) + dir * k;
    if (el.min !== '') v = Math.max(+el.min, v); if (el.max !== '') v = Math.min(+el.max, v);
    el.value = String(+v.toFixed(Math.min(6, dp)));
    el.dispatchEvent(new Event('input', { bubbles: true })); el.dispatchEvent(new Event('change', { bubbles: true }));
  }, true);
})();
/* float noise (0.899999976): values show rounded to 3 significant digits (whole numbers stay whole); the stored value
   only changes when the field is edited */
function fmt3(x) {
  if (typeof x !== 'number' || !isFinite(x) || x === 0 || Number.isInteger(x)) return x;
  return +x.toFixed(Math.min(6, Math.max(0, 2 - Math.floor(Math.log10(Math.abs(x))))));
}
/* a 0 / 1 value as the chunky two-button switch (Off / On); attrs go on the group, wired by its data attribute */
const boolSeg = (attrs, v, off = 'Off', on = 'On') => `<div class="seg bool" role="group" ${attrs}><button type="button" data-bv="0" aria-pressed="${v === 0}">${off}</button><button type="button" data-bv="1" aria-pressed="${v === 1}">${on}</button></div>`;
/* the effect picker (the character's FEPA records): Effect events, a projectile's origin effect (none = 0) */
function effectSelect(v, attrs, noneZero) {
  const opts = [...S.effects.values()].filter((x) => !(noneZero && x.id === 0)).map((x) => `<option value="${x.id}"${x.id === v ? ' selected' : ''}>${x.id} · ${esc(x.name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, ''))}${x.bones.length ? ' (' + esc(x.bones.join(', ')) + ')' : ''}</option>`).join('');
  return `<select ${attrs}>${noneZero ? `<option value="0"${v === 0 ? ' selected' : ''}>none</option>` : ''}${opts}${S.effects.has(v) || (noneZero && v === 0) ? '' : `<option selected value="${v}">${v}</option>`}</select>`;
}
const argLabel = (n) => (OPS.arg_labels && OPS.arg_labels[n]) || (n.charAt(0).toUpperCase() + n.slice(1)).replace(/_/g, ' ');
function numField(label, value, attrs = '', tip = '') { return `<div class="field"><label>${label}${fTip(tip)}</label><input type="number" ${attrs} value="${esc(value)}"></div>`; }
/* <details> sections (Parts › Hidden at start, Duplicate this animation, ...) stay open across re-renders: ticking a part
   rebuilds the panel, which used to fold the list shut. Key: the summary text without counts in brackets. */
const DET_OPEN = new Set();
const detKey = (d) => { const s = d.querySelector(':scope > summary'); return s ? s.textContent.replace(/\([^)]*\)/g, '').trim() : ''; };
(function detWire() {
  const insp = $('insp'); if (!insp) return;
  insp.addEventListener('toggle', (e) => {        // toggle does not bubble: listen in the capture phase
    const d = e.target; if (!(d instanceof HTMLDetailsElement)) return;
    const k = detKey(d); if (k) d.open ? DET_OPEN.add(k) : DET_OPEN.delete(k);
  }, true);
})();
function renderInspector() {
  // keep the panel's scroll when it is the same panel re-rendered (e.g. a part ticked)
  const I = $('insp'), sc = I && I.closest('.scroll'), top = sc ? (sc._want != null ? sc._want : sc.scrollTop) : 0, head = () => { const h = I && I.querySelector('h3'); return h ? h.textContent : ''; };
  const was = head();
  const sig = (x) => [x.tagName, x.type, x.id, x.dataset.n, x.dataset.k, x.dataset.i, x.dataset.arg, x.dataset.f].join('|');
  const act = document.activeElement, keep = I && act && I.contains(act) && act.matches('input,select,textarea') ? { s: sig(act), at: [...I.querySelectorAll('input,select,textarea')].indexOf(act), num: act.dataset.num } : null;
  renderInspector0();
  if (keep) {                                     // the field you were in, if it is still there (same place, same kind)
    const n = [...I.querySelectorAll('input,select,textarea')][keep.at];
    if (n && sig(n) === keep.s && document.activeElement !== n) { if (keep.num) n.dataset.num = keep.num; n.focus({ preventScroll: true }); }
  }
  if (I) {
    if (DET_OPEN.size) I.querySelectorAll('details').forEach((d) => { if (DET_OPEN.has(detKey(d))) d.open = true; });
    // the fold chevrons (and folded sections) now, not from the observer a moment later: the scroll is restored against
    // the panel's final height (2026-10-30: each spinner click shifted the panel down 2 px near its bottom)
    secFoldApply(I);
    if (sc && head() === was) {
      sc.scrollTop = top;
      // and again once the rest of the redraw has settled: the dummy card above the panel is redrawn in the same pass
      // and can be a few px off for a moment, which clipped the restore (2 px lower on every step)
      // (several redraws in one frame keep the first one's position: sc._want)
      if (sc._want == null) requestAnimationFrame(() => { const w = sc._want; sc._want = null; if (w != null && sc.scrollTop !== w && head() === was) sc.scrollTop = w; });
      sc._want = top;
    }
  }
  if (S.readOnly) $('insp').querySelectorAll('input, select, textarea, button:not(#ckClose)').forEach((el) => { el.disabled = true; });
}
function renderInspector0() {
  const I = $('insp');
  if (S.showChecks) return renderChecksPanel(I);
  if (S.showChanges && typeof renderChangesPanel === 'function') { if (dirtyCount()) return renderChangesPanel(I); S.showChanges = false; }
  if (!S.moves) return;
  if (S.fxEdit && S.fx) return fxuInspector(I);
  if (S.comp && typeof compObj === 'function' && compObj()) return compInspector(I);
  if (S.tab === 'fx' && S.fx && !S.sel && S.mfxSel == null && !S.newMove && typeof fxuPkgInspector === 'function') return fxuPkgInspector(I);   // until an effect is picked
  if (S.newMove) { I.innerHTML = mvNewHTML(); mvWireNew(); return; }
  if (S.tab === 'char' && !S.step && !S.sel && typeof ncInspector === 'function') return ncInspector(I);
  if (S.projOnly != null && !S.sel && typeof edProjHTML === 'function') { I.innerHTML = edProjHTML(S.projOnly, 'fired by no event'); edProjWire(S.projOnly, (nid) => { S.projOnly = nid; }); return; }   // an alert's projectile that no event fires
  if (S.mfxSel != null && !S.sel) { const r = motionBoundFx().find((x) => x.index === S.mfxSel); if (r) return mfxInspector(I, r); S.mfxSel = null; }
  const t = curTL();
  if (S.sel && t && t[S.sel.kind][S.sel.idx]) return renderEvent(I, S.sel.kind, S.sel.idx, t[S.sel.kind][S.sel.idx]);
  S.sel = null;
  let h = '';
  if (S.step && S.moveOnly) {                   // the whole move: its panel only, until a press is picked
    I.innerHTML = mvMoveHTML(); mvWire(); return;   // (each chain attack's animation opens in Blender from its own inspector)
  }
  if (S.step) h += (S.moves[S.step.move].steps[S.step.i] ? renderStepHTML() + edPressHTML() : `<div class="sec"><h3>${esc(motionName(S.motion))} <small>started by a runtime hook</small></h3><p class="note">${esc((mvSegments(S.moves[S.step.move])[S.step.i] || {}).why || '')}: not one of the move's presses; the preview shows it because the game would play it here.</p></div>`);
  h += renderAnimHTML() + edHooksHTML(S.motion);
  I.innerHTML = h;
  mvWire(); wireStep(); wireAnim();
  if (S.step && S.moves[S.step.move].steps[S.step.i]) { edPressWire(); if (typeof mvPressBtnWire === 'function') mvPressBtnWire(); }
  edHooksWire(S.motion);
}
/* "Open in Blender": the Dissidia add-on (blender/io_scene_dissidia/studio_link.py) listens on 127.0.0.1:47612 while
   Blender runs; it imports the character's rig from its mods folder if it isn't in the scene yet, then picks the
   animation and the frame. */
const studioModName = () => (S.charDir ? S.charDir.replace(/\/$/, '').split('/').pop() : (FS.base ? FS.base.split('/').pop() : (FS.root && FS.root.name) || ''));
async function blOpen(mi, b) {
  const mod = studioModName(), rig = S.rig.gmo.slice(S.charDir.length), f = S.files[S.rig.gmo];
  if (f && f.gmo && fileChanged(f)) { toast('Save first: Blender reads the rig from the folder.', 'bad'); return; }
  const q = new URLSearchParams({ mod, rig, motion: mi, name: motionName(mi), frame: mi === S.motion ? Math.round(S.frame || 0) : 0 });
  const label = b ? b.innerHTML : '';
  if (b) { b.disabled = true; b.textContent = 'Opening…'; }
  try {
    const j = await (await fetch('http://127.0.0.1:47612/open?' + q, { cache: 'no-store' })).json();
    if (j.ok) toast(`${motionName(mi)} is open in Blender${j.imported ? ' (imported ' + mod + ')' : ''}. Save moves to folder there, then Reload here.`, 'ok');
    else toast('Blender: ' + j.message, 'bad');
  } catch (e) {
    toast('Blender isn\'t answering. Open it with the Dissidia add-on (0.24+) and try again.', 'bad');
  } finally { if (b && b.isConnected) { b.disabled = false; b.innerHTML = label; } }
}
/* the animations shown (one, or every press of a move) with an "Open in Blender" each; mod characters only */
function blSectionHTML(rows, title, note) {
  if (!S.rig || S.rig.retail || !rows.length) return '';
  return `<div class="sec"><h3>${esc(title || 'Blender')}</h3>${note ? `<p class="note">${esc(note)}</p>` : ''}<div class="bl-rows">${rows.map((r) => `<div class="bl-row"><span class="mono">${esc(motionName(r.mi))}</span><small>${esc(r.note || '')}${r.note ? ' · ' : ''}${motionLen(r.mi)} f</small><button type="button" class="mini" data-blopen="${r.mi}" title="Show this animation in Blender (needs Blender open with the Dissidia add-on 0.24+)"><i class="ic ic-external-link" aria-hidden="true"></i> Open in Blender</button></div>`).join('')}</div></div>`;
}
function blWire(root) { (root || $('insp')).querySelectorAll('[data-blopen]').forEach((b) => b.onclick = () => blOpen(+b.dataset.blopen, b)); }
function renderStepHTML() {
  const m = S.moves[S.step.move], s = m.steps[S.step.i];
  const shared = S.moves.flatMap((mm) => mm.steps.filter((x) => x.script === s.script && !(mm === m && x.i === s.i)).map((x) => mm.name + ' ' + (x.i + 1)));
  const n = S.gmo ? S.gmo.motions.length : 0;
  let opts = '';
  for (let i = 0; i < n; i++) opts += `<option value="${i}"${i === s.motion ? ' selected' : ''}>${esc(motionName(i))}${S.tlOfMotion.has(i) ? '' : ' (no events)'}</option>`;
  // trimmed (2026-10-31: "Lots of the detail in here is unnecessary repetition"): no help text (the move's Details
  // has it), and the ids line is the press label's tooltip; whether it needs a press or follows on hit stays beside it
  const ids = m.burst ? `EX Burst · script ${s.script} · ${s.role}` : `command ${m.hexId} · action ${s.action} · script ${s.recordScript != null ? s.recordScript + ' → ' + s.script : s.script}`;
  const how = m.burst ? '' : s.flags & 0x100 ? ' · needs a press' : ' · follows on hit';
  return `<div class="sec"><h3>${esc(m.name)} <small title="${esc(ids)}">press ${s.i + 1} of ${m.steps.length}${how}</small></h3>
    <div class="field"><label>Animation</label><select id="stepMotion">${opts}</select></div>
    ${shared.length ? `<p class="warnline">${esc(shared.join(', '))} plays this too: a change here changes it there.</p>` : ''}
    ${typeof mvPressBtnHTML === 'function' ? mvPressBtnHTML() : ''}
    ${s.traced ? (() => { const mo = (sc) => { const x = S.motionsOfScript.get(sc); return x && x.length ? x[0] : -1; }, lnk = (sc, what) => mo(sc) >= 0 ? `<button type="button" class="mini" data-trmo="${mo(sc)}" title="Show this animation">${esc(what)}: ${esc(motionName(mo(sc)))}</button>` : '';
      return `<p class="note">Action ${s.action} runs script ${s.recordScript}, one of the game's shared scripts, and the game's code picks this character's own: script ${s.traced.start} (traced in the native build). It then goes on to</p><div class="btnrow">${[s.traced.on_hit ? lnk(s.traced.on_hit, 'on hit') : '', s.traced.on_landing ? lnk(s.traced.on_landing, 'on landing') : ''].join('')}</div><p class="note">Traced ${esc(s.traced.traced)}.</p>`; })() : ''}
    </div>`;
}
function wireStep() {
  document.querySelectorAll('[data-trmo]').forEach((b) => b.onclick = () => { setMotion(+b.dataset.trmo); renderAll(); });   // traced follow-ups
  const sm = $('stepMotion'); if (!sm) return;
  sm.onchange = () => {
    const m = S.moves[S.step.move], s = m.steps[S.step.i], p = msPath('motion_map.json'), mi = +sm.value;
    edit([p], () => { for (const r of F(p).rows) if (r.script === s.script) r.motion = mi; });
    derive(); setMotion(mi); renderAll();
    if (!S.tlOfMotion.has(mi)) toast(motionName(mi) + ' has no events yet: create its timeline in the inspector.');
  };
}
/* the Animation section (2026-10-31: "The only useful bit is the edit in blender. Perhaps we should trim this and
   have the detail in an optional accordion"): Open in Blender in its header, the animation's name, length and users on
   one line, the rest (events, the timeline hint, Duplicate) in a Details fold. Then Travel, the parts shown at start
   and the runtime hooks; the checks are in the Alerts section at the top. */
function renderAnimHTML() {
  const i = S.motion, t = curTL(), used = S.usedBy.get(i) || [], bl = S.rig && !S.rig.retail;
  let h = `<div class="sec" id="animSec"><h3>Animation${bl ? `<button type="button" class="mini" data-blopen="${i}" title="Show this animation in Blender to change the pose and timing (needs Blender open with the Dissidia add-on 0.24+). Save moves to folder there, then Reload here to see the change."><i class="ic ic-external-link" aria-hidden="true"></i> Open in Blender</button>` : ''}</h3>
    <div class="bl-row"><span class="mono">${esc(motionName(i))}</span><small>${motionLen(i)} f${used.length ? ' · ' + esc(used.join(', ')) : ''}</small></div>`;
  if (!t) {
    let opts = '<option value="">empty</option>';
    for (const p of S.tlPaths) { const mo = F(p).motion; opts += `<option value="${esc(p)}">copy ${esc(motionName(mo))}</option>`; }
    h += `<p class="note">No event timeline yet.</p><div class="field"><label>Start from</label><select id="tlFrom">${opts}</select></div><button type="button" id="tlCreate" class="primary">Create timeline</button>`;
  }
  const rows = t ? timelineRows(t) : [];
  h += `<details class="acc"><summary>Details</summary>
    <p class="note">${used.length ? 'Used by ' + esc(used.join(', ')) : 'Not used by a move yet'}${t ? ` · ${rows.length} event${rows.length === 1 ? '' : 's'}` : ''}</p>
    ${t ? '<p class="note">Click an event on the timeline to edit it; drag bars to move, ends to resize. <span class="kbd">Alt</span> = half frames.</p>' : ''}
    ${dupSectionHTML()}</details></div>`;
  if (typeof trvSafeHTML === 'function') h += trvSafeHTML(i, false);
  if (typeof tmgSafeHTML === 'function') h += tmgSafeHTML(i);
  if (typeof camkSafeHTML === 'function') h += camkSafeHTML(i);
  if (!t) return h;
  // the parts drawn when it starts (request: "Is this just for showing and hiding parts? If so it should be called that, and
  // lose the hidden at start accordion"): ticked = shown; unticking writes a frame-0 hide_part_once as before
  const hides = new Set(t.keyed.filter((e) => isSetupHide(e, 'keyed')).map((e) => partIndex(e.part ?? (e.args || [])[0])));
  const parts = S.gmo ? S.gmo.bones.map((b, bi) => ({ b, bi })).filter((x) => x.b.drawPart > 0) : [];
  parts.sort((a, b) => a.b.name.localeCompare(b.b.name, undefined, { numeric: true }));
  h += `<div class="sec" id="partsSec"><h3>Show / hide parts <small>${parts.length - hides.size} of ${parts.length} shown</small>${fTip('Ticked parts are drawn when this animation starts. Show part and Hide part events change them after that.')}</h3>
    <div class="parts">${parts.map((x) => `<label class="chk"><input type="checkbox" data-b="${x.bi}"${hides.has(x.bi) ? '' : ' checked'}> ${esc(x.b.name)}${S.exOnly.has(x.bi) ? ' <small style="color:var(--dim)">EX</small>' : !hides.has(x.bi) && !partDefault(x.bi) ? ' <small style="color:var(--dim)" title="The game keeps this part hidden unless a Show part event shows it">auto</small>' : ''}</label>`).join('')}</div></div>`;
  return h;
}
/* a copy of the current animation as a new motion of this character (mod characters only): in the Animation section's Details */
function dupSectionHTML() {
  if (!S.rig || S.rig.retail || S.readOnly) return '';
  const i = S.motion, s = S.step && !S.moveOnly && S.moves[S.step.move] && S.moves[S.step.move].steps[S.step.i], canPoint = s && s.motion === i;
  const shared = canPoint ? S.moves.flatMap((mm) => mm.steps.filter((x) => x.script === s.script && !(mm === S.moves[S.step.move] && x.i === s.i)).map((x) => mm.name + ' ' + (x.i + 1))) : [];
  return `<div class="pgrp"><h4>Duplicate this animation${fTip(`Adds a copy of ${motionName(i)} as a new animation of this character, so you can change it without touching the moves that share the original. Poses are then edited in Blender.`)}</h4>
    <div class="field"><label>Name</label><input id="dupName" type="text" maxlength="31" value="${esc(motionName(i) + '_2')}" spellcheck="false"></div>
    <label class="chk"><input type="checkbox" id="dupEvents"${S.tlOfMotion.has(i) ? ' checked' : ' disabled'}> Copy its events</label>
    ${canPoint ? `<label class="chk"><input type="checkbox" id="dupPoint" checked> This press (${esc(S.moves[S.step.move].name)} ${s.i + 1}) plays the copy</label>${shared.length ? `<p class="note">Script ${s.script} is shared with ${esc(shared.join(', '))}: they would play the copy too.</p>` : ''}` : ''}
    <div class="btnrow"><button type="button" id="dupGo">Duplicate</button></div></div>`;
}
function wireAnim() {
  blWire();
  if (typeof trvSafeWire === 'function') trvSafeWire(S.motion);
  if (typeof tmgSafeWire === 'function') tmgSafeWire(S.motion);
  if (typeof camkSafeWire === 'function') camkSafeWire(S.motion);
  const c = $('tlCreate');
  if (c) c.onclick = () => createTimeline($('tlFrom').value);
  const dg = $('dupGo');
  if (dg) dg.onclick = () => { dg.disabled = true; const pt = $('dupPoint'); dupMotion(S.motion, $('dupName').value.trim(), !!($('dupEvents') && $('dupEvents').checked), pt && pt.checked ? S.step : null).finally(() => { dg.disabled = false; }); };
  $('insp').querySelectorAll('.parts input').forEach((cb) => cb.onchange = () => {
    const p = curTLPath(), bi = +cb.dataset.b;
    edit([p], () => {
      const t = F(p), name = S.boneNames[bi], v = S.boneIndex.get(name) === bi ? name : bi;
      if (!cb.checked) t.keyed.push({ frame: 0, op: 'hide_part_once', part: v });   // unticked = hidden at start
      else t.keyed = t.keyed.filter((e) => !(isSetupHide(e, 'keyed') && partIndex(e.part ?? (e.args || [])[0]) === bi));
    });
    pose(); renderInspector();
  });
}
function createTimeline(fromPath) {
  const idxPath = msPath('timelines/index.json'), mi = S.motion;
  let fn = 'timelines/' + String(mi).padStart(3, '0') + '.json';
  while (S.files[msPath(fn)]) fn = fn.replace('.json', '_n.json');
  const src = fromPath ? JSON.parse(JSON.stringify(F(fromPath))) : { motion: mi, ranged: [], keyed: [] };
  src.motion = mi;
  pushUndo([idxPath]);
  S.files[msPath(fn)] = { obj: src, text: '', dirty: true };
  F(idxPath).files.push(fn);
  S.tlPaths.push(msPath(fn));
  markDirty([idxPath, msPath(fn)]);
  derive(); runChecks(); renderAll();
}
function renderEvent(I, kind, idx, e) {
  const info = opInfo(e, kind), p = curTLPath();
  const named = isHexOp(e) ? [] : (info.args || []).slice(0, info.nargs);
  // the event's code and kind sit in its Metadata fold, its description in the title's tip (request: no fluff text)
  let h = `<div class="sec"><h3 data-ev="${kind}">${esc(info.cat)}</h3>
    <p class="title-ev">${esc(fxModeOf(e, kind) != null ? 'Effect' : info.label)}${fxModeOf(e, kind) != null ? '' : fTip(info.desc || '')}</p>${fxModeOf(e, kind) != null ? fxModeHTML(e, kind) : ''}${typeof spEventNote === 'function' ? spEventNote(e) : ''}`;
  if (kind === 'ranged') {
    const open = e.end === FOREVER;
    h += `<div class="row2">${numField('Start (frame)', frames(e.start), 'id="evStart" min="0" step="0.5"', ED_EV_TIP.start)}
      ${numField('End (frame)', open ? '' : frames(e.end), `id="evEnd" min="0" step="0.5"${open ? ' disabled placeholder="end"' : ''}`, ED_EV_TIP.end)}</div>
      <label class="chk"><input type="checkbox" id="evOpen"${open ? ' checked' : ''}> Lasts to the end of the animation${fTip(ED_EV_TIP.open)}</label>`;
  } else h += numField('Frame', frames(e.frame), 'id="evFrame" min="0" step="0.5"', ED_EV_TIP.frame);
  // named values
  named.forEach((n) => { h += argEditor(n, e[n], 'n:' + n, info.name); });
  const rest = e.args || [], oc0 = opCode(e.op);
  if (rest.length) {
    // values the file keeps unnamed (so old mod files still read) but whose meaning is known: ops.js arg_notes
    const notes = info.arg_notes || {}, inp = (v, k) => `<input type="text" class="mono" data-arg="${k}" value="${esc(typeof v === 'object' ? fmt3(v.f) + 'f' : v)}" title="value ${named.length + k + 1}${typeof v === 'object' ? ' (float)' : ''}">`;
    const known = rest.map((v, k) => [v, k]).filter(([, k]) => notes[named.length + k]), unknown = rest.map((v, k) => [v, k]).filter(([, k]) => !notes[named.length + k]);
    // a 0 / 1 value is a two-button switch; the projectile's origin effect (0x0204) the effect picker
    const kv = (v, k, bool) => (bool && (v === 0 || v === 1) ? boolSeg(`data-argb="${k}"`, v)
      : oc0 === 0x0204 && named.length + k === 1 && Number.isInteger(v) && S.effects.size ? effectSelect(v, `data-arg="${k}" data-fxarg="1"`, true) : inp(v, k));
    if (known.length) h += `<div class="row2">${known.map(([v, k]) => { const [lb, tp, ty] = notes[named.length + k]; return `<div class="field"><label>${esc(lb)}${fTip(tp)}</label>${kv(v, k, ty === 'bool')}</div>`; }).join('')}</div>`;
    if (unknown.length) h += `<div class="field"><label>Other values${fTip(ED_EV_TIP.other)}</label><div class="row3">${unknown.map(([v, k]) => inp(v, k)).join('')}</div></div>`;
  }
  const fk = kind === 'ranged' ? 'pad' : 'x', fv = e[fk] || 0;
  h += `<details><summary class="lbl" style="cursor:pointer">Filter ${fv ? '(' + fv + ')' : '(always)'}</summary><div class="row2" style="margin-top:6px">
      ${numField('Phase (0 = any)', fv & 0xFF, 'id="fPhase" min="0" max="255"', ED_EV_TIP.phase)}
      <div class="field"><label>Mode${fTip(ED_EV_TIP.mode)}</label><select id="fMode">${[[0, 'Always'], [1, 'Outside EX Mode'], [2, 'EX Mode only']].map(([v, l]) => `<option value="${v}"${((fv >> 8) & 0xFF) === v ? ' selected' : ''}>${l}</option>`).join('')}${[0, 1, 2].includes((fv >> 8) & 0xFF) ? '' : `<option selected value="${(fv >> 8) & 0xFF}">${(fv >> 8) & 0xFF}</option>`}</select></div></div>
      </details>
    <details class="acc"><summary>Metadata</summary><p class="note mono">${esc(info.op)}${info.name ? ' · ' + esc(info.name) : ''} · ${kind === 'ranged' ? 'start → end' : 'one frame'}</p></details>`;
  h += `<div class="btnrow"><button type="button" id="evDup">Duplicate at playhead</button><button type="button" id="evDel" class="danger">Delete</button></div></div>`;
  // hit
  const hn = named.find((n) => n === 'hit');
  if (hn !== undefined) h += hitHTML(hitOf(e.hit));
  const oc = opCode(e.op), fxId = [0x0219, 0x042F, 0x0466].includes(oc) ? argsOf(e)[0] : oc === 0x042A ? argsOf(e)[1] : null;
  let fi = fxId != null && !(oc === 0x0219 && kind === 'ranged') ? fxInfoCached(fxId) : null;
  const placeFx = fi && fxId != null && oc !== 0x042A;   // bone_effect ids (9999 trails) are shared slots: not here
  if (fi) h += fxHTML(fi, fxId) + (placeFx ? fxPlaceHTML(fi, fxId) : '');
  else if (e.hit !== undefined) {               // a hit window: the contact effect its hit plays where it lands (2026-10-28)
    const c = hitContactFx(hitOf(e.hit));
    const hh = hitOf(e.hit), ro = !hh || hh.raw || S.readOnly ? ' disabled' : '';
    const pick = hh && S.fx ? `<button type="button" class="apbtn" id="cfxPick"${ro} aria-haspopup="listbox" title="Pick the effect this hit plays where it lands"><span class="mono">${esc(c ? fxuShort(c.fi.name) : 'None')}</span><small>${c ? 'bit ' + c.bit : 'no contact effect'}</small><span class="ap-ch" aria-hidden="true">▾</span></button>` : '';
    if (c) { fi = c.fi; const fh = fxHTML(fi, c.id).replace('<h3>Effect <small', `<h3>Contact effect <small title="played where the hit lands: the effect records whose mask has the hit's contact effect bit ${c.bit}"`).replace('<p class="title-ev">' + esc(fi.name) + '</p>', pick ? `<div class="field">${pick}</div>` : '<p class="title-ev">' + esc(fi.name) + '</p>'); h += fh.slice(0, fh.lastIndexOf('</div>')) + `<p class="note">Played where the hit lands (contact effect bit ${c.bit}${c.more ? `; ${c.more} more record${c.more > 1 ? 's' : ''} share the bit` : ''}).</p><div class="btnrow"><button type="button" class="mini" data-fxjump="${c.ni}">Edit effect ${esc(fxuShort(fi.name))} ›</button></div></div>`; }
    else if (pick) h += `<div class="sec fx"><h3>Contact effect</h3><div class="field">${pick}</div><p class="note">Nothing plays where this hit lands.</p></div>`;
  }
  h += fxRollHTML(e, kind);                        // a rare variant: the roll that decides it
  h += edEventHTML(kind, idx, e);                  // hitboxes, projectile, more hit settings (editors.js)
  const trv = typeof trvSectionHTML === 'function' && typeof TRV !== 'undefined' && TRV.ops.has(oc);   // Air motion etc.: how high / far the animation carries him
  if (trv) h += trvSafeHTML(S.motion, true);
  I.innerHTML = h;
  if (fi) { const T = $('fxThumbs'); if (T) for (const id of fi.tex.slice(0, 12)) { const cv = fxTexCanvas(S.fx.fep, id, 56); if (cv) T.appendChild(cv); } }
  wireEvent(p, kind, idx, named);
  I.querySelectorAll('[data-fxmode]').forEach((b) => b.onclick = () => fxModeSet(p, kind, idx, +b.dataset.fxmode));
  fxRollWire(p);
  if (placeFx) fxPlaceWire(p, kind, idx, fi, fxId);
  edEventWire(p, kind, idx);
  if (trv) trvSafeWire(S.motion);
  const cp = $('cfxPick'); if (cp) cp.onclick = () => cfxOpen(cp, hitOf(e.hit), e, (fn) => hitEdit(p, kind, idx, fn));
}
/* the contact effect a hit plays where it lands, picked from a list (request: "This should let me choose the effect"). A hit
   names a group bit, not an effect: every effect record whose mask has that bit plays (fxGroupHas). The list is the
   groups the character's package has, each shown by the effect it plays; hovering one previews it on this hit. */
function cfxChoices() {
  if (!S.fx || typeof fxGroupHas !== 'function') return [];
  const by = new Map();
  for (const r of S.fx.fepa) { if (r.trig) continue; for (let b = 1; b < 64; b++) if (fxGroupHas(r, b)) { if (!by.has(b)) by.set(b, []); by.get(b).push(r); } }
  const out = [];
  for (const [bit, grp] of [...by].sort((a, b) => a[0] - b[0])) {
    const c = grp.filter((r) => r.kinds & 1), recs = c.length ? c : grp, fi = fxInfoCached(recs[0].id);
    if (fi) out.push({ bit, id: recs[0].id, fi, n: recs.length });
  }
  return out;
}
let CFX = null;
function cfxClose(pick) {
  if (!CFX) return;
  const c = CFX; CFX = null;
  document.removeEventListener('mousedown', c.out, true); window.removeEventListener('resize', c.out); c.el.remove();
  cancelAnimationFrame(c.raf);
  c.h.hit_effect_bit = c.was; S.frame = c.save.frame; S.playing = c.save.playing; if ($('playBtn')) $('playBtn').textContent = S.playing ? '❚❚' : '▶';
  if (typeof FXP !== 'undefined') FXP.map.clear(); if (typeof DM !== 'undefined') DM.key = ''; pose();
  if (pick != null && pick !== c.was) {
    const x = c.apply ? c.apply((y) => { y.hit_effect_bit = pick; }) : (edit([msPath('hits.json')], () => { c.h.hit_effect_bit = pick; }), c.h);
    derive(); if (typeof DM !== 'undefined') DM.key = ''; renderAll(); pose();
    if (x === c.h) toast(pick === 255 ? `${c.h.name}: no contact effect.` : `${c.h.name}: contact effect bit ${pick}${hitUsers(c.h).length > 1 ? ` (every event using ${c.h.name} gets it)` : ''}.`);
  } else if (c.anchor.isConnected) c.anchor.focus();
}
function cfxOpen(anchor, h, ev, apply) {     // apply(fn): the edit, as hitEdit makes it (a shared hit copied first)
  cfxClose(); if (typeof apClose === 'function') apClose();
  const list0 = cfxChoices();
  const el = document.createElement('div'); el.className = 'appop'; el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Pick a contact effect');
  el.innerHTML = `<input type="search" class="ap-q" placeholder="Search effects" aria-label="Search contact effects"><div class="ap-list" role="listbox"></div><p class="ap-hint">Hover or ↑ ↓ to play it on this hit${typeof DM !== 'undefined' && DM.on ? ' (with the Dummy on it shows only if the hit lands)' : ''} · click or Enter to pick · Esc to cancel</p>`;
  document.body.appendChild(el);
  const r = anchor.getBoundingClientRect(), W = Math.min(innerWidth - 16, Math.max(r.width, 340));
  el.style.left = Math.max(8, Math.min(r.left, innerWidth - W - 8)) + 'px'; el.style.width = W + 'px';
  const below = innerHeight - r.bottom - 12, above = r.top - 12;
  if (below >= 260 || below >= above) { el.style.top = (r.bottom + 4) + 'px'; el.style.maxHeight = Math.min(460, below) + 'px'; }
  else { el.style.bottom = (innerHeight - r.top + 4) + 'px'; el.style.maxHeight = Math.min(460, above) + 'px'; }
  CFX = { el, anchor, h, apply, was: h.hit_effect_bit, hi: h.hit_effect_bit, vis: [], save: { frame: S.frame, playing: S.playing } };
  // the preview plays the hit's moment over and over: from its window's start for as long as the hovered effect lasts
  const from = ev && ev.start != null ? frames(ev.start) : S.frame, lenOf = (bit) => { const x = list0.find((y) => y.bit === bit); return Math.max(12, x && x.fi.duration ? x.fi.duration / 2 + 6 : 30); };
  CFX.len = lenOf(CFX.hi);
  const loop = () => { if (!CFX) return; if (!S.playing || S.frame < from - 0.5 || S.frame > from + CFX.len) { S.frame = from; S.playing = true; if ($('playBtn')) $('playBtn').textContent = '❚❚'; } CFX.raf = requestAnimationFrame(loop); };
  CFX.raf = requestAnimationFrame(loop);
  const rows = [{ bit: 255, label: 'None', sub: 'no effect where it lands' }, ...list0.map((x) => ({ bit: x.bit, id: x.id, label: fxuShort(x.fi.name), sub: `bit ${x.bit} · ${x.fi.duration ? x.fi.duration / 2 + ' f · ' : ''}${x.fi.kinds.slice(0, 3).join(', ')}${x.n > 1 ? ` · +${x.n - 1} more` : ''}`, tex: x.fi.tex }))];
  const list = el.querySelector('.ap-list'), q = el.querySelector('.ap-q');
  const draw = () => {
    const t = q.value.trim().toLowerCase(), vis = rows.filter((x) => !t || (x.label + ' ' + x.sub).toLowerCase().includes(t));
    CFX.vis = vis.map((x) => x.bit);
    list.innerHTML = vis.map((x) => `<div class="ap-row cfx-row${x.bit === CFX.hi ? ' hi' : ''}${x.bit === CFX.was ? ' cur' : ''}" role="option" id="cfx-${x.bit}" data-bit="${x.bit}" aria-selected="${x.bit === CFX.hi}"><span class="cfx-th" data-th="${x.bit}"></span><span class="cfx-tx"><span class="mono">${esc(x.label)}</span><small>${esc(x.sub)}</small></span></div>`).join('') || '<p class="note">No effect matches.</p>';
    for (const x of vis) if (x.tex && S.fx.fep) { const th = list.querySelector(`[data-th="${x.bit}"]`); for (const id of x.tex.slice(0, 3)) { const cv = fxTexCanvas(S.fx.fep, id, 30); if (cv) th.appendChild(cv); } }
  };
  const preview = (bit, scroll) => {
    CFX.hi = bit; h.hit_effect_bit = bit; CFX.len = lenOf(bit); S.frame = from;
    if (typeof FXP !== 'undefined') FXP.map.clear(); if (typeof DM !== 'undefined') DM.key = '';
    pose();
    list.querySelectorAll('.ap-row').forEach((d) => { const on = +d.dataset.bit === bit; d.classList.toggle('hi', on); d.setAttribute('aria-selected', on); if (on && scroll) d.scrollIntoView({ block: 'nearest' }); });
    q.setAttribute('aria-activedescendant', 'cfx-' + bit);
  };
  draw();
  list.onmousemove = (e) => { const d = e.target.closest('.ap-row'); if (d && +d.dataset.bit !== CFX.hi) preview(+d.dataset.bit); };
  list.onclick = (e) => { const d = e.target.closest('.ap-row'); if (d) cfxClose(+d.dataset.bit); };
  q.oninput = () => { draw(); if (CFX.vis.length && !CFX.vis.includes(CFX.hi)) preview(CFX.vis[0], true); };
  q.onkeydown = (e) => {
    e.stopPropagation();
    const v = CFX.vis, k = v.indexOf(CFX.hi);
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); if (v.length) preview(v[k < 0 ? 0 : Math.max(0, Math.min(v.length - 1, k + (e.key === 'ArrowDown' ? 1 : -1)))], true); }
    else if (e.key === 'Enter') { e.preventDefault(); if (v.includes(CFX.hi)) cfxClose(CFX.hi); }
    else if (e.key === 'Escape') { e.preventDefault(); cfxClose(); }
  };
  CFX.out = (e) => { if (CFX && !(e && e.target && el.contains(e.target))) cfxClose(); };
  document.addEventListener('mousedown', CFX.out, true); window.addEventListener('resize', CFX.out);
  const cur = list.querySelector('.ap-row.cur'); if (cur) cur.scrollIntoView({ block: 'nearest' });
  q.focus();
}
/* the effect event's picker plays the effect under the pointer (request: "When adding an effect, can we show a preview of
   the effect on screen"): the event's effect is swapped for the hovered one (not an edit: no undo step) and loops for
   as long as that effect lasts; closing the list puts the effect and play / pause back (a pick then commits as a
   normal edit). Effects is switched on while it previews if it was off. Only the effect plays (2026-11-01: "it
   should just show the effect, not play the animation as well"): the playhead and the pose hold still, playback is
   paused, and fxRefresh draws just this event's effect on its own clock (FXEV.t ticks since it starts), attached to
   the character as it stands. */
var FXEV = null;                         // var: fxRefresh (above) reads it, maybe before this line has run
function fxEvPreview(p, kind, idx) {
  return (v) => {
    const e = F(p) && F(p)[kind] && F(p)[kind][idx]; if (!e) return;
    const get = () => ('effect' in e ? e.effect : e.args && e.args[0]), set = (x) => { if ('effect' in e) e.effect = x; else if (e.args) e.args[0] = x; };
    const btn = (on) => { if ($('playBtn')) $('playBtn').textContent = on ? '❚❚' : '▶'; };
    if (v == null) {
      if (!FXEV || FXEV.e !== e) return;
      const c = FXEV; FXEV = null; cancelAnimationFrame(c.raf);
      set(c.was); S.frame = c.save.frame; S.playing = c.save.playing; btn(S.playing);
      if (c.fxOff && $('showFx')) $('showFx').checked = false;
      pose(); return;
    }
    const id = parseInt(v, 10); if (!Number.isInteger(id)) return;
    if (!FXEV || FXEV.e !== e) {
      FXEV = { e, kind, idx, was: get(), save: { frame: S.frame, playing: S.playing }, t: 0, last: 0, len: 30, fxOff: !!($('showFx') && !$('showFx').checked) };
      if (FXEV.fxOff) $('showFx').checked = true;
      S.playing = false; btn(false);
      const loop = (now) => {
        const c = FXEV; if (!c) return;
        const dt = c.last ? Math.min(0.1, (now - c.last) / 1000) : 0; c.last = now;
        c.t += dt * 60 * parseFloat(($('speed') || { value: 1 }).value || 1);
        if (c.t > c.len * 2) c.t = 0;
        fxRefresh(); c.raf = requestAnimationFrame(loop);
      };
      FXEV.raf = requestAnimationFrame(loop);
    }
    set(id);
    const fi = fxInfoCached(id), life = argsOf(e)[1];
    FXEV.len = Math.max(12, typeof life === 'number' && life > 0 && life < 0xFFFF ? life / 2 + 6 : fi && fi.duration ? fi.duration / 2 + 6 : 30);
    FXEV.t = 0; fxRefresh();
  };
}
function hitContactFx(h) {                     // as fx.js fxInstances does for a hit window
  const bit = h ? h.hit_effect_bit : 0; if (!bit || bit === 255 || !S.fx || typeof fxGroupHas !== 'function') return null;
  const grp = S.fx.fepa.filter((r) => fxGroupHas(r, bit) && !r.trig);
  const contact = grp.filter((r) => r.kinds & 1), recs = contact.length ? contact : grp;
  if (!recs.length) return null;
  const fi = fxInfoCached(recs[0].id); if (!fi) return null;
  return { fi, id: recs[0].id, ni: recs[0].effect, bit, more: Math.min(recs.length, 2) - 1 };
}
function fxHTML(fi, id) {
  const r = fi.recs[0], where = r.bone ? `on <b>${esc(r.bone)}</b>` : 'at the character';
  const what = fi.kinds.join(', ') || 'nothing drawable';
  const notes = fi.notes.slice();
  if (fi.missing) notes.push(FXR.resident ? `${fi.missing} shared piece${fi.missing > 1 ? 's' : ''} not found` : `${fi.missing} shared piece${fi.missing > 1 ? 's' : ''} need the game ISO (Locations)`);
  return `<div class="sec fx"><h3>Effect <small class="mono">${esc(String(id))}</small></h3>
    <p class="title-ev">${esc(fi.name)}</p>
    <p class="note">${fi.sustained ? 'Keeps playing until it is stopped' : fi.duration ? `Plays ${fi.duration / 2} frames` : 'Plays'} ${where}${fi.recs.length > 1 ? ` (and ${fi.recs.length - 1} more place${fi.recs.length > 2 ? 's' : ''})` : ''}. ${fi.nodes} piece${fi.nodes === 1 ? '' : 's'}: ${what}.</p>
    ${notes.length ? `<p class="note">Preview: ${esc(notes.join('; '))}.</p>` : ''}
    <div class="fxthumbs" id="fxThumbs"></div></div>`;
}
/* an effect event's placement (request: "If we can control the size and rotation can we add it to the inspector for an
   effect"): the FEPA record(s) of the event's effect id hold where it plays relative to its bone (offset, rotation in
   degrees, size). They belong to the id, so every event playing that id changes; "Give this event its own copy" first
   copies the record(s) to a new id and points this event at it. */
const FX_EV_OPS = [0x0219, 0x042F, 0x0466];
/* "A change goes to: This event / All N events": data that several events use (an effect placement, a hit, a hitbox group,
   a projectile). attr names the buttons' data attribute ("one" / "all"); cur is the current choice (default "one": the
   first change gives this event its own copy, in the same undo step) */
const scopeSeg = (attr, cur, users, oneTip, allTip, noun = 'events') => `<div class="field"><label>A change goes to</label><div class="seg fxscope"><button type="button" ${attr}="one" aria-pressed="${cur !== 'all'}" title="${esc(oneTip)}">This event</button><button type="button" ${attr}="all" aria-pressed="${cur === 'all'}" title="${esc(allTip)}">All ${users} ${noun}</button></div></div>`;
// hits, hitbox groups and projectiles (2026-11-01: the same switch as effect placements, This event by default)
S.dscope = { hit: 'one', group: 'one', proj: 'one' };
const dscopeShared = (k, users) => S.dscope[k] !== 'all' && users > 1;
function dscopeWire(k) {
  document.querySelectorAll(`#insp [data-dscope-${k}]`).forEach((b) => b.onclick = () => { S.dscope[k] = b.getAttribute('data-dscope-' + k); renderInspector(); });
}
function fxEvUsers(id) {
  let n = 0;
  for (const p of S.tlPaths) { const t = F(p); for (const k of ['keyed', 'ranged']) for (const e of t[k] || []) if (FX_EV_OPS.includes(opCode(e.op)) && argsOf(e)[0] === id) n++; }
  return n;
}
function fxPlaceHTML(fi, id) {
  if (!S.effe || !fi.recs.length) return '';
  const r = fi.recs[0], users = fxEvUsers(id);
  return `<div class="sec"><h3>Placement <small class="mono">effect id ${esc(String(id))}</small></h3>
    ${fxBoneField(fi)}${fxuVecField('Rotation (°)', 'evfxRot', r.rot.map(fmt3))}${fxuVecField('Size', 'evfxScl', r.scale.map(fmt3))}${fxuVecField('Offset (m)', 'evfxPos', r.pos.map(fmt3))}
    ${users > 1 && !S.readOnly ? scopeSeg('data-fxscope', S.fxScope, users, 'The first change gives this event its own copy of the placement (a new effect id); the others keep theirs', `Effect id ${id}'s placement, so every event that plays it`) : ''}
    <p class="note">Relative to ${r.bone ? 'the bone <b>' + esc(r.bone) + '</b>' : 'where the event puts it'}. ${users > 1 ? (S.fxScope === 'all' ? `Effect id ${esc(String(id))} is played by ${users} events: a change here changes all of them.` : `Effect id ${esc(String(id))} is played by ${users} events: your first change gives this one its own copy, the others stay as they are.`) : 'Only this event plays it.'}${fi.recs.length > 1 ? ` It has ${fi.recs.length} placements (by bone); a change here goes to all of them.` : ''}</p></div>`;
}
/* the bone an attached effect follows (FEPA +0xA8, by name, so any of the character's bones). 2026-11-01: "Why don't
   we add that to placement?" (to move NOC_ARM_SUMMON from l_wep2 to the hand). Target-1 records ignore the name in the
   game (fxRecBone), and an id with placements on several bones keeps one each, so the picker is off for both. */
function fxBoneField(fi) {
  const r = fi.recs[0];
  if (!r || !r.bone) return '';
  const fixed = r.model && r.target === 1, many = new Set(fi.recs.map((x) => x.bone)).size > 1;
  const names = [...new Set(['root', ...S.boneNames])];
  const why = fixed ? 'This effect sits on the character itself in the game: its bone name is not used' : many ? 'This effect id has a placement on each of several bones; change them in the Effects editor' : 'The bone the effect follows; the offset and rotation are in its frame';
  return `<div class="field"><label>Bone</label><select id="evfxBone" title="${esc(why)}"${fixed || many || S.readOnly ? ' disabled' : ''}>${names.map((b) => `<option${b === r.bone ? ' selected' : ''}>${esc(b)}</option>`).join('')}${names.includes(r.bone) ? '' : `<option selected>${esc(r.bone)}</option>`}</select>${fixed || many ? `<small class="note">${esc(why)}.</small>` : ''}</div>`;
}
let FX_PLACE_BUSY = false;
function fxPlaceWire(p, kind, idx, fi, id) {
  if (!$('evfxRot0')) return;
  // the placement belongs to the effect id, which other events may play too (2026-10-30: "If I change the size for
  // one instance of an effect it changes both"): with "This event" (the default) a shared id is copied to a new id for
  // this event first, in the same undo step, and only the copy changes
  const set = (o, k, v) => {
    if (!isFinite(v) || FX_PLACE_BUSY) return;   // (a redraw mid-change blurs the field: its change must not run twice)
    const rec = fxuPkg().fepa.records[fi.recs[0].index];
    if (Math.abs(fxeDV(rec).getFloat32(o + 4 * k, true) - Math.fround(v)) < 1e-7) return;   // unchanged: no edit, no copy
    FX_PLACE_BUSY = true;
    try { setNow((c) => fxeDV(c).setFloat32(o + 4 * k, v, true)); } finally { FX_PLACE_BUSY = false; }
  };
  const setBone = (name) => {
    if (FX_PLACE_BUSY || !name || name === fi.recs[0].bone) return;
    FX_PLACE_BUSY = true;
    try { setNow((c) => fxeSetStr(c, 0xA8, name)); } finally { FX_PLACE_BUSY = false; }
  };
  const setNow = (mut) => {
    if (S.fxScope !== 'all' && fxEvUsers(id) > 1) {
      let nid = null;
      fxuEdit((pkg) => {
        nid = fxeFreeFepaId(pkg, 760);
        for (const x of fi.recs) { const c = Uint8Array.from(pkg.fepa.records[x.index]); fxeSetU32(c, 0x24, nid); mut(c); pkg.fepa.records.push(c); }
      }, true);
      if (nid == null) return;
      edit([p], () => { const e = F(p)[kind][idx]; if ('effect' in e) e.effect = nid; else if (e.args) e.args[0] = nid; });
      const b = S.undo.pop(), a = S.undo.pop(); S.undo.push([...a, ...b]);   // one undo for both
      toast(`This event now has its own copy (effect id ${nid}); the other events keep effect id ${id}.`);
      renderAll(); return;
    }
    fxuEdit((pkg) => { for (const x of fi.recs) mut(pkg.fepa.records[x.index]); }, true); renderInspector();
  };
  if ($('evfxBone')) $('evfxBone').onchange = () => setBone($('evfxBone').value);
  document.querySelectorAll('[data-fxscope]').forEach((b) => b.onclick = () => { S.fxScope = b.dataset.fxscope; renderInspector(); });
  [['evfxPos', 0], ['evfxRot', 12], ['evfxScl', 24]].forEach(([nm, o]) => [0, 1, 2].forEach((k) => { const el = $(nm + k); if (el) el.onchange = () => set(o, k, +el.value); }));
}
function argEditor(n, v, key, op) {
  const lab = argLabel(n), tip = fTip(typeof edArgTip === 'function' ? edArgTip(op, n) : '');
  if (n === 'hit') {
    const keys = [...S.hitByName.keys()];
    return `<div class="field"><label>Hit${tip}</label><select data-n="${n}">${keys.map((k) => `<option${k === v || (typeof v === 'number' && k === hex4(v)) ? ' selected' : ''}>${esc(k)}</option>`).join('')}${S.hitByName.has(v) ? '' : `<option selected>${esc(v)}</option>`}</select></div>`;
  }
  if (n === 'part') {
    const cur = partIndex(v);
    return `<div class="field"><label>${n === 'part' ? 'Part / bone' : lab}${tip}</label><select data-n="${n}">${S.boneNames.map((b, i) => `<option value="${i}"${i === cur ? ' selected' : ''}>${esc(b)}</option>`).join('')}${cur < 0 ? `<option selected value="${esc(v)}">${esc(v)}</option>` : ''}</select></div>`;
  }
  if (n === 'effect' && S.effects.size && typeof v === 'number') {
    return `<div class="field"><label>${esc(lab)}${tip}</label>${effectSelect(v, `data-n="${n}"`)}</div>`;
  }
  if (n === 'effect' && op === 'effect' && v && typeof v === 'object')       // the ranged form: a number, not an effect id
    return `<div class="field"><label>Speed (m a tick)${tip}</label><input type="number" step="any" data-n="${n}" data-f="1" value="${v.f}">
      <p class="note">The ranged form of 0x0219 doesn't play an effect: each tick it moves him toward the target by up to this much (the next value raises or lowers the point he moves to, in metres). To play an effect, use the one-frame Effect.</p>
      <div class="btnrow"><button type="button" id="evToFx" class="primary">Make it a one-frame Effect</button></div></div>`;
  if (v && typeof v === 'object') return `<div class="field"><label>${esc(lab)}${tip}</label><input type="number" step="any" data-n="${n}" data-f="1" value="${fmt3(v.f)}"></div>`;
  if ((OPS.bool_args || []).includes(n) && (v === 0 || v === 1)) return `<div class="field"><label>${esc(lab)}${tip}</label>${boolSeg(`data-nb="${n}"`, v)}</div>`;
  let extra = '';
  if (n === 'projectile') { const a = S.projAtkp.get(v), hh = a != null ? hitOf(a) : null; if (hh) extra = `<p class="note">hits with ${esc(hh.name)}</p>`; }
  return `<div class="field"><label>${esc(lab)}${tip}</label><input type="number" data-n="${n}" value="${esc(v)}">${extra}</div>`;
}
function wireEvent(p, kind, idx, named) {
  const ev = () => F(p)[kind][idx];
  const change = (fn) => { edit([p], fn); renderTimeline(); pose(); };
  const g = (id) => $(id);
  if (kind === 'ranged') {
    g('evStart').onchange = () => change(() => { const e = ev(); e.start = Math.max(0, ticks(+g('evStart').value)); if (e.end !== FOREVER && e.end < e.start) e.end = e.start; });
    g('evEnd').onchange = () => change(() => { const e = ev(); e.end = Math.max(e.start, Math.min(0xFFFE, ticks(+g('evEnd').value))); });
    g('evOpen').onchange = () => { change(() => { const e = ev(); e.end = g('evOpen').checked ? FOREVER : Math.min(0xFFFE, ticks(motionLen(S.motion))); }); renderInspector(); };
  } else g('evFrame').onchange = () => change(() => { ev().frame = Math.max(0, Math.min(0xFFFE, ticks(+g('evFrame').value))); });
  $('insp').querySelectorAll('select[data-n="effect"]').forEach((el) => { el._preview = fxEvPreview(p, kind, idx); el._previewHint = 'Hover or ↑ ↓ to play it here'; });
  $('insp').querySelectorAll('[data-n]').forEach((el) => el.onchange = () => {
    const n = el.dataset.n;
    change(() => {
      const e = ev();
      if (n === 'part') { const i = parseInt(el.value, 10); const name = S.boneNames[i]; e[n] = (S.boneIndex.get(name) === i) ? name : i; }
      else if (n === 'hit') e[n] = el.value;
      else if (el.dataset.f) e[n] = { f: parseFloat(el.value) || 0 };
      else e[n] = parseInt(el.value, 10) || 0;
    });
    renderInspector();
  });
  $('insp').querySelectorAll('[data-arg]').forEach((el) => el.onchange = () => {
    const k = +el.dataset.arg, s = el.value.trim();
    let v;
    if (/f$/i.test(s)) v = { f: parseFloat(s) }; else if (/^0x/i.test(s)) v = parseInt(s, 16); else if (/[.e]/i.test(s)) v = { f: parseFloat(s) }; else v = parseInt(s, 10);
    if (v === undefined || (typeof v === 'number' && isNaN(v)) || (typeof v === 'object' && isNaN(v.f))) { toast('Not a number: ' + s); renderInspector(); return; }
    change(() => { ev().args[k] = v; });
  });
  // the two-button switches: a named value (data-nb) or an unnamed one (data-argb)
  $('insp').querySelectorAll('[data-nb] [data-bv], [data-argb] [data-bv]').forEach((b) => b.onclick = () => {
    const g = b.parentNode, v = +b.dataset.bv;
    if (b.getAttribute('aria-pressed') === 'true') return;
    change(() => { const e = ev(); if (g.dataset.nb) e[g.dataset.nb] = v; else e.args[+g.dataset.argb] = v; });
    renderInspector();
  });
  const fset = () => change(() => { const e = ev(), fk = kind === 'ranged' ? 'pad' : 'x'; const v = ((+$('fMode').value & 0xFF) << 8) | (+$('fPhase').value & 0xFF) | ((e[fk] || 0) & ~0xFFFF); if (v) e[fk] = v; else delete e[fk]; });
  $('fPhase').onchange = fset; $('fMode').onchange = fset;
  const tf = $('evToFx');                        // ranged 0x0219 -> a keyed Effect at its start, with an effect to pick
  if (tf) tf.onclick = () => {
    const e0 = ev(); let tpl = null;            // the effect a one-frame Effect of this animation (else the moveset) plays
    for (const src of [p, ...S.tlPaths]) { tpl = (F(src).keyed || []).find((x) => opCode(x.op) === 0x0219 && typeof x.effect === 'number'); if (tpl) break; }
    const k = { frame: e0.start, op: 'effect', effect: tpl ? tpl.effect : (S.effects && S.effects.size ? [...S.effects.keys()][0] : 0), args: tpl && tpl.args ? tpl.args.slice() : [0] };
    edit([p], () => { const t = F(p); t.ranged.splice(idx, 1); t.keyed.push(k); });
    S.sel = { kind: 'keyed', idx: F(p).keyed.length - 1 }; renderTimeline(); renderInspector(); pose();
  };
  $('evDel').onclick = () => deleteSel();
  $('evDup').onclick = () => {
    edit([p], () => {
      const src = JSON.parse(JSON.stringify(ev())), t0 = ticks(S.frame);
      if (kind === 'ranged') { if (src.end !== FOREVER) src.end = Math.min(0xFFFE, t0 + (src.end - src.start)); src.start = t0; } else src.frame = t0;
      delete src._note; delete src.what;
      F(p)[kind].push(src); S.sel = { kind, idx: F(p)[kind].length - 1 };
    });
    renderTimeline(); renderInspector(); selReveal();
  };
  wireHit(p, kind, idx);
}
function deleteSel() {
  if (!S.sel) return;
  const p = curTLPath(), { kind, idx } = S.sel;
  edit([p], () => { F(p)[kind].splice(idx, 1); });
  S.sel = null; renderTimeline(); renderInspector(); pose();
}

/* hits */
const REACT = Object.fromEntries(Object.entries(OPS.reactions).map(([k, v]) => [parseInt(k, 16), v]));
function hitUsers(h) {
  const keys = new Set([hitKey(h), h.id, parseInt(h.id, 16)]), out = [];
  for (const p of S.tlPaths) {
    const t = F(p);
    for (const e of [...t.ranged, ...t.keyed]) if (e.hit !== undefined && keys.has(e.hit)) out.push(motionName(t.motion));
  }
  for (const [pid, a] of S.projAtkp) if (a === parseInt(h.id, 16)) out.push('projectile ' + pid);
  return out;
}
// Up is a height for stagger / knock / blow, a speed for launch (0x06) and blow away (0x04) (ED_HIT_TIP.kb_up)
const hitUpLabel = (reaction) => ([0x04, 0x06].includes(reaction) ? 'Up speed (m/s)' : 'Up (m)');
function hitHTML(h) {
  if (!h) return '<div class="sec"><h3>Hit</h3><p class="warnline">This hit is not in hits.json.</p></div>';
  const users = hitUsers(h), flags = parseInt(h.flags, 16);
  const ro = h.raw ? ' disabled' : '';
  return `<div class="sec" id="hitSec"><h3 title="hits.json record ${esc(h.id)}">Hit · ${esc(h.name)}</h3>
    ${h.raw ? '<p class="warnline">This record keeps raw bytes (a value that does not round-trip); edit hits.json by hand.</p>' : ''}
    <p class="note">Used ${users.length}×: ${esc([...new Set(users)].join(', '))}</p>
    ${users.length > 1 && !h.raw && !S.readOnly ? scopeSeg('data-dscope-hit', S.dscope.hit, users.length, `The first change copies ${h.name} for this event (a new hit); the others keep theirs`, `${h.name} itself, so every event that uses it`) : ''}
    <div class="row2"><div class="field"><label>Name${fTip(ED_HIT_TIP.name)}</label><input type="text" id="h_name" maxlength="15" value="${esc(h.name)}"${ro}></div>
    ${numField('Power (brave damage)', h.power, `id="h_power" min="0" max="9999"${ro}`, ED_HIT_TIP.power)}</div>
    <div class="row2">${numField('Re-hit (ticks)', h.rehit, `id="h_rehit" min="0"${ro}`, ED_HIT_TIP.rehit)}
      <div class="field"><label>Reaction${fTip(ED_HIT_TIP.reaction)}</label><select id="h_reaction"${ro}>${Object.entries(REACT).map(([v, l]) => `<option value="${v}"${+v === h.reaction ? ' selected' : ''}>${l}</option>`).join('')}${REACT[h.reaction] ? '' : `<option selected value="${h.reaction}">${hex4(h.reaction).slice(4)} ?</option>`}</select></div></div>
    <div class="btnrow"><label class="chk"><input type="checkbox" id="h_hp"${flags & 2 ? ' checked' : ''}${ro}> HP attack${fTip(ED_HIT_TIP.hp)}</label><label class="chk"><input type="checkbox" id="h_rand"${flags & 1 ? ' checked' : ''}${ro}> Random power${fTip(ED_HIT_TIP.rand)}</label></div>
    <div class="lbl subhd">Knock-back</div>
    <div class="row2">${numField('Forward (m)', fmt3(h.kb_forward), `id="h_kb_forward" step="0.1"${ro}`, ED_HIT_TIP.kb_forward)}${numField(`<span id="h_kb_up_l">${hitUpLabel(h.reaction)}</span>`, fmt3(h.kb_up), `id="h_kb_up" step="0.1"${ro}`, ED_HIT_TIP.kb_up)}</div>
    <div class="row3">${numField('Stun (ticks)', fmt3(h.stun), `id="h_stun" step="1"${ro}`, ED_HIT_TIP.stun)}${numField('Follow', fmt3(h.follow), `id="h_follow" step="1"${ro}`, ED_HIT_TIP.follow)}${numField('Turn (°)', fmt3(h.kb_yaw), `id="h_kb_yaw" step="1"${ro}`, ED_HIT_TIP.kb_yaw)}</div>
    ${typeof edHit2CRow === 'function' ? edHit2CRow(h) : ''}
    ${typeof edHitExtraHTML === 'function' ? edHitExtraHTML(h) : ''}</div>`;
}
/* a copy of hit h for one event: a new id (the highest + 1) and name NAME_2 (_3 ...) */
function hitCopyOf(h, hits) {
  const ids = new Set(hits.map((x) => parseInt(x.id, 16)));
  let nid = Math.max(...ids) + 1; while (ids.has(nid)) nid++;
  const base = (h.name || 'HIT').slice(0, 11); let k = 2; const names = new Set(hits.map((x) => x.name));
  while (names.has(base + '_' + k)) k++;
  const copy = JSON.parse(JSON.stringify(h)); copy.id = hex4(nid); copy.name = base + '_' + k; delete copy.name_raw;
  return copy;
}
/* a change to the hit of the event F(p)[kind][idx]: fn(hit) edits it. With "This event" and a hit other events use too,
   the hit is first copied for this event (hitCopyOf) and the event pointed at the copy, all in one undo step.
   Returns the hit that changed (the copy, or the hit itself). */
function hitEdit(p, kind, idx, fn) {
  const hp = msPath('hits.json'), h = hitOf(F(p)[kind][idx].hit); if (!h) return null;
  if (dscopeShared('hit', hitUsers(h).length)) {
    const hits = F(hp).hits, copy = hitCopyOf(h, hits);
    edit([hp, p], () => { hits.push(copy); F(p)[kind][idx].hit = copy.name; fn(copy); });
    toast(`This event now has its own copy, ${copy.name}; the other events keep ${h.name}.`);
    return copy;
  }
  edit([hp], () => fn(h));
  return h;
}
function wireHit(p, kind, idx) {
  if (!$('hitSec')) return;
  const hp = msPath('hits.json'), e = F(p)[kind][idx], h = hitOf(e.hit);
  dscopeWire('hit');
  if (!h || h.raw) return;
  const set = (fn) => { const x = hitEdit(p, kind, idx, fn); derive(); if (x && x !== h) renderAll(); else renderTimeline(); };
  const num = (id, k, int) => { const el = $(id); if (el) el.onchange = () => { const v = parseFloat(el.value); if (isNaN(v) || (int ? Math.round(v) : v) === h[k]) return; set((x) => { x[k] = int ? Math.round(v) : v; }); }; };
  num('h_power', 'power', true); num('h_rehit', 'rehit', true); num('h_hitstop', 'hitstop', true);
  num('h_kb_forward', 'kb_forward'); num('h_kb_up', 'kb_up'); num('h_stun', 'stun'); num('h_follow', 'follow'); num('h_kb_yaw', 'kb_yaw');
  $('h_reaction').onchange = () => { const r = +$('h_reaction').value; set((x) => { x.reaction = r; }); const l = $('h_kb_up_l'); if (l) l.textContent = hitUpLabel(r); renderInspector(); };   // the knock-back bone / launch speed row depends on it
  const fl = () => { const hpOn = $('h_hp').checked, rnd = $('h_rand').checked; set((x) => { let f = parseInt(x.flags, 16) & ~3; if (hpOn) f |= 2; if (rnd) f |= 1; x.flags = '0x' + (f >>> 0).toString(16).toUpperCase().padStart(8, '0'); }); };
  $('h_hp').onchange = fl; $('h_rand').onchange = fl;
  $('h_name').onchange = () => {
    const nn = $('h_name').value.trim().slice(0, 15);
    if (!nn || S.hitByName.has(nn)) { toast(nn ? 'A hit is already called ' + nn : 'Name needed'); renderInspector(); return; }
    if (dscopeShared('hit', hitUsers(h).length)) {   // This event: a copy named nn, for this event only
      const hits = F(hp).hits, copy = hitCopyOf(h, hits); copy.name = nn;
      edit([hp, p], () => { hits.push(copy); F(p)[kind][idx].hit = nn; });
      derive(); renderAll(); toast(`This event now has its own copy, ${nn}; the other events keep ${h.name}.`); return;
    }
    const old = hitKey(h);
    edit([hp, ...S.tlPaths], () => { h.name = nn; for (const tp of S.tlPaths) for (const ev2 of [...F(tp).ranged, ...F(tp).keyed]) if (ev2.hit === old) ev2.hit = nn; });
    derive(); renderAll();
  };
}

/* a popup menu next to its button, kept on screen: below it if there is room, else above, never taller than the space */
function menuPlace(m, anchor, width = 360) {
  const r = anchor.getBoundingClientRect(), below = innerHeight - r.bottom - 12, above = r.top - 12;
  width = Math.max(width, m.offsetWidth || 0);      // its CSS width when that is wider (the effect picker ran off the right edge)
  m.style.left = Math.max(8, Math.min(innerWidth - width - 10, r.left)) + 'px';
  m.style.right = 'auto'; m.style.bottom = 'auto';
  if (below >= Math.min(320, above) || below >= above) { m.style.top = (r.bottom + 4) + 'px'; m.style.maxHeight = Math.max(160, below) + 'px'; }
  else { const h = Math.min(above, innerHeight * 0.7); m.style.maxHeight = h + 'px'; m.style.top = 'auto'; m.style.bottom = (innerHeight - r.top + 4) + 'px'; }
}
/* ====================================================== add event */
function openAddMenu(anchor) {
  closeMenus();
  const m = document.createElement('div'); m.className = 'menu'; m.id = 'addMenu';
  m.innerHTML = '<input type="search" id="addFilter" placeholder="Find an event (hit, chain, sound, invincible …)" aria-controls="addList" aria-label="Find an event"><div class="list" id="addList" role="listbox" aria-label="Events"></div><div class="menu-foot" aria-hidden="true">◆ one frame · ▬ range</div>';
  document.body.appendChild(m); menuPlace(m, anchor, 370);
  // Effect is one entry (2026-10-30): timed, variant and the roll are picked in its inspector (FX_MODES)
  const fxKey = opKey(0x0219, 'keyed');
  const entries = Object.entries(OPS.ops).filter(([k, v]) => (v.uses > 0 || v.name) && !FX_MENU_HIDE.has(parseInt(v.op, 16))).map(([k, v]) => ({ k, v: k === fxKey ? { ...v, desc: FX_MENU_DESC, more: 'timed time variant rare roll' } : v }));
  const draw = () => {
    const q = $('addFilter').value.toLowerCase();
    let h = '';
    for (const cat of OPS.cats) {
      const list = entries.filter(({ v }) => v.cat === cat && (!q || (v.label + ' ' + v.desc + ' ' + (v.name || '') + ' ' + v.op + ' ' + (v.more || '')).toLowerCase().includes(q))).sort((a, b) => b.v.uses - a.v.uses);
      if (!list.length) continue;
      h += `<div class="cat">${esc(cat)}</div>` + list.map(({ k, v }) => `<button type="button" data-k="${esc(k)}">${esc(v.label)} <span class="mono" style="color:var(--dim)">${addKind(v) === 'ranged' ? '▬' : '◆'}</span><small>${esc(v.desc)}</small></button>`).join('');
    }
    $('addList').innerHTML = h || '<p class="note" style="padding:8px">Nothing matches.</p>';
    $('addList').querySelectorAll('button').forEach((b, i) => { b.id = 'addOpt' + i; b.setAttribute('role', 'option'); b.tabIndex = -1; b.onclick = () => { addEvent(b.dataset.k); closeMenus(); }; });
    hi(0);
  };
  // the keyboard: ↑/↓ move the highlight (the filter keeps focus), Enter adds it, Esc closes
  const opts = () => [...$('addList').querySelectorAll('button')];
  const hi = (i) => {
    const o = opts(); o.forEach((b) => { b.classList.remove('hi'); b.setAttribute('aria-selected', 'false'); });
    if (!o.length) { $('addFilter').removeAttribute('aria-activedescendant'); return; }
    const b = o[Math.max(0, Math.min(o.length - 1, i))]; b.classList.add('hi'); b.setAttribute('aria-selected', 'true');
    $('addFilter').setAttribute('aria-activedescendant', b.id); b.scrollIntoView({ block: 'nearest' });
  };
  m.addEventListener('keydown', (e) => {
    const o = opts(), at = o.findIndex((b) => b.classList.contains('hi'));
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); hi(at + (e.key === 'ArrowDown' ? 1 : -1)); }
    else if (e.key === 'Enter' && at >= 0) { e.preventDefault(); o[at].click(); }
    else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closeMenus(); anchor.focus(); }
  });
  $('addFilter').oninput = draw; draw(); $('addFilter').focus();
  setTimeout(() => document.addEventListener('pointerdown', outside), 0);
}
function outside(ev) { const m = $('addMenu'); if (m && !m.contains(ev.target)) closeMenus(); }
function closeMenus() { const m = $('addMenu'); if (m) m.remove(); document.removeEventListener('pointerdown', outside); }
/* an op used both ways (Effect 0x0219: one frame with an effect id; ranged with a number that isn't an effect) is added
   in the form the game uses most (2026-10-30: adding Effect gave the ranged form, with no effect to pick) */
function addKind(info) {
  const r = info.kinds.includes('ranged'), k = info.kinds.includes('keyed'); if (!r || !k) return r ? 'ranged' : 'keyed';
  const u = (x) => ((info.shape || {})[x] || {}).uses || 0;
  return u('keyed') > u('ranged') ? 'keyed' : 'ranged';
}
function addEvent(key) {
  if (!curTLPath()) createTimeline('');
  const p = curTLPath(), t = F(p);
  const { kind, e } = newEvent(key, p, ticks(S.frame));
  edit([p], () => { t[kind].push(e); });
  S.sel = { kind, idx: t[kind].length - 1 };
  renderTimeline(); renderInspector(); pose(); selReveal();
}
function newEvent(key, p, t0) {                 // { kind, e } of op key at tick t0, in the shape the moveset (or retail) uses
  const info = OPS.ops[key], code = parseInt(info.op, 16);
  const kind = addKind(info);
  // copy the shape: one of this kind in this animation, else in the moveset, else the retail shape
  let tpl = null;
  for (const src of [p, ...S.tlPaths]) { tpl = F(src)[kind].find((e) => opCode(e.op) === code); if (tpl) break; }
  let e;
  if (tpl) {
    e = JSON.parse(JSON.stringify(tpl)); delete e._note; delete e.what;
    if (kind === 'ranged') { if (e.end !== FOREVER) e.end = Math.min(0xFFFE, t0 + Math.max(1, e.end - e.start)); e.start = t0; } else e.frame = t0;
  } else {
    const sh = (info.shape || {})[kind] || { args: new Array(info.nargs).fill(0), dur: 8 };
    const vals = (sh.args || []).slice(); while (vals.length < info.nargs) vals.push(0);
    e = kind === 'ranged' ? { start: t0, end: sh.dur === FOREVER || sh.dur == null ? FOREVER : Math.min(0xFFFE, t0 + Math.max(1, sh.dur)) } : { frame: t0 };
    e.op = info.name || info.op;
    const names = info.name ? info.args.slice(0, info.nargs) : [];
    names.forEach((n, i) => { e[n] = vals[i]; });
    if (names.includes('hit') && !S.hitByName.has(e.hit)) e.hit = [...S.hitByName.keys()][0];
    if (names.includes('part') && typeof e.part === 'number' && S.boneNames[e.part]) e.part = S.boneNames[e.part];
    const rest = vals.slice(names.length, info.nargs);
    if (rest.length || !info.name) e.args = rest;
  }
  return { kind, e };
}

/* Effect: one entry for the events that play an effect (2026-10-30: "What's the difference between all of these?
   Can we just have Effect as an option and you choose the subset in the inspector?"):
     - on its frame (0x0219 keyed): the usual one;
     - at a move time (0x0466, ranged): while its range runs, plays the effect once when the action's total time reaches
       a tick, on a bone; for animations that loop or hold, where a frame comes round again but the tick doesn't;
     - rare variant (0x042F): this effect, or another one if a rare-variant roll (0x032E) earlier in the action came up.
   Switching keeps the effect, its lifetime, the time it plays and the filter. */
const FX_MENU_HIDE = new Set([0x0466, 0x042F, 0x032E]);
const opKey = (code, kind) => (OPS.ops[hex4(code) + ':' + kind] ? hex4(code) + ':' + kind : hex4(code));   // ops used both ways have an entry per kind
const FX_MENU_DESC = 'Play an effect: on its frame, once at a move time, or as a rare variant (choose in the inspector).';
const FX_MODES = [
  [0x0219, 'On its frame', 'Plays the effect on the event\'s frame.'],
  [0x0466, 'At a move time', 'While its range runs, plays the effect once when the action\'s total time reaches Tick, on Part: for animations that loop or hold, where a frame comes round again but the tick doesn\'t.'],
  [0x042F, 'Rare variant', 'Plays the effect, or Alt effect instead when a rare-variant roll earlier in the action came up.']];
function fxModeOf(e, kind) { const c = opCode(e.op); return (c === 0x0219 && kind === 'keyed') || c === 0x0466 || c === 0x042F ? c : null; }
function fxModeHTML(e, kind) {
  const m = fxModeOf(e, kind); if (m == null) return '';
  const ro = S.readOnly ? ' disabled' : '';
  return `<div class="field"><label>Plays</label><div class="seg fxmode">${FX_MODES.map(([c, l, d]) => `<button type="button" data-fxmode="${c}" aria-pressed="${c === m}" title="${esc(d)}"${ro}>${esc(l)}</button>`).join('')}</div>
    <p class="note">${esc(FX_MODES.find(([c]) => c === m)[2])}</p></div>`;
}
function fxModeSet(p, kind, idx, code) {
  const t = F(p), e = t[kind][idx], from = fxModeOf(e, kind); if (from == null || from === code) return;
  const a = argsOf(e), eff = a[0], life = a[1] ?? 0;
  const at = kind === 'keyed' ? e.frame : (from === 0x0466 && typeof e.tick === 'number' ? e.tick : e.start);
  const filt = kind === 'keyed' ? e.x : e.pad;
  const key = opKey(code, code === 0x0466 ? 'ranged' : 'keyed');
  const { kind: nk, e: ne } = newEvent(key, p, at);
  const info = OPS.ops[key], names = (info.args || []).slice(0, info.nargs);
  if (code === 0x0219) { ne.frame = at; if (names.includes('effect')) ne.effect = eff; else ne.args = [eff, life]; if (names.length === 1) ne.args = [life]; }
  else if (code === 0x0466) { ne.start = at; ne.end = FOREVER; ne.effect = eff; ne.lifetime = life; ne.tick = at; }
  else { ne.frame = at; ne.effect = eff; ne.lifetime = life; if (ne.alt_effect == null || from !== 0x042F) { ne.alt_effect = eff; ne.alt_lifetime = life; } }
  delete ne.x; delete ne.pad;
  if (filt) ne[nk === 'keyed' ? 'x' : 'pad'] = filt;
  edit([p], () => { t[kind].splice(idx, 1); t[nk].push(ne); });
  S.sel = { kind: nk, idx: t[nk].length - 1 };
  runChecks(); renderTimeline(); renderInspector(); pose();
}
/* a rare variant (effect 0x042F, projectile 0x042D) plays its alternate only when a rare-variant roll (0x032E) earlier
   in the action came up: where that roll is, or a button to add one */
function fxRollHTML(e, kind) {
  const c = opCode(e.op); if (kind !== 'keyed' || (c !== 0x042F && c !== 0x042D)) return '';
  const t = curTL(), rolls = t.keyed.map((x, i) => ({ x, i })).filter(({ x }) => opCode(x.op) === 0x032E);
  const before = rolls.filter(({ x }) => x.frame <= e.frame).pop(), ro = S.readOnly ? ' disabled' : '';
  const say = (x) => { const v = argsOf(x); return `1 in ${v[2] ?? '?'}${v[1] ? `, unlock bit ${v[1]}` : ''}`; };
  return `<div class="sec"><h3>Rare-variant roll</h3>${before
    ? `<p class="note">The roll at frame ${fmtF(before.x.frame)} (${esc(say(before.x))}) decides: when it comes up, the alternate plays.</p><div class="btnrow"><button type="button" class="mini" id="fxRollGo" data-i="${before.i}">Select the roll</button></div>`
    : `<p class="note">No roll earlier in this animation, so the alternate only plays if a press before this one rolled.${rolls.length ? ` (The roll here is at frame ${fmtF(rolls[0].x.frame)}, after this event.)` : ''}</p><div class="btnrow"><button type="button" class="mini" id="fxRollAdd"${ro}>Add a roll at frame 0</button></div>`}</div>`;
}
function fxRollWire(p) {
  const go = $('fxRollGo'); if (go) go.onclick = () => selectEvent('keyed', +go.dataset.i, true);
  const add = $('fxRollAdd');
  if (add) add.onclick = () => {
    const t = F(p), { e } = newEvent(opKey(0x032E, 'keyed'), p, 0);
    edit([p], () => { t.keyed.push(e); });
    runChecks(); renderTimeline(); renderInspector();
    toast('Added a rare-variant roll at frame 0: select it to set its odds.');
  };
}

/* ====================================================== checks */
// The 16-at-once limit is the draw-part registry (Noctis: only the first 16 ranged hides of a timeline ever ran). Other
// ranged events have no such cap: retail Cloud runs 17 at once in _703A (8 bone effects, hits, sweeps, face textures).
// Events after the animation's end are fine too: the timeline keeps running while the last frame holds (retail Cloud
// plays sounds and voices there).
const VIS_OPS = [0x011B, 0x011C];
function runChecks() {
  const out = [];
  for (const p of S.tlPaths) {
    const t = F(p), mi = t.motion, nm = motionName(mi);
    const ticksSet = new Set([0]);
    t.ranged.forEach((e) => { ticksSet.add(e.start); if (e.end !== FOREVER) ticksSet.add(e.end); });
    // Events whose filter doesn't match never run together (EX Mode only vs outside it, other phases), so they are counted
    // per case. Retail relies on that: Shantotto has 27 ranged events at once in total, 7 in any one case.
    const phases = new Set([0]); t.ranged.forEach((e) => phases.add((e.pad || 0) & 0xFF));
    let peak = 0, at = 0, when = '';
    for (const ph of phases) for (const md of [1, 2]) {
      const evs = t.ranged.filter((e) => { const lo = (e.pad || 0) & 0xFF, m = ((e.pad || 0) >> 8) & 0xFF; return (!lo || lo === ph) && (!m || m === md) && VIS_OPS.includes(opCode(e.op)); });
      for (const k of ticksSet) {
        const n = evs.filter((e) => e.start <= k && (e.end === FOREVER || k < e.end)).length;
        if (n > peak) { peak = n; at = k; when = (ph ? `phase ${ph}, ` : '') + (md === 2 ? 'in EX Mode' : 'outside EX Mode'); }
      }
    }
    if (peak > 16) out.push({ level: 'err', motion: mi, text: `${nm}: ${peak} part show / hide ranges at once (frame ${fmtF(at)}, ${when}); the game keeps track of at most 16, so the rest never run and those parts stay as they were.` });
    for (const kind of ['ranged', 'keyed']) t[kind].forEach((e, idx) => {
      if (e.hit !== undefined && !hitOf(e.hit)) out.push({ level: 'err', motion: mi, kind, idx, text: `${nm}: hit ${e.hit} is not in hits.json.` });
      if (e.part !== undefined && partIndex(e.part) < 0) out.push({ level: 'err', motion: mi, kind, idx, text: `${nm}: part ${e.part} is not a bone of the model.` });
      if (isNaN(opCode(e.op))) out.push({ level: 'err', motion: mi, kind, idx, text: `${nm}: unknown event ${e.op}.` });
    });
  }
  S.moves.forEach((m, mi) => m.steps.forEach((s, i) => {
    if (s.motion < 0) { out.push({ level: 'err', move: mi, text: `${m.name} press ${i + 1}: action ${s.action} has no animation.` }); return; }
    const nx = m.steps[i + 1], br = (m.chains || []).some((c) => c.chainAt === i + 1);
    if (m.burst || (!nx && !br) || (m.chainOf != null && i < m.chainAt - 1)) return;   // (a chain's shared presses: checked on its move)
    const t = S.tlOfMotion.has(s.motion) && F(S.tlOfMotion.get(s.motion));
    const chain = t && t.ranged.some((e) => [0x0101, 0x0467, 0x0278, 0x0579].includes(opCode(e.op)));
    if (!chain) out.push({ level: 'warn', motion: s.motion, text: `${m.name} press ${i + 1} (${motionName(s.motion)}) has no chain window, so ${nx ? `press ${i + 2}` : 'its □ HP finisher'} can't follow.` });
  }));
  S.moves.forEach((m, mi) => { for (const c of m.chains || []) if (typeof mvKind === 'function' && mvKind(c).type !== 'hp' && c.chainAt < m.steps.length) out.push({ level: 'warn', move: mi, text: `${c.name} (○ in ${m.name}'s press ${c.chainAt}) and ${m.name}'s press ${c.chainAt + 1} both start on ○ there: one of them never plays.` }); });
  if (typeof mvWrongButton === 'function') S.moves.forEach((m, mi) => { const w = mvWrongButton(m); if (w.length) out.push({ level: 'warn', move: mi, text: `${m.name} is ${mvKind(m).type === 'hp' ? 'an HP' : 'a brave'} move but press${w.length > 1 ? 'es' : ''} ${w.map((s) => s.i + 1).join(', ')} answer${w.length > 1 ? '' : 's'} ${mvKind(m).type === 'hp' ? '○' : '□'}: fix it in the move's panel.` }); });
  if (typeof ckMore === 'function') try { out.push(...ckMore()); } catch (e) { console.warn('studio: checks', e); }
  S.checks = out;
  const nP = out.filter((c) => c.level !== 'info').length;
  // the alerts button next to Undo (2026-10-31: "Alerts should go in an icon popover next to undo")
  const b = $('checksBtn'); b.hidden = !S.moves || !out.length;
  if (!out.length && S.showChecks) { S.showChecks = false; renderInspector(); }
  b.innerHTML = `<i class="ic ${nP ? 'ic-triangle-alert' : 'ic-circle-alert'}" aria-hidden="true"></i><span class="cnt">${nP || out.length}</span>`;
  b.title = nP ? `${nP} problem${nP > 1 ? 's' : ''}` : `${out.length} note${out.length > 1 ? 's' : ''}`;
  b.setAttribute('aria-label', 'Alerts: ' + b.title);
  b.className = 'alerts-btn' + (nP ? ' warn' : '');
  if ($('alertsPop')) alertsPopFill();
}
/* the alerts popover under the button next to Undo: what is shown first (the animation, or every animation of the
   picked move), then the rest of the character; a row goes to its animation and event */
function alertsHere() {
  const ms = S.step && S.moves[S.step.move] ? new Set(S.moves[S.step.move].steps.map((x) => x.motion)) : null;
  return S.checks.filter((c) => c.motion != null && (S.moveOnly && ms ? ms.has(c.motion) : c.motion === S.motion));
}
function alertsPopFill() {
  const pop = $('alertsPop'); if (!pop) return;
  if (!S.checks.length) { alertsPopClose(); return; }
  const here = alertsHere(), rest = S.checks.filter((c) => !here.includes(c));
  const row = (c) => `<button type="button" class="ap-alert ${c.level}" data-ak="${S.checks.indexOf(c)}"><i class="ic ${c.level === 'info' ? 'ic-circle-alert' : 'ic-triangle-alert'}" aria-hidden="true"></i><span>${esc(c.text)}</span></button>`;
  pop.innerHTML = `<div class="ap-head"><b>Alerts</b><small>${S.checks.length}</small></div>
    ${here.length ? `<div class="ap-grp">Here</div>${here.map(row).join('')}` : ''}
    ${rest.length ? `<div class="ap-grp">${here.length ? 'Elsewhere' : 'In the character'}</div>${rest.map(row).join('')}` : ''}`;
  pop.querySelectorAll('[data-ak]').forEach((d) => d.onclick = () => { const c = S.checks[+d.dataset.ak]; alertsPopClose(); alertGo(c); });
}
/* an alert goes to what it is about: its animation and event (motion), a projectile (proj: the event that fires it, with
   that projectile in the inspector), a move (move: its index; hook: its runtime hooks section), or a tab (tab) */
function alertGo(c) {
  S.showChecks = false; S.showChanges = false; S.newMove = null; S.mfxSel = null;
  const leave = () => { S.fxEdit = null; S.projOnly = null; if (S.comp) S.comp = null; };
  if (c.motion != null) {
    leave(); S.tab = S.tab === 'anims' ? 'anims' : 'moves';
    if (c.motion !== S.motion) { S.step = null; setMotion(c.motion); }
    S.sel = c.kind ? { kind: c.kind, idx: c.idx } : null;
    renderAll(); selReveal(); return;
  }
  if (c.proj != null) {
    const P = mvProjMap(), id = c.proj;
    for (const p of S.tlPaths) {
      const t = F(p);
      for (const kind of ['keyed', 'ranged']) for (let i = 0; i < t[kind].length; i++) {
        const e = t[kind][i]; if (e.projectile === undefined || !mvProjChain(e.projectile, P).has(id)) continue;
        leave(); S.tab = 'moves';
        if (t.motion !== S.motion) { S.step = null; setMotion(t.motion); }
        S.sel = { kind, idx: i }; S.projFocus = null;
        if (e.projectile !== id) for (const q of mvProjChain(e.projectile, P)) for (const [l, v] of Object.entries(P.get(q))) if (l.startsWith('link_') && v === id && !S.projFocus) S.projFocus = { id, parent: q, field: l, how: '' };
        renderAll(); selReveal(); return;
      }
    }
    leave(); S.sel = null; S.projOnly = id; renderAll(); return;   // fired by no event (a runtime hook, or unused): the projectile on its own
  }
  if (c.move != null && S.moves[c.move] && S.moves[c.move].steps.length) {
    const m = S.moves[c.move];
    leave(); S.tab = 'moves'; S.whole = null; S.sel = null; S.openMove = null;
    const hm = c.hook && typeof ckMoveHooks === 'function' ? new Set(ckMoveHooks(m).flatMap((h) => h.motions)) : null;
    const i = hm ? m.steps.findIndex((x) => hm.has(x.motion)) : -1;
    S.step = { move: c.move, i: Math.max(0, i) }; S.moveOnly = i < 0;   // the hook's press: its animation's inspector has the hooks
    setMotion(m.steps[S.step.i].motion);
    if (i >= 0) { SEC_FOLD.delete('runtime hooks'); foldSave('studio.secFold', SEC_FOLD); }
    renderAll();
    const sec = i >= 0 && $('edHooks'); if (sec) sec.scrollIntoView({ block: 'start' });
    return;
  }
  if (c.tab) { leave(); S.tab = c.tab; if (c.tab === 'char') { S.step = null; S.sel = null; } renderAll(); return; }
  renderAll();
}
function alertsPopClose() {
  const pop = $('alertsPop'); if (!pop) return;
  pop.remove(); document.removeEventListener('mousedown', alertsPopOut, true); document.removeEventListener('keydown', alertsPopKey, true);
  $('checksBtn').setAttribute('aria-expanded', 'false');
}
function alertsPopOut(e) { if (!e.target.closest('#alertsPop, #checksBtn')) alertsPopClose(); }
function alertsPopKey(e) { if (e.key === 'Escape') { alertsPopClose(); $('checksBtn').focus(); } }
function alertsPopToggle() {
  if ($('alertsPop')) return alertsPopClose();
  const btn = $('checksBtn'), r = btn.getBoundingClientRect(), pop = document.createElement('div');
  pop.id = 'alertsPop'; pop.className = 'alerts-pop'; pop.setAttribute('role', 'dialog'); pop.setAttribute('aria-label', 'Alerts');
  pop.style.top = (r.bottom + 6) + 'px'; pop.style.right = Math.max(8, innerWidth - r.right) + 'px';
  document.body.appendChild(pop); alertsPopFill();
  btn.setAttribute('aria-expanded', 'true');
  document.addEventListener('mousedown', alertsPopOut, true); document.addEventListener('keydown', alertsPopKey, true);
}
function renderChecksPanel(I) {
  I.innerHTML = `<div class="sec"><h3>Checks <button type="button" id="ckClose">Close</button></h3>
    ${S.checks.length ? `<div class="checks">${S.checks.map((c, k) => `<div class="${c.level}" data-k="${k}">${esc(c.text)}</div>`).join('')}</div>` : '<p class="note">Nothing to report.</p>'}</div>`;
  $('ckClose').onclick = () => { S.showChecks = false; renderInspector(); };
  I.querySelectorAll('[data-k]').forEach((d) => d.onclick = () => {
    alertGo(S.checks[+d.dataset.k]);
  });
}

/* ====================================================== save */
async function ensureModsRoot() {               // a writable mods folder to put a new character folder in
  if (FS.kind && !FS.readonly && S.modsBase !== undefined) return true;
  if (!window.showDirectoryPicker) { toast('Saving a retail character as a mod needs Chrome or Edge (they can write into the mods folder).'); return false; }
  toast('Pick the repo\'s mods folder: the changed character goes in a folder there.');
  let root; try { root = await window.showDirectoryPicker({ id: 'dissidia-mods', mode: 'readwrite' }); } catch (e) { return false; }
  IDB.set('mods', root);
  const keep = { kind: FS.kind, root: FS.root, base: FS.base, mem: FS.mem, readonly: FS.readonly };
  FS.kind = 'handle'; FS.root = root; FS.readonly = false;
  for (const cand of MODS_CANDS) if (await scanMods(cand)) { fillCharPick(); $('charPick').value = charPickValue(); markMods(); return true; }
  Object.assign(FS, keep);
  toast('That folder has no character mods in it. Pick the repo\'s mods folder.');
  return false;
}
/* A changed retail character -> mods/<name>/ ("kind": "retail"): both movesets (fighter and assist) unpacked like
   charmod_objx.unpack, the 1P rigs, and a mod.json listing them. charmod.py build installs only the files that differ
   from the retail ones, over the retail paths, so the game uses them instead. */
async function saveRetailAsMod(quiet) {
  const { code } = S.rig.retail, c = S.retailChars.find((x) => x.code === code);
  if (!(await ensureModsRoot())) return null;
  let slug = slugOf(c.label), k = 1;
  while (await FS.exists(slug)) slug = slugOf(c.label) + '-retail' + (k++ > 1 ? '-' + k : '');
  const modFiles = {};
  try {
    for (const ms of [`p_${code}`, `p_${code}_a`]) {
      const objx = `obj/${ms}.objx`;
      if (!isoHas(objx)) continue;
      const cos = c.costumes.find(([x]) => x.endsWith('_a') === ms.endsWith('_a'));
      if (!cos) continue;
      const gmo = `obj/p_${code}_${cos[0]}.gmo`, gmoName = gmo.slice(4);
      const cur = S.rig.ms === 'retail/' + ms;
      const dec = await retailDecoded(objx, cur ? S.rig.gmo : gmo, cur ? S.boneNames : null);
      for (const [p, v] of Object.entries(dec)) {
        const live = cur && S.files[msPath(p)];
        if (cur && S.effe && S.effe.dirty && p === S.effe.rel) { await FS.write(`${slug}/moveset/${ms}/${p}`, S.effe.bytes); continue; }   // the edited effect package
        await FS.write(`${slug}/moveset/${ms}/${p}`, v instanceof Uint8Array ? v : wjText(live ? live.obj : v));
      }
      await FS.write(`${slug}/models/${gmoName}`, await isoGet(gmo));
      modFiles[objx] = `moveset/${ms}`;
      modFiles[gmo] = `models/${gmoName}`;
    }
    const mod = {
      kind: 'retail',
      character: { name: c.label, code },
      move_names: { ...(S.abilities[code] || {}) },
      files: modFiles,
    };
    await FS.write(`${slug}/mod.json`, JSON.stringify(mod, null, 1) + '\n');
  } catch (e) { toast('Save failed: ' + e.message, 'bad'); return null; }
  const rigGmo = `${slug}/models/${S.rig.gmo.slice(4).replace(/_(2p|3p|zako)(_a)?\.gmo$/, '_1p$2.gmo')}`;
  for (const f of Object.values(S.files)) f.dirty = false;
  if (S.effe) S.effe.dirty = false;
  const keepMotion = S.motion, keepStep = S.step, keepFrame = S.frame;
  await scanMods(S.modsBase);
  fillCharPick();
  await pickChar(slug, rigGmo);
  $('charPick').value = slug;
  S.step = keepStep; setMotion(keepMotion); setFrame(keepFrame); renderAll();
  if (!quiet) toast(`Saved as mods/${slug}. Start the game (run.sh builds it in) and it uses this ${c.label} instead of the retail one.`);
  return slug;
}
/* A retail character picked from the ISO, about to be changed (its body): switch to its mod folder, making it first
   (saveRetailAsMod) when there is none. -> true when the modded copy is open */
async function openRetailMod() {
  if (!S.rig || !S.rig.retail) return true;
  const { code } = S.rig.retail, have = retailModOf(code);
  if (have) {
    await pickChar(have.name, `${have.name}/models/p_${code}_1p.gmo`);
    $('charPick').value = have.name;
    renderAll();
    toast(`Your changed ${have.label.replace(/ \(Modded\)$/, '')} is in mods/${have.name}: the change goes there.`);
  } else {
    const slug = await saveRetailAsMod(true);
    if (!slug) return false;
    toast(`Made mods/${slug} for the change. Start the game (run.sh builds it in) to see it.`);
  }
  return !(S.rig && S.rig.retail);
}
async function save() {
  if (S.readOnly) return;
  if (S.rig && S.rig.retail) return saveRetailAsMod();
  const changed = Object.entries(S.files).filter(([p, f]) => fileChanged(f));
  const effe = S.effe && S.effe.dirty;
  if (!changed.length && !effe) return;
  if (FS.readonly) return downloadChanged(changed);
  // model files mod.json pointed at before and no longer does (costumes folded into one model): removed from the folder
  const mf = S.modPath && S.files[S.modPath], refs = (o) => new Set(Object.values((o && o.files) || {}).filter((v) => typeof v === 'string'));
  const dropped = mf && fileChanged(mf) ? [...refs(mf.orig)].filter((v) => /^models\/.*\.gmo$/.test(v) && !refs(mf.obj).has(v)) : [];
  try {
    for (const [p, f] of changed) {
      if (f.bin && f.created && !f.bin.length) continue;                    // a file a build would have written, undone
      if (f.bin) { await FS.write(p, f.bin); f.dirty = false; f.binOrig = f.bin; continue; }
      const text = fileText(f); await FS.write(p, text); f.text = text; f.dirty = false;
      if (f.mod) f.orig = JSON.parse(text);
    }
    if (effe) { await FS.write(S.effe.path, S.effe.bytes); S.effe.dirty = false; S.effe.orig = S.effe.bytes; changed.push([S.effe.path]); }
    undoSaved(changed.map(([p]) => p), effe);
    for (const rel of dropped) { await FS.remove(S.charDir + rel).catch((e) => console.warn('studio: remove', rel, e)); delete S.files[S.charDir + rel]; }
  } catch (e) { toast('Save failed: ' + e.message, 'bad'); return; }
  for (const f of Object.values(S.files)) f.dirty = false;
  refreshStatus();
  if (dropped.length) { await pickChar(S.char === 'character' ? '' : S.charDir.replace(/\/$/, ''), S.rig && S.rig.gmo); toast(`Saved, and removed ${dropped.length} costume model${dropped.length > 1 ? 's' : ''} the character no longer uses.`, 'ok'); return; }
  toast(`Saved ${changed.length} file${changed.length > 1 ? 's' : ''} into ${S.char}. Build the character to play it (run.sh builds it at launch).`);
}
function downloadChanged(changed) {
  for (const [p, f] of changed) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([f.bin || fileText(f)], { type: f.bin ? 'application/octet-stream' : 'application/json' }));
    a.download = p.replace(/\//g, '__'); a.click();
  }
  if (S.effe && S.effe.dirty) { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([S.effe.bytes])); a.download = S.effe.path.replace(/\//g, '__'); a.click(); changed = [...changed, [S.effe.path]]; }
  toast('Downloaded ' + changed.length + ' files (names show their folder path). Copy them into the mod folder.');
}

/* ====================================================== wiring */
function renderAll() { renderLeft(); renderTimeline(); renderInspector(); if ($('alertsPop')) alertsPopFill(); }
$('openBtn').onclick = openFolder; $('openBtn2').onclick = openFolder; $('modsReopen').onclick = reopenMods;
$('dirInput').onchange = (e) => openFromInput(e.target.files);
$('charPick').onchange = () => onCharPick($('charPick').value);
$('rigPick').onchange = async () => { if (dirtyCount() && !(await uiDiscard())) { $('rigPick').value = String(S.rigs.indexOf(S.rig)); return; } await loadRig(S.rigs[+$('rigPick').value]); };
$('reloadBtn').onclick = async () => { if (S.readOnly) return toast('Retail characters come from the ISO: nothing to reload.'); if (dirtyCount() && !(await uiConfirm('Reload and discard changes?', `${dirtyCount()} file${dirtyCount() > 1 ? 's have' : ' has'} unsaved changes. Reload reads the folder again and drops them.`, { ok: 'Discard and reload', danger: true }))) return; const keep = S.motion; await loadRig(S.rig); setMotion(keep); renderAll(); toast('Read ' + S.char + ' again'); };
$('saveBtn').onclick = save; $('undoBtn').onclick = undo; $('redoBtn').onclick = redo;
$('checksBtn').onclick = alertsPopToggle;
$('tabMoves').onclick = () => { const re = S.tab === 'fx' || S.tab === 'char'; S.tab = 'moves'; if (re) renderAll(); else renderLeft(); };   // (the inspector follows the tab)
$('tabAnims').onclick = () => { const re = S.tab === 'fx' || S.tab === 'char'; S.tab = 'anims'; if (re) renderAll(); else renderLeft(); };
$('tabFx').onclick = () => { S.tab = 'fx'; S.sel = null; renderLeft(); renderInspector(); };
if ($('tabChar')) $('tabChar').onclick = () => { S.tab = 'char'; S.step = null; S.sel = null; S.fxEdit = null; renderLeft(); renderInspector(); };
$('playBtn').onclick = () => {
  S.playing = !S.playing; S.autoplay = S.playing; $('playBtn').textContent = S.playing ? '❚❚' : '▶';
  if (!S.playing) return;                         // at the end (no Loop): play again from the start
  if (S.fxEdit && typeof fxuLength === 'function') { if (S.fxEdit.t >= fxuLength() - 0.01) S.fxEdit.t = 0; return; }
  if (S.moveOnly && S.step && !S.whole && S.moves[S.step.move] && S.moves[S.step.move].steps.length > 1) S.whole = { move: S.step.move };   // the whole move selected: press after press
  if (S.whole && typeof mvSegments === 'function') {           // at the end of the last press: from the first again
    const segs = mvSegments(S.moves[S.whole.move]), last = segs.length - 1;
    if (S.step.i >= last && S.frame >= (segs[last] ? segs[last].len : 0) - 0.01) { S.step = { move: S.whole.move, i: 0 }; S.motion = segs[0].motion; S.frame = 0; if (typeof dummyReset === 'function') dummyReset(); pose(); renderAll(); }
  } else if (S.motion >= 0 && S.frame >= motionLen(S.motion) - 0.01) S.frame = 0;
};
$('addBtn').onclick = (e) => openAddMenu(e.currentTarget);
$('zoom').oninput = () => { renderTimeline(); zoomSync(); };
/* zoom as two buttons (2026-11-01: "The zoom can be streamlined"); the hidden range keeps the level */
function zoomSync() { const z = $('zoom'); $('zoomOut').disabled = +z.value <= +z.min; $('zoomIn').disabled = +z.value >= +z.max; }
function zoomStep(d) { const z = $('zoom'), v = Math.max(+z.min, Math.min(+z.max, Math.round(+z.value * (d > 0 ? 1.25 : 0.8)) || +z.value + d)); if (v === +z.value) return; z.value = v; z.oninput(); }
$('lanes').addEventListener('wheel', (e) => { if (!e.ctrlKey && !e.metaKey) return; e.preventDefault(); zoomStep(e.deltaY < 0 ? 1 : -1); }, { passive: false });
$('zoomOut').onclick = () => zoomStep(-1); $('zoomIn').onclick = () => zoomStep(1); zoomSync();
$('showFx').onchange = () => { fxRefresh(); };
{ // Labels: name tags on the preview's effects and parts; remembered per browser
  const cb = $('fxLabelsOn');
  try { if (localStorage.getItem('studio.fxLabels') === '0') cb.checked = false; } catch (e) { /* defaults */ }
  cb.onchange = () => { try { localStorage.setItem('studio.fxLabels', cb.checked ? '1' : '0'); } catch (e) { /* per-viewer only */ } fxLabels(); };
}
{ // Hits: a tag on every hit (where it landed on the dummy, or the live windows); remembered per browser
  const cb = $('hitLabelsOn');
  if (cb) {
    try { if (localStorage.getItem('studio.hitLabels') === '0') cb.checked = false; } catch (e) { /* defaults */ }
    cb.onchange = () => { try { localStorage.setItem('studio.hitLabels', cb.checked ? '1' : '0'); } catch (e) { /* per-viewer only */ } fxLabels(); };
  }
}
$('showBones').onchange = () => { if (view.bonesHelper) view.bonesHelper.visible = $('showBones').checked; };
$('partsGame').onchange = () => updateParts();
function isTyping() { const a = document.activeElement; return !!a && (/SELECT|TEXTAREA/.test(a.tagName) || a.isContentEditable || (a.tagName === 'INPUT' && !/^(checkbox|radio|button|range|color)$/.test(a.type))); }
document.addEventListener('keydown', (e) => {
  const typing = isTyping();
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); save(); return; }
  if ((e.ctrlKey || e.metaKey) && !e.altKey && !typing && (e.key.toLowerCase() === 'y' || (e.key.toLowerCase() === 'z' && e.shiftKey))) { e.preventDefault(); redo(); return; }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z' && !typing) { e.preventDefault(); undo(); return; }
  if (typing || !S.moves) return;
  if (e.key === ' ') { e.preventDefault(); $('playBtn').click(); }
  else if (e.key === '/' && $('tlSearch') && !S.fxEdit) { e.preventDefault(); $('tlSearch').focus(); $('tlSearch').select(); }
  else if ((e.key === 'ArrowLeft' || e.key === 'ArrowRight') && e.altKey && !e.ctrlKey && !e.metaKey) { e.preventDefault(); tlNudge(e.key === 'ArrowRight' ? 1 : -1, e.shiftKey); }
  else if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && !e.altKey && !e.ctrlKey && !e.metaKey && (tlLabelFocused() || (S.sel && (!document.activeElement || document.activeElement === document.body)))) { e.preventDefault(); tlRowStep(e.key === 'ArrowDown' ? 1 : -1); }
  else if (e.key === 'Enter' && tlLabelFocused()) { e.preventDefault(); document.activeElement.click(); tlFocusSel(); }
  else if (e.key === 'ArrowRight') { if (tlWhole()) tlGoto(Math.floor(tlAt() + S.frame) + 1); else setFrame(Math.floor(S.frame) + 1); }
  else if ((e.key === 'f' || e.key === 'F') && !e.ctrlKey && !e.metaKey && !e.altKey) camCentre();
  else if (e.key === 'ArrowLeft') { if (tlWhole()) tlGoto(Math.ceil(tlAt() + S.frame) - 1); else setFrame(Math.ceil(S.frame) - 1); }
  else if ((e.key === 'Delete' || e.key === 'Backspace') && S.sel) { e.preventDefault(); deleteSel(); }
  else if (e.key === 'Escape') { closeMenus(); S.sel = null; renderTimeline(); renderInspector(); }
});
window.addEventListener('beforeunload', (e) => { if (dirtyCount()) { e.preventDefault(); e.returnValue = ''; } });
window.studio = { S, F, wjText, pyFloat, save, addEvent, selectEvent, setMotion, setFrame, renderAll, FS };
