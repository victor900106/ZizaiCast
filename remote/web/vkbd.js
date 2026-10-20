/*
 * SPDX-License-Identifier: MPL-2.0
 *
 * Phone on-screen keyboard -> key events, for 自在投影 遠端操控 (remote/web).
 *
 * Ported from noVNC, app/ui.js (keyboardinputReset / keyInput /
 * showVirtualKeyboard / hideVirtualKeyboard):
 *   https://github.com/novnc/noVNC
 *   Copyright (C) 2022 The noVNC authors
 *   Licensed under the Mozilla Public License 2.0 (MPL-2.0),
 *   https://mozilla.org/MPL/2.0/
 * This file stays under MPL-2.0 (file-level copyleft); it is combined with
 * the GPL-3 rest of 自在投影 as a "Larger Work" (MPL-2.0 section 3.3).
 *
 * Changes for 自在投影 (2026-10-20): standalone class instead of the UI
 * singleton; sends text / named keys to a sink instead of RFB keysyms;
 * waits for compositionend (Chinese / Japanese IME) before diffing; sticky
 * modifiers (Ctrl / Alt / Shift / Win) applied to the next key, like noVNC's
 * extra-keys bar.
 *
 * Why the diffing: mobile on-screen keyboards often send keydown with
 * keyCode 229 / no usable key, so the reliable signal is how the text of a
 * hidden input changed. The input always keeps some filler text so that
 * Backspace has something to delete (and is therefore seen).
 */
'use strict';

class RemoteVirtualKeyboard {
  /**
   * @param {HTMLInputElement} input hidden / visible text input
   * @param {{text(s:string):void, key(name:string):void, combo(keys:string[]):void}} sink
   */
  constructor(input, sink) {
    this.input = input;
    this.sink = sink;
    this.fillLen = 100;  // noVNC defaultKeyboardinputLen
    this.last = '';
    this.composing = false;
    this.mods = new Set();  // sticky modifiers for the next key: Control Alt Shift Meta
    this.onModsChange = null;
    input.addEventListener('compositionstart', () => { this.composing = true; });
    input.addEventListener('compositionend', e => { this.composing = false; this._onInput(e); });
    input.addEventListener('input', e => { if (!this.composing && !e.isComposing) this._onInput(e); });
    input.addEventListener('keydown', e => this._onKeyDown(e));
    this.reset();
  }

  reset() {
    this.input.value = new Array(this.fillLen).join('_');
    this.last = this.input.value;
  }

  show() {
    if (document.activeElement === this.input) return;
    this.input.focus();
    try { const l = this.input.value.length; this.input.setSelectionRange(l, l); } catch { /* not supported */ }
  }

  hide() { if (document.activeElement === this.input) this.input.blur(); }
  toggle() { document.activeElement === this.input ? this.hide() : this.show(); }

  toggleMod(m) {
    this.mods.has(m) ? this.mods.delete(m) : this.mods.add(m);
    if (this.onModsChange) this.onModsChange(this.mods);
  }

  // A named key (Enter, Tab, ArrowLeft, F5, ...) or one character, with the
  // sticky modifiers if any (then released).
  press(name) {
    if (this.mods.size) {
      this.sink.combo([...this.mods, name]);
      this.mods.clear();
      if (this.onModsChange) this.onModsChange(this.mods);
    } else if (name.length === 1) {
      this.sink.text(name);
    } else {
      this.sink.key(name);
    }
  }

  _onKeyDown(e) {
    if (this.composing || e.isComposing || e.keyCode === 229) return;
    // Keys that do not change the text (hardware / some soft keyboards).
    const named = ['Enter', 'Tab', 'Escape', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End', 'Delete'];
    if (named.includes(e.key)) { e.preventDefault(); this.press(e.key); }
  }

  // noVNC keyInput: compare the new value with the last one -> backspaces
  // for what disappeared, characters for what was added (handles
  // autocorrect replacing a word too).
  _onInput(e) {
    const t = this.input;
    const newValue = t.value;
    if (!this.last) this.reset();
    const oldValue = this.last;
    let newLen;
    try { newLen = Math.max(t.selectionStart, newValue.length); } catch { newLen = newValue.length; }
    const oldLen = oldValue.length;
    let inputs = newLen - oldLen;
    let backspaces = inputs < 0 ? -inputs : 0;
    for (let i = 0; i < Math.min(oldLen, newLen); i++) {
      if (newValue.charAt(i) !== oldValue.charAt(i)) { inputs = newLen - i; backspaces = oldLen - i; break; }
    }
    for (let i = 0; i < backspaces; i++) this.press('Backspace');
    const added = newValue.substring(newLen - inputs, newLen);
    if (added) {
      if (this.mods.size) for (const ch of added) this.press(ch);
      else this.sink.text(added);
    }
    if (newLen > 2 * this.fillLen) {
      this.reset();
    } else if (newLen < 1) {
      // Keep text for Backspace to act on; Android needs the blur/focus dance.
      this.reset();
      t.blur();
      setTimeout(() => t.focus(), 0);
    } else {
      this.last = newValue;
    }
  }
}
