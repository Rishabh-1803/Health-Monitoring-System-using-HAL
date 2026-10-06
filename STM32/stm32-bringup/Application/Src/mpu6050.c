/**
 * @file    mpu6050.c
 * @brief   MPU6050 I2C accelerometer/gyro driver.
 *
 * Wake-up is the one thing that catches people: the part ships in SLEEP mode
 * and returns zeros until PWR_MGMT_1 bit 6 is cleared. We also set the clock
 * to PLL-with-X-gyro (more stable than the internal RC) and pick the accel
 * full-scale from the config so the g divisor is always right.
 *
 * Acceleration math: the accel registers are big-endian signed 16-bit. At
 * +-2 g FSR the sensitivity is 16384 LSB/g. For +-4/8/16 g the divisor is
 * 8192/4096/2048 respectively. Vibration magnitude is the Euclidean norm of
 * the three axes; at rest it sits at ~1 g (gravity) and the app high-passes
 * that out so the displayed "vibration g" is the dynamic component only.
 */
#include "bringup_config.h"
#include "mpu6050.h"
#include "bsp.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define MPU6050_ADDR        (MPU6050_I2C_ADDR_7BIT)
#define MPU6050_WHOAMI_VAL  0x68u   /* MPU6050; MPU9250 is 0x71, clones vary */

/* Accel FSR -> LSB-per-g. Index by the 2-bit AFS_SEL field. */
static const float ACCEL_LSB_PER_G[4] = { 16384.0f, 8192.0f, 4096.0f, 2048.0f };

static bool s_present = false;
static float s_lsb_per_g = 16384.0f;

static bool read_reg(uint8_t reg, uint8_t *out)
{
    return bsp_i2c_read_reg(MPU6050_ADDR, reg, out, 1u);
}

static bool read_reg16_signed(uint8_t reg, int16_t *out)
{
    uint8_t buf[2];
    if (!bsp_i2c_read_reg(MPU6050_ADDR, reg, buf, 2u)) {
        return false;
    }
    /* Big-endian on the wire. */
    *out = (int16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
    return true;
}

static bool write_reg(uint8_t reg, uint8_t val)
{
    return bsp_i2c_write_reg(MPU6050_ADDR, reg, val);
}

bool mpu6050_init(void)
{
    /* WHO_AM_I is the cheap identity check; a missing sensor (or one at the
     * wrong address) returns 0xFF or 0x00 and we bail. */
    uint8_t who = 0u;
    if (!read_reg(MPU6050_REG_WHO_AM_I, &who)) {
        s_present = false;
        return false;
    }
    /* The genuine MPU6050 answers 0x68; some clones answer 0x98/0x7E etc.
     * Accept 0x68 strictly so a mis-wired INA219 doesn't masquerade. */
    if (who != MPU6050_WHOAMI_VAL) {
        s_present = false;
        return false;
    }

    /* PWR_MGMT_1: clear SLEEP (bit 6), pick PLL with X-gyro clock (bits 2:0 = 1).
     * A 0x00 write would also wake it but leaves the noisier internal RC. */
    if (!write_reg(MPU6050_REG_PWR_MGMT_1, 0x01u)) {
        s_present = false;
        return false;
    }
    /* Give the oscillator + PLL time to settle before further config. The
     * datasheet quotes ~30 ms from SLEEP=0 to stable; a short busy spin is
     * fine here because init runs once, outside the loop. */
    bsp_delay_us(40000u);

    /* Sample rate: 1 kHz / (1 + SMPLRT_DIV). DIV=0 -> 1 kHz (fastest). */
    (void)write_reg(MPU6050_REG_SMPLRT_DIV, 0x00u);
    /* No external frame sync, DLPF_CFG=1 (1 kHz gyro, ~188 Hz accel BW).
     * A little filtering keeps the vibration number from dancing on noise. */
    (void)write_reg(MPU6050_REG_CONFIG, 0x01u);

    /* Accelerometer FSR from config. AFS_SEL lives in bits 4:3 of ACCEL_CONFIG. */
    uint8_t afs = MPU6050_ACCEL_RANGE & 0x03u;
    s_lsb_per_g = ACCEL_LSB_PER_G[afs];
    (void)write_reg(MPU6050_REG_ACCEL_CONFIG, (uint8_t)(afs << 3));

    s_present = true;
    return true;
}

bool mpu6050_is_present(void) { return s_present; }

bool mpu6050_read_accel_g(float *ax_g, float *ay_g, float *az_g)
{
    if (!s_present) { return false; }
    int16_t rx, ry, rz;
    /* Six bytes starting at ACCEL_XOUT_H: XH XL YH YL ZH ZL. */
    uint8_t buf[6];
    if (!bsp_i2c_read_reg(MPU6050_ADDR, MPU6050_REG_ACCEL_XOUT_H, buf, 6u)) {
        return false;
    }
    rx = (int16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
    ry = (int16_t)(((uint16_t)buf[2] << 8) | (uint16_t)buf[3]);
    rz = (int16_t)(((uint16_t)buf[4] << 8) | (uint16_t)buf[5]);

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
    int16_t raw;
    if (!read_reg16_signed(0x41u, &raw)) {     /* TEMP_OUT_H */
        return false;
    }
    /* Datasheet: degC = raw/340 + 36.53 */
    *temp_c = ((float)raw / 340.0f) + 36.53f;
    return true;
}
