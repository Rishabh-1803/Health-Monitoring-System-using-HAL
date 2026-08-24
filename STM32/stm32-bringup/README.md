# stm32-bringup — component self-test firmware

A standalone STM32F411 Blackpill project whose only job is to answer one question per component:
**is this thing wired up and alive?** It boots, scans the I2C bus, prints the pinout it expects, and
then waits at a `bringup>` prompt over the board's own USB port. Each test prints the value it
actually measured and a verdict, so a failure comes with a number attached rather than a guess.

This is deliberately not the application. It shares no code with `main_code` and is meant to be
thrown away — or kept as a diagnostic — once the real firmware starts.

## Status

Written and statically verified. **Never compiled for ARM and never run on hardware.** See
[Verification](#verification) for exactly what was and was not checked; do not treat any of it as
working until it has been on the board.

## Build and run

Open `stm32-bringup` in STM32CubeIDE and build. Flash, then open the USB CDC port the board
presents — any terminal at any baud rate, since CDC ignores the setting. It appears as
`COMx` on Windows.

There is no separate serial adapter to connect. The console is the same USB cable that powers
the board.

Wait a second or two after plugging in before expecting output: `MX_USB_DEVICE_Init()` runs from
`LEDTask`, and the bring-up task waits for enumeration before printing. If your terminal attaches
late, press Enter — the prompt reprints every 30 seconds.

## Wiring

Every pin lives in `Application/Inc/bringup_config.h` and nowhere else. Move a wire, change it
there.

| Pin | Goes to | Notes |
|---|---|---|
| PC13 | onboard LED | active LOW on the Blackpill; already on the board |
| PB0 | buzzer | TIM3_CH3 PWM, for a **passive** buzzer |
| PB1 | relay IN | most opto relay boards are active LOW |
| PB6 | I2C SCL | needs a pull-up to 3V3 |
| PB7 | I2C SDA | needs a pull-up to 3V3 |
| PB5 | DS18B20 DQ | **4.7k pull-up to 3V3 is mandatory** |
| PB12 | vibration DO | internal pull-up enabled |
| PA1 | analog in | **3V3 maximum** |
| PA2 | ESP32 RX | USART2 TX — cross the wires |
| PA3 | ESP32 TX | USART2 RX |
| PA11 / PA12 | USB D− / D+ | this console |

Every sensor's ground must be common with the board's.

Two warnings worth repeating because both destroy hardware rather than merely failing:

The **ACS712 is a 5 V part.** Its output idles near 2.5 V and rises under load, which exceeds what a
3V3 F411 pin tolerates. Put a divider on it or use a 3V3-native sensor.

**Relay boards need their own 5 V supply** on JD-VCC, not the 3V3 rail. Driving the coil from the
board's regulator browns out the MCU mid-test.

### Pins that are already taken

`PA2`/`PA3` (USART2), `PA11`/`PA12` (USB), `PA13`/`PA14` (SWD — taking these kills debugging),
`PH0`/`PH1` (HSE crystal), and TIM1, which is the HAL time base.

Three more are avoided on purpose: `PA0` is the KEY button on most WeAct revisions, so the button
would short an analog input to ground; `PB3`/`PB4` are JTDO/NJTRST; `PC14`/`PC15` go to the
32.768 kHz crystal pads.

## The menu

```
1  outputs: LED, buzzer, relay      5  analog input / current sensor
2  I2C bus scan                     6  vibration switch
3  OLED display                     7  ESP32 UART link
4  DS18B20 temperature              a  run everything in order
                                    p  show the wiring table
                                    h  redisplay the menu
```

Any test that watches for activity can be stopped with a keypress.

## What each test proves, and what it does not

This distinction is the whole point of the tool, so it is stated per test rather than buried.

**Outputs (1).** Drives five slow blinks, a three-tone buzzer sweep and three relay cycles, then
asks what you saw. The firmware cannot read any of these back, so the operator's answer *is* the
measurement. PC13's heartbeat task is suspended for the duration and resumed afterwards, otherwise
it fights the test.

**I2C scan (2).** Addresses every device on the bus and reports which ones ACK. An ACK proves the
device is powered and strapped to the address you think it is — nothing more.

**OLED (3).** Runs all-pixels-on, all-off, a checkerboard, then framed text, checking that every
one of the 1024 bytes was ACKed. A passing byte count proves the bus and the address; whether
anything reached the glass is a question only you can answer. The four steps fail differently on
purpose: solid fills working but text scrambled points at addressing, not at wiring.

**DS18B20 (4).** Presence pulse, ROM code, then a real conversion, with the Maxim CRC checked on
both frames. It refuses an all-0x00 or all-0xFF frame *before* consulting the CRC, because CRC-8 is
linear: nine zero bytes carry a valid CRC of 0x00 and decode to a convincing 0.00 °C. It also
catches exactly +85.00 °C, which is the power-on register value and means the conversion never ran.

**ADC (5).** Prints an averaged level, then peak-to-peak noise over 200 samples, then watches the
value for ten seconds while you turn a pot or load the sensor. Stability proves nothing — a dead
ADC returns a beautifully steady number — so *movement* is the evidence. Timed-out conversions are
counted separately, because a timeout returns 0 and would otherwise be indistinguishable from a
grounded pin.

**Vibration (6).** Reports the idle level first, since that is what tells you whether
`VIBRATION_ACTIVE_LOW` is right, then counts debounced edges for fifteen seconds. Zero edges is
reported as inconclusive, not as a failure: firmware cannot tell an untapped sensor from a dead one.

**ESP32 link (7).** Two modes. *Loopback* jumpers PA2 to PA3 so the STM32 talks to itself, which
isolates the STM32 half completely — if loopback fails there is no point looking at the ESP32 at
all. *Echo* sends a line and judges what comes back on content, not on byte count, because a
baud-rate mismatch delivers a healthy-looking stream of nonsense.

Both run at 115200 rather than the 460800 the application will eventually want. Marginal wiring
passes at 115200 and fails at 460800, and those two failures should be distinguishable.

### Verdicts

`PASS` and `FAIL` mean something was measured. `SKIP` means the test was compiled out or you
declined it. `ABORT` means nobody answered, or you stopped it.

**An unanswered question never becomes a PASS.** A tool that awards itself a pass for a question
the operator never saw is worse than no tool: it produces a clean-looking report about hardware
that was never checked.

## Design notes

**Software I2C, not the peripheral.** This project has no `stm32f4xx_hal_i2c.c` — CubeMX never
generated it because the `.ioc` never enabled I2C — and there is no network here to fetch it.
Bit-banging also sidesteps the well-known STM32F4 I2C BUSY-flag lockup, which eats bring-up time.
A wedged bus is cleared once at startup, before anything tries to use it, and the result is
reported in the banner.

**Register-level ADC.** Same reason: no `stm32f4xx_hal_adc.c`. The prescaler is /4, giving 24 MHz,
because the F411's ADC ceiling is 36 MHz and PCLK2 is 96 MHz.

**DWT cycle counter for microsecond delays.** 1-Wire needs microsecond accuracy and `HAL_Delay` has
millisecond resolution. The counter's liveness is checked at init, and the DS18B20 test refuses to
run if it is not ticking — that way a core/debug-block problem is not misreported as a sensor fault.

**No `%f` anywhere.** Floating-point `printf` costs several kilobytes of newlib plus a linker flag,
so fractions are formatted from integers. That is what `test_print_milli` is for.

**The buzzer's timer clock is read at runtime,** not hardcoded, so changing the clock tree in CubeMX
cannot leave a stale constant behind and detune it.

**Pins are configured before the scheduler starts.** `bsp_init()` runs from `bringup_app_init()`,
which is called from `MX_FREERTOS_Init`. Waiting until the task ran would leave the relay and buzzer
floating for the first few hundred milliseconds.

## Regenerating from the .ioc

All new code lives under `Application/`, which CubeMX does not own, and the only edits inside
CubeMX-generated files are between `USER CODE BEGIN`/`END` markers. Regeneration is safe.

One thing to check afterwards: `Application/Src` is registered as a source folder and
`../Application/Inc` as an include path in `.cproject`, for both Debug and Release. Those were added
by hand. If a regeneration ever drops them, every file in `Application/` silently stops being built.

## Verification

Being precise about this, because the difference matters.

**What was checked.** All 11 `.c` files and 5 headers (3,376 lines) compile clean under host x86-64
gcc with `-Wall -Wextra`, against this project's *real* CMSIS, HAL, FreeRTOS and USB headers — not
stubs. Every file was then compiled to an actual object file, and the resulting symbol tables were
cross-checked: all 46 functions declared in `Application/Inc` have definitions, and every remaining
undefined symbol resolves to a HAL, FreeRTOS, CMSIS-RTOS, USB or libc symbol that was confirmed to
exist in this tree. `bsp.c` needed its ARM inline assembly neutered first, since x86 cannot assemble
`cpsid i`; the 23 affected statements were replaced mechanically after preprocessing.

Pin assignments were checked against the `.ioc` for collisions, and against `Core/Src/gpio.c` for
clock enables — `MX_GPIO_Init` enables GPIOA, GPIOC and GPIOH but *not* GPIOB, which is why
`bsp_init` enables it itself.

Several behaviours were confirmed by reading this project's own vendor sources rather than assumed:
that `CDC_Transmit_FS` dereferences `pClassData` without a NULL check and stores the caller's
pointer rather than copying, that `HAL_GPIO_Init` writes OTYPER before MODER and never touches ODR,
and that the four `__HAL_UART_CLEAR_*FLAG` macros are all aliases of `__HAL_UART_CLEAR_PEFLAG`, each
of which reads DR and can therefore swallow a byte.

**What was not checked.** There is no ARM toolchain, no CubeIDE and no hardware in the environment
this was written in. It has never been compiled for the target, never linked, never flashed, and no
sensor has ever been attached. Flash and RAM usage are unknown. Every timing figure comes from a
datasheet or the clock tree, not from a measurement.

Expect to fix things on first contact with the board. The value here is that the failures should be
diagnosable, not that there won't be any.

## Optional hardening

`configCHECK_FOR_STACK_OVERFLOW` and `configUSE_MALLOC_FAILED_HOOK` are both undefined, so both are
off. Turning them on catches two of the most confusing classes of FreeRTOS bug. They were left alone
deliberately: `FreeRTOSConfig.h` is CubeMX-owned, so the change belongs in the `.ioc` rather than
the file, and it is a change nobody asked for. Enable them from CubeMX's FreeRTOS config if a
mysterious hard fault shows up.
