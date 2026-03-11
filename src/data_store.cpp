#include "data_store.hpp"
#include "analytics/extensions.hpp"
#include "database.hpp"
#include <algorithm>
#include <chrono>
#include <utility>

static constexpr int DISCOVERY_UPDATE_INTERVAL_S = 7;

DataStore::DataStore(size_t max_history)
    : max_history_(max_history),
      startup_time_(std::chrono::steady_clock::now()) {}

DataStore::~DataStore() = default;

// Combined TX detection.
//
// Three trigger paths, any one is enough:
//   1. BMS:     net positive battery discharge  (bat_cur > threshold_a)
//   2. Charger: bat_a spikes high               (chg_a > 5 A, for high-power-charger ramp-up)
//   3. Charger: bat_a goes negative             (chg_a < -threshold_a)
//      EPG2 reports negative bat_a when the battery is sourcing current to the load,
//      so this catches TX events at 1-second charger resolution even when the BMS
//      notification rate is slow (60 s).
//
// Peak current/power is measured from whichever path fired.
static void run_tx_detection(bool& tx_active, double& tx_start_time,
                             double& tx_peak_current, double& tx_peak_power,
                             std::deque<TxEvent>& tx_events,
                             double now, double bat_cur, double bat_v,
                             double chg_a, double chg_v,
                             double threshold_a) {
    double discharge = (bat_cur > 0) ? bat_cur : 0;
    // Path 3: EPG2 reports negative bat_a when battery is discharging into the load.
    double chg_discharge = (chg_a < 0) ? -chg_a : 0;
    static constexpr double CHG_HIGH_A = 5.0;  // charger ramp-up floor (path 2)
    bool above = (discharge > threshold_a)
              || (chg_a > CHG_HIGH_A)
              || (chg_discharge > threshold_a);
    // Use the strongest discharge signal to compute peak
    double effective_cur = std::max({discharge, chg_discharge, chg_a > CHG_HIGH_A ? chg_a : 0.0});
    double effective_v = bat_v;
    if (chg_discharge > 0) effective_v = chg_v;
    double pwr = effective_cur * effective_v;

    if (above) {
        if (!tx_active) {
            tx_active = true;
            tx_start_time = now;
            tx_peak_current = effective_cur;
            tx_peak_power = pwr;
        } else if (effective_cur > tx_peak_current) {
            tx_peak_current = effective_cur;
            tx_peak_power = pwr;
        }
    } else if (tx_active) {
        TxEvent ev;
        ev.start_time = tx_start_time;
        ev.end_time = now;
        ev.duration = now - tx_start_time;
        ev.peak_current = tx_peak_current;
        ev.peak_power = tx_peak_power;
        tx_events.push_back(ev);
        while (tx_events.size() > 100) tx_events.pop_front();
        tx_active = false;
    }
}

void DataStore::process_tx_detection(const BatterySnapshot& snap) {
    if (!snap.valid) return;
    double now = static_cast<double>(std::chrono::system_clock::to_time_t(snap.timestamp));
    auto pg = latest_pwrgate_locked();
    double chg_a = pg ? pg->bat_a : 0;
    double chg_v = pg ? pg->bat_v : snap.total_voltage_v;
    run_tx_detection(tx_active_, tx_start_time_, tx_peak_current_, tx_peak_power_,
                     tx_events_, now, snap.current_a, snap.total_voltage_v, chg_a, chg_v,
                     tx_threshold_);
}

void DataStore::process_tx_detection(const PwrGateSnapshot& snap) {
    if (!snap.valid) return;
    double now = static_cast<double>(std::chrono::system_clock::to_time_t(snap.timestamp));
    auto bat = latest_locked();
    double bat_cur = bat ? bat->current_a : 0;
    double bat_v = bat ? bat->total_voltage_v : snap.bat_v;
    run_tx_detection(tx_active_, tx_start_time_, tx_peak_current_, tx_peak_power_,
                     tx_events_, now, bat_cur, bat_v, snap.bat_a, snap.bat_v,
                     tx_threshold_);
}

void DataStore::set_tx_threshold(double amps) {
    std::lock_guard<std::mutex> lk(mu_);
    tx_threshold_ = amps;
}

void DataStore::set_database(Database* db) {
    db_ = db;
}

void DataStore::load_system_events_from_db(const std::vector<SystemEvent>& events) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : events) {
        system_events_.push_back(e);
        if (system_events_.size() >= SYSTEM_EVENTS_MAX) break;
    }
}

void DataStore::set_loading_history(bool loading) {
    loading_history_ = loading;
}

void DataStore::push_system_event(const std::string& msg) {
    if (loading_history_) return;  // don't push or persist during startup load
    SystemEvent ev;
    ev.timestamp = std::chrono::system_clock::now();
    ev.message = msg;
    system_events_.push_back(ev);
    while (system_events_.size() > SYSTEM_EVENTS_MAX) system_events_.pop_front();
    if (db_) db_->insert_system_event(ev);
}

void DataStore::process_system_events(const BatterySnapshot& snap,
                                      const std::optional<PwrGateSnapshot>& pg,
                                      const AnalyticsSnapshot& an) {
    bool charging = (snap.current_a < -0.1);
    bool solar_active = pg && pg->sol_v > 1.0;

    double now = static_cast<double>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));

    // Charging stage changed — use as canonical charging event (avoids duplicate started/stopped)
    bool stage_changed = (an.charging_stage != prev_charging_stage_ && !prev_charging_stage_.empty());
    if (stage_changed && (now - last_charging_event_time_ >= 60.0)) {
        if (an.charging_stage == "Bulk" || an.charging_stage == "Float") {
            push_system_event("Charging started");
        } else if (an.charging_stage == "Idle") {
            push_system_event("Charging stopped");
        } else {
            push_system_event("Charger entered " + an.charging_stage);
        }
        last_charging_event_time_ = now;
    }
    // Battery-based fallback when no charger stage (e.g. charger disconnected)
    else if (!stage_changed && (now - last_charging_event_time_ >= 60.0)) {
        if (charging && !prev_charging_) {
            push_system_event("Charging started");
            last_charging_event_time_ = now;
        } else if (!charging && prev_charging_) {
            push_system_event("Charging stopped");
            last_charging_event_time_ = now;
        }
    }
    prev_charging_ = charging;
    prev_charging_stage_ = an.charging_stage;

    // Solar input detected/lost
    if (solar_active && !prev_solar_active_) {
        push_system_event("Solar input detected");
    } else if (!solar_active && prev_solar_active_) {
        push_system_event("Solar input lost");
    }
    prev_solar_active_ = solar_active;

    // Cell imbalance detected
    bool cell_imbalance = (an.cell_balance_status == "Imbalance" || an.cell_balance_status == "Warning");
    if (cell_imbalance && !prev_cell_imbalance_) {
        push_system_event("Cell imbalance detected");
    }
    prev_cell_imbalance_ = cell_imbalance;

    // High temperature warning
    bool high_temp = (an.temp_status == "Warning" || an.temp_status == "Warm");
    if (high_temp && !prev_high_temp_) {
        push_system_event("High temperature warning");
    }
    prev_high_temp_ = high_temp;

    // Battery reached 90% charge
    if (snap.soc_pct >= 90.0 && !soc_90_reported_) {
        push_system_event("Battery reached 90% charge");
        soc_90_reported_ = true;
    } else if (snap.soc_pct < 85.0) {
        soc_90_reported_ = false;
    }
}

void DataStore::update(BatterySnapshot snap) {
    std::vector<Observer> observers_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        process_tx_detection(snap);
        auto pg_opt = latest_pwrgate_locked();
        analytics_.on_battery(snap, pg_opt.has_value() ? &*pg_opt : nullptr);
        last_bms_update_ = snap.timestamp;
        process_system_events(snap, latest_pwrgate_locked(), analytics_.snapshot());
        ring_.push_back(snap);
        if (extensions_) {
            extensions_->update(snap, pg_opt.has_value() ? &*pg_opt : nullptr,
                               analytics_.snapshot(), ring_, tx_events_);
        }
        while (ring_.size() > max_history_) ring_.pop_front();
        observers_copy = observers_;
    }
    for (const auto& obs : observers_copy) obs(snap);
}

std::optional<BatterySnapshot> DataStore::latest_locked() const {
    if (ring_.empty()) return std::nullopt;
    return ring_.back();
}

std::optional<BatterySnapshot> DataStore::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_locked();
}

std::vector<BatterySnapshot> DataStore::history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= ring_.size()) {
        return std::vector<BatterySnapshot>(ring_.begin(), ring_.end());
    }
    auto it = ring_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<BatterySnapshot>(it, ring_.end());
}

void DataStore::on_update(Observer obs) {
    std::lock_guard<std::mutex> lk(mu_);
    observers_.push_back(std::move(obs));
}

void DataStore::set_ble_state(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    ble_state_ = s;
}
std::string DataStore::ble_state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ble_state_;
}

void DataStore::set_purchase_date(const std::string& d) {
    std::lock_guard<std::mutex> lk(mu_);
    purchase_date_ = d;
    analytics_.set_purchase_date(d);
}
std::string DataStore::purchase_date() const {
    std::lock_guard<std::mutex> lk(mu_);
    return purchase_date_;
}

void DataStore::upsert_discovered(DiscoveredDevice dev) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& d : discovered_) {
        if (d.id == dev.id) {
            // Throttle updates to existing entries so the list stays stable.
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - d.last_seen).count();
            if (age >= DISCOVERY_UPDATE_INTERVAL_S) {
                dev.last_seen = now;
                d = std::move(dev);
            }
            return;
        }
    }
    // New device — add immediately.
    dev.last_seen = now;
    discovered_.push_back(std::move(dev));
}
void DataStore::clear_discovered() {
    std::lock_guard<std::mutex> lk(mu_);
    discovered_.clear();
}
std::vector<DiscoveredDevice> DataStore::discovered_devices() const {
    std::lock_guard<std::mutex> lk(mu_);
    return discovered_;
}

void DataStore::update_pwrgate(PwrGateSnapshot snap) {
    std::lock_guard<std::mutex> lk(mu_);
    process_tx_detection(snap);
    auto bat_opt = latest_locked();
    analytics_.on_charger(snap, bat_opt.has_value() ? &*bat_opt : nullptr);
    last_charger_update_ = snap.timestamp;
    if (extensions_ && bat_opt) {
        extensions_->update(*bat_opt, &snap, analytics_.snapshot(), ring_, tx_events_);
    }
    process_system_events(latest_locked().value_or(BatterySnapshot{}),
                         std::optional<PwrGateSnapshot>(snap), analytics_.snapshot());
    pwrgate_ring_.push_back(std::move(snap));
    while (pwrgate_ring_.size() > max_history_) pwrgate_ring_.pop_front();
}

std::optional<PwrGateSnapshot> DataStore::latest_pwrgate_locked() const {
    if (pwrgate_ring_.empty()) return std::nullopt;
    return pwrgate_ring_.back();
}

std::optional<PwrGateSnapshot> DataStore::latest_pwrgate() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_pwrgate_locked();
}

std::vector<PwrGateSnapshot> DataStore::pwrgate_history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= pwrgate_ring_.size())
        return std::vector<PwrGateSnapshot>(pwrgate_ring_.begin(), pwrgate_ring_.end());
    auto it = pwrgate_ring_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<PwrGateSnapshot>(it, pwrgate_ring_.end());
}

std::vector<TxEvent> DataStore::tx_events(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= tx_events_.size())
        return std::vector<TxEvent>(tx_events_.begin(), tx_events_.end());
    auto it = tx_events_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<TxEvent>(it, tx_events_.end());
}

std::vector<SystemEvent> DataStore::system_events(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= system_events_.size())
        return std::vector<SystemEvent>(system_events_.begin(), system_events_.end());
    auto it = system_events_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<SystemEvent>(it, system_events_.end());
}

std::chrono::steady_clock::time_point DataStore::startup_time() const {
    std::lock_guard<std::mutex> lk(mu_);
    return startup_time_;
}

std::chrono::system_clock::time_point DataStore::last_bms_update() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_bms_update_;
}

std::chrono::system_clock::time_point DataStore::last_charger_update() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_charger_update_;
}

void DataStore::set_rated_capacity(double ah) {
    std::lock_guard<std::mutex> lk(mu_);
    analytics_.set_rated_capacity(ah);
}

AnalyticsSnapshot DataStore::analytics() const {
    std::lock_guard<std::mutex> lk(mu_);
    return analytics_.snapshot();
}

void DataStore::init_extensions(const Config& cfg, Database* db) {
    std::lock_guard<std::mutex> lk(mu_);
    extensions_ = std::make_unique<AnalyticsExtensions>(cfg, db);
}
