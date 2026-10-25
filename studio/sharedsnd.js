/* sharedsnd.js - Dissidia 012 (PSP) shared sound banks for the Move Studio.
 *
 * A timeline `sound` event (op 0x0303) carries a cue id. The engine (EBOOT 0x08991928) walks its 20 registered
 * SSCF banks and plays slot (cue - base) of the first bank with base <= cue < base + slot count. base comes from the
 * bank header (EBOOT 0x0899075C): 100 * u16@0x12 (+ 10000 * (u16@0x10 - 1), always 1 on the disc), or, when u16@0x12
 * is 0, 100 * u32@0x14 (u32 >= 600). So cue 100010 = bank 1000 (snd_battle.scd, in every battle stage) slot 10.
 *
 * This file knows where every bank on the disc lives (BANKS, generated from the US/EU disc by scanning PACKAGE.BIN)
 * and pulls the SSCF out of its container (MPK by member name, ARC by tag). Parsing / decoding / WAV is sndbank.js
 * (work/studio_snd/sndbank.js, the copy in studio/ is identical): load it first in the browser
 * (window.SndBank), or it is require()d in node.
 *
 *   SharedSnd.bankFiles({battle:true})   -> ['general_archive/main/main.bin', 'map/maparc_one00_2.mpk', ...]
 *   SharedSnd.cueLocation(100010)        -> {cue, id:1000, base:100000, slot:10, name:'snd_battle', group:'battle',
 *                                            path:'map/maparc_one00_2.mpk', member:['map_se_one00-msnd.mpk','snd_battle.scd']}
 *   await SharedSnd.loadBank(1000, isoGet)            -> sndbank bank object (cached per getFile)
 *   await SharedSnd.decodeCue(100010, isoGet)         -> sndbank decodeSlot result + {location}
 *   await SharedSnd.cueWav(100010, isoGet)            -> Uint8Array RIFF WAV
 * getFile(path) is the studio's isoGet (PACKAGE.BIN by path hash) or any async path -> Uint8Array.
 */
(function (root) {
  'use strict';
  var SB = (typeof module !== 'undefined' && module.exports && typeof require === 'function')
    ? (function () { try { return require('./sndbank.js'); } catch (e) { return require('../studio_snd/sndbank.js'); } })() : root.SndBank;

  // id, base (= first cue), slots, name (the .scd / objx name), group, path (PACKAGE.BIN), member (container chain),
  // battle (registered in a normal battle: true / false / 'unknown'), alsoIn (identical copies), variants (other banks
  // that reuse the same id; never loaded together: two00/two05 stages, an assist's _a/_w objx, Chaos's forms)
  var BANKS = [
    {"id":100,"base":10000,"slots":33,"name":"snd_common","group":"common","path":"general_archive/main/main.bin","member":["snd_common.scd"],"battle":true},
    {"id":1000,"base":100000,"slots":146,"name":"snd_battle","group":"battle","path":"map/maparc_one00_2.mpk","member":["map_se_one00-msnd.mpk","snd_battle.scd"],"battle":true,"alsoIn":["map/maparc_eht00_2.mpk","map/maparc_ele00_2.mpk","map/maparc_fiv00_2.mpk","map/maparc_for00_2.mpk","map/maparc_nin00_2.mpk","map/maparc_org00_2.mpk","map/maparc_org50_2.mpk","map/maparc_sev00_2.mpk","map/maparc_six00_2.mpk","map/maparc_ten00_2.mpk","map/maparc_thn00_2.mpk","map/maparc_thr00_2.mpk","map/maparc_twe00_2.mpk","map/maparc_two00_2.mpk","map/maparc_two05_2.mpk","map/maparc_vni00_2.mpk","map/maparc_vsi00_2.mpk","map/maparc_vth00_2.mpk","map/maparc_vtw00_2.mpk"]},
    {"id":4000,"base":400000,"slots":56,"name":"snd_menu","group":"menu","path":"general_archive/main/main.bin","member":["snd_menu.scd"],"battle":false},
    {"id":5000,"base":500000,"slots":68,"name":"snd_region","group":"region","path":"sound/snd_region.scd","member":[],"battle":false},
    {"id":5100,"base":510000,"slots":77,"name":"worldmap","group":"region","path":"worldmap.bin","member":["snd_"],"battle":false},
    {"id":6000,"base":600000,"slots":2,"name":"snd_region_enc","group":"region","path":"sound/snd_region_enc.scd","member":[],"battle":false},
    {"id":9000,"base":900000,"slots":1,"name":"snd_ptc_WALL","group":"party","path":"general_archive/party_command/wall.bin","member":["snd_ptc_WALL.scd"],"battle":true},
    {"id":10000,"base":1000000,"slots":76,"name":"p_one100","group":"character","path":"obj/p_one100.objx","member":["snd_"],"battle":true},
    {"id":10100,"base":1010000,"slots":45,"name":"voice_p_one100_00","group":"voice","path":"voice/en/battle/voice_p_one100_00.scd","member":[],"battle":true},
    {"id":10500,"base":1050000,"slots":73,"name":"p_one100_a","group":"character","path":"obj/p_one100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_one100_w","path":"obj/p_one100_w.objx","member":["snd_"],"slots":73}]},
    {"id":10600,"base":1060000,"slots":42,"name":"voice_p_one100_a_00","group":"voice","path":"voice/en/battle/voice_p_one100_a_00.scd","member":[],"battle":true},
    {"id":11000,"base":1100000,"slots":78,"name":"p_one200","group":"character","path":"obj/p_one200.objx","member":["snd_"],"battle":true},
    {"id":11100,"base":1110000,"slots":44,"name":"voice_p_one200_00","group":"voice","path":"voice/en/battle/voice_p_one200_00.scd","member":[],"battle":true},
    {"id":11500,"base":1150000,"slots":43,"name":"p_one200_a","group":"character","path":"obj/p_one200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_one200_w","path":"obj/p_one200_w.objx","member":["snd_"],"slots":43},{"name":"p_one200_zako_w","path":"obj/p_one200_zako_w.objx","member":["snd_"],"slots":43}]},
    {"id":11600,"base":1160000,"slots":40,"name":"voice_p_one200_a_00","group":"voice","path":"voice/en/battle/voice_p_one200_a_00.scd","member":[],"battle":true},
    {"id":12000,"base":1200000,"slots":91,"name":"p_two100","group":"character","path":"obj/p_two100.objx","member":["snd_"],"battle":true},
    {"id":12100,"base":1210000,"slots":51,"name":"voice_p_two100_00","group":"voice","path":"voice/en/battle/voice_p_two100_00.scd","member":[],"battle":true},
    {"id":12500,"base":1250000,"slots":62,"name":"p_two100_a","group":"character","path":"obj/p_two100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_two100_w","path":"obj/p_two100_w.objx","member":["snd_"],"slots":62}]},
    {"id":12600,"base":1260000,"slots":49,"name":"voice_p_two100_a_00","group":"voice","path":"voice/en/battle/voice_p_two100_a_00.scd","member":[],"battle":true},
    {"id":13000,"base":1300000,"slots":82,"name":"p_two200","group":"character","path":"obj/p_two200.objx","member":["snd_"],"battle":true},
    {"id":13100,"base":1310000,"slots":46,"name":"voice_p_two200_00","group":"voice","path":"voice/en/battle/voice_p_two200_00.scd","member":[],"battle":true},
    {"id":13500,"base":1350000,"slots":60,"name":"p_two200_a","group":"character","path":"obj/p_two200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_two200_w","path":"obj/p_two200_w.objx","member":["snd_"],"slots":60},{"name":"p_two200_zako_w","path":"obj/p_two200_zako_w.objx","member":["snd_"],"slots":60}]},
    {"id":13600,"base":1360000,"slots":36,"name":"voice_p_two200_a_00","group":"voice","path":"voice/en/battle/voice_p_two200_a_00.scd","member":[],"battle":true},
    {"id":14000,"base":1400000,"slots":70,"name":"p_thr100","group":"character","path":"obj/p_thr100.objx","member":["snd_"],"battle":true},
    {"id":14100,"base":1410000,"slots":54,"name":"voice_p_thr100_00","group":"voice","path":"voice/en/battle/voice_p_thr100_00.scd","member":[],"battle":true},
    {"id":14500,"base":1450000,"slots":66,"name":"p_thr100_a","group":"character","path":"obj/p_thr100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_thr100_w","path":"obj/p_thr100_w.objx","member":["snd_"],"slots":66}]},
    {"id":14600,"base":1460000,"slots":50,"name":"voice_p_thr100_a_00","group":"voice","path":"voice/en/battle/voice_p_thr100_a_00.scd","member":[],"battle":true},
    {"id":15000,"base":1500000,"slots":45,"name":"p_thr200","group":"character","path":"obj/p_thr200.objx","member":["snd_"],"battle":true},
    {"id":15100,"base":1510000,"slots":51,"name":"voice_p_thr200_00","group":"voice","path":"voice/en/battle/voice_p_thr200_00.scd","member":[],"battle":true},
    {"id":15500,"base":1550000,"slots":26,"name":"p_thr200_a","group":"character","path":"obj/p_thr200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_thr200_w","path":"obj/p_thr200_w.objx","member":["snd_"],"slots":26},{"name":"p_thr200_zako_w","path":"obj/p_thr200_zako_w.objx","member":["snd_"],"slots":26}]},
    {"id":15600,"base":1560000,"slots":42,"name":"voice_p_thr200_a_00","group":"voice","path":"voice/en/battle/voice_p_thr200_a_00.scd","member":[],"battle":true},
    {"id":16000,"base":1600000,"slots":83,"name":"p_for100","group":"character","path":"obj/p_for100.objx","member":["snd_"],"battle":true},
    {"id":16100,"base":1610000,"slots":40,"name":"voice_p_for100_00","group":"voice","path":"voice/en/battle/voice_p_for100_00.scd","member":[],"battle":true},
    {"id":16500,"base":1650000,"slots":74,"name":"p_for100_a","group":"character","path":"obj/p_for100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_for100_w","path":"obj/p_for100_w.objx","member":["snd_"],"slots":74}]},
    {"id":16600,"base":1660000,"slots":37,"name":"voice_p_for100_a_00","group":"voice","path":"voice/en/battle/voice_p_for100_a_00.scd","member":[],"battle":true},
    {"id":17000,"base":1700000,"slots":81,"name":"p_for200","group":"character","path":"obj/p_for200.objx","member":["snd_"],"battle":true},
    {"id":17100,"base":1710000,"slots":41,"name":"voice_p_for200_00","group":"voice","path":"voice/en/battle/voice_p_for200_00.scd","member":[],"battle":true},
    {"id":17500,"base":1750000,"slots":39,"name":"p_for200_a","group":"character","path":"obj/p_for200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_for200_w","path":"obj/p_for200_w.objx","member":["snd_"],"slots":39},{"name":"p_for200_zako_w","path":"obj/p_for200_zako_w.objx","member":["snd_"],"slots":39}]},
    {"id":17600,"base":1760000,"slots":37,"name":"voice_p_for200_a_00","group":"voice","path":"voice/en/battle/voice_p_for200_a_00.scd","member":[],"battle":true},
    {"id":18000,"base":1800000,"slots":124,"name":"p_fiv100","group":"character","path":"obj/p_fiv100.objx","member":["snd_"],"battle":true},
    {"id":18100,"base":1810000,"slots":71,"name":"voice_p_fiv100_00","group":"voice","path":"voice/en/battle/voice_p_fiv100_00.scd","member":[],"battle":true},
    {"id":18500,"base":1850000,"slots":120,"name":"p_fiv100_a","group":"character","path":"obj/p_fiv100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_fiv100_w","path":"obj/p_fiv100_w.objx","member":["snd_"],"slots":120}]},
    {"id":18600,"base":1860000,"slots":70,"name":"voice_p_fiv100_a_00","group":"voice","path":"voice/en/battle/voice_p_fiv100_a_00.scd","member":[],"battle":true},
    {"id":19000,"base":1900000,"slots":73,"name":"p_fiv200","group":"character","path":"obj/p_fiv200.objx","member":["snd_"],"battle":true},
    {"id":19100,"base":1910000,"slots":49,"name":"voice_p_fiv200_00","group":"voice","path":"voice/en/battle/voice_p_fiv200_00.scd","member":[],"battle":true},
    {"id":19500,"base":1950000,"slots":70,"name":"p_fiv200_a","group":"character","path":"obj/p_fiv200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_fiv200_w","path":"obj/p_fiv200_w.objx","member":["snd_"],"slots":70},{"name":"p_fiv200_zako_w","path":"obj/p_fiv200_zako_w.objx","member":["snd_"],"slots":70}]},
    {"id":19600,"base":1960000,"slots":45,"name":"voice_p_fiv200_a_00","group":"voice","path":"voice/en/battle/voice_p_fiv200_a_00.scd","member":[],"battle":true},
    {"id":20000,"base":2000000,"slots":121,"name":"p_six100","group":"character","path":"obj/p_six100.objx","member":["snd_"],"battle":true},
    {"id":20100,"base":2010000,"slots":41,"name":"voice_p_six100_00","group":"voice","path":"voice/en/battle/voice_p_six100_00.scd","member":[],"battle":true},
    {"id":20500,"base":2050000,"slots":100,"name":"p_six100_a","group":"character","path":"obj/p_six100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_six100_w","path":"obj/p_six100_w.objx","member":["snd_"],"slots":100}]},
    {"id":20600,"base":2060000,"slots":39,"name":"voice_p_six100_a_00","group":"voice","path":"voice/en/battle/voice_p_six100_a_00.scd","member":[],"battle":true},
    {"id":21000,"base":2100000,"slots":86,"name":"p_six200","group":"character","path":"obj/p_six200.objx","member":["snd_"],"battle":true},
    {"id":21100,"base":2110000,"slots":41,"name":"voice_p_six200_00","group":"voice","path":"voice/en/battle/voice_p_six200_00.scd","member":[],"battle":true},
    {"id":21500,"base":2150000,"slots":55,"name":"p_six200_a","group":"character","path":"obj/p_six200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_six200_w","path":"obj/p_six200_w.objx","member":["snd_"],"slots":55},{"name":"p_six200_zako_w","path":"obj/p_six200_zako_w.objx","member":["snd_"],"slots":55}]},
    {"id":21600,"base":2160000,"slots":40,"name":"voice_p_six200_a_00","group":"voice","path":"voice/en/battle/voice_p_six200_a_00.scd","member":[],"battle":true},
    {"id":22000,"base":2200000,"slots":70,"name":"p_sev100","group":"character","path":"obj/p_sev100.objx","member":["snd_"],"battle":true},
    {"id":22100,"base":2210000,"slots":43,"name":"voice_p_sev100_00","group":"voice","path":"voice/en/battle/voice_p_sev100_00.scd","member":[],"battle":true},
    {"id":22500,"base":2250000,"slots":42,"name":"p_sev100_a","group":"character","path":"obj/p_sev100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_sev100_w","path":"obj/p_sev100_w.objx","member":["snd_"],"slots":42}]},
    {"id":22600,"base":2260000,"slots":42,"name":"voice_p_sev100_a_00","group":"voice","path":"voice/en/battle/voice_p_sev100_a_00.scd","member":[],"battle":true},
    {"id":23000,"base":2300000,"slots":77,"name":"p_sev200","group":"character","path":"obj/p_sev200.objx","member":["snd_"],"battle":true},
    {"id":23100,"base":2310000,"slots":37,"name":"voice_p_sev200_00","group":"voice","path":"voice/en/battle/voice_p_sev200_00.scd","member":[],"battle":true},
    {"id":23500,"base":2350000,"slots":53,"name":"p_sev200_a","group":"character","path":"obj/p_sev200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_sev200_w","path":"obj/p_sev200_w.objx","member":["snd_"],"slots":53},{"name":"p_sev200_zako_w","path":"obj/p_sev200_zako_w.objx","member":["snd_"],"slots":53}]},
    {"id":23600,"base":2360000,"slots":35,"name":"voice_p_sev200_a_00","group":"voice","path":"voice/en/battle/voice_p_sev200_a_00.scd","member":[],"battle":true},
    {"id":24000,"base":2400000,"slots":71,"name":"p_eht100","group":"character","path":"obj/p_eht100.objx","member":["snd_"],"battle":true},
    {"id":24100,"base":2410000,"slots":54,"name":"voice_p_eht100_00","group":"voice","path":"voice/en/battle/voice_p_eht100_00.scd","member":[],"battle":true},
    {"id":24500,"base":2450000,"slots":61,"name":"p_eht100_a","group":"character","path":"obj/p_eht100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_eht100_w","path":"obj/p_eht100_w.objx","member":["snd_"],"slots":61}]},
    {"id":24600,"base":2460000,"slots":42,"name":"voice_p_eht100_a_00","group":"voice","path":"voice/en/battle/voice_p_eht100_a_00.scd","member":[],"battle":true},
    {"id":25000,"base":2500000,"slots":67,"name":"p_eht200","group":"character","path":"obj/p_eht200.objx","member":["snd_"],"battle":true},
    {"id":25100,"base":2510000,"slots":42,"name":"voice_p_eht200_00","group":"voice","path":"voice/en/battle/voice_p_eht200_00.scd","member":[],"battle":true},
    {"id":25500,"base":2550000,"slots":37,"name":"p_eht200_a","group":"character","path":"obj/p_eht200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_eht200_w","path":"obj/p_eht200_w.objx","member":["snd_"],"slots":37},{"name":"p_eht200_zako_w","path":"obj/p_eht200_zako_w.objx","member":["snd_"],"slots":37}]},
    {"id":25600,"base":2560000,"slots":37,"name":"voice_p_eht200_a_00","group":"voice","path":"voice/en/battle/voice_p_eht200_a_00.scd","member":[],"battle":true},
    {"id":26000,"base":2600000,"slots":78,"name":"p_nin100","group":"character","path":"obj/p_nin100.objx","member":["snd_"],"battle":true},
    {"id":26100,"base":2610000,"slots":48,"name":"voice_p_nin100_00","group":"voice","path":"voice/en/battle/voice_p_nin100_00.scd","member":[],"battle":true},
    {"id":26500,"base":2650000,"slots":29,"name":"p_nin100_a","group":"character","path":"obj/p_nin100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_nin100_w","path":"obj/p_nin100_w.objx","member":["snd_"],"slots":29}]},
    {"id":26600,"base":2660000,"slots":46,"name":"voice_p_nin100_a_00","group":"voice","path":"voice/en/battle/voice_p_nin100_a_00.scd","member":[],"battle":true},
    {"id":27000,"base":2700000,"slots":81,"name":"p_nin200","group":"character","path":"obj/p_nin200.objx","member":["snd_"],"battle":true},
    {"id":27100,"base":2710000,"slots":42,"name":"voice_p_nin200_00","group":"voice","path":"voice/en/battle/voice_p_nin200_00.scd","member":[],"battle":true},
    {"id":27500,"base":2750000,"slots":56,"name":"p_nin200_a","group":"character","path":"obj/p_nin200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_nin200_w","path":"obj/p_nin200_w.objx","member":["snd_"],"slots":56},{"name":"p_nin200_zako_w","path":"obj/p_nin200_zako_w.objx","member":["snd_"],"slots":56}]},
    {"id":27600,"base":2760000,"slots":41,"name":"voice_p_nin200_a_00","group":"voice","path":"voice/en/battle/voice_p_nin200_a_00.scd","member":[],"battle":true},
    {"id":28000,"base":2800000,"slots":67,"name":"p_ten100","group":"character","path":"obj/p_ten100.objx","member":["snd_"],"battle":true},
    {"id":28100,"base":2810000,"slots":56,"name":"voice_p_ten100_00","group":"voice","path":"voice/en/battle/voice_p_ten100_00.scd","member":[],"battle":true},
    {"id":28500,"base":2850000,"slots":64,"name":"p_ten100_a","group":"character","path":"obj/p_ten100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_ten100_w","path":"obj/p_ten100_w.objx","member":["snd_"],"slots":64}]},
    {"id":28600,"base":2860000,"slots":55,"name":"voice_p_ten100_a_00","group":"voice","path":"voice/en/battle/voice_p_ten100_a_00.scd","member":[],"battle":true},
    {"id":29000,"base":2900000,"slots":78,"name":"p_ten200","group":"character","path":"obj/p_ten200.objx","member":["snd_"],"battle":true},
    {"id":29100,"base":2910000,"slots":55,"name":"voice_p_ten200_00","group":"voice","path":"voice/en/battle/voice_p_ten200_00.scd","member":[],"battle":true},
    {"id":29500,"base":2950000,"slots":61,"name":"p_ten200_a","group":"character","path":"obj/p_ten200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_ten200_w","path":"obj/p_ten200_w.objx","member":["snd_"],"slots":61},{"name":"p_ten200_zako_w","path":"obj/p_ten200_zako_w.objx","member":["snd_"],"slots":61}]},
    {"id":29600,"base":2960000,"slots":53,"name":"voice_p_ten200_a_00","group":"voice","path":"voice/en/battle/voice_p_ten200_a_00.scd","member":[],"battle":true},
    {"id":30000,"base":3000000,"slots":81,"name":"p_gst100","group":"character","path":"obj/p_gst100.objx","member":["snd_"],"battle":true},
    {"id":30100,"base":3010000,"slots":42,"name":"voice_p_gst100_00","group":"voice","path":"voice/en/battle/voice_p_gst100_00.scd","member":[],"battle":true},
    {"id":30500,"base":3050000,"slots":37,"name":"p_gst100_a","group":"character","path":"obj/p_gst100_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_gst100_w","path":"obj/p_gst100_w.objx","member":["snd_"],"slots":37}]},
    {"id":30600,"base":3060000,"slots":41,"name":"voice_p_gst100_a_00","group":"voice","path":"voice/en/battle/voice_p_gst100_a_00.scd","member":[],"battle":true},
    {"id":31000,"base":3100000,"slots":63,"name":"p_gst200","group":"character","path":"obj/p_gst200.objx","member":["snd_"],"battle":true},
    {"id":31100,"base":3110000,"slots":38,"name":"voice_p_gst200_00","group":"voice","path":"voice/en/battle/voice_p_gst200_00.scd","member":[],"battle":true},
    {"id":31500,"base":3150000,"slots":49,"name":"p_gst200_a","group":"character","path":"obj/p_gst200_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_gst200_w","path":"obj/p_gst200_w.objx","member":["snd_"],"slots":49}]},
    {"id":31600,"base":3160000,"slots":36,"name":"voice_p_gst200_a_00","group":"voice","path":"voice/en/battle/voice_p_gst200_a_00.scd","member":[],"battle":true},
    {"id":32000,"base":3200000,"slots":5,"name":"p_org200_00","group":"character","path":"obj/p_org200_00.objx","member":["snd_"],"battle":true},
    {"id":32100,"base":3210000,"slots":24,"name":"voice_p_org200_normal","group":"voice","path":"voice/en/battle/voice_p_org200_normal.scd","member":[],"battle":true},
    {"id":33000,"base":3300000,"slots":3,"name":"p_org200_01","group":"character","path":"obj/p_org200_01.objx","member":["snd_"],"battle":true},
    {"id":33100,"base":3310000,"slots":27,"name":"voice_p_org200_hard","group":"voice","path":"voice/en/battle/voice_p_org200_hard.scd","member":[],"battle":true},
    {"id":34000,"base":3400000,"slots":41,"name":"p_org200_02","group":"character","path":"obj/p_org200_02.objx","member":["snd_"],"battle":true},
    {"id":34100,"base":3410000,"slots":25,"name":"voice_p_org200_s_hard","group":"voice","path":"voice/en/battle/voice_p_org200_s_hard.scd","member":[],"battle":true},
    {"id":35000,"base":3500000,"slots":75,"name":"p_org200_00 (2nd snd_)","group":"character","path":"obj/p_org200_00.objx","member":["snd_#1"],"battle":true,"variants":[{"name":"p_org200_01 (2nd snd_)","path":"obj/p_org200_01.objx","member":["snd_#1"],"slots":75},{"name":"p_org200_02 (2nd snd_)","path":"obj/p_org200_02.objx","member":["snd_#1"],"slots":75}]},
    {"id":35100,"base":3510000,"slots":9,"name":"voice_p_org200_s_hard2","group":"voice","path":"voice/en/battle/voice_p_org200_s_hard2.scd","member":[],"battle":true},
    {"id":40000,"base":4000000,"slots":18,"name":"snd_one00","group":"stage","path":"map/maparc_one00_2.mpk","member":["map_se_one00-msnd.mpk","snd_one00.scd"],"battle":true},
    {"id":40100,"base":4010000,"slots":9,"name":"snd_two00","group":"stage","path":"map/maparc_two00_2.mpk","member":["map_se_two00-msnd.mpk","snd_two00.scd"],"battle":true,"variants":[{"name":"snd_two05","path":"map/maparc_two05_2.mpk","member":["map_se_two05-msnd.mpk","snd_two05.scd"],"slots":9}]},
    {"id":40200,"base":4020000,"slots":17,"name":"snd_thr00","group":"stage","path":"map/maparc_thr00_2.mpk","member":["map_se_thr00-msnd.mpk","snd_thr00.scd"],"battle":true},
    {"id":40300,"base":4030000,"slots":6,"name":"snd_for00","group":"stage","path":"map/maparc_for00_2.mpk","member":["map_se_for00-msnd.mpk","snd_for00.scd"],"battle":true},
    {"id":40400,"base":4040000,"slots":6,"name":"snd_fiv00","group":"stage","path":"map/maparc_fiv00_2.mpk","member":["map_se_fiv00-msnd.mpk","snd_fiv00.scd"],"battle":true},
    {"id":40500,"base":4050000,"slots":11,"name":"snd_six00","group":"stage","path":"map/maparc_six00_2.mpk","member":["map_se_six00-msnd.mpk","snd_six00.scd"],"battle":true},
    {"id":40600,"base":4060000,"slots":8,"name":"snd_sev00","group":"stage","path":"map/maparc_sev00_2.mpk","member":["map_se_sev00-msnd.mpk","snd_sev00.scd"],"battle":true},
    {"id":40700,"base":4070000,"slots":42,"name":"snd_eht00","group":"stage","path":"map/maparc_eht00_2.mpk","member":["map_se_eht00-msnd.mpk","snd_eht00.scd"],"battle":true},
    {"id":40800,"base":4080000,"slots":13,"name":"snd_nin00","group":"stage","path":"map/maparc_nin00_2.mpk","member":["map_se_nin00-msnd.mpk","snd_nin00.scd"],"battle":true},
    {"id":40900,"base":4090000,"slots":5,"name":"snd_ten00","group":"stage","path":"map/maparc_ten00_2.mpk","member":["map_se_ten00-msnd.mpk","snd_ten00.scd"],"battle":true},
    {"id":41200,"base":4120000,"slots":2,"name":"snd_org00","group":"stage","path":"map/maparc_org00_2.mpk","member":["map_se_org00-msnd.mpk","snd_org00.scd"],"battle":true},
    {"id":41300,"base":4130000,"slots":4,"name":"snd_org50","group":"stage","path":"map/maparc_org50_2.mpk","member":["map_se_org50-msnd.mpk","snd_org50.scd"],"battle":true},
    {"id":50000,"base":5000000,"slots":28,"name":"snd_stone","group":"floor","path":"map/maparc_one00_2.mpk","member":["map_se_one00-msnd.mpk","snd_stone.scd"],"battle":true},
    {"id":50100,"base":5010000,"slots":28,"name":"snd_bright_stone","group":"floor","path":"map/maparc_nin00_2.mpk","member":["map_se_nin00-msnd.mpk","snd_bright_stone.scd"],"battle":true},
    {"id":50200,"base":5020000,"slots":28,"name":"snd_rock","group":"floor","path":"map/maparc_for00_2.mpk","member":["map_se_for00-msnd.mpk","snd_rock.scd"],"battle":true},
    {"id":50300,"base":5030000,"slots":28,"name":"snd_sand","group":"floor","path":"map/maparc_wld01_1.mpk","member":["map_se_wld01-msnd.mpk","snd_sand.scd"],"battle":true},
    {"id":50400,"base":5040000,"slots":28,"name":"snd_wood","group":"floor","path":"map/maparc_eht00_2.mpk","member":["map_se_eht00-msnd.mpk","snd_wood.scd"],"battle":true},
    {"id":50500,"base":5050000,"slots":28,"name":"snd_grass","group":"floor","path":"map/maparc_fiv00_2.mpk","member":["map_se_fiv00-msnd.mpk","snd_grass.scd"],"battle":true},
    {"id":50600,"base":5060000,"slots":28,"name":"snd_steel","group":"floor","path":"map/maparc_eht00_2.mpk","member":["map_se_eht00-msnd.mpk","snd_steel.scd"],"battle":true},
    {"id":50700,"base":5070000,"slots":28,"name":"snd_wire_net","group":"floor","path":"map/maparc_six00_2.mpk","member":["map_se_six00-msnd.mpk","snd_wire_net.scd"],"battle":true},
    {"id":50900,"base":5090000,"slots":28,"name":"snd_cloth","group":"floor","path":"map/maparc_one00_2.mpk","member":["map_se_one00-msnd.mpk","snd_cloth.scd"],"battle":true},
    {"id":51000,"base":5100000,"slots":28,"name":"snd_magic_floor","group":"floor","path":"map/maparc_thr00_2.mpk","member":["map_se_thr00-msnd.mpk","snd_magic_floor.scd"],"battle":true},
    {"id":51100,"base":5110000,"slots":28,"name":"snd_orgamic","group":"floor","path":"map/maparc_two00_2.mpk","member":["map_se_two00-msnd.mpk","snd_orgamic.scd"],"battle":true},
    {"id":51400,"base":5140000,"slots":28,"name":"snd_petrifaction","group":"floor","path":"map/maparc_two00_2.mpk","member":["map_se_two00-msnd.mpk","snd_petrifaction.scd"],"battle":true},
    {"id":51500,"base":5150000,"slots":28,"name":"snd_tiling_red","group":"floor","path":"map/maparc_two00_2.mpk","member":["map_se_two00-msnd.mpk","snd_tiling_red.scd"],"battle":true},
    {"id":51700,"base":5170000,"slots":28,"name":"snd_clear_magic_floor","group":"floor","path":"map/maparc_thr00_2.mpk","member":["map_se_thr00-msnd.mpk","snd_clear_magic_floor.scd"],"battle":true},
    {"id":51900,"base":5190000,"slots":28,"name":"snd_black_cloud","group":"floor","path":"map/maparc_thr00_2.mpk","member":["map_se_thr00-msnd.mpk","snd_black_cloud.scd"],"battle":true},
    {"id":52200,"base":5220000,"slots":28,"name":"snd_glass","group":"floor","path":"map/maparc_ele00_2.mpk","member":["map_se_ele00-msnd.mpk","snd_glass.scd"],"battle":true},
    {"id":52300,"base":5230000,"slots":28,"name":"snd_floating_stone","group":"floor","path":"map/maparc_sev00_2.mpk","member":["map_se_sev00-msnd.mpk","snd_floating_stone.scd"],"battle":true},
    {"id":52700,"base":5270000,"slots":28,"name":"snd_black_stone","group":"floor","path":"map/maparc_nin00_2.mpk","member":["map_se_nin00-msnd.mpk","snd_black_stone.scd"],"battle":true},
    {"id":52800,"base":5280000,"slots":28,"name":"snd_white_stone","group":"floor","path":"map/maparc_org50_2.mpk","member":["map_se_org50-msnd.mpk","snd_white_stone.scd"],"battle":true},
    {"id":52900,"base":5290000,"slots":28,"name":"snd_water","group":"floor","path":"map/maparc_fiv00_2.mpk","member":["map_se_fiv00-msnd.mpk","snd_water.scd"],"battle":true},
    {"id":60300,"base":6030000,"slots":4,"name":"snd_ptc_RERAISE","group":"party","path":"general_archive/party_command/reraise.bin","member":["snd_ptc_RERAISE.scd"],"battle":true},
    {"id":60500,"base":6050000,"slots":3,"name":"snd_ptc_HOLD","group":"party","path":"general_archive/party_command/hold.bin","member":["snd_ptc_HOLD.scd"],"battle":true},
    {"id":60700,"base":6070000,"slots":3,"name":"snd_ptc_COUNTUP","group":"party","path":"general_archive/party_command/countup.bin","member":["snd_ptc_COUNTUP.scd"],"battle":true},
    {"id":60900,"base":6090000,"slots":3,"name":"snd_ptc_REPRESS","group":"party","path":"general_archive/party_command/repress.bin","member":["snd_ptc_REPRESS.scd"],"battle":true},
    {"id":61100,"base":6110000,"slots":6,"name":"snd_ptc_HALF","group":"party","path":"general_archive/party_command/half.bin","member":["snd_ptc_HALF.scd"],"battle":true},
    {"id":61300,"base":6130000,"slots":4,"name":"snd_ptc_SILENCE","group":"party","path":"general_archive/party_command/silence.bin","member":["snd_ptc_SILENCE.scd"],"battle":true},
    {"id":61500,"base":6150000,"slots":3,"name":"snd_ptc_COUNTDOWN","group":"party","path":"general_archive/party_command/countdown.bin","member":["snd_ptc_COUNTDOWN.scd"],"battle":true},
    {"id":61700,"base":6170000,"slots":4,"name":"snd_ptc_COPY","group":"party","path":"general_archive/party_command/copy.bin","member":["snd_ptc_COPY.scd"],"battle":true},
    {"id":61900,"base":6190000,"slots":5,"name":"snd_ptc_PASTE","group":"party","path":"general_archive/party_command/paste.bin","member":["snd_ptc_PASTE.scd"],"battle":true},
    {"id":62100,"base":6210000,"slots":5,"name":"snd_ptc_BREAK","group":"party","path":"general_archive/party_command/break.bin","member":["snd_ptc_BREAK.scd"],"battle":true},
    {"id":62300,"base":6230000,"slots":4,"name":"snd_ptc_UP","group":"party","path":"general_archive/party_command/up.bin","member":["snd_ptc_UP.scd"],"battle":true},
    {"id":62500,"base":6250000,"slots":4,"name":"snd_ptc_T_ZERO","group":"party","path":"general_archive/party_command/t_zero.bin","member":["snd_ptc_T_ZERO.scd"],"battle":true},
    {"id":62600,"base":6260000,"slots":3,"name":"snd_ptc_T_HOLD","group":"party","path":"general_archive/party_command/t_hold.bin","member":["snd_ptc_T_HOLD.scd"],"battle":true},
    {"id":62700,"base":6270000,"slots":4,"name":"snd_ptc_T_TRADE","group":"party","path":"general_archive/party_command/t_trade.bin","member":["snd_ptc_T_TRADE.scd"],"battle":true},
    {"id":62800,"base":6280000,"slots":4,"name":"snd_ptc_T_UP","group":"party","path":"general_archive/party_command/t_up.bin","member":["snd_ptc_T_UP.scd"],"battle":true},
    {"id":62900,"base":6290000,"slots":4,"name":"snd_ptc_C_ZERO","group":"party","path":"general_archive/party_command/c_zero.bin","member":["snd_ptc_C_ZERO.scd"],"battle":true},
    {"id":63000,"base":6300000,"slots":3,"name":"snd_ptc_C_HOLD","group":"party","path":"general_archive/party_command/c_hold.bin","member":["snd_ptc_C_HOLD.scd"],"battle":true},
    {"id":63100,"base":6310000,"slots":4,"name":"snd_ptc_C_TRADE","group":"party","path":"general_archive/party_command/c_trade.bin","member":["snd_ptc_C_TRADE.scd"],"battle":true},
    {"id":63200,"base":6320000,"slots":4,"name":"snd_ptc_C_UP","group":"party","path":"general_archive/party_command/c_up.bin","member":["snd_ptc_C_UP.scd"],"battle":true},
    {"id":63300,"base":6330000,"slots":4,"name":"snd_ptc_S_UP","group":"party","path":"general_archive/party_command/s_up.bin","member":["snd_ptc_S_UP.scd"],"battle":true},
    {"id":63400,"base":6340000,"slots":6,"name":"snd_ptc_S_DRAIN","group":"party","path":"general_archive/party_command/s_drain.bin","member":["snd_ptc_S_DRAIN.scd"],"battle":true},
    {"id":63500,"base":6350000,"slots":4,"name":"snd_ptc_S_DOWN","group":"party","path":"general_archive/party_command/s_down.bin","member":["snd_ptc_S_DOWN.scd"],"battle":true},
    {"id":63600,"base":6360000,"slots":6,"name":"snd_ptc_RANDOM_PTC","group":"party","path":"general_archive/party_command/random_ptc.bin","member":["snd_ptc_RANDOM_PTC.scd"],"battle":true},
    {"id":63700,"base":6370000,"slots":7,"name":"snd_ptc_SHAFFLE","group":"party","path":"general_archive/party_command/shaffle.bin","member":["snd_ptc_SHAFFLE.scd"],"battle":true},
    {"id":63900,"base":6390000,"slots":6,"name":"snd_ptc_BOMB","group":"party","path":"general_archive/party_command/bomb.bin","member":["snd_ptc_BOMB.scd"],"battle":true},
    {"id":64000,"base":6400000,"slots":5,"name":"snd_ptc_BLOW","group":"party","path":"general_archive/party_command/blow.bin","member":["snd_ptc_BLOW.scd"],"battle":true},
    {"id":64100,"base":6410000,"slots":4,"name":"snd_ptc_BLIND","group":"party","path":"general_archive/party_command/blind.bin","member":["snd_ptc_BLIND.scd"],"battle":true},
    {"id":64200,"base":6420000,"slots":3,"name":"snd_ptc_ADD_LOSTHP","group":"party","path":"general_archive/party_command/add_losthp.bin","member":["snd_ptc_ADD_LOSTHP.scd"],"battle":true},
    {"id":64300,"base":6430000,"slots":7,"name":"snd_ptc_BREAK_FIVE","group":"party","path":"general_archive/party_command/break_five.bin","member":["snd_ptc_BREAK_FIVE.scd"],"battle":true},
    {"id":64400,"base":6440000,"slots":6,"name":"snd_ptc_THOUSAND","group":"party","path":"general_archive/party_command/thousand.bin","member":["snd_ptc_THOUSAND.scd"],"battle":true},
    {"id":64500,"base":6450000,"slots":3,"name":"snd_ptc_COUNTDOWN_DISTANCE","group":"party","path":"general_archive/party_command/countdown_distance.bin","member":["snd_ptc_COUNTDOWN_DISTANCE.scd"],"battle":true},
    {"id":64600,"base":6460000,"slots":4,"name":"snd_ptc_UP_AND_COUNTDOWN","group":"party","path":"general_archive/party_command/up_and_countdown.bin","member":["snd_ptc_UP_AND_COUNTDOWN.scd"],"battle":true},
    {"id":64700,"base":6470000,"slots":5,"name":"snd_ptc_DOWN_AND_COUNTUP","group":"party","path":"general_archive/party_command/down_and_countup.bin","member":["snd_ptc_DOWN_AND_COUNTUP.scd"],"battle":true},
    {"id":64800,"base":6480000,"slots":3,"name":"snd_ptc_REGENE","group":"party","path":"general_archive/party_command/regene.bin","member":["snd_ptc_REGENE.scd"],"battle":true},
    {"id":64900,"base":6490000,"slots":2,"name":"snd_ptc_UP_OR_DOWN","group":"party","path":"general_archive/party_command/up_or_down.bin","member":["snd_ptc_UP_OR_DOWN.scd"],"battle":true},
    {"id":65000,"base":6500000,"slots":4,"name":"snd_ptc_ULTIMA_WEAPON","group":"party","path":"general_archive/party_command/ultima_weapon.bin","member":["snd_ptc_ULTIMA_WEAPON.scd"],"battle":true},
    {"id":65100,"base":6510000,"slots":7,"name":"snd_ptc_OMEGA","group":"party","path":"general_archive/party_command/omega.bin","member":["snd_ptc_OMEGA.scd"],"battle":true},
    {"id":65200,"base":6520000,"slots":5,"name":"snd_ptc_DISTANCE_FAR","group":"party","path":"general_archive/party_command/distance_far.bin","member":["snd_ptc_DISTANCE_FAR.scd"],"battle":true},
    {"id":65300,"base":6530000,"slots":4,"name":"snd_ptc_BREAK_RELEASE","group":"party","path":"general_archive/party_command/break_release.bin","member":["snd_ptc_BREAK_RELEASE.scd"],"battle":true},
    {"id":65400,"base":6540000,"slots":4,"name":"snd_ptc_SHARE_ABSORB","group":"party","path":"general_archive/party_command/share_absorb.bin","member":["snd_ptc_SHARE_ABSORB.scd"],"battle":true},
    {"id":65500,"base":6550000,"slots":4,"name":"snd_ptc_ERACE","group":"party","path":"general_archive/party_command/erace.bin","member":["snd_ptc_ERACE.scd"],"battle":true},
    {"id":65600,"base":6560000,"slots":3,"name":"snd_ptc_SHARE_GET","group":"party","path":"general_archive/party_command/share_get.bin","member":["snd_ptc_SHARE_GET.scd"],"battle":true},
    {"id":65700,"base":6570000,"slots":7,"name":"snd_ptc_BREAK_9999","group":"party","path":"general_archive/party_command/break_9999.bin","member":["snd_ptc_BREAK_9999.scd"],"battle":true},
    {"id":65800,"base":6580000,"slots":4,"name":"snd_ptc_SHARE_DOUBLE","group":"party","path":"general_archive/party_command/share_double.bin","member":["snd_ptc_SHARE_DOUBLE.scd"],"battle":true},
    {"id":65900,"base":6590000,"slots":1,"name":"snd_ptc_SHARE_ZERO","group":"party","path":"general_archive/party_command/share_zero.bin","member":["snd_ptc_SHARE_ZERO.scd"],"battle":true},
    {"id":66000,"base":6600000,"slots":0,"name":"snd_ptc_DESPERADO_CHAOS","group":"party","path":"general_archive/party_command/desperado_chaos.bin","member":["snd_ptc_DESPERADO_CHAOS.scd"],"battle":true},
    {"id":70000,"base":7000000,"slots":50,"name":"snd_rt","group":"rt","path":"sound/snd_rt.scd","member":[],"battle":"unknown"},
    {"id":210000,"base":21000000,"slots":56,"name":"p_for110","group":"character","path":"obj/p_for110.objx","member":["snd_"],"battle":true},
    {"id":210100,"base":21010000,"slots":51,"name":"voice_p_for110_00","group":"voice","path":"voice/en/battle/voice_p_for110_00.scd","member":[],"battle":true},
    {"id":210500,"base":21050000,"slots":56,"name":"p_for110_a","group":"character","path":"obj/p_for110_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_for110_w","path":"obj/p_for110_w.objx","member":["snd_"],"slots":56}]},
    {"id":210600,"base":21060000,"slots":51,"name":"voice_p_for110_a_00","group":"voice","path":"voice/en/battle/voice_p_for110_a_00.scd","member":[],"battle":true},
    {"id":211000,"base":21100000,"slots":95,"name":"p_fiv210","group":"character","path":"obj/p_fiv210.objx","member":["snd_"],"battle":true},
    {"id":211100,"base":21110000,"slots":44,"name":"voice_p_fiv210_00","group":"voice","path":"voice/en/battle/voice_p_fiv210_00.scd","member":[],"battle":true},
    {"id":211500,"base":21150000,"slots":85,"name":"p_fiv210_a","group":"character","path":"obj/p_fiv210_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_fiv210_w","path":"obj/p_fiv210_w.objx","member":["snd_"],"slots":85}]},
    {"id":211600,"base":21160000,"slots":41,"name":"voice_p_fiv210_a_00","group":"voice","path":"voice/en/battle/voice_p_fiv210_a_00.scd","member":[],"battle":true},
    {"id":212000,"base":21200000,"slots":54,"name":"p_sev110","group":"character","path":"obj/p_sev110.objx","member":["snd_"],"battle":true},
    {"id":212100,"base":21210000,"slots":48,"name":"voice_p_sev110_00","group":"voice","path":"voice/en/battle/voice_p_sev110_00.scd","member":[],"battle":true},
    {"id":212500,"base":21250000,"slots":46,"name":"p_sev110_a","group":"character","path":"obj/p_sev110_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_sev110_w","path":"obj/p_sev110_w.objx","member":["snd_"],"slots":46}]},
    {"id":212600,"base":21260000,"slots":49,"name":"voice_p_sev110_a_00","group":"voice","path":"voice/en/battle/voice_p_sev110_a_00.scd","member":[],"battle":true},
    {"id":213000,"base":21300000,"slots":65,"name":"p_eht110","group":"character","path":"obj/p_eht110.objx","member":["snd_"],"battle":true},
    {"id":213100,"base":21310000,"slots":42,"name":"voice_p_eht110_00","group":"voice","path":"voice/en/battle/voice_p_eht110_00.scd","member":[],"battle":true},
    {"id":213500,"base":21350000,"slots":66,"name":"p_eht110_a","group":"character","path":"obj/p_eht110_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_eht110_w","path":"obj/p_eht110_w.objx","member":["snd_"],"slots":66}]},
    {"id":213600,"base":21360000,"slots":42,"name":"voice_p_eht110_a_00","group":"voice","path":"voice/en/battle/voice_p_eht110_a_00.scd","member":[],"battle":true},
    {"id":214000,"base":21400000,"slots":141,"name":"p_ten110","group":"character","path":"obj/p_ten110.objx","member":["snd_"],"battle":true},
    {"id":214100,"base":21410000,"slots":38,"name":"voice_p_ten110_00","group":"voice","path":"voice/en/battle/voice_p_ten110_00.scd","member":[],"battle":true},
    {"id":214500,"base":21450000,"slots":140,"name":"p_ten110_a","group":"character","path":"obj/p_ten110_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_ten110_w","path":"obj/p_ten110_w.objx","member":["snd_"],"slots":140}]},
    {"id":214600,"base":21460000,"slots":38,"name":"voice_p_ten110_a_00","group":"voice","path":"voice/en/battle/voice_p_ten110_a_00.scd","member":[],"battle":true},
    {"id":215000,"base":21500000,"slots":79,"name":"p_gst110","group":"character","path":"obj/p_gst110.objx","member":["snd_"],"battle":true},
    {"id":215100,"base":21510000,"slots":55,"name":"voice_p_gst110_00","group":"voice","path":"voice/en/battle/voice_p_gst110_00.scd","member":[],"battle":true},
    {"id":215400,"base":21540000,"slots":55,"name":"voice_p_gst110_01","group":"voice","path":"voice/en/battle/voice_p_gst110_01.scd","member":[],"battle":true},
    {"id":215500,"base":21550000,"slots":72,"name":"p_gst110_a","group":"character","path":"obj/p_gst110_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_gst110_w","path":"obj/p_gst110_w.objx","member":["snd_"],"slots":72}]},
    {"id":215600,"base":21560000,"slots":54,"name":"voice_p_gst110_a_00","group":"voice","path":"voice/en/battle/voice_p_gst110_a_00.scd","member":[],"battle":true},
    {"id":216000,"base":21600000,"slots":93,"name":"p_gst120","group":"character","path":"obj/p_gst120.objx","member":["snd_"],"battle":true},
    {"id":216100,"base":21610000,"slots":56,"name":"voice_p_gst120_00","group":"voice","path":"voice/en/battle/voice_p_gst120_00.scd","member":[],"battle":true},
    {"id":216500,"base":21650000,"slots":73,"name":"p_gst120_a","group":"character","path":"obj/p_gst120_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_gst120_w","path":"obj/p_gst120_w.objx","member":["snd_"],"slots":73}]},
    {"id":216600,"base":21660000,"slots":57,"name":"voice_p_gst120_a_00","group":"voice","path":"voice/en/battle/voice_p_gst120_a_00.scd","member":[],"battle":true},
    {"id":217000,"base":21700000,"slots":72,"name":"p_gst130","group":"character","path":"obj/p_gst130.objx","member":["snd_"],"battle":true},
    {"id":217100,"base":21710000,"slots":47,"name":"voice_p_gst130_00","group":"voice","path":"voice/en/battle/voice_p_gst130_00.scd","member":[],"battle":true},
    {"id":217500,"base":21750000,"slots":71,"name":"p_gst130_a","group":"character","path":"obj/p_gst130_a.objx","member":["snd_"],"battle":true,"variants":[{"name":"p_gst130_w","path":"obj/p_gst130_w.objx","member":["snd_"],"slots":71}]},
    {"id":217600,"base":21760000,"slots":46,"name":"voice_p_gst130_a_00","group":"voice","path":"voice/en/battle/voice_p_gst130_a_00.scd","member":[],"battle":true},
    {"id":218000,"base":21800000,"slots":98,"name":"p_org210","group":"character","path":"obj/p_org210.objx","member":["snd_"],"battle":true},
    {"id":218100,"base":21810000,"slots":27,"name":"voice_p_org210_00","group":"voice","path":"voice/en/battle/voice_p_org210_00.scd","member":[],"battle":true},
    {"id":219500,"base":21950000,"slots":19,"name":"p_sev120_a","group":"character","path":"obj/p_sev120_a.objx","member":["snd_"],"battle":true},
    {"id":219600,"base":21960000,"slots":7,"name":"voice_p_sev120_a_00","group":"voice","path":"voice/en/battle/voice_p_sev120_a_00.scd","member":[],"battle":true},
    {"id":240000,"base":24000000,"slots":4,"name":"snd_ele00","group":"stage","path":"map/maparc_ele00_2.mpk","member":["map_se_ele00-msnd.mpk","snd_ele00.scd"],"battle":true},
    {"id":240100,"base":24010000,"slots":8,"name":"snd_twe00","group":"stage","path":"map/maparc_twe00_2.mpk","member":["map_se_twe00-msnd.mpk","snd_twe00.scd"],"battle":true},
    {"id":240200,"base":24020000,"slots":12,"name":"snd_thn00","group":"stage","path":"map/maparc_thn00_2.mpk","member":["map_se_thn00-msnd.mpk","snd_thn00.scd"],"battle":true},
    {"id":240300,"base":24030000,"slots":4,"name":"snd_vsi00","group":"stage","path":"map/maparc_vsi00_2.mpk","member":["map_se_vsi00-msnd.mpk","snd_vsi00.scd"],"battle":true},
    {"id":240400,"base":24040000,"slots":3,"name":"snd_vni00","group":"stage","path":"map/maparc_vni00_2.mpk","member":["map_se_vni00-msnd.mpk","snd_vni00.scd"],"battle":true},
    {"id":240500,"base":24050000,"slots":6,"name":"snd_vth00","group":"stage","path":"map/maparc_vth00_2.mpk","member":["map_se_vth00-msnd.mpk","snd_vth00.scd"],"battle":true},
    {"id":240600,"base":24060000,"slots":5,"name":"snd_vtw00","group":"stage","path":"map/maparc_vtw00_2.mpk","member":["map_se_vtw00-msnd.mpk","snd_vtw00.scd"],"battle":true},
    {"id":240700,"base":24070000,"slots":4,"name":"snd_wld01","group":"stage","path":"map/maparc_wld01_1.mpk","member":["map_se_wld01-msnd.mpk","snd_wld01.scd"],"battle":true},
    {"id":240800,"base":24080000,"slots":9,"name":"snd_wld02","group":"stage","path":"map/maparc_wld02_1.mpk","member":["map_se_wld02-msnd.mpk","snd_wld02.scd"],"battle":true},
    {"id":240900,"base":24090000,"slots":11,"name":"snd_wld03","group":"stage","path":"map/maparc_wld03_1.mpk","member":["map_se_wld03-msnd.mpk","snd_wld03.scd"],"battle":true},
    {"id":241000,"base":24100000,"slots":6,"name":"snd_wld04","group":"stage","path":"map/maparc_wld04_1.mpk","member":["map_se_wld04-msnd.mpk","snd_wld04.scd"],"battle":true},
    {"id":241100,"base":24110000,"slots":4,"name":"snd_wld05","group":"stage","path":"map/maparc_wld05_1.mpk","member":["map_se_wld05-msnd.mpk","snd_wld05.scd"],"battle":true},
    {"id":241200,"base":24120000,"slots":7,"name":"snd_wld06","group":"stage","path":"map/maparc_wld06_1.mpk","member":["map_se_wld06-msnd.mpk","snd_wld06.scd"],"battle":true},
    {"id":241300,"base":24130000,"slots":6,"name":"snd_wld07","group":"stage","path":"map/maparc_wld07_1.mpk","member":["map_se_wld07-msnd.mpk","snd_wld07.scd"],"battle":true}
  ];

  var GROUPS = {
    common: 'resident system SE (main.bin; always loaded)',
    battle: 'common battle SE (map_se_<map>-msnd.mpk of the stage; identical on every stage)',
    stage: 'stage SE (snd_<map>.scd of the stage being fought on)',
    floor: 'floor-material SE (footsteps / landings, picked by map_sound_param.bin; op 0x060F)',
    character: "a fighter's own bank (objx 'snd_', sndbank.js); listed for completeness",
    voice: 'battle voice bank (en)',
    party: 'party-command SE (general_archive/party_command/<cmd>.bin)',
    menu: 'menu SE (main.bin)', region: 'world map / region SE', rt: 'sound/snd_rt.scd (use not traced)'
  };

  var byId = {};
  BANKS.forEach(function (b) { byId[b.id] = b; });

  function banks(filter) {
    filter = filter || {};
    return BANKS.filter(function (b) {
      if (filter.battle && b.battle !== true) return false;
      if (filter.group && [].concat(filter.group).indexOf(b.group) < 0) return false;
      if (filter.shared && b.group === 'character') return false;
      return true;
    });
  }
  // PACKAGE.BIN paths to read. Default: what a Move Studio sound picker needs, the two banks every battle has
  // (snd_common in main.bin, snd_battle in map/maparc_one00_2.mpk). {all:true} = every bank; any banks() filter works
  // too, e.g. {group:['stage','floor']} (one file per stage) or {battle:true, shared:true} (adds voices, party commands).
  function bankFiles(filter) {
    var list = filter && filter.all ? BANKS : banks(filter || { group: ['common', 'battle'] });
    var seen = {}, out = [];
    list.forEach(function (b) { if (!seen[b.path]) { seen[b.path] = 1; out.push(b.path); } });
    return out;
  }
  function cueLocation(cue) {
    cue = cue >>> 0;
    for (var i = 0; i < BANKS.length; i++) {
      var b = BANKS[i];
      if (cue >= b.base && cue < b.base + b.slots)
        return { cue: cue, id: b.id, base: b.base, slot: cue - b.base, name: b.name, group: b.group, path: b.path,
                 member: b.member.slice(), battle: b.battle, variants: b.variants || null };
    }
    return null;
  }
  function cueOf(id, slot) { return 100 * id + slot; }

  // engine base of a bank header (the u32@0x14 form is 100 * value too; sndbank.cueBase returns it raw)
  function headerBase(u8) {
    var v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    var k = (v.getUint16(0x10, true) - 1) * 10000, id = v.getUint16(0x12, true), w = v.getUint32(0x14, true);
    return id ? k + 100 * id : (w >= 600 ? 100 * w : k);
  }
  function headerId(u8) {
    var v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    return v.getUint16(0x12, true) || v.getUint32(0x14, true);
  }

  function str4(u8, o) { return String.fromCharCode(u8[o], u8[o + 1], u8[o + 2], u8[o + 3]); }
  function cstr(u8, o) { var s = ''; while (u8[o]) s += String.fromCharCode(u8[o++]); return s; }
  // members of an MPK ("MPK " 06 06 07 20, u32 n @8, 16-byte {name offset, offset, size, 0}) or ARC (ARC\1, u32 n @4,
  // 16-byte {type, tag, offset, size}); offsets relative to the container start
  function members(u8) {
    var v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), out = [], n, i, o;
    if (str4(u8, 0) === 'MPK ') {
      n = v.getUint32(8, true);
      for (i = 0; i < n; i++) { o = 16 + 16 * i; out.push({ name: cstr(u8, v.getUint32(o, true)), offset: v.getUint32(o + 4, true), size: v.getUint32(o + 8, true) }); }
    } else if (str4(u8, 0) === 'ARC\x01') {
      n = v.getUint32(4, true);
      for (i = 0; i < n; i++) { o = 16 + 16 * i; out.push({ name: str4(u8, o + 4), type: v.getUint32(o, true), offset: v.getUint32(o + 8, true), size: v.getUint32(o + 12, true) }); }
    } else throw new Error('not an MPK / ARC container');
    return out;
  }
  // SSCF bytes of a bank from its PACKAGE.BIN file (member = container chain). Returns a copy whose u32@0x08 size
  // field matches the data: snd_battle.scd (436580 vs 321888) and snd_menu.scd (362615 vs 339440) carry stale sizes
  // that sndbank.parseBank rejects; all slots lie inside the real data.
  function extract(fileU8, member) {
    var cur = fileU8;
    (member || []).forEach(function (name) {
      var k = name.indexOf('#') > 0 ? +name.slice(name.indexOf('#') + 1) : 0;   // 'snd_#1' = the 2nd member of that name
      if (k) name = name.slice(0, name.indexOf('#'));
      var m = members(cur).filter(function (e) { return e.name === name; })[k];
      if (!m) throw new Error('member ' + name + ' not found');
      cur = cur.subarray(m.offset, m.offset + m.size);
    });
    if (str4(cur, 0) !== 'SSCF') throw new Error('not an SSCF bank');
    var out = new Uint8Array(cur);
    new DataView(out.buffer).setUint32(8, out.length, true);
    return out;
  }
  function parse(sscfU8) { return SB.parseBank(extract(sscfU8, [])); }   // a bare .scd (size field fixed on a copy)

  var caches = typeof WeakMap !== 'undefined' ? new WeakMap() : null;
  function cacheFor(getFile) {
    if (!caches) return {};
    var c = caches.get(getFile); if (!c) { c = { files: {}, banks: {} }; caches.set(getFile, c); } return c;
  }
  // bank: an id, a BANKS record or a cueLocation result; getFile(path) -> Promise<Uint8Array>
  function loadBank(bank, getFile) {
    var rec = typeof bank === 'number' ? byId[bank] : bank;
    if (!rec) return Promise.reject(new Error('unknown bank ' + bank));
    var c = cacheFor(getFile), key = rec.path + '|' + rec.member.join('/');
    if (c.banks && c.banks[key]) return c.banks[key];
    var p = Promise.resolve(c.files && c.files[rec.path] || (c.files[rec.path] = Promise.resolve(getFile(rec.path))))
      .then(function (u8) { var b = SB.parseBank(extract(u8 instanceof Uint8Array ? u8 : new Uint8Array(u8), rec.member)); b.shared = rec; return b; });
    if (c.banks) c.banks[key] = p;
    return p;
  }
  // a random-group slot (byte +03 bit 0x10; sndbank calls it kind 'link'): the engine plays one of the `count` slots
  // from +04 at random (FUN_08991928 via FUN_0899136C). Returns [first, count] or null.
  function randomGroup(slot) {
    var r = slot.raw; if (!r || !(r[3] & 0x10)) return null;
    var v = new DataView(r.buffer, r.byteOffset, r.byteLength);
    return [v.getUint32(4, true), v.getUint32(8, true)];
  }
  // opts.pick: for a random group, the member to play (index 0.., or 'random'); default 0 (= what sndbank decodes)
  function decodeCue(cue, getFile, opts) {
    var loc = cueLocation(cue);
    if (!loc) return Promise.reject(new Error('cue ' + cue + ' is in no known bank'));
    return loadBank(byId[loc.id], getFile).then(function (bank) {
      var s = bank.slots[loc.slot], slot = loc.slot, g = s && randomGroup(s);
      if (!s || s.kind === 'empty') throw new Error('cue ' + cue + ' (' + loc.name + ' slot ' + loc.slot + ') is an empty slot');
      if (g) { var k = opts && opts.pick === 'random' ? Math.floor(Math.random() * g[1]) : Math.min(opts && opts.pick || 0, g[1] - 1); slot = g[0] + k; }
      var d = SB.decodeSlot(bank, slot); d.location = loc; d.slotInfo = s; d.played = slot; d.randomGroup = g; return d;
    });
  }
  function cueWav(cue, getFile, opts) { return decodeCue(cue, getFile, opts).then(function (d) { return SB.writeWav(d); }); }
  // (sndbank's export `encodeWav` is readWav by mistake, hence writeWav above)
  // non-empty slots of a bank as picker rows {cue, slot, kind, owner, rate, seconds, volume, loop}
  function listCues(bank) {
    var base = bank.shared ? bank.shared.base : headerBase(bank.header);
    return bank.slots.filter(function (s) { return s.kind !== 'empty'; }).map(function (s) {
      var g = randomGroup(s);
      return { cue: base + s.index, slot: s.index, kind: g ? 'random' : s.kind, owner: s.owner, rate: s.rate, seconds: s.seconds,
               volume: s.volume, loop: !!s.loop, choices: g ? Array.from({ length: g[1] }, function (_, i) { return g[0] + i; }) : null };
    });
  }

  // PACKAGE.BIN path hash (CRC-32, poly 0x04C11DB7, MSB first, lower-cased path; = package_extract.py, = retail.js dhash)
  var T = (function () { var t = [], i, k, c; for (i = 0; i < 256; i++) { c = i << 24; for (k = 0; k < 8; k++) c = c & 0x80000000 ? (c << 1) ^ 0x04C11DB7 : c << 1; t.push(c >>> 0); } return t; })();
  function pathHash(path) {
    var c = 0xFFFFFFFF, s = path.toLowerCase().replace(/\\/g, '/');
    for (var i = 0; i < s.length; i++) c = ((c << 8) ^ T[((c >>> 24) ^ s.charCodeAt(i)) & 255]) >>> 0;
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  var API = {
    BANKS: BANKS, GROUPS: GROUPS, banks: banks, bankFiles: bankFiles, bank: function (id) { return byId[id] || null; },
    cueLocation: cueLocation, cueOf: cueOf, headerBase: headerBase, headerId: headerId,
    members: members, extract: extract, parse: parse, loadBank: loadBank, decodeCue: decodeCue, cueWav: cueWav,
    listCues: listCues, randomGroup: randomGroup, pathHash: pathHash
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = API;
  else root.SharedSnd = API;
})(typeof self !== 'undefined' ? self : this);
