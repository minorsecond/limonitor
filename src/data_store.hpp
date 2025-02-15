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

// ring buffer for battery snapshots
class DataStore {
public:
    explicit DataStore(size_t max_history = 2880);

    void update(BatterySnapshot snap);
    std::optional<BatterySnapshot> latest() const;
    std::vector<BatterySnapshot> history(size_t n = 0) const;

    using Observer = std::function<void(const BatterySnapshot&)>;
    void on_update(Observer obs);

    void set_ble_state(const std::string& s);
    std::string ble_state() const;

    void upsert_discovered(DiscoveredDevice dev);
    void clear_discovered();
    std::vector<DiscoveredDevice> discovered_devices() const;

    void update_pwrgate(PwrGateSnapshot snap);
    std::optional<PwrGateSnapshot> latest_pwrgate() const;
    std::vector<PwrGateSnapshot>   pwrgate_history(size_t n = 0) const;

    void        set_purchase_date(const std::string& d);
    std::string purchase_date() const;

private:
    mutable std::mutex mu_;
    std::deque<BatterySnapshot> ring_;
    std::deque<PwrGateSnapshot> pwrgate_ring_;
    size_t max_history_;
    std::vector<Observer> observers_;
    std::string ble_state_{"disconnected"};
    std::string purchase_date_;
    std::vector<DiscoveredDevice> discovered_;
};
