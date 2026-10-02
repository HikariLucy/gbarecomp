// host_perf.h — presentation/emulation rate meters for the host overlay.
//
// Two numbers are kept strictly apart:
//   EMU     frames the guest actually advanced (PPU frame counter delta).
//   PRESENT frames handed to the display (HostWindow::present calls).
// In Native presentation they match; under fast-forward EMU rises, and in
// Monitor Refresh presentation PRESENT rises alone (repeats of the same frame). Neither is
// the monitor's refresh rate, which is reported separately by the window.
//
// Pure arithmetic over caller-supplied millisecond/microsecond timestamps so
// it is testable without SDL.

#pragma once

#include <cmath>
#include <cstdint>

namespace gbarecomp {

// Rate of a monotonically increasing counter, refreshed once per window.
class RateMeter {
public:
    explicit RateMeter(std::uint32_t window_ms = 500) : window_ms_(window_ms) {}

    // `total` is the cumulative counter value at `now_ms`.
    void sample(std::uint64_t total, std::uint32_t now_ms) {
        if (!started_ || total < start_total_ || now_ms < start_ms_) {
            start_total_ = total;
            start_ms_ = now_ms;
            started_ = true;
            return;
        }
        const std::uint32_t span = now_ms - start_ms_;
        if (span >= window_ms_) {
            rate_ = static_cast<double>(total - start_total_) * 1000.0 / span;
            start_total_ = total;
            start_ms_ = now_ms;
        }
    }
    double rate() const { return rate_; }
    void reset() { started_ = false; rate_ = 0.0; }

private:
    std::uint32_t window_ms_;
    std::uint64_t start_total_ = 0;
    std::uint32_t start_ms_ = 0;
    double rate_ = 0.0;
    bool started_ = false;
};

// Interval (frame time) statistics between consecutive events, refreshed once
// per window: mean, worst interval and jitter (standard deviation of the
// interval) in milliseconds.
class IntervalMeter {
public:
    explicit IntervalMeter(std::uint32_t window_ms = 500) : window_ms_(window_ms) {}

    void tick(std::uint64_t now_us) {
        if (last_us_ != 0 && now_us >= last_us_) {
            const std::uint64_t d = now_us - last_us_;
            sum_us_ += d;
            sum_sq_us_ += static_cast<double>(d) * static_cast<double>(d);
            max_us_ = d > max_us_ ? d : max_us_;
            ++n_;
        }
        last_us_ = now_us;
        if (win_start_us_ == 0) win_start_us_ = now_us;
        if (now_us - win_start_us_ >= static_cast<std::uint64_t>(window_ms_) * 1000u) {
            if (n_) {
                const double mean = static_cast<double>(sum_us_) / n_;
                avg_ms_ = mean / 1000.0;
                max_ms_ = static_cast<double>(max_us_) / 1000.0;
                const double var = sum_sq_us_ / n_ - mean * mean;
                jitter_ms_ = var > 0.0 ? std::sqrt(var) / 1000.0 : 0.0;
            }
            sum_us_ = max_us_ = n_ = 0;
            sum_sq_us_ = 0.0;
            win_start_us_ = now_us;
        }
    }
    double avg_ms() const { return avg_ms_; }
    double max_ms() const { return max_ms_; }
    double jitter_ms() const { return jitter_ms_; }
    void reset() { *this = IntervalMeter(window_ms_); }

private:
    std::uint32_t window_ms_;
    std::uint64_t last_us_ = 0, win_start_us_ = 0;
    std::uint64_t sum_us_ = 0, max_us_ = 0, n_ = 0;
    double sum_sq_us_ = 0.0;
    double avg_ms_ = 0.0, max_ms_ = 0.0, jitter_ms_ = 0.0;
};

}  // namespace gbarecomp
