/**
 * @file    uart_link.c
 * @brief   USART2 driver: interrupt RX into ring buffer + blocking TX.
 *
 * The ring buffer is 256 bytes — enough for ~3 full-size (136-byte) packets
 * in burst. At 460800 baud, the buffer fills in ~5.6ms if not drained, which
 * is far longer than any FreeRTOS tick (1ms) so the consumer task will always
 * get a chance to run.
 *
 * ISR safety: head is written ONLY in HAL_UART_RxCpltCallback (ISR context).
 * tail is written ONLY in uart_link_get_byte (task context). No locks needed.
 */

#include "uart_link.h"
#include "bringup_config.h"
#include "usart.h"
#include "stm32f4xx_hal.h"

#define RX_BUF_SIZE 256u   /* Power of 2 for cheap modulo */

static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static uint8_t  s_rx_byte;   /* Single-byte scratch for HAL_UART_Receive_IT */

void uart_link_init(void)
{
    s_rx_head = 0;
    s_rx_tail = 0;
    /* Arm the first receive. The callback re-arms after each byte. */
    (void)HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1u);
}

bool uart_link_send(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u) return false;
    return HAL_UART_Transmit(&huart2, (uint8_t *)data, (uint16_t)len, 100u) == HAL_OK;
}

bool uart_link_get_byte(uint8_t *out)
{
    if (s_rx_head == s_rx_tail) {
        return false;   /* empty */
    }
    *out = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & (RX_BUF_SIZE - 1u));
    return true;
}

uint32_t uart_link_bytes_available(void)
{
    int32_t diff = (int32_t)s_rx_head - (int32_t)s_rx_tail;
    if (diff < 0) diff += RX_BUF_SIZE;
    return (uint32_t)diff;
}

/**
 * HAL callback: one byte received via interrupt. Store it and re-arm.
 *
 * This is a WEAK override of the HAL default. Only ONE definition of this
 * function may exist in the entire project — so test_esp32.c (which uses
 * blocking HAL_UART_Receive, not interrupt) does NOT define it.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uint16_t next = (uint16_t)((s_rx_head + 1u) & (RX_BUF_SIZE - 1u));
        if (next != s_rx_tail) {
            s_rx_buf[s_rx_head] = s_rx_byte;
            s_rx_head = next;
        }
        /* If next == tail, buffer is full — drop the byte (best effort). */
        /* Re-arm the next receive regardless. */
        (void)HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1u);
    }
}
