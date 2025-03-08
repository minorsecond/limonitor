#pragma once
#include <chrono>
#include <string>

struct SystemEvent {
    std::chrono::system_clock::time_point timestamp;
    std::string message;
};
