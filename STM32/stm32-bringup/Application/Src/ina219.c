/**
 * @file    ina219.c
 * @brief   INA219 I2C current/voltage sensor driver.
 *
 * The INA219 is a delta-sigma ADC behind an I2C register file. We configure
 * it for the simplest useful mode:
 *   - PGA range  /1  -> 40 mV full scale, 10 uV / LSB on the shunt voltage
 *   - BUS_ADC    12-bit, single shot (512 us) -- the app polls, so no need
 *                for continuous conversion and its power cost
 *   - SHUNT_ADC  12-bit, single shot
 *   - Mode       shunt-and-bus, triggered once per read
 *
 * Current is computed in the MCU from V_shunt / R_shunt rather than using
 * the chip's internal CALIBRATION register + CURRENT register. That avoids
 * the rounding/scale bookkeeping and keeps the math transparent; the
 * trade-off (losing the chip's power register) is irrelevant here.
 *
 * LSB maths:
 *   shunt_voltage_raw is a signed 15-bit value (bit 0..14) in units of
 *   10 uV (PGA /1). So shunt_mv = raw * 10 uV = raw * 0.01 mV, i.e. the
 *   value in hundredths of a millivolt IS the raw count.
 *   bus_voltage_raw: bits 3..15 of the 16-bit word, 4 mV / LSB, with a
 *   conversion-ready bit at bit 1 and an overflow bit at bit 0.
 */
#include "bringup_config.h"
#include "ina219.h"
#include "bsp.h"

#include <stdbool.h>
#include <stdint.h>

#define INA219_ADDR         (INA219_I2C_ADDR_7BIT)

/* Config word (INA219 datasheet SBOS448C, config register 0x00):
 *   bit 15  RST  = 0
 *   bit 14  -    = 0  (reserved)
 *   bit 13  BRNG = 1  (32 V bus FSR so 12/24 V rails do not overflow)
 *   bits 12:11 PG  = 00 (PGA /1, +-40 mV shunt FSR, 10 uV LSB)
 *   bits 10:7  BADC  = 0011 (12-bit, 532 us conversion)
 *   bits 6:3   SADC  = 0011 (12-bit, 532 us conversion)
 *   bits 2:0   MODE  = 111  (shunt + bus, CONTINUOUS)
 *
 * Continuous mode keeps the ADC free-running so a register read always
 * returns the latest completed conversion; the first read after init may
 * be stale (the very first conversion finishes ~0.5 ms after the config
 * write), which is fine for a fire-and-forget telemetry stream.
 *
 *   0x2000 | (0x03<<7) | (0x03<<3) | 0x07 = 0x219F
 */
#define INA219_CONFIG_VAL   0x219Fu

static bool s_present = false;

static bool read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    if (!bsp_i2c_read_reg(INA219_ADDR, reg, buf, 2u)) {
        return false;
    }
    /* INA219 is big-endian on the wire. */
    *out = (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
    return true;
}

static bool write_reg16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
    return bsp_i2c_write(INA219_ADDR, buf, 3u);
}

bool ina219_init(void)
{
    /* A probe is enough: if the address ACKs, an INA219 (or a clone) is
     * there. Writing a known config then reading it back would be stronger,
     * but the shared bus + the telemetry loop's tolerance for a missing
     * sensor make a probe the right cost. */
    if (!bsp_i2c_probe(INA219_ADDR)) {
        s_present = false;
        return false;
    }
    if (!write_reg16(INA219_REG_CONFIG, INA219_CONFIG_VAL)) {
        s_present = false;
        return false;
    }
    s_present = true;
    return true;
}

bool ina219_is_present(void) { return s_present; }

bool ina219_read_shunt_mv(int32_t *shunt_mv_x100)
{
    if (!s_present) { return false; }
    uint16_t raw;
    if (!read_reg16(INA219_REG_SHUNT_V, &raw)) {
        return false;
    }
    /* The shunt register is a signed 16-bit two's-complement value whose LSB
     * is 10 uV (PGA /1) regardless of the gain setting (gain only changes the
     * full-scale range, not the LSB). So the count IS the voltage in units of
     * 0.01 mV, sign-extended. */
    int16_t signed_raw = (int16_t)raw;
    *shunt_mv_x100 = (int32_t)signed_raw;       /* hundredths of a millivolt */
    return true;
}

bool ina219_read_bus_mv(uint32_t *bus_mv)
{
    if (!s_present) { return false; }
    uint16_t raw;
    if (!read_reg16(INA219_REG_BUS_V, &raw)) {
        return false;
    }
    /* Bits 3..15 hold the bus voltage, 4 mV / LSB. Bit 1 = CNVR (ready),
     * bit 0 = OVF. Shift right 3 to get the count. */
    uint32_t counts = (uint32_t)(raw >> 3);
    *bus_mv = counts * 4u;
    return true;
}

bool ina219_read_current_ma(int32_t *current_ma)
{
    int32_t shunt_mv_x100;
    if (!ina219_read_shunt_mv(&shunt_mv_x100)) {
        return false;
    }
    /* current = V_shunt / R_shunt.
     * shunt_mv_x100 is in units of 0.01 mV = 1e-5 V.
     * R_shunt = INA219_SHUNT_OHM_X10000 * 1e-4 ohm.
     * I (A) = (shunt_mv_x100 * 1e-5) / (R_x10000 * 1e-4)
     *       = (shunt_mv_x100) / (R_x10000 * 10)
     * I (mA) = I(A) * 1000 = shunt_mv_x100 * 100 / R_x10000
     * Integer form keeps full precision:
     *   current_ma = (shunt_mv_x100 * 100) / R_x10000
     * Range at 0.1 ohm, 40 mV FSR: +-400 mA -> +-4000 (0.01 mA) ... but we
     * want mA, so this gives e.g. 0.42 A -> 420 mA. Good. */
    int32_t r = (int32_t)INA219_SHUNT_OHM_X10000;
    if (r == 0) { r = 1; }
    *current_ma = (shunt_mv_x100 * 100) / r;
    return true;
}
