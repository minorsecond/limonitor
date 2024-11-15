#include "battery_data.hpp"
#include "ble_manager.hpp"
#include "ble_types.hpp"
#include "config.hpp"
#include "data_store.hpp"
#include "http_server.hpp"
#include "litime_protocol.hpp"
#include "logger.hpp"
#include "tui.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
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
static void print_usage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [OPTIONS]\n"
        "\n"
        "BLE LiTime/JBD LiFePO4 battery monitor.\n"
        "\n"
        "Options:\n"
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

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "-a") cfg.device_address = next();
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
    Config cfg = parse_args(argc, argv);

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

    if (cfg.demo_mode) {
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

    // TUI — blocks until 'q' or signal
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

    // Shutdown
    LOG_INFO("Shutting down");
    http.stop();
    if (ble) ble->stop();
    if (demo_thread.joinable()) demo_thread.join();
    if (quit_watcher.joinable()) quit_watcher.join();

    return 0;
}
