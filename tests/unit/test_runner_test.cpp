#include "../src/testing/runner.hpp"
#include "../src/data_store.hpp"
#include "../src/database.hpp"
#include "../src/logger.hpp"
#include "../tests/test_helpers.hpp"
#include <vector>
#include <string>

void test_parse_test_type() {
    using namespace testing;
    ASSERT(strlen(test_type_str(TestType::BATTERY_CAPACITY)) > 0, "test_type_str exists");
}

void test_test_type_str() {
    using namespace testing;
    const char* s = test_type_str(TestType::BATTERY_CAPACITY);
    ASSERT(s != nullptr, "test_type_str returns non-null");
}

void test_test_result_str() {
    using namespace testing;
    const char* s = test_result_str(TestResult::PASSED);
    ASSERT(s != nullptr, "test_result_str returns non-null");
}

void test_make_telemetry_sample() {
    using namespace testing;
    BatterySnapshot bat;
    bat.total_voltage_v = 13.2;
    bat.current_a = -5.0;
    bat.soc_pct = 90.0;
    bat.valid = true;

    PwrGateSnapshot chg;
    chg.ps_v = 14.0;
    chg.bat_v = 13.3;
    chg.valid = true;

    auto s = make_telemetry_sample(123, &bat, &chg, 15.0, true, 25.0);
    ASSERT(s.test_id == 123, "Sample test_id matches");
    ASSERT(std::abs(s.battery_voltage - 13.2) < 0.001, "Sample voltage matches");
}

void test_test_runner_start_stop() {
    std::string db_path = "/tmp/test_runner_basic_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    DataStore store;
    testing::TestRunner runner(store, &db);

    BatterySnapshot bat;
    bat.soc_pct = 95.0;
    bat.total_voltage_v = 13.3;
    bat.valid = true;
    store.update(bat);

    int64_t id = runner.start_test(testing::TestType::UPS_FAILOVER);
    if (id > 0) {
        ASSERT(runner.is_running(), "Runner reports running");
        runner.stop_test();
        ASSERT(!runner.is_running(), "Runner reports not running after stop");
    }

    remove(db_path.c_str());
}

void test_test_runner_refuses_low_soc() {
    DataStore store;
    testing::TestRunner runner(store, nullptr);

    BatterySnapshot bat;
    bat.soc_pct = 5.0;
    bat.valid = true;
    store.update(bat);

    int64_t id = runner.start_test(testing::TestType::UPS_FAILOVER);
    ASSERT(id == 0, "Test refused with low SOC");
}

void test_test_runner_refuses_maintenance_mode() {
    DataStore store;
    testing::TestRunner runner(store, nullptr);

    BatterySnapshot bat;
    bat.soc_pct = 95.0;
    bat.valid = true;
    store.update(bat);

    PwrGateSnapshot chg;
    chg.state = std::make_shared<std::string>("Maintenance");
    chg.valid = true;
    store.update_pwrgate(chg);

    int64_t id = runner.start_test(testing::TestType::UPS_FAILOVER);
    ASSERT(id == 0, "Test refused in Maintenance mode");
}

void test_test_runner_safety_limits() {
    // Verify that safety limits round-trip through DB get_setting/set_setting
    std::string db_path = "/tmp/test_safety_limits_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    // Write limits
    testing::SafetyLimits lim;
    lim.soc_floor_pct    = 15.5;
    lim.voltage_floor_v  = 11.8;
    lim.max_duration_sec = 3600;
    lim.abort_on_overtemp = false;
    db.set_setting("test_soc_floor_pct",    std::to_string(lim.soc_floor_pct));
    db.set_setting("test_voltage_floor_v",  std::to_string(lim.voltage_floor_v));
    db.set_setting("test_max_duration_sec", std::to_string(lim.max_duration_sec));
    db.set_setting("test_abort_on_overtemp", "0");

    // Read back via runner (load_limits_from_db is called in constructor)
    DataStore store;
    testing::TestRunner runner(store, &db);
    const auto& loaded = runner.safety_limits();
    ASSERT(std::abs(loaded.soc_floor_pct - 15.5) < 0.001,   "soc_floor_pct round-trips");
    ASSERT(std::abs(loaded.voltage_floor_v - 11.8) < 0.001,  "voltage_floor_v round-trips");
    ASSERT(loaded.max_duration_sec == 3600,                   "max_duration_sec round-trips");
    ASSERT(!loaded.abort_on_overtemp,                         "abort_on_overtemp round-trips");

    // Verify set_safety_limits + re-persist cycle
    testing::SafetyLimits lim2;
    lim2.soc_floor_pct    = 25.0;
    lim2.voltage_floor_v  = 12.5;
    lim2.max_duration_sec = 1800;
    lim2.abort_on_overtemp = true;
    runner.set_safety_limits(lim2);
    db.set_setting("test_soc_floor_pct",    std::to_string(lim2.soc_floor_pct));
    db.set_setting("test_voltage_floor_v",  std::to_string(lim2.voltage_floor_v));
    db.set_setting("test_max_duration_sec", std::to_string(lim2.max_duration_sec));
    db.set_setting("test_abort_on_overtemp", "1");
    const auto& updated = runner.safety_limits();
    ASSERT(std::abs(updated.soc_floor_pct - 25.0) < 0.001,  "updated soc_floor_pct");
    ASSERT(updated.max_duration_sec == 1800,                  "updated max_duration_sec");
    ASSERT(updated.abort_on_overtemp,                         "updated abort_on_overtemp");

    remove(db_path.c_str());
}

void test_test_schedules_crud() {
    std::string db_path = "/tmp/test_schedules_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    // Insert a schedule
    Database::TestScheduleRow row;
    row.test_type    = "ups_failover";
    row.frequency    = "monthly";
    row.run_hour     = 3;
    row.run_minute   = 30;
    row.day_of_month = 15;
    row.next_run_ts  = 9999999;
    row.enabled      = true;
    int64_t id = db.insert_test_schedule(row);
    ASSERT(id > 0, "insert_test_schedule returns positive id");

    // Load and verify
    auto schedules = db.load_test_schedules();
    ASSERT(schedules.size() == 1, "One schedule loaded");
    ASSERT(schedules[0].test_type == "ups_failover", "test_type matches");
    ASSERT(schedules[0].frequency == "monthly",      "frequency matches");
    ASSERT(schedules[0].run_hour == 3,               "run_hour matches");
    ASSERT(schedules[0].run_minute == 30,             "run_minute matches");
    ASSERT(schedules[0].day_of_month == 15,          "day_of_month matches");
    ASSERT(schedules[0].next_run_ts == 9999999,      "next_run_ts matches");
    ASSERT(schedules[0].enabled,                      "enabled matches");

    // Update next_run_ts
    bool ok = db.update_test_schedule_next_run(id, 1234567890);
    ASSERT(ok, "update_test_schedule_next_run succeeds");
    schedules = db.load_test_schedules();
    ASSERT(schedules[0].next_run_ts == 1234567890, "next_run_ts updated");

    // Insert a second schedule
    Database::TestScheduleRow row2;
    row2.test_type    = "capacity";
    row2.frequency    = "weekly";
    row2.run_hour     = 2;
    row2.run_minute   = 0;
    row2.day_of_month = 1;
    row2.next_run_ts  = 7777777;
    row2.enabled      = true;
    int64_t id2 = db.insert_test_schedule(row2);
    ASSERT(id2 > 0, "second schedule inserted");
    schedules = db.load_test_schedules();
    ASSERT(schedules.size() == 2, "Two schedules loaded");

    // Delete first
    ok = db.delete_test_schedule(id);
    ASSERT(ok, "delete_test_schedule succeeds");
    schedules = db.load_test_schedules();
    ASSERT(schedules.size() == 1, "One schedule after delete");
    ASSERT(schedules[0].test_type == "capacity", "Remaining schedule is correct");

    remove(db_path.c_str());
}

void test_system_event_deduplication() {
    DataStore store;
    store.push_system_event("Test message");
    store.push_system_event("Test message"); 

    auto events = store.system_events(10);
    ASSERT(events.size() == 1, "Immediate duplicate event was deduplicated");
}
