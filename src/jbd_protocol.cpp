#include "jbd_protocol.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace jbd {

std::vector<uint8_t> build_request(uint8_t cmd) {
    // DD A5 [cmd] 00 [ck_hi] [ck_lo] 77
    // checksum covers: cmd, 0x00
    uint16_t ck = (0x10000u - cmd - 0x00u) & 0xFFFFu;
    return { 0xDD, 0xA5, cmd, 0x00,
             static_cast<uint8_t>(ck >> 8),
             static_cast<uint8_t>(ck & 0xFF),
             0x77 };
}

uint16_t Parser::checksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += d[i];
    return static_cast<uint16_t>((0x10000u - (sum & 0xFFFFu)) & 0xFFFFu);
}

void Parser::reset() {
    buf_.clear();
    payload_.clear();
    last_cmd_ = 0;
}

Parser::Result Parser::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    return try_parse();
}

Parser::Result Parser::try_parse() {
    // Discard bytes until we find start byte 0xDD
    size_t skip = 0;
    while (skip < buf_.size() && buf_[skip] != 0xDD) ++skip;
    if (skip > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(skip));

    if (buf_.size() < 4) return Result::NEED_MORE;

    uint8_t data_len = buf_[3];
    size_t total = 4u + data_len + 2u + 1u;

    if (buf_.size() < total) return Result::NEED_MORE;

    if (buf_[total - 1] != 0x77) {
        LOG_WARN("JBD: bad end byte 0x%02X, discarding", buf_[total - 1]);
        buf_.erase(buf_.begin());
        return try_parse();
    }

    uint16_t stored_ck = (static_cast<uint16_t>(buf_[4 + data_len]) << 8) |
                          buf_[5 + data_len];
    uint16_t calc_ck   = checksum(buf_.data() + 1, 3u + data_len);
    if (stored_ck != calc_ck) {
        LOG_WARN("JBD: checksum mismatch (got %04X, expected %04X)", stored_ck, calc_ck);
        buf_.erase(buf_.begin());
        return try_parse();
    }

    if (buf_[2] != 0x00) {
        LOG_WARN("JBD: error response status=0x%02X for cmd=0x%02X", buf_[2], buf_[1]);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(total));
        return Result::ERROR;
    }

    last_cmd_ = buf_[1];
    payload_.assign(buf_.begin() + 4, buf_.begin() + 4 + data_len);
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(total));
    return Result::COMPLETE;
}

static inline uint16_t u16be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
static inline int16_t s16be(const uint8_t* p) {
    return static_cast<int16_t>(u16be(p));
}

void Parser::apply(BatterySnapshot& snap) const {
    if (payload_.empty()) return;
    const uint8_t* d = payload_.data();
    size_t n = payload_.size();

    if (last_cmd_ == CMD_BASIC_INFO) {
        // Minimum useful size: 23 bytes (no temperatures)
        if (n < 23) { LOG_WARN("JBD: basic info too short (%zu)", n); return; }

        snap.total_voltage_v   = static_cast<float>(u16be(d + 0)  * 0.01);   // 10mV units
        snap.current_a         = static_cast<float>(s16be(d + 2)  * 0.01);   // 10mA units, signed
        snap.remaining_ah      = static_cast<float>(u16be(d + 4)  * 0.01);   // 10mAh units
        snap.nominal_ah        = static_cast<float>(u16be(d + 6)  * 0.01);
        snap.cycle_count       = u16be(d + 8);
        // bytes 10-11: production date (skip)
        // bytes 12-15: balance status (skip for now)
        uint16_t prot = u16be(d + 16);
        snap.protection.cell_overvoltage     = (prot >> 0)  & 1;
        snap.protection.cell_undervoltage    = (prot >> 1)  & 1;
        snap.protection.pack_overvoltage     = (prot >> 2)  & 1;
        snap.protection.pack_undervoltage    = (prot >> 3)  & 1;
        snap.protection.charge_overtemp      = (prot >> 4)  & 1;
        snap.protection.charge_undertemp     = (prot >> 5)  & 1;
        snap.protection.discharge_overtemp   = (prot >> 6)  & 1;
        snap.protection.discharge_undertemp  = (prot >> 7)  & 1;
        snap.protection.charge_overcurrent   = (prot >> 8)  & 1;
        snap.protection.discharge_overcurrent= (prot >> 9)  & 1;
        snap.protection.short_circuit        = (prot >> 10) & 1;
        snap.protection.front_end_ic_error   = (prot >> 11) & 1;
        snap.protection.mosfet_software_lock = (prot >> 12) & 1;

        // byte 18: SW version (BCD)
        uint8_t sw_ver = d[18];
        char ver_buf[8];
        std::snprintf(ver_buf, sizeof(ver_buf), "%d.%d", sw_ver >> 4, sw_ver & 0x0F);
        if (!snap.device) snap.device = std::make_shared<DeviceInfo>();
        snap.device->sw_version = ver_buf;

        snap.soc_pct           = static_cast<float>(d[19]);
        uint8_t fet_status     = d[20];
        snap.charge_mosfet     = (fet_status >> 0) & 1;
        snap.discharge_mosfet  = (fet_status >> 1) & 1;
        uint8_t num_ntc        = (n > 22) ? d[22] : 0;

        snap.temperatures_c.clear();
        for (uint8_t i = 0; i < num_ntc; ++i) {
            size_t off = 23 + i * 2;
            if (off + 1 >= n) break;
            float raw_k = static_cast<float>(u16be(d + off) * 0.1);
            snap.temperatures_c.push_back(raw_k - 273.15f);
        }

        // Derived values
        snap.power_w = snap.total_voltage_v * snap.current_a;
        if (snap.current_a > 0.01f)
            snap.time_remaining_h = snap.remaining_ah / snap.current_a;
        else if (snap.current_a < -0.01f)
            snap.time_remaining_h = (snap.nominal_ah - snap.remaining_ah) / (-snap.current_a);
        else
            snap.time_remaining_h = 0.0f;

        snap.valid = true;

    } else if (last_cmd_ == CMD_CELL_VOLTS) {
        size_t cell_count = n / 2;
        snap.cell_voltages_v.resize(cell_count);
        snap.cell_min_v = 9999.0f;
        snap.cell_max_v = 0.0f;
        for (size_t i = 0; i < cell_count; ++i) {
            float v = static_cast<float>(u16be(d + i * 2) * 0.001);   // 1mV units
            snap.cell_voltages_v[i] = v;
            if (v < snap.cell_min_v) snap.cell_min_v = v;
            if (v > snap.cell_max_v) snap.cell_max_v = v;
        }
        snap.cell_delta_v = snap.cell_max_v - snap.cell_min_v;

    } else if (last_cmd_ == CMD_DEVICE_INFO) {
        // Device name: up to 24 bytes, null-terminated
        if (!snap.device) snap.device = std::make_shared<DeviceInfo>();
        size_t name_len = std::min(n, size_t{24});
        snap.device->device_name.assign(reinterpret_cast<const char*>(d),
                                strnlen(reinterpret_cast<const char*>(d), name_len));
    }
}

} // namespace jbd
