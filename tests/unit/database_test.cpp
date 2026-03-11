#include "../src/database.hpp"
#include "../src/logger.hpp"
#include "../tests/test_helpers.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <thread>

void test_database() {
    std::string db_path = "/tmp/limonitor_unit_test_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());

    Database db(db_path);
    ASSERT(db.open(), "Database::open temp file");

    db.set_setting("theme", "dark");
    ASSERT(db.get_setting("theme") == "dark", "get_setting theme");

    db.set_setting("verbose", "1");
    ASSERT(db.get_setting("verbose") == "1", "get_setting verbose");

    db.set_setting("solar_enabled", "true");
    ASSERT(db.get_setting("solar_enabled") == "true", "get_setting solar_enabled");

    db.set_setting("weather_api_key", "secret");
    ASSERT(db.get_setting("weather_api_key") == "secret", "get_setting weather_api_key");

    ASSERT(db.get_setting("nonexistent") == "", "get_setting nonexistent returns empty");

    remove(db_path.c_str());
}

void test_database_event_logging() {
    std::string db_path = "/tmp/limonitor_db_log_test_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());

    {
        Database db(db_path);
        ASSERT(db.open(), "Database::open for logging test");

        auto events = db.load_system_events(10);
        bool found_open = false;
        for (const auto& ev : events) {
            if (ev.message.find("Database opened") != std::string::npos) {
                found_open = true;
                break;
            }
        }
        ASSERT(found_open, "Event logged for database open");

        db.checkpoint();
        events = db.load_system_events(10);
        bool found_checkpoint = false;
        for (const auto& ev : events) {
            if (ev.message.find("Database WAL checkpoint") != std::string::npos) {
                found_checkpoint = true;
                break;
            }
        }
        ASSERT(found_checkpoint, "Event logged for database checkpoint");

        db.backup("/tmp/limonitor_backup_test.db");
        events = db.load_system_events(10);
        bool found_backup = false;
        for (const auto& ev : events) {
            if (ev.message.find("Database backup created") != std::string::npos) {
                found_backup = true;
                break;
            }
        }
        ASSERT(found_backup, "Event logged for database backup");

        db.cleanup(90, 180, 3650);
        events = db.load_system_events(10);
        bool found_cleanup = false;
        for (const auto& ev : events) {
            if (ev.message.find("Database cleanup and WAL checkpoint") != std::string::npos) {
                found_cleanup = true;
                break;
            }
        }
        ASSERT(found_cleanup, "Event logged for database cleanup");
    }

    remove(db_path.c_str());
    remove("/tmp/limonitor_backup_test.db");
}

void test_database_concurrency() {
    std::string db_path = "/tmp/limonitor_concurrency_test_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());

    Database db(db_path);
    ASSERT(db.open(), "Database::open for concurrency test");

    std::atomic<bool> stop{false};

    auto writer = std::thread([&]() {
        int i = 0;
        while (!stop) {
            BatterySnapshot s;
            s.total_voltage_v = 13.2 + (i % 10) * 0.01;
            db.insert_battery_batch({s});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            i++;
        }
    });

    auto maintenance = std::thread([&]() {
        while (!stop) {
            db.checkpoint();
            db.cleanup(90, 180, 3650);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    writer.join();
    maintenance.join();

    remove(db_path.c_str());
}

void test_database_test_runs() {
    std::string db_path = "/tmp/limonitor_test_runs_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());

    Database db(db_path);
    db.open();

    Database::TestRunRow row;
    row.test_type = "BATTERY_CAPACITY";
    row.start_time = 123456789;
    row.initial_voltage = 13.5;
    row.initial_soc = 95.0;
    row.result = "RUNNING";

    int64_t id = db.insert_test_run(row);
    ASSERT(id > 0, "insert_test_run returns positive ID");

    auto runs = db.load_test_runs(10);
    ASSERT(runs.size() == 1, "load_test_runs returns one run");
    ASSERT(runs[0].id == id, "test run ID matches");
    ASSERT(runs[0].test_type == "BATTERY_CAPACITY", "test run type matches");
    ASSERT(runs[0].result == "RUNNING", "test run result defaults to RUNNING");

    db.update_test_run(id, 123456800, 11, "PASSED", "{\"ok\": true}");
    runs = db.load_test_runs(10);
    ASSERT(runs[0].result == "PASSED", "test run result updated to PASSED");

    remove(db_path.c_str());
}

void test_database_test_telemetry() {
    std::string db_path = "/tmp/limonitor_test_telemetry_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());

    Database db(db_path);
    db.open();

    int64_t test_id = 1;
    std::vector<testing::TestTelemetrySample> samples;
    for (int i = 0; i < 5; ++i) {
        testing::TestTelemetrySample s;
        s.test_id = test_id;
        s.timestamp = 123456789 + i;
        s.battery_voltage = 13.0 - i * 0.1;
        samples.push_back(s);
    }

    db.insert_test_telemetry_batch(samples);
    auto fetched = db.load_test_telemetry(test_id);
    ASSERT(fetched.size() == 5, "get_test_telemetry returns 5 samples");
    ASSERT(fetched[0].battery_voltage == 13.0, "first sample voltage matches");
    ASSERT(fetched[4].battery_voltage == 12.6, "last sample voltage matches");

    remove(db_path.c_str());
}
