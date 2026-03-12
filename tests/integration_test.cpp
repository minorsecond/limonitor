// connects to real battery via BLE, validates one snapshot. -n NAME -t SECS

#include "../src/ble_manager.hpp"
#include "../src/battery_data.hpp"
#include "../src/litime_protocol.hpp"
#include "../src/logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

int g_poll_interval_s = 5;

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-n NAME] [-t TIMEOUT_S]\n", prog);
    fprintf(stderr, "  -n NAME     device name substring  [L-12100BNNA70]\n");
    fprintf(stderr, "  -t SECS     timeout in seconds     [45]\n");
}

struct Check {
    const char* name;
    bool        pass;
};

static bool run_checks(const BatterySnapshot& s, std::vector<Check>& out) {
    bool all_pass = true;
    auto add = [&](const char* name, bool cond) {
        out.push_back({name, cond});
        if (!cond) all_pass = false;
    };

    // A 12V (or 24V / 48V) LiFePO4 pack: cells are 2.5–3.65 V each.
    // 4S (12V): 10–15V   |  8S (24V): 20–30V   |  16S (48V): 40–60V
    add("voltage > 10 V",                  s.total_voltage_v > 10.0);
    add("voltage < 65 V",                  s.total_voltage_v < 65.0);
    add("SoC in [0 %, 100 %]",             s.soc_pct >= 0.0 && s.soc_pct <= 100.0);
    add("nominal capacity > 0 Ah",         s.nominal_ah > 0.0);
    add("remaining <= nominal + epsilon",  s.remaining_ah <= s.nominal_ah * 1.02);
    add("at least 4 cells reported",       s.cell_voltages_v.size() >= 4);
    add("no more than 16 cells",           s.cell_voltages_v.size() <= 16);
    add("cell min >= 2.5 V",               s.cell_min_v >= 2.5);
    add("cell max <= 3.75 V",              s.cell_max_v <= 3.75);
    add("at least 1 temperature reported", !s.temperatures_c.empty());
    add("temperature in [-20 °C, 80 °C]",
        !s.temperatures_c.empty() &&
        s.temperatures_c[0] > -20.0 && s.temperatures_c[0] < 80.0);

    return all_pass;
}

int main(int argc, char** argv) {
    std::string device_name = "L-12100BNNA70";
    int timeout_s = 45;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) { device_name = argv[++i]; }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) { timeout_s = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    Logger::instance().init("", true, 0); // log to stderr

    printf("Integration test: scanning for '%s' (timeout %ds)…\n",
           device_name.c_str(), timeout_s);

    BleManager ble(device_name,
                   "/org/bluez/hci0",                        // Linux adapter path (ignored on macOS)
                   "0000ffe0-0000-1000-8000-00805f9b34fb",   // LiTime service
                   "0000ffe1-0000-1000-8000-00805f9b34fb",   // notify
                   "0000ffe2-0000-1000-8000-00805f9b34fb");  // write
    ble.set_poll_command(litime::build_request());

    std::mutex              mu;
    std::condition_variable cv;
    BatterySnapshot         snap;
    bool                    got_snap = false;
    std::string             last_state;

    auto parser  = std::make_shared<litime::Parser>();
    auto current = std::make_shared<BatterySnapshot>();

    ble.set_state_callback([&](BleState s, const std::string& msg) {
        std::string desc = std::string(ble_state_str(s));
        if (!msg.empty()) desc += ": " + msg;
        printf("[state] %s\n", desc.c_str());
        fflush(stdout);
        std::lock_guard<std::mutex> lk(mu);
        last_state = ble_state_str(s);
        cv.notify_all();
    });

    ble.set_discovery_callback([&](const DiscoveredDevice& dev) {
        printf("[scan]  %-30s  %4d dBm  %s  %s\n",
               dev.name.empty() ? "(unnamed)" : dev.name.c_str(),
               dev.rssi,
               dev.has_target_service ? "[JBD/LiTime]" : "            ",
               dev.id.c_str());
        fflush(stdout);
    });

    ble.set_data_callback([&](const std::vector<uint8_t>& raw) {
        printf("[data]  %zu bytes received\n", raw.size());
        fflush(stdout);
        auto res = parser->feed(raw.data(), raw.size());
        if (res == litime::Parser::Result::COMPLETE) {
            parser->apply(*current);
            parser->reset();
            if (current->valid) {
                std::lock_guard<std::mutex> lk(mu);
                snap     = *current;
                got_snap = true;
                cv.notify_all();
            }
        }
    });

    if (!ble.start()) {
        fprintf(stderr, "FAIL: BleManager::start() returned false\n");
        return 1;
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        bool ok = cv.wait_for(lk, std::chrono::seconds(timeout_s),
                              [&] { return got_snap; });
        if (!ok) {
            fprintf(stderr, "\nFAIL: timed out after %ds without receiving battery data\n",
                    timeout_s);
            ble.stop();
            return 1;
        }
    }

    ble.stop();

    printf("\n=== Battery Snapshot ===\n");
    printf("  Device:      %s  (%s)\n",
           snap.device ? snap.device->device_name.c_str() : "",
           snap.device ? snap.device->ble_address.c_str() : "");
    printf("  Voltage:     %.3f V\n",   static_cast<double>(snap.total_voltage_v));
    printf("  Current:     %+.3f A\n",  static_cast<double>(snap.current_a));
    printf("  Power:       %+.1f W\n",  static_cast<double>(snap.power_w));
    printf("  SoC:         %.1f %%\n",  static_cast<double>(snap.soc_pct));
    printf("  Remaining:   %.2f / %.2f Ah\n", static_cast<double>(snap.remaining_ah), static_cast<double>(snap.nominal_ah));
    printf("  Cells (%zu):", snap.cell_voltages_v.size());
    for (float v : snap.cell_voltages_v) printf("  %.3f", static_cast<double>(v));
    printf("\n");
    printf("  Cell delta:  %.3f V\n",   static_cast<double>(snap.cell_delta_v));
    printf("  Temps (°C):");
    for (float t : snap.temperatures_c) printf("  %.1f", static_cast<double>(t));
    printf("\n");
    if (snap.time_remaining_h > 0.0f)
        printf("  Time left:   %.2f h\n", static_cast<double>(snap.time_remaining_h));

    printf("\n=== Checks ===\n");
    std::vector<Check> checks;
    bool all_pass = run_checks(snap, checks);
    for (const auto& c : checks)
        printf("  [%s]  %s\n", c.pass ? "PASS" : "FAIL", c.name);

    printf("\n%s\n", all_pass ? "RESULT: PASS" : "RESULT: FAIL");
    return all_pass ? 0 : 1;
}
