/**
 * usb_serial.c — Wrapper around CubeMX-generated USB CDC.
 *
 * CubeMX generates usbd_cdc_if.c with the function:
 *   uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len)
 *
 * We wrap it here so application code never calls CDC directly.
 */
#include "usb_serial.h"
#include "usbd_cdc_if.h"   /* CubeMX-generated — provides CDC_Transmit_FS */
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

void usb_serial_init(void) {
    /* USB init is done by CubeMX-generated MX_USB_DEVICE_Init() in main.c.
     * Nothing to do here. This function exists for API symmetry. */
}

bool usb_serial_write(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0)
        return false;

    /*
     * CDC_Transmit_FS() can return:
     *   USBD_OK   -> transmission accepted
     *   USBD_BUSY -> previous USB transfer still active
     *
     * Wait for the USB CDC endpoint to become available.
     */
    for (int retry = 0; retry < 500; retry++)
    {
        uint8_t res = CDC_Transmit_FS((uint8_t *)data, (uint16_t)len);

        if (res == USBD_OK)
            return true;

        /*
         * Give the USB stack/RTOS some time rather than
         * burning the CPU in a tight loop.
         */
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return false;
}

bool usb_serial_print(const char *str) {
    if (str == NULL) return false;
    return usb_serial_write((const uint8_t *)str, (uint32_t)strlen(str));
}
