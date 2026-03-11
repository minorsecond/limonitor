cmake_minimum_required(VERSION 3.16)
project(limonitor VERSION 1.0.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Curses)
if(CURSES_FOUND)
    add_definitions(-DHAVE_NCURSES)
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

# Protocol sources (shared between main binary and integration test)
set(PROTO_SOURCES
    src/litime_protocol.cpp
    src/jbd_protocol.cpp
)

# --- Hyper-optimization detection logic ---
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Common optimization flags for all platforms in Release mode
set(COMMON_OPT_FLAGS -O3 -flto -fomit-frame-pointer)

# Detect architecture and apply specific micro-optimizations
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    # Apple Silicon (M1/M2/M3)
    set(ARCH_NAME "Apple Silicon (M1/M2/M3)")
    set(ARCH_OPT_FLAGS -mcpu=apple-m1)
elseif(NOT APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64"))
    # Raspberry Pi detection (rough check by processor and system)
    # RPi 3B+, Zero 2W use Cortex-A53
    # RPi 4 uses Cortex-A72 (part 0xd08)
    # RPi 5 uses Cortex-A76 (part 0xd0b)

    # 1. Try 'pinout' command (if installed, usually via python3-gpiozero)
    execute_process(COMMAND which pinout RESULT_VARIABLE HAS_PINOUT OUTPUT_QUIET ERROR_QUIET)
    if(HAS_PINOUT EQUAL 0)
        execute_process(COMMAND pinout ERROR_QUIET OUTPUT_VARIABLE PINOUT_RAW)
        # Standardize for matching
        string(TOUPPER "${PINOUT_RAW}" PINOUT_UPPER)
    endif()

    # 2. Check /proc/device-tree/model (most reliable on Pi)
    execute_process(COMMAND cat /proc/device-tree/model ERROR_QUIET OUTPUT_VARIABLE PI_MODEL)
    # 3. Check /proc/device-tree/compatible
    execute_process(COMMAND cat /proc/device-tree/compatible ERROR_QUIET OUTPUT_VARIABLE PI_COMPAT)
    # 4. Check CPU part number in /proc/cpuinfo (0xd03 = Cortex-A53, 0xd08 = Cortex-A72, 0xd0b = Cortex-A76)
    execute_process(COMMAND grep -qiE "0xd03|Part.*0xd03" /proc/cpuinfo RESULT_VARIABLE IS_A53)
    execute_process(COMMAND grep -qiE "Cortex-A72|0xd08|Part.*0xd08" /proc/cpuinfo RESULT_VARIABLE IS_A72)
    execute_process(COMMAND grep -qiE "Cortex-A76|0xd0b|Part.*0xd0b" /proc/cpuinfo RESULT_VARIABLE IS_A76)

    if(PINOUT_UPPER MATCHES "RASPBERRY PI 5" OR PI_MODEL MATCHES "Raspberry Pi 5" OR PI_COMPAT MATCHES "raspberrypi,5" OR IS_A76 EQUAL 0)
        set(ARCH_NAME "Raspberry Pi 5 (Cortex-A76)")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            # Cortex-A76 optimization flags
            set(ARCH_OPT_FLAGS -march=armv8.2-a+crc+simd -mtune=cortex-a76)
        else()
            # 32-bit mode for Pi 5 is unlikely but possible
            set(ARCH_OPT_FLAGS -march=armv8.2-a+crc+simd -mtune=cortex-a76 -mfpu=neon-fp-armv8 -mfloat-abi=hard)
        endif()
    elseif(PINOUT_UPPER MATCHES "RASPBERRY PI 4" OR PI_MODEL MATCHES "Raspberry Pi 4" OR PI_COMPAT MATCHES "raspberrypi,4-model-b" OR IS_A72 EQUAL 0)
        set(ARCH_NAME "Raspberry Pi 4 (Cortex-A72)")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(ARCH_OPT_FLAGS -march=armv8-a+crc+simd -mtune=cortex-a72)
        else()
            set(ARCH_OPT_FLAGS -march=armv8-a+crc+simd -mtune=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard)
        endif()
    elseif(PINOUT_UPPER MATCHES "RASPBERRY PI (3|ZERO 2)" OR PI_MODEL MATCHES "Raspberry Pi (3|Zero 2)" OR PI_COMPAT MATCHES "raspberrypi,(3|zero2)" OR IS_A53 EQUAL 0)
        set(ARCH_NAME "Raspberry Pi 3B+ / Zero 2W (Cortex-A53)")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            # 64-bit mode doesn't need -mfpu=neon as it's standard
            set(ARCH_OPT_FLAGS -march=armv8-a+crc -mtune=cortex-a53)
        else()
            set(ARCH_OPT_FLAGS -march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard)
        endif()
    else()
        # Fallback for other ARM/RPi
        set(ARCH_NAME "Native ARM")
        set(ARCH_OPT_FLAGS -march=native)
    endif()
else()
    # Generic or other arch
    set(ARCH_NAME "Native Host")
    set(ARCH_OPT_FLAGS -march=native)
endif()

message(STATUS "------------------------------------------------------------")
message(STATUS "  HYPER-OPTIMIZATION PROFILE: ${ARCH_NAME}")
message(STATUS "  Build Type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "  Arch Flags:      ${ARCH_OPT_FLAGS}")
message(STATUS "  Common Flags:    ${COMMON_OPT_FLAGS}")
message(STATUS "------------------------------------------------------------")

# Optional libcurl for weather API
find_package(CURL)
if(CURL_FOUND)
    add_definitions(-DHAVE_LIBCURL)
endif()

# Platform-independent sources
set(SOURCES
    src/main.cpp
    src/logger.cpp
    src/analytics.cpp
    src/shelly_client.cpp
    src/data_store.cpp
    src/database.cpp
    src/ops_util.cpp
    src/http_server.cpp
    src/tui.cpp
    src/pwrgate.cpp
    src/pwrgate_client.cpp
    src/serial_reader.cpp
    src/analytics/anomaly_detection.cpp
    src/analytics/resistance_estimator.cpp
    src/analytics/solar_simulation.cpp
    src/analytics/weather_forecast.cpp
    src/analytics/health_score.cpp
    src/analytics/extensions.cpp
    src/testing/types.cpp
    src/testing/telemetry.cpp
    src/testing/runner.cpp
    ${PROTO_SOURCES}
)

# Platform-specific BLE backend
if(APPLE)
    list(APPEND SOURCES src/ble_manager_macos.mm)
else()
    list(APPEND SOURCES src/ble_manager_linux.cpp)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
endif()

add_executable(limonitor ${SOURCES})
target_compile_options(limonitor PRIVATE -Wall -Wextra -Wno-psabi ${COMMON_OPT_FLAGS} ${ARCH_OPT_FLAGS})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(limonitor PRIVATE -flto)
endif()

# Common link libs (atomic needed for 64-bit atomics on 32-bit ARM)
target_link_libraries(limonitor pthread SQLite::SQLite3)
if(NOT APPLE)
    target_link_libraries(limonitor atomic)
endif()
if(CURL_FOUND)
    target_link_libraries(limonitor CURL::libcurl)
endif()

# Platform link libs
if(APPLE)
    target_link_libraries(limonitor
        "-framework CoreBluetooth"
        "-framework Foundation"
    )
else()
    target_link_libraries(limonitor PkgConfig::GIO)
endif()

if(CURSES_FOUND)
    target_include_directories(limonitor PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(limonitor ${CURSES_LIBRARIES})
endif()

install(TARGETS limonitor RUNTIME DESTINATION bin)

set(ITEST_SOURCES
    tests/integration_test.cpp
    src/logger.cpp
    ${PROTO_SOURCES}
)
if(APPLE)
    list(APPEND ITEST_SOURCES src/ble_manager_macos.mm)
else()
    list(APPEND ITEST_SOURCES src/ble_manager_linux.cpp)
endif()

add_executable(limonitor_integration_test ${ITEST_SOURCES})
target_compile_options(limonitor_integration_test PRIVATE -Wall -Wextra -Wno-psabi ${COMMON_OPT_FLAGS} ${ARCH_OPT_FLAGS})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(limonitor_integration_test PRIVATE -flto)
endif()
target_link_libraries(limonitor_integration_test pthread)
if(APPLE)
    target_link_libraries(limonitor_integration_test
        "-framework CoreBluetooth"
        "-framework Foundation"
    )
else()
    target_link_libraries(limonitor_integration_test PkgConfig::GIO)
endif()

# Unit tests (Database, WeatherForecast, HttpServer CORS/settings)
set(UTEST_SOURCES
    tests/unit_test.cpp
    src/logger.cpp
    src/analytics.cpp
    src/shelly_client.cpp
    src/data_store.cpp
    src/database.cpp
    src/ops_util.cpp
    src/http_server.cpp
    src/pwrgate.cpp
    src/pwrgate_client.cpp
    src/serial_reader.cpp
    src/analytics/anomaly_detection.cpp
    src/analytics/resistance_estimator.cpp
    src/analytics/solar_simulation.cpp
    src/analytics/weather_forecast.cpp
    src/analytics/health_score.cpp
    src/analytics/extensions.cpp
    src/testing/types.cpp
    src/testing/telemetry.cpp
    src/testing/runner.cpp
    ${PROTO_SOURCES}
)
if(APPLE)
    list(APPEND UTEST_SOURCES src/ble_manager_macos.mm)
else()
    list(APPEND UTEST_SOURCES src/ble_manager_linux.cpp)
endif()

add_executable(limonitor_unit_test ${UTEST_SOURCES})
target_compile_options(limonitor_unit_test PRIVATE -Wall -Wextra -Wno-psabi ${COMMON_OPT_FLAGS} ${ARCH_OPT_FLAGS})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(limonitor_unit_test PRIVATE -flto)
endif()
target_link_libraries(limonitor_unit_test pthread SQLite::SQLite3)
if(NOT APPLE)
    target_link_libraries(limonitor_unit_test atomic)
endif()
if(CURL_FOUND)
    target_link_libraries(limonitor_unit_test CURL::libcurl)
endif()
if(APPLE)
    target_link_libraries(limonitor_unit_test
        "-framework CoreBluetooth"
        "-framework Foundation"
    )
else()
    target_link_libraries(limonitor_unit_test PkgConfig::GIO)
endif()

add_custom_target(test
    COMMAND ${CMAKE_BINARY_DIR}/limonitor_unit_test
    COMMENT "Running unit tests..."
    USES_TERMINAL
)
add_dependencies(test limonitor_unit_test)
