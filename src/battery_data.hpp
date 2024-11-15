#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct ProtectionStatus {
    bool cell_overvoltage{false};
    bool cell_undervoltage{false};
    bool pack_overvoltage{false};
    bool pack_undervoltage{false};
    bool charge_overtemp{false};
    bool charge_undertemp{false};
    bool discharge_overtemp{false};
    bool discharge_undertemp{false};
    bool charge_overcurrent{false};
    bool discharge_overcurrent{false};
    bool short_circuit{false};
    bool front_end_ic_error{false};
    bool mosfet_software_lock{false};

    bool any() const {
        return cell_overvoltage || cell_undervoltage || pack_overvoltage ||
               pack_undervoltage || charge_overtemp || charge_undertemp ||
               discharge_overtemp || discharge_undertemp || charge_overcurrent ||
               discharge_overcurrent || short_circuit || front_end_ic_error ||
               mosfet_software_lock;
    }
};

struct BatterySnapshot {
    std::chrono::system_clock::time_point timestamp;

    // Pack level
    double total_voltage_v{0.0};
    double current_a{0.0};       // positive = discharging, negative = charging
    double remaining_ah{0.0};
    double nominal_ah{0.0};
    double soc_pct{0.0};
    double power_w{0.0};
    double time_remaining_h{0.0};
    uint16_t cycle_count{0};

    // MOSFET states
    bool charge_mosfet{false};
    bool discharge_mosfet{false};

    // Cell voltages (V)
    std::vector<double> cell_voltages_v;
    double cell_min_v{0.0};
    double cell_max_v{0.0};
    double cell_delta_v{0.0};

    // Temperatures (°C)
    std::vector<double> temperatures_c;

    ProtectionStatus protection;

    // Device info
    std::string device_name;
    std::string hw_version;
    std::string sw_version;
    std::string ble_address;

    bool valid{false};
};
