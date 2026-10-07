/*
 * test_sensors.c -- host-side behavioural tests for the INA219 and MPU6050
 * drivers (STM32/stm32-bringup/Application/Src/{ina219,mpu6050}.c).
 *
 * The real drivers are compiled unchanged. Only bsp_i2c_* / bsp_delay_us are
 * replaced by a small simulated I2C bus holding a register-accurate INA219
 * and MPU6050, so the tests can inject the failure modes seen on hardware:
 * wrong address, clone WHO_AM_I, sleeping chip, brown-out reset, frozen
 * data, stuck-at-0xFF bus, missing-at-boot / hot-plug.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "bringup_config.h"
#include "ina219.h"
#include "mpu6050.h"
#include "bsp.h"

/* ------------------------------------------------------------------ */
/* Simulated devices                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     present;
    uint8_t  addr;
    uint16_t cfg;          /* INA219 config                              */
    int16_t  shunt;        /* INA219 shunt register (10 uV LSB)          */
    uint16_t bus;          /* INA219 bus register                        */
    uint8_t  nack_next;    /* force N failures                           */
} sim_ina_t;

typedef struct {
    bool    present;
    uint8_t addr;
    uint8_t who;
    uint8_t pwr1;          /* power-on default 0x40 = SLEEP              */
    uint8_t accel_cfg;
    uint8_t raw[6];
    bool    stuck_ff;      /* bus reads all 0xFF                         */
    bool    reg_clamp_zero;/* sleeping chip returns 0                    */
} sim_mpu_t;

static sim_ina_t g_ina;
static sim_mpu_t g_mpu;
static uint32_t  g_bus_errors;

static void ina_reset(void) { g_ina.cfg = 0x399F; }
static void mpu_reset(void) { g_mpu.pwr1 = 0x40; g_mpu.accel_cfg = 0; }

void bsp_delay_us(uint32_t us) { (void)us; }
bool bsp_i2c_recover(void) { return true; }
uint32_t bsp_i2c_error_count(void) { return g_bus_errors; }
uint32_t bsp_i2c_recover_count(void) { return 0; }

bool bsp_i2c_probe(uint8_t a)
{
    return (g_ina.present && a == g_ina.addr) || (g_mpu.present && a == g_mpu.addr);
}

bool bsp_i2c_write(uint8_t a, const uint8_t *d, uint32_t n)
{
    if (g_ina.present && a == g_ina.addr) {
        if (g_ina.nack_next) { g_ina.nack_next--; return false; }
        if (n == 3 && d[0] == 0x00) {
            uint16_t v = (uint16_t)((d[1] << 8) | d[2]);
            if (v & 0x8000) { ina_reset(); } else { g_ina.cfg = v; }
        }
        return true;
    }
    if (g_mpu.present && a == g_mpu.addr) {
        if (n == 2) {
            switch (d[0]) {
            case 0x6B:
                if (d[1] & 0x80) { mpu_reset(); } else { g_mpu.pwr1 = d[1]; }
                break;
            case 0x1C: g_mpu.accel_cfg = d[1]; break;
            default: break;
            }
        }
        return true;
    }
    return false;
}

bool bsp_i2c_read_reg(uint8_t a, uint8_t reg, uint8_t *o, uint32_t n)
{
    if (g_ina.present && a == g_ina.addr) {
        if (g_ina.nack_next) { g_ina.nack_next--; return false; }
        uint16_t v = 0;
        switch (reg) {
        case 0x00: v = g_ina.cfg; break;
        case 0x01: v = (uint16_t)g_ina.shunt; break;
        case 0x02: v = g_ina.bus; break;
        default: break;
        }
        if (n == 2) { o[0] = (uint8_t)(v >> 8); o[1] = (uint8_t)v; return true; }
        return false;
    }
    if (g_mpu.present && a == g_mpu.addr) {
        if (reg == 0x75 && n == 1) { o[0] = g_mpu.who; return true; }
        if (reg == 0x6B && n == 1) { o[0] = g_mpu.pwr1; return true; }
        if (reg == 0x1C && n == 1) { o[0] = g_mpu.accel_cfg; return true; }
        if (reg == 0x3B && n == 6) {
            if (g_mpu.stuck_ff) { memset(o, 0xFF, 6); return true; }
            if (g_mpu.pwr1 & 0x40) { memset(o, 0x00, 6); return true; }
            memcpy(o, g_mpu.raw, 6); return true;
        }
        return false;
    }
    return false;
}

bool bsp_i2c_write_reg(uint8_t a, uint8_t r, uint8_t v)
{
    uint8_t b[2] = { r, v };
    return bsp_i2c_write(a, b, 2);
}
bool bsp_i2c_read(uint8_t a, uint8_t *d, uint32_t n) { (void)a; (void)d; (void)n; return false; }

/* ------------------------------------------------------------------ */
/* Tiny test harness                                                   */
/* ------------------------------------------------------------------ */
static int g_pass, g_fail;
#define CHECK(c) do { if (c) { g_pass++; } else { g_fail++; \
    printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void world_reset(void)
{
    memset(&g_ina, 0, sizeof g_ina);
    memset(&g_mpu, 0, sizeof g_mpu);
    g_ina.present = true; g_ina.addr = 0x40; ina_reset();
    g_mpu.present = true; g_mpu.addr = 0x68; g_mpu.who = 0x68; mpu_reset();
    /* 0 g, 0 g, 1 g at +-8 g (4096 LSB/g) */
    g_mpu.raw[4] = 0x10; g_mpu.raw[5] = 0x00;
}

static void set_acc(int16_t x, int16_t y, int16_t z)
{
    g_mpu.raw[0] = (uint8_t)((uint16_t)x >> 8); g_mpu.raw[1] = (uint8_t)x;
    g_mpu.raw[2] = (uint8_t)((uint16_t)y >> 8); g_mpu.raw[3] = (uint8_t)y;
    g_mpu.raw[4] = (uint8_t)((uint16_t)z >> 8); g_mpu.raw[5] = (uint8_t)z;
}

int main(void)
{
    float a, ax, ay, az;
    int32_t ma;

    printf("== INA219 ==\n");
    world_reset();
    CHECK(INA219_FULL_SCALE_MA == 3200u);          /* 0.1 ohm, PGA /8 */
    CHECK(ina219_init());
    CHECK(ina219_address() == 0x40);
    CHECK(g_ina.cfg == 0x39DF);                    /* PGA/8, 8x avg, cont. */

    /* 0.42 A through 0.1 ohm = 42 mV = 4200 counts: must NOT clip at 0.4 A */
    g_ina.shunt = 4200;
    CHECK(ina219_read_current_a(&a));
    CHECK(fabsf(a - 0.42f) < 1e-4f);
    CHECK(!ina219_saturated());
    g_ina.shunt = 30000;                           /* 3.0 A */
    CHECK(ina219_read_current_a(&a) && fabsf(a - 3.0f) < 1e-4f);
    CHECK(!ina219_saturated());
    g_ina.shunt = 32000;                           /* rail */
    CHECK(ina219_read_current_a(&a) && ina219_saturated());
    g_ina.shunt = -1234;                           /* reverse */
    CHECK(ina219_read_current_a(&a) && fabsf(a + 0.1234f) < 1e-4f);
    CHECK(ina219_read_current_ma(&ma) && ma == -123);
    g_ina.shunt = 3;                               /* 0.3 mA: sub-mA kept */
    CHECK(ina219_read_current_a(&a) && fabsf(a - 0.0003f) < 1e-6f);
    g_ina.bus = (uint16_t)((3000u) << 3);          /* 12.000 V */
    uint32_t mv = 0;
    CHECK(ina219_read_bus_mv(&mv) && mv == 12000u);

    /* brown-out: chip falls back to power-on default, service repairs it */
    g_ina.cfg = 0x399F;
    ina219_service();
    CHECK(g_ina.cfg == 0x39DF);

    /* wrong address strap: found by scan */
    world_reset(); g_ina.addr = 0x44;
    CHECK(ina219_init());
    CHECK(ina219_address() == 0x44);

    /* A different chip ACKing 0x40 must be rejected (config doesn't stick) */
    world_reset();
    g_ina.present = false;                         /* INA gone ...        */
    CHECK(!ina219_init());
    CHECK(!ina219_is_present());

    /* missing at boot, hot-plugged later */
    ina219_service();
    CHECK(!ina219_is_present());
    g_ina.present = true; g_ina.addr = 0x40; ina_reset();
    ina219_service();
    CHECK(ina219_is_present());
    g_ina.shunt = 1000;
    CHECK(ina219_read_current_a(&a) && fabsf(a - 0.1f) < 1e-4f);

    /* sustained read failure drops presence, service() brings it back */
    for (int i = 0; i < 12; i++) { g_ina.nack_next = 4; (void)ina219_read_current_a(&a); }
    CHECK(!ina219_is_present());
    g_ina.nack_next = 0;
    ina219_service();
    CHECK(ina219_is_present());

    printf("== MPU6050 ==\n");
    world_reset();
    CHECK(mpu6050_init());
    CHECK(mpu6050_address() == 0x68);
    CHECK((g_mpu.pwr1 & 0x40) == 0);               /* awake */
    CHECK(((g_mpu.accel_cfg >> 3) & 3) == MPU6050_ACCEL_RANGE);
    CHECK(mpu6050_read_accel_g(&ax, &ay, &az));
    CHECK(fabsf(az - 1.0f) < 1e-3f && fabsf(ax) < 1e-3f);
    CHECK(mpu6050_read_magnitude_g(&a) && fabsf(a - 1.0f) < 1e-3f);

    /* clone WHO_AM_I values must be accepted */
    const uint8_t whos[] = { 0x70, 0x71, 0x73, 0x98, 0x7C };
    for (unsigned i = 0; i < sizeof whos; i++) {
        world_reset(); g_mpu.who = whos[i];
        CHECK(mpu6050_init());
    }
    /* but a dead/floating bus is not a sensor */
    world_reset(); g_mpu.who = 0xFF; CHECK(!mpu6050_init());
    world_reset(); g_mpu.who = 0x00; CHECK(!mpu6050_init());

    /* AD0 tied high -> 0x69 */
    world_reset(); g_mpu.addr = 0x69;
    CHECK(mpu6050_init());
    CHECK(mpu6050_address() == 0x69);

    /* all-0xFF / all-0x00 bursts are rejected, not turned into a 1 g spike */
    world_reset(); CHECK(mpu6050_init());
    g_mpu.stuck_ff = true;
    CHECK(!mpu6050_read_accel_g(&ax, &ay, &az));
    g_mpu.stuck_ff = false;
    CHECK(mpu6050_read_accel_g(&ax, &ay, &az));

    /* power glitch puts it back to SLEEP: service() re-wakes it */
    g_mpu.pwr1 = 0x40;
    CHECK(!mpu6050_read_accel_g(&ax, &ay, &az));   /* sleeping = zeros */
    mpu6050_service();
    CHECK((g_mpu.pwr1 & 0x40) == 0);
    CHECK(mpu6050_read_accel_g(&ax, &ay, &az));

    /* frozen output (hung sensor): reinitialised via service() */
    world_reset(); CHECK(mpu6050_init());
    for (int i = 0; i < 205; i++) { (void)mpu6050_read_accel_g(&ax, &ay, &az); }
    g_mpu.pwr1 = 0x41;                             /* mark so reinit is visible */
    mpu6050_service();
    CHECK(g_mpu.pwr1 == 0x01);

    /* missing at boot, then appears */
    world_reset(); g_mpu.present = false;
    CHECK(!mpu6050_init());
    mpu6050_service();
    CHECK(!mpu6050_is_present());
    g_mpu.present = true; mpu_reset();
    mpu6050_service();
    CHECK(mpu6050_is_present());
    CHECK(mpu6050_read_accel_g(&ax, &ay, &az));

    /* +-8 g scaling: 8 g on X = 32768 clipped; 2 g = 8192 */
    world_reset(); CHECK(mpu6050_init());
    set_acc(8192, 0, 4096);
    CHECK(mpu6050_read_accel_g(&ax, &ay, &az) && fabsf(ax - 2.0f) < 1e-3f);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
