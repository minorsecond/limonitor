#pragma once
#include "tx_events.hpp"
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
    // Skip BLE entirely (e.g. Pi running as serial/HTTP-only node)
    bool no_ble = false;

    // EpicPowerGate 2 — local serial (Pi) or remote HTTP poll (Mac)
    std::string serial_device;          // e.g. "/dev/ttyACM0"
    int         serial_baud = 115200;
    std::string pwrgate_remote;         // e.g. "ham-pi:8081"

    // SQLite database (empty = use Database::default_path())
    std::string db_path;
    int         db_write_interval_s = 60;  // write at most once per N seconds (0 = every update)

    // Daemon mode: skip TUI, log to stderr/file only
    bool daemon_mode = false;

    // TX detection threshold — net positive battery current in amps that starts a TX event.
    // Default 1.0 A works for most VHF/UHF radios. Raise to 4–6 for 100 W HF rigs.
    double tx_threshold_a = TX_THRESHOLD_A;

    // Battery metadata
    std::string battery_purchased;      // e.g. "2024-03-15" — stored as-is
    double rated_capacity_ah = 0.0;     // 0 = auto-detect from BMS nominal_ah
    int    data_retention_days = 90;    // Number of days to keep high-res telemetry
    int    event_retention_days = 3650;  // Number of days to keep system/ops events (10 years)

    // Solar simulation (config.toml [solar] equivalent via key=value)
    bool solar_enabled = false;
    double solar_panel_watts = 400.0;
    double solar_system_efficiency = 0.75;
    std::string solar_zip_code = "80112";

    // Weather API (config.toml [weather] equivalent).
    std::string weather_api_key;
    std::string weather_zip_code = "80112";
};
