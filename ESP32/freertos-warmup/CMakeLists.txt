# ============================================================
# FreeRTOS Warmup Project - Top-Level CMakeLists.txt
# ============================================================

# 1. Standard CMake minimum version requirement
cmake_minimum_required(VERSION 3.16)

# 2. Include ESP-IDF's build system scripts
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 3. Project name (shows up in build artifacts and boot logs)
project(freertos_warmup)