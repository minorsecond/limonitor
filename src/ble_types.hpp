#pragma once
#include <chrono>
#include <string>

// A BLE device seen during a scan.
// id   — platform identifier (CoreBluetooth UUID string on macOS, MAC address on Linux)
// name — advertised local name, may be empty
struct DiscoveredDevice {
    std::string id;
    std::string name;
    int         rssi{0};
    bool        has_target_service{false};

    // Set by DataStore; not populated by the BLE manager.
    std::chrono::steady_clock::time_point last_seen{};
};
