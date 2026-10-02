// presentation_pacing.h — host PRESENTATION pacing model (no SDL, no guest state).
//
// Two clocks, never mixed:
//   guest   the GBA's 59.7275 Hz. Owned by FramePacer; nothing here changes it.
//   present how often the host hands a framebuffer to the display.
//
// Presentation: Native      one present per guest frame (historical behaviour).
// Presentation: Monitor     the host may re-present the CURRENT framebuffer at the
//                           monitor's refresh rate while it waits for the next guest
//                           frame. A repeated present never runs the guest, never
//                           drains audio and never touches machine state; it is not
//                           interpolation (the same pixels are shown again).
//
// Everything here is pure arithmetic over caller-supplied monotonic nanosecond
// timestamps so it is testable with a fake clock.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace gbarecomp {

enum class PresentationMode : std::uint8_t { Native = 0, MonitorRefresh = 1 };

inline const char* presentation_mode_name(PresentationMode m) {
    return m == PresentationMode::MonitorRefresh ? "monitor" : "native";
}

// "native" / "monitor" (case-insensitive aliases accepted). Returns false and
// leaves *out untouched for anything else so an invalid config value falls back.
inline bool parse_presentation_mode(std::string_view v, PresentationMode* out) {
    auto eq = [&](std::string_view w) {
        if (v.size() != w.size()) return false;
        for (std::size_t i = 0; i < v.size(); ++i) {
            char c = v[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != w[i]) return false;
        }
        return true;
    };
    if (eq("native")) { *out = PresentationMode::Native; return true; }
    if (eq("monitor") || eq("monitor-refresh") || eq("monitor_refresh")) {
        *out = PresentationMode::MonitorRefresh;
        return true;
    }
    return false;
}

// The GBA frame rate: 16'777'216 Hz / 280'896 cycles.
constexpr double kPresentGuestHz = 16777216.0 / 280896.0;

// Plausible monitor refresh range. SDL reports 0 when it does not know; some
// virtual displays report absurd values. Anything outside is "unknown" (0).
constexpr int kMinMonitorHz = 24;
constexpr int kMaxMonitorHz = 1000;
// Presentation never targets more than this, even on a faster panel: the extra
// presents would only burn CPU for the same pixels.
constexpr int kMaxPresentHz = 360;
// A monitor must beat the guest rate by this factor before repeats make sense.
// Near 1x (a 60 Hz panel against 59.73 Hz) one present per guest frame already
// tracks the refresh; slicing the wait would only add jitter.
constexpr double kRepeatMinRatio = 1.5;

inline int sanitize_monitor_hz(int hz) {
    return (hz >= kMinMonitorHz && hz <= kMaxMonitorHz) ? hz : 0;
}

// Effective repeat-present rate for a mode and a (possibly unknown) monitor
// refresh. 0 means "no repeats": Native, an unknown refresh (safe fallback) or a
// monitor too close to the guest rate.
inline int resolve_present_hz(PresentationMode mode, int monitor_hz) {
    if (mode != PresentationMode::MonitorRefresh) return 0;
    const int hz = sanitize_monitor_hz(monitor_hz);
    if (hz == 0) return 0;
    if (static_cast<double>(hz) < kPresentGuestHz * kRepeatMinRatio) return 0;
    return std::min(hz, kMaxPresentHz);
}

// Schedules repeated presents inside the wait for the next guest frame.
//
// The guest deadline is authoritative: a repeat is only issued when its
// predicted end (tick + measured cost of a present, including any blocking
// swap) lands before that deadline, so the guest frame is never delayed by a
// repeat. The guest-frame present counts as a present in the grid. The host is
// single-threaded: ticks that fall while the guest is computing a frame cannot
// be served, so the achieved rate is the monitor rate minus those (reported, not
// hidden, by the PRESENT meter). Ticks sit on a fixed grid (no drift); a present that happens close to
// a tick (the guest-frame present itself) swallows that tick; a stall longer
// than a few periods resynchronises instead of bursting to catch up.
class RepeatPresentScheduler {
public:
    enum class Action : std::uint8_t { Done, Sleep, Present };
    struct Step {
        Action action = Action::Done;
        std::int64_t wake_ns = 0;   // Sleep: wake at this time
    };

    // hz <= 0 disables the scheduler.
    void configure(double hz) {
        if (hz <= 0.0) { period_ns_ = 0; return; }
        period_ns_ = static_cast<std::int64_t>(1e9 / hz + 0.5);
        if (period_ns_ < 1) period_ns_ = 1;
    }
    bool enabled() const { return period_ns_ > 0; }
    std::int64_t period_ns() const { return period_ns_; }
    std::int64_t next_tick_ns() const { return next_tick_ns_; }
    std::int64_t cost_ns() const { return cost_ns_; }

    void reset(std::int64_t now_ns) {
        next_tick_ns_ = now_ns + period_ns_;
        cost_ns_ = 0;
    }

    // Decide what to do at `now_ns` with the guest frame due at
    // `guest_deadline_ns`.
    Step next(std::int64_t now_ns, std::int64_t guest_deadline_ns) const {
        Step s;
        if (!enabled()) return s;
        // Keep a small margin so a repeat that runs a little long still leaves
        // the guest deadline intact.
        const std::int64_t margin = std::max<std::int64_t>(200'000, cost_ns_ / 2);
        // Only the cost and a margin are reserved: a repeat may run until just
        // before the guest deadline. (The guest-frame present itself happens a
        // frame's compute time after the deadline; note_present() drops the ticks
        // that fall next to it.)
        const std::int64_t reserve = cost_ns_ + margin;
        if (next_tick_ns_ > guest_deadline_ns - reserve) return s;
        if (now_ns < next_tick_ns_) {
            s.action = Action::Sleep;
            s.wake_ns = next_tick_ns_;
        } else {
            s.action = Action::Present;
        }
        return s;
    }

    // A present just completed at `now_ns` (guest-frame present or repeat).
    void note_present(std::int64_t now_ns) {
        if (!enabled()) return;
        const std::int64_t half = period_ns_ / 2;
        if (now_ns - next_tick_ns_ > 4 * period_ns_) {      // long stall: resync
            next_tick_ns_ = now_ns + period_ns_;
            return;
        }
        // Measure from when the present started, not when it returned: a slow
        // present (blocking swap) must not also eat the next tick.
        const std::int64_t started = now_ns - cost_ns_;
        while (next_tick_ns_ < started + half) next_tick_ns_ += period_ns_;
    }

    // Wall time one repeat took (render + swap, including a blocking vsync).
    // Smoothed, but a slower sample is adopted at once so the guard reacts fast.
    void note_repeat_cost(std::int64_t cost_ns) {
        cost_ns = std::max<std::int64_t>(cost_ns, 0);
        cost_ns_ = cost_ns > cost_ns_ ? cost_ns : (cost_ns_ * 7 + cost_ns) / 8;
    }

private:
    std::int64_t period_ns_ = 0;
    std::int64_t next_tick_ns_ = 0;
    std::int64_t cost_ns_ = 0;
};

}  // namespace gbarecomp
