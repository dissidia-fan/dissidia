/* Move Studio: a mod character's skins (its body and weapons from mods/<name>/skins/<skin>/<rig>.skin) on the model in
   the viewport, and the one the game build uses (mod.json body.skin). A port of tools/charmod_gmo.py apply_skin /
   apply_weapons over a port of charmod_chunks.py (the lossless GMO chunk tree). Making skins stays in Blender
   (Character mods > Make skin). Loaded after app.js. */

/* ---------------------------------------------------------------- chunk tree (charmod_chunks.py) */
const GC = { MODEL: 0x0003, BONE: 0x0004, PART: 0x0005, MESH: 0x0006, ARRAYS: 0x0007, MATERIAL: 0x0008, TEXTURE: 0x000A, FCURVE: 0x000C,
  SET_MATERIAL: 0x8061, SET_TEXTURE: 0x8091, BLEND_BONES: 0x8044, BLEND_OFFSETS: 0x8045, DRAW_PART: 0x804E, FILE_IMAGE: 0x8013 };
const GC_LEAF = new Set([GC.ARRAYS, GC.FCURVE]);
function gcParse(d, start, end) {
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength), out = [];
  let o = start;
  while (o + 8 <= end) {
    const t = dv.getUint16(o, true), hs = dv.getUint16(o + 2, true), sz = dv.getUint32(o + 4, true);
    if (sz < 8 || o + sz > end) throw new Error(`bad GMO chunk 0x${t.toString(16)} at 0x${o.toString(16)}`);
    if (t & 0x8000) out.push({ type: t, payload: d.subarray(o + 8, o + sz) });
    else {
      const c = { type: t, header: d.subarray(o + 8, o + hs), children: [], payload: null };
      if (GC_LEAF.has(t)) c.payload = d.subarray(o + hs, o + sz); else c.children = gcParse(d, o + hs, o + sz);
      out.push(c);
    }
    o += sz;
  }
  if (o !== end) throw new Error('trailing bytes in a GMO chunk');
  return out;
}
function gcSize(c) {
  if (c.type & 0x8000) return 8 + c.payload.length;
  return 8 + c.header.length + (GC_LEAF.has(c.type) ? c.payload.length : c.children.reduce((a, x) => a + gcSize(x), 0));
}
function gcWrite(c, out, o) {
  const dv = new DataView(out.buffer, out.byteOffset, out.byteLength), sz = gcSize(c);
  dv.setUint16(o, c.type, true);
  if (c.type & 0x8000) { dv.setUint16(o + 2, 0, true); dv.setUint32(o + 4, sz, true); out.set(c.payload, o + 8); return o + sz; }
  const hs = 8 + c.header.length;
  dv.setUint16(o + 2, hs, true); dv.setUint32(o + 4, sz, true); out.set(c.header, o + 8);
  let p = o + hs;
  if (GC_LEAF.has(c.type)) { out.set(c.payload, p); p += c.payload.length; } else for (const x of c.children) p = gcWrite(x, out, p);
  return p;
}
function* gcWalk(c) { yield c; for (const x of c.children || []) yield* gcWalk(x); }
const gcFind = (c, t) => (c.children || []).filter((x) => x.type === t);
const gcFirst = (c, t) => (c.children || []).find((x) => x.type === t) || null;
const gcU32 = (c) => new DataView(c.payload.buffer, c.payload.byteOffset, 4).getUint32(0, true);
const gcName = (c) => { if (c.header.length <= 8) return ''; let s = ''; for (let i = 8; i < c.header.length && c.header[i]; i++) s += String.fromCharCode(c.header[i]); return s; };

class SkinModel {
  constructor(bytes) {
    this.magic = bytes.subarray(0, 16);
    const roots = gcParse(bytes, 16, bytes.length);
    if (roots.length !== 1) throw new Error('expected one File block');
    this.root = roots[0];
    const ms = [...gcWalk(this.root)].filter((c) => c.type === GC.MODEL);
    this.mm = ms.find((m) => m.children.some((c) => c.type === GC.PART)) || ms[ms.length - 1];   // the drawn one (some retail models carry a second)
    this.bones = gcFind(this.mm, GC.BONE); this.parts = gcFind(this.mm, GC.PART);
    this.mats = gcFind(this.mm, GC.MATERIAL); this.texs = gcFind(this.mm, GC.TEXTURE);
  }
  toBytes() { const out = new Uint8Array(16 + gcSize(this.root)); out.set(this.magic, 0); gcWrite(this.root, out, 16); return out; }
  owner(pi) {
    const i = this.bones.findIndex((b) => { const dp = gcFirst(b, GC.DRAW_PART); return dp && (gcU32(dp) & 0xFFF) === pi; });
    if (i < 0) throw new Error('no bone draws part ' + pi);
    return i;
  }
  partMaterials(pi) {
    const out = new Set();
    for (const m of gcFind(this.parts[pi], GC.MESH)) for (const c of gcFind(m, GC.SET_MATERIAL)) out.add(gcU32(c) & 0xFFF);
    return [...out].sort((a, b) => a - b);
  }
  matTextures(mi) { return [...gcWalk(this.mats[mi])].filter((c) => c.type === GC.SET_TEXTURE).map((c) => gcU32(c) & 0xFFF); }
  ghostParts() {
    return this.parts.map((p, i) => [gcName(p), i]).filter(([n]) => /^warp_ghost\d+$/.test(n)).sort((a, b) => +a[0].slice(10) - +b[0].slice(10)).map(([, i]) => i);
  }
  partSlots(pi) {
    const mats = this.partMaterials(pi), tex = [];
    for (const m of mats) for (const t of this.matTextures(m)) if (!tex.includes(t)) tex.push(t);
    return { part: pi, owner: this.owner(pi), materials: mats, textures: tex };
  }
  slots() { return { body: this.partSlots(0), ghosts: this.ghostParts().map((i) => this.partSlots(i)) }; }
  replacePart(pi, bytes) { const c = gcParse(bytes, 0, bytes.length)[0]; this.mm.children[this.mm.children.indexOf(this.parts[pi])] = c; this.parts[pi] = c; }
  setBlend(bi, bb, bo) { const b = this.bones[bi]; gcFirst(b, GC.BLEND_BONES).payload = bb; gcFirst(b, GC.BLEND_OFFSETS).payload = bo; }
  setImage(t, img) { gcFirst(this.texs[t], GC.FILE_IMAGE).payload = img; }
  /* charmod_gmo.own_body_slots: a body (and ghosts) sharing a material or texture with other parts (assists, retail
     atlases) gets appended copies of its own, same pixels; apply_skin does this first, so the skins fit. -> copies */
  ownBodySlots() {
    const mine = new Set([0, ...this.ghostParts()]), matUsers = new Map(), texUsers = new Map(), add = (m, k, v) => { if (!m.has(k)) m.set(k, new Set()); m.get(k).add(v); };
    this.parts.forEach((_, pi) => { for (const mi of this.partMaterials(pi)) { add(matUsers, mi, pi); for (const t of this.matTextures(mi)) add(texUsers, t, pi); } });
    const foreign = (set) => [...(set || [])].some((p) => !mine.has(p));
    if (![...mine].some((p) => this.partMaterials(p).some((mi) => this.matTextures(mi).some((t) => foreign(texUsers.get(t)))))) return 0;
    const clone = (c) => ({ type: c.type, header: c.header ? c.header.slice() : c.header, payload: c.payload ? c.payload.slice() : c.payload, children: (c.children || []).map(clone) });
    const named = (c, name) => {                              // charmod_chunks Chunk.name setter
      const raw = [...name].map((ch) => ch.charCodeAt(0) & 255); raw.push(0); while (raw.length % 4) raw.push(0);
      const hs = 16 + raw.length, h = new Uint8Array(8 + raw.length), dv = new DataView(h.buffer);
      dv.setUint32(0, hs, true); dv.setUint32(4, hs, true); h.set(raw, 8); c.header = h; return c;
    };
    const setRef = (c, i) => { const p = c.payload.slice(), dv = new DataView(p.buffer); dv.setUint32(0, ((dv.getUint32(0, true) & ~0xFFF) | i) >>> 0, true); c.payload = p; };
    const newMats = [], newTexs = [], matCopy = new Map(), texCopy = new Map();
    const matFor = (mi) => {
      if (!foreign(matUsers.get(mi))) return mi;
      if (!matCopy.has(mi)) { matCopy.set(mi, this.mats.length + newMats.length); newMats.push(named(clone(this.mats[mi]), (gcName(this.mats[mi]) || 'material').slice(0, 22) + '_body')); }
      return matCopy.get(mi);
    };
    const texFor = (t) => {
      if (!foreign(texUsers.get(t))) return t;
      if (!texCopy.has(t)) { texCopy.set(t, this.texs.length + newTexs.length); newTexs.push(named(clone(this.texs[t]), (gcName(this.texs[t]) || 'file').slice(0, 22) + '_body')); }
      return texCopy.get(t);
    };
    const bodyMats = new Set();
    for (const p of [...mine].sort((a, b) => a - b)) for (const mesh of gcFind(this.parts[p], GC.MESH)) for (const c of gcFind(mesh, GC.SET_MATERIAL)) {
      const ni = matFor(gcU32(c) & 0xFFF); bodyMats.add(ni); setRef(c, ni);
    }
    for (const mi of [...bodyMats].sort((a, b) => a - b)) {
      const mc = mi < this.mats.length ? this.mats[mi] : newMats[mi - this.mats.length];
      for (const c of gcWalk(mc)) if (c.type === GC.SET_TEXTURE) setRef(c, texFor(gcU32(c) & 0xFFF));
    }
    const afterLast = (t, list) => { if (!list.length) return; const k = this.mm.children; let i = -1; k.forEach((c, j) => { if (c.type === t) i = j; }); k.splice(i + 1, 0, ...list); };
    afterLast(GC.MATERIAL, newMats); afterLast(GC.TEXTURE, newTexs);
    this.mats = gcFind(this.mm, GC.MATERIAL); this.texs = gcFind(this.mm, GC.TEXTURE);
    return newMats.length + newTexs.length;
  }
}

/* ---------------------------------------------------------------- skin files */
function skinFromBytes(d) {
  if (String.fromCharCode(...d.subarray(0, 4)) !== 'DSKN') throw new Error('not a skin file');
  const n = new DataView(d.buffer, d.byteOffset, 8).getUint32(4, true);
  const head = JSON.parse(new TextDecoder().decode(d.subarray(8, 8 + n))), base = 8 + n, blobs = {};
  for (const [k, [o, s]] of Object.entries(head.blobs)) blobs[k] = d.subarray(base + o, base + o + s);
  return { info: head.info || '', slots: head.slots, blobs };
}
const sameSlots = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function applySkin(gmoBytes, skin) {
  const m = new SkinModel(gmoBytes); m.ownBodySlots();
  const s = m.slots(), ss = skin.slots, B = skin.blobs;
  if (ss.ghosts.length !== s.ghosts.length) throw new Error(`the skin has ${ss.ghosts.length} ghosts, the model ${s.ghosts.length}`);
  const groups = [[s.body, ss.body, 'body.'], ...s.ghosts.map((g, k) => [g, ss.ghosts[k], `ghost${k}.`])];
  for (const [g, sg, pre] of groups) {
    if (!sameSlots({ part: g.part, owner: g.owner, materials: g.materials, textures: g.textures }, { part: sg.part, owner: sg.owner, materials: sg.materials, textures: sg.textures })) throw new Error('the skin was made for other slots');
    m.replacePart(g.part, B[pre + 'part']);
    m.setBlend(g.owner, B[pre + 'blend_bones'], B[pre + 'blend_offsets']);
    for (const t of g.textures) m.setImage(t, B[pre + 'tex' + t]);
  }
  const ws = ss.weapons;
  if (ws) {
    const P = new Map(m.parts.map((p, i) => [gcName(p), i]));
    for (const w of ws.parts) {
      if (P.get(w.name) !== w.part || m.owner(w.part) !== w.owner) throw new Error('the skin\'s weapons were made for other slots: ' + w.name);
      const pre = `wep.${w.name}.`;
      m.replacePart(w.part, B[pre + 'part']);
      m.setBlend(w.owner, B[pre + 'blend_bones'], B[pre + 'blend_offsets']);
    }
    for (const t of ws.textures) m.setImage(t, B['wep.tex' + t]);
  }
  return m.toBytes();
}

/* ---------------------------------------------------------------- the studio side */
const SKIN_HIDDEN = new Set(['installed']);        // what the last build installed (charmod.py keeps it), not a choice
async function skinsOfRig() {                      // [{name, path}] that have a file for this rig
  if (!S.rig || S.rig.retail || !S.charDir && S.charDir !== '') return [];
  const stem = S.rig.gmo.split('/').pop().replace(/\.gmo$/, ''), out = [];
  for (const it of await FS.list(S.charDir + 'skins').catch(() => [])) {
    if (it.kind !== 'directory' || SKIN_HIDDEN.has(it.name)) continue;
    const p = `${S.charDir}skins/${it.name}/${stem}.skin`;
    if (await FS.exists(p)) out.push({ name: it.name, path: p });
  }
  return out;
}
const skinGameChoice = () => (S.mod && S.mod.body && S.mod.body.skin) || 'manikin';
async function skinSetup() {                       // after loadRig: the skins this rig has; show the one the game uses
  S.skins = []; S.skinShown = null; S.rigBytes = null;
  const tag = $('skinTag'); if (tag) tag.hidden = true;
  if (!S.gmo || !S.rig || S.rig.retail) return;
  try { S.skins = await skinsOfRig(); } catch (e) { S.skins = []; }
  if (!S.skins.length) return;
  S.rigBytes = S.files[S.rig.gmo] && S.files[S.rig.gmo].bin ? S.files[S.rig.gmo].bin : new Uint8Array(await FS.read(S.rig.gmo));
  const want = skinGameChoice();
  if (S.skins.some((s) => s.name === want)) await skinShow(want, true);
  skinHud();
}
async function skinShow(name, quiet) {
  let bytes = S.rigBytes;
  if (name) {
    const s = S.skins.find((x) => x.name === name);
    try { bytes = applySkin(S.rigBytes, skinFromBytes(new Uint8Array(await FS.read(s.path)))); }
    catch (e) { if (!quiet) toast(`Could not put on the ${name} skin: ${e.message}`, 'bad'); console.warn('studio: skin', name, e); return; }
  }
  try { S.gmo = parseGMO(bytes); } catch (e) { toast('Could not read the skinned model: ' + e.message, 'bad'); return; }
  S.skinShown = name;
  buildCharacter(S.gmo, view); view.bonesHelper.visible = $('showBones').checked;
  if (typeof FXP !== 'undefined') { FXP.gmo = null; FXP.map.clear(); }
  pose(); skinHud();
}
function skinHud() {                               // the skins live in the Character tab: refresh it when it shows them
  if (S.tab !== 'char') return;
  if (typeof renderLeft === 'function') renderLeft();
  if (S.ncSel === 'skin' && typeof renderInspector === 'function') renderInspector();
}
async function skinUseInGame(name) {               // mod.json body.skin: takes effect at the next build or game start
  const v = name || 'manikin', have = [];
  if (name && name !== 'as_is' && name !== 'manikin') {           // every model the skin has a file for (assists too)
    for (const it of await FS.list(`${S.charDir}skins/${name}`).catch(() => [])) {
      const gp = it.name.endsWith('.skin') ? 'obj/' + it.name.replace(/\.skin$/, '.gmo') : null;
      if (gp && S.mod.files && S.mod.files[gp]) have.push(gp);
    }
  }
  edit([S.modPath], () => { S.mod.body = S.mod.body || {}; S.mod.body.skin = v; if (have.length) S.mod.body.models = [...new Set([...(S.mod.body.models || []), ...have])]; });
  skinHud(); refreshStatus();
  toast(`The game will use ${v} after you save and build (or start the game: run.sh builds it).`);
}
/* the rig as it is stored: "as_is" for characters made in the Studio, "manikin" (the based-on character's) for others */
const skinStored = () => (S.mod && S.mod.body && S.mod.body.skin === 'as_is' ? 'as_is' : 'manikin');
const skinLabel = (name) => (!name || name === 'as_is' || name === 'manikin' ? (skinStored() === 'as_is' ? 'as stored' : 'manikin') : name);
function skinPanel(I) {                            // Character tab > Body: preview a skin on the model, pick the game's
  const game = skinLabel(skinGameChoice()), shown = skinLabel(S.skinShown);
  const rows = [{ name: null, label: skinLabel(null), note: skinStored() === 'as_is' ? 'the models in models/' : 'the rig as stored' }].concat((S.skins || []).map((s) => ({ name: s.name, label: s.name, note: s.path.split('/').slice(-2, -1)[0] === s.name ? '' : s.path })));
  I.innerHTML = `<div class="sec"><h3>Body</h3>
    <p class="note">Click one to see it on the model. The game uses the one marked <b>in game</b>.</p>
    <div class="skin-list">${rows.map((r) => { const id = r.label, on = id === shown, ing = id === game;
      return `<button type="button" class="skin-row${on ? ' sel' : ''}" data-skin="${esc(r.name || '')}" aria-pressed="${on}"><span class="mono">${esc(r.label)}</span>${r.note ? `<small>${esc(r.note)}</small>` : ''}${ing ? '<em class="skin-game">in game</em>' : ''}</button>`; }).join('')}</div>
    ${shown !== game && S.modPath && !S.readOnly ? `<button type="button" id="skinUseBtn" class="primary">Use ${esc(shown)} in game</button>` : ''}</div>${skinOwnHTML()}${skinWepHTML()}`;
  I.querySelectorAll('[data-skin]').forEach((b) => b.onclick = () => skinShow(b.dataset.skin || null));
  const u = I.querySelector('#skinUseBtn'); if (u) u.onclick = () => skinUseInGame(S.skinShown || (skinStored() === 'as_is' ? 'as_is' : null));
  skinOwnWire();
  I.querySelectorAll('[data-own-wep]').forEach((b) => b.onclick = () => skinOwnStart(b.dataset.ownWep));
}

/* ---------------------------------------------------------------- your own model as the body (Blender does the fitting)
   The Dissidia add-on (0.25+, own_model.py) listens on 127.0.0.1:47612 while Blender runs: /own_model opens the
   character there and asks for the model file; it is fitted onto the skeleton and saved as skins/<name>/; /job tells
   how it is going. When it is done the Studio points mod.json at the new skin (body.skin, body.models). */
const OWN = { job: null, state: '', message: '', timer: null, name: '', slot: '' };
function skinOwnHTML() {
  if (!S.modPath || S.readOnly || !S.mod || S.mod.kind === 'retail' || !S.rig || S.rig.retail) return '';
  const busy = OWN.job && (OWN.state === 'picking' || OWN.state === 'working');
  if (OWN.slot) return `<div class="sec"><h3>Your own model</h3><div class="btnrow"><button type="button" id="ownBtn" class="primary"${busy ? ' disabled' : ''}>Replace with my model…</button></div></div>`;
  return `<div class="sec"><h3>Your own model</h3>
    ${S.mod.body && S.mod.body.model_hint ? `<p class="note"><b>Body:</b> ${esc(S.mod.body.model_hint)}</p>` : ''}
    <p class="note">Blender asks for an FBX, glTF, VRM, OBJ or .blend model and fits it onto the skeleton.</p>
    <div class="btnrow"><button type="button" id="ownBtn" class="primary"${busy ? ' disabled' : ''}>Replace with my model…</button></div>
    ${OWN.job || OWN.message ? `<p class="note own-status ${OWN.state === 'error' ? 'bad' : ''}" id="ownStatus">${esc(skinOwnText())}</p>` : ''}
    <p class="note">Needs Blender open with the Dissidia add-on.</p></div>`;
}
/* a weapon of your own: the same, for one wepN part (Blender lines it up with the old one, gives it one picture and
   puts it into the skin the game uses, or own-weapons) */
const skinWeapons = () => (S.gmo && S.gmo.parts ? S.gmo.parts.map((p) => p.name).filter((n) => /^wep\d+$/.test(n)) : []);
function skinWepHTML() {
  if (!S.modPath || S.readOnly || !S.mod || S.mod.kind === 'retail' || !S.rig || S.rig.retail) return '';
  const have = new Set(skinWeapons()), gs = ((S.mod.body && S.mod.body.weapon_groups) || []).filter((g) => (g.parts || []).some((p) => have.has(p)));
  const ws = gs.length ? gs : [...have].map((n) => ({ name: n, parts: [n] })); if (!ws.length) return '';
  const busy = OWN.job && (OWN.state === 'picking' || OWN.state === 'working');
  return `<div class="sec"><h3>Weapons</h3>
    <p class="note">Blender asks for an FBX, glTF or OBJ model and puts it where the old weapon is, hilt in the hand. Weapons drawn with the same picture change together.</p>
    <div class="skin-list">${ws.map((g) => `<div class="skin-row wep-row"><span class="wep-name"><b>${esc(g.name)}</b>${g.parts.length > 1 || g.name !== g.parts[0] || g.hint ? `<small>${esc([g.hint, g.parts.join(', ')].filter(Boolean).join(' · '))}</small>` : ''}</span><span class="grow"></span><button type="button" class="mini" data-own-wep="${esc(g.parts[0])}"${busy ? ' disabled' : ''}>Use my model…</button></div>`).join('')}</div>
    ${OWN.slot && (OWN.job || OWN.message) ? `<p class="note own-status ${OWN.state === 'error' ? 'bad' : ''}" id="ownStatus">${esc(skinOwnText())}</p>` : ''}
    <p class="note">Needs Blender open with the Dissidia add-on (0.29 or later).</p></div>`;
}
function skinOwnText() {
  return { picking: 'Blender is waiting: pick the model file there.', working: 'Blender: ' + OWN.message, done: 'Done: ' + OWN.message,
    error: 'Blender: ' + OWN.message, cancelled: 'No model picked.' }[OWN.state] || OWN.message;
}
function skinOwnWire() {
  const b = $('ownBtn'); if (!b) return;
  b.onclick = skinOwnStart;
}
function skinOwnRefresh() {
  const st = $('ownStatus');
  if (st && S.ncSel === 'skin') { st.textContent = skinOwnText(); st.classList.toggle('bad', OWN.state === 'error'); }
  else if (S.tab === 'char' && S.ncSel === 'skin') renderInspector();
}
async function skinOwnStart(slot) {
  slot = typeof slot === 'string' ? slot : '';
  const rf = S.files[S.rig.gmo];
  if (rf && rf.gmo && fileChanged(rf)) { toast('Save first: Blender reads the rig from the folder.'); return; }
  const rig1p = (S.rigs.find((r) => /_1p\.gmo$/.test(r.gmo)) || S.rig).gmo, rig = rig1p.slice(S.charDir.length);
  const q = new URLSearchParams(slot ? { mod: studioModName(), rig, slot } : { mod: studioModName(), rig, skin: '' });   // Blender names the skin
  Object.assign(OWN, { job: null, state: 'picking', message: 'Asking Blender…', slot }); renderInspector();
  let j;
  try { j = await (await fetch(`http://127.0.0.1:47612/${slot ? 'own_weapon' : 'own_model'}?` + q, { cache: 'no-store' })).json(); }
  catch (e) { Object.assign(OWN, { state: 'error', message: `not answering. Open Blender with the Dissidia add-on (${slot ? '0.29' : '0.25'} or later), then try again.` }); renderInspector(); return; }
  if (slot && j.ok === false && /unknown request/.test(j.message || '')) j.message = 'this Blender add-on is older than 0.29: update it to replace weapons';
  if (!j.ok || !j.job) { Object.assign(OWN, { state: 'error', message: j.message || 'could not start' }); renderInspector(); return; }
  Object.assign(OWN, { job: j.job, state: 'picking', message: '' }); renderInspector();
  const t0 = Date.now(), char = S.charDir;
  clearInterval(OWN.timer);
  OWN.timer = setInterval(async () => {
    if (S.charDir !== char || Date.now() - t0 > 3600e3) { clearInterval(OWN.timer); return; }
    let s; try { s = await (await fetch('http://127.0.0.1:47612/job?id=' + OWN.job, { cache: 'no-store' })).json(); } catch (e) { return; }   // Blender busy or restarting: ask again
    const was = OWN.state; Object.assign(OWN, { state: s.state, message: s.message || '' });
    if (s.state === 'done') { clearInterval(OWN.timer); await skinOwnDone(s); return; }
    if (s.state === 'error' || s.state === 'cancelled' || s.state === 'unknown') { clearInterval(OWN.timer); renderInspector(); return; }
    if (was !== s.state) renderInspector(); else skinOwnRefresh();
  }, 1000);
}
async function skinOwnDone(s) {
  const code = (S.mod.character || {}).code || '', costume = new RegExp(`^obj/p_${code}_(2p|3p|zako)(_a|_w)?\\.gmo$`);
  edit([S.modPath], () => {
    const b = S.mod.body = S.mod.body || {}, F = S.mod.files || {};
    b.skin = s.skin;
    b.models = [...new Set([...(b.models || []), ...(s.models || [])])];
    // one look now: every costume plays the first one's model (its assist the first assist); the save removes the
    // costume models left over from the based-on character
    for (const gp of Object.keys(F)) {
      const m = costume.exec(gp), one = m && `obj/p_${code}_1p${m[2] || ''}.gmo`;
      if (m && F[one] !== undefined) { F[gp] = JSON.parse(JSON.stringify(F[one])); if (b.models.includes(one) && !b.models.includes(gp)) b.models.push(gp); }
    }
  });
  OWN.name = '';
  try { S.skins = await skinsOfRig(); } catch (e) { /* keep */ }
  // Blender may have given the rig's body its own copy of a shared texture: read the rig again
  const f = S.files[S.rig.gmo], fresh = new Uint8Array(await FS.read(S.rig.gmo));
  S.rigBytes = fresh; if (f && f.bin) { f.bin = fresh; f.binOrig = fresh; }
  if (S.skins.some((x) => x.name === s.skin)) await skinShow(s.skin, false);
  skinHud(); renderInspector(); refreshStatus();
  toast(OWN.slot ? `${(s.slots || [OWN.slot]).join(', ')}: your model, in the ${s.skin} skin. Save, then build.` : `${s.skin} is the body now. Save, then build.`);
}
