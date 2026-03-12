#include "types.hpp"
#include <cstring>

namespace testing {

const char* test_type_str(TestType t) {
    switch (t) {
        case TestType::UPS_FAILOVER:      return "ups_failover";
        case TestType::BATTERY_CAPACITY:  return "capacity";
        case TestType::LOAD_SPIKE:        return "load_spike";
        case TestType::CHARGER_RECOVERY:  return "charger_recovery";
        case TestType::SIMULATED_OUTAGE:  return "simulated_outage";
    }
    return "unknown";
}

TestType parse_test_type(const char* s) {
    if (!s) return TestType::UPS_FAILOVER;
    if (std::strcmp(s, "ups_failover") == 0) return TestType::UPS_FAILOVER;
    if (std::strcmp(s, "capacity") == 0) return TestType::BATTERY_CAPACITY;
    if (std::strcmp(s, "load_spike") == 0) return TestType::LOAD_SPIKE;
    if (std::strcmp(s, "charger_recovery") == 0) return TestType::CHARGER_RECOVERY;
    if (std::strcmp(s, "simulated_outage") == 0) return TestType::SIMULATED_OUTAGE;
    return TestType::UPS_FAILOVER;
}

const char* test_result_str(TestResult r) {
    switch (r) {
        case TestResult::RUNNING: return "running";
        case TestResult::PASSED:  return "passed";
        case TestResult::FAILED: return "failed";
        case TestResult::ABORTED: return "aborted";
    }
    return "unknown";
}

TestTelemetrySample make_telemetry_sample(
    int64_t test_id,
    const BatterySnapshot* bat,
    const PwrGateSnapshot* chg,
    double load_w,
    bool tx_active,
    double tx_power)
{
    TestTelemetrySample s;
    s.test_id = test_id;
    s.timestamp = bat ? static_cast<int64_t>(
        std::chrono::system_clock::to_time_t(bat->timestamp)) : 0;

    if (bat && bat->valid) {
        s.battery_voltage = static_cast<double>(bat->total_voltage_v);
        s.battery_current = static_cast<double>(bat->current_a);
        s.battery_soc = static_cast<double>(bat->soc_pct);
        s.load_power = load_w > 0 ? load_w : (static_cast<double>(bat->power_w) > 0 ? static_cast<double>(bat->power_w) : 0);
        s.cell_delta = static_cast<double>(bat->cell_delta_v);
        s.temperature = bat->temperatures_c.empty() ? 0 : static_cast<double>(bat->temperatures_c[0]);
    }

    if (chg && chg->valid) {
        s.grid_voltage = static_cast<double>(chg->ps_v);
        s.solar_voltage = static_cast<double>(chg->sol_v);
        s.solar_current = chg->bat_a > 0.0f ? static_cast<double>(chg->bat_a) : 0;  // charge current when solar
        s.charger_state = chg->state ? *chg->state : "";
        s.charger_voltage = static_cast<double>(chg->bat_v);
        s.charger_current = static_cast<double>(chg->bat_a);
    }

    s.radio_tx_active = tx_active;
    s.radio_tx_power = tx_power;

    return s;
}

} // namespace testing
