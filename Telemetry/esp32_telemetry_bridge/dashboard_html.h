#pragma once
#include <Arduino.h>

// ARIA dual-sensor dashboard.
// ADS1263 and ICM-20948 are rendered simultaneously with independent
// histories and independent dynamic model-variant colors.
const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ARIA Live Dashboard</title>

  <style>
    :root {
      --bg0: #05070a;
      --bg1: #08101a;
      --panel: rgba(13, 20, 30, 0.72);
      --panel-strong: rgba(16, 25, 37, 0.88);
      --line: rgba(255,255,255,0.075);
      --line-soft: rgba(255,255,255,0.045);
      --text: #f4f7fb;
      --muted: #8490a3;
      --muted2: #586578;

      --ads: #46d8ff;
      --ads-soft: rgba(70,216,255,0.17);
      --imu: #9b8cff;
      --imu-soft: rgba(155,140,255,0.17);

      --green: #4ade80;
      --yellow: #facc15;
      --red: #ff7272;
    }

    * {
      box-sizing: border-box;
    }

    html {
      min-height: 100%;
      background: var(--bg0);
    }

    body {
      margin: 0;
      padding: 26px;
      min-height: 100vh;
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Arial, sans-serif;
      background:
        radial-gradient(circle at 13% -8%, rgba(54, 160, 255, .18), transparent 34%),
        radial-gradient(circle at 88% 4%, rgba(139, 92, 246, .16), transparent 31%),
        linear-gradient(180deg, #08111c 0%, #05080d 58%, #040608 100%);
      position: relative;
      overflow-x: hidden;
    }

    body::before {
      content: "";
      position: fixed;
      inset: 0;
      pointer-events: none;
      opacity: .24;
      background-image:
        linear-gradient(rgba(255,255,255,.018) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,.018) 1px, transparent 1px);
      background-size: 38px 38px;
      mask-image: linear-gradient(to bottom, rgba(0,0,0,.7), transparent 78%);
    }

    .shell {
      max-width: 1540px;
      margin: 0 auto;
      position: relative;
      z-index: 1;
    }

    .topbar {
      display: flex;
      justify-content: space-between;
      align-items: flex-end;
      gap: 18px;
      flex-wrap: wrap;
      margin-bottom: 22px;
      padding: 3px 2px 1px;
    }

    .brand-kicker {
      margin-bottom: 5px;
      color: #6f7d91;
      font-size: 10px;
      font-weight: 700;
      letter-spacing: .20em;
      text-transform: uppercase;
    }

    h1 {
      margin: 0;
      font-size: clamp(24px, 2.5vw, 34px);
      line-height: 1.05;
      letter-spacing: -0.02em;
      font-weight: 740;
      background: linear-gradient(100deg, #ffffff 0%, #a7f3d0 46%, #74d7ff 78%, #bca7ff 100%);
      -webkit-background-clip: text;
      background-clip: text;
      color: transparent;
      filter: drop-shadow(0 0 18px rgba(94, 234, 212, .08));
    }

    .overall {
      display: flex;
      align-items: center;
      gap: 9px;
      padding: 9px 12px;
      border: 1px solid rgba(255,255,255,.07);
      border-radius: 999px;
      color: #97a4b6;
      font-size: 11px;
      letter-spacing: .025em;
      background: rgba(255,255,255,.025);
      box-shadow: inset 0 1px 0 rgba(255,255,255,.035);
      backdrop-filter: blur(12px);
    }

    .overall::before {
      content: "";
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: #5eead4;
      box-shadow: 0 0 13px rgba(94,234,212,.72);
    }

    .sensor-section {
      position: relative;
      margin-bottom: 24px;
      padding: 20px;
      border: 1px solid var(--line);
      border-radius: 20px;
      background:
        linear-gradient(145deg, rgba(255,255,255,.027), rgba(255,255,255,.006)),
        rgba(7, 12, 19, .76);
      box-shadow:
        0 18px 52px rgba(0,0,0,.34),
        inset 0 1px 0 rgba(255,255,255,.038);
      overflow: hidden;
      backdrop-filter: blur(14px);
    }

    .sensor-section::before {
      content: "";
      position: absolute;
      top: 0;
      left: 34px;
      right: 34px;
      height: 1px;
      opacity: .72;
    }

    .sensor-section::after {
      content: "";
      position: absolute;
      width: 280px;
      height: 280px;
      border-radius: 50%;
      right: -130px;
      top: -160px;
      pointer-events: none;
      filter: blur(2px);
      opacity: .34;
    }

    .ads-section::before {
      background: linear-gradient(90deg, transparent, var(--ads), transparent);
    }

    .ads-section::after {
      background: radial-gradient(circle, var(--ads-soft), transparent 68%);
    }

    .imu-section::before {
      background: linear-gradient(90deg, transparent, var(--imu), transparent);
    }

    .imu-section::after {
      background: radial-gradient(circle, var(--imu-soft), transparent 68%);
    }

    .sensor-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      flex-wrap: wrap;
      margin-bottom: 15px;
      position: relative;
      z-index: 1;
    }

    .sensor-title-wrap {
      display: flex;
      align-items: center;
      gap: 11px;
      min-width: 0;
    }

    .sensor-orb {
      width: 34px;
      height: 34px;
      border-radius: 11px;
      display: grid;
      place-items: center;
      position: relative;
      flex: 0 0 auto;
      border: 1px solid rgba(255,255,255,.09);
      box-shadow: inset 0 1px 0 rgba(255,255,255,.06);
    }

    .sensor-orb::before {
      content: "";
      width: 10px;
      height: 10px;
      border-radius: 3px;
      border: 1px solid currentColor;
      box-shadow: 0 0 12px currentColor;
      opacity: .86;
    }

    .ads-orb {
      color: var(--ads);
      background: rgba(70,216,255,.08);
    }

    .imu-orb {
      color: var(--imu);
      background: rgba(155,140,255,.08);
    }

    .sensor-title {
      font-size: 16px;
      font-weight: 760;
      letter-spacing: .035em;
    }

    .sensor-subtitle {
      margin-top: 2px;
      color: var(--muted);
      font-size: 11px;
      letter-spacing: .015em;
    }

    .title-copy {
      min-width: 0;
    }

    .status {
      display: flex;
      align-items: center;
      gap: 8px;
      color: var(--muted);
      font-size: 11px;
      padding: 7px 10px;
      border-radius: 999px;
      background: rgba(255,255,255,.024);
      border: 1px solid rgba(255,255,255,.055);
    }

    .dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 11px 2px rgba(74,222,128,.42);
      animation: pulse 1.7s ease-in-out infinite;
    }

    .dot.stale {
      animation: none;
      background: #4c596b;
      box-shadow: none;
    }

    @keyframes pulse {
      0%,100% { transform: scale(1); opacity: 1; }
      50% { transform: scale(1.24); opacity: .58; }
    }

    .cards {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 12px;
      margin-bottom: 13px;
      position: relative;
      z-index: 1;
    }

    .card {
      position: relative;
      min-width: 0;
      padding: 16px 18px 17px;
      border-radius: 15px;
      border: 1px solid var(--line-soft);
      background:
        linear-gradient(145deg, rgba(255,255,255,.035), rgba(255,255,255,.011)),
        rgba(17, 25, 36, .72);
      box-shadow:
        inset 0 1px 0 rgba(255,255,255,.03),
        0 8px 24px rgba(0,0,0,.15);
      transition:
        transform .22s ease,
        border-color .22s ease,
        background .22s ease;
      overflow: hidden;
    }

    .card:hover {
      transform: translateY(-2px);
      border-color: rgba(255,255,255,.11);
      background:
        linear-gradient(145deg, rgba(255,255,255,.046), rgba(255,255,255,.013)),
        rgba(19, 29, 42, .78);
    }

    .card::after {
      content: "";
      position: absolute;
      left: 18px;
      right: 18px;
      bottom: 0;
      height: 1px;
      opacity: .5;
      background: linear-gradient(90deg, transparent, rgba(255,255,255,.08), transparent);
    }

    .card .label {
      color: #758398;
      font-size: 9.5px;
      font-weight: 750;
      text-transform: uppercase;
      letter-spacing: .13em;
    }

    .card .value {
      margin-top: 8px;
      font-size: clamp(26px, 2.2vw, 34px);
      line-height: 1;
      font-weight: 760;
      letter-spacing: -0.035em;
      font-variant-numeric: tabular-nums;
    }

    .card .unit {
      margin-left: 5px;
      color: #7d899b;
      font-size: 12px;
      font-weight: 600;
      letter-spacing: 0;
    }

    .model-fast {
      color: var(--green);
      text-shadow: 0 0 22px rgba(74,222,128,.30);
    }

    .model-balanced {
      color: var(--yellow);
      text-shadow: 0 0 22px rgba(250,204,21,.27);
    }

    .model-accurate {
      color: var(--red);
      text-shadow: 0 0 22px rgba(255,114,114,.30);
    }

    .model-unknown {
      color: #8190a4;
    }

    .graph-wrap {
      position: relative;
      padding: 13px 14px 10px;
      border-radius: 15px;
      border: 1px solid var(--line-soft);
      background:
        linear-gradient(180deg, rgba(255,255,255,.016), rgba(255,255,255,.004)),
        rgba(6, 11, 17, .65);
      overflow: hidden;
      z-index: 1;
    }

    .graph-wrap::before {
      content: "";
      position: absolute;
      inset: 0;
      pointer-events: none;
      background:
        linear-gradient(rgba(255,255,255,.018) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,.012) 1px, transparent 1px);
      background-size: 100% 44px, 80px 100%;
      opacity: .55;
    }

    .graph-label-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 6px;
      position: relative;
      z-index: 1;
    }

    .graph-label {
      color: #758398;
      font-size: 9.5px;
      font-weight: 750;
      text-transform: uppercase;
      letter-spacing: .12em;
    }

    .graph-tag {
      color: #59687b;
      font-size: 9px;
      letter-spacing: .09em;
      text-transform: uppercase;
    }

    canvas {
      display: block;
      width: 100%;
      height: 186px;
      position: relative;
      z-index: 1;
    }

    .stale-msg,
    .fresh-msg,
    .waiting-msg {
      margin-top: 9px;
      min-height: 15px;
      font-size: 10.5px;
      letter-spacing: .02em;
      position: relative;
      z-index: 1;
    }

    .stale-msg { color: #fb7185; }
    .fresh-msg { color: #5eead4; }
    .waiting-msg { color: #fbbf24; }

    .footer-note {
      margin-top: 8px;
      text-align: center;
      color: #475569;
      font-size: 9.5px;
      letter-spacing: .08em;
      text-transform: uppercase;
    }

    @media (max-width: 800px) {
      body { padding: 14px; }

      .cards {
        grid-template-columns: 1fr;
      }

      .sensor-section {
        padding: 14px;
        border-radius: 17px;
      }

      .sensor-header {
        margin-bottom: 12px;
      }

      canvas {
        height: 165px;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .dot { animation: none; }
      .card { transition: none; }
    }
  </style>
</head>

<body>
  <div class="shell">
    <div class="topbar">
      <div>
        <div class="brand-kicker">Adaptive Runtime Inference Accelerator</div>
        <h1>ARIA &mdash; Live Telemetry</h1>
      </div>
      <div class="overall">
        <span>EK-RA8D1 &rarr; ESP32 &rarr; Web UI</span>
      </div>
    </div>

  <!-- ================================================================
       ADS1263
       ================================================================ -->
  <section class="sensor-section ads-section">
    <div class="sensor-header">
      <div class="sensor-title-wrap">
        <div class="sensor-orb ads-orb"></div>
        <div class="title-copy">
          <div class="sensor-title">ADS1263</div>
          <div class="sensor-subtitle">Precision ADC &middot; anomaly inference</div>
        </div>
      </div>

      <div class="status">
        <div class="dot stale" id="adsDot"></div>
        <span id="adsStatus">waiting for ADS1263</span>
      </div>
    </div>

    <div class="cards">
      <div class="card">
        <div class="label">Active Model</div>
        <div class="value model-unknown" id="adsModel">-</div>
      </div>

      <div class="card">
        <div class="label">Voltage</div>
        <div class="value">
          <span id="adsVoltage">-</span>
          <span class="unit">V</span>
        </div>
      </div>

      <div class="card">
        <div class="label">Latency</div>
        <div class="value">
          <span id="adsLatency">-</span>
          <span class="unit">ms</span>
        </div>
      </div>
    </div>

    <div class="graph-wrap">
      <div class="graph-label-row">
        <div class="graph-label">ADS1263 voltage &mdash; live waveform</div>
        <div class="graph-tag">60-sample window</div>
      </div>
      <canvas id="adsGraph" width="1200" height="186"></canvas>
    </div>

    <div id="adsFreshness" class="waiting-msg">
      Waiting for ADS1263 telemetry...
    </div>
  </section>

  <!-- ================================================================
       ICM-20948
       ================================================================ -->
  <section class="sensor-section">
    <div class="sensor-header">
      <div class="sensor-title-wrap">
        <div class="sensor-orb imu-orb"></div>
        <div class="title-copy">
          <div class="sensor-title">ICM-20948</div>
          <div class="sensor-subtitle">6-axis IMU &middot; fault inference</div>
        </div>
      </div>

      <div class="status">
        <div class="dot stale" id="imuDot"></div>
        <span id="imuStatus">waiting for ICM-20948</span>
      </div>
    </div>

    <div class="cards">
      <div class="card">
        <div class="label">Active Model</div>
        <div class="value model-unknown" id="imuModel">-</div>
      </div>

      <div class="card">
        <div class="label">Fault Confidence</div>
        <div class="value">
          <span id="imuConfidence">-</span>
          <span class="unit">%</span>
        </div>
      </div>

      <div class="card">
        <div class="label">Latency</div>
        <div class="value">
          <span id="imuLatency">-</span>
          <span class="unit">ms</span>
        </div>
      </div>
    </div>

    <div class="graph-wrap">
      <div class="graph-label-row">
        <div class="graph-label">ICM-20948 fault confidence &mdash; live</div>
        <div class="graph-tag">0&ndash;100% confidence</div>
      </div>
      <canvas id="imuGraph" width="1200" height="186"></canvas>
    </div>

    <div id="imuFreshness" class="waiting-msg">
      Waiting for ICM-20948 telemetry...
    </div>
  </section>

    <div class="footer-note">ARIA &middot; adaptive multi-model embedded intelligence</div>
  </div>

  <script>
    const modelColor = {
      fast: '#4ade80',
      balanced: '#facc15',
      accurate: '#f87171'
    };

    let lastSuccessfulFetchMs = performance.now();

    function setModel(id, model) {
      const el = document.getElementById(id);
      const m = model || 'unknown';

      el.textContent = m;
      el.className = 'value model-' + m;
    }

    function updateFreshness(prefix, stream) {
      const dot = document.getElementById(prefix + 'Dot');
      const status = document.getElementById(prefix + 'Status');
      const line = document.getElementById(prefix + 'Freshness');

      if (!stream || !stream.available) {
        dot.classList.add('stale');
        status.textContent = 'waiting';
        line.className = 'waiting-msg';
        line.textContent = 'Waiting for telemetry...';
        return;
      }

      const age = Number(stream.age_ms || 0);

      if (age > 3000) {
        dot.classList.add('stale');
        status.textContent = 'stale';
        line.className = 'stale-msg';
        line.textContent =
          'No fresh data for ' + (age / 1000).toFixed(1) + 's';
      }
      else {
        dot.classList.remove('stale');
        status.textContent = 'live';
        line.className = 'fresh-msg';
        line.textContent =
          'Live \u2014 updated ' + (age / 1000).toFixed(1) + 's ago';
      }
    }

    function renderAds(ads) {
      if (!ads || !ads.available) {
        setModel('adsModel', 'unknown');
        document.getElementById('adsVoltage').textContent = '-';
        document.getElementById('adsLatency').textContent = '-';
        updateFreshness('ads', ads);
        clearGraph('adsGraph');
        return;
      }

      setModel('adsModel', ads.model);

      document.getElementById('adsVoltage').textContent =
        Number(ads.voltage).toFixed(3);

      document.getElementById('adsLatency').textContent =
        Number(ads.latency_ms).toFixed(3);

      updateFreshness('ads', ads);

      drawSeries(
        'adsGraph',
        ads.history,
        h => Number(h.voltage),
        false
      );
    }

    function renderImu(imu) {
      if (!imu || !imu.available) {
        setModel('imuModel', 'unknown');
        document.getElementById('imuConfidence').textContent = '-';
        document.getElementById('imuLatency').textContent = '-';
        updateFreshness('imu', imu);
        clearGraph('imuGraph');
        return;
      }

      setModel('imuModel', imu.model);

      document.getElementById('imuConfidence').textContent =
        (Number(imu.confidence) * 100.0).toFixed(1);

      document.getElementById('imuLatency').textContent =
        Number(imu.latency_ms).toFixed(3);

      updateFreshness('imu', imu);

      drawSeries(
        'imuGraph',
        imu.history,
        h => Number(h.confidence),
        true
      );
    }

    function clearGraph(canvasId) {
      const c = document.getElementById(canvasId);
      const ctx = c.getContext('2d');
      ctx.clearRect(0, 0, c.width, c.height);
    }

    function drawSeries(canvasId, history, valueFn, fixedUnitRange) {
      const c = document.getElementById(canvasId);
      const ctx = c.getContext('2d');

      ctx.clearRect(0, 0, c.width, c.height);

      if (!history || history.length < 2) {
        return;
      }

      const values = history.map(valueFn);

      let minV;
      let maxV;

      if (fixedUnitRange) {
        minV = 0.0;
        maxV = 1.0;
      }
      else {
        minV = Math.min(...values, -0.01);
        maxV = Math.max(...values, 0.01);

        if (Math.abs(maxV - minV) < 1e-9) {
          maxV += 0.01;
          minV -= 0.01;
        }

        const padding = (maxV - minV) * 0.08;
        minV -= padding;
        maxV += padding;
      }

      const range = maxV - minV;
      const stepX = c.width / (history.length - 1);

      const lastModel =
        history[history.length - 1].model || 'unknown';

      const lineColor =
        modelColor[lastModel] || '#38bdf8';

      // Horizontal reference lines.
      ctx.lineWidth = 1;
      ctx.strokeStyle = 'rgba(255,255,255,0.065)';

      for (let g = 1; g < 4; g++) {
        const y = (c.height * g) / 4;

        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(c.width, y);
        ctx.stroke();
      }

      const toY = value => {
        const clamped = Math.max(minV, Math.min(maxV, value));

        return (
          c.height -
          ((clamped - minV) / range) *
          (c.height - 20) -
          10
        );
      };

      // Filled region.
      ctx.beginPath();

      history.forEach((h, i) => {
        const x = i * stepX;
        const y = toY(valueFn(h));

        if (i === 0) {
          ctx.moveTo(x, y);
        }
        else {
          ctx.lineTo(x, y);
        }
      });

      ctx.lineTo(c.width, c.height - 10);
      ctx.lineTo(0, c.height - 10);
      ctx.closePath();

      ctx.fillStyle = lineColor + '22';
      ctx.fill();

      // Main waveform.
      ctx.beginPath();
      ctx.strokeStyle = lineColor;
      ctx.lineWidth = 2.4;

      history.forEach((h, i) => {
        const x = i * stepX;
        const y = toY(valueFn(h));

        if (i === 0) {
          ctx.moveTo(x, y);
        }
        else {
          ctx.lineTo(x, y);
        }
      });

      ctx.stroke();

      // Per-sample dots colored by the variant that produced each point.
      history.forEach((h, i) => {
        const x = i * stepX;
        const y = toY(valueFn(h));

        ctx.fillStyle =
          modelColor[h.model] || '#cbd5e1';

        ctx.beginPath();
        ctx.arc(x, y, 2.35, 0, Math.PI * 2);
        ctx.fill();
      });

      // IMU confidence reference markers.
      if (fixedUnitRange) {
        ctx.fillStyle = 'rgba(255,255,255,0.45)';
        ctx.font = '12px monospace';

        ctx.fillText('100%', 8, 14);
        ctx.fillText('50%', 8, c.height / 2);
        ctx.fillText('0%', 8, c.height - 5);
      }
    }

    function render(data) {
      renderAds(data.ads);
      renderImu(data.imu);
    }

    async function poll() {
      try {
        const res = await fetch(
          '/telemetry?_=' + Date.now(),
          { cache: 'no-store' }
        );

        if (!res.ok) {
          throw new Error('HTTP ' + res.status);
        }

        const data = await res.json();

        lastSuccessfulFetchMs = performance.now();
        render(data);
      }
      catch (e) {
        // Sensor-specific age is supplied by the ESP32 on the next
        // successful request. Keep the last rendered values meanwhile.
      }

      setTimeout(poll, 500);
    }

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        poll();
      }
    });

    poll();
  </script>
</body>
</html>
)HTML";