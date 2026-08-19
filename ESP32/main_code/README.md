# ESP32-S3 Industrial Monitor — Project Skeleton

This is the **ESP-IDF project skeleton** for the Industrial Equipment Health Monitoring project. Unlike the STM32 skeleton (which requires CubeMX generation), this is a **complete, buildable ESP-IDF project** that you can flash to your ESP32-S3 right now.

**Phase 1 status:** Skeleton only. `main.c` boots, prints a banner, and exits. All tasks/services/drivers are stubs with TODO comments indicating which phase will implement them.

---

## What This Skeleton Provides

- ✅ Project structure following the layered architecture from `docs/ARCHITECTURE.md`
- ✅ `CMakeLists.txt` configured for ESP32-S3
- ✅ `sdkconfig.defaults` with FreeRTOS trace facilities enabled
- ✅ `partitions.csv` with reserved space for LittleFS log storage
- ✅ Empty stub files for all 9 tasks, 7 services, 3 drivers, 3 protocol files
- ✅ Compiles and flashes — prints "Phase 1 skeleton ready" on boot

## What This Skeleton Does NOT Provide (Yet)

- ❌ WiFi connection (Phase 7)
- ❌ Web server (Phase 8)
- ❌ UART driver to STM32 (Phase 2)
- ❌ CLI parser (Phase 8)
- ❌ WebSocket dashboard push (Phase 8)

---

## Folder Layout

```
esp32-skeleton/
├── CMakeLists.txt             Top-level ESP-IDF CMake
├── sdkconfig.defaults         FreeRTOS + logging config
├── partitions.csv             Flash partition table (NVS + factory + LittleFS)
├── README.md                  This file
└── main/
    ├── CMakeLists.txt         Component CMake
    ├── main.c                 app_main() entry point — Phase 1 stub
    ├── project_config.h       FreeRTOS priorities, stack sizes, queue sizes
    ├── Tasks/                 9 FreeRTOS task stubs
    │   ├── task_wifi.{c,h}
    │   ├── task_webserver.{c,h}
    │   ├── task_dashboard.{c,h}
    │   ├── task_uart_rx.{c,h}
    │   ├── task_uart_tx.{c,h}
    │   ├── task_logger.{c,h}
    │   ├── task_cli.{c,h}
    │   ├── task_diagnostics.{c,h}
    │   └── task_watchdog.{c,h}
    ├── Services/              7 service stubs
    │   ├── wifi_manager.{c,h}
    │   ├── http_server.{c,h}
    │   ├── websocket_server.{c,h}
    │   ├── dashboard_data.{c,h}
    │   ├── command_dispatcher.{c,h}
    │   ├── littlefs_storage.{c,h}
    │   └── system_stats.{c,h}
    ├── Drivers/               3 driver stubs
    │   ├── uart_link.{c,h}     UART1 to STM32 (460800 8N1)
    │   ├── cli.{c,h}           Command-line parser
    │   └── led.{c,h}           Onboard RGB LED (if available)
    ├── Protocol/              Same as STM32 side (shared code)
    │   ├── packet.{c,h}
    │   ├── crc16.{c,h}
    │   └── protocol_types.h
    └── Web/                   Dashboard static files (Phase 8)
        ├── index.html         Placeholder
        ├── style.css          Placeholder
        └── app.js             Placeholder
```

---

## How to Use

### Step 1 — Copy to Your ESP-IDF Workspace

Move this folder to your ESP-IDF projects directory, e.g.:
```
C:\esp\projects\industrial-monitor-esp32\
```

### Step 2 — Open in VS Code

File → Open Folder → select `industrial-monitor-esp32`

### Step 3 — Set Target, Build, Flash

In ESP-IDF Terminal:
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```
(Replace `COMx` with your ESP32-S3's COM port)

### Step 4 — Verify

You should see on the monitor:
```
I (xxx) APP_MAIN: ========================================
I (xxx) APP_MAIN:   Industrial Monitor ESP32-S3
I (xxx) APP_MAIN:   Phase 1 — Skeleton Ready
I (xxx) APP_MAIN: ========================================
I (xxx) APP_MAIN: See docs/ARCHITECTURE.md and docs/UART_PROTOCOL_SPEC.md
I (xxx) APP_MAIN: Phase 2 will implement the UART link to STM32
```

If you see this, your ESP-IDF toolchain is good and we can move to Phase 2.

---

## Phase 2 Build Order

When Phase 2 starts, we'll fill in this order (mirroring the STM32 side):

1. `Protocol/crc16.c` + `Protocol/packet.c` + `Protocol/protocol_types.h` — protocol implementation
2. `Drivers/uart_link.c` — UART1 driver with ring buffer (ESP-IDF UART driver API)
3. `Tasks/task_uart_rx.c` + `Tasks/task_uart_tx.c` — packet exchange + heartbeat
4. `Tasks/task_logger.c` — log queue + single-writer to USB-CDC JTAG
5. `Tasks/task_watchdog.c` — smart watchdog
6. **END-TO-END TEST**: STM32 ↔ ESP32 heartbeat exchange working

Once Phase 2 is done, the two MCUs talk. Phases 3+ add sensors, web, CLI, dashboard on top.
