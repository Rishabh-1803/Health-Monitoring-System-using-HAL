# main/CMakeLists.txt — ESP-IDF main component
# Phase 2B — Industrial Monitor ESP32-S3

idf_component_register(
    SRCS
        "main.c"
        "Drivers/uart_link.c"
        "Tasks/task_uart_tx.c"
        "Tasks/task_uart_rx.c"
        "Protocol/crc16.c"
        "Protocol/packet.c"
        "Protocol/protocol_selftest.c"
    INCLUDE_DIRS
        "."
        "Drivers"
        "Tasks"
        "Protocol"
)
