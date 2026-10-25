#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace lsm {

struct TraceSpan {
    std::string name;
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    uint64_t duration_ns{0};
};

// -----------------------------------------------------------------------------
// ReadPathTracer: Sub-microsecond RAII execution span profiler
// Overhead <= 200 ns per query
// -----------------------------------------------------------------------------
class ReadPathTracer {
public:
    explicit ReadPathTracer(bool enabled = true);
    ~ReadPathTracer() = default;

    void StartSpan(const char* name);
    void EndSpan();

    bool is_enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    const std::vector<TraceSpan>& GetSpans() const { return spans_; }
    uint64_t TotalDurationNs() const;

    std::string DumpJSON() const;
    std::string ToString() const;
    void Reset();

private:
    bool enabled_{true};
    std::vector<TraceSpan> spans_{};
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> active_stack_{};
};

class TraceScope {
public:
    TraceScope(ReadPathTracer* tracer, const char* name)
        : tracer_(tracer) {
        if (tracer_ && tracer_->is_enabled()) {
            tracer_->StartSpan(name);
        }
    }

    ~TraceScope() {
        if (tracer_ && tracer_->is_enabled()) {
            tracer_->EndSpan();
        }
    }

private:
    ReadPathTracer* tracer_{nullptr};
};

#define TRACE_READ_SPAN(tracer, name) lsm::TraceScope __scope_##__LINE__(tracer, name)

} // namespace lsm
