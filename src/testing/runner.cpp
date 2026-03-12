#include "runner.hpp"
#include "../logger.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace testing {

TestRunner::TestRunner(DataStore& store, Database* db)
    : store_(store), db_(db), telemetry_buffer_(50) {
    // Recover from crashes: any test left as 'running' in the DB is stale.
    if (db_ && db_->is_open()) {
        db_->abort_interrupted_tests();
        load_limits_from_db();
    }
}

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
    capture_running_ = false; // Ensure loop exits even if not through finish_test
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    running_ = false;
}

TestRunner::ActiveStats TestRunner::active_stats() const {
    ActiveStats s;
    if (!running_ || current_test_id_ == 0) return s;
    std::lock_guard<std::mutex> lk(mu_);
    s.test_id = current_test_id_;
    s.start_time = test_start_ts_;
    s.test_type = test_type_str(current_test_type_);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    s.duration_seconds = static_cast<int>(now - test_start_ts_);
    s.energy_delivered_wh = load_sum_wh_;
    s.voltage_at_start = voltage_at_start_;
    return s;
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
        // Sleep in smaller increments so stop() responds faster
        for (int i = 0; i < TELEMETRY_INTERVAL_SEC * 10 && capture_running_ && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
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
        std::lock_guard<std::mutex> lk(mu_);
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
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (limits_.max_duration_sec > 0 && (now - test_start_ts_) >= limits_.max_duration_sec)
        return true;
    if (limits_.abort_on_overtemp &&
        (bat.protection.charge_overtemp || bat.protection.discharge_overtemp))
        return true;
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
    LOG_INFO("DEBUG: finish_test exit");
}

static int64_t compute_next_run(const std::string& freq, int hour, int min, int day) {
    std::time_t now = std::time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    t.tm_sec = 0; t.tm_min = min; t.tm_hour = hour;
    if (freq == "daily") {
        std::time_t cand = std::mktime(&t);
        return cand <= now ? cand + 86400 : cand;
    }
    if (freq == "weekly") {
        int wday = (day >= 0 && day <= 6) ? day : 0;
        int delta = (wday - t.tm_wday + 7) % 7;
        if (delta == 0) t.tm_mday += 7;
        else t.tm_mday += delta;
        return std::mktime(&t);
    }
    if (freq == "monthly") {
        t.tm_mday = (day >= 1 && day <= 28) ? day : 1;
        std::time_t cand = std::mktime(&t);
        if (cand <= now) { t.tm_mon += 1; cand = std::mktime(&t); }
        return cand;
    }
    return now + 86400;
}

void check_scheduled_tests(Database* db, DataStore& store, TestRunner* runner) {
    if (!db || !db->is_open() || !runner || runner->is_running()) return;
    auto scheds = db->load_test_schedules();
    std::time_t now = std::time(nullptr);
    auto bat = store.latest();
    if (!bat || !bat->valid) return;
    const auto& lim = runner->safety_limits();
    for (const auto& s : scheds) {
        if (!s.enabled || s.next_run_ts > now) continue;
        TestType type = parse_test_type(s.test_type.c_str());
        std::string reason;
        if (bat->soc_pct < lim.soc_floor_pct + 5.0)
            reason = "SOC too low (" + std::to_string(static_cast<int>(bat->soc_pct)) + "%)";
        else if (bat->total_voltage_v < lim.voltage_floor_v + 0.5)
            reason = "Voltage too low (" + std::to_string(static_cast<int>(bat->total_voltage_v * 10) / 10.0) + "V)";
        else if (db->get_setting("maintenance_mode") == "1")
            reason = "Maintenance mode active";
        if (!reason.empty()) {
            db->update_test_schedule_skip(s.id, now, reason);
            db->update_test_schedule_next_run(s.id, compute_next_run(s.frequency, s.run_hour, s.run_minute, s.day_of_month));
            LOG_WARN("Scheduled test %s skipped: %s", s.test_type.c_str(), reason.c_str());
            continue;
        }
        int64_t id = runner->start_test(type);
        if (id > 0) {
            db->update_test_schedule_next_run(s.id, compute_next_run(s.frequency, s.run_hour, s.run_minute, s.day_of_month));
            LOG_INFO("Scheduled test %s started (id=%lld)", s.test_type.c_str(), (long long)id);
            return;
        }
        db->update_test_schedule_skip(s.id, now, "start_test failed");
        db->update_test_schedule_next_run(s.id, compute_next_run(s.frequency, s.run_hour, s.run_minute, s.day_of_month));
    }
}

void TestRunner::load_limits_from_db() {
    if (!db_) return;
    auto s = db_->get_setting("test_soc_floor_pct");
    if (!s.empty()) try { limits_.soc_floor_pct = std::stod(s); } catch (...) {}
    s = db_->get_setting("test_voltage_floor_v");
    if (!s.empty()) try { limits_.voltage_floor_v = std::stod(s); } catch (...) {}
    s = db_->get_setting("test_max_duration_sec");
    if (!s.empty()) try { limits_.max_duration_sec = std::stoi(s); } catch (...) {}
    s = db_->get_setting("test_abort_on_overtemp");
    if (!s.empty()) limits_.abort_on_overtemp = (s == "1" || s == "true");
}

} // namespace testing
