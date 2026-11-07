// KeyboardEvent.code is a physical key: translate to the native protocol's USB HID usage.
const keys = {Enter:40, Escape:41, Backspace:42, Tab:43, Space:44, Minus:45, Equal:46,
  BracketLeft:47, BracketRight:48, Backslash:49, Semicolon:51, Quote:52, Backquote:53,
  Comma:54, Period:55, Slash:56, CapsLock:57, PrintScreen:70, ScrollLock:71, Pause:72,
  Insert:73, Home:74, PageUp:75, Delete:76, End:77, PageDown:78, ArrowRight:79,
  ArrowLeft:80, ArrowDown:81, ArrowUp:82, NumLock:83, NumpadDivide:84, NumpadMultiply:85,
  NumpadSubtract:86, NumpadAdd:87, NumpadEnter:88, NumpadDecimal:99, IntlBackslash:100,
  ContextMenu:101, NumpadEqual:103, IntlRo:135, KanaMode:136, IntlYen:137,
  Convert:138, NonConvert:139, Lang1:144, Lang2:145,
  ControlLeft:224, ShiftLeft:225, AltLeft:226, MetaLeft:227,
  ControlRight:228, ShiftRight:229, AltRight:230, MetaRight:231};
for (let i = 0; i < 26; i++) keys['Key' + String.fromCharCode(65 + i)] = 4 + i;
for (let i = 1; i <= 9; i++) { keys['Digit' + i] = 29 + i; keys['Numpad' + i] = 88 + i; }
keys.Digit0 = 39; keys.Numpad0 = 98;
for (let i = 1; i <= 12; i++) keys['F' + i] = 57 + i;
const chords = {'ctrl-space':[224,44], 'ctrl-alt-delete':[224,226,76],
  'alt-tab':[226,43], escape:[41], super:[227]};
// Cursor serials are uint64 strings; Number would lose ordering above 2^53.
function cursorSerial(value) {
  if (typeof value !== 'string' || !/^[0-9]{1,20}$/.test(value)) return null;
  const serial = BigInt(value);
  return serial <= 18446744073709551615n ? serial : null;
}
export class RemoteInput {
  constructor(video, canvas, send) {
    Object.assign(this, {video, canvas, send}); this.sequence = 0; this.enabled = false; this.epoch = '';
    this.cursorSequence = 0n; this.shapeSequence = 0n; this.lastPointerSequence = 0n;
    this.events = new AbortController(); const opts = {signal:this.events.signal};
    video.addEventListener('keydown', e => this.key(e, true), opts);
    video.addEventListener('keyup', e => this.key(e, false), opts);
    video.addEventListener('contextmenu', e => e.preventDefault(), opts);
    video.addEventListener('pointerdown', e => {
      if (!this.enabled || e.button > 2) return;
      video.focus({preventScroll:true}); video.setPointerCapture(e.pointerId); this.pointer(e, true); e.preventDefault();
    }, opts);
    video.addEventListener('pointerup', e => { this.pointer(e, false); e.preventDefault(); }, opts);
    video.addEventListener('pointermove', e => { if (this.enabled) this.pointer(e, false, true); }, opts);
    video.addEventListener('pointercancel', () => this.release(), opts);
    video.addEventListener('pointerleave', () => { if (!video.hasPointerCapture?.(this.pointerId)) this.resetPrediction(); }, opts);
    video.addEventListener('blur', () => this.release(), opts);
    window.addEventListener('blur', () => this.release(), opts);
    document.addEventListener('visibilitychange', () => { if (document.hidden) this.release(); }, opts);
    video.addEventListener('wheel', e => {
      if (!this.enabled) return;
      e.preventDefault(); const factor = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 400 : 1;
      this.packet({kind:'wheel', x:Math.max(-1200, Math.min(1200, Math.round(-e.deltaX * factor))),
        y:Math.max(-1200, Math.min(1200, Math.round(-e.deltaY * factor)))});
    }, {...opts, passive:false});
    this.observer = new ResizeObserver(() => this.drawCursor()); this.observer.observe(video);
    video.addEventListener('loadedmetadata', () => this.drawCursor(), opts);
  }
  packet(value) {
    if (!this.epoch) return 0n;
    const sequence = String(++this.sequence);
    this.send({...value, epoch:this.epoch, sequence});
    return BigInt(sequence);
  }
  configure(value, allowed) {
    if (this.epoch !== value.epoch || !value.input || value.paused || !allowed) this.release();
    const epoch = typeof value.epoch === 'string' ? value.epoch : '';
    if (epoch !== this.epoch) {
      this.cursorSequence = this.shapeSequence = this.lastPointerSequence = 0n;
      this.pendingCursor = this.cursorState = this.cursorImage = null;
      this.resetPrediction();
    }
    this.epoch = epoch;
    this.enabled = !!this.epoch && !!value.input && !value.paused && allowed;
    this.sas = value.sas === true;
    this.paused = value.paused === true;
    if (!this.enabled || this.paused) this.resetPrediction();
    this.drawCursor();
  }
  release() { this.resetPrediction(); this.packet({kind:'release'}); }
  resetPrediction() {
    clearTimeout(this.predictionTimer); this.prediction = null; this.drawCursor();
  }
  settlePrediction() {
    if (!this.prediction) return;
    const acknowledged = cursorSerial(this.cursorState?.input_sequence);
    if (acknowledged !== null && acknowledged < this.prediction.sequence &&
        performance.now() - this.prediction.at < 1000) {
      this.predictionTimer = setTimeout(() => this.settlePrediction(), 50); return;
    }
    this.resetPrediction();
  }
  key(event, down) {
    if (!this.enabled || event.isComposing || !keys[event.code]) return;
    event.preventDefault(); this.packet({kind:'key', usb:0x70000 + keys[event.code], down});
  }
  chord(name) {
    const chord = chords[name];
    if (!this.enabled || !chord || (name === 'ctrl-alt-delete' && !this.sas)) return;
    this.release();
    for (const key of chord) this.packet({kind:'key', usb:0x70000 + key, down:true});
    for (const key of [...chord].reverse()) this.packet({kind:'key', usb:0x70000 + key, down:false});
  }
  rect() {
    const box = this.video.getBoundingClientRect();
    if (!this.video.videoWidth || !this.video.videoHeight) return null;
    const scale = Math.min(box.width / this.video.videoWidth, box.height / this.video.videoHeight);
    const width = this.video.videoWidth * scale, height = this.video.videoHeight * scale;
    return {left:box.left + (box.width - width) / 2, top:box.top + (box.height - height) / 2, width, height, box};
  }
  pointer(event, down, motion = false) {
    if (!this.enabled) return;
    const r = this.rect(); if (!r || r.width <= 0 || r.height <= 0) return;
    const x = (event.clientX - r.left) / r.width, y = (event.clientY - r.top) / r.height;
    if ((motion || down) && (x < 0 || x > 1 || y < 0 || y > 1) && !this.video.hasPointerCapture(event.pointerId)) {
      this.resetPrediction(); return;
    }
    const button = motion ? 0 : [1, 2, 3][event.button]; if (button === undefined) return;
    const px = Math.max(0, Math.min(1, x)), py = Math.max(0, Math.min(1, y));
    this.pointerId = event.pointerId;
    this.lastPointerSequence = this.packet({kind:'pointer', x:px, y:py, button, down});
    if (this.cursorState?.enabled && this.cursorImage) {
      this.prediction = {x:px, y:py, sequence:this.lastPointerSequence, at:performance.now()};
      clearTimeout(this.predictionTimer);
      this.predictionTimer = setTimeout(() => this.settlePrediction(), 250);
      this.drawCursor();
    }
  }
  cursor(m) {
    if (this.closed || this.paused || (m.epoch !== undefined && m.epoch !== this.epoch)) return;
    if (m.cursor_position_only || m.cursor_sequence !== undefined) {
      const sequence = cursorSerial(m.cursor_sequence), shape = cursorSerial(m.cursor_shape_sequence);
      if (sequence === null || shape === null || sequence <= this.cursorSequence || shape <= 0n || shape < this.shapeSequence) return;
      if (m.cursor_position_only && (m.width !== undefined || m.pixels !== undefined)) return;
      if (m.cursor_position_only && shape !== this.shapeSequence) {
        if (!this.pendingCursor || sequence > cursorSerial(this.pendingCursor.cursor_sequence)) this.pendingCursor = m;
        return;
      }
      this.cursorSequence = sequence; this.shapeSequence = shape;
    }
    this.applyCursor(m);
    if (this.pendingCursor) {
      const pending = this.pendingCursor; this.pendingCursor = null; this.cursor(pending);
    }
  }
  applyCursor(m) {
    this.cursorState = m;
    if (!m.enabled) this.resetPrediction();
    const acknowledged = cursorSerial(m.input_sequence);
    if (this.prediction && (acknowledged === null || acknowledged >= this.prediction.sequence) &&
        Math.abs(this.prediction.x * Math.max(1, m.source_width - 1) - m.x) <= 2 &&
        Math.abs(this.prediction.y * Math.max(1, m.source_height - 1) - m.y) <= 2) this.resetPrediction();
    if (Number.isInteger(m.width)) {
      this.cursorImage = null;
      if (m.width > 0 && m.width <= 128 && Number.isInteger(m.height) && m.height > 0 && m.height <= 128 &&
          typeof m.pixels === 'string' && m.pixels.length <= 90000) {
        const data = Uint8Array.from(atob(m.pixels), c => c.charCodeAt(0));
        if (data.length === m.width * m.height * 4) {
          const rgba = new Uint8ClampedArray(data.length);
          for (let i = 0; i < data.length; i += 4) {
            const a = data[i + 3]; rgba[i] = a ? data[i + 2] * 255 / a : 0;
            rgba[i + 1] = a ? data[i + 1] * 255 / a : 0; rgba[i + 2] = a ? data[i] * 255 / a : 0; rgba[i + 3] = a;
          }
          this.cursorImage = document.createElement('canvas'); this.cursorImage.width = m.width; this.cursorImage.height = m.height;
          this.cursorImage.getContext('2d').putImageData(new ImageData(rgba, m.width, m.height), 0, 0);
          this.hotX = Math.max(0, Math.min(m.width - 1, m.hot_x || 0)); this.hotY = Math.max(0, Math.min(m.height - 1, m.hot_y || 0));
        }
      }
    }
    this.drawCursor();
  }
  drawCursor() {
    if (this.closed || this.cursorFrame) return;
    this.cursorFrame = requestAnimationFrame(() => { this.cursorFrame = 0; this.paintCursor(); });
  }
  paintCursor() {
    const r = this.rect(), m = this.cursorState;
    const ctx = this.canvas.getContext('2d');
    if (!r) { ctx.clearRect(0, 0, this.canvas.width, this.canvas.height); this.cursorRect = null; return; }
    const dpr = window.devicePixelRatio || 1;
    const width = Math.ceil(r.box.width * dpr), height = Math.ceil(r.box.height * dpr);
    if (this.canvas.width !== width || this.canvas.height !== height || this.cursorDpr !== dpr) {
      this.canvas.width = width; this.canvas.height = height; this.cursorRect = null; this.cursorDpr = dpr;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    // Reuse the backing canvas and only clear the previous cursor rectangle.
    if (this.cursorRect) {
      const old = this.cursorRect; ctx.clearRect(old.x - 2, old.y - 2, old.w + 4, old.h + 4);
      this.cursorRect = null;
    }
    if (this.paused || !m?.enabled || !this.cursorImage || !Number.isFinite(m.x) || !Number.isFinite(m.y) ||
        !(m.source_width > 0) || !(m.source_height > 0)) return;
    const position = this.prediction && this.enabled ? {
      x:this.prediction.x * Math.max(1, m.source_width - 1), y:this.prediction.y * Math.max(1, m.source_height - 1)} : m;
    if (position.x < 0 || position.y < 0 || position.x >= m.source_width || position.y >= m.source_height) return;
    const sx = r.width / m.source_width, sy = r.height / m.source_height;
    const rect = {x:r.left - r.box.left + (position.x - this.hotX) * sx,
      y:r.top - r.box.top + (position.y - this.hotY) * sy, w:this.cursorImage.width * sx, h:this.cursorImage.height * sy};
    ctx.save(); ctx.beginPath(); ctx.rect(r.left - r.box.left, r.top - r.box.top, r.width, r.height); ctx.clip();
    ctx.drawImage(this.cursorImage, rect.x, rect.y, rect.w, rect.h); ctx.restore();
    this.cursorRect = rect;
  }
  close() {
    this.release(); this.closed = true; this.enabled = false;
    clearTimeout(this.predictionTimer); cancelAnimationFrame(this.cursorFrame);
    this.pendingCursor = this.cursorState = this.cursorImage = null;
    this.events.abort(); this.observer.disconnect(); this.canvas.width = this.canvas.width;
  }
}
