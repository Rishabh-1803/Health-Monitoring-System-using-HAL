# Test Plan — Industrial Equipment Health Monitor

Dual-MCU node: STM32F411 (sensor head) ↔ ESP32-S3 (gateway + dashboard).
This plan covers every layer from host-side unit tests to the soak run
that gates a "demo ready" verdict.

---

## 1. Host unit tests (no hardware)

**What:** the pure-C modules shared with the firmware — `crc16`,
`packet`, `history_ring`, `json_util`.

**How:**
```bash
cd tests/host
make && ./test_host
```

**Pass criteria:** `51 passed, 0 failed` on the console. Any failure
here is a firmware bug (the same source files build into both images).

**Covered:**
- CRC-16/CCITT-FALSE against independently computed vectors
  (`""→0xFFFF`, `"123456789"→0x29B1`, …)
- Packet roundtrip (type/seq/payload/CRC), resync after 20 garbage
  bytes, corrupted payload → `PACKET_ERR_CRC`, bad footer, oversize
  length, encoder guard return codes
- History ring: oldest-first ordering, overwrite at capacity,
  `copy_since` filtering and bounds
- JSON writer: exact escaping output, control chars, bounded
  truncation; scanners: `json_find_*`, substring-key guard,
  urlencoded forms with `%XX` and `+`

---

## 2. STM32 bring-up gate (menu mode)

**Prerequisite:** the board enumerates as a USB CDC device; open a
terminal at the console (any baud — CDC ignores it) and press any key
within 3 s of the banner to enter the menu.

| Menu item | Checks | Pass criteria |
|-----------|--------|---------------|
| 1 outputs | LED / buzzer / relay | operator confirms each |
| 2 I2C scan | bus + pull-ups | OLED (0x3C) answers |
| 3 OLED | panel + framebuffer | visible pattern |
| 4 DS18B20 | 1-Wire timing | CRC ok, plausible °C |
| 5 ADC | ACS712 level + noise | mV tracks load, p2p < 200 mV |
| 6 vibration | SW-420 edges | transitions when tapped |
| 7 ESP32 link | UART wiring | echo/loopback works |
| 8 protocol | CRC + packet self-test | PASS |
| 9 UART heartbeat | 460800 8N1 exchange | packets received, 0 CRC fails |

`a` runs everything in order and prints a summary. Fix any FAIL before
moving on — every later phase assumes this hardware is good.

---

## 3. Telemetry application (app mode)

Boot **without** pressing a key (or run menu item `r`).

**Expected within ~5 s (console):**
- `Telemetry application (Phase 3)` banner
- periodic status line every 10 s: `T x/10 C I x/100 A V x/100 g ...`
- no watchdog resets (IWDG ≈ 33 s, fed every 10 ms)

**Drills:**
1. **Sensor sanity** — warm the DS18B20 with a finger: temperature
   rises smoothly (EMA-filtered), no SENSOR_FAIL.
2. **Unplug DS18B20** — after 5 failed reads (≈ 10 s) the sensor-fail
   alarm fires; replug → clears.
3. **Keypress exit** — any key returns to the bring-up menu; `r` runs
   the app again. IWDG stays fed in the menu (console keep-alive).
4. **Alarm actions** — threshold-drill via the dashboard (§5) shows on
   the console as `[EVT] ... RAISED/CLEARED`.

---

## 4. ESP32 boot + link (serial console, 115200 8N1)

Watch the boot log over the USB-UART bridge.

**Expected sequence:**
1. `Phase 7 — Watchdog + diagnostics + storage` banner
2. `Protocol self-test passed`
3. WiFi: `got IP <ip>` (or the provisioning AP banner, §6)
4. `Telemetry pipeline running`, `All services running`
5. `Dashboard: http://<ip>/`

**REPL checks (type after the `monitor> ` prompt):**
- `status` — link UP, telemetry age < 1 s, thresholds listed
- `link` — rx/tx counters climbing, `crc errors: 0`
- `thr` / `rate 500` / `led g on` — each answers `OK (acked by STM32)`
- `tasks` — task table with watermarks (needs RUN_TIME_STATS)
- `logs` — incident lines (alarm raise/clear from drills)
- `hist 10` — last ten 1 Hz samples

---

## 5. Dashboard + alarm drill (browser)

Open `http://<ip>/`.

**Checks:**
- KPI cards live-update at 1 Hz; WS dot green (falls back to 2.5 s
  polling when the socket drops — verify by killing the tab's socket)
- charts draw after `/api/history` backfill; window buttons (1m/5m/15m/1h)
  refetch
- **Threshold drill:** set temp threshold to 5 °C → alarm banner +
  overtemp KPI goes red within ~4 s (1.5 s confirm on the STM32);
  event appears in the log; restore to 60 → clears within ~4 s (3 s
  hysteresis) → `Reset alarms` button ACKs
- Relay/buzzer: on overtemp raise the relay clicks (LED red = relay),
  buzzer sounds ≤ 3 s; `alarm reset` releases the trip
- Controls all report ok/err toasts; `Reboot STM32` re-establishes
  telemetry within ~5 s

**CLI equivalent:** the same drill from §4 (`thr set t 5`).

---

## 6. WiFi provisioning

1. `wifi clear` on the REPL (or erase NVS) → ESP32 reboots into the AP
2. Join `monitor-setup` / `12345678` from a phone or laptop
3. The captive portal opens the dashboard automatically (DNS hijack);
   otherwise browse to `http://192.168.4.1/`
4. Enter real SSID/password → save → device reboots into station mode
   and appears on the network

**Edge cases:** wrong password → 10 retries with backoff → falls back
to the provisioning AP; `wifi set <ssid> <pass>` from the REPL does the
same without the portal.

---

## 7. Watchdog supervision (the drill that matters)

With the system running in app mode:

1. **Kill the STM32** (power or UART wire). Within 5 s the dashboard
   shows `STM32 LINK DOWN`; `/api/logs` gains `COMM FAIL`.
2. After ~60 s: `supervisor` logs `STM32 silent — sending reboot`
   (STM32 stays dead if powered off).
3. After ~120 s: `rebooting ESP32` — the ESP32 restarts cleanly.
4. Power the STM32 back up: link recovers, `COMM RESTORED` logged,
   thresholds re-pushed (config sync) and verified via RESP_STATUS.
5. **STM32-side IWDG:** stall the app (e.g. hold in a debugger or
   crash it) → the STM32 self-resets within ~35 s and telemetry
   resumes. In menu mode the IWDG is fed by the console loops — verify
   no resets during a 15 s vibration test.
6. **ESP32 TWDT:** (advanced) hang a subscribed task via a debugger →
   panic + reboot within 15 s.

---

## 8. Soak run (the gate)

```bash
python tools/soak_test.py --host <ip> --minutes 30 --csv soak.csv
```

**Pass criteria (printed by the tool):**
- `GET /api/status` OK; WS handshake OK
- push rate ≥ 0.8 Hz across the whole window
- every status: link up, telemetry age < 5 s, finite temp/cur/vib
- threshold drill: raise ≤ 20 s, clear ≤ 20 s, reset ACK
- history: points present, monotonic timestamps, finite values

**Then review:**
- `soak.csv` — no gaps longer than 3 s (1 Hz stream)
- `/api/logs` — no unexpected supervisor/heap-low lines
- `status` heap: drift from boot < 20 KB (no leak)
- STM32 status line CPU: single digits %, heap stable
- LittleFS (if enabled): `/lfs/hist0.csv` grows ~1 line/min;
  `config.json` matches the thresholds after a reboot

**Overnight variant:** `--minutes 720 --no-drill` after the drills
pass once; verify the 1 h chart window fills and the event log shows
only expected entries (e.g. DS18B20 glitches if the bench is noisy).

---

## 9. Flash/reboot persistence

1. Set thresholds (e.g. 45 / 3 / 0.4) and rate 250 via the dashboard
2. Reboot the ESP32 (`reboot` CLI)
3. After link-up the config sync re-pushes them; `thr` shows the stored
   values; a RESP_STATUS confirms the STM32 agrees
4. Power-cycle both boards — values survive (LittleFS `config.json`)
5. With `HMS_USE_LITTLEFS 0`: system still boots and runs; thresholds
   fall back to defaults (documented degradation)
