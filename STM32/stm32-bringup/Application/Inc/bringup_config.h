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
#define I2C_HALF_BIT_US           5u       /* ~100 kHz; raise to slow the bus */
#define I2C_TIMEOUT_US            2000u    /* clock-stretch / stuck-line guard */

/* SSD1306 OLED. 0x3C is by far the most common; some modules are 0x3D. */
#define OLED_I2C_ADDR_7BIT        0x3Cu
#define OLED_WIDTH                128
#define OLED_HEIGHT               64

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
 *  ESP32 link -- USART2 on PA2 (TX) / PA3 (RX), already initialised by
 *  CubeMX at 115200 8N1. main_code eventually wants 460800, but
 *  proving the wiring at 115200 first is the sane order.
 *
 *  Cross the wires: STM32 PA2 -> ESP32 RX, STM32 PA3 -> ESP32 TX, and
 *  tie the grounds together. Both sides are 3V3, so no level shifter.
 * ------------------------------------------------------------------ */
#define ESP32_LINK_BAUD           115200u
#define ESP32_LINK_PROBE_TIMEOUT_MS 1500u

#endif /* BRINGUP_CONFIG_H */
