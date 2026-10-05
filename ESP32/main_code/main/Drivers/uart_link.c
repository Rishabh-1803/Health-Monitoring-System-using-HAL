/**
 * @file    uart_link.c
 * @brief   ESP-IDF UART1 driver for the STM32 ↔ ESP32 link.
 */

#include "uart_link.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "UART_LINK";

#define UART_NUM_USED       UART_NUM_1
#define UART_TX_PIN         17
#define UART_RX_PIN         18
#define UART_BAUD_RATE      460800
#define UART_BUF_SIZE       512
#define UART_QUEUE_SIZE     20

/* TX serialiser: the heartbeat task and the command dispatcher both
 * transmit. The ESP-IDF driver makes single uart_write_bytes() calls
 * atomic, but whole-packet atomicity across tasks is guaranteed here
 * rather than hoped for. RX is single-consumer (task_uart_rx). */
static SemaphoreHandle_t s_tx_lock;

void uart_link_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_USED,
                                         UART_BUF_SIZE,
                                         UART_BUF_SIZE,
                                         UART_QUEUE_SIZE,
                                         NULL,
                                         0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_USED, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_USED,
                                  UART_TX_PIN,
                                  UART_RX_PIN,
                                  UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE));

    s_tx_lock = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "UART1 initialised: %d baud, TX=GPIO%d, RX=GPIO%d",
             UART_BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
}

bool uart_link_send(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u) return false;
    if (s_tx_lock != NULL) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    int written = uart_write_bytes(UART_NUM_USED, (const char *)data, len);
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
    return (written == (int)len);
}

bool uart_link_get_byte(uint8_t *out, TickType_t timeout)
{
    if (out == NULL) return false;
    int n = uart_read_bytes(UART_NUM_USED, out, 1, timeout);
    return (n == 1);
}

uint32_t uart_link_bytes_available(void)
{
    size_t waiting = 0;
    uart_get_buffered_data_len(UART_NUM_USED, &waiting);
    return (uint32_t)waiting;
}
