/**
 * @file    bsp.c
 * @brief   Board support implementation.
 *
 * Three things here are done at register level rather than through HAL,
 * and the reason is the same in each case: the driver simply is not in
 * this project.
 *
 *   I2C  -- stm32f4xx_hal_i2c.c was never generated, because the .ioc
 *           never enabled I2C. Rather than hand-import ST's driver, the
 *           bus is bit-banged. For a bring-up tool that is arguably the
 *           better choice anyway: it cannot get stuck in the F4's
 *           notorious BUSY-flag lockup, it works with any pull-ups, and
 *           clock stretching and timeouts are under our control.
 *
 *   ADC  -- stm32f4xx_hal_adc.c is likewise absent. A single-channel
 *           polled conversion is about thirty lines of register writes,
 *           so that is what this does.
 *
 *   DWT  -- the cycle counter gives a microsecond delay. 1-Wire needs
 *           6 us pulses; the FreeRTOS tick is 1 ms, so HAL_Delay and
 *           vTaskDelay are both useless at this scale.
 *
 * TIM3 (buzzer) and GPIO DO go through HAL, because those drivers are
 * present and enabled in stm32f4xx_hal_conf.h.
 */

#include "main.h"
#include "bringup_config.h"
#include "bsp.h"

/* ================================================================== */
/*  DWT microsecond timing                                            */
/* ================================================================== */

static uint32_t s_cycles_per_us = 96u;   /* corrected in bsp_init */
static bool     s_dwt_ok        = false;

static void dwt_init(void)
{
    /* TRCENA gates the whole trace/debug block, DWT included. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    s_cycles_per_us = SystemCoreClock / 1000000u;
    if (s_cycles_per_us == 0u) {
        s_cycles_per_us = 1u;
    }

    /* Prove it actually counts. On most F4 parts it runs without a
     * debugger attached, but confirming beats assuming -- if it is
     * dead, every 1-Wire timing below would silently collapse. */
    uint32_t a = DWT->CYCCNT;
    for (volatile int i = 0; i < 100; i++) { __NOP(); }
    uint32_t b = DWT->CYCCNT;
    s_dwt_ok = (b != a);
}

bool bsp_dwt_ok(void) { return s_dwt_ok; }

void bsp_delay_us(uint32_t us)
{
    if (s_dwt_ok) {
        /* us * cycles_per_us must stay inside 32 bits. At 96 MHz that caps
         * a single call at ~44.7 s; clamp rather than silently wrapping
         * into a near-zero delay. Nothing here asks for more than 500 ms. */
        const uint32_t max_us = 0xFFFFFFFFu / s_cycles_per_us;
        if (us > max_us) { us = max_us; }

        uint32_t start  = DWT->CYCCNT;
        uint32_t target = us * s_cycles_per_us;
        /* Unsigned subtraction handles the 32-bit wrap correctly. */
        while ((DWT->CYCCNT - start) < target) {
            /* spin */
        }
    } else {
        /* Fallback for a part where the cycle counter will not run. The
         * divisor is an ESTIMATE of the cost of one volatile decrement
         * plus loop overhead on this core (load, subtract, store, compare,
         * branch, nop), not a measured figure, so treat the result as
         * "roughly the right order" and nothing better. bsp_dwt_ok() is
         * reported in the boot banner and the 1-Wire test refuses to run
         * without DWT, precisely because this is not good enough for it. */
        volatile uint32_t n = us * (s_cycles_per_us / 8u + 1u);
        while (n-- > 0u) { __NOP(); }
    }
}

uint32_t bsp_cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t bsp_elapsed_us(uint32_t start_cycles)
{
    /* Subtract first, divide second. Doing it the other way round -- the
     * obvious "return CYCCNT / cycles_per_us" -- destroys the modular
     * arithmetic: the quotient climbs to 44739242 and then drops to 0, so
     * any difference taken across the rollover comes out as roughly
     * 4.25e9 instead of a small number, and every timeout built on it
     * fires instantly once every 44.7 s. */
    return (DWT->CYCCNT - start_cycles) / s_cycles_per_us;
}

/* ================================================================== */
/*  Pin helpers                                                       */
/* ================================================================== */

/* Open-drain "release" and "pull low", used by both software I2C and
 * 1-Wire. In open-drain output mode the input data register still shows
 * the real line level, which is what lets us read back a line we are
 * nominally driving. */
#define PIN_LOW(port, pin)      HAL_GPIO_WritePin((port), (pin), GPIO_PIN_RESET)
#define PIN_RELEASE(port, pin)  HAL_GPIO_WritePin((port), (pin), GPIO_PIN_SET)
#define PIN_READ(port, pin)     (HAL_GPIO_ReadPin((port), (pin)) == GPIO_PIN_SET)

static void gpio_init_all(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Every output below sets its safe level BEFORE HAL_GPIO_Init, not
     * after. HAL_GPIO_Init does not touch ODR, and ODR resets to 0, so a
     * pin driven from a fresh reset goes low the instant MODER is written.
     * On the relay that means a brief asserted pulse, because the relay is
     * active-low -- microseconds, far too short for a coil to answer, but
     * this is a board with mains-capable switching on it and there is no
     * reason to leave the window open. Writing ODR first closes it, and it
     * is the same order CubeMX itself uses for PC13 in Core/Src/gpio.c. */

    /* ---- onboard LED (PC13) ---- */
    bsp_led_set(false);
    g.Pin   = LED_ONBOARD_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_ONBOARD_PORT, &g);

    /* ---- relay ---- */
    bsp_relay_set(false);
    g.Pin = RELAY_PIN;
    HAL_GPIO_Init(RELAY_PORT, &g);

    /* ---- buzzer ---- */
#if BUZZER_IS_ACTIVE_TYPE
    PIN_LOW(BUZZER_PORT, BUZZER_PIN);
    g.Pin  = BUZZER_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(BUZZER_PORT, &g);
#else
    g.Pin       = BUZZER_PIN;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = BUZZER_TIM_AF;
    HAL_GPIO_Init(BUZZER_PORT, &g);
#endif

    /* ---- software I2C: open-drain, internal pull-up as a fallback ---- */
    PIN_RELEASE(I2C_SCL_PORT, I2C_SCL_PIN);
    PIN_RELEASE(I2C_SDA_PORT, I2C_SDA_PIN);
    g.Pin       = I2C_SCL_PIN;
    g.Mode      = GPIO_MODE_OUTPUT_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = 0;
    HAL_GPIO_Init(I2C_SCL_PORT, &g);
    g.Pin = I2C_SDA_PIN;
    HAL_GPIO_Init(I2C_SDA_PORT, &g);

    /* ---- 1-Wire: same open-drain trick ---- */
    PIN_RELEASE(DS18B20_PORT, DS18B20_PIN);
    g.Pin   = DS18B20_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DS18B20_PORT, &g);

    /* ---- vibration switch input ---- */
    g.Pin  = VIBRATION_PIN;
    g.Mode = GPIO_MODE_INPUT;
#if VIBRATION_ACTIVE_LOW
    g.Pull = GPIO_PULLUP;
#else
    g.Pull = GPIO_PULLDOWN;
#endif
    HAL_GPIO_Init(VIBRATION_PORT, &g);

    /* ---- analog input: no pull, no digital buffer ---- */
    g.Pin  = ADC_INPUT_PIN;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ADC_INPUT_PORT, &g);
}

void bsp_led_set(bool on)
{
#if LED_ONBOARD_ACTIVE_LOW
    HAL_GPIO_WritePin(LED_ONBOARD_PORT, LED_ONBOARD_PIN,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(LED_ONBOARD_PORT, LED_ONBOARD_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

void bsp_led_toggle(void)
{
    HAL_GPIO_TogglePin(LED_ONBOARD_PORT, LED_ONBOARD_PIN);
}

void bsp_relay_set(bool on)
{
#if RELAY_ACTIVE_LOW
    HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

bool bsp_vibration_asserted(void)
{
    bool high = PIN_READ(VIBRATION_PORT, VIBRATION_PIN);
#if VIBRATION_ACTIVE_LOW
    return !high;
#else
    return high;
#endif
}

/* ================================================================== */
/*  Buzzer PWM on TIM3                                                */
/* ================================================================== */

#if !BUZZER_IS_ACTIVE_TYPE

/* A 1 MHz timer tick means the reload value IS the period in microseconds,
 * which keeps the frequency maths readable. The tick is only exact if the
 * prescaler divides the timer clock evenly, and 20 Hz is the lowest tone
 * that still fits a 16-bit reload at 1 MHz. */
#if (BUZZER_DEFAULT_HZ < 20u) || (BUZZER_DEFAULT_HZ > 20000u)
#error "BUZZER_DEFAULT_HZ must be 20..20000 -- outside that the 16-bit reload truncates"
#endif

static TIM_HandleTypeDef s_htim_buzzer;

/**
 * The APB1 timer clock, read out of the RCC rather than hardcoded.
 *
 * RM0383 section 6.2: when the APB prescaler is 1 the timer clock equals
 * the APB clock; for any other prescaler it is twice the APB clock. On
 * this project's tree APB1 is /2, so PCLK1 is 48 MHz and TIM3 sees 96 MHz.
 * Deriving it means changing the clock tree in CubeMX shifts the prescaler
 * automatically instead of silently detuning every tone.
 */
static uint32_t apb1_timer_clk_hz(void)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) {
        return pclk1;
    }
    return pclk1 * 2u;
}

static void buzzer_init(void)
{
    TIM_OC_InitTypeDef oc = {0};

    __HAL_RCC_TIM3_CLK_ENABLE();

    uint32_t tim_clk = apb1_timer_clk_hz();
    if (tim_clk < 1000000u) { tim_clk = 1000000u; }   /* keep the prescaler sane */

    s_htim_buzzer.Instance           = TIM3;
    s_htim_buzzer.Init.Prescaler     = (tim_clk / 1000000u) - 1u;
    s_htim_buzzer.Init.CounterMode   = TIM_COUNTERMODE_UP;
    s_htim_buzzer.Init.Period        = (1000000u / BUZZER_DEFAULT_HZ) - 1u;
    s_htim_buzzer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    s_htim_buzzer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_PWM_Init(&s_htim_buzzer);

    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = 0u;                 /* start silent */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    (void)HAL_TIM_PWM_ConfigChannel(&s_htim_buzzer, &oc, BUZZER_TIM_CHANNEL);
    (void)HAL_TIM_PWM_Start(&s_htim_buzzer, BUZZER_TIM_CHANNEL);
}
#endif

void bsp_buzzer_tone(uint32_t freq_hz)
{
#if BUZZER_IS_ACTIVE_TYPE
    /* An active buzzer generates its own tone; it only wants a level. */
    (void)freq_hz;
    PIN_RELEASE(BUZZER_PORT, BUZZER_PIN);
#else
    if (freq_hz < 20u || freq_hz > 20000u) {
        bsp_buzzer_off();
        return;
    }
    uint32_t arr = (1000000u / freq_hz);
    if (arr == 0u) { arr = 1u; }
    arr -= 1u;
    __HAL_TIM_SET_AUTORELOAD(&s_htim_buzzer, arr);
    /* 50 % duty is the loudest point for most passive buzzers. */
    __HAL_TIM_SET_COMPARE(&s_htim_buzzer, BUZZER_TIM_CHANNEL, arr / 2u);
    __HAL_TIM_SET_COUNTER(&s_htim_buzzer, 0u);
#endif
}

void bsp_buzzer_off(void)
{
#if BUZZER_IS_ACTIVE_TYPE
    PIN_LOW(BUZZER_PORT, BUZZER_PIN);
#else
    __HAL_TIM_SET_COMPARE(&s_htim_buzzer, BUZZER_TIM_CHANNEL, 0u);
#endif
}

/* ================================================================== */
/*  Software I2C                                                      */
/* ================================================================== */

#define SCL_HIGH()  PIN_RELEASE(I2C_SCL_PORT, I2C_SCL_PIN)
#define SCL_LOW()   PIN_LOW(I2C_SCL_PORT, I2C_SCL_PIN)
#define SDA_HIGH()  PIN_RELEASE(I2C_SDA_PORT, I2C_SDA_PIN)
#define SDA_LOW()   PIN_LOW(I2C_SDA_PORT, I2C_SDA_PIN)
#define SDA_READ()  PIN_READ(I2C_SDA_PORT, I2C_SDA_PIN)
#define SCL_READ()  PIN_READ(I2C_SCL_PORT, I2C_SCL_PIN)

static void i2c_half(void) { bsp_delay_us(I2C_HALF_BIT_US); }

/**
 * Release SCL and wait for it to actually read high. A slave holding it
 * down is clock stretching, which is legal; a wiring fault looks the
 * same, so we time out rather than hang forever.
 */
static bool scl_release_and_wait(void)
{
    SCL_HIGH();
    uint32_t waited = 0u;
    while (!SCL_READ()) {
        bsp_delay_us(1u);
        if (++waited > I2C_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

static bool i2c_start(void)
{
    SDA_HIGH();
    if (!scl_release_and_wait()) { return false; }
    i2c_half();
    SDA_LOW();
    i2c_half();
    SCL_LOW();
    i2c_half();
    return true;
}

static void i2c_stop(void)
{
    SDA_LOW();
    i2c_half();
    (void)scl_release_and_wait();
    i2c_half();
    SDA_HIGH();
    i2c_half();
}

static bool i2c_write_bit(bool bit)
{
    if (bit) { SDA_HIGH(); } else { SDA_LOW(); }
    i2c_half();
    if (!scl_release_and_wait()) { return false; }
    i2c_half();
    SCL_LOW();
    return true;
}

static bool i2c_read_bit(bool *bit)
{
    SDA_HIGH();                 /* let the slave drive it */
    i2c_half();
    if (!scl_release_and_wait()) { return false; }
    *bit = SDA_READ();
    i2c_half();
    SCL_LOW();
    return true;
}

/** Returns true when the slave ACKed (pulled SDA low on the 9th clock). */
static bool i2c_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (!i2c_write_bit((b >> i) & 1u)) { return false; }
    }
    bool nack = true;
    if (!i2c_read_bit(&nack)) { return false; }
    return !nack;
}

static bool i2c_read_byte(uint8_t *out, bool ack)
{
    uint8_t v = 0u;
    for (int i = 0; i < 8; i++) {
        bool bit = false;
        if (!i2c_read_bit(&bit)) { return false; }
        v = (uint8_t)((v << 1) | (bit ? 1u : 0u));
    }
    *out = v;
    /* ACK = pull SDA low, NACK = leave it high. */
    return i2c_write_bit(!ack);
}

bool bsp_i2c_recover(void)
{
    /* Nine clocks with SDA released is the standard way to walk a slave
     * out of a half-finished byte (NXP UM10204 section 3.1.16).
     *
     * The clocks are driven through scl_release_and_wait(), not a bare
     * SCL_HIGH(). If SCL is the stuck line -- no pull-up fitted, or a
     * slave jamming it -- then a bare release never produces an edge on
     * the wire, and the routine would clock nothing while cheerfully
     * reporting success. Waiting for SCL to actually read high means a
     * stuck clock is detected here instead of turning into a confusing
     * "no devices found" from the scan. */
    SDA_HIGH();
    for (int i = 0; i < 9; i++) {
        SCL_LOW();
        i2c_half();
        if (!scl_release_and_wait()) {
            return false;              /* SCL is held low: not recoverable here */
        }
        i2c_half();
        if (SDA_READ()) {
            break;
        }
    }
    i2c_stop();

    /* Both lines must end up released. Checking only SDA would report a
     * successful recovery on a bus whose clock is still stuck. */
    return SDA_READ() && SCL_READ();
}

bool bsp_i2c_probe(uint8_t addr7)
{
    if (!i2c_start()) { return false; }
    bool ack = i2c_write_byte((uint8_t)((addr7 << 1) | 0u));   /* write bit */
    i2c_stop();
    return ack;
}

bool bsp_i2c_write(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    if (!i2c_start()) { return false; }
    if (!i2c_write_byte((uint8_t)((addr7 << 1) | 0u))) { i2c_stop(); return false; }
    for (uint32_t i = 0u; i < len; i++) {
        if (!i2c_write_byte(data[i])) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

bool bsp_i2c_read(uint8_t addr7, uint8_t *data, uint32_t len)
{
    if (len == 0u) { return false; }
    if (!i2c_start()) { return false; }
    if (!i2c_write_byte((uint8_t)((addr7 << 1) | 1u))) { i2c_stop(); return false; }
    for (uint32_t i = 0u; i < len; i++) {
        /* ACK every byte except the last one. */
        if (!i2c_read_byte(&data[i], (i + 1u) < len)) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

bool bsp_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *data, uint32_t len)
{
    if (len == 0u) { return false; }

    /* Write the register pointer, then a repeated start for the read.
     * No stop in between -- some sensors reset their pointer on a stop. */
    if (!i2c_start()) { return false; }
    if (!i2c_write_byte((uint8_t)((addr7 << 1) | 0u))) { i2c_stop(); return false; }
    if (!i2c_write_byte(reg))                          { i2c_stop(); return false; }

    if (!i2c_start()) { i2c_stop(); return false; }     /* repeated start */
    if (!i2c_write_byte((uint8_t)((addr7 << 1) | 1u))) { i2c_stop(); return false; }
    for (uint32_t i = 0u; i < len; i++) {
        if (!i2c_read_byte(&data[i], (i + 1u) < len)) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

bool bsp_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return bsp_i2c_write(addr7, buf, 2u);
}

/* ================================================================== */
/*  ADC1, polled, single channel                                      */
/* ================================================================== */

/* Counts conversions that never raised EOC. Read by the ADC test so a
 * sporadically broken ADC is reported as broken rather than folded into
 * the numbers as if it were noise. */
static uint32_t s_adc_timeouts;

static void adc_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    /* PCLK2 is 96 MHz on this clock tree and the F411 ADC tops out at
     * 36 MHz, so /4 (24 MHz) is the fastest legal prescaler. */
    ADC1_COMMON->CCR &= ~(ADC_CCR_ADCPRE_1 | ADC_CCR_ADCPRE_0);
    ADC1_COMMON->CCR |= ADC_CCR_ADCPRE_0;          /* 01 = PCLK2 / 4 */

    ADC1->CR1 = 0u;                                /* 12-bit, no scan */
    ADC1->CR2 = ADC_CR2_EOCS;                      /* EOC after each conversion */

    /* Longest sampling time (480 cycles). Sensor outputs are often high
     * impedance and this costs nothing in a test tool.
     *
     * Chosen with #if, not a runtime `if`: the channel is a compile-time
     * constant, so a runtime branch still gets compiled on both sides,
     * and for any channel below 10 the SMPR1 shift count folds to a huge
     * unsigned value. The compiler then warns "shift count >= width of
     * type" about a line that can never execute. Selecting in the
     * preprocessor means only the correct register is ever touched.
     *
     * The 3-bit field is cleared before it is set. SMPRx reads as 0 after
     * reset so a bare |= works on the first pass, but not if anything ever
     * calls this twice or leaves the ADC configured. */
#if (ADC_INPUT_CHANNEL <= 9)
    ADC1->SMPR2 = (ADC1->SMPR2 & ~(0x7uL << (3u * ADC_INPUT_CHANNEL)))
                | (0x7uL << (3u * ADC_INPUT_CHANNEL));
#elif (ADC_INPUT_CHANNEL <= 18)
    ADC1->SMPR1 = (ADC1->SMPR1 & ~(0x7uL << (3u * (ADC_INPUT_CHANNEL - 10u))))
                | (0x7uL << (3u * (ADC_INPUT_CHANNEL - 10u)));
#else
#error "ADC_INPUT_CHANNEL must be 0..18 on STM32F411"
#endif

    ADC1->SQR1 &= ~ADC_SQR1_L;                     /* one conversion */
    ADC1->SQR3  = ADC_INPUT_CHANNEL;               /* ...this channel */

    s_adc_timeouts = 0u;
    ADC1->CR2 |= ADC_CR2_ADON;
    bsp_delay_us(10u);                             /* tSTAB */
}

uint16_t bsp_adc_read_raw(void)
{
    /* Every ADC_SR flag is rc_w0: writing 1 does nothing, writing 0
     * clears. So this read-modify-write clears EOC and leaves the rest of
     * the flags exactly as they were -- the same thing ST's own
     * __HAL_ADC_CLEAR_FLAG does. */
    ADC1->SR &= ~ADC_SR_EOC;
    ADC1->CR2 |= ADC_CR2_SWSTART;

    /* One conversion is 492 ADC clocks -- about 20.5 us at 24 MHz -- so
     * 100000 spins of a load/test/branch at 96 MHz cannot false-trip. */
    uint32_t guard = 0u;
    while ((ADC1->SR & ADC_SR_EOC) == 0u) {
        if (++guard > 100000u) {
            /* A returned 0 is indistinguishable from a genuine 0 V, which
             * is why the failure is also counted. bsp_adc_timeouts() is
             * what lets the test tell "reading zero volts" apart from
             * "the ADC is not converting". */
            s_adc_timeouts++;
            return 0u;
        }
    }
    return (uint16_t)(ADC1->DR & 0x0FFFu);
}

uint32_t bsp_adc_timeouts(void)
{
    return s_adc_timeouts;
}

uint16_t bsp_adc_read_avg(uint32_t n)
{
    if (n == 0u)    { n = 1u; }
    if (n > 1024u)  { n = 1024u; }

    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        sum += bsp_adc_read_raw();
    }
    return (uint16_t)(sum / n);
}

uint32_t bsp_adc_raw_to_mv(uint16_t raw)
{
    return ((uint32_t)raw * ADC_VREF_MV) / 4095u;
}

/* ================================================================== */
/*  1-Wire (DS18B20)                                                  */
/* ================================================================== */

/*
 * 1-Wire bit timing has microsecond tolerances, so the low pulse and the
 * sample point run with interrupts masked; the recovery tail afterwards
 * does not need to be.
 *
 * No FreeRTOS tick is actually lost. The longest masked window is about
 * 550 us in bsp_ow_reset, under the 1 ms tick, and SysTick latches its
 * pending bit -- so the tick ISR simply runs late. A tick would only go
 * missing if two SysTick overflows fell inside one masked window.
 *
 * PRIMASK is saved and restored rather than blindly re-enabled. Nothing
 * currently calls these from an already-masked context, and FreeRTOS
 * critical sections use BASEPRI on this core so they do not interact, but
 * a bare __enable_irq() would silently unmask a caller that had masked
 * deliberately. Save/restore costs nothing and removes the trap.
 *
 * Timings are the standard-speed values from Maxim AN126: A=6, B=64,
 * C=60, D=10, E=7, F=57, H=480, I=70, J=410.
 */

bool bsp_ow_reset(void)
{
    bool present;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    PIN_LOW(DS18B20_PORT, DS18B20_PIN);
    bsp_delay_us(480u);                  /* H: reset pulse, min 480 */
    PIN_RELEASE(DS18B20_PORT, DS18B20_PIN);
    bsp_delay_us(70u);                   /* I: slave answers inside 60-75 us */
    present = !PIN_READ(DS18B20_PORT, DS18B20_PIN);
    __set_PRIMASK(primask);

    bsp_delay_us(410u);                  /* J: completes the 960 us slot */
    return present;
}

static void ow_write_bit(bool bit)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    PIN_LOW(DS18B20_PORT, DS18B20_PIN);
    if (bit) {
        bsp_delay_us(6u);                /* A: t_LOW1, must stay under 15 us */
        PIN_RELEASE(DS18B20_PORT, DS18B20_PIN);
        __set_PRIMASK(primask);
        bsp_delay_us(64u);               /* B: completes the 70 us slot */
    } else {
        bsp_delay_us(60u);               /* C: t_LOW0, 60-120 us */
        PIN_RELEASE(DS18B20_PORT, DS18B20_PIN);
        __set_PRIMASK(primask);
        bsp_delay_us(10u);               /* D: completes the slot */
    }
}

static bool ow_read_bit(void)
{
    bool bit;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    PIN_LOW(DS18B20_PORT, DS18B20_PIN);
    bsp_delay_us(6u);                    /* A */
    PIN_RELEASE(DS18B20_PORT, DS18B20_PIN);
    /* E: sample at 6+7 = 13 us. AN126's own value is 9, putting the sample
     * at exactly t_RDV max (15 us) -- zero margin, and once the two GPIO
     * writes and the delay-loop entry cost are counted the real sample
     * lands past it. A slave releasing a read-0 right at 15 us would then
     * be misread as a 1 once the pull-up brought the line back up. 7 buys
     * ~2 us of margin and still samples well after the slave has decided;
     * this is the same place Arduino's OneWire library samples. */
    bsp_delay_us(7u);
    bit = PIN_READ(DS18B20_PORT, DS18B20_PIN);
    __set_PRIMASK(primask);

    bsp_delay_us(57u);                   /* F: completes the 70 us slot */
    return bit;
}

void bsp_ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {        /* LSB first */
        ow_write_bit((byte >> i) & 1u);
    }
}

uint8_t bsp_ow_read_byte(void)
{
    uint8_t v = 0u;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) {
            v |= (uint8_t)(1u << i);
        }
    }
    return v;
}

/* ================================================================== */
/*  Init                                                              */
/* ================================================================== */

void bsp_init(void)
{
    dwt_init();
    gpio_init_all();
#if !BUZZER_IS_ACTIVE_TYPE
    buzzer_init();
#endif
    adc_init();
}
