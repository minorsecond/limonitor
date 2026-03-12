#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <memory>

struct ProtectionStatus {
    union {
        struct {
            uint16_t cell_overvoltage : 1;
            uint16_t cell_undervoltage : 1;
            uint16_t pack_overvoltage : 1;
            uint16_t pack_undervoltage : 1;
            uint16_t charge_overtemp : 1;
            uint16_t charge_undertemp : 1;
            uint16_t discharge_overtemp : 1;
            uint16_t discharge_undertemp : 1;
            uint16_t charge_overcurrent : 1;
            uint16_t discharge_overcurrent : 1;
            uint16_t short_circuit : 1;
            uint16_t front_end_ic_error : 1;
            uint16_t mosfet_software_lock : 1;
            uint16_t reserved : 3;
        };
        uint16_t all{0};
    };

    bool any() const {
        return all != 0;
    }
};

struct DeviceInfo {
    std::string device_name;
    std::string hw_version;
    std::string sw_version;
    std::string ble_address;
};

struct BatterySnapshot {
    std::chrono::system_clock::time_point timestamp;

    // Pack level (floats for memory efficiency, double precision not required for telemetry)
    float total_voltage_v{0.0f};
    float current_a{0.0f};       // positive = discharging, negative = charging
    float remaining_ah{0.0f};
    float nominal_ah{0.0f};
    float soc_pct{0.0f};
    float power_w{0.0f};
    float time_remaining_h{0.0f};
    float cell_min_v{0.0f};
    float cell_max_v{0.0f};
    float cell_delta_v{0.0f};
    
    uint16_t cycle_count{0};
    ProtectionStatus protection;

    // MOSFET states
    bool charge_mosfet{false};
    bool discharge_mosfet{false};
    bool valid{false};

    // Cell voltages (V)
    std::vector<float> cell_voltages_v;
    // Temperatures (°C)
    std::vector<float> temperatures_c;

    // Shared device info (saves ~120 bytes per snapshot in history)
    std::shared_ptr<DeviceInfo> device;
};
