'use strict';

// ---- helpers -----------------------------------------------------------------

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function el(tag, attrs, ...children) {
  const e = document.createElement(tag);
  if (attrs) {
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v;
      else if (k === 'style') e.style.cssText = v;
      else e.setAttribute(k, v);
    }
  }
  for (const c of children) {
    if (c == null) continue;
    e.append(c instanceof Node ? c : String(c));  // text only: SSIDs are untrusted
  }
  return e;
}

async function request(path, opts = {}) {
  const res = await fetch(path, { cache: 'no-store', ...opts });
  let body = null;
  try { body = await res.json(); } catch (_) { /* non-JSON */ }
  if (!res.ok) throw new Error((body && body.error) || `${res.status} ${res.statusText}`);
  return body;
}
const apiGet = (p) => request(p);
const apiPost = (p, data) => request(p, {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'wifi-monitor' },
  body: JSON.stringify(data || {}),
});

function fmtDuration(s) {
  if (s == null) return '—';
  s = Math.round(s);
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
        m = Math.floor(s % 3600 / 60), sec = s % 60;
  const p2 = (n) => String(n).padStart(2, '0');
  if (d) return `${d}d ${p2(h)}h ${p2(m)}m`;
  if (h) return `${h}h ${p2(m)}m`;
  if (m) return `${m}m ${p2(sec)}s`;
  return `${sec}s`;
}
const fmtAgo = (s) => (s < 5 ? 'now' : fmtDuration(s) + ' ago');
const fmtBytes = (b) => (b >= 1048576 ? (b / 1048576).toFixed(2) + ' MB' : b >= 1024 ? (b / 1024).toFixed(1) + ' KB' : b + ' B');

// RSSI quality classes
const QUALITY = [
  [-40, 'Excellent', 100, 'var(--ok)'],
  [-55, 'Very Good', 80, 'var(--ok)'],
  [-65, 'Good', 60, 'var(--ok)'],
  [-69, 'Fair', 45, 'var(--warn)'],
  [-79, 'Weak', 25, 'var(--warn)'],
  [-999, 'Very Weak', 10, 'var(--bad)'],
];
const quality = (rssi) => QUALITY.find(([min]) => rssi >= min);

function storage(key, value) {
  try {
    if (value === undefined) return localStorage.getItem(key);
    localStorage.setItem(key, value);
  } catch (_) { /* storage unavailable */ }
  return null;
}

// ---- state -------------------------------------------------------------------

const state = {
  page: 'dashboard',
  status: null,
  networks: [],
  networksScanId: -1,
  sort: { key: 'rssi', dir: -1 },
  pageIndex: 0,
  pageSize: 25,
  hist: { range: 'live', bssid: null, loadedKey: null, loadedAt: 0 },
  wifi: { selected: null, connecting: false, listKey: '' },
  alerts: { list: [], loadedId: -1, fetching: false },
  ble: { scanId: -1, devices: [], history: [] },
  chan: { scanId: -1, data: null },
};

const charts = {
  spectrum: new Charts.Spectrum($('#spectrum').parentElement, $('#spectrum-legend')),
  bars: new Charts.Bars($('#chan-bars').parentElement, { legend: $('#bars-legend'), catLabel: 'Channel ' }),
  range: new Charts.Range($('#chan-range').parentElement),
  histCount: new Charts.Line($('#hist-count').parentElement, { integer: false, legend: $('#hist-count-legend'), yMin: 0 }),
  histRssi: new Charts.Line($('#hist-rssi').parentElement, { yLabel: 'dBm', integer: true, unit: ' dBm', legend: $('#hist-rssi-legend') }),
  bleHist: new Charts.Line($('#ble-hist').parentElement, { yLabel: 'dBm', integer: true, unit: ' dBm', legend: $('#ble-hist-legend'), empty: 'Waiting for BLE scans' }),
  heat: new Charts.Heatmap($('#heatmap').parentElement, $('#heat-legend')),
  histAp: new Charts.Line($('#hist-ap-chart').parentElement, { yLabel: 'dBm', integer: true, unit: ' dBm', empty: 'No data for this access point in the selected range' }),
};

// ---- routing -----------------------------------------------------------------

const ROUTES = {
  '/': 'dashboard', '/networks': 'networks', '/channels': 'channels', '/history': 'history', '/alerts': 'alerts',
  '/wifi': 'wifi', '/ble': 'ble', '/mqtt': 'mqtt', '/update': 'update', '/settings': 'settings', '/system': 'system',
};

function showPage(page, push, query) {
  state.page = page;
  for (const s of $$('.page')) s.hidden = s.id !== 'page-' + page;
  for (const a of $$('nav a')) a.classList.toggle('active', a.dataset.page === page);
  if (push) {
    const path = Object.keys(ROUTES).find((p) => ROUTES[p] === page);
    history.pushState({ page }, '', path + (query || ''));
  }
  if (page === 'settings') loadConfig();
  if (page === 'mqtt') loadMqtt();
  if (page === 'alerts') loadAlertSettings();
  if (page === 'ble') { loadBleSettings(); state.ble.scanId = -1; }
  if (page === 'history') {
    const b = new URLSearchParams(location.search).get('bssid');
    if (b) state.hist.bssid = b;
    state.hist.loadedKey = null;
  }
  if (page === 'wifi') state.wifi.listKey = '';
  render();
}

document.addEventListener('click', (e) => {
  const a = e.target.closest('a[data-page]');
  if (!a || e.ctrlKey || e.metaKey || e.shiftKey) return;
  e.preventDefault();
  showPage(a.dataset.page, true);
});
window.addEventListener('popstate', () => showPage(ROUTES[location.pathname] || 'dashboard', false));

// ---- theme -------------------------------------------------------------------

function applyTheme(t) {
  document.documentElement.dataset.theme = t;
  for (const c of Object.values(charts)) c.draw();
}
const osTheme = () => (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
applyTheme(storage('theme') || osTheme());

// Device-wide appearance from Settings: default theme (unless this browser chose one) and accent color.
let appliedUi = '';
function applyUi(ui) {
  if (!ui) return;
  const key = ui.theme + ui.accent;
  if (key === appliedUi) return;
  appliedUi = key;
  if (!storage('theme')) applyTheme(ui.theme === 'auto' ? osTheme() : ui.theme);
  const root = document.documentElement.style;
  if (ui.accent.toLowerCase() === '#2563eb') {  // default: keep the per-theme accents from the CSS
    root.removeProperty('--accent');
    root.removeProperty('--accent-text');
  } else {
    const n = parseInt(ui.accent.slice(1), 16);
    const lum = (0.2126 * (n >> 16) + 0.7152 * ((n >> 8) & 255) + 0.0722 * (n & 255)) / 255;
    root.setProperty('--accent', ui.accent);
    root.setProperty('--accent-text', lum > 0.6 ? '#111111' : '#ffffff');
  }
}
$('#theme-btn').addEventListener('click', () => {
  const t = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  applyTheme(t);
  storage('theme', t);
});

// ---- polling -----------------------------------------------------------------

const refreshSel = $('#refresh');
refreshSel.value = storage('refresh') || '5';
if (!refreshSel.value) refreshSel.value = '5';
refreshSel.addEventListener('change', () => {
  storage('refresh', refreshSel.value);
  schedulePoll(0);
});

let pollTimer = null;
function schedulePoll(delayMs) {
  clearTimeout(pollTimer);
  // While joining a network, follow the attempt closely regardless of the setting.
  const s = state.wifi.connecting ? 2 : Number(refreshSel.value);
  if (delayMs == null) {
    if (!s) return;
    delayMs = s * 1000;
  }
  pollTimer = setTimeout(async () => {
    await poll();
    schedulePoll();
  }, delayMs);
}

async function poll() {
  try {
    const st = await apiGet('/api/status');
    state.status = st;
    syncClock(st);
    applyUi(st.ui);
    await syncAlerts(st);
    // The AP table only changes after a scan, so refetch it only then.
    if (st.scan.id !== state.networksScanId) {
      const net = await apiGet('/api/networks');
      state.networks = net.networks;
      state.networksScanId = net.scan_id;
    }
    setBanner(null);
  } catch (err) {
    state.status = null;
    setBanner(state.wifi.connecting
      ? 'Device is switching networks, reconnecting…'
      : 'Device not reachable: ' + err.message);
  }
  render();
}

// ---- clock -------------------------------------------------------------------
// Shows the device's local time (NTP + its timezone setting). Between polls it
// ticks locally from the last value the device reported.
const clock = { base: null, at: 0 };
const WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];

function syncClock(st) {
  // st.time is the device's local wall time without zone; treat it as UTC to do plain arithmetic
  clock.base = st && st.time ? Date.parse(st.time + 'Z') : null;
  clock.at = performance.now();
  renderClock();
}

function renderClock() {
  const t = $('#clock-time'), d = $('#clock-date');
  if (clock.base == null) {
    t.textContent = '--:--:--';
    d.textContent = state.status ? 'time not synced (NTP)' : '';
    return;
  }
  const now = new Date(clock.base + (performance.now() - clock.at));
  const p2 = (n) => String(n).padStart(2, '0');
  t.textContent = `${p2(now.getUTCHours())}:${p2(now.getUTCMinutes())}:${p2(now.getUTCSeconds())}`;
  d.textContent = `${WEEKDAYS[now.getUTCDay()]}, ${p2(now.getUTCDate())}.${p2(now.getUTCMonth() + 1)}.${now.getUTCFullYear()}.`;
}
setInterval(renderClock, 1000);

function setBanner(text) {
  const b = $('#banner');
  b.hidden = !text;
  b.textContent = text || '';
}

// ---- render ------------------------------------------------------------------

function render() {
  renderHeader();
  switch (state.page) {
    case 'dashboard': renderDashboard(); break;
    case 'networks': renderNetworks(); break;
    case 'channels': renderChannels(); break;
    case 'alerts': renderAlerts(); break;
    case 'history': renderHistory(); break;
    case 'wifi': renderWifi(); break;
    case 'mqtt': renderMqtt(); break;
    case 'update': renderUpdate(); break;
    case 'ble': renderBle(); break;
    case 'system': renderSystem(); break;
  }
}

function renderHeader() {
  const st = state.status;
  // BLE page and widget only on boards with a BLE radio
  const bleOn = !!(st && st.ble && st.ble.available);
  $('#nav-ble').hidden = !bleOn;
  $('#w-ble-box').hidden = !bleOn;
  $('#hdr-dot').className = 'dot ' + (st ? 'ok' : 'bad');
  if (!st) return;
  const w = st.wifi;
  const link = w.connected ? `${w.ssid} · ${w.ip}` : w.ap_active ? `Setup hotspot ${w.ap_ssid}` : 'WiFi disconnected';
  const scan = st.scan.scanning ? 'scanning…' : st.scan.last_scan_ago == null ? 'no scan yet' : 'scan ' + fmtAgo(st.scan.last_scan_ago);
  $('#hdr-device').textContent = st.device_name;
  $('#hdr-link').textContent = link;
  $('#hdr-scan').textContent = scan;
}

const presentNetworks = () => state.networks.filter((n) => n.present);

// ---- dashboard -----------------------------------------------------------------

function renderDashboard() {
  const st = state.status;
  const set = (id, v) => { $(id).textContent = v; };
  if (st) {
    const s = st.summary;
    set('#w-aps', s.detected);
    set('#w-channels', s.channels_used);
    set('#w-strongest', s.strongest_rssi == null ? '—' : s.strongest_rssi + ' dBm');
    set('#w-open', s.open);
    set('#w-hidden', s.hidden);
    set('#w-new', s.new);
    set('#w-rssi', st.wifi.connected ? st.wifi.rssi + ' dBm' : '—');
    set('#w-uptime', fmtDuration(st.uptime));
    set('#w-alerts', st.alerts.unread);
    if (st.ble.available) set('#w-ble', st.ble.devices ?? '—');

    const sel = $('#dash-scan-interval');
    if (document.activeElement !== sel) {
      if (![...sel.options].some((o) => Number(o.value) === st.scan.interval)) {
        sel.append(el('option', { value: st.scan.interval }, st.scan.interval + ' s'));
      }
      sel.value = st.scan.interval;
    }
    let info;
    if (st.scan.scanning) info = 'scanning…';
    else if (st.scan.last_scan_ago == null) info = 'waiting for first scan';
    else if (!st.scan.auto) info = `last scan ${fmtAgo(st.scan.last_scan_ago)}, auto scan off`;
    else info = `last scan ${fmtAgo(st.scan.last_scan_ago)}, next in ~${Math.max(0, st.scan.interval - st.scan.last_scan_ago)}s`;
    $('#dash-scan-info').textContent = info;
  }
  charts.spectrum.setData(presentNetworks(), st ? st.scan.strong_rssi : null);
  if (st) {
    $('#spectrum-note').textContent = st.scan.width_reported
      ? 'Each network is drawn with the channel width reported by the scan: 20 MHz (±2 channels) or 40 MHz (primary + secondary channel).'
      : "Each network is drawn as a 20 MHz channel (±2 channels). This board's WiFi scan does not report the real channel width.";
  }
  renderChannelBars();
  renderTopTable();
}

$('#dash-scan-interval').addEventListener('change', async (e) => {
  try {
    await apiPost('/api/config', { scan_interval: Number(e.target.value) });
    schedulePoll(0);
  } catch (err) {
    alert(err.message);
  }
});

async function scanNow(btn) {
  btn.disabled = true;
  try {
    await apiPost('/api/scan');
    btn.textContent = 'Scanning…';
    schedulePoll(3500);
  } catch (err) {
    alert(err.message);
  } finally {
    setTimeout(() => { btn.disabled = false; btn.textContent = btn.dataset.label; }, 5000);
  }
}
for (const id of ['#dash-scan', '#btn-scan', '#wifi-rescan']) {
  const b = $(id);
  b.dataset.label = b.textContent;
  b.addEventListener('click', () => scanNow(b));
}

function channelStats(nets) {
  const ch = [];
  const last = nets.some((n) => n.channel === 14) ? 14 : 13;
  for (let c = 1; c <= last; c++) ch.push({ ch: c, aps: 0, strong: 0, sum: 0, max: null, min: null });
  const strongRssi = state.status ? state.status.scan.strong_rssi : -67;
  for (const n of nets) {
    const c = ch[n.channel - 1];
    if (!c) continue;
    c.aps++;
    c.sum += n.rssi;
    if (n.rssi >= strongRssi) c.strong++;
    c.max = c.max == null ? n.rssi : Math.max(c.max, n.rssi);
    c.min = c.min == null ? n.rssi : Math.min(c.min, n.rssi);
  }
  for (const c of ch) {
    c.avg = c.aps ? Math.round(c.sum / c.aps) : null;
    c.overlap = ch.filter((o) => o.ch !== c.ch && Math.abs(o.ch - c.ch) <= 4).reduce((a, o) => a + o.aps, 0);
  }
  return ch;
}

function renderChannelBars() {
  const stats = channelStats(presentNetworks());
  const maxAps = Math.max(1, ...stats.map((c) => c.aps));
  const chart = $('#chan-chart');
  chart.replaceChildren();
  for (const c of stats) {
    const main = c.ch === 1 || c.ch === 6 || c.ch === 11;
    const label = el('div', { class: 'chan-label' + (main ? ' main' : '') }, c.ch);
    const bar = el('div', { class: 'chan-bar', style: `width:${(c.aps / maxAps) * 100}%` });
    const text = el('div', { class: 'chan-text' },
      c.aps ? `${c.aps} AP · avg ${c.avg} · max ${c.max}` : '0 AP');
    chart.append(label, el('div', { class: 'chan-track' }, bar), text);
  }
}

// 20 / 40 MHz from the scan (ESP32); the ESP8266 scan cannot report it
function fmtWidth(n) {
  if (!n.width) return 'Unknown';
  return n.width === 40 ? `40 MHz ${n.secondary > 0 ? '+' : '−'}` : '20 MHz';
}
function widthTitle(n) {
  if (!n.width) return "This board's WiFi scan does not report the channel width";
  return n.width === 40 ? `Primary channel ${n.channel}, secondary ${n.channel + 4 * n.secondary}` : `Channel ${n.channel} only`;
}

function ssidCell(n) {
  const td = el('td', null, n.ssid ? n.ssid : el('span', { class: 'hidden-ssid' }, '(hidden)'));
  if (n.new) td.append(el('span', { class: 'tag' }, 'NEW'));
  return td;
}

function qualityCell(rssi) {
  const [, name, pct, color] = quality(rssi);
  return el('td', null, el('span', { class: 'q' },
    el('span', { class: 'q-bar' }, el('span', { class: 'q-fill', style: `display:block;width:${pct}%;background:${color}` })),
    name));
}

function securityCell(sec) {
  return el('td', sec === 'Open' ? { class: 'sec-open' } : null, sec);
}

function renderTopTable() {
  const rows = presentNetworks().sort((a, b) => b.rssi - a.rssi).slice(0, 8);
  const tbody = $('#top-table tbody');
  tbody.replaceChildren(...rows.map((n) => el('tr', null,
    ssidCell(n),
    el('td', { class: 'num' }, 'ch ' + n.channel),
    el('td', { class: 'num' }, n.rssi + ' dBm'),
    qualityCell(n.rssi),
    securityCell(n.security))));
  if (!rows.length) tbody.append(el('tr', null, el('td', { class: 'muted' }, 'No networks detected yet.')));
}

// ---- networks page -------------------------------------------------------------

function filteredNetworks() {
  const q = $('#f-search').value.trim().toLowerCase();
  const chan = $('#f-channel').value;
  const strongOnly = $('#f-strong').checked;
  const newOnly = $('#f-new').checked;
  const includeGone = $('#f-gone').checked;
  const strongRssi = state.status ? state.status.scan.strong_rssi : -67;
  const { key, dir } = state.sort;

  return state.networks
    .filter((n) => includeGone || n.present)
    .filter((n) => !chan || n.channel === Number(chan))
    .filter((n) => !strongOnly || n.rssi >= strongRssi)
    .filter((n) => !newOnly || n.new)
    .filter((n) => !q || [n.ssid, n.bssid, n.security].some((v) => v.toLowerCase().includes(q)))
    .sort((a, b) => {
      const x = a[key], y = b[key];
      const c = typeof x === 'string' ? x.localeCompare(y) : x - y;
      return c * dir;
    });
}

function renderNetworks() {
  // channel filter options follow the channels actually seen
  const sel = $('#f-channel');
  const chans = [...new Set(state.networks.map((n) => n.channel))].sort((a, b) => a - b);
  const existing = $$('option', sel).slice(1).map((o) => Number(o.value));
  if (chans.join() !== existing.join()) {
    const cur = sel.value;
    sel.replaceChildren(el('option', { value: '' }, 'All channels'), ...chans.map((c) => el('option', { value: c }, 'Channel ' + c)));
    sel.value = chans.includes(Number(cur)) ? cur : '';
  }

  for (const th of $$('#net-table th[data-sort]')) {
    th.classList.toggle('asc', th.dataset.sort === state.sort.key && state.sort.dir === 1);
    th.classList.toggle('desc', th.dataset.sort === state.sort.key && state.sort.dir === -1);
  }

  const rows = filteredNetworks();
  const pages = Math.max(1, Math.ceil(rows.length / state.pageSize));
  state.pageIndex = Math.min(state.pageIndex, pages - 1);
  const start = state.pageIndex * state.pageSize;
  const pageRows = rows.slice(start, start + state.pageSize);

  $('#net-table tbody').replaceChildren(...pageRows.map((n) => {
    const tr = el('tr', n.present ? null : { class: 'gone' },
      ssidCell(n),
      el('td', { class: 'mono' }, n.bssid),
      el('td', { class: 'num' }, n.channel),
      el('td', { class: 'num', title: widthTitle(n) }, fmtWidth(n)),
      el('td', { class: 'num' }, n.rssi),
      el('td', { class: 'num' }, n.rssi_min),
      el('td', { class: 'num' }, n.rssi_max),
      el('td', { class: 'num' }, n.rssi_avg),
      qualityCell(n.rssi),
      securityCell(n.security),
      el('td', { class: 'num' }, n.seen),
      el('td', { class: 'num' }, fmtAgo(n.first_seen_ago)),
      el('td', { class: 'num' }, fmtAgo(n.last_seen_ago)));
    tr.addEventListener('click', () => {
      state.hist.bssid = n.bssid;
      showPage('history', true, '?bssid=' + encodeURIComponent(n.bssid));
    });
    return tr;
  }));

  $('#pager-info').textContent = rows.length
    ? `${start + 1}–${start + pageRows.length} of ${rows.length} networks`
    : 'No networks match the filters.';
  $('#pg-prev').disabled = state.pageIndex === 0;
  $('#pg-next').disabled = state.pageIndex >= pages - 1;
}

$('#net-table thead').addEventListener('click', (e) => {
  const th = e.target.closest('th[data-sort]');
  if (!th) return;
  const key = th.dataset.sort;
  // signal/count columns start descending (strongest first), the rest ascending
  state.sort = state.sort.key === key
    ? { key, dir: -state.sort.dir }
    : { key, dir: ['rssi', 'rssi_min', 'rssi_max', 'rssi_avg', 'seen'].includes(key) ? -1 : 1 };
  renderNetworks();
});
for (const id of ['#f-search', '#f-channel', '#f-strong', '#f-new', '#f-gone']) {
  $(id).addEventListener('input', () => { state.pageIndex = 0; renderNetworks(); });
}
$('#pg-prev').addEventListener('click', () => { state.pageIndex--; renderNetworks(); });
$('#pg-next').addEventListener('click', () => { state.pageIndex++; renderNetworks(); });

function download(name, type, text) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = el('a', { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
const exportName = (ext) => `wifi-scan-${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.${ext}`;

$('#btn-csv').addEventListener('click', () => {
  const esc = (v) => {
    const s = String(v ?? '');
    return /[",\n\r]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  };
  const lines = ['timestamp,ssid,bssid,channel,rssi,security'];
  for (const n of filteredNetworks()) {
    lines.push([n.last_seen || '', n.ssid, n.bssid, n.channel, n.rssi, n.security].map(esc).join(','));
  }
  download(exportName('csv'), 'text/csv', lines.join('\r\n') + '\r\n');
});

$('#btn-json').addEventListener('click', () => {
  const data = {
    timestamp: (state.status && state.status.time) || new Date().toISOString(),
    device: state.status ? state.status.device_name : null,
    networks: filteredNetworks(),
  };
  download(exportName('json'), 'application/json', JSON.stringify(data, null, 2));
});

// ---- channels page -------------------------------------------------------------

function renderChannels() {
  const nets = presentNetworks();
  const stats = channelStats(nets);
  const strongRssi = state.status ? state.status.scan.strong_rssi : -67;
  charts.bars.setData(stats.map((c) => String(c.ch)), [
    { name: `Weaker (< ${strongRssi} dBm)`, slot: 0, values: stats.map((c) => c.aps - c.strong) },
    { name: `Strong (≥ ${strongRssi} dBm)`, slot: 1, values: stats.map((c) => c.strong) },
  ]);
  charts.range.setData(stats.map((c) => ({ cat: String(c.ch), min: c.min, avg: c.avg, max: c.max })));

  const est = state.chan.data;
  const estOf = (ch) => (est ? est.channels.find((x) => x.channel === ch) : null);
  $('#chan-table tbody').replaceChildren(...stats.map((c) => el('tr', null,
    el('td', { class: 'num' }, c.ch),
    el('td', { class: 'num' }, c.ch === 14 ? 2484 : 2407 + 5 * c.ch),
    el('td', { class: 'num' }, c.aps),
    el('td', { class: 'num' }, c.strong),
    el('td', { class: 'num' }, c.avg ?? '—'),
    el('td', { class: 'num' }, c.max ?? '—'),
    el('td', { class: 'num' }, c.min ?? '—'),
    el('td', { class: 'num' }, c.overlap),
    el('td', { class: 'num' }, estOf(c.ch) ? estOf(c.ch).load.toFixed(1) : '—'),
    el('td', null, estOf(c.ch) ? congestionChip(estOf(c.ch).congestion) : '—'))));
  renderReco();
  const sid = state.status ? state.status.scan.id : -1;
  if (sid !== state.chan.scanId) {
    state.chan.scanId = sid;
    apiGet('/api/channels').then((d) => { state.chan.data = d; if (state.page === 'channels') renderChannels(); })
      .catch(() => { state.chan.scanId = -1; });
  }

  const count = (f) => nets.filter(f).length;
  const tiles = [
    ['Open', count((n) => n.security === 'Open')],
    ['WEP', count((n) => n.security === 'WEP')],
    ['WPA', count((n) => n.security === 'WPA')],
    ['WPA2', count((n) => n.security === 'WPA2')],
    ['WPA/WPA2', count((n) => n.security === 'WPA/WPA2')],
    ['Hidden SSID', count((n) => n.hidden)],
  ];
  $('#sec-widgets').replaceChildren(...tiles.map(([k, v]) =>
    el('div', { class: 'widget' }, el('div', { class: 'w-label' }, k), el('div', { class: 'w-value' }, v))));
}

function congestionChip(level) {
  return el('span', { class: 'cong cong-' + level.split(' ')[0] }, level);
}

function renderReco() {
  const d = state.chan.data;
  const box = $('#chan-reco');
  if (!d) { box.replaceChildren(el('span', { class: 'muted' }, 'Analysing channels…')); return; }
  const rec = d.channels.find((c) => c.channel === d.recommended);
  box.replaceChildren(
    el('div', null, el('div', { class: 'w-label' }, 'Recommended channel'), el('div', { class: 'reco-big' }, d.recommended)),
    el('div', { class: 'reco-text' },
      el('div', null, el('b', null, `Channel ${d.recommended}`), ' has the lowest estimated load among 1, 6 and 11 (',
        congestionChip(rec.congestion), `, load ${rec.load.toFixed(1)}).`),
      el('div', null, `Least populated channel: ${d.least_populated} (fewest detected APs in the latest scan).`),
      el('div', null, `Confidence: ${d.confidence}, averaged over ${d.scans_used} scan${d.scans_used === 1 ? '' : 's'}.`),
      el('div', { class: 'muted small' }, 'A recommendation from detected access points, their signal strength and ',
        'channel overlap. It is not an RF measurement: non-WiFi interference and real airtime are not visible to this device.')));
}

// ---- alerts ----------------------------------------------------------------------

const ALERT_GROUPS = {
  ap: ['new_ap', 'open_ap', 'strong_ap'],
  channel: ['channel_density'],
};
const SEVERITY = {
  info: ['ℹ', 'Info'], warning: ['▲', 'Warning'], critical: ['■', 'Critical'],
};

function sevChip(sev) {
  const [icon, label] = SEVERITY[sev] || SEVERITY.info;
  return el('span', { class: 'sev sev-' + sev }, el('span', { 'aria-hidden': 'true' }, icon), label);
}

function alertTime(a) {
  if (a.epoch) return Charts.fmtDateTime(a.epoch * 1000);
  if (a.ago != null) return fmtAgo(a.ago);
  return 'before the last restart';
}

function updateBadge(n) {
  const b = $('#nav-badge');
  b.hidden = !n;
  b.textContent = n > 99 ? '99+' : n;
}

async function syncAlerts(st) {
  updateBadge(st.alerts.unread);
  if (st.alerts.last_id === state.alerts.loadedId || state.alerts.fetching) return;
  state.alerts.fetching = true;
  try {
    const r = await apiGet('/api/alerts');
    const prev = state.alerts.loadedId;
    state.alerts.list = r.alerts;
    state.alerts.loadedId = r.last_id;
    // Pop-ups only for alerts that arrived while the page was open
    if (prev >= 0) r.alerts.filter((a) => a.id > prev).slice(0, 3).reverse().forEach(toast);
  } catch (_) { /* next poll retries */ }
  state.alerts.fetching = false;
}

function toast(a) {
  const t = el('div', { class: 'toast ' + a.severity, role: 'status' }, sevChip(a.severity), el('div', null, a.message));
  t.addEventListener('click', () => { t.remove(); showPage('alerts', true); });
  $('#toasts').append(t);
  setTimeout(() => t.remove(), 9000);
}

function renderAlerts() {
  const st = state.status;
  if (st) $('#alert-topic').textContent = st.mqtt.base_topic + '/alerts';
  const group = $('#alert-filter').value;
  const unreadOnly = $('#alert-unread').checked;
  const list = state.alerts.list.filter((a) => {
    if (unreadOnly && !a.unread) return false;
    if (!group) return true;
    if (group === 'system') return !ALERT_GROUPS.ap.includes(a.type) && !ALERT_GROUPS.channel.includes(a.type);
    return ALERT_GROUPS[group].includes(a.type);
  });
  const unread = state.alerts.list.filter((a) => a.unread).length;
  $('#alert-count').textContent = `${state.alerts.list.length} alerts, ${unread} unread`;
  $('#alert-ack').disabled = !unread;
  $('#alert-list').replaceChildren(...list.map((a) => el('div', { class: 'alert-item' + (a.unread ? ' unread' : '') },
    sevChip(a.severity),
    el('div', { class: 'alert-body' },
      el('div', { class: 'alert-msg' }, a.message),
      el('div', { class: 'alert-meta muted small' }, `${alertTime(a)} · ${a.type}`)))));
  if (!list.length) {
    $('#alert-list').append(el('div', { class: 'muted', style: 'padding:12px 4px' },
      state.alerts.list.length ? 'No alerts match the filter.' : 'No alerts yet.'));
  }
}

$('#alert-filter').addEventListener('input', renderAlerts);
$('#alert-unread').addEventListener('input', renderAlerts);
$('#alert-ack').addEventListener('click', async () => {
  try {
    await apiPost('/api/alerts/ack');
    state.alerts.loadedId = -2;  // same last_id, but the unread flags changed: refetch
    schedulePoll(0);
  } catch (err) {
    alert(err.message);
  }
});

const alertForm = $('#alert-form');

async function loadAlertSettings() {
  try {
    const c = await apiGet('/api/config');
    for (const k of ['alert_new_ap', 'alert_open_ap', 'alert_strong_ap', 'alert_density', 'alert_system']) {
      alertForm[k].checked = c[k];
    }
    alertForm.alert_strong_rssi.value = c.alert_strong_rssi;
    alertForm.alert_density_aps.value = c.alert_density_aps;
  } catch (err) {
    $('#alert-msg').textContent = 'Could not load settings: ' + err.message;
  }
}

alertForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = {};
  for (const k of ['alert_new_ap', 'alert_open_ap', 'alert_strong_ap', 'alert_density', 'alert_system']) {
    body[k] = alertForm[k].checked;
  }
  body.alert_strong_rssi = Number(alertForm.alert_strong_rssi.value);
  body.alert_density_aps = Number(alertForm.alert_density_aps.value);
  const m = $('#alert-msg');
  try {
    await apiPost('/api/config', body);
    m.className = 'small msg-ok';
    m.textContent = 'Saved.';
  } catch (err) {
    m.className = 'small msg-err';
    m.textContent = err.message;
  }
});

// ---- history page --------------------------------------------------------------

const RANGE_LABELS = { live: 'Live', 1: '1 h', 6: '6 h', 12: '12 h', 24: '24 h', 72: '3 days', 168: '7 days' };

function renderHistory() {
  const st = state.status;
  if (!st) return;
  const maxHours = st.history.hours;
  const ranges = ['live', ...[1, 6, 12, 24, 72, 168].filter((h) => h <= maxHours)];
  if (!ranges.map(String).includes(String(state.hist.range))) state.hist.range = 'live';

  const seg = $('#hist-range');
  const segKey = ranges.join();
  if (seg.dataset.key !== segKey) {
    seg.dataset.key = segKey;
    seg.replaceChildren(...ranges.map((r) => {
      const b = el('button', { type: 'button', 'data-range': r }, RANGE_LABELS[r]);
      b.addEventListener('click', () => {
        state.hist.range = r;
        state.hist.loadedKey = null;
        renderHistory();
      });
      return b;
    }));
  }
  for (const b of $$('button', seg)) b.classList.toggle('active', b.dataset.range === String(state.hist.range));

  // AP picker: every tracked AP, strongest first
  const sel = $('#hist-ap');
  const nets = [...state.networks].sort((a, b) => b.rssi - a.rssi);
  const optKey = nets.map((n) => n.bssid).join();
  if (sel.dataset.key !== optKey) {
    sel.dataset.key = optKey;
    sel.replaceChildren(...nets.map((n) => el('option', { value: n.bssid },
      `${n.ssid || '(hidden)'} · ${n.bssid} · ch ${n.channel}`)));
  }
  if (!state.hist.bssid || !nets.some((n) => n.bssid === state.hist.bssid)) {
    state.hist.bssid = nets.length ? nets[0].bssid : null;
  }
  if (state.hist.bssid) sel.value = state.hist.bssid;

  const live = state.hist.range === 'live';
  const note = $('#hist-note');
  note.hidden = live || st.history.persistent;
  note.textContent = 'Stored history starts once the device knows the time (NTP). Connect it to a network with internet access; until then only the Live view has data.';
  $('#hist-info').textContent = live
    ? `Last ${st.history.live_points} scans, kept in RAM`
    : `Saved every ${st.history.bucket_s / 60} min · retention ${RANGE_LABELS[maxHours] || maxHours + ' h'}`;

  // Reload when the range/AP changes, after each scan in live mode, or every
  // minute for stored ranges (they only change once per bucket anyway).
  const key = `${state.hist.range}|${state.hist.bssid}|${live ? st.scan.id : ''}`;
  const stale = !live && Date.now() - state.hist.loadedAt > 60000;
  if (key !== state.hist.loadedKey || stale) {
    state.hist.loadedKey = key;
    state.hist.loadedAt = Date.now();
    loadHistory().catch((err) => setBanner('History: ' + err.message));
  }
}

$('#hist-ap').addEventListener('change', (e) => {
  state.hist.bssid = e.target.value;
  history.replaceState({ page: 'history' }, '', '/history?bssid=' + encodeURIComponent(e.target.value));
  renderHistory();
});

async function loadHistory() {
  const range = state.hist.range;
  const bssid = state.hist.bssid;
  const now = Date.now();
  let summary, ap = [];
  if (range === 'live') {
    const r = await apiGet('/api/history/live');
    summary = r.points.map((p) => ({ t: now - p[0] * 1000, aps: p[1], open: p[2], hidden: p[3], strongest: p[4], avg: p[5], ch: p[6] }));
    if (bssid) {
      const a = await apiGet('/api/history/ap?live=1&bssid=' + encodeURIComponent(bssid)).catch(() => ({ points: [] }));
      ap = a.points.map((p) => [now - p[0] * 1000, p[1]]);
    }
  } else {
    const r = await apiGet('/api/history?hours=' + range);
    summary = r.points.map((p) => ({ t: p[0] * 1000, aps: p[1], open: p[2], hidden: p[3], strongest: p[4], avg: p[5], ch: p[6] }));
    if (bssid) {
      const a = await apiGet(`/api/history/ap?hours=${range}&bssid=${encodeURIComponent(bssid)}`);
      ap = a.points.map((p) => [p[0] * 1000, p[1]]);
    }
  }
  const series = (name, slot, f) => ({ name, slot, points: summary.map((p) => [p.t, f(p)]) });
  charts.histCount.setData([
    series('All APs', 0, (p) => p.aps),
    series('Open', 1, (p) => p.open),
    series('Hidden', 2, (p) => p.hidden),
  ]);
  charts.histRssi.setData([
    series('Strongest AP', 0, (p) => p.strongest),
    series('Average of APs', 1, (p) => p.avg),
  ]);
  charts.heat.setData(summary.map((p) => p.t), summary.map((p) => (p.ch || []).slice(0, 13)));
  const n = state.networks.find((x) => x.bssid === bssid);
  charts.histAp.setData(n ? [{ name: n.ssid || '(hidden)', slot: 0, points: ap }] : []);

  const vals = ap.map((p) => p[1]).filter((v) => v != null);
  const kv = n ? [
    ['SSID', n.ssid || '(hidden)'], ['Channel', n.channel], ['Now', n.present ? n.rssi + ' dBm' : 'not visible'],
    ['Min', (vals.length ? Math.min(...vals) : n.rssi_min) + ' dBm'],
    ['Max', (vals.length ? Math.max(...vals) : n.rssi_max) + ' dBm'],
    ['Average', (vals.length ? Math.round(vals.reduce((a, b) => a + b, 0) / vals.length) : n.rssi_avg) + ' dBm'],
    ['Seen', n.seen + '×'], ['First seen', fmtAgo(n.first_seen_ago)],
  ] : [];
  $('#hist-ap-stats').replaceChildren(...kv.flatMap(([k, v]) => [el('dt', null, k), el('dd', null, v)]));
}

// ---- WiFi setup page -----------------------------------------------------------

const ATTEMPT_TEXT = {
  wrong_password: 'Wrong password.',
  no_ssid: 'Network not found. It may be out of range or on 5 GHz (this device supports 2.4 GHz only).',
  failed: 'Could not connect.',
};

function signalBars(rssi) {
  const lvl = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return el('span', { class: 'wifi-bars', title: rssi + ' dBm' },
    ...[5, 8, 11, 14].map((hgt, i) => el('i', { class: i < lvl ? 'on' : '', style: `height:${hgt}px` })));
}

function renderWifi() {
  const st = state.status;
  if (st) {
    const w = st.wifi;
    const cur = $('#wifi-current');
    if (w.connected) {
      cur.replaceChildren('Connected to ', el('b', null, w.ssid), ` · IP ${w.ip} · ${w.rssi} dBm · `,
        el('a', { href: `http://${w.hostname}.local/` }, `${w.hostname}.local`));
    } else if (w.ap_active) {
      cur.replaceChildren('Not connected to a network. You are using the setup hotspot ', el('b', null, w.ap_ssid),
        ` (${w.ap_ip}). Pick your network below.`);
    } else {
      cur.replaceChildren('Not connected' + (w.ssid ? ` (trying ${w.ssid})` : '') + '.');
    }
    renderWifiProgress(w);
  }

  // Keep the list stable while a password is being typed.
  if (state.wifi.selected) return;
  const best = new Map();
  for (const n of presentNetworks()) {
    if (!n.ssid) continue;
    const b = best.get(n.ssid);
    if (!b || n.rssi > b.rssi) best.set(n.ssid, n);
  }
  const list = [...best.values()].sort((a, b) => b.rssi - a.rssi);
  const key = list.map((n) => n.ssid + n.security).join('|');
  if (key === state.wifi.listKey) return;
  state.wifi.listKey = key;
  const current = st && st.wifi.connected ? st.wifi.ssid : null;
  $('#wifi-list').replaceChildren(...list.map((n) => wifiItem(n, n.ssid === current)));
  if (!list.length) $('#wifi-list').append(el('div', { class: 'wifi-row muted' }, 'No networks found yet. Press Rescan.'));
}

function wifiItem(n, isCurrent) {
  const item = el('div', { class: 'wifi-item' });
  const row = el('button', { type: 'button', class: 'wifi-row' },
    signalBars(n.rssi),
    el('span', { class: 'wifi-name' }, n.ssid),
    el('span', { class: 'wifi-meta' }, `${isCurrent ? 'connected · ' : ''}${n.security === 'Open' ? 'open' : '🔒 ' + n.security} · ch ${n.channel}`));
  row.addEventListener('click', () => {
    const open = item.querySelector('.wifi-join');
    for (const j of $$('.wifi-join')) j.remove();
    if (open) { state.wifi.selected = null; return; }
    state.wifi.selected = n.ssid;
    const pass = el('input', { type: 'password', placeholder: 'Password', maxlength: '63', autocomplete: 'current-password' });
    const btn = el('button', { type: 'submit' }, 'Connect');
    const cancel = el('button', { type: 'button', class: 'secondary' }, 'Cancel');
    const form = el('form', { class: 'wifi-join' });
    if (n.security !== 'Open') form.append(pass);
    form.append(btn, cancel);
    cancel.addEventListener('click', () => { form.remove(); state.wifi.selected = null; state.wifi.listKey = ''; renderWifi(); });
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      if (n.security !== 'Open' && pass.value.length < 8) {
        pass.setCustomValidity('At least 8 characters');
        pass.reportValidity();
        return;
      }
      connectWifi(n.ssid, n.security === 'Open' ? '' : pass.value);
    });
    pass.addEventListener('input', () => pass.setCustomValidity(''));
    item.append(form);
    if (n.security !== 'Open') pass.focus();
  });
  item.append(row);
  return item;
}

$('#wifi-manual-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const f = e.target;
  if (f.password.value && f.password.value.length < 8) {
    f.password.setCustomValidity('At least 8 characters, or empty for an open network');
    f.password.reportValidity();
    return;
  }
  connectWifi(f.ssid.value, f.password.value);
});
$('#wifi-manual-form').password.addEventListener('input', (e) => e.target.setCustomValidity(''));

async function connectWifi(ssid, password) {
  const box = $('#wifi-progress');
  try {
    await apiPost('/api/wifi/connect', { ssid, password });
    state.wifi.connecting = true;
    state.wifi.target = ssid;
    box.hidden = false;
    box.className = 'wifi-progress';
    box.replaceChildren(`Connecting to ${ssid}… this takes up to 20 seconds.`);
    schedulePoll(1500);
  } catch (err) {
    box.hidden = false;
    box.className = 'wifi-progress err';
    box.replaceChildren(err.message);
  }
}

function renderWifiProgress(w) {
  if (!state.wifi.connecting) return;
  const box = $('#wifi-progress');
  if (w.attempt_ssid !== state.wifi.target || w.attempt_state === 'connecting') return;
  state.wifi.connecting = false;
  state.wifi.selected = null;
  state.wifi.listKey = '';
  for (const j of $$('.wifi-join')) j.remove();
  if (w.attempt_state === 'connected' && w.connected) {
    const url = `http://${w.ip}/`;
    box.className = 'wifi-progress ok';
    box.replaceChildren(
      el('b', null, `Connected to ${w.ssid}.`), el('br'),
      'On your local network the monitor is at ', el('a', { href: url }, url),
      ' (or ', el('a', { href: `http://${w.hostname}.local/` }, `http://${w.hostname}.local`), ').', el('br'),
      'The setup hotspot switches off in a few minutes. Reconnect this phone/PC to your normal WiFi and open that address.');
  } else {
    box.className = 'wifi-progress err';
    box.replaceChildren(ATTEMPT_TEXT[w.attempt_state] || 'Could not connect.', ' The previous settings are still in use.');
  }
  schedulePoll();
}

// ---- Update (OTA) page -------------------------------------------------------------

const upd = { fw: null, fwInfo: null, web: [], waitingFor: null };

function renderUpdate() {
  const st = state.status;
  if (!st) return;
  $('#fw-current').replaceChildren(
    el('dt', null, 'Running'), el('dd', null, `${st.firmware} (${fmtStamp(st.build_time) || '—'})`),
    el('dt', null, 'Board'), el('dd', null, `${st.system.board} (${st.system.chip})`),
    el('dt', null, 'Space for update'), el('dd', null, fmtBytes(st.system.free_sketch)));
  if (upd.waitingFor) {
    if (st.firmware === upd.waitingFor) {
      fwMsg(`Update complete: now running ${st.firmware}.`, true);
      upd.waitingFor = null;
      setProgress('#fw-progress', null);
    }
  }
}

function setProgress(sel, pct, label) {
  const p = $(sel);
  if (pct == null) { p.hidden = true; return; }
  p.hidden = false;
  p.querySelector('.progress-fill').style.width = pct + '%';
  p.querySelector('.progress-pct').textContent = label || Math.round(pct) + '%';
}

function fwMsg(text, ok) {
  const m = $('#fw-msg');
  m.className = 'small ' + (ok === true ? 'msg-ok' : ok === false ? 'msg-err' : 'muted');
  m.textContent = text;
}

// Reads "ESPWM-FW|<version>|<board>|" from the image (see System kFirmwareTag).
function readFirmwareTag(bytes) {
  const prefix = [...'ESPWM-FW|'].map((c) => c.charCodeAt(0));
  outer: for (let i = 0; i < bytes.length - prefix.length; i++) {
    for (let j = 0; j < prefix.length; j++) if (bytes[i + j] !== prefix[j]) continue outer;
    let s = '';
    for (let k = i + prefix.length; k < bytes.length && bytes[k] !== 0 && s.length < 40; k++) s += String.fromCharCode(bytes[k]);
    const [version, board] = s.split('|');
    if (version && /^\d/.test(version)) return { version, board };
  }
  return null;
}

$('#fw-file').addEventListener('change', async (e) => {
  const f = e.target.files[0];
  upd.fw = null;
  $('#fw-upload').disabled = true;
  fwMsg('');
  const info = $('#fw-info');
  if (!f) { info.textContent = ''; return; }
  const bytes = new Uint8Array(await f.arrayBuffer());
  const st = state.status;
  const tag = readFirmwareTag(bytes);
  let problem = null;
  if (bytes[0] !== 0xe9) problem = 'This is not an ESP firmware image (.bin).';
  else if (!tag) problem = 'This file is not ESP WiFi Monitor firmware (no version tag).';
  else if (st && tag.board !== st.system.board_id) {
    problem = `This firmware is for board "${tag.board}"; this device is ${st.system.board}.`;
  } else if (st && f.size > st.system.free_sketch) problem = 'The file is larger than the space for updates.';
  info.replaceChildren(
    tag ? `Selected: version ${tag.version} for ${tag.board}, ${fmtBytes(f.size)}` : `Selected: ${f.name}, ${fmtBytes(f.size)}`,
    st && tag && tag.version === st.firmware ? ' (same version as running)' : '');
  if (problem) { fwMsg(problem, false); return; }
  upd.fw = f;
  upd.fwInfo = tag;
  $('#fw-upload').disabled = false;
});

$('#fw-upload').addEventListener('click', () => {
  const f = upd.fw;
  if (!f || !confirm(`Install firmware ${upd.fwInfo.version}? The device restarts afterwards.`)) return;
  $('#fw-upload').disabled = true;
  const fd = new FormData();
  fd.append('file', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/update/firmware');
  xhr.setRequestHeader('X-Requested-With', 'wifi-monitor');
  xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) setProgress('#fw-progress', (ev.loaded / ev.total) * 100); };
  xhr.onload = () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (_) { /* not JSON */ }
    if (xhr.status === 200) {
      setProgress('#fw-progress', 100, 'Installed, restarting…');
      fwMsg(`Firmware ${r.version} installed. The device is restarting; this page reconnects by itself.`, true);
      upd.waitingFor = r.version;
    } else {
      setProgress('#fw-progress', null);
      fwMsg('Update failed: ' + (r.error || xhr.status + ' ' + xhr.statusText), false);
      $('#fw-upload').disabled = false;
    }
  };
  xhr.onerror = () => { fwMsg('Upload failed: connection lost.', false); $('#fw-upload').disabled = false; };
  fwMsg('Uploading… do not close this page.');
  xhr.send(fd);
});

const WEB_PATH = /^\/(index\.html|embed\.html|favicon\.ico|css\/[\w.-]+\.css|js\/[\w.-]+\.js)$/;

$('#web-dir').addEventListener('change', (e) => {
  // "data/js/app.js" -> "/js/app.js"; everything else in the folder is ignored
  upd.web = [...e.target.files]
    .map((f) => ({ f, path: '/' + (f.webkitRelativePath || f.name).split('/').slice(1).join('/') }))
    .filter((x) => WEB_PATH.test(x.path))
    .sort((a, b) => (a.path.endsWith('.html') ? 1 : 0) - (b.path.endsWith('.html') ? 1 : 0));  // pages last
  const total = upd.web.reduce((s, x) => s + x.f.size, 0);
  $('#web-info').textContent = upd.web.length
    ? `${upd.web.length} files, ${fmtBytes(total)}: ${upd.web.map((x) => x.path).join(', ')}`
    : 'No web interface files found in this folder. Choose the project\'s data folder.';
  $('#web-upload').disabled = !upd.web.length;
  $('#web-msg').textContent = '';
});

$('#web-upload').addEventListener('click', async () => {
  const btn = $('#web-upload');
  btn.disabled = true;
  const m = $('#web-msg');
  m.className = 'small muted';
  let done = 0;
  try {
    for (const x of upd.web) {
      setProgress('#web-progress', (done / upd.web.length) * 100, `${done}/${upd.web.length} ${x.path}`);
      const fd = new FormData();
      fd.append('file', x.f, x.path.split('/').pop());
      await request('/api/update/webfile?path=' + encodeURIComponent(x.path), {
        method: 'POST', headers: { 'X-Requested-With': 'wifi-monitor' }, body: fd,
      });
      done++;
    }
    setProgress('#web-progress', 100, `${done}/${upd.web.length} uploaded`);
    m.className = 'small msg-ok';
    m.replaceChildren(`All ${done} files uploaded. `);
    const b = el('button', { type: 'button' }, 'Reload page');
    b.addEventListener('click', () => location.reload());
    m.append(b, ' (the browser may still show cached files; use Ctrl+Shift+R if so)');
  } catch (err) {
    m.className = 'small msg-err';
    m.textContent = `Stopped after ${done} of ${upd.web.length} files: ${err.message}`;
    btn.disabled = false;
  }
});

// ---- dashboard customization (per browser) ------------------------------------------

function dashItems() {
  const items = [];
  for (const w of $$('#page-dashboard .widgets .widget')) {
    if (w.id === 'w-ble-box' && w.hidden) continue;  // no BLE on this board
    items.push({ el: w, name: $('.w-label', w).textContent });
  }
  for (const c of $$('#page-dashboard > .card:not(.w-panel)')) items.push({ el: c, name: $('h2', c).textContent });
  return items;
}

function applyDashPrefs() {
  let hidden = [];
  try { hidden = JSON.parse(storage('hiddenWidgets') || '[]'); } catch (_) { /* reset */ }
  for (const it of dashItems()) it.el.classList.toggle('w-hidden', hidden.includes(it.name));
  return hidden;
}

$('#w-custom').addEventListener('click', () => {
  const panel = $('#w-panel');
  panel.hidden = !panel.hidden;
  if (panel.hidden) return;
  const hidden = applyDashPrefs();
  $('#w-checks').replaceChildren(...dashItems().map((it) => {
    const cb = el('input', { type: 'checkbox' });
    cb.checked = !hidden.includes(it.name);
    cb.addEventListener('change', () => {
      const now = dashItems().filter((x) => x.name !== it.name && x.el.classList.contains('w-hidden')).map((x) => x.name);
      if (!cb.checked) now.push(it.name);
      storage('hiddenWidgets', JSON.stringify(now));
      applyDashPrefs();
      charts.spectrum.draw();
    });
    return el('label', null, cb, it.name);
  }));
});
applyDashPrefs();

// ---- BLE page ---------------------------------------------------------------------

const bleForm = $('#ble-form');

function renderBle() {
  const st = state.status;
  if (!st || !st.ble.available) return;
  const b = st.ble;
  const kv = [
    ['State', b.enabled ? (b.advertising ? 'on, advertising' : 'on') : 'off'],
    ['Name', b.name],
    ['BLE address', b.address || '—'],
    ['BTHome', b.bthome ? 'advertising (Home Assistant)' : 'off'],
    ['Scan', b.scan ? (b.last_scan_ago == null ? 'waiting for the first scan' : `${b.devices} devices, ${fmtAgo(b.last_scan_ago)}`) : 'off'],
  ];
  $('#ble-kv').replaceChildren(...kv.flatMap(([k, v]) => [el('dt', null, k), el('dd', null, v)]));
  if (b.scan_id !== state.ble.scanId) {
    state.ble.scanId = b.scan_id;
    apiGet('/api/ble').then((r) => {
      state.ble.devices = r.devices;
      state.ble.history = r.history || [];
      state.ble.fetchedAt = Date.now();
      renderBleTable();
      renderBleCharts();
    }).catch(() => { state.ble.scanId = -1; });
  } else {
    renderBleTable();
  }
}

function renderBleTable() {
  const presentOnly = $('#ble-present').checked;
  const rows = state.ble.devices.filter((d) => !presentOnly || d.present).sort((a, b) => b.rssi - a.rssi);
  $('#ble-table tbody').replaceChildren(...rows.map((d) => el('tr', d.present ? null : { class: 'gone' },
    el('td', { class: 'mono' }, d.address + (d.random ? ' *' : '')),
    el('td', null, d.name || el('span', { class: 'hidden-ssid' }, '(no name)')),
    el('td', null, d.company || '—'),
    el('td', { class: 'num' }, d.rssi),
    qualityCell(d.rssi),
    el('td', { class: 'num' }, d.seen),
    el('td', { class: 'num' }, fmtAgo(d.last_seen_ago)))));
  if (!rows.length) $('#ble-table tbody').append(el('tr', null, el('td', { class: 'muted' }, 'No BLE devices yet.')));
}
$('#ble-present').addEventListener('input', renderBleTable);

// Label for a BLE device: its name, else the maker, else the address
const bleLabel = (d) => d.name || (d.company ? `${d.company} device` : d.address);

function renderBleCharts() {
  // Bars: devices from the latest scan, strongest first (-100 dBm empty .. -30 dBm full)
  const rows = state.ble.devices.filter((d) => d.present).sort((a, b) => b.rssi - a.rssi).slice(0, 25);
  const pct = (rssi) => Math.max(2, Math.min(100, ((rssi + 100) / 70) * 100));
  const bars = $('#ble-bars');
  bars.replaceChildren(...rows.flatMap((d) => [
    el('div', { class: 'rssi-label', title: `${d.address}${d.random ? ' (random address)' : ''}` }, bleLabel(d)),
    el('div', { class: 'rssi-track' }, el('div', { class: 'rssi-fill', style: `width:${pct(d.rssi)}%` })),
    el('div', { class: 'rssi-val' }, `${d.rssi} dBm`),
  ]));
  if (!rows.length) bars.append(el('div', { class: 'muted' }, 'No BLE devices in the latest scan.'));

  // Line: strongest and average RSSI per BLE scan
  const t0 = state.ble.fetchedAt || Date.now();
  const h = state.ble.history;
  charts.bleHist.setData([
    { name: 'Strongest device', slot: 0, points: h.map((p) => [t0 - p[0] * 1000, p[2]]) },
    { name: 'Average of devices', slot: 1, points: h.map((p) => [t0 - p[0] * 1000, p[3]]) },
  ]);
  const counts = h.map((p) => p[1]);
  $('#ble-hist-info').textContent = h.length
    ? `last ${h.length} BLE scans · ${Math.min(...counts)}–${Math.max(...counts)} devices per scan`
    : '';
}

async function loadBleSettings() {
  try {
    const c = await apiGet('/api/config');
    bleForm.ble_enabled.checked = c.ble_enabled;
    bleForm.ble_name.value = c.ble_name;
    bleForm.ble_bthome.checked = c.ble_bthome;
    bleForm.ble_scan.checked = c.ble_scan;
    bleForm.ble_scan_interval.value = c.ble_scan_interval;
  } catch (err) {
    $('#ble-msg').textContent = 'Could not load settings: ' + err.message;
  }
}

bleForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const m = $('#ble-msg');
  try {
    const r = await apiPost('/api/config', {
      ble_enabled: bleForm.ble_enabled.checked,
      ble_name: bleForm.ble_name.value.trim(),
      ble_bthome: bleForm.ble_bthome.checked,
      ble_scan: bleForm.ble_scan.checked,
      ble_scan_interval: Number(bleForm.ble_scan_interval.value),
    });
    m.className = 'small msg-ok';
    m.replaceChildren(r.reboot_required ? 'Saved. Reboot to apply. ' : 'Saved.');
    if (r.reboot_required) {
      const btn = el('button', { type: 'button' }, 'Reboot now');
      btn.addEventListener('click', reboot);
      m.append(btn);
    }
  } catch (err) {
    m.className = 'small msg-err';
    m.textContent = err.message;
  }
});

// ---- MQTT page -----------------------------------------------------------------

const mqttForm = $('#mqtt-form');
const MQTT_TOPICS = ['availability', 'status', 'uptime', 'rssi', 'ap_count', 'channel_count', 'scan', 'channels',
  'networks', 'new_ap', 'alerts', 'cmd'];

function renderMqtt() {
  const st = state.status;
  if (!st) return;
  const m = st.mqtt;
  const kv = [
    ['State', m.enabled ? m.state : 'disabled'],
    ['Broker', m.broker || '—'],
    ['Base topic', m.base_topic],
    ['Messages sent', m.published],
    ['HA discovery', m.discovery ? 'on' : 'off'],
  ];
  $('#mqtt-kv').replaceChildren(...kv.flatMap(([k, v]) => [el('dt', null, k),
    el('dd', k === 'State' ? { class: m.connected ? 'msg-ok' : m.enabled ? 'msg-err' : '' } : null, v)]));
  $('#mqtt-cmd-topic').textContent = m.base_topic + '/cmd';
  $('#mqtt-topics').replaceChildren(...MQTT_TOPICS.map((t) => el('li', null, `${m.base_topic}/${t}`)));
  $('#guide-device').textContent = st.device_name;
  $('#guide-yaml').textContent = [
    'alias: New WiFi access point',
    'triggers:',
    '  - trigger: mqtt',
    `    topic: ${m.base_topic}/alerts`,
    'actions:',
    '  - action: notify.notify',
    '    data:',
    '      message: >-',
    '        New AP {{ trigger.payload_json.ssid | default(\'(hidden)\') }}',
    '        on channel {{ trigger.payload_json.channel }},',
    '        {{ trigger.payload_json.rssi }} dBm',
  ].join('\n');
  const host = st.wifi.ip || location.host;
  const iframeCard = (title, chart, ratio) => [
    'type: iframe',
    `title: ${title}`,
    `url: http://${host}/embed?chart=${chart}&theme=dark&refresh=10`,
    `aspect_ratio: ${ratio}`,
  ].join('\n');
  $('#card-spectrum').textContent = iframeCard('WiFi signal by channel', 'spectrum', '55%');
  $('#card-channels').textContent = iframeCard('Access points per channel', 'channels', '45%');
  // HA derives entity IDs from "<device name> <entity name>"
  const dev = st.device_name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '');
  $('#card-history').textContent = [
    'type: history-graph',
    'title: WiFi monitor',
    'hours_to_show: 24',
    'entities:',
    `  - sensor.${dev}_ap_count`,
    `  - sensor.${dev}_channel_1`,
    `  - sensor.${dev}_channel_6`,
    `  - sensor.${dev}_channel_11`,
  ].join('\n');
}

async function loadMqtt() {
  try {
    const c = await apiGet('/api/config');
    const f = mqttForm;
    f.mqtt_enabled.checked = c.mqtt_enabled;
    f.mqtt_host.value = c.mqtt_host;
    f.mqtt_port.value = c.mqtt_port;
    f.mqtt_user.value = c.mqtt_user;
    f.mqtt_password.value = '';
    f.mqtt_password.placeholder = c.mqtt_password_set ? 'unchanged (set)' : 'not set';
    f.mqtt_password_clear.checked = false;
    f.mqtt_client_id.value = c.mqtt_client_id;
    f.mqtt_client_id.placeholder = c.hostname;
    f.mqtt_topic.value = c.mqtt_topic;
    f.mqtt_topic.placeholder = c.mqtt_topic_default;
    f.mqtt_discovery.checked = c.mqtt_discovery;
    f.mqtt_discovery_prefix.value = c.mqtt_discovery_prefix;
    f.mqtt_publish_networks.checked = c.mqtt_publish_networks;
    f.privacy_hide_ssid.checked = c.privacy_hide_ssid;
    f.privacy_anon_bssid.checked = c.privacy_anon_bssid;
  } catch (err) {
    mqttMsg('Could not load settings: ' + err.message, false);
  }
}

function mqttMsg(text, ok) {
  const m = $('#mqtt-msg');
  m.className = 'small ' + (ok ? 'msg-ok' : 'msg-err');
  m.textContent = text;
}

mqttForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const f = mqttForm;
  const body = {
    mqtt_enabled: f.mqtt_enabled.checked,
    mqtt_host: f.mqtt_host.value.trim(),
    mqtt_port: Number(f.mqtt_port.value),
    mqtt_user: f.mqtt_user.value.trim(),
    mqtt_client_id: f.mqtt_client_id.value.trim(),
    mqtt_topic: f.mqtt_topic.value.trim(),
    mqtt_discovery: f.mqtt_discovery.checked,
    mqtt_discovery_prefix: f.mqtt_discovery_prefix.value.trim(),
    mqtt_publish_networks: f.mqtt_publish_networks.checked,
    privacy_hide_ssid: f.privacy_hide_ssid.checked,
    privacy_anon_bssid: f.privacy_anon_bssid.checked,
  };
  // Password is write-only: send it only when typed, or explicitly cleared.
  if (f.mqtt_password_clear.checked) body.mqtt_password = '';
  else if (f.mqtt_password.value) body.mqtt_password = f.mqtt_password.value;
  try {
    await apiPost('/api/config', body);
    mqttMsg('Saved. Connecting…', true);
    f.mqtt_password.value = '';
    await loadMqtt();
    schedulePoll(1500);
  } catch (err) {
    mqttMsg(err.message, false);
  }
});

// Copy buttons on YAML snippets. navigator.clipboard needs HTTPS, which the
// device does not have, so fall back to a temporary textarea + execCommand.
function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text);
  const ta = el('textarea', { style: 'position:fixed;opacity:0' });
  ta.value = text;
  document.body.append(ta);
  ta.select();
  const ok = document.execCommand('copy');
  ta.remove();
  return ok ? Promise.resolve() : Promise.reject(new Error('copy failed'));
}
for (const pre of $$('pre.yaml')) {
  const wrap = el('div', { class: 'code-wrap' });
  pre.replaceWith(wrap);
  const btn = el('button', { type: 'button', class: 'secondary copy-btn' }, 'Copy');
  btn.addEventListener('click', () => copyText(pre.textContent)
    .then(() => { btn.textContent = 'Copied'; })
    .catch(() => { btn.textContent = 'Select & copy manually'; })
    .finally(() => setTimeout(() => { btn.textContent = 'Copy'; }, 1500)));
  wrap.append(pre, btn);
}

// ---- settings ----------------------------------------------------------------

const form = $('#cfg-form');

async function loadConfig() {
  try {
    const c = await apiGet('/api/config');
    form.device_name.value = c.device_name;
    form.hostname.value = c.hostname;
    form.timezone.value = c.timezone;
    if (![...form.scan_interval.options].some((o) => Number(o.value) === c.scan_interval)) {
      form.scan_interval.append(el('option', { value: c.scan_interval }, c.scan_interval + ' s'));
    }
    form.scan_interval.value = c.scan_interval;
    form.auto_scan.checked = c.auto_scan;
    form.max_aps.min = c.limits.min_aps;
    form.max_aps.max = c.limits.max_aps;
    form.max_aps.value = c.max_aps;
    form.history_hours.value = c.history_hours;
    form.ip_static.checked = c.ip_static;
    for (const k of ['ip_address', 'ip_gateway', 'ip_subnet', 'ip_dns1', 'ip_dns2']) form[k].value = c[k];
    toggleIpFields();
    form.scan_channel.value = c.scan_channel;
    form.ui_theme.value = c.ui_theme;
    form.ui_accent.value = c.ui_accent;
  } catch (err) {
    showMsg('Could not load settings: ' + err.message, false);
  }
}

function showMsg(text, ok) {
  const m = $('#cfg-msg');
  m.className = 'small ' + (ok ? 'msg-ok' : 'msg-err');
  m.replaceChildren(text);
  return m;
}

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  const body = {
    device_name: form.device_name.value.trim(),
    hostname: form.hostname.value.trim().toLowerCase(),
    timezone: form.timezone.value.trim(),
    scan_interval: Number(form.scan_interval.value),
    auto_scan: form.auto_scan.checked,
    max_aps: Number(form.max_aps.value),
    history_hours: Number(form.history_hours.value),
    ip_static: form.ip_static.checked,
    ip_address: form.ip_address.value.trim(),
    ip_gateway: form.ip_gateway.value.trim(),
    ip_subnet: form.ip_subnet.value.trim(),
    ip_dns1: form.ip_dns1.value.trim(),
    ip_dns2: form.ip_dns2.value.trim(),
    scan_channel: Number(form.scan_channel.value),
    ui_theme: form.ui_theme.value,
    ui_accent: form.ui_accent.value,
  };
  try {
    const r = await apiPost('/api/config', body);
    const m = showMsg(r.reboot_required ? 'Saved. Reboot required to apply. ' : 'Saved.', true);
    if (r.reboot_required) {
      const b = el('button', { type: 'button' }, 'Reboot now');
      b.addEventListener('click', reboot);
      m.append(b);
    }
  } catch (err) {
    showMsg(err.message, false);
  }
});

function toggleIpFields() {
  $('#ip-fields').hidden = !form.ip_static.checked;
}
form.ip_static.addEventListener('change', toggleIpFields);

// ---- backup / restore ----
// Secrets and the WiFi network are never part of a backup (the API does not return them).
const BACKUP_SKIP = ['limits', 'wifi_ssid', 'wifi_password_set', 'mqtt_password_set', 'mqtt_topic_default'];

$('#btn-backup').addEventListener('click', async () => {
  try {
    const c = await apiGet('/api/config');
    for (const k of BACKUP_SKIP) delete c[k];
    const st = state.status || {};
    const data = { backup: 'esp-wifi-monitor', firmware: st.firmware, created: new Date().toISOString(), config: c };
    const name = `wifi-monitor-backup-${c.device_name}-${new Date().toISOString().slice(0, 10)}.json`;
    download(name, 'application/json', JSON.stringify(data, null, 2));
  } catch (err) {
    backupMsg(err.message, false);
  }
});

$('#restore-file').addEventListener('change', async (e) => {
  const f = e.target.files[0];
  e.target.value = '';
  if (!f) return;
  try {
    const parsed = JSON.parse(await f.text());
    const c = parsed.config || parsed;
    if (typeof c !== 'object' || !c.device_name) throw new Error('Not an ESP WiFi Monitor backup file');
    for (const k of [...BACKUP_SKIP, 'wifi_password', 'mqtt_password']) delete c[k];
    if (!confirm(`Restore settings of "${c.device_name}" onto this device?`)) return;
    const r = await apiPost('/api/config', c);
    backupMsg(r.reboot_required ? 'Restored. Reboot to apply all settings. ' : 'Restored.', true, r.reboot_required);
    loadConfig();
  } catch (err) {
    backupMsg('Restore failed: ' + err.message, false);
  }
});

function backupMsg(text, ok, offerReboot) {
  const m = $('#backup-msg');
  m.className = 'small ' + (ok ? 'msg-ok' : 'msg-err');
  m.replaceChildren(text);
  if (offerReboot) {
    const b = el('button', { type: 'button' }, 'Reboot now');
    b.addEventListener('click', reboot);
    m.append(b);
  }
}

async function reboot() {
  try {
    await apiPost('/api/reboot');
    setBanner('Rebooting… the page will reconnect automatically.');
  } catch (err) {
    alert(err.message);
  }
}
$('#btn-reboot').addEventListener('click', () => { if (confirm('Reboot the device?')) reboot(); });
$('#btn-factory').addEventListener('click', async () => {
  if (!confirm('Factory reset: erase WiFi, MQTT and all other settings and the history, and remove the device from Home Assistant? The device will restart as the setup hotspot.')) return;
  try {
    await apiPost('/api/factory-reset');
    setBanner('Factory reset done. Connect to the ESP-WIFI-MONITOR-xxxx hotspot and open http://192.168.4.1');
  } catch (err) {
    alert(err.message);
  }
});

// ---- system page ---------------------------------------------------------------

// Easter egg, intentionally in Serbian (the rest of the UI is English)
function cpuJoke(mhz) {
  if (mhz >= 200) return 'U jbt, pa ovo može da potera i neku igricu 🙂';
  if (mhz > 150) return 'Aha, skoro pa Pentium dvojka 🙂';
  if (mhz > 100) return 'Ovo je već ekvivalentno pentium kec 🙂';
  return 'Skoro pa kao pentium kec 🙂';
}

// Device time strings "YYYY-MM-DD[T ]HH:MM[:SS]" (already local) -> "DD.MM.YYYY HH:MM[:SS]"
function fmtStamp(s) {
  if (!s) return null;
  return `${s.slice(8, 10)}.${s.slice(5, 7)}.${s.slice(0, 4)} ${s.slice(11)}`;
}

function renderSystem() {
  const st = state.status;
  if (!st) return;
  const s = st.system, w = st.wifi;
  const rows = [
    ['Device', st.device_name],
    ['Firmware', `${st.firmware} (${fmtStamp(st.build_time) || 'build time unknown'})`],
    ['Board', s.board],
    ['Chip', `${s.chip} (id ${s.chip_id})`],
    ['CPU', el('span', null, s.cpu_mhz + ' MHz', el('span', { class: 'easter' }, cpuJoke(s.cpu_mhz)))],
    ['Flash', `${fmtBytes(s.sketch_size + s.fs_used)} used (firmware + files) of ${fmtBytes(s.flash_size)}`,
      usage((s.sketch_size + s.fs_used) / s.flash_size)],
    ['Sketch', `${fmtBytes(s.sketch_size)} used, ${fmtBytes(s.free_sketch)} free`,
      usage(s.sketch_size / (s.sketch_size + s.free_sketch))],
    ['LittleFS', `${fmtBytes(s.fs_used)} / ${fmtBytes(s.fs_total)}`, usage(s.fs_used / s.fs_total)],
    ['Free heap', `${fmtBytes(s.free_heap)} (max block ${fmtBytes(s.max_free_block)}, frag ${s.heap_fragmentation}%)`,
      s.ram_total ? usage((s.ram_total - s.free_heap) / s.ram_total, 'RAM used') : null],
    ['Core / SDK', `${s.core} / ${s.sdk}`],
    ['Reset reason', s.reset_reason],
    ['Boots', `${s.boots} (watchdog resets ${s.watchdog_resets}, crash resets ${s.exception_resets})`],
    ['Uptime', fmtDuration(st.uptime)],
    ['Local time', fmtStamp(st.time) || 'not synced (NTP)'],
    ['WiFi mode', w.mode],
    ['WiFi', w.connected ? `Connected to ${w.ssid}` : 'Disconnected'],
    ['IP', w.ip || '—'],
    ['RSSI', w.connected ? `${w.rssi} dBm (channel ${w.channel})` : '—',
      w.connected ? signal(w.rssi) : null],
    ['Hostname', `${w.hostname}.local`],
    ['MAC', w.mac],
    ['Setup hotspot', w.ap_active ? `${w.ap_ssid} @ ${w.ap_ip} (${w.ap_clients} clients)` : 'off'],
    ['Scans', `${st.scan.id} completed, last took ${st.scan.last_duration_ms} ms`],
    ['Scan interval', `${st.scan.interval} s${st.scan.auto ? '' : ' (auto scan off)'}`],
    ['Tracked APs', `${st.scan.tracked} / ${st.scan.max_aps}`, usage(st.scan.tracked / st.scan.max_aps)],
    ['History', st.history.persistent
      ? `stored in flash, ${st.history.bucket_s / 60} min resolution, retention ${RANGE_LABELS[st.history.hours] || st.history.hours + ' h'}`
      : 'RAM only (waiting for NTP time)'],
    ['MQTT', st.mqtt.enabled ? `${st.mqtt.state} (${st.mqtt.broker})` : 'disabled'],
    ['BLE', st.ble.available
      ? (st.ble.enabled ? `on · ${st.ble.address} · ${st.ble.devices} devices nearby` : 'off (enable on the BLE page)')
      : 'not available on this board'],
    ['About', `Developed by Zoomtronic and a bit of Claude · firmware built and uploaded ${fmtStamp(st.build_time) || '—'}`],
  ];
  $('#sys-kv').replaceChildren(...rows.flatMap(([k, v, bar]) => [
    el('dt', null, k),
    bar ? el('dd', { class: 'with-meter' }, el('span', null, v), bar) : el('dd', null, v),
  ]));
}

// Percentage bar. Level colors always come with the number, never alone.
function meter(pct, level, label) {
  pct = Math.max(0, Math.min(100, Math.round(pct)));
  return el('span', { class: 'meter-row' },
    el('span', { class: 'meter', role: 'meter', 'aria-valuenow': pct, 'aria-valuemin': 0, 'aria-valuemax': 100 },
      el('span', { class: `meter-fill ${level}`, style: `width:${pct}%` })),
    el('span', { class: 'meter-pct' }, `${pct}%${label ? ' ' + label : ''}`));
}
// Resource usage: fine below 70 %, tight up to 90 %, critical above.
const usage = (fraction, label = 'used') => {
  const pct = fraction * 100;
  return meter(pct, pct < 70 ? 'ok' : pct < 90 ? 'warn' : 'bad', label);
};
// Signal quality: -100 dBm = 0 %, -50 dBm or better = 100 %.
const signal = (rssi) => {
  const pct = Math.max(0, Math.min(100, 2 * (rssi + 100)));
  return meter(pct, pct >= 60 ? 'ok' : pct >= 35 ? 'warn' : 'bad', 'signal quality');
};

// ---- start ---------------------------------------------------------------------

showPage(ROUTES[location.pathname] || 'dashboard', false);
poll().then(() => schedulePoll());
