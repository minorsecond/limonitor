#pragma once
#include "battery_data.hpp"
#include <cstdint>
#include <vector>

// LiTime LiFePO4 battery BLE protocol.
// Service UUID: FFE0  |  Notify (data from device): FFE1  |  Write (cmd to device): FFE2
//
// A single 8-byte request is written to FFE2.
// The device responds with a ~66-byte notification on FFE1 containing all status fields.

namespace litime {

// Build the 8-byte poll request.
std::vector<uint8_t> build_request();

// Minimum length of a valid response packet.
static constexpr size_t MIN_RESPONSE_LEN = 66;

// Parse a raw response into snap.  Returns true on success.
bool parse(const uint8_t* data, size_t len, BatterySnapshot& snap);

// Incremental parser: accumulates BLE notification bytes across fragments.
// Usage mirrors jbd::Parser for drop-in use in main.cpp data_cb.
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
