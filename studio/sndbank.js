/* sndbank.js - Dissidia 012 (PSP) character sound bank (`snd_` SSCF, objx ARC type 14) for the Move Studio.
 *
 * Plain browser JS, no dependencies; in node: const SB = require('./sndbank.js').
 * Mirrors psprecomp/games/dissidia/tools/voice_bank.py (Bank, decode_vag, encode_vag, read_wav, resample) and the slot
 * building of forge/ffxv_tools/sfx/noc_sfx.py, so a bank built here is byte-identical to one built there.
 * See README.md for the format. Rule of the project: never change slots 0..123 of a moveset bank, append only.
 */
(function (root) {
  'use strict';

  var HDR = 0x20, ENT = 0x20, BLK = 16;
  var FILTERS = [[0, 0], [60, 0], [115, -52], [98, -55], [122, -60]];
  var SENTINEL = [0x00, 0x07, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77];
  // voice_bank.DEFAULT_ENTRY: used only when the bank has no data slot to copy parameters from
  var DEFAULT_ENTRY = hex('000000000000000000000000803e50006400ff00ca5f0000d200000000000000');
  var MIN_RATE = 8000, MAX_RATE = 22050, MAX_SLOTS = 10000;

  function hex(s) { var u = new Uint8Array(s.length / 2); for (var i = 0; i < u.length; i++) u[i] = parseInt(s.substr(2 * i, 2), 16); return u; }
  function toHex(u) { var s = ''; for (var i = 0; i < u.length; i++) s += (u[i] < 16 ? '0' : '') + u[i].toString(16); return s; }
  function u8of(x) {
    if (x instanceof Uint8Array) return x;
    if (x instanceof ArrayBuffer) return new Uint8Array(x);
    if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
    throw new Error('expected Uint8Array / ArrayBuffer');
  }
  function dv(u) { return new DataView(u.buffer, u.byteOffset, u.byteLength); }
  function clamp16(s) { return s < -32768 ? -32768 : s > 32767 ? 32767 : s; }
  function roundHalfEven(x) { var r = Math.round(x); if (Math.abs(x % 1) === 0.5 && r % 2 !== 0) r -= 1; return r; }

  // ------------------------------------------------------------------ SSCF container
  function parseBank(input) {
    var d = u8of(input), v = dv(d);
    if (d[0] !== 0x53 || d[1] !== 0x53 || d[2] !== 0x43 || d[3] !== 0x46) throw new Error('not an SSCF bank');
    var total = v.getUint32(8, true), n = v.getUint32(12, true);
    if (total !== d.length) throw new Error('SSCF size field ' + total + ' != data size ' + d.length);
    var bank = { header: d.slice(0, HDR), slots: [] };
    var seen = {}, pos = HDR + ENT * n;
    for (var i = 0; i < n; i++) {
      var raw = d.slice(HDR + ENT * i, HDR + ENT * (i + 1)), rv = dv(raw);
      var off = rv.getUint32(4, true), size = rv.getUint32(8, true);
      var s = { index: i, raw: raw, kind: null, owner: null, blob: null };
      if (raw[3] & 0x10) { s.kind = 'link'; s.owner = off; }
      else if (size === 0) s.kind = 'empty';
      else if (seen[off + ':' + size] !== undefined) { s.kind = 'alias'; s.owner = seen[off + ':' + size]; }
      else {
        if (off !== pos) throw new Error('slot ' + i + ': data at 0x' + off.toString(16) + ', expected 0x' + pos.toString(16) + ' (bank not packed in slot order; unsupported)');
        s.kind = 'data'; s.blob = d.slice(off, off + size); seen[off + ':' + size] = i; pos += size;
      }
      bank.slots.push(s);
    }
    if (pos !== d.length) throw new Error('bank has ' + (d.length - pos) + ' trailing bytes after the last clip');
    for (i = 0; i < n; i++) refresh(bank, i);
    return bank;
  }

  // derived, read-only view fields of a slot (recomputed after edits); raw/blob/kind/owner are the source of truth
  function refresh(bank, i) {
    var s = bank.slots[i], rv = dv(s.raw);
    s.index = i;
    s.cue = cueOf(bank, i);
    s.offset = rv.getUint32(4, true); s.size = rv.getUint32(8, true);
    s.flags = s.raw[2];                       // 0x20 empty, 0x01 looped clip
    s.channels = 1;
    if (s.kind === 'data' || s.kind === 'alias') {
      s.rate = rv.getUint16(0xC, true);
      s.volume = s.raw[0xE];
      s.params = toHex(s.raw.subarray(0x10, 0x20));
      var src = s.kind === 'data' ? s.blob : bank.slots[s.owner].blob;
      var sc = scanVag(src);
      s.samples = sc.samples; s.loop = sc.loop; s.bytes = src.length;
      s.seconds = s.samples / (s.rate || 1);
    } else {
      s.rate = 0; s.volume = 0; s.samples = 0; s.loop = null; s.bytes = 0; s.seconds = 0; s.params = toHex(s.raw.subarray(0x10, 0x20));
    }
    return s;
  }

  function bankToBytes(bank) {
    var n = bank.slots.length, pos = HDR + ENT * n, offs = [], body = 0, i;
    for (i = 0; i < n; i++) if (bank.slots[i].kind === 'data') { offs[i] = [pos + body, bank.slots[i].blob.length]; body += bank.slots[i].blob.length; }
    var out = new Uint8Array(HDR + ENT * n + body), ov = dv(out);
    out.set(bank.header.subarray(0, HDR), 0);
    ov.setUint32(8, out.length, true); ov.setUint32(12, n, true);
    var p = HDR + ENT * n;
    for (i = 0; i < n; i++) {
      var s = bank.slots[i], raw = new Uint8Array(s.raw), rv = dv(raw);
      if (s.kind === 'data') { rv.setUint32(4, offs[i][0], true); rv.setUint32(8, offs[i][1], true); }
      else if (s.kind === 'alias') { rv.setUint32(4, offs[s.owner][0], true); rv.setUint32(8, offs[s.owner][1], true); }
      out.set(raw, HDR + ENT * i);
      if (s.kind === 'data') { out.set(s.blob, p); p += s.blob.length; }
    }
    return out;
  }

  function bankId(bank) { return dv(bank.header).getUint16(0x12, true); }
  // engine (EBOOT 0x0899075C / 0x08991928): base = 100 * u16 @0x12 (+ 10000 * (u16 @0x10 - 1); 0x10 is 1 in every disc bank),
  // or 100 * the u32 @0x14 when that is 0 (the 012 cast: Kain's cues start at 21000000; work/studio_sharedsnd/README.md)
  function cueBase(bank) { var v = dv(bank.header), id = v.getUint16(0x12, true); return id ? 100 * id + 10000 * Math.max(0, v.getUint16(0x10, true) - 1) : 100 * v.getUint32(0x14, true); }
  function cueOf(bank, slot) { return cueBase(bank) + slot; }
  function slotOfCue(bank, cue) { var k = cue - cueBase(bank); return k >= 0 && k < bank.slots.length ? k : -1; }
  // keyed timeline event (studio op `sound`, 0x0303): struct '<HHIIII' tick, 0x0303, 0, cue, 0, 0
  function soundEventBytes(tick, cue) {
    var u = new Uint8Array(20), v = dv(u);
    v.setUint16(0, tick, true); v.setUint16(2, 0x0303, true); v.setUint32(8, cue >>> 0, true);
    return u;
  }

  // ------------------------------------------------------------------ PS-ADPCM (VAG)
  function scanVag(data) {
    var nblk = (data.length / BLK) | 0, first = leadSilent(data) ? 1 : 0, n = 0, ls = null;
    for (var b = first; b < nblk; b++) {
      var flag = data[b * BLK + 1];
      if ((flag & 4) && ls === null) ls = n;
      n += 28;
      if (flag & 1) return { samples: n, loop: (flag & 2) && ls !== null ? { start: ls, end: n } : null };
    }
    return { samples: n, loop: null };
  }
  function leadSilent(d) { if (d.length < BLK) return false; for (var i = 0; i < BLK; i++) if (d[i]) return false; return true; }

  // raw clip -> {int16, loop}; voice_bank.decode_vag (PPSSPP VagDecoder semantics)
  function decodeVag(data) {
    var nblk = (data.length / BLK) | 0, sc = scanVag(data), out = new Int16Array(sc.samples), k = 0, s1 = 0, s2 = 0;
    for (var b = leadSilent(data) ? 1 : 0; b < nblk; b++) {
      var o = b * BLK, shift = data[o] & 15, filt = (data[o] >> 4) & 7, flag = data[o + 1];
      if (filt > 4) filt = 4;
      var f0 = FILTERS[filt][0], f1 = FILTERS[filt][1];
      for (var i = 0; i < 28; i++) {
        var nib = (data[o + 2 + (i >> 1)] >> (4 * (i & 1))) & 15;
        if (nib & 8) nib -= 16;
        var s = clamp16(((nib << 12) >> shift) + ((s1 * f0 + s2 * f1) >> 6));
        out[k++] = s; s2 = s1; s1 = s;
      }
      if (flag & 1) break;
    }
    return { int16: out, loop: sc.loop };
  }

  function encodeBlock(x, s1, s2, filt, shift) {
    var f0 = FILTERS[filt][0], f1 = FILTERS[filt][1], scale = 1 << (12 - shift), nibs = new Array(28), err = 0;
    for (var i = 0; i < 28; i++) {
      var v = x[i], pred = (s1 * f0 + s2 * f1) >> 6, q = (v - pred) / scale;
      q = q >= 0 ? Math.trunc(q + 0.5) : -Math.trunc(-q + 0.5);
      q = q < -8 ? -8 : q > 7 ? 7 : q;
      var y = clamp16(q * scale + pred), e = v - y;
      err += e * e; nibs[i] = q & 15; s2 = s1; s1 = y;
    }
    return { nibs: nibs, err: err, s1: s1, s2: s2 };
  }

  // int16 samples -> PS-ADPCM framed like retail: silent lead block, data blocks, then either the sentinel
  // block (one-shot, last data block flag 1) or, with loopStart (samples), loop flags 6 / 2 / 3 and no sentinel.
  // Identical to voice_bank.encode_vag for one-shot clips (including its shift candidate order, a Python set).
  function encodeVag(samples, loopStart) {
    var n = samples.length, nb = Math.max(1, Math.ceil(n / 28)), looped = loopStart !== undefined && loopStart !== null;
    var lb = looped ? Math.min(nb - 1, Math.max(0, Math.floor(loopStart / 28))) : -1;
    var out = new Uint8Array(BLK * (nb + (looped ? 1 : 2))), s1 = 0, s2 = 0, x = new Array(28);
    for (var b = 0; b < nb; b++) {
      for (var i = 0; i < 28; i++) { var j = b * 28 + i; x[i] = j < n ? samples[j] : 0; }
      var best = null;
      for (var filt = 0; filt < 5; filt++) {
        var f0 = FILTERS[filt][0], f1 = FILTERS[filt][1], a = s1, c = s2, mx = 0;
        for (i = 0; i < 28; i++) { var r = Math.abs(x[i] - ((a * f0 + c * f1) >> 6)); if (r > mx) mx = r; c = a; a = x[i]; }
        var sh = 12;
        while (sh > 0 && 7 * (1 << (12 - sh)) < mx) sh--;
        var cands = [Math.max(0, sh - 1), sh, Math.min(12, sh + 1)].filter(function (v, k, arr) { return arr.indexOf(v) === k; });
        cands.sort(function (p, q) { return (p & 7) - (q & 7); });       // CPython small-int set iteration order
        for (var ci = 0; ci < cands.length; ci++) {
          var res = encodeBlock(x, s1, s2, filt, cands[ci]);
          if (best === null || res.err < best.err) { best = res; best.filt = filt; best.sh = cands[ci]; }
        }
      }
      s1 = best.s1; s2 = best.s2;
      var o = BLK * (b + 1);
      out[o] = (best.filt << 4) | best.sh;
      out[o + 1] = looped ? (b === nb - 1 ? 3 : b === lb ? 6 : 2) : (b === nb - 1 ? 1 : 0);
      for (i = 0; i < 14; i++) out[o + 2 + i] = best.nibs[2 * i] | (best.nibs[2 * i + 1] << 4);
    }
    if (!looped) out.set(SENTINEL, BLK * (nb + 1));
    return out;
  }

  function decodeSlot(bank, i) {
    var s = bank.slots[i];
    if (s.kind === 'link') return decodeSlot(bank, s.owner);
    if (s.kind === 'empty') return { rate: 0, channels: 1, pcm: [new Float32Array(0)], int16: [new Int16Array(0)], loop: null };
    var rate = dv(s.raw).getUint16(0xC, true), d = decodeVag(s.kind === 'data' ? s.blob : bank.slots[s.owner].blob);
    var f = new Float32Array(d.int16.length);
    for (var k = 0; k < f.length; k++) f[k] = d.int16[k] / 32768;
    return { rate: rate, channels: 1, pcm: [f], int16: [d.int16], loop: d.loop, volume: s.raw[0xE] };
  }

  // ------------------------------------------------------------------ WAV
  // reads a RIFF WAVE (PCM 8/16/24/32-bit, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE) -> {rate, channels, pcm, int16}
  // int16 conversion matches voice_bank.read_wav (8-bit (b-128)<<8, 24/32-bit arithmetic shift down).
  function readWav(input) {
    var d = u8of(input), v = dv(d);
    var tag = function (o) { return String.fromCharCode(d[o], d[o + 1], d[o + 2], d[o + 3]); };
    if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('not a RIFF WAVE file');
    var fmt = null, data = null, o = 12;
    while (o + 8 <= d.length) {
      var id = tag(o), sz = v.getUint32(o + 4, true), body = o + 8;
      if (id === 'fmt ') fmt = { tag: v.getUint16(body, true), ch: v.getUint16(body + 2, true), rate: v.getUint32(body + 4, true),
        bits: v.getUint16(body + 14, true), sub: sz >= 26 ? v.getUint16(body + 24, true) : 0 };
      else if (id === 'data') data = [body, Math.min(sz, d.length - body)];
      o = body + sz + (sz & 1);
    }
    if (!fmt || !data) throw new Error('WAV without fmt or data chunk');
    var ft = fmt.tag === 0xFFFE ? fmt.sub : fmt.tag, ch = fmt.ch, bps = fmt.bits >> 3;
    if (ft !== 1 && ft !== 3) throw new Error('unsupported WAV format tag ' + ft + ' (PCM or float only)');
    var nf = Math.floor(data[1] / (bps * ch)), ints = [], fl = [];
    for (var c = 0; c < ch; c++) { ints.push(new Int16Array(nf)); fl.push(new Float32Array(nf)); }
    for (var f = 0; f < nf; f++) for (c = 0; c < ch; c++) {
      var p = data[0] + (f * ch + c) * bps, s;
      if (ft === 3) { var x = bps === 8 ? v.getFloat64(p, true) : v.getFloat32(p, true); s = clamp16(Math.round(x * 32768)); }
      else if (bps === 1) s = (d[p] - 128) << 8;
      else if (bps === 2) s = v.getInt16(p, true);
      else if (bps === 3) s = ((d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)) << 8) >> 16;
      else if (bps === 4) s = v.getInt32(p, true) >> 16;
      else throw new Error('unsupported sample width ' + fmt.bits);
      ints[c][f] = s; fl[c][f] = s / 32768;
    }
    return { rate: fmt.rate, channels: ch, pcm: fl, int16: ints };
  }

  // {rate, pcm: Float32Array[] | Int16Array[]} or (pcm, rate) -> 16-bit PCM WAV bytes
  function writeWav(snd, rate) {
    var chans = snd.int16 || snd.pcm || snd; rate = rate || snd.rate;
    chans = chans.map(toInt16);
    var ch = chans.length, nf = ch ? chans[0].length : 0, out = new Uint8Array(44 + nf * ch * 2), v = dv(out);
    var w = function (o, s) { for (var i = 0; i < 4; i++) out[o + i] = s.charCodeAt(i); };
    w(0, 'RIFF'); v.setUint32(4, 36 + nf * ch * 2, true); w(8, 'WAVE'); w(12, 'fmt ');
    v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, ch, true); v.setUint32(24, rate, true);
    v.setUint32(28, rate * ch * 2, true); v.setUint16(32, ch * 2, true); v.setUint16(34, 16, true);
    w(36, 'data'); v.setUint32(40, nf * ch * 2, true);
    for (var f = 0; f < nf; f++) for (var c = 0; c < ch; c++) v.setInt16(44 + (f * ch + c) * 2, chans[c][f], true);
    return out;
  }

  function toInt16(a) {
    if (a instanceof Int16Array) return a;
    var o = new Int16Array(a.length), isF = a instanceof Float32Array || a instanceof Float64Array;
    for (var i = 0; i < a.length; i++) o[i] = isF ? clamp16(Math.round(a[i] * 32768)) : clamp16(a[i] | 0);
    return o;
  }

  // voice_bank.resample: box filter when decimating, linear when interpolating
  function resample(x, rIn, rOut) {
    if (rIn === rOut || !x.length) return Int16Array.from(x);
    var n = Math.max(1, Math.trunc(x.length * rOut / rIn)), ratio = rIn / rOut, out = new Int16Array(n), i;
    if (ratio > 1) {
      for (i = 0; i < n; i++) {
        var lo = Math.trunc(i * ratio), hi = Math.min(x.length, Math.max(lo + 1, Math.trunc(Math.min(x.length, (i + 1) * ratio))));
        var sum = 0; for (var k = lo; k < hi; k++) sum += x[k];
        out[i] = Math.floor(sum / (hi - lo));
      }
    } else {
      for (i = 0; i < n; i++) {
        var p = i * ratio, q = Math.trunc(p), fr = p - q, v0 = x[q], v1 = q + 1 < x.length ? x[q + 1] : v0;
        out[i] = Math.trunc(v0 + (v1 - v0) * fr);
      }
    }
    return out;
  }

  // ------------------------------------------------------------------ adding slots (append only)
  function templateRaw(bank, tpl) {
    if (tpl === undefined || tpl === null) {               // default: the most common parameter set among data slots
      var cnt = {}, best = null;
      bank.slots.forEach(function (s, i) { if (s.kind === 'data') { var k = toHex(s.raw.subarray(0x10, 0x20)); cnt[k] = (cnt[k] || 0) + 1; if (best === null || cnt[k] > cnt[best[0]]) best = [k, i]; } });
      tpl = best ? best[1] : -1;
    }
    var raw = tpl >= 0 ? new Uint8Array(bank.slots[tpl].raw) : new Uint8Array(DEFAULT_ENTRY);
    if (tpl >= 0 && bank.slots[tpl].kind !== 'data' && bank.slots[tpl].kind !== 'alias') throw new Error('template slot ' + tpl + ' has no clip');
    raw[2] = raw[3] = 0;
    return raw;
  }
  function checkRoom(bank) { if (bank.slots.length >= MAX_SLOTS) throw new Error('slot count limit (' + MAX_SLOTS + ': cues would reach the next bank id)'); }

  // pcm: readWav() result, {rate, pcm|int16}, or an array of channels (Float32Array in -1..1 or Int16Array) with opts.rate.
  // opts: rate (source rate), targetRate (force), volume 0..100 (default 80), template (slot whose +0x10..+0x1F
  //       parameter bytes are copied; noc_sfx uses 28, 49, 52), trimSeconds, fadeSeconds (noc_sfx 'trim'),
  //       loopStart (samples at the stored rate; makes a looped clip, unverified in game), gain (linear, default 1).
  // Returns the new slot index. Mixes to mono (floor average, as voice_bank), resamples into [8000, 22050] Hz.
  function addSound(bank, pcm, opts) {
    opts = opts || {}; checkRoom(bank);
    var chans = Array.isArray(pcm) ? pcm : (pcm.int16 || pcm.pcm);
    var rate = opts.rate || pcm.rate;
    if (!rate) throw new Error('addSound: no sample rate');
    chans = chans.map(toInt16);
    var n = chans[0].length, mono;
    if (chans.length === 1) mono = chans[0];
    else { mono = new Int16Array(n); for (var i = 0; i < n; i++) { var s = 0; for (var c = 0; c < chans.length; c++) s += chans[c][i]; mono[i] = Math.floor(s / chans.length); } }
    if (opts.gain !== undefined && opts.gain !== 1) mono = mono.map(function (v) { return clamp16(Math.round(v * opts.gain)); });
    var target = opts.targetRate || Math.min(MAX_RATE, Math.max(MIN_RATE, rate));
    if (target !== rate) { mono = resample(mono, rate, target); rate = target; }
    if (opts.trimSeconds !== undefined) {
      mono = Array.from(mono.subarray(0, Math.trunc(opts.trimSeconds * rate)));
      var nf = Math.trunc((opts.fadeSeconds || 0) * rate), L = mono.length;
      for (var j = 0; j < nf && j < L; j++) mono[L - nf + j] = Math.trunc(mono[L - nf + j] * Math.pow(1 - (j + 1) / nf, 1.5));
    }
    if (!mono.length) throw new Error('addSound: empty clip');
    var raw = templateRaw(bank, opts.template), looped = opts.loopStart !== undefined && opts.loopStart !== null;
    raw[2] = looped ? 1 : 0;
    var rv = dv(raw);
    rv.setUint16(0xC, rate, true); raw[0xE] = opts.volume === undefined ? 80 : opts.volume;
    bank.slots.push({ raw: raw, kind: 'data', owner: null, blob: encodeVag(mono, looped ? opts.loopStart : undefined) });
    var idx = bank.slots.length - 1; refresh(bank, idx);
    return idx;
  }

  // a slot playing an existing clip at another pitch / volume (no new ADPCM). opts: pitch (rate factor) or rate, volume, template
  function addAlias(bank, owner, opts) {
    opts = opts || {}; checkRoom(bank);
    var o = bank.slots[owner];
    while (o.kind === 'alias') { owner = o.owner; o = bank.slots[owner]; }
    if (o.kind !== 'data') throw new Error('alias owner ' + owner + ' has no clip');
    var raw = templateRaw(bank, opts.template === undefined ? owner : opts.template), rv = dv(raw);
    raw[2] = o.raw[2] & 1;
    rv.setUint16(0xC, opts.rate || roundHalfEven(dv(o.raw).getUint16(0xC, true) * (opts.pitch || 1)), true);
    raw[0xE] = opts.volume === undefined ? o.raw[0xE] : opts.volume;
    bank.slots.push({ raw: raw, kind: 'alias', owner: owner, blob: null });
    var idx = bank.slots.length - 1; refresh(bank, idx);
    return idx;
  }

  // an empty placeholder (keeps later slot numbers / cues stable)
  function addEmpty(bank) {
    checkRoom(bank);
    var e = null;
    for (var i = 0; i < bank.slots.length && !e; i++) if (bank.slots[i].kind === 'empty') e = new Uint8Array(bank.slots[i].raw);
    if (!e) { e = new Uint8Array(32); e[2] = 0x20; e[0x12] = 0xff; e[0x14] = 0xca; e[0x15] = 0x5f; }
    bank.slots.push({ raw: e, kind: 'empty', owner: null, blob: null });
    refresh(bank, bank.slots.length - 1);
    return bank.slots.length - 1;
  }

  // ------------------------------------------------------------------ objx (ARC) helpers
  // ARC\x01: u32 count @4, 16-byte entries from 0x10: u32 type, char tag[4], u32 offset, u32 size
  function findBankInObjx(input) {
    var d = u8of(input), v = dv(d);
    if (!(d[0] === 0x41 && d[1] === 0x52 && d[2] === 0x43 && d[3] === 1)) throw new Error('not an ARC/objx file');
    var n = v.getUint32(4, true);
    for (var i = 0; i < n; i++) {
      var e = 16 + 16 * i;
      if (v.getUint32(e, true) === 14) {
        var off = v.getUint32(e + 8, true), size = v.getUint32(e + 12, true);
        return { entry: i, offset: off, size: size, bytes: d.subarray(off, off + size) };
      }
    }
    throw new Error('objx has no type-14 snd_ entry');
  }

  var API = {
    parseBank: parseBank, bankToBytes: bankToBytes, decodeSlot: decodeSlot,
    encodeWav: readWav, readWav: readWav, writeWav: writeWav,   // encodeWav: an old name of readWav (WAV bytes -> PCM)
    addSound: addSound, addAlias: addAlias, addEmpty: addEmpty,
    cueOf: cueOf, cueBase: cueBase, slotOfCue: slotOfCue, bankId: bankId, soundEventBytes: soundEventBytes,
    decodeVag: decodeVag, encodeVag: encodeVag, resample: resample, findBankInObjx: findBankInObjx, refresh: refresh,
    LIMITS: { MIN_RATE: MIN_RATE, MAX_RATE: MAX_RATE, MAX_SLOTS: MAX_SLOTS, PROTECTED_SLOTS: 124 }
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = API;
  else root.SndBank = API;
})(typeof self !== 'undefined' ? self : this);
