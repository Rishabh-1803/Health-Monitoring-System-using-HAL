/**
 * @file    app_init.c
 * @brief   The bring-up task: boot banner, automatic scan, then the menu.
 *
 * Split deliberately in two.
 *
 * bringup_app_init() runs from MX_FREERTOS_Init, BEFORE the scheduler
 * starts, and does the two things that must not wait:
 *
 *   1. bsp_init() -- pins, DWT, ADC, buzzer timer. This has to happen as
 *      early as possible. Until it runs, PB0/PB1/PB5/PB6/PB7 are still
 *      inputs, so the relay input pin is floating on a board that has a
 *      coil attached to it. Leaving that until after the task has waited
 *      up to 10.5 s for a USB host is a real window in which the relay
 *      can chatter. bsp_init needs no scheduler -- HAL GPIO/TIM plus a
 *      DWT busy-wait -- and HAL_Init, SystemClock_Config and MX_GPIO_Init
 *      have all already run by this point.
 *   2. Free the I2C bus if a slave is holding it down, and remember the
 *      answer so the banner can report it. Doing this before the first
 *      scan turns a confusing "no devices" into a clean result. Also a
 *      busy-wait, bounded to about 18 ms worst case.
 *   3. console_init() -- the ring buffer must exist before the USB
 *      receive interrupt can fire into it.
 *
 * bringup_task() then does everything that needs the scheduler:
 *
 *   4. Wait for the USB host to enumerate. Printing before that is a
 *      NULL dereference inside CDC_Transmit_FS, and the whole tool is
 *      useless if it hard-faults at boot.
 *   5. Print what the firmware knows about itself: clock, reset cause,
 *      whether the cycle counter is running. A wrong clock explains
 *      every timing failure downstream, so it is worth stating up front.
 *   6. Scan the I2C bus automatically -- it is non-destructive and the
 *      answer is wanted every single time.
 *   7. Hand over to the menu.
 *
 * Stack size is 2048 words (8 KB). The OLED frame buffer is static, but
 * the console formatting buffer and the flush buffer are on the stack,
 * and vsnprintf is not frugal. FreeRTOS heap is 32 KB, so this fits with
 * room to spare.
 */

#include "app_init.h"
#include "bringup_config.h"
#include "bsp.h"
#include "console.h"
#include "tests.h"

#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

static osThreadId_t s_bringup_task;

/* Result of the pre-scheduler bus recovery, held so the banner can print
 * it. Written before the scheduler starts and only read afterwards, so no
 * synchronisation is needed. */
static bool s_i2c_bus_free;

static const osThreadAttr_t bringup_task_attr = {
    .name       = "bringup",
    .stack_size = 2048 * 4,
    /* Deliberately one band below LEDTask, which CubeMX creates at
     * osPriorityNormal. LEDTask is what calls MX_USB_DEVICE_Init(), and
     * nothing here may print before that has run. Sitting lower means the
     * USB stack is guaranteed to be up before this task gets its first
     * slice, instead of relying on the two round-robining. LEDTask then
     * spends its life in osDelay(2000), so there is no starvation risk. */
    .priority   = (osPriority_t) osPriorityBelowNormal,
};

/* ================================================================== */
/*  Boot report                                                       */
/* ================================================================== */

static void print_reset_cause(void)
{
    /* Worth printing: a watchdog or brown-out reset changes how you read
     * everything that follows. Brown-out in particular points at the
     * supply, which is the usual cause of flaky sensors. */
    (void)console_print("  Last reset: ");

    if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST)) {
        (void)console_print("brown-out (check the 3V3 supply) ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)) {
        (void)console_print("NRST pin ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)) {
        (void)console_print("power-on ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)) {
        (void)console_print("software ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) {
        (void)console_print("independent watchdog ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) {
        (void)console_print("window watchdog ");
    }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) {
        (void)console_print("low-power ");
    }
    (void)console_println("");

    __HAL_RCC_CLEAR_RESET_FLAGS();
}

static void print_banner(void)
{
    (void)console_println("");
    (void)console_println("==================================================");
    (void)console_println(" STM32F411 Blackpill -- component bring-up");
    (void)console_println("==================================================");
    (void)console_printf("  Built: %s %s\r\n", __DATE__, __TIME__);
    (void)console_printf("  SYSCLK: %lu Hz   HCLK: %lu Hz\r\n",
                         (unsigned long)HAL_RCC_GetSysClockFreq(),
                         (unsigned long)HAL_RCC_GetHCLKFreq());
    (void)console_printf("  PCLK1: %lu Hz    PCLK2: %lu Hz\r\n",
                         (unsigned long)HAL_RCC_GetPCLK1Freq(),
                         (unsigned long)HAL_RCC_GetPCLK2Freq());

    if (HAL_RCC_GetSysClockFreq() != 96000000u) {
        (void)console_println("  NOTE: SYSCLK is not the expected 96 MHz. Every timing");
        (void)console_println("  figure below is derived from it, so fix this first.");
    }

    (void)console_printf("  DWT cycle counter: %s\r\n",
                         bsp_dwt_ok() ? "running (microsecond timing is good)"
                                      : "NOT RUNNING -- 1-Wire will fail");
    if (!s_i2c_bus_free) {
        (void)console_println("  I2C bus: STUCK at boot. SCL or SDA is being held low.");
        (void)console_println("  Check the 4.7k pull-ups to 3V3 first, then unplug the");
        (void)console_println("  slaves one at a time. The scan below will find nothing");
        (void)console_println("  until this is fixed.");
    }
    print_reset_cause();
    (void)console_println("");
}

static void print_pinout(void)
{
    (void)console_println("-- Wiring ----------------------------------------");
    (void)console_println("  PC13  onboard LED        (active LOW, on-board)");
    (void)console_println("  PB0   buzzer             TIM3_CH3 PWM");
    (void)console_println("  PB1   relay IN           (active LOW typical)");
    (void)console_println("  PB6   I2C SCL            4.7k pull-up to 3V3");
    (void)console_println("  PB7   I2C SDA            4.7k pull-up to 3V3");
    (void)console_println("  PB5   DS18B20 DQ         4.7k pull-up to 3V3");
    (void)console_println("  PB12  vibration DO       internal pull-up");
    (void)console_println("  PA1   analog in          3V3 MAXIMUM");
    (void)console_println("  PA2   USART2 TX  ->      ESP32 RX");
    (void)console_println("  PA3   USART2 RX  <-      ESP32 TX");
    (void)console_println("  PA11/PA12  USB D-/D+     this console");
    (void)console_println("");
    (void)console_println("  Every sensor needs GND commoned with the board.");
    (void)console_println("  Change any pin in Application/Inc/bringup_config.h.");
    (void)console_println("");
}

/* ================================================================== */
/*  Menu                                                              */
/* ================================================================== */

static void print_menu(void)
{
    (void)console_println("");
    (void)console_println("-- Menu ------------------------------------------");
#if BRINGUP_TEST_GPIO
    (void)console_println("  1  outputs: LED, buzzer, relay");
#endif
#if BRINGUP_TEST_I2C_SCAN
    (void)console_println("  2  I2C bus scan");
#endif
#if BRINGUP_TEST_OLED
    (void)console_println("  3  OLED display");
#endif
#if BRINGUP_TEST_DS18B20
    (void)console_println("  4  DS18B20 temperature");
#endif
#if BRINGUP_TEST_ADC
    (void)console_println("  5  analog input / current sensor");
#endif
#if BRINGUP_TEST_VIBRATION
    (void)console_println("  6  vibration switch");
#endif
#if BRINGUP_TEST_ESP32_LINK
    (void)console_println("  7  ESP32 UART link");
#endif
    (void)console_println("  a  run everything in order");
    (void)console_println("  p  show the wiring table");
    (void)console_println("  h  redisplay this menu");
    (void)console_println("");
}

/** Run one test, printing a framed verdict around it. */
static test_result_t run_one(const char *name, test_result_t (*fn)(void))
{
    (void)console_println("");
    test_result_t r = fn();
    (void)console_printf(">> %s: %s\r\n", name, test_result_str(r));
    return r;
}

static void run_all(void)
{
    int pass = 0, fail = 0, other = 0;

    (void)console_println("");
    (void)console_println("Running every test in order. Each one still asks for");
    (void)console_println("confirmation where it cannot measure the answer itself.");

    struct {
        const char *name;
        test_result_t (*fn)(void);
    } const suite[] = {
        { "outputs",   test_gpio_run       },
        { "I2C scan",  test_i2c_scan_run   },
        { "OLED",      test_oled_run       },
        { "DS18B20",   test_ds18b20_run    },
        { "ADC",       test_adc_run        },
        { "vibration", test_vibration_run  },
        { "ESP32 link", test_esp32_link_run },
    };

    for (unsigned i = 0; i < (sizeof(suite) / sizeof(suite[0])); i++) {
        switch (run_one(suite[i].name, suite[i].fn)) {
            case TEST_PASS: pass++;  break;
            case TEST_FAIL: fail++;  break;
            default:        other++; break;
        }
    }

    (void)console_println("");
    (void)console_println("== Summary =======================================");
    (void)console_printf("  passed: %d   failed: %d   skipped/inconclusive: %d\r\n",
                         pass, fail, other);
    if (fail == 0 && other == 0) {
        (void)console_println("  Every component answered. The board is ready.");
    } else if (fail == 0) {
        (void)console_println("  Nothing failed outright, but not everything was proven.");
    } else {
        (void)console_println("  Fix the failures before building on this board -- each");
        (void)console_println("  test printed its own likely cause above.");
    }
    (void)console_println("==================================================");
}

/* ================================================================== */
/*  Task                                                              */
/* ================================================================== */

static void bringup_task(void *argument)
{
    (void)argument;

    /* Wait for enumeration. LEDTask calls MX_USB_DEVICE_Init(), then the
     * host needs a moment; printing before that faults. Give up after
     * 10 s and carry on anyway -- writes fail safely, and if the board is
     * running standalone the tests are still worth executing.
     *
     * The pins are already safe by this point: bsp_init() ran before the
     * scheduler started, so nothing is floating while this waits. */
    for (int i = 0; i < 200 && !console_is_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* A further settle: the port exists before a terminal has attached,
     * and output sent in that gap is simply lost. */
    vTaskDelay(pdMS_TO_TICKS(500));

    print_banner();
    print_pinout();

#if BRINGUP_TEST_I2C_SCAN
    /* Free, non-destructive, and wanted every time. */
    (void)run_one("I2C scan", test_i2c_scan_run);
#endif

    print_menu();

    for (;;) {
        char line[CONSOLE_LINE_MAX];

        (void)console_print("bringup> ");
        /* A finite timeout rather than waiting forever. If the terminal
         * was attached late, or reattached after a replug, the prompt is
         * long gone from its scrollback -- reprinting it every 30 s means
         * the tool never looks dead. */
        if (!console_readline(line, sizeof(line), 30000u)) {
            (void)console_println("");
            continue;
        }
        if (line[0] == '\0') {
            continue;                       /* bare Enter */
        }

        switch (line[0]) {
#if BRINGUP_TEST_GPIO
            case '1': (void)run_one("outputs",    test_gpio_run);      break;
#endif
#if BRINGUP_TEST_I2C_SCAN
            case '2': (void)run_one("I2C scan",   test_i2c_scan_run);  break;
#endif
#if BRINGUP_TEST_OLED
            case '3': (void)run_one("OLED",       test_oled_run);      break;
#endif
#if BRINGUP_TEST_DS18B20
            case '4': (void)run_one("DS18B20",    test_ds18b20_run);   break;
#endif
#if BRINGUP_TEST_ADC
            case '5': (void)run_one("ADC",        test_adc_run);       break;
#endif
#if BRINGUP_TEST_VIBRATION
            case '6': (void)run_one("vibration",  test_vibration_run); break;
#endif
#if BRINGUP_TEST_ESP32_LINK
            case '7': (void)run_one("ESP32 link", test_esp32_link_run); break;
#endif
            case 'a': case 'A': run_all();      break;
            case 'p': case 'P': print_pinout(); break;
            case 'h': case 'H': print_menu();   break;
            default:
                (void)console_printf("Unknown option '%c'. Press h for the menu.\r\n",
                                     line[0]);
                break;
        }
    }
}

void bringup_app_init(void)
{
    /* Pins first. See the file header: this is the earliest point at which
     * the relay and buzzer stop being floating inputs, and it is reached
     * before osKernelStart(), so there is no window at all. */
    bsp_init();

    /* Clear a wedged bus before anything tries to use it. The verdict is
     * kept for the banner rather than printed here -- there is no console
     * yet, and no scheduler to run one. */
    s_i2c_bus_free = bsp_i2c_recover();

    /* The console must exist before the task can print, and before the
     * USB receive interrupt can push into its ring buffer. */
    console_init();

    s_bringup_task = osThreadNew(bringup_task, NULL, &bringup_task_attr);
    (void)s_bringup_task;
}
