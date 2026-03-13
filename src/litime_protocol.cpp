#include "litime_protocol.hpp"
#include "logger.hpp"

namespace litime {

// Request: 00 00 04 01 13 55 AA 17
//   bytes 0-1: fixed frame start
//   byte  2:   payload length (04)
//   byte  3:   command (01 = telemetry request; only supported command)
//   byte  4:   argument (0x13)
//   bytes 5-6: magic 55 AA
//   byte  7:   checksum = sum(bytes[2..6]) & 0xFF = 0x17
// Response header: 00 00 65 01 93 55 AA <status>
//   byte  2:   0x65 = 101 bytes of data follow
//   byte  4:   arg | 0x80 (response flag)
//   byte  7:   0x00 = OK, 0x01/0x03 = error
// JBD protocol and all other command variants return an error response.
std::vector<uint8_t> build_request() {
    static const std::vector<uint8_t> req = {0x00, 0x00, 0x04, 0x01, 0x13, 0x55, 0xAA, 0x17};
    return req;
}

static inline uint16_t u16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
static inline uint32_t u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
static inline int32_t s32le(const uint8_t* p) { return static_cast<int32_t>(u32le(p)); }
static inline int16_t s16le(const uint8_t* p) { return static_cast<int16_t>(u16le(p)); }

// 105-byte response layout (offsets, little-endian, confirmed 2026-03-13):
//    0-7   header  00 00 65 01 93 55 AA 00
//    8-11  uint32  instantaneous voltage, mV (fluctuates; use [12] for monitoring)
//   12-15  uint32  filtered total voltage, mV
//   16-47  16×uint16  cell voltages, mV (zero = slot not populated)
//   48-51  int32   current, mA (LiTime: positive=charging; negated on parse → positive=discharging)
//   52-53  int16   cell temperature, °C  (direct, no scaling)
//   54-55  int16   BMS temperature, °C   (direct, no scaling)
//   56-75  —       zeros (reserved)
//   76-79  uint32  protection status bitmask:
//                    0x00000004 pack overvoltage       0x00000020 pack undervoltage
//                    0x00000040 charge overcurrent      0x00000080 discharge overcurrent
//                    0x00000100 charge overtemp         0x00000200 discharge overtemp
//                    0x00000400 charge undertemp        0x00000800 discharge undertemp
//                    0x00004000 short circuit
//   80-87  —       zeros (reserved)
//   88-89  uint16  battery state flags (0x0004 = charge FET disabled)
//   90-91  uint16  BMS SoC % (0–100; overrides rem/nom ratio when > 0)
//   92-95  uint32  unknown constant (observed: 100; likely rated Ah capacity)
//   96-99  uint32  cycle count
//  100-103 uint32  unknown constant (observed: 72)
//     104  uint8   checksum
bool parse(const uint8_t* d, size_t len, BatterySnapshot& snap) {
    if (len < MIN_RESPONSE_LEN) {
        LOG_WARN("LiTime: response too short (%zu bytes, need %zu)", len, MIN_RESPONSE_LEN);
        return false;
    }

    snap.total_voltage_v = static_cast<float>(u32le(d + 12) / 1000.0);

    // LiTime sign convention: positive int32 = charging, negative = discharging.
    // We negate here to match the rest of the codebase (positive = discharging).
    snap.current_a = -static_cast<float>(s32le(d + 48) / 1000.0);

    snap.cell_voltages_v.clear();
    snap.cell_voltages_v.reserve(16);
    snap.cell_min_v = 9999.0f;
    snap.cell_max_v = 0.0f;
    for (int i = 0; i < 16; ++i) {
        uint16_t raw = u16le(d + 16 + i * 2);
        if (raw == 0) continue;
        float v = static_cast<float>(raw / 1000.0);
        snap.cell_voltages_v.push_back(v);
        if (v < snap.cell_min_v) snap.cell_min_v = v;
        if (v > snap.cell_max_v) snap.cell_max_v = v;
    }
    if (!snap.cell_voltages_v.empty())
        snap.cell_delta_v = snap.cell_max_v - snap.cell_min_v;

    float cell_temp_c = static_cast<float>(s16le(d + 52));
    float bms_temp_c  = static_cast<float>(s16le(d + 54));
    snap.temperatures_c = {cell_temp_c, bms_temp_c};

    snap.remaining_ah = static_cast<float>(u16le(d + 62) / 100.0);
    snap.nominal_ah   = static_cast<float>(u16le(d + 64) / 100.0);

    if (snap.nominal_ah > 0.0f)
        snap.soc_pct = (snap.remaining_ah / snap.nominal_ah) * 100.0f;

    // Protection flags at offset 76 (uint32 bitmask)
    if (len > 79) {
        uint32_t prot = u32le(d + 76);
        snap.protection.pack_overvoltage      = (prot & 0x00000004) != 0;
        snap.protection.pack_undervoltage     = (prot & 0x00000020) != 0;
        snap.protection.charge_overcurrent    = (prot & 0x00000040) != 0;
        snap.protection.discharge_overcurrent = (prot & 0x00000080) != 0;
        snap.protection.charge_overtemp       = (prot & 0x00000100) != 0;
        snap.protection.discharge_overtemp    = (prot & 0x00000200) != 0;
        snap.protection.charge_undertemp      = (prot & 0x00000400) != 0;
        snap.protection.discharge_undertemp   = (prot & 0x00000800) != 0;
        snap.protection.short_circuit         = (prot & 0x00004000) != 0;
        // MOSFET discharge state: off when over-discharge protection active
        snap.discharge_mosfet = !(prot & 0x00000020);
    }

    // Battery state + BMS SoC at offsets 88-91
    if (len > 91) {
        uint16_t bstate = u16le(d + 88);
        snap.charge_mosfet = !(bstate & 0x0004);  // off when charge explicitly disabled
        uint16_t bms_soc = u16le(d + 90);
        if (bms_soc > 0)
            snap.soc_pct = static_cast<float>(bms_soc);
    }

    // Cycle count at offset 96 (uint32 LE)
    if (len > 99)
        snap.cycle_count = static_cast<uint16_t>(u32le(d + 96));

    snap.power_w = snap.total_voltage_v * snap.current_a;
    // positive current_a = discharging: time until empty
    // negative current_a = charging:   time until full
    if (snap.current_a > 0.01f)
        snap.time_remaining_h = snap.remaining_ah / snap.current_a;
    else if (snap.current_a < -0.01f)
        snap.time_remaining_h = (snap.nominal_ah - snap.remaining_ah) / (-snap.current_a);
    else
        snap.time_remaining_h = 0.0f;

    snap.valid = true;
    return true;
}

void Parser::reset() { buf_.clear(); buf_.reserve(MIN_RESPONSE_LEN); }

Parser::Result Parser::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    return (buf_.size() >= MIN_RESPONSE_LEN) ? Result::COMPLETE : Result::NEED_MORE;
}

void Parser::apply(BatterySnapshot& snap) const {
    if (buf_.size() >= MIN_RESPONSE_LEN)
        parse(buf_.data(), buf_.size(), snap);
}

} // namespace litime
