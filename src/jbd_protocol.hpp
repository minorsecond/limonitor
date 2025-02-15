#pragma once
#include "battery_data.hpp"
#include <cstdint>
#include <vector>

// JBD BMS protocol

namespace jbd {

constexpr uint8_t CMD_BASIC_INFO   = 0x03;
constexpr uint8_t CMD_CELL_VOLTS   = 0x04;
constexpr uint8_t CMD_DEVICE_INFO  = 0x05;

std::vector<uint8_t> build_request(uint8_t cmd);

class Parser {
public:
    enum class Result { NEED_MORE, COMPLETE, ERROR };

    Result feed(const uint8_t* data, size_t len);
    uint8_t last_command() const { return last_cmd_; }
    void apply(BatterySnapshot& snap) const;

    void reset();

private:
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> payload_;
    uint8_t last_cmd_{0};

    Result try_parse();
    static uint16_t checksum(const uint8_t* d, size_t n);
};

} // namespace jbd
