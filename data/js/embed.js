'use strict';

// Chart-only view for iframes (Home Assistant Webpage card). See embed.html for query options.
(() => {
  // The device serves one request at a time; if charts.js was dropped (e.g. during
  // a scan or right after boot), try again instead of leaving an empty iframe.
  if (typeof Charts === 'undefined') {  // top-level const, so not on window
    setTimeout(() => location.reload(), 3000);
    return;
  }
  const q = new URLSearchParams(location.search);
  const kind = q.get('chart') === 'channels' ? 'channels' : 'spectrum';
  const refresh = Math.max(2, Number(q.get('refresh')) || 10);
  const theme = q.get('theme') || 'auto';

  document.documentElement.dataset.theme = theme === 'dark' || theme === 'light'
    ? theme
    : (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
  if (q.get('bg') === 'transparent') document.body.classList.add('transparent');

  const box = document.getElementById('chart').parentElement;
  const legend = document.getElementById('legend');
  const info = document.getElementById('info');
  let chart;
  if (kind === 'channels') {
    document.getElementById('title').textContent = 'Access points per channel';
    chart = new Charts.Bars(box, { legend, catLabel: 'Channel ' });
  } else {
    chart = new Charts.Spectrum(box, legend);
  }

  let lastScan = -1;
  let strongRssi = -67;

  async function getJson(path) {
    const r = await fetch(path, { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
    return r.json();
  }

  function render(nets) {
    if (kind === 'spectrum') {
      chart.setData(nets, strongRssi);
      return;
    }
    const last = nets.some((n) => n.channel === 14) ? 14 : 13;
    const strong = [], weak = [], cats = [];
    for (let c = 1; c <= last; c++) {
      const on = nets.filter((n) => n.channel === c);
      const s = on.filter((n) => n.rssi >= strongRssi).length;
      cats.push(String(c));
      strong.push(s);
      weak.push(on.length - s);
    }
    chart.setData(cats, [
      { name: `Weaker (< ${strongRssi} dBm)`, slot: 0, values: weak },
      { name: `Strong (≥ ${strongRssi} dBm)`, slot: 1, values: strong },
    ]);
  }

  async function poll() {
    try {
      const st = await getJson('/api/status');
      strongRssi = st.scan.strong_rssi;
      if (st.scan.id !== lastScan) {
        const net = await getJson('/api/networks?present=1');
        lastScan = net.scan_id;
        render(net.networks);
      }
      const ago = st.scan.last_scan_ago;
      info.textContent = `${st.summary.detected} APs · ` + (ago == null ? 'no scan yet' : `scan ${ago < 5 ? 'now' : ago + ' s ago'}`);
    } catch (err) {
      info.textContent = 'device not reachable';
    }
    setTimeout(poll, refresh * 1000);
  }
  poll();
})();
