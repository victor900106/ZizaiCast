// Ground truth for the OCR tests: every rendered text line with its box,
// normalised to the 390x844 page, written into <pre id="gt"> (one line per
// text line: x0 y0 x1 y1 TAB text).  render.sh reads it with --dump-dom.
window.addEventListener('load', () => {
  const W = 390, H = 844, rows = [];
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  for (let n; (n = walker.nextNode());) {
    const t = n.textContent;
    if (!t.trim()) continue;
    const st = getComputedStyle(n.parentElement);
    if (st.visibility === 'hidden' || st.display === 'none') continue;
    // Split the node into rendered lines (characters grouped by their top).
    let cur = null;
    for (let i = 0; i < t.length; ++i) {
      const r = document.createRange();
      r.setStart(n, i);
      r.setEnd(n, i + 1);
      const b = r.getBoundingClientRect();
      if (b.width === 0 && b.height === 0) continue;
      if (!cur || Math.abs(b.top - cur.top) > b.height * 0.5) {
        if (cur) rows.push(cur);
        cur = { top: b.top, x0: b.left, y0: b.top, x1: b.right, y1: b.bottom, text: '' };
      }
      cur.x0 = Math.min(cur.x0, b.left); cur.x1 = Math.max(cur.x1, b.right);
      cur.y0 = Math.min(cur.y0, b.top); cur.y1 = Math.max(cur.y1, b.bottom);
      cur.text += t[i];
    }
    if (cur) rows.push(cur);
  }
  const out = rows.filter(r => r.text.trim()).map(r =>
    [r.x0 / W, r.y0 / H, r.x1 / W, r.y1 / H].map(v => v.toFixed(4)).join(' ') + '\t' + r.text.trim());
  const pre = document.createElement('pre');
  pre.id = 'gt';
  pre.style.display = 'none';
  pre.textContent = out.join('\n');
  document.body.appendChild(pre);
});
