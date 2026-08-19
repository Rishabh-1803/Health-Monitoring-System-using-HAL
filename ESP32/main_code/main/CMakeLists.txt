# main/CMakeLists.txt — ESP-IDF main component
#
# Lists all source files in the main component. ESP-IDF scans
# recursively for .c files in main/, but explicit listing is
# clearer and matches our layered architecture.

idf_component_register(
    SRCS
        "main.c"
        "Tasks/task_wifi.c"
        "Tasks/task_webserver.c"
        "Tasks/task_dashboard.c"
        "Tasks/task_uart_rx.c"
        "Tasks/task_uart_tx.c"
        "Tasks/task_logger.c"
        "Tasks/task_cli.c"
        "Tasks/task_diagnostics.c"
        "Tasks/task_watchdog.c"
        "Services/wifi_manager.c"
        "Services/http_server.c"
        "Services/websocket_server.c"
        "Services/dashboard_data.c"
        "Services/command_dispatcher.c"
        "Services/littlefs_storage.c"
        "Services/system_stats.c"
        "Drivers/uart_link.c"
        "Drivers/cli.c"
        "Drivers/led.c"
        "Protocol/packet.c"
        "Protocol/crc16.c"
    INCLUDE_DIRS
        "."
        "Tasks"
        "Services"
        "Drivers"
        "Protocol"
)
