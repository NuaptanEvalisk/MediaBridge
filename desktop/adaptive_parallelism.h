// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>

// Throughput-based, deliberately conservative concurrency search.
// It samples acknowledged bytes only while the transfer is actively sending
// chunks. After each improvement it probes a slightly higher concurrency and
// settles on the best tested value at the first plateau/regression.
class AdaptiveParallelism {
public:
    enum class Decision { None, Increase, Settled, Backoff };
    static constexpr int kInitial = 4;
    static constexpr int kMaximum = 24;
    static constexpr std::int64_t kWarmupMs = 3000;
    static constexpr std::int64_t kMeasureMs = 10000;
    static constexpr std::int64_t kMinSampleBytes = 16LL * 1024 * 1024;
    static constexpr double kMinImprovement = 0.08;

    struct Result {
        Decision decision = Decision::None;
        int concurrency = kInitial;
        double measuredMiBps = 0;
        double bestMiBps = 0;
    };

    void reset() {
        current_ = best_ = kInitial;
        bestRate_ = 0;
        hasBaseline_ = settled_ = false;
        stage_ = Stage::Warmup;
        stageStartMs_ = -1;
        bytesAtStart_ = 0;
    }

    AdaptiveParallelism() { reset(); }

    int current() const { return current_; }
    int best() const { return best_; }
    bool settled() const { return settled_; }

    // Call when a file starts or after a manual pause. Keep the best-known
    // concurrency but discard a potentially misleading partial measurement.
    void interrupt() {
        stage_ = Stage::Warmup;
        stageStartMs_ = -1;
    }

    Result sample(std::int64_t activeMs, std::int64_t acknowledgedBytes,
                  int chunksRemaining, bool hasEnoughWork = true) {
        Result result{Decision::None, current_, 0, bestRate_ / 1048576.0};
        if (settled_ || !hasEnoughWork || chunksRemaining < current_ * 2) {
            interrupt();
            return result;
        }
        if (stageStartMs_ < 0) {
            stageStartMs_ = activeMs;
            bytesAtStart_ = acknowledgedBytes;
            return result;
        }
        const std::int64_t elapsed = activeMs - stageStartMs_;
        if (elapsed < 0 || acknowledgedBytes < bytesAtStart_) {
            interrupt();
            return result;
        }
        if (stage_ == Stage::Warmup) {
            if (elapsed >= kWarmupMs) {
                stage_ = Stage::Measure;
                stageStartMs_ = activeMs;
                bytesAtStart_ = acknowledgedBytes;
            }
            return result;
        }
        if (elapsed < kMeasureMs ||
            acknowledgedBytes - bytesAtStart_ < kMinSampleBytes) return result;

        const double rate = 1000.0 * double(acknowledgedBytes - bytesAtStart_) / double(elapsed);
        result.measuredMiBps = rate / 1048576.0;
        if (!hasBaseline_ || rate > bestRate_ * (1.0 + kMinImprovement)) {
            hasBaseline_ = true;
            best_ = current_;
            bestRate_ = rate;
            result.bestMiBps = rate / 1048576.0;
            if (current_ == kMaximum) {
                settled_ = true;
                result.decision = Decision::Settled;
            } else {
                // Fine probing at low concurrency, coarser probing at high
                // concurrency. Never overshoot the hard limit of 24.
                current_ = std::min(kMaximum, current_ + (current_ < 12 ? 2 : 4));
                result.decision = Decision::Increase;
            }
        } else {
            settled_ = true;
            current_ = best_;
            result.decision = Decision::Settled;
        }
        result.concurrency = current_;
        interrupt();
        return result;
    }

    // A transient failure during a probe is a reason to stop increasing
    // pressure; keep the last known stable concurrency.
    Result networkFailure() {
        Result result{Decision::None, current_, 0, bestRate_ / 1048576.0};
        if (!settled_) {
            const int before = current_;
            if (before > best_) {
                current_ = best_;
                settled_ = true;
                result.decision = Decision::Backoff;
            }
            result.concurrency = current_;
        }
        interrupt();
        return result;
    }

private:
    enum class Stage { Warmup, Measure };
    int current_ = kInitial;
    int best_ = kInitial;
    double bestRate_ = 0;
    bool hasBaseline_ = false, settled_ = false;
    Stage stage_ = Stage::Warmup;
    std::int64_t stageStartMs_ = -1, bytesAtStart_ = 0;
};
