# main/CMakeLists.txt — ESP-IDF main component
# Industrial Monitor ESP32-S3

idf_component_register(
    SRCS
        "main.c"
        "Drivers/uart_link.c"
        "Tasks/task_uart_tx.c"
        "Tasks/task_uart_rx.c"
        "Protocol/crc16.c"
        "Protocol/packet.c"
        "Protocol/protocol_selftest.c"
        "Services/history_ring.c"
        "Services/json_util.c"
        "Services/dashboard_data.c"
        "Services/command_dispatcher.c"
    INCLUDE_DIRS
        "."
        "Drivers"
        "Tasks"
        "Protocol"
        "Services"
)
