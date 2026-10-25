/* Move Studio: projectile (magi) flight for the dummy, from docs/archive/projectile-flight/README.md §9 (copied as
   written there, inside a scope so its names don't meet fx.js's). window.MAGI = {spawnShots, stepShot, ...}. */
window.MAGI = (() => {
/* Magi projectile flight for the Move Studio (docs/archive/projectile-flight/README.md).
   Metres, ticks (1/60 s, dt = 1), the record's angles in degrees. Vectors are {x, y, z}; an actor is
   { pos, fwd, axisZ?, centre?, aim?, bone?(id) -> {X, Y, Z, T} }  (fwd = +0x90 facing, axisZ = root matrix Z row +0x20,
   centre = +0x100, aim = +0x110; bone rows = three.js matrix columns). */
const MAGI_PARAMS = ['shots', 'first_delay', 'interval', 'fwd_dist', 'fwd_yaw', 'off_x', 'off_y', 'off_z', 'launch_pitch',
  'launch_yaw', 'aim_dist', 'aim_yaw', 'aim_height', 'move_time', 'speed', 'speed_min', 'speed_max', 'accel', 'accel_start',
  'turn_rate', 'homing_start', 'homing_time', 'homing_stop', 'gravity', 'gravity_start', 'size', 'size_min', 'size_max',
  'size_rate', 'size_start', 'emit_count', 'emit_delay', 'emit_interval', 'tgt_dist_min', 'tgt_dist_max', 'tgt_height_min',
  'tgt_height_max', 'pull_start', 'pull_time', 'pull_strength', 'pull_full_dist', 'pull_min_dist', 'pull_max_dist',
  'lifetime', 'hit_time', 'hit_delay'];
const MF = { PIERCE: 0x1, NO_TERRAIN: 0x2, GROUND_HUG: 0x4, DEATH_EMIT: 0x20, BOUNCE: 0x800, HOMING_XZ: 0x2000,
  HOMING_Y: 0x4000, INHERIT_SIZE: 0x20000, STOP_ON_FAIL: 0x80000, PULL_FIELD: 0x400000, HOMING_STOP: 0x8000000 };
const MAGI_LOCK_H = 1.0;            // GUESS: height of an actor's +0x100 / +0x110 points above its root (see README §2.3)
const MAGI_FOE = { lo: 0.25, hi: 1.45, r: 0.3 };   // the dummy's hurt capsule (as dummy.js)
const MD2R = Math.PI / 180;

const mv = (x = 0, y = 0, z = 0) => ({ x, y, z });
const mAdd = (a, b) => mv(a.x + b.x, a.y + b.y, a.z + b.z), mSub = (a, b) => mv(a.x - b.x, a.y - b.y, a.z - b.z);
const mMul = (a, k) => mv(a.x * k, a.y * k, a.z * k), mDot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const mCross = (a, b) => mv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
const mLen = (a) => Math.sqrt(mDot(a, a)), mNorm = (a) => { const l = mLen(a); return l > 0 ? mMul(a, 1 / l) : mv(); };
const mRotY = (a, t) => { const c = Math.cos(t), s = Math.sin(t); return mv(a.x * c + a.z * s, a.y, -a.x * s + a.z * c); };
const mClamp = (x, lo, hi) => (x < lo ? lo : x <= hi ? x : hi);           // the game's clamp (lo wins if lo > hi)
const mFlags = (r) => (typeof r.flags === 'string' ? parseInt(r.flags, 16) : r.flags) >>> 0;
const mAt = (x, t) => (typeof x === 'function' ? x(t) : x);

/* the game's LCG (word 0x09AFB898): seed = seed*0x343FD + 0x269EC3, value = (seed >> 16 & 0x7FFF) / 32768 */
let magiSeed = 1;
function magiRand() { magiSeed = (Math.imul(magiSeed, 0x343FD) + 0x269EC3) | 0; return ((magiSeed >>> 16) & 0x7FFF) / 32768; }

/* FUN_08AD0694: the 46 parameters of shot k; a later shot adds `step` instead of redrawing */
function magiDraw(rec, k, prev) {
  const P = {};
  for (const n of MAGI_PARAMS) {
    const [lo, hi, st] = rec.values[n];
    P[n] = k === 0 || st === 0 ? lo + magiRand() * (hi - lo) : prev[n] + st;
  }
  return P;
}
const aPt = (A, which) => which === 256 ? (A.centre || mAdd(A.pos, mv(0, MAGI_LOCK_H, 0)))
  : which === 272 ? (A.aim || mAdd(A.pos, mv(0, MAGI_LOCK_H, 0))) : A.pos;

/* FUN_08AD70B0: the target point = target +0x110 + rotY(target facing * aim_dist, aim_yaw) + (0, aim_height, 0) */
function magiTargetActor(rec, ctx) {
  switch (rec.target) {
    case 0: return ctx.lock; case 1: return ctx.owner; case 2: return ctx.second;
    case 3: return ctx.lock || ctx.second; case 4: return ctx.opponent;
    default: return null;                  // 5: the code returns 0 -> never aims / homes
  }
}
function magiAimPoint(rec, P, ctx) {
  const T = magiTargetActor(rec, ctx); if (!T) return null;
  const off = mRotY(mMul(T.fwd, P.aim_dist), P.aim_yaw * MD2R);
  return mAdd(mAdd(aPt(T, 272), off), mv(0, P.aim_height, 0));
}

/* FUN_08AD5D70 origin cases (jump table 0x08AD7668): {pos, F, up} or null when the shot can't spawn */
function magiOrigin(rec, P, ctx) {
  const o = ctx.owner, up = mv(0, 1, 0);
  const clampTo = (pt) => {                                               // FUN_08AD3A2C
    const h = mv(pt.x - o.pos.x, 0, pt.z - o.pos.z), dy = pt.y - o.pos.y;
    const hd = mClamp(mLen(h), P.tgt_dist_min, P.tgt_dist_max), vd = mClamp(Math.abs(dy), P.tgt_height_min, P.tgt_height_max);
    return mAdd(mAdd(o.pos, mMul(mNorm(h), hd)), mv(0, Math.sign(dy) * vd, 0));
  };
  const onActor = (A, which) => A ? { pos: clampTo(aPt(A, which)), F: A.fwd, up }
    : { pos: mAdd(o.pos, mMul(o.fwd, P.tgt_dist_min)), F: o.fwd, up };
  switch (rec.origin) {
    case 0: return { pos: o.pos, F: o.fwd, up };
    case 1: case 2: {
      const B = o.bone && o.bone(rec.origin_arg);
      if (!B) return { pos: o.pos, F: o.fwd, up };
      return { pos: B.T, F: mNorm(B.Z), up: rec.origin === 2 ? mNorm(B.Y) : up };
    }
    case 3: case 4: return onActor(rec.target === 2 ? ctx.second : ctx.lock, rec.origin === 3 ? 128 : 256);
    case 5: return ctx.parent ? { pos: ctx.parent.pos, F: ctx.parent.dir, up } : { pos: o.pos, F: o.fwd, up };
    case 6: {                                                              // FUN_08AD3C24: the playing FEPA effect's matrix
      const M = ctx.effect && ctx.effect(ctx.arg2 || rec.origin_arg);
      return M ? { pos: M.T, F: M.Z, up } : null;                         // effect not playing -> no shot
    }
    case 7: return { pos: mv(), F: mv(0, 0, 1), up };
    case 8: case 9: return onActor(ctx.second, rec.origin === 8 ? 128 : 256);
    case 10: return ctx.summon ? { pos: ctx.summon.pos, F: ctx.summon.fwd, up } : null;
    case 11: case 12: return onActor(ctx.lock || ctx.second, rec.origin === 11 ? 128 : 256);
    case 13: return ctx.opponent ? { pos: aPt(ctx.opponent, 256), F: ctx.opponent.fwd, up } : { pos: o.pos, F: o.fwd, up };
  }
  return null;
}

/* FUN_08AD5D70 tail + FUN_08AD46F0: one bullet */
function magiBirth(rec, P, ctx, tick) {
  const fr = magiOrigin(rec, P, ctx); if (!fr) return null;
  const o = ctx.owner;
  let F = fr.F;
  if (rec.direction === 1) F = o.fwd;
  else if (rec.direction === 2) F = o.axisZ || o.fwd;
  else if (rec.direction === 3) F = ctx.parent ? ctx.parent.dir : o.fwd;           // top level: emitter +16 (GUESS owner facing)
  const Z = mNorm(F), X = mNorm(mCross(fr.up, Z)), Y = mCross(Z, X);
  const L = (q) => mAdd(mAdd(mMul(X, q.x), mMul(Y, q.y)), mMul(Z, q.z));
  const fy = P.fwd_yaw * MD2R;
  let fw = mv(Math.sin(fy) * P.fwd_dist, 0, Math.cos(fy) * P.fwd_dist);
  if (rec.orient === 0) fw = mv(-fw.y, fw.x, fw.z);                         // rotZ(90) (GUESS order)
  else if (rec.orient === 2) fw = mv(fw.x, -fw.z, fw.y);                    // rotX(90)
  const pos = mAdd(mAdd(fr.pos, L(mv(P.off_x, P.off_y, P.off_z))), L(fw));
  let dir;
  if (rec.direction === 4) {
    const A = magiAimPoint(rec, P, ctx);
    dir = A ? mNorm(mSub(A, pos)) : o.fwd;
  } else {
    const p = P.launch_pitch * MD2R, y = P.launch_yaw * MD2R, lp = mv(0, -Math.sin(p), Math.cos(p));   // +pitch = down
    dir = rec.origin === 2 ? L(mRotY(lp, y)) : mRotY(L(lp), y);
  }
  const f = mFlags(rec), parent = ctx.parent;
  return { rec, P, f, ctx, tick, state: 0, age: 0, pos, prev: pos, dir: mNorm(dir), speed: P.speed,
    size: (f & MF.INHERIT_SIZE) && parent ? parent.size : P.size, homing: true, stopped: false,
    emitCount: Math.trunc(P.emit_count), emitN: 0, emitT: P.emit_delay, emitIv: 0,
    hitMade: false, hitOn: false, hitC: null, reflected: false };
}

/* FUN_08AD1588: the emitter. owner / target: actor objects or functions of the tick.
   opts: { parent: {pos, dir, size}, effect(id) -> {X,Y,Z,T}, arg2, floorY } */
function spawnShots(rec, owner, target, opts = {}, fireTick = 0) {
  const out = [];
  let P = magiDraw(rec, 0, null), k = 0, timer = P.first_delay;
  const n = Math.trunc(P.shots);
  for (let u = 0; k < n && u < 4000; u++) {
    while (timer <= 0 && k < n) {
      const T = fireTick + u, tgt = mAt(target, T);
      const ctx = { owner: mAt(owner, T), lock: tgt, second: tgt, opponent: tgt, parent: opts.parent, effect: opts.effect,
        arg2: opts.arg2, summon: opts.summon, floorY: opts.floorY === undefined ? 0 : opts.floorY, ownerAt: owner, targetAt: target, opts };
      const s = magiBirth(rec, P, ctx, T);
      if (s) out.push(s); else if (mFlags(rec) & MF.STOP_ON_FAIL) { k = n; break; }
      P = magiDraw(rec, k + 1, P); timer = P.interval; k++;
    }
    timer -= 1;
  }
  return out;
}

/* Rodrigues turn of unit a toward unit b by at most maxAng (FUN_088690E8) */
function magiTurn(a, b, maxAng) {
  const d = mDot(a, b), ang = Math.min(d >= 1 ? 0 : d <= -1 ? Math.PI : Math.acos(d), maxAng);
  const k = mNorm(mCross(a, b)), c = Math.cos(ang), s = Math.sin(ang);
  return mAdd(mAdd(mMul(a, c), mMul(mCross(k, a), s)), mMul(k, mDot(k, a) * (1 - c)));
}

/* FUN_08AD48E8 */
function magiMotion(s, ev) {
  const r = s.rec, P = s.P, f = s.f, ctx = s.ctx;
  if (r.motion === 1 && s.homing && P.homing_start <= s.age && (P.homing_time === 0 || s.age <= P.homing_start + P.homing_time)) {
    const A = magiAimPoint(r, P, ctx);
    if (A) {
      const d = mSub(A, s.pos), dl = mLen(d);
      if (dl < P.homing_stop) { s.homing = false; if (f & MF.HOMING_STOP) s.stopped = true; }
      else {
        const t = magiTurn(s.dir, mNorm(d), P.turn_rate * MD2R);
        s.dir = f & MF.HOMING_XZ ? mNorm(mv(t.x, s.dir.y, t.z)) : f & MF.HOMING_Y ? mNorm(mv(s.dir.x, t.y, s.dir.z)) : t;
      }
    }
  }
  if (r.motion === 3 && (r.origin === 1 || r.origin === 2)) {           // glued: direction = the bone's Z row
    const B = ctx.owner.bone && ctx.owner.bone(r.origin_arg); if (B) s.dir = mNorm(B.Z);
  }
  if (s.age >= P.gravity_start && (s.speed !== 0 || P.gravity !== 0)) {   // v = dir*speed - (0, g/2, 0)
    const vel = mAdd(mMul(s.dir, s.speed), mv(0, -P.gravity * 0.5, 0));
    s.speed = mLen(vel); s.dir = mNorm(vel);
  }
  if (f & MF.GROUND_HUG) s.dir = mNorm(mv(s.dir.x, 0, s.dir.z));
  if (s.stopped) return;
  if (r.motion === 2) return;                                             // parked: no move, no accel, no terrain
  if (r.motion === 3) {                                                   // GUESS: re-place at the origin every tick
    const fr = magiOrigin(r, P, ctx); if (fr) s.pos = mAdd(fr.pos, s.glue || (s.glue = mSub(s.pos, fr.pos)));
  } else if (P.move_time <= 0 || s.age <= P.move_time) s.pos = mAdd(s.pos, mMul(s.dir, s.speed));
  if (s.age >= P.accel_start) s.speed += P.accel;
  s.speed = mClamp(s.speed, P.speed_min, P.speed_max);                    // speed_max 0 stops the shot
  if (!(f & MF.NO_TERRAIN) && r.kind !== 4 && ctx.floorY !== null && s.pos.y < ctx.floorY && s.prev.y >= ctx.floorY) {
    // terrain: a ray from the previous position (FUN_0889BEB0); the studio's only terrain is the floor (GUESS: kind 1)
    const link = r.link_terrain_1 >= 0 ? r.link_terrain_1 : r.link_terrain;
    const u = (s.prev.y - ctx.floorY) / (s.prev.y - s.pos.y), at = mAdd(s.prev, mMul(mSub(s.pos, s.prev), u));
    if (link >= 0) ev.push({ fire: link, parent: { pos: at, dir: s.dir, size: s.size } });
    if (f & MF.BOUNCE) { s.dir = mv(s.dir.x, -s.dir.y, s.dir.z); s.speed *= r.bounce_speed; s.pos = mv(s.pos.x, 2 * ctx.floorY - s.pos.y, s.pos.z); }
    else { s.pos = at; s.state = 2; }
  }
  if (f & MF.GROUND_HUG && ctx.floorY !== null) s.pos = mv(s.pos.x, ctx.floorY, s.pos.z);
}

/* FUN_08AD37E8 size, FUN_08AD384C in-flight children */
function magiSizeAndEmits(s, ev) {
  const P = s.P, r = s.rec;
  if (s.age >= P.size_start) s.size += P.size_rate;
  s.size = mClamp(s.size, P.size_min, P.size_max);
  if (r.link_emit < 0) return;
  const [lo, hi, st] = r.values.emit_interval;
  while (s.emitN < s.emitCount && s.emitT <= 0) {
    ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.emitIv = s.emitN === 0 || st === 0 ? lo + magiRand() * (hi - lo) : s.emitIv + st;
    s.emitT = s.emitIv; s.emitN++;
  }
  s.emitT -= 1;
}

/* FUN_08AD12B0 / 0880B3FC / 0880B494: the hit shape (null when the record has no atkp) */
function magiHitShape(s) {
  const r = s.rec, atkp = s.reflected && r.atkp_reflected !== 0xFFFF ? r.atkp_reflected : r.atkp;
  if (atkp === 0xFFFF || atkp < 0) return null;
  const R = r.hit_radius * s.size;
  if (!r.hit_length) return { type: 'sphere', c: s.pos, c0: s.prev, r: R, atkp };
  const h = r.hit_length * s.size * 0.5;
  return { type: 'upright', c: mAdd(s.pos, mv(0, h, 0)), c0: mAdd(s.prev, mv(0, h, 0)), r: R, halfH: h, atkp };
}

/* one tick of a bullet: birth (FUN_08AD2FA8) / flight (FUN_08AD30CC) / death (FUN_08AD3234), then the hit (FUN_08ADAD54).
   Returns { pos, vel, dir, speed, size, hit, alive, events } ; events = [{fire: id, parent}] for spawnShots(byId[id], ...) */
function stepShot(s, target, tick) {
  const ev = [], r = s.rec, P = s.P;
  if (tick !== undefined) { const T = mAt(s.ctx.targetAt, tick); s.ctx.owner = mAt(s.ctx.ownerAt, tick); s.ctx.lock = s.ctx.second = s.ctx.opponent = T; }
  else if (target) s.ctx.lock = s.ctx.second = s.ctx.opponent = target;
  s.prev = s.pos;
  if (s.state === 0) {
    if (r.link_emit >= 0 && !(s.f & MF.DEATH_EMIT) && !(P.emit_count > 0)) ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.state = 1;
  } else if (s.state === 1) {
    magiMotion(s, ev);
    magiSizeAndEmits(s, ev);
    if (s.age >= P.lifetime) s.state = 2;
    s.age += 1;
  } else if (s.state === 2) {
    if (r.link_emit >= 0 && s.f & MF.DEATH_EMIT) ev.push({ fire: r.link_emit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
    s.state = 3;
  }
  let hit = null;
  if (s.state <= 2) {
    if (!s.hitMade && s.age >= P.hit_delay) s.hitMade = s.hitOn = true;
    if (s.hitOn && s.age >= P.hit_delay + P.hit_time) s.hitOn = false;
    if (s.hitOn) { hit = magiHitShape(s); if (hit) hit.fresh = !s.touched; }
  }
  return { pos: s.pos, vel: mMul(s.dir, s.speed), dir: s.dir, speed: s.speed, size: s.size, hit, alive: s.state <= 2, events: ev };
}

/* the caller reports a contact: link_hit child, and the bullet dies unless PIERCE */
function shotContact(s) {
  const ev = []; s.touched = true;                                        // GUESS: one hit per foe per hit object
  if (s.rec.link_hit >= 0) ev.push({ fire: s.rec.link_hit, parent: { pos: s.pos, dir: s.dir, size: s.size } });
  if (!(s.f & MF.PIERCE)) { s.state = 3; s.hitOn = false; }
  return ev;
}

/* contact with the dummy: closest distance between the shot's segment (swept centre, or the upright's axis) and the
   foe's capsule (pos + 0.25..1.45 m, r 0.3) */
function segDist(p1, q1, p2, q2) {
  const d1 = mSub(q1, p1), d2 = mSub(q2, p2), r = mSub(p1, p2), a = mDot(d1, d1), e = mDot(d2, d2), f = mDot(d2, r);
  let s = 0, t = 0;
  if (a < 1e-12 && e < 1e-12) return mLen(r);
  if (a < 1e-12) t = mClamp(f / e, 0, 1);
  else {
    const c = mDot(d1, r);
    if (e < 1e-12) s = mClamp(-c / a, 0, 1);
    else { const b = mDot(d1, d2), den = a * e - b * b; s = den > 1e-12 ? mClamp((b * f - c * e) / den, 0, 1) : 0; t = (b * s + f) / e;
      if (t < 0) { t = 0; s = mClamp(-c / a, 0, 1); } else if (t > 1) { t = 1; s = mClamp((b - c) / a, 0, 1); } }
  }
  return mLen(mSub(mAdd(p1, mMul(d1, s)), mAdd(p2, mMul(d2, t))));
}
function magiTouches(hit, foePos) {
  const a = mAdd(foePos, mv(0, MAGI_FOE.lo, 0)), b = mAdd(foePos, mv(0, MAGI_FOE.hi, 0));
  const [p, q] = hit.type === 'sphere' ? [hit.c0, hit.c] : [mSub(hit.c, mv(0, hit.halfH, 0)), mAdd(hit.c, mv(0, hit.halfH, 0))];
  return segDist(p, q, a, b) <= hit.r + MAGI_FOE.r;
}

/* FUN_08AD2CE0: the pull on a victim this tick (displacement toward the bullet, or null) */
function magiPull(s, victim) {
  const P = s.P; if (!(s.f & MF.PULL_FIELD) || s.state > 2) return null;
  const on = P.pull_start === 0 && P.pull_time === 0 ? true : P.pull_start <= s.age && s.age <= P.pull_start + P.pull_time;
  if (!on) return null;
  const d = mSub(s.pos, aPt(victim, 256)), dl = mLen(d);
  if (dl < P.pull_min_dist || dl > P.pull_max_dist) return null;
  const span = Math.max(0, P.pull_full_dist - P.pull_min_dist), k = span === 0 ? 1 : Math.min(1, (dl - P.pull_min_dist) / span);
  return mMul(mNorm(d), P.pull_strength * k);
}
return { spawnShots, stepShot, shotContact, magiTouches, magiPull, magiHitShape, segDist, MF, setSeed: (x) => { magiSeed = x | 0; } };

})();
