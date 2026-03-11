#pragma once
#include <chrono>
#include <string>

// Operations log event types
struct OpsEvent {
    int64_t id{0};
    std::chrono::system_clock::time_point timestamp;
    std::string type;       // note, maintenance_start, maintenance_end, grid_outage,
                            // system_event, user_annotation, grid_test_start, grid_test_end
    std::string subtype;    // e.g. outage, maintenance, test, unknown
    std::string message;
    std::string notes;
    std::string metadata_json;  // JSON: soc, voltage, load, charger, etc.
};
