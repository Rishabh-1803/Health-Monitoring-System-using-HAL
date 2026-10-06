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
