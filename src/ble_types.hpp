#pragma once
#include <chrono>
#include <string>

struct DiscoveredDevice {
    std::string id;
    std::string name;
    int         rssi{0};
    bool        has_target_service{false};

    std::chrono::steady_clock::time_point last_seen{};
};
