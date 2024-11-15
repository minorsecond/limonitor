#pragma once
#include "ble_types.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class BleState {
    DISCONNECTED,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    READY,
    ERROR,
};

inline const char* ble_state_str(BleState s) {
    switch (s) {
        case BleState::DISCONNECTED: return "disconnected";
        case BleState::SCANNING:     return "scanning";
        case BleState::CONNECTING:   return "connecting";
        case BleState::DISCOVERING:  return "discovering";
        case BleState::READY:        return "ready";
        case BleState::ERROR:        return "error";
    }
    return "unknown";
}

// Platform-independent BLE manager.
// On Linux  : BlueZ via GLib/GIO D-Bus  (ble_manager_linux.cpp)
// On macOS  : CoreBluetooth             (ble_manager_macos.mm)
//
// Discovery mode (no target set):
//   Scans and reports all visible devices via discovery_cb.
//   Call connect_to(device.id) from any thread to connect to one.
//
// Targeted mode (-n NAME or -a ADDR):
//   Auto-connects when the matching device appears; discovery_cb still fires
//   for every device seen so the TUI can show "nearby" context.
//
// macOS note: device.id is the CoreBluetooth UUID, not the BLE MAC address.
class BleManager {
public:
    using DataCb      = std::function<void(const std::vector<uint8_t>&)>;
    using StateCb     = std::function<void(BleState, const std::string&)>;
    using DiscoveryCb = std::function<void(const DiscoveredDevice&)>;

    BleManager(const std::string& target_addr_or_name,
               const std::string& adapter_path,
               const std::string& service_uuid,
               const std::string& notify_uuid,
               const std::string& write_uuid);
    ~BleManager();

    BleManager(const BleManager&) = delete;
    BleManager& operator=(const BleManager&) = delete;

    bool start();
    void stop();

    // Must be called before start() or from the same thread.
    void set_poll_command(std::vector<uint8_t> cmd);

    // Thread-safe: queue bytes for the write characteristic.
    bool send(const std::vector<uint8_t>& data);

    // Thread-safe: initiate connection to a device seen during scanning.
    // Typically called after the user selects a device from the picker.
    void connect_to(const std::string& device_id);

    void set_data_callback(DataCb cb);
    void set_state_callback(StateCb cb);
    void set_discovery_callback(DiscoveryCb cb);   // fired for every device seen

    BleState    state()          const;
    std::string device_address() const;
    std::string device_name()    const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
