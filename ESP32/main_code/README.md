# ESP32-S3 Industrial Monitor — Gateway + Dashboard

ESP-IDF project (target `esp32s3`, built against **v6.0.2**) for the
gateway half of the dual-MCU health monitor: it consumes the STM32's
telemetry over UART, serves a live WebSocket dashboard, speaks a serial
REPL, supervises the link with a two-layer watchdog, and persists
history/config to flash.

## Build & flash

```bash
# Windows (ESP-IDF 6.x PowerShell/CMD):
cd ESP32\main_code
idf.py set-target esp32s3        # once, if sdkconfig is deleted
idf.py build
idf.py -p COMx flash monitor     # 115200 8N1 for the console
```

The console (logs + `monitor>` REPL) is on the USB-UART bridge port.
First build fetches one managed component (`espressif/esp_littlefs`);
if your build host is offline, set `HMS_USE_LITTLEFS 0` in
`main/project_config.h`, delete `main/idf_component.yml`, and the
system runs RAM+NVS-only (documented degradation).

**Partition note:** `partitions.csv` reserves factory 2 MB + littlefs
1 MB — fits 4 MB and 16 MB modules alike. If you keep an old
`sdkconfig`, delete it once so `sdkconfig.defaults` (TWDT 15 s panic,
runtime stats) applies.

## First boot (provisioning)

With no stored credentials the ESP32 comes up as the access point
**`monitor-setup`** (password `12345678`) and hijacks DNS so phones
land on the captive portal. Open `http://192.168.4.1/`, enter your
WiFi SSID/password, and the device reboots into station mode and
prints its IP. Credentials live in NVS (`wifi set/clear` on the REPL
manage them).

## What runs on it

| Piece | Where |
|-------|-------|
| UART link (460800 8N1, GPIO17/18) | `main/Drivers/uart_link.c` |
| Protocol layer (shared with STM32) | `main/Protocol/` |
| Telemetry dispatch + state store | `main/Tasks/task_uart_rx.c`, `main/Services/dashboard_data.c` |
| Acked commands (3x retry) | `main/Services/command_dispatcher.c` |
| WiFi STA + provisioning AP + captive DNS | `main/Services/wifi_manager.c` |
| HTTP + WebSocket + REST | `main/Services/http_server.c`, `websocket_server.c` |
| Embedded dashboard (no CDN, vanilla JS charts) | `main/Web/` (EMBEDFILES) |
| Serial REPL (status/thr/rate/led/wifi/logs/tasks/hist…) | `main/Drivers/cli.c` |
| TWDT 15 s + supervisor escalation | `main/Tasks/task_watchdog.c` |
| LittleFS history/config/incident log | `main/Services/littlefs_storage.c` |
| CPU/watermark stats | `main/Services/system_stats.c` |

REST API (all JSON): `GET /api/status`, `GET /api/history?win=60..3600`,
`GET /api/logs`, `POST /api/threshold {id,value}`, `POST /api/rate {ms}`,
`POST /api/led {id,on}`, `POST /api/alarm/reset`, `POST /api/reboot-stm32`,
`GET|POST /api/wifi/...`. WebSocket push: `GET /ws` (1 Hz status).

## Dashboard

Dark industrial theme, zero dependencies: KPI cards, alarm banner,
three live canvas charts (1m/5m/15m/1h windows, threshold lines,
min/avg/max), controls (thresholds, sample rate, LED/relay, alarm
reset, STM32 reboot), event log, and the WiFi provisioning form. Falls
back to 2.5 s polling when the socket drops; refetches history on
reconnect.

## Verification status

- Host unit tests (same protocol/json/ring sources):
  `cd tests/host && make && ./test_host` → 51 checks.
- All C files pass `gcc -fsyntax-only -Wall -Wextra` against stub
  headers; JS validated with `node --check`.
- Not yet compiled with the real xtensa toolchain or run on hardware —
  run `docs/TEST_PLAN.md` §4–§8 on the bench.

See `docs/ARCHITECTURE.md` (system design), `docs/UART_PROTOCOL_SPEC.md`
(wire format), `docs/TEST_PLAN.md` (bring-up to soak).
