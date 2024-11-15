#pragma once
#include "battery_data.hpp"
#include <cstdint>
#include <vector>

// JBD BMS (Jiabaida) protocol — used by LiTime and many other LiFePO4 packs.
//
// Request packet: DD A5 [cmd] 00 [ck_hi] [ck_lo] 77
// Response packet: DD [cmd] [status] [len] [data...] [ck_hi] [ck_lo] 77
//
// Checksum = (0x10000 - sum(bytes from cmd to end-of-data)) & 0xFFFF

namespace jbd {

constexpr uint8_t CMD_BASIC_INFO   = 0x03;
constexpr uint8_t CMD_CELL_VOLTS   = 0x04;
constexpr uint8_t CMD_DEVICE_INFO  = 0x05;

// Build a read-request packet for the given register/command.
std::vector<uint8_t> build_request(uint8_t cmd);

// Incremental parser that handles reassembly across multiple BLE notifications.
class Parser {
public:
    enum class Result { NEED_MORE, COMPLETE, ERROR };

    // Feed raw BLE notification bytes. Returns COMPLETE when a full valid
    // response has been parsed and apply_*() can be called.
    Result feed(const uint8_t* data, size_t len);

    uint8_t last_command() const { return last_cmd_; }

    // Populate a BatterySnapshot from the last parsed packet.
    // Only the fields relevant to the command are written.
    void apply(BatterySnapshot& snap) const;

    void reset();

private:
    std::vector<uint8_t> buf_;   // reassembly buffer
    std::vector<uint8_t> payload_; // parsed payload (after header, before checksum/end)
    uint8_t last_cmd_{0};

    Result try_parse();
    static uint16_t checksum(const uint8_t* d, size_t n);
};

} // namespace jbd
