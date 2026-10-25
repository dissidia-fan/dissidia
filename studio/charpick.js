/* Move Studio: the character picker as a searchable list (+ New character… at the top, then mod characters, then the
   ISO's). The native <select id="charPick"> stays underneath as the source of truth: app.js fills it and sets its
   value, and picking here sets it and fires its change. Loaded after newchar.js. */
(function charPicker() {
  const sel = $('charPick'); if (!sel) return;
  const btn = document.createElement('button');
  btn.type = 'button'; btn.id = 'charPickBtn'; btn.className = 'cpk-btn'; btn.setAttribute('aria-haspopup', 'listbox');
  btn.title = 'Character (type to search)';
  sel.after(btn); sel.hidden = true;
  let pop = null, away = null;

  const items = () => {                             // [{value, label, note, cat}] from the select's options
    const out = [];
    for (const o of sel.options) {
      if (o.value === 'new:') continue;
      const g = o.parentElement && o.parentElement.tagName === 'OPTGROUP' ? o.parentElement.label : 'Characters';
      let note = '';
      if (o.value.startsWith('iso:')) note = o.value.slice(4);
      else { const c = (S.chars || []).find((x) => x.name === o.value); note = c ? [c.code, c.kind === 'retail' ? 'changes a retail character' : ''].filter(Boolean).join(' · ') : ''; }
      out.push({ value: o.value, label: o.textContent, note, cat: g });
    }
    return out;
  };
  const hasNew = () => [...sel.options].some((o) => o.value === 'new:');
  function sync() {
    const o = sel.options[sel.selectedIndex];
    btn.innerHTML = `<span>${esc(o ? o.textContent : 'Character')}</span><i class="ic ic-chevron-down" aria-hidden="true"></i>`;
  }
  // every place that sets sel.value (app.js, newchar.js) updates the button too
  const desc = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value');
  Object.defineProperty(sel, 'value', { get() { return desc.get.call(this); }, set(v) { desc.set.call(this, v); sync(); }, configurable: true });
  new MutationObserver(sync).observe(sel, { childList: true, subtree: true });

  function close() { if (pop) { pop.remove(); pop = null; } if (away) { document.removeEventListener('pointerdown', away); away = null; } btn.setAttribute('aria-expanded', 'false'); }
  function pick(v) { close(); if (v === sel.value) return; desc.set.call(sel, v); sync(); sel.dispatchEvent(new Event('change')); }
  function open() {
    if (pop) { close(); return; }
    pop = document.createElement('div'); pop.className = 'menu cpk-pop'; pop.setAttribute('role', 'listbox');
    pop.innerHTML = `${hasNew() ? '<button type="button" class="cpk-new" data-v="new:">+ New character…<small>A new roster slot, built on a retail character</small></button>' : ''}
      <input type="search" placeholder="Search characters" aria-label="Search characters"><div class="list"></div>`;
    document.body.appendChild(pop);
    menuPlace(pop, btn, 360);
    const list = pop.querySelector('.list'), inp = pop.querySelector('input'), all = items();
    let shown = [], at = 0;
    const draw = () => {
      const f = inp.value.trim().toLowerCase();
      shown = all.filter((it) => !f || (it.label + ' ' + it.note).toLowerCase().includes(f));
      let h = '', cat = null;
      shown.forEach((it, i) => {
        if (it.cat !== cat) { cat = it.cat; h += `<div class="cat">${esc(cat)}</div>`; }
        h += `<button type="button" role="option" data-i="${i}" aria-selected="${it.value === sel.value}" class="${i === at ? 'cpk-at' : ''}"><span class="cpk-tick">${it.value === sel.value ? '✓' : ''}</span>${esc(it.label)}${it.note ? `<small>${esc(it.note)}</small>` : ''}</button>`;
      });
      list.innerHTML = h || '<p class="note" style="padding:8px">No character matches.</p>';
      list.querySelectorAll('button[data-i]').forEach((b) => { b.onclick = () => pick(shown[+b.dataset.i].value); b.onmousemove = () => { if (at !== +b.dataset.i) { at = +b.dataset.i; mark(); } }; });
    };
    const mark = () => { list.querySelectorAll('button[data-i]').forEach((b) => b.classList.toggle('cpk-at', +b.dataset.i === at)); const c = list.querySelector('.cpk-at'); if (c) c.scrollIntoView({ block: 'nearest' }); };
    inp.oninput = () => { at = 0; draw(); };
    inp.onkeydown = (e) => {
      if (e.key === 'ArrowDown') { e.preventDefault(); at = Math.min(shown.length - 1, at + 1); mark(); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); at = Math.max(0, at - 1); mark(); }
      else if (e.key === 'Enter') { e.preventDefault(); if (shown[at]) pick(shown[at].value); }
      else if (e.key === 'Escape') { e.preventDefault(); close(); btn.focus(); }
    };
    const nb = pop.querySelector('.cpk-new'); if (nb) nb.onclick = () => pick('new:');
    at = Math.max(0, all.findIndex((it) => it.value === sel.value));
    draw(); mark(); inp.focus();
    btn.setAttribute('aria-expanded', 'true');
    setTimeout(() => document.addEventListener('pointerdown', away = (e) => { if (!pop.contains(e.target) && e.target !== btn && !btn.contains(e.target)) close(); }), 0);
  }
  btn.onclick = open;
  btn.onkeydown = (e) => { if (e.key === 'ArrowDown' || (e.key.length === 1 && /\S/.test(e.key))) { e.preventDefault(); open(); if (e.key.length === 1) { const i = pop && pop.querySelector('input'); if (i) { i.value = e.key; i.dispatchEvent(new Event('input')); } } } };
  sync();
})();

/* the same list as a drop-in for any <select>: fancySelect(sel) puts a button in front of it (the select stays the source
   of truth, hidden) and opens a popup list; an option's data-note shows under its text. Search when there are many. */
function fancySelect(sel, o = {}) {
  if (!sel || sel._fancy) return sel && sel._fancy;
  const btn = document.createElement('button');
  btn.type = 'button'; btn.className = ['cpk-btn fsel-btn', o.className || '', sel.className || ''].join(' ').trim(); btn.setAttribute('aria-haspopup', 'listbox');
  if (sel.title) btn.title = sel.title;
  if (sel.id) btn.id = sel.id + 'Btn';
  sel.after(btn); sel.hidden = true; sel._fancy = btn;
  let pop = null, away = null;
  const opts = () => [...sel.options].map((x) => ({ value: x.value, label: x.textContent, note: x.dataset.note || '', cat: x.parentElement && x.parentElement.tagName === 'OPTGROUP' ? x.parentElement.label : '', off: x.disabled }));
  const sync = () => { const x = sel.options[sel.selectedIndex]; btn.innerHTML = `<span>${esc(x ? x.textContent : '')}${x && x.dataset.note && o.noteInButton ? ` <small>${esc(x.dataset.note)}</small>` : ''}</span><i class="ic ic-chevron-down" aria-hidden="true"></i>`; btn.disabled = sel.disabled; };
  const desc = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value');
  Object.defineProperty(sel, 'value', { get() { return desc.get.call(this); }, set(v) { desc.set.call(this, v); sync(); }, configurable: true });
  new MutationObserver(sync).observe(sel, { childList: true, subtree: true, attributes: true });
  const close = () => { if (pop) { pop.remove(); pop = null; } if (away) { document.removeEventListener('pointerdown', away); away = null; } btn.setAttribute('aria-expanded', 'false'); };
  const pick = (v) => { close(); if (v === sel.value) return; desc.set.call(sel, v); sync(); sel.dispatchEvent(new Event('input', { bubbles: true })); sel.dispatchEvent(new Event('change', { bubbles: true })); btn.focus(); };
  btn.onclick = () => {
    if (pop) { close(); return; }
    const all = opts(), search = o.search ?? all.length > 10;
    pop = document.createElement('div'); pop.className = 'menu cpk-pop fsel-pop'; pop.setAttribute('role', 'listbox');
    pop.innerHTML = `${search ? '<input type="search" placeholder="Search" aria-label="Search">' : ''}<div class="list"></div>`;
    document.body.appendChild(pop); menuPlace(pop, btn, Math.max(btn.offsetWidth, o.width || 240));
    const list = pop.querySelector('.list'), inp = pop.querySelector('input');
    let shown = [], at = Math.max(0, all.findIndex((x) => x.value === sel.value));
    const mark = () => { list.querySelectorAll('button[data-i]').forEach((b) => b.classList.toggle('cpk-at', +b.dataset.i === at)); const c = list.querySelector('.cpk-at'); if (c) c.scrollIntoView({ block: 'nearest' }); };
    const draw = () => {
      const f = inp ? inp.value.trim().toLowerCase() : '';
      shown = all.filter((x) => !f || (x.label + ' ' + x.note).toLowerCase().includes(f));
      let h = '', cat = null;
      shown.forEach((x, i) => {
        if (x.cat && x.cat !== cat) { cat = x.cat; h += `<div class="cat">${esc(cat)}</div>`; }
        h += `<button type="button" role="option" data-i="${i}" aria-selected="${x.value === sel.value}"${x.off ? ' disabled' : ''}><span class="cpk-tick">${x.value === sel.value ? '✓' : ''}</span>${esc(x.label)}${x.note ? `<small>${esc(x.note)}</small>` : ''}</button>`;
      });
      list.innerHTML = h || '<p class="note" style="padding:8px">Nothing matches.</p>';
      list.querySelectorAll('button[data-i]').forEach((b) => { b.onclick = () => pick(shown[+b.dataset.i].value); b.onmousemove = () => { if (at !== +b.dataset.i) { at = +b.dataset.i; mark(); } }; });
    };
    const key = (e) => {
      if (e.key === 'ArrowDown') { e.preventDefault(); at = Math.min(shown.length - 1, at + 1); mark(); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); at = Math.max(0, at - 1); mark(); }
      else if (e.key === 'Enter') { e.preventDefault(); if (shown[at]) pick(shown[at].value); }
      else if (e.key === 'Escape') { e.preventDefault(); close(); btn.focus(); }
    };
    if (inp) { inp.oninput = () => { at = 0; draw(); mark(); }; inp.onkeydown = key; } else pop.onkeydown = key;
    draw(); mark(); (inp || list.querySelector('button[aria-selected=true]') || list.querySelector('button') || pop).focus();
    btn.setAttribute('aria-expanded', 'true');
    setTimeout(() => document.addEventListener('pointerdown', away = (e) => { if (pop && !pop.contains(e.target) && !btn.contains(e.target)) close(); }), 0);
  };
  btn.onkeydown = (e) => { if (e.key === 'ArrowDown') { e.preventDefault(); btn.click(); } };
  sync();
  return btn;
}

/* every <select> in the Studio uses that list, the ones made later too (the inspector re-renders often) */
(function autoFancy() {
  const skip = (s) => s.id === 'charPick' || s._fancy || s.multiple || s.size > 1;
  const run = (root) => root.querySelectorAll('select').forEach((s) => { if (!skip(s)) fancySelect(s); });
  run(document);
  new MutationObserver((ms) => { for (const m of ms) for (const n of m.addedNodes) if (n.nodeType === 1) { if (n.tagName === 'SELECT') { if (!skip(n)) fancySelect(n); } else if (n.querySelector && n.querySelector('select')) run(n); } })
    .observe(document.body, { childList: true, subtree: true });
})();
