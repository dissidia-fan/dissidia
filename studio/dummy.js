/* Move Studio: a dummy foe that takes the move's hits, for juggles and hit-stun. Loaded after app.js and moves.js.

   The rules are the game's, from docs/archive/hit-reaction/README.md (read from the EBOOT): the reaction byte picks
   the movement (drag to a point in front of the attacker, straight flight, launch), the spec record 0 gravity
   (g = 2·3.75/42² m/tick²), 50-tick hit-stun motions, a fresh 50 ticks on every hit, no juggle cap, 702/752 down
   (invincible, can act at 70), 102 landing (can act at 14).
   Approximations (the note leaves them open, or the studio has no world):
     - contact is the game's (below), but the dummy's hurt spheres stay in the rig's standing pose (they don't follow
       its hit-stun motions);
     - projectiles fly as the game flies them (magi.js), but an origin-6 shot starts at its effect record's bone and
       offset (not the playing effect), and the target's aim point is 1 m above its feet (a guess in the note);
       forced_hit (0x0108) hits at once;
     - the attacker moves as the game moves him (actor.js: root motion, homing dashes, lunges, slides, falls), and his
       landing doesn't switch to the landing motion;
     - body push-apart (kind-1 spheres) is the game's resolver (docs/archive/open-questions/dummy-and-sound.md §1:
       mass 100 / 1000 heavy_body, the whole overlap + 0.05 m in one tick, horizontal), but resolved once per tick
       after both have moved (the attacker's pass, then the dummy's), with the dummy's sphere at the attacker's
       standing kind-1 height; the attacker's no_body_collision skips both passes (in game it gates only the other
       fighter's pass);
     - reactions 3, 14, 15, 17, 18, 20 as traced (same note §2), but: 20 flies along the push direction (the game uses
       the hit object's +0x10 vector, which the studio doesn't have); 601 / 610 / 611 can act only once landed (their
       own timelines' can_act isn't read); 14's drop eases like 11's (cos), ending on the floor;
     - no hit-stop;
     - guard, dodge and the wall (docs/archive/guard-dodge-walls): the dummy guards or dodges when it can act and a
       hit window or shot starts within a few ticks; the clash table decides a guarded hit; a blocked attacker recoils
       (901/911, speed (2 + 3k)/16 from where it was in its latest hit window, note §3) and its move ends there; the
       wall check re-arms when a flight or drag ends.
   The simulation is re-run from the start of the move (all its presses, as the whole-move strip plays them) whenever
   the move, its files or the dummy's settings change, and the dummy is drawn at the playhead. */

const DM = { on: false, dist: 2.0, height: null, above: 0, sim: null, key: '', group: null, does: 'none', wall: null };   // above: the dummy's start height over the attacker (m)   // does: what the dummy does when it can act; wall: m behind it   // height null = from the move (midair: 4 m)
const DMG = 2 * 3.75 / (42 * 42), DM_MAX_FALL = 0.6, DM_STUN = 50, DM_DOWN_ACT = 70;
const DM_REACT = { 0: 'no movement', 1: 'flinch / launch', 2: 'slide + launch', 3: 'floor slide / down', 4: 'launch', 5: 'blown away', 6: 'blown up', 7: 'blown down',
  8: 'pushed away', 11: 'launched (arc)', 12: 'blown (dragged)', 13: 'stagger (held)', 14: 'dragged', 15: 'dragged', 17: 'floor slide', 18: 'timer', 19: 'launch (hang)', 20: 'vent' };

const v3 = (x, y, z) => ({ x, y, z });
const vAdd = (a, b) => v3(a.x + b.x, a.y + b.y, a.z + b.z), vSub = (a, b) => v3(a.x - b.x, a.y - b.y, a.z - b.z);
const vMul = (a, k) => v3(a.x * k, a.y * k, a.z * k), vLen = (a) => Math.hypot(a.x, a.y, a.z);
const vNorm = (a) => { const l = vLen(a); return l > 0 ? vMul(a, 1 / l) : v3(0, 0, 0); };
const vLerp = (a, b, u) => vAdd(a, vMul(vSub(b, a), u));
const vRotY = (v, a) => { const c = Math.cos(a), s = Math.sin(a); return v3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c); };
const dmFwd = (f) => v3(Math.sin(f.facing), 0, Math.cos(f.facing));

/* ---------------------------------------------------------------- the foe (the note's section 8) */
function dmFoe(pos, facing) { return { pos, facing, state: 'idle', motion: 0, motionT: 0, canActIn: 0, move: null, vel: v3(0, 0, 0), drag: 0.975, air: null, inv: 0, freeze: 0 }; }
function dmStartAir(f, h, maxDy = 0) { h = Math.min(Math.max(h, 0), 100); f.air = { t: 0, T: Math.sqrt(2 * h / DMG), y0: f.pos.y, h, maxDy }; }
function dmAirDy(f) {
  const a = f.air; a.t += 1;
  const y = a.t < a.T ? a.y0 + a.h * Math.sin(Math.PI / 2 * Math.sin(Math.PI / 2 * a.t / a.T)) : a.y0 + a.h - 0.5 * DMG * (a.t - a.T) ** 2;
  let dy = y - f.pos.y;
  if (a.t >= a.T) dy = Math.min(Math.max(dy, -DM_MAX_FALL), a.maxDy);
  return dy;
}
function dmApplyHit(f, k, A) {
  const R = k.reaction;
  f.guard = null; f.dodge = null; f.freeze = 0;          // any later hit clears reaction 18's freeze (FUN_0883A150)
  if (R === 0 || R === 16) return;
  if (R === 18) { f.freeze = 60; return; }               // no movement, no script: 60 ticks frozen in place (defender+0x31C)
  if (f.move && (f.move.kind === 'line' || f.move.kind === 'drag')) f.wallRushed = false;   // leaving a flight / drag re-arms the wall
  let push = vSub(f.pos, A.pos); push.y = 0; push = vLen(push) ? vNorm(push) : vMul(dmFwd(f), -1);
  const ff = dmFwd(f), front = Math.acos(Math.max(-1, Math.min(1, ff.x * push.x + ff.z * push.z))) > Math.PI / 2;
  f.facing = Math.atan2(front ? -push.x : push.x, front ? -push.z : push.z);
  const airborne = f.pos.y > 0.001 || f.state === 'air' || f.state === 'launched';
  const play = (m) => { f.motion = m; f.motionT = 0; };
  const fly = (dir, dist, speed, m) => { f.state = 'launched'; play(m); f.air = null; f.move = { kind: 'line', dir: vNorm(dir), speed, T: speed ? dist / speed : 0, t: 0 }; };
  const launch = (h, speed, m) => { f.state = 'air'; play(m); f.move = null; f.vel = vMul(push, speed); f.drag = 0.975; dmStartAir(f, h); };
  const r1 = () => {
    if (!k.kb_forward && !airborne) { f.state = 'stun'; play(front ? 500 : 501); f.air = null; f.move = { kind: 'slide', dir: push, S: k.stun }; }
    else launch(k.kb_forward, k.kb_up, front ? 550 : 551);
  };
  const drag = (R2, m, extra = {}) => {          // the drags of 11–15 (FUN_08832BB0 / FUN_08A51750)
    f.state = R2 === 13 ? 'stun' : 'launched'; f.air = null; play(m);
    f.move = { kind: 'drag', start: { ...f.pos }, prev: { ...f.pos }, t: 1, phase: 0, arc: R2 === 11, linear: R2 !== 13, R: R2,
      yaw: k.kb_yaw, fwd: k.kb_forward, up: k.kb_up, dur: k.stun > 0 ? k.stun : DM_STUN, follow: k.follow, bone: k.kb_bone, ...extra };
    f.move.target = dmDragTarget(f.move, A);
  };
  const low = f.pos.y <= 2;                       // height above the floor ≤ 2.0 m (const 0x089C7EC8)
  switch (R) {
    case 1: r1(); break;
    case 3: case 17:                              // low: snap to the floor, script 601, slide +0x24, launch height +0x28
      if (low) { f.pos.y = 0; f.state = 'air'; play(601); f.move = { kind: 'slide', dir: push, S: k.stun }; f.vel = v3(0, 0, 0); dmStartAir(f, k.follow); }
      else if (R === 3) fly(v3(0, -1, 0), k.kb_yaw, k.kb_up, 700);   // high: reaction 7
      else r1();                                  // 17 high: reaction 1
      break;
    case 14: drag(14, 610, { arc: true, linear: false, dur: 64 }); break;
    case 15:
      if (!low) drag(12, front ? 700 : 750);      // high: reaction 12
      else { f.pos.y = 0; drag(15, 611, { arc: true, linear: false, dur: 54, t: -9 }); }   // held 10 ticks, then dragged 54
      break;
    case 20: fly(push, k.kb_yaw, k.kb_up, front ? 700 : 750); break;   // game: along normalize(hit record +0x10); studio: the push
    case 4: case 19: launch(k.kb_forward, k.kb_up, 700); break;
    case 2: f.state = 'air'; play(600); f.move = { kind: 'slide', dir: push, S: k.stun }; f.vel = v3(0, 0, 0); dmStartAir(f, k.follow); break;
    case 5: fly(vSub(f.pos, A.pos), k.kb_yaw, k.kb_up, front ? 700 : 750); break;
    case 6: fly(v3(0, 1, 0), k.kb_yaw, k.kb_up, 700); break;
    case 7: fly(v3(0, -1, 0), k.kb_yaw, k.kb_up, 700); break;
    case 8: fly(push, k.kb_yaw, k.kb_up, front ? 700 : 750); break;
    case 11: case 12: case 13: drag(R, R === 13 ? (front ? 560 : 561) : (front ? 700 : 750)); break;
    default: f.state = 'stun'; play(front ? 500 : 501);
  }
  f.canActIn = f.motion >= 500 && f.motion <= 561 ? DM_STUN : Infinity;
}
function dmDragTarget(m, A) {
  const origin = m.bone > 0 && A.bonePos ? A.bonePos(m.bone) : A.pos;
  const t = vAdd(origin, vMul(vRotY(A.fwd, m.yaw), m.fwd));
  if (!m.arc) t.y += m.up;
  return t;
}
function dmStep(f, m, p) {
  const step = vSub(p, m.prev); m.prev = p; m.lastStep = step;
  let d = vSub(p, f.pos); const s = vLen(step);
  if (vLen(d) > 1.5 * s) d = vMul(vNorm(d), s);
  f.pos = vAdd(f.pos, d);
}
function dmEndDrag(f, m) {
  f.move = null; f.wallRushed = false;
  if (m.R === 13) { f.vel = v3(0, 0, 0); dmStartAir(f, 0); }
  else { const s = m.lastStep || v3(0, 0, 0); f.vel = v3(s.x, 0, s.z); f.drag = 0.95; f.state = 'air'; dmStartAir(f, 0, Math.min(0, s.y)); }
}
function dmLand(f) {
  f.pos.y = 0;
  if (f.motion >= 500 && f.motion <= 561 && f.motionT < DM_STUN) return;      // held: no landing before frame 50
  if (f.move && (f.move.kind === 'line' || f.move.kind === 'drag')) f.wallRushed = false;
  f.air = null; f.vel = v3(0, 0, 0); f.move = null;
  if ([700, 701, 750, 751].includes(f.motion)) { f.motion = f.motion < 750 ? 702 : 752; f.motionT = 0; f.state = 'down'; f.canActIn = DM_DOWN_ACT; f.inv = 100; }
  else { f.motion = 102; f.motionT = 0; f.state = 'recovering'; f.canActIn = 14; }
}
function dmTickFoe(f, A) {
  f.motionT += 1; if (f.canActIn > 0) f.canActIn -= 1; if (f.inv > 0) f.inv -= 1;
  if (f.freeze > 0) { f.freeze -= 1; return; }   // reaction 18: the whole move of the tick is dropped (FUN_0883AF80)
  const m = f.move;
  if (m && m.kind === 'drag' && m.phase === 0 && m.t < 1) m.t += 1;   // reaction 15 low: held still while t < 0
  else if (m && m.kind === 'drag') {
    if (m.phase === 0) {
      if (m.t <= m.follow && A) m.target = dmDragTarget(m, A);
      let u = Math.min(m.t / m.dur, 1); if (!m.linear) u = Math.sin(Math.PI / 2 * u);
      const p = vLerp(m.start, m.target, u); if (m.arc) p.y += m.up * Math.sin(Math.PI / 2 * u);
      dmStep(f, m, p);
      if (m.t > m.dur) {
        if ((f.motion === 700 || f.motion === 750) && m.R !== 14 && m.R !== 15) f.motion += 1;
        if (m.arc && m.up > 0) { const d = vMul(vSub(m.target, m.start), m.R === 14 || m.R === 15 ? 0.1 : 0.7); m.start = { ...m.target }; m.target = vAdd(m.target, d); m.dur = m.up / 0.09; m.t = 0; m.phase = 2; }
        else dmEndDrag(f, m);
      }
    } else {
      const u = Math.min(m.t / m.dur, 1), p = vLerp(m.start, m.target, u); p.y += m.up * Math.cos(Math.PI / 2 * u);
      dmStep(f, m, p); if (u >= 1 || ((m.R === 14 || m.R === 15) && f.pos.y <= 0)) dmEndDrag(f, m);   // 14/15: the drop ends on the floor
    }
    m.t += 1;
  } else if (m && m.kind === 'line') {
    m.t += 1; f.pos = vAdd(f.pos, vMul(m.dir, m.speed));
    if (m.t > m.T) {
      if (f.motion === 700 || f.motion === 750) f.motion += 1;
      f.vel = v3(m.dir.x * m.speed, 0, m.dir.z * m.speed); f.drag = 0.975; f.move = null; f.state = 'air'; f.wallRushed = false; dmStartAir(f, 0, Math.min(0, m.dir.y * m.speed));
    }
  } else if (m && m.kind === 'slide') {
    if (m.S >= 0.01) { f.pos = vAdd(f.pos, vMul(m.dir, m.S)); m.S *= 0.9; } else f.move = null;
  }
  if (f.air) { f.vel = vMul(f.vel, f.drag); f.pos = vAdd(f.pos, v3(f.vel.x, dmAirDy(f), f.vel.z)); }
  const held = f.move && f.move.kind === 'drag' && (f.move.R === 14 || f.move.R === 15);   // 14 / 15: a drag (state 0x17) lands only once it ends
  // a straight flight (reactions 5, 8, 20) that runs level or upward from the floor rides it until the line ends;
  // only a downward line lands on contact (it used to land on its first tick when the hit came on the ground)
  const level = f.move && f.move.kind === 'line' && f.move.dir.y >= 0;
  if (level && f.pos.y < 0) f.pos.y = 0;
  if (f.pos.y <= 0 && (f.air || f.state === 'launched') && !held && !level) dmLand(f);
  if (f.state === 'stun' && f.motionT >= DM_STUN) { f.state = f.pos.y > 0.001 ? 'falling' : 'idle'; f.canActIn = 0; }
  if ((f.state === 'down' || f.state === 'recovering') && f.canActIn <= 0) f.state = 'idle';
  if (f.state === 'wall' && f.motionT >= Math.max(20, motionLen(dmScriptMotion(f.motion)) * 2)) { f.motion = 101; f.motionT = 0; f.inv = 0; f.state = 'falling'; f.vel = v3(0, 0, 0); dmStartAir(f, 0); }
}

/* ---------------------------------------------------------------- guard, dodge, walls (docs/archive/guard-dodge-walls) */
// the clash table (0x089D4978): rows = own atkp class, columns = the other's; outcome codes 0 nothing, 1 recoil back,
// 2 recoil in place, 3 guard success, 4 delivered through the guard, 5 both cancelled, 6 recoil sideways
const DM_CLASH = [
  [1,2,2,6,1,1,0,0,6,0,0,0,1,1,1,0],[0,1,2,6,0,1,0,0,0,0,0,0,1,1,1,0],[0,0,1,0,0,0,0,0,0,0,0,0,1,1,0,0],
  [0,0,6,0,0,0,0,0,0,0,0,0,6,6,0,0],[3,1,4,4,0,0,0,3,1,3,0,0,0,0,0,0],[3,3,1,1,0,0,0,3,3,3,0,0,0,0,0,0],
  [0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0],[6,6,6,6,6,6,6,0,6,6,0,0,6,6,0,6],[0,6,6,6,6,6,0,0,0,0,0,0,6,6,0,6],
  [0,0,0,0,1,1,0,6,0,1,6,0,1,1,1,0],[6,6,6,6,0,0,0,0,6,6,0,0,0,0,0,0],[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
  [3,3,3,3,0,0,0,3,3,3,0,0,0,0,0,0],[3,3,1,3,0,0,0,3,3,3,0,0,0,0,0,0],[3,3,1,0,0,0,0,0,0,3,0,0,0,0,0,0],
  [0,0,0,1,0,0,0,3,3,0,0,0,0,0,0,0]];
const DM_DOES = [['none', 'takes it'], ['guard', 'guards'], ['back', 'dodges back'], ['side', 'dodges sideways'], ['forward', 'dodges toward'], ['air', 'air dodge']];
const DM_DODGE_SCRIPT = { back: 400, forward: 410, air: 440, side: 460 };
function dmGuardStart(f) {                    // block: script 470 (ground) / 471 (air), a GUARD window over ticks 0–36
  f.guard = { t: 0, ok: false }; f.state = 'guard'; f.motion = f.pos.y > 0.001 ? 471 : 470; f.motionT = 0; f.move = null;
}
function dmGuardTick(f) {
  const g = f.guard; if (!g) return;
  g.t += 1;
  if (g.ok || g.t >= 50) { f.guard = null; f.state = f.pos.y > 0.001 ? 'falling' : 'idle'; f.canActIn = 0; }
}
/* a hit sphere set `sp` of an attack with hit record h against the guard: null = not involved, else the codes */
function dmGuardResolve(f, sp, h, swing, gs) {
  const g = f.guard;
  if (!g || g.t > 36 || f.inv > 0) return null;
  if (!sp.some((s) => vLen(vSub(s.c, gs.c)) < s.r + gs.r)) return null;
  const a = v3(-swing.x, 0, -swing.z), gf = dmFwd(f), la = Math.hypot(a.x, a.z);
  if (la > 1e-6) { const d = Math.acos(Math.max(-1, Math.min(1, (a.x * gf.x + a.z * gf.z) / la))) * 180 / Math.PI; if (d > 90) return null; }   // from behind: the guard doesn't face it
  const cA = h.class & 15; let oA = DM_CLASH[cA][4], oD = DM_CLASH[4][cA];
  if (oA === 1 && oD === 1) oA = oD = 5;
  if (oD === 3) g.ok = true;
  return { attacker: oA, guard: oD, blocked: [1, 2, 3, 5, 6].includes(oA), crushed: oA === 4 || oD === 4 };
}
function dmDodgeStart(f, kind, apos, root) {
  const to = v3(apos.x - f.pos.x, 0, apos.z - f.pos.z), L = Math.hypot(to.x, to.z), toT = L ? v3(to.x / L, 0, to.z / L) : dmFwd(f);
  if (f.pos.y > 0.001) kind = 'air';
  f.facing = Math.atan2(toT.x, toT.z);
  if (kind === 'side') f.facing += Math.PI / 2;
  f.dodge = { kind, t: 0, origin: { ...f.pos }, facing: f.facing, root };
  f.state = 'dodge'; f.motion = DM_DODGE_SCRIPT[kind]; f.motionT = 0; f.move = null;
  if (kind === 'forward') {                    // a hop toward the foe from frame 4 (spec record 0 defaults)
    const d = Math.min(Math.max(L, 1), 5), u = (d - 1) / 4;
    f.dodge.hop = { at: 4, h: 3 + u, speed: 0.1 + 0.2 * u, dir: toT };
  }
}
function dmDodgeTick(f) {
  const D = f.dodge; if (!D) return;
  D.t += 1;
  f.inv = D.t <= 20 ? 1 : 0;                   // invincible 0–20
  if (D.hop) { if (D.t === D.hop.at) { f.vel = vMul(D.hop.dir, D.hop.speed); f.drag = 0.975; dmStartAir(f, D.hop.h); } }
  else if (D.root) { const p = vRotY(D.root(D.t), D.facing); f.pos = v3(D.origin.x + p.x, f.pos.y, D.origin.z + p.z); }
  if (D.t >= 56) { f.dodge = null; f.inv = 0; f.state = f.pos.y > 0.001 ? 'falling' : 'idle'; f.canActIn = 0; }
}
/* a wall rush: a foe in a straight flight (reactions 5–8, 20) or a reaction-12 drag that runs into the wall within 60°
   of head on is pinned there (script 704, 754 after a back blow), invincible, can act from 70; then it falls (101) */
function dmWallCheck(f, wall, v) {
  const m = f.move;
  const flying = m && (m.kind === 'line' || (m.kind === 'drag' && m.R === 12 && m.phase === 0 && m.t > 2));
  if (!flying || f.wallRushed) return false;
  if (m.kind === 'drag' && ![700, 701, 750, 751].includes(f.motion)) return false;
  const L = vLen(v); if (!L) return false;
  const n = wall.normal, R = 0.6, c = v3(f.pos.x, f.pos.y + 0.9, f.pos.z);
  const side = (c.x - wall.point.x) * n.x + (c.y - wall.point.y) * n.y + (c.z - wall.point.z) * n.z;
  if (side > R) return false;
  const u = vMul(v, 1 / L), dot = u.x * n.x + u.y * n.y + u.z * n.z;
  if (Math.acos(Math.max(-1, Math.min(1, dot))) * 180 / Math.PI < 120) return false;
  f.wallRushed = true;
  const back = f.motion === 750 || f.motion === 751;
  f.pos = v3(f.pos.x + n.x * (R - side), f.pos.y, f.pos.z + n.z * (R - side));
  f.facing = Math.atan2(back ? -n.x : n.x, back ? -n.z : n.z);
  f.move = null; f.air = null; f.vel = v3(0, 0, 0);
  f.motion = back ? 754 : 704; f.motionT = 0; f.state = 'wall'; f.inv = Infinity; f.canActIn = 70;
  return true;
}

/* ---------------------------------------------------------------- the character's hit volumes (objx section type 5) */
function dmParseVolumes(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), n = dv.getUint32(8, true), G = new Map();
  for (let i = 0; i < n; i++) {
    const o = 16 + 32 * i, g = dv.getUint32(o, true);
    if (!G.has(g)) G.set(g, []);
    G.get(g).push({ kind: bytes[o + 4], bone: dv.getUint16(o + 6, true), off: [dv.getFloat32(o + 8, true), dv.getFloat32(o + 12, true), dv.getFloat32(o + 16, true)], r: dv.getFloat32(o + 28, true) });
  }
  return G;
}
async function dmLoadVolumes() {
  DM.group = null;
  try {
    if (S.rig.retail) {
      const dec = await retailDecoded(S.rig.ms.replace('retail/', 'obj/') + '.objx', S.rig.gmo, S.boneNames);
      const e = dec['arc.json'].entries.find((x) => x.type === 5); if (e) DM.group = dmParseVolumes(dec[e.file]);
    } else {
      const arc = await FS.json(msPath('arc.json')), e = arc.entries.find((x) => x.type === 5);
      if (e) DM.group = dmParseVolumes(new Uint8Array(await FS.read(msPath(e.file))));
    }
  } catch (err) { console.warn('studio: no hit volumes', err); }
}

/* ---------------------------------------------------------------- body push-apart (docs/archive/open-questions/dummy-and-sound.md §1)
   FUN_08812414: the push d of sphere Y off sphere X (r = h for body spheres), horizontal only, the whole overlap plus
   0.05 m (const 0x089C6E28) in one go; null when they don't overlap (3-D test |Yc − Xc| < rX + rY) */
function dmPushVec(xc, xr, yc, yr) {
  const rs = xr + yr, o = vSub(yc, xc);
  if (vLen(o) >= rs) return null;
  const n = vMul(o, 1 / rs), hz = n.x * n.x + n.z * n.z;
  if (hz <= 0) return v3(rs + 0.05, 0, 0);       // same column: along +x
  const k = Math.sqrt((1 - n.y * n.y) / hz);
  return v3(n.x * (k - 1) * (rs + 0.05), 0, n.z * (k - 1) * (rs + 0.05));
}
/* FUN_0883E3BC: how d splits between the mover (self) and the fighter it walks into (other). mass 1000 with heavy_body
   (0x004B), else 100; cant_be_pushed (0x0049) on the OTHER side makes the mover take it all (on its own side it does
   nothing). Returns [self's move, other's move]; self moves back (−), other forward (+). */
function dmPushSplit(d, self, other) {
  const mS = self.heavy ? 1000 : 100, mO = other.heavy ? 1000 : 100;
  if (other.unpushable && !self.unpushable) return [vMul(d, -1), v3(0, 0, 0)];
  return [vMul(d, -mO / (mS + mO)), vMul(d, mS / (mS + mO))];
}

/* ---------------------------------------------------------------- the simulation */
const DM_HIT_OPS = new Set([0x0200, 0x0743, 0x027B]);
function dmSegments() {                       // [{motion, start tick, len ticks}] of the move (or the animation) on show
  if (S.step && S.moves && S.moves[S.step.move]) {
    let at = 0;
    return mvSegments(S.moves[S.step.move], true).map((s) => { const r = { motion: s.motion, start: at, len: Math.round(s.len * 2), action: s.action }; at += r.len; return r; });
  }
  return S.motion >= 0 ? [{ motion: S.motion, start: 0, len: motionLen(S.motion) * 2 }] : [];
}
function dmAttackerAt(T, pi) {                // where the simulation has the attacker at tick T of the current press (or of press pi) (effects posed then)
  if (!DM.on || !DM.sim || !view.root || S.fxEdit) return null;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return null;
  const i = Math.min(pi != null ? pi : S.step ? S.step.i : 0, segs.length - 1);
  const fr = DM.sim.frames[Math.max(0, Math.min(DM.sim.frames.length - 1, Math.round(segs[i].start + Math.min(T, segs[i].len))))];
  return fr ? { x: fr.apos.x, y: fr.apos.y - DM.sim.H, z: fr.apos.z, yaw: fr.ayaw } : null;
}
/* the simulated flight of shot n of a projectile fired at tick `fire` of the current press (homing on the dummy), as
   fx.js's fxShot gives it: {life, at(T) -> matrix (+Z along the flight)}; null when the simulation has no such shot */
function dmShotAt(id, fire, n) {
  if (!DM.on || !DM.sim || !DM.sim.shotLog || !view.root || S.fxEdit) return null;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return null;
  const s0 = segs[S.step ? Math.min(S.step.i, segs.length - 1) : 0].start, T0 = s0 + Math.round(fire);
  const L = DM.sim.shotLog.filter((x) => x.id === id && !x.child && Math.abs(x.T - T0) <= 1 && x.path.length).sort((a, b) => a.T - b.T)[n];
  if (!L) return null;
  const H = DM.sim.H, P = L.path, z = new THREE.Vector3(0, 0, 1);
  return { life: P[P.length - 1].T - L.T + 1, at: (t) => {
    const T = s0 + t, k = Math.max(0, Math.min(P.length - 1, Math.round(T) - P[0].T)), q = P[k];
    const d = new THREE.Vector3(q.d.x, q.d.y, q.d.z); if (d.lengthSq() < 1e-9) d.copy(z); else d.normalize();
    return new THREE.Matrix4().makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(z, d)).setPosition(q.p.x, q.p.y - H, q.p.z);
  } };
}
/* apocarms=ARM0,NARM,ID1[,ID2..] (runtime roster.cpp apoc_bone_post): the move's royal arms ride their magi bullets.
   When a listed bullet is born, the free arm of that set that the motion showed within the last 12 ticks and lies
   nearest it (at most 3 m) takes it; while the bullet lives the arm sits on it, blade (+Y) along the flight, scale 1. */
/* what the simulation adds to the effect preview (app.js passes it as extra instances): the contact effect of every
   projectile hit on the dummy, and the shots born of a hit (link_hit, and their own children), flown as simulated */
function dmSimFxInsts() {
  if (!DM.on || !DM.sim || !DM.sim.shotLog || !view.root || S.fxEdit || !S.fx || typeof projVisuals !== 'function') return [];
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return [];
  const s0 = segs[S.step ? Math.min(S.step.i, segs.length - 1) : 0].start, H = DM.sim.H, z = new THREE.Vector3(0, 0, 1), out = [];
  const hits = (F(msPath('hits.json')) || {}).hits || [];
  DM.sim.hits.forEach((h, k) => {
    if (h.proj == null || h.guarded || h.T < s0) return;
    const hr = hits.find((x) => (x.name || x.id) === h.name), bit = hr ? hr.hit_effect_bit : 0; if (!bit || bit === 255) return;
    const grp = S.fx.fepa.filter((r) => fxGroupHas(r, bit) && !r.trig), contact = grp.filter((r) => r.kinds & 1), recs = (contact.length ? contact : grp).slice(0, 2);
    const fr = DM.sim.frames[Math.min(DM.sim.frames.length - 1, h.T)];
    if (recs.length && fr) out.push({ type: 'hit', tick: h.T - s0, recs, kind: 'sim', idx: k, hit: h.name, foe: [fr.pos.x, fr.pos.y - H + 0.85, fr.pos.z] });
  });
  DM.sim.shotLog.forEach((L, k) => {
    if (!L.hit || !L.path.length || L.T < s0) return;
    const pr = S.fx.magi && S.fx.magi.get(L.id), recs = pr ? projVisuals(S.fx, pr) : []; if (!recs.length) return;
    const P = L.path;
    out.push({ type: 'proj', tick: L.T - s0, proj: pr, recs, kind: 'sim', idx: k, arg2: 0, simShot: { life: P[P.length - 1].T - L.T + 1, at: (t) => {
      const q = P[Math.max(0, Math.min(P.length - 1, Math.round(s0 + t) - P[0].T))], d = new THREE.Vector3(q.d.x, q.d.y, q.d.z);
      if (d.lengthSq() < 1e-9) d.copy(z); else d.normalize();
      return new THREE.Matrix4().makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(z, d)).setPosition(q.p.x, q.p.y - H, q.p.z);
    } } });
  });
  return out;
}
function dmApocSets() {
  const out = [];
  for (const t of (S.mod && S.mod.runtime_hooks) || []) {
    if (typeof t !== 'string' || !t.startsWith('apocarms=')) continue;
    const [a0, n, ...ids] = t.slice(9).split(',').map(Number);
    if (n > 0 && ids.length) out.push({ arms: Array.from({ length: n }, (_, k) => a0 + k), ids: new Set(ids) });
  }
  return out;
}
function dmApocAssign(sim) {
  const sets = dmApocSets(); if (!sets.length || !sim.shotLog || !sim.shotLog.length) return [];
  const segs = sim.segs, cache = new Map(), inv = new THREE.Matrix4();
  const armsAt = (T) => {                     // world positions (simulation frame) of the arms the motion shows at tick T
    if (cache.has(T)) return cache.get(T);
    const i = segs.findIndex((x) => T < x.start + x.len), sg = segs[i < 0 ? segs.length - 1 : i], P = dmPoseOf(sg.motion, T - sg.start);
    inv.copy(P.root).invert(); const fr = sim.frames[Math.min(sim.frames.length - 1, T)], m = new Map();
    if (fr) for (const set of sets) for (const b of set.arms) {
      const M = P.bones[b]; if (!M) continue;
      const L = inv.clone().multiply(M), e = L.elements; if (Math.hypot(e[0], e[1], e[2]) < 1e-3) continue;   // hidden (scale 0)
      const c = Math.cos(fr.ayaw), sn = Math.sin(fr.ayaw);
      m.set(b, { x: fr.apos.x + e[12] * c + e[14] * sn, y: fr.apos.y + e[13], z: fr.apos.z - e[12] * sn + e[14] * c });
    }
    cache.set(T, m); return m;
  };
  const out = [];
  for (const L of sim.shotLog.filter((x) => x.path.length).sort((a, b) => a.T - b.T)) {
    const set = sets.find((st) => st.ids.has(L.id)); if (!set) continue;
    if (L.child) {                            // a listed child takes the arm of a listed shot that just ended near where it is born (roster.cpp hand-off)
      const p0 = L.path[0].p, prev = out.filter((o) => set.arms.includes(o.arm) && !out.some((o2) => o2 !== o && o2.arm === o.arm && o2.T > o.T)).find((o) => {
        const P = o.L.path, e = P[P.length - 1]; return L.T - e.T >= -1 && L.T - e.T <= 4 && Math.hypot(e.p.x - p0.x, e.p.y - p0.y, e.p.z - p0.z) <= 3; });
      if (prev) out.push({ arm: prev.arm, T: L.T, L });
      continue;
    }
    const busy = new Set(out.filter((o) => o.T <= L.T && L.T <= o.T + o.L.path.length).map((o) => o.arm));
    const p = L.path[0].p; let best = null, bd = 3;
    for (let T = L.T; T >= Math.max(0, L.T - 12) && !best; T--) for (const [b, q] of armsAt(T)) {
      if (!set.arms.includes(b) || busy.has(b)) continue;
      const d = Math.hypot(q.x - p.x, q.y - p.y, q.z - p.z); if (d < bd) { bd = d; best = b; }
    }
    if (best != null) out.push({ arm: best, T: L.T, L });
  }
  return out;
}
function dmApocNow() { if (DM.on && DM.sim && !S.fxEdit && view.root) dmApocPlace(Math.min(DM.sim.frames.length - 1, Math.round(dmNowTick()))); else dmApocStraight(); }   // after a re-pose of the playhead
/* the same ride with the Dummy off: each listed bullet of this timeline flies as the effect preview flies it (fx.js
   fxShot: straight ahead from its origin, no homing), and the arm the motion showed nearest its start within the
   12 ticks before (at most 3 m) sits on it, blade (+Y) along the flight, while it lives. Called after every pose. */
function dmApocStraight() {
  if (DM.on || S.fxEdit || !S.gmo || !view.root || !view.bones) return;
  dmApocSetArms(dmApocRides());
}
function dmApocSetArms(rides) {                // the playhead's riding / held arms onto the live skeleton
  const now = S.frame * 2, list = [];
  for (const b of new Set(rides.map((r) => r.arm))) { const M = dmApocArmAt(rides, b, now); if (M) list.push([b, M]); }
  if (typeof FXP !== 'undefined' && FXP.dirty) { FXP.dirty = false; poseMotion(S.gmo, view, S.motion, S.frame); view.root.updateMatrixWorld(true); }   // back to the playhead
  const inv = new THREE.Matrix4();
  for (const [arm, M] of list) {
    const b = view.bones[arm]; if (!b) continue;
    b.parent.updateMatrixWorld(true); inv.copy(b.parent.matrixWorld).invert();
    inv.multiply(M).decompose(b.position, b.quaternion, b.scale); b.updateMatrixWorld(true);
  }
}
/* armtrailhold=N (roster.cpp apoc_bone_post): when an arm's bullet ends it stays where the flight ended, full size for
   the APOC_LIVE passes the game takes to notice, then N passes as a speck (scale 0.06, still shown for its trail), so
   its trail runs into the point of contact; then the motion's pose again. */
const APOC_LIVE = 3, ARM_HOLD_SCALE = 0.06;
function dmArmHold() { const h = ((S.mod && S.mod.runtime_hooks) || []).find((x) => typeof x === 'string' && x.startsWith('armtrailhold=')); return h ? Math.max(0, +h.slice(13) || 0) : 0; }
/* arm b's world matrix at press tick T while a ride or its hold moves it, else null (the motion's pose) */
function dmApocArmAt(rides, b, T) {
  const mine = rides.filter((r) => r.arm === b), on = mine.find((r) => T >= r.T0 && T < r.T0 + r.life);
  if (on) return on.at(T);
  const hold = dmArmHold();
  const r = mine.filter((x) => T >= x.T0 + x.life && T < x.T0 + x.life + APOC_LIVE + hold).sort((p, q) => q.T0 - p.T0)[0];
  if (!r) return null;
  const end = r.T0 + r.life, M = r.at(end - 1).clone();
  if (T >= end + APOC_LIVE) {
    const p = new THREE.Vector3(), q = new THREE.Quaternion(), sc = new THREE.Vector3(); M.decompose(p, q, sc);
    M.compose(p, q, new THREE.Vector3(ARM_HOLD_SCALE, ARM_HOLD_SCALE, ARM_HOLD_SCALE));
  }
  return M;
}
/* every apocarms= ride of the press on show: [{arm, T0, life, at(T) -> the arm's world matrix}] in press ticks.
   Dummy on: the simulation's bullets (DM.apoc); off: each listed bullet of this timeline flown as fx.js fxShot flies
   it, taken by the arm the motion showed nearest its start within the 12 ticks before (at most 3 m). */
function dmApocRides() {
  const Y = new THREE.Vector3(0, 1, 0), out = [];
  const onFlight = (p, d) => new THREE.Matrix4().makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(Y, d.normalize())).setPosition(p);
  if (DM.on && DM.sim && !S.fxEdit) {
    if (!DM.apoc || !DM.apoc.length) return out;
    const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return out;
    const s0 = segs[S.step ? Math.min(S.step.i, segs.length - 1) : 0].start, H = DM.sim.H;
    for (const a of DM.apoc) {
      const P = a.L.path; if (!P.length) continue;
      out.push({ arm: a.arm, T0: P[0].T - s0, life: P.length, at: (T) => {
        const q = P[Math.max(0, Math.min(P.length - 1, Math.round(T + s0) - P[0].T))], d = new THREE.Vector3(q.d.x, q.d.y, q.d.z);
        if (d.lengthSq() < 1e-9) d.set(0, 0, 1);
        return onFlight(new THREE.Vector3(q.p.x, q.p.y - H, q.p.z), d);
      } });
    }
    return out;
  }
  if (S.fxEdit || !S.gmo || !view.bones || !S.fx || !S.fx.magi || typeof fxShot !== 'function') return out;
  const sets = dmApocSets(), tl = typeof curTL === 'function' ? curTL() : null; if (!sets.length || !tl || !tl.keyed) return out;
  const names = view.bones.map((b) => b.name), ctx = { fx: S.fx, opts: {}, poseAt: fxPoseAt, boneOk: (i) => i >= 0 && i < names.length };
  try {
    for (const e of tl.keyed) {
      if (opCode(e.op) !== 0x0204) continue;
      const a = argsOf(e), id = typeof hkPickMagi === 'function' ? hkPickMagi(a[0]) : a[0];
      const set = sets.find((st) => st.ids.has(id)), pr = set && S.fx.magi.get(id); if (!pr) continue;
      const T0 = e.frame, shot = fxShot(ctx, pr, T0, typeof a[1] === 'number' ? a[1] : 0, null);
      const p0 = new THREE.Vector3().setFromMatrixPosition(shot.at(T0));
      const busy = new Set(out.filter((r) => T0 < r.T0 + r.life).map((r) => r.arm));
      let best = null, bd = 3;
      for (let T = T0; T >= Math.max(0, T0 - 12) && best == null; T--) {
        const P = fxPoseAt(T);
        for (const b of set.arms) {
          const M = P.bones[b]; if (!M || busy.has(b)) continue;
          const el = M.elements; if (Math.hypot(el[4], el[5], el[6]) < 1e-3) continue;   // hidden (scale 0)
          const d = new THREE.Vector3().setFromMatrixPosition(M).distanceTo(p0); if (d < bd) { bd = d; best = b; }
        }
      }
      if (best == null) continue;
      out.push({ arm: best, T0, life: shot.life, at: (T) => {
        const Mn = shot.at(T), Mp = shot.at(T + 1), pn = new THREE.Vector3().setFromMatrixPosition(Mn);
        const d = new THREE.Vector3().setFromMatrixPosition(Mp).sub(pn); if (d.lengthSq() < 1e-9) d.set(0, 0, 1).transformDirection(Mn);
        return onFlight(pn, d);
      } });
    }
  } catch (err) { console.warn('studio: apocarms preview', err); }
  return out;
}
/* armtrail=ID,SLOT0,NSLOTS,ARM0:N:BIT0:DRAW,... (roster.cpp armtrail_bone_post + its FUN_0884E660 wrapper): arm k of a
   set leaves the first FEPA record of trail id ID with mask bit BIT0+k while it is shown (bone scale > 0.05 and its
   draw part drawn), on at most NSLOTS slots at once (earlier sets win). The preview plays that trail on the arm as the
   preview moves it, riding its bullet under apocarms=, so the trail follows the whole flight. -> extra fx instances */
function dmArmTrailInsts() {
  const h = ((S.mod && !(S.rig && S.rig.retail) && S.mod.runtime_hooks) || []).find((x) => typeof x === 'string' && x.startsWith('armtrail='));
  if (!h || S.fxEdit || !S.gmo || !view.root || !view.bones || !S.fx || typeof curTL !== 'function') return [];
  const tl = curTL(); if (!tl) return [];
  const [id, , nslots, ...rest] = h.slice(9).split(','), ID = +id, NS = Math.max(1, +nslots || 11), arms = [];
  for (const s of rest) {
    const [a0, n, b0, draw] = s.split(':').map(Number);
    for (let k = 0; k < n; k++) {
      const bit = b0 + k, rec = (S.fx.byId.get(ID) || []).find((r) => r.trig === 0 && ((r.keys >>> bit) & 1));
      if (rec && a0 + k < view.bones.length) arms.push({ bone: a0 + k, draw, rec });
    }
  }
  if (!arms.length) return [];
  const rides = dmApocRides(), now = S.frame * 2, out = [], live = new Map();
  const ride = (b, T) => dmApocArmAt(rides, b, T);
  const drawn = (st, d) => (st.has(d) ? st.get(d) : partDefault(d)) && (S.exMode || !S.exOnly.has(d));
  for (let T = 0; T <= now; T++) {
    const st = partStates(tl, T), P = fxPoseAt(T);
    for (const a of arms) {
      const r = ride(a.bone, T), M = P.bones[a.bone], e = M ? M.elements : null;
      const shown = drawn(st, a.draw) && (r || (e && Math.hypot(e[4], e[5], e[6]) > 0.05));
      const cur = live.get(a);
      if (shown && !cur && live.size < NS) {
        const inst = { type: 'fx', tick: T, end: null, recs: [a.rec], kind: 'armtrail', idx: a.bone, trail: true,
          boneAt: (t) => ride(a.bone, t) || fxPoseAt(t).bones[a.bone] };
        live.set(a, inst); out.push(inst);
      } else if (!shown && cur) { cur.end = T; live.delete(a); }
    }
  }
  return out;
}
function dmApocPlace(T) {
  if (!DM.apoc || !DM.apoc.length || !view.bones) return;
  if (view.root) { dmApocSetArms(dmApocRides()); return; }     // the rides with their holds (dmApocArmAt); below: no skeleton root
  const H = DM.sim.H, Y = new THREE.Vector3(0, 1, 0), M = new THREE.Matrix4(), inv = new THREE.Matrix4();
  for (const a of DM.apoc) {
    const P = a.L.path, k = T - P[0].T; if (k < 0 || k >= P.length) continue;
    const b = view.bones[a.arm]; if (!b) continue;
    const q = P[k], d = new THREE.Vector3(q.d.x, q.d.y, q.d.z); if (d.lengthSq() < 1e-9) continue;
    M.makeRotationFromQuaternion(new THREE.Quaternion().setFromUnitVectors(Y, d.normalize())).setPosition(q.p.x, q.p.y - H, q.p.z);
    b.parent.updateMatrixWorld(true); inv.copy(b.parent.matrixWorld).invert();
    inv.multiply(M).decompose(b.position, b.quaternion, b.scale); b.updateMatrixWorld(true);
  }
}
function dmFoePosAt(T) {                     // where the simulation has the dummy at tick T of the current press (THREE vector), or null
  if (!DM.on || !DM.sim || !view.root || S.fxEdit) return null;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return null;
  const i = S.step ? Math.min(S.step.i, segs.length - 1) : 0;
  const fr = DM.sim.frames[Math.max(0, Math.min(DM.sim.frames.length - 1, Math.round(segs[i].start + Math.min(T, segs[i].len))))];
  return fr ? new THREE.Vector3(fr.pos.x, fr.pos.y - DM.sim.H, fr.pos.z) : null;
}
function dmFoeAt(name, start, end) {         // a hit's landing on the dummy in this press: {T (press tick), pos}, false = it missed
  if (!DM.on || !DM.sim || S.fxEdit) return null;
  const segs = DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return null;
  const i = S.step ? Math.min(S.step.i, segs.length - 1) : 0, s0 = segs[i].start;
  const h = DM.sim.hits.find((x) => x.press === i && x.name === name && !x.guarded && x.T >= s0 + start - 1 && x.T <= s0 + Math.max(start, end == null ? start : end) + 1);
  if (!h) return false;
  const fr = DM.sim.frames[Math.min(DM.sim.frames.length - 1, h.T)];
  return { T: h.T - s0, pos: [fr.pos.x, fr.pos.y - DM.sim.H + 0.85, fr.pos.z] };
}
function dmNowTick() {
  const segs = DM.sim && DM.sim.segs && DM.sim.move === (S.step ? S.step.move : -1) ? DM.sim.segs : dmSegments(); if (!segs.length) return 0;
  const i = S.step ? Math.min(S.step.i, segs.length - 1) : 0;
  return segs[i].start + Math.min(S.frame * 2, segs[i].len);
}
function dmPoseOf(motion, tick) {             // bone world matrices at one tick of one motion (the live pose is restored after)
  poseMotion(S.gmo, view, motion, frames(tick)); view.root.updateMatrixWorld(true);
  return { root: view.root.matrixWorld.clone(), bones: view.bones.map((b) => b.matrixWorld.clone()) };
}
function dmSpheres(pose, group) {
  const out = [];
  for (const v of DM.group.get(group) || []) {
    const M = v.bone === 0x4000 ? pose.root : pose.bones[v.bone]; if (!M) continue;
    const p = new THREE.Vector3(...v.off).applyMatrix4(M);
    out.push({ c: v3(p.x, p.y, p.z), r: v.r, kind: v.kind });
  }
  return out;
}
/* The game's contact test (docs/archive/hit-contact): every volume is a sphere, hit when |b − a|² < (ra + rb)². Only
   kind 4 spheres of the armed group hurt (kind 6 = clash / guard, 5 = projectile, 7 = trail points); the defender's
   hurt spheres are its group-0 kind 3 (legs) then kind 2 (hip, neck). Each kind 4 sphere is also swept back toward
   where it was last tick, one copy every r metres until the gap left is at most 1.5 r. A window's first tick only
   records positions, so it connects from start + 1. Attack classes 4, 5, 9 and 12–15 never hurt. */
const DM_NO_HURT = new Set([4, 5, 9, 12, 13, 14, 15]);
function dmHurtLocal() {                     // the rig's own hurt spheres in its standing pose, relative to its root
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === 1), mo = row ? row.motion : 0;
  const pose = dmPoseOf(mo, 0), out = [];
  for (const v of DM.group.get(0) || []) {
    if (v.kind !== 2 && v.kind !== 3) continue;
    const M = v.bone === 0x4000 ? pose.root : pose.bones[v.bone]; if (!M) continue;
    const p = new THREE.Vector3(...v.off).applyMatrix4(M);
    out.push({ c: v3(p.x, p.y, p.z), r: v.r, kind: v.kind });
  }
  return out.length ? out : [{ c: v3(0, 0.95, 0), r: 0.3, kind: 2 }, { c: v3(0, 1.45, 0), r: 0.3, kind: 2 }, { c: v3(0.1, 0.35, 0), r: 0.3, kind: 3 }, { c: v3(-0.1, 0.35, 0), r: 0.3, kind: 3 }];
}
/* The dummy is a second copy of the character: it plays the motion of its reaction script (500 flinch, 560 stagger,
   700 blow-away, 702 down, 102 landing …, from motion_map.json; 1 = standing) and its hurt spheres follow that pose. */
const DM_BODY = { scene: null, root: null, bones: [], partMeshes: [], gmo: null, cache: new Map() };
function dmScriptMotion(script) {
  const rows = F(msPath('motion_map.json')).rows, r = rows.find((x) => x.script === script) || rows.find((x) => x.script === 1);
  return r ? r.motion : 0;
}
function dmBodyEnsure() {
  if (DM_BODY.gmo === S.gmo && DM_BODY.root) return DM_BODY;
  if (DM_BODY.root) view.scene.remove(DM_BODY.root);
  if (DM_BODY.bonesHelper) view.scene.remove(DM_BODY.bonesHelper);
  DM_BODY.scene = view.scene; DM_BODY.root = null; DM_BODY.cache.clear();
  buildCharacter(S.gmo, DM_BODY);
  DM_BODY.gmo = S.gmo;
  DM_BODY.partMeshes.forEach((pm, pi) => { if (pm) pm.mesh.visible = pi === 0; });   // the body only, no weapons
  DM_BODY.root.visible = false;
  return DM_BODY;
}
function dmBodyPose(motion, t) {             // pose the dummy copy at motion tick t (held at its last frame, idle loops)
  const B = dmBodyEnsure(), L = motionLen(motion), fr = motion === dmScriptMotion(1) ? (t / 2) % Math.max(1, L) : Math.min(t / 2, L);
  poseMotion(S.gmo, B, motion, fr);
}
function dmHurtAt(script, t) {               // the hurt spheres of that pose, in the dummy's own frame (cached)
  const mo = dmScriptMotion(script || 1), key = mo + ':' + Math.round(t);
  const B = dmBodyEnsure();
  if (B.cache.has(key)) return B.cache.get(key);
  const keep = { p: B.root.position.clone(), r: B.root.rotation.y };
  B.root.position.set(0, 0, 0); B.root.rotation.y = 0;
  dmBodyPose(mo, t); B.root.updateMatrixWorld(true);
  const out = [];
  for (const v of DM.group ? DM.group.get(0) || [] : []) {
    if (v.kind !== 2 && v.kind !== 3) continue;
    const M = v.bone === 0x4000 ? B.root.matrixWorld : B.bones[v.bone] && B.bones[v.bone].matrixWorld; if (!M) continue;
    const p = new THREE.Vector3(...v.off).applyMatrix4(M);
    out.push({ c: v3(p.x, p.y, p.z), r: v.r, kind: v.kind });
  }
  B.root.position.copy(keep.p); B.root.rotation.y = keep.r;
  const res = out.length ? out : DM.hurt;
  B.cache.set(key, res);
  return res;
}
function dmHurt(f) {                          // the dummy's hurt spheres where it stands (turned to its facing), in its reaction's pose
  const hs = f.motion != null && S.gmo ? dmHurtAt(f.motion, f.motionT || 0) : DM.hurt;
  return hs.map((h) => { const c = vRotY(h.c, -f.facing); return { c: vAdd(f.pos, c), r: h.r, kind: h.kind }; });
}
function dmSweep(now, prev) {                 // the kind 4 spheres plus their copies back toward last tick
  const out = [];
  now.forEach((s, i) => {
    out.push(s);
    const p = prev && prev[i]; if (!p) return;
    let gap = vSub(p.c, s.c), d = vLen(gap); const step = vNorm(gap);
    let c = s.c;
    while (d > 1.5 * s.r && out.length < 64) { c = vAdd(c, vMul(step, s.r)); d -= s.r; out.push({ c, r: s.r, kind: s.kind, swept: true }); }
  });
  return out;
}
function dmTouch(f, spheres) {
  const hurt = dmHurt(f);
  for (const k of [3, 2]) for (const h of hurt) if (h.kind === k) for (const s of spheres) {
    const d = vSub(s.c, h.c), rr = s.r + h.r;
    if (d.x * d.x + d.y * d.y + d.z * d.z < rr * rr) return k;
  }
  return 0;
}
function dmHeight() {                        // how high above the floor the fight is (the attacker hovers there)
  if (DM.height != null) return DM.height;
  const m = S.step && S.moves && S.moves[S.step.move];
  if (!m) return 0;
  const ph = mvKind(m).phase, a = m.steps[0] && S.actionById.get(m.steps[0].action);
  return (ph ? ph === 'midair' : a && mvRaw(a)[MV_START_BYTE] === 1) ? 4 : 0;    // without the ISO: the first press's start byte
}
function dmKey() {
  const segs = dmSegments();
  return JSON.stringify([segs, DM.dist, dmHeight(), DM.above, S.gen || 0, S.skinShown, S.rig && S.rig.gmo, !!S.exMode, DM.does, DM.wall, typeof HK_PREV !== 'undefined' ? [HK_PREV.level, HK_PREV.pick] : 0]);   // the move panel's charge level / shot pick
}
/* projectiles (magi.js, docs/archive/projectile-flight): fired by fire_projectile (0x0204) and by a hit_window_fire
   (0x0743) that connects; flown tick by tick with homing on the dummy; their hit sphere (or upright column) is swept
   from last tick and tested against the dummy's hurt spheres; link_* children are fired as the game does. */
function dmMagiRecords() {
  if (S.fx && S.fx.magi && S.fx.magi.size) return S.fx.magi;
  const j = F(msPath('projectiles.json'));
  return j ? magiFromJson(j) : new Map();
}
const dmMat = (M) => { const e = M.elements; return { X: v3(e[0], e[1], e[2]), Y: v3(e[4], e[5], e[6]), Z: v3(e[8], e[9], e[10]), T: v3(e[12], e[13], e[14]) }; };
function dmShotTouch(hit, f) {               // the shot's (swept) hit shape against the dummy's hurt spheres
  const hurt = dmHurt(f);
  const [p, q] = hit.type === 'sphere' ? [hit.c0, hit.c] : [vSub(hit.c, v3(0, hit.halfH, 0)), vAdd(hit.c, v3(0, hit.halfH, 0))];
  for (const k of [3, 2]) for (const h of hurt) if (h.kind === k && MAGI.segDist(p, q, h.c, h.c) < hit.r + h.r) return k;
  return 0;
}
/* The recomp runtime's per-character hooks (mod.json runtime_hooks = roster.txt tokens; runtime/roster.cpp) that move the
   attacker or chain his actions in game, so the dummy sees what the game does with them:
     warp=MOTION[/MOTION...],TRAVEL,STOP[,LIFT[,FROM]]  warpreach=MOTION[/...],REACH   (warp_step_hook)
       while the motion plays, each root-motion step becomes the same share of the way to E = S + (T−S)·min(|T−S|−STOP,
       REACH or TRAVEL)/|T−S| (S = where the travel started, T = the target's feet now), the share being this step's part
       of what is left of TRAVEL metres; LIFT −1 = flat (horizontal only, the step's own height kept), LIFT > 0 = end no
       lower than LIFT above a lower target
     warpabove=MOTION[/...],FROM,TO,UP[,BACK]  (warpabove_step)  between motion ticks FROM..TO each step is an equal
       share of the way left to UP m above the target's feet, BACK m short of it horizontally (a teleport onto the foe)
     vtrack=MOTION[/...],FROM,TO,MAX[,LIFT]  (vtrack_step)  between motion ticks FROM..TO the height eases (smoothstep)
       toward the target's feet, at most MAX from the motion's own height, at most 2·MAX/(TO−FROM) per tick
     hitact=MAGI,ACTION  (hit_spawn_hook)  spawning that projectile (an on-hit child) starts ACTION
     landact=ACTION,MOTION[,...]  (land_hook)  a touchdown while one of those motions plays starts ACTION */
function dmHooks() {
  const H = { warps: [], reach: new Map(), vtracks: [], aboves: [], hitact: null, landact: null };
  if (!S.mod || S.mod.kind === 'retail' || (S.rig && S.rig.retail)) return H;
  const nums = (s) => s.split(',').map(Number);
  for (const t of S.mod.runtime_hooks || []) {
    if (typeof t !== 'string' || !t.includes('=')) continue;
    const k = t.slice(0, t.indexOf('=')), v = t.slice(t.indexOf('=') + 1), [ms, ...rest] = v.split(',');
    const motions = ms.split('/').map(Number);
    if (k === 'warp') { const [travel, stop, lift, from] = rest.map(Number); H.warps.push({ motions, travel, stop, flat: lift === -1, lift: lift > 0 ? lift : 0, from: from > 0 ? from : 0 }); }
    else if (k === 'warpreach') for (const m of motions) H.reach.set(m, Number(rest[0]));
    else if (k === 'warpabove') { const [from, to, up, back] = rest.map(Number); H.aboves.push({ motions, from, to, up, back: back || 0 }); }
    else if (k === 'vtrack') { const [from, to, max, lift] = rest.map(Number); H.vtracks.push({ motions, from, to, max, lift: lift || 0 }); }
    else if (k === 'hitact') { const [m, a] = nums(v); H.hitact = { magi: m, action: a }; }
    else if (k === 'landact') { const [a, ...mo] = nums(v); H.landact = { action: a, motions: mo }; }
  }
  return H;
}
function dmRootHook(H, ctx) {                 // ctx: { motion, t (motion tick), run } -> (rd, a, foe) => rd
  return (o, a, foe) => {
    if (!foe) return o;
    const R = ctx.run;
    const wa = (H.aboves || []).find((w) => w.motions.includes(ctx.motion) && ctx.t >= w.from && ctx.t < w.to);
    if (wa) {                                 // warpabove_step: an equal share of the way left to just above the foe
      const T = foe.pos, hx = T.x - a.pos.x, hz = T.z - a.pos.z, hd = Math.hypot(hx, hz), b = hd > 1e-4 ? Math.min(wa.back, hd) / hd : 0;
      const E = v3(T.x - hx * b, T.y + wa.up, T.z - hz * b), left = Math.max(1, Math.ceil(wa.to - ctx.t - 1e-3));
      return v3((E.x - a.pos.x) / left, (E.y - a.pos.y) / left, (E.z - a.pos.z) / left);
    }
    const vt = H.vtracks.find((v) => v.motions.includes(ctx.motion));
    if (vt) {                                 // vtrack_step
      if (R.vtMotion !== ctx.motion) { R.vtMotion = ctx.motion; R.vtApplied = 0; }
      if (ctx.t >= vt.from && ctx.t < vt.to) {
        const span = Math.max(vt.to - vt.from, 1), u = Math.min(Math.max((ctx.t + 1 - vt.from) / span, 0), 1), ease = u * u * (3 - 2 * u);
        const B = a.pos.y + o.y - R.vtApplied; let Y = foe.pos.y;
        if (Y < B && vt.lift > 0) Y = Math.min(B, Y + vt.lift);
        const goal = Math.min(Math.max(Y - B, -vt.max), vt.max), lim = 2 * vt.max / span;
        const d = Math.min(Math.max(ease * goal - R.vtApplied, -lim), lim);
        o = v3(o.x, o.y + d, o.z); R.vtApplied += d;
      }
    }
    const sp = H.warps.find((w) => w.motions.includes(ctx.motion));
    if (!sp) { R.warp = null; return o; }
    if (!R.warp || R.warp.motion !== ctx.motion) R.warp = { motion: ctx.motion, done: -1, S: null };
    if (ctx.t < sp.from) return o;           // FROM: the motion's own steps until then
    const W = R.warp, m = sp.flat ? Math.hypot(o.x, o.z) : vLen(o);
    if (!(m > 1e-6) || W.done >= sp.travel) return o;
    if (W.done < 0) { W.done = 0; W.S = { ...a.pos }; }
    const rem = sp.travel - W.done, frac = m >= rem - 1e-4 ? 1 : m / rem;
    W.done = frac >= 1 ? sp.travel : W.done + m;
    const T = foe.pos, D = v3(T.x - W.S.x, sp.flat ? 0 : T.y - W.S.y, T.z - W.S.z), d = vLen(D);
    let E = { ...W.S };
    if (d > 1e-4) {
      const along = Math.min(Math.max(d - sp.stop, 0), H.reach.has(ctx.motion) ? H.reach.get(ctx.motion) : sp.travel);
      E = vAdd(W.S, vMul(D, along / d));
      if (sp.lift > 0 && T.y + sp.lift < W.S.y && E.y < T.y + sp.lift) E.y = T.y + sp.lift;
    }
    return v3((E.x - a.pos.x) * frac, sp.flat ? o.y : (E.y - a.pos.y) * frac, (E.z - a.pos.z) * frac);
  };
}
function dmActionMotion(action) {           // an action id -> the motion its script plays
  const a = S.actionById.get(action); if (!a) return -1;
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === a.script);
  return row ? row.motion : -1;
}
function dmStand() {                         // the rig's volumes in its standing pose: hurt (2, 3), reference (1), target (10)
  const row = F(msPath('motion_map.json')).rows.find((r) => r.script === 1), pose = dmPoseOf(row ? row.motion : 0, 0);
  const out = { hurt: [], refY: 0.9, refR: 0.6, aimY: null };
  for (const v of DM.group ? DM.group.get(0) || [] : []) {
    const M = v.bone === 0x4000 ? pose.root : pose.bones[v.bone]; if (!M) continue;
    const p = new THREE.Vector3(...v.off).applyMatrix4(M);
    if (v.kind === 2 || v.kind === 3) out.hurt.push({ c: v3(p.x, p.y, p.z), r: v.r, kind: v.kind });
    else if (v.kind === 1) { out.refY = p.y; out.refR = v.r; }
    else if (v.kind === 10) out.aimY = p.y;
  }
  if (!out.hurt.length) out.hurt = dmHurtLocal();
  return out;
}
function dmSimulate() {
  let segs = dmSegments().map((x) => ({ ...x }));
  const endOf = () => segs.reduce((x, s) => Math.max(x, s.start + s.len), 0);
  const hooks = dmHooks(), run = {};
  const H = dmHeight();                       // the floor is at 0; the fight starts H above it
  if (typeof edVolPath === 'function' && edVolPath()) DM.group = dmParseVolumes(S.files[edVolPath()].bin);   // edits show at once
  const keep = view.root ? { p: view.root.position.clone(), r: view.root.rotation.y } : null;
  if (view.root) { view.root.position.set(0, 0, 0); view.root.rotation.y = 0; view.root.updateMatrixWorld(true); }   // model space while posing
  const code = S.rig.retail ? S.rig.retail.code : ((S.mod && S.mod.character) || {}).based_on || '';
  const stand = dmStand(); DM.hurt = stand.hurt;
  const a = ACT.makeActor(v3(0, H, 0), 0, ACT.specOf(code), stand.refY); a.aimY = stand.aimY;
  const f = dmFoe(v3(0, Math.max(0, H + DM.above), DM.dist), Math.PI);   // above / below the attacker, never under the floor
  const frames = [], hits = [], hitAt = new Map(), prevSph = new Map(), notes = [];
  const magi = dmMagiRecords(); let shots = []; const shotLog = []; MAGI.setSeed(1);
  const poseCache = new Map(), segOf = (T) => { let i = segs.findIndex((x) => T < x.start + x.len); return i < 0 ? segs.length - 1 : i; };
  const poseAt = (T) => { if (!poseCache.has(T)) { const i = segOf(T); poseCache.set(T, dmPoseOf(segs[i].motion, T - segs[i].start)); } return poseCache.get(T); };
  const ti = S.boneIndex.get('trans_include');
  const rootAt = (T) => {                     // the animated trans_include bone (model space, +z forward)
    const M = ti != null && poseAt(T).bones[ti]; if (!M) return v3(0, 0, 0);
    const e = M.elements; return { x: e[12], y: e[13], z: e[14], yaw: Math.atan2(e[8], e[10]) };
  };
  const hist = [];                             // the attacker per tick: {pos, yaw}
  const toW = (T, p) => { const h = hist[T] || { pos: a.pos, yaw: a.yaw }; return vAdd(h.pos, vRotY(p, h.yaw)); };
  const matW = (T, M) => { const m = dmMat(M), y = (hist[T] || a).yaw; return { X: vRotY(m.X, y), Y: vRotY(m.Y, y), Z: vRotY(m.Z, y), T: toW(T, m.T) }; };
  const attacker = (T) => ({ pos: { ...hist[T].pos }, fwd: ACT.fwd(hist[T].yaw),
    bonePos: (bi) => { const M = poseAt(T).bones[bi]; return M ? toW(T, v3(M.elements[12], M.elements[13], M.elements[14])) : hist[T].pos; } });
  const ownerAt = (T) => ({ pos: { ...hist[T].pos }, fwd: ACT.fwd(hist[T].yaw), axisZ: ACT.fwd(hist[T].yaw),
    centre: v3(hist[T].pos.x, hist[T].pos.y + stand.refY, hist[T].pos.z), aim: v3(hist[T].pos.x, hist[T].pos.y + (stand.aimY ?? stand.refY), hist[T].pos.z),
    bone: (bi) => { const M = poseAt(T).bones[bi]; return M ? matW(T, M) : null; } });
  const foeActor = () => ({ pos: { ...f.pos }, fwd: dmFwd(f), centre: v3(f.pos.x, f.pos.y + stand.refY, f.pos.z), aim: v3(f.pos.x, f.pos.y + (stand.aimY ?? stand.refY), f.pos.z) });
  const effectAt = (T) => (id) => {           // origin 6: where the owner's FEPA record id plays (bone × offset)
    const rec = S.fx && (S.fx.byId.get(id) || [])[0]; if (!rec) return null;
    const P = poseAt(T), bi = rec.bone !== 'root' && S.boneIndex.has(rec.bone) ? S.boneIndex.get(rec.bone) : -1;   // 'root' = the character's root
    return matW(T, (bi >= 0 ? P.bones[bi] : P.root).clone().multiply(new THREE.Matrix4().makeTranslation(...(rec.pos || [0, 0, 0]))));
  };
  const fire = (id, T, opts) => { if (typeof hkPickMagi === 'function') id = hkPickMagi(id);   // magipick=: the member the move panel picked
    if (hooks.hitact && id === hooks.hitact.magi) switchTo(Math.max(0, T - 1), hooks.hitact.action, `projectile ${id} spawned (hitact=)`);
    const r = magi.get(id); if (!r) return;
    const made = MAGI.spawnShots(r, () => ownerAt(Math.min(T, hist.length - 1)), foeActor, { effect: effectAt(Math.min(T, hist.length - 1)), floorY: 0, ...opts }, T);
    for (const sh of made) { sh.path = []; sh.hitChain = !!opts.viaHit; shotLog.push({ id: r.id, T: sh.tick, child: !!opts.parent, hit: sh.hitChain, path: sh.path }); }   // hit: born of a link_hit (or its children)   // the flight, for the effects (dmShotAt)
    shots.push(...made); };
  const hitRec = (id) => F(msPath('hits.json')).hits.find((x) => parseInt(x.id, 16) === id);
  const log = (T, si, h, extra = {}) => hits.push({ T, press: si, name: h.name || h.id, reaction: h.reaction, hp: !!(parseInt(h.flags, 16) & 2), power: h.power, height: f.pos.y, ...extra });
  let landedAt = null, switches = 0;
  const cancelled = new Set(), defence = [];
  const wall = DM.wall != null ? { point: v3(0, 0, DM.dist + DM.wall), normal: v3(0, 0, -1) } : null;
  const gvol = ((DM.group && DM.group.get(4)) || []).find((r) => r.kind === 6);       // the fighter's own GUARD sphere
  const guardSphere = () => ({ c: v3(f.pos.x, f.pos.y + (gvol && gvol.bone === 0x4000 ? gvol.off[1] : 0.85), f.pos.z), r: gvol ? gvol.r : 1.1 });
  const rootCache = new Map();
  const dodgeRoot = (script) => {             // the dodge motion's trans_include travel from its first tick (model space)
    const mo = dmScriptMotion(script), r0 = dmPoseOf(mo, 0).bones[ti], e0 = r0 ? r0.elements : null;
    return (t) => { const k = mo + ':' + t; if (!rootCache.has(k)) { const M = dmPoseOf(mo, t).bones[ti]; rootCache.set(k, M && e0 ? v3(M.elements[12] - e0[12], 0, M.elements[14] - e0[14]) : v3(0, 0, 0)); } return rootCache.get(k); };
  };
  const threat = (si, local, lead) => {       // an attack starts within `lead` ticks in this press
    const s = segs[si], p = S.tlOfMotion.get(s.motion), t = p && F(p); if (!t) return false;
    return t.ranged.some((e) => DM_HIT_OPS.has(opCode(e.op)) && evOk(e, s) && e.start > local && e.start <= local + lead) ||
      t.keyed.some((e) => opCode(e.op) === 0x0204 && evOk(e, s) && e.frame > local && e.frame <= local + lead);
  };
  const recoilAttacker = (T, code) => {       // a blocked attack: the attacker recoils (901, 911 in the air), its move ends
    const air = a.pos.y > 0.001, mo = dmScriptMotion(air ? 911 : 901);
    const si = segOf(T); segs = segs.slice(0, si + 1); segs[si].len = T - segs[si].start + 1;
    segs.push({ motion: mo, start: T + 1, len: Math.min(80, motionLen(mo) * 2), why: 'blocked: recoil' });
    const back = vMul(ACT.fwd(a.yaw), -1);
    // FUN_0884A4C4: S = (2 + 3k)/16, k from the attacker's motion time t against its latest hit window [wa, wb] so far
    // in this script (FUN_0882FD08: wa = max start, wb = max end, an open end = the motion's end): 1 up to the window's
    // start, falling to 0 at its end
    const sg = segs[si], local = T - sg.start, tp = S.tlOfMotion.get(sg.motion), tl = tp && F(tp);
    let wa = -1, wb = -1;
    if (tl) for (const e of tl.ranged) if ((opCode(e.op) === 0x0200 || opCode(e.op) === 0x0743) && evOk(e, sg) && e.start <= local) {
      wa = Math.max(wa, e.start); wb = Math.max(wb, e.end === FOREVER || e.end === 0xFFFF ? Math.max(local, motionLen(sg.motion) * 2) : e.end);
    }
    const kk = local <= wa ? 1 : local >= wb ? 0 : 1 - (local - wa) / (wb - wa), SS = (2 + 3 * kk) / 16;
    a.recoil = code === 6 ? { dir: vRotY(back, Math.PI / 2), S: 2 * SS } : { dir: back, S: code === 2 ? 0 : SS };
    notes.push(`tick ${T}: guarded (outcome ${code}) → recoil`);
  };
  // An EX Burst holds its target: the foe is never out of reach (down, invincible) between the burst's hits. How the
  // game does it isn't traced; the dummy just stays hittable while the burst plays.
  const burst = !!(S.step && S.moves[S.step.move] && S.moves[S.step.move].burst);
  const switchTo = (T, action, why) => {      // a runtime hook starts another action next tick: the rest of the move is replaced
    const mo = dmActionMotion(action); if (mo < 0 || switches > 3) return;
    switches++;
    const si = segOf(T); segs = segs.slice(0, si + 1); segs[si].len = T - segs[si].start + 1;
    segs.push({ motion: mo, start: T + 1, len: motionLen(mo) * 2, why, action });
    notes.push(`tick ${T}: ${why} → action ${action} (${motionName(mo)})`);
  };
  // the event filter's phase byte (low byte of keyed x / ranged pad): an event runs only for the press whose motion-map
  // phase matches (a charge's three releases share one animation and fire one shot each)
  const phCache = new Map();
  const segPhase = (sg) => {
    if (!sg || sg.action == null) return null;
    if (!phCache.has(sg.action)) {
      const a = S.actionById.get(sg.action), row = a && F(msPath('motion_map.json')).rows.find((r) => r.script === a.script && r.motion === sg.motion);
      phCache.set(sg.action, row ? row.phase & 0xFF : null);
    }
    return phCache.get(sg.action);
  };
  const evOk = (e, sg) => { if (!evMode(e)) return false; const ph = (('pad' in e ? e.pad : e.x) || 0) & 0xFF, sp = segPhase(sg); return !ph || sp == null || ph === sp; };
  let charged = false;
  for (let T = 0; T <= endOf(); T++) {
    const si = segOf(T), seg = segs[si], local = T - seg.start, p = S.tlOfMotion.get(seg.motion), tl = p && F(p);
    const chg = !charged && !seg.why && typeof hkChargeOf === 'function' ? hkChargeOf(seg.motion) : null;   // charge= (charge_tick)
    if (chg && local === hkHold(chg, HK_PREV.level)) { charged = true; switchTo(T, chg.acts[HK_PREV.level - 1], `charge= release, level ${HK_PREV.level} (held ${local} ticks)`); }
    // 1. the attacker: events, movement state, root motion, floor (docs/archive/attacker-movement)
    const keyed = tl ? tl.keyed.filter((e) => e.frame === local && evOk(e, seg)).map(ACT.evArgs) : [];
    const ranged = tl ? tl.ranged.filter((e) => evOk(e, seg) && e.start <= local && (e.end === FOREVER || local < e.end)).map(ACT.evArgs) : [];
    if (local === 0) {
      ACT.startMove(a, ranged);
      const act = seg.action != null && S.actionById.get(seg.action);   // plya +0x18 bit 0: a homing dash as the action starts
      if (act && (edU32(act, 0x18) & 1)) {
        const fl = edU32(act, 0x18), g = (o) => edF32(act, o);
        ACT.startHoming(a, { pos: f.pos, refY: stand.refY, aimY: stand.aimY }, { speed: g(0x40), vspeed: g(0x44), track_ticks: g(0x48), arrive_radius: g(0x4C), max_ticks: g(0x50), height: g(0x54), angle: g(0x58) * 180 / Math.PI, distance: g(0x5C),
          args: [fl & 0x08000000 ? 1 : 0, fl & 0x10000000 ? 1 : 0, fl & 0x20000000 ? 1 : 0, 0, 0] }, false);
      }
    }
    if (DM.does !== 'none' && f.state === 'idle' && f.canActIn <= 0 && !f.guard && !f.dodge && threat(si, local, DM.does === 'guard' ? 6 : 4)) {
      if (DM.does === 'guard') dmGuardStart(f); else dmDodgeStart(f, DM.does, a.pos, dodgeRoot(DM_DODGE_SCRIPT[f.pos.y > 0.001 ? 'air' : DM.does]));
      defence.push({ T, what: DM.does === 'guard' ? 'guard' : 'dodge ' + f.dodge.kind });
    }
    const reeling = f.motion >= 500 && f.motion <= 798 && f.state !== 'idle';
    a.log = notes; const wasLanded = a.landed;
    ACT.stepActor(a, { pos: f.pos, refY: stand.refY, aimY: stand.aimY, busy: !!f.dodge, reeling }, keyed, ranged, rootAt(T), dmRootHook(hooks, { motion: seg.motion, t: local, run }));
    if (a.landed && !wasLanded && T > 0) {
      if (hooks.landact && hooks.landact.motions.includes(seg.motion)) switchTo(T, hooks.landact.action, 'touchdown (landact=)');
      else if (landedAt == null) landedAt = T;
    }
    if (a.groundContact) {                    // 0x0070 (Kain's dive): on the floor, the action's +0x12 script plays (FUN_08B2F054)
      a.groundContact = false;
      const act = seg.action != null && S.actionById.get(seg.action), ls = act ? new DataView(mvRaw(act).buffer).getUint16(0x12, true) : 0xFFFF;
      const mo = ls && ls !== 0xFFFF ? dmScriptMotion(ls) : -1;
      if (mo >= 0 && !seg.why && switches <= 3) {
        switches++; const si2 = segOf(T); segs = segs.slice(0, si2 + 1); segs[si2].len = T - segs[si2].start + 1;
        segs.push({ motion: mo, start: T + 1, len: motionLen(mo) * 2, why: `touchdown (ground_contact_switch, script ${ls})`, action: seg.action });
        notes.push(`tick ${T}: touchdown → script ${ls} (${motionName(mo)})`);
      }
    }
    if (a.recoil) { a.pos = vAdd(a.pos, vMul(a.recoil.dir, a.recoil.S)); a.recoil.S *= 0.9; if (a.recoil.S < 0.005) a.recoil = null; }
    hist[T] = { pos: { ...a.pos }, yaw: a.yaw };
    // 2. the dummy moves (its reaction), then 3. the hit test against everyone's pose of this tick
    const before = { ...f.pos };
    dmTickFoe(f, attacker(T)); dmGuardTick(f); dmDodgeTick(f);
    if (wall && dmWallCheck(f, wall, vSub(f.pos, before))) { defence.push({ T, what: 'wall rush' }); notes.push(`tick ${T}: wall rush (${f.motion})`); }
    if (wall && f.pos.z > wall.point.z - 0.3) f.pos.z = wall.point.z - 0.3;      // the wall stops the body
    // body push (kind-1 spheres, FUN_08833360 → FUN_0883E904): each fighter that moved this tick resolves against the
    // other, the attacker first; a dummy in a straight flight (state 3) is never pushed; no_body_collision skips it
    if (!a.flags.no_body_collision && !(f.move && f.move.kind === 'line')) {
      const A1 = { heavy: !!a.flags.heavy_body, unpushable: !!a.flags.cant_be_pushed }, F1 = { heavy: false, unpushable: false };
      const prevA = hist[T - 1] ? hist[T - 1].pos : a.pos, aMoved = vLen(vSub(a.pos, prevA)) > 1e-6, fMoved = vLen(vSub(f.pos, before)) > 1e-6;
      const pass = (selfIsA) => {
        const ca = v3(a.pos.x, a.pos.y + stand.refY, a.pos.z), cf = v3(f.pos.x, f.pos.y + stand.refY, f.pos.z);
        const d = selfIsA ? dmPushVec(ca, stand.refR, cf, stand.refR) : dmPushVec(cf, stand.refR, ca, stand.refR); if (!d) return;
        const [ds, dO] = selfIsA ? dmPushSplit(d, A1, F1) : dmPushSplit(d, F1, A1);
        if (selfIsA) { a.pos = vAdd(a.pos, ds); f.pos = vAdd(f.pos, dO); } else { f.pos = vAdd(f.pos, ds); a.pos = vAdd(a.pos, dO); }
      };
      if (aMoved || !fMoved) pass(true);
      if (fMoved) pass(false);
      hist[T] = { pos: { ...a.pos }, yaw: a.yaw };
    }
    if (burst) f.inv = 0;
    let shown = []; const seen = new Set();
    if (tl && DM.group) tl.ranged.forEach((e, idx) => {
      const c = opCode(e.op); if (!DM_HIT_OPS.has(c) || !evOk(e, seg) || !(e.start <= local && (e.end === FOREVER || local < e.end))) return;
      const h = hitOf(e.hit), group = e.group ?? argsOf(e)[1]; if (!h) return;
      const key = si + ':' + idx; seen.add(key);
      const now = dmSpheres(poseAt(T), group).filter((s) => s.kind === 4).map((s) => ({ ...s, c: toW(T, s.c) }));
      const first = !prevSph.has(key), sp = dmSweep(now, prevSph.get(key)); prevSph.set(key, now);
      shown = shown.concat(sp);
      if (first || f.inv > 0 || DM_NO_HURT.has(h.class) || cancelled.has(key)) return;     // a window's first tick only records positions
      if (f.guard) {
        const g = dmGuardResolve(f, sp, h, ACT.fwd(a.yaw), guardSphere());
        if (g && g.blocked) { cancelled.add(key); log(T, si, h, { guarded: g.attacker }); if ([1, 2, 6].includes(g.attacker)) recoilAttacker(T, g.attacker); if (g.guard === 1) { f.guard = null; f.state = 'stun'; f.motion = 901; f.motionT = 0; f.canActIn = 30; f.move = { kind: 'slide', dir: vMul(dmFwd(f), -1), S: 0.31 }; } return; }
        if (g && g.crushed) { hitAt.set(key, T); dmApplyHit(f, h, attacker(T)); log(T, si, h, { crushed: true }); return; }
      }
      const last = hitAt.get(key);
      if (last !== undefined && !(h.rehit > 0 && T - last >= h.rehit)) return;
      if (!dmTouch(f, sp)) return;
      hitAt.set(key, T); dmApplyHit(f, h, attacker(T)); log(T, si, h);
      if (c === 0x0743 && e.projectile != null) fire(e.projectile, T + 1, {});
    });
    for (const k of [...prevSph.keys()]) if (!seen.has(k)) prevSph.delete(k);
    if (tl) for (const e of tl.keyed) if (opCode(e.op) === 0x0204 && e.frame === local && evOk(e, seg)) fire(e.projectile ?? argsOf(e)[0], T, { arg2: (e.args || [])[0] });
    const next = [], drawn = [];
    for (const sh of shots) {
      if (sh.tick > T) { next.push(sh); continue; }
      const o = MAGI.stepShot(sh, foeActor());
      if (sh.path) sh.path.push({ T, p: { ...sh.pos }, d: { ...sh.dir } });
      let ev = o.events; const onHit = new Set();
      if (o.hit && o.hit.fresh && f.guard) {
        const h = hitRec(o.hit.atkp), g = h && dmGuardResolve(f, [{ c: o.hit.c || sh.pos, r: o.hit.r || 0.2 }], h, vNorm(v3(sh.dir.x, 0, sh.dir.z)), guardSphere());
        if (g && g.blocked) { log(T, si, h, { proj: sh.rec.id, guarded: g.attacker }); continue; }
      }
      if (o.hit && o.hit.fresh && f.inv <= 0 && dmShotTouch(o.hit, f)) {
        const ce = MAGI.shotContact(sh); ce.forEach((x) => onHit.add(x)); ev = ev.concat(ce);
        const h = hitRec(o.hit.atkp);
        if (h && !DM_NO_HURT.has(h.class)) {
          const d = v3(sh.dir.x, 0, sh.dir.z);
          dmApplyHit(f, h, { pos: { ...sh.pos }, fwd: vLen(d) ? vNorm(d) : ACT.fwd(a.yaw) }); log(T, si, h, { proj: sh.rec.id });
        }
      }
      const pull = MAGI.magiPull(sh, foeActor()); if (pull) f.pos = vAdd(f.pos, pull);
      for (const e2 of ev) fire(e2.fire, T + 1, { parent: e2.parent, viaHit: sh.hitChain || onHit.has(e2) });
      if (o.alive && sh.state <= 2) { next.push(sh); drawn.push({ c: { ...sh.pos }, r: o.hit ? o.hit.r : 0.15, live: !!o.hit }); }
    }
    shots = next;
    if (tl) for (const e of tl.keyed.filter((x) => opCode(x.op) === 0x0108 && x.frame === local).concat(tl.ranged.filter((x) => opCode(x.op) === 0x0108 && x.start === local)).filter(evMode)) {
      const h = hitOf(e.hit); if (h && f.inv <= 0) { dmApplyHit(f, h, attacker(T)); log(T, si, h, { forced: true }); }
    }
    if (wall && f.pos.z > wall.point.z - 0.3) f.pos.z = wall.point.z - 0.3;
    frames.push({ pos: { ...f.pos }, facing: f.facing, state: f.state, motion: f.motion, motionT: f.motionT, canAct: f.canActIn, inv: f.inv,
      spheres: shown, hurt: dmHurt(f), shots: drawn, apos: { ...a.pos }, ayaw: a.yaw, astate: a.state });
  }
  if (keep) { view.root.position.copy(keep.p); view.root.rotation.y = keep.r; view.root.updateMatrixWorld(true); }
  return { frames, hits, defence, wall, shotLog, total: endOf(), H, landedAt, spec: a.spec, segs, extra: segs.filter((x) => x.why), notes, move: S.step ? S.step.move : -1 };
}

/* ---------------------------------------------------------------- drawing */
const DM_COL = { guard: 0x6fb7ff, dodge: 0x8fd3ff, wall: 0xff9f6f, idle: 0x9097bd, stun: 0xe8ac52, launched: 0xff6f7f, air: 0xff6f7f, falling: 0xff9f6f, down: 0xc39bff, recovering: 0x6fd39b };
function dmEnsureMeshes() {
  if (DM.mesh) return;
  const g = new THREE.Group(), mat = new THREE.MeshBasicMaterial({ color: 0x9097bd, transparent: true, opacity: 0.55, depthWrite: false });
  const body = new THREE.Mesh(new THREE.CylinderGeometry(0.3, 0.3, 1.2, 16, 1, true), mat); body.position.y = 0.85;
  const top = new THREE.Mesh(new THREE.SphereGeometry(0.3, 16, 10), mat); top.position.y = 1.45;
  const bot = new THREE.Mesh(new THREE.SphereGeometry(0.3, 16, 10), mat); bot.position.y = 0.25;
  const nose = new THREE.Mesh(new THREE.ConeGeometry(0.1, 0.25, 10), new THREE.MeshBasicMaterial({ color: 0xffffff })); nose.rotation.x = Math.PI / 2; nose.position.set(0, 1.3, 0.35);
  const ring = new THREE.Mesh(new THREE.RingGeometry(0.42, 0.5, 40), mat); ring.rotation.x = -Math.PI / 2; ring.position.y = 0.02;   // the state colour under its feet
  g.add(body, top, bot, nose, ring); g.userData.mat = mat;
  DM.mesh = g;
  DM.path = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0xe8ac52, transparent: true, opacity: 0.7 }));
  DM.marks = new THREE.Group(); DM.vols = new THREE.Group();
  view.scene.add(DM.mesh, DM.path, DM.marks, DM.vols);
}
function dmShow(on) {
  if (DM_BODY.root) DM_BODY.root.visible = on && !!S.gmo && DM_BODY.gmo === S.gmo;
  if (!on && view.root && (view.root.position.lengthSq() || view.root.rotation.y)) { view.root.position.set(0, 0, 0); view.root.rotation.y = 0; view.root.updateMatrixWorld(true); DM.cam = null; }
  if (!DM.mesh) return; DM.mesh.visible = DM.path.visible = DM.marks.visible = on;
  DM.vols.visible = on && (typeof hbOn !== 'function' || hbOn());   // the spheres follow View → Hitboxes
}
function dmPlaceAttacker(fr, H) {           // the character where the simulation has him (root motion, homing, falls)
  view.root.position.set(fr.apos.x, fr.apos.y - H, fr.apos.z); view.root.rotation.y = fr.ayaw; view.root.updateMatrixWorld(true);
  const mid = new THREE.Vector3((fr.apos.x + fr.pos.x) / 2, 1.1 + Math.min(fr.apos.y, fr.pos.y) - H, (fr.apos.z + fr.pos.z) / 2);
  // Follow on: the camera keeps both fighters in view as they travel; off, it stays where you put it (2026-11-01:
  // it used to follow with Follow off, and then framed empty space late in Blitz Attack)
  if ($('camFollow') && $('camFollow').checked && !(typeof GCAM !== 'undefined' && GCAM.on) && (typeof camFree !== 'function' || camFree()))
    dmFrameBoth(new THREE.Vector3(fr.apos.x, fr.apos.y - H, fr.apos.z), new THREE.Vector3(fr.pos.x, fr.pos.y - H, fr.pos.z), mid);
  DM.cam = mid;
}
/* aim at the point between them from the same direction, far enough back that both bodies fit the field of view; your
   own zoom stays unless they need more room (a zoom since the last framing becomes the new least distance) */
function dmFrameBoth(A, B, mid) {
  const c = view.camera, t = view.controls.target, dir = c.position.clone().sub(t); let dist = dir.length();
  if (!(dist > 1e-3) || !(c.aspect > 0)) return;          // (a hidden viewport, the Character tab's art, has no width)
  dir.divideScalar(dist);
  if (DM.camSet == null || Math.abs(dist - DM.camSet) > 1e-3) DM.camBase = dist;
  const f = dir.clone().negate(), right = new THREE.Vector3().crossVectors(f, c.up).normalize(), up = new THREE.Vector3().crossVectors(right, f);
  const half = B.clone().sub(A).multiplyScalar(0.5), tv = Math.tan(c.fov * Math.PI / 360), th = tv * c.aspect;
  const need = Math.max((Math.abs(half.dot(right)) + 1.5) / th, (Math.abs(half.dot(up)) + 1.3) / tv) + Math.abs(half.dot(dir));
  const d = Math.min(150, Math.max(DM.camBase, need)); if (!isFinite(d)) return;
  t.copy(mid); c.position.copy(mid).addScaledVector(dir, d); view.controls.update(); DM.camSet = d;
}
function dmRefresh() {                        // called after every pose: re-simulate when needed, draw at the playhead
  const info = $('dmInfo'), card = $('dmCard');     // the Inspector's dummy card: its set-up, then what happens at the playhead
  if (!DM.on || !S.gmo || !view.root || S.fxEdit) { dmShow(false); if (card) card.hidden = true; dmApocStraight(); return; }
  dmEnsureMeshes();
  const key = dmKey();
  if (key !== DM.key) {
    // the run before an edit (only S.gen differs in the key) stays as the readout's "since edit" baseline
    // (edits clear DM.key to force a run, so the key of the last run is kept apart: DM.simKey)
    const was = DM.sim, same = (() => { try { const a = JSON.parse(DM.simKey), b = JSON.parse(key); a[4] = b[4] = 0; return JSON.stringify(a) === JSON.stringify(b); } catch (e) { return false; } })();
    if (key !== DM.simKey) DM.prev = same && was ? was : null;
    DM.key = DM.simKey = key; DM.sim = dmSimulate(); DM.apoc = dmApocAssign(DM.sim);
    pose0();                                  // the simulation posed other ticks: back to the playhead
    const H = DM.sim.H;
    const pts = DM.sim.frames.map((fr) => new THREE.Vector3(fr.pos.x, fr.pos.y - H + 0.85, fr.pos.z));
    DM.path.geometry.dispose(); DM.path.geometry = new THREE.BufferGeometry().setFromPoints(pts);
    DM.marks.clear();
    for (const h of DM.sim.hits) {
      const fr = DM.sim.frames[h.T], m = new THREE.Mesh(new THREE.SphereGeometry(0.09, 8, 6), new THREE.MeshBasicMaterial({ color: h.hp ? 0xff6f7f : 0xffd27a }));
      m.position.set(fr.pos.x, fr.pos.y - H + 0.85, fr.pos.z); DM.marks.add(m);
    }
    if (DM.sim.wall) {                        // the wall behind the dummy
      const w = new THREE.Mesh(new THREE.PlaneGeometry(8, 6), new THREE.MeshBasicMaterial({ color: 0x8fd3ff, transparent: true, opacity: 0.12, side: THREE.DoubleSide, depthWrite: false }));
      w.position.set(0, 3 - H, DM.sim.wall.point.z); DM.marks.add(w);
      const e = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.PlaneGeometry(8, 6)), new THREE.LineBasicMaterial({ color: 0x8fd3ff, transparent: true, opacity: 0.5 }));
      e.position.copy(w.position); DM.marks.add(e);
    }
    if (H > 0) {                              // the floor the dummy falls to
      const ring = new THREE.Mesh(new THREE.RingGeometry(1.2, 1.25, 48), new THREE.MeshBasicMaterial({ color: 0xc39bff, transparent: true, opacity: 0.5, side: THREE.DoubleSide }));
      ring.rotation.x = -Math.PI / 2; ring.position.set(DM.sim.frames[0].pos.x, -H, DM.sim.frames[0].pos.z); DM.marks.add(ring);
    }
  }
  dmShow(true);
  const T = Math.min(DM.sim.frames.length - 1, Math.round(dmNowTick())), fr = DM.sim.frames[T];
  const H = DM.sim.H;
  dmPlaceAttacker(fr, H); dmApocPlace(T);
  DM.mesh.position.set(fr.pos.x, fr.pos.y - H, fr.pos.z); DM.mesh.rotation.y = fr.facing;
  if (S.gmo) {                                 // the character copy playing its reaction
    const B = dmBodyEnsure();
    B.root.visible = true;
    B.root.position.set(0, 0, 0); B.root.rotation.y = 0;
    dmBodyPose(dmScriptMotion(fr.motion || 1), fr.motionT || 0);
    B.root.position.set(fr.pos.x, fr.pos.y - H, fr.pos.z); B.root.rotation.y = fr.facing; B.root.updateMatrixWorld(true);
    DM.mesh.children.forEach((c, i) => { c.visible = i === 3 || i === 4; });   // keep the facing cone and the state ring
  }
  DM.mesh.userData.mat.color.setHex(DM_COL[fr.state] || 0x9097bd);
  DM.mesh.userData.mat.opacity = fr.inv > 0 ? 0.25 : 0.55;
  DM.vols.clear();
  for (const s of fr.spheres) {               // the attack's spheres (fainter: the sweep back to last tick)
    const m = new THREE.Mesh(new THREE.SphereGeometry(s.r, 12, 8), new THREE.MeshBasicMaterial({ color: 0xff6f7f, wireframe: true, transparent: true, opacity: s.swept ? 0.15 : 0.4 }));
    m.position.set(s.c.x, s.c.y - H, s.c.z); DM.vols.add(m);
  }
  for (const s of fr.shots || []) {           // projectiles in flight (their hit sphere while it's live)
    const m = new THREE.Mesh(new THREE.SphereGeometry(Math.max(0.08, s.r), 12, 8), new THREE.MeshBasicMaterial({ color: 0xffd27a, wireframe: !s.live, transparent: true, opacity: s.live ? 0.35 : 0.5 }));
    m.position.set(s.c.x, s.c.y - H, s.c.z); DM.vols.add(m);
  }
  for (const s of fr.hurt || []) {            // the dummy's hurt spheres (green, as the attacker's)
    const m = new THREE.Mesh(new THREE.SphereGeometry(s.r, 12, 8), new THREE.MeshBasicMaterial({ color: 0x6fd39b, wireframe: true, transparent: true, opacity: 0.45 }));
    m.position.set(s.c.x, s.c.y - H, s.c.z); DM.vols.add(m);
  }
  if (card) card.hidden = false;
  if (info) {
    const got = DM.sim.hits.filter((h) => h.T <= T), last = got[got.length - 1], landed = got.filter((x) => !x.guarded).length, guarded = got.length - landed;
    const st = { guard: 'guarding', dodge: 'dodging', wall: 'wall rush', idle: 'standing', stun: 'hit-stun', launched: 'knocked', air: 'in the air', falling: 'falling', down: 'down', recovering: 'landing' }[fr.state] || fr.state;
    const stTip = { dodge: 'invincible 0–20', wall: 'pinned, invincible', down: 'invincible' }[fr.state] || '';
    const ast = { ground: 'on the ground', air: 'in the air', homing: 'homing dash', chase: 'chase dash', fall: 'falling' }[fr.astate] || fr.astate;
    const lastTxt = last ? `${last.name} · ${DM_REACT[last.reaction] || 'reaction ' + last.reaction}${last.hp ? ' · HP' : ` · ${last.power} brave`}` : (DM.sim.hits.length || DM.group ? '—' : 'no hit volumes in this moveset');
    const notes = [...(H ? [`fight at ${H} m`] : []), ...(DM.sim.landedAt != null && T >= DM.sim.landedAt ? [`landed at tick ${DM.sim.landedAt}: the game cuts the move there`] : []), ...DM.sim.notes.filter((n) => n.includes('→'))];
    const row = (k, v, tip) => `<span class="dmc-k">${k}</span><span class="dmc-v"${tip ? ` title="${esc(tip)}"` : ''}>${esc(v)}</span>`;
    info.innerHTML = `<div class="dmc"><div class="dmc-who"><b>Dummy</b><span class="dmc-st dmc-${esc(fr.state)}"${stTip ? ` title="${esc(stTip)}"` : ''}>${esc(st)}</span></div>
      <div class="dmc-grid">${row('Can act', fr.canAct > 0 && isFinite(fr.canAct) ? 'in ' + Math.ceil(fr.canAct) : 'now')}${row('Height', fr.pos.y.toFixed(2) + ' m')}${row('Hits', DM.sim.hits.length ? `${landed} / ${DM.sim.hits.length}${guarded ? ` (${guarded} guarded)` : ''}` : '—')}${row('Distance', vLen(v3(fr.pos.x - fr.apos.x, 0, fr.pos.z - fr.apos.z)).toFixed(1) + ' m')}</div>
      <div class="dmc-grid dmc-wide">${row('Last hit', lastTxt, lastTxt)}${(() => {   // knock-back since the last edit, at this frame
        const P = DM.prev && DM.prev.frames, q = P && P[Math.min(P.length - 1, T)]; if (!q) return '';
        const d = vLen(v3(fr.pos.x - fr.apos.x, 0, fr.pos.z - fr.apos.z)) - vLen(v3(q.pos.x - q.apos.x, 0, q.pos.z - q.apos.z));
        return Math.abs(d) < 0.05 ? '' : row('Since edit', (d > 0 ? '+' : '−') + Math.abs(d).toFixed(1) + ' m', 'Distance at this frame compared with before your last edit');
      })()}</div></div>
      <div class="dmc"><div class="dmc-who"><b>${esc((S.mod && S.mod.character && S.mod.character.name) || 'Attacker')}</b><span class="dmc-st">${esc(ast)}</span>${notes.length ? `<span class="dmc-note" title="${esc(notes.join('\n'))}">${notes.length === 1 ? '1 note' : notes.length + ' notes'}</span>` : ''}</div>
      <div class="dmc-grid">${row('Height', fr.apos.y.toFixed(2) + ' m')}${row('Travelled', vLen(v3(fr.apos.x, 0, fr.apos.z)).toFixed(1) + ' m')}</div></div>`;
  }
}
const pose0 = () => { poseMotion(S.gmo, view, S.motion, S.frame); if (typeof updateParts === 'function') updateParts(); };
function dmWire() {
  const on = $('dmOn'), d = $('dmDist'), h = $('dmHeight'); if (!on) return;
  on.onchange = async () => {
    DM.on = on.checked;
    if (DM.on && !DM.group) await dmLoadVolumes();
    if (DM.on) {                              // a side view, so both fighters and the gap between them show
      const c = view.camera, t = view.controls.target, mid = DM.dist / 2;
      c.position.set(7.5, 2.6, mid + 1.2); t.set(0, 1.1, mid); view.controls.update();
    }
    DM.key = ''; dmRefresh();
  };
  d.onchange = () => { DM.dist = Math.max(0, parseFloat(d.value) || 0); dmRefresh(); };
  const dd = $('dmDoes'); if (dd) { dd.innerHTML = DM_DOES.map(([v, l]) => `<option value="${v}">${l}</option>`).join(''); dd.onchange = () => { DM.does = dd.value; dmRefresh(); }; }
  const dw = $('dmWall'); if (dw) dw.onchange = () => { DM.wall = dw.value === '' ? null : Math.max(0.5, parseFloat(dw.value) || 0); dmRefresh(); };
  h.onchange = () => { DM.height = h.value === '' ? null : Math.max(0, parseFloat(h.value) || 0); dmRefresh(); };
  const ab = $('dmAbove'); if (ab) ab.onchange = () => { DM.above = parseFloat(ab.value) || 0; dmRefresh(); };
}
async function dmRigChanged() { DM.group = null; DM.key = ''; if (DM.on) { await dmLoadVolumes(); dmRefresh(); } }
dmWire();
