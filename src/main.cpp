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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

// Exposed to ble_manager.cpp for poll interval
int g_poll_interval_s = 5;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
static std::atomic<bool> g_quit{false};
static void sig_handler(int) { g_quit = true; }

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Config file support
// ---------------------------------------------------------------------------
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
            else if (key == "daemon")         cfg.daemon_mode        = strtobool(val);
            else std::fprintf(stderr, "[config] line %d: unknown key '%s'\n", lineno, key.c_str());
        } catch (...) {
            std::fprintf(stderr, "[config] line %d: bad value for '%s'\n", lineno, key.c_str());
        }
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
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
        "  -h             Show this help\n"
        "\n"
        "API endpoints (served on http://HOST:PORT/):\n"
        "  GET /              HTML dashboard\n"
        "  GET /api/status    Full JSON status\n"
        "  GET /api/cells     Cell voltages JSON\n"
        "  GET /api/history   Historical snapshots JSON (?n=100)\n"
        "  GET /metrics       Prometheus metrics\n"
        "\n"
        "Examples:\n"
        "  " << prog << " -a AA:BB:CC:DD:EE:FF -p 8080 -l /var/log/battery.log\n"
        "  " << prog << " -n JBD --demo\n";
}

// parse_args applies CLI flags on top of an existing Config (e.g. loaded from file).
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
        else if (arg == "--daemon")  cfg.daemon_mode = true;
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); std::exit(0); }
        else { std::cerr << "Unknown option: " << arg << "\n"; print_usage(argv[0]); std::exit(1); }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Demo mode: synthesize realistic battery data
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
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

    // Logger
    Logger::instance().init(cfg.log_file, cfg.log_verbose, cfg.log_rotate_bytes);
    LOG_INFO("limonitor v1.0.0 starting");

    // Apply poll interval globally (used by ble_manager.cpp)
    g_poll_interval_s = cfg.poll_interval_s;

    // Signal handling
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    std::signal(SIGPIPE, SIG_IGN);

    // Data store
    DataStore store(cfg.history_size);

    // SQLite database
    std::string dbpath = cfg.db_path.empty() ? Database::default_path() : cfg.db_path;
    auto db = std::make_shared<Database>(dbpath);
    if (db->open()) {
        // Pre-populate history rings BEFORE registering the observer (avoids re-inserting)
        for (auto& s : db->load_battery_history(cfg.history_size))
            store.update(std::move(s));
        for (auto& p : db->load_charger_history(cfg.history_size))
            store.update_pwrgate(std::move(p));

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

    // HTTP server
    HttpServer http(store, cfg.http_bind, cfg.http_port);
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
        while (!g_quit)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else {
        TUI tui(store, cfg.http_port);
        if (ble) {
            tui.set_connect_callback([&ble](const std::string& id) {
                ble->connect_to(id);
            });
        }
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
