'use strict';

// Minimal canvas charts for the ESP WiFi Monitor UI (no library: it has to fit in flash).
// All colors come from CSS custom properties so light/dark themes just work.
const Charts = (() => {
  const SERIES = ['--s1', '--s2', '--s3', '--s4', '--s5', '--s6', '--s7', '--s8'];
  const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const FONT = '12px system-ui, -apple-system, "Segoe UI", sans-serif';

  // One date/time format for the whole UI: DD.MM.YYYY HH:MM:SS (local time)
  const p2 = (n) => String(n).padStart(2, '0');
  const fmtDate = (d) => `${p2(d.getDate())}.${p2(d.getMonth() + 1)}.${d.getFullYear()}`;
  const fmtTime = (d) => `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
  const fmtDateTime = (ms) => { const d = new Date(ms); return `${fmtDate(d)} ${fmtTime(d)}`; };

  function alpha(hex, a) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex);
    if (!m) return hex;
    const n = parseInt(m[1], 16);
    return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`;
  }

  function chrome() {
    return {
      ink: css('--text'), ink2: css('--muted'), grid: css('--grid'), axis: css('--axis'),
      surface: css('--card'),
    };
  }

  // Sizes the canvas backing store to its CSS box at device pixel ratio.
  function setup(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = FONT;
    return { ctx, w, h };
  }

  function niceStep(span, target) {
    const raw = span / Math.max(1, target);
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    const f = raw / p;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * p;
  }

  // Base: canvas + tooltip inside a positioned .chart-box, redraw on resize.
  class Base {
    constructor(box) {
      this.box = box;
      this.canvas = box.querySelector('canvas');
      this.tip = box.querySelector('.chart-tip');
      this.hover = null;
      this.canvas.addEventListener('mousemove', (e) => this.onPointer(e));
      this.canvas.addEventListener('mouseleave', () => this.clearHover());
      this.canvas.addEventListener('touchstart', (e) => this.onPointer(e.touches[0]), { passive: true });
      if (window.ResizeObserver) new ResizeObserver(() => this.draw()).observe(this.canvas);
    }
    pointer(e) {
      const r = this.canvas.getBoundingClientRect();
      return { x: e.clientX - r.left, y: e.clientY - r.top };
    }
    clearHover() {
      if (this.hover == null) return;
      this.hover = null;
      this.tip.hidden = true;
      this.draw();
    }
    showTip(x, y, rows) {
      const tip = this.tip;
      tip.replaceChildren(...rows.map((r) => {
        const line = document.createElement('div');
        if (r.color) {
          const sw = document.createElement('span');
          sw.className = 'sw';
          sw.style.background = r.color;
          line.append(sw);
        }
        const t = document.createElement(r.strong ? 'b' : 'span');
        t.textContent = r.text;
        line.append(t);
        return line;
      }));
      tip.hidden = false;
      const bw = this.box.clientWidth;
      const tw = tip.offsetWidth, th = tip.offsetHeight;
      let left = x + 14;
      if (left + tw > bw) left = x - tw - 14;
      tip.style.left = Math.max(0, left) + 'px';
      tip.style.top = Math.max(0, y - th - 10) + 'px';
    }
    onPointer() {}
    draw() {}
  }

  // x ticks: time of day, with the date when a tick step is a whole day
  function timeTicks(ctx, c, x0, x1, X, y, plotW) {
    const span = x1 - x0;
    const steps = [60e3, 120e3, 300e3, 600e3, 900e3, 1800e3, 3600e3, 7200e3, 10800e3, 21600e3, 43200e3, 86400e3];
    const xs = steps.find((s) => span / s <= Math.max(2, plotW / 80)) || 86400e3;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.fillStyle = c.ink2;
    const tzOff = new Date().getTimezoneOffset() * 60e3;
    for (let t = Math.ceil((x0 - tzOff) / xs) * xs + tzOff; t <= x1; t += xs) {
      const dt = new Date(t);
      const hm = `${String(dt.getHours()).padStart(2, '0')}:${String(dt.getMinutes()).padStart(2, '0')}`;
      ctx.fillText(xs >= 86400e3 ? `${p2(dt.getDate())}.${p2(dt.getMonth() + 1)}.` : hm, X(t), y);
    }
  }

  function drawYAxis(ctx, c, pad, w, yTicks, Y, fmt) {
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 1;
    for (const v of yTicks) {
      const y = Math.round(Y(v)) + 0.5;
      ctx.strokeStyle = c.grid;
      ctx.beginPath();
      ctx.moveTo(pad.l, y);
      ctx.lineTo(w - pad.r, y);
      ctx.stroke();
      ctx.fillStyle = c.ink2;
      ctx.fillText(fmt(v), pad.l - 6, y);
    }
  }

  // ---- Spectrum: x = WiFi channel, y = RSSI; each AP drawn as a 20 MHz hump ----

  class Spectrum extends Base {
    constructor(box, legend) {
      super(box);
      this.legend = legend;
      this.nets = [];
      this.slots = new Map();  // bssid -> series slot, stable while the AP is visible
      this.strongRssi = -67;
    }

    setData(nets, strongRssi) {
      this.nets = nets;
      if (strongRssi != null) this.strongRssi = strongRssi;
      // Color follows the AP, not its rank: keep existing slots, give free
      // slots to the strongest APs that do not have one yet.
      const present = new Set(nets.map((n) => n.bssid));
      for (const b of [...this.slots.keys()]) if (!present.has(b)) this.slots.delete(b);
      const used = new Set(this.slots.values());
      const free = [...SERIES.keys()].filter((i) => !used.has(i));
      for (const n of [...nets].sort((a, b) => b.rssi - a.rssi)) {
        if (!free.length) break;
        if (!this.slots.has(n.bssid)) this.slots.set(n.bssid, free.shift());
      }
      this.draw();
      this.drawLegend();
    }

    colorOf(n) {
      const s = this.slots.get(n.bssid);
      return s == null ? null : css(SERIES[s]);
    }

    geometry(w, h) {
      const pad = { l: 44, r: 12, t: 14, b: 34 };
      const xMax = this.nets.some((n) => n.channel === 14) ? 16 : 15;
      const xMin = -1;
      const strongest = Math.max(-30, ...this.nets.map((n) => n.rssi));
      const yMax = Math.ceil((strongest + 10) / 10) * 10;
      const yMin = -100;
      const X = (ch) => pad.l + ((ch - xMin) / (xMax - xMin)) * (w - pad.l - pad.r);
      const Y = (db) => pad.t + ((yMax - Math.max(yMin, db)) / (yMax - yMin)) * (h - pad.t - pad.b);
      return { pad, xMin, xMax, yMin, yMax, X, Y, invX: (x) => xMin + ((x - pad.l) / (w - pad.l - pad.r)) * (xMax - xMin) };
    }

    // Shape of a 20 MHz channel (+-2 channels), flat top, steep skirts.
    // Width is an assumption: a standard scan does not report channel width.
    level(n, ch, yMin) {
      const d = Math.abs(ch - n.channel);
      if (d >= 2) return yMin;
      return yMin + (n.rssi - yMin) * (1 - Math.pow(d / 2, 4));
    }

    draw() {
      const { ctx, w, h } = setup(this.canvas);
      if (w < 10) return;
      const c = chrome();
      const g = this.geometry(w, h);
      const { pad, X, Y } = g;

      const yTicks = [];
      for (let v = g.yMin; v <= g.yMax; v += 10) yTicks.push(v);
      drawYAxis(ctx, c, pad, w, yTicks, Y, (v) => v);

      // strong threshold
      ctx.save();
      ctx.setLineDash([4, 4]);
      ctx.strokeStyle = c.axis;
      const ys = Math.round(Y(this.strongRssi)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(pad.l, ys);
      ctx.lineTo(w - pad.r, ys);
      ctx.stroke();
      ctx.restore();
      ctx.fillStyle = c.ink2;
      ctx.textAlign = 'right';
      ctx.textBaseline = 'bottom';
      ctx.fillText('strong', w - pad.r - 2, ys - 2);

      // x axis: channels
      const lastCh = g.xMax === 16 ? 14 : 13;
      ctx.strokeStyle = c.axis;
      ctx.beginPath();
      ctx.moveTo(pad.l, h - pad.b + 0.5);
      ctx.lineTo(w - pad.r, h - pad.b + 0.5);
      ctx.stroke();
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      for (let ch = 1; ch <= lastCh; ch++) {
        const x = X(ch);
        ctx.fillStyle = ch === 1 || ch === 6 || ch === 11 ? c.ink : c.ink2;
        ctx.font = (ch === 1 || ch === 6 || ch === 11 ? '600 ' : '') + FONT;
        ctx.fillText(ch, x, h - pad.b + 6);
      }
      ctx.font = FONT;
      ctx.fillStyle = c.ink2;
      ctx.fillText('Channel', (pad.l + w - pad.r) / 2, h - 14);
      ctx.save();
      ctx.translate(12, (pad.t + h - pad.b) / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.textBaseline = 'middle';
      ctx.fillText('dBm', 0, 0);
      ctx.restore();

      // clip humps to the plot area
      ctx.save();
      ctx.beginPath();
      ctx.rect(pad.l, pad.t, w - pad.l - pad.r, h - pad.t - pad.b);
      ctx.clip();

      const hovered = this.hover;
      const order = [...this.nets].sort((a, b) => {
        const ca = this.slots.has(a.bssid) ? 1 : 0, cb = this.slots.has(b.bssid) ? 1 : 0;
        return ca - cb || a.rssi - b.rssi;  // grey first, then colored weakest -> strongest
      });
      for (const n of order) {
        if (hovered && n.bssid === hovered.bssid) continue;
        this.hump(ctx, g, n, this.colorOf(n), c, false);
      }
      if (hovered) this.hump(ctx, g, hovered, this.colorOf(hovered) || c.ink, c, true);
      ctx.restore();

      // labels for colored APs, strongest first, skipping collisions
      const boxes = [];
      ctx.textAlign = 'center';
      ctx.textBaseline = 'bottom';
      for (const n of [...this.nets].sort((a, b) => b.rssi - a.rssi)) {
        if (!this.slots.has(n.bssid) && !(hovered && hovered.bssid === n.bssid)) continue;
        const label = n.ssid || '(hidden)';
        const tw = ctx.measureText(label).width;
        const x = X(n.channel), y = Y(n.rssi) - 4;
        const b = { x0: x - tw / 2 - 2, x1: x + tw / 2 + 2, y0: y - 14, y1: y };
        if (boxes.some((o) => b.x0 < o.x1 && b.x1 > o.x0 && b.y0 < o.y1 && b.y1 > o.y0)) continue;
        boxes.push(b);
        ctx.fillStyle = c.ink;
        ctx.fillText(label, Math.min(Math.max(x, pad.l + tw / 2), w - pad.r - tw / 2), y);
      }

      if (!this.nets.length) {
        ctx.fillStyle = c.ink2;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('No networks detected yet', (pad.l + w - pad.r) / 2, (pad.t + h - pad.b) / 2);
      }
    }

    hump(ctx, g, n, color, c, emphasized) {
      const { X, Y, yMin } = g;
      ctx.beginPath();
      for (let i = 0; i <= 48; i++) {
        const ch = n.channel - 2 + (4 * i) / 48;
        const x = X(ch), y = Y(this.level(n, ch, yMin));
        if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      }
      if (color) {
        ctx.fillStyle = alpha(color, emphasized ? 0.22 : 0.1);
        ctx.fill();
        ctx.strokeStyle = color;
        ctx.lineWidth = emphasized ? 3 : 2;
      } else {
        ctx.strokeStyle = alpha(c.ink2.startsWith('#') ? c.ink2 : '#888888', 0.55);
        ctx.lineWidth = 1;
      }
      ctx.stroke();
    }

    onPointer(e) {
      if (!this.nets.length) return;
      const p = this.pointer(e);
      const g = this.geometry(this.canvas.clientWidth, this.canvas.clientHeight);
      const ch = g.invX(p.x);
      let best = null, bestD = 28;
      for (const n of this.nets) {
        if (Math.abs(ch - n.channel) >= 2) continue;
        const d = Math.abs(g.Y(this.level(n, ch, g.yMin)) - p.y);
        if (d < bestD) { best = n; bestD = d; }
      }
      if ((best && best.bssid) !== (this.hover && this.hover.bssid)) {
        this.hover = best;
        this.draw();
      }
      if (!best) { this.tip.hidden = true; return; }
      this.showTip(p.x, p.y, [
        { text: best.ssid || '(hidden)', strong: true, color: this.colorOf(best) || css('--muted') },
        { text: best.bssid },
        { text: `Channel ${best.channel} · ${best.rssi} dBm · ${best.security}` },
      ]);
    }

    drawLegend() {
      if (!this.legend) return;
      const items = this.nets.filter((n) => this.slots.has(n.bssid)).sort((a, b) => b.rssi - a.rssi);
      const els = items.map((n) => {
        const li = document.createElement('span');
        li.className = 'lg-item';
        const sw = document.createElement('span');
        sw.className = 'sw';
        sw.style.background = this.colorOf(n);
        li.append(sw, `${n.ssid || '(hidden)'} ${n.rssi}`);
        li.addEventListener('mouseenter', () => { this.hover = n; this.draw(); });
        li.addEventListener('mouseleave', () => { this.hover = null; this.draw(); });
        return li;
      });
      const others = this.nets.length - items.length;
      if (others > 0) {
        const li = document.createElement('span');
        li.className = 'lg-item muted';
        const sw = document.createElement('span');
        sw.className = 'sw sw-line';
        li.append(sw, `${others} other network${others > 1 ? 's' : ''}`);
        els.push(li);
      }
      this.legend.replaceChildren(...els);
    }
  }

  // ---- Time series: x = time (ms epoch), one y axis, one or more series ----

  class Line extends Base {
    constructor(box, opts) {
      super(box);
      this.opts = Object.assign({ yLabel: '', integer: false, unit: '' }, opts);
      this.series = [];
      this.legend = opts.legend || null;
    }

    setData(series) {
      this.series = series;  // [{name, slot, points: [[ms, y|null], ...]}]
      this.draw();
      if (this.legend) {
        this.legend.replaceChildren(...(series.length > 1 ? series : []).map((s) => {
          const li = document.createElement('span');
          li.className = 'lg-item';
          const sw = document.createElement('span');
          sw.className = 'sw sw-line';
          sw.style.background = css(SERIES[s.slot]);
          li.append(sw, s.name);
          return li;
        }));
      }
    }

    domain() {
      let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
      for (const s of this.series) {
        for (const [x, y] of s.points) {
          x0 = Math.min(x0, x); x1 = Math.max(x1, x);
          if (y != null) { y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
        }
      }
      if (!isFinite(x0)) return null;
      if (x0 === x1) { x0 -= 60000; x1 += 60000; }
      if (this.opts.yMin != null) y0 = Math.min(this.opts.yMin, isFinite(y0) ? y0 : this.opts.yMin);
      if (this.opts.yMax != null) y1 = Math.max(this.opts.yMax, isFinite(y1) ? y1 : this.opts.yMax);
      if (!isFinite(y0)) { y0 = 0; y1 = 1; }
      if (y0 === y1) { y0 -= 1; y1 += 1; }
      // integer series (dBm, counts) never get fractional ticks, which would print as duplicates
      const step = this.opts.integer ? Math.max(1, niceStep(y1 - y0, 5)) : niceStep(y1 - y0, 5);
      return { x0, x1, y0: Math.floor(y0 / step) * step, y1: Math.ceil(y1 / step) * step, step };
    }

    geometry(w, h) {
      const d = this.domain();
      if (!d) return null;
      const pad = { l: 44, r: 14, t: 12, b: 26 };
      const X = (x) => pad.l + ((x - d.x0) / (d.x1 - d.x0)) * (w - pad.l - pad.r);
      const Y = (y) => pad.t + ((d.y1 - y) / (d.y1 - d.y0)) * (h - pad.t - pad.b);
      return { d, pad, X, Y };
    }

    draw() {
      const { ctx, w, h } = setup(this.canvas);
      if (w < 10) return;
      const c = chrome();
      const g = this.geometry(w, h);
      if (!g) {
        ctx.fillStyle = c.ink2;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(this.opts.empty || 'No data yet', w / 2, h / 2);
        return;
      }
      const { d, pad, X, Y } = g;
      const ticks = [];
      for (let v = d.y0; v <= d.y1 + 1e-9; v += d.step) ticks.push(v);
      drawYAxis(ctx, c, pad, w, ticks, Y, (v) => (this.opts.integer ? Math.round(v) : +v.toFixed(1)));

      timeTicks(ctx, c, d.x0, d.x1, X, h - pad.b + 6, w - pad.l - pad.r);
      ctx.strokeStyle = c.axis;
      ctx.beginPath();
      ctx.moveTo(pad.l, h - pad.b + 0.5);
      ctx.lineTo(w - pad.r, h - pad.b + 0.5);
      ctx.stroke();
      if (this.opts.yLabel) {
        ctx.save();
        ctx.translate(12, (pad.t + h - pad.b) / 2);
        ctx.rotate(-Math.PI / 2);
        ctx.textBaseline = 'middle';
        ctx.fillText(this.opts.yLabel, 0, 0);
        ctx.restore();
      }

      // lines (gaps at null), lone points as dots
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      for (const s of this.series) {
        const color = css(SERIES[s.slot]);
        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.lineWidth = 2;
        let run = [];
        const flush = () => {
          if (run.length === 1) {
            ctx.beginPath();
            ctx.arc(run[0][0], run[0][1], 3, 0, Math.PI * 2);
            ctx.fill();
          } else if (run.length > 1) {
            ctx.beginPath();
            run.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
            ctx.stroke();
          }
          run = [];
        };
        for (const [x, y] of s.points) {
          if (y == null) flush(); else run.push([X(x), Y(y)]);
        }
        flush();
      }

      // hover crosshair + markers
      if (this.hover != null) {
        const x = X(this.hover);
        ctx.strokeStyle = c.axis;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(Math.round(x) + 0.5, pad.t);
        ctx.lineTo(Math.round(x) + 0.5, h - pad.b);
        ctx.stroke();
        for (const s of this.series) {
          const p = s.points.find((q) => q[0] === this.hover);
          if (!p || p[1] == null) continue;
          ctx.beginPath();
          ctx.arc(x, Y(p[1]), 4.5, 0, Math.PI * 2);
          ctx.fillStyle = css(SERIES[s.slot]);
          ctx.strokeStyle = c.surface;
          ctx.lineWidth = 2;
          ctx.fill();
          ctx.stroke();
        }
      }
    }

    onPointer(e) {
      const p = this.pointer(e);
      const g = this.geometry(this.canvas.clientWidth, this.canvas.clientHeight);
      if (!g) return;
      // snap to the nearest sample of any series
      let best = null, bestD = Infinity;
      for (const s of this.series) {
        for (const [x] of s.points) {
          const dd = Math.abs(g.X(x) - p.x);
          if (dd < bestD) { bestD = dd; best = x; }
        }
      }
      if (best == null) return;
      if (best !== this.hover) { this.hover = best; this.draw(); }
      const dt = new Date(best);
      const rows = [{ text: fmtDateTime(dt.getTime()), strong: true }];
      for (const s of this.series) {
        const q = s.points.find((pt) => pt[0] === best);
        const v = q && q[1] != null ? (this.opts.integer ? Math.round(q[1]) : +q[1].toFixed(1)) + this.opts.unit : '—';
        rows.push({ text: `${s.name}: ${v}`, color: css(SERIES[s.slot]) });
      }
      this.showTip(g.X(best), p.y, rows);
    }
  }

  // ---- Per-channel bars: stacked series, one bar per channel ----

  class Bars extends Base {
    constructor(box, opts) {
      super(box);
      this.opts = opts || {};
      this.cats = [];
      this.series = [];
      this.legend = this.opts.legend || null;
    }

    setData(cats, series) {
      this.cats = cats;        // labels
      this.series = series;    // [{name, slot, values:[...]}], stacked bottom -> top
      this.draw();
      if (this.legend) {
        this.legend.replaceChildren(...series.map((s) => {
          const li = document.createElement('span');
          li.className = 'lg-item';
          const sw = document.createElement('span');
          sw.className = 'sw';
          sw.style.background = css(SERIES[s.slot]);
          li.append(sw, s.name);
          return li;
        }));
      }
    }

    geometry(w, h) {
      const pad = { l: 36, r: 10, t: 12, b: 34 };
      const totals = this.cats.map((_, i) => this.series.reduce((a, s) => a + s.values[i], 0));
      const max = Math.max(1, ...totals);
      const step = Math.max(1, niceStep(max, 4));
      const yMax = Math.ceil(max / step) * step;
      const bw = (w - pad.l - pad.r) / Math.max(1, this.cats.length);
      const Y = (v) => pad.t + ((yMax - v) / yMax) * (h - pad.t - pad.b);
      return { pad, totals, yMax, step, bw, Y };
    }

    draw() {
      const { ctx, w, h } = setup(this.canvas);
      if (w < 10 || !this.cats.length) return;
      const c = chrome();
      const g = this.geometry(w, h);
      const { pad, bw, Y } = g;
      const ticks = [];
      for (let v = 0; v <= g.yMax; v += g.step) ticks.push(v);
      drawYAxis(ctx, c, pad, w, ticks, Y, (v) => v);

      const barW = Math.min(28, bw * 0.6);
      this.cats.forEach((cat, i) => {
        const cx = pad.l + bw * (i + 0.5);
        let base = 0;
        const segs = this.series.map((s) => ({ s, v: s.values[i] })).filter((q) => q.v > 0);
        segs.forEach(({ s, v }, k) => {
          const y0 = Y(base), y1 = Y(base + v);
          const top = k === segs.length - 1;
          ctx.fillStyle = css(SERIES[s.slot]);
          if (this.hover === i) ctx.globalAlpha = 1; else ctx.globalAlpha = this.hover == null ? 1 : 0.55;
          // 2px surface gap between stacked segments, rounded data end on top only
          const gap = k > 0 ? 2 : 0;
          roundRect(ctx, cx - barW / 2, y1, barW, Math.max(0, y0 - y1 - gap), top ? 4 : 0);
          ctx.fill();
          base += v;
        });
        ctx.globalAlpha = 1;
        ctx.fillStyle = cat === '1' || cat === '6' || cat === '11' ? c.ink : c.ink2;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText(cat, cx, h - pad.b + 6);
      });
      ctx.strokeStyle = c.axis;
      ctx.beginPath();
      ctx.moveTo(pad.l, h - pad.b + 0.5);
      ctx.lineTo(w - pad.r, h - pad.b + 0.5);
      ctx.stroke();
      ctx.fillStyle = c.ink2;
      ctx.fillText('Channel', (pad.l + w - pad.r) / 2, h - 14);
    }

    onPointer(e) {
      if (!this.cats.length) return;
      const p = this.pointer(e);
      const g = this.geometry(this.canvas.clientWidth, this.canvas.clientHeight);
      const i = Math.floor((p.x - g.pad.l) / g.bw);
      if (i < 0 || i >= this.cats.length) return this.clearHover();
      if (i !== this.hover) { this.hover = i; this.draw(); }
      const rows = [{ text: `${this.opts.catLabel || ''}${this.cats[i]}: ${g.totals[i]} total`, strong: true }];
      for (const s of [...this.series].reverse()) rows.push({ text: `${s.name}: ${s.values[i]}`, color: css(SERIES[s.slot]) });
      this.showTip(p.x, p.y, rows);
    }
  }

  // ---- Per-channel RSSI range: min-max bar with average and max markers ----

  class Range extends Base {
    constructor(box) {
      super(box);
      this.rows = [];
    }

    setData(rows) {
      this.rows = rows;  // [{cat, min, avg, max}] (nulls when no AP)
      this.draw();
    }

    geometry(w, h) {
      const pad = { l: 44, r: 10, t: 12, b: 34 };
      const vals = this.rows.flatMap((r) => (r.max == null ? [] : [r.max, r.min]));
      const yMax = Math.ceil((Math.max(-40, ...vals) + 5) / 10) * 10;
      const yMin = Math.floor((Math.min(-90, ...vals) - 5) / 10) * 10;
      const bw = (w - pad.l - pad.r) / Math.max(1, this.rows.length);
      const Y = (v) => pad.t + ((yMax - v) / (yMax - yMin)) * (h - pad.t - pad.b);
      return { pad, yMin, yMax, bw, Y };
    }

    draw() {
      const { ctx, w, h } = setup(this.canvas);
      if (w < 10 || !this.rows.length) return;
      const c = chrome();
      const g = this.geometry(w, h);
      const { pad, bw, Y } = g;
      const ticks = [];
      for (let v = g.yMin; v <= g.yMax; v += 10) ticks.push(v);
      drawYAxis(ctx, c, pad, w, ticks, Y, (v) => v);
      const color = css('--s1');
      this.rows.forEach((r, i) => {
        const cx = pad.l + bw * (i + 0.5);
        if (r.max != null) {
          ctx.globalAlpha = this.hover == null || this.hover === i ? 1 : 0.55;
          ctx.fillStyle = alpha(color, 0.5);
          roundRect(ctx, cx - 5, Y(r.max), 10, Math.max(2, Y(r.min) - Y(r.max)), 4);
          ctx.fill();
          ctx.beginPath();
          ctx.arc(cx, Y(r.avg), 4.5, 0, Math.PI * 2);
          ctx.fillStyle = color;
          ctx.strokeStyle = c.surface;
          ctx.lineWidth = 2;
          ctx.fill();
          ctx.stroke();
          ctx.globalAlpha = 1;
        }
        ctx.fillStyle = r.cat === '1' || r.cat === '6' || r.cat === '11' ? c.ink : c.ink2;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText(r.cat, cx, h - pad.b + 6);
      });
      ctx.strokeStyle = c.axis;
      ctx.beginPath();
      ctx.moveTo(pad.l, h - pad.b + 0.5);
      ctx.lineTo(w - pad.r, h - pad.b + 0.5);
      ctx.stroke();
      ctx.fillStyle = c.ink2;
      ctx.fillText('Channel', (pad.l + w - pad.r) / 2, h - 14);
      ctx.save();
      ctx.translate(12, (pad.t + h - pad.b) / 2);
      ctx.rotate(-Math.PI / 2);
      ctx.textBaseline = 'middle';
      ctx.fillText('dBm', 0, 0);
      ctx.restore();
    }

    onPointer(e) {
      const p = this.pointer(e);
      const g = this.geometry(this.canvas.clientWidth, this.canvas.clientHeight);
      const i = Math.floor((p.x - g.pad.l) / g.bw);
      if (i < 0 || i >= this.rows.length) return this.clearHover();
      if (i !== this.hover) { this.hover = i; this.draw(); }
      const r = this.rows[i];
      this.showTip(p.x, p.y, r.max == null
        ? [{ text: `Channel ${r.cat}`, strong: true }, { text: 'No APs detected' }]
        : [{ text: `Channel ${r.cat}`, strong: true },
           { text: `Max ${r.max} dBm` }, { text: `Average ${r.avg} dBm`, color: css('--s1') }, { text: `Min ${r.min} dBm` }]);
    }
  }

  // ---- Heatmap: x = time, y = channel, color = detected APs (sequential, one hue) ----

  // Blue ramp; in dark mode it runs dark -> light so low values recede into the surface.
  const RAMP_LIGHT = ['#cde2fb', '#86b6ef', '#3987e5', '#1c5cab', '#0d366b'];
  const RAMP_DARK = ['#104281', '#1c5cab', '#2a78d6', '#6da7ec', '#cde2fb'];

  function mix(a, b, t) {
    const pa = parseInt(a.slice(1), 16), pb = parseInt(b.slice(1), 16);
    const ch = (s) => Math.round(((pa >> s) & 255) + (((pb >> s) & 255) - ((pa >> s) & 255)) * t);
    return `rgb(${ch(16)},${ch(8)},${ch(0)})`;
  }

  class Heatmap extends Base {
    constructor(box, legend) {
      super(box);
      this.legend = legend;
      this.times = [];
      this.values = [];  // values[i][channel-1]
      this.max = 1;
    }

    ramp() { return document.documentElement.dataset.theme === 'dark' ? RAMP_DARK : RAMP_LIGHT; }

    color(v) {
      const r = this.ramp();
      const t = Math.min(1, v / this.max) * (r.length - 1);
      const i = Math.min(r.length - 2, Math.floor(t));
      return mix(r[i], r[i + 1], t - i);
    }

    setData(times, values) {
      this.times = times;
      this.values = values;
      this.max = Math.max(1, ...values.flat());
      this.draw();
      if (this.legend) {
        const bar = document.createElement('span');
        bar.className = 'heat-bar';
        bar.style.background = `linear-gradient(90deg, ${this.color(0.001 * this.max)}, ${this.color(this.max)})`;
        this.legend.replaceChildren('0 APs', bar, `${+this.max.toFixed(1)} APs`, ' · empty cell = none detected');
      }
    }

    geometry(w, h) {
      const pad = { l: 36, r: 10, t: 6, b: 26 };
      const cols = Math.max(1, this.times.length);
      return { pad, cw: (w - pad.l - pad.r) / cols, rh: (h - pad.t - pad.b) / 13 };
    }

    draw() {
      const { ctx, w, h } = setup(this.canvas);
      if (w < 10) return;
      const c = chrome();
      if (!this.times.length) {
        ctx.fillStyle = c.ink2;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('No data yet', w / 2, h / 2);
        return;
      }
      const { pad, cw, rh } = this.geometry(w, h);
      const gap = cw > 4 ? 1 : 0;
      this.values.forEach((row, i) => {
        for (let ch = 0; ch < 13; ch++) {
          const v = row[ch] || 0;
          if (!v) continue;
          ctx.fillStyle = this.color(v);
          ctx.fillRect(pad.l + i * cw, pad.t + ch * rh, Math.max(1, cw - gap), rh - 1);
        }
      });
      if (this.hover) {
        ctx.strokeStyle = c.ink;
        ctx.lineWidth = 1.5;
        ctx.strokeRect(pad.l + this.hover.i * cw, pad.t + this.hover.ch * rh, Math.max(1, cw - gap), rh - 1);
      }
      ctx.textAlign = 'right';
      ctx.textBaseline = 'middle';
      for (let ch = 1; ch <= 13; ch++) {
        const main = ch === 1 || ch === 6 || ch === 11;
        ctx.fillStyle = main ? c.ink : c.ink2;
        ctx.font = (main ? '600 ' : '') + FONT;
        ctx.fillText(ch, pad.l - 8, pad.t + (ch - 0.5) * rh);
      }
      ctx.font = FONT;
      const x0 = this.times[0], x1 = this.times[this.times.length - 1];
      const span = Math.max(1, x1 - x0);
      const X = (t) => pad.l + ((t - x0) / span) * (w - pad.l - pad.r - cw) + cw / 2;
      timeTicks(ctx, c, x0, x1, X, h - pad.b + 6, w - pad.l - pad.r);
    }

    onPointer(e) {
      if (!this.times.length) return;
      const p = this.pointer(e);
      const { pad, cw, rh } = this.geometry(this.canvas.clientWidth, this.canvas.clientHeight);
      const i = Math.floor((p.x - pad.l) / cw), ch = Math.floor((p.y - pad.t) / rh);
      if (i < 0 || i >= this.times.length || ch < 0 || ch > 12) return this.clearHover();
      if (!this.hover || this.hover.i !== i || this.hover.ch !== ch) { this.hover = { i, ch }; this.draw(); }
      const v = this.values[i][ch] || 0;
      this.showTip(p.x, p.y, [
        { text: fmtDateTime(this.times[i]), strong: true },
        { text: `Channel ${ch + 1}: ${+v.toFixed(1)} APs detected`, color: v ? this.color(v) : null },
      ]);
    }
  }

  function roundRect(ctx, x, y, w, h, r) {
    r = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x, y + h);
    ctx.lineTo(x, y + r);
    ctx.quadraticCurveTo(x, y, x + r, y);
    ctx.lineTo(x + w - r, y);
    ctx.quadraticCurveTo(x + w, y, x + w, y + r);
    ctx.lineTo(x + w, y + h);
    ctx.closePath();
  }

  return { Spectrum, Line, Bars, Range, Heatmap, css, fmtDateTime };
})();
