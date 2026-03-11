#pragma once

#include "battery_data.hpp"
#include "pwrgate.hpp"
#include <optional>
#include <string>

// Builds a JSON snapshot of system characteristics for ops log metadata.
// Uses whatever data is available (partial snapshot if battery or charger missing).
std::string build_ops_snapshot_json(
    const std::optional<BatterySnapshot>* snap,
    const std::optional<PwrGateSnapshot>* pg);
