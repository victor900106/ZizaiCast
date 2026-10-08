// 投投 app icon — Direction C construction (grid-built cloud: two circles + one pill, two flat tones,
// cast-signal arcs) in the A palette, on a pink-gradient rounded tile (reads on dark AND light taskbars;
// a bare white cloud vanishes on the light one).  Every size is hand-tuned in its own pixel grid.
//   iconSvg(size, { accent, tile })  -> <svg> of exactly size x size px
import { mix } from './hero.mjs';

const INK = '#3A2830';
const f = (n) => +(+n).toFixed(3);

// Design grid (256): C cloud = big puff + small puff + pill base.
const D256 = {
  tile: { inset: 6, r: 54 },
  puffs: [[108, 118, 52], [168, 136, 35]],
  base: [44, 134, 168, 54, 27],   // x y w h r
  eyes: [[98, 160], [148, 160]], er: [8, 11.5], hl: 3.6,
  blush: [[78, 176, 11, 6.5], [168, 176, 11, 6.5]],
  mouth: [123, 170, 6.5, 4.2],    // centre x, y, half width, depth
  shade: [8, 11],                 // light layer offset (two flat tones -> shade crescent bottom-right)
  sig: { x: 198, y: 70, dot: 9, arcs: [22, 36], w: 8 },
};
// Hand-tuned small sizes (pixel units).  null field = omitted at that size.
const SMALL = {
  64: { tile: { inset: 1.5, r: 13.5 }, puffs: [[27, 30, 13], [42, 34, 8.8]], base: [11, 33.5, 42, 14, 7], eyes: [[25, 40], [37, 40]], er: [2, 2.8], hl: 0.9,
    blush: [[19.5, 44, 2.8, 1.7], [42, 44, 2.8, 1.7]], mouth: [30.8, 42.6, 1.7, 1.2], shade: [2, 2.6], sig: { x: 49, y: 18, dot: 2.4, arcs: [6, 9.8], w: 2.2 } },
  48: { tile: { inset: 1, r: 10.5 }, puffs: [[20.5, 22.5, 10], [31.5, 25.5, 6.6]], base: [8, 25, 32, 11, 5.5], eyes: [[18.5, 30], [28.5, 30]], er: [1.5, 2], hl: 0.7,
    blush: [[14.5, 33, 2.1, 1.3], [32, 33, 2.1, 1.3]], mouth: null, shade: [1.5, 2], sig: { x: 37, y: 13.5, dot: 1.9, arcs: [4.6, 7.4], w: 1.7 } },
  40: { tile: { inset: 0.5, r: 9 }, puffs: [[17, 19, 8.4], [26.5, 21.5, 5.6]], base: [6.5, 21, 27, 9, 4.5], eyes: [[15, 24], [23, 24]], eyeRect: [2, 3], er: [1.45, 2], hl: 0.7,
    blush: null, mouth: null, shade: [1.2, 1.7], sig: { x: 31, y: 11, dot: 1.6, arcs: [3.9], w: 1.5 } },
  32: { tile: { inset: 0, r: 7 }, puffs: [[12.8, 14.4, 7.2], [21.8, 17.2, 5]], base: [3.5, 16.5, 25, 9.5, 4.75],
    eyes: [[10, 19], [18, 19]], eyeRect: [2, 3], blushRect: [[6, 23, 2, 1], [22, 23, 2, 1]], mouth: null, shade: [0.8, 1.1],
    sig: { x: 26.5, y: 7.5, dot: 1.6, arcs: [4], w: 1.4 } },
  24: { tile: { inset: 0, r: 5.5 }, puffs: [[9.6, 11, 5.4], [16.4, 13, 3.8]], base: [2.5, 12.5, 19, 7, 3.5],
    eyes: [[7, 15], [13, 15]], eyeRect: [2, 2], mouth: null, shade: null, sig: { x: 20, y: 5.5, dot: 1.3, arcs: [3.2], w: 1.1 } },
  20: { tile: { inset: 0, r: 4.5 }, puffs: [[8, 9.4, 4.6], [13.6, 11, 3.2]], base: [2, 10.4, 16, 6, 3],
    eyes: [[6, 12], [11, 12]], eyeRect: [1, 2], mouth: null, shade: null, sig: { x: 16.5, y: 4.8, dot: 1.25, arcs: null } },
  16: { tile: { inset: 0, r: 3.5 }, puffs: [[6.3, 7.6, 3.7], [10.9, 8.8, 2.6]], base: [1.5, 8.2, 13, 5, 2.5],
    eyes: [[5, 10], [9, 10]], eyeRect: [1, 2], mouth: null, shade: null, sig: null },
};

export function iconSvg(size, o = {}) {
  const acc = o.accent || '#F5A7A7';
  const G = size >= 128 ? scaleDesign(D256, size / 256) : SMALL[size] || scaleDesign(D256, size / 256);
  const tileOn = o.tile !== false;
  const t1 = mix(acc, '#ffffff', 0.22), t2 = mix(acc, '#8a3f50', 0.3);
  const light = '#FFFFFF', shade = '#E8DCE8';
  const shapes = (attrs = '') => G.puffs.map(([x, y, r]) => `<circle cx="${f(x)}" cy="${f(y)}" r="${f(r)}" ${attrs}/>`).join('') +
    `<rect x="${f(G.base[0])}" y="${f(G.base[1])}" width="${f(G.base[2])}" height="${f(G.base[3])}" rx="${f(G.base[4])}" ${attrs}/>`;
  let s = '';
  if (tileOn) {
    const i = G.tile.inset;
    s += `<defs><linearGradient id="t" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${t1}"/><stop offset="1" stop-color="${t2}"/></linearGradient></defs>
      <rect x="${i}" y="${i}" width="${size - 2 * i}" height="${size - 2 * i}" rx="${G.tile.r}" fill="url(#t)"/>`;
    // soft top sheen on big sizes
  }
  // signal (dot + quarter arcs), white on the tile, accent without it
  const sc = tileOn ? '#FFFFFF' : acc;
  if (G.sig) {
    const { x, y, dot, arcs, w } = G.sig;
    s += `<circle cx="${x}" cy="${y}" r="${dot}" fill="${sc}"/>`;
    for (const [k, r] of (arcs || []).entries())
      s += `<path d="M${f(x)} ${f(y - r)}A${r} ${r} 0 0 1 ${f(x + r)} ${f(y)}" fill="none" stroke="${sc}" stroke-width="${w}" stroke-linecap="round" opacity="${1 - k * 0.22}"/>`;
  }
  // cloud: two flat tones (shade silhouette + light layer shifted up-left, clipped)
  if (G.shade) {
    s += `<clipPath id="c">${shapes()}</clipPath><g fill="${shade}">${shapes()}</g>
      <g clip-path="url(#c)"><g fill="${light}" transform="translate(${-G.shade[0]},${-G.shade[1]})">${shapes()}</g></g>`;
  } else {
    s += `<g fill="${light}">${shapes()}</g>`;
  }
  if (G.blush) s += G.blush.map(([x, y, rx, ry]) => `<ellipse cx="${f(x)}" cy="${f(y)}" rx="${f(rx)}" ry="${f(ry)}" fill="#F8A9B9"/>`).join('');
  if (G.blushRect) s += G.blushRect.map(([x, y, w, h]) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="#F6A3B4"/>`).join('');
  if (G.eyeRect) {
    s += G.eyes.map(([x, y]) => `<rect x="${x}" y="${y}" width="${G.eyeRect[0]}" height="${G.eyeRect[1]}" fill="${INK}"/>`).join('');
  } else {
    s += G.eyes.map(([x, y]) => `<ellipse cx="${f(x)}" cy="${f(y)}" rx="${f(G.er[0])}" ry="${f(G.er[1])}" fill="${INK}"/>` +
      `<circle cx="${f(x - G.er[0] * 0.3)}" cy="${f(y - G.er[1] * 0.38)}" r="${f(G.hl)}" fill="#fff"/>`).join('');
  }
  if (G.mouth) {
    const [x, y, w, d] = G.mouth;
    s += `<path d="M${f(x - w)} ${f(y)}Q${f(x)} ${f(y + d * 1.6)} ${f(x + w)} ${f(y)}" fill="none" stroke="${INK}" stroke-width="${f(Math.max(1.1, size / 60))}" stroke-linecap="round"/>`;
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">${s}</svg>`;
}

function scaleDesign(D, k) {
  const m = (v) => v * k;
  return {
    tile: { inset: m(D.tile.inset), r: m(D.tile.r) },
    puffs: D.puffs.map(([x, y, r]) => [m(x), m(y), m(r)]),
    base: D.base.map(m),
    eyes: D.eyes.map(([x, y]) => [m(x), m(y)]), er: D.er.map(m), hl: m(D.hl),
    blush: D.blush.map((b) => b.map(m)), mouth: D.mouth.map(m), shade: D.shade.map(m),
    sig: { x: m(D.sig.x), y: m(D.sig.y), dot: m(D.sig.dot), arcs: D.sig.arcs.map(m), w: m(D.sig.w) },
  };
}
export const ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 256];
