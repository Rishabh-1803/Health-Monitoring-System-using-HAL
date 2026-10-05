/**
 * @file    cli.c
 * @brief   REPL implementation (esp_console + linenoise, no argtable).
 *
 * Argument parsing is deliberately sscanf-based rather than argtable3:
 * the command set is small and fixed, and keeping the parser local
 * means zero extra components on the build.
 *
 * The REPL task is NOT subscribed to the task watchdog: a user may
 * legitimately leave a prompt open for minutes. Supervision of the
 * real-time tasks lives in task_watchdog (Phase 7).
 */

#include "cli.h"
#include "dashboard_data.h"
#include "command_dispatcher.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "websocket_server.h"
#include "task_uart_rx.h"
#include "task_uart_tx.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "linenoise.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Interactive STM32 command budget: longer than the HTTP path. */
#define CLI_CMD_TIMEOUT_MS  1500

/* ================================================================== */
/*  Formatting helpers                                                */
/* ================================================================== */

static const char *thr_name(uint8_t id)
{
    switch (id) {
        case 0:  return "temp";
        case 1:  return "current";
        default: return "vib";
    }
}

static const char *alm_names(uint8_t bits)
{
    static char buf[80];
    buf[0] = '\0';
    if (bits == 0u) {
        return "none";
    }
    if (bits & ALARM_BIT_OVERTEMP)    { strcat(buf, "overtemp "); }
    if (bits & ALARM_BIT_OVERCURRENT) { strcat(buf, "overcurrent "); }
    if (bits & ALARM_BIT_VIBRATION)   { strcat(buf, "vibration "); }
    if (bits & ALARM_BIT_SENSOR_FAIL) { strcat(buf, "sensor-fail "); }
    if (bits & ALARM_BIT_COMM_FAIL)   { strcat(buf, "comm-fail "); }
    return buf;
}

static void print_cmd_result(cmd_result_t r)
{
    switch (r) {
        case CMD_OK:     printf("OK (acked by STM32)\n"); break;
        case CMD_NAKED:  printf("REJECTED by STM32 (bad value?)\n"); break;
        case CMD_NO_ACK: printf("NO ACK from STM32 (link down?)\n"); break;
        default:         printf("LOCAL ERROR (encode/send)\n"); break;
    }
}

/* ================================================================== */
/*  Commands: inspection                                              */
/* ================================================================== */

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    dash_snapshot_t s;
    dashboard_data_snapshot(&s);

    char ip[16], ssid[33];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_ssid(ssid, sizeof(ssid));

    printf("== STM32 node ==\n");
    printf("  link:        %s\n",
           s.link_up ? "UP" : "DOWN (comm-fail)");
    if (s.ever_linked) {
        printf("  telemetry:   %lu ms old\n",
               (unsigned long)s.telemetry_age_ms);
        printf("  temp/cur/vib: %.2f C / %.3f A / %.2f g (est.)\n",
               (double)s.temp_c, (double)s.current_a, (double)s.vib_g);
    } else {
        printf("  telemetry:   never received\n");
    }
    printf("  alarms:      0x%02X (%s)\n",
           (unsigned)s.alarm_bits, alm_names(s.alarm_bits));
    printf("  sensor:      %s\n",
           (s.sensor_status & SENSOR_BIT_DS18B20_OK) ? "DS18B20 ok"
                                                    : "DS18B20 FAIL");
    printf("  cpu/heap/up: %.1f%% / %lu B / %u s\n",
           (double)s.stm32_cpu_10000 / 100.0,
           (unsigned long)s.stm32_heap, (unsigned)s.stm32_uptime_s);
    printf("  thresholds:  temp %.1f C  cur %.2f A  vib %.2f g\n",
           (double)s.thr[0], (double)s.thr[1], (double)s.thr[2]);
    printf("  sample rate: %u ms\n", (unsigned)s.sample_period_ms);

    printf("== ESP32 ==\n");
    printf("  wifi:        %s  ssid=%s  ip=%s\n",
           wifi_manager_state_str(wifi_manager_get_state()), ssid, ip);
    printf("  heap:        %lu / min %lu bytes\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    printf("  httpd:       %s  ws clients: %d\n",
           http_server_running() ? "up" : "down",
           websocket_server_client_count());
    return 0;
}

static int cmd_link(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t rx, crc, alarms, unknown;
    task_uart_rx_get_stats(&rx, &crc, &alarms, &unknown);
    uint32_t tx = task_uart_tx_get_packets();
    uint32_t retries = 0, timeouts = 0;
    command_dispatcher_get_stats(&retries, &timeouts);
    printf("packets rx/tx:  %lu / %lu\n",
           (unsigned long)rx, (unsigned long)tx);
    printf("crc errors:     %lu   framing/unknown: %lu\n",
           (unsigned long)crc, (unsigned long)unknown);
    printf("alarm packets:  %lu\n", (unsigned long)alarms);
    printf("cmd retries:    %lu   cmd timeouts: %lu\n",
           (unsigned long)retries, (unsigned long)timeouts);
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("free: %lu   min since boot: %lu\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("up %lu ms\n", (unsigned long)dashboard_data_uptime_ms());
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* system_stats (Phase 7) adds watermarks + CPU; the REPL version
     * lists the core facts via the FreeRTOS CLI hook when enabled. */
    printf("task introspection arrives with system_stats (Phase 7).\n");
    printf("use `status` for link + heap, `link` for counters.\n");
    return 0;
}

static int cmd_hist(int argc, char **argv)
{
    int n = 10;
    if (argc > 1) {
        n = atoi(argv[1]);
    }
    if (n < 1)  { n = 1; }
    if (n > 100) { n = 100; }

    hr_sample_t *buf = malloc(sizeof(hr_sample_t) * (uint16_t)n);
    if (buf == NULL) {
        printf("oom\n");
        return 1;
    }
    uint32_t since = 0;
    uint16_t count = dashboard_data_copy_history_since(since, buf, (uint16_t)n);
    if (count == 0u) {
        printf("no samples yet\n");
        free(buf);
        return 0;
    }
    uint16_t first = (count > (uint16_t)n) ? (uint16_t)(count - n) : 0;
    printf("    t(s)     temp     curr     vib\n");
    for (uint16_t i = first; i < count; i++) {
        printf("  %6lu   %6.2f   %6.3f   %6.2f\n",
               (unsigned long)(buf[i].t_ms / 1000u),
               (double)buf[i].v[0], (double)buf[i].v[1], (double)buf[i].v[2]);
    }
    free(buf);
    return 0;
}

/* ================================================================== */
/*  Commands: STM32 control (via the dispatcher)                      */
/* ================================================================== */

static int cmd_thr(int argc, char **argv)
{
    if (argc == 1) {
        for (uint8_t id = 0; id < 3u; id++) {
            printf("%-8s %.2f\n", thr_name(id),
                   (double)dashboard_data_get_threshold(id));
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        uint8_t id;
        if (strcmp(argv[2], "t") == 0)      { id = 0; }
        else if (strcmp(argv[2], "c") == 0) { id = 1; }
        else if (strcmp(argv[2], "v") == 0) { id = 2; }
        else {
            printf("id must be t, c or v\n");
            return 1;
        }
        float value = strtof(argv[3], NULL);
        printf("setting %s to %.2f ... ", thr_name(id), (double)value);
        cmd_result_t r = command_set_threshold(id, value, CLI_CMD_TIMEOUT_MS);
        print_cmd_result(r);
        if (r == CMD_OK) {
            dashboard_data_note_threshold(id, value);
        }
        return (r == CMD_OK) ? 0 : 1;
    }
    printf("usage: thr           show thresholds\n");
    printf("       thr set <t|c|v> <value>\n");
    return 1;
}

static int cmd_rate(int argc, char **argv)
{
    if (argc == 2) {
        int ms = atoi(argv[1]);
        if (ms < 50 || ms > 1000) {
            printf("ms must be 50..1000\n");
            return 1;
        }
        printf("setting sample period to %d ms ... ", ms);
        cmd_result_t r = command_set_sample_rate((uint16_t)ms,
                                                 CLI_CMD_TIMEOUT_MS);
        print_cmd_result(r);
        if (r == CMD_OK) {
            dashboard_data_note_sample_rate((uint16_t)ms);
        }
        return (r == CMD_OK) ? 0 : 1;
    }
    printf("usage: rate <50..1000>   (current mirror: %u ms)\n",
           (unsigned)dashboard_data_get_sample_rate());
    return 1;
}

static int cmd_led(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: led <g|r|all> <on|off>\n");
        return 1;
    }
    uint8_t id;
    if (strcmp(argv[1], "g") == 0)      { id = LED_ID_GREEN; }
    else if (strcmp(argv[1], "r") == 0) { id = LED_ID_RED; }
    else if (strcmp(argv[1], "all") == 0) { id = LED_ID_ALL; }
    else {
        printf("id must be g, r or all\n");
        return 1;
    }
    bool on;
    if (strcmp(argv[2], "on") == 0)       { on = true; }
    else if (strcmp(argv[2], "off") == 0) { on = false; }
    else {
        printf("state must be on or off\n");
        return 1;
    }
    printf("led %s %s ... ", argv[1], argv[2]);
    cmd_result_t r = command_led(id, on, CLI_CMD_TIMEOUT_MS);
    print_cmd_result(r);
    return (r == CMD_OK) ? 0 : 1;
}

static int cmd_alarm_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("resetting alarms ... ");
    print_cmd_result(command_alarm_reset(CLI_CMD_TIMEOUT_MS));
    return 0;
}

static int cmd_reboot_stm32(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting STM32 ... ");
    print_cmd_result(command_reboot_stm32(CLI_CMD_TIMEOUT_MS));
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting ESP32 in 200 ms\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;   /* not reached */
}

/* ================================================================== */
/*  Commands: wifi                                                    */
/* ================================================================== */

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        char ip[16], ssid[33];
        wifi_manager_get_ip(ip, sizeof(ip));
        wifi_manager_get_ssid(ssid, sizeof(ssid));
        printf("state: %s\n", wifi_manager_state_str(wifi_manager_get_state()));
        printf("ssid:  %s\n", ssid);
        printf("ip:    %s\n", ip);
        printf("rssi:  %d dBm\n", wifi_manager_get_rssi());
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        /* wifi set <ssid> <pass> — reboots into station mode */
        printf("saving credentials and rebooting...\n");
        (void)wifi_manager_save_credentials(argv[2], argv[3]);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        printf("erasing credentials and rebooting into provisioning...\n");
        (void)wifi_manager_clear_credentials();
        return 0;
    }
    printf("usage: wifi\n");
    printf("       wifi set <ssid> <pass>\n");
    printf("       wifi clear\n");
    return 1;
}

/* ================================================================== */
/*  Registration + REPL                                               */
/* ================================================================== */

static void reg(const char *cmd, const char *help, const char *hint,
                esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = {
        .command = cmd, .help = help, .hint = hint, .func = fn,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "monitor> ";
    repl_cfg.max_history_len = 24;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    reg("status",       "snapshot: STM32 telemetry + ESP32 facts", NULL, cmd_status);
    reg("link",         "UART link counters (rx/tx/crc/retries)",   NULL, cmd_link);
    reg("heap",         "free + minimum heap",                      NULL, cmd_heap);
    reg("uptime",       "milliseconds since boot",                  NULL, cmd_uptime);
    reg("tasks",        "task introspection (Phase 7)",             NULL, cmd_tasks);
    reg("hist",         "last N history samples", "<n=10>",         cmd_hist);
    reg("thr",          "show or set alarm thresholds", "[set t|c|v VALUE]", cmd_thr);
    reg("rate",         "set STM32 sample period (ms)", "<50..1000>", cmd_rate);
    reg("led",          "control STM32 outputs", "<g|r|all> <on|off>", cmd_led);
    reg("alarm-reset",  "acknowledge + clear STM32 alarms",         NULL, cmd_alarm_reset);
    reg("reboot-stm32", "hard-reset the STM32 node",                NULL, cmd_reboot_stm32);
    reg("reboot",       "reboot the ESP32",                         NULL, cmd_reboot);
    reg("wifi",         "show / set / clear wifi credentials",      "[set SSID PASS | clear]", cmd_wifi);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI("CLI", "REPL started — type help for the command list");
}
