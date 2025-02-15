#include "data_store.hpp"
#include <algorithm>
#include <chrono>

static constexpr int DISCOVERY_UPDATE_INTERVAL_S = 7;

DataStore::DataStore(size_t max_history) : max_history_(max_history) {}

void DataStore::update(BatterySnapshot snap) {
    std::vector<Observer> observers_copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(snap);
        while (ring_.size() > max_history_) ring_.pop_front();
        observers_copy = observers_;
    }
    for (const auto& obs : observers_copy) obs(snap);
}

std::optional<BatterySnapshot> DataStore::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (ring_.empty()) return std::nullopt;
    return ring_.back();
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
    pwrgate_ring_.push_back(std::move(snap));
    while (pwrgate_ring_.size() > max_history_) pwrgate_ring_.pop_front();
}

std::optional<PwrGateSnapshot> DataStore::latest_pwrgate() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (pwrgate_ring_.empty()) return std::nullopt;
    return pwrgate_ring_.back();
}

std::vector<PwrGateSnapshot> DataStore::pwrgate_history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= pwrgate_ring_.size())
        return std::vector<PwrGateSnapshot>(pwrgate_ring_.begin(), pwrgate_ring_.end());
    auto it = pwrgate_ring_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<PwrGateSnapshot>(it, pwrgate_ring_.end());
}
