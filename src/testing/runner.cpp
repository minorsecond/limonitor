#include "runner.hpp"
#include "../logger.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace testing {

TestRunner::TestRunner(DataStore& store, Database* db)
    : store_(store), db_(db), telemetry_buffer_(50) {}

TestRunner::~TestRunner() {
    stop_test();
}

int64_t TestRunner::start_test(TestType type) {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return 0;
    if (!db_ || !db_->is_open()) return 0;

    auto bat = store_.latest();
    if (!bat || !bat->valid) {
        LOG_WARN("Test: cannot start — no battery data");
        return 0;
    }

    // Pre-flight checks
    if (bat->soc_pct < limits_.soc_floor_pct + 5.0) {
        LOG_WARN("Test: SOC too low (%.0f%%), need > %.0f%%", bat->soc_pct, limits_.soc_floor_pct + 5.0);
        return 0;
    }
    if (bat->total_voltage_v < limits_.voltage_floor_v + 0.5) {
        LOG_WARN("Test: voltage too low (%.2fV)", bat->total_voltage_v);
        return 0;
    }

    // Check maintenance mode
    if (db_->get_setting("maintenance_mode") == "1") {
        LOG_WARN("Test: maintenance mode active — abort");
        return 0;
    }

    Database::TestRunRow row;
    row.test_type = test_type_str(type);
    row.start_time = std::chrono::system_clock::to_time_t(bat->timestamp);
    row.end_time = 0;
    row.duration_seconds = 0;
    row.result = "running";
    row.initial_soc = bat->soc_pct;
    row.initial_voltage = bat->total_voltage_v;
    row.average_load = 0;

    int64_t id = db_->insert_test_run(row);
    if (id <= 0) return 0;

    current_test_id_ = id;
    current_test_type_ = type;
    running_ = true;
    stop_requested_ = false;
    test_start_ts_ = row.start_time;
    energy_delivered_wh_ = 0;
    soc_at_start_ = bat->soc_pct;
    voltage_at_start_ = bat->total_voltage_v;
    load_sum_wh_ = 0;
    load_sample_count_ = 0;
    last_telemetry_ts_ = 0;

    capture_running_ = true;
    capture_thread_ = std::thread(&TestRunner::capture_loop, this);

    LOG_INFO("Test started: id=%lld type=%s soc=%.0f%%",
             (long long)id, row.test_type.c_str(), bat->soc_pct);
    return id;
}

void TestRunner::stop_test() {
    if (!running_) return;
    stop_requested_ = true;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    capture_running_ = false;
}

void TestRunner::capture_loop() {
    while (capture_running_ && running_) {
        auto bat = store_.latest();
        auto chg = store_.latest_pwrgate();
        double load_w = bat && bat->valid && bat->current_a > 0
            ? (bat->power_w > 0 ? bat->power_w : bat->total_voltage_v * bat->current_a)
            : 0;
        bool tx_active = false;
        double tx_power = 0;
        auto tx_evs = store_.tx_events(5);
        for (const auto& e : tx_evs) {
            if (e.peak_current > 0.5) { tx_active = true; tx_power = e.peak_power; break; }
        }
        on_telemetry_tick(bat ? &*bat : nullptr, chg ? &*chg : nullptr,
                          load_w, tx_active, tx_power);
        std::this_thread::sleep_for(std::chrono::seconds(TELEMETRY_INTERVAL_SEC));
    }
}

void TestRunner::on_telemetry_tick(const BatterySnapshot* bat, const PwrGateSnapshot* chg,
                                   double load_w, bool tx_active, double tx_power) {
    if (!running_ || current_test_id_ == 0) return;

    auto now_ts = bat ? static_cast<int64_t>(
        std::chrono::system_clock::to_time_t(bat->timestamp)) : 0;

    // Safety check
    if (bat && bat->valid && check_safety_limits(*bat)) {
        LOG_WARN("Test aborted: safety limits exceeded");
        finish_test("aborted", "{\"reason\":\"safety_limits\"}");
        return;
    }

    // Max duration
    if (now_ts - test_start_ts_ >= limits_.max_duration_sec) {
        LOG_WARN("Test aborted: max duration exceeded");
        finish_test("aborted", "{\"reason\":\"max_duration\"}");
        return;
    }

    // Stop requested
    if (stop_requested_) {
        finish_test("aborted", "{\"reason\":\"user_stop\"}");
        return;
    }

    // Integrate energy (discharge only)
    if (bat && bat->valid && bat->current_a > 0.01) {
        double pwr = bat->power_w > 0 ? bat->power_w : (bat->total_voltage_v * bat->current_a);
        load_sum_wh_ += pwr * (1.0 / 3600.0);  // assume 1s tick
        load_sample_count_++;
    }

    // Capture telemetry at interval
    if (now_ts - last_telemetry_ts_ >= TELEMETRY_INTERVAL_SEC) {
        last_telemetry_ts_ = now_ts;
        auto sample = make_telemetry_sample(current_test_id_, bat, chg, load_w, tx_active, tx_power);
        telemetry_buffer_.push(sample);
        if (telemetry_buffer_.needs_flush()) {
            flush_telemetry();
        }
    }
}

void TestRunner::flush_telemetry() {
    auto batch = telemetry_buffer_.flush();
    if (!batch.empty() && db_) {
        db_->insert_test_telemetry_batch(batch);
    }
}

bool TestRunner::check_safety_limits(const BatterySnapshot& bat) const {
    if (bat.soc_pct < limits_.soc_floor_pct) return true;
    if (bat.total_voltage_v < limits_.voltage_floor_v) return true;
    return false;
}

void TestRunner::finish_test(const std::string& result, const std::string& metadata_json) {
    capture_running_ = false;  // capture_loop will exit on next iteration

    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return;

    flush_telemetry();

    auto now = std::chrono::system_clock::now();
    int64_t end_ts = std::chrono::system_clock::to_time_t(now);
    int duration = std::max(1, static_cast<int>(end_ts - test_start_ts_));

    auto bat = store_.latest();
    double soc_end = bat && bat->valid ? bat->soc_pct : 0;

    std::string meta = metadata_json;
    double measured_wh = 0;
    double health_pct = -1;
    if (current_test_type_ == TestType::BATTERY_CAPACITY && load_sample_count_ > 0) {
        double soc_drop = soc_at_start_ - soc_end;
        if (soc_drop > 1.0) {
            measured_wh = load_sum_wh_ / (soc_drop / 100.0);
            double rated_wh = 0;
            if (bat && bat->valid && bat->nominal_ah > 0) {
                double pack_v = bat->total_voltage_v > 0 ? bat->total_voltage_v : 13.0;
                rated_wh = bat->nominal_ah * pack_v;
                if (rated_wh > 0) health_pct = 100.0 * (measured_wh / rated_wh);
            }
            if (db_) {
                db_->insert_battery_capacity_test(end_ts, measured_wh, soc_at_start_, soc_end,
                                                  load_sum_wh_, health_pct, current_test_id_);
            }
        }
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"energy_delivered_wh\":%.1f,\"soc_start\":%.1f,\"soc_end\":%.1f,\"measured_capacity_wh\":%.1f,\"health_percent\":%.1f}",
            load_sum_wh_, soc_at_start_, soc_end, measured_wh, health_pct);
        meta = buf;
    }

    if (db_) {
        db_->update_test_run(current_test_id_, end_ts, duration, result, meta);
    }

    LOG_INFO("Test finished: id=%lld result=%s duration=%ds",
             (long long)current_test_id_, result.c_str(), duration);

    running_ = false;
    current_test_id_ = 0;
    stop_requested_ = false;
}

} // namespace testing
