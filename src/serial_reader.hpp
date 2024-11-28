#pragma once
#include "pwrgate.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Reads EpicPowerGate 2 ASCII lines from a serial port in a background thread.
// Pairs the status line ("PS=...") with the following target line ("TargetV=...")
// and fires the callback with a parsed PwrGateSnapshot.
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
