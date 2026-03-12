#pragma once
#include "analytics.hpp"
#include "system_load.hpp"
#include "battery_data.hpp"
#include "ble_types.hpp"
#include "config.hpp"
#include "pwrgate.hpp"
#include "system_events.hpp"
#include "tx_events.hpp"
#include <chrono>

class Database;
class AnalyticsExtensions;

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ring buffer for battery snapshots
class DataStore {
public:
    explicit DataStore(size_t max_history = 2880);
    ~DataStore();

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

    std::vector<SystemEvent> system_events(size_t n = 50) const;

    std::chrono::steady_clock::time_point startup_time() const;
    std::chrono::system_clock::time_point last_bms_update() const;
    std::chrono::system_clock::time_point last_charger_update() const;

    void set_tx_threshold(double amps);

    void        set_purchase_date(const std::string& d);
    std::string purchase_date() const;

    void set_database(Database* db);
    void set_config(const Config& cfg);
    void load_system_events_from_db(const std::vector<SystemEvent>& events);
    void set_loading_history(bool loading);  // when true, don't push events (used during startup load)

    void flush_db();  // manually trigger telemetry flush to DB

    // Analytics — call after DataStore is constructed
    void set_rated_capacity(double ah);
    void update_self_monitor();
    AnalyticsSnapshot analytics() const;
    void push_system_event(const std::string& msg); // Also used for testing

    // Analytics extensions (anomaly, resistance, solar, weather, health)
    void init_extensions(const Config& cfg, Database* db = nullptr);
    const AnalyticsExtensions* extensions() const { return extensions_.get(); }

    // System load calibration config (for runtime estimates and analytics)
    void set_system_load_config(const SystemLoadConfig& cfg);
    const SystemLoadConfig& system_load_config() const { return system_load_config_; }

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
    std::deque<SystemEvent> system_events_;
    static constexpr size_t TX_EVENTS_MAX = 100;
    static constexpr size_t SYSTEM_EVENTS_MAX = 200;
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

    std::chrono::steady_clock::time_point startup_time_;
    std::chrono::system_clock::time_point last_bms_update_{};
    std::chrono::system_clock::time_point last_charger_update_{};

    // Previous state for event detection
    bool prev_solar_active_{false};
    std::string prev_charging_stage_;
    bool prev_charging_{false};
    double last_charging_event_time_{0};  // debounce charging started/stopped
    double last_imbalance_event_time_{0}; // debounce cell imbalance
    double last_temp_event_time_{0};      // debounce high temp
    bool soc_90_reported_{false};
    bool prev_cell_imbalance_{false};
    bool prev_high_temp_{false};

    Database* db_{nullptr};
    bool loading_history_{false};

    void process_system_events(const BatterySnapshot& snap,
                              const std::optional<PwrGateSnapshot>& pg,
                              const AnalyticsSnapshot& an);

    void maybe_flush_db();
    std::vector<BatterySnapshot> battery_buffer_;
    std::vector<PwrGateSnapshot> charger_buffer_;
    std::chrono::steady_clock::time_point last_db_write_{std::chrono::steady_clock::now()};

    // Analytics engine
    AnalyticsEngine analytics_;
    Config cfg_;
    SystemLoadConfig system_load_config_;

    // Analytics extensions (optional)
    std::unique_ptr<AnalyticsExtensions> extensions_;
};
