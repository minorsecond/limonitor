#include "telemetry.hpp"

namespace testing {

TelemetryBuffer::TelemetryBuffer(size_t batch_size)
    : batch_size_(batch_size > 0 ? batch_size : 50) {}

void TelemetryBuffer::push(const TestTelemetrySample& s) {
    buffer_.push_back(s);
}

std::vector<TestTelemetrySample> TelemetryBuffer::flush() {
    std::vector<TestTelemetrySample> out;
    out.swap(buffer_);
    return out;
}

} // namespace testing
