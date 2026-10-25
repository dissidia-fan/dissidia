/* Move Studio: the EX Burst as a move, and the game camera. Loaded after app.js and moves.js.

   The EX Burst is nine scripts the game starts from code (no command or action points at them; burst_re.md):
     8000 lead-in (its first hit triggers the burst), 8101 weapons appear, 8102 draw / wait for input,
     8103–8106 one strike per prompt, 8500 finisher, 8600 exit.
   The prompts, how many land and the brave → HP payout are game code; the studio plays every prompt as hit.
   The game camera: burst motions (and some HP moves) animate the `camera_include` bone, which the game uses as the
   camera: its position is the eye, it looks down its −Z with +Y up, and its Z scale is the field of view in degrees. */

const BURST_SCRIPTS = [[8000, 'lead-in'], [8101, 'weapons appear'], [8102, 'draw'], [8103, 'prompt 1'], [8104, 'prompt 2'], [8105, 'prompt 3'], [8106, 'prompt 4'], [8500, 'finisher'], [8600, 'exit']];
function burstMove() {                        // a move-like entry for the burst (derive() appends it to S.moves)
  const mm = (F(msPath('motion_map.json')) || {}).rows || [];
  const steps = [];
  for (const [script, role] of BURST_SCRIPTS) {
    const row = mm.find((r) => r.script === script);
    if (row) steps.push({ i: steps.length, action: null, flags: 0, script, motion: row.motion, role });
  }
  if (steps.length < 3) return null;
  return { id: -1, hexId: 'EX', name: 'EX Burst', info: null, burst: true, disabled: false, steps };
}
function burstHTML(m) {                       // the move panel for the burst
  return `<div class="sec" id="mvSec"><h3>EX Burst <small>${m.steps.length} scripts</small></h3>
    <p class="note">Nine scripts the game starts from code: ${m.steps.map((s) => `${s.script} ${s.role}`).join(', ')}. The burst only starts if the lead-in's first hit lands; the prompts, how many succeed and the brave → HP payout are game code, so this plays every prompt as a success. Turn on <b>Game camera</b> to see it as the game frames it.</p></div>`;
}

/* ====================================================== the game camera (camera_include) */
const GCAM = { on: false, keep: null };
function gcamBone() { const i = S.boneIndex && S.boneIndex.get('camera_include'); return i == null ? -1 : i; }
function gcamAnimated(mi) {                    // does this motion move the camera bone?
  const bi = gcamBone(), m = S.gmo && S.gmo.motions[mi];
  return bi >= 0 && !!m && m.binds.some((b) => b.bone === bi);
}
// the option only shows where the camera bone really is a camera: its Z scale carries a field of view at some point
const GCAM_OK = new Map();
function gcamUsable(mi) {
  if (!S.gmo || !gcamAnimated(mi)) return false;
  if (GCAM_OK.gmo !== S.gmo) { GCAM_OK.clear(); GCAM_OK.gmo = S.gmo; }
  if (!GCAM_OK.has(mi)) {
    const bi = gcamBone(), n = motionLen(mi), s = new THREE.Vector3(), p = new THREE.Vector3(), q = new THREE.Quaternion();
    let ok = false;
    for (let k = 0; k <= 8 && !ok; k++) {
      poseMotion(S.gmo, view, mi, n * k / 8); view.root.updateMatrixWorld(true);
      view.bones[bi].matrixWorld.decompose(p, q, s); ok = Math.abs(s.z) > 1 && Math.abs(s.z) < 170;
    }
    poseMotion(S.gmo, view, S.motion, S.frame); view.root.updateMatrixWorld(true);
    GCAM_OK.set(mi, ok);
  }
  return GCAM_OK.get(mi);
}
function gcamRefresh() {                       // after every pose
  const tag = $('gcamTag');
  const can = S.gmo && !S.fxEdit && gcamUsable(S.motion);
  if (tag) tag.hidden = !can;
  if (!GCAM.on || !can) { gcamRelease(); return; }
  const bi = gcamBone(), b = view.bones[bi]; if (!b) return;
  view.root.updateMatrixWorld(true);
  const p = new THREE.Vector3(), q = new THREE.Quaternion(), s = new THREE.Vector3();
  b.matrixWorld.decompose(p, q, s);
  // a real camera track carries the field of view in its Z scale; where it doesn't (the bone only rides along with
  // the body in this animation, or sits at rest between shots), keep the studio's own camera
  const info0 = $('gcamInfo');
  if (!(Math.abs(s.z) > 1 && Math.abs(s.z) < 170)) { gcamRelease(); if (info0) info0.textContent = ''; return; }   // between shots
  if (!GCAM.keep) GCAM.keep = { pos: view.camera.position.clone(), q: view.camera.quaternion.clone(), fov: view.camera.fov, target: view.controls.target.clone() };
  view.controls.enabled = false;
  view.camera.position.copy(p); view.camera.quaternion.copy(q);
  const fov = Math.abs(s.z);
  if (view.camera.fov !== fov) { view.camera.fov = fov; view.camera.updateProjectionMatrix(); }
  const info = $('gcamInfo'); if (info) info.textContent = `${fov.toFixed(1)}°`;
}
function gcamRelease() {
  if (!GCAM.keep) return;
  view.camera.position.copy(GCAM.keep.pos); view.camera.quaternion.copy(GCAM.keep.q); view.camera.fov = GCAM.keep.fov; view.camera.updateProjectionMatrix();
  view.controls.target.copy(GCAM.keep.target); view.controls.enabled = true; view.controls.update();
  GCAM.keep = null;
}
(function gcamWire() {
  const cb = $('gcamOn'); if (!cb) return;
  cb.onchange = () => { GCAM.on = cb.checked; if (!GCAM.on) gcamRelease(); pose(); };
})();
