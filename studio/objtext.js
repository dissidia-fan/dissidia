/* Move Studio: the character objx 'text' section (ARC tag 'text', type 1): the banner / caption names shown at the
   top of the screen in battle. Plain browser JS, no dependencies: window.OBJTEXT, or require('./objtext.js') in node.
   Format, glyph table and what selects a record: README.md next to this file.

     parseText(u8)                -> {langs: [[{index, text, raw}...] per language], counts: [slots per language]}
     textToBytes(obj)             -> Uint8Array (byte identical to the input when nothing was changed)
     encodeGlyphs(str)            -> Uint8Array of glyph code bytes (no 1C 00 prefix, no terminator)
     decodeGlyphs(bytes)          -> string (stops at the 00 terminator)
     setRecord(obj, lang, index, str)   replace / add record `index` of language `lang` (str null = remove it)
     unsupportedChars(str)        -> characters encodeGlyphs would refuse (for live validation in an input box)
*/
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root) root.OBJTEXT = api;
})(typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : null), function () {
  'use strict';

  /* Every u16 of a record is XORed with KEY[(record index + word index) % 16] (the mess_text.py key). */
  const KEY = [0xED98, 0x90AB, 0x5E28, 0x8442, 0x32B1, 0x2DDA, 0xA657, 0xC8BF,
    0x0033, 0x579A, 0xF227, 0x4B4C, 0x36A7, 0x927D, 0x739D, 0x84CE];

  const CTL_STYLE = 0x1C;   // 1C n      : style; every retail record starts with 1C 00
  const CTL_ICON = 0x1E;    // 1E 0 s i  : inline icon i of set s (set 1 = PSP buttons, set 2 = item/element icons)
  const CTL_ARGS = { [CTL_STYLE]: 1, [CTL_ICON]: 3 };

  /* English block (language 0) glyph codes. status: 'confirmed' = seen in retail text where the meaning is certain;
     'inferred' = follows from the confirmed neighbours (digits 3, 4, 6-9). Anything else is unknown. */
  const GLYPHS = [];
  const add = (code, ch, status) => GLYPHS.push({ code, ch, status });
  add(0x20, ' ', 'confirmed');
  for (let d = 0; d < 10; d++) add(0x56 + d, String(d), [0, 1, 2, 5].includes(d) ? 'confirmed' : 'inferred');
  for (let i = 0; i < 26; i++) add(0x60 + i, String.fromCharCode(65 + i), 'confirmed');
  for (let i = 0; i < 26; i++) add(0x7A + i, String.fromCharCode(97 + i), 'confirmed');
  add(0x12D, '!', 'confirmed');      // "Powered Up!"
  add(0x12F, '%', 'confirmed');      // "until you reach 120%!"
  add(0x130, '&', 'confirmed');      // "Slice & Dice"
  add(0x131, "'", 'confirmed');      // "Dragon's Fang", "Thor's Hammer"
  add(0x134, '+', 'confirmed');      // "press (O)+(X) to finish them off!"
  add(0x135, ',', 'confirmed');      // "Look, the faeries are here!"
  add(0x139, ':', 'confirmed');      // "Skillchain: Fusion"
  add(0x159, '-', 'confirmed');      // "Cross-Slash", "Two-Hour Ability"
  add(0x15B, '“', 'confirmed'); // opening quote: “Anti-air” Particle Beam
  add(0x15C, '”', 'confirmed'); // closing quote

  const CODE_OF = new Map(GLYPHS.map((g) => [g.ch, g.code]));
  const CHAR_OF = new Map(GLYPHS.map((g) => [g.code, g.ch]));
  CODE_OF.set('’', 0x131);        // typographic apostrophe -> the apostrophe glyph
  CODE_OF.set('‐', 0x159);        // Unicode hyphen -> the hyphen glyph

  const hex = (v) => v.toString(16).toUpperCase();

  /* glyph codes are LEB128: 7 bits per byte, low bits first, bit 7 = more bytes follow (0x81 = 81 01, 0x12D = AD 02) */
  function readCode(b, p) {
    let v = 0, sh = 0, c;
    do {
      if (p >= b.length) throw new Error('glyph code runs past the end of the record');
      c = b[p++]; v += (c & 0x7F) * Math.pow(2, sh); sh += 7;
    } while (c & 0x80);
    return [v, p];
  }
  function writeCode(v, out) {
    if (!(Number.isInteger(v) && v >= 0 && v < 0x200000)) throw new Error('bad glyph code ' + v);
    do { let c = v & 0x7F; v >>>= 7; if (v) c |= 0x80; out.push(c); } while (v);
  }

  /* tokens: {g: code} | {ctl: code, args: [...]} */
  function tokenize(bytes) {
    const toks = [];
    let p = 0;
    while (p < bytes.length) {
      let v; [v, p] = readCode(bytes, p);
      if (v === 0) break;
      if (v in CTL_ARGS) {
        const args = [];
        for (let k = 0; k < CTL_ARGS[v]; k++) { let a; [a, p] = readCode(bytes, p); args.push(a); }
        toks.push({ ctl: v, args });
      } else toks.push({ g: v });
    }
    return { toks, end: p };
  }

  /* bytes -> string. Known glyphs as characters; the rest as escapes that encodeGlyphs reads back:
     {icon:S,I} = 1E 00 S I    {style:N} = 1C N    {ctl:C,a,b,..} = other control    {#12E} = raw glyph code */
  function decodeGlyphs(bytes) {
    let s = '';
    for (const t of tokenize(bytes).toks) {
      if (t.g !== undefined) s += CHAR_OF.has(t.g) ? CHAR_OF.get(t.g) : '{#' + hex(t.g) + '}';
      else if (t.ctl === CTL_ICON && t.args[0] === 0) s += '{icon:' + hex(t.args[1]) + ',' + hex(t.args[2]) + '}';
      else if (t.ctl === CTL_STYLE) s += '{style:' + hex(t.args[0]) + '}';
      else s += '{ctl:' + [t.ctl].concat(t.args).map(hex).join(',') + '}';
    }
    return s;
  }

  function parseEscape(body) {
    const m = /^(#|icon:|style:|ctl:)([0-9A-Fa-f]+(?:,[0-9A-Fa-f]+)*)$/.exec(body);
    if (!m) return null;
    const n = m[2].split(',').map((x) => parseInt(x, 16));
    if (m[1] === '#' && n.length === 1 && !(n[0] in CTL_ARGS) && n[0] !== 0) return [n[0]];
    if (m[1] === 'icon:' && n.length === 2) return [CTL_ICON, 0, n[0], n[1]];
    if (m[1] === 'style:' && n.length === 1) return [CTL_STYLE, n[0]];
    if (m[1] === 'ctl:' && n[0] in CTL_ARGS && n.length === 1 + CTL_ARGS[n[0]]) return n;
    return null;
  }

  /* walk a string: calls onCode(codes) per character / escape, onBad(ch) for something with no glyph */
  function walk(str, onCodes, onBad) {
    let open = true;                                    // straight " alternates opening / closing quote
    for (let i = 0; i < str.length;) {
      const ch = String.fromCodePoint(str.codePointAt(i));
      if (ch === '{') {
        const j = str.indexOf('}', i);
        const codes = j > 0 ? parseEscape(str.slice(i + 1, j)) : null;
        if (codes) { onCodes(codes); i = j + 1; continue; }
      }
      if (ch === '"') { onCodes([open ? 0x15B : 0x15C]); open = !open; }
      else if (CODE_OF.has(ch)) {
        const c = CODE_OF.get(ch);
        if (c === 0x15B) open = false; else if (c === 0x15C) open = true;
        onCodes([c]);
      } else onBad(ch);
      i += ch.length;
    }
  }

  function unsupportedChars(str) {
    const bad = [];
    walk(String(str), () => {}, (ch) => { if (!bad.includes(ch)) bad.push(ch); });
    return bad;
  }

  function encodeGlyphs(str) {
    const out = [], bad = [];
    walk(String(str), (codes) => codes.forEach((c) => writeCode(c, out)), (ch) => { if (!bad.includes(ch)) bad.push(ch); });
    if (bad.length) {
      throw new Error('no known glyph for ' + bad.map((c) => JSON.stringify(c)).join(' ') +
        ' (known: A-Z a-z 0-9 space ! % & \' + , : - " ; or write a raw glyph code as {#12E})');
    }
    return Uint8Array.from(out);
  }

  /* record plaintext = 1C <style> + glyphs + 00, padded with 00 to an even length */
  function recordRaw(str, style) {
    const g = encodeGlyphs(str), out = [CTL_STYLE];
    writeCode(style || 0, out);
    for (const b of g) out.push(b);
    out.push(0);
    if (out.length & 1) out.push(0);
    return Uint8Array.from(out);
  }

  function recordText(raw) {
    const { toks } = tokenize(raw);
    if (!(toks.length && toks[0].ctl === CTL_STYLE && raw[0] === CTL_STYLE)) return { text: decodeGlyphs(raw), style: null };
    const [style, skip] = readCode(raw, 1);                   // the leading 1C <style> is not part of the shown text
    return { text: decodeGlyphs(raw.subarray(skip)), style };
  }

  const u16 = (b, p) => b[p] | (b[p + 1] << 8);
  const u32 = (b, p) => (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;

  function parseText(u8) {
    const b = u8 instanceof Uint8Array ? u8 : new Uint8Array(u8);
    if (b.length < 8 || String.fromCharCode(b[0], b[1], b[2], b[3]) !== 'mlng') throw new Error('not an mlng section');
    const n = u32(b, 4);
    if (8 + 8 * n > b.length) throw new Error('mlng: bad language count ' + n);
    const langs = [], counts = [];
    for (let l = 0; l < n; l++) {
      const off = u32(b, 8 + 4 * l), size = u32(b, 8 + 4 * n + 4 * l);
      if (off + size > b.length) throw new Error('mlng: language ' + l + ' runs past the end');
      const blk = b.subarray(off, off + size), cnt = u16(blk, 0), recs = [];
      for (let i = 0; i < cnt; i++) {
        const st = u16(blk, 2 + 2 * i);
        if (!st) continue;                                   // 0 = no record in this slot
        const p = 2 * st, len = u16(blk, p);
        if (p + 2 + len > blk.length || (len & 1)) throw new Error('mlng: bad record ' + l + '/' + i);
        const raw = new Uint8Array(len);
        for (let j = 0; j < len / 2; j++) {
          const w = u16(blk, p + 2 + 2 * j) ^ KEY[(i + j) % 16];
          raw[2 * j] = w & 0xFF; raw[2 * j + 1] = w >> 8;
        }
        const { text, style } = recordText(raw);
        recs.push({ index: i, text, raw, style });
      }
      langs.push(recs); counts.push(cnt);
    }
    return { magic: 'mlng', langs, counts };
  }

  function textToBytes(obj) {
    const blocks = obj.langs.map((recs, l) => {
      const sorted = recs.slice().sort((a, c) => a.index - c.index);
      const cnt = Math.max(obj.counts ? obj.counts[l] || 0 : 0, sorted.length ? sorted[sorted.length - 1].index + 1 : 0);
      let size = 2 + 2 * cnt;
      for (const r of sorted) size += 2 + r.raw.length + (r.raw.length & 1);
      if (size > 0x1FFFE) throw new Error('language ' + l + ': text block is over 128 KiB');
      const blk = new Uint8Array(size), starts = new Map();
      blk[0] = cnt & 0xFF; blk[1] = cnt >> 8;
      let pos = 2 + 2 * cnt;
      for (const r of sorted) {
        if (starts.has(r.index)) throw new Error('language ' + l + ': record ' + r.index + ' twice');
        if (r.index < 0 || r.index > 0xFFFF) throw new Error('bad record index ' + r.index);
        starts.set(r.index, pos / 2);
        let raw = r.raw;
        if (raw.length & 1) { raw = new Uint8Array(raw.length + 1); raw.set(r.raw); }
        if (raw.length > 0xFFFF) throw new Error('record ' + r.index + ' is too long');
        blk[pos] = raw.length & 0xFF; blk[pos + 1] = raw.length >> 8;
        for (let j = 0; j < raw.length / 2; j++) {
          const w = (raw[2 * j] | (raw[2 * j + 1] << 8)) ^ KEY[(r.index + j) % 16];
          blk[pos + 2 + 2 * j] = w & 0xFF; blk[pos + 3 + 2 * j] = w >> 8;
        }
        pos += 2 + raw.length;
      }
      for (const [i, st] of starts) { blk[2 + 2 * i] = st & 0xFF; blk[3 + 2 * i] = st >> 8; }
      return blk;
    });
    const n = blocks.length, head = 8 + 8 * n;
    const out = new Uint8Array(head + blocks.reduce((s, x) => s + x.length, 0));
    out.set([0x6D, 0x6C, 0x6E, 0x67]);                                   // 'mlng'
    const put32 = (p, v) => { out[p] = v & 0xFF; out[p + 1] = (v >>> 8) & 0xFF; out[p + 2] = (v >>> 16) & 0xFF; out[p + 3] = v >>> 24; };
    put32(4, n);
    let off = head;
    blocks.forEach((blk, l) => { put32(8 + 4 * l, off); put32(8 + 4 * n + 4 * l, blk.length); out.set(blk, off); off += blk.length; });
    return out;
  }

  function setRecord(obj, lang, index, str) {
    const recs = obj.langs[lang];
    if (!recs) throw new Error('no language ' + lang);
    if (!(Number.isInteger(index) && index >= 0 && index <= 0xFFFF)) throw new Error('bad record index ' + index);
    const at = recs.findIndex((r) => r.index === index);
    if (str === null || str === undefined) {
      if (at >= 0) recs.splice(at, 1);
      return null;
    }
    const style = at >= 0 && recs[at].style !== null ? recs[at].style : 0;
    const raw = recordRaw(str, style);
    const rec = { index, text: recordText(raw).text, raw, style };
    if (at >= 0) recs[at] = rec; else { recs.push(rec); recs.sort((a, c) => a.index - c.index); }
    if (obj.counts && obj.counts[lang] <= index) obj.counts[lang] = index + 1;
    return rec;
  }

  /* an action record (actions.json "raw", 116 bytes from the action id) names its banner at byte +0x21; 0xFF = none */
  const ACTION_BANNER_OFFSET = 0x21;

  return { parseText, textToBytes, encodeGlyphs, decodeGlyphs, setRecord, unsupportedChars, GLYPHS, KEY,
    ACTION_BANNER_OFFSET };
});
