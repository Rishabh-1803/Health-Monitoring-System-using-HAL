# System Architecture — Industrial Equipment Health Monitor

```
                 sensors                     UART 460800 8N1
  ┌───────────────────────────┐              ┌──────────────────────────┐
  │ STM32F411 Blackpill       │  telemetry   │ ESP32-S3                 │
  │ (sensor head)             │─────────────>│ (gateway + dashboard)    │
  │                           │  alarms      │                          │
  │ DS18B20  PB5   1-Wire     │              │ WiFi STA / provisioning  │
  │ ACS712   PA1   ADC1_IN1   │<─────────────│ HTTP + WebSocket server  │
  │ SW-420   PB12  digital    │  commands    │ esp_console REPL         │
  │ LED      PC13             │  ACK/NAK     │ LittleFS history/config  │
  │ Buzzer   PB0   TIM3 PWM   │              │ TWDT + supervisor        │
  │ Relay    PB1   trip       │              │ NVS wifi credentials     │
  │ USB CDC  console          │              │ USB-UART console         │
  │ IWDG     ~33 s            │              │ IWDG-equivalent: TWDT 15s│
  └───────────────────────────┘              └────────────┬─────────────┘
                                                          │ WiFi
                                                   ┌──────┴──────┐
                                                   │ Browser(s)  │
                                                   │ dashboard   │
                                                   └─────────────┘
```

---

## 1. Design principles

1. **One truth per fact.** All live state passes through
   `dashboard_data` on the ESP32 (mutex-guarded store) and through the
   single telemetry loop on the STM32. Nothing else keeps shadow state.
2. **The protocol layer is shared code.** `crc16`, `packet`,
   `protocol_types` are byte-identical on both MCUs and compile
   unchanged in the host unit tests.
3. **Telemetry is lossy, commands are confirmed.** A CRC-bad telemetry
   frame costs one sample period; a command costs a state change, so
   it is ACK'd, NAK'd, and retried with bounds.
4. **Degrade, never brick.** LittleFS missing → RAM + NVS. WiFi
   credentials missing → provisioning AP. STM32 absent → comm-fail
   alarm + supervisor, but a never-linked bench setup never escalates.
5. **Boot is a ladder.** Self-test → stores → link → wifi → servers →
   CLI → supervision. Each rung logs its own success; a failure at the
   protocol self-test stops the climb.

## 2. STM32 firmware (`STM32/stm32-bringup`)

CubeMX skeleton (Core/, Drivers) + an Application/ layer that owns
everything the generator would wipe:

| Module | Role |
|--------|------|
| `bsp.{c,h}` | pins, DWT µs timing, bit-bang I²C, ADC1, buzzer PWM, 1-Wire primitives |
| `console.{c,h}` | USB CDC line console (ring buffers both ways) |
| `uart_link.{c,h}` | USART2 ISR RX ring + blocking TX |
| `packet/crc16/protocol_types` | shared protocol layer |
| `protocol_selftest.c` | CRC + packet vectors at boot |
| `test_*.c` | bring-up menu tests (one per component) |
| `app_init.c` | boot selector: 3 s key window → menu, else telemetry app |
| `app_telemetry.c` | **the application**: 10 ms cooperative loop — ADC EMA, vibration edge window, DS18B20 non-blocking state machine, hysteresis alarm engine, telemetry/heartbeat TX, command handling + ACK/NAK, relay/buzzer/LED outputs, register-level IWDG |

**Task map:** LEDTask (CubeMX, USB init + 1 Hz blink), bringup task
(8 KB stack; runs boot selector, then menu **or** telemetry app).
The telemetry app is deliberately single-task cooperative: every
slice of work is bounded well under one tick, so the IWDG fed at the
top of the loop is a genuine liveness proof for the whole pipeline.

**Why register-level IWDG:** the CubeMX project never enabled the IWDG
HAL module, and editing `stm32f4xx_hal_conf.h` would be wiped by the
next `.ioc` regeneration. Five register writes cannot be regenerated
away. The console's wait paths (`console_key_pressed`,
`console_readline`) feed the IWDG too, so an operator in the menu
never trips a reset; a hung telemetry loop stops feeding → reset.

## 3. ESP32 firmware (`ESP32/main_code`)

Layered, with the same one-way dependency rule (top depends down):

```
Tasks (FreeRTOS loops)      Services                   Drivers        Protocol
─────────────────────       ───────────────────        ───────        ────────
task_uart_rx   ─────┬────>  dashboard_data   ─────┐
task_uart_tx         │     command_dispatcher──┐  │    uart_link       crc16
task_dashboard ─────┼────> websocket_server    │  │                    packet
task_wifi           │     http_server          │  │                    protocol
task_webserver ─────┘     wifi_manager         │  │                    _types
task_cli (REPL)           system_stats         │  │
task_watchdog            littlefs_storage  <───┘  │
task_diagnostics         logger               <────┘
task_logger
```

| Task | Cadence | Job |
|------|---------|-----|
| uart_rx | event | decode + dispatch (telemetry/alarm/ack/nak/debug), TWDT-subscribed |
| uart_tx | 1 Hz | heartbeat to STM32, comm-fail state machine, TWDT-subscribed |
| dashboard | 1 Hz | history sample, stats, status JSON, WS broadcast, config sync once, 1/min flash history |
| wifi | 2 s | RSSI/state into the store (connection logic is event-driven in wifi_manager) |
| webserver | boot | start httpd once IP or AP exists |
| cli | boot | esp_console REPL task (own task, not TWDT-subscribed) |
| watchdog | 5 s | TWDT reconfigure + supervisor escalation |
| diagnostics | 10 s | CPU/watermark sweep, 1/min health log |
| logger | 5 min | flash flush safety net |

**Rate domains** (deliberately decoupled):
- UART: 5 Hz telemetry (configurable 50..1000 ms) + 1 Hz heartbeat
- Store: 1 Hz decimation into the 3600-sample history ring (1 h)
- WS push: 1 Hz status JSON (~1–2 KB)
- Flash: 1 history CSV line/min, log flush 5 min (wear budget in
  `littlefs_storage.c`)

**Web assets** are EMBEDFILES — compiled into the firmware image, no
filesystem dependency to serve the dashboard. Charts are a
zero-dependency canvas engine; the provisioning form is the same page
reacting to `esp.wifi == 0`.

## 4. Boot sequences

**STM32:** `bsp_init` (pins safe before the scheduler!) → I²C recover →
console init → LEDTask starts USB → 3 s console wait → key? menu : 
`app_telemetry_run()` (any key inside returns to the menu; menu `r`
re-runs it).

**ESP32:** banner → protocol self-test (halt on fail) →
`dashboard_data`/dispatcher/logger init → NVS (reformat-if-needed) →
wifi_manager (STA or provisioning AP) → LittleFS mount + stored config
→ uart link + RX/TX → dashboard/webserver/wifi/cli/watchdog/
diagnostics/logger tasks → return.

## 5. Failure model

| Failure | Detection | Response |
|---------|-----------|----------|
| UART CRC error | decoder | drop frame; next sample heals |
| Command lost | dispatcher | 3 retries, then reported to caller |
| STM32 silent 5 s | either side | COMM_FAIL alarm + event |
| STM32 silent 60/120 s | ESP32 supervisor | reboot command / self-reboot |
| ESP32 task hang | TWDT (15 s) | panic + reboot |
| STM32 app hang | IWDG (~33 s) | reset |
| WiFi credentials bad | 10 retries | provisioning AP |
| LittleFS unmountable | mount check | RAM/NVS-only mode |
| Heap < 15 KB | supervisor | logged every 5 min |

## 6. Repository map

```
Health-Monitoring-System-using-HAL/
├── ESP32/main_code/          ESP-IDF 6.x project (esp32s3)
│   ├── main/
│   │   ├── main.c            boot ladder
│   │   ├── project_config.h  task tunables + HMS_USE_LITTLEFS
│   │   ├── Drivers/          uart_link, cli (REPL)
│   │   ├── Protocol/         shared protocol layer + self-test
│   │   ├── Services/         dashboard_data, command_dispatcher,
│   │   │                     wifi_manager, http_server, websocket_server,
│   │   │                     system_stats, logger, littlefs_storage,
│   │   │                     history_ring, json_util
│   │   ├── Tasks/            11 FreeRTOS task modules
│   │   └── Web/              embedded dashboard (html/css/js)
│   ├── partitions.csv        nvs 24K · otadata 8K · factory 2M · littlefs 1M
│   └── sdkconfig.defaults    TWDT 15 s panic, RUN_TIME_STATS, 1 kHz tick
├── STM32/stm32-bringup/      STM32CubeIDE project (F411)
│   ├── Core/                 CubeMX-generated (freertos.c, usart.c, gpio…)
│   └── Application/          bsp, console, uart_link, protocol, tests,
│                             app_init (boot selector), app_telemetry
├── tests/host/               host unit tests (make && ./test_host)
├── tools/soak_test.py        stdlib-only integration/soak test
└── docs/                     this file, UART_PROTOCOL_SPEC, TEST_PLAN
```

`DatasheetsAndManuals/` folders carry the component PDFs; the hidden
`.git` history contains one commit per phase.
