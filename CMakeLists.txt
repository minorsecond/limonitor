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
target_compile_options(limonitor PRIVATE -Wall -Wextra)

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
target_compile_options(limonitor_integration_test PRIVATE -Wall -Wextra)
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
target_compile_options(limonitor_unit_test PRIVATE -Wall -Wextra)
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
