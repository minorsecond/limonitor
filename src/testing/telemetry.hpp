#pragma once

#include "types.hpp"
#include <vector>

namespace testing {

// Buffered telemetry writer — batches samples for efficient DB writes
class TelemetryBuffer {
public:
    explicit TelemetryBuffer(size_t batch_size = 50);
    void push(const TestTelemetrySample& s);
    std::vector<TestTelemetrySample> flush();

    size_t size() const { return buffer_.size(); }
    bool needs_flush() const { return buffer_.size() >= batch_size_; }

private:
    std::vector<TestTelemetrySample> buffer_;
    size_t batch_size_;
};

} // namespace testing
