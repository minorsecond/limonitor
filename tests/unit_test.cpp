
int g_poll_interval_s = 5;  // required by ble_manager

#include "../src/config.hpp"
#include "../src/database.hpp"
#include "../src/data_store.hpp"
#include "../src/http_server.hpp"
#include "../src/logger.hpp"
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
            char date_buf[16];
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

    std::fprintf(stderr, "\n=== %d failure(s) ===\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
