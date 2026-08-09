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
    body { font-family: -apple-system, Arial, sans-serif; background:#0f1216; color:#e6e6e6; margin:0; padding:20px; }
    h1 { font-size:20px; margin-bottom:4px; }
    .sub { color:#8a919c; font-size:13px; margin-bottom:20px; }
    .cards { display:flex; gap:12px; flex-wrap:wrap; margin-bottom:20px; }
    .card { background:#1a1f26; border-radius:10px; padding:16px 20px; min-width:140px; }
    .card .label { color:#8a919c; font-size:12px; text-transform:uppercase; letter-spacing:0.05em; }
    .card .value { font-size:28px; font-weight:600; margin-top:6px; }
    .model-fast { color:#4ade80; }
    .model-balanced { color:#facc15; }
    .model-accurate { color:#f87171; }
    canvas { background:#1a1f26; border-radius:10px; width:100%; height:160px; }
    .stale { color:#f87171; font-size:12px; margin-top:8px; }
  </style>
</head>
<body>
  <h1>ARIA - Live Telemetry</h1>
  <div class="sub" id="modeLabel">mode: -</div>

  <div class="cards">
    <div class="card">
      <div class="label">Active Model</div>
      <div class="value" id="model">-</div>
    </div>
    <div class="card">
      <div class="label">Confidence</div>
      <div class="value" id="confidence">-</div>
    </div>
    <div class="card">
      <div class="label">Latency</div>
      <div class="value" id="latency">-</div>
    </div>
  </div>

  <canvas id="graph" width="600" height="160"></canvas>
  <div class="stale" id="stale"></div>

  <script>
    const modelColor = { fast: '#4ade80', balanced: '#facc15', accurate: '#f87171' };

    async function poll() {
      try {
        const res = await fetch('/telemetry');
        const data = await res.json();
        render(data);
      } catch (e) {
        document.getElementById('stale').textContent = 'Connection lost - retrying...';
      }
      setTimeout(poll, 500);
    }

    function render(data) {
      document.getElementById('modeLabel').textContent = 'mode: ' + data.mode;
      const modelEl = document.getElementById('model');
      modelEl.textContent = data.model;
      modelEl.className = 'value model-' + data.model;
      document.getElementById('confidence').textContent = (data.confidence * 100).toFixed(0) + '%';
      document.getElementById('latency').textContent = data.latency_ms + ' ms';

      const staleEl = document.getElementById('stale');
      staleEl.textContent = data.age_ms > 3000 ? 'No data for ' + (data.age_ms/1000).toFixed(1) + 's' : '';

      drawGraph(data.history);
    }

    function drawGraph(history) {
      const c = document.getElementById('graph');
      const ctx = c.getContext('2d');
      ctx.clearRect(0, 0, c.width, c.height);
      if (!history || history.length < 2) return;

      const maxLatency = Math.max(...history.map(h => h.latency_ms), 10);
      const stepX = c.width / (history.length - 1);

      ctx.beginPath();
      ctx.strokeStyle = '#38bdf8';
      ctx.lineWidth = 2;
      history.forEach((h, i) => {
        const x = i * stepX;
        const y = c.height - (h.latency_ms / maxLatency) * (c.height - 20) - 10;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();

      history.forEach((h, i) => {
        const x = i * stepX;
        const y = c.height - (h.latency_ms / maxLatency) * (c.height - 20) - 10;
        ctx.fillStyle = modelColor[h.model] || '#e6e6e6';
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        ctx.fill();
      });
    }

    poll();
  </script>
</body>
</html>
)HTML";
