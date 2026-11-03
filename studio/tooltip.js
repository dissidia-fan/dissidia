/* Styled tooltips for every title= in the Studio (2026-11-01: "use styled tooltips instead of titles everywhere").
   The markup and the code keep setting title as before; while the pointer (or the keyboard focus) is on an element, its
   title is held back, so the browser's own tooltip never shows, and this one shows it instead. The title goes back when
   the pointer leaves, so code that reads it still sees it. */
(function tooltips() {
  const tip = document.createElement('div');
  tip.className = 'stip'; tip.setAttribute('role', 'tooltip'); tip.hidden = true;
  document.body.appendChild(tip);
  let cur = null, text = '', timer = 0, lastHide = 0;
  const take = (el) => { const t = el.getAttribute('title'); if (t != null) { el.removeAttribute('title'); el._stip = t; } return el._stip || ''; };
  const give = (el) => { if (el && el._stip != null && !el.hasAttribute('title')) el.setAttribute('title', el._stip); if (el) el._stip = null; };
  const place = () => {
    if (!cur || !cur.isConnected) return hide();
    const r = cur.getBoundingClientRect(), W = window.innerWidth, H = window.innerHeight;
    tip.style.left = '0px'; tip.style.top = '0px';
    const w = tip.offsetWidth, h = tip.offsetHeight, gap = 6;
    const list = cur.closest('.vp-menu,.menu');        // in a menu: beside it, so the tip never covers the other items
    if (list) {
      const m = list.getBoundingClientRect();
      let left = m.right + gap; if (left + w > W - 4) left = Math.max(4, m.left - gap - w);
      tip.style.left = left + 'px'; tip.style.top = Math.max(4, Math.min(H - h - 4, r.top + r.height / 2 - h / 2)) + 'px'; return;
    }
    let top = r.bottom + gap; if (top + h > H - 4 && r.top - gap - h >= 4) top = r.top - gap - h;
    const left = Math.max(4, Math.min(W - w - 4, r.left + r.width / 2 - w / 2));
    tip.style.left = left + 'px'; tip.style.top = Math.max(4, top) + 'px';
  };
  const show = () => { if (!cur || !text) return; tip.textContent = text; tip.hidden = false; place(); };
  function hide() { clearTimeout(timer); if (!tip.hidden) lastHide = Date.now(); tip.hidden = true; give(cur); cur = null; text = ''; }
  const enter = (el) => {
    if (el === cur) return;
    hide(); cur = el; text = take(el);
    if (!text) return;
    timer = setTimeout(show, Date.now() - lastHide < 400 ? 0 : 450);   // straight away when moving from one tip to the next
  };
  document.addEventListener('pointerover', (e) => {
    if (e.pointerType === 'touch') return;
    if (cur && cur.contains(e.target)) {                 // still inside; pick up a title the code changed meanwhile
      if (cur.hasAttribute('title')) { text = take(cur); if (!tip.hidden) { tip.textContent = text; place(); } }
      const inner = e.target.closest('[title]'); if (!inner || inner === cur || !cur.contains(inner)) return;
      return enter(inner);
    }
    const el = e.target.closest && e.target.closest('[title]');
    if (el) enter(el); else if (cur) hide();
  }, true);
  document.addEventListener('pointerout', (e) => { if (cur && !cur.contains(e.relatedTarget)) hide(); }, true);
  document.addEventListener('pointerdown', () => { if (cur) { clearTimeout(timer); tip.hidden = true; } }, true);
  document.addEventListener('focusin', (e) => {
    const el = e.target; if (!el.matches || !el.matches(':focus-visible')) return;
    const t = el.closest('[title]'); if (t) { enter(t); clearTimeout(timer); timer = setTimeout(show, 300); }
  });
  document.addEventListener('focusout', () => { if (cur) hide(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !tip.hidden) hide(); }, true);
  window.addEventListener('scroll', () => { if (!tip.hidden) hide(); }, true);
  window.addEventListener('blur', hide);
})();
