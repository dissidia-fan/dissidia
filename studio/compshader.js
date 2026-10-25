/* Move Studio: shader layers of an effect (comp.js). Loaded after comp.js.

   A shader layer is a normal layer (an effect of the package with one placement) whose pixels the native build draws
   with your own GLSL (psprecomp/FX_SHADERS.md). The runtime picks the draws by texture: every texture gets a
   texture-pack id (xxh64 of its texels and the palette entries they use), and a manifest entry with that id swaps
   the draw's fragment shader for yours. So:
     - each shader layer gets textures of its own: its part's textures are copied with one texel changed (invisible),
       so their ids are unique, and its meshes / flipbooks are pointed at the copies (compShaderOwn);
     - Build writes the shader to mods/<character>/files/fx_shaders/<effect>/<layer>.frag and one mod.json
       fx_shaders draw per layer (hash = those ids), which charmod.py merges into the game's manifest;
     - the PSP build, and the native build with the shaders off, draw the part's own look: keep it a sensible
       stand-in.
   The preview runs the same GLSL (WebGL 2) with the runtime's preamble, on the same draws, picked the same way. */
'use strict';

/* ------------------------------------------------------------------ texture-pack id (runtime psp_texpack_make_id) */
const XXH = (() => {
  const M = (1n << 64n) - 1n, P1 = 11400714785074694791n, P2 = 14029467366897019727n, P3 = 1609587929392839161n,
    P4 = 9650029242287828579n, P5 = 2870177450012600261n;
  const rotl = (x, r) => ((x << BigInt(r)) | (x >> BigInt(64 - r))) & M;
  const round = (acc, v) => (rotl((acc + v * P2) & M, 31) * P1) & M;
  const merge = (acc, v) => ((((acc ^ round(0n, v)) * P1) & M) + P4) & M;
  function xxh64(u8, seed) {
    seed = BigInt(seed) & M;
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), n = u8.length, q = (p) => dv.getBigUint64(p, true);
    let p = 0, h;
    if (n >= 32) {
      let v1 = (seed + P1 + P2) & M, v2 = (seed + P2) & M, v3 = seed, v4 = (seed - P1 + (1n << 64n)) & M;
      for (; p <= n - 32; p += 32) { v1 = round(v1, q(p)); v2 = round(v2, q(p + 8)); v3 = round(v3, q(p + 16)); v4 = round(v4, q(p + 24)); }
      h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M;
      h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else h = (seed + P5) & M;
    h = (h + BigInt(n)) & M;
    for (; p + 8 <= n; p += 8) { h ^= round(0n, q(p)); h = (rotl(h, 27) * P1 + P4) & M; }
    if (p + 4 <= n) { h ^= (BigInt(dv.getUint32(p, true)) * P1) & M; h = (rotl(h, 23) * P2 + P3) & M; p += 4; }
    for (; p < n; p++) { h ^= (BigInt(u8[p]) * P5) & M; h = (rotl(h, 11) * P1) & M; }
    h ^= h >> 33n; h = (h * P2) & M; h ^= h >> 29n; h = (h * P3) & M; h ^= h >> 32n;
    return h;
  }
  return { xxh64 };
})();
/* the texels (unswizzled; CLUT8 w*h bytes, CLUT4 w*h/2) and palette of an FEP 'TEX' blob */
function texIndexed(u8) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), fmt = dv.getUint32(16, true), sw = dv.getUint32(20, true), co = dv.getUint32(24, true);
  const w = dv.getUint32(28, true), h = dv.getUint32(32, true), po = dv.getUint32(36, true);
  const bits = fmt === 4 ? 8 : 4, wb = w * bits / 8, n = dv.getUint32(co, true);
  let px = u8.slice(po, po + wb * h);
  if (sw) {
    const out = new Uint8Array(wb * h), bw = wb / 16; let i = 0;
    for (let ty = 0; ty < h / 8; ty++) for (let tx = 0; tx < bw; tx++) for (let r = 0; r < 8; r++) { out.set(px.subarray(i, i + 16), (ty * 8 + r) * wb + tx * 16); i += 16; }
    px = out;
  }
  const pal = new Uint8Array(1024); pal.set(u8.subarray(co + 16, co + 16 + 4 * Math.min(256, n)));
  return { w, h, bits, px, pal };
}
/* -> '0123456789abcdef', the id FX_SHADERS.md manifests match (checked against the Alterna ids: 12 of 12) */
function texpackId(u8) {
  const t = texIndexed(u8), used = new Set();
  if (t.bits === 8) for (const b of t.px) used.add(b); else for (const b of t.px) { used.add(b & 15); used.add(b >> 4); }
  const ix = [...used].sort((a, b) => a - b), buf = new Uint8Array(5 * ix.length);
  ix.forEach((i, k) => { buf[5 * k] = i; buf.set(t.pal.subarray(4 * i, 4 * i + 4), 5 * k + 1); });
  const key = new DataView(new ArrayBuffer(40));
  [XXH.xxh64(t.px, 0), XXH.xxh64(buf, 1), BigInt(t.bits === 8 ? 5 : 4), BigInt(t.w), BigInt(t.h)].forEach((v, k) => key.setBigUint64(8 * k, v, true));
  return XXH.xxh64(new Uint8Array(key.buffer), 0x70737074).toString(16).padStart(16, '0');
}
const texBlobSize = (u8) => { const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), co = dv.getUint32(24, true); return co + 16 + 4 * dv.getUint32(co, true); };

/* ------------------------------------------------------------------ a part's resources */
/* every resource field of effect `ei`'s nodes: fn(kind, value, set(newValue)) (fxeImportNodeRes walks the same) */
function csLinks(fep, ei, fn) {
  for (const nd of fxeBodyNodes(fxeEffect(fep, ei).body)) {
    if (!nd || !nd.data[8]) continue;
    const tracks = fxeNodeTracks(fep, nd), sets = [...(nd.data[18] ? [nd.ptrs.get(32)] : []), ...fxeNodeSets(nd)];
    for (const ps of sets) {
      const r = fxePsRead(fep, ps, tracks); let ch = false;
      for (const en of r.entries) for (const [kd, fo, sz] of FXE_LINKS[tracks[en.track].ch] || []) {
        if (kd === 'VTL' || fo + sz > en.bytes.length) continue;
        const dv = fxeDV(en.bytes), v = sz === 4 ? dv.getInt32(fo, true) : dv.getInt16(fo, true); if (v < 0) continue;
        fn(kd, v, (nv) => { if (sz === 4) dv.setInt32(fo, nv, true); else dv.setInt16(fo, nv, true); ch = true; });
      }
      if (ch) fxePsWrite(ps, r.frame0, r.frame1, r.entries);
    }
  }
}
const csLocalRaw = (r) => (r && r.size ? r.data.data.subarray(0, r.size) : null);
function csResRaw(fep, kind, r) {                     // local bytes, or the resident (system) blob
  if (r.size) return csLocalRaw(r);
  const lib = FXR.resident && FXR.resident.get(r.id);
  if (!lib) return null;
  return kind === 'TEX' ? lib.u8.subarray(0, texBlobSize(lib.u8)) : lib.u8;
}
/* texture ids a part draws with: {ids: [...], system: bool (some of them are the game's shared ones)} */
function csTexIdsIn(fep, ei) {
  const ids = new Set(); let system = false;
  csLinks(fep, ei, (kind, v) => {
    const r = fxeRefs(fep, kind)[v]; if (!r) return;
    if (kind === 'TEX') { ids.add(r.id); if (!r.size) system = true; return; }
    const raw = csResRaw(fep, kind, r); if (!raw) return;
    if (!r.size) system = true;
    for (const t of fxeTexIdsOf(kind, raw)) ids.add(t);
  });
  for (const id of ids) { const t = fxeRefs(fep, 'TEX').find((x) => x.id === id); if (!t || !t.size) system = true; }
  return { ids: [...ids], system };
}
/* which effects use each resource: {MDL|ANM|TEX: Map(index -> Set(effect)), texId: Map(id -> Set(effect))} */
function csUsage(fep) {
  const u = { MDL: new Map(), ANM: new Map(), TEX: new Map(), texId: new Map() }, add = (m, k, e) => { if (!m.has(k)) m.set(k, new Set()); m.get(k).add(e); };
  fep.effectBlocks().forEach((_, ei) => csLinks(fep, ei, (kind, v) => {
    add(u[kind], v, ei);
    const r = fxeRefs(fep, kind)[v]; if (!r) return;
    if (kind === 'TEX') add(u.texId, r.id, ei);
    else { const raw = csResRaw(fep, kind, r); if (raw) for (const t of fxeTexIdsOf(kind, raw)) add(u.texId, t, ei); }
  }));
  return u;
}

/* one texel changed so the copy's id is its own (the look stays: an invisible pixel gets a new colour, or, in a
   full 256-colour palette, a pixel takes its neighbour's colour). Returns {bytes, hash}. */
function csUniqueTex(src, salt, avoid) {
  const t0 = decodeTex(src);
  let { w, h, rgba } = t0;
  if (w < 16 || h < 8) {                              // the encoder's swizzle needs 16 x 8 texel tiles: scale up (UVs are 0..1)
    const W = Math.max(16, w), H = Math.max(8, h), out = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) out.set(rgba.subarray(4 * ((Math.floor(y * h / H)) * w + Math.floor(x * w / W)), 4 * ((Math.floor(y * h / H)) * w + Math.floor(x * w / W)) + 4), 4 * (y * W + x));
    w = W; h = H; rgba = out;
  }
  const n = w * h, cols = new Set();
  for (let i = 0; i < n; i++) cols.add(((rgba[4 * i] << 24) | (rgba[4 * i + 1] << 16) | (rgba[4 * i + 2] << 8) | rgba[4 * i + 3]) >>> 0);
  let minA = 255; for (let i = 0; i < n; i++) minA = Math.min(minA, rgba[4 * i + 3]);
  for (let k = 0; k < 64; k++) {
    const px = Uint8Array.from(rgba), s = (salt * 7919 + k * 104729 + 17) >>> 0;
    if (cols.size < 256) {
      let p = s % n; while (rgba[4 * p + 3] !== minA) p = (p + 1) % n;      // the most transparent texel from a seeded start
      if (minA === 0) { px[4 * p] = (s >> 3) & 255; px[4 * p + 1] = (s >> 11) & 255; px[4 * p + 2] = 1 + ((s >> 19) & 127); }
      else px[4 * p] = px[4 * p] ^ (1 + (k & 3));
    } else {
      const p = s % n, q = (p + 1 + (k % (n - 1))) % n;
      px.set(rgba.subarray(4 * q, 4 * q + 4), 4 * p);
    }
    const bytes = fxeEncodeTex(px, w, h), hash = texpackId(bytes);
    if (!avoid.has(hash)) return { bytes, hash };
  }
  throw new Error('could not give the texture an id of its own');
}

/* give layer `l` textures of its own; returns their ids (also kept in l.shader.hashes by the caller) */
function compShaderOwn(l, avoidIn) {
  const ni = compPartNi(l); if (ni < 0) { toast('The layer\'s part is not in the package.'); return null; }
  const avoid = new Set(avoidIn || csTakenHashes(l));
  CS.lastErr = null; CS.stood = false;
  const res = fxuEdit((pkg) => { try { return csOwnIn(pkg, ni, avoid); } catch (e) { CS.lastErr = e.message; throw e; } }, true);
  if (res && CS.stood) toast('Some of this part\'s textures are the game\'s shared ones and the ISO isn\'t set, so a soft white glow stands in for them (what the PSP build draws).');
  return res;
}
function csOwnIn(pkg, ni, avoid) {
  const fep = pkg.fep, ei = fxeEffectOfName(fep, ni); if (ei < 0) throw new Error('no such effect');
  const use = csUsage(fep), mine = (set) => set && set.size === 1 && set.has(ei);
  const newTex = new Map(), hashes = new Set(); let salt = ni * 31 + 1;
  const texFor = (id) => {                           // old texture id ('glow': none, a stand-in) -> its own copy's id
    if (newTex.has(id)) return newTex.get(id);
    if ([...newTex.values()].includes(id)) return id;   // already one of this layer's own
    const r = id === 'glow' ? null : fxeRefs(fep, 'TEX').find((x) => x.id === id);
    if (id !== 'glow' && !r) throw new Error('missing texture ' + id.toString(16));
    let raw = r && csResRaw(fep, 'TEX', r);
    if (!raw) { raw = csGlowTex(); CS.stood = true; }   // one of the game's shared textures, not loaded (no ISO): a soft white glow stands in
    avoid.add(texpackId(raw));
    const u = csUniqueTex(raw, salt++, avoid); avoid.add(u.hash); hashes.add(u.hash);
    let nid;
    if (r && r.size && mine(use.texId.get(id))) { fxeSetResBytes(fep, 'TEX', r.i, u.bytes); nid = id; }   // only this part draws it: change it in place
    else { const ti = fxeAddRes(fep, 'TEX', u.bytes); nid = fxeRefs(fep, 'TEX')[ti].id; }   // (the table is read after it grows)
    newTex.set(id, nid); return nid;
  };
  const done = new Map();
  csLinks(fep, ei, (kind, v, set) => {
    const key = kind + v; if (done.has(key)) { if (done.get(key) !== v) set(done.get(key)); return; }
    const r = fxeRefs(fep, kind)[v]; if (!r) return;
    let nv = v;
    if (kind === 'TEX') {
      const nid = texFor(r.id);
      nv = nid === r.id ? v : fxeRefs(fep, 'TEX').findIndex((x) => x.id === nid);
    } else {
      let raw = csResRaw(fep, kind, r);
      if (!r.size) {                                   // one of the game's shared ones: this layer gets a copy of its own
        if (kind !== 'ANM') throw new Error('this part draws one of the game\'s shared meshes, which can\'t take a texture of its own: use another part for the shader');
        raw = raw ? csAnmBytes(raw) : null;
        if (!raw) { raw = fxeMakeAnm(texFor('glow'), 1, 1, 60, true, csAnmSize(fep), 'add'); CS.stood = true; }   // not loaded (no ISO): a one-picture flipbook
      }
      const m = new Map(); for (const t of fxeTexIdsOf(kind, raw)) m.set(t, texFor(t));
      const raw2 = fxeRetex(kind, raw, m);
      if (r.size && mine(use[kind].get(v))) fxeSetResBytes(fep, kind, v, raw2);
      else nv = fxeAddRes(fep, kind, raw2);
    }
    done.set(key, nv); if (nv !== v) set(nv);
  });
  fep.dirty();
  if (!hashes.size) throw new Error('this part draws nothing with a texture, so a shader can\'t be matched to it');
  return [...hashes];
}
/* a resident ANM's own bytes (the library blob runs on into the next one): up to the end of its last table */
function csAnmBytes(u8) {
  try {
    const dv = fxDV(u8), U = (o) => dv.getUint32(o, true);
    const [nTex, nClip, nSeq, nPart] = [0x20, 0x24, 0x28, 0x2C].map(U), [oTex, oClip, oSeq, oPart] = [0x30, 0x34, 0x38, 0x3C].map(U);
    let end = Math.max(0x50, oTex + 8 * nTex, oClip + 12 * nClip, oSeq + 12 * nSeq, oPart + 28 * nPart);
    for (let i = 0; i < nSeq; i++) end = Math.max(end, U(oSeq + 12 * i + 8) + 4 * U(oSeq + 12 * i + 4));
    for (let i = 0; i < nPart; i++) { const o = oPart + 28 * i; end = Math.max(end, U(o + 12) + 5 * U(o + 8)); }
    end = (end + 15) & ~15;
    return end <= u8.length && end < 1 << 20 ? Uint8Array.from(u8.subarray(0, end)) : null;
  } catch (e) { return null; }
}
function csAnmSize(fep) {                              // a flipbook size of this package (the stand-in's)
  const s = fxeRefs(fep, 'ANM').filter((r) => r.size).map((r) => fxDV(csLocalRaw(r)).getFloat32(0x40, true)).filter((x) => x > 0 && isFinite(x)).sort((a, b) => a - b);
  return s.length ? s[s.length >> 2] : 1;                                 // a smallish one
}
function csGlowTex() {                               // 64 x 64 white, alpha falling off from the centre
  const n = 64, rgba = new Uint8Array(n * n * 4);
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
    const r = Math.min(1, Math.hypot(x + 0.5 - n / 2, y + 0.5 - n / 2) / (n / 2)), i = 4 * (y * n + x);
    rgba[i] = rgba[i + 1] = rgba[i + 2] = 255; rgba[i + 3] = Math.round(255 * (1 - r) * (1 - r));
  }
  return fxeEncodeTex(rgba, n, n);
}
/* ids other shader layers (all effects of this character) and the mod's manifest already use */
function csTakenHashes(except) {
  const out = new Set();
  for (const p of S.comps || []) for (const l of S.files[p].obj.layers) if (l !== except && l.shader) for (const h of l.shader.hashes || []) out.add(h);
  const fx = S.mod && S.mod.fx_shaders, mine = S.comp ? compSlug(S.comp.path) + '/' : null;
  for (const d of (fx && fx.draws) || []) if (!mine || !String(d.name || '').startsWith(mine)) for (const h of [].concat(d.hash || [])) out.add(h);
  return out;
}
/* the ids a layer's textures have now ({hashes, texIds, system}); cached per package version */
const CS = { cache: null, bytes: null, progs: new Map(), texCache: new Map(), black: null };
function csNow(l) {
  if (!S.effe) return null;
  if (CS.bytes !== S.effe.bytes) { CS.bytes = S.effe.bytes; CS.cache = new Map(); }
  if (CS.cache.has(l.part)) return CS.cache.get(l.part);
  let out = null;
  try {
    const pkg = fxuPkg(), fep = pkg.fep, ni = fep.names().findIndex((n) => n.name === l.part), ei = ni >= 0 ? fxeEffectOfName(fep, ni) : -1;
    if (ei >= 0) {
      const t = csTexIdsIn(fep, ei), hashes = [];
      for (const id of t.ids) { const r = fxeRefs(fep, 'TEX').find((x) => x.id === id); const raw = r && csResRaw(fep, 'TEX', r); if (raw) hashes.push(texpackId(raw)); }
      out = { texIds: t.ids, hashes: [...new Set(hashes)], system: t.system };
    }
  } catch (e) { console.warn('studio: shader layer textures', e); }
  CS.cache.set(l.part, out);
  return out;
}

/* ------------------------------------------------------------------ templates */
const CS_BLENDS = [['game', 'As the effect blends'], ['add_one', 'Add (glow)'], ['add', 'Add, by alpha'], ['alpha', 'Alpha'], ['premul', 'Premultiplied alpha'], ['screen', 'Screen'], ['off', 'Opaque']];
const CS_DEPTHS = [['game', 'As the effect does'], ['nowrite', 'Test, don\'t write'], ['write', 'Test and write'], ['off', 'Always on top']];
const CS_TEMPLATES = [
  { id: 'glow', label: 'Glow orb (shader)', carrier: 'flipbook', note: 'a round glow with a pulsing core, on a camera-facing picture',
    blend: 'add_one', uniforms: { u_color: '#ff6a30', u_pulse: 3.0 },
    code: `// Glow orb: a soft round glow with a hot core that pulses.
// The effect's colour and fade keys still drive it (fx_tint, fx_env).
uniform vec3 u_color;     // colour of the glow
uniform float u_pulse;    // pulses per second

vec4 fx_shade() {
    vec2 p = v_texcoord * 2.0 - 1.0;          // -1..1 across the picture
    float r = length(p);
    float glow = exp(-r * r * 4.0) * (1.0 - smoothstep(0.7, 1.0, r));   // fades out before the picture's edge
    float core = 1.0 - smoothstep(0.0, 0.35, r);
    float beat = 0.85 + 0.15 * sin(u_fx_age_ticks / 60.0 * 6.2832 * u_pulse);
    vec3 rgb = (u_color * glow + vec3(core)) * beat * fx_tint();
    return vec4(rgb * fx_env(), 0.0);         // additive: alpha 0 leaves the screen's alpha alone
}
` },
  { id: 'rim', label: 'Glowing rim (shader)', carrier: 'mesh', note: 'a see-through shell that glows at its edges',
    blend: 'add_one', uniforms: { u_color: '#7fd0ff', u_power: 2.5 },
    code: `// Glowing rim: bright where the mesh turns away from the camera (fresnel), clear in the middle.
uniform vec3 u_color;     // colour of the rim
uniform float u_power;    // 1 = a wide rim, 4 = a thin one

vec4 fx_shade() {
    float rim = pow(fx_rim(), u_power);
    vec3 rgb = u_color * rim * 2.0 * fx_tint();
    return vec4(rgb * fx_env(), 0.0);
}
` },
  { id: 'dissolve', label: 'Dissolve in and out (shader)', carrier: 'flipbook', note: 'the part\'s own picture grows in and breaks up, with a glowing edge',
    blend: 'game', uniforms: { u_edge: 0.08, u_edge_color: '#ffd080' },
    code: `// Dissolve: the part's own look, appearing and breaking up through noise over the layer's length.
uniform float u_edge;          // width of the glowing edge
uniform vec3 u_edge_color;     // colour of the edge

float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), f.x), mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), f.x), f.y);
}

vec4 fx_shade() {
    vec4 c = fx_ge_color();                   // what the game would draw
    float n = noise(v_texcoord * 8.0) * 0.6 + noise(v_texcoord * 23.0) * 0.4;
    float t = u_fx_progress;                  // 0 when the layer starts, 1 when it ends
    float cut = t < 0.5 ? 1.0 - t * 2.0 : (t - 0.5) * 2.0;
    if (n < cut) discard;
    float e = 1.0 - smoothstep(cut, cut + u_edge, n);
    c.rgb = mix(c.rgb, u_edge_color, e);
    return c;
}
` },
  { id: 'flow', label: 'Flowing energy (shader)', carrier: 'mesh', note: 'bands of light running along a mesh',
    blend: 'add_one', uniforms: { u_color: '#a070ff', u_speed: 1.5, u_bands: 6.0 },
    code: `// Flowing energy: bright bands running along the mesh's UVs over its own texture.
uniform vec3 u_color;     // colour of the bands
uniform float u_speed;    // bands per second
uniform float u_bands;    // how many along the mesh

vec4 fx_shade() {
    vec2 uv = fx_uv();
    float s = sin((uv.y * u_bands - u_fx_age_ticks / 60.0 * u_speed) * 6.2832) * 0.5 + 0.5;
    vec4 t = texture(u_texture, v_texcoord);
    vec3 rgb = (t.rgb * 0.6 + u_color * pow(s, 6.0)) * fx_tint();
    return vec4(rgb * t.a * fx_env(), 0.0);
}
` },
  { id: 'blank', label: 'Blank shader', carrier: 'flipbook', note: 'starts as the game\'s own look, to edit',
    blend: 'game', uniforms: {},
    code: `// fx_shade() returns this pixel's colour. fx_ge_color() is what the game would draw;
// see FX_SHADERS.md for everything the preamble gives you (fx_tint, fx_env, fx_rim, u_fx_progress...).
vec4 fx_shade() {
    return fx_ge_color();
}
` },
];
function compShaderTemplates() {
  return CS_TEMPLATES.map((t) => ({ label: t.label, name: t.id, carrier: t.carrier, note: t.note, make: () => ({ template: t.id, code: t.code, blend: t.blend, depth: 'game', uniforms: { ...t.uniforms }, textures: {} }) }));
}
function compShaderAdded(l) {
  const hashes = compShaderOwn(l);
  if (!hashes) { compEdit(() => { delete l.shader; }); toast('The layer has no shader: ' + (CS.lastErr || 'its part can\'t have textures of its own') + '.'); return; }
  compEdit(() => { l.shader.hashes = hashes; });
}

/* ------------------------------------------------------------------ inspector */
const csName = (o, l) => compSlug(S.comp ? S.comp.path : '') + '/' + compCode(l.name).toLowerCase();
function csUniText(v) { return Array.isArray(v) ? v.join(' ') : String(v); }
function csUniParse(s) {
  s = s.trim();
  if (/^#[0-9a-f]{6}$/i.test(s)) return s.toLowerCase();
  const n = s.split(/[\s,]+/).filter(Boolean).map(Number);
  if (!n.length || n.some((x) => !isFinite(x))) return null;
  return n.length === 1 ? n[0] : n.length <= 4 ? n : null;
}
function compShaderHTML(l) {
  if (!l.shader) return `<div class="cs-box"><div class="btnrow"><button type="button" id="csGive" title="Draw this layer with your own GLSL in the native build">Give it a shader…</button></div></div>`;
  const sh = l.shader, st = csStatus(l), now = csNow(l), texNames = Object.keys((S.mod && S.mod.fx_shaders && S.mod.fx_shaders.textures) || {});
  let h = `<div class="cs-box"><h4>Shader <small>native build only</small></h4>
    <p class="note">The native build draws this layer with the GLSL below; the PSP build draws the part's own look. <span class="mono">fx_shade()</span> returns the pixel's colour. The preview runs it too.</p>
    <div class="cs-status ${st.ok ? 'ok' : 'bad'}">${st.ok ? '✓ Compiles' + (st.note ? ' · ' + esc(st.note) : '') : '<b>Doesn\'t compile</b> (the game would draw the part\'s own look):<pre>' + esc(st.err) + '</pre>'}</div>
    <textarea id="csCode" class="cs-code mono" spellcheck="false" rows="16">${esc(sh.code)}</textarea>
    <div class="row2"><div class="field"><label>Blend</label><select id="csBlend">${CS_BLENDS.map(([k, n]) => `<option value="${k}"${(sh.blend || 'game') === k ? ' selected' : ''}>${esc(n)}</option>`).join('')}</select></div>
      <div class="field"><label>Depth</label><select id="csDepth">${CS_DEPTHS.map(([k, n]) => `<option value="${k}"${(sh.depth || 'game') === k ? ' selected' : ''}>${esc(n)}</option>`).join('')}</select></div></div>
    <div class="lbl">Values <small>uniforms: a number, 2–4 numbers, or #rrggbb</small></div>
    <div class="cs-rows">${Object.entries(sh.uniforms || {}).map(([k, v]) => `<div class="cs-row"><span class="mono">${esc(k)}</span>${/^#/.test(String(v)) ? `<input type="color" data-ucol="${esc(k)}" value="${esc(v)}">` : ''}<input type="text" data-uni="${esc(k)}" value="${esc(csUniText(v))}"><button type="button" data-udel="${esc(k)}" title="Remove">✕</button></div>`).join('')}</div>
    <div class="btnrow"><button type="button" id="csUniAdd">+ Value</button></div>
    <div class="lbl">Textures <small>extra pictures for samplers</small></div>
    <div class="cs-rows">${Object.entries(sh.textures || {}).map(([k, v]) => `<div class="cs-row"><span class="mono">${esc(k)}</span><select data-tex="${esc(k)}">${texNames.map((n) => `<option${n === v ? ' selected' : ''}>${esc(n)}</option>`).join('')}${texNames.includes(v) ? '' : `<option selected>${esc(v)}</option>`}</select><button type="button" data-tdel="${esc(k)}" title="Remove">✕</button></div>`).join('')}</div>
    <div class="btnrow"><button type="button" id="csTexAdd">+ Texture</button><button type="button" id="csPng" title="Add a PNG to this character's shader textures">Add a PNG…</button></div>
    <p class="note" title="The game finds this layer's draws by these texture ids">Matched by <span class="mono">${now && now.hashes.length ? esc(now.hashes.join(' ')) : 'no texture ids yet'}</span>${now && sh.hashes && now.hashes.some((x) => !sh.hashes.includes(x)) ? ' · <b>changed since the layer got its textures</b>' : ''}</p>
    <div class="btnrow"><button type="button" id="csApply" class="primary" title="Ctrl+Enter in the code">Apply</button><button type="button" id="csOwn" title="Copy this part's textures again so its ids are its own">New texture ids</button><button type="button" id="csRemove" class="danger">Remove the shader</button></div></div>`;
  return h;
}
function compShaderWire(l) {
  if (!l.shader) {
    $('csGive').onclick = (e) => fxuMenu(e.currentTarget, 'Start from…', compShaderTemplates(), (t) => {
      compEdit(() => { l.shader = t.make(); });
      compShaderAdded(l);
    });
    return;
  }
  const sh = l.shader, code = $('csCode');
  const apply = () => { if (code.value !== sh.code) compEdit(() => { sh.code = code.value; }); else renderInspector(); };
  code.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); apply(); }
    else if (e.key === 'Tab') { e.preventDefault(); const a = code.selectionStart; code.setRangeText('    ', a, code.selectionEnd, 'end'); }
    e.stopPropagation();                             // typing here is not a shortcut
  });
  code.onchange = apply;
  $('csApply').onclick = apply;
  $('csBlend').onchange = (e) => compEdit(() => { sh.blend = e.target.value; });
  $('csDepth').onchange = (e) => compEdit(() => { sh.depth = e.target.value; });
  document.querySelectorAll('[data-uni]').forEach((el) => el.onchange = () => {
    const v = csUniParse(el.value); if (v == null) { toast('A number, 2–4 numbers or #rrggbb.'); return; }
    compEdit(() => { sh.uniforms[el.dataset.uni] = v; });
  });
  document.querySelectorAll('[data-ucol]').forEach((el) => el.oninput = () => { const t = document.querySelector(`[data-uni="${el.dataset.ucol}"]`); if (t) t.value = el.value; });
  document.querySelectorAll('[data-ucol]').forEach((el) => el.onchange = () => compEdit(() => { sh.uniforms[el.dataset.ucol] = el.value; }));
  document.querySelectorAll('[data-udel]').forEach((el) => el.onclick = () => compEdit(() => { delete sh.uniforms[el.dataset.udel]; }));
  $('csUniAdd').onclick = async () => {
    const r = await uiDialog({ title: 'Add a uniform', ok: 'Add', fields: [
      { label: 'Name', value: 'u_', hint: 'As the shader declares it, e.g. u_speed', check: (x) => (/^[A-Za-z_]\w*$/.test(x.trim()) ? '' : 'A name is letters, digits and _, not starting with a digit.') },
      { label: 'Value', value: '1', hint: 'A number, 2–4 numbers, or #rrggbb', check: (x) => (csUniParse(x) == null ? 'Use a number, 2–4 numbers or a #rrggbb colour.' : '') }] });
    if (!r) return;
    compEdit(() => { sh.uniforms = sh.uniforms || {}; sh.uniforms[r[0].trim()] = csUniParse(r[1]); });
  };
  document.querySelectorAll('[data-tex]').forEach((el) => el.onchange = () => compEdit(() => { sh.textures[el.dataset.tex] = el.value; }));
  document.querySelectorAll('[data-tdel]').forEach((el) => el.onclick = () => compEdit(() => { delete sh.textures[el.dataset.tdel]; }));
  $('csTexAdd').onclick = () => {
    const names = Object.keys((S.mod && S.mod.fx_shaders && S.mod.fx_shaders.textures) || {});
    if (!names.length) { toast('Add a PNG first: it becomes one of this character\'s shader textures.'); return; }
    uiDialog({ title: 'Add a texture', ok: 'Add', fields: [
      { label: 'Sampler name', value: 'u_tex', hint: 'As the shader declares it: uniform sampler2D u_…', check: (x) => (/^[A-Za-z_]\w*$/.test(x.trim()) ? '' : 'A name is letters, digits and _, not starting with a digit.') },
      { label: 'Texture', value: names[0], options: names.map((x) => [x, x]) }] })
      .then((r) => { if (r) compEdit(() => { sh.textures = sh.textures || {}; sh.textures[r[0].trim()] = r[1]; }); });
  };
  $('csPng').onclick = csAddPng;
  $('csOwn').onclick = () => { const hs = compShaderOwn(l); if (hs) compEdit(() => { sh.hashes = hs; }); };
  $('csRemove').onclick = async () => { if (await uiConfirm('Remove the shader?', 'The layer keeps its part and draws its own look again.', { ok: 'Remove', danger: true })) compEdit(() => { delete l.shader; }); };
}
/* a PNG into mods/<character>/files/fx_shaders/tex/, named in mod.json fx_shaders.textures */
function csAddPng() {
  if (S.readOnly) { readOnlyNote(); return; }
  if (!S.modPath) { toast('This character has no mod.json to name the texture in.'); return; }
  const inp = document.createElement('input'); inp.type = 'file'; inp.accept = 'image/png';
  inp.onchange = async () => {
    const f = inp.files[0]; if (!f) return;
    const stem = f.name.replace(/\.png$/i, '').toLowerCase().replace(/[^a-z0-9_]+/g, '_') || 'tex';
    const key = S.char.toLowerCase().replace(/[^a-z0-9]+/g, '_') + '_' + stem, rel = 'tex/' + key + '.png', p = S.charDir + 'files/fx_shaders/' + rel;
    const bin = new Uint8Array(await f.arrayBuffer());
    if (!S.files[p]) S.files[p] = { bin: new Uint8Array(0), binOrig: null, dirty: false, created: true };
    edit([S.modPath, p], () => {
      S.files[p].bin = bin;
      const m = S.files[S.modPath].obj;
      m.fx_shaders = m.fx_shaders || { textures: {}, draws: [] };
      m.fx_shaders.textures = { ...(m.fx_shaders.textures || {}), [key]: rel };
      if (m.files && !Array.isArray(m.files)) m.files['fx_shaders/' + rel] = 'files/fx_shaders/' + rel;
    });
    S.mod = S.files[S.modPath].obj; CS.texCache.delete(key);
    renderInspector(); toast(`${key}: pick it for a sampler with "+ Texture". Save writes the PNG into the character's folder.`);
  };
  inp.click();
}

/* ------------------------------------------------------------------ preview: the runtime's preamble, in WebGL 2 */
const CS_VS = `precision highp float;
in vec3 position; in vec2 uv; in vec4 fxcol;
uniform mat4 modelMatrix; uniform mat4 viewMatrix; uniform mat4 projectionMatrix;
uniform float bias; uniform float screen; uniform vec2 uvOff; uniform float psize;
uniform vec3 C; uniform float A2; uniform float lit2;
out vec2 v_texcoord; out vec4 v_color; out vec3 v_fx_opos; out vec3 v_fx_wpos; out vec3 v_fx_wnormal; out vec2 v_fx_uv;
void main() {
  v_texcoord = uv + uvOff; v_fx_uv = uv; v_fx_opos = position; v_fx_wnormal = vec3(0.0);
  v_color = vec4(min(vec3(1.0), lit2 * C * fxcol.rgb), min(1.0, A2 * fxcol.a));   // GE lighting: what FX_FS calls lit
  gl_PointSize = psize;
  vec4 wp = modelMatrix * vec4(position, 1.0); v_fx_wpos = wp.xyz;
  if (screen > 0.5) gl_Position = vec4(position.xy, 0.0, 1.0);
  else { gl_Position = projectionMatrix * viewMatrix * wp; gl_Position.z += bias * gl_Position.w; }
}`;
const CS_PRE = `precision highp float; precision highp int;
in vec2 v_texcoord; in vec4 v_color; in vec3 v_fx_opos; in vec3 v_fx_wpos; in vec3 v_fx_wnormal; in vec2 v_fx_uv;
uniform sampler2D u_texture; uniform bool u_texture_enable; uniform int u_tex_func; uniform bool u_tex_rgba; uniform bool u_tex_double; uniform vec3 u_tex_env; uniform bool u_alpha_double;
uniform float u_fx_time; uniform float u_fx_ticks; uniform float u_fx_age; uniform float u_fx_age_ticks; uniform float u_fx_life_ticks; uniform float u_fx_progress;
uniform mat4 u_fx_world; uniform mat4 u_fx_view; uniform mat4 u_fx_proj; uniform vec3 u_fx_camera; uniform vec3 u_fx_origin;
uniform vec2 u_fx_screen_size; uniform vec4 u_fx_viewport; uniform int u_fx_has_world; uniform int u_fx_has_normal; uniform sampler2D u_fx_screen; uniform float u_fx_env_scale;
uniform int cs_mode;
out vec4 cs_out;
#define texture2D texture
vec4 fx_ge_color() {
    vec4 color = v_color;
    if (u_texture_enable) {
        vec4 tex = texture(u_texture, v_texcoord);
        color.rgb *= tex.rgb; if (u_tex_rgba) color.a *= tex.a;
        if (u_tex_double) color.rgb = min(color.rgb * 2.0, vec3(1.0));
    }
    return color;
}
float fx_env() { return clamp(v_color.a * u_fx_env_scale, 0.0, 1.0); }
vec3 fx_tint() { return u_tex_double ? v_color.rgb * 2.0 : v_color.rgb; }
vec2 fx_uv() { return u_fx_has_world != 0 ? v_fx_uv : v_texcoord; }
vec3 fx_view_dir() { return u_fx_has_world != 0 ? normalize(u_fx_camera - v_fx_wpos) : vec3(0.0, 0.0, 1.0); }
vec3 fx_normal() {
    if (u_fx_has_normal != 0 && dot(v_fx_wnormal, v_fx_wnormal) > 1e-12) return normalize(v_fx_wnormal);
    if (u_fx_has_world != 0) { vec3 d = v_fx_wpos - u_fx_origin; if (dot(d, d) > 1e-12) return normalize(d); }
    return vec3(0.0, 0.0, 1.0);
}
float fx_rim() { return 1.0 - abs(dot(fx_normal(), fx_view_dir())); }
vec2 fx_screen_uv() { return gl_FragCoord.xy / u_fx_screen_size; }
float fx_saturate(float x) { return clamp(x, 0.0, 1.0); }
float fx_ease(float t) { t = clamp(t, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
`;
// the Studio draws premultiplied (source factor ONE): each blend of the manifest as that
const CS_POST = `
void main() {
    vec4 c = fx_shade();
    if (u_alpha_double) c.a = min(c.a * 2.0, 1.0);
    if (cs_mode == 0) { float f = clamp(c.a, 0.0, 1.0); cs_out = vec4(c.rgb * f, f); }               // game (factor 2·As folded in)
    else if (cs_mode == 1 || cs_mode == 5 || cs_mode == 6) cs_out = vec4(c.rgb, c.a);                // add_one / premul / screen
    else if (cs_mode == 2) cs_out = vec4(c.rgb * c.a, 0.0);                                          // add
    else if (cs_mode == 3) cs_out = vec4(c.rgb * c.a, c.a);                                          // alpha
    else cs_out = vec4(c.rgb, 1.0);                                                                  // off
}`;
const CS_MODES = { game: 0, add_one: 1, add: 2, alpha: 3, off: 4, premul: 5, screen: 6 };
function csFragSource(code) { return CS_PRE + '#line 1\n' + code + '\n' + CS_POST; }
/* compile check with the viewport's own GL (WebGL 2), lines reported in the user's code */
function csCompile(code) {
  if (CS.progs.has(code)) return CS.progs.get(code);
  let res;
  const R = typeof view !== 'undefined' && view.renderer, gl = R && R.getContext();
  if (!gl || !(R.capabilities && R.capabilities.isWebGL2)) res = { ok: true, preview: false, note: 'no WebGL 2 here: not previewed' };
  else {
    const sh = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(sh, '#version 300 es\n' + csFragSource(code)); gl.compileShader(sh);
    const ok = gl.getShaderParameter(sh, gl.COMPILE_STATUS), log = gl.getShaderInfoLog(sh) || '';
    gl.deleteShader(sh);
    res = ok ? { ok: true, preview: true } : { ok: false, err: log.replace(/ERROR: 0:(\d+)/g, (_, n) => 'line ' + n).trim() };
    if (ok && /\bgl_FragColor\b/.test(code)) res.note = 'return the colour instead of writing gl_FragColor';
  }
  if (CS.progs.size > 50) CS.progs.clear();
  CS.progs.set(code, res);
  return res;
}
function csStatus(l) { return csCompile(l.shader.code); }
function csColor(v) {
  if (typeof v === 'string' && /^#/.test(v)) { const n = parseInt(v.slice(1), 16); return new THREE.Vector3(((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255); }
  if (Array.isArray(v)) return v.length === 2 ? new THREE.Vector2(...v) : v.length === 3 ? new THREE.Vector3(...v) : new THREE.Vector4(...v);
  return v;
}
function csTexture(key) {                           // a named PNG of the mod (fx_shaders.textures), loaded once
  if (CS.texCache.has(key)) return CS.texCache.get(key);
  const tx = new THREE.Texture(); tx.flipY = false; tx.wrapS = tx.wrapT = THREE.RepeatWrapping; tx.minFilter = THREE.LinearMipmapLinearFilter;
  CS.texCache.set(key, tx);
  const rel = S.mod && S.mod.fx_shaders && S.mod.fx_shaders.textures && S.mod.fx_shaders.textures[key];
  const p = rel && S.charDir + 'files/fx_shaders/' + (typeof rel === 'string' ? rel : rel.file);
  (async () => {
    try {
      const live = S.files[p] && S.files[p].bin, bin = live && live.length ? live : await FS.read(p);
      tx.image = await createImageBitmap(new Blob([bin], { type: 'image/png' }), { imageOrientation: 'none', premultiplyAlpha: 'none' });
      tx.needsUpdate = true;
    } catch (e) { console.warn('studio: shader texture', key, e); }
  })();
  return tx;
}
function csMaterial(l, base) {
  const u = base.uniforms, sh = l.shader;
  const m = new THREE.RawShaderMaterial({ glslVersion: THREE.GLSL3, vertexShader: CS_VS, fragmentShader: csFragSource(sh.code),
    uniforms: { map: u.map, C: u.C, A2: u.A2, lit2: u.lit2, bias: u.bias, screen: u.screen, uvOff: u.uvOff, psize: u.psize,
      u_texture: { value: null }, u_texture_enable: { value: true }, u_tex_func: { value: 0 }, u_tex_rgba: { value: true }, u_tex_double: { value: true },
      u_tex_env: { value: new THREE.Vector3() }, u_alpha_double: { value: false }, u_fx_time: { value: 0 }, u_fx_ticks: { value: 0 }, u_fx_age: { value: 0 },
      u_fx_age_ticks: { value: 0 }, u_fx_life_ticks: { value: 0 }, u_fx_progress: { value: 0 }, u_fx_world: { value: new THREE.Matrix4() },
      u_fx_view: { value: new THREE.Matrix4() }, u_fx_proj: { value: new THREE.Matrix4() }, u_fx_camera: { value: new THREE.Vector3() },
      u_fx_origin: { value: new THREE.Vector3() }, u_fx_screen_size: { value: new THREE.Vector2(1, 1) }, u_fx_viewport: { value: new THREE.Vector4() },
      u_fx_has_world: { value: 1 }, u_fx_has_normal: { value: 0 }, u_fx_screen: { value: null }, u_fx_env_scale: { value: 1 },
      cs_mode: { value: 0 } },
    transparent: true, side: THREE.DoubleSide });
  m.csKey = sh.code;
  return m;
}
/* fx.js calls this after drawing: the draws whose texture is a shader layer's take its material */
function compShaderPass(ctx, tick) {
  if (!S.comps || !S.comps.length || !S.fx || !ctx.fx || ctx.fx !== S.fx) return;
  const byTex = new Map();
  for (const p of S.comps) {
    const o = S.files[p] && S.files[p].obj; if (!o) continue;
    for (const l of o.layers) {
      if (!l.shader || l.hidden) continue;
      const st = csCompile(l.shader.code); if (!st.ok || !st.preview) continue;
      const now = csNow(l); if (!now) continue;
      for (const id of now.texIds) { const tx = fxTexture(S.fx.fep, id); if (tx) byTex.set(tx, { o, l }); }
    }
  }
  if (!byTex.size) return;
  const cam = ctx.cam, size = ctx.view.renderer.getDrawingBufferSize(new THREE.Vector2());
  for (const k of FXV.used) {
    const m = FXV.pool.get(k); if (!m || !m.visible) continue;
    const base = m.fxBase || m.material, bu = base.uniforms, hit = bu && bu.map && bu.map.value && byTex.get(bu.map.value);
    if (!hit) continue;
    const { o, l } = hit, sh = l.shader;
    m.fxBase = base;
    if (!m.fxOver || m.fxOver.csKey !== sh.code || m.fxOver.csLayer !== l) { if (m.fxOver) m.fxOver.dispose(); m.fxOver = csMaterial(l, base); m.fxOver.csLayer = l; }
    const M = m.fxOver, U = M.uniforms, mode = CS_MODES[sh.blend || 'game'] || 0, a2 = (bu.aMul && bu.aMul.value) === 2;
    for (const n of ['map', 'C', 'A2', 'lit2', 'bias', 'screen', 'uvOff', 'psize']) U[n] = bu[n];
    U.u_texture.value = bu.map.value; U.u_texture_enable.value = bu.useMap.value > 0.5;
    U.cs_mode.value = mode; U.u_alpha_double.value = mode === 0 && a2; U.u_fx_env_scale.value = mode !== 0 && a2 ? 2 : 1;
    // age: the layer's start in the animation it plays in (or the effect editor's own clock)
    const start = !ctx.opts.solo && compMotion(o) === S.motion ? Math.round((o.plays.frame + l.start) * 2) : 0;
    const age = Math.max(0, tick - start), life = Math.round(l.life * 2);
    U.u_fx_age_ticks.value = age; U.u_fx_age.value = age / 60; U.u_fx_ticks.value = tick; U.u_fx_time.value = tick / 60;
    U.u_fx_life_ticks.value = life; U.u_fx_progress.value = life > 0 ? Math.min(1, age / life) : 0;
    U.u_fx_world.value.copy(m.matrixWorld); U.u_fx_view.value.copy(cam.matrixWorldInverse); U.u_fx_proj.value.copy(cam.projectionMatrix);
    U.u_fx_camera.value.setFromMatrixPosition(cam.matrixWorld); U.u_fx_origin.value.setFromMatrixPosition(m.matrixWorld);
    U.u_fx_screen_size.value.copy(size); U.u_fx_viewport.value.set(0, 0, size.x, size.y); U.u_fx_has_world.value = bu.screen.value > 0.5 ? 0 : 1;
    if (!CS.black) { CS.black = new THREE.DataTexture(new Uint8Array(4), 1, 1, THREE.RGBAFormat); CS.black.needsUpdate = true; }
    U.u_fx_screen.value = CS.black;
    for (const [k2, v] of Object.entries(sh.uniforms || {})) { if (!U[k2]) U[k2] = { value: null }; U[k2].value = csColor(v); }
    for (const [k2, v] of Object.entries(sh.textures || {})) { if (!U[k2]) U[k2] = { value: null }; U[k2].value = csTexture(v); }
    // blending: 'game' keeps the effect's; the others as the manifest's
    M.blending = THREE.CustomBlending; M.blendEquationAlpha = THREE.AddEquation; M.blendSrcAlpha = THREE.ZeroFactor; M.blendDstAlpha = THREE.OneFactor;
    M.blendSrc = THREE.OneFactor; M.blendEquation = mode === 0 ? base.blendEquation : THREE.AddEquation;
    M.blendDst = mode === 0 ? base.blendDst : mode === 1 || mode === 2 ? THREE.OneFactor : mode === 6 ? THREE.OneMinusSrcColorFactor : THREE.OneMinusSrcAlphaFactor;
    if (mode === 4) M.blending = THREE.NoBlending;
    const dp = sh.depth || 'game';
    M.depthTest = dp === 'off' ? false : base.depthTest; M.depthWrite = dp === 'write' ? true : dp === 'game' ? base.depthWrite : false;
    M.depthFunc = base.depthFunc; M.side = base.side;
    m.material = M;
  }
}

/* ------------------------------------------------------------------ build: .frag files and mod.json fx_shaders */
function compShaderBuild(o) {
  if (!S.modPath) { toast('This character has no mod.json, so its shaders can\'t be listed for the game.'); return false; }
  const slug = compSlug(S.comp.path), lays = o.layers.filter((l) => l.shader && !l.hidden);
  for (const l of lays) { const st = csCompile(l.shader.code); if (!st.ok) { toast(`${l.name}: the shader doesn't compile. Fix it first (the inspector shows where).`); return false; } }
  // every layer's ids must be its own (a duplicate layer, or a part that shares textures, gets new ones)
  const seen = new Set(), others = new Set();
  for (const p of S.comps) if (p !== S.comp.path) for (const l of S.files[p].obj.layers) if (l.shader) for (const h of (csNow(l) || {}).hashes || []) others.add(h);
  for (const l of lays) {
    let now = csNow(l), shared = false;
    if (now && !now.system) {
      const fep = fxuPkg().fep, use = csUsage(fep), ei = fxeEffectOfName(fep, compPartNi(l));
      shared = now.texIds.some((id) => { const s = use.texId.get(id); return s && [...s].some((e) => e !== ei); });
    }
    if (!now || now.system || shared || !now.hashes.length || now.hashes.some((h) => seen.has(h) || others.has(h))) {
      const hs = compShaderOwn(l, new Set([...seen, ...others])); if (!hs) return false;
      l.shader.hashes = hs; now = csNow(l);
    }
    for (const h of now.hashes) seen.add(h);
  }
  const files = [], drawOf = (l) => {
    const lc = compCode(l.name).toLowerCase(), rel = `${slug}/${lc}.frag`, sh = l.shader, now = csNow(l);
    files.push([S.charDir + 'files/fx_shaders/' + rel, rel, sh.code]);
    const d = { name: `${slug}/${lc}`, _note: `Move Studio effect "${o.name}", layer "${l.name}" (effects/${slug}.json). Built from there: edit it in the Studio.`,
      hash: now.hashes.length === 1 ? now.hashes[0] : now.hashes, frag: rel, blend: sh.blend || 'game', depth: sh.depth || 'game',
      life_ticks: Math.round(l.life * 2), age_offset_ticks: 0 };
    if (Object.keys(sh.uniforms || {}).length) d.uniforms = { ...sh.uniforms };
    if (Object.keys(sh.textures || {}).length) d.textures = { ...sh.textures };
    return d;
  };
  const draws = lays.map(drawOf);
  for (const [p] of files) if (!S.files[p]) S.files[p] = { bin: new Uint8Array(0), binOrig: null, dirty: false, created: true };
  edit([S.modPath, S.comp.path, ...files.map(([p]) => p)], () => {
    for (const [p, , code] of files) S.files[p].bin = new TextEncoder().encode(code);
    const m = S.files[S.modPath].obj;
    m.fx_shaders = m.fx_shaders || {};
    m.fx_shaders.textures = m.fx_shaders.textures || {};
    m.fx_shaders.draws = (m.fx_shaders.draws || []).filter((d) => !String(d.name || '').startsWith(slug + '/')).concat(draws);
    if (!m.fx_shaders.draws.length) delete m.fx_shaders;
    if (!Array.isArray(m.files)) {
      m.files = m.files || {};
      for (const k of Object.keys(m.files)) if (k.startsWith(`fx_shaders/${slug}/`)) delete m.files[k];
      for (const [, rel] of files) m.files['fx_shaders/' + rel] = 'files/fx_shaders/' + rel;
    }
    for (const l of lays) l.shader.hashes = csNow(l).hashes;
  });
  S.mod = S.files[S.modPath].obj;
  return true;
}
