# CHANGES — what was fixed and how to use it now

This document summarises every fix made in this pass and gives you the
step-by-step for the two things you asked how to do: **open the ESP32 web
dashboard** and **confirm/correct the sensor assumptions**.

Working copy of the firmware: `/home/z/my-project/firmware/`
(original upload untouched in `/home/z/my-project/upload/`).

---

## 1. OLED now shows a live status page (was: blank in app mode)

**Before:** the SSD1306 driver lived only inside `test_oled.c` as `static`
functions, reachable from menu test #3 but never from the running telemetry
app — so the panel went dark the moment the app auto-started.

**After:** a new reusable module `Application/Inc/oled.h` +
`Application/Src/oled.c` exposes `oled_init / oled_clear / oled_text /
oled_pixel / oled_flush_page / oled_flush_all`. `app_telemetry.c` now calls
`oled_init()` at boot and, inside the 10 ms loop:
- rebuilds the framebuffer every **250 ms** (`oled_render_status()`) with
  temperature, current, vibration, alarm bits, sensor presence, uptime, CPU
  load and link state;
- flushes **one page per tick** (round-robin 0..7) so the whole 128×64
  screen refreshes in ~80 ms (≈12 fps) without ever blocking long enough to
  overflow the 256-byte UART RX ring.

The OLED layout (6 lines, uppercase 5×7 font):
```
HEALTH MONITOR
T 25.4 C    I 0.42 A
V 0.03 g
ALM OK      S:TIV
UP 00:01:23
CPU 3%      LINK OK
```
`S:TIV` = sensor presence: T=DS18B20, I=INA219, V=MPU6050 (`-` if missing).

## 2. Console updates every 0.5 s (was: every 10 s)

`STATUS_PRINT_MS` changed from `10000u` to `500u` in `app_telemetry.c`. The
telemetry line:
```
T 254/10 C   I 42/100 A   V 3/100 g   ALM 0x00   CPU 3/100 %   link ok
```
now prints twice per second. Telemetry to the ESP32 was already 5 Hz (200 ms)
and is unchanged.

## 3. Real-time, 8-sample-averaged current from INA219 (was: ACS712 analog)

**Before:** `sample_adc()` read the ADC on PA1 and scaled it with the ACS712
constants — wrong hardware, wrong values.

**After:** new `Application/Src/ina219.c` + `ina219.h` driver reads the INA219
over I2C (shared PB6/PB7 bus). `sample_current()` reads the shunt current
**every tick (100 Hz)** and keeps an **8-sample rolling average**
(`CUR_AVG_N = 8`) — exactly the "average of 8 values" you described, but
updating in real time every 10 ms. Current = V_shunt / R_shunt, so it is
correct by physics, not by a calibrated scale factor.

Sensor-ok bit `SENSOR_BIT_INA219_OK` (0x04) is now set in the telemetry
`sensor_status` byte when the INA219 answers.

## 4. Real vibration in g from MPU6050 (was: SW-420 edges/s estimate)

**Before:** `sample_vibration()` counted debounced edges on PB12 and divided
by 50 to fake a "g" number. With an MPU6050 on the bus instead, PB12 reads
garbage → "no values".

**After:** new `Application/Src/mpu6050.c` + `mpu6050.h` driver wakes the
MPU6050, sets ±2 g full scale, and reads the acceleration magnitude
`sqrt(ax²+ay²+az²)`. A slow baseline (init = 1 g for gravity) is tracked so
mounting orientation does not matter; `vibration_g` is the **high-passed
dynamic component in true g** — directly comparable to the threshold, no
calibration factor needed. Sampled every tick (100 Hz), smoothed with a light
EMA.

Sensor-ok bit `SENSOR_BIT_MPU6050_OK` (0x02) is now set in the telemetry
`sensor_status` byte when the MPU6050 answers.

## 5. I2C bus bumped to 250 kHz

`I2C_HALF_BIT_US` changed from `5` (~100 kHz) to `2` (~250 kHz). All three
I2C devices (SSD1306, INA219, MPU6050) support 400 kHz, so 250 kHz is within
spec. This is what makes a one-page OLED flush (~4.6 ms) fit inside the 10 ms
tick without overflowing the UART RX ring (which fills in 5.6 ms at 460800
baud).

## 6. Stale baud-rate documentation corrected

`bringup_config.h` claimed `ESP32_LINK_BAUD 115200` and "CubeMX at 115200",
but the real CubeMX `usart.c` and the ESP32 both run at **460800**. The
constant and comment now say 460800. (The link always worked — both sides
agreed — the doc was just misleading.)

## 7. Files added / changed

| File | Status |
|------|--------|
| `Application/Inc/bringup_config.h` | edited (INA219/MPU6050 config, I2C speed, baud doc) |
| `Application/Inc/oled.h` | **new** |
| `Application/Src/oled.c` | **new** |
| `Application/Inc/ina219.h` | **new** |
| `Application/Src/ina219.c` | **new** |
| `Application/Inc/mpu6050.h` | **new** |
| `Application/Src/mpu6050.c` | **new** |
| `Application/Src/app_telemetry.c` | edited (INA219/MPU6050/OLED, 0.5 s console, 8-sample avg) |

The new `.c` files live in `Application/Src/`, which STM32CubeIDE already
treats as a source folder, so they are picked up by the build automatically —
no `.cproject` edit needed. The menu tests (1–9) are untouched and still work.

## 8. Verification done so far

- `gcc -fsyntax-only -std=gnu11 -Wall -Wextra` on every `Application/Src/*.c`
  → **0 errors** (only the expected 64-bit-vs-32-bit CMSIS pointer-cast
  warnings that are host-only artifacts).
- Host unit tests `cd tests/host && make && ./test_host` → **51 passed, 0
  failed** (shared protocol/ring/json layer unchanged).
- Not yet done (needs the ARM toolchain + hardware): on-target compile,
  flash, sensor readout. Rebuild in STM32CubeIDE and flash to verify on the
  bench.

---

## How to open the ESP32-S3 web dashboard

The dashboard is served by the ESP32 over **its own WiFi** — there is no
separate server. Two situations:

### A) First boot / no WiFi credentials yet (provisioning mode)

When the ESP32 has no stored WiFi credentials it comes up as a WiFi access
point:

1. On your phone or laptop, join the WiFi network:
   - **SSID:** `monitor-setup`
   - **Password:** `12345678`
2. A captive-portal page should pop up automatically. If it does not, open a
   browser and go to **`http://192.168.4.1/`**.
3. On the portal page, enter your home/office WiFi **SSID** and **password**,
   then submit.
4. The ESP32 saves the credentials to NVS and reboots into station mode. It
   will print the IP it got from your router on the **ESP32 serial console**
   (the USB-UART bridge, 115200 8N1), e.g.:
   ```
   I (xxxx) wifi:got ip:192.168.1.42
   ```
5. Now go to **step B**.

### B) After joining your WiFi (station mode)

1. Make sure your phone/laptop is on the **same WiFi** as the ESP32.
2. Find the ESP32's IP — it is printed on the ESP32 serial console at boot
   (see step A4), or run the REPL command `status` / `wifi` on that console.
3. Open a browser to **`http://<that-ip>/`**, e.g. `http://192.168.1.42/`.

The dashboard loads from flash (embedded, no internet needed): KPI cards,
live charts (1 m / 5 m / 15 m / 1 h windows), alarm banner, threshold/sample
-rate/LED controls, event log, and the WiFi provisioning form. It uses a
WebSocket for 1 Hz live push and falls back to 2.5 s polling if the socket
drops.

### Tips / gotchas

- If the page won't load, the ESP32 is probably still in provisioning AP
  mode — rejoin `monitor-setup` and re-enter your WiFi details.
- To clear stored credentials and force provisioning again: on the ESP32
  serial console run `wifi clear` then `reboot`.
- The STM32 USB CDC console (the one that prints `T 254/10 C …`) is a
  **different** serial port from the ESP32 console. The STM32 console is the
  sensor head; the ESP32 console prints the WiFi IP you need.
- Both boards need a **common GND** and the UART crossed
  (STM32 PA2 → ESP32 GPIO18, STM32 PA3 ← ESP32 GPIO17) for telemetry to flow
  to the dashboard.

---

## Confirm / correct these assumptions (one-line edits)

I used the most common defaults. If any is wrong, change the single `#define`
in `Application/Inc/bringup_config.h` and rebuild:

| Assumption | Default | Where to change | How to verify |
|------------|---------|-----------------|---------------|
| INA219 I2C address | `0x40` | `INA219_I2C_ADDR_7BIT` | menu test 2 (I2C scan) |
| INA219 shunt resistor | `0.1 Ω` | `INA219_SHUNT_OHM_X10000` (1000 = 0.1 Ω; 100 = 0.01 Ω) | read the marking on the breakout (R100 = 0.1 Ω, R010 = 0.01 Ω) |
| MPU6050 I2C address | `0x68` | `MPU6050_I2C_ADDR_7BIT` (use `0x69` if AD0 tied high) | menu test 2 |
| MPU6050 accel range | ±2 g | `MPU6050_ACCEL_RANGE` (0..3) | — |
| DS18B20 still used | yes (PB5) | unchanged | menu test 4 |

**Quick sanity check after flashing:** open the STM32 USB console, press a
key within 3 s to enter the menu, run test `2` (I2C scan). You should see
three devices: the OLED (`0x3C`), the INA219 (`0x40`) and the MPU6050
(`0x68`). If any is missing or at a different address, correct the
`#define` above.


---

# Fix pass 2 — INA219 current + MPU6050 vibration errors

## Root causes found

| # | Where | Bug | Effect |
|---|-------|-----|--------|
| 1 | `ina219.c` | PGA set to /1 (+-40 mV) | with the usual 0.1 ohm shunt the reading **clipped at 0.4 A**; real load of 2 A showed 0.4 A, overcurrent alarm unreachable |
| 2 | `ina219.c` | integer maths `raw*100/R` | truncated to whole mA |
| 3 | `ina219.c` | probe only, no read-back, fixed address | a wrong/clone chip, or A0/A1 strapped to another address, looked "present" or was never found |
| 4 | both drivers | init ran **once** at boot | sensor powered late, loose jumper at boot, or a relay-click brown-out reset => reads failed **forever** |
| 5 | `mpu6050.c` | WHO_AM_I had to be exactly 0x68 | many GY-521 clones / MPU6500 parts answer 0x70/0x71/0x98 => "sensor missing" |
| 6 | `mpu6050.c` | address fixed at 0x68 | AD0 strapped high (0x69) never worked |
| 7 | `mpu6050.c` | no device reset, no verification | chip left in SLEEP/old config returned zeros |
| 8 | `mpu6050.c` | all-0x00/0xFF reads accepted | a glitch became a fake ~1 g vibration spike => false alarm |
| 9 | `app_telemetry.c` | vibration = |magnitude - baseline| | sideways shaking (perpendicular to gravity) barely changes the magnitude and was almost invisible |
| 10 | `app_telemetry.c` | +-2 g range | real machine vibration clips the sensor and under-reports |
| 11 | `bsp.c` | bit-banged I2C never recovered a wedged bus; SDA sampled at the clock edge | one aborted transfer = every later read fails; marginal edges = random NACKs |
| 12 | `bsp.c` / config | 250 kHz half-bit 2 us on jumper wires | marginal rise time |
| 13 | defaults | overcurrent threshold 4.0 A vs sensor max 3.2 A | alarm could never fire |
| 14 | web UI | `sstat` (sensor health) was sent but never shown | a dead sensor looked like a calm "0.000" |

## What changed

* **INA219**: PGA /8 (+-320 mV => +-3.2 A at 0.1 ohm), 8x hardware averaging, float
  maths, config read-back verification, 0x40..0x4F auto-scan, saturation flag,
  1 Hz `ina219_service()` that re-programs after a reset and re-probes when absent,
  5 mA dead-band, current -> 0 + fault flag after ~0.5 s of failed reads.
* **MPU6050**: device reset + verified wake, accepts clone WHO_AM_I, probes 0x68 and
  0x69, rejects stuck/zero frames, detects frozen output, 1 Hz `mpu6050_service()`,
  +-8 g range. Vibration is now the **3-axis RMS** of the high-passed acceleration
  (per-axis baseline seeded from the first sample, so no start-up transient).
* **I2C layer (`bsp.c`)**: every transfer retries once, freeing a stuck bus with the
  9-clock recovery first; SDA sampled mid-clock; half-bit 3 us (~140 kHz).
  `bsp_i2c_error_count()` / `bsp_i2c_recover_count()` added.
* **Console**: new `[I2C] ...` line every 2 s: addresses found, bus voltage, raw shunt
  count (and `CLIPPED` if at full scale), WHO_AM_I, bus error/recover counters. **Read
  this line first** when a value looks wrong.
* **Dashboard**: Current and Vibration cards now show a live `OK` / `FAULT` tag
  (also on Temperature), values show `--` when their sensor is faulted. CLI `status`
  lists all three sensors. Values already reached the page via `cur` / `vib` / `sstat`
  in the status JSON; nothing in the wire protocol changed.
* **Defaults**: overcurrent threshold 4.0 A -> 2.5 A on both MCUs (reachable by the
  0.1 ohm INA219). A warning is printed if you set a threshold above the sensor range.
* **Tests**: `tests/host/test_sensors.c` runs the real driver sources against a
  simulated I2C bus (54 checks: no clipping at 0.4 A, clone WHO_AM_I, 0x69, hot-plug,
  brown-out repair, frozen data, stuck-0xFF bus). `cd tests/host && make run`.

## Hardware notes that code cannot fix

* The INA219 sits **in series with the load** (VIN+ -> supply, VIN- -> load) and the
  load/supply ground must be common with the STM32 ground. If VIN+/VIN- are swapped the
  value is just negative (we rectify it), but a floating VIN- gives garbage.
* DC only, <= 26 V, <= 3.2 A on the stock 0.1 ohm board. It is **not isolated** — never
  use it on mains. For AC mains use an isolated sensor.
* SDA/SCL need pull-ups (the GY-521 and most OLED boards have them; many INA219 boards do
  not). Keep I2C wires short and 3V3 only. MPU6050 VCC at 3V3 (5 V on VCC is fine only
  on boards with an onboard regulator, but SDA/SCL must stay 3V3).
* Mount the MPU6050 rigidly on the machine body; a floppy mount measures the wire, not
  the machine.

---

# Fix pass 3 — dashboard "not connected" / not refreshing

* **Dashboard connection layer rewritten** (`Web/app.js`): WebSocket push plus an
  always-running 1 s refresh loop. If no status arrived for 2.5 s it polls
  `/api/status` (2.5 s timeout, no-store). A WebSocket that is open but silent for
  6 s is closed and re-opened; reconnect back-off 1..5 s. History re-syncs every
  minute and after any outage. Tab-visible / network-online events refresh
  immediately. A yellow banner shows "Dashboard not connected ... last data: N s ago"
  instead of silently freezing. Status rendering is exception-safe.
* **ESP32 web server** (`http_server.c`): `lru_purge_enable` + 5 s socket timeouts.
  Previously stale keep-alive sockets (browser + phone captive-portal probes) could
  fill the 7-socket table and the page would stop loading until reboot.
* **STM32**: periodic console status/diagnostic lines are skipped when the USB host is
  not draining (`console_can_print_now()`), so an unopened terminal can no longer
  stall the telemetry loop.

---

# Fix pass 4 — WiFi "Save & reboot" showed "Failed to fetch"

* `wifi_manager_save_credentials()` restarted the ESP32 150 ms after the HTTP reply, which
  often reset the connection before the browser read it. The restart now runs from a small
  task 2 s later.
* `/api/wifi/save` stores the credentials first and reports the real result (a storage
  failure is no longer answered with "ok").
* The setup form shows an inline message. A dropped connection right after saving is now
  explained as "saved, ESP32 restarting" with the next steps, instead of an error pop-up.

---

# Fix pass 5 — dashboard disconnects, freezes and ESP32 crashes

Found by reading the ESP32 firmware end to end:

| # | Where | Bug | Symptom |
|---|-------|-----|---------|
| 1 | `websocket_server.c` | handler called `httpd_ws_recv_frame()` during the upgrade (GET) call | the single httpd thread blocked ~5 s per WebSocket connect, then the socket was closed: connect/drop loop, whole page frozen, "not connected" |
| 2 | `wifi_manager.c` | `vTaskDelay()` (up to 10 s) inside the system event handler | WiFi/IP events stalled, reconnect looked stuck |
| 3 | `wifi_manager.c` | after 10 failed reconnects the saved WiFi credentials were **erased** and the board rebooted into setup mode — even if it had been working for hours | a router reboot or weak-signal gap "crashed" the dashboard back to `monitor-setup` |
| 4 | `wifi_manager.c` | modem power-save left on | late/timed-out pushes and polls |
| 5 | `task_uart_tx` / `supervisor` / `wifi_poll` / `dns` stacks 2-3 KB (bytes, not words) | stack overflow on the first comm-fail log line / reboot command | random panics when the link flapped |
| 6 | `task_dashboard.c` | 2.4 KB JSON buffer + float formatting on a 6 KB stack | marginal stack, possible overflow |
| 7 | `json_util.c` | a NaN/inf float printed `nan` (invalid JSON) | browser silently dropped the whole status message => values stop updating |
| 8 | `sdkconfig` | main task stack 3.5 KB | boot-time overflow risk |
| 9 | status JSON | current/vibration printed with 2 decimals | 0.004 A / 0.004 g looked frozen at 0.00 |

Fixes: WS handler returns immediately on the upgrade call; reconnect via one-shot esp_timer
(no blocking), credentials are kept forever once a connection has worked this boot (only a
never-connected board falls back to the setup AP); `esp_wifi_set_ps(WIFI_PS_NONE)`; stacks
raised (TX/supervisor/wifi/dns 4 KB, dashboard 8 KB with the JSON buffer moved off the stack,
httpd 10 KB, main task 8 KB); JSON writer turns NaN/inf into 0 and cur/vib use 3 decimals;
status buffers 3.5 KB. Host tests: 53 + 54 pass.

---

## v5 — dashboard drops + WiFi credentials not working

1. **Wrong password / 5 GHz / typo wiped the saved WiFi** (wifi_manager.c). After 10 failed
   attempts the old code erased the credentials and rebooted into setup mode, so it looked like
   "I entered it and nothing happened". Now credentials are kept, retries continue forever,
   and the `monitor-setup` AP is opened *beside* the station so you can correct them.
   The disconnect reason (wrong password / SSID not found) is printed on the serial monitor.
2. **Auth threshold** was WPA/WPA2 only; open and WPA3 networks were rejected. Now any
   network is accepted, all channels are scanned, strongest AP wins. Password field is optional.
3. **Captive-portal DNS reply was malformed** (no name pointer, stray extra bytes, A answer
   for AAAA queries). Phones judged the AP broken and dropped to mobile data mid-setup. Rewritten.
4. **Dashboard dropped every few seconds**: httpd had only 7 sockets and purges the
   "least recently used" one; the WebSocket never sends, so it was always the victim.
   Sockets raised to 13 (LWIP_MAX_SOCKETS 16) and the page sends a ping every 8 s.
5. **ESP32 rebooted itself 2 min after the STM32 went silent** (task_watchdog.c), taking the
   dashboard down exactly when it should say "STM32 LINK DOWN". It now keeps serving.
6. Hostname `health-monitor` set on the station interface.
