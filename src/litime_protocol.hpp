#pragma once
#include "battery_data.hpp"
#include <cstdint>
#include <vector>

// LiTime BLE protocol. Service FFE0, notify FFE1, write FFE2.

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
