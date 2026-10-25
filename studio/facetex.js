// Dissidia Move Studio: face expressions. The model's GMO "TexAnm" groups (eyes, mouth, ...) and the face_texture
// event (0x0214) that switches a group to another pattern while its range runs. Loaded after gmo.js.
'use strict';
/* GMO layout (model chunk > BlindData 0x0F "TexAnm" > one 0x0F chunk per group, named "<id>_<label>", e.g. "2_lip"):
   its 0x80F1 leaves are
     "target:<tex>" + pad4 + u32 x, y, w, h   the rectangle of the drawn texture the group repaints (pixels)
     "source:<tex>"                           the sheet of cells it copies from
     "default:<NAME>"                         the pattern shown when no event overrides it
     "<n>_<NAME>" + pad4 + u32 words          pattern n, a little program the game steps every tick (FUN_0886E5D0):
        word & 3 == 0: show cell (u = bits 2-6, v = bits 7-11, in 16 px) for lo..hi ticks
                       (lo = bits 12-21, hi = bits 22-31; random in [lo, hi) when they differ, e.g. blinking)
        otherwise:     jump back (word >> 2) words (loop)
   Texture names are matched on the base name (the GMO's own texture list says .tga where TexAnm says .tm2). */
const FT = { c: null, groups: [], key: null };

function ftStr(u8, p, end) { let s = ''; for (let i = p; i < end && u8[i]; i++) s += String.fromCharCode(u8[i]); return s; }
const ftBase = (s) => (s || '').replace(/^.*[\\/]/, '').replace(/\.[^.]*$/, '').toLowerCase();

function ftParse(c) {
  const u8 = c.u8, dv = c.dv;
  let root; try { root = parseChunks(u8, 16, u8.length)[0]; } catch (e) { return []; }
  const models = root.children.filter((m) => m.type === 3);
  const meshM = models.find((m) => m.children.some((x) => x.type === 0x05)) || models[models.length - 1];
  const ta = meshM && meshM.children.find((x) => x.type === 0x0F && x.name === 'TexAnm');
  if (!ta) return [];
  const texIx = (name) => c.textures.findIndex((t) => ftBase(t.file || t.name) === ftBase(name));
  const out = [];
  for (const g of ta.children.filter((x) => x.type === 0x0F)) {
    const id = parseInt(g.name, 10); if (!(id >= 0)) continue;
    const grp = { id, label: g.name.replace(/^\d+_?/, ''), target: -1, source: -1, rect: null, def: null, pats: new Map() };
    let defName = '';
    for (const l of g.children.filter((x) => x.type === 0x80F1)) {
      const end = l.p + l.plen, s = ftStr(u8, l.p, end), after = l.p + ((s.length + 1 + 3) & ~3);
      if (s.startsWith('target:')) {
        grp.target = texIx(s.slice(7));
        if (after + 16 <= end) grp.rect = { x: dv.getUint32(after, true), y: dv.getUint32(after + 4, true), w: dv.getUint32(after + 8, true), h: dv.getUint32(after + 12, true) };
      } else if (s.startsWith('source:')) grp.source = texIx(s.slice(7));
      else if (s.startsWith('default:')) defName = s.slice(8);
      else if (/^\d+_/.test(s)) {
        const words = []; for (let o = after; o + 4 <= end; o += 4) words.push(dv.getUint32(o, true));
        grp.pats.set(parseInt(s, 10), { name: s.replace(/^\d+_/, ''), words });
      }
    }
    for (const [n, p] of grp.pats) if (p.name === defName) grp.def = n;
    if (grp.def == null && grp.pats.size) grp.def = [...grp.pats.keys()][0];
    if (grp.target >= 0 && grp.source >= 0 && grp.rect && grp.pats.size) out.push(grp);
  }
  return out;
}

/* the cell a pattern shows `t` ticks after it started; the random durations use a fixed seed so scrubbing is stable */
function ftCell(words, t, seed) {
  let r = (seed * 2654435761) >>> 0;
  const rnd = () => { r = (Math.imul(r, 0x343FD) + 0x269EC3) >>> 0; return (r >>> 16) & 0x7FFF; };
  let i = 0, at = 0, jumps = 0, last = null;
  for (let guard = 0; guard < 4096; guard++) {
    const w = words[i]; if (w === undefined) return last;
    if (w & 3) { if (++jumps >= 10) return last; i -= w >>> 2; if (i < 0) return last; continue; }
    jumps = 0;
    const lo = (w >>> 12) & 0x3FF, hi = w >>> 22, dur = Math.max(1, lo === hi || hi < lo ? lo : lo + rnd() % (hi - lo));
    last = { u: ((w >>> 2) & 31) * 16, v: ((w >>> 7) & 31) * 16 };
    if (t < at + dur) return last;
    at += dur; i++;
  }
  return last;
}

function ftSetup(c, v) {
  FT.c = c; FT.tex = v.texObjs; FT.groups = ftParse(c); FT.src = new Map(); FT.shown = new Map();
  for (const g of FT.groups) if (!FT.src.has(g.source)) {
    let im = null; try { im = decodeGIM(c.textures[g.source].gim); } catch (e) { /* sheet missing */ }
    FT.src.set(g.source, im);
  }
}

/* the {group: [pattern, start tick]} the timeline asks for at tick T (later events win, like the game's tick order) */
function ftWanted(t, T) {
  const want = new Map(); if (!t || !t.ranged) return want;
  const ph = typeof stepPhase === 'function' ? stepPhase() : null;
  for (const e of t.ranged) {
    if (opCode(e.op) !== 0x0214) continue;
    if (typeof evMode === 'function' && !evMode(e)) continue;
    const p = (e.pad || 0) & 0xFF; if (p && ph != null && p !== ph) continue;
    if (!(e.start <= T && (e.end === FOREVER || T < e.end))) continue;
    const a = e.args || [];
    want.set(+(e.face_part ?? a[0]), [+(e.pattern ?? a[1]), e.start]);
  }
  return want;
}

function ftApply(c = S.gmo, v = view) {
  if (!c || !v || !v.texObjs) return;
  if (FT.c !== c || FT.tex !== v.texObjs) ftSetup(c, v);
  if (!FT.groups.length) return;
  const posed = S.gmo && !S.modelPose && S.motion >= 0;
  const T = posed ? ticks(S.frame) : 0, want = posed ? ftWanted(curTL(), T) : new Map();
  for (const g of FT.groups) {
    const w = want.get(g.id);
    const ov = w && g.pats.has(w[0]), pat = ov ? w[0] : g.def, t0 = ov ? w[1] : -64;   // the default runs all along: join it mid-cycle
    const cell = ftCell(g.pats.get(pat).words, T - t0, g.id * 977 + pat * 131 + t0);
    const tex = FT.tex[g.target], src = FT.src.get(g.source);
    if (!cell || !tex || !src) continue;
    const k = cell.u + ',' + cell.v, prev = FT.shown.get(g);
    if (prev === k) continue;
    FT.shown.set(g, k);
    const dst = tex.image, { x, y, w: rw, h: rh } = g.rect;
    for (let yy = 0; yy < rh; yy++) {
      const sy = cell.v + yy, dy = y + yy; if (sy >= src.h || dy >= dst.height) break;
      const n = Math.min(rw, src.w - cell.u, dst.width - x); if (n <= 0) break;
      dst.data.set(src.rgba.subarray((sy * src.w + cell.u) * 4, (sy * src.w + cell.u + n) * 4), (dy * dst.width + x) * 4);
    }
    tex.needsUpdate = true;
  }
}
/* for tests and the console: the groups of the loaded model, and what each shows now */
function ftInfo() { return FT.groups.map((g) => ({ id: g.id, label: g.label, rect: g.rect, def: g.def, pats: [...g.pats].map(([n, p]) => n + '_' + p.name), shown: FT.shown.get(g) })); }
