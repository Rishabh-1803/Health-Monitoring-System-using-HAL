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
  S.lastStatus = st;

  const alarmNames = [];
  for (const [bit, name] of Object.entries(ALM_BITS)) {
    if (st.alm & Number(bit)) alarmNames.push(name);
  }

  $("v-temp").textContent = st.temp.toFixed(2);
  $("v-cur").textContent = st.cur.toFixed(3);
  $("v-vib").textContent = st.vib.toFixed(2);
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

/* ================= history / charts (engine lands in Phase 8) ===== */

async function refreshHistory() {
  try {
    const r = await fetch("/api/history?win=" + S.win);
    const h = await r.json();
    S.hist = [[], [], []];
    for (const p of h.pts) {
      S.hist[0].push([p[0], p[1]]);
      S.hist[1].push([p[0], p[2]]);
      S.hist[2].push([p[0], p[3]]);
    }
    charts.redraw();
  } catch (e) { /* offline; live appends continue */ }
}

const charts = {
  redraw() {
    /* Phase 8 fills this in — live values keep the KPI cards current. */
    const stubs = [ $("chart-temp"), $("chart-cur"), $("chart-vib") ];
    stubs.forEach((cv) => {
      const ctx = cv.getContext("2d");
      ctx.clearRect(0, 0, cv.width, cv.height);
      ctx.strokeStyle = "#273040";
      ctx.strokeRect(0.5, 0.5, cv.width - 1, cv.height - 1);
      ctx.fillStyle = "#7d8899";
      ctx.font = "12px sans-serif";
      ctx.fillText("charts arrive in phase 8", 20, cv.height / 2);
    });
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

/* ================= websocket + fallback ================= */

function setWsUp(up) {
  S.wsUp = up;
  $("ws-dot").classList.toggle("up", up);
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss://" : "ws://";
  const ws = new WebSocket(proto + location.host + "/ws");
  S.ws = ws;

  ws.onopen = () => { setWsUp(true); refreshHistory(); };
  ws.onmessage = (m) => {
    try {
      const st = JSON.parse(m.data);
      if (st.type === "status") {
        renderStatus(st);
        appendLivePoint(st);
      }
    } catch (e) { /* ignore malformed */ }
  };
  ws.onclose = () => {
    setWsUp(false);
    setTimeout(connectWs, 2000);
  };
  ws.onerror = () => ws.close();
}

/* Polling fallback: when the socket is down, keep the page alive. */
setInterval(() => {
  if (S.wsUp) return;
  fetch("/api/status").then((r) => r.json()).then((st) => {
    if (st && st.type === "status") renderStatus(st);
  }).catch(() => {});
}, 2500);

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
  if (!r.ok) alert("save failed: " + (r.err || "?"));
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
