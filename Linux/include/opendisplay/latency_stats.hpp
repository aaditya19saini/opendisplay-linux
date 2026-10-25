#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace od {

/// Summary of latency samples (milliseconds) over one reporting window.
struct LatencySummary {
    std::size_t count = 0;
    std::int64_t p50 = 0;
    std::int64_t p95 = 0;
    std::int64_t max = 0;
};

/// Nearest-rank percentile of an unsorted sample set (copied, so the caller's
/// order is untouched). Returns 0 for an empty set.
inline std::int64_t latencyPercentile(std::vector<std::int64_t> samples, const double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    const auto rank = static_cast<std::size_t>(
        std::max(1.0, p / 100.0 * static_cast<double>(samples.size()) + 0.999999));
    return samples[std::min(rank, samples.size()) - 1];
}

inline LatencySummary summarizeLatency(const std::vector<std::int64_t>& samples) {
    LatencySummary summary;
    summary.count = samples.size();
    if (samples.empty()) return summary;
    summary.p50 = latencyPercentile(samples, 50);
    summary.p95 = latencyPercentile(samples, 95);
    summary.max = *std::max_element(samples.begin(), samples.end());
    return summary;
}

/// Thread-safe sample window. `take()` returns the window's summary and
/// starts a new one once `windowMs` has elapsed since the window opened.
class LatencyWindow {
public:
    explicit LatencyWindow(std::int64_t windowMs = 5'000) : windowMs_(windowMs) {}

    void add(const std::int64_t nowMs, const std::int64_t sampleMs) {
        std::lock_guard lock(mutex_);
        if (!openedAt_) openedAt_ = nowMs;
        samples_.push_back(std::max<std::int64_t>(0, sampleMs));
    }

    std::optional<LatencySummary> take(const std::int64_t nowMs) {
        std::lock_guard lock(mutex_);
        if (!openedAt_ || nowMs - *openedAt_ < windowMs_) return std::nullopt;
        auto summary = summarizeLatency(samples_);
        samples_.clear();
        openedAt_ = nowMs;
        return summary;
    }

private:
    std::int64_t windowMs_;
    std::mutex mutex_;
    std::vector<std::int64_t> samples_;
    std::optional<std::int64_t> openedAt_;
};

inline std::string formatLatency(const LatencySummary& summary) {
    return "p50 " + std::to_string(summary.p50) + " ms, p95 " + std::to_string(summary.p95)
        + " ms, max " + std::to_string(summary.max) + " ms";
}

}  // namespace od
