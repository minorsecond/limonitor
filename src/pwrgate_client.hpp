#pragma once
#include "pwrgate.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// polls remote /api/charger, runs in background thread
class PwrGateClient {
public:
    using SnapCb = std::function<void(const PwrGateSnapshot&)>;

    PwrGateClient(std::string host, uint16_t port, int poll_interval_s = 5);
    ~PwrGateClient();

    bool start();
    void stop();
    void set_callback(SnapCb cb) { cb_ = std::move(cb); }

    const std::string& host() const { return host_; }
    uint16_t           port() const { return port_; }

private:
    std::string           host_;
    uint16_t              port_;
    int                   poll_interval_s_;
    std::atomic<bool>     running_{false};
    std::thread           thread_;
    SnapCb                cb_;
    std::mutex            wake_mu_;
    std::condition_variable wake_cv_;

    void        poll_loop();
    std::string http_get(const std::string& path);
    static bool parse_json(const std::string& body, PwrGateSnapshot& snap);
};
