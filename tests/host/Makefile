# tests/host/Makefile — host unit tests for the pure-C modules
#
# Usage:  make && ./test_host
#
# Only FreeRTOS-free, ESP-IDF-free modules are linked; everything else
# on the firmware side shares the exact same source files.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2

MAIN_SRC = ../../ESP32/main_code/main

SRCS = test_host.c \
       $(MAIN_SRC)/Protocol/crc16.c \
       $(MAIN_SRC)/Protocol/packet.c \
       $(MAIN_SRC)/Services/history_ring.c \
       $(MAIN_SRC)/Services/json_util.c

CFLAGS += -I$(MAIN_SRC)/Protocol -I$(MAIN_SRC)/Services

test_host: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

APP = ../../STM32/stm32-bringup/Application

# Driver behaviour tests for the STM32 I2C sensors (simulated bus).
test_sensors: test_sensors.c $(APP)/Src/ina219.c $(APP)/Src/mpu6050.c
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I$(APP)/Inc -o $@ $^ -lm

run: test_host test_sensors
	./test_host
	./test_sensors

clean:
	rm -f test_host test_sensors

.PHONY: run clean
