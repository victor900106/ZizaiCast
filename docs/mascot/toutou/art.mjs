// 投投 Toutou — three style directions, hand-authored vector construction.
// Every character is built in one local frame (0 0 300 240): cloud base sits on y=166, visual centre x=133.
// toutou(dir, { mood, accent, phone, beam, monitor, tilt }) -> SVG <g> string.

export const THEMES = {
  Sakura: { accent: '#F5A7A7', bg: '#2A1C1F', bg2: '#1D1315', text: '#F6E9E6', sub: '#C9A9A6' },
  Mint: { accent: '#8FE3C4', bg: '#123230', bg2: '#0B2220', text: '#E6F5EF', sub: '#9CC3B6' },
  Night: { accent: '#BBA9F7', bg: '#1B1D3D', bg2: '#12132B', text: '#ECEAF8', sub: '#A9A6CF' },
  MilkTea: { accent: '#E3B98A', bg: '#30231B', bg2: '#211710', text: '#F5EBDD', sub: '#C7AE92' },
};
export const MOODS = ['idle', 'happy', 'connecting', 'surprised', 'sleepy', 'asleep'];

let uid = 0;
const id = (p) => `${p}${++uid}`;
const f = (n) => +n.toFixed(2);

// ---------- colour utils ----------
const h2r = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
const r2h = (c) => '#' + c.map((v) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
export const mix = (a, b, t) => { const x = h2r(a), y = h2r(b); return r2h(x.map((v, i) => v + (y[i] - v) * t)); };

// ---------- shared construction ----------
// Cloud = 3 puffs (big–medium–small, asymmetric) + one rounded base. Pure primitives.
const CLOUD = {
  puffs: [[86, 112, 30], [132, 90, 40], [178, 114, 26]],
  base: { x: 54, y: 108, w: 158, h: 58, r: 29 },
};
// Geometric-premium cloud: strict 8 px grid. Pill 18u x 6u, puffs r = 4u / 6u / 3u.
const GEO = {
  puffs: [[116, 104, 48], [172, 120, 32]],
  base: { x: 56, y: 120, w: 152, h: 48, r: 24 },
};
const cloudShapes = (C, attrs = '') =>
  C.puffs.map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" ${attrs}/>`).join('') +
  `<rect x="${C.base.x}" y="${C.base.y}" width="${C.base.w}" height="${C.base.h}" rx="${C.base.r}" ${attrs}/>`;

const heart = (x, y, s, fill, rot = 0, extra = '') =>
  `<path transform="translate(${x},${y}) rotate(${rot}) scale(${s})" d="M0 3.2C-1.4 1.4-5 -.4-5 -3.1-5 -5.2-3.2 -6.4-1.6 -6.1-.8 -6 -.3 -5.4 0 -4.8.3 -5.4.8 -6 1.6 -6.1 3.2 -6.4 5 -5.2 5 -3.1 5 -.4 1.4 1.4 0 3.2Z" fill="${fill}" ${extra}/>`;
const sparkle = (x, y, s, fill, op = 1) =>
  `<path transform="translate(${x},${y}) scale(${s})" d="M0-6C.6-1.4 1.4-.6 6 0 1.4.6.6 1.4 0 6-.6 1.4-1.4.6-6 0-1.4-.6-.6-1.4 0-6Z" fill="${fill}" opacity="${op}"/>`;
const zzz = (x, y, s, fill, sw = 2.6) => {
  const z = (dx, dy, k) => `<path d="M${f(x + dx)} ${f(y + dy)}h${f(7 * k)}l${f(-7 * k)} ${f(7 * k)}h${f(7 * k)}" fill="none" stroke="${fill}" stroke-width="${f(sw * k)}" stroke-linecap="round" stroke-linejoin="round"/>`;
  return z(0, 10, s) + z(11 * s, 0, s * 0.8) + z(20 * s, -8, s * 0.62);
};

// ---------- face system (shared grammar, per-direction parameters) ----------
// P: { ink, ex:[lx,rx], ey, erx, ery, hl:[r1,r2], mouthY, cheekY, cheekDX, blush, bodyFill (for lids), lidW }
function eyes(mood, P) {
  const out = [];
  for (const [i, x] of P.ex.entries()) {
    const y = P.ey;
    const side = i === 0 ? -1 : 1;
    const open = (sx = 1, sy = 1, hlS = 1) => {
      const rx = P.erx * sx, ry = P.ery * sy;
      let g = `<ellipse cx="${x}" cy="${y}" rx="${f(rx)}" ry="${f(ry)}" fill="${P.ink}"/>`;
      // consistent light from upper-left of the viewer -> highlight up-left on both eyes
      g += `<circle cx="${f(x - rx * 0.28)}" cy="${f(y - ry * 0.38)}" r="${f(P.hl[0] * hlS)}" fill="#fff"/>`;
      if (P.hl[1]) g += `<circle cx="${f(x + rx * 0.38)}" cy="${f(y + ry * 0.42)}" r="${f(P.hl[1] * hlS)}" fill="#fff" opacity=".9"/>`;
      return g;
    };
    const lw = P.lineW;
    if (mood === 'idle') out.push(open());
    else if (mood === 'surprised') out.push(open(1.22, 1.22, 1.15));
    else if (mood === 'happy') {
      const w = P.erx * 1.25, h = P.ery * 0.95;
      out.push(`<path d="M${f(x - w)} ${f(y + h * 0.35)}Q${x} ${f(y - h * 1.15)} ${f(x + w)} ${f(y + h * 0.35)}" fill="none" stroke="${P.ink}" stroke-width="${lw}" stroke-linecap="round"/>`);
    } else if (mood === 'connecting') {
      // determined = eager focus: eyes a touch bigger, gaze up-right toward the beam, star-shaped glint
      const rx = P.erx * 1.1, ry = P.ery * 1.1, ex = x + 1.4, ey = y - 0.8;
      const k = P.hl[0] * 0.62;
      out.push(`<ellipse cx="${f(ex)}" cy="${f(ey)}" rx="${f(rx)}" ry="${f(ry)}" fill="${P.ink}"/>` +
        `<path transform="translate(${f(ex - rx * 0.12)},${f(ey - ry * 0.3)}) scale(${f(k * 0.8)})" d="M0-6C.7-1.6 1.6-.7 6 0 1.6.7.7 1.6 0 6-.7 1.6-1.6.7-6 0-1.6-.7-.7-1.6 0-6Z" fill="#fff"/>` +
        (P.hl[1] ? `<circle cx="${f(ex - rx * 0.4)}" cy="${f(ey + ry * 0.45)}" r="${f(P.hl[1])}" fill="#fff" opacity=".9"/>` : ''));
    } else if (mood === 'sleepy') {
      const cid = id('lid');
      const rx = P.erx, ry = P.ery;
      // heavy upper lid, drooping toward the OUTER corner (sleepy), never toward the nose (angry)
      const yIn = y + ry * 0.12, yOut = y + ry * 0.24, yMid = y + ry * 0.42;
      const xi = x - side * rx * 1.18, xo = x + side * rx * 1.18;
      const lid = `M${f(xi)} ${f(yIn)}Q${x} ${f(yMid)} ${f(xo)} ${f(yOut)}`;
      out.push(`<clipPath id="${cid}"><path d="${lid}L${f(xo)} ${f(y + ry * 2)}L${f(xi)} ${f(y + ry * 2)}Z"/></clipPath>`);
      out.push(`<g clip-path="url(#${cid})"><ellipse cx="${x}" cy="${y}" rx="${f(rx)}" ry="${f(ry)}" fill="${P.ink}"/></g>`);
      out.push(`<path d="${lid}" fill="none" stroke="${P.ink}" stroke-width="${lw * 0.8}" stroke-linecap="round"/>`);
    } else if (mood === 'asleep') {
      const w = P.erx * 1.25;
      out.push(`<path d="M${f(x - w)} ${f(y)}Q${x} ${f(y + P.ery * 0.95)} ${f(x + w)} ${f(y)}" fill="none" stroke="${P.ink}" stroke-width="${lw}" stroke-linecap="round"/>`);
    }
  }
  return out.join('');
}
function mouth(mood, P) {
  const x = P.mx, y = P.my, k = P.mk, ink = P.ink, lw = P.lineW * 0.9;
  if (mood === 'idle') return `<path d="M${f(x - 5.5 * k)} ${f(y - 1 * k)}Q${x} ${f(y + 5.5 * k)} ${f(x + 5.5 * k)} ${f(y - 1 * k)}" fill="none" stroke="${ink}" stroke-width="${lw}" stroke-linecap="round"/>`;
  if (mood === 'happy') {
    const c = id('m');
    return `<clipPath id="${c}"><path d="M${f(x - 8 * k)} ${f(y - 2 * k)}Q${x} ${f(y - 3.4 * k)} ${f(x + 8 * k)} ${f(y - 2 * k)}Q${f(x + 6.5 * k)} ${f(y + 10 * k)} ${x} ${f(y + 10 * k)}Q${f(x - 6.5 * k)} ${f(y + 10 * k)} ${f(x - 8 * k)} ${f(y - 2 * k)}Z"/></clipPath>` +
      `<g clip-path="url(#${c})"><rect x="${x - 10 * k}" y="${y - 5 * k}" width="${20 * k}" height="${18 * k}" fill="${ink}"/><ellipse cx="${x}" cy="${f(y + 10 * k)}" rx="${f(6 * k)}" ry="${f(4.6 * k)}" fill="${P.tongue}"/></g>`;
  }
  if (mood === 'connecting') return mouth('happy', { ...P, mk: P.mk * 0.62, my: P.my + 0.5 });
  if (mood === 'x') return `<path d="M${f(x - 4.5 * k)} ${f(y + 0.5 * k)}Q${x} ${f(y + 3.2 * k)} ${f(x + 4.5 * k)} ${f(y + 0.5 * k)}" fill="none" stroke="${ink}" stroke-width="${lw}" stroke-linecap="round"/>`;
  if (mood === 'surprised') return `<ellipse cx="${x}" cy="${f(y + 3 * k)}" rx="${f(3.8 * k)}" ry="${f(4.8 * k)}" fill="${ink}"/>`;
  if (mood === 'sleepy') return `<ellipse cx="${x}" cy="${f(y + 2.4 * k)}" rx="${f(3 * k)}" ry="${f(3.4 * k)}" fill="${ink}"/>`;
  if (mood === 'asleep') return `<path d="M${f(x - 3 * k)} ${f(y + 2 * k)}Q${x} ${f(y + 4 * k)} ${f(x + 3 * k)} ${f(y + 2 * k)}" fill="none" stroke="${ink}" stroke-width="${lw * 0.9}" stroke-linecap="round"/>`;
  return '';
}

// squash & stretch, volume-preserving, pivot on the cloud's bottom centre
const SQUASH = { idle: [1, 1], happy: [0.94, 1.07], connecting: [1.03, 0.97], surprised: [0.92, 1.09], sleepy: [1.05, 0.95], asleep: [1.12, 0.88] };
const squash = (mood, inner, px = 133, py = 166) => {
  const [sx, sy] = SQUASH[mood];
  return `<g transform="translate(${px},${py}) scale(${sx},${sy}) translate(${-px},${-py})">${inner}</g>`;
};

// ---------- phone + beam props ----------
function phoneProp(D, accent, o) {
  // D: { x, y, w, h, rot, frame, stroke, sw, glow }
  const { x, y, w, h, rot } = o.phoneAt;
  const gid = id('scr');
  const fx = D.glow ? `filter="url(#${D.glow})"` : '';
  const lit = o.mood !== 'asleep';
  const scr = lit ? accent : mix(accent, '#2b2226', 0.55);
  return `<g transform="translate(${x},${y}) rotate(${rot})">
    <linearGradient id="${gid}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${mix(scr, '#ffffff', 0.45)}"/><stop offset="1" stop-color="${scr}"/></linearGradient>
    ${lit && D.glow ? `<rect x="${-w / 2 - 4}" y="${-h / 2 - 4}" width="${w + 8}" height="${h + 8}" rx="10" fill="${accent}" opacity=".55" ${fx}/>` : ''}
    <rect x="${-w / 2}" y="${-h / 2}" width="${w}" height="${h}" rx="${D.prx}" fill="${D.frame}" ${D.stroke ? `stroke="${D.stroke}" stroke-width="${D.sw}"` : ''}/>
    <rect x="${-w / 2 + D.bez}" y="${-h / 2 + D.bez}" width="${w - 2 * D.bez}" height="${h - 2 * D.bez}" rx="${D.prx - D.bez + 1}" fill="url(#${gid})"/>
    ${lit ? `<path d="M${-3} ${-5}L${5} 0L${-3} 5Z" fill="#fff" stroke="#fff" stroke-width="2" stroke-linejoin="round" opacity=".95"/>` : ''}
  </g>`;
}

// ============================================================================================
// Direction A — Soft 3D-ish (no outline, radial shading, glow, pixel ribbon beam)
// ============================================================================================
function dirA(o) {
  const acc = o.accent, ink = '#3A2830';
  const g = id('a'), sh = id('as'), clip = id('ac'), blur = id('ab'), blur2 = id('ab2'), glow = id('ag'), rim = id('ar');
  const P = { ink, ex: [111, 155], ey: 131, erx: 7, ery: 9.2, hl: [2.9, 1.3], mx: 133, my: 140, mk: 1, lineW: 3.4, tongue: '#F27F94' };
  const shapes = cloudShapes(CLOUD);
  const body = `
    <defs>
      <radialGradient id="${g}" gradientUnits="userSpaceOnUse" cx="118" cy="70" r="125"><stop offset="0" stop-color="#FFFFFF"/><stop offset=".55" stop-color="#FBF7F8"/><stop offset="1" stop-color="#E3DAE6"/></radialGradient>
      <linearGradient id="${rim}" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="${acc}" stop-opacity="0"/><stop offset=".6" stop-color="${acc}" stop-opacity=".55"/><stop offset="1" stop-color="${acc}" stop-opacity=".85"/></linearGradient>
      <filter id="${blur}" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="6"/></filter>
      <filter id="${blur2}" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="2.2"/></filter>
      <filter id="${glow}" x="-80%" y="-80%" width="260%" height="260%"><feGaussianBlur stdDeviation="7"/></filter>
      <clipPath id="${clip}">${shapes}</clipPath>
    </defs>
    <g fill="url(#${g})">${shapes}</g>
    <g clip-path="url(#${clip})">
      <ellipse cx="140" cy="186" rx="100" ry="34" fill="#C9BCD3" opacity=".55" filter="url(#${blur})"/>
      <path d="M150 168Q205 160 214 112" fill="none" stroke="url(#${rim})" stroke-width="9" filter="url(#${blur2})"/>
      <ellipse cx="116" cy="66" rx="22" ry="12" transform="rotate(-24 116 66)" fill="#fff" opacity=".95" filter="url(#${blur2})"/>
      <ellipse cx="80" cy="102" rx="9" ry="5" transform="rotate(-35 80 102)" fill="#fff" opacity=".8" filter="url(#${blur2})"/>
    </g>
    <ellipse cx="${P.ex[0] - 15}" cy="143" rx="10" ry="6" fill="#FF9AAE" opacity=".55" filter="url(#${blur2})"/>
    <ellipse cx="${P.ex[1] + 15}" cy="143" rx="10" ry="6" fill="#FF9AAE" opacity=".55" filter="url(#${blur2})"/>
    ${eyes(o.mood, P)}${mouth(o.mood, P)}`;
  const arms = armsSet(o, (x, y, r) => `<ellipse cx="${x + 1}" cy="${y + 3.5}" rx="${r + 1}" ry="${r}" fill="#B9A9C4" opacity=".55" filter="url(#${blur2})"/><circle cx="${x}" cy="${y}" r="${r}" fill="url(#${g})"/><circle cx="${x - r * .3}" cy="${y - r * .35}" r="${r * .35}" fill="#fff" opacity=".7" filter="url(#${blur2})"/>`, 10.5);
  const D = { frame: '#3A2830', prx: 7, bez: 2.6, glow };
  return wrap(o, `
    ${o.shadow === false ? "" : `<ellipse cx="135" cy="196" rx="62" ry="9" fill="${acc}" opacity="${o.mood === 'asleep' ? .12 : .28}" filter="url(#${blur})"/>`}
    ${o.beam ? beamA(o, acc, glow) : ''}
    ${squash(o.mood, body + (o.phone ? phoneProp(D, acc, o) : '') + arms)}
    ${extras(o, { ink: '#FFFFFF', acc, heart: '#FF8FA6', z: '#FFFFFF' })}`);
}
function beamA(o, acc, glow) {
  // ribbon of light from the phone top to the big screen, with drifting pixels
  const [x0, y0] = o.beamFrom, [x1, y1] = o.beamTo;
  const cx = x0 + 6, cy = y1 + 40;
  const pts = [];
  for (let i = 1; i < 9; i++) {
    const t = i / 9, u = 1 - t;
    const px = u * u * x0 + 2 * u * t * cx + t * t * x1, py = u * u * y0 + 2 * u * t * cy + t * t * y1;
    const s = 2.8 + (i % 3) * 1.3, off = (i % 2 ? 1 : -1) * (9 + (i % 3) * 2.5);
    pts.push(`<rect x="${f(px + off - s / 2)}" y="${f(py - s / 2)}" width="${f(s)}" height="${f(s)}" rx=".6" fill="${i % 3 ? acc : '#fff'}" opacity="${f(0.55 + 0.45 * (i % 2))}"/>`);
  }
  const d = `M${x0} ${y0}Q${cx} ${cy} ${x1} ${y1}`;
  return `<path d="${d}" fill="none" stroke="${acc}" stroke-width="24" stroke-linecap="round" opacity=".32" filter="url(#${glow})"/>
    <path d="${d}" fill="none" stroke="${acc}" stroke-width="10" stroke-linecap="round" opacity=".5"/>
    <path d="${d}" fill="none" stroke="${acc}" stroke-width="5" stroke-linecap="round" opacity=".8"/>
    <path d="${d}" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" opacity=".95"/>${pts.join('')}`;
}

// ============================================================================================
// Direction B — Flat sticker (uniform warm outline, flat fill, ▶ forehead mark, wave beam)
// ============================================================================================
function dirB(o) {
  const acc = o.accent, ink = '#4A2C30', OW = 6.5, dc = id('dc');
  const clip = id('bc');
  const P = { ink, ex: [111, 156], ey: 130, erx: 8, ery: 10.2, hl: [3.3, 1.5], mx: 133.5, my: 141, mk: 1.15, lineW: 4.2, tongue: '#F27F94' };
  const shapes = cloudShapes(CLOUD);
  const shade = mix('#FFFFFF', acc, 0.28);
  const body = `
    <clipPath id="${clip}">${shapes}</clipPath>
    <g fill="${ink}" stroke="${ink}" stroke-width="${OW * 2}" stroke-linejoin="round">${shapes}</g>
    <g fill="${shade}">${shapes}</g>
    <g clip-path="url(#${clip})">
      <g fill="#FFFDFB" transform="translate(-5,-9)">${shapes}</g>
    </g>
    <path d="M100 74Q106 60 120 56" fill="none" stroke="#fff" stroke-width="5" stroke-linecap="round" opacity=".0"/>
    <path d="M128.5 79.5L139.5 86.5L128.5 93.5Z" fill="${acc}" stroke="${acc}" stroke-width="4.5" stroke-linejoin="round"/>
    <ellipse cx="${P.ex[0] - 16}" cy="144" rx="9.5" ry="5.6" fill="#FFA8B8"/>
    <ellipse cx="${P.ex[1] + 16}" cy="144" rx="9.5" ry="5.6" fill="#FFA8B8"/>
    ${eyes(o.mood, P)}${mouth(o.mood, P)}`;
  const arms = armsSet(o, (x, y, r, kind) => kind === 'rest'
    ? `<path d="M${x - 9} ${y - 4}Q${x - 8} ${y + 7} ${x} ${y + 7}Q${x + 8} ${y + 7} ${x + 9} ${y - 4}" fill="none" stroke="${ink}" stroke-width="4.5" stroke-linecap="round"/>`
    : `<circle cx="${x}" cy="${y}" r="${r}" fill="#FFFDFB" stroke="${ink}" stroke-width="5"/>`, 12.5);
  const D = { frame: '#FFFDFB', stroke: ink, sw: OW, prx: 8, bez: 5 };
  return wrap(o, `
    ${o.shadow === false ? "" : `<ellipse cx="135" cy="194" rx="50" ry="6" fill="#000" opacity="${o.mood === 'asleep' ? .14 : .2}"/>`}
    ${o.beam ? beamB(o, acc, ink) : ''}
    <filter id="${dc}" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur in="SourceAlpha" stdDeviation="3.2"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 14 -1" result="d"/><feFlood flood-color="#FFF4EF"/><feComposite in2="d" operator="in" result="w"/><feMerge><feMergeNode in="w"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
    <g filter="url(#${dc})">${squash(o.mood, body + (o.phone ? phoneProp(D, acc, o) : "") + arms)}${extras(o, { ink, acc, heart: '#FF7F98', z: ink, outline: ink })}</g>`);
}
function beamB(o, acc, ink) {
  const [x0, y0] = o.beamFrom, [x1, y1] = o.beamTo;
  const ang = Math.atan2(y1 - y0, x1 - x0) * 180 / Math.PI;
  const len = Math.hypot(x1 - x0, y1 - y0);
  let s = '';
  // chunky dashed path of ▶ chevrons travelling to the screen
  for (let i = 0; i < 4; i++) {
    const t = 0.12 + i * 0.22, k = 0.8 + i * 0.12;
    s += `<g transform="translate(${f(x0 + (x1 - x0) * t)},${f(y0 + (y1 - y0) * t)}) rotate(${f(ang)}) scale(${f(k)})"><path d="M-4 -8L5 0L-4 8" fill="none" stroke="${ink}" stroke-width="10" stroke-linecap="round" stroke-linejoin="round"/><path d="M-4 -8L5 0L-4 8" fill="none" stroke="${acc}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/></g>`;
  }
  return s;
}

// ============================================================================================
// Direction C — Geometric premium (strict circle grid, no outline, 2-tone, wisp + screen glint)
// ============================================================================================
function dirC(o) {
  const acc = o.accent, ink = '#33262C';
  const clip = id('cc');
  const P = { ink, ex: [106, 152], ey: 139, erx: 6.2, ery: 9, hl: [2.3, 0], mx: 129, my: 149, mk: 0.95, lineW: 3.6, tongue: '#EE7C92' };
  const shapes = cloudShapes(GEO);
  const light = '#FBF5F1', shade = '#DBD0DB';
  const wispLit = o.mood !== 'asleep' && o.mood !== 'sleepy';
  const body = `
    <clipPath id="${clip}">${shapes}</clipPath>
    ${castMark(186, 74, wispLit ? acc : '#8E7F86', o.mood === 'connecting' ? 2 : wispLit ? 1 : 0)}
    <g fill="${shade}">${shapes}</g>
    <g clip-path="url(#${clip})"><g fill="${light}" transform="translate(-7,-10)">${shapes}</g></g>
    <rect x="86" y="68" width="11" height="17" rx="3.5" transform="rotate(-28 91 76)" fill="#fff"/>
    <circle cx="103" cy="66" r="2.8" fill="#fff"/>
    <circle cx="${P.ex[0] - 16}" cy="151" r="6.5" fill="#F8BAC6"/>
    <circle cx="${P.ex[1] + 16}" cy="151" r="6.5" fill="#F8BAC6"/>
    ${eyes(o.mood, P)}${mouth(o.mood, P)}`;
  const arms = armsSet(o, (x, y, r) => `<circle cx="${x}" cy="${y}" r="${r}" fill="${shade}"/><circle cx="${x - 1.5}" cy="${y - 2}" r="${r - 2}" fill="${light}"/>`, 10);
  const D = { frame: ink, prx: 7, bez: 3 };
  return wrap(o, `
    ${o.shadow === false ? "" : `<ellipse cx="135" cy="194" rx="56" ry="6" fill="#000" opacity=".2"/>`}
    ${o.beam ? beamC(o, acc) : ''}
    ${squash(o.mood, body + (o.phone ? phoneProp(D, acc, o) : '') + arms)}
    ${extras(o, { ink: light, acc, heart: '#FF8FA6', z: light })}`);
}
// signature: a floating cast-signal mark (dot + quarter arcs) riding above the right puff
function castMark(x, y, c, level) {
  let s = `<circle cx="${x}" cy="${y}" r="5" fill="${c}"/>`;
  for (let i = 1; i <= level; i++) { const r = 5 + i * 7; s += `<path d="M${x} ${y - r}A${r} ${r} 0 0 1 ${x + r} ${y}" fill="none" stroke="${c}" stroke-width="4" stroke-linecap="round" opacity="${1 - i * .2}"/>`; }
  return `<g transform="rotate(-8 ${x} ${y})">${s}</g>`;
}
function beamC(o, acc) {
  const [x0, y0] = o.beamFrom, [x1, y1] = o.beamTo;
  let s = '';
  for (let i = 0; i < 5; i++) {
    const t = 0.08 + i * 0.165, r = 3 + i * 1.1;
    s += `<circle cx="${f(x0 + (x1 - x0) * t)}" cy="${f(y0 + (y1 - y0) * t)}" r="${f(r)}" fill="${acc}" opacity="${f(0.45 + i * 0.12)}"/>`;
  }
  return s;
}

// ---------- shared pose plumbing ----------
function armsSet(o, nub, r) {
  if (!o.phone) {
    // no prop: nubs rest on the sides, happy raises them
    const up = o.mood === 'happy' || o.mood === 'surprised';
    return nub(up ? 50 : 80, up ? 112 : 154, r, up ? 'wave' : 'rest') + nub(up ? 216 : 186, up ? 112 : 154, r, up ? 'wave' : 'rest');
  }
  const { x, y } = o.phoneAt;
  const wave = o.mood === 'happy' || o.mood === 'surprised';
  const rest = o.mood === 'asleep' || o.mood === 'sleepy';
  if (wave) return nub(48, 112, r, 'wave') + nub(x - 13, y + 12, r, 'hold');
  // calm poses: only the phone hand shows (fewest shapes; the free nub is tucked behind the cloud)
  return nub(x - 13, y + 12, r, 'hold');
}
function extras(o, c) {
  const m = o.mood;
  let s = '';
  if (m === 'happy') s += heart(70, 62, 1.9, c.heart, -14) + heart(198, 50, 1.5, c.heart, 12) + heart(88, 36, 1.0, c.heart, -6, 'opacity=".75"') + sparkle(214, 84, .9, c.ink, .8);
  if (m === 'surprised') {
    const w = c.outline ? 4.5 : 3.6;
    s += [[78, 52, 66, 40], [96, 40, 90, 26], [188, 52, 200, 40], [170, 40, 176, 26]].map(([a, b, c2, d]) => `<path d="M${a} ${b}L${c2} ${d}" stroke="${c.ink}" stroke-width="${w}" stroke-linecap="round"/>`).join('');
  }
  if (m === 'sleepy') s += zzz(194, 54, .9, c.z, 2.6).split('<path').slice(0, 2).join('<path');
  if (m === 'asleep') s += zzz(186, 50, 1.2, c.z, 2.8);
  if (m === 'connecting' && !o.beam) s += sparkle(214, 70, 1, c.acc) + sparkle(228, 96, .6, c.acc, .7);
  if (m === 'idle' && o.sparkles !== false) s += sparkle(214, 72, .8, c.acc, .9) + sparkle(60, 70, .55, c.ink, .5);
  return s;
}
function wrap(o, inner) {
  return `<g class="toutou" transform="rotate(${o.tilt ?? -3} 133 166)">${inner}</g>`;
}

const DIRS = { A: dirA, B: dirB, C: dirC };
export function toutou(dir, opts = {}) {
  const o = { mood: 'idle', accent: '#F5A7A7', phone: true, ...opts };
  o.phoneAt = o.phoneAt || { x: 214, y: 132, w: 30, h: 48, rot: 12 };
  if (o.mood === 'asleep' && o.phone) o.phoneAt = { x: 196, y: 172, w: 30, h: 48, rot: 78 };
  if (o.mood === 'sleepy' && o.phone) o.phoneAt = { x: 200, y: 154, w: 30, h: 48, rot: 28 };
  if (!o.beamFrom) { const { x, y, h, rot } = o.phoneAt; const a = rot * Math.PI / 180; o.beamFrom = [f(x + Math.sin(a) * (h / 2 + 4)), f(y - Math.cos(a) * (h / 2 + 4))]; }
  o.beamTo = o.beamTo || [262, 46];
  if (o.mood === 'connecting' && o.beam === undefined) o.beam = true;
  return DIRS[dir](o);
}

// mini "big screen" the beam lands on
export function monitor(x, y, acc, style) {
  const ink = style === 'B' ? '#4A2C30' : '#3A2830';
  const st = style === 'B' ? `stroke="${ink}" stroke-width="5" stroke-linejoin="round"` : '';
  const frame = style === 'C' ? '#4A3A42' : style === 'B' ? '#FFFDFB' : '#4A3A42';
  return `<g transform="translate(${x},${y})">
    <rect x="-8" y="26" width="16" height="9" fill="${frame}" ${st}/><rect x="-16" y="34" width="32" height="5" rx="2.5" fill="${frame}" ${st}/>
    <rect x="-34" y="-22" width="68" height="48" rx="8" fill="${frame}" ${st}/>
    <rect x="-28" y="-16" width="56" height="36" rx="4" fill="${acc}"/>
    <rect x="-28" y="-16" width="56" height="12" rx="4" fill="#fff" opacity=".25"/>
    <path d="M-4 -6L6 2L-4 10Z" fill="#fff" stroke="#fff" stroke-width="2.5" stroke-linejoin="round"/>
  </g>`;
}

// ============================================================================================
// Icons — hand-hinted per size class. size in px; returns full <svg>.
// ============================================================================================
export function icon(dir, size, accent = '#F5A7A7', bgTile = '#2A1C1F') {
  const s = size;
  if (s > 48) {
    // full cloud + face + glowing phone, inside the direction's tile
    const inner = toutou(dir, { mood: 'idle', accent, phone: true, tilt: -4, sparkles: false, shadow: false });
    const tile = tileFor(dir, accent, bgTile, 256);
    const T = dir === 'B' ? 'translate(-46,-14) scale(1.22)' : 'translate(-50,-10) scale(1.25)';
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 256 256">${tile}<g transform="${T}">${inner}</g></svg>`;
  }
  return smallIcon(dir, s, accent, bgTile);
}
function tileFor(dir, acc, bg, S) {
  const r = S * 0.22;
  if (dir === 'A') return `<defs><linearGradient id="ta" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${mix(acc, '#ffffff', .25)}"/><stop offset="1" stop-color="${mix(acc, '#7a3a48', .25)}"/></linearGradient></defs><rect width="${S}" height="${S}" rx="${r}" fill="url(#ta)"/>`;
  if (dir === 'B') return '';
  return `<rect width="${S}" height="${S}" rx="${r}" fill="${bg}"/>`;
}
// 16 / 24 / 32: drawn on a 16-unit grid (scaled), silhouette + 2 eyes + accent cue only.
function smallIcon(dir, s, acc, bg) {
  const ink = dir === 'B' ? '#4A2C30' : '#33262C';
  // cloud on a 16 grid: puffs (5.5,8.5,r3) (9.5,6.5,r4) (12,9.5,r2.5) + base x2.5..14.5 y8..13
  const C = s >= 32
    ? { p: [[5.6, 8.6, 3.2], [9.3, 6.6, 4.1], [12.3, 9.2, 2.6]], b: [2.4, 8, 12.4, 5.2, 2.6], e: [[6.9, 10.3], [10.9, 10.3]], er: [0.8, 1.05] }
    : { p: [[5.5, 8.5, 3.2], [9.5, 6.5, 4.2], [12.3, 9.2, 2.7]], b: [2.3, 8, 12.6, 5.5, 2.75], e: [[7, 10.5], [11, 10.5]], er: [0.95, 1.25] };
  if (dir === 'C') { const k = 0.088, ox = 1.2 - 56 * k, oy = 3.6 - 56 * k, T = (x, y) => [f(x * k + ox), f(y * k + oy)];
    C.p = GEO.puffs.map(([x, y, r]) => [...T(x, y), f(r * k)]); const b = GEO.base; C.b = [...T(b.x, b.y), f(b.w * k), f(b.h * k), f(b.r * k)];
    C.e = [T(106, 140), T(150, 140)]; C.er = s >= 32 ? [0.62, 0.9] : [0.8, 1.15]; }
  const sh = (attrs) => C.p.map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" ${attrs}/>`).join('') + `<rect x="${C.b[0]}" y="${C.b[1]}" width="${C.b[2]}" height="${C.b[3]}" rx="${C.b[4]}" ${attrs}/>`;
  const eyesS = C.e.map(([x, y]) => `<ellipse cx="${x}" cy="${y}" rx="${C.er[0]}" ry="${C.er[1]}" fill="${ink}"/>`).join('') +
    (s >= 24 ? C.e.map(([x, y]) => `<circle cx="${x - 0.3}" cy="${y - 0.45}" r="${s >= 32 ? 0.38 : 0.34}" fill="#fff"/>`).join('') : '');
  const cheeks = s >= 32 ? `<ellipse cx="${f(C.e[0][0] - 1.6)}" cy="${f(C.e[0][1] + 1.1)}" rx="0.9" ry="0.55" fill="#FF9AAE"/><ellipse cx="${f(C.e[1][0] + 1.6)}" cy="${f(C.e[1][1] + 1.1)}" rx="0.9" ry="0.55" fill="#FF9AAE"/>` : "";
  let body;
  if (dir === 'A') {
    body = `<defs><linearGradient id="tA" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${mix(acc, '#ffffff', .25)}"/><stop offset="1" stop-color="${mix(acc, '#7a3a48', .25)}"/></linearGradient><radialGradient id="cA" cx=".4" cy=".3" r=".8"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#E6DDE9"/></radialGradient></defs>
      <rect width="16" height="16" rx="3.6" fill="url(#tA)"/><g fill="url(#cA)" transform="translate(-0.4,0.2)">${sh('')}</g><g transform="translate(-0.4,0.2)">${eyesS}${cheeks}</g>`;
  } else if (dir === 'B') {
    const w = s >= 32 ? 1.1 : s >= 24 ? 1.25 : 1.35;
    body = `<g transform="translate(-0.45,0.1)"><g fill="${ink}" stroke="${ink}" stroke-width="${w * 2}" stroke-linejoin="round">${sh('')}</g><g fill="#FFFDFB">${sh('')}</g>
      ${s >= 24 ? `<path d="M8.9 4.6L10.8 5.8L8.9 7Z" fill="${acc}" stroke="${acc}" stroke-width=".7" stroke-linejoin="round"/>` : `<rect x="8.6" y="4.5" width="2" height="2" rx=".6" fill="${acc}"/>`}${eyesS}${cheeks}</g>`;
  } else {
    body = `<rect width="16" height="16" rx="3.6" fill="${bg}"/>
      <g transform="translate(-0.4,0.3)"><g fill="#E9E1E4">${sh('')}</g><clipPath id="cC">${sh('')}</clipPath><g clip-path="url(#cC)"><g fill="#FFFCF8" transform="translate(-.5,-.7)">${sh('')}</g></g>${eyesS}${cheeks}
      <circle cx="13" cy="4.3" r="${s >= 24 ? 1.05 : 1.3}" fill="${acc}"/>${s >= 24 ? `<path d="M13 2.2A2.1 2.1 0 0 1 15.1 4.3" fill="none" stroke="${acc}" stroke-width=".8" stroke-linecap="round"/>` : ""}</g>`;
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 16 16">${body}</svg>`;
}
