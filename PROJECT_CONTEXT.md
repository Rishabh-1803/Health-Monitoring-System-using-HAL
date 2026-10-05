# PROJECT_CONTEXT — Industrial Equipment Health Monitor

**Hand-off document for the next developer/AI session.**
Last updated: 2026-10-05, after the "complete system" implementation run.
Everything below describes the repository **as it is now** — verified
against the code, not aspirational.

---

## 1. Project overview

A dual-MCU industrial monitoring node. An **STM32F411 Blackpill**
("sensor head") samples DS18B20 temperature, ACS712 current and SW-420
vibration, runs the alarm engine, and streams telemetry to an
**ESP32-S3** ("gateway") over UART at 460800 8N1. The ESP32 joins
WiFi, serves a live web dashboard (WebSocket + historical charts),
runs a serial REPL CLI, supervises the link with a two-layer watchdog,
and persists history/config to LittleFS. The protocol uses
CRC-16/CCITT-Frames with ACK/NAK for commands and fire-and-forget
telemetry.

Goal: a demonstrable end-to-end system — sensor → alarm → browser.

## 2. Hardware (BOM + wiring)

| Part | Role | Price (INR, approx) |
|------|------|------|
| WeAct STM32F411CEU6 Blackpill | sensor head | 600 |
| ESP32-S3-DevKitC-1 N16R8 | gateway | 900 |
| DS18B20 (waterproof probe) | temperature | 150 |
| ACS712-5A module | current (5 V part — divider!) | 150 |
| SW-420 module | vibration | 80 |
| SSD1306 128×64 I²C OLED | bring-up diagnostics only | 350 |
| Passive buzzer, relay module, wires, pull-ups | | 300 |

**Pin map (STM32, source of truth: `bringup_config.h`)**
PC13 LED (active low, protocol "green") · PB0 buzzer (TIM3_CH3 PWM) ·
PB1 relay (active low, protocol "red" = industrial trip) · PB5 DS18B20
DQ (4.7k pull-up) · PA1 ACS712 via divider (3V3 max!) · PB12 SW-420 DO
· PB6/PB7 I²C to OLED · PA2/PA3 USART2 → ESP32 · PA11/PA12 USB CDC
console.

**ESP32:** GPIO17 TX → STM32 PA3, GPIO18 RX ← STM32 PA2, GND↔GND,
console on the USB-UART bridge (115200).

## 3. Software architecture

- **STM32** (`STM32/stm32-bringup`, STM32CubeIDE, CMSIS-RTOS2):
  CubeMX-generated Core/ + hand-written Application/ layer that the
  .ioc cannot wipe (bsp, console, uart_link, shared protocol, menu
  tests, `app_telemetry.c`). The telemetry app is a **single
  cooperative 10 ms loop** (every work slice bounded → the IWDG it
  feeds is a genuine liveness proof). Register-level IWDG ≈ 33 s.
- **ESP32** (`ESP32/main_code`, ESP-IDF v6.0.2, target esp32s3):
  11 FreeRTOS tasks + 12 services, layered one-way (Tasks → Services →
  Drivers → Protocol). `dashboard_data.c` is THE thread boundary
  (mutex-guarded store). Web assets are EMBEDFILES (no filesystem to
  serve). Charts are vanilla canvas, no CDN.
- Full details: **`docs/ARCHITECTURE.md`**.

## 4. Binary protocol

Full spec: **`docs/UART_PROTOCOL_SPEC.md`**. Summary:
`[0xAA][len][type][seq LE16][payload][crc LE16][0x55]`, CRC-16/CCITT-
FALSE, max 136 B. 16 message types. Telemetry = 20 B @ 5 Hz default
(50..1000 ms configurable). Commands ACK'd/NAK'd + 3× retry
(ESP32→STM32). Alarm bits: 1 overtemp · 2 overcurrent · 4 vibration ·
8 sensor-fail · 16 comm-fail; hysteresis 1.5 s raise / 3 s clear.
Protocol files byte-identical on both MCUs + compiled in host tests.

## 5. Completed phases (all verified statically, see §11)

- **Protocol layer** — crc16/packet/protocol_types + boot self-tests
  on both MCUs (pre-existing, phases 1–2).
- **STM32 bring-up menu** — 9 component tests, operator-confirmed
  verdicts (pre-existing).
- **Phase 3 — STM32 telemetry app** — non-blocking DS18B20 state
  machine, ADC EMA + rectify, SW-420 debounced edge window → est. g,
  hysteresis alarm engine, telemetry + 1 Hz heartbeat, command handler
  (ACK/NAK) for all 7 command types, relay/buzzer/LED actions,
  register-level IWDG, boot selector (app-first, 3 s key → menu, menu
  `r` → app, keypress in app → menu).
- **Phase 4 — ESP32 telemetry pipeline** — RX dispatch, dashboard_data
  store (events, thresholds mirror, 1 h history ring), command
  dispatcher (ACK matching, bounded retries, seq 0x8000+), TX mutex.
- **Phase 5 — WiFi + web** — NVS credentials, STA with 10-retry
  backoff, fallback SoftAP `monitor-setup`/`12345678` + hand-rolled
  DNS hijack captive portal, httpd (static + REST + history chunk
  streaming + captive redirects), WebSocket fan-out via
  httpd_queue_work, dark dashboard (KPIs, banner, controls, event
  log, provisioning form) with WS + polling fallback.
- **Phase 6 — CLI** — esp_console REPL: status/link/heap/uptime/tasks/
  logs/hist/thr/rate/led/alarm-reset/reboot-stm32/reboot/wifi.
- **Phase 7 — Supervision** — TWDT 15 s panic (uart_rx/uart_tx/
  dashboard subscribed), supervisor (60 s silent → STM32 reboot cmd;
  120 s → ESP32 self-reboot; never-linked exempt; heap-low logger),
  incident logger (RAM ring + flash), LittleFS (config.json, 1/min
  history CSV with 256 KB rotation, bounded log), system_stats (CPU
  via idle-task runtime accounting, watermarks), diagnostics sweep.
- **Phase 8 — Charts** — vanilla canvas engine: 3 channels, 1m/5m/15m/
  1h windows, auto-scaled Y, threshold lines, min/avg/max, live dot,
  HiDPI-aware, history backfill + live append.
- **Phase 9 — Testing** — 51 host unit checks (gcc), stdlib-only
  `tools/soak_test.py` (minimal RFC6455 client, rate/age/finiteness
  assertions, alarm drill, history checks, CSV export),
  `docs/TEST_PLAN.md` (9-section gate).

## 6. What to do next (bench bring-up order)

1. Build + flash STM32, run menu tests `1..9` (docs/TEST_PLAN.md §2).
2. Build + flash ESP32 (IDF ≥ 5.3; written for 6.0.2; first build
   fetches `espressif/esp_littlefs` — offline? set `HMS_USE_LITTLEFS 0`
   and delete `main/idf_component.yml`).
3. Provision WiFi via the captive portal; run TEST_PLAN §4–§6.
4. Run `tools/soak_test.py --host <ip> --minutes 30`; then the §7–§9
   watchdog + persistence drills.
5. Likely follow-up work: OLED status page in app mode (extract the
   driver from `test_oled.c`), mDNS (`health-monitor.local`), NTP
   timestamps on events, MQTT export, OTA, real MPU-6050/INA219
   support (protocol bits already reserved).

## 7. STM32 structure

```
STM32/stm32-bringup/
├── Core/                    CubeMX: freertos.c(LEDTask), usart.c(460800),
│                            gpio.c, main.c, usb_device, stm32f4xx_it.c
└── Application/
    ├── Inc/  bringup_config.h (ALL pins) · bsp.h · console.h ·
    │         uart_link.h · crc16.h packet.h protocol_types.h
    │         protocol_selftest.h · tests.h · app_init.h · app_telemetry.h
    └── Src/  bsp.c (DWT µs, bit-bang I²C, ADC1, buzzer PWM, 1-Wire) ·
              console.c (CDC, feeds IWDG) · uart_link.c (ISR ring) ·
              crc16/packet/protocol_selftest (shared) · test_*.c (9) ·
              app_init.c (boot selector) · app_telemetry.c (~700 lines)
```

Key decisions: single cooperative task instead of per-sensor tasks
(IWDG honesty); register-level IWDG (CubeMX regen immunity);
console_key_pressed doubles as the bench-mode WDT keep-alive; fire-and
-forget telemetry (self-healing stream); ACK only for commands.

## 8. ESP32 structure

```
ESP32/main_code/
├── CMakeLists.txt, partitions.csv (nvs 24K·otadata 8K·factory 2M·littlefs 1M)
├── sdkconfig.defaults (TWDT 15 s panic, RUN_TIME_STATS, 1 kHz tick)
└── main/
    ├── main.c                boot ladder (§ architecture)
    ├── project_config.h      task tunables + HMS_USE_LITTLEFS
    ├── Drivers/              uart_link.c (TX mutex) · cli.c (REPL, 13 cmds)
    ├── Protocol/             crc16 · packet · protocol_types · protocol_selftest
    ├── Services/             dashboard_data · command_dispatcher ·
    │                         wifi_manager · http_server · websocket_server ·
    │                         system_stats · logger · littlefs_storage ·
    │                         history_ring · json_util   (all pure where possible)
    ├── Tasks/                uart_rx · uart_tx · dashboard · wifi · webserver ·
    │                         cli · watchdog · diagnostics · logger
    └── Web/                  index.html · style.css · app.js (embedded)
```

Key decisions: dashboard_data as the only shared-state boundary;
httpd_queue_work + httpd_ws_send_frame_async for WS push (handler
returns per frame — never blocks httpd); embedded web assets;
decoupled rate domains (UART 5 Hz → ring 1 Hz → WS 1 Hz → flash 1/min);
polling fallback; supervisor exempt until first link.

## 9. Build instructions

**STM32:** import `STM32/stm32-bringup` into STM32CubeIDE → build →
flash → USB CDC terminal (any baud) → 3 s key window.

**ESP32 (Windows, IDF 6.x):** `cd ESP32\main_code` → `idf.py build` →
`idf.py -p COMx flash monitor`. If an old `sdkconfig` misbehaves,
delete it (defaults re-apply). Partition table changed in Phase 7 —
`idf.py flash` rewrites it; a full `erase_flash` is cleanest once.

**Host tests:** `cd tests/host && make && ./test_host` (51 checks).
**Soak:** `python tools/soak_test.py --host <ip> --minutes 30 --csv
soak.csv`.

## 10. Verification status (be precise about this)

✅ **Done:** all 28 C files pass `gcc -fsyntax-only -std=gnu11 -Wall
-Wextra` against faithful stub headers (two API bugs caught this way:
ws_send_frame variant, missing string.h); 51/51 host unit tests on the
real protocol/ring/json sources; `node --check` on app.js; py_compile
on soak_test.py; JS chart engine logic reviewed; every commit built
from a clean tree.

❌ **Not done (needs hardware + real toolchains):** xtensa/arm compile,
on-target boot, bring-up menu re-run, telemetry exchange, provisioning,
watchdog drills, soak. The code is written to run, not yet proven to
run — follow `docs/TEST_PLAN.md` top-to-bottom.

## 11. Important notes (the traps)

1. **ACS712 is a 5 V part** — output idles at 2.5 V; PA1 tolerates
   3V3. Divider or 3V3-native sensor only.
2. **Vibration is an estimate** (edges/s ÷ 50 = "g"); calibrate
   `VIB_EDGES_PER_G` in `app_telemetry.c`; threshold uses the same
   units.
3. Yellow LED is **not wired** — protocol id 1 is ACK'd + logged as
   no-op. Red = relay, green = PC13.
4. STM32 boots **into the app** after ~3 s of no keypress; a human at
   the menu keeps the IWDG fed via console polling.
5. ESP32 `esp_console` REPL and the supervisor deliberately do NOT
   subscribe to the TWDT (idle prompts ≠ fault).
6. `sdkconfig` is committed (user's flow); Phase 7 options were
   appended to BOTH it and `sdkconfig.defaults`. Deleting `sdkconfig`
   once regenerates from defaults cleanly.
7. esp_littlefs is a managed component — needs one online build;
   `HMS_USE_LITTLEFS 0` + removing `main/idf_component.yml` is the
   offline fallback (RAM+NVS mode, thresholds fall back to defaults).
8. Telemetry fire-and-forget is a **design decision**, not a gap —
   see protocol spec §2 for the rationale.
9. The 48 MB of `DatasheetsAndManuals/` is untouched reference PDFs.
10. The old ZIP's uncommitted deletions (`ESP32/freertos-warmup/`)
    were committed as housekeeping first — history is clean.

## 12. Design decisions worth knowing ("why this, not that")

| Decision | Rationale |
|----------|-----------|
| One cooperative STM32 task vs per-sensor tasks | bounded slices make the IWDG a true liveness proof; no inter-task race surface |
| Register-level IWDG, not HAL | CubeMX regen would wipe hal_conf edits |
| Fire-and-forget telemetry | CRC + 200 ms refresh beats retransmission cost at 5 Hz |
| ACK + 3× retry for commands only | state changes need confirmation; streams don't |
| dashboard_data single store | one mutex boundary, no shadow state, JSON built under one lock |
| httpd_queue_work for WS push | documented-safe async send; handler never blocks httpd |
| EMBEDFILES web assets | dashboard works with zero filesystem deps |
| SoftAP + DNS hijack provisioning | no extra component, works on any phone, reboot = clean mode switch |
| Vanilla canvas charts | LAN-offline, zero deps, ~300 lines vs a CDN dependency |
| NVS for WiFi creds, LittleFS for data | NVS is guaranteed + tiny; FS degrades gracefully |
| Supervisor exempts never-linked setups | bench STM32-in-menu must not reboot the ESP32 forever |
| `HMS_USE_LITTLEFS` guard | online component fetch can fail; degradation documented |

## 13. Git history (phase per commit)

```
dc91a4c fix: drop unused linenoise include
b50a6e5 Docs: protocol spec, architecture, READMEs
27409b0 Phase 9: host unit tests, soak-test tool, test plan
32faab6 Phase 8: historical charts (vanilla canvas)
7348624 Phase 7: watchdogs, supervisor, logging, diagnostics, storage
164585c Phase 6: ESP32 CLI (esp_console REPL)
155c49c Phase 5: WiFi manager + HTTP/WS server + live dashboard
348b74f Phase 4: ESP32 telemetry pipeline
c6560d7 Phase 3: STM32 continuous telemetry application
013d789 chore: remove freertos-warmup scratch project
7eff0eb (previous) ESP32↔STM32 link working — heartbeat exchange
```

## 14. How to continue with another AI

Give it this file + the repository (or ZIP). Useful first prompts:
- "Run docs/TEST_PLAN.md §2–§5 on hardware and fix what fails" (bench
  bring-up — expect small API/wiring fixes)
- "Add the OLED status page to app mode" (extract driver from
  test_oled.c into a reusable module)
- "Add mDNS + NTP timestamps, then MQTT export"
- "Port the soak test results into a report and fix the leak it found"
Everything it needs is here or in `docs/`.
