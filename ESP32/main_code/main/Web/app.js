/* Industrial Monitor dashboard — zero-dependency client.
 *
 * Layout:
 *   conn     WebSocket (1 Hz status push) + polling fallback
 *   charts   canvas line charts fed from /api/history + live appends
 *   ui       DOM rendering for the status payload
 *   api      REST wrappers for the control endpoints
 *
 * The WebSocket server pushes {"type":"status",...} once a second.
 * On (re)connect the client refetches the history window, so charts
 * heal after any outage.
 */
"use strict";

/* ================= session state ================= */

const S = {
  ws: null,
  wsUp: false,
  lastStatus: null,
  win: 300,               // chart window, seconds
  hist: [[], [], []],     // per-channel ring: [t, value]
  led: { g: false, r: false },
};

const ALM_BITS = {
  0x01: "overtemp", 0x02: "overcurrent", 0x04: "vibration",
  0x08: "sensor-fail", 0x10: "comm-fail",
};
const $ = (id) => document.getElementById(id);

function fmtAge(ms) {
  if (ms === 0xFFFFFFFF) return "never";
  const s = Math.floor(ms / 1000);
  if (s < 90) return s + "s";
  return Math.floor(s / 60) + "m";
}
function fmtDur(s) {
  if (s < 90) return s + "s";
  if (s < 5400) return Math.floor(s / 60) + "m";
  return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m";
}
function fmtClock(ms) {
  const d = new Date();
  return d.toTimeString().slice(0, 8);
}

/* ================= REST api ================= */

async function post(path, body) {
  try {
    const r = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {}),
    });
    return await r.json();
  } catch (e) {
    return { ok: false, err: String(e) };
  }
}

/* ================= ui ================= */

function renderStatus(st) {
  try { renderStatusInner(st); }
  catch (e) { console.error("renderStatus", e); }
}

function renderStatusInner(st) {
  S.lastStatus = st;
  st.th  = st.th  || { t: 0, c: 0, v: 0 };
  st.esp = st.esp || {};

  const alarmNames = [];
  for (const [bit, name] of Object.entries(ALM_BITS)) {
    if (st.alm & Number(bit)) alarmNames.push(name);
  }

  // Sensor-health bits from the STM32: 0x01 DS18B20, 0x02 MPU6050, 0x04 INA219.
  // A failed sensor shows "--" and a red FAULT tag instead of a confident 0.
  const sst = st.sstat | 0;
  const linked = st.link !== false && st.age !== 4294967295;
  const sensorTag = (id, ok) => {
    const el = $(id);
    if (!el) return;
    if (!linked) { el.textContent = "no data"; el.className = "sens idle"; return; }
    el.textContent = ok ? "OK" : "FAULT";
    el.className = "sens " + (ok ? "ok" : "bad");
  };
  sensorTag("sens-ds",  !!(sst & 0x01));
  sensorTag("sens-mpu", !!(sst & 0x02));
  sensorTag("sens-ina", !!(sst & 0x04));
  const num = (v, d) => (typeof v === "number" && isFinite(v)) ? v.toFixed(d) : "--";

  $("v-temp").textContent = (sst & 0x01) ? num(st.temp, 2) : "--";
  $("v-cur").textContent  = (sst & 0x04) ? num(st.cur, 3)  : "--";
  $("v-vib").textContent  = (sst & 0x02) ? num(st.vib, 3)  : "--";
  $("v-alm").textContent = alarmNames.length;
  $("v-alm-names").textContent = alarmNames.join(", ") || "none";

  $("kpi-temp-sub").textContent = "thr " + st.th.t.toFixed(1);
  $("kpi-cur-sub").textContent = "thr " + st.th.c.toFixed(2);
  $("kpi-vib-sub").textContent = "thr " + st.th.v.toFixed(2);

  $("kpi-temp").classList.toggle("alert", !!(st.alm & 0x01));
  $("kpi-cur").classList.toggle("alert", !!(st.alm & 0x02));
  $("kpi-vib").classList.toggle("alert", !!(st.alm & 0x04));

  $("v-stm-cpu").textContent = st.cpu.toFixed(1) + "%";
  $("v-stm-heap").textContent = (st.heap / 1024).toFixed(1) + " KB";
  $("v-stm-up").textContent = fmtDur(st.up);

  const e = st.esp || {};
  $("v-esp-cpu").textContent = (e.cpu == null ? "--" : e.cpu + "%");
  $("v-esp-heap").textContent = (e.heap / 1024).toFixed(0) + " KB";
  $("v-esp-rssi").textContent = e.rssi ? e.rssi : "--";
  $("hdr-wifi").textContent = e.ssid || "wifi " + (e.wifi ?? "?");
  $("hdr-clock").textContent = fmtClock(st.ts);

  const banner = $("alarm-banner");
  if (alarmNames.length) {
    banner.classList.remove("hidden");
    $("alarm-banner-text").textContent =
      "ALARMS: " + alarmNames.join(" · ").toUpperCase();
  } else {
    banner.classList.add("hidden");
  }

  $("ftr-link").textContent = st.link
    ? "link up (" + fmtAge(st.age) + ")"
    : "STM32 LINK DOWN";

  /* provisioning panel vs live view */
  const provisioning = e.wifi === 0;
  $("wifi-setup").classList.toggle("hidden", !provisioning);
  $("live").classList.toggle("hidden", provisioning);

  /* threshold + rate inputs follow the live values until edited */
  if (document.activeElement !== $("thr-temp")) $("thr-temp").value = st.th.t;
  if (document.activeElement !== $("thr-cur"))  $("thr-cur").value = st.th.c;
  if (document.activeElement !== $("thr-vib"))  $("thr-vib").value = st.th.v;
  if (document.activeElement !== $("rate-ms"))  $("rate-ms").value = st.rate;

  renderEvents(st.ev || []);
}

function renderEvents(evs) {
  const body = $("ev-body");
  body.innerHTML = "";
  for (const ev of evs.slice(-12).reverse()) {
    const tr = document.createElement("tr");
    tr.className = ev.k === 0 ? "raise" : ev.k === 1 ? "clear" : "";
    const name = ev.s + (ev.b ? " (" + (ALM_BITS[ev.b] || ev.b) + ")" : "");
    const val = ev.k === 2 && ev.v < 10 ? "" : (ev.v / 100).toFixed(2);
    tr.innerHTML =
      "<td>" + fmtClock(ev.ts) + "</td>" +
      "<td>" + name + "</td>" +
      "<td>" + val + "</td>";
    body.appendChild(tr);
  }
}

/* ================= history / charts =============================== */

async function refreshHistory() {
  if (typeof CONN !== "undefined") CONN.histAt = Date.now();
  try {
    const h = await fetchJson("/api/history?win=" + S.win, 5000);
    if (!h || !Array.isArray(h.pts)) return;
    S.hist = [[], [], []];
    for (const p of h.pts) {
      S.hist[0].push([p[0], p[1]]);
      S.hist[1].push([p[0], p[2]]);
      S.hist[2].push([p[0], p[3]]);
    }
    charts.redraw();
  } catch (e) { /* offline; live appends continue */ }
}

/* Chart engine: zero dependencies, canvas 2D.
 * Each channel gets a time-series plot with auto-scaled Y, grid,
 * threshold line, min/max/avg and the live value. Redraws at 1 Hz. */

const CHARTS = [
  { cv: null, color: "#e8823e", unit: "C",  thrKey: "t", fmt: (v) => v.toFixed(2) },
  { cv: null, color: "#4aa3ff", unit: "A",  thrKey: "c", fmt: (v) => v.toFixed(3) },
  { cv: null, color: "#a06aff", unit: "g",  thrKey: "v", fmt: (v) => v.toFixed(2) },
];

const charts = {
  init() {
    CHARTS[0].cv = $("chart-temp");
    CHARTS[1].cv = $("chart-cur");
    CHARTS[2].cv = $("chart-vib");
    window.addEventListener("resize", () => this.resize());
    this.resize();
  },

  resize() {
    for (const c of CHARTS) {
      if (!c.cv) continue;
      const dpr = window.devicePixelRatio || 1;
      const w = c.cv.clientWidth || 560;
      c.cv.width = Math.round(w * dpr);
      c.cv.height = Math.round(170 * dpr);
    }
    this.redraw();
  },

  redraw() {
    if (!CHARTS[0].cv) this.init();
    const now = S.lastStatus ? S.lastStatus.ts : 0;
    const tEnd = now || (S.hist[0].length ? S.hist[0][S.hist[0].length - 1][0] : 0);
    const tStart = tEnd - S.win * 1000;

    for (let i = 0; i < CHARTS.length; i++) {
      this.drawOne(CHARTS[i], S.hist[i], tStart, tEnd,
                   S.lastStatus ? S.lastStatus.th[CHARTS[i].thrKey] : null);
    }
  },

  drawOne(c, pts, tStart, tEnd, threshold) {
    const cv = c.cv, ctx = cv.getContext("2d");
    const dpr = window.devicePixelRatio || 1;
    const W = cv.width / dpr, H = cv.height / dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);

    const padL = 42, padR = 10, padT = 10, padB = 20;
    const plotW = W - padL - padR, plotH = H - padT - padB;

    /* --- value range over the visible window (data + threshold) --- */
    let lo = Infinity, hi = -Infinity, n = 0, sum = 0;
    for (const [t, v] of pts) {
      if (t < tStart - 5000) continue;
      n++; sum += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (threshold != null && isFinite(threshold)) {
      lo = Math.min(lo, threshold);
      hi = Math.max(hi, threshold);
    }
    if (!isFinite(lo)) { lo = 0; hi = 1; }
    if (hi - lo < 1e-6) { hi = lo + Math.max(Math.abs(lo) * 0.05, 0.01); }
    const pad = (hi - lo) * 0.12;
    lo -= pad; hi += pad;

    const xOf = (t) => padL + ((t - tStart) / (tEnd - tStart)) * plotW;
    const yOf = (v) => padT + plotH - ((v - lo) / (hi - lo)) * plotH;

    /* --- grid + Y labels --- */
    ctx.strokeStyle = "#232b38";
    ctx.fillStyle = "#7d8899";
    ctx.font = "10px sans-serif";
    ctx.lineWidth = 1;
    const ticks = 4;
    for (let i = 0; i <= ticks; i++) {
      const v = lo + ((hi - lo) * i) / ticks;
      const y = Math.round(yOf(v)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(W - padR, y);
      ctx.stroke();
      ctx.fillText(c.fmt(v), 4, y + 3);
    }

    /* --- X labels: window-relative seconds --- */
    const xt = 4;
    for (let i = 0; i <= xt; i++) {
      const f = i / xt;
      const x = Math.round(padL + f * plotW) + 0.5;
      ctx.strokeStyle = "#232b38";
      ctx.beginPath();
      ctx.moveTo(x, padT);
      ctx.lineTo(x, padT + plotH);
      ctx.stroke();
      const s = Math.round((1 - f) * S.win);
      const lab = s >= 90 ? Math.round(s / 60) + "m" : s + "s";
      ctx.fillStyle = "#7d8899";
      ctx.fillText(lab, x - 8, H - 6);
    }

    /* --- threshold line --- */
    if (threshold != null && isFinite(threshold)
        && threshold >= lo && threshold <= hi) {
      ctx.strokeStyle = "#e8564e";
      ctx.setLineDash([5, 4]);
      const y = Math.round(yOf(threshold)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(W - padR, y);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    /* --- the series --- */
    ctx.strokeStyle = c.color;
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    let started = false, lastX = 0, lastY = 0;
    for (const [t, v] of pts) {
      if (t < tStart - 5000) continue;
      const x = xOf(t), y = yOf(v);
      if (!started) { ctx.moveTo(x, y); started = true; }
      else ctx.lineTo(x, y);
      lastX = x; lastY = y;
    }
    ctx.stroke();

    /* --- live dot + stats --- */
    if (started) {
      ctx.fillStyle = c.color;
      ctx.beginPath();
      ctx.arc(lastX, lastY, 3, 0, 2 * Math.PI);
      ctx.fill();
    }
    if (n > 0) {
      const mean = sum / n;
      let mn = Infinity, mx = -Infinity;
      for (const [t, v] of pts) {
        if (t < tStart - 5000) continue;
        if (v < mn) mn = v; if (v > mx) mx = v;
      }
      ctx.fillStyle = "#7d8899";
      ctx.font = "10px sans-serif";
      ctx.fillText(
        "n " + n + "   min " + c.fmt(mn) + "   avg " + c.fmt(mean) +
        "   max " + c.fmt(mx) + " " + c.unit,
        padL + 6, padT + 11);
    }
  },
};

function appendLivePoint(st) {
  const t = st.ts;
  S.hist[0].push([t, st.temp]);
  S.hist[1].push([t, st.cur]);
  S.hist[2].push([t, st.vib]);
  const cutoff = t - (S.win * 1000 + 5000);
  for (const ch of S.hist) {
    while (ch.length && ch[0][0] < cutoff) ch.shift();
  }
  charts.redraw();
}

/* ================= live connection =================
 *
 * Three layers, so the page keeps updating whatever goes wrong:
 *   1. WebSocket push (1 Hz) -- the normal path.
 *   2. Polling /api/status every second whenever nothing arrived in the
 *      last 2.5 s (socket down, stalled, or blocked by a proxy).
 *   3. A stale-socket watchdog: a socket that is "open" but silent for 6 s
 *      is closed and re-opened (half-dead TCP after WiFi drops).
 * Every fetch has a hard timeout so a hung request cannot freeze the
 * refresh loop, and the header shows exactly how fresh the data is.
 */

const CONN = {
  lastRx: 0,          // performance.now() of the last status, any path
  lastOkWall: 0,      // wall-clock of the last status
  wsTries: 0,
  polling: false,
  histAt: 0,
};

function setWsUp(up) {
  S.wsUp = up;
  $("ws-dot").classList.toggle("up", up);
}

function onStatus(st) {
  CONN.lastRx = performance.now();
  CONN.lastOkWall = Date.now();
  renderStatus(st);
  appendLivePoint(st);
  showConn(true);
}

/* Banner + chip showing connection state and data age. */
function showConn(ok) {
  const chip = $("hdr-wifi");
  const ftr = $("ftr-link");
  const ban = $("conn-banner");
  if (ok) {
    if (ban) ban.classList.add("hidden");
    return;
  }
  const age = CONN.lastOkWall
    ? Math.round((Date.now() - CONN.lastOkWall) / 1000) + " s ago" : "never";
  if (ban) {
    ban.classList.remove("hidden");
    $("conn-banner-text").textContent =
      "Dashboard not connected to the ESP32 — retrying every second " +
      "(last data: " + age + ")";
  }
  if (chip) chip.textContent = "offline";
  if (ftr) ftr.textContent = "ESP32 unreachable";
}

function connectWs() {
  let ws;
  try {
    const proto = location.protocol === "https:" ? "wss://" : "ws://";
    ws = new WebSocket(proto + location.host + "/ws");
  } catch (e) {
    setTimeout(connectWs, 2000);
    return;
  }
  S.ws = ws;

  ws.onopen = () => {
    CONN.wsTries = 0; setWsUp(true); refreshHistory();
    // Client->server traffic marks the socket "recently used" in httpd's LRU
    // table; without it the idle-looking WebSocket is the first one purged.
    clearInterval(S.wsPing);
    S.wsPing = setInterval(() => {
      try { if (ws.readyState === 1) ws.send("ping"); } catch (e) { /* closing */ }
    }, 8000);
  };
  ws.onmessage = (m) => {
    try {
      const st = JSON.parse(m.data);
      if (st && st.type === "status") onStatus(st);
    } catch (e) { /* ignore malformed */ }
  };
  ws.onclose = () => {
    clearInterval(S.wsPing);
    setWsUp(false);
    CONN.wsTries++;
    // 1 s, 2 s, 3 s ... capped at 5 s: quick recovery without hammering.
    setTimeout(connectWs, Math.min(5000, 1000 * CONN.wsTries));
  };
  ws.onerror = () => { try { ws.close(); } catch (e) { /* already closed */ } };
}

async function fetchJson(path, timeoutMs) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(path, { cache: "no-store", signal: ctl.signal });
    if (!r.ok) throw new Error("HTTP " + r.status);
    return await r.json();
  } finally {
    clearTimeout(timer);
  }
}

/* The refresh loop: runs every second, forever. */
async function refreshTick() {
  const idleMs = performance.now() - CONN.lastRx;

  // Watchdog: socket claims to be open but has been silent too long.
  if (S.wsUp && S.ws && idleMs > 6000) {
    try { S.ws.close(); } catch (e) { /* ignore */ }
  }

  // Poll whenever push data is not arriving on time.
  if (idleMs > 2500 && !CONN.polling) {
    CONN.polling = true;
    try {
      const st = await fetchJson("/api/status", 2500);
      if (st && st.type === "status") onStatus(st);
    } catch (e) {
      showConn(false);
    } finally {
      CONN.polling = false;
    }
  }

  // Heal the charts once a minute (and straight after any outage).
  if (Date.now() - CONN.histAt > 60000) refreshHistory();
}
setInterval(refreshTick, 1000);

// Coming back to the tab / network: refresh immediately.
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) { CONN.lastRx = 0; refreshTick(); }
});
window.addEventListener("online", () => { CONN.lastRx = 0; refreshTick(); });

/* ================= control wiring ================= */

function flash(el, ok, text) {
  el.textContent = text;
  el.className = "msg " + (ok ? "ok" : "err");
  setTimeout(() => { el.textContent = ""; el.className = "msg"; }, 3500);
}

$("thr-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const jobs = [
    [0, parseFloat($("thr-temp").value), "thr-temp"],
    [1, parseFloat($("thr-cur").value), "thr-cur"],
    [2, parseFloat($("thr-vib").value), "thr-vib"],
  ].filter((j) => isFinite(j[1]));

  let allOk = true, lastErr = "";
  for (const [id, v] of jobs) {
    const r = await post("/api/threshold", { id: id, value: v });
    if (!r.ok) { allOk = false; lastErr = r.err; }
  }
  flash($("thr-msg"), allOk, allOk ? "thresholds applied" : lastErr);
});

$("rate-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const ms = parseInt($("rate-ms").value, 10);
  const r = await post("/api/rate", { ms: ms });
  flash($("rate-msg"), r.ok, r.ok ? "sample rate applied" : r.err);
});

$("btn-led-g").addEventListener("click", async () => {
  S.led.g = !S.led.g;
  const r = await post("/api/led", { id: 0, on: S.led.g });
  $("btn-led-g").classList.toggle("on", r.ok && S.led.g);
  flash($("ctl-msg"), r.ok, r.ok ? "green LED " + (S.led.g ? "on" : "off") : r.err);
});

$("btn-led-r").addEventListener("click", async () => {
  S.led.r = !S.led.r;
  const r = await post("/api/led", { id: 2, on: S.led.r });
  $("btn-led-r").classList.toggle("on", r.ok && S.led.r);
  flash($("ctl-msg"), r.ok, r.ok ? "relay " + (S.led.r ? "tripped" : "released") : r.err);
});

$("btn-alarm-reset").addEventListener("click", async () => {
  const r = await post("/api/alarm/reset", {});
  flash($("ctl-msg"), r.ok, r.ok ? "alarm reset sent" : r.err);
});

$("btn-reboot-stm32").addEventListener("click", async () => {
  if (!confirm("Reboot the STM32 node?")) return;
  const r = await post("/api/reboot-stm32", {});
  flash($("ctl-msg"), r.ok, r.ok ? "reboot command acked" : r.err);
});

$("wifi-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const r = await post("/api/wifi/save", {
    ssid: $("wifi-ssid").value.trim(),
    pass: $("wifi-pass").value,
  });
  const msg = $("wifi-msg");
  const say = (t, ok) => {
    if (msg) { msg.textContent = t; msg.className = "msg " + (ok ? "ok" : "err"); }
    else { alert(t); }
  };
  if (r.ok) {
    say("Saved. The ESP32 is restarting and will join \"" +
        $("wifi-ssid").value.trim() + "\". Reconnect this device to that " +
        "WiFi, then open the IP printed on the ESP32 serial monitor.", true);
  } else if (/Failed to fetch|NetworkError|abort/i.test(String(r.err))) {
    // The reply can be lost if the ESP32 restarts (and its AP vanishes)
    // before the browser reads it: that normally means the save worked.
    say("Connection to the ESP32 dropped right after sending — it has most " +
        "likely saved and is restarting. Reconnect to your normal WiFi and " +
        "open the IP shown on the ESP32 serial monitor. If the " +
        "\"monitor-setup\" network is still there after 20 s, try again.", true);
  } else {
    say("Save failed: " + (r.err || "unknown error"), false);
  }
});

$("win-btns").addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-win]");
  if (!btn) return;
  S.win = parseInt(btn.dataset.win, 10);
  for (const b of $("win-btns").querySelectorAll("button")) {
    b.classList.toggle("sel", b === btn);
  }
  refreshHistory();
});

/* ================= boot ================= */

refreshHistory();
connectWs();
