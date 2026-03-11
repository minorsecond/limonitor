#pragma once

#include "telemetry.hpp"
#include "types.hpp"
#include "../battery_data.hpp"
#include "../database.hpp"
#include "../data_store.hpp"
#include "../pwrgate.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace testing {

// Test runner — manages active test, telemetry capture, safety limits
class TestRunner {
public:
    TestRunner(DataStore& store, Database* db);
    ~TestRunner();

    // Start a test. Returns test_id or 0 on failure.
    int64_t start_test(TestType type);
    void stop_test();

    bool is_running() const { return running_; }
    int64_t current_test_id() const { return current_test_id_; }
    TestType current_test_type() const { return current_test_type_; }

    void set_safety_limits(const SafetyLimits& limits) { limits_ = limits; }
    const SafetyLimits& safety_limits() const { return limits_; }

    // Active test stats for live monitoring
    struct ActiveStats {
        int64_t test_id{0};
        int64_t start_time{0};
        std::string test_type;
        int duration_seconds{0};
        double energy_delivered_wh{0};
        double voltage_at_start{0};
    };
    ActiveStats active_stats() const;

    // Called from main loop when battery/charger data updates — captures telemetry
    void on_telemetry_tick(const BatterySnapshot* bat, const PwrGateSnapshot* chg,
                           double load_w, bool tx_active, double tx_power);

private:
    DataStore& store_;
    Database* db_{nullptr};
    SafetyLimits limits_;

    std::atomic<bool> running_{false};
    std::atomic<int64_t> current_test_id_{0};
    std::atomic<TestType> current_test_type_{TestType::UPS_FAILOVER};
    std::atomic<bool> stop_requested_{false};
    std::thread capture_thread_;
    std::atomic<bool> capture_running_{false};

    int64_t test_start_ts_{0};
    double energy_delivered_wh_{0};
    double soc_at_start_{0};
    double voltage_at_start_{0};
    double load_sum_wh_{0};
    int load_sample_count_{0};

    // Telemetry capture
    TelemetryBuffer telemetry_buffer_;
    int64_t last_telemetry_ts_{0};
    static constexpr int TELEMETRY_INTERVAL_SEC = 2;

    mutable std::mutex mu_;

    void capture_loop();
    void flush_telemetry();
    bool check_safety_limits(const BatterySnapshot& bat) const;
    void finish_test(const std::string& result, const std::string& metadata_json);
};

// Called from main loop (e.g. every 60s) to run due scheduled tests
void check_scheduled_tests(Database* db, DataStore& store, TestRunner* runner);

} // namespace testing
