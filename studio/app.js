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
function pushUndo(paths) {
  S.undo.push(paths.map((p) => ({ p, text: S.files[p].bin ? null : JSON.stringify(S.files[p].obj), bin: S.files[p].bin ? S.files[p].bin.slice() : null, dirty: S.files[p].dirty })));
  if (S.undo.length > 200) S.undo.shift();
  $('undoBtn').disabled = false;
}
function undo() {
  const u = S.undo.pop(); if (!u) return;
  S.gen = (S.gen || 0) + 1;
  for (const x of u) {
    if (x.effe) { fxuUndoEntry(x); continue; }
    if (x.bin) S.files[x.p].bin = x.bin; else S.files[x.p].obj = JSON.parse(x.text);
    S.files[x.p].dirty = x.dirty;
  }
  if (S.modPath && S.files[S.modPath]) S.mod = S.files[S.modPath].obj;
  if (typeof cpAfterUndo === 'function') cpAfterUndo(u);
  $('undoBtn').disabled = !S.undo.length;
  derive(); if (S.fxEdit) fxRefresh(); renderAll(); refreshStatus();
}
function edit(paths, fn) {
  if (S.readOnly) { readOnlyNote(); return; }
  pushUndo(paths); fn(); S.gen = (S.gen || 0) + 1; markDirty(paths); runChecks();
}
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
  st.title = S.readOnly ? `The retail ${S.mod.character.name} from ${ISO.name}; your changed one is under Mod characters` : n && retail ? `Save makes mods/${slugOf(S.mod.character.name)}` : '';
  st.classList.toggle('dirty', n > 0);
  $('saveBtn').disabled = !n;
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
  S.rig = rig; S.msDir = rig.ms; S.files = {}; S.undo = []; S.sel = null; S.step = null; S.newMove = null; S.whole = null; S.comp = null; S.comps = []; $('undoBtn').disabled = true;
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
    if (ent) { const bytes = await FS.read(msPath(ent.file)); S.effe = { path: msPath(ent.file), rel: ent.file, bytes, orig: bytes, dirty: false, pkg: null }; S.fx = loadEffects(bytes); }
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
    fxTexCache.clear(); fxGeoCache.clear();
    S.abilities = retailAbilities(mainBin, await isoGet('general_archive/main/en/main_lang.bin'));
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
  S.rig = rig; S.msDir = rig.ms; S.files = {}; S.undo = []; S.sel = null; S.step = null; S.newMove = null; S.whole = null; S.comp = null; S.comps = []; S.modPath = null; $('undoBtn').disabled = true;
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
  S.tlOfMotion = new Map();
  for (const p of S.tlPaths) { const m = F(p).motion; if (!S.tlOfMotion.has(m)) S.tlOfMotion.set(m, p); }
  const names = {};
  for (const [k, v] of Object.entries(S.mod.move_names || {})) if (!k.startsWith('_') && v && v.cmd) names[parseInt(v.cmd, 16)] = { ...v, key: k };
  S.moves = F(msPath('moves.json')).commands.map((c) => {
    const id = parseInt(c.id, 16), nm = names[id];
    const steps = c.steps.map((s, i) => {
      const a = S.actionById.get(s.action), script = a ? a.script : null, ms = script != null ? (S.motionsOfScript.get(script) || []) : [];
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
  scene.background = (() => {                     // the .viewport CSS gradient: ellipse at 50% 85%, #1b2140 -> --void at 70%
    const c = document.createElement('canvas'); c.width = c.height = 512; const g = c.getContext('2d');
    g.fillStyle = '#0b0d18'; g.fillRect(0, 0, 512, 512);
    g.save(); g.translate(256, 0.85 * 512); g.scale(1, 1.2 / 0.707);
    const r = 0.707 * 512, gr = g.createRadialGradient(0, 0, 0, 0, 0, r);
    gr.addColorStop(0, '#1b2140'); gr.addColorStop(0.7, '#0b0d18'); gr.addColorStop(1, '#0b0d18');
    g.fillStyle = gr; g.fillRect(-512, -1024, 1024, 2048); g.restore();
    const tx = new THREE.CanvasTexture(c); tx.encoding = THREE.sRGBEncoding; return tx;
  })();
  const camera = new THREE.PerspectiveCamera(35, 1, 0.01, 200);
  camera.position.set(2.4, 1.5, 4.6);
  const controls = new THREE.OrbitControls(camera, renderer.domElement);
  controls.target.set(0, 0.85, 0); controls.update();
  scene.add(new THREE.GridHelper(8, 32, 0x2b3466, 0x1a2046));
  const ring = new THREE.Mesh(new THREE.RingGeometry(0.55, 0.57, 64), new THREE.MeshBasicMaterial({ color: 0x8fd3ff, transparent: true, opacity: 0.35, side: THREE.DoubleSide }));
  ring.rotation.x = -Math.PI / 2; ring.position.y = 0.002; scene.add(ring);
  const resize = () => { const r = host.getBoundingClientRect(); renderer.setSize(r.width, r.height, false); camera.aspect = r.width / Math.max(1, r.height); camera.updateProjectionMatrix(); };
  new ResizeObserver(resize).observe(host); resize();
  return { renderer, scene, camera, controls, root: null, bones: [], partMeshes: [] };
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
function updateParts() {
  if (!view.partMeshes) return;
  const game = $('partsGame').checked;
  if (S.modelPose && game) {                        // Body / Costumes: the body and its weapons, not the effect parts
    const gs = (S.mod && S.mod.body && S.mod.body.weapon_groups) || [], shown = new Set(gs.flatMap((g) => g.parts || []));
    view.partMeshes.forEach((pm, pi) => { if (!pm) return; const n = (S.gmo.parts[pi] || {}).name || ''; pm.mesh.visible = pi === 0 || (shown.size ? shown.has(n) : /^wep\d+$/.test(n)); });
    return;
  }
  const st = partStates(curTL(), ticks(S.frame));
  view.partMeshes.forEach((pm, pi) => { if (!pm) return; pm.mesh.visible = !game || pi === 0 || (st.has(pm.draw) ? st.get(pm.draw) : true) && (S.exMode || !S.exOnly.has(pm.draw)); });
}
function pose() { if (S.gmo && S.modelPose) { poseMotion(S.gmo, view, -1, 0); updateParts(); return; } if (S.gmo) { poseMotion(S.gmo, view, S.motion, S.frame); updateParts(); if (typeof psndAfterPose === 'function') psndAfterPose(); if (typeof dmRefresh === 'function') dmRefresh(); if (typeof spRefresh === 'function') spRefresh(); fxRefresh(); if (typeof edVolDraw === 'function') edVolDraw(); if (typeof gcamRefresh === 'function') gcamRefresh(); } if (typeof fxfApply === 'function') fxfApply(); if (typeof ftApply === 'function') ftApply(); }
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
function fxPoseAt(tick) {
  const dmk = typeof DM !== 'undefined' && DM.on && DM.sim && !S.fxEdit ? DM.key + '|' + (S.step ? S.step.move + ':' + S.step.i : '') : '';
  if (FXP.gmo !== S.gmo || FXP.motion !== S.motion || FXP.dmk !== dmk) { FXP.gmo = S.gmo; FXP.motion = S.motion; FXP.dmk = dmk; FXP.map.clear(); }
  const T = Math.max(0, Math.round(tick));
  if (!FXP.map.has(T)) {
    if (!S.gmo || !view.root) return { root: new THREE.Matrix4(), bones: (view.bones || []).map(() => new THREE.Matrix4()) };
    // with the dummy on the attacker travels: pose him where he was at that tick, so what he left behind stays there
    const at = dmk && typeof dmAttackerAt === 'function' ? dmAttackerAt(T) : null;
    const keep = at ? { p: view.root.position.clone(), r: view.root.rotation.y } : null;
    if (at) { view.root.position.set(at.x, at.y, at.z); view.root.rotation.y = at.yaw; }
    poseMotion(S.gmo, view, S.motion, frames(T)); view.root.updateMatrixWorld(true);
    FXP.map.set(T, { root: view.root.matrixWorld.clone(), bones: view.bones.map((b) => b.matrixWorld.clone()) });
    if (keep) { view.root.position.copy(keep.p); view.root.rotation.y = keep.r; }
    FXP.dirty = true;
  }
  return FXP.map.get(T);
}
function fxRefresh() {
  try {
    if (view.root) view.root.updateMatrixWorld(true);
    const now = { root: view.root ? view.root.matrixWorld.clone() : new THREE.Matrix4(), bones: (view.bones || []).map((b) => b.matrixWorld.clone()) };
    const tickNow = S.frame * 2, names = (view.bones || []).map((b) => b.name);
    const solo = S.fxEdit && S.fx ? fxuSoloOpts() : null;
    fxUpdate(view, S.fx, curTL(), solo ? S.fxEdit.t : tickNow, { on: $('showFx').checked || !!solo, hitOf, motionName: motionName(S.motion), phase: stepPhase(), ex: !!S.exMode, solo, mute: solo && S.fxEdit.mute,
      poseAt: (T) => (solo || T >= tickNow - 1e-6 ? now : fxPoseAt(T)), extra: !solo ? [...(typeof compInsts === 'function' ? compInsts() || [] : []), ...(typeof spFxInsts === 'function' ? spFxInsts() : [])] : null, skipComp: S.comp ? compSlug(S.comp.path) : null, foeAt: !solo && typeof dmFoeAt === 'function' ? dmFoeAt : null, foePosAt: !solo && typeof dmFoePosAt === 'function' ? dmFoePosAt : null, boneIndex: (n) => (n === 'root' ? FX_ROOT_BONE : names.indexOf(n)), boneOk: (i) => i >= 0 && i < names.length, hasBone: (n) => names.includes(n) });
  } catch (e) { console.warn('studio: effect preview', e); }
  if (FXP.dirty) { FXP.dirty = false; if (S.gmo) { poseMotion(S.gmo, view, S.motion, S.frame); if (view.root) view.root.updateMatrixWorld(true); } }
}
function stepPhase() {                 // motion-map phase of the selected press (events filtered on another phase don't run)
  if (!S.step) return null;
  const s = S.moves[S.step.move] && S.moves[S.step.move].steps[S.step.i]; if (!s) return null;
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === s.script && r.motion === S.motion);
  return row ? row.phase & 0xFF : null;
}
function fxInfoCached(id) {
  if (!S.fx) return null;
  if (!S.fx.cache.has(id)) S.fx.cache.set(id, fxInfo(S.fx, id));
  return S.fx.cache.get(id);
}
function fxLabels() {                    // effect names at their attach points
  const L = $('fxLabels'); if (!L) return;
  if (!$('showFx').checked || !FXV.labels.length) { if (L.innerHTML) L.innerHTML = ''; return; }
  const r = view.renderer.domElement.getBoundingClientRect(), seen = new Set(); let h = '';
  for (const lb of FXV.labels) {
    if (seen.has(lb.text)) continue; seen.add(lb.text);
    const p = new THREE.Vector3().setFromMatrixPosition(lb.m).project(view.camera);
    if (p.z > 1) continue;
    h += `<span class="fxlabel" style="left:${((p.x + 1) / 2 * r.width).toFixed(0)}px;top:${((1 - p.y) / 2 * r.height).toFixed(0)}px">${esc((lb.text || '').replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, ''))}</span>`;
  }
  if (L.innerHTML !== h) L.innerHTML = h;
}
let lastT = 0;
function tick(t) {
  const dt = Math.min(0.1, (t - lastT) / 1000); lastT = t;
  if (typeof PH !== 'undefined' && PH.on) phTick(dt);           // Character tab > Jump, gravity, dash (newchar.js)
  else if (S.playing && S.fxEdit) fxuTick(dt);
  else if (S.playing && S.whole && mvTick(dt)) { /* a whole move, press after press (moves.js) */ }
  else if (S.playing && S.motion >= 0) {
    let f = S.frame + dt * 30 * parseFloat($('speed').value);
    const L = motionLen(S.motion);
    if (f >= L) { if ($('loop').checked) f = f % L; else { f = L; S.playing = false; $('playBtn').textContent = '▶'; } }
    S.frame = f; pose(); updatePlayhead();
  }
  if (!(typeof GCAM !== 'undefined' && GCAM.keep)) view.controls.update();   // the game camera drives it instead
  fxRender(view); fxLabels();
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

/* ====================================================== left pane */
// The move list grouped like the in-game kit: Brave attacks, then HP attacks, then the rest (EX Burst, unknown),
// ground before midair inside each; the file's order otherwise.
function mvGrouped() {
  const TYPES = ['brave', 'hp'], PH = ['ground', 'midair', 'follow-up'];
  const rows = S.moves.map((m, mi) => ({ m, mi })).filter(({ m }) => !m.folded).map(({ m, mi }) => { const k = typeof mvKind === 'function' ? mvKind(m) : {}; return { m, mi, t: TYPES.indexOf(k.type), p: PH.indexOf(k.phase), k }; });
  const rank = (x) => (x.t < 0 ? 9 : x.t) * 10 + (x.p < 0 ? 9 : x.p);
  rows.sort((a, b) => rank(a) - rank(b) || a.mi - b.mi);
  // a branch sits under the move it is a version of (Vaan's weapon switches: 0x25B under Sword & Shield), not where the
  // file has it
  const ids = new Set(rows.map((r) => r.m.id)), under = new Map();
  const isBranch = (r) => r.m.branchAt != null && r.m.variantOf != null && ids.has(r.m.variantOf) && r.m.variantOf !== r.m.id;
  for (const r of rows) if (isBranch(r)) { if (!under.has(r.m.variantOf)) under.set(r.m.variantOf, []); under.get(r.m.variantOf).push(r); }
  const flat = [];
  for (const r of rows) { if (isBranch(r)) continue; flat.push(r); for (const b of under.get(r.m.id) || []) flat.push({ ...b, t: r.t, p: r.p }); }
  const out = []; let last = null;
  for (const r of flat) {
    const g = r.t < 0 ? 'Other' : (r.t === 0 ? 'Brave attacks' : 'HP attacks') + (r.p < 0 ? '' : ' · ' + PH[r.p]);
    if (g !== last && !(g === 'Other' && last === null && rows.every((x) => x.t < 0))) out.push({ head: g });
    last = g; out.push(r);
  }
  return out;
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
    const kindTag = (m) => { const k = mvKind(m), rt = typeof ckMoveHooks === 'function' && ckMoveHooks(m).length; return (k.type ? `<em class="mvk ${k.type}">${k.type === 'hp' ? 'HP' : 'Brave'}</em>` : '') + (rt ? '<em class="mvk rt" title="Uses runtime hooks: in game it needs the recomp (see the move panel)">recomp</em>' : ''); };
    L.innerHTML = (mvCanEdit() ? '<button type="button" id="newMoveBtn" class="newmove" title="A new ability with its own command, presses and animation">+ New move</button>' : '')
      + mvGrouped().map(({ m, mi, head }) => {
        if (head) return `<div class="mv-group">${esc(head)}</div>`;
        const cur = S.step && S.step.move === mi, open = cur ? S.openMove !== -1 : S.openMove === mi;
        const tip = `${m.name}${m.branchAt != null ? ` (a branch: ${m.branchAt > 0 ? `from press ${m.branchAt + 1}` : 'instead of it'} it plays ${m.branchTag || 'other actions'})` : m.variantOf != null ? ' (a shorter version)' : ''} · command ${m.hexId}${m.info ? ' · ability ' + m.info.key : ''}${m.info && m.info.type ? ' · ' + m.info.type + (m.info.phase ? ', ' + m.info.phase : '') : ''}${m.disabled ? ' · switched off in mod.json' : ''}`;
        const steps = open ? `<div class="steps">${m.steps.map((s) => `<button type="button" class="step${cur && !S.moveOnly && S.step.i === s.i ? ' sel' : ''}" data-m="${mi}" data-s="${s.i}" title="press ${s.i + 1}: action ${s.action}, script ${s.script}${s.flags & 0x100 ? ', needs a press' : ', follows on hit'}"><i class="st-n">${s.i + 1}</i><span class="st-trig">${s.i === 0 ? 'Start' : s.flags & 0x100 ? 'Press again' : 'On hit'}</span><small class="st-m">${esc(s.motion >= 0 ? motionName(s.motion) : '?')}</small><small class="st-len">${s.motion >= 0 ? motionLen(s.motion) + 'f' : ''}</small></button>`).join('')}</div>` : '';
        return `<div class="move${m.disabled ? ' off' : ''}${cur ? ' sel' : ''}${cur && S.moveOnly ? ' whole' : ''}${open ? ' open' : ''}">
      <div class="mv-head" data-m="${mi}" title="${esc(tip)}" role="button" tabindex="0" aria-expanded="${open}">${kindTag(m)}<b>${m.branchAt != null ? `<span class="mv-branch">↳ ${m.branchEx ? 'EX Mode' : 'Variant'}${m.branchAt > 0 ? ` · from press ${m.branchAt + 1}` : ''}</span> <small class="mv-var mono">${esc(m.branchTag || '')}</small>` : esc(m.name)}${m.variantOf != null && m.branchAt == null ? ` <small class="mv-var">· ${m.steps.length} press${m.steps.length > 1 ? 'es' : ''}</small>` : ''}${m.disabled ? ' <small>off</small>' : ''}</b><span class="mv-n">${m.steps.length}</span><span class="mv-chev" data-m="${mi}" aria-hidden="true">▸</span></div>${steps}</div>`;
      }).join('');
    L.querySelectorAll('.step').forEach((b) => b.onclick = () => { const m = +b.dataset.m, s = +b.dataset.s; S.whole = null; S.step = { move: m, i: s }; S.moveOnly = false; S.sel = null; S.newMove = null; setMotion(S.moves[m].steps[s].motion); renderAll(); });
    const openMove = (m, chev) => {
      const cur = S.step && S.step.move === m, open = cur ? S.openMove !== -1 : S.openMove === m;
      if (chev && open) { S.openMove = cur ? -1 : null; renderLeft(); return; }   // the chevron folds it again
      if (!S.moves[m].steps.length) return;
      if (S.whole && S.whole.move !== m) S.whole = null;
      if (!cur || !S.moveOnly) { S.step = { move: m, i: 0 }; S.whole = null; setMotion(S.moves[m].steps[0].motion); }
      S.openMove = null; S.newMove = null;
      mvSelectWhole();                                 // the whole move: play goes press after press (a press is picked below)
    };
    L.querySelectorAll('.mv-head').forEach((h) => {
      h.onclick = (e) => openMove(+h.dataset.m, !!e.target.closest('.mv-chev'));
      h.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openMove(+h.dataset.m, false); } };
    });
    const nb = $('newMoveBtn'); if (nb) nb.onclick = mvStartNew;
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
function setMotion(mi) { S.fxEdit = null; S.motion = mi; S.frame = 0; if (!S.whole) playAuto(); pose(); }
/* the play button is a mode: while it's on, whatever you pick next (an animation, a press, an effect) plays from the start;
   paused, it stays paused. An animation that ends without Loop doesn't turn it off. */
function playAuto() { S.playing = !!S.autoplay; if ($('playBtn')) $('playBtn').textContent = S.playing ? '❚❚' : '▶'; }
function setFrame(f) { S.frame = Math.max(0, Math.min(f, motionLen(S.motion))); pose(); updatePlayhead(); }
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
  if (typeof v === 'object') return v.f;
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
function renderTimeline() {
  document.body.classList.toggle('fxmode', !!(S.fxEdit && S.fx));
  if (S.fxEdit && S.fx) return fxuTimeline();
  if (S.comp && typeof compObj === 'function' && compObj()) return compTimeline();
  const inner = $('lanesInner'), t = curTL(), px = PX();
  const L = S.motion >= 0 ? motionLen(S.motion) : 60;
  $('tlTitle').textContent = S.motion >= 0 ? motionName(S.motion) + (S.step ? ' — ' + S.moves[S.step.move].name + ' ' + (S.step.i + 1) + '/' + S.moves[S.step.move].steps.length : '') : 'No animation';
  const width = LABEL_W + (L + 10) * px;
  let h = `<div class="ruler" id="ruler" style="width:${width}px">`;
  const stepF = px >= 14 ? 5 : px >= 7 ? 10 : 20;
  for (let f = 0; f <= L + 10; f += stepF) h += `<span style="left:${LABEL_W + f * px}px">${f}</span>`;
  h += '</div>';
  if (!t) h += `<div class="lane" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px">No events yet</span></div>`;
  const rowsAll = timelineRows(t), cats = [];
  for (const r of rowsAll) { if (!cats.length || cats[cats.length - 1].cat !== r.info.cat) cats.push({ cat: r.info.cat, rows: [] }); cats[cats.length - 1].rows.push(r); }
  for (const g of cats) {
    const folded = TL_FOLD.has(g.cat) && !g.rows.some((r) => S.sel && S.sel.kind === r.kind && S.sel.idx === r.idx);   // the selected event's stays open
    // a folded category keeps one lane with its events drawn small, so their timing still shows
    const marks = folded ? g.rows.map((r) => r.kind === 'ranged'
      ? `<div class="ev mini" style="left:${LABEL_W + frames(r.e.start) * px}px;width:${Math.max(3, ((r.e.end === FOREVER ? L : Math.max(frames(r.e.end), frames(r.e.start) + 0.25)) - frames(r.e.start)) * px)}px;background:var(${CAT_VAR[r.info.cat] || '--c-other'})"></div>`
      : `<div class="ev key mini" style="left:${LABEL_W + frames(r.e.frame) * px}px;background:var(${CAT_VAR[r.info.cat] || '--c-other'})"></div>`).join('') : '';
    h += `<div class="lane cat${folded ? ' folded' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-fold="${esc(g.cat)}" title="${folded ? 'Show' : 'Fold'} ${esc(g.cat)}"><i class="ic ic-chevron-down fold-ch" aria-hidden="true"></i>${esc(g.cat)}<small>${g.rows.length}</small></span>${marks}</div>`;
    if (folded) continue;
    for (const r of g.rows) {
    const sel = S.sel && S.sel.kind === r.kind && S.sel.idx === r.idx;
    const col = `var(${CAT_VAR[r.info.cat] || '--c-other'})`, sum = argSummary(r.e, r.info);
    const label = r.info.label + (sum !== '' ? ' · ' + sum : '');
    let bar;
    if (r.kind === 'ranged') {
      const s = frames(r.e.start), en = r.e.end === FOREVER ? L : Math.max(frames(r.e.end), s + 0.25);
      bar = `<div class="ev${r.e.end === FOREVER ? ' open' : ''}${sel ? ' sel' : ''}${typeof evMode === 'function' && !evMode(r.e) ? ' off' : ''}" data-k="r" data-i="${r.idx}" style="left:${LABEL_W + s * px}px;width:${Math.max(3, (en - s) * px)}px;background:${col}" title="${esc(label)}: ${fmtF(r.e.start)}–${fmtF(r.e.end)}"><span class="h l"></span><span class="h r"></span></div>`;
    } else {
      bar = `<div class="ev key${sel ? ' sel' : ''}${typeof evMode === 'function' && !evMode(r.e) ? ' off' : ''}" data-k="k" data-i="${r.idx}" style="left:${LABEL_W + frames(r.e.frame) * px}px;background:${col}" title="${esc(label)} @ ${fmtF(r.e.frame)}"></div>`;
      const c = opCode(r.e.op);
      if (c === 0x0219 || c === 0x042F) {                // how long the effect plays
        const fi = fxInfoCached(argsOf(r.e)[0]);
        if (fi && fi.duration) bar = `<div class="fxtail" style="left:${LABEL_W + frames(r.e.frame) * px}px;width:${(fi.duration / 2) * px}px;background:${col}" title="${esc(fi.name)}: plays ${fi.duration / 2} frames"></div>` + bar;
      }
    }
    h += `<div class="lane${sel ? ' selrow' : ''}" style="width:${width}px"><span class="lane-label" style="width:${LABEL_W}px" data-k="${r.kind[0]}" data-i="${r.idx}" title="${esc(r.info.desc || '')}">${esc(label)}</span>${bar}</div>`;
    }
  }
  h += `<div class="endline" style="left:${LABEL_W + L * px}px"></div><div class="playhead" id="playhead"></div>`;
  inner.innerHTML = h; inner.style.width = width + 'px';
  mvStrip();
  updatePlayhead();
  inner.querySelectorAll('.lane-label[data-k]').forEach((el) => el.onclick = () => selectEvent(el.dataset.k === 'r' ? 'ranged' : 'keyed', +el.dataset.i, true));
  inner.querySelectorAll('[data-fold]').forEach((el) => el.onclick = () => { const c = el.dataset.fold; TL_FOLD.has(c) ? TL_FOLD.delete(c) : TL_FOLD.add(c); foldSave('studio.tlFold', TL_FOLD); renderTimeline(); });
  inner.querySelectorAll('.ev:not(.mini)').forEach((el) => el.addEventListener('pointerdown', (ev) => startDrag(ev, el)));
  const ruler = $('ruler');
  const scrub = (ev) => { const r = ruler.getBoundingClientRect(); setFrame(Math.round(((ev.clientX - r.left) - LABEL_W) / px * 2) / 2); };
  ruler.addEventListener('pointerdown', (ev) => { ruler.setPointerCapture(ev.pointerId); scrub(ev); ruler.onpointermove = scrub; ruler.onpointerup = () => { ruler.onpointermove = null; }; });
}
/* inspector sections fold by their title (click it): the title's own text is the key, so "Effects & sound", "Voice",
   "Hit" ... stay folded from one event to the next */
const secKey = (h3) => { for (const n of h3.childNodes) if (n.nodeType === 3 && n.textContent.trim()) return n.textContent.trim().toLowerCase(); return ''; };
function secFoldApply(root) {
  for (const sec of root.querySelectorAll('.sec')) {
    const h3 = sec.querySelector(':scope > h3'); if (!h3) continue;
    const k = secKey(h3); if (!k) continue;
    sec.classList.add('foldable'); sec.classList.toggle('folded', SEC_FOLD.has(k));
    h3.title = SEC_FOLD.has(k) ? 'Show' : 'Fold';
  }
}
(function secFoldWire() {
  const insp = $('insp'); if (!insp) return;
  new MutationObserver(() => secFoldApply(insp)).observe(insp, { childList: true, subtree: true });
  insp.addEventListener('click', (e) => {
    const h3 = e.target.closest('.sec > h3'); if (!h3 || e.target.closest('button, input, select, label, a, textarea')) return;
    const k = secKey(h3); if (!k) return;
    SEC_FOLD.has(k) ? SEC_FOLD.delete(k) : SEC_FOLD.add(k); foldSave('studio.secFold', SEC_FOLD);
    secFoldApply(insp);
  });
})();
function updatePlayhead() {
  const ph = $('playhead'); if (ph) ph.style.left = (LABEL_W + S.frame * PX()) + 'px';
  $('tlFrame').textContent = S.motion >= 0 ? `frame ${S.frame.toFixed(1)} / ${motionLen(S.motion)}` : '';
  mvStripHead();
}
function selectEvent(kind, idx, jump) {
  S.sel = { kind, idx };
  const e = curTL()[kind][idx];
  if (jump) setFrame(frames(kind === 'ranged' ? e.start : e.frame));
  renderTimeline(); renderInspector();
}
function startDrag(ev, el) {
  ev.preventDefault();
  const kind = el.dataset.k === 'r' ? 'ranged' : 'keyed', idx = +el.dataset.i, p = curTLPath(), e = F(p)[kind][idx];
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
    if (kind === 'keyed') el.style.left = (LABEL_W + frames(ev2.frame) * px) + 'px';
    else {
      const en = ev2.end === FOREVER ? motionLen(S.motion) : Math.max(frames(ev2.end), frames(ev2.start) + 0.25);
      el.style.left = (LABEL_W + frames(ev2.start) * px) + 'px';
      el.style.width = Math.max(3, (en - frames(ev2.start)) * px) + 'px';
    }
    setFrame(frames(kind === 'ranged' ? (mode === 'r' ? (ev2.end === FOREVER ? ev2.start : ev2.end) : ev2.start) : ev2.frame));
  };
  el.onpointerup = () => { el.onpointermove = null; if (moved) { runChecks(); renderTimeline(); renderInspector(); } else selectEvent(kind, idx, true); };
}

/* ====================================================== inspector */
function numField(label, value, attrs = '') { return `<div class="field"><label>${label}</label><input type="number" ${attrs} value="${esc(value)}"></div>`; }
function renderInspector() {
  renderInspector0();
  if (S.readOnly) $('insp').querySelectorAll('input, select, textarea, button:not(#ckClose)').forEach((el) => { el.disabled = true; });
}
function renderInspector0() {
  const I = $('insp');
  if (S.showChecks) return renderChecksPanel(I);
  if (S.showChanges && typeof renderChangesPanel === 'function') { if (dirtyCount()) return renderChangesPanel(I); S.showChanges = false; }
  if (!S.moves) return;
  if (S.fxEdit && S.fx) return fxuInspector(I);
  if (S.comp && typeof compObj === 'function' && compObj()) return compInspector(I);
  if (S.newMove) { I.innerHTML = mvNewHTML(); mvWireNew(); return; }
  if (S.tab === 'char' && !S.step && !S.sel && typeof ncInspector === 'function') return ncInspector(I);
  const t = curTL();
  if (S.sel && t && t[S.sel.kind][S.sel.idx]) return renderEvent(I, S.sel.kind, S.sel.idx, t[S.sel.kind][S.sel.idx]);
  S.sel = null;
  let h = '';
  if (S.step && S.moveOnly) {                   // the whole move: its panel only, until a press is picked
    const m = S.moves[S.step.move], seen = new Set(), rows = [];
    (m.steps || []).forEach((st, k) => { if (st.motion >= 0 && !seen.has(st.motion)) { seen.add(st.motion); rows.push({ mi: st.motion, note: 'press ' + (k + 1) }); } });
    I.innerHTML = mvMoveHTML() + blSectionHTML(rows, 'Animations', 'Edit any of the move\'s animations in Blender. Save moves to folder there, then Reload here to see the change.') + '<p class="note" style="padding:0 10px">Pick a press under the move for its animation and events.</p>'; mvWire(); blWire(); return;
  }
  if (S.step) h += mvMoveHTML() + (S.moves[S.step.move].steps[S.step.i] ? renderStepHTML() + edPressHTML() : `<div class="sec"><h3>${esc(motionName(S.motion))} <small>started by a runtime hook</small></h3><p class="note">${esc((mvSegments(S.moves[S.step.move])[S.step.i] || {}).why || '')}: not one of the move's presses; the dummy shows it because the game would play it here.</p></div>`);
  h += renderAnimHTML() + blSectionHTML([{ mi: S.motion }], 'Edit this animation', 'Opens it in Blender to change the pose and timing. Save moves to folder there, then Reload here to see the change.') + edHooksHTML(S.motion);
  I.innerHTML = h;
  mvWire(); wireStep(); wireAnim();
  if (S.step && S.moves[S.step.move].steps[S.step.i]) edPressWire();
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
  return `<div class="sec"><h3>${esc(m.name)} <small>press ${s.i + 1} of ${m.steps.length}</small></h3>
    <p class="note">${m.info && m.info.help ? esc(m.info.help.split('\n')[0]) : ''}</p>
    <div class="field"><label>Animation this press plays</label><select id="stepMotion">${opts}</select></div>
    ${shared.length ? `<p class="warnline">Script ${s.script} is also used by ${esc(shared.join(', '))}: changing the animation changes it there too.</p>` : ''}
    <p class="note mono">${m.burst ? `EX Burst · script ${s.script} · ${esc(s.role)}` : `command ${esc(m.hexId)} · action ${s.action} · script ${s.script}${s.flags & 0x100 ? ' · needs a press' : ' · follows on hit'}`}</p></div>`;
}
function wireStep() {
  const sm = $('stepMotion'); if (!sm) return;
  sm.onchange = () => {
    const m = S.moves[S.step.move], s = m.steps[S.step.i], p = msPath('motion_map.json'), mi = +sm.value;
    edit([p], () => { for (const r of F(p).rows) if (r.script === s.script) r.motion = mi; });
    derive(); setMotion(mi); renderAll();
    if (!S.tlOfMotion.has(mi)) toast(motionName(mi) + ' has no events yet: create its timeline in the inspector.');
  };
}
function renderAnimHTML() {
  const i = S.motion, t = curTL(), used = S.usedBy.get(i) || [];
  let h = `<div class="sec"><h3>${esc(motionName(i))} <small>${motionLen(i)} frames</small></h3>
    <p class="note">${used.length ? 'Used by ' + esc(used.join(', ')) : 'Not used by a move yet.'}</p>`;
  if (!t) {
    let opts = '<option value="">empty</option>';
    for (const p of S.tlPaths) { const mo = F(p).motion; opts += `<option value="${esc(p)}">copy ${esc(motionName(mo))}</option>`; }
    h += `<p class="note">No event timeline yet.</p><div class="field"><label>Start from</label><select id="tlFrom">${opts}</select></div><button type="button" id="tlCreate" class="primary">Create timeline</button></div>`;
    return h + dupSectionHTML();
  }
  const rows = timelineRows(t);
  h += `<p class="note">${rows.length} events. Click one on the timeline (or its name) to edit it; drag bars to move them, their ends to resize. <span class="kbd">Alt</span> = half frames.</p></div>`;
  // parts hidden at start
  const hides = new Set(t.keyed.filter((e) => isSetupHide(e, 'keyed')).map((e) => partIndex(e.part ?? (e.args || [])[0])));
  const parts = S.gmo ? S.gmo.bones.map((b, bi) => ({ b, bi })).filter((x) => x.b.drawPart > 0) : [];
  parts.sort((a, b) => a.b.name.localeCompare(b.b.name, undefined, { numeric: true }));
  h += `<details class="sec"><summary class="lbl" style="cursor:pointer">Parts hidden at start (${hides.size} of ${parts.length})</summary>
    <p class="note">Ticked = not drawn when this animation starts. A <i>Show part</i> event can still show it.</p>
    <div class="parts">${parts.map((x) => `<label class="chk"><input type="checkbox" data-b="${x.bi}"${hides.has(x.bi) ? ' checked' : ''}> ${esc(x.b.name)}${S.exOnly.has(x.bi) ? ' <small style="color:var(--dim)">EX</small>' : ''}</label>`).join('')}</div></details>`;
  const mine = S.checks.filter((c) => c.motion === i);
  if (mine.length) h += `<div class="sec"><h3>Checks</h3><div class="checks">${mine.map((c) => `<div class="${c.level}">${esc(c.text)}</div>`).join('')}</div></div>`;
  h += dupSectionHTML();
  return h;
}
/* a copy of the current animation as a new motion of this character (mod characters only) */
function dupSectionHTML() {
  if (!S.rig || S.rig.retail || S.readOnly) return '';
  const i = S.motion, s = S.step && !S.moveOnly && S.moves[S.step.move] && S.moves[S.step.move].steps[S.step.i], canPoint = s && s.motion === i;
  const shared = canPoint ? S.moves.flatMap((mm) => mm.steps.filter((x) => x.script === s.script && !(mm === S.moves[S.step.move] && x.i === s.i)).map((x) => mm.name + ' ' + (x.i + 1))) : [];
  return `<details class="sec"><summary class="lbl" style="cursor:pointer">Duplicate this animation</summary>
    <p class="note">Adds a copy of ${esc(motionName(i))} as a new animation of this character, so you can change it without touching the moves that share the original. Poses are then edited in Blender.</p>
    <div class="field"><label>Name</label><input id="dupName" type="text" maxlength="31" value="${esc(motionName(i) + '_2')}" spellcheck="false"></div>
    <label class="chk"><input type="checkbox" id="dupEvents"${S.tlOfMotion.has(i) ? ' checked' : ' disabled'}> Copy its events</label>
    ${canPoint ? `<label class="chk"><input type="checkbox" id="dupPoint" checked> This press (${esc(S.moves[S.step.move].name)} ${s.i + 1}) plays the copy</label>${shared.length ? `<p class="note">Script ${s.script} is shared with ${esc(shared.join(', '))}: they would play the copy too.</p>` : ''}` : ''}
    <button type="button" id="dupGo" class="primary">Duplicate</button></details>`;
}
function wireAnim() {
  blWire();
  const c = $('tlCreate');
  if (c) c.onclick = () => createTimeline($('tlFrom').value);
  const dg = $('dupGo');
  if (dg) dg.onclick = () => { dg.disabled = true; const pt = $('dupPoint'); dupMotion(S.motion, $('dupName').value.trim(), !!($('dupEvents') && $('dupEvents').checked), pt && pt.checked ? S.step : null).finally(() => { dg.disabled = false; }); };
  $('insp').querySelectorAll('.parts input').forEach((cb) => cb.onchange = () => {
    const p = curTLPath(), bi = +cb.dataset.b;
    edit([p], () => {
      const t = F(p), name = S.boneNames[bi], v = S.boneIndex.get(name) === bi ? name : bi;
      if (cb.checked) t.keyed.push({ frame: 0, op: 'hide_part_once', part: v });
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
  let h = `<div class="sec"><h3>${esc(info.cat)} <small class="mono">${esc(info.op)} · ${kind === 'ranged' ? 'start → end' : 'one frame'}</small></h3>
    <p class="title-ev">${esc(info.label)}</p><p class="note">${esc(info.desc || '')}</p>${typeof spEventNote === 'function' ? spEventNote(e) : ''}`;
  if (kind === 'ranged') {
    const open = e.end === FOREVER;
    h += `<div class="row2">${numField('Start (frame)', frames(e.start), 'id="evStart" min="0" step="0.5"')}
      ${numField('End (frame)', open ? '' : frames(e.end), `id="evEnd" min="0" step="0.5"${open ? ' disabled placeholder="end"' : ''}`)}</div>
      <label class="chk"><input type="checkbox" id="evOpen"${open ? ' checked' : ''}> Lasts to the end of the animation</label>`;
  } else h += numField('Frame', frames(e.frame), 'id="evFrame" min="0" step="0.5"');
  // named values
  named.forEach((n) => { h += argEditor(n, e[n], 'n:' + n); });
  const rest = e.args || [];
  if (rest.length) {
    h += `<div class="field"><label>Other values</label><div class="row3">${rest.map((v, k) => `<input type="text" class="mono" data-arg="${k}" value="${esc(typeof v === 'object' ? v.f + 'f' : v)}" title="value ${named.length + k + 1}${typeof v === 'object' ? ' (float)' : ''}">`).join('')}</div>
      <p class="note">Numbers; <span class="mono">1.5f</span> for a float.</p></div>`;
  }
  const fk = kind === 'ranged' ? 'pad' : 'x', fv = e[fk] || 0;
  h += `<details><summary class="lbl" style="cursor:pointer">Filter ${fv ? '(' + fv + ')' : '(always)'}</summary><div class="row2" style="margin-top:6px">
      ${numField('Phase (0 = any)', fv & 0xFF, 'id="fPhase" min="0" max="255"')}
      <div class="field"><label>Mode</label><select id="fMode">${[[0, 'Always'], [1, 'Outside EX Mode'], [2, 'EX Mode only']].map(([v, l]) => `<option value="${v}"${((fv >> 8) & 0xFF) === v ? ' selected' : ''}>${l}</option>`).join('')}${[0, 1, 2].includes((fv >> 8) & 0xFF) ? '' : `<option selected value="${(fv >> 8) & 0xFF}">${(fv >> 8) & 0xFF}</option>`}</select></div></div>
      <p class="note">Phase: when one animation is shared by several moves, only the move whose motion-map phase matches runs it.</p></details>`;
  h += `<div class="btnrow"><button type="button" id="evDup">Duplicate at playhead</button><button type="button" id="evDel" class="danger">Delete</button></div></div>`;
  // hit
  const hn = named.find((n) => n === 'hit');
  if (hn !== undefined) h += hitHTML(hitOf(e.hit));
  const oc = opCode(e.op), fxId = [0x0219, 0x042F, 0x0466].includes(oc) ? argsOf(e)[0] : oc === 0x042A ? argsOf(e)[1] : null;
  const fi = fxId != null && !(oc === 0x0219 && kind === 'ranged') ? fxInfoCached(fxId) : null;
  if (fi) h += fxHTML(fi, fxId);
  h += edEventHTML(kind, idx, e);                  // hitboxes, projectile, more hit settings (editors.js)
  I.innerHTML = h;
  if (fi) { const T = $('fxThumbs'); if (T) for (const id of fi.tex.slice(0, 12)) { const cv = fxTexCanvas(S.fx.fep, id, 56); if (cv) T.appendChild(cv); } }
  wireEvent(p, kind, idx, named);
  edEventWire(p, kind, idx);
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
function argEditor(n, v, key) {
  const lab = n.replace(/_/g, ' ');
  if (n === 'hit') {
    const keys = [...S.hitByName.keys()];
    return `<div class="field"><label>Hit</label><select data-n="${n}">${keys.map((k) => `<option${k === v || (typeof v === 'number' && k === hex4(v)) ? ' selected' : ''}>${esc(k)}</option>`).join('')}${S.hitByName.has(v) ? '' : `<option selected>${esc(v)}</option>`}</select></div>`;
  }
  if (n === 'part') {
    const cur = partIndex(v);
    return `<div class="field"><label>${n === 'part' ? 'Part / bone' : lab}</label><select data-n="${n}">${S.boneNames.map((b, i) => `<option value="${i}"${i === cur ? ' selected' : ''}>${esc(b)}</option>`).join('')}${cur < 0 ? `<option selected value="${esc(v)}">${esc(v)}</option>` : ''}</select></div>`;
  }
  if (n === 'effect' && S.effects.size && typeof v === 'number') {
    return `<div class="field"><label>Effect</label><select data-n="${n}">${[...S.effects.values()].map((x) => `<option value="${x.id}"${x.id === v ? ' selected' : ''}>${x.id} · ${esc(x.name.replace(/^P\d+_/, '').replace(/_LEAF_\d+$/, ''))}${x.bones.length ? ' (' + esc(x.bones.join(', ')) + ')' : ''}</option>`).join('')}${S.effects.has(v) ? '' : `<option selected value="${v}">${v}</option>`}</select></div>`;
  }
  if (v && typeof v === 'object') return `<div class="field"><label>${esc(lab)}</label><input type="number" step="any" data-n="${n}" data-f="1" value="${v.f}"></div>`;
  let extra = '';
  if (n === 'projectile') { const a = S.projAtkp.get(v), hh = a != null ? hitOf(a) : null; if (hh) extra = `<p class="note">hits with ${esc(hh.name)}</p>`; }
  return `<div class="field"><label>${esc(lab)}</label><input type="number" data-n="${n}" value="${esc(v)}">${extra}</div>`;
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
  const fset = () => change(() => { const e = ev(), fk = kind === 'ranged' ? 'pad' : 'x'; const v = ((+$('fMode').value & 0xFF) << 8) | (+$('fPhase').value & 0xFF) | ((e[fk] || 0) & ~0xFFFF); if (v) e[fk] = v; else delete e[fk]; });
  $('fPhase').onchange = fset; $('fMode').onchange = fset;
  $('evDel').onclick = () => deleteSel();
  $('evDup').onclick = () => {
    edit([p], () => {
      const src = JSON.parse(JSON.stringify(ev())), t0 = ticks(S.frame);
      if (kind === 'ranged') { if (src.end !== FOREVER) src.end = Math.min(0xFFFE, t0 + (src.end - src.start)); src.start = t0; } else src.frame = t0;
      delete src._note; delete src.what;
      F(p)[kind].push(src); S.sel = { kind, idx: F(p)[kind].length - 1 };
    });
    renderTimeline(); renderInspector();
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
function hitHTML(h) {
  if (!h) return '<div class="sec"><h3>Hit</h3><p class="warnline">This hit is not in hits.json.</p></div>';
  const users = hitUsers(h), flags = parseInt(h.flags, 16);
  const ro = h.raw ? ' disabled' : '';
  return `<div class="sec" id="hitSec"><h3>Hit · ${esc(h.name)} <small class="mono">${esc(h.id)}</small></h3>
    ${h.raw ? '<p class="warnline">This record keeps raw bytes (a value that does not round-trip); edit hits.json by hand.</p>' : ''}
    <p class="note">Used ${users.length}×: ${esc([...new Set(users)].join(', '))}</p>
    ${users.length > 1 ? '<button type="button" id="hitCopy">Give this event its own copy</button>' : ''}
    <div class="row2"><div class="field"><label>Name</label><input type="text" id="h_name" maxlength="15" value="${esc(h.name)}"${ro}></div>
    ${numField('Power (brave damage)', h.power, `id="h_power" min="0" max="9999"${ro}`)}</div>
    <div class="row2">${numField('Re-hit every N ticks (0 = once)', h.rehit, `id="h_rehit" min="0"${ro}`)}
      <div class="field"><label>Reaction</label><select id="h_reaction"${ro}>${Object.entries(REACT).map(([v, l]) => `<option value="${v}"${+v === h.reaction ? ' selected' : ''}>${l}</option>`).join('')}${REACT[h.reaction] ? '' : `<option selected value="${h.reaction}">${hex4(h.reaction).slice(4)} ?</option>`}</select></div></div>
    <div class="btnrow"><label class="chk"><input type="checkbox" id="h_hp"${flags & 2 ? ' checked' : ''}${ro}> HP attack</label><label class="chk"><input type="checkbox" id="h_rand"${flags & 1 ? ' checked' : ''}${ro}> Random power</label></div>
    <div class="lbl">Knock-back</div>
    <div class="row2">${numField('Forward (m)', h.kb_forward, `id="h_kb_forward" step="0.1"${ro}`)}${numField('Up (m)', h.kb_up, `id="h_kb_up" step="0.1"${ro}`)}</div>
    <div class="row3">${numField('Stun (ticks)', h.stun, `id="h_stun" step="1"${ro}`)}${numField('Follow', h.follow, `id="h_follow" step="1"${ro}`)}${numField('Turn (°)', h.kb_yaw, `id="h_kb_yaw" step="1"${ro}`)}</div>
    ${numField('Hit-stop ?', h.hitstop, `id="h_hitstop" min="0" max="255"${ro}`)}
    <p class="note">${esc(OPS.hit_info.power || '')}</p></div>`;
}
function wireHit(p, kind, idx) {
  if (!$('hitSec')) return;
  const hp = msPath('hits.json'), e = F(p)[kind][idx], h = hitOf(e.hit);
  if (!h || h.raw) return;
  const set = (fn) => { edit([hp], fn); derive(); renderTimeline(); };
  const num = (id, k, int) => { const el = $(id); if (el) el.onchange = () => set(() => { const v = parseFloat(el.value); h[k] = isNaN(v) ? h[k] : (int ? Math.round(v) : v); }); };
  num('h_power', 'power', true); num('h_rehit', 'rehit', true); num('h_hitstop', 'hitstop', true);
  num('h_kb_forward', 'kb_forward'); num('h_kb_up', 'kb_up'); num('h_stun', 'stun'); num('h_follow', 'follow'); num('h_kb_yaw', 'kb_yaw');
  $('h_reaction').onchange = () => set(() => { h.reaction = +$('h_reaction').value; });
  const fl = () => set(() => { let f = parseInt(h.flags, 16) & ~3; if ($('h_hp').checked) f |= 2; if ($('h_rand').checked) f |= 1; h.flags = '0x' + (f >>> 0).toString(16).toUpperCase().padStart(8, '0'); });
  $('h_hp').onchange = fl; $('h_rand').onchange = fl;
  $('h_name').onchange = () => {
    const nn = $('h_name').value.trim().slice(0, 15);
    if (!nn || S.hitByName.has(nn)) { toast(nn ? 'A hit is already called ' + nn : 'Name needed'); renderInspector(); return; }
    const old = hitKey(h);
    edit([hp, ...S.tlPaths], () => { h.name = nn; for (const tp of S.tlPaths) for (const ev2 of [...F(tp).ranged, ...F(tp).keyed]) if (ev2.hit === old) ev2.hit = nn; });
    derive(); renderAll();
  };
  const cp = $('hitCopy');
  if (cp) cp.onclick = () => {
    const hits = F(hp).hits, ids = new Set(hits.map((x) => parseInt(x.id, 16)));
    let nid = Math.max(...ids) + 1; while (ids.has(nid)) nid++;
    const base = (h.name || 'HIT').slice(0, 11); let k = 2; const names = new Set(hits.map((x) => x.name));
    while (names.has(base + '_' + k)) k++;
    const copy = JSON.parse(JSON.stringify(h)); copy.id = hex4(nid); copy.name = base + '_' + k; delete copy.name_raw;
    edit([hp, p], () => { hits.push(copy); F(p)[kind][idx].hit = copy.name; });
    derive(); renderAll(); toast('New hit ' + copy.name + ' for this event');
  };
}

/* a popup menu next to its button, kept on screen: below it if there is room, else above, never taller than the space */
function menuPlace(m, anchor, width = 360) {
  const r = anchor.getBoundingClientRect(), below = innerHeight - r.bottom - 12, above = r.top - 12;
  m.style.left = Math.max(8, Math.min(innerWidth - width - 10, r.left)) + 'px';
  m.style.right = 'auto'; m.style.bottom = 'auto';
  if (below >= Math.min(320, above) || below >= above) { m.style.top = (r.bottom + 4) + 'px'; m.style.maxHeight = Math.max(160, below) + 'px'; }
  else { const h = Math.min(above, innerHeight * 0.7); m.style.maxHeight = h + 'px'; m.style.top = 'auto'; m.style.bottom = (innerHeight - r.top + 4) + 'px'; }
}
/* ====================================================== add event */
function openAddMenu(anchor) {
  closeMenus();
  const m = document.createElement('div'); m.className = 'menu'; m.id = 'addMenu';
  m.innerHTML = '<input type="search" id="addFilter" placeholder="Find an event (hit, chain, sound, invincible …)"><div class="list" id="addList"></div>';
  document.body.appendChild(m); menuPlace(m, anchor, 370);
  const entries = Object.entries(OPS.ops).filter(([k, v]) => v.uses > 0 || v.name).map(([k, v]) => ({ k, v }));
  const draw = () => {
    const q = $('addFilter').value.toLowerCase();
    let h = '';
    for (const cat of OPS.cats) {
      const list = entries.filter(({ v }) => v.cat === cat && (!q || (v.label + ' ' + v.desc + ' ' + (v.name || '') + ' ' + v.op).toLowerCase().includes(q))).sort((a, b) => b.v.uses - a.v.uses);
      if (!list.length) continue;
      h += `<div class="cat">${esc(cat)}</div>` + list.map(({ k, v }) => `<button type="button" data-k="${esc(k)}">${esc(v.label)} <span class="mono" style="color:var(--dim)">${v.kinds.includes('ranged') ? '▬' : '◆'}</span><small>${esc(v.desc)}</small></button>`).join('');
    }
    $('addList').innerHTML = h || '<p class="note" style="padding:8px">Nothing matches.</p>';
    $('addList').querySelectorAll('button').forEach((b) => b.onclick = () => { addEvent(b.dataset.k); closeMenus(); });
  };
  $('addFilter').oninput = draw; draw(); $('addFilter').focus();
  setTimeout(() => document.addEventListener('pointerdown', outside), 0);
}
function outside(ev) { const m = $('addMenu'); if (m && !m.contains(ev.target)) closeMenus(); }
function closeMenus() { const m = $('addMenu'); if (m) m.remove(); document.removeEventListener('pointerdown', outside); }
function addEvent(key) {
  const info = OPS.ops[key], code = parseInt(info.op, 16);
  if (!curTLPath()) createTimeline('');
  const p = curTLPath(), t = F(p);
  const kind = info.kinds.includes('ranged') ? 'ranged' : 'keyed';
  // copy the shape: one of this kind in this animation, else in the moveset, else the retail shape
  let tpl = null;
  for (const src of [p, ...S.tlPaths]) { tpl = F(src)[kind].find((e) => opCode(e.op) === code); if (tpl) break; }
  const t0 = ticks(S.frame);
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
  edit([p], () => { t[kind].push(e); });
  S.sel = { kind, idx: t[kind].length - 1 };
  renderTimeline(); renderInspector(); pose();
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
  for (const m of S.moves) m.steps.forEach((s, i) => {
    if (s.motion < 0) { out.push({ level: 'err', text: `${m.name} press ${i + 1}: action ${s.action} has no animation.` }); return; }
    const nx = m.steps[i + 1];
    if (!nx || !(nx.flags & 0x100)) return;
    const t = S.tlOfMotion.has(s.motion) && F(S.tlOfMotion.get(s.motion));
    const chain = t && t.ranged.some((e) => [0x0101, 0x0467, 0x0278, 0x0579].includes(opCode(e.op)));
    if (!chain) out.push({ level: 'warn', motion: s.motion, text: `${m.name} press ${i + 1} (${motionName(s.motion)}) has no chain window, so press ${i + 2} can't follow.` });
  });
  if (typeof ckMore === 'function') try { out.push(...ckMore()); } catch (e) { console.warn('studio: checks', e); }
  S.checks = out;
  const nP = out.filter((c) => c.level !== 'info').length;
  const b = $('checksBtn'); b.hidden = !S.moves || !out.length;          // shown when something is wrong, or worth knowing
  if (!out.length && S.showChecks) { S.showChecks = false; renderInspector(); }
  b.textContent = nP ? `${nP} problem${nP > 1 ? 's' : ''}` : `${out.length} note${out.length > 1 ? 's' : ''}`;
  b.title = 'Click to list them';
  b.className = nP ? 'badge warn' : 'badge';
}
function renderChecksPanel(I) {
  I.innerHTML = `<div class="sec"><h3>Checks <button type="button" id="ckClose">Close</button></h3>
    ${S.checks.length ? `<div class="checks">${S.checks.map((c, k) => `<div class="${c.level}" data-k="${k}">${esc(c.text)}</div>`).join('')}</div>` : '<p class="note">Nothing to report.</p>'}</div>`;
  $('ckClose').onclick = () => { S.showChecks = false; renderInspector(); };
  I.querySelectorAll('[data-k]').forEach((d) => d.onclick = () => {
    const c = S.checks[+d.dataset.k]; S.showChecks = false;
    if (c.motion != null) { S.step = null; setMotion(c.motion); S.sel = c.kind ? { kind: c.kind, idx: c.idx } : null; }
    renderAll();
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
async function saveRetailAsMod() {
  const { code } = S.rig.retail, c = S.retailChars.find((x) => x.code === code);
  if (!(await ensureModsRoot())) return;
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
      _read_me: [`Changes the retail ${c.label} (${code}): charmod.py build installs the files below that differ from the retail ones over the retail paths, so the game uses them instead.`,
        'Made by the Move Studio from the game ISO. Edit it there (it is listed as "' + c.label + ' (Modded)") or in Blender. Disable it ("enabled": false) or delete the folder and the next build puts the retail files back.',
        'models/ holds the 1P rigs (skeleton, animations): Blender saves animation changes into them; the other costumes keep the retail animations.'],
      kind: 'retail',
      character: { name: c.label, code },
      move_names: { _note: 'The retail ability names (notes: renaming retail moves is not supported yet).', ...(S.abilities[code] || {}) },
      files: modFiles,
    };
    await FS.write(`${slug}/mod.json`, JSON.stringify(mod, null, 1) + '\n');
  } catch (e) { toast('Save failed: ' + e.message, 'bad'); return; }
  const rigGmo = `${slug}/models/${S.rig.gmo.slice(4).replace(/_(2p|3p|zako)(_a)?\.gmo$/, '_1p$2.gmo')}`;
  for (const f of Object.values(S.files)) f.dirty = false;
  if (S.effe) S.effe.dirty = false;
  const keepMotion = S.motion, keepStep = S.step, keepFrame = S.frame;
  await scanMods(S.modsBase);
  fillCharPick();
  await pickChar(slug, rigGmo);
  $('charPick').value = slug;
  S.step = keepStep; setMotion(keepMotion); setFrame(keepFrame); renderAll();
  toast(`Saved as mods/${slug}. Start the game (run.sh builds it in) and it uses this ${c.label} instead of the retail one.`);
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
function renderAll() { renderLeft(); renderTimeline(); renderInspector(); }
$('openBtn').onclick = openFolder; $('openBtn2').onclick = openFolder; $('modsReopen').onclick = reopenMods;
$('dirInput').onchange = (e) => openFromInput(e.target.files);
$('charPick').onchange = () => onCharPick($('charPick').value);
$('rigPick').onchange = async () => { if (dirtyCount() && !(await uiDiscard())) { $('rigPick').value = String(S.rigs.indexOf(S.rig)); return; } await loadRig(S.rigs[+$('rigPick').value]); };
$('reloadBtn').onclick = async () => { if (S.readOnly) return toast('Retail characters come from the ISO: nothing to reload.'); if (dirtyCount() && !(await uiConfirm('Reload and discard changes?', `${dirtyCount()} file${dirtyCount() > 1 ? 's have' : ' has'} unsaved changes. Reload reads the folder again and drops them.`, { ok: 'Discard and reload', danger: true }))) return; const keep = S.motion; await loadRig(S.rig); setMotion(keep); renderAll(); toast('Read ' + S.char + ' again'); };
$('saveBtn').onclick = save; $('undoBtn').onclick = undo;
$('checksBtn').onclick = () => { S.showChecks = !S.showChecks; renderInspector(); };
$('tabMoves').onclick = () => { S.tab = 'moves'; renderLeft(); };
$('tabAnims').onclick = () => { S.tab = 'anims'; renderLeft(); };
$('tabFx').onclick = () => { S.tab = 'fx'; renderLeft(); };
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
$('zoom').oninput = () => renderTimeline();
$('showFx').onchange = () => { fxRefresh(); };
$('showBones').onchange = () => { if (view.bonesHelper) view.bonesHelper.visible = $('showBones').checked; };
$('partsGame').onchange = () => updateParts();
function isTyping() { const a = document.activeElement; return !!a && (/SELECT|TEXTAREA/.test(a.tagName) || a.isContentEditable || (a.tagName === 'INPUT' && !/^(checkbox|radio|button|range|color)$/.test(a.type))); }
document.addEventListener('keydown', (e) => {
  const typing = isTyping();
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); save(); return; }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z' && !typing) { e.preventDefault(); undo(); return; }
  if (typing || !S.moves) return;
  if (e.key === ' ') { e.preventDefault(); $('playBtn').click(); }
  else if (e.key === 'ArrowRight') setFrame(Math.floor(S.frame) + 1);
  else if (e.key === 'ArrowLeft') setFrame(Math.ceil(S.frame) - 1);
  else if ((e.key === 'Delete' || e.key === 'Backspace') && S.sel) { e.preventDefault(); deleteSel(); }
  else if (e.key === 'Escape') { closeMenus(); S.sel = null; renderTimeline(); renderInspector(); }
});
window.addEventListener('beforeunload', (e) => { if (dirtyCount()) { e.preventDefault(); e.returnValue = ''; } });
window.studio = { S, F, wjText, pyFloat, save, addEvent, selectEvent, setMotion, setFrame, renderAll, FS };
