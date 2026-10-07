/**
 * @file    bringup_config.h
 * @brief   Single place to describe how the board is wired.
 *
 * EVERY pin, address and channel the bring-up firmware touches is named here.
 * If you move a wire, change it here and nowhere else.
 *
 * Pins already committed by the CubeMX configuration -- do not reuse:
 *   PC13        onboard LED (active LOW on the WeAct Blackpill)
 *   PA2 / PA3   USART2 TX / RX  (the ESP32 link)
 *   PA11 / PA12 USB D- / D+     (the CDC console you read this on)
 *   PA13 / PA14 SWDIO / SWCLK   (taking these kills debugging)
 *   PH0 / PH1   HSE crystal
 *
 * Deliberately avoided:
 *   PA0         KEY button on most WeAct Blackpill revisions, so it is a
 *               poor analog input -- the button would short it to ground.
 *   PB3 / PB4   JTDO / NJTRST. Usable as GPIO once SWD-only is selected,
 *               but not worth the risk during bring-up.
 *   PC14 / PC15 wired to the 32.768 kHz crystal pads on the Blackpill.
 *
 * Clocks (read out of the .ioc, not assumed): SYSCLK 96 MHz, HCLK 96 MHz,
 * APB1 prescaler /2 so PCLK1 is 48 MHz and the APB1 TIMER clock is 96 MHz.
 * TIM3 lives on APB1, so its input clock is 96 MHz -- that is what the
 * buzzer prescaler is derived from.
 */

#ifndef BRINGUP_CONFIG_H
#define BRINGUP_CONFIG_H

/* ------------------------------------------------------------------ *
 *  Which tests are compiled in.
 *  Set a macro to 0 and that component's code and menu entry vanish,
 *  which is what you want when a part has not arrived yet.
 * ------------------------------------------------------------------ */
#define BRINGUP_TEST_GPIO         1   /* onboard LED + buzzer + relay      */
#define BRINGUP_TEST_I2C_SCAN     1   /* bus scan -- always worth having   */
#define BRINGUP_TEST_OLED         1   /* SSD1306 over I2C                  */
#define BRINGUP_TEST_DS18B20      1   /* 1-Wire temperature                */
#define BRINGUP_TEST_ADC          1   /* current sensor / any analog input  */
#define BRINGUP_TEST_VIBRATION    1   /* SW-420 style digital vibration    */
#define BRINGUP_TEST_ESP32_LINK   1   /* UART loopback / echo to the ESP32 */
#define BRINGUP_TEST_PROTOCOL     1   /* Phase 2A: CRC + packet self-test  */
#define BRINGUP_TEST_UART_HEARTBEAT 1 /* Phase 2B: real heartbeat exchange */

/* ------------------------------------------------------------------ *
 *  Onboard LED -- PC13, active LOW.
 * ------------------------------------------------------------------ */
#define LED_ONBOARD_PORT          GPIOC
#define LED_ONBOARD_PIN           GPIO_PIN_13
#define LED_ONBOARD_ACTIVE_LOW    1

/* ------------------------------------------------------------------ *
 *  Buzzer -- PB0, driven by TIM3_CH3 in PWM mode so a passive buzzer
 *  actually makes a tone. An active buzzer only needs a level, so set
 *  BUZZER_IS_ACTIVE_TYPE to 1 and it is driven as plain GPIO instead.
 *
 *  There is deliberately no timer-clock constant here. bsp.c reads the
 *  APB1 prescaler at runtime and derives it, so changing the clock tree
 *  in CubeMX cannot leave a stale number behind and detune the buzzer.
 * ------------------------------------------------------------------ */
#define BUZZER_PORT               GPIOB
#define BUZZER_PIN                GPIO_PIN_0
#define BUZZER_IS_ACTIVE_TYPE     0        /* 1 = active buzzer, level only  */
#define BUZZER_TIM_AF             GPIO_AF2_TIM3
#define BUZZER_TIM_CHANNEL        TIM_CHANNEL_3
#define BUZZER_DEFAULT_HZ         2000u     /* most passive buzzers peak here */

/* ------------------------------------------------------------------ *
 *  Relay -- PB1. Most opto-isolated relay boards are active LOW;
 *  check yours, and expect an audible click either way.
 * ------------------------------------------------------------------ */
#define RELAY_PORT                GPIOB
#define RELAY_PIN                 GPIO_PIN_1
#define RELAY_ACTIVE_LOW          1

/* ------------------------------------------------------------------ *
 *  Software (bit-banged) I2C -- PB6 = SCL, PB7 = SDA.
 *
 *  Why software rather than the I2C peripheral: this project has no
 *  stm32f4xx_hal_i2c.c (CubeMX never generated it because the .ioc
 *  never enabled I2C), and bit-banging also sidesteps the well-known
 *  STM32F4 I2C BUSY-flag lockup that eats bring-up time.
 *
 *  Both lines need pull-ups. The internal ones are enabled as a
 *  fallback and will usually work on a short bench wire, but fit
 *  real 4.7k resistors to 3V3 for anything longer than a few cm.
 * ------------------------------------------------------------------ */
#define I2C_SCL_PORT              GPIOB
#define I2C_SCL_PIN               GPIO_PIN_6
#define I2C_SDA_PORT              GPIOB
#define I2C_SDA_PIN               GPIO_PIN_7
/* 3 us half-bit -> roughly 130-150 kHz including GPIO overhead. All three
 * devices (SSD1306 / INA219 / MPU6050) are rated for 400 kHz, but this bus
 * is bit-banged, often on jumper wires with only the breakout boards'
 * pull-ups, and a slow/marginal edge shows up as random NACKs. 3 us leaves
 * comfortable rise-time margin and still fits a one-page OLED flush in the
 * loop. Raise to 5 for ~100 kHz on long wires, drop to 2 only on a short,
 * well pulled-up bus.                                                  */
#define I2C_HALF_BIT_US           3u
#define I2C_TIMEOUT_US            2000u    /* clock-stretch / stuck-line guard */

/* SSD1306 OLED. 0x3C is by far the most common; some modules are 0x3D. */
#define OLED_I2C_ADDR_7BIT        0x3Cu
#define OLED_WIDTH                128
#define OLED_HEIGHT               64

/* ------------------------------------------------------------------ *
 *  INA219 bidirectional current/voltage/power sensor (I2C).
 *  Shares the PB6/PB7 bus with the OLED and the MPU6050.
 *
 *  Address 0x40 when A0=A1=GND (strap on most breakouts); A0/A1
 *  jumpers move it across 0x40..0x4F. If current reads 0 forever,
 *  run the I2C scan (menu 2) and set the address it finds.
 *
 *  Shunt: nearly all INA219 breakouts ship a 0.1 ohm (R100) sense
 *  resistor -> 0.1 mA resolution and +-3.2 A range at PGA /8. A 0.01
 *  ohm board (R010) reads up to 32 A: set INA219_SHUNT_OHM_X10000 to
 *  100. The maths is current = V_shunt / R_shunt either way.
 *
 *  DC ONLY: the INA219 is not isolated and tops out at 26 V on the
 *  bus pin. Never put it in series with mains.
 * ------------------------------------------------------------------ */
#define INA219_I2C_ADDR_7BIT      0x40u
#define INA219_SHUNT_OHM_X10000   1000u   /* 0.1 ohm = 1000 * 0.0001 ohm   */

/* PGA gain: 0 = /1 (+-40 mV)  1 = /2 (+-80 mV)  2 = /4 (+-160 mV)
 *           3 = /8 (+-320 mV, the chip's power-on default).
 * IMPORTANT: full-scale current = range / R_shunt. At 0.1 ohm, /1 only
 * reaches +-0.4 A and silently clips everything above it (the old setting
 * -- the "current stuck at 0.4 A" bug). /8 reaches +-3.2 A, which is the
 * rating of the standard 0.1 ohm breakout. */
#define INA219_PGA_SETTING        3u

/* Full-scale current the chosen PGA + shunt can measure, in mA:
 *   range_mV / R  with range_mV = 40 << PGA                             */
#define INA219_FULL_SCALE_MA      ((uint32_t)(((40u << INA219_PGA_SETTING) * 10000u) \
                                              / INA219_SHUNT_OHM_X10000))

/* Readings below this are INA219 offset/noise, not load: shown as 0 A. */
#define INA219_DEADBAND_MA        5u

/* ------------------------------------------------------------------ *
 *  Alarm defaults that depend on what the sensors can physically read.
 *  The overcurrent default must sit BELOW INA219_FULL_SCALE_MA, or the
 *  alarm could never fire (a 4 A threshold on a 3.2 A-range sensor).
 * ------------------------------------------------------------------ */
#define DEFAULT_THRESH_CURRENT_A  2.5f

/* ------------------------------------------------------------------ *
 *  MPU6050 6-axis accel/gyro (I2C) used as the vibration source.
 *  Address 0x68 when AD0=GND, 0x69 when AD0=VCC. Accelerometer set
 *  to +-2 g full scale -> 16384 LSB per g (0.061 mg/LSB). Vibration
 *  is the high-passed total acceleration magnitude in g.
 * ------------------------------------------------------------------ */
#define MPU6050_I2C_ADDR_7BIT     0x68u   /* 0x69 is tried automatically too */
/* +-8 g: a machine that is genuinely vibrating easily peaks past +-2 g, and
 * a clipped accelerometer under-reports exactly when it matters most.
 * 4096 LSB/g still resolves 0.24 mg, far below the 0.5 g alarm level.   */
#define MPU6050_ACCEL_RANGE       2u      /* 0=+-2g 1=+-4g 2=+-8g 3=+-16g */


/* ------------------------------------------------------------------ *
 *  DS18B20 1-Wire temperature sensor -- PB5. Needs a 4.7k pull-up to
 *  3V3; without it you get a permanent "no presence pulse". Timing
 *  comes from a DWT cycle-counter microsecond delay, not HAL_Delay.
 * ------------------------------------------------------------------ */
#define DS18B20_PORT              GPIOB
#define DS18B20_PIN               GPIO_PIN_5

/* ------------------------------------------------------------------ *
 *  Analog input -- PA1 = ADC1_IN1. Suits an ACS712 current sensor or
 *  any 0-3V3 analog source.
 *
 *  WARNING: the ACS712 is a 5 V part and its midpoint output sits near
 *  2.5 V, swinging higher with load. That exceeds what a 3V3 F411 pin
 *  tolerates. Put a divider on it or use a 3V3-native sensor.
 * ------------------------------------------------------------------ */
#define ADC_INPUT_PORT            GPIOA
#define ADC_INPUT_PIN             GPIO_PIN_1
#define ADC_INPUT_CHANNEL         1u       /* ADC1_IN1 */
#define ADC_VREF_MV               3300u    /* measure your 3V3 rail, correct this */

/* ACS712 scaling, used only to print a current estimate.
 * 185 mV/A = 5 A part, 100 = 20 A, 66 = 30 A. */
#define ACS712_MV_PER_AMP         185u
#define ACS712_ZERO_MV            2500u    /* VCC/2; calibrate with no load */

/* ------------------------------------------------------------------ *
 *  Vibration sensor (SW-420 / tilt switch / any dry contact) -- PB12,
 *  read as a digital input with the internal pull-up enabled.
 * ------------------------------------------------------------------ */
#define VIBRATION_PORT            GPIOB
#define VIBRATION_PIN             GPIO_PIN_12
#define VIBRATION_ACTIVE_LOW      1

/* ------------------------------------------------------------------ *
 *  ESP32 link -- USART2 on PA2 (TX) / PA3 (RX), initialised by CubeMX
 *  at 460800 8N1 (Core/Src/usart.c). The ESP32 side matches at 460800
 *  (ESP32/main_code/main/Drivers/uart_link.c). Both sides agree, so the
 *  link works; the menu probe test (option 7) deliberately drops to
 *  115200 to prove the wiring before trusting the higher rate.
 *
 *  Cross the wires: STM32 PA2 -> ESP32 RX, STM32 PA3 -> ESP32 TX, and
 *  tie the grounds together. Both sides are 3V3, so no level shifter.
 * ------------------------------------------------------------------ */
#define ESP32_LINK_BAUD           460800u  /* matches usart.c + ESP32 */
#define ESP32_LINK_PROBE_TIMEOUT_MS 1500u

#endif /* BRINGUP_CONFIG_H */
