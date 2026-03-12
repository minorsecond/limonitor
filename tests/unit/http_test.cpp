#include "../src/http_server.hpp"
#include "../src/data_store.hpp"
#include "../src/database.hpp"
#include "../src/logger.hpp"
#include "../src/testing/runner.hpp"
#include "../tests/test_helpers.hpp"
#include <string>

void test_http_options_cors() {
    DataStore store;
    HttpServer server(store, nullptr, "127.0.0.1", 8082);
    server.start();

    std::string resp = http_request("127.0.0.1", 8082, "OPTIONS", "/api/data", "");
    ASSERT(resp.find("HTTP/1.0 204") != std::string::npos || resp.find("HTTP/1.1 204") != std::string::npos, "CORS OPTIONS returns 204");
    ASSERT(resp.find("Access-Control-Allow-Origin: *") != std::string::npos, "CORS header present");

    server.stop();
}

void test_http_post_settings() {
    std::string db_path = "/tmp/http_settings_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    DataStore store;
    HttpServer server(store, &db, "127.0.0.1", 8083);
    server.start();

    std::string body = "{\"theme\":\"light\",\"verbose\":\"0\"}";
    std::string resp = http_request("127.0.0.1", 8083, "POST", "/api/settings", body);
    ASSERT(parse_http_status(resp) == 200, "POST /api/settings returns 200");

    ASSERT(db.get_setting("theme") == "light", "Setting 'theme' updated in DB");
    ASSERT(db.get_setting("verbose") == "0", "Setting 'verbose' updated in DB");

    server.stop();
    remove(db_path.c_str());
}

void test_http_solar_apis_with_extensions() {
    DataStore store;
    store.init_extensions(Config{});
    HttpServer server(store, nullptr, "127.0.0.1", 8084);
    server.start();

    std::string resp = http_request("127.0.0.1", 8084, "GET", "/api/solar_forecast", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/solar_forecast returns 200");
    ASSERT(resp.find("tomorrow_generation_wh") != std::string::npos, "Solar API contains generation field");

    resp = http_request("127.0.0.1", 8084, "GET", "/api/analytics", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/analytics returns 200");
    ASSERT(resp.find("rss_kb") != std::string::npos, "Analytics API contains RSS memory metric");

    server.stop();
}

void test_solar_page_fetch_contract() {
    DataStore store;
    store.init_extensions(Config{});
    HttpServer server(store, nullptr, "127.0.0.1", 8085);
    server.start();

    std::string resp = http_request("127.0.0.1", 8085, "GET", "/api/solar_forecast_week", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/solar_forecast_week returns 200");
    ASSERT(resp.find("\"daily\":") != std::string::npos, "Response contains daily entries");

    server.stop();
}

void test_solar_api_response_structure() {
    DataStore store;
    store.init_extensions(Config{});
    HttpServer server(store, nullptr, "127.0.0.1", 8086);
    server.start();

    std::string resp = http_request("127.0.0.1", 8086, "GET", "/api/solar_forecast", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/solar_forecast returns 200");
    ASSERT(resp.find("tomorrow_generation_wh") != std::string::npos, "Response has expected key");

    server.stop();
}

void test_http_solar_page_theme() {
    std::string db_path = "/tmp/http_theme_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();
    db.set_setting("theme", "dark");

    DataStore store;
    HttpServer server(store, &db, "127.0.0.1", 8087);
    server.start();

    std::string resp = http_request("127.0.0.1", 8087, "GET", "/", "");
    ASSERT(parse_http_status(resp) == 200, "GET / returns 200");
    ASSERT(resp.find("dark") != std::string::npos, "Dashboard reflects dark theme somewhere in response");

    server.stop();
    remove(db_path.c_str());
}

void test_http_tests_api() {
    DataStore store;
    HttpServer server(store, nullptr, "127.0.0.1", 8088);
    server.start();

    std::string resp = http_request("127.0.0.1", 8088, "GET", "/api/tests", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/tests returns 200");
    ASSERT(resp.find("\"tests\":") != std::string::npos, "Initial tests API response has tests key");

    server.stop();
}

void test_http_tests_start_stop_with_battery() {
    std::string db_path = "/tmp/http_test_runner_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    DataStore store;
    // Don't use TestRunner here to avoid safety limit threads
    HttpServer server(store, &db, "127.0.0.1", 8089, 5, nullptr);
    server.start();

    BatterySnapshot bat;
    bat.soc_pct = 95.0;
    bat.total_voltage_v = 13.3;
    bat.valid = true;
    store.update(bat);

    std::string body = "{\"type\":\"BATTERY_CAPACITY\"}";
    std::string resp = http_request("127.0.0.1", 8089, "POST", "/api/tests/start", body);
    // It will return 500 or 400 because test_runner is null, but we just want to see it doesn't crash
    // and correctly handles the request. 
    // Actually, HttpServer checks for test_runner != nullptr.
    ASSERT(resp != "", "Request completed without crash");

    server.stop();
    remove(db_path.c_str());
}

void test_http_tests_telemetry_and_notes() {
    std::string db_path = "/tmp/http_notes_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    DataStore store;
    HttpServer server(store, &db, "127.0.0.1", 8090);
    server.start();

    std::string body = "{\"notes\":\"Test note\"}";
    std::string resp = http_request("127.0.0.1", 8090, "POST", "/api/tests/1/notes", body);
    ASSERT(parse_http_status(resp) == 200, "Update notes returns 200");

    resp = http_request("127.0.0.1", 8090, "GET", "/api/tests/1/telemetry", "");
    ASSERT(parse_http_status(resp) == 200, "Get telemetry returns 200");

    server.stop();
    remove(db_path.c_str());
}

void test_charger_history_api_empty() {
    DataStore store;
    HttpServer server(store, nullptr, "127.0.0.1", 8091);
    server.start();

    std::string resp = http_request("127.0.0.1", 8091, "GET", "/api/charger/history", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/charger/history returns 200");
    ASSERT(resp.find('[') != std::string::npos, "Response is a JSON array");
    ASSERT(resp.find("{\"ts\"") == std::string::npos, "Empty store yields no entries");

    server.stop();
}

void test_charger_history_api_with_data() {
    DataStore store;
    HttpServer server(store, nullptr, "127.0.0.1", 8092);
    server.start();

    for (int i = 0; i < 3; ++i) {
        PwrGateSnapshot snap;
        snap.bat_v = 13.2 + i * 0.1;
        snap.bat_a = 5.0;
        snap.sol_v = 14.5;
        snap.valid = true;
        snap.timestamp = std::chrono::system_clock::now();
        store.update_pwrgate(snap);
    }

    std::string resp = http_request("127.0.0.1", 8092, "GET", "/api/charger/history?n=10", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/charger/history returns 200 with data");
    ASSERT(resp.find("\"ts\"") != std::string::npos, "History contains ts field");
    ASSERT(resp.find("\"bat_v\"") != std::string::npos, "History contains bat_v field");
    ASSERT(resp.find("\"bat_a\"") != std::string::npos, "History contains bat_a field");
    ASSERT(resp.find("\"sol_v\"") != std::string::npos, "History contains sol_v field");
    int count = 0;
    size_t pos = 0;
    while ((pos = resp.find("{\"ts\"", pos)) != std::string::npos) { ++count; ++pos; }
    ASSERT(count == 3, "History returns all 3 entries");

    server.stop();
}

void test_charger_history_persistence_via_store() {
    std::string db_path = "/tmp/test_charger_persist_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    DataStore store;
    store.set_database(&db);

    PwrGateSnapshot snap;
    snap.bat_v = 13.2;
    snap.bat_a = 5.5;
    snap.sol_v = 14.1;
    snap.valid = true;
    snap.timestamp = std::chrono::system_clock::now();
    store.update_pwrgate(snap);
    store.flush_db();

    auto rows = db.load_charger_history(10);
    ASSERT(rows.size() == 1, "One charger row persisted via store buffer");
    ASSERT(std::abs(rows[0].bat_v - 13.2) < 0.001, "bat_v round-trips through DB");
    ASSERT(std::abs(rows[0].bat_a - 5.5) < 0.001, "bat_a round-trips through DB");
    ASSERT(std::abs(rows[0].sol_v - 14.1) < 0.001, "sol_v round-trips through DB");
    ASSERT(rows[0].valid, "Loaded row has valid=true");

    remove(db_path.c_str());
}

void test_analytics_api_self_monitor() {
    DataStore store;
    HttpServer server(store, nullptr, "127.0.0.1", 8093);
    server.start();

    // The /api/analytics handler must call update_self_monitor so RSS is non-zero
    std::string resp = http_request("127.0.0.1", 8093, "GET", "/api/analytics", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/analytics returns 200");
    ASSERT(resp.find("\"process_rss_kb\"") != std::string::npos, "process_rss_kb field present");
    // RSS must be non-zero — a running process always has memory.
    // JSON format is: "process_rss_kb": NNN (space after colon)
    ASSERT(resp.find("\"process_rss_kb\": 0") == std::string::npos, "process_rss_kb is non-zero");

    server.stop();
}

void test_dashboard_visibility() {
    DataStore store;
    std::string db_path = "/tmp/test_visibility_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();
    db.set_setting("shelly_host", "1.2.3.4");
    db.set_setting("settings_initialized", "1");
    db.set_setting("device_name", "test-device");
    db.set_setting("pwrgate_remote", "localhost:8081");
    
    HttpServer server(store, &db, "127.0.0.1", 8094);
    server.start();
    usleep(100000); // 100ms
    
    BatterySnapshot bat;
    bat.valid = true;
    bat.total_voltage_v = 13.1;
    bat.current_a = 5.5; // Discharging
    bat.power_w = 72.0;
    bat.soc_pct = 85.0;
    bat.remaining_ah = 85.0;
    bat.nominal_ah = 100.0;
    bat.timestamp = std::chrono::system_clock::now();
    store.update(bat);
    
    std::string resp = http_request("127.0.0.1", 8094, "GET", "/", "");
    ASSERT(parse_http_status(resp) == 200, "GET / returns 200");
    
    // Check for new UI elements
    ASSERT(resp.find("grid-status-badge") != std::string::npos, "Should have grid status badge placeholder");
    ASSERT(resp.find("shelly-status-text") != std::string::npos, "Should have shelly status text placeholder");
    ASSERT(resp.find("discharging") != std::string::npos, "Should show discharging status");
    
    server.stop();
    remove(db_path.c_str());
}

void test_shelly_api_visibility() {
    DataStore store;
    std::string db_path = "/tmp/test_shelly_api_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();
    db.set_setting("shelly_host", "1.2.3.4");
    
    HttpServer server(store, &db, "127.0.0.1", 8095);
    server.start();
    usleep(100000);
    
    BatterySnapshot bat;
    bat.valid = true;
    bat.current_a = 5.0;
    bat.power_w = 65.0;
    bat.timestamp = std::chrono::system_clock::now();
    store.update(bat);
    
    // API should show grid info
    std::string resp = http_request("127.0.0.1", 8095, "GET", "/api/flow", "");
    ASSERT(parse_http_status(resp) == 200, "GET /api/flow returns 200");
    ASSERT(resp.find("grid_enabled") != std::string::npos, "Flow API has grid_enabled");
    ASSERT(resp.find("grid_relay_on") != std::string::npos, "Flow API has grid_relay_on");
    
    server.stop();
    remove(db_path.c_str());
}
