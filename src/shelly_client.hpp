#pragma once

class Database;
class DataStore;

namespace shelly {

// Turn relay on or off. Returns true on success.
bool turn_on(Database* db);
bool turn_off(Database* db);

// Live power/status from the Shelly meter (Gen1: /meter/0, Gen2: /rpc/Switch.GetStatus?id=0).
struct Status {
    bool ok{false};        // HTTP request succeeded
    bool relay_on{false};  // current relay state
    double power_w{0};     // current power draw (W)
    double current_a{0};   // current (A), Gen2 only
    double voltage_v{0};   // voltage (V), Gen2 only
    double total_kwh{0};   // accumulated energy (kWh)
};
// Returns Status::ok=false if shelly_host not configured or request fails.
Status get_status(Database* db);

// If battery test is active, check safety/auto-end conditions and turn grid on if needed.
// Call periodically (e.g. every 60s) from battery observer.
void check_test_conditions(Database* db, const DataStore& store);

} // namespace shelly
