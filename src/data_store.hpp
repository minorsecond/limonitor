#pragma once
#include "battery_data.hpp"
#include "ble_types.hpp"
#include "pwrgate.hpp"
#include "tx_events.hpp"
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

    std::vector<TxEvent> tx_events(size_t n = 100) const;

    // Set the net-positive-battery-current threshold that starts a TX event.
    // Default matches TX_THRESHOLD_A in tx_events.hpp.
    void set_tx_threshold(double amps);

    void        set_purchase_date(const std::string& d);
    std::string purchase_date() const;

private:
    void process_tx_detection(const BatterySnapshot& snap);
    void process_tx_detection(const PwrGateSnapshot& snap);
    // Call only while holding mu_
    std::optional<PwrGateSnapshot> latest_pwrgate_locked() const;
    std::optional<BatterySnapshot> latest_locked() const;

    mutable std::mutex mu_;
    std::deque<BatterySnapshot> ring_;
    std::deque<PwrGateSnapshot> pwrgate_ring_;
    std::deque<TxEvent> tx_events_;
    static constexpr size_t TX_EVENTS_MAX = 100;
    size_t max_history_;
    std::vector<Observer> observers_;
    std::string ble_state_{"disconnected"};
    std::string purchase_date_;
    std::vector<DiscoveredDevice> discovered_;

    // TX detection threshold (amps, net positive battery discharge)
    double tx_threshold_{TX_THRESHOLD_A};

    // Active TX event state
    bool tx_active_{false};
    double tx_start_time_{0};
    double tx_peak_current_{0};
    double tx_peak_power_{0};
};
