#pragma once
#include "battery_data.hpp"
#include "ble_types.hpp"
#include "pwrgate.hpp"
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Thread-safe ring buffer for battery snapshots.
// Observers registered via on_update() are called after every write
// (from whatever thread called update()).
class DataStore {
public:
    explicit DataStore(size_t max_history = 2880);

    // Write a new snapshot (thread-safe).
    void update(BatterySnapshot snap);

    // Read latest snapshot (thread-safe). Returns nullopt if no data yet.
    std::optional<BatterySnapshot> latest() const;

    // Get last N snapshots, newest last (thread-safe).
    std::vector<BatterySnapshot> history(size_t n = 0) const;

    // Register a callback fired on every update (called from update()'s thread).
    using Observer = std::function<void(const BatterySnapshot&)>;
    void on_update(Observer obs);

    // Connection state for display purposes
    void set_ble_state(const std::string& s);
    std::string ble_state() const;

    // BLE device picker list (populated during scanning)
    void upsert_discovered(DiscoveredDevice dev);   // add or update by id
    void clear_discovered();
    std::vector<DiscoveredDevice> discovered_devices() const;

    // PwrGate / charger data
    void update_pwrgate(PwrGateSnapshot snap);
    std::optional<PwrGateSnapshot> latest_pwrgate() const;
    std::vector<PwrGateSnapshot>   pwrgate_history(size_t n = 0) const;

private:
    mutable std::mutex mu_;
    std::deque<BatterySnapshot> ring_;
    std::deque<PwrGateSnapshot> pwrgate_ring_;
    size_t max_history_;
    std::vector<Observer> observers_;
    std::string ble_state_{"disconnected"};
    std::vector<DiscoveredDevice> discovered_;
};
