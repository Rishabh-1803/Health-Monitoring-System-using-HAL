# ============================================================
# main/CMakeLists.txt
# ============================================================
# Registers the main component and lists all source files.
# ESP-IDF scans for main/ automatically — this file tells it
# which .c files to compile and which headers to expose.
# ============================================================

idf_component_register(
    SRCS
        "main.c"
        "task_sensor.c"
        "task_filter.c"
        "task_display.c"
        "task_logger.c"
        "task_stats.c"
        "shared_config.c"
        "alarm_event.c"
        "log_queue.c"
    INCLUDE_DIRS
        "."
)
