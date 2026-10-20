/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phone trackpad gestures for 自在投影 遠端操控 (remote/web).
 *
 * Ported from MoonlightWeb, frontend/js/ui/StreamViewTouch.js
 *   https://github.com/linckosz/moonlight-web
 *   MoonlightWeb — browser-based Sunshine/GameStream client.
 *   Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *   Licensed under the GNU General Public License v3 or later.
 *
 * Changes for 自在投影 (2026-10-20): turned the StreamView mixin into a
 * standalone class talking to a small `sink` (move / button / wheel /
 * keyboard); dropped the TV-browser cursor, touch-screen mode, client-drawn
 * cursor, three-finger desktop switch and the shortcuts slide; cursor
 * sensitivity comes from the page (scaled to the PC's desktop size);
 * multi-finger taps are judged by centroid travel, not by the distance of
 * whichever finger lifted last.
 * 0.8.0: the finger speed goes to sink.sensitivity(speed) (the page applies
 * an acceleration curve); a pinch also pans with the fingers' centre (zoom
 * and move in one gesture); "direct" mode: one finger points / clicks where
 * it touches (sink.toVideo / sink.abs), hold = right click, touch-and-move =
 * drag; two and three fingers work as in the trackpad mode; zoomLocked keeps
 * the picture still (two fingers only scroll).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see
 * <https://www.gnu.org/licenses/>.
 *
 * Gestures (laptop-trackpad model, as in MoonlightWeb; mode = 'trackpad'):
 *   1 finger drag        relative cursor move (fractions carried, no lost motion)
 *   1 finger tap         left click
 *   hold still 450 ms    left button down -> drag -> lift = button up
 *   2 finger tap         right click
 *   2 finger drag        scroll both axes (natural direction) + flick momentum
 *   2 finger pinch       zoom the picture locally (1x..8x)
 *   3 finger drag        pan the zoomed picture
 *   3 finger tap         show / hide the keyboard
 */
'use strict';

class RemoteTrackpad {
  /**
   * @param {HTMLElement} surface  element receiving the touches
   * @param {HTMLVideoElement} view element zoomed / panned locally
   * @param {{move(dx:number,dy:number):void, button(b:number,down:boolean):void,
   *          wheel(dx:number,dy:number):void, keyboard():void, sensitivity(speed:number):number,
   *          toVideo(clientX,clientY):{x,y}|null, abs(x:number,y:number):void, zoomed?():void}} sink
   *        button: 0 left, 2 right. wheel: Windows units (120 = one notch).
   *        sensitivity: desktop px per CSS px at a finger speed (CSS px / ms).
   */
  constructor(surface, view, sink) {
    this.surface = surface;
    this.view = view;
    this.sink = sink;
    // Tuning (MoonlightWeb defaults).
    this.tapThreshold = 10;     // px max travel for a tap
    this.tapTimeThreshold = 300; // ms max duration for a tap
    this.longPressMs = 450;     // still hold before a drag engages
    this.scrollScale = 4;       // finger px -> wheel units
    this.zoom = 1; this.panX = 0; this.panY = 0;
    this.mode = 'trackpad';     // or 'direct'
    this.zoomLocked = false;    // true: no pinch zoom / pan (the picture stays put)

    this.directHoldMs = 550;    // direct mode: hold still = right click
    this._reset();
    this._scrollSamples = []; this._panSamples = [];
    this._scrollRaf = null; this._panRaf = null; this._longPress = null;
    const opt = {passive: false};
    surface.addEventListener('touchstart', e => this.start(e), opt);
    surface.addEventListener('touchmove', e => this.move(e), opt);
    surface.addEventListener('touchend', e => this.end(e), opt);
    surface.addEventListener('touchcancel', e => this.end(e), opt);
  }

  _reset() {
    this.active = false; this.dragging = false; this.moved = false;
    this.maxFingers = 0; this.fingers = 0; this.lastCount = 0;
    this.accX = 0; this.accY = 0; this.scrollAcc = 0; this.scrollAccX = 0;
    this.pinchDist = 0; this.pinchCx = null; this.pinchCy = null; this.twoMode = null;
    this.lastMoveT = 0; this.speed = 0; this.directDone = false;
  }

  _direct() { return this.mode === 'direct'; }

  start(e) {
    e.preventDefault();
    const n = e.touches.length;
    this._stopScrollMomentum(); this._stopPanMomentum();
    if (!this.active) {
      this._reset();
      this.active = true;
      const t = e.touches[0];
      this.startX = this.lastX = t.clientX; this.startY = this.lastY = t.clientY;
      this.startTime = performance.now();
      this.lastCount = n;
      this._scrollSamples.length = 0; this._panSamples.length = 0;
    }
    this.fingers = n;
    this.maxFingers = Math.max(this.maxFingers, n);
    if (n === 1 && this._direct()) {
      // Direct: the pointer goes where the finger is; hold = right click.
      const t = e.touches[0];
      const p = this.sink.toVideo(t.clientX, t.clientY);
      if (p) this.sink.abs(p.x, p.y);
      this._clearLongPress();
      this._longPress = setTimeout(() => {
        if (this.active && !this.moved && this.fingers === 1 && this.maxFingers === 1 && !this.dragging) {
          this.directDone = true;
          this.sink.button(2, true); this.sink.button(2, false);
          if (navigator.vibrate) navigator.vibrate(15);
        }
      }, this.directHoldMs);
    } else if (n === 1) {
      // Hold still -> grab the left button (drag a window, select text). Right
      // click is the two-finger tap, which frees the long press for this.
      this._clearLongPress();
      this._longPress = setTimeout(() => {
        if (this.active && !this.moved && this.fingers === 1 && this.maxFingers === 1 && !this.dragging) {
          this.dragging = true;
          this.sink.button(0, true);
          if (navigator.vibrate) navigator.vibrate(15);
        }
      }, this.longPressMs);
    } else {
      this._clearLongPress();
      // Direct mode: a second finger ends a one-finger drag (scroll / zoom now).
      if (this._direct() && this.dragging) { this.dragging = false; this.sink.button(0, false); }
      this._seedMulti(e.touches);
    }
  }

  _seedMulti(touches) {
    const n = touches.length;
    if (n < 2) return;
    this.pinchDist = Math.hypot(touches[0].clientX - touches[1].clientX, touches[0].clientY - touches[1].clientY);
    let cx = 0, cy = 0;
    for (let i = 0; i < n; i++) { cx += touches[i].clientX; cy += touches[i].clientY; }
    this.pinchCx = this.originCx = cx / n;
    this.pinchCy = this.originCy = cy / n;
    this.originDist = this.pinchDist;
    this.twoMode = null;
  }

  _movedBeyond(cx, cy, dist, tol) {
    const c = Math.hypot(cx - this.originCx, cy - this.originCy);
    const d = dist != null ? Math.abs(dist - this.originDist) : 0;
    return c > tol || d > tol;
  }

  _emitScroll(dCx, dCy, sx, sy) {
    // Content follows the finger: finger up -> wheel negative (scroll down).
    this.scrollAcc += dCy * this.scrollScale;
    const wy = Math.trunc(this.scrollAcc);
    this.scrollAccX += -dCx * this.scrollScale;
    const wx = Math.trunc(this.scrollAccX);
    if (wy || wx) { this.sink.wheel(wx, wy); this.scrollAcc -= wy; this.scrollAccX -= wx; }
    const now = performance.now();
    this._scrollSamples.push({t: now, x: sx, y: sy});
    while (this._scrollSamples.length > 2 && now - this._scrollSamples[0].t > 120) this._scrollSamples.shift();
  }

  move(e) {
    e.preventDefault();
    if (!this.active) return;
    const n = e.touches.length;
    this.fingers = n;
    // Finger count changed: reseed and drop this frame's delta (no jump).
    if (n !== this.lastCount) {
      this.lastCount = n;
      if (n === 1) { this.lastX = e.touches[0].clientX; this.lastY = e.touches[0].clientY; this.accX = this.accY = 0; }
      else this._seedMulti(e.touches);
      return;
    }
    if (n === 1 && this._direct()) {
      const t = e.touches[0];
      if (!this.moved && Math.hypot(t.clientX - this.startX, t.clientY - this.startY) > this.tapThreshold) {
        this.moved = true;
        this._clearLongPress();
        if (this.maxFingers === 1 && !this.directDone) { this.dragging = true; this.sink.button(0, true); }
      }
      if (this.moved && this.maxFingers === 1) {
        const p = this.sink.toVideo(t.clientX, t.clientY);
        if (p) this.sink.abs(p.x, p.y);
      }
      this.lastX = t.clientX; this.lastY = t.clientY;
    } else if (n === 1) {
      const t = e.touches[0];
      if (Math.hypot(t.clientX - this.startX, t.clientY - this.startY) > this.tapThreshold) {
        this.moved = true;
        if (!this.dragging) this._clearLongPress();
      }
      // Finger speed (CSS px / ms, smoothed) for the page's acceleration curve.
      const now = performance.now(), dt = Math.max(1, now - (this.lastMoveT || now - 16));
      const inst = Math.hypot(t.clientX - this.lastX, t.clientY - this.lastY) / dt;
      this.speed = this.lastMoveT ? this.speed * 0.6 + inst * 0.4 : inst;
      this.lastMoveT = now;
      // Slower when zoomed in, for finer aim (MoonlightWeb: -0.1 per zoom step).
      const sens = this.sink.sensitivity(this.speed) * Math.max(0.1, 1 - 0.1 * (this.zoom - 1));
      // Fractions are carried: slow drags still move (sub-pixel deltas per event).
      this.accX += (t.clientX - this.lastX) * sens;
      this.accY += (t.clientY - this.lastY) * sens;
      const dx = Math.trunc(this.accX), dy = Math.trunc(this.accY);
      if (dx || dy) { this.accX -= dx; this.accY -= dy; this.sink.move(dx, dy); }
      this.lastX = t.clientX; this.lastY = t.clientY;
    } else if (n === 2) {
      this._clearLongPress();
      const a = e.touches[0], b = e.touches[1];
      const cx = (a.clientX + b.clientX) / 2, cy = (a.clientY + b.clientY) / 2;
      const dist = Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY);
      if (this._movedBeyond(cx, cy, dist, 12)) this.moved = true;
      const dDist = this.pinchDist > 0 ? dist - this.pinchDist : 0;
      const dCx = this.pinchCx != null ? cx - this.pinchCx : 0;
      const dCy = this.pinchCy != null ? cy - this.pinchCy : 0;
      const drag = Math.hypot(dCx, dCy);
      // Lock to zoom OR scroll for the whole sequence, biased toward scroll.
      // zoomLocked (畫面鎖定): two fingers only ever scroll.
      if (this.twoMode == null && this.zoomLocked) this.twoMode = 'scroll';
      if (this.twoMode == null) {
        if (Math.abs(dDist) > 3 && Math.abs(dDist) >= drag * 1.6) this.twoMode = 'zoom';
        else if (drag > 1.5) this.twoMode = 'scroll';
      }
      if (this.twoMode === 'zoom') {
        const z = Math.min(8, Math.max(1, this.zoom * (dist / this.pinchDist)));
        const f = z / this.zoom;
        if (f !== 1) {
          const r = this.surface.getBoundingClientRect();
          const fx = cx - (r.left + r.width / 2), fy = cy - (r.top + r.height / 2);
          this.panX = fx * (1 - f) + f * this.panX;
          this.panY = fy * (1 - f) + f * this.panY;
        }
        // The picture also follows the fingers' centre: zoom and pan in one gesture.
        this.panX += dCx; this.panY += dCy;
        this.zoom = z;
        if (this.zoom <= 1.001) this.panX = this.panY = 0;
        this._applyZoom();
        if (this.sink.zoomed) this.sink.zoomed();
        this._scrollSamples.length = 0;
      } else if (this.twoMode === 'scroll' && drag > 0.1) {
        this._emitScroll(dCx, dCy, cx, cy);
      }
      this.pinchDist = dist; this.pinchCx = cx; this.pinchCy = cy;
    } else {
      this._clearLongPress();
      let cx = 0, cy = 0;
      for (let i = 0; i < n; i++) { cx += e.touches[i].clientX; cy += e.touches[i].clientY; }
      cx /= n; cy /= n;
      if (this._movedBeyond(cx, cy, null, 24)) this.moved = true;
      const dCx = this.pinchCx != null ? cx - this.pinchCx : 0;
      const dCy = this.pinchCy != null ? cy - this.pinchCy : 0;
      if (this.zoom > 1.01 && !this.zoomLocked) {
        this.panX += dCx; this.panY += dCy;
        this._applyZoom();
        const now = performance.now();
        this._panSamples.push({t: now, x: cx, y: cy});
        while (this._panSamples.length > 2 && now - this._panSamples[0].t > 120) this._panSamples.shift();
      }
      this.pinchCx = cx; this.pinchCy = cy;
    }
  }

  end(e) {
    e.preventDefault();
    this.fingers = e.touches.length;
    if (e.touches.length > 0) return;  // resolve once every finger is up
    this._clearLongPress();
    const elapsed = performance.now() - this.startTime;
    const t = e.changedTouches[0];
    const dist = t ? Math.hypot(t.clientX - this.startX, t.clientY - this.startY) : 0;
    // Multi-finger taps land less cleanly: looser tolerances.
    const distTol = this.maxFingers >= 3 ? 45 : this.maxFingers === 2 ? 28 : this.tapThreshold;
    const timeTol = this.maxFingers >= 3 ? 600 : this.maxFingers === 2 ? 450 : this._direct() ? this.directHoldMs : this.tapTimeThreshold;
    // Multi-finger: the finger lifted last is often not the first one, so its
    // distance to the first finger's start says nothing; the centroid
    // tolerance (12 / 24 px, tracked in move()) decides instead. (自在投影 fix;
    // MoonlightWeb compares changedTouches[0] for every tap.)
    const isTap = e.type !== 'touchcancel' && !this.moved && (this.maxFingers > 1 || dist < distTol) && elapsed < timeTol;
    if (this.dragging) {
      this.sink.button(0, false);
    } else if (isTap) {
      if (this.maxFingers >= 3) this.sink.keyboard();
      else if (this.maxFingers === 2) { this.sink.button(2, true); this.sink.button(2, false); }
      else if (!this.directDone) { this.sink.button(0, true); this.sink.button(0, false); }
    }

    if (!isTap && !this.dragging && this.maxFingers === 2) this._startScrollMomentum();
    if (!isTap && this.maxFingers >= 3 && this.zoom > 1.01) this._startPanMomentum();
    this._reset();
  }

  _clearLongPress() { if (this._longPress) { clearTimeout(this._longPress); this._longPress = null; } }

  // Pinch zoom / pan of the local picture; pan clamped so no black shows.
  _applyZoom() {
    const r = this.surface.getBoundingClientRect();
    const iw = this.view.videoWidth, ih = this.view.videoHeight;
    let w = r.width, h = r.height;
    if (iw > 0 && ih > 0) { const fit = Math.min(r.width / iw, r.height / ih); w = iw * fit; h = ih * fit; }
    const maxX = Math.max(0, (w * this.zoom - r.width) / 2), maxY = Math.max(0, (h * this.zoom - r.height) / 2);
    this.panX = Math.max(-maxX, Math.min(maxX, this.panX));
    this.panY = Math.max(-maxY, Math.min(maxY, this.panY));
    this.view.style.transform = this.zoom > 1.001 ? `translate(${this.panX}px, ${this.panY}px) scale(${this.zoom})` : '';
  }

  // Flick: keep scrolling with a decaying velocity (phone-like momentum).
  _startScrollMomentum() {
    this._stopScrollMomentum();
    const s = this._scrollSamples;
    if (s.length < 2) return;
    const a = s[0], b = s[s.length - 1], dt = b.t - a.t;
    if (dt <= 0 || performance.now() - b.t > 80) return;
    const clamp = v => Math.max(-400, Math.min(400, v));
    let v = clamp((b.y - a.y) / dt * 16.67 * this.scrollScale);
    let vx = clamp((a.x - b.x) / dt * 16.67 * this.scrollScale);
    if (Math.hypot(v, vx) < 1.5) return;
    let acc = 0, accX = 0;
    const step = () => {
      v *= 0.95; vx *= 0.95;
      if (Math.hypot(v, vx) < 0.4) { this._scrollRaf = null; return; }
      acc += v; accX += vx;
      const wy = Math.trunc(acc), wx = Math.trunc(accX);
      if (wy || wx) { this.sink.wheel(wx, wy); acc -= wy; accX -= wx; }
      this._scrollRaf = requestAnimationFrame(step);
    };
    this._scrollRaf = requestAnimationFrame(step);
  }
  _stopScrollMomentum() { if (this._scrollRaf != null) { cancelAnimationFrame(this._scrollRaf); this._scrollRaf = null; } }

  _startPanMomentum() {
    this._stopPanMomentum();
    const s = this._panSamples;
    if (s.length < 2) return;
    const a = s[0], b = s[s.length - 1], dt = b.t - a.t;
    if (dt <= 0 || performance.now() - b.t > 80) return;
    const clamp = v => Math.max(-90, Math.min(90, v));
    let vx = clamp((b.x - a.x) / dt * 16.67), vy = clamp((b.y - a.y) / dt * 16.67);
    if (Math.hypot(vx, vy) < 1.2) return;
    const friction = 0.955;
    let prev = performance.now();
    const step = () => {
      const now = performance.now(), frames = Math.min(4, (now - prev) / 16.67);
      prev = now;
      const decay = Math.pow(friction, frames);
      if (Math.hypot(vx, vy) * decay < 0.15 || this.zoom <= 1.01) { this._panRaf = null; return; }
      const travel = (decay - 1) / Math.log(friction);
      const wantX = this.panX + vx * travel, wantY = this.panY + vy * travel;
      this.panX = wantX; this.panY = wantY;
      this._applyZoom();
      if (Math.abs(this.panX - wantX) > 0.01) vx = 0;
      if (Math.abs(this.panY - wantY) > 0.01) vy = 0;
      if (!vx && !vy) { this._panRaf = null; return; }
      vx *= decay; vy *= decay;
      this._panRaf = requestAnimationFrame(step);
    };
    this._panRaf = requestAnimationFrame(step);
  }
  _stopPanMomentum() { if (this._panRaf != null) { cancelAnimationFrame(this._panRaf); this._panRaf = null; } }
}
