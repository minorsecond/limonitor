#include "battery_data.hpp"
#include "ble_manager.hpp"
#include "ble_types.hpp"
#include "config.hpp"
#include "data_store.hpp"
#include "database.hpp"
#include "http_server.hpp"
#include "litime_protocol.hpp"
#include "logger.hpp"
#include "pwrgate_client.hpp"
#include "serial_reader.hpp"
#include "tui.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

// Exposed to ble_manager.cpp for poll interval
int g_poll_interval_s = 5;

static std::atomic<bool> g_quit{false};
static void sig_handler(int) { g_quit = true; }

// Config file support
static std::string default_config_path() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home)
        return std::string(home) + "/Library/Application Support/limonitor/limonitor.conf";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/limonitor/limonitor.conf";
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/.config/limonitor/limonitor.conf";
#endif
    return "./limonitor.conf";
}

static bool strtobool(const std::string& v) {
    return v == "true" || v == "1" || v == "yes";
}

// Migrate config values to DB (only on first run). Then load from DB into cfg.
static void migrate_and_load_config(Database* db, Config& cfg) {
    if (!db || !db->is_open()) return;
    bool first_run = db->get_setting("config_migrated").empty();
    if (first_run) {
        auto set_if_missing = [db](const std::string& key, const std::string& val) {
            if (val.empty()) return;
            if (db->get_setting(key).empty()) db->set_setting(key, val);
        };
        set_if_missing("device_address", cfg.device_address);
        set_if_missing("device_name", cfg.device_name);
        set_if_missing("adapter_path", cfg.adapter_path);
        set_if_missing("service_uuid", cfg.service_uuid);
        set_if_missing("notify_uuid", cfg.notify_char_uuid);
        set_if_missing("write_uuid", cfg.write_char_uuid);
        set_if_missing("poll_interval", std::to_string(cfg.poll_interval_s));
        set_if_missing("http_port", std::to_string(cfg.http_port));
        set_if_missing("http_bind", cfg.http_bind);
        set_if_missing("log_file", cfg.log_file);
        set_if_missing("verbose", cfg.log_verbose ? "1" : "0");
        set_if_missing("serial_device", cfg.serial_device);
        set_if_missing("serial_baud", std::to_string(cfg.serial_baud));
        set_if_missing("pwrgate_remote", cfg.pwrgate_remote);
        set_if_missing("db_path", cfg.db_path);
        set_if_missing("db_interval", std::to_string(cfg.db_write_interval_s));
        set_if_missing("daemon", cfg.daemon_mode ? "1" : "0");
        set_if_missing("battery_purchased", cfg.battery_purchased);
        set_if_missing("rated_capacity_ah", std::to_string(cfg.rated_capacity_ah));
        set_if_missing("tx_threshold", std::to_string(cfg.tx_threshold_a));
        set_if_missing("solar_enabled", cfg.solar_enabled ? "1" : "0");
        set_if_missing("solar_panel_watts", std::to_string(cfg.solar_panel_watts));
        set_if_missing("solar_system_efficiency", std::to_string(cfg.solar_system_efficiency));
        set_if_missing("solar_zip_code", cfg.solar_zip_code);
        set_if_missing("weather_api_key", cfg.weather_api_key);
        set_if_missing("weather_zip_code", cfg.weather_zip_code);
        db->set_setting("config_migrated", "1");
        LOG_INFO("Config: migrated to DB (first run)");
    }

    auto v = [db](const std::string& k) { return db->get_setting(k); };
    auto apply = [&cfg, &v]() {
        std::string s;
        cfg.device_address = v("device_address");
        cfg.device_name = v("device_name");
        if (!(s = v("adapter_path")).empty()) cfg.adapter_path = s;
        if (!(s = v("service_uuid")).empty()) cfg.service_uuid = s;
        if (!(s = v("notify_uuid")).empty()) cfg.notify_char_uuid = s;
        if (!(s = v("write_uuid")).empty()) cfg.write_char_uuid = s;
        if (!(s = v("poll_interval")).empty()) { try { cfg.poll_interval_s = std::stoi(s); } catch (...) {} }
        if (!(s = v("http_port")).empty()) { try { cfg.http_port = static_cast<uint16_t>(std::stoul(s)); } catch (...) {} }
        cfg.http_bind = v("http_bind").empty() ? "0.0.0.0" : v("http_bind");
        cfg.log_file = v("log_file");
        cfg.log_verbose = (v("verbose") == "1" || v("verbose") == "true");
        cfg.serial_device = v("serial_device");
        if (!(s = v("serial_baud")).empty()) { try { cfg.serial_baud = std::stoi(s); } catch (...) {} }
        cfg.pwrgate_remote = v("pwrgate_remote");
        cfg.db_path = v("db_path");
        if (!(s = v("db_interval")).empty()) { try { cfg.db_write_interval_s = std::stoi(s); } catch (...) {} }
        cfg.daemon_mode = (v("daemon") == "1" || v("daemon") == "true");
        cfg.battery_purchased = v("battery_purchased");
        if (!(s = v("rated_capacity_ah")).empty()) { try { cfg.rated_capacity_ah = std::stod(s); } catch (...) {} }
        if (!(s = v("tx_threshold")).empty()) { try { cfg.tx_threshold_a = std::stod(s); } catch (...) {} }
        cfg.solar_enabled = (v("solar_enabled") == "1" || v("solar_enabled") == "true");
        if (!(s = v("solar_panel_watts")).empty()) { try { cfg.solar_panel_watts = std::stod(s); } catch (...) {} }
        if (!(s = v("solar_system_efficiency")).empty()) { try { cfg.solar_system_efficiency = std::stod(s); } catch (...) {} }
        cfg.solar_zip_code = v("solar_zip_code").empty() ? "80112" : v("solar_zip_code");
        cfg.weather_api_key = v("weather_api_key");
        cfg.weather_zip_code = v("weather_zip_code").empty() ? "80112" : v("weather_zip_code");
    };
    apply();
}

static bool needs_setup(const Config& cfg) {
    std::string target = cfg.device_address.empty() ? cfg.device_name : cfg.device_address;
    return target.empty() && cfg.serial_device.empty() && cfg.pwrgate_remote.empty();
}

static void run_tui_settings(Database* db, Config& cfg) {
    if (!db || !db->is_open()) return;
    std::cout << "\n=== limonitor settings ===\n"
              << "Enter value or press Enter to keep current.\n\n";

    auto prompt = [](const char* label, const std::string& current) -> std::string {
        std::cout << label;
        if (!current.empty()) std::cout << " [" << current << "]";
        std::cout << ": ";
        std::string line;
        std::getline(std::cin, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return line.empty() ? current : line;
    };

    auto save_str = [db](const char* k, const std::string& v, std::string& out) {
        if (!v.empty()) { db->set_setting(k, v); out = v; }
    };
    auto save_int = [db](const char* k, const std::string& v, int& out) {
        if (!v.empty()) { try { int n = std::stoi(v); db->set_setting(k, v); out = n; } catch (...) {} }
    };
    auto save_dbl = [db](const char* k, const std::string& v, double& out) {
        if (!v.empty()) { try { double d = std::stod(v); db->set_setting(k, v); out = d; } catch (...) {} }
    };
    auto save_bool = [db](const char* k, const std::string& v, bool& out) {
        if (!v.empty()) { bool b = (v == "1" || v == "true" || v == "yes"); db->set_setting(k, b ? "1" : "0"); out = b; }
    };

    std::string v;
    v = prompt("BLE device name", cfg.device_name); save_str("device_name", v, cfg.device_name);
    v = prompt("BLE device address (MAC)", cfg.device_address); save_str("device_address", v, cfg.device_address);
    v = prompt("Adapter path", cfg.adapter_path); save_str("adapter_path", v, cfg.adapter_path);
    v = prompt("HTTP port", std::to_string(cfg.http_port)); if (!v.empty()) { try { cfg.http_port = static_cast<uint16_t>(std::stoul(v)); db->set_setting("http_port", v); } catch (...) {} }
    v = prompt("HTTP bind address", cfg.http_bind); save_str("http_bind", v, cfg.http_bind);
    v = prompt("Serial device", cfg.serial_device); save_str("serial_device", v, cfg.serial_device);
    v = prompt("Serial baud", std::to_string(cfg.serial_baud)); save_int("serial_baud", v, cfg.serial_baud);
    v = prompt("Remote PwrGate (host:port)", cfg.pwrgate_remote); save_str("pwrgate_remote", v, cfg.pwrgate_remote);
    v = prompt("Poll interval (seconds)", std::to_string(cfg.poll_interval_s)); save_int("poll_interval", v, cfg.poll_interval_s);
    v = prompt("DB path", cfg.db_path); save_str("db_path", v, cfg.db_path);
    v = prompt("DB write interval (seconds)", std::to_string(cfg.db_write_interval_s)); save_int("db_interval", v, cfg.db_write_interval_s);
    v = prompt("Battery purchase date (YYYY-MM-DD)", cfg.battery_purchased); save_str("battery_purchased", v, cfg.battery_purchased);
    v = prompt("Rated capacity Ah (0=auto)", std::to_string(cfg.rated_capacity_ah)); save_dbl("rated_capacity_ah", v, cfg.rated_capacity_ah);
    v = prompt("TX threshold (amps)", std::to_string(cfg.tx_threshold_a)); save_dbl("tx_threshold", v, cfg.tx_threshold_a);
    v = prompt("Solar enabled (1/0)", cfg.solar_enabled ? "1" : "0"); save_bool("solar_enabled", v, cfg.solar_enabled);
    v = prompt("Solar panel watts", std::to_string(cfg.solar_panel_watts)); save_dbl("solar_panel_watts", v, cfg.solar_panel_watts);
    v = prompt("Solar system efficiency (0-1)", std::to_string(cfg.solar_system_efficiency)); save_dbl("solar_system_efficiency", v, cfg.solar_system_efficiency);
    v = prompt("Solar ZIP code", cfg.solar_zip_code); save_str("solar_zip_code", v, cfg.solar_zip_code);
    v = prompt("Weather API key", cfg.weather_api_key); save_str("weather_api_key", v, cfg.weather_api_key);
    v = prompt("Weather ZIP code", cfg.weather_zip_code); save_str("weather_zip_code", v, cfg.weather_zip_code);
    v = prompt("Daemon mode (1/0)", cfg.daemon_mode ? "1" : "0"); save_bool("daemon", v, cfg.daemon_mode);

    db->set_setting("settings_initialized", "1");
    std::cout << "\nSettings saved. Restart limonitor to apply.\n\n";
}

static void load_config_file(const std::string& path, Config& cfg) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    std::fprintf(stderr, "[config] loading %s\n", path.c_str());
    char line[512];
    int lineno = 0;
    while (std::fgets(line, sizeof(line), f)) {
        ++lineno;
        // Strip trailing newline/CR
        size_t len = std::strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        // Skip blanks and comments
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        // Split on '='
        const char* eq = std::strchr(p, '=');
        if (!eq) { std::fprintf(stderr, "[config] line %d: no '=' — skipped\n", lineno); continue; }
        std::string key(p, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        const char* vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') ++vp;
        std::string val(vp);
        // Strip inline comment
        auto hpos = val.find(" #");
        if (hpos != std::string::npos) val = val.substr(0, hpos);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        // Map to Config fields
        try {
            if      (key == "device_name")    cfg.device_name        = val;
            else if (key == "device_address") cfg.device_address     = val;
            else if (key == "adapter_path")   cfg.adapter_path       = val;
            else if (key == "http_port")      cfg.http_port          = static_cast<uint16_t>(std::stoul(val));
            else if (key == "http_bind")      cfg.http_bind          = val;
            else if (key == "log_file")       cfg.log_file           = val;
            else if (key == "verbose")        cfg.log_verbose        = strtobool(val);
            else if (key == "poll_interval")  cfg.poll_interval_s    = std::stoi(val);
            else if (key == "service_uuid")   cfg.service_uuid       = val;
            else if (key == "notify_uuid")    cfg.notify_char_uuid   = val;
            else if (key == "write_uuid")     cfg.write_char_uuid    = val;
            else if (key == "demo")           cfg.demo_mode          = strtobool(val);
            else if (key == "no_ble")         cfg.no_ble             = strtobool(val);
            else if (key == "serial_device")  cfg.serial_device      = val;
            else if (key == "serial_baud")    cfg.serial_baud        = std::stoi(val);
            else if (key == "pwrgate_remote") cfg.pwrgate_remote     = val;
            else if (key == "db_path")        cfg.db_path            = val;
            else if (key == "db_interval")    cfg.db_write_interval_s= std::stoi(val);
            else if (key == "daemon")              cfg.daemon_mode        = strtobool(val);
            else if (key == "battery_purchased")   cfg.battery_purchased  = val;
            else if (key == "tx_threshold")        cfg.tx_threshold_a     = std::stod(val);
            else if (key == "rated_capacity_ah")   cfg.rated_capacity_ah  = std::stod(val);
            else if (key == "solar_enabled")       cfg.solar_enabled      = strtobool(val);
            else if (key == "solar_panel_watts")   cfg.solar_panel_watts  = std::stod(val);
            else if (key == "solar_system_efficiency") cfg.solar_system_efficiency = std::stod(val);
            else if (key == "solar_zip_code")      cfg.solar_zip_code     = val;
            else if (key == "weather_api_key")     cfg.weather_api_key    = val;
            else if (key == "weather_zip_code")    cfg.weather_zip_code   = val;
            else std::fprintf(stderr, "[config] line %d: unknown key '%s'\n", lineno, key.c_str());
        } catch (...) {
            std::fprintf(stderr, "[config] line %d: bad value for '%s'\n", lineno, key.c_str());
        }
    }
    std::fclose(f);
}

static void print_usage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [OPTIONS]\n"
        "\n"
        "BLE LiTime/JBD LiFePO4 battery monitor.\n"
        "\n"
        "Config file (loaded before CLI flags, CLI overrides):\n"
        "  macOS: ~/Library/Application Support/limonitor/limonitor.conf\n"
        "  Linux: ~/.config/limonitor/limonitor.conf\n"
        "  Override path: --config FILE\n"
        "\n"
        "Options:\n"
        "  --config FILE  Config file path\n"
        "  -a ADDR       BLE device address (AA:BB:CC:DD:EE:FF)\n"
        "  -n NAME       BLE device name (substring match)\n"
        "  -i IFACE      BlueZ adapter path  [/org/bluez/hci0]\n"
        "  -p PORT       HTTP API port        [8080]\n"
        "  -b ADDR       HTTP bind address    [0.0.0.0]\n"
        "  -l FILE       Log to file\n"
        "  -v            Verbose (show DEBUG messages)\n"
        "  -I SECS       Poll interval        [5]\n"
        "  --service UUID     GATT service UUID    [ff00...]\n"
        "  --notify  UUID     GATT notify char UUID [ff01...]\n"
        "  --write   UUID     GATT write char UUID  [ff02...]\n"
        "  --demo         Run in demo mode (no BLE, synthetic data)\n"
        "  --no-ble       Skip BLE entirely (serial/HTTP-only node, e.g. on Pi)\n"
        "  -s DEVICE      Serial port for EpicPowerGate 2  [e.g. /dev/ttyACM0]\n"
        "  --baud N       Serial baud rate                 [115200]\n"
        "  --pwrgate-remote HOST:PORT  Poll remote limonitor for charger data\n"
        "  --db PATH      SQLite database path             [platform default]\n"
        "  --db-interval N  DB write throttle seconds      [60]\n"
        "  --daemon       Run headless (no TUI) — for background/service use\n"
        "  --purchase-date DATE  Record battery purchase date (e.g. 2024-03-15)\n"
        "  --rated-capacity N    Rated battery capacity in Ah (default: auto from BMS)\n"
        "  --tx-threshold A  TX detection threshold in amps [1.0]\n"
        "                    Net positive battery current that starts a TX event.\n"
        "                    1.0 works for 25-50W VHF/UHF. Raise to 4-6 for 100W HF.\n"
        "  -h             Show this help\n"
        "\n"
        "API endpoints (served on http://HOST:PORT/):\n"
        "  GET /                   HTML dashboard\n"
        "  GET /api/status         Full battery JSON snapshot\n"
        "  GET /api/analytics      Battery analytics JSON (energy, health, DoD, load…)\n"
        "  GET /api/cells          Cell voltages JSON\n"
        "  GET /api/history        Historical battery snapshots (?n=N)\n"
        "  GET /api/charger        Latest charger snapshot JSON\n"
        "  GET /api/charger/history  Charger history (?n=N)\n"
        "  GET /api/flow           Energy flow diagram data JSON\n"
        "  GET /api/tx_events      Radio TX event log JSON (?n=N)\n"
        "  GET /metrics            Prometheus text metrics\n"
        "\n"
        "Examples:\n"
        "  " << prog << " -a AA:BB:CC:DD:EE:FF -p 8080 -l /var/log/battery.log\n"
        "  " << prog << " -n JBD --demo\n"
        "  " << prog << " --no-ble -s /dev/ttyACM0 --purchase-date 2023-01-15 --rated-capacity 100\n";
}

static Config parse_args(int argc, char** argv, Config cfg = {}) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--config") { next(); /* already handled */ }
        else if (arg == "-a") cfg.device_address = next();
        else if (arg == "-n") cfg.device_name = next();
        else if (arg == "-i") cfg.adapter_path = next();
        else if (arg == "-p") cfg.http_port = static_cast<uint16_t>(std::stoul(next()));
        else if (arg == "-b") cfg.http_bind = next();
        else if (arg == "-l") cfg.log_file = next();
        else if (arg == "-v") cfg.log_verbose = true;
        else if (arg == "-I") { cfg.poll_interval_s = std::stoi(next()); }
        else if (arg == "--service") cfg.service_uuid = next();
        else if (arg == "--notify")  cfg.notify_char_uuid = next();
        else if (arg == "--write")   cfg.write_char_uuid = next();
        else if (arg == "--demo")    cfg.demo_mode = true;
        else if (arg == "--no-ble")  cfg.no_ble    = true;
        else if (arg == "-s")               cfg.serial_device = next();
        else if (arg == "--baud")           cfg.serial_baud = std::stoi(next());
        else if (arg == "--pwrgate-remote") cfg.pwrgate_remote = next();
        else if (arg == "--db")      cfg.db_path = next();
        else if (arg == "--db-interval") cfg.db_write_interval_s = std::stoi(next());
        else if (arg == "--daemon")         cfg.daemon_mode       = true;
        else if (arg == "--purchase-date")  cfg.battery_purchased = next();
        else if (arg == "--rated-capacity") cfg.rated_capacity_ah = std::stod(next());
        else if (arg == "--tx-threshold")   cfg.tx_threshold_a = std::stod(next());
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); std::exit(0); }
        else { std::cerr << "Unknown option: " << arg << "\n"; print_usage(argv[0]); std::exit(1); }
    }
    return cfg;
}

static void run_demo(DataStore& store, int interval_s) {
    LOG_INFO("Demo mode: generating synthetic battery data");
    double soc = 85.0;   // starting SoC %
    double nominal = 100.0; // 100Ah
    int cycle = 42;
    int tick = 0;

    while (!g_quit) {
        BatterySnapshot s;
        s.timestamp = std::chrono::system_clock::now();
        s.nominal_ah = nominal;
        s.soc_pct = soc;
        s.remaining_ah = soc / 100.0 * nominal;

        // Simulate discharge + small noise
        double current = 5.0 + std::sin(tick * 0.1) * 0.5;
        s.current_a = current;
        soc -= current * (interval_s / 3600.0) / nominal * 100.0;
        if (soc < 5.0) soc = 100.0; // "recharge"

        // 16-cell 48V pack
        s.cell_voltages_v.resize(16);
        double base_cell_v = 3.0 + (soc / 100.0) * 0.65;
        s.cell_min_v = 9999; s.cell_max_v = 0;
        for (int i = 0; i < 16; ++i) {
            double jitter = (i == 7) ? -0.005 : ((i % 3 == 0) ? 0.003 : 0.001);
            double cv = base_cell_v + jitter + std::sin(tick + i) * 0.001;
            s.cell_voltages_v[i] = cv;
            if (cv < s.cell_min_v) s.cell_min_v = cv;
            if (cv > s.cell_max_v) s.cell_max_v = cv;
        }
        s.cell_delta_v = s.cell_max_v - s.cell_min_v;
        s.total_voltage_v = s.cell_min_v * 16 + (s.cell_max_v - s.cell_min_v) * 8;
        // More realistic: sum of cells
        s.total_voltage_v = 0;
        for (double cv : s.cell_voltages_v) s.total_voltage_v += cv;

        s.temperatures_c = {25.3 + std::sin(tick * 0.05) * 2.0, 24.8};
        s.cycle_count = cycle;
        s.charge_mosfet = true;
        s.discharge_mosfet = true;
        s.power_w = s.total_voltage_v * s.current_a;
        s.time_remaining_h = (s.current_a > 0) ? (s.remaining_ah / s.current_a) : 0;
        s.device_name = "LITIME-DEMO";
        s.ble_address = "00:00:00:00:00:00";
        s.sw_version = "1.0";
        s.valid = true;

        store.update(std::move(s));
        store.set_ble_state("demo");

        ++tick;
        for (int i = 0; i < interval_s * 10 && !g_quit; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char** argv) {
    // Determine config file path (--config can appear anywhere in argv)
    std::string config_path = default_config_path();
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config") { config_path = argv[i+1]; break; }
    }
    // Load config file first, then let CLI args override
    Config cfg;
    load_config_file(config_path, cfg);
    cfg = parse_args(argc, argv, cfg);

    // Signal handling
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    std::signal(SIGPIPE, SIG_IGN);

    std::string dbpath = cfg.db_path.empty() ? Database::default_path() : cfg.db_path;
    try { dbpath = std::filesystem::absolute(dbpath).string(); } catch (...) {}
    auto db = std::make_shared<Database>(dbpath);
    if (db->open()) {
        migrate_and_load_config(db.get(), cfg);
        if (needs_setup(cfg) && !cfg.daemon_mode) {
            run_tui_settings(db.get(), cfg);
        }
    }

    // Logger: init after loading from DB so verbose/log_file come from DB
    Logger::instance().init(cfg.log_file, cfg.log_verbose, cfg.log_rotate_bytes);
    LOG_INFO("limonitor v1.0.0 starting");
    if (!db->is_open())
        LOG_WARN("DB: running without persistence; config will not be migrated");

    // Apply poll interval globally (used by ble_manager.cpp)
    g_poll_interval_s = cfg.poll_interval_s;

    // Data store
    DataStore store(cfg.history_size);
    store.set_tx_threshold(cfg.tx_threshold_a);
    store.init_extensions(cfg, db->is_open() ? db.get() : nullptr);
    LOG_INFO("TX detection threshold: %.2f A (net positive battery discharge)", cfg.tx_threshold_a);
    if (cfg.solar_enabled)
        LOG_INFO("Solar: enabled, %g W panels, zip=%s, weather_api_key=%s",
                 cfg.solar_panel_watts, cfg.solar_zip_code.c_str(),
                 cfg.weather_api_key.empty() ? "(not set)" : "set");
    if (!cfg.battery_purchased.empty())
        store.set_purchase_date(cfg.battery_purchased);
    if (cfg.rated_capacity_ah > 0) {
        store.set_rated_capacity(cfg.rated_capacity_ah);
        LOG_INFO("Rated capacity override: %.1f Ah", cfg.rated_capacity_ah);
    }

    // Attach DB to store (already opened above)
    if (db->is_open()) {
        store.set_database(db.get());
        store.set_loading_history(true);
        // Pre-populate history rings BEFORE registering the observer (avoids re-inserting)
        for (auto& s : db->load_battery_history(cfg.history_size))
            store.update(std::move(s));
        for (auto& p : db->load_charger_history(cfg.history_size))
            store.update_pwrgate(std::move(p));
        store.load_system_events_from_db(db->load_system_events(200));
        store.set_loading_history(false);

        int interval = cfg.db_write_interval_s;
        // Battery observer — throttled by db_write_interval_s
        store.on_update([db, interval](const BatterySnapshot& snap) {
            using clock = std::chrono::steady_clock;
            static auto last = clock::time_point{};
            auto now = clock::now();
            if (interval <= 0 ||
                std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= interval) {
                db->insert_battery(snap);
                last = now;
            }
        });
    } else {
        LOG_WARN("DB: running without persistence");
    }

    // HTTP server (pass db for settings persistence; works even when db->open() failed, via file fallback)
    HttpServer http(store, db.get(), cfg.http_bind, cfg.http_port, cfg.poll_interval_s);
    if (!http.start()) {
        LOG_ERROR("HTTP server failed to start on port %d", cfg.http_port);
        return 1;
    }
    LOG_INFO("HTTP API: http://%s:%d/", cfg.http_bind.c_str(), cfg.http_port);

    // BLE manager + JBD parser
    std::unique_ptr<BleManager> ble;
    std::thread demo_thread;

    if (cfg.no_ble) {
        store.set_ble_state("disabled");
        LOG_INFO("BLE disabled (--no-ble)");
    } else if (cfg.demo_mode) {
        demo_thread = std::thread([&]{ run_demo(store, cfg.poll_interval_s); });
    } else {
        // Determine target: prefer address, fall back to name
        std::string target = cfg.device_address.empty() ? cfg.device_name : cfg.device_address;
        if (target.empty()) {
            LOG_WARN("No device address or name specified; will match any JBD service (ff00)");
        }

        ble = std::make_unique<BleManager>(target,
            cfg.adapter_path, cfg.service_uuid,
            cfg.notify_char_uuid, cfg.write_char_uuid);

        // Per-connection LiTime parser (lives on data callback thread)
        auto parser = std::make_shared<litime::Parser>();
        auto current = std::make_shared<BatterySnapshot>();

        ble->set_poll_command(litime::build_request());

        ble->set_data_callback([&store, parser, current, &ble](const std::vector<uint8_t>& raw) {
            auto res = parser->feed(raw.data(), raw.size());
            if (res == litime::Parser::Result::COMPLETE) {
                current->timestamp   = std::chrono::system_clock::now();
                current->ble_address = ble->device_address();
                if (current->device_name.empty())
                    current->device_name = ble->device_name();
                parser->apply(*current);
                parser->reset(); // clear buffer for next packet
                if (current->valid) {
                    store.update(*current);
                    LOG_DEBUG("Updated: %.2fV  %+.2fA  %.1f%%",
                        current->total_voltage_v, current->current_a, current->soc_pct);
                }
            }
        });

        ble->set_state_callback([&store, parser](BleState s, const std::string& msg) {
            store.set_ble_state(std::string(ble_state_str(s)) + (msg.empty() ? "" : ": " + msg));
            if (s == BleState::DISCONNECTED || s == BleState::ERROR)
                parser->reset();
            if (s == BleState::SCANNING)
                store.clear_discovered(); // fresh scan — clear stale list
        });

        ble->set_discovery_callback([&store](const DiscoveredDevice& dev) {
            store.upsert_discovered(dev);
        });

        if (!ble->start()) {
            LOG_ERROR("BLE manager failed to start (is BlueZ running?)");
            return 1;
        }
    }

    // Serial reader for EpicPowerGate 2 (optional)
    std::unique_ptr<SerialReader> serial;
    if (!cfg.serial_device.empty()) {
        serial = std::make_unique<SerialReader>(cfg.serial_device, cfg.serial_baud);
        int pg_interval = std::max(1, cfg.db_write_interval_s / 5);
        serial->set_callback([&store, db, pg_interval](const PwrGateSnapshot& snap) {
            store.update_pwrgate(snap);
            LOG_DEBUG("PwrGate: %s  PS=%.2fV  Bat=%.2fV %.2fA  Sol=%.2fV",
                snap.state.c_str(), snap.ps_v, snap.bat_v, snap.bat_a, snap.sol_v);
            if (db && db->is_open()) {
                using clock = std::chrono::steady_clock;
                static auto last = clock::time_point{};
                auto now = clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count()
                        >= pg_interval) {
                    db->insert_charger(snap);
                    last = now;
                }
            }
        });
        if (!serial->start())
            LOG_WARN("Serial: failed to open %s — charger data unavailable", cfg.serial_device.c_str());
    }

    // Remote PwrGate client (polls another limonitor instance's /api/charger)
    std::unique_ptr<PwrGateClient> pgclient;
    if (!cfg.pwrgate_remote.empty()) {
        // Parse "host:port"
        auto colon = cfg.pwrgate_remote.rfind(':');
        std::string pg_host = (colon != std::string::npos)
            ? cfg.pwrgate_remote.substr(0, colon) : cfg.pwrgate_remote;
        uint16_t pg_port = (colon != std::string::npos)
            ? static_cast<uint16_t>(std::stoul(cfg.pwrgate_remote.substr(colon + 1)))
            : 8080;

        pgclient = std::make_unique<PwrGateClient>(pg_host, pg_port, cfg.poll_interval_s);
        int pg_interval = std::max(1, cfg.db_write_interval_s / 5);
        pgclient->set_callback([&store, db, pg_interval](const PwrGateSnapshot& snap) {
            store.update_pwrgate(snap);
            if (db && db->is_open()) {
                using clock = std::chrono::steady_clock;
                static auto last = clock::time_point{};
                auto now = clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count()
                        >= pg_interval) {
                    db->insert_charger(snap);
                    last = now;
                }
            }
        });
        pgclient->start();
    }

    // TUI — blocks until 'q' or signal
    if (cfg.daemon_mode) {
        LOG_INFO("Daemon mode: running headless (HTTP on port %d)", cfg.http_port);
        auto last_checkpoint = std::chrono::steady_clock::now();
        while (!g_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (db && db->is_open()) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::minutes>(now - last_checkpoint).count() >= 5) {
                    db->checkpoint();
                    last_checkpoint = now;
                }
            }
        }
    } else {
        TUI tui(store, cfg.http_port);
        if (ble) {
            tui.set_connect_callback([&ble](const std::string& id) {
                ble->connect_to(id);
            });
        }
        tui.set_settings_callback([&] { run_tui_settings(db.get(), cfg); });
        std::thread quit_watcher([&]{
            while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            tui.stop();
        });

        tui.run();
        g_quit = true;

        if (quit_watcher.joinable()) quit_watcher.join();
    }

    // Shutdown
    LOG_INFO("Shutting down");
    http.stop();
    if (serial)   serial->stop();
    if (pgclient) pgclient->stop();
    db->close();
    if (ble) ble->stop();
    if (demo_thread.joinable()) demo_thread.join();

    return 0;
}
