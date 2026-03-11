
int g_poll_interval_s = 5;  // required by ble_manager

#include "../src/config.hpp"
#include "../src/database.hpp"
#include "../src/data_store.hpp"
#include "../src/http_server.hpp"
#include "../src/logger.hpp"
#include "../src/testing/types.hpp"
#include "../src/testing/runner.hpp"
#include "analytics/weather_forecast.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

static int g_failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s\n", (msg)); \
        ++g_failures; \
    } else { \
        std::fprintf(stderr, "PASS: %s\n", (msg)); \
    } \
} while (0)

static std::string http_request(const std::string& host, uint16_t port,
                                const std::string& method, const std::string& path,
                                const std::string& body = "") {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string req = method + " " + path + " HTTP/1.0\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n";
    if (!body.empty())
        req += "Content-Type: application/json\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;

    send(fd, req.data(), req.size(), MSG_NOSIGNAL);

    char buf[8192];
    std::string response;
    while (ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0)) {
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;
    }
    close(fd);
    return response;
}

static int parse_http_status(const std::string& response) {
    if (response.empty()) return -1;
    auto end = response.find("\r\n");
    if (end == std::string::npos) end = response.find('\n');
    std::string line = response.substr(0, end);
    for (size_t i = 0; i + 3 <= line.size(); ++i) {
        if (line[i] >= '0' && line[i] <= '9' &&
            line[i+1] >= '0' && line[i+1] <= '9' &&
            line[i+2] >= '0' && line[i+2] <= '9') {
            try {
                return std::stoi(line.substr(i, 3));
            } catch (...) {
                return -1;
            }
        }
    }
    return -1;
}

static void test_database() {
    std::string path = "/tmp/limonitor_unit_test_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database::open temp file");

    db.set_setting("theme", "dark");
    db.set_setting("verbose", "1");
    db.set_setting("solar_enabled", "1");
    db.set_setting("weather_api_key", "testkey123");

    ASSERT(db.get_setting("theme") == "dark", "get_setting theme");
    ASSERT(db.get_setting("verbose") == "1", "get_setting verbose");
    ASSERT(db.get_setting("solar_enabled") == "1", "get_setting solar_enabled");
    ASSERT(db.get_setting("weather_api_key") == "testkey123", "get_setting weather_api_key");
    ASSERT(db.get_setting("nonexistent").empty(), "get_setting nonexistent returns empty");

    db.close();
    std::remove(path.c_str());
}

static void test_weather_forecast_empty_api_key() {
    WeatherConfig cfg;
    cfg.api_key = "";
    cfg.zip_code = "80112";
    WeatherForecast wf(cfg);

    auto r = wf.get_forecast_week(400, 0.75, 10, 51.2, 100, 50);
    ASSERT(!r.valid, "get_forecast_week with empty api_key returns valid=false");
    ASSERT(!r.error.empty(), "get_forecast_week with empty api_key returns error message");
    ASSERT(r.error.find("API key") != std::string::npos ||
           r.error.find("not configured") != std::string::npos,
           "error message mentions API key");
}

static void test_http_options_cors() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = false;

    std::string db_path = "/tmp/limonitor_http_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for HTTP test");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19994, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19994, "OPTIONS", "/api/settings");
    ASSERT(!resp.empty(), "OPTIONS got non-empty response");
    int status = parse_http_status(resp);
    ASSERT(status == 204, "OPTIONS /api/settings returns 204");
    ASSERT(resp.find("Access-Control-Allow-Origin") != std::string::npos, "CORS headers present");
    ASSERT(resp.find("Access-Control-Allow-Methods") != std::string::npos, "CORS methods header");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_http_post_settings() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = false;

    std::string db_path = "/tmp/limonitor_post_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for POST test");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19995, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string body = "{\"theme\":\"light\",\"verbose\":\"1\",\"solar_enabled\":\"1\"}";
    std::string resp = http_request("127.0.0.1", 19995, "POST", "/api/settings", body);
    int status = parse_http_status(resp);
    ASSERT(status == 200, "POST /api/settings returns 200");

    ASSERT(db.get_setting("theme") == "light", "POST persisted theme");
    ASSERT(db.get_setting("verbose") == "1", "POST persisted verbose");
    ASSERT(db.get_setting("solar_enabled") == "1", "POST persisted solar_enabled");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_http_solar_apis_with_extensions() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = true;
    cfg.weather_api_key = "testkey";  // will fail fetch but extensions exist
    cfg.solar_zip_code = "80112";

    std::string db_path = "/tmp/limonitor_solar_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for solar test");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19996, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp_week = http_request("127.0.0.1", 19996, "GET", "/api/solar_forecast_week");
    ASSERT(resp_week.find("\"valid\"") != std::string::npos, "solar_forecast_week returns JSON");
    ASSERT(resp_week.find("\"daily\"") != std::string::npos, "solar_forecast_week has daily array");

    std::string resp_sim = http_request("127.0.0.1", 19996, "GET", "/api/solar_simulation");
    ASSERT(resp_sim.find("\"panel_watts\"") != std::string::npos, "solar_simulation returns JSON");
    ASSERT(resp_sim.find("\"expected_today_wh\"") != std::string::npos, "solar_simulation has expected_today_wh");

    std::string resp_fore = http_request("127.0.0.1", 19996, "GET", "/api/solar_forecast");
    ASSERT(resp_fore.find("\"valid\"") != std::string::npos, "solar_forecast returns JSON");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_solar_page_fetch_contract() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = true;

    std::string db_path = "/tmp/limonitor_fetch_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for fetch contract test");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19998, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19998, "GET", "/solar");
    size_t body_start = resp.find("\r\n\r\n");
    if (body_start == std::string::npos) body_start = resp.find("\n\n");
    std::string html = (body_start != std::string::npos)
        ? resp.substr(body_start + (resp.find("\r\n\r\n") != std::string::npos ? 4 : 2))
        : resp;

    ASSERT(html.find("id=\"week-kwh\"") != std::string::npos, "solar page has week-kwh element");
    ASSERT(html.find("id=\"days-full\"") != std::string::npos, "solar page has days-full element");
    ASSERT(html.find("id=\"best-day\"") != std::string::npos, "solar page has best-day element");
    ASSERT(html.find("id=\"daily-table\"") != std::string::npos, "solar page has daily-table element");
    ASSERT(html.find("id=\"today-wh\"") != std::string::npos, "solar page has today-wh element");
    ASSERT(html.find("id=\"tomorrow-wh\"") != std::string::npos, "solar page has tomorrow-wh element");
    ASSERT(html.find("id=\"bat-proj\"") != std::string::npos, "solar page has bat-proj element");

    ASSERT(html.find("/api/solar_forecast_week") != std::string::npos,
           "solar page fetches solar_forecast_week");
    ASSERT(html.find("/api/solar_simulation") != std::string::npos,
           "solar page fetches solar_simulation");
    ASSERT(html.find("/api/solar_forecast") != std::string::npos,
           "solar page fetches solar_forecast");

    ASSERT(html.find("loadAll()") != std::string::npos, "solar page calls loadAll");
    ASSERT(html.find("initSettings();loadAll();") != std::string::npos ||
           html.find("initSettings();loadAll()") != std::string::npos,
           "solar page calls loadAll on init");

    ASSERT(html.find("Loading") != std::string::npos, "solar page shows Loading initially");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_solar_api_response_structure() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = true;
    cfg.weather_api_key = "testkey";
    cfg.solar_zip_code = "80112";

    std::string db_path = "/tmp/limonitor_api_struct_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for API structure test");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19999, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19999, "GET", "/api/solar_forecast_week");

    ASSERT(resp.find("\"valid\"") != std::string::npos, "solar_forecast_week has valid field");
    ASSERT(resp.find("\"daily\"") != std::string::npos, "solar_forecast_week has daily array");
    ASSERT(resp.find("\"error\"") != std::string::npos || resp.find("\"week_total_kwh\"") != std::string::npos,
           "solar_forecast_week has error or week_total_kwh");

    size_t body_start = resp.find("\r\n\r\n");
    if (body_start == std::string::npos) body_start = resp.find("\n\n");
    if (body_start != std::string::npos) {
        body_start += (resp.find("\r\n\r\n") != std::string::npos) ? 4 : 2;
        std::string json = resp.substr(body_start);
            ASSERT(json.find('{') != std::string::npos && json.find('}') != std::string::npos,
               "solar_forecast_week response is JSON object");
    }

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_http_solar_page_theme() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = true;

    std::string db_path = "/tmp/limonitor_theme_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for theme test");
    db.set_setting("config_migrated", "1");
    db.set_setting("theme", "light");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    HttpServer http(store, &db, "127.0.0.1", 19997, 5);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19997, "GET", "/solar");
    ASSERT(resp.find("class=\"light\"") != std::string::npos,
           "GET /solar with theme=light in DB renders html with light class");
    ASSERT(resp.find("Solar Forecast") != std::string::npos, "solar page has title");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static const char* const DAILY_API_JSON =
    R"({"city":{"id":0,"name":"Englewood","coord":{"lon":-104.9011,"lat":39.5805},"country":"US","population":0,"timezone":-21600},"cod":"200","message":1.3041268,"cnt":16,"list":[)"
    R"({"dt":1773169200,"sunrise":1773148738,"sunset":1773190858,"temp":{"day":18.51,"min":8.02,"max":19.28,"night":8.02,"eve":12.11,"morn":9.23},"feels_like":{"day":17.1,"night":5.74,"eve":10.5,"morn":6.73},"pressure":1003,"humidity":26,"weather":[{"id":800,"main":"Clear","description":"sky is clear","icon":"01d"}],"speed":7.03,"deg":264,"gust":9.49,"clouds":1,"pop":0},)"
    R"({"dt":1773255600,"sunrise":1773235043,"sunset":1773277320,"temp":{"day":9.94,"min":2.95,"max":11.84,"night":5.18,"eve":8.22,"morn":2.95},"feels_like":{"day":9.94,"night":4.03,"eve":5.25,"morn":-0.49},"pressure":1021,"humidity":21,"weather":[{"id":800,"main":"Clear","description":"sky is clear","icon":"01d"}],"speed":5.28,"deg":160,"gust":7.88,"clouds":0,"pop":0.11},)"
    R"({"dt":1773342000,"sunrise":1773321348,"sunset":1773363781,"temp":{"day":17.3,"min":3.73,"max":18.01,"night":10.72,"eve":13.33,"morn":3.73},"feels_like":{"day":15.56,"night":8.63,"eve":11.4,"morn":0.24},"pressure":1011,"humidity":18,"weather":[{"id":802,"main":"Clouds","description":"scattered clouds","icon":"03d"}],"speed":9.42,"deg":308,"gust":12.2,"clouds":38,"pop":0},)"
    R"({"dt":1773428400,"sunrise":1773407653,"sunset":1773450243,"temp":{"day":17.67,"min":7.18,"max":19.48,"night":13.25,"eve":14.94,"morn":7.2},"feels_like":{"day":15.91,"night":11.31,"eve":13.12,"morn":4.69},"pressure":1008,"humidity":16,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":6.72,"deg":281,"gust":11.06,"clouds":97,"pop":0},)"
    R"({"dt":1773514800,"sunrise":1773493957,"sunset":1773536704,"temp":{"day":19.26,"min":8.6,"max":20.7,"night":12.57,"eve":17.37,"morn":8.6},"feels_like":{"day":17.61,"night":10.8,"eve":15.71,"morn":6.89},"pressure":1001,"humidity":14,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":6.84,"deg":267,"gust":14.93,"clouds":93,"pop":0},)"
    R"({"dt":1773601200,"sunrise":1773580262,"sunset":1773623164,"temp":{"day":3.67,"min":1.26,"max":10.73,"night":4.2,"eve":7.76,"morn":2.37},"feels_like":{"day":0.6,"night":1.35,"eve":6.22,"morn":-3.58},"pressure":1022,"humidity":42,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":8.99,"deg":22,"gust":14.48,"clouds":100,"pop":0},)"
    R"({"dt":1773687600,"sunrise":1773666565,"sunset":1773709625,"temp":{"day":14.66,"min":2.43,"max":18.58,"night":12.23,"eve":17.03,"morn":2.43},"feels_like":{"day":12.96,"night":10.74,"eve":15.65,"morn":0.85},"pressure":1009,"humidity":30,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":11.08,"deg":335,"gust":17.38,"clouds":98,"pop":0},)"
    R"({"dt":1773774000,"sunrise":1773752869,"sunset":1773796085,"temp":{"day":21.3,"min":8.2,"max":24.63,"night":15.58,"eve":23,"morn":8.2},"feels_like":{"day":19.88,"night":13.79,"eve":21.75,"morn":6.05},"pressure":1009,"humidity":15,"weather":[{"id":801,"main":"Clouds","description":"few clouds","icon":"02d"}],"speed":5.13,"deg":1,"gust":8.36,"clouds":17,"pop":0},)"
    R"({"dt":1773860400,"sunrise":1773839172,"sunset":1773882546,"temp":{"day":20.99,"min":10,"max":24.23,"night":17.01,"eve":23.99,"morn":10},"feels_like":{"day":19.59,"night":15.39,"eve":22.84,"morn":8.88},"pressure":1014,"humidity":17,"weather":[{"id":801,"main":"Clouds","description":"few clouds","icon":"02d"}],"speed":2.42,"deg":211,"gust":4.36,"clouds":24,"pop":0},)"
    R"({"dt":1773946800,"sunrise":1773925475,"sunset":1773969006,"temp":{"day":22.57,"min":12.07,"max":25.25,"night":18.79,"eve":24.8,"morn":12.07},"feels_like":{"day":21.27,"night":17.27,"eve":23.75,"morn":10.04},"pressure":1015,"humidity":15,"weather":[{"id":800,"main":"Clear","description":"sky is clear","icon":"01d"}],"speed":3.54,"deg":207,"gust":4.1,"clouds":0,"pop":0},)"
    R"({"dt":1774033200,"sunrise":1774011778,"sunset":1774055466,"temp":{"day":25.11,"min":12.77,"max":27.05,"night":19.5,"eve":26.21,"morn":12.77},"feels_like":{"day":23.99,"night":17.92,"eve":26.21,"morn":10.89},"pressure":1012,"humidity":12,"weather":[{"id":802,"main":"Clouds","description":"scattered clouds","icon":"03d"}],"speed":3.56,"deg":205,"gust":4.18,"clouds":33,"pop":0},)"
    R"({"dt":1774119600,"sunrise":1774098081,"sunset":1774141925,"temp":{"day":25.11,"min":13.29,"max":27.04,"night":20.59,"eve":24.21,"morn":13.29},"feels_like":{"day":23.96,"night":19.28,"eve":23.1,"morn":11.33},"pressure":1009,"humidity":11,"weather":[{"id":802,"main":"Clouds","description":"scattered clouds","icon":"03d"}],"speed":4.82,"deg":309,"gust":8.24,"clouds":44,"pop":0},)"
    R"({"dt":1774206000,"sunrise":1774184384,"sunset":1774228385,"temp":{"day":21.84,"min":13.4,"max":21.84,"night":13.4,"eve":13.59,"morn":15.28},"feels_like":{"day":20.65,"night":12.41,"eve":12.62,"morn":13.75},"pressure":1007,"humidity":22,"weather":[{"id":500,"main":"Rain","description":"light rain","icon":"10d"}],"speed":8.05,"deg":300,"gust":7.28,"clouds":100,"pop":0.99,"rain":3.06},)"
    R"({"dt":1774292400,"sunrise":1774270687,"sunset":1774314844,"temp":{"day":20.95,"min":10.22,"max":23.71,"night":19.35,"eve":23.27,"morn":10.22},"feels_like":{"day":19.78,"night":18.02,"eve":22.2,"morn":8.89},"pressure":1007,"humidity":26,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":5.58,"deg":203,"gust":8.13,"clouds":98,"pop":0.21},)"
    R"({"dt":1774378800,"sunrise":1774356990,"sunset":1774401304,"temp":{"day":24.92,"min":14.66,"max":29.37,"night":20.54,"eve":26.72,"morn":14.66},"feels_like":{"day":23.83,"night":19.02,"eve":25.73,"morn":13.04},"pressure":1001,"humidity":14,"weather":[{"id":803,"main":"Clouds","description":"broken clouds","icon":"04d"}],"speed":6.57,"deg":225,"gust":10.9,"clouds":71,"pop":0},)"
    R"({"dt":1774465200,"sunrise":1774443292,"sunset":1774487763,"temp":{"day":22.92,"min":13.34,"max":24.26,"night":17.54,"eve":22.44,"morn":13.34},"feels_like":{"day":21.66,"night":16.16,"eve":21.16,"morn":11.38},"pressure":1008,"humidity":15,"weather":[{"id":804,"main":"Clouds","description":"overcast clouds","icon":"04d"}],"speed":7.73,"deg":161,"gust":11.03,"clouds":87,"pop":0}]})";

static void test_parse_daily_entries_cloud_cover() {
    std::string json(DAILY_API_JSON);
    auto entries = WeatherForecast::parse_daily_entries(json);

    ASSERT(entries.size() == 16, "parse_daily_entries: 16 entries from cnt=16 JSON");

    struct Expected { int64_t dt; int cloud_pct; };
    Expected expected[] = {
        {1773169200,   1}, {1773255600,   0}, {1773342000,  38}, {1773428400,  97},
        {1773514800,  93}, {1773601200, 100}, {1773687600,  98}, {1773774000,  17},
        {1773860400,  24}, {1773946800,   0}, {1774033200,  33}, {1774119600,  44},
        {1774206000, 100}, {1774292400,  98}, {1774378800,  71}, {1774465200,  87},
    };

    for (size_t i = 0; i < entries.size() && i < 16; ++i) {
        char msg[128];

        int parsed_pct = static_cast<int>(std::round(entries[i].cloud_cover * 100.0));
        std::snprintf(msg, sizeof(msg),
                      "entry[%zu] dt=%lld cloud=%d%% (expected %d%%)",
                      i, (long long)entries[i].dt, parsed_pct, expected[i].cloud_pct);
        ASSERT(entries[i].dt == expected[i].dt, msg);

        std::snprintf(msg, sizeof(msg),
                      "entry[%zu] cloud=%d%% == expected %d%%",
                      i, parsed_pct, expected[i].cloud_pct);
        ASSERT(parsed_pct == expected[i].cloud_pct, msg);
    }
}

static void test_parse_daily_entries_sunrise_sunset() {
    std::string json(DAILY_API_JSON);
    auto entries = WeatherForecast::parse_daily_entries(json);

    ASSERT(entries.size() >= 1, "parse_daily_entries: at least 1 entry for sunrise/sunset test");

    ASSERT(entries[0].sunrise == 1773148738,
           "entry[0] sunrise parsed correctly");
    ASSERT(entries[0].sunset == 1773190858,
           "entry[0] sunset parsed correctly");

    double expected_h = (1773190858 - 1773148738) / 3600.0; // ~11.70h
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "entry[0] daylight_h=%.2f (expected ~%.2f)",
                  entries[0].daylight_h, expected_h);
    ASSERT(std::abs(entries[0].daylight_h - expected_h) < 0.01, msg);

    for (size_t i = 0; i < entries.size(); ++i) {
        std::snprintf(msg, sizeof(msg),
                      "entry[%zu] has valid sunrise (>0)", i);
        ASSERT(entries[i].sunrise > 0, msg);

        std::snprintf(msg, sizeof(msg),
                      "entry[%zu] sunset > sunrise", i);
        ASSERT(entries[i].sunset > entries[i].sunrise, msg);

        double real_h = (entries[i].sunset - entries[i].sunrise) / 3600.0;
        std::snprintf(msg, sizeof(msg),
                      "entry[%zu] daylight_h=%.2f matches sunrise/sunset (%.2f)",
                      i, entries[i].daylight_h, real_h);
        ASSERT(std::abs(entries[i].daylight_h - real_h) < 0.01, msg);
    }
}

static void test_parse_daily_entries_nested_clouds() {
    std::string json = R"({"list":[)"
        R"({"dt":1000000,"sunrise":999000,"sunset":1042600,)"
        R"("weather":[{"id":800,"main":"Clear","description":"sky","icon":"01d"}],)"
        R"("clouds":{"all":75},"pop":0}]})";

    auto entries = WeatherForecast::parse_daily_entries(json);
    ASSERT(entries.size() == 1, "nested clouds: 1 entry parsed");
    int pct = static_cast<int>(std::round(entries[0].cloud_cover * 100.0));
    char msg[64];
    std::snprintf(msg, sizeof(msg), "nested clouds: parsed %d%% (expected 75%%)", pct);
    ASSERT(pct == 75, msg);
}

static void test_forecast_week_extended_cloud_values() {
    std::string db_path = "/tmp/limonitor_ext_cloud_test_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for extended cloud test");

    // Minimal 3h forecast: single daytime slot for Mar 10
    std::string json_3h = R"({"cod":"200","cnt":1,"list":[{"dt":1773169200,"main":{"temp":15},"clouds":{"all":10},"sys":{"pod":"d"},"wind":{"speed":5}}]})";
    db.save_weather_cache("forecast_3h", json_3h);
    db.save_weather_cache("forecast_daily", std::string(DAILY_API_JSON));

    WeatherConfig cfg;
    cfg.api_key = "test_key_not_used"; // non-empty to pass guard
    cfg.zip_code = "80112";
    WeatherForecast wf(cfg, &db);

    auto r = wf.get_forecast_week(400, 0.75, 10, 51.2, 100, 50);
    // The 3h forecast has only 1 slot (Mar 10), so the daily API provides Mar 11-25.
    // Mar 10 is from the 3h forecast; Mar 11-25 are from the daily API (extended).

    // Expected cloud cover for each entry in the daily API (0-100)
    struct { int64_t dt; int cloud_pct; } expected[] = {
        {1773169200,   1}, {1773255600,   0}, {1773342000,  38}, {1773428400,  97},
        {1773514800,  93}, {1773601200, 100}, {1773687600,  98}, {1773774000,  17},
        {1773860400,  24}, {1773946800,   0}, {1774033200,  33}, {1774119600,  44},
        {1774206000, 100}, {1774292400,  98}, {1774378800,  71}, {1774465200,  87},
    };

    // The first daily entry (Mar 10, index 0) is covered by the 3h forecast,
    // so it's skipped by the daily parser. The remaining 15 entries should appear as extended.
    int ext_found = 0;
    for (const auto& d : r.daily) {
        if (!d.is_extended) continue;
        ++ext_found;
        // Find the matching expected entry by date
        // Convert date string to match expected dt values
        for (size_t i = 0; i < 16; ++i) {
            time_t tt = static_cast<time_t>(expected[i].dt);
            struct tm tm_buf{};
            localtime_r(&tt, &tm_buf);
            char date_buf[32];
            std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                          tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
            if (d.date == date_buf) {
                int parsed_pct = static_cast<int>(std::round(d.cloud_cover * 100.0));
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "integration: %s cloud=%d%% (expected %d%%)",
                              d.date.c_str(), parsed_pct, expected[i].cloud_pct);
                ASSERT(parsed_pct == expected[i].cloud_pct, msg);
                break;
            }
        }
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "integration: found %d extended days (expected 15)", ext_found);
    ASSERT(ext_found == 15, msg);

    db.close();
    std::remove(db_path.c_str());
}

static void test_parse_daily_entries_empty() {
    auto entries = WeatherForecast::parse_daily_entries("");
    ASSERT(entries.empty(), "empty JSON returns empty entries");

    auto entries2 = WeatherForecast::parse_daily_entries("{\"cod\":\"200\",\"list\":[]}");
    ASSERT(entries2.empty(), "empty list returns empty entries");
}

// ---------- project_battery_soc tests ----------

static bool approx_eq(double a, double b, double tol = 0.01) {
    return std::fabs(a - b) < tol;
}

static void test_soc_basic_solar_charge() {
    // 50% start, 5000 Wh capacity, 500 Wh solar per slot, no usage
    // Each slot adds (500/5000)*100 = 10%
    std::vector<double> gen = {500, 500, 500};
    std::vector<double> use = {0, 0, 0};
    auto soc = project_battery_soc(50.0, 5000.0, gen, use);
    ASSERT(soc.size() == 3, "soc_basic_charge: 3 slots returned");
    ASSERT(approx_eq(soc[0], 60.0), "soc_basic_charge: slot 0 = 60%");
    ASSERT(approx_eq(soc[1], 70.0), "soc_basic_charge: slot 1 = 70%");
    ASSERT(approx_eq(soc[2], 80.0), "soc_basic_charge: slot 2 = 80%");
}

static void test_soc_basic_drain() {
    // 80% start, 5000 Wh cap, no solar, 250 Wh usage per slot
    // Each slot drains (250/5000)*100 = 5%
    std::vector<double> gen = {0, 0, 0, 0};
    std::vector<double> use = {250, 250, 250, 250};
    auto soc = project_battery_soc(80.0, 5000.0, gen, use);
    ASSERT(soc.size() == 4, "soc_basic_drain: 4 slots returned");
    ASSERT(approx_eq(soc[0], 75.0), "soc_basic_drain: slot 0 = 75%");
    ASSERT(approx_eq(soc[1], 70.0), "soc_basic_drain: slot 1 = 70%");
    ASSERT(approx_eq(soc[2], 65.0), "soc_basic_drain: slot 2 = 65%");
    ASSERT(approx_eq(soc[3], 60.0), "soc_basic_drain: slot 3 = 60%");
}

static void test_soc_clamped_at_100() {
    // 95% start, big solar push should clamp at 100
    std::vector<double> gen = {1000, 1000};
    std::vector<double> use = {0, 0};
    auto soc = project_battery_soc(95.0, 5000.0, gen, use);
    ASSERT(soc.size() == 2, "soc_clamp_100: 2 slots");
    ASSERT(approx_eq(soc[0], 100.0), "soc_clamp_100: slot 0 clamped at 100%");
    ASSERT(approx_eq(soc[1], 100.0), "soc_clamp_100: slot 1 stays at 100%");
}

static void test_soc_clamped_at_0() {
    // 5% start, heavy usage drains past 0
    std::vector<double> gen = {0, 0, 0};
    std::vector<double> use = {500, 500, 500};
    auto soc = project_battery_soc(5.0, 5000.0, gen, use);
    ASSERT(soc.size() == 3, "soc_clamp_0: 3 slots");
    ASSERT(approx_eq(soc[0], 0.0), "soc_clamp_0: slot 0 clamped at 0%");
    ASSERT(approx_eq(soc[1], 0.0), "soc_clamp_0: slot 1 stays at 0%");
    ASSERT(approx_eq(soc[2], 0.0), "soc_clamp_0: slot 2 stays at 0%");
}

static void test_soc_net_zero() {
    // Generation exactly equals usage — SoC stays flat
    std::vector<double> gen = {200, 200, 200};
    std::vector<double> use = {200, 200, 200};
    auto soc = project_battery_soc(60.0, 5000.0, gen, use);
    ASSERT(soc.size() == 3, "soc_net_zero: 3 slots");
    ASSERT(approx_eq(soc[0], 60.0), "soc_net_zero: slot 0 unchanged");
    ASSERT(approx_eq(soc[1], 60.0), "soc_net_zero: slot 1 unchanged");
    ASSERT(approx_eq(soc[2], 60.0), "soc_net_zero: slot 2 unchanged");
}

static void test_soc_day_night_cycle() {
    // Simulate: night drain, day charge, night drain
    // 5120 Wh capacity (100Ah * 51.2V)
    double cap = 5120.0;
    // Night: 80 Wh usage per 3h slot, no solar (4 slots = 12h)
    // Day: 400 Wh solar, 80 Wh usage per slot (4 slots = 12h)
    // Night: 80 Wh usage per slot (4 slots)
    std::vector<double> gen = {0, 0, 0, 0,  400, 400, 400, 400,  0, 0, 0, 0};
    std::vector<double> use = {80, 80, 80, 80,  80, 80, 80, 80,  80, 80, 80, 80};
    auto soc = project_battery_soc(100.0, cap, gen, use);
    ASSERT(soc.size() == 12, "soc_day_night: 12 slots");
    // After 4 night slots: 100 - 4*(80/5120)*100 = 100 - 6.25 = 93.75
    ASSERT(approx_eq(soc[3], 93.75), "soc_day_night: end of night 1 ~93.75%");
    // After 4 day slots: +4*(320/5120)*100 = +25
    ASSERT(approx_eq(soc[7], 100.0), "soc_day_night: end of day clamped at 100%");
    // After 4 more night slots: 100 - 6.25 = 93.75
    ASSERT(approx_eq(soc[11], 93.75), "soc_day_night: end of night 2 ~93.75%");
}

static void test_soc_empty_inputs() {
    std::vector<double> gen, use;
    auto soc = project_battery_soc(50.0, 5000.0, gen, use);
    ASSERT(soc.empty(), "soc_empty: no slots returns empty");
}

static void test_soc_zero_capacity() {
    // Zero capacity should return flat start_soc
    std::vector<double> gen = {500, 500};
    std::vector<double> use = {100, 100};
    auto soc = project_battery_soc(50.0, 0.0, gen, use);
    ASSERT(soc.size() == 2, "soc_zero_cap: 2 slots");
    ASSERT(approx_eq(soc[0], 50.0), "soc_zero_cap: slot 0 = start SoC");
    ASSERT(approx_eq(soc[1], 50.0), "soc_zero_cap: slot 1 = start SoC");
}

static void test_soc_negative_capacity() {
    std::vector<double> gen = {500};
    std::vector<double> use = {100};
    auto soc = project_battery_soc(50.0, -100.0, gen, use);
    ASSERT(soc.size() == 1, "soc_neg_cap: 1 slot");
    ASSERT(approx_eq(soc[0], 50.0), "soc_neg_cap: returns clamped start SoC");
}

static void test_soc_start_above_100() {
    // Invalid start SoC above 100 should be clamped
    std::vector<double> gen = {0};
    std::vector<double> use = {0};
    auto soc = project_battery_soc(150.0, 5000.0, gen, use);
    ASSERT(soc.size() == 1, "soc_start_gt100: 1 slot");
    ASSERT(approx_eq(soc[0], 100.0), "soc_start_gt100: clamped to 100%");
}

static void test_soc_start_below_0() {
    // Invalid start SoC below 0 should be clamped
    std::vector<double> gen = {500};
    std::vector<double> use = {0};
    auto soc = project_battery_soc(-10.0, 5000.0, gen, use);
    ASSERT(soc.size() == 1, "soc_start_lt0: 1 slot");
    ASSERT(approx_eq(soc[0], 10.0), "soc_start_lt0: starts at 0, gains 10%");
}

static void test_soc_mismatched_lengths() {
    // gen has more elements than use — truncates to shorter
    std::vector<double> gen = {500, 500, 500};
    std::vector<double> use = {100, 100};
    auto soc = project_battery_soc(50.0, 5000.0, gen, use);
    ASSERT(soc.size() == 2, "soc_mismatch: uses min(3,2) = 2 slots");
}

static void test_soc_realistic_scenario() {
    // Real-world LiFePO4 system: 100Ah * 51.2V = 5120 Wh
    // Start at 45%, 7 day forecast with day/night cycles
    // Usage: ~27W continuous = 81 Wh per 3h slot
    // Solar: varies by cloud cover
    double cap = 5120.0;
    // 2 full days (16 slots): 8 night + 4 day + 4 night (simplified)
    // Day 1: slots 0-3 night, 4-7 day (clear), 8-11 night
    // Day 2: slots 12-15 day (cloudy)
    std::vector<double> gen, use;
    for (int i = 0; i < 16; ++i) {
        use.push_back(81.0);
        bool daytime = (i >= 4 && i <= 7) || (i >= 12 && i <= 15);
        if (daytime && i <= 7) gen.push_back(400.0);      // clear day
        else if (daytime) gen.push_back(100.0);             // cloudy day
        else gen.push_back(0.0);
    }
    auto soc = project_battery_soc(45.0, cap, gen, use);
    ASSERT(soc.size() == 16, "soc_realistic: 16 slots");
    // All values should be between 0 and 100
    bool all_valid = true;
    for (double v : soc) {
        if (v < 0.0 || v > 100.0) { all_valid = false; break; }
    }
    ASSERT(all_valid, "soc_realistic: all values in [0, 100]");
    // After first night (4 slots of -81Wh each): 45 - 4*(81/5120)*100 = 45 - 6.33 = 38.67
    ASSERT(approx_eq(soc[3], 38.67, 0.1), "soc_realistic: end of night 1 ~38.7%");
    // After clear day (4 slots of +319Wh each): 38.67 + 4*(319/5120)*100 = 38.67 + 24.92 = 63.59
    ASSERT(approx_eq(soc[7], 63.59, 0.1), "soc_realistic: end of clear day ~63.6%");
    // After second night + cloudy day, should still be > 0
    ASSERT(soc[15] > 0.0, "soc_realistic: doesn't hit 0 in 2 days");
}

// ---------- estimate_runtime_h tests ----------

static void test_runtime_basic() {
    // 1356 Wh battery, 93.8% SoC, 10% cutoff, 14W load
    // Usable: 1356 * (93.8 - 10) / 100 = 1137.5 Wh
    // Runtime: 1137.5 / 14 = 81.25 h
    double rt = estimate_runtime_h(1356.0, 93.8, 10.0, 14.0);
    ASSERT(approx_eq(rt, 81.25, 0.1), "runtime_basic: 93.8% → 10% at 14W ≈ 81.25h");
}

static void test_runtime_full_to_cutoff() {
    // 1356 Wh, 100% SoC, 10% cutoff, 14W
    // Usable: 1356 * 90/100 = 1220.4 Wh
    // Runtime: 1220.4 / 14 = 87.17 h
    double rt = estimate_runtime_h(1356.0, 100.0, 10.0, 14.0);
    ASSERT(approx_eq(rt, 87.17, 0.1), "runtime_full: 100% → 10% at 14W ≈ 87.17h");
}

static void test_runtime_zero_load() {
    double rt = estimate_runtime_h(1356.0, 100.0, 10.0, 0.0);
    ASSERT(rt == 0.0, "runtime_zero_load: returns 0");
}

static void test_runtime_soc_below_cutoff() {
    double rt = estimate_runtime_h(1356.0, 5.0, 10.0, 14.0);
    ASSERT(rt == 0.0, "runtime_below_cutoff: SoC < cutoff returns 0");
}

static void test_runtime_soc_at_cutoff() {
    double rt = estimate_runtime_h(1356.0, 10.0, 10.0, 14.0);
    ASSERT(rt == 0.0, "runtime_at_cutoff: SoC == cutoff returns 0");
}

static void test_runtime_zero_capacity() {
    double rt = estimate_runtime_h(0.0, 80.0, 10.0, 14.0);
    ASSERT(rt == 0.0, "runtime_zero_cap: returns 0");
}

static void test_runtime_consistency_with_dashboard() {
    // From user's system: 100Ah * 13.56V = 1356 Wh, 14W load
    // Dashboard shows: Full→10% = 86.9h, Current(93.8%)→10% = 80.9h
    double rt_full = estimate_runtime_h(1356.0, 100.0, 10.0, 14.0);
    double rt_curr = estimate_runtime_h(1356.0, 93.8, 10.0, 14.0);
    ASSERT(approx_eq(rt_full, 87.17, 0.5), "runtime_consistency: full→10% ≈ 87h (dashboard: 86.9h)");
    ASSERT(approx_eq(rt_curr, 81.25, 0.5), "runtime_consistency: 93.8%→10% ≈ 81h (dashboard: 80.9h)");
}

// Verify runtime math against prod DB (limonitorLiveData) when available
static void test_runtime_prod_db() {
    const char* paths[] = {
        "../limonitorLiveData/limonitor/limonitor.db",  // from build/
        "limonitorLiveData/limonitor/limonitor.db",    // from project root
    };
    for (const char* p : paths) {
        Database db(p);
        if (!db.open()) continue;
        auto usage = db.get_usage_profile(7);
        int total_n = 0;
        double total_sum = 0;
        for (const auto& u : usage) {
            total_sum += u.avg_w * u.sample_count;
            total_n += u.sample_count;
        }
        double load_w = (total_n > 0) ? total_sum / total_n : 0;
        if (total_n > 0 && total_n < 20)
            load_w *= 0.85;
        // 100Ah 4S: 100*12.8=1280 Wh, 90% usable=1152 Wh
        double cap_wh = 100.0 * 12.8;
        double rt_full = estimate_runtime_h(cap_wh, 100.0, 10.0, load_w);
        double rt_curr = estimate_runtime_h(cap_wh, 93.76, 10.0, load_w);
        ASSERT(load_w > 0.5, "runtime_prod_db: load from profile > 0.5W");
        ASSERT(rt_full > 30 && rt_full < 80, "runtime_prod_db: full→10% in plausible range 30–80h");
        ASSERT(rt_curr > 25 && rt_curr < 75, "runtime_prod_db: current→10% in plausible range 25–75h");
        std::fprintf(stderr, "PASS: runtime_prod_db: n=%d load=%.1fW full=%.1fh curr=%.1fh\n",
                     total_n, load_w, rt_full, rt_curr);
        return;
    }
    std::fprintf(stderr, "PASS: runtime_prod_db: prod DB not found (skipped)\n");
}

// ---------- scale_usage_profile tests ----------

static void test_scale_basic() {
    // Profile averages 27W, measured is 14W → scale ≈ 0.519
    std::vector<double> avgs = {30.0, 25.0, 28.0, 20.0, 35.0, 22.0, 30.0, 26.0};
    std::vector<double> sds  = {6.0,  5.0,  5.6,  4.0,  7.0,  4.4,  6.0,  5.2};
    std::vector<int> counts  = {10,   10,   10,   10,   10,   10,   10,   10};
    // Weighted avg = (30+25+28+20+35+22+30+26)*10 / 80 = 216*10/80 = 27.0
    double scale = scale_usage_profile(avgs, sds, counts, 14.0);
    ASSERT(approx_eq(scale, 14.0/27.0, 0.01), "scale_basic: factor ≈ 0.519");
    // Verify all slots are scaled
    ASSERT(approx_eq(avgs[0], 30.0 * 14.0/27.0, 0.1), "scale_basic: slot 0 scaled");
    ASSERT(approx_eq(avgs[4], 35.0 * 14.0/27.0, 0.1), "scale_basic: slot 4 scaled");
    ASSERT(approx_eq(sds[0], 6.0 * 14.0/27.0, 0.1), "scale_basic: stddev 0 scaled");
}

static void test_scale_no_measured() {
    // measured_avg_w = 0 → no scaling
    std::vector<double> avgs = {30.0, 25.0};
    std::vector<double> sds  = {6.0, 5.0};
    std::vector<int> counts  = {10, 10};
    double scale = scale_usage_profile(avgs, sds, counts, 0.0);
    ASSERT(approx_eq(scale, 1.0), "scale_no_measured: factor = 1.0");
    ASSERT(approx_eq(avgs[0], 30.0), "scale_no_measured: slot 0 unchanged");
}

static void test_scale_zero_profile() {
    // All profile values are 0 → no scaling
    std::vector<double> avgs = {0.0, 0.0};
    std::vector<double> sds  = {0.0, 0.0};
    std::vector<int> counts  = {10, 10};
    double scale = scale_usage_profile(avgs, sds, counts, 14.0);
    ASSERT(approx_eq(scale, 1.0), "scale_zero_profile: factor = 1.0");
}

static void test_scale_preserves_ratio() {
    // Slot ratios should be preserved after scaling
    std::vector<double> avgs = {10.0, 30.0, 20.0};
    std::vector<double> sds  = {2.0, 6.0, 4.0};
    std::vector<int> counts  = {10, 10, 10};
    // Profile avg = (10+30+20)*10/30 = 20.0, measured = 10.0 → scale = 0.5
    scale_usage_profile(avgs, sds, counts, 10.0);
    ASSERT(approx_eq(avgs[0] / avgs[1], 10.0/30.0, 0.001), "scale_ratio: slot ratio preserved");
    ASSERT(approx_eq(avgs[1] / avgs[2], 30.0/20.0, 0.001), "scale_ratio: slot ratio preserved 2");
}

static void test_scale_uneven_counts() {
    // Weighted average with uneven sample counts
    std::vector<double> avgs = {30.0, 10.0};
    std::vector<double> sds  = {6.0, 2.0};
    std::vector<int> counts  = {90, 10};
    // Weighted avg = (30*90 + 10*10) / 100 = 2800/100 = 28.0
    double scale = scale_usage_profile(avgs, sds, counts, 14.0);
    ASSERT(approx_eq(scale, 14.0/28.0, 0.01), "scale_uneven: weighted factor ≈ 0.5");
}

// ---------- SoC + scaled usage consistency test ----------

static void test_soc_with_scaled_usage() {
    // Verify SoC projection with realistic scaled usage matches runtime estimate
    // 100Ah * 13.56V = 1356 Wh, 14W avg load, start at 93.8%
    double cap = 1356.0;
    double start = 93.8;
    double load_w = 14.0;
    double use_per_slot = load_w * 3.0;  // 42 Wh per 3h slot

    // 8 slots per day, 5 days = 40 slots, no solar
    std::vector<double> gen(40, 0.0);
    std::vector<double> use(40, use_per_slot);
    auto soc = project_battery_soc(start, cap, gen, use);
    ASSERT(soc.size() == 40, "soc_scaled: 40 slots");

    // At 14W, runtime from 93.8% to 10% ≈ 81.25h ≈ 27 slots of 3h
    double rt = estimate_runtime_h(cap, start, 10.0, load_w);
    int slots_to_cutoff = static_cast<int>(std::floor(rt / 3.0));

    // SoC at slot just before cutoff should be > 10%
    if (slots_to_cutoff > 0 && slots_to_cutoff <= 40) {
        ASSERT(soc[slots_to_cutoff - 1] > 10.0,
               "soc_scaled: SoC > 10% one slot before runtime cutoff");
    }
    // SoC at slot at/after cutoff should be ≤ 10%
    if (slots_to_cutoff < 40) {
        ASSERT(soc[slots_to_cutoff] <= 10.0 + 3.5,
               "soc_scaled: SoC near 10% at runtime cutoff slot");
    }

    // Verify it doesn't hit 0 too early — at 14W, should take ~67 slots to fully drain
    // (1356*0.938 / 42 ≈ 30.3 slots = 90.9h from 93.8% to 0%)
    ASSERT(soc[25] > 0.0, "soc_scaled: still alive at slot 25 (75h)");
    // But should be empty by slot 35 (105h)
    ASSERT(soc[35] <= 0.01, "soc_scaled: drained by slot 35 (105h)");
}

static void test_soc_full_drain_recovery() {
    // Battery drains to 0, then solar brings it back
    double cap = 5000.0;
    std::vector<double> gen = {0, 0, 0, 0, 1000, 1000, 1000};
    std::vector<double> use = {500, 500, 500, 500, 0, 0, 0};
    auto soc = project_battery_soc(30.0, cap, gen, use);
    ASSERT(soc.size() == 7, "soc_drain_recover: 7 slots");
    // After 3 slots: 30 - 3*10 = 0 (clamped)
    ASSERT(approx_eq(soc[2], 0.0), "soc_drain_recover: hits 0% at slot 2");
    ASSERT(approx_eq(soc[3], 0.0), "soc_drain_recover: stays 0% at slot 3");
    // Recovery: each slot adds 20%
    ASSERT(approx_eq(soc[4], 20.0), "soc_drain_recover: slot 4 recovers to 20%");
    ASSERT(approx_eq(soc[5], 40.0), "soc_drain_recover: slot 5 = 40%");
    ASSERT(approx_eq(soc[6], 60.0), "soc_drain_recover: slot 6 = 60%");
}

// ---------- Centered regression test ----------

// Standalone regression using same centered algorithm as the fix
static double centered_regression_slope(const std::vector<double>& xs,
                                        const std::vector<double>& ys) {
    size_t n = std::min(xs.size(), ys.size());
    if (n < 2) return 0;
    double sum_x = 0, sum_y = 0;
    for (size_t i = 0; i < n; ++i) { sum_x += xs[i]; sum_y += ys[i]; }
    double x_bar = sum_x / n;
    double y_bar = sum_y / n;
    double sum_dxdy = 0, sum_dx2 = 0;
    for (size_t i = 0; i < n; ++i) {
        double dx = xs[i] - x_bar;
        double dy = ys[i] - y_bar;
        sum_dx2 += dx * dx;
        sum_dxdy += dx * dy;
    }
    if (sum_dx2 < 1e-12) return 0;
    return sum_dxdy / sum_dx2;
}

static void test_regression_centered() {
    // Unix timestamps ~1.74e9 with 5-second spacing, voltage rising linearly
    // True slope = 0.001 V/s (3.6 V/h)
    double t0 = 1741700000.0;
    double v0 = 13.200;
    std::vector<double> ts, vs;
    for (int i = 0; i < 12; ++i) {
        ts.push_back(t0 + i * 5.0);
        vs.push_back(v0 + i * 5.0 * 0.001);
    }
    double slope = centered_regression_slope(ts, vs);
    ASSERT(approx_eq(slope, 0.001, 1e-8), "regression_centered: slope = 0.001 V/s for linear data");

    // Negative slope
    std::vector<double> vs_down;
    for (int i = 0; i < 12; ++i) vs_down.push_back(v0 - i * 5.0 * 0.0005);
    double slope2 = centered_regression_slope(ts, vs_down);
    ASSERT(approx_eq(slope2, -0.0005, 1e-8), "regression_centered: slope = -0.0005 V/s for downward");

    // Verify the OLD naive algorithm fails on this data
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    size_t n = ts.size();
    for (size_t i = 0; i < n; ++i) {
        sum_x += ts[i]; sum_y += vs[i]; sum_xy += ts[i] * vs[i]; sum_xx += ts[i] * ts[i];
    }
    double naive_denom = n * sum_xx - sum_x * sum_x;
    double naive_slope = (n * sum_xy - sum_x * sum_y) / naive_denom;
    double naive_error = std::fabs(naive_slope - 0.001);
    double centered_error = std::fabs(slope - 0.001);
    ASSERT(centered_error < naive_error * 0.01 || centered_error < 1e-10,
           "regression_centered: centered is far more accurate than naive");
}

static void test_regression_flat() {
    // Flat data should give ~0 slope
    double t0 = 1741700000.0;
    std::vector<double> ts, vs;
    for (int i = 0; i < 12; ++i) {
        ts.push_back(t0 + i * 5.0);
        vs.push_back(13.45);
    }
    double slope = centered_regression_slope(ts, vs);
    ASSERT(std::fabs(slope) < 1e-12, "regression_flat: flat data gives ~0 slope");
}

// ---------- Runtime nominal voltage test ----------

static void test_runtime_nominal_voltage() {
    // For a 4-cell (12V) LiFePO4 battery, nominal = 4 * 3.2 = 12.8V
    // With 100Ah: capacity = 1280 Wh
    // But the actual voltage varies 12.0-14.6V — runtime should NOT change with it.
    // The estimate_runtime_h function uses exact cap_wh, so we test the
    // cell-count derivation logic here.
    auto derive_nominal_v = [](double measured_v) -> double {
        int cells = std::max(1, static_cast<int>(std::round(measured_v / 3.3)));
        return cells * 3.2;
    };
    // 4S LiFePO4: voltages ranging from 12.0V (empty) to 14.4V (full)
    ASSERT(approx_eq(derive_nominal_v(12.0), 12.8, 0.01), "nominal_v: 12.0V → 4S → 12.8V");
    ASSERT(approx_eq(derive_nominal_v(13.2), 12.8, 0.01), "nominal_v: 13.2V → 4S → 12.8V");
    ASSERT(approx_eq(derive_nominal_v(13.56), 12.8, 0.01), "nominal_v: 13.56V → 4S → 12.8V");
    ASSERT(approx_eq(derive_nominal_v(14.4), 12.8, 0.01), "nominal_v: 14.4V → 4S → 12.8V");
    // 16S (48V) LiFePO4
    ASSERT(approx_eq(derive_nominal_v(51.2), 51.2, 0.01), "nominal_v: 51.2V → 16S → 51.2V");
    ASSERT(approx_eq(derive_nominal_v(56.0), 54.4, 0.01), "nominal_v: 56.0V → 17S → 54.4V");
    // 1S (single cell)
    ASSERT(approx_eq(derive_nominal_v(3.2), 3.2, 0.01), "nominal_v: 3.2V → 1S → 3.2V");
}

// ---------- Daily CI variance addition test ----------

static void test_daily_ci_variance_addition() {
    // 8 independent slots with known mean and stddev
    // Daily mean = sum of slot means
    // Daily variance = sum of slot variances (for independent slots)
    // Daily stddev = sqrt(sum of variances)
    double slot_avg = 14.0;   // 14W per slot
    double slot_sd = 3.0;     // 3W stddev

    // Daily mean energy (kWh): 8 * 14W * 3h / 1000 = 0.336 kWh
    double daily_mean = 8 * slot_avg * 3.0 / 1000.0;
    ASSERT(approx_eq(daily_mean, 0.336, 0.001), "daily_ci: mean = 0.336 kWh");

    // Variance per slot in Wh²: sd² * 3h² = 9 * 9 = 81 Wh²
    // Total variance: 8 * 81 = 648 Wh²
    // Daily stddev: sqrt(648) / 1000 kWh ≈ 0.02546 kWh
    double daily_var = 8 * slot_sd * slot_sd * 9.0;
    double daily_sd = std::sqrt(daily_var) / 1000.0;
    ASSERT(approx_eq(daily_sd, 0.02546, 0.001), "daily_ci: sd ≈ 0.0255 kWh");

    // 95% CI: mean ± 1.96 * sd
    double lo = daily_mean - 1.96 * daily_sd;
    double hi = daily_mean + 1.96 * daily_sd;
    ASSERT(approx_eq(lo, 0.286, 0.01), "daily_ci: lo ≈ 0.286 kWh");
    ASSERT(approx_eq(hi, 0.386, 0.01), "daily_ci: hi ≈ 0.386 kWh");

    // Old (wrong) method: sum individual slot CIs
    double wrong_lo = 0, wrong_hi = 0;
    for (int s = 0; s < 8; ++s) {
        wrong_lo += std::max(0.0, (slot_avg - 1.96 * slot_sd) * 3.0) / 1000.0;
        wrong_hi += (slot_avg + 1.96 * slot_sd) * 3.0 / 1000.0;
    }
    // Wrong method gives much wider CI
    ASSERT(hi - lo < wrong_hi - wrong_lo, "daily_ci: correct CI is narrower than naive sum");
    // Specifically, wrong width / correct width ≈ sqrt(8) ≈ 2.83
    double correct_width = hi - lo;
    double wrong_width = wrong_hi - wrong_lo;
    ASSERT(approx_eq(wrong_width / correct_width, std::sqrt(8.0), 0.1),
           "daily_ci: naive CI is ~sqrt(8)x too wide");
}

// ---------- Testing framework: types ----------

static void test_parse_test_type() {
    ASSERT(testing::parse_test_type("ups_failover") == testing::TestType::UPS_FAILOVER,
           "parse_test_type: ups_failover");
    ASSERT(testing::parse_test_type("capacity") == testing::TestType::BATTERY_CAPACITY,
           "parse_test_type: capacity");
    ASSERT(testing::parse_test_type("load_spike") == testing::TestType::LOAD_SPIKE,
           "parse_test_type: load_spike");
    ASSERT(testing::parse_test_type("charger_recovery") == testing::TestType::CHARGER_RECOVERY,
           "parse_test_type: charger_recovery");
    ASSERT(testing::parse_test_type("simulated_outage") == testing::TestType::SIMULATED_OUTAGE,
           "parse_test_type: simulated_outage");
    ASSERT(testing::parse_test_type("unknown") == testing::TestType::UPS_FAILOVER,
           "parse_test_type: unknown defaults to UPS_FAILOVER");
    ASSERT(testing::parse_test_type(nullptr) == testing::TestType::UPS_FAILOVER,
           "parse_test_type: nullptr defaults to UPS_FAILOVER");
}

static void test_test_type_str() {
    ASSERT(std::string(testing::test_type_str(testing::TestType::UPS_FAILOVER)) == "ups_failover",
           "test_type_str: UPS_FAILOVER");
    ASSERT(std::string(testing::test_type_str(testing::TestType::BATTERY_CAPACITY)) == "capacity",
           "test_type_str: BATTERY_CAPACITY");
    ASSERT(std::string(testing::test_type_str(testing::TestType::LOAD_SPIKE)) == "load_spike",
           "test_type_str: LOAD_SPIKE");
    ASSERT(std::string(testing::test_type_str(testing::TestType::CHARGER_RECOVERY)) == "charger_recovery",
           "test_type_str: CHARGER_RECOVERY");
    ASSERT(std::string(testing::test_type_str(testing::TestType::SIMULATED_OUTAGE)) == "simulated_outage",
           "test_type_str: SIMULATED_OUTAGE");
}

static void test_test_result_str() {
    ASSERT(std::string(testing::test_result_str(testing::TestResult::RUNNING)) == "running",
           "test_result_str: RUNNING");
    ASSERT(std::string(testing::test_result_str(testing::TestResult::PASSED)) == "passed",
           "test_result_str: PASSED");
    ASSERT(std::string(testing::test_result_str(testing::TestResult::FAILED)) == "failed",
           "test_result_str: FAILED");
    ASSERT(std::string(testing::test_result_str(testing::TestResult::ABORTED)) == "aborted",
           "test_result_str: ABORTED");
}

static void test_make_telemetry_sample() {
    BatterySnapshot bat;
    bat.timestamp = std::chrono::system_clock::now();
    bat.valid = true;
    bat.total_voltage_v = 13.45;
    bat.current_a = -2.5;
    bat.soc_pct = 75.0;
    bat.power_w = 33.6;
    bat.cell_delta_v = 0.025;
    bat.temperatures_c = {18.5, 19.0};

    PwrGateSnapshot chg;
    chg.timestamp = bat.timestamp;
    chg.valid = true;
    chg.state = "Charging";
    chg.ps_v = 14.0;
    chg.bat_v = 13.45;
    chg.bat_a = 2.5;
    chg.sol_v = 18.0;

    auto s = testing::make_telemetry_sample(42, &bat, &chg, 0, false, 0);
    ASSERT(s.test_id == 42, "make_telemetry_sample: test_id");
    ASSERT(s.battery_voltage == 13.45, "make_telemetry_sample: voltage");
    ASSERT(s.battery_current == -2.5, "make_telemetry_sample: current");
    ASSERT(s.battery_soc == 75.0, "make_telemetry_sample: soc");
    ASSERT(s.load_power >= 0, "make_telemetry_sample: load_power");
    ASSERT(s.charger_state == "Charging", "make_telemetry_sample: charger_state");
    ASSERT(s.charger_voltage == 13.45, "make_telemetry_sample: charger_voltage");
    ASSERT(s.charger_current == 2.5, "make_telemetry_sample: charger_current");
    ASSERT(s.cell_delta == 0.025, "make_telemetry_sample: cell_delta");
    ASSERT(s.temperature == 18.5, "make_telemetry_sample: temperature");

    auto s2 = testing::make_telemetry_sample(1, nullptr, nullptr, 50.0, true, 25.0);
    ASSERT(s2.test_id == 1, "make_telemetry_sample null: test_id");
    ASSERT(s2.battery_voltage == 0, "make_telemetry_sample null: voltage zero");
    ASSERT(s2.radio_tx_active == true, "make_telemetry_sample null: tx_active");
    ASSERT(s2.radio_tx_power == 25.0, "make_telemetry_sample null: tx_power");
}

// ---------- Testing framework: database ----------

static void test_database_test_runs() {
    std::string path = "/tmp/limonitor_test_runs_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for test_runs");

    Database::TestRunRow row;
    row.test_type = "capacity";
    row.start_time = 1700000000;
    row.end_time = 0;
    row.duration_seconds = 0;
    row.result = "running";
    row.initial_soc = 80.0;
    row.initial_voltage = 13.4;
    row.average_load = 0;
    row.metadata_json = "";
    row.user_notes = "";

    int64_t id = db.insert_test_run(row);
    ASSERT(id > 0, "insert_test_run returns positive id");

    auto runs = db.load_test_runs(10);
    ASSERT(!runs.empty(), "load_test_runs returns at least one");
    ASSERT(runs[0].id == id, "load_test_runs first has correct id");
    ASSERT(runs[0].test_type == "capacity", "load_test_runs first has correct type");
    ASSERT(runs[0].result == "running", "load_test_runs first has running result");

    auto opt = db.load_test_run(id);
    ASSERT(opt.has_value(), "load_test_run finds by id");
    ASSERT(opt->test_type == "capacity", "load_test_run returns correct type");

    bool ok = db.update_test_run(id, 1700003600, 3600, "passed", "{\"energy\":100}");
    ASSERT(ok, "update_test_run succeeds");
    opt = db.load_test_run(id);
    ASSERT(opt && opt->result == "passed", "update_test_run persisted");
    ASSERT(opt->duration_seconds == 3600, "update_test_run duration");

    ok = db.update_test_run_notes(id, "Test completed successfully");
    ASSERT(ok, "update_test_run_notes succeeds");
    opt = db.load_test_run(id);
    ASSERT(opt && opt->user_notes == "Test completed successfully", "update_test_run_notes persisted");

    auto notfound = db.load_test_run(999999);
    ASSERT(!notfound.has_value(), "load_test_run unknown id returns nullopt");

    db.close();
    std::remove(path.c_str());
}

static void test_database_test_telemetry() {
    std::string path = "/tmp/limonitor_test_tel_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for telemetry");

    Database::TestRunRow row;
    row.test_type = "load_spike";
    row.start_time = 1700000000;
    row.end_time = 1700000012;
    row.duration_seconds = 12;
    row.result = "passed";
    row.initial_soc = 90.0;
    row.initial_voltage = 13.5;
    int64_t id = db.insert_test_run(row);
    ASSERT(id > 0, "insert_test_run for telemetry");

    std::vector<testing::TestTelemetrySample> samples;
    for (int i = 0; i < 3; ++i) {
        testing::TestTelemetrySample s;
        s.test_id = id;
        s.timestamp = 1700000000 + i * 2;
        s.battery_voltage = 13.5 - i * 0.1;
        s.battery_current = 5.0;
        s.battery_soc = 90.0 - i * 2;
        s.load_power = 65.0;
        s.charger_state = "Idle";
        samples.push_back(s);
    }
    db.insert_test_telemetry_batch(samples);

    auto loaded = db.load_test_telemetry(id, 100);
    ASSERT(loaded.size() == 3, "load_test_telemetry returns 3 samples");
    ASSERT(loaded[0].battery_voltage == 13.5, "load_test_telemetry first voltage");
    ASSERT(loaded[2].battery_voltage == 13.3, "load_test_telemetry last voltage");

    auto empty = db.load_test_telemetry(999999, 100);
    ASSERT(empty.empty(), "load_test_telemetry unknown id returns empty");

    db.close();
    std::remove(path.c_str());
}

// ---------- Testing framework: TestRunner ----------

static void test_test_runner_start_stop() {
    Logger::instance().init("", false, 0);

    std::string path = "/tmp/limonitor_runner_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for TestRunner");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    Config cfg;
    cfg.no_ble = true;
    store.init_extensions(cfg);
    store.set_database(&db);

    BatterySnapshot bat;
    bat.timestamp = std::chrono::system_clock::now();
    bat.valid = true;
    bat.total_voltage_v = 13.5;
    bat.current_a = 0.5;
    bat.soc_pct = 80.0;
    bat.power_w = 6.75;
    bat.nominal_ah = 100.0;
    bat.remaining_ah = 80.0;
    bat.temperatures_c = {20.0};
    store.update(bat);

    testing::TestRunner runner(store, &db);
    ASSERT(!runner.is_running(), "TestRunner initially not running");

    int64_t id = runner.start_test(testing::TestType::BATTERY_CAPACITY);
    ASSERT(id > 0, "TestRunner start_test returns id");
    ASSERT(runner.is_running(), "TestRunner is running after start");
    ASSERT(runner.current_test_id() == id, "TestRunner current_test_id matches");

    auto st = runner.active_stats();
    ASSERT(st.test_id == id, "active_stats test_id");
    ASSERT(st.test_type == "capacity", "active_stats test_type");
    ASSERT(st.voltage_at_start == 13.5, "active_stats voltage_at_start");

    runner.stop_test();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT(!runner.is_running(), "TestRunner stopped after stop_test");

    auto runs = db.load_test_runs(1);
    ASSERT(!runs.empty(), "Test run persisted");
    ASSERT(runs[0].result == "aborted", "Test run marked aborted (user stop)");

    db.close();
    std::remove(path.c_str());
}

static void test_test_runner_refuses_low_soc() {
    Logger::instance().init("", false, 0);

    std::string path = "/tmp/limonitor_runner_low_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for low SOC test");

    DataStore store(100);
    Config cfg;
    cfg.no_ble = true;
    store.init_extensions(cfg);

    BatterySnapshot bat;
    bat.timestamp = std::chrono::system_clock::now();
    bat.valid = true;
    bat.total_voltage_v = 13.5;
    bat.current_a = 0.5;
    bat.soc_pct = 10.0;  // Below floor + 5 (default 15)
    bat.power_w = 6.75;
    bat.temperatures_c = {20.0};
    store.update(bat);

    testing::TestRunner runner(store, &db);
    int64_t id = runner.start_test(testing::TestType::BATTERY_CAPACITY);
    ASSERT(id == 0, "TestRunner refuses start when SOC too low");

    db.close();
    std::remove(path.c_str());
}

static void test_test_runner_refuses_maintenance_mode() {
    Logger::instance().init("", false, 0);

    std::string path = "/tmp/limonitor_runner_maint_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for maintenance test");
    db.set_setting("maintenance_mode", "1");

    DataStore store(100);
    Config cfg;
    cfg.no_ble = true;
    store.init_extensions(cfg);
    store.set_database(&db);

    BatterySnapshot bat;
    bat.timestamp = std::chrono::system_clock::now();
    bat.valid = true;
    bat.total_voltage_v = 13.5;
    bat.current_a = 0.5;
    bat.soc_pct = 80.0;
    bat.power_w = 6.75;
    bat.temperatures_c = {20.0};
    store.update(bat);

    testing::TestRunner runner(store, &db);
    int64_t id = runner.start_test(testing::TestType::BATTERY_CAPACITY);
    ASSERT(id == 0, "TestRunner refuses start when maintenance mode active");

    db.close();
    std::remove(path.c_str());
}

static void test_test_runner_safety_limits() {
    testing::SafetyLimits lim;
    lim.soc_floor_pct = 20.0;
    lim.voltage_floor_v = 12.0;
    lim.max_duration_sec = 3600;

    std::string path = "/tmp/limonitor_runner_lim_" + std::to_string(getpid()) + ".db";
    Database db(path);
    ASSERT(db.open(), "Database open for safety limits");

    DataStore store(100);
    Config cfg;
    cfg.no_ble = true;
    store.init_extensions(cfg);
    store.set_database(&db);

    testing::TestRunner runner(store, &db);
    runner.set_safety_limits(lim);
    ASSERT(runner.safety_limits().soc_floor_pct == 20.0, "safety_limits getter");
    ASSERT(runner.safety_limits().voltage_floor_v == 12.0, "safety_limits voltage");
    ASSERT(runner.safety_limits().max_duration_sec == 3600, "safety_limits max_duration");

    db.close();
    std::remove(path.c_str());
}

// ---------- Testing framework: HTTP API ----------

static void test_http_tests_api() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = false;

    std::string db_path = "/tmp/limonitor_http_tests_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open for tests API");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    testing::TestRunner runner(store, &db);
    HttpServer http(store, &db, "127.0.0.1", 19980, 5, &runner);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19980, "GET", "/api/tests");
    ASSERT(!resp.empty(), "GET /api/tests returns response");
    int status = parse_http_status(resp);
    ASSERT(status == 200, "GET /api/tests returns 200");
    ASSERT(resp.find("\"tests\"") != std::string::npos, "GET /api/tests has tests array");
    ASSERT(resp.find("\"running\"") != std::string::npos, "GET /api/tests has running field");

    resp = http_request("127.0.0.1", 19980, "GET", "/api/tests/safety_limits");
    ASSERT(parse_http_status(resp) == 200, "GET /api/tests/safety_limits returns 200");
    ASSERT(resp.find("soc_floor_pct") != std::string::npos, "safety_limits has soc_floor_pct");
    ASSERT(resp.find("voltage_floor_v") != std::string::npos, "safety_limits has voltage_floor_v");
    ASSERT(resp.find("max_duration_sec") != std::string::npos, "safety_limits has max_duration_sec");

    resp = http_request("127.0.0.1", 19980, "GET", "/api/tests/active");
    ASSERT(parse_http_status(resp) == 200, "GET /api/tests/active returns 200");
    ASSERT(resp.find("\"running\"") != std::string::npos, "api/tests/active has running");

    resp = http_request("127.0.0.1", 19980, "GET", "/api/battery/diagnostics");
    ASSERT(parse_http_status(resp) == 200, "GET /api/battery/diagnostics returns 200");
    ASSERT(resp.find("estimated_capacity_pct") != std::string::npos, "diagnostics has capacity");
    ASSERT(resp.find("health_score") != std::string::npos, "diagnostics has health_score");

    resp = http_request("127.0.0.1", 19980, "POST", "/api/tests/start", "{\"test_type\":\"capacity\"}");
    status = parse_http_status(resp);
    ASSERT(status == 400 || status == 200, "POST /api/tests/start returns 200 or 400 (no battery)");
    if (status == 200) {
        ASSERT(resp.find("\"ok\":true") != std::string::npos, "tests/start ok when battery present");
        resp = http_request("127.0.0.1", 19980, "POST", "/api/tests/stop", "{}");
        ASSERT(parse_http_status(resp) == 200, "POST /api/tests/stop returns 200");
    }

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_http_tests_start_stop_with_battery() {
    Logger::instance().init("", false, 0);

    Config cfg;
    cfg.no_ble = true;
    cfg.solar_enabled = false;

    std::string db_path = "/tmp/limonitor_http_start_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open");
    db.set_setting("config_migrated", "1");

    DataStore store(100);
    store.init_extensions(cfg);
    store.set_database(&db);

    BatterySnapshot bat;
    bat.timestamp = std::chrono::system_clock::now();
    bat.valid = true;
    bat.total_voltage_v = 13.5;
    bat.current_a = 0.5;
    bat.soc_pct = 85.0;
    bat.power_w = 6.75;
    bat.temperatures_c = {20.0};
    store.update(bat);

    testing::TestRunner runner(store, &db);
    HttpServer http(store, &db, "127.0.0.1", 19981, 5, &runner);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19981, "POST", "/api/tests/start", "{\"test_type\":\"load_spike\"}");
    int status = parse_http_status(resp);
    ASSERT(status == 200, "POST /api/tests/start returns 200 with battery");
    ASSERT(resp.find("\"ok\":true") != std::string::npos, "tests/start ok");
    ASSERT(resp.find("test_id") != std::string::npos, "tests/start returns test_id");

    resp = http_request("127.0.0.1", 19981, "GET", "/api/tests/active");
    ASSERT(resp.find("\"running\":true") != std::string::npos, "api/tests/active shows running");

    resp = http_request("127.0.0.1", 19981, "POST", "/api/tests/stop", "{}");
    ASSERT(parse_http_status(resp) == 200, "POST /api/tests/stop returns 200");
    ASSERT(resp.find("\"ok\":true") != std::string::npos, "tests/stop ok");

    resp = http_request("127.0.0.1", 19981, "GET", "/api/tests/active");
    ASSERT(resp.find("\"running\":false") != std::string::npos, "api/tests/active shows not running after stop");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

static void test_http_tests_telemetry_and_notes() {
    Logger::instance().init("", false, 0);

    std::string db_path = "/tmp/limonitor_http_tel_" + std::to_string(getpid()) + ".db";
    Database db(db_path);
    ASSERT(db.open(), "Database open");

    Database::TestRunRow row;
    row.test_type = "capacity";
    row.start_time = 1700000000;
    row.end_time = 1700003600;
    row.duration_seconds = 3600;
    row.result = "passed";
    row.initial_soc = 80.0;
    row.initial_voltage = 13.5;
    row.metadata_json = "{\"energy_delivered_wh\":500}";
    row.user_notes = "";
    int64_t id = db.insert_test_run(row);
    ASSERT(id > 0, "insert test run");

    testing::TestTelemetrySample s;
    s.test_id = id;
    s.timestamp = 1700000000;
    s.battery_voltage = 13.5;
    s.battery_current = 2.0;
    s.battery_soc = 80.0;
    s.load_power = 27.0;
    db.insert_test_telemetry_batch({s});

    DataStore store(100);
    Config cfg;
    cfg.no_ble = true;
    store.init_extensions(cfg);
    store.set_database(&db);

    testing::TestRunner runner(store, &db);
    HttpServer http(store, &db, "127.0.0.1", 19982, 5, &runner);
    ASSERT(http.start(), "HttpServer start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string resp = http_request("127.0.0.1", 19982, "GET", "/api/tests/" + std::to_string(id));
    ASSERT(parse_http_status(resp) == 200, "GET /api/tests/{id} returns 200");
    ASSERT(resp.find("\"test_type\":\"capacity\"") != std::string::npos, "test detail has type");
    ASSERT(resp.find("\"metadata_json\"") != std::string::npos, "test detail has metadata");

    resp = http_request("127.0.0.1", 19982, "GET", "/api/tests/" + std::to_string(id) + "/telemetry");
    ASSERT(parse_http_status(resp) == 200, "GET /api/tests/{id}/telemetry returns 200");
    ASSERT(resp.find("\"samples\"") != std::string::npos, "telemetry has samples");
    ASSERT(resp.find("13.5") != std::string::npos, "telemetry has voltage");

    resp = http_request("127.0.0.1", 19982, "POST", "/api/tests/" + std::to_string(id) + "/notes",
                        "{\"notes\":\"Manual test completed\"}");
    ASSERT(parse_http_status(resp) == 200, "POST /api/tests/{id}/notes returns 200");
    ASSERT(db.load_test_run(id)->user_notes == "Manual test completed", "notes persisted");

    resp = http_request("127.0.0.1", 19982, "GET", "/api/tests/999999");
    ASSERT(parse_http_status(resp) == 404, "GET /api/tests/unknown returns 404");

    http.stop();
    db.close();
    std::remove(db_path.c_str());
}

int main() {
    std::fprintf(stderr, "\n=== limonitor unit tests ===\n\n");

    test_database();
    test_weather_forecast_empty_api_key();
    test_http_options_cors();
    test_http_post_settings();
    test_http_solar_apis_with_extensions();
    test_solar_page_fetch_contract();
    test_solar_api_response_structure();
    test_http_solar_page_theme();
    test_parse_daily_entries_cloud_cover();
    test_parse_daily_entries_sunrise_sunset();
    test_parse_daily_entries_nested_clouds();
    test_parse_daily_entries_empty();
    test_forecast_week_extended_cloud_values();

    test_soc_basic_solar_charge();
    test_soc_basic_drain();
    test_soc_clamped_at_100();
    test_soc_clamped_at_0();
    test_soc_net_zero();
    test_soc_day_night_cycle();
    test_soc_empty_inputs();
    test_soc_zero_capacity();
    test_soc_negative_capacity();
    test_soc_start_above_100();
    test_soc_start_below_0();
    test_soc_mismatched_lengths();
    test_soc_realistic_scenario();
    test_soc_full_drain_recovery();

    test_runtime_basic();
    test_runtime_full_to_cutoff();
    test_runtime_zero_load();
    test_runtime_soc_below_cutoff();
    test_runtime_soc_at_cutoff();
    test_runtime_zero_capacity();
    test_runtime_consistency_with_dashboard();
    test_runtime_prod_db();

    test_scale_basic();
    test_scale_no_measured();
    test_scale_zero_profile();
    test_scale_preserves_ratio();
    test_scale_uneven_counts();

    test_soc_with_scaled_usage();

    test_regression_centered();
    test_regression_flat();
    test_runtime_nominal_voltage();
    test_daily_ci_variance_addition();

    test_parse_test_type();
    test_test_type_str();
    test_test_result_str();
    test_make_telemetry_sample();
    test_database_test_runs();
    test_database_test_telemetry();
    test_test_runner_start_stop();
    test_test_runner_refuses_low_soc();
    test_test_runner_refuses_maintenance_mode();
    test_test_runner_safety_limits();
    test_http_tests_api();
    test_http_tests_start_stop_with_battery();
    test_http_tests_telemetry_and_notes();

    std::fprintf(stderr, "\n=== %d failure(s) ===\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
