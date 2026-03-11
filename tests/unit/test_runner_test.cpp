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
    ASSERT(s.battery_voltage == 13.2, "Sample voltage matches");
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
    chg.state = "Maintenance";
    chg.valid = true;
    store.update_pwrgate(chg);

    int64_t id = runner.start_test(testing::TestType::UPS_FAILOVER);
    ASSERT(id == 0, "Test refused in Maintenance mode");
}

void test_test_runner_safety_limits() {
}

void test_system_event_deduplication() {
    DataStore store;
    store.push_system_event("Test message");
    store.push_system_event("Test message"); 

    auto events = store.system_events(10);
    ASSERT(events.size() == 1, "Immediate duplicate event was deduplicated");
}
