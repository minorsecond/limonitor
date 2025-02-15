#pragma once
#include "pwrgate.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// EpicPowerGate 2 serial reader, background thread
class SerialReader {
public:
    using SnapCb = std::function<void(const PwrGateSnapshot&)>;

    SerialReader(std::string device, int baud = 115200);
    ~SerialReader();

    bool start();
    void stop();

    void set_callback(SnapCb cb) { cb_ = std::move(cb); }

    bool        connected() const { return fd_ >= 0; }
    const std::string& device()  const { return device_; }

private:
    std::string       device_;
    int               baud_;
    int               fd_{-1};
    std::atomic<bool> running_{false};
    std::thread       thread_;
    SnapCb            cb_;

    int  open_port();
    void read_loop();
};
