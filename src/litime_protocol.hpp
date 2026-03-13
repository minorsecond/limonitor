#pragma once
#include "battery_data.hpp"
#include <cstdint>
#include <vector>

// LiTime BLE protocol (confirmed via live GATT probing 2026-03-13).
// Device: L-12100BNNA70 (Beken BK343x BLE chip, BMS app FW 6.3.0)
// Service FFE0: notify FFE1 (responses), write FFE2 (commands), AT-config FFE3.
// Only one command is supported — see build_request() / parse().

namespace litime {

std::vector<uint8_t> build_request();

static constexpr size_t MIN_RESPONSE_LEN = 66;

bool parse(const uint8_t* data, size_t len, BatterySnapshot& snap);

class Parser {
public:
    enum class Result { NEED_MORE, COMPLETE, ERROR };

    Result feed(const uint8_t* data, size_t len);
    void   apply(BatterySnapshot& snap) const;
    void   reset();

private:
    std::vector<uint8_t> buf_;
};

} // namespace litime
