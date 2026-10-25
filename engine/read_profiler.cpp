#include "read_profiler.hpp"
#include <sstream>
#include <iomanip>

namespace lsm {

ReadPathTracer::ReadPathTracer(bool enabled)
    : enabled_(enabled) {
    spans_.reserve(16);
    active_stack_.reserve(8);
}

void ReadPathTracer::StartSpan(const char* name) {
    if (!enabled_) return;
    active_stack_.emplace_back(name, std::chrono::steady_clock::now());
}

void ReadPathTracer::EndSpan() {
    if (!enabled_ || active_stack_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    auto top = std::move(active_stack_.back());
    active_stack_.pop_back();

    auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(top.second.time_since_epoch()).count();
    auto end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    uint64_t dur_ns = static_cast<uint64_t>(end_ns - start_ns);

    TraceSpan span;
    span.name = std::move(top.first);
    span.start_ns = static_cast<uint64_t>(start_ns);
    span.end_ns = static_cast<uint64_t>(end_ns);
    span.duration_ns = dur_ns;

    spans_.push_back(std::move(span));
}

uint64_t ReadPathTracer::TotalDurationNs() const {
    uint64_t total = 0;
    for (const auto& s : spans_) {
        total += s.duration_ns;
    }
    return total;
}

std::string ReadPathTracer::DumpJSON() const {
    std::ostringstream oss;
    oss << "{\"spans\":[";
    for (size_t i = 0; i < spans_.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"name\":\"" << spans_[i].name << "\","
            << "\"duration_ns\":" << spans_[i].duration_ns << "}";
    }
    oss << "],\"total_duration_ns\":" << TotalDurationNs() << "}";
    return oss.str();
}

std::string ReadPathTracer::ToString() const {
    std::ostringstream oss;
    oss << "Query Read-Path Execution Trace (Total: " << (TotalDurationNs() / 1000.0) << " us):\n";
    for (const auto& s : spans_) {
        oss << "  - [" << std::setw(20) << std::left << s.name << "] "
            << std::setw(8) << std::right << (s.duration_ns / 1000.0) << " us ("
            << s.duration_ns << " ns)\n";
    }
    return oss.str();
}

void ReadPathTracer::Reset() {
    spans_.clear();
    active_stack_.clear();
}

} // namespace lsm
