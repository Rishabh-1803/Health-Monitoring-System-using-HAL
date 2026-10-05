# Industrial Equipment Health Monitoring System

A dual-MCU bench instrument: an STM32F411 Blackpill reads a DS18B20
temperature probe, an ACS712 current sensor and an SW-420 vibration
switch, and streams filtered telemetry to an ESP32-S3 over UART
(460800 8N1, CRC-16-checked packets). The ESP32 joins WiFi, serves a
live dashboard (WebSocket push + historical charts), relays commands
back to the STM32 with ACK/retry, and both sides supervise each other
with watchdogs.

```
DS18B20 ─┐                                    ┌─ WiFi ── browser(s)
ACS712 ──┤ STM32F411 ── UART 460800 8N1 ── ESP32-S3 ── serial REPL console
SW-420 ──┘  sensors + alarms    telemetry/commands   dashboard + supervision
```

## Quick start

1. **STM32** — open `STM32/stm32-bringup` in STM32CubeIDE, build,
   flash. A terminal on the board's USB port shows the bring-up menu
   (press any key within 3 s) or boots straight into the telemetry app.
   Verify hardware with menu items `1..9` before anything else.
2. **ESP32** — `cd ESP32/main_code && idf.py build flash monitor`
   (ESP-IDF v6.x). Join the `monitor-setup` AP to provision WiFi.
3. **Dashboard** — open the printed IP; run the drills in
   `docs/TEST_PLAN.md`.
4. **Host tests** — `cd tests/host && make && ./test_host` (51 checks,
   no hardware needed).
5. **Soak** — `python tools/soak_test.py --host <ip> --minutes 30`.

## Documentation

| Doc | Contents |
|-----|----------|
| `docs/ARCHITECTURE.md` | system design, task map, failure model, repo map |
| `docs/UART_PROTOCOL_SPEC.md` | full wire protocol: frames, 16 message types, CRC, ACK/retry |
| `docs/TEST_PLAN.md` | from host unit tests to the 30-min soak gate |

## Hardware map (single source: `bringup_config.h`)

| Pin | Peripheral | Notes |
|-----|------------|-------|
| PC13 | onboard LED | protocol "green" LED |
| PB0 | buzzer | TIM3 PWM, passive |
| PB1 | relay | protocol "red" — industrial trip |
| PB5 | DS18B20 DQ | 4.7k pull-up to 3V3 |
| PA1 | ACS712 out | 3V3 max — divider required |
| PB12 | SW-420 DO | internal pull-up |
| PB6/PB7 | I2C | SSD1306 OLED (bring-up menu only) |
| PA2/PA3 | USART2 | crossed to ESP32 GPIO18/17 |

## Layout

```
ESP32/main_code     ESP-IDF project (esp32s3) — gateway + dashboard
STM32/stm32-bringup STM32CubeIDE project — sensor head + bring-up tool
tests/host          host unit tests (pure C, gcc)
tools/soak_test.py  stdlib-only integration/soak test
docs/               architecture, protocol spec, test plan
```

The `.git` history carries one commit per phase (3→9), so
`git log --oneline` reads as the build order.

Verification status: statically verified (host gcc, 51 unit checks,
JS/Python syntax) — not yet compiled with the xtensa/arm toolchains or
run on hardware. Start with `docs/TEST_PLAN.md`.
