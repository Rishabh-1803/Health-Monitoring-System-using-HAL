/**
 * @file    bsp.h
 * @brief   Board support: pin setup, microsecond delay, software I2C,
 *          register-level ADC1 and the buzzer PWM.
 *
 * Everything here is deliberately independent of CubeMX. All of it lives
 * in files CubeMX does not own, so regenerating the .ioc cannot wipe it.
 * That matters because this project has no HAL I2C or HAL ADC driver
 * available -- see bsp.c for the detail.
 */

#ifndef BSP_H
#define BSP_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Init                                                              */
/* ------------------------------------------------------------------ */

/**
 * Configure every pin the bring-up firmware uses, start the DWT cycle
 * counter, bring up ADC1 and prepare the buzzer timer.
 *
 * Call this once, BEFORE the scheduler starts. It uses nothing but HAL
 * GPIO/TIM and a busy-wait, so it does not need a running scheduler --
 * and calling it early matters: until it runs, the relay and buzzer pins
 * are inputs, so the relay coil is at the mercy of whatever its module
 * does with a floating input.
 */
void bsp_init(void);

/** True when the DWT cycle counter is running and delay_us is trustworthy. */
bool bsp_dwt_ok(void);

/* ------------------------------------------------------------------ */
/*  Timing                                                            */
/* ------------------------------------------------------------------ */

/**
 * Busy-wait delay, microsecond resolution, driven by the DWT cycle
 * counter. Needed because 1-Wire wants pulses far shorter than the
 * 1 ms FreeRTOS tick. Does not yield -- keep the argument small.
 *
 * Clamped internally to whatever fits 32 bits of cycle count, roughly
 * 44.7 ms at 96 MHz. Ask for more and you get the clamp, silently.
 */
void bsp_delay_us(uint32_t us);

/**
 * Raw free-running DWT cycle count. Wraps every 2^32 cycles, which is
 * about 44.7 s at 96 MHz.
 *
 * Never compare two of these directly, and never scale one on its own.
 * Take the difference first -- that is what bsp_elapsed_us does, and the
 * reason it exists as a separate call.
 */
uint32_t bsp_cycles(void);

/**
 * Microseconds elapsed since a bsp_cycles() timestamp. Correct across a
 * single counter wrap because it subtracts before it divides. Only valid
 * for intervals shorter than one wrap (~44.7 s at 96 MHz).
 */
uint32_t bsp_elapsed_us(uint32_t start_cycles);

/* ------------------------------------------------------------------ */
/*  Digital outputs                                                   */
/* ------------------------------------------------------------------ */

void bsp_led_set(bool on);          /* onboard PC13, active-low handled here */
void bsp_led_toggle(void);
void bsp_relay_set(bool on);        /* active-low handled here               */

/**
 * Sound the buzzer.
 *
 * Passive buzzer (BUZZER_IS_ACTIVE_TYPE 0): freq_hz sets the PWM
 * frequency. Anything outside 20..20000 Hz is treated as "off", because
 * outside that band the 16-bit reload at a 1 MHz tick either truncates
 * or is inaudible anyway.
 *
 * Active buzzer (BUZZER_IS_ACTIVE_TYPE 1): freq_hz is IGNORED. The part
 * generates its own tone and only wants a level, so ANY value -- zero
 * included -- turns it ON. Silence it with bsp_buzzer_off(), never with
 * bsp_buzzer_tone(0).
 */
void bsp_buzzer_tone(uint32_t freq_hz);

/** Silence the buzzer. The only correct way to stop it for either type. */
void bsp_buzzer_off(void);

/* ------------------------------------------------------------------ */
/*  Digital input                                                     */
/* ------------------------------------------------------------------ */

/** True when the vibration switch is asserted (active level normalised). */
bool bsp_vibration_asserted(void);

/* ------------------------------------------------------------------ */
/*  Software I2C                                                      */
/* ------------------------------------------------------------------ */

/**
 * Drive SCL and SDA high and clock out up to 9 bits to free a slave
 * that is holding SDA low after a partial transfer. Worth doing once
 * at startup -- a wedged bus is a very common bring-up symptom.
 * Returns true if SDA ended up released.
 */
bool bsp_i2c_recover(void);

/** True if a slave at this 7-bit address ACKs its address byte. */
bool bsp_i2c_probe(uint8_t addr7);

/** Write len bytes to a 7-bit address. Returns false on NACK or timeout. */
bool bsp_i2c_write(uint8_t addr7, const uint8_t *data, uint32_t len);

/** Read len bytes from a 7-bit address. Returns false on NACK or timeout. */
bool bsp_i2c_read(uint8_t addr7, uint8_t *data, uint32_t len);

/** Write a register address then read len bytes back (the usual sensor idiom). */
bool bsp_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *data, uint32_t len);

/** Write a single register byte. */
bool bsp_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t value);

/* ------------------------------------------------------------------ */
/*  ADC                                                               */
/* ------------------------------------------------------------------ */

/** Single 12-bit conversion on the configured input. Returns 0..4095. */
uint16_t bsp_adc_read_raw(void);

/** Mean of n conversions, to damp noise. n is clamped to 1..1024. */
uint16_t bsp_adc_read_avg(uint32_t n);

/** Convert a raw count to millivolts using ADC_VREF_MV. */
uint32_t bsp_adc_raw_to_mv(uint16_t raw);

/**
 * Number of conversions that hit the end-of-conversion timeout since
 * bsp_init(). Those return 0, which is indistinguishable from a genuine
 * 0 V reading -- so a test that reports 0 mV must check this counter
 * before calling it a measurement.
 */
uint32_t bsp_adc_timeouts(void);

/* ------------------------------------------------------------------ */
/*  1-Wire (DS18B20)                                                  */
/* ------------------------------------------------------------------ */

/**
 * Issue a reset pulse and look for the slave presence pulse.
 * Returns true if at least one device answered.
 */
bool bsp_ow_reset(void);
void bsp_ow_write_byte(uint8_t byte);
uint8_t bsp_ow_read_byte(void);

#endif /* BSP_H */
