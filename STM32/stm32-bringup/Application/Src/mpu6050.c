/**
 * @file    mpu6050.c
 * @brief   MPU6050 I2C accelerometer driver (see mpu6050.h).
 *
 * Wake-up is the one thing that catches people: the part powers up in SLEEP
 * (PWR_MGMT_1 = 0x40) and returns zeros until bit 6 is cleared. We do a full
 * DEVICE_RESET first so a half-configured chip left over from a previous run
 * cannot confuse us, then select the PLL-with-X-gyro clock (more stable than
 * the internal RC) and pick the accel full-scale from the config so the
 * LSB-per-g divisor is always right.
 *
 * Accel registers are big-endian signed 16-bit. Sensitivity (LSB/g):
 *   +-2 g 16384, +-4 g 8192, +-8 g 4096, +-16 g 2048.
 */
#include "bringup_config.h"
#include "mpu6050.h"
#include "bsp.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PWR_MGMT_1_WANTED   0x01u   /* SLEEP=0, CLKSEL=1 (PLL, X gyro)    */
#define FROZEN_LIMIT        200u    /* identical bursts in a row = hung   */
#define MAX_CONSEC_FAILS    10u

static const float ACCEL_LSB_PER_G[4] = { 16384.0f, 8192.0f, 4096.0f, 2048.0f };

static bool     s_present     = false;
static uint8_t  s_addr        = 0u;
static uint8_t  s_who         = 0u;
static float    s_lsb_per_g   = 4096.0f;
static uint32_t s_fail_streak = 0u;
static uint32_t s_same_count  = 0u;
static uint8_t  s_last_raw[6];

static bool rd(uint8_t reg, uint8_t *buf, uint32_t n)
{
    return bsp_i2c_read_reg(s_addr, reg, buf, n);
}

static bool wr(uint8_t reg, uint8_t val)
{
    return bsp_i2c_write_reg(s_addr, reg, val);
}

/* Program everything except the (slow) reset; used by init and by the
 * brown-out repair path in service(). */
static bool configure(void)
{
    if (!wr(MPU6050_REG_PWR_MGMT_1, PWR_MGMT_1_WANTED)) { return false; }
    bsp_delay_us(10000u);                       /* PLL settle              */

    (void)wr(MPU6050_REG_PWR_MGMT_2, 0x00u);    /* all axes enabled        */
    (void)wr(MPU6050_REG_SMPLRT_DIV, 0x00u);    /* 1 kHz internal rate     */
    /* DLPF_CFG=1: ~188 Hz accel bandwidth -- wide enough for the 25..100 Hz
     * machine-vibration band, narrow enough to keep broadband noise down. */
    (void)wr(MPU6050_REG_CONFIG, 0x01u);

    uint8_t afs = (uint8_t)(MPU6050_ACCEL_RANGE & 0x03u);
    s_lsb_per_g = ACCEL_LSB_PER_G[afs];
    if (!wr(MPU6050_REG_ACCEL_CONFIG, (uint8_t)(afs << 3))) { return false; }

    /* Verify: wake bit really cleared and FSR really latched. */
    uint8_t pwr = 0xFFu, acc = 0xFFu;
    if (!rd(MPU6050_REG_PWR_MGMT_1, &pwr, 1u))     { return false; }
    if (!rd(MPU6050_REG_ACCEL_CONFIG, &acc, 1u))   { return false; }
    if ((pwr & 0x40u) != 0u)                       { return false; }   /* still asleep */
    if (((acc >> 3) & 0x03u) != afs)               { return false; }
    return true;
}

static bool try_address(uint8_t addr)
{
    if (!bsp_i2c_probe(addr)) { return false; }
    s_addr = addr;

    uint8_t who = 0u;
    if (!rd(MPU6050_REG_WHO_AM_I, &who, 1u)) { return false; }
    s_who = who;
    /* 0x00 / 0xFF = a dead or floating bus, not a sensor. Anything else is
     * accepted: genuine MPU6050 = 0x68, clones and MPU6500/9250/9255 report
     * 0x70/0x71/0x73/0x98 and share this register map. */
    if (who == 0x00u || who == 0xFFu) { return false; }

    (void)wr(MPU6050_REG_PWR_MGMT_1, 0x80u);    /* DEVICE_RESET            */
    bsp_delay_us(40000u);
    bsp_delay_us(40000u);                       /* datasheet: >= 100 ms    */
    bsp_delay_us(20000u);

    return configure();
}

bool mpu6050_init(void)
{
    s_present     = false;
    s_fail_streak = 0u;
    s_same_count  = 0u;
    memset(s_last_raw, 0, sizeof(s_last_raw));

    const uint8_t first  = (uint8_t)MPU6050_I2C_ADDR_7BIT;
    const uint8_t second = (first == 0x68u) ? 0x69u : 0x68u;

    if (try_address(first) || try_address(second)) {
        s_present = true;
        return true;
    }
    s_addr = 0u;
    return false;
}

bool mpu6050_is_present(void)  { return s_present; }
uint8_t mpu6050_address(void)  { return s_present ? s_addr : 0u; }
uint8_t mpu6050_who_am_i(void) { return s_who; }

void mpu6050_service(void)
{
    if (!s_present) {
        (void)mpu6050_init();           /* missing at boot / hot-plug      */
        return;
    }

    /* Hung data path: identical bursts for ~4 s on a live sensor is not
     * physics. Full re-init. */
    if (s_same_count >= FROZEN_LIMIT) {
        (void)mpu6050_init();
        return;
    }

    uint8_t pwr = 0u;
    if (!rd(MPU6050_REG_PWR_MGMT_1, &pwr, 1u)) {
        if (++s_fail_streak >= 3u) {
            s_present = false;          /* stopped answering: re-probe     */
        }
        return;
    }
    s_fail_streak = 0u;
    if (pwr != PWR_MGMT_1_WANTED) {
        /* Power glitch dropped it back into SLEEP / default clock. */
        if (!configure()) {
            s_present = false;
        }
    }
}

bool mpu6050_read_accel_g(float *ax_g, float *ay_g, float *az_g)
{
    if (!s_present) { return false; }

    uint8_t buf[6];     /* XH XL YH YL ZH ZL */
    if (!rd(MPU6050_REG_ACCEL_XOUT_H, buf, 6u)) {
        if (++s_fail_streak >= MAX_CONSEC_FAILS) { s_present = false; }
        return false;
    }

    /* A stuck bus reads all-0xFF, a sleeping/reset chip reads all-0x00.
     * Neither is an acceleration; do not let it become a fake 1 g spike. */
    bool all0 = true, allF = true;
    for (int i = 0; i < 6; i++) {
        if (buf[i] != 0x00u) { all0 = false; }
        if (buf[i] != 0xFFu) { allF = false; }
    }
    if (all0 || allF) {
        if (++s_fail_streak >= MAX_CONSEC_FAILS) { s_present = false; }
        return false;
    }
    s_fail_streak = 0u;

    if (memcmp(buf, s_last_raw, 6u) == 0) {
        if (s_same_count < 0xFFFFu) { s_same_count++; }
    } else {
        s_same_count = 0u;
        memcpy(s_last_raw, buf, 6u);
    }

    int16_t rx = (int16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
    int16_t ry = (int16_t)(((uint16_t)buf[2] << 8) | (uint16_t)buf[3]);
    int16_t rz = (int16_t)(((uint16_t)buf[4] << 8) | (uint16_t)buf[5]);

    *ax_g = (float)rx / s_lsb_per_g;
    *ay_g = (float)ry / s_lsb_per_g;
    *az_g = (float)rz / s_lsb_per_g;
    return true;
}

bool mpu6050_read_magnitude_g(float *mag_g)
{
    float ax, ay, az;
    if (!mpu6050_read_accel_g(&ax, &ay, &az)) {
        return false;
    }
    *mag_g = sqrtf(ax * ax + ay * ay + az * az);
    return true;
}

bool mpu6050_read_temp_c(float *temp_c)
{
    if (!s_present) { return false; }
    uint8_t b[2];
    if (!rd(MPU6050_REG_TEMP_OUT_H, b, 2u)) {
        return false;
    }
    int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
    /* MPU6050 datasheet: degC = raw/340 + 36.53 */
    *temp_c = ((float)raw / 340.0f) + 36.53f;
    return true;
}
