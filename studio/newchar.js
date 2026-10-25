/* Move Studio: a new character folder from a retail base (what `charmod.py new` makes), and the Character tab: who
   it is (name, series, code, slot) and its menu / HUD pictures (avatars). Loaded after clip.js.
   The build (charmod.py build, or run.sh at game start) adds a new folder to the game: a roster slot, the menu ids,
   and every menu picture cut from avatars.generated_from (the picture and its crop boxes). */

const NC_AVATARS = [                              // charmod_avatars.FILE_AVATARS / PACK_AVATARS as the shipped folders list them
  ['grid_tile', '32x64', 'Quick Battle character grid tile', null, ['tile']],
  ['slidein', '256x256', 'Player Select slide-in art and the battle setup panel portrait', null, ['art', 'artl']],
  ['name_plate', '128x32', 'Name plate on Player Select', null, ['name']],
  ['logo', '128x64', 'Series logo plate on Player Select', null, ['logo']],
  ['hud', '128x128', 'In-battle portrait at the bottom of the screen (the same picture is used for P1 and the CPU, never mirrored)', ['menu/jp/battle/chara_image/p_{c}_1p.gim', 'menu/jp/battle/chara_image/p_{c}_2p.gim', 'menu/jp/battle/chara_image/p_{c}_3p.gim']],
  ['vs_face', '64x64', 'Square face (VS screen / story mode)', ['menu/jp/story_mode/chara2/p_{c}_1p.gim']],
  ['face_strip', '128x32', 'Face strip in name panels', ['menu/jp/common/chara_face_in_name/p_{c}_face_in_name.gim']],
  ['face_strip_mirrored', '128x32', 'Face strip, mirrored copy (the other side of the screen)', ['menu/jp/common/chara_face_in_name_r/p_{c}_face_in_name_r.gim']],
  ['name_text', '128x32', 'Name text in menus', ['menu/en/common/chara_name/p_{c}_name.gim']],
  ['encounter_text', '256x64', 'Name banner when a battle starts', ['menu/en/battle/encount/encount_text_tim2/encount_{c}.gim']],
  ['select_portrait', null, 'Character-select portrait (arcade / story)', ['menu/jp/character_select/chara2/p_{c}_1p.gim']],
  ['arcade_last', null, 'Arcade mode final-opponent picture', ['menu/jp/arcademode/arcade_last_chara_tim2/arcade_last_{c}.gim']],
  ['talk_top', null, 'Talk-event portrait (top)', ['menu/jp/talkevent/tim2/chara/t_talk_{c}.gim']],
  ['talk_bottom', null, 'Talk-event portrait (bottom)', ['menu/jp/talkevent/tim2/chara/b_talk_{c}.gim']],
];
const NC_CROPS = { slidein: [1, 1], grid_tile: [1, 2], hud: [1, 1], vs_face: [1, 1], face_strip: [4, 1] };   // the crop boxes the build reads (w : h)
const NC_INFO = 'Pictures the menus and HUD show. "picture": null = the based-on character\'s picture. The build cuts them from generated_from (the picture and its crop boxes) the first time and whenever it changes. A PNG of your own in avatars/ with "picture" set replaces one.';

/* ---------------------------------------------------------------- new character */
async function ncCharIds() {                       // {code: charId} from main.bin's chrp (as retailAbilities reads it)
  if (S.charIds) return S.charIds;
  const sys = arcRead(mpkNamed(await isoGet('general_archive/main/main.bin'))['system.bin']);
  const chrp = sys.find((e) => e.tag === 'chrp').data, dv = DV(chrp), out = {};
  for (let i = 0, n = dv.getUint32(4, true); i < n; i++) {
    const r = 8 + 0x90 * i; let c = '';
    for (let k = 0; k < 8 && chrp[r + 0x74 + k]; k++) c += String.fromCharCode(chrp[r + 0x74 + k]);
    out[c.toLowerCase().replace(/^p_/, '')] = chrp[r];
  }
  return (S.charIds = out);
}
async function ncUsed() {                          // codes and slots the mods folder and the disc already use
  const codes = new Set(ROSTER.map(([c]) => c)), slots = new Set();
  for (const c of S.chars || []) {
    try { const m = await FS.json(c.name + '/mod.json'); if (m.character) { codes.add(m.character.code); slots.add(m.character.slot); } } catch (e) { /* skip */ }
  }
  return { codes, slots };
}
/* roster slots: 1..32 retail, 33 SEV120's unused retail slot, 34 a spare retail slot, 35..42 extra (DISSIDIA_ROSTER_MAX 43) */
const NC_SLOTS = [33, 34, 35, 36, 37, 38, 39, 40, 41, 42];
const NC_SLOT_NOTE = { 33: 'a retail slot the game leaves off the grid', 34: 'a spare retail slot' };
async function ncSlotsLoad() {                     // {slot: [character names]} of the other extra characters in the mods folder
  const out = {}, me = ((S.mod || {}).character || {}).code;
  for (const ch of S.chars || []) {
    if (ch.kind === 'retail') continue;
    try { const c = ((await FS.json(ch.name + '/mod.json')).character) || {}; if (c.code && c.code !== me && c.slot != null) (out[c.slot] = out[c.slot] || []).push(c.name || ch.name); } catch (e) { /* skip */ }
  }
  return (S.ncTaken = out);
}
function ncSlotOptions(cur) {
  const T = S.ncTaken || {}, opt = (n) => `<option value="${n}"${n === cur ? ' selected' : ''} data-note="${esc([NC_SLOT_NOTE[n] ? NC_SLOT_NOTE[n].replace(/^./, (c) => c.toUpperCase()) : 'Extra slot', T[n] ? 'also ' + T[n].join(', ') : ''].filter(Boolean).join(' · '))}">Slot ${n}</option>`;
  const free = NC_SLOTS.filter((n) => !T[n] || n === cur);
  if (cur != null && !free.includes(cur)) free.unshift(cur);
  return free.map(opt).join('');
}
function ncSlotClash(cur) {
  const w = $('ncSlotClash'), T = S.ncTaken || {}; if (!w) return;
  w.hidden = !T[cur];
  if (T[cur]) w.textContent = `${T[cur].join(', ')} ${T[cur].length > 1 ? 'are' : 'is'} on slot ${cur} too: pick a free one, or the build moves one of you.`;
}
function ncCodeFor(name, used) {
  const base = (name.toLowerCase().replace(/[^a-z]/g, '') + 'xxx').slice(0, 3);
  for (let n = 100; n < 1000; n++) { const c = base + n; if (!used.codes.has(c) && !(ISO.index && isoHas(`obj/p_${c}.objx`))) return c; }
  return null;
}
function ncExtras() {                              // extra characters (own roster slots) in the mods folder
  return (S.chars || []).filter((c) => c.kind !== 'retail').length;
}
function ncStart() {
  if (!S.retailChars.length) { toast('Set the game ISO first: a new character starts from a retail one (its skeleton, animations and moves).'); return; }
  const back = document.createElement('div'); back.className = 'modal-back';
  const opts = S.retailChars.map((c) => `<option value="${c.code}"${c.code === 'fiv100' ? ' selected' : ''}>${esc(c.label)} (${c.code})</option>`).join('');
  back.innerHTML = `<div class="modal" role="dialog" aria-label="New character"><h3>New character</h3>
    <div class="field"><label>Name (menus)</label><input type="text" id="ncName" maxlength="28" placeholder="Terra"></div>
    <div class="row2"><div class="field"><label>Full name (VS screen, banner)</label><input type="text" id="ncFull" placeholder="Terra Branford"></div>
      <div class="field"><label>Series (logo plate)</label><input type="text" id="ncSeries" value="FINAL FANTASY"></div></div>
    <div class="row2"><div class="field"><label>Built on</label><select id="ncLike">${opts}</select></div>
      <div class="field"><label>Code</label><input type="text" id="ncCode" maxlength="6" placeholder="(from the name)"></div></div>
    <div class="field"><label>Picture (full body art, optional)</label><input type="file" id="ncPic" accept="image/png,image/jpeg,image/webp"></div>
    ${ncExtras() >= 6 ? `<p class="note rt-note">You already have ${ncExtras()} extra characters, the most the character select screens hold. The game leaves a new one out (it says so when it starts) until you delete or disable another (<span class="mono">"enabled": false</span> in its mod.json).</p>` : ''}
    <p class="note">Makes mods/&lt;name&gt;/ with the built-on character's moves to edit (moveset/), its four battle models (models/), and the menu pictures cut from your picture at the first build (a grey silhouette until you set one). The skeleton and animations are the built-on character's: give it a body with a skin in Blender.</p>
    <div class="btnrow"><button type="button" id="ncMake" class="primary">Make the character</button><button type="button" id="ncCancel">Cancel</button></div></div>`;
  document.body.appendChild(back);
  const close = () => back.remove();
  $('ncCancel').onclick = close;
  back.addEventListener('pointerdown', (e) => { if (e.target === back) close(); });
  $('ncName').focus();
  $('ncMake').onclick = async () => {
    const o = { name: $('ncName').value.trim(), full: $('ncFull').value.trim(), series: $('ncSeries').value.trim() || 'FINAL FANTASY', like: $('ncLike').value, code: $('ncCode').value.trim().toLowerCase(), pic: $('ncPic').files[0] || null };
    if (!o.name) { toast('Give the character a name.'); return; }
    $('ncMake').disabled = true;
    const ok = await ncMake(o);
    if (ok) close(); else $('ncMake').disabled = false;
  };
}
async function ncPlaceholder() {                    // charmod_avatars.placeholder_art: a plain silhouette (900 x 1400)
  const c = document.createElement('canvas'); c.width = 900; c.height = 1400;
  const g = c.getContext('2d'); g.fillStyle = 'rgb(70,78,104)';
  const rr = (x0, y0, x1, y1, r) => { g.beginPath(); g.roundRect(x0, y0, x1 - x0, y1 - y0, r); g.fill(); };
  g.beginPath(); g.ellipse(450, 200, 100, 110, 0, 0, 2 * Math.PI); g.fill();
  rr(300, 330, 600, 820, 60); rr(230, 350, 300, 780, 30); rr(600, 350, 670, 780, 30); rr(320, 800, 440, 1340, 40); rr(460, 800, 580, 1340, 40);
  g.fillStyle = 'rgb(230,230,240)'; g.font = '40px sans-serif'; g.fillText('NEW', 405, 560);
  return new Uint8Array(await (await new Promise((r) => c.toBlob(r, 'image/png'))).arrayBuffer());
}
async function ncImageSize(bytes, type) {
  const bm = await createImageBitmap(new Blob([bytes], { type: type || 'image/png' }));
  const s = { w: bm.width, h: bm.height }; bm.close && bm.close(); return s;
}
function ncDefaultCrops(w, h) {                    // starting boxes on a full-body picture: the art whole, the face near the top
  const fw = w * 0.22, fx = w / 2 - fw / 2, fy = h * 0.06;
  const box = (x0, y0, x1, y1) => [x0, y0, x1, y1].map(Math.round).join(',');
  const side = Math.max(w, h);
  return {
    slidein: box(w / 2 - side / 2, h / 2 - side / 2, w / 2 + side / 2, h / 2 + side / 2),
    face_strip: box(fx - fw * 0.3, fy + fw * 0.35, fx + fw * 1.3, fy + fw * 0.75),
    hud: box(fx - fw * 0.2, fy - fw * 0.1, fx + fw * 1.2, fy + fw * 1.3),
    grid_tile: box(fx - fw * 0.1, fy - fw * 0.1, fx + fw * 1.1, fy + fw * 2.3),
    vs_face: box(fx, fy, fx + fw, fy + fw),
  };
}
async function ncMake(o) {
  if (!(await ensureModsRoot())) return false;
  const used = await ncUsed();
  if (o.code && !/^[a-z][a-z0-9]{0,5}$/.test(o.code)) { toast('Code: 1–6 lower-case letters / digits, starting with a letter (e.g. ter100).'); return false; }
  const code = o.code || ncCodeFor(o.name, used);
  if (!code || used.codes.has(code) || isoHas(`obj/p_${code}.objx`)) { toast(`Code ${code} is already used: pick another.`); return false; }
  let slug = slugOf(o.name), k = 2; while (await FS.exists(slug)) slug = slugOf(o.name) + '-' + k++;
  const slot = [35, 36, 37, 38, 39, 40, 41, 42, 34, 33].find((n) => !used.slots.has(n)) ?? 42;   // the extra slots first, then the spare retail ones
  const like = o.like;
  let likeId = null; try { likeId = (await ncCharIds())[like] ?? null; } catch (e) { /* the build fills it in */ }
  const c = S.retailChars.find((x) => x.code === like);
  try {
    // the moves, unpacked (charmod_objx.unpack's layout), and the battle models
    const dec = await retailDecoded(`obj/p_${like}.objx`, `obj/p_${like}_1p.gmo`);
    for (const [p, v] of Object.entries(dec)) await FS.write(`${slug}/moveset/p_${code}/${p}`, v instanceof Uint8Array ? v : wjText(v));
    const files = { _auto: true, _info: 'game path -> folder path (moveset/..., models/..., files/...) or {"like": retail path}. _auto: every file of the based-on character not listed here is used as it is.', [`obj/p_${code}.objx`]: `moveset/p_${code}` };
    // one model: the costume slots show it until you add a costume (Character tab > Costumes)
    if (isoHas(`obj/p_${like}_1p.gmo`)) {
      await FS.write(`${slug}/models/p_${code}_1p.gmo`, await isoGet(`obj/p_${like}_1p.gmo`));
      for (const [cos] of c.costumes.filter(([x]) => !x.endsWith('_a'))) files[`obj/p_${code}_${cos}.gmo`] = `models/p_${code}_1p.gmo`;
    }
    // the picture the menu art is cut from
    let picRel = 'avatars/source/placeholder.png', bytes, type = 'image/png';
    if (o.pic) { bytes = new Uint8Array(await o.pic.arrayBuffer()); type = o.pic.type; picRel = 'avatars/source/' + slug + '-art.' + (o.pic.name.split('.').pop() || 'png').toLowerCase(); }
    else bytes = await ncPlaceholder();
    await FS.write(`${slug}/${picRel}`, bytes);
    const sz = await ncImageSize(bytes, type);
    const avatars = { _info: NC_INFO };
    for (const [key, size, shows, pats, ids] of NC_AVATARS) {
      const e = { picture: null, size, shows };
      if (!size) delete e.size;
      if (pats) { e._files = pats.map((p) => p.replace('{c}', code)).filter((p) => isoHas(p.replace(code, like))); if (!e._files.length) continue; }
      if (ids) e._menu_ids = ids;
      avatars[key] = e;
    }
    avatars.generated_from = { _info: 'what the avatars are made from: picture (full body), optional portrait (head and shoulders), crop boxes x0,y0,x1,y1 in picture pixels per avatar. Change them in the Move Studio (Character tab): the next build cuts the pictures again',
      picture: picRel, portrait: o.pic ? picRel : null, logo: null, crops: o.pic ? ncDefaultCrops(sz.w, sz.h) : {}, options: { tile_cutout: true, hud_flip: true, hud_from: 'slidein', art_faces: 'left' } };
    const mod = {
      _read_me: ['One extra character, made in the Move Studio. Edit it there or in Blender, then start the game: run.sh builds every character folder in.',
        'character: who it is (slot = its roster slot; the build moves it to a free one if taken). based_on = the retail character it is built on (skeleton, animations, moves).',
        'avatars: the menu / HUD pictures, cut from avatars.generated_from at the first build. body: "as_is" = the models in models/ as they are; give it a body with a skin (Blender: Character mods > Make skin).',
        'move_names: in-game name and help text per ability. Moves: moveset/p_' + code + '/. Full guide: mods/README.md'],
      character: { name: o.name, full_name: o.full || o.name, series: o.series, code, slot, based_on: like, based_on_id: likeId },
      avatars,
      body: { _info: '"as_is" = the models in models/ as they are. Another body: a skin made in Blender (Character mods > Make skin), then "skin": its name.', skin: 'as_is', models: [] },
      move_names: { _info: 'Rename moves: key = the ability\'s number (hex), cmd = its command id in moveset/p_' + code + '/moves.json. Keys starting with _ are ignored.', ...Object.fromEntries(Object.entries(S.abilities[like] || {}).map(([kk, v]) => [kk, { cmd: v.cmd, was: v.name }])) },
      runtime_hooks: [],
      menu_ids: { _info: 'entry ids in the shared menu pack; filled in by the build when the character is added; leave as they are' },
      files,
    };
    if (mod.character.based_on_id == null) delete mod.character.based_on_id;
    await FS.write(`${slug}/mod.json`, JSON.stringify(mod, null, 1) + '\n');
  } catch (e) { console.warn('studio: new character', e); toast('Could not make the character: ' + e.message, 'bad'); return false; }
  await scanMods(S.modsBase); fillCharPick();
  $('charPick').value = slug;
  await pickChar(slug);
  S.tab = 'char'; renderAll();
  toast(`Made mods/${slug}: ${o.name} (code ${code}), built on ${c.label}. Set the crops of its menu pictures here, then start the game: run.sh builds it in and adds it to the roster.`);
  return true;
}

/* ---------------------------------------------------------------- the Character tab */
const ncMod = () => S.mod || {};
const NC_CUT = ['slidein', 'grid_tile', 'hud', 'vs_face', 'face_strip', 'face_strip_mirrored'];   // cut from the art (the mirrored strip: face_strip's box, flipped)
const NC_DRAWN = ['name_plate', 'logo', 'name_text', 'encounter_text'];                            // drawn from the name and series
const NC_NAMES = { slidein: 'Slide-in art', hud: 'HUD portrait', vs_face: 'VS face', grid_tile: 'Grid tile', face_strip: 'Face strip', face_strip_mirrored: 'Face strip, mirrored', encounter_text: 'Battle banner' };
const ncTitle = (k) => NC_NAMES[k] || k.replace(/_/g, ' ').replace(/^./, (x) => x.toUpperCase());
const ncCropKey = (k) => (k === 'face_strip_mirrored' ? 'face_strip' : k);
const ncBoxOf = (cr, k) => { const b = String((cr || {})[k] || '').split(',').map(Number); return b.length === 4 && b.every(isFinite) ? b : null; };
const ncShows = (e) => String((e && e.shows) || '').replace(/(\s*\([^()]*\))+$/, '');   // the short form: no trailing (...) notes   // the build's note on a borrowed picture: the card says that
const NC_ROMAN = { I: 1, II: 2, III: 3, IV: 4, V: 5, VI: 6, VII: 7, VIII: 8, IX: 9, X: 10, XI: 11, XII: 12, XIII: 13 };
function ncRetailLogo(series) {                    // new_character.retail_logo: the game's own plate for FINAL FANTASY I..XIII (f2, FF I = 0), else null
  const m = String(series || '').trim().match(/^(.*\S)\s+([IVX]{1,5}|\d{1,2})$/);
  if (!m || m[1].toUpperCase() !== 'FINAL FANTASY') return null;
  const n = /^\d+$/.test(m[2]) ? +m[2] : NC_ROMAN[m[2].toUpperCase()];
  return n >= 1 && n <= 13 ? n - 1 : null;
}
async function ncRetailLogoGim(f2) {               // its picture: menu_pk_free_battle_gim_2.bin, entity 0x706F + f2 (character_art.BASE_LOGO)
  const pak = await isoGet('menu/menu_pk_free_battle_gim_2.bin'), dv = new DataView(pak.buffer, pak.byteOffset, pak.byteLength);
  const n = dv.getUint16(10, true), i = 0x706F + f2 - dv.getUint32(12, true);
  if (i < 0 || i >= n) return null;
  const off = dv.getUint32(0x10 + 8 * i, true), size = dv.getUint32(0x14 + 8 * i, true);
  return size ? decodeGIM(pak.subarray(off, off + size)) : null;
}
const ncAvRows = () => Object.entries(ncMod().avatars || {}).filter(([k, e]) => !k.startsWith('_') && e && e.shows);
function ncLeft(L) {
  const m = ncMod(), c = m.character || {}, av = m.avatars || {};
  if (!S.modPath || m.kind === 'retail') { L.innerHTML = `<p class="note" style="padding:10px">${S.rig && S.rig.retail ? 'A retail character: its pictures are the game\'s.' : m.kind === 'retail' ? 'A changed retail character: it keeps the retail pictures and roster place.' : 'No mod.json.'}</p>`; return; }
  if (S.ncSel === 'crops' || (S.ncSel && av[S.ncSel])) S.ncSel = 'art';          // the menu art is one screen now
  const rows = ncAvRows(), own = rows.filter(([, e]) => e._own && !e._cut).length;
  const like = (S.retailChars || []).find((x) => x.code === c.based_on), base = (p) => String(p || '').split('/').pop();
  const dash = (m.runtime_hooks || []).find((t) => typeof t === 'string' && t.startsWith('dash='));
  const item = (key, title, sub) => `<button type="button" class="nc-item${S.ncSel === key ? ' sel' : ''}" data-nc="${esc(key)}"><span>${esc(title)}</span><small>${esc(sub)}</small></button>`;
  const gameBody = skinLabel(skinGameChoice()), shown = skinLabel(S.skinShown);
  const art = base((av.generated_from || {}).picture);
  L.innerHTML = `<div class="nc-who"><b>${esc(c.name || '')}</b><small>${esc(c.full_name || '')}${c.series ? ' · ' + esc(c.series) : ''}</small>
      <div class="nc-chips"><span class="mono">${esc(c.code || '')}</span><span>slot ${esc(c.slot ?? '?')}</span><span>built on ${esc(like ? like.label : c.based_on || '?')}</span></div></div>
    <div class="nc-group">Character</div>
    ${item('who', 'Name and roster', `${c.name || ''} · slot ${c.slot ?? '?'}`)}
    ${item('physics', 'Jump, gravity, dash', dash ? 'dash ×' + dash.slice(5) : 'as ' + (like ? like.label : 'the built-on character'))}
    <div class="nc-group">Look</div>
    ${item('skin', 'Body', gameBody + (shown !== gameBody ? ' · showing ' + shown : ''))}
    ${typeof costumePanel === 'function' ? item('costumes', 'Costumes', cstSummary()) : ''}
    ${item('art', 'Menu art', `${rows.length} pictures · ${art ? 'from ' + art : 'no art yet'}${own ? ' · ' + own + ' your own' : ''}`)}`;
  L.querySelectorAll('[data-nc]').forEach((r) => r.onclick = () => { S.ncSel = r.dataset.nc; S.step = null; S.sel = null; renderLeft(); renderInspector(); });
}
function ncInspector(I) {
  const m = ncMod(), c = m.character || {}, sel = S.ncSel || 'who';
  if (sel === 'who') {
    const av = m.avatars || {};
    if (S.ncArtKey && av[S.ncArtKey] && NC_DRAWN.includes(S.ncArtKey)) return ncArtInspector(I);   // a picture card picked in the middle
    I.innerHTML = `<div class="sec" id="ncSec"><h3>${esc(c.name || 'Character')}</h3>
      <div class="row2"><div class="field fact"><label>Code</label><span class="mono">${esc(c.code || '?')}</span></div><div class="field fact"><label>Roster slot</label><span>${esc(c.slot ?? '?')}</span></div></div>
      <div class="field fact"><label>Built on</label><span>${esc(((S.retailChars || []).find((x) => x.code === c.based_on) || {}).label || '')} <span class="mono">${esc(c.based_on || '?')}</span>${c.based_on_id ? ` <small>charId ${esc(c.based_on_id)}</small>` : ''}</span></div>
      <p class="note">The name, full name and series go into the menus when the build adds the character; the code names its files, so it stays. The skeleton, animations and moves start as the built-on character's.</p>
      ${typeof ckCharRecomp === 'function' ? ckCharRecomp().map((t) => `<p class="note rt-note"><b>Needs the recomp:</b> ${esc(t)}</p>`).join('') : ''}</div>`;
    return;
  }
  if (sel === 'skin' && typeof skinPanel === 'function') return skinPanel(I);
  if (sel === 'costumes' && typeof costumePanel === 'function') return costumePanel(I);
  if (sel === 'art') return ncArtInspector(I);
  if (sel === 'physics') return ncPhysics(I);
  S.ncSel = 'who'; return ncInspector(I);
}
async function ncFit(file, size) {                  // an image as a PNG of the avatar's size (contain, centred, transparent)
  const bm = await createImageBitmap(file);
  const [w, h] = size ? size.split('x').map(Number) : [bm.width, bm.height];
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  const s = Math.min(w / bm.width, h / bm.height), dw = bm.width * s, dh = bm.height * s;
  c.getContext('2d').drawImage(bm, (w - dw) / 2, (h - dh) / 2, dw, dh);
  return new Uint8Array(await (await new Promise((r) => c.toBlob(r, 'image/png'))).arrayBuffer());
}

/* ---------------------------------------------------------------- menu art: one screen in place of the 3D view and timeline
   The art (generated_from.picture) with a box per cut picture (drag to move, drag the corner to resize; its shape stays),
   and every menu / HUD picture as a card: what it is now (cut, placed by the build, drawn, yours, the built-on one's), and
   your own picture for any of them (written to avatars/own/, so the cut one stays to go back to). */
/* the other pictures (select portrait, arcade, talk portraits) aren't cut by the build: a box for one lives in
   generated_from.studio_crops, and the Studio cuts it itself, at the retail picture's size, into avatars/own/<key>.png */
const NC_SIZE = {};                                // key -> [w, h] of the retail picture (from the ISO)
const ncAllCrops = () => { const g = (ncMod().avatars || {}).generated_from || {}; return { ...(g.studio_crops || {}), ...(g.crops || {}) }; };
const ncStudioKey = (k) => !NC_CROPS[k] && !NC_CUT.includes(k) && !NC_DRAWN.includes(k);
const ncAspect = (k) => NC_CROPS[k] || NC_SIZE[k] || null;
function ncSetBox(k, v) {                         // inside an edit()
  const g = S.mod.avatars.generated_from, key = NC_CROPS[k] ? 'crops' : 'studio_crops';
  g[key] = g[key] || {};
  if (v == null) { delete g[key][k]; if (key === 'studio_crops' && !Object.keys(g[key]).length) delete g[key]; } else g[key][k] = v;
}
async function ncRetailSize(k) {                   // the built-on character's picture size, from the ISO
  if (NC_SIZE[k]) return NC_SIZE[k];
  const c = ncMod().character || {}, f = ((ncMod().avatars || {})[k] || {})._files, p = f && f[0] && f[0].replace(c.code, c.based_on);
  if (!p || !ISO.index || !isoHas(p)) return null;
  const im = decodeGIM(await isoGet(p)); return (NC_SIZE[k] = [im.w, im.h]);
}
async function ncStudioCut(k) {                    // the box's part of the art as the picture's PNG
  const img = await ncArtImg(), b = ncBoxOf(ncAllCrops(), k), sz = await ncRetailSize(k);
  if (!img || !b || !sz) return;
  const c = document.createElement('canvas'); c.width = sz[0]; c.height = sz[1];
  c.getContext('2d').drawImage(img, b[0], b[1], b[2] - b[0], b[3] - b[1], 0, 0, sz[0], sz[1]);
  const rel = `avatars/own/${k}.png`;
  await FS.write(S.charDir + rel, new Uint8Array(await (await new Promise((r) => c.toBlob(r, 'image/png'))).arrayBuffer()));
  const x = S.mod.avatars[k]; x.picture = rel; x._own = true; x._cut = true; delete x._keep; markDirty([S.modPath]);
}
function ncArtState(k) {
  const av = ncMod().avatars || {}, e = av[k] || {}, cr = (av.generated_from || {}).crops;
  if (ncStudioKey(k) && ncBoxOf(ncAllCrops(), k)) return { kind: 'cut', label: 'Cut from the art', long: 'Cut from the art by its box, at the size of the game\'s picture. The build uses it as your picture.' };
  if (e._own) return { kind: 'own', label: 'Your picture', long: `Your own picture (${e.picture}). The build leaves it as it is.` };
  if (NC_CUT.includes(k)) {
    if (ncBoxOf(cr, ncCropKey(k))) return { kind: 'cut', label: 'Cut from the art', long: k === 'face_strip_mirrored' ? 'The face strip\'s box, mirrored.' : 'Cut from the art by its box.' };
    return { kind: 'auto', label: 'Placed by the build', long: k === 'slidein' ? 'No box: the build fits the whole art. Set a box to choose the part it shows.' : 'No box: the build finds the head itself. Set a box to choose it.' };
  }
  const f2 = k === 'logo' && !(av.generated_from || {}).logo ? ncRetailLogo((ncMod().character || {}).series) : null;
  if (f2 != null) return { kind: 'game', label: 'The game\'s own', long: `${(ncMod().character || {}).series} has a logo plate in the game, so the build uses that one (no menu entry of its own). Use mine… to replace it.` };
  if (NC_DRAWN.includes(k)) return { kind: 'drawn', label: k === 'logo' ? 'Drawn from the series' : 'Drawn from the name', long: k === 'logo' ? 'The build draws the series logo plate from the series name.' : 'The build writes the name in the game\'s menu font.' };
  return { kind: 'base', label: 'The built-on one\'s', long: 'The built-on character\'s picture, until you cut one from the art (Set a box) or give it one of your own.' };
}
function ncArtInspector(I) {
  const av = ncMod().avatars || {}, k = av[S.ncArtKey] ? S.ncArtKey : null;
  if (!k) {
    I.innerHTML = `<div class="sec" id="ncSec"><h3>Menu art</h3>
      <p class="note">Every menu and HUD picture is in the middle. Drag the boxes on the art to choose what each one shows, or give any picture one of your own (Use mine…, or drop an image on it).</p>
      <p class="note">Click a picture to see where it goes in the game and to show only its box.</p>
      <p class="note">The next build (run.sh at game start) cuts the pictures again whenever the art, a box or an option changes. Your own pictures stay as they are.</p></div>`;
    return;
  }
  const e = av[k], st = ncArtState(k);
  I.innerHTML = `<div class="sec" id="ncSec"><h3>${esc(ncTitle(k))} <small>${esc((e.size || '').replace('x', '×'))}</small></h3>
    <p class="note">${esc(ncShows(e))}</p>
    <div class="lbl">Now</div><p class="note"><span class="art-st st-${st.kind}">${esc(st.label)}</span> ${esc(st.long)}</p>
    <div class="lbl">Where it goes</div><p class="note mono" style="word-break:break-all">${e._files ? e._files.map(esc).join('<br>') : 'the shared menu pack (' + esc((e._menu_ids || []).join(', ') || '?') + ')'}</p></div>`;
}
/* what the middle shows on the Character tab: Name and roster and Menu art are pages of their own (no 3D view or
   timeline); Jump, gravity, dash previews them in the 3D view; Body and Costumes show the model in its rest pose */
function ncArtSync() {
  const C = document.querySelector('.center'); if (!C) return;
  const mine = S.tab === 'char' && !!S.moves && !!S.modPath && ncMod().kind !== 'retail', sel = S.ncSel || 'who';
  const pane = mine && sel === 'art' && ncMod().avatars ? 'art' : mine && sel === 'who' ? 'who' : null;
  const was = C.classList.contains('art-mode');
  C.classList.toggle('art-mode', !!pane);
  const W = document.querySelector('.work'); if (W) W.classList.toggle('art-wide', pane === 'art');   // Menu art: no inspector, its details are on the cards
  if (pane !== 'art' && S.ncRO) { S.ncRO.disconnect(); S.ncRO = null; }
  if (pane && S.playing) { S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶'; if (typeof psndStop === 'function') psndStop(false); }   // no 3D view: paused (and quiet)
  if (pane === 'art') ncArtPane();
  else if (pane === 'who') ncWhoPane();
  else if (was) window.dispatchEvent(new Event('resize'));
  phSync(mine && sel === 'physics');
  ncModelSync(mine && (sel === 'skin' || sel === 'costumes'));
  // the Character tab has nothing for the dummy: it and its card go while it's open, and come back with the move after
  const charTab = S.tab === 'char' && !!S.moves;
  if (charTab) { if (typeof dmShow === 'function') dmShow(false); const di = $('dmInfo'); if (di) di.hidden = true; }
  else if (S.ncWasChar && S.gmo) pose();
  S.ncWasChar = charTab;
}
function ncModelSync(on) {                         // Body / Costumes: the 3D view alone, the model in its rest pose
  const C = document.querySelector('.center'); if (!C || !!S.modelPose === on) return;
  S.modelPose = on; C.classList.toggle('model-mode', on);
  if (on) {
    S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶';
    try { fxUpdate(view, S.fx, null, 0, { on: false }); } catch (e) { /* nothing shown */ }
    if (typeof dmShow === 'function') dmShow(false);
  }
  pose(); window.dispatchEvent(new Event('resize'));
}
function ncWhoPane() {                             // Name and roster, as a page
  const P = $('artPane'); if (!P) return;
  const m = ncMod(), c = m.character || {}, av = m.avatars || {}, ro = S.readOnly ? ' disabled' : '';
  const shows = NC_DRAWN.filter((k) => av[k] && av[k].shows);
  P.innerHTML = `<div class="art-bar"><b>Name and roster</b><span class="meta">${esc(c.name || '')}${c.code ? ' · <span class="mono">' + esc(c.code) + '</span>' : ''}</span></div>
    <div class="art-body who-body">
      <div class="who-form">
        <div class="field"><label for="ncWName">Name</label><input type="text" id="ncWName" data-c="name" maxlength="28" value="${esc(c.name || '')}"${ro}><small class="ui-hint">Menus, the character select grid and the name plate.</small></div>
        <div class="field"><label for="ncWFull">Full name</label><input type="text" id="ncWFull" data-c="full_name" value="${esc(c.full_name || '')}"${ro}><small class="ui-hint">The VS screen and the banner when a battle starts.</small></div>
        <div class="field"><label for="ncWSeries">Series</label><input type="text" id="ncWSeries" data-c="series" value="${esc(c.series || '')}"${ro}><small class="ui-hint">The logo plate. FINAL FANTASY I to XIII use the game's own logo.</small></div>
        <div class="field"><label for="ncSlot">Roster slot</label><select id="ncSlot" data-c="slot"${ro}>${ncSlotOptions(c.slot)}</select><small class="ui-hint">The free slots (33–42). Two characters on one slot are split up at the build: the later one moves to the next free slot.</small></div>
        <p class="warnline" id="ncSlotClash" hidden></p>
      </div>
      <div class="art-list" id="artList">${shows.length ? `<div class="art-group"><h4>Where they show</h4><small>As the last build drew them</small></div>${shows.map(ncArtCard).join('')}` : ''}</div></div>`;
  if (typeof fancySelect === 'function') fancySelect($('ncSlot'), { noteInButton: true, className: 'fsel-wide' });
  ncSlotsLoad().then(() => { const sel = $('ncSlot'); if (sel && (S.ncSel || 'who') === 'who') { sel.innerHTML = ncSlotOptions(ncMod().character.slot); ncSlotClash(ncMod().character.slot); } });
  P.querySelectorAll('[data-c]').forEach((el) => el.onchange = () => {
    edit([S.modPath], () => { const k = el.dataset.c; S.mod.character[k] = k === 'slot' ? parseInt(el.value, 10) : el.value; });
    renderLeft(); renderInspector();
  });
  ncArtWire(P); ncArtThumbs(P);
}
async function ncArtImg() {
  const gf = (ncMod().avatars || {}).generated_from || {}; if (!gf.picture) return null;
  const key = S.charDir + gf.picture;
  if (S.ncImg && S.ncImg.key === key) return S.ncImg.img;
  const img = await createImageBitmap(new Blob([await FS.read(key)]));
  S.ncImg = { key, img }; return img;
}
function ncArtPane() {
  const P = $('artPane'); if (!P) return;
  const av = ncMod().avatars || {}, gf = av.generated_from || {}, o = gf.options || {};
  const noArt = !gf.picture || /(^|\/)placeholder\.png$/.test(gf.picture);   // only the new character's grey silhouette: ask for the art
  const keys = ncAvRows().map(([k]) => k);
  const cut = keys.filter((k) => NC_CUT.includes(k)), drawn = keys.filter((k) => NC_DRAWN.includes(k)), other = keys.filter((k) => !NC_CUT.includes(k) && !NC_DRAWN.includes(k));
  const group = (title, note, ks) => ks.length ? `<div class="art-group"><h4>${title}</h4><small>${note}</small></div>${ks.map(ncArtCard).join('')}` : '';
  const ro = S.readOnly ? ' disabled' : '';
  P.innerHTML = `<div class="art-bar"><b>Menu art</b><span class="meta mono" title="${esc(gf.picture || '')}">${noArt ? 'no art yet' : esc(gf.picture.split('/').pop())}</span><span class="art-tools">
      ${[['tile_cutout', 'Tile cut-out', 'Grid tile: cut the figure out of its background'], ['hud_flip', 'Mirror HUD', 'HUD portrait: mirror the face']].map(([k, l, t]) => `<label class="meta" title="${t}"><input type="checkbox" data-opt="${k}"${o[k] ? ' checked' : ''}${ro}> ${l}</label>`).join('')}
      <label class="btn mini${ro ? ' off' : ''}" title="A full-body picture to cut the menu art from"><input type="file" id="ncSrc" accept="image/png,image/jpeg,image/webp" hidden${ro}>Custom art…</label>
      ${noArt ? '' : `<button type="button" class="mini" id="ncReset"${ro ? ' disabled' : ''} title="Every box back to its starting place">Reset boxes</button>`}</span></div>
    <div class="art-body">
      <div class="art-stage">${noArt ? `<label class="art-drop art-drop-big" id="ncArtDrop"><input type="file" id="ncSrc2" accept="image/png,image/jpeg,image/webp" hidden${ro}>
          <b>Choose your character's art</b><span>or drop it here</span><small>A full-body picture (PNG, JPEG or WebP): every menu picture is cut from it</small></label>` : `<div class="art-stagebar"><div class="art-focus" id="ncFocusNote" hidden></div><span class="grow"></span>
          <label class="meta" title="Zoom into the art (Ctrl / ⌘ + scroll wheel too)">Zoom <input type="range" id="ncZoom" min="1" max="6" step="0.25" value="${S.ncZoom || 1}"></label><span class="meta mono" id="ncZoomV">${Math.round((S.ncZoom || 1) * 100)}%</span></div>
        <div class="nc-crop" id="ncCrop"><canvas id="ncCanvas"></canvas></div>`}</div>
      <div class="art-list" id="artList">${ncArtGroupsHTML()}</div></div>`;
  ncArtWire(P);
  ncArtThumbs(P);
  if (noArt) {                                    // the big drop zone: the same as Custom art…
    const z = $('ncArtDrop'), f2 = $('ncSrc2'), src = $('ncSrc');
    const take = (file) => { if (!file || !src) return; const dt = new DataTransfer(); dt.items.add(file); src.files = dt.files; src.dispatchEvent(new Event('change')); };
    if (f2) f2.onchange = () => take(f2.files[0]);
    z.ondragover = (ev) => { ev.preventDefault(); z.classList.add('drop'); };
    z.ondragleave = () => z.classList.remove('drop');
    z.ondrop = (ev) => { ev.preventDefault(); z.classList.remove('drop'); take(ev.dataTransfer.files[0]); };
    return;
  }
  ncArtImg().then((img) => {
    if (!img || !$('ncCanvas')) return;
    ncCropEditor(img);
    if (S.ncRO) S.ncRO.disconnect();
    S.ncRO = new ResizeObserver(() => ncCropEditor(img)); S.ncRO.observe($('ncCrop'));
    const setZoom = (z, at) => {                      // at: {x, y} in the stage to keep still
      const host = $('ncCrop'), old = S.ncZoom || 1; z = Math.min(6, Math.max(1, z)); if (z === old) return;
      const ax = at ? at.x : host.clientWidth / 2, ay = at ? at.y : host.clientHeight / 2, fx = (host.scrollLeft + ax) / old, fy = (host.scrollTop + ay) / old;
      S.ncZoom = z; $('ncZoom').value = z; $('ncZoomV').textContent = Math.round(z * 100) + '%';
      S.ncScrollFor = S.ncFocus || null; ncCropEditor(img);
      host.scrollLeft = fx * z - ax; host.scrollTop = fy * z - ay;
    };
    $('ncZoom').oninput = () => setZoom(parseFloat($('ncZoom').value));
    $('ncCrop').onwheel = (ev) => { if (!ev.ctrlKey && !ev.metaKey) return; ev.preventDefault(); const r = $('ncCrop').getBoundingClientRect(); setZoom((S.ncZoom || 1) * (ev.deltaY < 0 ? 1.15 : 1 / 1.15), { x: ev.clientX - r.left, y: ev.clientY - r.top }); };
    $('ncReset').onclick = () => { edit([S.modPath], () => { S.mod.avatars.generated_from.crops = ncDefaultCrops(img.width, img.height); }); ncArtPane(); renderLeft(); };
  }).catch((e) => { const h = $('ncCrop'); if (h) h.innerHTML = `<p class="warnline">${esc(gf.picture)}: ${esc(e.message)}</p>`; });
}
/* a picture's card: its preview and name; picked, two tabs choose where it comes from: the art (a crop box; for the name
   pictures the build's drawing, for the logo the game's plate) or a custom image (a file picker / drop zone) */
const NC_TAB = {};                                 // key -> the tab looked at ('src' or 'own'), until the picture changes
function ncArtCard(k) {
  const e = ncMod().avatars[k], on = S.ncArtKey === k;
  return `<div class="art-card${on ? ' on' : ''}" data-art="${k}" tabindex="0" role="button" aria-pressed="${on}" title="${esc(ncShows(e))}">
    <div class="art-pv" data-thumb="${k}"></div>
    <div class="art-meta"><b>${esc(ncTitle(k))}</b><small>${esc((e.size || (NC_SIZE[k] ? NC_SIZE[k].join('x') : 'any size')).replace('x', '×'))} · ${esc(ncShows(e))}</small></div>
    ${on ? ncArtDetail(k) : ''}</div>`;
}
function ncArtDetail(k) {                          // the picked card: the source tabs, then where it goes in the game
  const m = ncMod(), e = m.avatars[k], ro = S.readOnly ? ' disabled' : '';
  const custom = e._own && !e._cut, studio = ncStudioKey(k), box = !!ncBoxOf(ncAllCrops(), ncCropKey(k));
  const logo = k === 'logo' && ncRetailLogo((m.character || {}).series) != null;
  const srcName = NC_DRAWN.includes(k) ? (logo ? 'Game\'s logo' : k === 'logo' ? 'Drawn from the series' : 'Drawn from the name') : 'Crop from Art';
  const tab = NC_TAB[k] || (custom ? 'own' : 'src'), art = !!(m.avatars.generated_from || {}).picture;
  let src = '';
  if (NC_DRAWN.includes(k)) src = '';
  else if (!art) src = '<p>Pick the art first (Custom art… above).</p>';
  else if (!box && k !== 'face_strip_mirrored') src = `<div class="btnrow"><button type="button" class="mini" data-art-box="${k}"${ro}${studio && !ISO.index && !NC_SIZE[k] ? ' disabled title="Needs the game ISO: the box takes the shape of the game\'s picture"' : ''}>Set a box</button></div>`;
  if (custom && tab === 'src') src += `<div class="btnrow"><button type="button" class="mini primary" data-art-clear="${k}"${ro}>Use this instead of my image</button></div>`;
  const size = (e.size || (NC_SIZE[k] ? NC_SIZE[k].join('x') : '')).replace('x', '×');
  const own = `<label class="art-drop${ro ? ' off' : ''}" data-art-dropzone="${k}"><input type="file" data-art-file="${k}" accept="image/png,image/jpeg,image/webp" hidden${ro}>
      <b>${custom ? 'Choose another image' : 'Choose an image'}</b><span>or drop one here</span><small>PNG, JPEG or WebP${size ? ' · fitted to ' + esc(size) : ''}</small></label>`;
  return `<div class="art-detail">
    <div class="art-tabs" role="tablist" aria-label="Where ${esc(ncTitle(k))} comes from">
      <button type="button" role="tab" data-art-tab="src" aria-selected="${tab === 'src'}">${esc(srcName)}${!custom ? ' <i class="ic ic-circle-check" aria-label="in use"></i>' : ''}</button>
      <button type="button" role="tab" data-art-tab="own" aria-selected="${tab === 'own'}">Custom Image${custom ? ' <i class="ic ic-circle-check" aria-label="in use"></i>' : ''}</button></div>
    ${(tab === 'own' ? own : src) ? `<div class="art-tabpanel" role="tabpanel">${tab === 'own' ? own : src}</div>` : ''}</div>`;
}
function ncArtRefresh() {                          // the card list again (a pick, a tab, a new picture)
  if ((S.ncSel || 'who') === 'who') return ncWhoPane();
  const L = $('artList'), P = $('artPane'); if (!L || !P) return ncArtPane();
  const top = L.scrollTop; L.innerHTML = ncArtGroupsHTML(); L.scrollTop = top;
  ncArtWire(P); ncArtThumbs(P);
}
function ncArtGroupsHTML() {
  const keys = ncAvRows().map(([k]) => k);
  const cut = keys.filter((k) => NC_CUT.includes(k)), drawn = keys.filter((k) => NC_DRAWN.includes(k)), other = keys.filter((k) => !NC_CUT.includes(k) && !NC_DRAWN.includes(k));
  const group = (title, note, ks) => ks.length ? `<div class="art-group"><h4>${title}</h4><small>${note}</small></div>${ks.map(ncArtCard).join('')}` : '';
  return group('Cut from the art', 'Click one to show only its box', cut) + group('Drawn from the name', 'The build draws these', drawn) + group('Other pictures', 'The built-on character\'s until you set a box or an image', other);
}
function ncArtWire(P) {
  P.querySelectorAll('[data-opt]').forEach((el) => el.onchange = () => { edit([S.modPath], () => { const g = S.mod.avatars.generated_from; g.options = g.options || {}; g.options[el.dataset.opt] = el.checked; }); ncArtImg().then((img) => img && ncArtPreviews(img)); });
  const src = $('ncSrc');
  if (src) src.onchange = async (ev) => {
    const f = ev.target.files[0]; if (!f) return;
    const rel = 'avatars/source/' + slugOf(S.char) + '-art.' + (f.name.split('.').pop() || 'png').toLowerCase(), bytes = new Uint8Array(await f.arrayBuffer());
    await FS.write(S.charDir + rel, bytes);
    const sz = await ncImageSize(bytes, f.type);
    S.ncImg = null; S.ncFocus = null;
    edit([S.modPath], () => { const g = S.mod.avatars.generated_from = S.mod.avatars.generated_from || {}; g.picture = rel; g.portrait = rel; g.crops = ncDefaultCrops(sz.w, sz.h); g.options = g.options || { tile_cutout: true, hud_flip: true, hud_from: 'slidein', art_faces: 'left' }; });
    renderLeft(); renderInspector();
  };
  const pick = (k) => {
    const ck = ncCropKey(k), again = S.ncArtKey === k;
    S.ncArtKey = again ? null : k;
    S.ncFocus = !again && ncBoxOf(ncAllCrops(), ck) ? ck : null;
    if (S.ncFocus) S.ncBox = S.ncFocus;
    ncArtRefresh();
    ncArtImg().then((img) => img && ncCropEditor(img)).catch(() => {});
    renderInspector();
  };
  const own = async (k, f) => {
    if (S.readOnly) { readOnlyNote(); return; }
    if (!f || !/^image\//.test(f.type)) { toast('That isn\'t a picture: use a PNG, JPEG or WebP.', 'bad'); return; }
    const e = S.mod.avatars[k], png = await ncFit(f, e.size), rel = `avatars/own/${k}.png`;
    await FS.write(S.charDir + rel, png);
    edit([S.modPath], () => { const x = S.mod.avatars[k]; x.picture = rel; x._own = true; delete x._keep; delete x._cut; if (ncStudioKey(k)) ncSetBox(k, null); });   // _own: the build's cut pictures don't replace it
    delete NC_TAB[k];
    renderLeft(); renderInspector();
    toast(`${ncTitle(k)}: your picture. Save, and the next build puts it in the game.`, 'ok');
  };
  P.querySelectorAll('[data-art]').forEach((c) => {
    const k = c.dataset.art;
    c.onclick = (ev) => { if (!ev.target.closest('button,label,input')) pick(k); };
    c.onkeydown = (ev) => { if ((ev.key === 'Enter' || ev.key === ' ') && ev.target === c) { ev.preventDefault(); pick(k); } };
    c.ondragover = (ev) => { if (S.readOnly) return; ev.preventDefault(); c.classList.add('drop'); };
    c.ondragleave = () => c.classList.remove('drop');
    c.ondrop = (ev) => { ev.preventDefault(); c.classList.remove('drop'); own(k, ev.dataTransfer.files[0]); };
  });
  P.querySelectorAll('[data-art-file]').forEach((el) => el.onchange = (ev) => own(el.dataset.artFile, ev.target.files[0]));
  P.querySelectorAll('[data-art-tab]').forEach((t) => t.onclick = (ev) => { ev.stopPropagation(); const k = t.closest('[data-art]').dataset.art; NC_TAB[k] = t.dataset.artTab; ncArtRefresh(); });
  P.querySelectorAll('[data-art-dropzone]').forEach((z) => {
    z.ondragover = (ev) => { if (S.readOnly) return; ev.preventDefault(); ev.stopPropagation(); z.classList.add('drop'); };
    z.ondragleave = () => z.classList.remove('drop');
    z.ondrop = (ev) => { ev.preventDefault(); ev.stopPropagation(); z.classList.remove('drop'); own(z.dataset.artDropzone, ev.dataTransfer.files[0]); };
  });
  P.querySelectorAll('[data-art-box]').forEach((b) => b.onclick = async () => {
    const k = b.dataset.artBox, img = await ncArtImg(); if (!img) return;
    if (ncStudioKey(k)) {
      const sz = await ncRetailSize(k).catch(() => null); if (!sz) { toast('Set the game ISO first: the box takes the shape of the game\'s picture.', 'bad'); return; }
      let w = img.width * 0.6, h = w * sz[1] / sz[0]; if (h > img.height * 0.8) { h = img.height * 0.8; w = h * sz[0] / sz[1]; }
      const x0 = (img.width - w) / 2, y0 = Math.max(0, img.height * 0.05);
      edit([S.modPath], () => ncSetBox(k, [x0, y0, x0 + w, y0 + h].map(Math.round).join(',')));
      await ncStudioCut(k);
    } else edit([S.modPath], () => { const g = S.mod.avatars.generated_from; g.crops = g.crops || {}; g.crops[k] = ncDefaultCrops(img.width, img.height)[k]; });
    S.ncArtKey = k; S.ncFocus = k; S.ncBox = k;
    renderLeft(); renderInspector();
  });
  P.querySelectorAll('[data-art-clear]').forEach((b) => b.onclick = async () => {
    const k = b.dataset.artClear, made = `avatars/${k}.png`; delete NC_TAB[k];
    const keep = await FS.exists(S.charDir + made).catch(() => false);
    edit([S.modPath], () => {
      const x = S.mod.avatars[k]; delete x._own; delete x._cut;
      if (ncStudioKey(k)) ncSetBox(k, null);
      x.picture = keep && x.picture !== made ? made : null;          // the cut / drawn one is still there (own pictures go to avatars/own/)
      if (NC_CUT.includes(k) || x.picture === made) delete S.mod.avatars.generated_from._made;   // older folders kept their own picture at that path: cut again
    });
    renderLeft(); renderInspector();
  });
}
async function ncArtThumbs(P) {                     // each card's picture: the live cut, or the file it uses now, or the built-on one's from the ISO
  const av = ncMod().avatars || {}, cr = (av.generated_from || {}).crops, like = (ncMod().character || {}).based_on, code = (ncMod().character || {}).code;
  const showBlob = (el, bytes, name) => { const u = URL.createObjectURL(new Blob([bytes])); el.innerHTML = `<img src="${u}" alt="${esc(name)}">`; };
  for (const el of P.querySelectorAll('[data-thumb]')) {
    const k = el.dataset.thumb, e = av[k], [w, h] = (e.size || '64x64').split('x').map(Number);
    if (!e._own && NC_CUT.includes(k) && ncBoxOf(cr, ncCropKey(k))) { el.innerHTML = `<canvas data-pv="${k}" width="${w}" height="${h}"></canvas>`; continue; }
    if (ncStudioKey(k) && ncBoxOf(ncAllCrops(), k)) { const sz = NC_SIZE[k] || (await ncRetailSize(k).catch(() => null)) || [w, h]; el.innerHTML = `<canvas data-pv="${k}" width="${sz[0]}" height="${sz[1]}"></canvas>`; continue; }
    const toCanvas = (im) => { const c = document.createElement('canvas'); c.width = im.w; c.height = im.h; c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(im.rgba.buffer, im.rgba.byteOffset, im.w * im.h * 4), im.w, im.h), 0, 0); el.innerHTML = ''; el.appendChild(c); };
    if (ncArtState(k).kind === 'game') {               // the series' plate from the game (avatars/logo.png, if any, is an older drawn one the build doesn't use)
      try { const im = ISO.index ? await ncRetailLogoGim(ncRetailLogo((ncMod().character || {}).series)) : null; if (im) { toCanvas(im); continue; } } catch (err) { /* fall through */ }
      el.innerHTML = `<span class="art-none">${esc((ncMod().character || {}).series || '')}<br>from the game</span>`; continue;
    }
    if (e.picture) { try { showBlob(el, await FS.read(S.charDir + e.picture), k); continue; } catch (err) { /* missing: fall through */ } }
    const f = (e._files || [])[0];
    if (f && like && typeof isoHas === 'function' && ISO.index && isoHas(f.replace(code, like))) {
      try {
        const im = decodeGIM(await isoGet(f.replace(code, like))); if (ncStudioKey(k)) NC_SIZE[k] = [im.w, im.h];
        toCanvas(im); continue;
      } catch (err) { /* fall through */ }
    }
    el.innerHTML = `<span class="art-none">${e.picture ? 'missing' : 'after the build'}</span>`;
  }
  ncArtImg().then((img) => img && ncArtPreviews(img)).catch(() => {});
}
function ncArtPreviews(img) {                       // the cut cards, live while a box moves
  const av = ncMod().avatars || {}, cr = ncAllCrops(), o = (av.generated_from || {}).options || {};
  for (const c of document.querySelectorAll('#artList canvas[data-pv]')) {
    const k = c.dataset.pv, b = ncBoxOf(cr, ncCropKey(k)); if (!b) continue;
    const flip = k === 'face_strip_mirrored' || (k === 'hud' && o.hud_flip) || (k === 'grid_tile' && o.tile_flip);
    const g = c.getContext('2d'); g.save(); g.clearRect(0, 0, c.width, c.height);
    if (flip) { g.translate(c.width, 0); g.scale(-1, 1); }
    g.drawImage(img, b[0], b[1], b[2] - b[0], b[3] - b[1], 0, 0, c.width, c.height); g.restore();
  }
}
const NC_COL = { slidein: '#8fd3ff', grid_tile: '#e8ac52', hud: '#6fd39b', vs_face: '#ff6f7f', face_strip: '#c89bff', select_portrait: '#f2d16b', arcade_last: '#ff9f6b', talk_top: '#7fe0d6', talk_bottom: '#a0b4ff' };
function ncCropEditor(img) {                        // the art and its boxes, as big as the stage allows
  const cv = $('ncCanvas'), host = $('ncCrop'); if (!cv || !host) return;
  const pad = 0.12, AW = Math.max(160, host.clientWidth - 12), AH = Math.max(160, host.clientHeight - 12);
  host.classList.toggle('zoomed', (S.ncZoom || 1) > 1);
  const sc = Math.min(AW / (img.width * (1 + 2 * pad)), AH / (img.height * (1 + 2 * pad))) * (S.ncZoom || 1);
  const W = Math.round(img.width * (1 + 2 * pad) * sc), H = Math.round(img.height * (1 + 2 * pad) * sc), dpr = window.devicePixelRatio || 1;
  if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
  cv.style.width = W + 'px'; cv.style.height = H + 'px';
  const ox = img.width * pad, oy = img.height * pad;
  const toC = (x, y) => [(x + ox) * sc, (y + oy) * sc];
  const boxes = () => { const cr = ncAllCrops(); return [...Object.keys(NC_CROPS), ...Object.keys(cr).filter((k) => !NC_CROPS[k])].filter((k) => !S.ncFocus || k === S.ncFocus).map((k) => [k, ncBoxOf(cr, k)]).filter(([, b]) => b); };
  const draw = () => {
    const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, W, H);
    g.fillStyle = 'rgba(255,255,255,0.04)'; g.fillRect(0, 0, W, H);
    const [x0, y0] = toC(0, 0); g.drawImage(img, x0, y0, img.width * sc, img.height * sc);
    for (const [k, b] of boxes()) {
      const [a, c] = toC(b[0], b[1]), [e, d] = toC(b[2], b[3]);
      g.strokeStyle = NC_COL[k] || '#ddd'; g.lineWidth = k === S.ncBox ? 2.5 : 1.5; g.strokeRect(a, c, e - a, d - c);
      g.fillStyle = NC_COL[k] || '#ddd'; g.fillRect(e - 6, d - 6, 12, 12);
      g.font = '12px sans-serif'; const t = k.replace(/_/g, ' '), tw = g.measureText(t).width;
      g.fillStyle = 'rgba(0,0,0,.55)'; g.fillRect(a + 1, c + 1, tw + 8, 16); g.fillStyle = NC_COL[k] || '#ddd'; g.fillText(t, a + 5, c + 13);
    }
    ncArtPreviews(img);
  };
  if ((S.ncZoom || 1) > 1 && S.ncFocus && S.ncScrollFor !== S.ncFocus) {   // zoomed: bring a newly picked box into view
    const b = ncBoxOf(ncAllCrops(), S.ncFocus);
    if (b) { const [a, c] = toC(b[0], b[1]), [e, d] = toC(b[2], b[3]); requestAnimationFrame(() => { host.scrollLeft = (a + e) / 2 - host.clientWidth / 2; host.scrollTop = (c + d) / 2 - host.clientHeight / 2; }); }
    S.ncScrollFor = S.ncFocus;
  }
  let drag = null;
  const hit = (px, py) => {
    for (const [k, b] of boxes().reverse()) {
      const [a, c] = toC(b[0], b[1]), [e, d] = toC(b[2], b[3]);
      if (Math.abs(px - e) < 10 && Math.abs(py - d) < 10) return { k, mode: 'size', b };
      if (px >= a && px <= e && py >= c && py <= d) return { k, mode: 'move', b };
    }
    return null;
  };
  cv.onpointerdown = (ev) => {
    if (S.readOnly) return;
    const r = cv.getBoundingClientRect(), px = ev.clientX - r.left, py = ev.clientY - r.top, h = hit(px, py);
    if (!h) return;
    pushUndo([S.modPath]);
    drag = { ...h, px, py, b0: h.b.slice() }; S.ncBox = h.k; cv.setPointerCapture(ev.pointerId);
  };
  cv.onpointermove = (ev) => {
    if (!drag) { const r = cv.getBoundingClientRect(), h = hit(ev.clientX - r.left, ev.clientY - r.top); cv.style.cursor = h && !S.readOnly ? (h.mode === 'size' ? 'nwse-resize' : 'move') : 'default'; return; }
    const r = cv.getBoundingClientRect(), dx = (ev.clientX - r.left - drag.px) / sc, dy = (ev.clientY - r.top - drag.py) / sc, b = drag.b0.slice();
    if (drag.mode === 'move') { b[0] += dx; b[2] += dx; b[1] += dy; b[3] += dy; }
    else { const [aw, ah] = ncAspect(drag.k) || [drag.b0[2] - drag.b0[0], drag.b0[3] - drag.b0[1]], w = Math.max(8, b[2] - b[0] + dx); b[2] = b[0] + w; b[3] = b[1] + w * ah / aw; }
    ncSetBox(drag.k, b.map(Math.round).join(','));
    draw();
  };
  cv.onpointerup = () => { if (!drag) return; const k = drag.k; drag = null; markDirty([S.modPath]); S.gen = (S.gen || 0) + 1; if (ncStudioKey(k)) ncStudioCut(k); };
  const fb = $('ncFocusNote');
  if (fb) {
    fb.hidden = !S.ncFocus;
    fb.innerHTML = S.ncFocus ? `Showing <b class="ncb-${S.ncFocus}">${S.ncFocus.replace(/_/g, ' ')}</b> only · <button type="button" class="linkish" id="ncShowAll">Show all</button>` : '';
    const sa = $('ncShowAll'); if (sa) sa.onclick = () => { S.ncFocus = null; S.ncArtKey = null; document.querySelectorAll('#artList [data-art]').forEach((c) => { c.classList.remove('on'); c.setAttribute('aria-pressed', 'false'); const d = c.querySelector('.art-detail'); if (d) d.remove(); }); ncCropEditor(img); renderInspector(); };
  }
  draw();
}

/* ---------------------------------------------------------------- physics preview: the 3D view alone (no timeline)
   The character plays its own jump (script 100: the squat, take-off at tick 4), rising to the spec record's height in T
   ticks (rise gravity 2h/T²), falls (script 101) with that gravity × P60 down to the terminal speed −P96, lands (102),
   then dashes (R + △, script 240: 24 ticks of start-up, then forward; 241 the stop). The dash speed is Bartz's start speed
   (spec +0x114, 0.6 m/tick) × the dash= factor and held: the game's slow-down over the dash isn't modelled. Loops. */
const PH = { on: false, t: 0, track: null, mode: 'both', slow: false, guide: null, keep: null, lastZ: 0 };
const PH_DASH_V = 0.6, PH_DASH_TICKS = 36;
function phSync(on) {
  if (typeof view === 'undefined' || !view.root || !S.gmo) on = false;
  const C = document.querySelector('.center');
  if (on && PH.on) { phPlan(); return; }
  if (on === PH.on) return;
  PH.on = on; if (C) C.classList.toggle('phys-mode', on);
  if (on) {
    S.playing = false; if ($('playBtn')) $('playBtn').textContent = '▶';
    try { fxUpdate(view, S.fx, null, 0, { on: false }); } catch (e) { /* nothing shown */ }
    if (typeof DM_BODY !== 'undefined' && DM_BODY.root) DM_BODY.root.visible = false;
    PH.keep = { p: view.root.position.clone(), r: view.root.rotation.y, cam: view.camera.position.clone(), tgt: view.controls.target.clone() };
    PH.t = 0; PH.lastZ = 0; phPlan(); phCam();
  } else {
    phGuide(null);
    const H = $('physHud'); if (H) H.hidden = true;
    if (PH.keep) { view.root.position.copy(PH.keep.p); view.root.rotation.y = PH.keep.r; view.camera.position.copy(PH.keep.cam); view.controls.target.copy(PH.keep.tgt); }
    pose(); window.dispatchEvent(new Event('resize'));
  }
}
function phCam() {                              // from the side, far enough to see the whole jump; the dash runs across the view
  const k = PH.keep, yaw = k.r, h = phNums().sp.h, d = Math.max(7, (h + 2) / Math.tan(17.5 * Math.PI / 180) * 0.75);
  const right = new THREE.Vector3(Math.cos(yaw), 0, -Math.sin(yaw)), fw = new THREE.Vector3(Math.sin(yaw), 0, Math.cos(yaw));
  const tgt = k.p.clone().add(new THREE.Vector3(0, PH.mode === 'dash' ? 0.8 : h / 2 + 0.4, 0));
  view.controls.target.copy(tgt); view.camera.position.copy(tgt).addScaledVector(right, d).add(new THREE.Vector3(0, 2.2, 0)).addScaledVector(fw, -2);
}
function phNums() {
  const m = ncMod(), c = m.character || {}, sp = ACT.specOf(c.based_on || '');
  const tok = (m.runtime_hooks || []).find((t) => typeof t === 'string' && t.startsWith('dash=')), dash = tok ? parseFloat(tok.slice(5)) || 1 : 1;
  return { sp, g: 2 * sp.h / (sp.T * sp.T), dash };
}
function phPlan() {
  const { sp, g, dash } = phNums(), rows = (F(msPath('motion_map.json')) || {}).rows || [];
  const mo = (s) => { const r = rows.find((x) => x.script === s); return r ? r.motion : null; };
  const stand = mo(1) ?? 0, len = (m) => (m == null ? 1 : motionLen(m));
  const T = [], add = (n, rec) => { for (let k = 0; k < n; k++) T.push(rec(k)); };
  const idle = (n, label, z) => add(n, (k) => ({ mo: stand, fr: (k / 2) % Math.max(1, len(stand)), y: 0, z, label }));
  let z = 0;
  idle(30, 'Standing', 0);
  if (PH.mode !== 'dash') {
    const j = mo(100) ?? stand, fall = mo(101) ?? j, land = mo(102);
    add(4, (k) => ({ mo: j, fr: k / 2, y: 0, z, label: 'Jump: squat' }));
    add(sp.T, (k) => { const t = k + 1; return { mo: j, fr: Math.min((4 + t) / 2, len(j)), y: sp.h - 0.5 * g * (sp.T - t) ** 2, z, label: 'Jump: rising' }; });
    let y = sp.h, t = 0;
    while (y > 0 && t < 600) {
      t += 1; const want = sp.h - 0.5 * g * sp.P60 * t * t; y = Math.max(0, y + Math.max(want - y, sp.P96));
      T.push({ mo: fall, fr: Math.min(t / 2, len(fall)), y, z, label: y - (T[T.length - 1].y) <= sp.P96 + 1e-6 ? 'Falling (top speed)' : 'Falling' });
    }
    if (land != null) add(Math.min(30, len(land) * 2), (k) => ({ mo: land, fr: k / 2, y: 0, z, label: 'Landing' }));
    idle(24, 'Standing', z);
  }
  if (PH.mode !== 'jump') {
    const d = mo(240), stop = mo(241);
    if (d != null) {
      const fr = (k) => { const f = k / 2; return f < 34 ? f : 20 + ((f - 20) % 14); };
      add(24, (k) => ({ mo: d, fr: fr(k), y: 0, z, label: 'Dash: start-up' }));
      add(PH_DASH_TICKS, (k) => { z += PH_DASH_V * dash; return { mo: d, fr: fr(24 + k), y: 0, z, label: `Dash: ${(PH_DASH_V * dash).toFixed(2)} m/tick` }; });
      if (stop != null) add(Math.min(30, len(stop) * 2), (k) => ({ mo: stop, fr: k / 2, y: 0, z, label: 'Dash: stop' }));
      idle(30, 'Standing', z);
    }
  }
  PH.track = T; PH.dashLen = PH_DASH_V * dash * PH_DASH_TICKS;
  phGuide({ h: sp.h, dash: PH.mode !== 'jump' && mo(240) != null ? PH.dashLen : 0 });
}
function phGuide(o) {                            // the apex ring (jump height) and the dash's path on the floor
  if (PH.guide) { view.scene.remove(PH.guide); PH.guide.traverse((x) => { x.geometry && x.geometry.dispose(); x.material && x.material.dispose(); }); PH.guide = null; }
  if (!o) return;
  const G = new THREE.Group(), p = PH.keep ? PH.keep.p : new THREE.Vector3(), yaw = PH.keep ? PH.keep.r : 0;
  G.position.set(p.x, 0, p.z); G.rotation.y = yaw;
  if (PH.mode !== 'dash') {
    const ring = new THREE.Mesh(new THREE.RingGeometry(0.42, 0.45, 48), new THREE.MeshBasicMaterial({ color: 0xe8ac52, transparent: true, opacity: 0.8, side: THREE.DoubleSide }));
    ring.rotation.x = -Math.PI / 2; ring.position.y = o.h; G.add(ring);
    const pole = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0.44, 0, 0), new THREE.Vector3(0.44, o.h, 0)]), new THREE.LineDashedMaterial({ color: 0xe8ac52, dashSize: 0.08, gapSize: 0.06 }));
    pole.computeLineDistances(); G.add(pole);
  }
  if (o.dash) {
    const path = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0, 0.01, 0), new THREE.Vector3(0, 0.01, o.dash)]), new THREE.LineBasicMaterial({ color: 0x8fd3ff, transparent: true, opacity: 0.7 }));
    G.add(path);
    const end = new THREE.Mesh(new THREE.RingGeometry(0.25, 0.28, 32), new THREE.MeshBasicMaterial({ color: 0x8fd3ff, side: THREE.DoubleSide }));
    end.rotation.x = -Math.PI / 2; end.position.set(0, 0.012, o.dash); G.add(end);
  }
  const floor = new THREE.GridHelper(80, 80, 0x2b3466, 0x1a2046); floor.position.set(0, -0.001, 0); G.add(floor);   // the floor goes on past the 8 m grid
  view.scene.add(G); PH.guide = G;
}
function phTick(dt) {
  if (!PH.track || !PH.track.length || !S.gmo || !view.root) return;
  PH.t = (PH.t + dt * 60 * (PH.slow ? 0.25 : 1)) % PH.track.length;
  const r = PH.track[Math.floor(PH.t)], k = PH.keep, yaw = k.r;
  poseMotion(S.gmo, view, r.mo, r.fr);
  view.root.position.set(k.p.x + Math.sin(yaw) * r.z, k.p.y + r.y, k.p.z + Math.cos(yaw) * r.z);
  view.root.rotation.y = yaw;
  const dz = r.z - PH.lastZ; PH.lastZ = r.z;                  // the camera follows the dash (and comes back when it loops)
  if (dz) { const d = new THREE.Vector3(Math.sin(yaw) * dz, 0, Math.cos(yaw) * dz); view.camera.position.add(d); view.controls.target.add(d); }
  let H = $('physHud');
  if (!H) { H = document.createElement('div'); H.id = 'physHud'; H.className = 'phys-hud'; $('viewport').appendChild(H); }
  H.hidden = false;
  const txt = `<b>${esc(r.label)}</b><span>height ${r.y.toFixed(2)} m</span>${r.z ? `<span>travelled ${r.z.toFixed(1)} m</span>` : ''}<span class="mono">tick ${Math.floor(PH.t)} / ${PH.track.length}</span>`;
  if (H.innerHTML !== txt) H.innerHTML = txt;
}

/* ---------------------------------------------------------------- physics: the spec record (read) and dash= (runtime) */
function ncPhysics(I) {
  const c = ncMod().character || {}, sp = ACT.specOf(c.based_on || ''), g = 2 * sp.h / (sp.T * sp.T);
  const tok = (ncMod().runtime_hooks || []).find((t) => typeof t === 'string' && t.startsWith('dash=')), dash = tok ? parseFloat(tok.slice(5)) : '';
  const air = Math.sqrt(2 * sp.h / (g * sp.P60)), total = sp.T + air;
  I.innerHTML = `<div class="sec" id="ncSec"><h3>Jump, gravity, dash</h3>
    <div class="lbl">The jump and fall <small>battle.bin spec record ${sp.id}${sp.guess ? ' (a guess: this character\'s record isn\'t in the studio\'s table)' : ''}, the built-on character's</small></div>
    <table class="nc-spec">
      <tr><td>Jump height</td><td class="mono">${sp.h} m</td><td>reached in ${sp.T} ticks (${(sp.T / 60).toFixed(2)} s)</td></tr>
      <tr><td>Rise gravity</td><td class="mono">${g.toFixed(5)} m/tick²</td><td>2·height / time²</td></tr>
      <tr><td>Fall</td><td class="mono">× ${sp.P60}</td><td>${(g * sp.P60).toFixed(5)} m/tick², down again in ${Math.round(air)} ticks: a jump lasts about ${Math.round(total)} ticks</td></tr>
      <tr><td>Terminal fall</td><td class="mono">${-sp.P96} m/tick</td><td>${(-sp.P96 * 60).toFixed(0)} m/s</td></tr>
      <tr><td>Air drift kept</td><td class="mono">${sp.P148}</td><td>per tick with no stick input</td></tr></table>
    <p class="note">These come with the character the mod is built on (its charId's spec record); the studio's dummy and attacker use them. A mod can't change them yet.</p>
    <div class="lbl">Dash (R + △)</div>
    <div class="row2"><div class="field"><label>Dash speed ×</label><input type="number" id="ncDash" min="0.25" max="4" step="0.05" value="${dash}" placeholder="1 (as built on)"${S.readOnly ? ' disabled' : ''}></div>
      <div class="field fact"><label>Runtime hook</label><span class="mono">${esc(tok || 'none')}</span></div></div>
    <p class="note">dash=F in mod.json runtime_hooks: the native runtime scales the dash's speed (0.25–4; roster.cpp dash_move_hook). For example 1.75.</p></div>
    <div class="sec" id="phSec"><h3>Preview</h3>
      <div class="btnrow seg" role="group" aria-label="What the 3D view shows">${[['both', 'Jump and dash'], ['jump', 'Jump'], ['dash', 'Dash']].map(([k, l]) => `<button type="button" class="mini" data-ph="${k}" aria-pressed="${PH.mode === k}">${l}</button>`).join('')}
        <label class="chk"><input type="checkbox" id="phSlow"${PH.slow ? ' checked' : ''}> Slow motion</label></div>
      <p class="note">The character's own jump, fall and landing animations, moved by the numbers above; the ring is the jump height. The dash moves at Bartz's start speed (0.6 m/tick) × the dash speed after its 24-tick start-up, held for ${PH_DASH_TICKS} ticks: the game slows it down over the dash, which isn't shown.</p></div>`;
  I.querySelectorAll('[data-ph]').forEach((b) => b.onclick = () => { PH.mode = b.dataset.ph; PH.t = 0; PH.lastZ = 0; if (PH.on) { phPlan(); phCam(); } I.querySelectorAll('[data-ph]').forEach((x) => x.setAttribute('aria-pressed', x === b)); });
  const sl = $('phSlow'); if (sl) sl.onchange = () => { PH.slow = sl.checked; };
  const d = $('ncDash');
  if (d) d.onchange = () => {
    const v = parseFloat(d.value);
    edHookSet('dash', isNaN(v) || v === 1 ? null : `dash=${Math.min(4, Math.max(0.25, v))}`, (t) => t.startsWith('dash='));
    renderLeft(); renderInspector();
  };
}
