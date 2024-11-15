#pragma once
#include <cstdint>
#include <string>

struct Config {
    // BLE device targeting — set one or both; addr takes precedence
    std::string device_address;            // e.g. "AA:BB:CC:DD:EE:FF"
    std::string device_name;               // e.g. "JBD-SP04S034"

    // GATT UUIDs — LiTime L1200/compatible defaults (FFE0 service)
    // JBD-based packs use FF00/FF01/FF02; override with --service/--notify/--write
    std::string service_uuid    = "0000ffe0-0000-1000-8000-00805f9b34fb";
    std::string notify_char_uuid= "0000ffe1-0000-1000-8000-00805f9b34fb";
    std::string write_char_uuid = "0000ffe2-0000-1000-8000-00805f9b34fb";
    std::string adapter_path    = "/org/bluez/hci0";

    // Polling
    int poll_interval_s = 5;

    // HTTP API
    uint16_t http_port = 8080;
    std::string http_bind = "0.0.0.0";

    // Logging
    std::string log_file;           // empty = no file logging
    bool log_verbose = false;       // show DEBUG messages
    size_t log_rotate_bytes = 10 * 1024 * 1024; // 10 MB

    // History ring buffer size
    size_t history_size = 2880;     // ~4 hours at 5s intervals

    // Demo / offline mode (generates synthetic data, no BLE needed)
    bool demo_mode = false;
};
