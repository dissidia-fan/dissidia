/* Move Studio: the attacker's own movement during a move, for the dummy (docs/archive/attacker-movement/README.md §8,
   ported as written there). Per tick: keyed events (they still see last tick's flags), the flag block cleared, ranged
   events (turning, lunges, slides, chase), the movement state (homing dash 0xF, chase, free fall), root motion (the
   change of the animated trans_include bone, turned by the actor's yaw), then the floor and landing.
   window.ACT = { SPEC, specOf, makeActor, startMove, stepActor, evArgs }. Uses dummy.js's vector helpers. */
window.ACT = (() => {
  const SPEC = {                                 // battle.bin spec records (h, T, P60, P96, P148)
    1: { h: 3.5, T: 48, P60: 2.5, P96: -0.6, P148: 0.95 },       // WoL
    6: { h: 4.3, T: 32, P60: 1.1, P96: -0.6, P148: 0.95 },       // CoD
    9: { h: 2.5, T: 34, P60: 2.0, P96: -0.6, P148: 0.95 },       // Bartz, Noctis
    13: { h: 2.5, T: 36, P60: 2.2, P96: -0.6, P148: 0.95 },      // Cloud
    28: { h: 6.5, T: 44, P60: 2.2, P96: -0.6, P148: 0.80 },      // Yuna (Y'shtola)
  };
  const SPEC_OF = { one100: 1, thr200: 6, fiv100: 9, sev100: 13, ten110: 28 };
  const specOf = (code) => SPEC[SPEC_OF[code]] ? { id: SPEC_OF[code], ...SPEC[SPEC_OF[code]] } : { id: 9, guess: true, ...SPEC[9] };
  const D2R = Math.PI / 180, wrapPi = (a) => Math.atan2(Math.sin(a), Math.cos(a));
  const fwd = (yaw) => v3(Math.sin(yaw), 0, Math.cos(yaw));
  const flat = (v) => v3(v.x, 0, v.z);
  const fv = (x) => (x && typeof x === 'object' && 'f' in x ? x.f : x);

  /* a timeline event with its {"f": x} floats unwrapped and the op name resolved */
  function evArgs(e) {
    const out = { op: typeof e.op === 'string' && !e.op.startsWith('0x') ? e.op : ((STUDIO_OPS.ops[hex4(opCode(e.op))] || {}).name || e.op) };
    for (const [k, v] of Object.entries(e)) if (k !== 'op') out[k] = Array.isArray(v) ? v.map(fv) : fv(v);
    return out;
  }

  function makeActor(pos, yaw, spec, refY = 0.9) {
    return { pos: { ...pos }, yaw, spec, refY, state: 'ground', vel: v3(0, 0, 0), carry: v3(0, 0, 0), extra: v3(0, 0, 0),
      flags: {}, J: null, armed: null, fall: null, chase: null, root: null, landed: false, gravMul: 1 };
  }
  function startMove(a, ranged0) {
    const air = a.pos.y > 0.001 || ranged0.some((e) => e.op === 'air_motion');
    a.state = air ? 'air' : 'ground'; a.root = null; a.landed = false; a.fall = null;
  }
  function turnToward(a, dir, maxRad) {
    if (Math.hypot(dir.x, dir.z) <= 0.01) return;
    const d = wrapPi(Math.atan2(dir.x, dir.z) - a.yaw);
    a.yaw = wrapPi(a.yaw + Math.max(-maxRad, Math.min(maxRad, d)));
  }
  const isAir = (a) => !!a.flags.air_motion || (a.state === 'homing' && !a.J.ground) || a.state === 'chase';
  const airborne = (a) => isAir(a) || a.state === 'air' || a.state === 'fall';
  const refPt = (o) => v3(o.pos.x, o.pos.y + o.refY, o.pos.z);
  const aimPt = (o) => v3(o.pos.x, o.pos.y + (o.aimY ?? o.refY), o.pos.z);     // foe+0x110: kind-10 centre, else kind-1

  function startHoming(a, foe, p, ground) {
    const g = p.args || [];
    a.J = { ground, hs: p.speed, vs: ground ? 0 : (p.vspeed > 0 ? p.vspeed : p.speed), track: p.track_ticks,
      height: ground ? 0 : (p.height || 0), angle: (p.angle || 0) * D2R, dist: p.distance || 0,
      arrive: p.arrive_radius ?? -1, max: p.max_ticks || 0,
      noRise: !ground && !!g[0], noFall: !ground && !!g[1],
      skipChecks: !!g[ground ? 0 : 2], stopIfBusy: !!g[ground ? 1 : 3],
      dir: vNorm(flat(vSub(refPt(a), refPt(foe)))), dest: null, elapsed: 0, frozen: false, arrived: false, stopped: false };
    if (ground || airborne(a)) a.state = 'homing'; else a.armed = 'homing';
    a.log && a.log.push((ground ? 'ground homing dash' : 'homing dash') + (a.state === 'homing' ? '' : ' (armed)'));
  }
  function startOp(a, ev, foe) {
    switch (ev.op) {
      case 'homing_dash': if (foe) startHoming(a, foe, ev, false); break;
      case 'ground_homing_dash': if (foe) startHoming(a, foe, ev, true); break;
      case 'enter_state': if (a.armed === 'homing') { a.state = 'homing'; a.armed = null; } break;
      case 'take_off': if (!(ev.only_without_target && foe)) a.state = 'air'; break;
    }
  }
  function startFall(a) { a.state = 'fall'; a.fall = { y0: a.pos.y, t: 0 }; a.fallMotion = !a.flags.keep_motion_when_falling; }

  /* one tick; keyed / ranged = evArgs()-normalised events of this tick; root = trans_include {x, y, z, yaw} now */
  function stepActor(a, foe, keyed, ranged, root, rootHook) {
    a.extra = v3(0, 0, 0); a.gravMul = 1;
    for (const ev of keyed) startOp(a, ev, foe);
    a.flags = {};
    for (const ev of ranged) {
      const f = ev.args || [];
      switch (ev.op) {
        case 'air_motion': case 'ground_follow': case 'landing_allowed': case 'floating_physics':
        case 'keep_motion_when_falling': case 'no_body_collision': case 'root_motion_3d': a.flags[ev.op] = true; break;
        case 'stop_momentum': a.carry = v3(0, 0, 0); break;
        case 'gravity': a.gravMul = ev.multiplier ?? f[0]; break;
        case 'face_target': case 'turn_to_opponent': case 'turn_to_target': {
          if (!foe) break;
          const deg = ev.deg_per_tick ?? ev.max_deg_per_tick ?? f[0];
          // arg1 of face_target / turn_to_target picks FUN_088360BC: turn only while the foe is in a hit reaction
          // (500-798; event-ops G.md 0x046D). It is not "turn away": Cloud of Darkness's _620A {30,1,0,0} kept turning her back on the dummy
          const onlyIfReeling = ev.op === 'face_target' ? f[1] : ev.op === 'turn_to_target' ? (ev.only_if_reeling ?? f[1]) : 0;
          const evenIfEvading = ev.even_if_evading ?? (ev.op === 'face_target' ? f[2] : f[1]);
          const skipIfReeling = ev.skip_if_reeling ?? (ev.op === 'face_target' ? f[3] : 0);
          if (skipIfReeling && foe.reeling) break;
          if (onlyIfReeling && !foe.reeling) break;
          if (!evenIfEvading && foe.busy) break;
          turnToward(a, flat(vSub(foe.pos, a.pos)), Math.min(180, deg || 0) * D2R); break; }
        case 'face_flight_direction': if (vLen(flat(a.vel)) > 0) turnToward(a, (ev.backwards ?? f[1]) ? vMul(a.vel, -1) : a.vel, (ev.deg_per_tick ?? f[0]) * D2R); break;
        case 'lunge': {
          if (a.state === 'homing') {
            if (ev.after_homing) { if (!a.J.stopped) break; } else if (ev.only_without_target && foe) break;
          } else {
            if (ev.only_without_target && foe) break;
            if (a.state !== 'air') a.state = 'air';
            a.flags.air_motion = true;
          }
          const p = (ev.pitch || 0) * D2R, F = fwd(a.yaw);
          a.extra = vAdd(a.extra, vMul(v3(F.x * Math.cos(p), Math.sin(p), F.z * Math.cos(p)), ev.speed || 0)); break; }
        case 'ground_slide': {
          if (airborne(a)) break;
          if (a.state === 'homing' && ev.after_homing) { if (!a.J.stopped) break; } else if (ev.skip_if_target && foe) break;
          a.extra = vAdd(a.extra, vMul(fwd(a.yaw + (ev.angle || 0) * D2R), ev.speed || 0)); break; }
        case 'chase_dash': if (a.state !== 'chase' && foe) {
          const gap = Math.max(0, vLen(flat(vSub(foe.pos, a.pos))) - 2 * 0.45);
          a.state = 'chase'; a.chase = { t: 0, T: 26, gap0: gap, dir: vNorm(flat(vSub(foe.pos, a.pos))) }; } break;
      }
    }
    let sv = v3(0, 0, 0);
    if (a.state === 'homing') {
      const J = a.J;
      if (!J.skipChecks && foe) {
        if (J.noRise && a.pos.y < foe.pos.y) J.stopped = true;
        if (J.noFall && foe.pos.y < a.pos.y) J.stopped = true;
        if (J.stopIfBusy && foe.busy) J.stopped = true;
      }
      if (J.track >= 0 && !J.frozen && foe) J.dest = vAdd(vAdd(aimPt(foe), vMul(vRotY(J.dir, J.angle), J.dist)), v3(0, J.height, 0));
      J.track -= 1;
      if (!J.skipChecks && foe && foe.busy) J.frozen = true;
      const end = J.arrived || (J.max > 0 && J.elapsed >= J.max);
      if (end) { a.state = J.ground ? 'ground' : 'air'; a.carry = v3(0, 0, 0); a.J = null; }
      else {
        J.elapsed += 1;
        if (!J.stopped && J.track > 0 && J.dest) {
          const d = vSub(J.dest, refPt(a)), L = vLen(d);
          if (J.arrive > L) J.arrived = true;
          if (L < 2 * Math.max(J.hs, J.vs)) sv = L <= 0.02 ? v3(0, 0, 0) : d;
          else { const n = vMul(d, 1 / L); sv = v3(n.x * J.hs, n.y * J.vs, n.z * J.hs); }
          if (J.noRise && sv.y > 0) sv.y = 0;
          if (J.noFall && sv.y < 0) sv.y = 0;
        }
      }
    } else if (a.state === 'chase') {
      const C = a.chase; C.t = Math.min(C.t + 1, C.T);
      const want = 1.0 + C.gap0 * (C.T - C.t) / C.T, now = Math.max(0, vLen(flat(vSub(foe.pos, a.pos))) - 2 * 0.45);
      sv = vMul(C.dir, Math.max(0, now - want));
      if (C.t >= C.T) a.state = 'air';
    } else if (a.state === 'fall') {
      const s = a.spec, g = 2 * s.h / (s.T * s.T), Fa = a.fall; Fa.t += 1;
      const y = Fa.y0 - 0.5 * g * s.P60 * a.gravMul * Fa.t * Fa.t;
      a.carry = vMul(a.carry, 1 - (1 - s.P148) / 2);
      sv = v3(a.carry.x, Math.max(y - a.pos.y, s.P96), a.carry.z);
    } else if (a.flags.floating_physics) { a.carry = vMul(a.carry, 1 - (1 - a.spec.P148) / 2); sv = { ...a.carry }; }
    else a.carry = v3(0, 0, 0);
    if (a.state === 'air' && !isAir(a)) startFall(a);
    const prev = a.root || root; a.root = root;
    if (root.yaw !== undefined && prev.yaw !== undefined) a.yaw = wrapPi(a.yaw + (root.yaw - prev.yaw));
    let rd = vRotY(vSub(root, prev), a.yaw);
    if (rootHook) rd = rootHook(rd, a, foe);        // the runtime's root-motion hooks (warp=, vtrack=; dummy.js)
    const D = vAdd(vAdd(a.extra, sv), rd);
    a.pos = vAdd(a.pos, D); a.vel = D;
    const h = a.pos.y;
    if (isAir(a) || a.state === 'air' || a.state === 'fall') {
      const canLand = a.flags.landing_allowed || a.state === 'fall';
      if (h <= 0 && canLand) { a.pos.y = 0; if (!a.flags.floating_physics) a.carry = v3(0, 0, 0); a.state = 'ground'; a.fall = null; a.landed = true; }
      else if (h < 0) a.pos.y = 0;
    } else if (h >= 0.4) startFall(a);
    else a.pos.y = 0;
    return a;
  }
  return { SPEC, specOf, makeActor, startMove, startHoming, stepActor, evArgs, fwd };
})();
