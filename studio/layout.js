/* Move Studio: resizable, collapsible panes. Drag a divider to resize; drag it to the edge and the pane folds into a thin
   strip (click the strip, or drag the divider back out, to bring it back). Focused, a divider also takes the arrow
   keys; double-click resets it. The layout is remembered per browser. Loaded last. */
(function paneLayout() {
  const work = document.querySelector('.work'), center = document.querySelector('.center');
  if (!work || !center) return;
  const [left, insp] = work.querySelectorAll(':scope > .pane');
  const vp = $('viewport'), tl = center.querySelector('.timeline');
  const DEF = { lw: 340, rw: 330, vh: null, left: false, insp: false, vp: false, tl: false };
  const FOLD_W = 120, FOLD_H = 90;                    // drag a pane smaller than this and it folds
  let L = { ...DEF };
  try { Object.assign(L, JSON.parse(localStorage.getItem('studio.layout') || '{}')); } catch (e) { /* defaults */ }
  const save = () => { try { localStorage.setItem('studio.layout', JSON.stringify(L)); } catch (e) { /* per-viewer only */ } };

  const mkSplit = (dir, label) => {
    const s = document.createElement('div');
    s.className = 'split split-' + dir; s.tabIndex = 0;
    s.setAttribute('role', 'separator'); s.setAttribute('aria-orientation', dir === 'v' ? 'vertical' : 'horizontal');
    s.setAttribute('aria-label', label); s.title = label + ': drag to resize, to the edge to fold; double-click resets';
    return s;
  };
  const sL = mkSplit('v', 'The left pane'), sR = mkSplit('v', 'The inspector'), sC = mkSplit('h', 'The 3D view and the timeline');
  left.after(sL); insp.before(sR); vp.after(sC);

  const strip = (pane, key, label, side) => {         // what a folded pane shows: click it to open the pane again
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'fold-strip fold-strip-' + side; b.textContent = label; b.title = 'Show ' + label.toLowerCase();
    b.addEventListener('click', () => { L[key] = false; apply(); save(); });
    pane.appendChild(b);
  };
  strip(left, 'left', 'Moves', 'l'); strip(insp, 'insp', 'Inspector', 'r'); strip(vp, 'vp', '3D view', 't');
  tl.querySelector('.tl-bar').addEventListener('click', (e) => {     // a folded timeline keeps its bar: click it to open
    if (L.tl && !e.target.closest('button,input,label,select')) { L.tl = false; apply(); save(); }
  });

  function apply() {
    const W = work.clientWidth, H = center.clientHeight;
    L.lw = Math.max(200, Math.min(L.lw, W * 0.45)); L.rw = Math.max(220, Math.min(L.rw, W * 0.45));
    work.style.gridTemplateColumns = `${L.left ? '30px' : L.lw + 'px'} 10px minmax(0,1fr) 10px ${L.insp ? '30px' : L.rw + 'px'}`;
    let rows;
    if (L.vp && L.tl) rows = '30px 10px minmax(0,1fr)';
    else if (L.vp) rows = '30px 10px minmax(0,1fr)';
    else if (L.tl) rows = 'minmax(0,1fr) 10px auto';
    else rows = L.vh ? `${Math.max(140, Math.min(L.vh, H - 150))}px 10px minmax(150px,1fr)` : 'minmax(220px,1fr) 10px minmax(200px,1fr)';
    center.style.gridTemplateRows = rows;
    left.classList.toggle('folded', L.left); insp.classList.toggle('folded', L.insp);
    vp.classList.toggle('folded', L.vp); tl.classList.toggle('folded', L.tl && !L.vp);
    tl.title = L.tl && !L.vp ? 'Click the bar to show the timeline' : '';
  }

  const drag = (s, onMove) => {
    s.addEventListener('pointerdown', (e) => {
      e.preventDefault(); s.setPointerCapture(e.pointerId); s.classList.add('on');
      document.body.classList.add('resizing', s.classList.contains('split-h') ? 'rz-h' : 'rz-v');
      const mv = (ev) => { onMove(ev); apply(); };
      const up = () => { s.classList.remove('on'); document.body.classList.remove('resizing', 'rz-h', 'rz-v'); s.removeEventListener('pointermove', mv); s.removeEventListener('pointerup', up); s.removeEventListener('pointercancel', up); save(); };
      s.addEventListener('pointermove', mv); s.addEventListener('pointerup', up); s.addEventListener('pointercancel', up);
    });
  };
  drag(sL, (e) => { const w = e.clientX - work.getBoundingClientRect().left; L.left = w < FOLD_W; if (!L.left) L.lw = w; });
  drag(sR, (e) => { const w = work.getBoundingClientRect().right - e.clientX; L.insp = w < FOLD_W; if (!L.insp) L.rw = w; });
  drag(sC, (e) => {
    const r = center.getBoundingClientRect(), y = e.clientY - r.top;
    L.vp = y < FOLD_H; L.tl = r.height - y < FOLD_H;
    if (!L.vp && !L.tl) L.vh = y;
  });
  const key = (s, f) => s.addEventListener('keydown', (e) => {
    const d = { ArrowLeft: -20, ArrowRight: 20, ArrowUp: -20, ArrowDown: 20 }[e.key];
    if (d == null) return;
    e.preventDefault(); f(d); apply(); save();
  });
  key(sL, (d) => { if (L.left) { if (d > 0) L.left = false; } else if (L.lw + d < 200) L.left = true; else L.lw += d; });
  key(sR, (d) => { if (L.insp) { if (d < 0) L.insp = false; } else if (L.rw - d < 220) L.insp = true; else L.rw -= d; });
  key(sC, (d) => { L.vp = L.tl = false; L.vh = (L.vh || vp.getBoundingClientRect().height) + d; });
  sL.addEventListener('dblclick', () => { L.left = false; L.lw = DEF.lw; apply(); save(); });
  sR.addEventListener('dblclick', () => { L.insp = false; L.rw = DEF.rw; apply(); save(); });
  sC.addEventListener('dblclick', () => { L.vp = L.tl = false; L.vh = null; apply(); save(); });
  new ResizeObserver(apply).observe(work);
  apply();
})();
