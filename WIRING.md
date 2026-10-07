# WIRING — every pin, every wire

Exhaustive pin-by-pin connection list for the whole system. Source of truth:
`STM32/stm32-bringup/Application/Inc/bringup_config.h` (verified against
`Core/Src/usart.c` and `ESP32/main_code/main/Drivers/uart_link.c`).

If you move a wire, change the single `#define` in `bringup_config.h` and
nowhere else.

---

## 0. Power rails (read this first — most "broken" sensors are a power mistake)

| Rail | Source | Used by |
|------|--------|---------|
| **5 V** | STM32 USB (5V pin) **or** ESP32 USB (5V pin) | relay module coil |
| **3V3** | STM32 3V3 pin (onboard LDO from USB 5V) | OLED, INA219, MPU6050, DS18B20, buzzer |
| **GND** | **MUST be common** to STM32, ESP32, and every sensor | everything |

> The STM32 and ESP32 each have their own onboard 3V3 regulator. Their 3V3
> rails do NOT need to be joined, but their **GND must be joined** or the
> UART and I2C signals have no reference.

---

## 1. STM32F411CEU6 Blackpill — complete pin map

| Pin | Function | Active | Wired to |
|-----|----------|--------|----------|
| **PC13** | onboard LED ("green") | LOW | onboard, nothing to wire |
| **PB0** | buzzer (TIM3_CH3 PWM) | — | buzzer `+` |
| **PB1** | relay IN ("red" / industrial trip) | LOW | relay module `IN` |
| **PB5** | DS18B20 1-Wire DQ | — | DS18B20 `DQ` + 4.7 kΩ pull-up to 3V3 |
| **PB6** | I2C SCL (shared bus) | — | OLED `SCL` + INA219 `SCL` + MPU6050 `SCL` + 4.7 kΩ pull-up to 3V3 |
| **PB7** | I2C SDA (shared bus) | — | OLED `SDA` + INA219 `SDA` + MPU6050 `SDA` + 4.7 kΩ pull-up to 3V3 |
| **PB12** | *(unused — was SW-420 DO)* | — | leave floating |
| **PA1** | *(unused — was ACS712 analog)* | — | leave floating |
| **PA2** | USART2 TX → ESP32 | — | ESP32 **GPIO18** (RX) |
| **PA3** | USART2 RX ← ESP32 | — | ESP32 **GPIO17** (TX) |
| **PA11** | USB D- | — | onboard USB (CDC console) |
| **PA12** | USB D+ | — | onboard USB (CDC console) |
| **PA13** | SWDIO | — | SWD debugger (optional) |
| **PA14** | SWCLK | — | SWD debugger (optional) |
| **PH0** | HSE crystal 25 MHz | — | onboard |
| **PH1** | HSE crystal 25 MHz | — | onboard |
| **3V3** | power output | — | OLED, INA219, MPU6050, DS18B20, buzzer, pull-ups |
| **GND** | ground | — | common ground bus |
| **5V** | power input/output (from USB) | — | relay module `VCC` |
| **USB** | console + power | — | your computer (STM32 CDC terminal) |

UART config: **460800 8N1, no flow control** (set in `usart.c` + ESP32 `uart_link.c`).

---

## 2. ESP32-S3-DevKitC-1 N16R8 — complete pin map

| Pin | Function | Wired to |
|-----|----------|----------|
| **GPIO17** | UART1 TX → STM32 | STM32 **PA3** (RX) |
| **GPIO18** | UART1 RX ← STM32 | STM32 **PA2** (TX) |
| **USB** | console (USB-UART bridge) + power | your computer (115200 8N1 terminal) |
| **3V3** | power output | (not used for sensors — sensors are on the STM32) |
| **GND** | ground | **common ground bus with STM32** |
| **5V** | power input (from USB) | — |

> Only **two wires + GND** between the two MCUs: GPIO17↔PA3, GPIO18↔PA2, GND↔GND.
> Both sides are 3V3 logic — **no level shifter needed.**

---

## 3. Per-component wiring (every wire named)

### 3a. SSD1306 OLED 128×64 (I2C address 0x3C)

| OLED pin | Wire to |
|----------|---------|
| **VCC** | STM32 **3V3** |
| **GND** | common **GND** |
| **SCL** | STM32 **PB6** |
| **SDA** | STM32 **PB7** |

> Some modules are labelled VDD instead of VCC — same thing. A few ship at
> address 0x3D; the driver auto-detects.

### 3b. INA219 current sensor (I2C address 0x40)

| INA219 pin | Wire to |
|------------|---------|
| **VCC** | STM32 **3V3** (logic supply) |
| **GND** | common **GND** |
| **SCL** | STM32 **PB6** |
| **SDA** | STM32 **PB7** |
| **VIN+** (V+ / supply side) | **Power supply +** (the high side of your load) |
| **VIN−** (V− / load side) | **Load +** (the shunt is between VIN+ and VIN−) |

> The load current path is a SERIES loop:
> `Supply+ → VIN+ → (internal 0.1 Ω shunt) → VIN− → Load+ → Load− → Supply−`
> The INA219 measures the drop across the internal shunt, so your load must
> flow *through* the VIN+/VIN− path. Get this backwards and current reads
> negative (the app rectifies it, so it still shows a number).

### 3c. MPU6050 accel/gyro (I2C address 0x68)

| MPU6050 pin | Wire to |
|-------------|---------|
| **VCC** | STM32 **3V3** |
| **GND** | common **GND** |
| **SCL** | STM32 **PB6** |
| **SDA** | STM32 **PB7** |
| **AD0** | **GND** (sets address 0x68; tie to 3V3 for 0x69) |
| **INT** | leave unconnected (not used) |

### 3d. DS18B20 temperature probe (1-Wire)

| DS18B20 wire | Wire to |
|--------------|---------|
| **VDD** (red) | STM32 **3V3** |
| **GND** (black) | common **GND** |
| **DQ** (yellow) | STM32 **PB5** |
| 4.7 kΩ resistor | between **DQ** and **3V3** (pull-up) |

> Powered mode (not parasitic). The 4.7 kΩ pull-up is mandatory — without it
> you get a permanent "no presence pulse" and the sensor-fail alarm.

### 3e. Passive buzzer

| Buzzer pin | Wire to |
|------------|---------|
| **+** | STM32 **PB0** |
| **−** | common **GND** |

> Passive buzzer (no oscillator) — driven by TIM3_CH3 PWM at 2 kHz.
> If yours is an **active** buzzer, set `BUZZER_IS_ACTIVE_TYPE 1` in
> `bringup_config.h` and the code switches PB0 to plain GPIO.

### 3f. Relay module (opto-isolated, active LOW)

| Relay module pin | Wire to |
|------------------|---------|
| **VCC** | STM32 **5V** (coil needs 5 V) |
| **GND** | common **GND** |
| **IN** | STM32 **PB1** |
| **COM** (contact) | your load circuit common |
| **NO** (contact) | your load circuit normally-open |

> Active LOW: PB1 low = relay energised. The relay contacts are isolated
> from the MCU — wire your mains/high-power load through COM/NO. If your
> module has a JD-VCC jumper, keep it fitted (VCC = JD-VCC = 5V).

### 3g. Onboard LED

PC13 on the Blackpill — already wired on the board. Nothing to connect.
Active LOW (writing 0 lights it).

---

## 4. Pull-up resistors (mandatory, all on the STM32 side)

| Bus/signal | Resistor | Between | Notes |
|------------|----------|---------|-------|
| I2C SCL (PB6) | 4.7 kΩ | PB6 ↔ 3V3 | one per bus, not per device |
| I2C SDA (PB7) | 4.7 kΩ | PB7 ↔ 3V3 | one per bus, not per device |
| DS18B20 DQ (PB5) | 4.7 kΩ | PB5 ↔ 3V3 | 1-Wire needs it |

> The code also enables the STM32 internal pull-ups as a fallback, but they
> are weak (~40 kΩ) and unreliable for anything longer than a few cm. Fit
> the real 4.7 kΩ resistors.

---

## 5. Inter-MCU UART (the two-wire link)

```
STM32 PA2 (TX)  ───────────────►  ESP32 GPIO18 (RX)
STM32 PA3 (RX)  ◄───────────────  ESP32 GPIO17 (TX)
STM32 GND       ───────────────  ESP32 GND
```

- **Cross the wires** (TX→RX, RX→TX). This is the #1 wiring mistake.
- 460800 8N1, no flow control, no level shifter (both 3V3).
- Common GND is mandatory or you get garbage / CRC fails.

---

## 6. I2C bus diagram (three devices, one bus)

```
          3V3
           │
          4.7k        4.7k
           │           │
  PB6 ─────┴──┬──┬──┬──┴── SCL  (OLED, INA219, MPU6050)
             │  │  │
  PB7 ───────┴──┴──┴───── SDA  (OLED, INA219, MPU6050)
           │  │  │
        ┌──┴──┴──┴──┐
        │ OLED INA MPU│  (each also has VCC→3V3, GND→GND)
        └────────────┘
```

All three devices share PB6 (SCL) and PB7 (SDA) in parallel, each with its
own VCC to 3V3 and GND to the common bus. Only **one** set of 4.7 kΩ
pull-ups per line (not three).

---

## 7. Unused pins (after the hardware change)

| Pin | Was | Now |
|-----|-----|-----|
| **PA1** | ACS712 analog out | unused — leave floating (INA219 replaced it) |
| **PB12** | SW-420 digital out | unused — leave floating (MPU6050 replaced it) |

The code still configures these as inputs (harmless), it just never reads
them. You can leave the old ACS712 / SW-420 disconnected.

---

## 8. Sanity-check sequence after wiring

1. **Power off.** Multi-meter check: continuity on every GND connection
   (STM32 GND ↔ ESP32 GND ↔ every sensor GND).
2. **Power on via STM32 USB.** 3V3 rail should read 3.3 V. Nothing hot.
3. Open the STM32 USB CDC terminal, press a key within 3 s → menu.
4. Run **test 2 (I2C scan)**. You must see three ACKs:
   `0x3C` (OLED), `0x40` (INA219), `0x68` (MPU6050). If any is missing,
   check its VCC/GND/SDA/SCL and the pull-ups.
5. Run **test 3 (OLED)** — confirm the 4-step sequence on the glass.
6. Run **test 4 (DS18B20)** — confirm a plausible temperature.
7. Power the ESP32 via its USB, run `status` on its console — you should
   see the ESP32 IP and, once the STM32 app is running, telemetry flowing.
8. Open the ESP32's IP in a browser — the dashboard should show live
   temp / current / vibration.
