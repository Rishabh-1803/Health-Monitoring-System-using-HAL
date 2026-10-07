/**
 * @file    ina219.c
 * @brief   INA219 I2C current/voltage sensor driver (see ina219.h).
 *
 * Configuration register (SBOS448, reg 0x00), built from named fields:
 *   bit 15     RST   0
 *   bit 13     BRNG  1      32 V bus range (12/24 V rails do not overflow)
 *   bits 12:11 PG    PGA    gain, from INA219_PGA_SETTING (3 = /8, +-320 mV)
 *   bits 10:7  BADC  0011   bus ADC, 12-bit, 1 sample (532 us)
 *   bits 6:3   SADC  1011   shunt ADC, 12-bit, 8-sample hardware average
 *                           (4.26 ms) -- this is the noise filter
 *   bits 2:0   MODE  111    shunt + bus, continuous
 *
 * With PGA /8 and BRNG=1 this differs from the chip's power-on value
 * (0x399F) only in the SADC averaging field, which is exactly what lets
 * ina219_service() tell "chip was reset" from "chip is still configured".
 */
#include "bringup_config.h"
#include "ina219.h"
#include "bsp.h"

#include <stdbool.h>
#include <stdint.h>

#define CFG_BRNG_32V        (1u << 13)
#define CFG_PGA(g)          ((uint16_t)(((uint16_t)(g) & 0x3u) << 11))
#define CFG_BADC_12BIT      (0x3u << 7)
#define CFG_SADC_12BIT_X8   (0xBu << 3)
#define CFG_MODE_CONT       0x7u

#define INA219_CONFIG_VAL   ((uint16_t)(CFG_BRNG_32V | CFG_PGA(INA219_PGA_SETTING) | \
                                        CFG_BADC_12BIT | CFG_SADC_12BIT_X8 |         \
                                        CFG_MODE_CONT))
#define INA219_RESET_BIT    0x8000u

/* Shunt register saturates at the PGA full scale: 40 mV << PGA, in 10 uV. */
#define SHUNT_FULL_SCALE_COUNTS   ((int32_t)(4000 << INA219_PGA_SETTING))

#define MAX_CONSEC_FAILS    10u

static bool     s_present      = false;
static uint8_t  s_addr         = 0u;
static uint32_t s_fail_streak  = 0u;
static bool     s_saturated    = false;

static bool read_reg16_at(uint8_t addr, uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    if (!bsp_i2c_read_reg(addr, reg, buf, 2u)) {
        return false;
    }
    /* INA219 is big-endian on the wire. */
    *out = (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
    return true;
}

static bool write_reg16_at(uint8_t addr, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
    return bsp_i2c_write(addr, buf, 3u);
}

#define INA219_POR_CONFIG   0x399Fu     /* power-on default of the config reg */

/* Reset, program and VERIFY one candidate address. `strict` is used for
 * addresses we were NOT told about: we only touch such a device if its
 * config register already looks like an INA219's, so scanning can never
 * scribble on some unrelated chip that happens to live at 0x4x. */
static bool try_address(uint8_t addr, bool strict)
{
    if (!bsp_i2c_probe(addr)) {
        return false;
    }
    if (strict) {
        uint16_t cur = 0u;
        if (!read_reg16_at(addr, INA219_REG_CONFIG, &cur) ||
            ((cur & 0x7FFFu) != INA219_POR_CONFIG &&
             (cur & 0x7FFFu) != (INA219_CONFIG_VAL & 0x7FFFu))) {
            return false;
        }
    }
    (void)write_reg16_at(addr, INA219_REG_CONFIG, INA219_RESET_BIT);
    bsp_delay_us(1000u);
    if (!write_reg16_at(addr, INA219_REG_CONFIG, INA219_CONFIG_VAL)) {
        return false;
    }
    uint16_t rb = 0u;
    if (!read_reg16_at(addr, INA219_REG_CONFIG, &rb)) {
        return false;
    }
    /* Bit 15 (RST) self-clears; compare everything else. A chip that is
     * not an INA219 will not echo our config word back. */
    return (rb & 0x7FFFu) == (INA219_CONFIG_VAL & 0x7FFFu);
}

bool ina219_init(void)
{
    s_present     = false;
    s_fail_streak = 0u;

    /* Configured address first, then the rest of the strap range. */
    if (try_address((uint8_t)INA219_I2C_ADDR_7BIT, false)) {
        s_addr = (uint8_t)INA219_I2C_ADDR_7BIT;
        s_present = true;
        return true;
    }
    for (uint8_t a = 0x40u; a <= 0x4Fu; a++) {
        if (a == (uint8_t)INA219_I2C_ADDR_7BIT) { continue; }
        if (try_address(a, true)) {
            s_addr = a;
            s_present = true;
            return true;
        }
    }
    s_addr = 0u;
    return false;
}

bool ina219_is_present(void) { return s_present; }
uint8_t ina219_address(void) { return s_addr; }
bool ina219_saturated(void)  { return s_saturated; }

void ina219_service(void)
{
    if (!s_present) {
        (void)ina219_init();            /* hot-plug / late power-up        */
        return;
    }

    uint16_t cfg = 0u;
    if (!read_reg16_at(s_addr, INA219_REG_CONFIG, &cfg)) {
        if (++s_fail_streak >= 3u) {
            s_present = false;          /* stopped answering: re-probe     */
        }
        return;
    }
    s_fail_streak = 0u;
    if ((cfg & 0x7FFFu) != (INA219_CONFIG_VAL & 0x7FFFu)) {
        /* Brown-out / glitch reset it to the power-on default. */
        (void)write_reg16_at(s_addr, INA219_REG_CONFIG, INA219_CONFIG_VAL);
    }
}

/* Count a failed data read; drop presence after a sustained run so
 * ina219_service() starts re-probing. */
static void note_fail(void)
{
    if (++s_fail_streak >= MAX_CONSEC_FAILS) {
        s_present = false;
    }
}

bool ina219_read_shunt_raw(int32_t *raw_10uv)
{
    if (!s_present) { return false; }
    uint16_t raw;
    if (!read_reg16_at(s_addr, INA219_REG_SHUNT_V, &raw)) {
        note_fail();
        return false;
    }
    s_fail_streak = 0u;

    /* Signed two's complement; LSB is 10 uV at every PGA setting. */
    int32_t v = (int32_t)(int16_t)raw;
    s_saturated = (v >= SHUNT_FULL_SCALE_COUNTS) || (v <= -SHUNT_FULL_SCALE_COUNTS);
    *raw_10uv = v;
    return true;
}

bool ina219_read_shunt_mv(int32_t *shunt_mv_x100)
{
    return ina219_read_shunt_raw(shunt_mv_x100);
}

bool ina219_read_bus_mv(uint32_t *bus_mv)
{
    if (!s_present) { return false; }
    uint16_t raw;
    if (!read_reg16_at(s_addr, INA219_REG_BUS_V, &raw)) {
        note_fail();
        return false;
    }
    /* Bits 15:3 = voltage in 4 mV steps; bit 1 = CNVR, bit 0 = OVF. */
    *bus_mv = (uint32_t)(raw >> 3) * 4u;
    return true;
}

bool ina219_read_current_a(float *amps)
{
    int32_t raw;
    if (!ina219_read_shunt_raw(&raw)) {
        return false;
    }
    /* I = V/R:  V = raw * 10 uV,  R = R_x10000 * 0.0001 ohm
     *   -> I[A] = raw * 1e-5 / (R_x10000 * 1e-4) = raw / (R_x10000 * 10) */
    *amps = (float)raw / ((float)INA219_SHUNT_OHM_X10000 * 10.0f);
    return true;
}

bool ina219_read_current_ma(int32_t *current_ma)
{
    float a;
    if (!ina219_read_current_a(&a)) {
        return false;
    }
    float ma = a * 1000.0f;
    *current_ma = (int32_t)(ma >= 0.0f ? ma + 0.5f : ma - 0.5f);
    return true;
}
