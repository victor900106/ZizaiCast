// 投投 Toutou — FINAL hero art (Direction A 「軟綿光暈 Soft Glow」) + app icon (Direction C construction,
// A palette).  Everything is built in the concept frame (0 0 300 240): cloud base on y=166, visual centre
// x=133, whole character tilted TILT degrees around (133,166).
//
// Layer model (what the app embeds; see export.mjs):
//   cloud      body shading only (no face)                      neutral
//   rim        alpha mask of the phone-side rim glow            tinted with the theme accent at runtime
//   face_<m>   eyes + mouth + blush (+ the waving nub for happy / surprised)
//   hand_<p>   the cloud-nub holding the phone, per phone pose (drawn ABOVE the phone)
//   phone      procedural in the app (frame + accent screen + ▶), PHONE[pose] gives its placement
//   beam       procedural in the app (ribbon + square pixels), beamPath() is the reference shape
// hero(opts) composes the same layers as SVG for sheets / docs.

export const THEMES = {
  Sakura: { accent: '#F5A7A7', bg: '#2A1C1F', bg2: '#181012', text: '#FFF4F1', sub: '#D1B0B0' },
  Mint: { accent: '#8FE3C4', bg: '#123230', bg2: '#0A1D1C', text: '#F0FCF8', sub: '#A6CFC4' },
  Night: { accent: '#BBA9F7', bg: '#1B1D3D', bg2: '#0D0E23', text: '#F4F2FF', sub: '#B3AFD8' },
  MilkTea: { accent: '#E3B98A', bg: '#30231B', bg2: '#1A120D', text: '#FFF6EC', sub: '#D8C3AC' },
};
export const MOODS = ['idle', 'blink', 'happy', 'connecting', 'surprised', 'sleepy', 'asleep'];
export const TILT = -3;
export const PIVOT = [133, 166];
// Phone placement (centre, size, rotation in degrees) per pose, before the tilt.
export const PHONE = {
  hold: { x: 221, y: 126, w: 30, h: 48, rot: 14 },
  sleepy: { x: 208, y: 150, w: 30, h: 48, rot: 34 },
  asleep: { x: 200, y: 174, w: 30, h: 48, rot: 80 },
};
export const poseOf = (mood) => (mood === 'sleepy' ? 'sleepy' : mood === 'asleep' ? 'asleep' : 'hold');
// Layer canvas in frame units (all PNG layers share it so they align 1:1).
export const CANVAS = { x: 30, y: 38, w: 210, h: 150 };

const INK = '#3A2830';
const f = (n) => +(+n).toFixed(2);
let uid = 0;
const id = (p) => `${p}${++uid}`;

const h2r = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
const r2h = (c) => '#' + c.map((v) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
export const mix = (a, b, t) => { const x = h2r(a), y = h2r(b); return r2h(x.map((v, i) => v + (y[i] - v) * t)); };

// ------------------------------------------------------------------ cloud
const CLOUD = {
  puffs: [[86, 112, 30], [132, 90, 40], [178, 114, 26]],
  base: { x: 54, y: 108, w: 158, h: 58, r: 29 },
};
const cloudShapes = (attrs = '') =>
  CLOUD.puffs.map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" ${attrs}/>`).join('') +
  `<rect x="${CLOUD.base.x}" y="${CLOUD.base.y}" width="${CLOUD.base.w}" height="${CLOUD.base.h}" rx="${CLOUD.base.r}" ${attrs}/>`;

// Shared body gradient (userSpaceOnUse so the hand nubs continue the same light).
function bodyDefs(p) {
  return `<radialGradient id="${p}g" gradientUnits="userSpaceOnUse" cx="116" cy="66" r="132">
      <stop offset="0" stop-color="#FFFFFF"/><stop offset=".5" stop-color="#FCF8F8"/><stop offset=".82" stop-color="#EEE6EE"/><stop offset="1" stop-color="#DDD2E0"/></radialGradient>
    <filter id="${p}b6" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="6"/></filter>
    <filter id="${p}b3" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="3"/></filter>
    <filter id="${p}b2" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="2"/></filter>
    <filter id="${p}b1" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="1.1"/></filter>`;
}

function cloudLayer(p) {
  const clip = id('cc');
  return `<defs>${bodyDefs(p)}<clipPath id="${clip}">${cloudShapes()}</clipPath></defs>
    <g fill="url(#${p}g)">${cloudShapes()}</g>
    <g clip-path="url(#${clip})">
      <ellipse cx="138" cy="190" rx="104" ry="38" fill="#C3B4CF" opacity=".62" filter="url(#${p}b6)"/>
      <ellipse cx="96" cy="150" rx="40" ry="8" fill="#D9CCDD" opacity=".35" filter="url(#${p}b6)"/>
      <path d="M91 82Q99 89 99 99" fill="none" stroke="#D8CCDC" stroke-width="5" opacity=".55" filter="url(#${p}b3)"/>
      <path d="M173 88Q166 95 167 105" fill="none" stroke="#D8CCDC" stroke-width="5" opacity=".5" filter="url(#${p}b3)"/>
      <ellipse cx="114" cy="66" rx="23" ry="12" transform="rotate(-24 114 66)" fill="#fff" opacity=".95" filter="url(#${p}b2)"/>
      <ellipse cx="76" cy="100" rx="9" ry="5" transform="rotate(-38 76 100)" fill="#fff" opacity=".85" filter="url(#${p}b2)"/>
      <ellipse cx="172" cy="96" rx="7" ry="4" transform="rotate(-30 172 96)" fill="#fff" opacity=".7" filter="url(#${p}b2)"/>
    </g>`;
}
// Rim light from the phone: white, the app tints it with the accent (FillOpacityMask).
function rimLayer(p, color = '#FFFFFF') {
  const clip = id('rc');
  return `<defs>${bodyDefs(p)}<clipPath id="${clip}">${cloudShapes()}</clipPath>
      <linearGradient id="${p}rim" gradientUnits="userSpaceOnUse" x1="150" y1="0" x2="214" y2="0"><stop offset="0" stop-color="${color}" stop-opacity="0"/><stop offset=".55" stop-color="${color}" stop-opacity=".6"/><stop offset="1" stop-color="${color}" stop-opacity="1"/></linearGradient></defs>
    <g clip-path="url(#${clip})">
      <path d="M146 170Q206 164 213 108Q210 92 196 92" fill="none" stroke="url(#${p}rim)" stroke-width="11" filter="url(#${p}b3)"/>
    </g>`;
}

// ------------------------------------------------------------------ face
// Eye: vertical oval, warm lighter lower half, soft lower-lid crescent, two highlights (light from the
// upper left on BOTH eyes).  k = per-eye size (slight asymmetry), gx/gy = gaze offset.
function openEye(x, y, rx, ry, o = {}) {
  const g = id('eg'), c = id('ec');
  const hl = o.hl ?? 1, gx = o.gx ?? 0, gy = o.gy ?? 0;
  let s = `<defs><linearGradient id="${g}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#24161C"/><stop offset=".55" stop-color="${INK}"/><stop offset="1" stop-color="#6A4552"/></linearGradient>
    <clipPath id="${c}"><ellipse cx="${f(x)}" cy="${f(y)}" rx="${f(rx)}" ry="${f(ry)}"/></clipPath></defs>
    <ellipse cx="${f(x)}" cy="${f(y)}" rx="${f(rx)}" ry="${f(ry)}" fill="url(#${g})"/>
    <g clip-path="url(#${c})"><ellipse cx="${f(x + gx * 0.4)}" cy="${f(y + ry * 1.02)}" rx="${f(rx * 0.95)}" ry="${f(ry * 0.5)}" fill="#9A6676" opacity=".32"/></g>`;
  if (o.star) {
    const k = rx * 0.17 * hl;
    s += `<path transform="translate(${f(x - rx * 0.22 + gx)},${f(y - ry * 0.32 + gy)}) scale(${f(k)})" d="M0-6C.8-1.7 1.7-.8 6 0 1.7.8.8 1.7 0 6-.8 1.7-1.7.8-6 0-1.7-.8-.8-1.7 0-6Z" fill="#fff"/>`;
  } else {
    s += `<ellipse cx="${f(x - rx * 0.3 + gx)}" cy="${f(y - ry * 0.36 + gy)}" rx="${f(rx * 0.44 * hl)}" ry="${f(rx * 0.48 * hl)}" fill="#fff"/>`;
  }
  s += `<circle cx="${f(x + rx * 0.36 + gx * 0.5)}" cy="${f(y + ry * 0.36 + gy * 0.5)}" r="${f(rx * 0.2 * hl)}" fill="#fff" opacity=".9"/>`;
  return s;
}
const stroke = (d, w, c = INK, extra = '') => `<path d="${d}" fill="none" stroke="${c}" stroke-width="${w}" stroke-linecap="round" stroke-linejoin="round" ${extra}/>`;

const EYES = { lx: 110.5, rx: 155.5, y: 131, erx: 7.3, ery: 9.6 };
function face(mood, p) {
  const { lx, rx, y, erx, ery } = EYES;
  const mx = 133, my = 140;
  let eyes = '', mouth = '', blushA = 0.55, blushK = 1, extra = '';
  // asymmetry: the near (left) eye a touch larger, the far one a touch higher (the head is tilted)
  const L = { x: lx, y: y + 0.3, k: 1.04 }, R = { x: rx, y: y - 0.3, k: 0.98 };
  if (mood === 'idle') {
    for (const e of [L, R]) eyes += openEye(e.x, e.y, erx * e.k, ery * e.k);
    mouth = stroke(`M${mx - 5.2} ${my - 0.6}Q${mx} ${my + 5.2} ${mx + 5.2} ${my - 0.6}`, 3.2);
  } else if (mood === 'blink') {
    for (const e of [L, R]) eyes += stroke(`M${f(e.x - erx * 1.15)} ${f(e.y + 1.5)}Q${e.x} ${f(e.y + 5.2)} ${f(e.x + erx * 1.15)} ${f(e.y + 1.5)}`, 3.2);
    mouth = stroke(`M${mx - 5.2} ${my - 0.6}Q${mx} ${my + 5.2} ${mx + 5.2} ${my - 0.6}`, 3.2);
  } else if (mood === 'happy') {
    for (const e of [L, R]) {
      const w = erx * 1.35, h = ery * 1.05;
      eyes += stroke(`M${f(e.x - w)} ${f(e.y + h * 0.42)}Q${f(e.x)} ${f(e.y - h * 1.3)} ${f(e.x + w)} ${f(e.y + h * 0.42)}`, 3.9);
    }
    const c = id('m');
    mouth = `<clipPath id="${c}"><path d="M${mx - 8.5} ${my - 2.4}Q${mx} ${my - 3.6} ${mx + 8.5} ${my - 2.4}Q${mx + 7} ${my + 10.5} ${mx} ${my + 10.5}Q${mx - 7} ${my + 10.5} ${mx - 8.5} ${my - 2.4}Z"/></clipPath>
      <g clip-path="url(#${c})"><rect x="${mx - 11}" y="${my - 6}" width="22" height="20" fill="${INK}"/><ellipse cx="${mx}" cy="${my + 10.5}" rx="6.4" ry="5" fill="#F27F94"/></g>`;
    blushA = 0.8; blushK = 1.15;
    // free nub thrown up in a wave (left side) — a small puff growing out of the left cheek
    extra = waveNub(p);
  } else if (mood === 'connecting') {
    // eager focus: eyes a touch bigger, gaze up-right toward the beam, star glints; mouth pressed into a small
    // determined smile, shifted toward the phone.
    for (const e of [L, R]) eyes += openEye(e.x + 2, e.y - 0.8, erx * e.k * 1.08, ery * e.k * 1.06, { gx: 1.8, gy: -1.8, hl: 1.2 });
    const c = id('m'), x = mx + 2, y = my + 0.4;
    mouth = `<clipPath id="${c}"><path d="M${x - 5.6} ${y - 1.4}Q${x} ${y - 2.4} ${x + 5.6} ${y - 1.4}Q${x + 4.6} ${y + 6.4} ${x} ${y + 6.4}Q${x - 4.6} ${y + 6.4} ${x - 5.6} ${y - 1.4}Z"/></clipPath>
      <g clip-path="url(#${c})"><rect x="${x - 8}" y="${y - 4}" width="16" height="12" fill="${INK}"/><ellipse cx="${x}" cy="${y + 6.6}" rx="4" ry="3" fill="#F27F94"/></g>`;
    blushA = 0.7;
  } else if (mood === 'surprised') {
    for (const e of [L, R]) eyes += openEye(e.x, e.y - 1.2, erx * e.k * 1.2, ery * e.k * 1.2, { hl: 1.05 });
    mouth = `<ellipse cx="${mx}" cy="${my + 3.6}" rx="3.9" ry="5" fill="${INK}"/><ellipse cx="${mx}" cy="${my + 6}" rx="2.4" ry="1.6" fill="#E07A8E" opacity=".9"/>`;
    extra = waveNub(p, true);
    blushA = 0.45;
  } else if (mood === 'sleepy') {
    for (const [i, e] of [L, R].entries()) {
      const side = i === 0 ? -1 : 1, c = id('lid');
      const rx_ = erx * e.k, ry_ = ery * e.k;
      const xi = e.x - side * rx_ * 1.2, xo = e.x + side * rx_ * 1.2;
      const yIn = e.y + ry_ * 0.42, yOut = e.y + ry_ * 0.52, yMid = e.y + ry_ * 0.74;
      const lid = `M${f(xi)} ${f(yIn)}Q${f(e.x)} ${f(yMid)} ${f(xo)} ${f(yOut)}`;
      eyes += `<clipPath id="${c}"><path d="${lid}L${f(xo)} ${f(e.y + ry_ * 2)}L${f(xi)} ${f(e.y + ry_ * 2)}Z"/></clipPath>
        <g clip-path="url(#${c})">${openEye(e.x, e.y, rx_, ry_, { hl: 0.7, gy: 6.2 })}</g>` + stroke(lid, 3.2);
    }
    mouth = `<ellipse cx="${mx + 0.5}" cy="${my + 3.2}" rx="3.6" ry="4.6" fill="${INK}"/><ellipse cx="${mx + 0.5}" cy="${my + 5.6}" rx="2.3" ry="1.7" fill="#E07A8E" opacity=".85"/>`;
    blushA = 0.5;
  } else if (mood === 'asleep') {
    for (const e of [L, R]) {
      const w = erx * 1.3;
      eyes += stroke(`M${f(e.x - w)} ${f(e.y + 0.5)}Q${f(e.x)} ${f(e.y + ery * 1.0)} ${f(e.x + w)} ${f(e.y + 0.5)}`, 3.4);
    }
    mouth = stroke(`M${mx - 2.8} ${my + 2.6}Q${mx} ${my + 4.6} ${mx + 2.8} ${my + 2.6}`, 2.9);
    blushA = 0.6;
  }
  const blush = [lx - 16, rx + 16].map((x) => `<ellipse cx="${x}" cy="143.5" rx="${f(10.5 * blushK)}" ry="${f(6.2 * blushK)}" fill="#FF94AA" opacity="${blushA}" filter="url(#${p}b2)"/>`).join('');
  return `<defs>${bodyDefs(p)}</defs>${extra}${blush}${eyes}${mouth}`;
}

// ------------------------------------------------------------------ nubs
// A nub is a little puff that grows OUT of the cloud: egg-shaped, lit like the body (light from the upper
// left), a soft lavender underside, a faint occlusion shadow where it lies on the body and NO specular dot
// (a glossy dot + dark drop shadow is what made the old one read as a pearl).
function nubDefs(n) {
  return `<radialGradient id="${n}" cx=".34" cy=".26" r=".85"><stop offset="0" stop-color="#FFFFFF"/><stop offset=".55" stop-color="#FAF6F9"/><stop offset="1" stop-color="#E2D8E7"/></radialGradient>`;
}
// shapes: [[cx, cy, rx, ry]] in nub-local units (union, one gradient over the whole union)
function nub(p, x, y, rot, shapes, occl = true) {
  const n = id('ng'), c = id('nc');
  const sh = shapes.map(([cx, cy, rx, ry]) => `<ellipse cx="${cx}" cy="${cy}" rx="${rx}" ry="${ry}"/>`).join('');
  const xs = shapes.flatMap(([cx, , rx]) => [cx - rx, cx + rx]), ys = shapes.flatMap(([, cy, , ry]) => [cy - ry, cy + ry]);
  const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)];
  return `<g transform="translate(${f(x)},${f(y)}) rotate(${f(rot)})">
      <defs>${nubDefs(n)}<clipPath id="${c}">${sh}</clipPath></defs>
      ${occl ? `<g fill="#4A3552" opacity=".26" filter="url(#${p}b2)" transform="translate(1.5,3.2)">${sh}</g>` : ''}
      <g clip-path="url(#${c})">
        <rect x="${x0}" y="${y0}" width="${x1 - x0}" height="${y1 - y0}" fill="url(#${n})"/>
        <ellipse cx="${f((x0 + x1) / 2 + 2)}" cy="${f(y1 + 1)}" rx="${f((x1 - x0) * 0.62)}" ry="${f((y1 - y0) * 0.36)}" fill="#CDBFD8" opacity=".75" filter="url(#${p}b2)"/>
      </g></g>`;
}
function waveNub(p, startled = false) {
  // free nub thrown up on the left: root buried in the left puff, tip up-left
  return nub(p, startled ? 64 : 63, startled ? 101 : 99, startled ? -38 : -50, [[0, -4, 9.4, 14.5]], false);
}
// Hand holding the phone: a mitten nub reaching from the body's right edge over the phone's left edge, a
// small thumb bump resting on the screen.  Placed in phone-local coordinates so it follows each pose.
function handLayer(p, pose) {
  const P = PHONE[pose];
  const a = (P.rot * Math.PI) / 180, ca = Math.cos(a), sa = Math.sin(a);
  const L = (u, v) => [P.x + u * ca - v * sa, P.y + u * sa + v * ca];
  const [hx, hy] = L(-P.w * 0.36, P.h * 0.28);
  // arm stub: tip (mitten) over the phone's lower-left corner, root buried in the body, pointing up-right
  const rot = pose === 'asleep' ? -10 : pose === 'sleepy' ? 6 : -18;
  const c = id('cr');
  return `<defs>${bodyDefs(p)}</defs>
    ${nub(p, hx, hy, rot, [[0, 0, 9.6, 8.8], [-9, 1.2, 10, 7.8]], true)}
    <g transform="translate(${f(hx)},${f(hy)}) rotate(${rot})"><path d="M-17 -6Q-21 1 -16 8" fill="none" stroke="#CDBFD8" stroke-width="2.4" stroke-linecap="round" opacity=".55" filter="url(#${p}b1)"/></g>`;
}

// ------------------------------------------------------------------ phone (reference; the app draws it)
export function phoneSvg(pose, accent, lit = true, glow = true) {
  const P = PHONE[pose];
  const g = id('ps'), b = id('pb');
  const scr = lit ? accent : mix(accent, '#2b2226', 0.62);
  return `<defs><linearGradient id="${g}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${mix(scr, '#ffffff', lit ? 0.5 : 0.1)}"/><stop offset="1" stop-color="${scr}"/></linearGradient>
      <filter id="${b}" x="-80%" y="-80%" width="260%" height="260%"><feGaussianBlur stdDeviation="7"/></filter></defs>
    <g transform="translate(${P.x},${P.y}) rotate(${P.rot})">
      ${lit && glow ? `<rect x="${-P.w / 2 - 5}" y="${-P.h / 2 - 5}" width="${P.w + 10}" height="${P.h + 10}" rx="12" fill="${accent}" opacity=".6" filter="url(#${b})"/>` : ''}
      <rect x="${-P.w / 2}" y="${-P.h / 2}" width="${P.w}" height="${P.h}" rx="7" fill="${INK}"/>
      <rect x="${-P.w / 2 + 2.6}" y="${-P.h / 2 + 2.6}" width="${P.w - 5.2}" height="${P.h - 5.2}" rx="5" fill="url(#${g})"/>
      ${lit ? `<path d="M-3.6 -5.6L5.4 0L-3.6 5.6Z" fill="#fff" stroke="#fff" stroke-width="2.2" stroke-linejoin="round"/>` : ''}
      <rect x="-4" y="${-P.h / 2 + 4.4}" width="8" height="2" rx="1" fill="${INK}" opacity=".55"/>
    </g>`;
}
export function phoneTop(pose) {
  const P = PHONE[pose], a = (P.rot * Math.PI) / 180;
  return [f(P.x + Math.sin(a) * (P.h / 2 + 1)), f(P.y - Math.cos(a) * (P.h / 2 + 1))];
}

// ------------------------------------------------------------------ beam (reference implementation)
// Ribbon of light: centre line = quadratic Bézier from the phone top, leaving along the phone's axis and
// curving to the upper right; width grows (projection) and breathes with a slow twist; square pixels ride
// along it.  t0..t1 = visible part (0..1), phase = twist / pixel phase (animation), k = intensity.
export function beamGeom(from, to, dirDeg = 14) {
  const [x0, y0] = from, [x1, y1] = to;
  const d = Math.hypot(x1 - x0, y1 - y0), a = (dirDeg * Math.PI) / 180;
  const c = [x0 + Math.sin(a) * d * 0.55, y0 - Math.cos(a) * d * 0.55];
  const at = (t) => {
    const u = 1 - t;
    return [u * u * x0 + 2 * u * t * c[0] + t * t * x1, u * u * y0 + 2 * u * t * c[1] + t * t * y1];
  };
  const tan = (t) => {
    const dx = 2 * (1 - t) * (c[0] - x0) + 2 * t * (x1 - c[0]), dy = 2 * (1 - t) * (c[1] - y0) + 2 * t * (y1 - c[1]);
    const l = Math.hypot(dx, dy) || 1;
    return [dx / l, dy / l];
  };
  return { at, tan };
}
export function beamSvg(from, to, accent, o = {}) {
  const { at, tan } = beamGeom(from, to, o.dir ?? 14);
  const w0 = o.w0 ?? 13, w1 = o.w1 ?? 24, phase = o.phase ?? 0.3, k = o.k ?? 1;
  const N = 40;
  const ribbon = (scale, t0 = 0, t1 = 1) => {
    const L = [], R = [];
    for (let i = 0; i <= N; i++) {
      const t = t0 + ((t1 - t0) * i) / N;
      const [x, y] = at(t), [tx, ty] = tan(t);
      const twist = 0.84 + 0.16 * Math.cos(2 * Math.PI * (t * 1.1 - phase));
      const w = (w0 + (w1 - w0) * t) * twist * scale * 0.5;
      L.push([x - ty * w, y + tx * w]);
      R.push([x + ty * w, y - tx * w]);
    }
    const pts = [...L, ...R.reverse()];
    return 'M' + pts.map(([x, y]) => `${f(x)} ${f(y)}`).join('L') + 'Z';
  };
  const g1 = id('bg'), g2 = id('bc'), bl = id('bb');
  const [ax, ay] = at(0), [bx, by] = at(1);
  let s = `<defs>
    <linearGradient id="${g1}" gradientUnits="userSpaceOnUse" x1="${ax}" y1="${ay}" x2="${bx}" y2="${by}"><stop offset="0" stop-color="${accent}" stop-opacity="${0.95 * k}"/><stop offset=".7" stop-color="${accent}" stop-opacity="${0.7 * k}"/><stop offset="1" stop-color="${accent}" stop-opacity="0"/></linearGradient>
    <linearGradient id="${g2}" gradientUnits="userSpaceOnUse" x1="${ax}" y1="${ay}" x2="${bx}" y2="${by}"><stop offset="0" stop-color="#fff" stop-opacity="${0.95 * k}"/><stop offset=".55" stop-color="#fff" stop-opacity="${0.5 * k}"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
    <filter id="${bl}" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="5"/></filter></defs>
    <path d="${ribbon(2.0)}" fill="url(#${g1})" opacity=".45" filter="url(#${bl})"/>
    <path d="${ribbon(1)}" fill="url(#${g1})"/>
    <path d="${ribbon(0.4)}" fill="url(#${g2})"/>`;
  // square pixel sparkles: axis-aligned, a few sizes, white + accent + light accent
  const px = [[0.16, 0.9, 3.4, 0], [0.27, -1.0, 2.6, 1], [0.38, 1.25, 4.4, 2], [0.5, -1.35, 3.2, 0], [0.6, 0.6, 2.4, 1], [0.7, 1.55, 5, 2], [0.82, -1.2, 3.6, 0], [0.93, 0.9, 2.6, 1]];
  for (const [t0, side, sz, ci] of px) {
    const t = (t0 + (o.drift ?? 0)) % 1;
    const [x, y] = at(t), [tx, ty] = tan(t);
    const w = (w0 + (w1 - w0) * t) * 0.5;
    const cx = x + -ty * w * side, cy = y + tx * w * side;
    const col = ci === 0 ? '#ffffff' : ci === 1 ? accent : mix(accent, '#ffffff', 0.5);
    const a = Math.min(1, 1.6 * Math.sin(Math.PI * t)) * k;
    s += `<rect x="${f(cx - sz / 2)}" y="${f(cy - sz / 2)}" width="${sz}" height="${sz}" rx=".5" fill="${col}" opacity="${f(a)}"/>`;
  }
  return s;
}

// ------------------------------------------------------------------ extras (hearts / zzz / pop lines) — sheets only
const heart = (x, y, s, fill, rot = 0, op = 1) =>
  `<path transform="translate(${x},${y}) rotate(${rot}) scale(${s})" d="M0 3.2C-1.4 1.4-5 -.4-5 -3.1-5 -5.2-3.2 -6.4-1.6 -6.1-.8 -6 -.3 -5.4 0 -4.8.3 -5.4.8 -6 1.6 -6.1 3.2 -6.4 5 -5.2 5 -3.1 5 -.4 1.4 1.4 0 3.2Z" fill="${fill}" opacity="${op}"/>`;
const zzz = (x, y, s, fill, n = 3) => {
  const z = (dx, dy, k) => `<path d="M${f(x + dx)} ${f(y + dy)}h${f(7 * k)}l${f(-7 * k)} ${f(7 * k)}h${f(7 * k)}" fill="none" stroke="${fill}" stroke-width="${f(2.8 * k)}" stroke-linecap="round" stroke-linejoin="round"/>`;
  return [z(0, 10, s), z(11 * s, 0, s * 0.8), z(20 * s, -8, s * 0.62)].slice(0, n).join('');
};
function extras(mood, acc) {
  if (mood === 'happy') return heart(78, 56, 2.0, acc, -14) + heart(196, 44, 1.6, acc, 12) + heart(98, 32, 1.1, mix(acc, '#ffffff', 0.35), -6, 0.85);
  if (mood === 'surprised') return [[84, 50, 74, 36], [102, 40, 98, 25], [166, 40, 172, 25], [184, 50, 196, 38]].map(([a, b, c, d]) => stroke(`M${a} ${b}L${c} ${d}`, 3.6, '#ffffff')).join('');
  if (mood === 'sleepy') return zzz(192, 52, 0.95, '#ffffff', 1);
  if (mood === 'asleep') return zzz(186, 48, 1.2, '#ffffff');
  return '';
}

// ------------------------------------------------------------------ squash & stretch (app does it live)
export const SQUASH = { idle: [1, 1], blink: [1, 1], happy: [0.95, 1.06], connecting: [1.02, 0.98], surprised: [0.93, 1.08], sleepy: [1.04, 0.96], asleep: [1.1, 0.9] };

// ------------------------------------------------------------------ layer entry points
const tilt = (inner) => `<g transform="rotate(${TILT} ${PIVOT[0]} ${PIVOT[1]})">${inner}</g>`;
export const layer = {
  cloud: () => tilt(cloudLayer(id('L'))),
  rim: () => tilt(rimLayer(id('L'))),
  face: (m) => tilt(face(m, id('L'))),
  hand: (pose) => tilt(handLayer(id('L'), pose)),
};

// Full composition (SVG <g>) for sheets.  opts: mood, accent, beam (bool | {phase, drift, k}), beamTo, phone,
// shadow, fx (hearts/zzz), squash.
export function hero(o = {}) {
  const mood = o.mood || 'idle', acc = o.accent || THEMES.Sakura.accent, pose = poseOf(mood);
  const p = id('H');
  const lit = mood !== 'asleep';
  const [sx, sy] = o.squash === false ? [1, 1] : SQUASH[mood];
  const body = `<defs>${bodyDefs(p)}</defs>` + cloudLayer(p) +
    `<g opacity="${lit ? (mood === 'sleepy' ? 0.55 : 0.85) : 0.15}">${rimLayer(p, acc)}</g>` + face(mood, p) +
    (o.phone === false ? '' : phoneSvg(pose, acc, lit)) + handLayer(p, pose);
  const from = phoneTop(pose);
  const beamOn = o.beam ?? mood === 'connecting';
  const beam = beamOn ? beamSvg(from, o.beamTo || [262, 40], acc, typeof o.beam === 'object' ? o.beam : {}) : '';
  const shadow = o.shadow === false ? '' : `<ellipse cx="135" cy="${o.shadowY ?? 194}" rx="60" ry="8" fill="#000" opacity="${lit ? 0.28 : 0.2}" filter="url(#${p}b6)"/>`;
  const sq = `<g transform="translate(${PIVOT[0]},${PIVOT[1]}) scale(${sx},${sy}) translate(${-PIVOT[0]},${-PIVOT[1]})">${body}</g>`;
  return `<g>${shadow}${tilt(sq)}${beam ? tilt(beam) : ''}${o.fx === false ? '' : extras(mood, acc)}</g>`;
}

// mini "big screen" for the master
export function monitor(x, y, acc) {
  return `<g transform="translate(${x},${y})">
    <rect x="-8" y="26" width="16" height="9" fill="#4A3A42"/><rect x="-16" y="34" width="32" height="5" rx="2.5" fill="#4A3A42"/>
    <rect x="-34" y="-22" width="68" height="48" rx="8" fill="#4A3A42"/>
    <rect x="-28" y="-16" width="56" height="36" rx="4" fill="${acc}"/>
    <rect x="-28" y="-16" width="56" height="12" rx="4" fill="#fff" opacity=".25"/>
    <path d="M-4 -6L6 2L-4 10Z" fill="#fff" stroke="#fff" stroke-width="2.5" stroke-linejoin="round"/></g>`;
}
