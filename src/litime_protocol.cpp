#include "litime_protocol.hpp"
#include "logger.hpp"

namespace litime {

// ---------------------------------------------------------------------------
// Request builder
// ---------------------------------------------------------------------------
// Bytes (from reference JS, big-endian uint16 pairs):
//   setUint16(0, 0x0000) -> 0x00 0x00
//   setUint16(2, 0x0401) -> 0x04 0x01
//   setUint16(4, 0x1355) -> 0x13 0x55
//   setUint16(6, 0xAA17) -> 0xAA 0x17
std::vector<uint8_t> build_request() {
    return {0x00, 0x00, 0x04, 0x01, 0x13, 0x55, 0xAA, 0x17};
}

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Response layout (offsets into the notification payload, little-endian):
//   12-15  uint32  total voltage, mV
//   16-47  16×uint16  cell voltages, mV  (skip zeros = absent cells)
//   48-51  int32   current, mA  (positive = charging, negative = discharging — negated on parse)
//   52-53  int16   cell temperature, °C
//   54-55  int16   BMS temperature, °C
//   62-63  uint16  remaining capacity, 0.01 Ah
//   64-65  uint16  nominal capacity, 0.01 Ah
// ---------------------------------------------------------------------------
bool parse(const uint8_t* d, size_t len, BatterySnapshot& snap) {
    if (len < MIN_RESPONSE_LEN) {
        LOG_WARN("LiTime: response too short (%zu bytes, need %zu)", len, MIN_RESPONSE_LEN);
        return false;
    }

    snap.total_voltage_v = u32le(d + 12) / 1000.0;

    // LiTime sign convention: positive int32 = charging, negative = discharging.
    // We negate here to match the rest of the codebase (positive = discharging).
    snap.current_a = -(s32le(d + 48) / 1000.0);

    snap.cell_voltages_v.clear();
    snap.cell_min_v = 9999.0;
    snap.cell_max_v = 0.0;
    for (int i = 0; i < 16; ++i) {
        uint16_t raw = u16le(d + 16 + i * 2);
        if (raw == 0) continue;
        double v = raw / 1000.0;
        snap.cell_voltages_v.push_back(v);
        if (v < snap.cell_min_v) snap.cell_min_v = v;
        if (v > snap.cell_max_v) snap.cell_max_v = v;
    }
    if (!snap.cell_voltages_v.empty())
        snap.cell_delta_v = snap.cell_max_v - snap.cell_min_v;

    double cell_temp_c = s16le(d + 52);
    double bms_temp_c  = s16le(d + 54);
    snap.temperatures_c = {cell_temp_c, bms_temp_c};

    snap.remaining_ah = u16le(d + 62) / 100.0;
    snap.nominal_ah   = u16le(d + 64) / 100.0;

    if (snap.nominal_ah > 0.0)
        snap.soc_pct = (snap.remaining_ah / snap.nominal_ah) * 100.0;

    snap.power_w = snap.total_voltage_v * snap.current_a;
    // positive current_a = discharging: time until empty
    // negative current_a = charging:   time until full
    if (snap.current_a > 0.01)
        snap.time_remaining_h = snap.remaining_ah / snap.current_a;
    else if (snap.current_a < -0.01)
        snap.time_remaining_h = (snap.nominal_ah - snap.remaining_ah) / (-snap.current_a);
    else
        snap.time_remaining_h = 0.0;

    snap.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Incremental parser
// ---------------------------------------------------------------------------
void Parser::reset() { buf_.clear(); }

Parser::Result Parser::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    return (buf_.size() >= MIN_RESPONSE_LEN) ? Result::COMPLETE : Result::NEED_MORE;
}

void Parser::apply(BatterySnapshot& snap) const {
    if (buf_.size() >= MIN_RESPONSE_LEN)
        parse(buf_.data(), buf_.size(), snap);
}

} // namespace litime
