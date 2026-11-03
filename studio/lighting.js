/* Live character lighting preview (mod.json body.lighting; docs/character-lighting.md).

   The native build lights a body with a key and a rim light that follow the camera (games/dissidia/fx_shaders/lit/
   character.frag, set up by charmod_light.py). This draws the viewport's body the same way: the same normals (welded
   across the exporter's seam splits, creased at 60°, the head's rounded) and the same cel shading on top of the
   game's colour, worked in the game's (gamma) colour space. Settings change it live; Character tab > Body > Lighting edits them.
   The game lights a body only when it wears a skin (body.skin: a skin or manikin), so the preview does the same. */

const LIT_DEFAULTS = {                    // charmod_light.DEFAULTS: keep in step
  key_dir: [-0.35, 0.55, 0.76], key: 1.0, wrap: 0.6, threshold: 0.4, edge: 0.04, bands: 2,
  light: '#fffaf2', shadow: '#8b889c', shadow_sat: 0.3, spec: 0.25, spec_size: 0.15,
  rim: 0.75, rim_width: 0.18, rim_dir: [0.6, 0.3, -0.75], rim_colour: '#e2efff', face: 0.7,
};
const LIT_NOT_UNIFORMS = new Set(['face']);
/* named looks (body.lighting.preset), each over the defaults: charmod_light.PRESETS, keep in step */
const LIT_PRESETS = {
  cel: { label: 'Cel', tip: 'Duellum: a lit and a shadow tone with a crisp edge (the default)', v: {} },
  anime: { label: 'Anime', tip: 'A harder edge, deeper and richer shadows, a brighter highlight',
    v: { threshold: 0.46, edge: 0.015, shadow: '#7d7895', shadow_sat: 0.45, spec: 0.35, rim: 0.9, rim_width: 0.14 } },
  three_tone: { label: 'Three-tone', tip: 'Lit, mid and shadow tones', v: { bands: 3, threshold: 0.3, edge: 0.035, shadow: '#86829a' } },
  soft: { label: 'Soft', tip: 'A smooth gradient, no hard edge',
    v: { bands: 0, key: 0.8, wrap: 0.45, shadow: '#727aa6', shadow_sat: 0.15, spec: 0.1, rim: 0.55, rim_width: 0.3, face: 0.5 } },
  dramatic: { label: 'Dramatic', tip: 'Light from the side, deep shadows, a strong warm rim',
    v: { key_dir: [-0.8, 0.45, 0.4], threshold: 0.5, edge: 0.03, shadow: '#5e5878', shadow_sat: 0.45, spec: 0.3, rim: 1.2, rim_width: 0.22, rim_colour: '#ffe0bd' } },
  subtle: { label: 'Subtle', tip: 'A little form, close to the retail look',
    v: { bands: 0, key: 0.45, wrap: 0.7, shadow: '#7a80a0', shadow_sat: 0.1, spec: 0, rim: 0.35, rim_width: 0.25, face: 0.5 } },
};
const litPresetOf = (v) => (v && typeof v === 'object' && LIT_PRESETS[v.preset] ? v.preset : 'cel');
const litBase = (preset) => Object.assign({}, LIT_DEFAULTS, (LIT_PRESETS[preset] || LIT_PRESETS.cel).v);       // build time: bends the head's normals (charmod_light.head_info)
const LIT = { uniforms: null, mats: new Map(), applied: null, sig: '', live: null, compare: false };

/* the settings in force: body.lighting merged over the defaults (null when off) */
function litSettings() {
  const v = S.mod && S.mod.body && S.mod.body.lighting;
  if (!v) return null;
  const own = {}; if (typeof v === 'object') for (const k of Object.keys(LIT_DEFAULTS)) if (k in v) own[k] = v[k];
  return Object.assign(litBase(litPresetOf(v)), own, LIT.live || {}, { preset: litPresetOf(v) });
}
/* what the game would light: a body skin is shown (not the rig as stored, which is the game's own or "as_is") */
function litBodyLit() { return !!S.skinShown || (typeof skinStored === 'function' && skinStored() === 'manikin'); }

function litUnit(v) { const n = Math.hypot(...v) || 1; return new THREE.Vector3(v[0] / n, v[1] / n, v[2] / n); }
function litUniforms() {
  if (LIT.uniforms) return LIT.uniforms;
  LIT.uniforms = {};
  for (const [k, v] of Object.entries(LIT_DEFAULTS)) {
    if (LIT_NOT_UNIFORMS.has(k)) continue;
    LIT.uniforms['u_' + k] = { value: Array.isArray(v) ? new THREE.Vector3() : typeof v === 'string' ? new THREE.Color() : 0 };
  }
  return LIT.uniforms;
}
function litSetUniforms(s) {
  const u = litUniforms();
  for (const k of Object.keys(LIT_DEFAULTS)) {
    const x = u['u_' + k]; if (!x) continue;
    if (x.value.isVector3) x.value.copy(litUnit(s[k]));
    else if (x.value.isColor) x.value.set(s[k]);   // gamma-space values, as the game uses them
    else x.value = +s[k];
  }
}

/* smooth normals the way charmod_light.add_normals makes them */
function litNormals(g) {
  const P = g.attributes.position.array, idx = g.index.array, nv = P.length / 3, nt = idx.length / 3;
  let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < nv; i++) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], P[i * 3 + k]); hi[k] = Math.max(hi[k], P[i * 3 + k]); }
  const q = 1e-4 * Math.max(1e-6, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
  const keys = new Map(), gid = new Int32Array(nv);
  for (let i = 0; i < nv; i++) {
    const k = Math.round(P[i * 3] / q) + ',' + Math.round(P[i * 3 + 1] / q) + ',' + Math.round(P[i * 3 + 2] / q);
    if (!keys.has(k)) keys.set(k, keys.size);
    gid[i] = keys.get(k);
  }
  const fn = new Float32Array(nt * 3), un = new Float32Array(nt * 3);
  let vol = 0;
  for (let t = 0; t < nt; t++) {
    const a = idx[t * 3] * 3, b = idx[t * 3 + 1] * 3, c = idx[t * 3 + 2] * 3;
    const e1 = [P[b] - P[a], P[b + 1] - P[a + 1], P[b + 2] - P[a + 2]], e2 = [P[c] - P[a], P[c + 1] - P[a + 1], P[c + 2] - P[a + 2]];
    const n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
    vol += P[a] * (P[b + 1] * P[c + 2] - P[b + 2] * P[c + 1]) - P[a + 1] * (P[b] * P[c + 2] - P[b + 2] * P[c]) + P[a + 2] * (P[b] * P[c + 1] - P[b + 1] * P[c]);
    const l = Math.hypot(...n) || 1e-12;
    for (let k = 0; k < 3; k++) { fn[t * 3 + k] = n[k]; un[t * 3 + k] = n[k] / l; }
  }
  if (vol < 0) for (let i = 0; i < fn.length; i++) { fn[i] = -fn[i]; un[i] = -un[i]; }
  const at = new Map(), own = Array.from({ length: nv }, () => []);
  for (let t = 0; t < nt; t++) for (let j = 0; j < 3; j++) {
    const v = idx[t * 3 + j]; own[v].push(t);
    const gk = gid[v]; if (!at.has(gk)) at.set(gk, []); at.get(gk).push(t);
  }
  const cosC = Math.cos(60 * Math.PI / 180), N = new Float32Array(nv * 3);
  for (let v = 0; v < nv; v++) {
    const mine = own[v];
    if (!mine.length) { N[v * 3 + 2] = 1; continue; }
    const s = [0, 0, 0]; for (const t of mine) for (let k = 0; k < 3; k++) s[k] += un[t * 3 + k];
    const sl = Math.hypot(...s) || 1e-12;
    const r = [0, 0, 0]; let any = false;
    for (const t of at.get(gid[v])) {
      if ((un[t * 3] * s[0] + un[t * 3 + 1] * s[1] + un[t * 3 + 2] * s[2]) / sl < cosC) continue;
      for (let k = 0; k < 3; k++) r[k] += fn[t * 3 + k]; any = true;
    }
    const o = any ? r : s, l = Math.hypot(...o) || 1;
    for (let k = 0; k < 3; k++) N[v * 3 + k] = o[k] / l;
  }
  g.userData.litBase = N;
  g.setAttribute('normal', new THREE.Float32BufferAttribute(N.slice(), 3));
}
/* charmod_light's head trick: the head's (and hair's) normals bent towards a sphere round it, by face x the vertex's
   weight on those bones, so the face gets one clean light / shadow side instead of blotches */
function litFace(g, bones, face) {
  const base = g.userData.litBase, P = g.attributes.position.array, SI = g.attributes.skinIndex, SW = g.attributes.skinWeight;
  const nv = P.length / 3, out = g.attributes.normal.array;
  const head = new Set((bones || []).map((b, i) => (/^(head|hair)$/i.test(b.name) ? i : -1)).filter((i) => i >= 0));
  const w = new Float32Array(nv);
  if (head.size && SI && SW) for (let v = 0; v < nv; v++) for (let k = 0; k < 4; k++) if (head.has(SI.array[v * 4 + k])) w[v] += SW.array[v * 4 + k];
  let c = [0, 0, 0], n = 0;
  for (let v = 0; v < nv; v++) if (w[v] > 0.5) { c[0] += P[v * 3]; c[1] += P[v * 3 + 1]; c[2] += P[v * 3 + 2]; n++; }
  out.set(base);
  if (n > 8 && face > 0) {
    c = c.map((x) => x / n);
    for (let v = 0; v < nv; v++) {
      const k = Math.min(1, w[v]) * face; if (k <= 0) continue;
      const d = [P[v * 3] - c[0], P[v * 3 + 1] - c[1], P[v * 3 + 2] - c[2]], dl = Math.hypot(...d) || 1;
      const r = [0, 1, 2].map((j) => base[v * 3 + j] * (1 - k) + d[j] / dl * k), rl = Math.hypot(...r) || 1;
      for (let j = 0; j < 3; j++) out[v * 3 + j] = r[j] / rl;
    }
  }
  g.attributes.normal.needsUpdate = true; g.userData.litFace = face;
}

/* a lit copy of one of the body's materials: MeshBasicMaterial (the game's flat look) plus character.frag's shading */
function litMaterial(m) {
  if (LIT.mats.has(m)) return LIT.mats.get(m);
  const lm = m.clone();
  lm.onBeforeCompile = (sh) => {
    Object.assign(sh.uniforms, litUniforms());
    // MeshBasicMaterial skins the normal only for an environment map: do it here otherwise (the skinned, view-space normal)
    sh.vertexShader = 'varying vec3 vLitN;\nvarying vec3 vLitV;\n' + sh.vertexShader
      .replace('#include <skinbase_vertex>', '#include <skinbase_vertex>\n#ifndef USE_ENVMAP\n#include <beginnormal_vertex>\n#include <morphnormal_vertex>\n#include <skinnormal_vertex>\n#include <defaultnormal_vertex>\n#endif')
      .replace('#include <fog_vertex>', '#include <fog_vertex>\n  vLitN = normalize( transformedNormal );\n  vLitV = -mvPosition.xyz;');
    sh.fragmentShader = `varying vec3 vLitN;
varying vec3 vLitV;
uniform vec3 u_key_dir; uniform float u_key; uniform float u_wrap; uniform float u_threshold; uniform float u_edge;
uniform float u_bands; uniform vec3 u_light; uniform vec3 u_shadow; uniform float u_shadow_sat; uniform float u_spec;
uniform float u_spec_size; uniform float u_rim; uniform float u_rim_width; uniform vec3 u_rim_dir; uniform vec3 u_rim_colour;
float lit_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lit_step(float x, float at, float e) { return smoothstep(at - e, at + e, x); }
vec3 lit_shade(vec3 base) {                      // character.frag, in view space (x right, y up, z towards the viewer)
  vec3 Nv = normalize(vLitN); if (!gl_FrontFacing) Nv = -Nv;
  vec3 Vv = normalize(vLitV);
  float h = clamp((dot(Nv, u_key_dir) + u_wrap) / (1.0 + u_wrap), 0.0, 1.0);
  float e = max(u_edge, fwidth(h) * 0.75), t;
  if (u_bands >= 2.0) {
    float n = floor(u_bands + 0.5) - 1.0; t = 0.0;
    for (int k = 0; k < 3; k++) { if (float(k) >= n) break; t += lit_step(h, u_threshold + float(k) * (1.0 - u_threshold) / n, e); }
    t /= n;
  } else t = h;
  vec3 lum = vec3(lit_luma(base));
  vec3 rich = clamp(mix(lum, base, 1.0 + u_shadow_sat), 0.0, 1.0);
  vec3 col = mix(base, mix(rich * u_shadow, base * u_light, t), u_key);
  vec3 H = normalize(u_key_dir + Vv);
  float hs = dot(Nv, H), sc = mix(0.995, 0.88, clamp(u_spec_size, 0.0, 1.0));
  col += u_light * lit_step(hs, sc, max(0.006, fwidth(hs))) * t * smoothstep(0.3, 0.75, lum.r) * u_spec * 0.5;
  float fres = 1.0 - abs(dot(Nv, Vv));
  float side = clamp(dot(normalize(Nv.xy + 1e-5), normalize(u_rim_dir.xy + 1e-5)) * 0.75 + 0.25, 0.0, 1.0);
  float rim = lit_step(fres, 1.0 - clamp(u_rim_width, 0.01, 1.0), max(0.03, fwidth(fres))) * side * u_rim;
  return min(col + u_rim_colour * rim * mix(0.45, 1.0, lum.r), vec3(1.0));
}
` + sh.fragmentShader.replace('#include <envmap_fragment>',
      '#include <envmap_fragment>\n  outgoingLight = sRGBToLinear( vec4( lit_shade( LinearTosRGB( vec4( outgoingLight, 1.0 ) ).rgb ), 1.0 ) ).rgb;');
  };
  lm.customProgramCacheKey = () => 'dfx-lit-cel';
  LIT.mats.set(m, lm);
  return lm;
}

/* put the lit materials on (or take them off) the body as the settings say; cheap when nothing changed */
function litSync() {
  const pm = view.partMeshes && view.partMeshes[0], mesh = pm && pm.mesh;
  const s = litSettings(), on = !!(s && mesh && litBodyLit() && !LIT.compare);
  if (s) litSetUniforms(s);
  if (on && mesh.geometry.userData.litBase && mesh.geometry.userData.litFace !== +s.face) litFace(mesh.geometry, view.bones, +s.face);
  const sig = on ? 'on' : 'off';
  if (mesh === LIT.applied && sig === LIT.sig) return;
  if (LIT.applied && LIT.applied !== mesh) LIT.mats.clear();            // a new body: new materials
  if (mesh) {
    if (!mesh.userData.flatMats) mesh.userData.flatMats = mesh.material;
    if (on && !mesh.geometry.userData.litBase) litNormals(mesh.geometry);
    if (on && mesh.geometry.userData.litFace !== +s.face) litFace(mesh.geometry, view.bones, +s.face);
    mesh.material = on ? mesh.userData.flatMats.map(litMaterial) : mesh.userData.flatMats;
  }
  LIT.applied = mesh; LIT.sig = sig;
}
(function litLoop() { try { if (typeof view !== 'undefined') litSync(); } catch (e) { console.warn('lighting preview', e); } requestAnimationFrame(litLoop); })();

/* ---------------------------------------------------------------- Character tab > Body > Lighting */
const litDeg = (r) => Math.round(r * 180 / Math.PI);
function litAngles(s) {                         // key_dir -> around / height (degrees); rim_dir -> side (degrees)
  const k = litUnit(s.key_dir);
  return { around: litDeg(Math.atan2(k.x, k.z)), height: litDeg(Math.asin(Math.max(-1, Math.min(1, k.y)))),
           side: litDeg(Math.atan2(s.rim_dir[1], s.rim_dir[0])) };
}
function litFromAngles(a) {
  const r = (d) => d * Math.PI / 180, ch = Math.cos(r(a.height));
  return { key_dir: [+(Math.sin(r(a.around)) * ch).toFixed(3), +Math.sin(r(a.height)).toFixed(3), +(Math.cos(r(a.around)) * ch).toFixed(3)],
           rim_dir: [+Math.cos(r(a.side)).toFixed(3), +Math.sin(r(a.side)).toFixed(3), -0.75] };
}
const LIT_SLIDERS = [
  ['key', 'Shading', 0, 1, 0.05, 'How much the light shapes the body: 0 is the game\'s flat look'],
  ['bands', 'Tones', 0, 4, 1, '2: lit + shadow, like Duellum; 3 adds a mid tone; 0 a smooth gradient'],
  ['threshold', 'Shadow amount', 0, 0.9, 0.02, 'Where the shadow starts: higher puts more of the body in shadow'],
  ['edge', 'Edge softness', 0.01, 0.5, 0.01, 'The edge between light and shadow: low is crisp, high is soft'],
  ['wrap', 'Light wrap', 0, 1, 0.05, 'How far the light reaches round the body'],
  ['around', 'Light from (around)', -180, 180, 5, 'Degrees round from the camera: 0 in front, minus to the left'],
  ['height', 'Light from (height)', -30, 90, 5, 'Degrees above the camera\'s eye line'],
  ['shadow_sat', 'Shadow richness', 0, 1, 0.05, 'How much more saturated the shadow colours are'],
  ['face', 'Face smoothing', 0, 1, 0.05, 'Rounds the head\'s shading so the face gets one clean shadow side (head and hair bones)'],
  ['spec', 'Highlight', 0, 1, 0.05, 'A small hard highlight on bright parts: hair, metal, skin'],
  ['spec_size', 'Highlight size', 0, 1, 0.05, 'How big the highlight is'],
  ['rim', 'Rim light', 0, 1.5, 0.05, 'How bright the outline light is'],
  ['rim_width', 'Rim width', 0.02, 0.6, 0.01, 'How wide the outline light is'],
  ['side', 'Rim side', -180, 180, 5, 'Which side of the outline is lit: 0 right, 90 top, 180 left'],
];
const LIT_COLOURS = [['light', 'Lit side'], ['shadow', 'Shadow side'], ['rim_colour', 'Rim']];

function litHTML() {
  if (!S.mod) return '';
  const s = litSettings(), on = !!s, cur = s || LIT_DEFAULTS, a = litAngles(cur), val = (k) => (k in a ? a[k] : cur[k]);
  const iso = S.rig && S.rig.retail && S.modsBase !== undefined;           // from the ISO: the first change makes the mod folder
  const ro = !iso && (!S.modPath || S.readOnly);
  return `<div class="sec"><h3>Lighting <small class="mono" title="Only the native build draws it">recomp</small></h3>
    <p class="note">Cel shading like Duellum's: a lit and a shadow tone with a crisp edge, a small highlight and a rim light, all following the camera, so a ripped model reads in 3D next to the painted retail cast. The native build draws it; a stock game keeps the flat look.</p>
    <label class="chk"><input type="checkbox" id="litOn"${on ? ' checked' : ''}${ro ? ' disabled' : ''}> Light this body in game (mod.json body.lighting)</label>
    ${on ? `${litBodyLit() ? '' : '<p class="note rt-note">The body shown is the game\'s own: the game lights a skin only. Pick a skin above to see it.</p>'}
    <div class="lit-presets" role="group" aria-label="Look">${Object.entries(LIT_PRESETS).map(([k, p]) => `<button type="button" class="mini lit-preset${cur.preset === k ? ' on' : ''}" data-litp="${k}" aria-pressed="${cur.preset === k}" title="${esc(p.tip)}"${ro ? ' disabled' : ''}>${esc(p.label)}</button>`).join('')}</div>
    ${(() => { const e = litEdited(cur); return e.length ? `<p class="note">${esc(LIT_PRESETS[cur.preset].label)}, changed: ${esc(e.join(', '))}. Pick the look again to undo the changes.</p>` : `<p class="note">${esc(LIT_PRESETS[cur.preset].tip)}.</p>`; })()}
    <div class="lit-rows">${LIT_SLIDERS.map(([k, lab, mn, mx, st, tip]) => `<div class="field lit-row" title="${esc(tip)}"><label>${esc(lab)}</label><input type="range" data-lit="${k}" min="${mn}" max="${mx}" step="${st}" value="${val(k)}"${ro ? ' disabled' : ''}><span class="mono" data-litv="${k}">${val(k)}</span></div>`).join('')}
    <div class="field lit-row"><label>Colours</label><span class="lit-cols">${LIT_COLOURS.map(([k, lab]) => `<label title="${esc(lab)}"><input type="color" data-litc="${k}" value="${esc(cur[k])}"${ro ? ' disabled' : ''}> ${esc(lab)}</label>`).join('')}</span></div></div>
    <div class="btnrow"><button type="button" id="litCompare" title="Hold to see the game's flat look">Hold: flat</button></div>` : ''}
    </div>`;
}
/* the settings to store: the preset (unless the default one) and only what differs from it (true when nothing does) */
function litStored(s) {
  const preset = s.preset && LIT_PRESETS[s.preset] ? s.preset : 'cel', base = litBase(preset), out = {};
  if (preset !== 'cel') out.preset = preset;
  for (const [k, v] of Object.entries(s)) if (k in LIT_DEFAULTS && JSON.stringify(v) !== JSON.stringify(base[k])) out[k] = v;
  return Object.keys(out).length ? out : true;
}
/* changed from its preset: the settings that differ (for the "edited" note) */
function litEdited(s) { const b = litBase(s.preset); return Object.keys(LIT_DEFAULTS).filter((k) => JSON.stringify(s[k]) !== JSON.stringify(b[k])); }
async function litCommit(s) {
  if (S.rig && S.rig.retail && typeof openRetailMod === 'function' && !(await openRetailMod())) return;
  edit([S.modPath], () => { S.mod.body = S.mod.body || {}; if (s) S.mod.body.lighting = litStored(s); else delete S.mod.body.lighting; });
  LIT.live = null; refreshStatus();
}
function litWire(I) {
  const on = I.querySelector('#litOn'); if (!on) return;
  on.onchange = async () => { await litCommit(on.checked ? Object.assign({}, LIT_DEFAULTS, { preset: 'cel' }) : null); renderInspector(); };
  const read = () => {                            // the panel's values -> settings
    const s = Object.assign({}, litSettings() || LIT_DEFAULTS), a = litAngles(s), moved = new Set();
    I.querySelectorAll('[data-lit]').forEach((r) => {
      const k = r.dataset.lit;
      if (r.value === r.dataset.v0) return;          // a slider snaps the stored value to its step: only a moved one counts
      if (k in a) { a[k] = +r.value; moved.add(k); } else s[k] = +r.value;
    });
    I.querySelectorAll('[data-litc]').forEach((c) => { s[c.dataset.litc] = c.value; });
    const d = litFromAngles(a);                    // directions change only when their sliders moved (keeps the stored ones)
    if (moved.has('around') || moved.has('height')) s.key_dir = d.key_dir;
    if (moved.has('side')) s.rim_dir = d.rim_dir;
    return s;
  };
  I.querySelectorAll('[data-lit]').forEach((r) => { r.dataset.v0 = r.value; });
  I.querySelectorAll('[data-lit], [data-litc]').forEach((el) => {
    el.oninput = () => {                          // live, without an undo step per pixel
      const s = read(); LIT.live = s;
      const k = el.dataset.lit, out = k && I.querySelector(`[data-litv="${k}"]`); if (out) out.textContent = el.value;
    };
    el.onchange = async () => { await litCommit(read()); I.querySelectorAll('[data-lit]').forEach((r) => { r.dataset.v0 = r.value; }); };
  });
  const cmp = I.querySelector('#litCompare');
  if (cmp) { const set = (v) => () => { LIT.compare = v; }; cmp.onpointerdown = set(true); cmp.onpointerup = cmp.onpointerleave = set(false); }
  I.querySelectorAll('[data-litp]').forEach((b) => b.onclick = async () => {
    await litCommit(Object.assign(litBase(b.dataset.litp), { preset: b.dataset.litp })); renderInspector();
  });
}
