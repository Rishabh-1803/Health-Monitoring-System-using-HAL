# FreeRTOS Warmup Project

A teaching project for ESP32-S3 + ESP-IDF that demonstrates the core
FreeRTOS patterns you'll need for the Industrial Equipment Health
Monitoring project.

## What This Project Teaches

| Pattern | Where | Why It Matters |
|---------|-------|----------------|
| Periodic scheduling with `vTaskDelayUntil()` | task_sensor.c | Maintains exact sample rate regardless of work time |
| Sending structs through queues | main.c, task_sensor.c, task_filter.c | Type-safe, atomic data passing between tasks |
| Mutex for shared state | shared_config.c | Protects thresholds/config from race conditions |
| Event groups for alarms | alarm_event.c | One-to-many broadcast of alarm signals |
| Logger queue pattern | log_queue.c, task_logger.c | Single-writer for serial — no interleaved output |
| Blocking-on-receive consumer | task_filter.c | Zero CPU usage while waiting for data |
| Stack high-water monitoring | task_stats.c | Catch stack overflow BEFORE it crashes the system |

## Project Structure

```
freertos-warmup/
├── CMakeLists.txt              # Top-level ESP-IDF project file
├── sdkconfig.defaults          # Default config (enables FreeRTOS stats)
└── main/
    ├── CMakeLists.txt          # Lists source files for build
    ├── main.c                  # Boot sequence — inits everything, starts tasks
    ├── project_config.h        # Central config: priorities, stacks, queue sizes
    ├── sensor_data.h           # Structs passed through queues
    ├── shared_config.h / .c    # Mutex-protected shared config (thresholds, etc.)
    ├── alarm_event.h / .c      # Event group wrapper for alarm bits
    ├── log_queue.h / .c        # Queue + log_send() API — single logger access
    ├── task_sensor.c           # Generates fake sensor data at 10 Hz
    ├── task_filter.c           # 5-sample moving average filter
    ├── task_display.c          # Logs filtered samples, reacts to alarm transitions
    ├── task_logger.c           # The ONLY task that touches serial output
    └── task_stats.c            # Prints diagnostics every 5 seconds
```

## Data Flow

```
[task_sensor] --q_sensor--> [task_filter] --q_display--> [task_display]
  10 Hz                       moving avg                    |
  vTaskDelayUntil                                           v
  reads shared_config                            (logs via log_queue)
  sets alarm_event bits                                     |
                                                            v
                                          --q_logger--> [task_logger] --> UART

[task_stats] (every 5s: reads all task/queue states, logs via log_queue)
```

## How to Build & Run

### 1. Copy this folder into your ESP-IDF workspace

Place it somewhere convenient, e.g.:
```
C:\esp\projects\freertos-warmup\
```

### 2. Open in VS Code with ESP-IDF extension

- File → Open Folder → select `freertos-warmup`
- ESP-IDF: Set Espressif Device Target → esp32s3

### 3. Configure, build, flash, monitor

From the ESP-IDF Terminal:
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor    # Replace COMx with your port
```

Or use the VS Code ESP-IDF extension buttons:
- ⚙️ Gear icon → Build
- ⚡ Lightning icon → Flash
- 📺 Monitor icon → Monitor

### 4. What you should see

After boot, you'll see:

1. Boot messages from each task starting up
2. Every 100ms: a filtered sensor sample log
3. Occasionally: "ALARM RAISED" messages when injected faults occur
4. Every 5 seconds: a diagnostics report showing:
   - Free heap (and minimum ever)
   - Stack high-water marks for each task (in words + bytes)
   - Queue depths (current / capacity)
   - Task names

Example output:
```
[    10] [SENSOR ] [INFO] TaskSensor started
[    12] [FILTER ] [INFO] TaskFilter started
[    14] [DISPLAY] [INFO] TaskDisplay started
[    16] [LOGGER ] [INFO] TaskLogger started — owns serial output
[    18] [STATS  ] [INFO] TaskStats started
[   110] [DISPLAY] [INFO] seq=1  T=33.5°C  I=2.85A  V=1.21g  (n=1)
[   210] [DISPLAY] [INFO] seq=2  T=34.1°C  I=2.92A  V=1.18g  (n=2)
...
[  5010] [STATS  ] [INFO] ==== Diagnostics Report #1 (uptime=5010 ms) ====
[  5010] [STATS  ] [INFO]   free_heap=234567 bytes  min_ever=233102 bytes
[  5010] [STATS  ] [INFO]   sensor     stack_free=1840 words (7360 bytes)
[  5010] [STATS  ] [INFO]   filter     stack_free=1900 words (7600 bytes)
...
```

## Exercises to Try (HIGHLY Recommended)

Once it's running, do these exercises — they'll deepen your understanding:

### Exercise 1: Reduce a stack size and watch it fail
Change `TASK_SENSOR_STACK` from 2048 to 256 in project_config.h. Build, flash,
watch the crash. Then restore.

### Exercise 2: Cause a queue overflow
Change `LOGGER_QUEUE_LEN` to 2 in project_config.h. The logger can't keep up,
queue fills, sensor samples get dropped. Watch for "Queue full, dropping sample"
warnings.

### Exercise 3: Verify periodic timing
Add a `log_send` at the START of `task_sensor` that prints `xTaskGetTickCount()`.
You should see ticks increment by exactly 100 (10 Hz) regardless of work time.

### Exercise 4: Add a config change at runtime
In `task_sensor`, every 100 samples, double the temperature threshold via
`shared_config_set`. Watch the alarms stop firing.

### Exercise 5: Replace the fake sensor with a real one
Wire up the DS18B20 or INA219 once your hardware arrives. Replace the sine-wave
simulation with real driver calls. The downstream tasks (filter, display,
logger) need ZERO changes — that's the power of layered design.

## What's Next After This Warmup

When you can answer YES to all of these, you're ready for Phase 1 of the real
project:

- [ ] Code builds without warnings
- [ ] All 5 tasks start and print their boot messages
- [ ] Filtered samples print at exactly 10 Hz (verify with the tick count)
- [ ] Alarms fire and log correctly when injected faults happen
- [ ] Diagnostics report prints every 5 seconds with sensible stack high-water
- [ ] You understand WHY each FreeRTOS primitive was chosen where it was

Then we move on to Phase 1: the real project's folder structure, architecture
diagram, and binary UART protocol specification.
