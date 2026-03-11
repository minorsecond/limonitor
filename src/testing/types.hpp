#pragma once

#include "../battery_data.hpp"
#include "../pwrgate.hpp"
#include <chrono>
#include <string>

namespace testing {

// Test type identifiers
enum class TestType {
    UPS_FAILOVER,
    BATTERY_CAPACITY,
    LOAD_SPIKE,
    CHARGER_RECOVERY,
    SIMULATED_OUTAGE,
};

const char* test_type_str(TestType t);
TestType parse_test_type(const char* s);

// Test result
enum class TestResult {
    RUNNING,
    PASSED,
    FAILED,
    ABORTED,
};

const char* test_result_str(TestResult r);

// Safety limits (configurable)
struct SafetyLimits {
    double soc_floor_pct{10.0};       // Abort if SOC drops below
    double voltage_floor_v{11.0};     // Abort if pack voltage drops below (12V system)
    int max_duration_sec{7200};       // Max 2 hours per test
    bool abort_on_overtemp{true};     // Abort if temperature exceeds limit
};

// High-resolution telemetry sample (1–2 second interval during tests)
struct TestTelemetrySample {
    int64_t test_id{0};
    int64_t timestamp{0};            // Unix seconds

    double battery_voltage{0};
    double battery_current{0};
    double battery_soc{0};
    double load_power{0};

    double grid_voltage{0};           // ps_v when grid present
    double solar_voltage{0};
    double solar_current{0};

    std::string charger_state;
    double charger_voltage{0};
    double charger_current{0};

    double cell_delta{0};
    double temperature{0};            // Primary temp sensor

    bool radio_tx_active{false};
    double radio_tx_power{0};
};

// Build telemetry sample from current system state
TestTelemetrySample make_telemetry_sample(
    int64_t test_id,
    const BatterySnapshot* bat,
    const PwrGateSnapshot* chg,
    double load_w,
    bool tx_active,
    double tx_power);

} // namespace testing
