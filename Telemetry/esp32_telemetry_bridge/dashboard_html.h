#pragma once
#include <Arduino.h>

// Single-file dashboard: polls /telemetry every 500ms and redraws.
// No external JS libraries -> works over the ESP32's own Wi-Fi with
// no internet connection required.
const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ARIA Live Dashboard</title>
  <style>
    :root {
      --accent: #38bdf8;
      --glow: rgba(56, 189, 248, 0.35);
    }
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, 'SF Mono', 'Segoe UI', Arial, sans-serif;
      background: radial-gradient(circle at 20% 0%, #131a24 0%, #0a0d12 55%, #060708 100%);
      color: #e6e6e6;
      margin: 0;
      padding: 24px;
      min-height: 100vh;
      transition: box-shadow 0.6s ease;
    }
    .topbar { display:flex; align-items:center; justify-content:space-between; margin-bottom:22px; flex-wrap:wrap; gap:10px; }
    h1 {
      font-size: 22px;
      margin: 0;
      letter-spacing: 0.02em;
      background: linear-gradient(90deg, #ffffff, var(--accent));
      -webkit-background-clip: text;
      background-clip: text;
      color: transparent;
    }
    .status { display:flex; align-items:center; gap:8px; font-size:13px; color:#8a919c; }
    .dot {
      width:10px; height:10px; border-radius:50%;
      background: var(--accent);
      box-shadow: 0 0 8px 2px var(--glow);
      animation: pulse 1.6s ease-in-out infinite;
    }
    .dot.stale { animation: none; background:#4b5563; box-shadow:none; }
    @keyframes pulse {
      0%, 100% { transform: scale(1); opacity: 1; }
      50% { transform: scale(1.35); opacity: 0.55; }
    }
    .sub { color:#8a919c; font-size:13px; margin-bottom:20px; }

    .cards { display:flex; gap:14px; flex-wrap:wrap; margin-bottom:22px; }
    .card {
      background: rgba(26, 31, 38, 0.65);
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255,255,255,0.06);
      border-radius: 14px;
      padding: 18px 22px;
      min-width: 160px;
      flex: 1 1 160px;
      transition: border-color 0.4s ease, box-shadow 0.4s ease;
    }
    .card.glow { border-color: var(--accent); box-shadow: 0 0 24px -6px var(--glow); }
    .card .label { color:#8a919c; font-size:11px; text-transform:uppercase; letter-spacing:0.08em; }
    .card .value {
      font-size: 32px;
      font-weight: 700;
      margin-top: 8px;
      font-variant-numeric: tabular-nums;
      transition: color 0.3s ease, text-shadow 0.3s ease;
    }
    .card .unit { font-size: 15px; font-weight:500; color:#8a919c; margin-left:4px; }

    .model-fast      { color:#4ade80; text-shadow: 0 0 16px rgba(74,222,128,0.5); }
    .model-balanced  { color:#facc15; text-shadow: 0 0 16px rgba(250,204,21,0.5); }
    .model-accurate  { color:#f87171; text-shadow: 0 0 16px rgba(248,113,113,0.5); }

    .graph-wrap {
      background: rgba(26, 31, 38, 0.65);
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255,255,255,0.06);
      border-radius: 14px;
      padding: 14px;
    }
    .graph-label { color:#8a919c; font-size:11px; text-transform:uppercase; letter-spacing:0.08em; margin-bottom:8px; }
    canvas { width:100%; height:170px; display:block; }

    .stale-msg { color:#f87171; font-size:12px; margin-top:10px; min-height:16px; }
    .fresh-msg { color:#4ade80; font-size:12px; margin-top:10px; min-height:16px; }
  </style>
</head>
<body>
  <div class="topbar">
    <h1>ARIA &mdash; Live Telemetry</h1>
    <div class="status">
      <div class="dot" id="liveDot"></div>
      <span id="modeLabel">mode: -</span>
    </div>
  </div>

  <div class="cards">
    <div class="card" id="modelCard">
      <div class="label">Active Model</div>
      <div class="value" id="model">-</div>
    </div>
    <div class="card">
      <div class="label">Voltage</div>
      <div class="value"><span id="voltage">-</span><span class="unit">V</span></div>
    </div>
    <div class="card">
      <div class="label">Latency</div>
      <div class="value"><span id="latency">-</span><span class="unit">ms</span></div>
    </div>
  </div>

  <div class="graph-wrap">
    <div class="graph-label">Voltage &mdash; live waveform</div>
    <canvas id="graph" width="600" height="170"></canvas>
  </div>
  <div id="staleLine" class="fresh-msg"></div>

  <script>
    const modelColor = { fast: '#4ade80', balanced: '#facc15', accurate: '#f87171' };
    let lastGoodFetch = performance.now();
    let lastData = null;

    async function poll() {
      try {
        // Cache-bust the URL AND tell fetch not to use any cached copy --
        // some browsers cache identical-URL GETs more aggressively than
        // expected, which is a common cause of a dashboard looking "stuck".
        const res = await fetch('/telemetry?_=' + Date.now(), { cache: 'no-store' });
        const data = await res.json();
        lastGoodFetch = performance.now();
        lastData = data;
        render(data);
      } catch (e) {
        // swallow -- tickStale() below reports staleness independent of this
      }
      setTimeout(poll, 500);
    }

    // Runs on its own clock so the page always shows an honest "age" even
    // if poll() itself is being throttled (e.g. phone screen just locked,
    // or the tab was backgrounded -- mobile browsers slow JS timers hard
    // in that state, which is the usual real cause of "frozen" dashboards).
    function tickStale() {
      const ageMs = performance.now() - lastGoodFetch;
      const dot = document.getElementById('liveDot');
      const line = document.getElementById('staleLine');
      if (ageMs > 3000) {
        dot.classList.add('stale');
        line.className = 'stale-msg';
        line.textContent = 'No fresh data for ' + (ageMs / 1000).toFixed(1) + 's';
      } else {
        dot.classList.remove('stale');
        line.className = 'fresh-msg';
        line.textContent = 'Live \u2014 updated ' + (ageMs / 1000).toFixed(1) + 's ago';
      }
      requestAnimationFrame(() => setTimeout(tickStale, 250));
    }

    // When the tab regains focus/visibility (phone unlocked, tab switched
    // back to), force an immediate poll instead of waiting for the next
    // throttled timer tick.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') poll();
    });

    function render(data) {
      document.getElementById('modeLabel').textContent = 'mode: ' + data.mode;

      const modelEl = document.getElementById('model');
      modelEl.textContent = data.model;
      modelEl.className = 'value model-' + data.model;

      document.getElementById('modelCard').style.setProperty('--accent', modelColor[data.model] || '#38bdf8');
      document.documentElement.style.setProperty('--accent', modelColor[data.model] || '#38bdf8');

      document.getElementById('voltage').textContent = Number(data.voltage).toFixed(3);
      document.getElementById('latency').textContent = data.latency_ms;

      drawGraph(data.history);
    }

    function drawGraph(history) {
      const c = document.getElementById('graph');
      const ctx = c.getContext('2d');
      ctx.clearRect(0, 0, c.width, c.height);
      if (!history || history.length < 2) return;

      const values = history.map(h => h.voltage);
      const maxV = Math.max(...values, 0.01);
      const minV = Math.min(...values, -0.01);
      const range = (maxV - minV) || 1;
      const stepX = c.width / (history.length - 1);
      const zeroY = c.height - ((0 - minV) / range) * (c.height - 20) - 10;

      // filled area under the waveform, colored by the most recent model
      const lastModel = history[history.length - 1].model;
      const color = modelColor[lastModel] || '#38bdf8';

      ctx.beginPath();
      history.forEach((h, i) => {
        const x = i * stepX;
        const y = c.height - ((h.voltage - minV) / range) * (c.height - 20) - 10;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.lineTo(c.width, zeroY);
      ctx.lineTo(0, zeroY);
      ctx.closePath();
      ctx.fillStyle = color + '22';
      ctx.fill();

      ctx.beginPath();
      ctx.strokeStyle = color;
      ctx.lineWidth = 2.2;
      history.forEach((h, i) => {
        const x = i * stepX;
        const y = c.height - ((h.voltage - minV) / range) * (c.height - 20) - 10;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();

      history.forEach((h, i) => {
        const x = i * stepX;
        const y = c.height - ((h.voltage - minV) / range) * (c.height - 20) - 10;
        ctx.fillStyle = modelColor[h.model] || '#e6e6e6';
        ctx.beginPath();
        ctx.arc(x, y, 2.8, 0, Math.PI * 2);
        ctx.fill();
      });
    }

    poll();
    tickStale();
  </script>
</body>
</html>
)HTML";