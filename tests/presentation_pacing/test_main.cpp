// Presentation pacing, controller navigation and display-settings logic.
// Pure arithmetic with a fake clock: no SDL, no window, no sleeps. Real timing
// evidence lives in the host-display harness and the performance report.

#include "display_settings.h"
#include "host_perf.h"
#include "menu_nav.h"
#include "presentation_pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace gbarecomp;

namespace {
int g_failures = 0;
void check(bool ok, const char* what, int line) {
    if (!ok) { std::fprintf(stderr, "FAIL line %d: %s\n", line, what); ++g_failures; }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr std::int64_t kGuestPeriodNs = 16'742'706;   // 1e9 / 59.7275

// Drive the scheduler exactly the way the runtime does: a guest-frame present at
// each guest deadline, then repeats until the scheduler says Done, then the
// guest waits for its deadline. The fake clock advances by sleeps and by the
// per-present cost.
struct Sim {
    std::vector<std::int64_t> present_times;   // all presents
    std::vector<std::int64_t> guest_present_times;
    std::vector<std::int64_t> guest_deadlines;
    std::int64_t guest_late_max = 0;
};

Sim simulate(int present_hz, std::int64_t cost_ns, std::int64_t duration_ns,
             std::int64_t stall_at_ns = -1, std::int64_t stall_ns = 0,
             std::int64_t compute_ns = 0) {
    RepeatPresentScheduler s;
    s.configure(present_hz);
    Sim out;
    std::int64_t now = 1'000'000'000;
    const std::int64_t t0 = now;
    std::int64_t deadline = now + kGuestPeriodNs;
    s.reset(now);
    bool stalled = false;
    while (now - t0 < duration_ns) {
        // guest-frame compute (single-threaded host: no presents meanwhile), present
        now += compute_ns + cost_ns;
        out.present_times.push_back(now);
        out.guest_present_times.push_back(now);
        out.guest_deadlines.push_back(deadline);
        if (stall_at_ns >= 0 && !stalled && now - t0 >= stall_at_ns) {
            now += stall_ns;
            stalled = true;
        }
        s.note_present(now);
        for (int guard = 0; guard < 100000; ++guard) {
            const auto st = s.next(now, deadline);
            if (st.action == RepeatPresentScheduler::Action::Done) break;
            if (st.action == RepeatPresentScheduler::Action::Sleep) {
                now = std::max(now, st.wake_ns);
                continue;
            }
            const std::int64_t a = now;
            now += cost_ns;
            s.note_repeat_cost(now - a);
            s.note_present(now);
            out.present_times.push_back(now);
        }
        // the guest pacer: wait for the deadline, resync when far behind
        if (now > deadline + kGuestPeriodNs) {
            deadline = now + kGuestPeriodNs;
        } else {
            out.guest_late_max = std::max(out.guest_late_max, now - deadline);
            now = std::max(now, deadline);
            deadline += kGuestPeriodNs;
        }
    }
    return out;
}

void test_mode_parse() {
    PresentationMode m = PresentationMode::MonitorRefresh;
    CHECK(DisplaySettings{}.presentation == PresentationMode::Native);
    CHECK(parse_presentation_mode("native", &m) && m == PresentationMode::Native);
    CHECK(parse_presentation_mode("Monitor", &m) && m == PresentationMode::MonitorRefresh);
    CHECK(parse_presentation_mode("MONITOR-REFRESH", &m) && m == PresentationMode::MonitorRefresh);
    m = PresentationMode::Native;
    CHECK(!parse_presentation_mode("144", &m) && m == PresentationMode::Native);
    CHECK(!parse_presentation_mode("", &m));
    CHECK(std::string(presentation_mode_name(PresentationMode::Native)) == "native");
    CHECK(std::string(presentation_mode_name(PresentationMode::MonitorRefresh)) == "monitor");
}

void test_refresh_selection() {
    const auto mon = PresentationMode::MonitorRefresh;
    const auto nat = PresentationMode::Native;
    for (int hz : {60, 75, 120, 144, 165, 240, 0, -5, 23, 5000})
        CHECK(resolve_present_hz(nat, hz) == 0);
    CHECK(resolve_present_hz(mon, 60) == 0);     // too close to 59.73: no repeats
    CHECK(resolve_present_hz(mon, 75) == 0);     // below 1.5x (89.6)
    CHECK(resolve_present_hz(mon, 90) == 90);
    CHECK(resolve_present_hz(mon, 120) == 120);
    CHECK(resolve_present_hz(mon, 144) == 144);
    CHECK(resolve_present_hz(mon, 165) == 165);
    CHECK(resolve_present_hz(mon, 240) == 240);
    CHECK(resolve_present_hz(mon, 500) == kMaxPresentHz);   // capped
    CHECK(resolve_present_hz(mon, 0) == 0);      // unknown: safe fallback
    CHECK(resolve_present_hz(mon, -1) == 0);
    CHECK(resolve_present_hz(mon, 12) == 0);
    CHECK(resolve_present_hz(mon, 100000) == 0); // absurd report = unknown
    CHECK(sanitize_monitor_hz(144) == 144 && sanitize_monitor_hz(0) == 0);
}

void test_scheduler_144() {
    // The host is single-threaded: while the guest computes a frame (here 4.7 ms,
    // MZM-like) no present can be served, so the achieved rate is below the monitor
    // rate by roughly compute/guest-period. The guest itself is never late.
    const std::int64_t compute = 4'700'000;
    const Sim r = simulate(144, 500'000, 20'000'000'000LL, -1, 0, compute);
    const double secs = 20.0;
    const double rate = static_cast<double>(r.present_times.size()) / secs;
    CHECK(rate > 144.0 * 0.75 && rate < 144.0 * 1.05);
    CHECK(r.guest_late_max <= 0);                  // repeats never delay the guest
    const double guest_rate = static_cast<double>(r.guest_present_times.size()) / secs;
    CHECK(std::fabs(guest_rate - 59.7275) < 0.3);  // guest clock unchanged
    std::int64_t mn = INT64_MAX, mx = 0;
    for (std::size_t i = 1; i < r.present_times.size(); ++i) {
        const std::int64_t d = r.present_times[i] - r.present_times[i - 1];
        mn = std::min(mn, d);
        mx = std::max(mx, d);
    }
    CHECK(mn >= 1'000'000);                        // never two presents back to back
    CHECK(mx <= compute + 2 * 6'944'444);          // the longest gap is the compute time + a tick or two
    // Compute-free frames (a lighter guest) approach the monitor rate.
    const Sim q = simulate(144, 500'000, 20'000'000'000LL, -1, 0, 0);
    const double qrate = static_cast<double>(q.present_times.size()) / secs;
    CHECK(qrate > 144.0 * 0.95 && qrate < 144.0 * 1.25);
    CHECK(q.guest_late_max <= 0);
    // More compute never raises the rate.
    const Sim heavy = simulate(144, 500'000, 20'000'000'000LL, -1, 0, 9'000'000);
    CHECK(heavy.present_times.size() < r.present_times.size());
    CHECK(heavy.guest_late_max <= 0);
}

void test_scheduler_rates() {
    for (int hz : {90, 120, 165, 240}) {
        const Sim r = simulate(hz, 500'000, 10'000'000'000LL, -1, 0, 2'000'000);
        const double rate = static_cast<double>(r.present_times.size()) / 10.0;
        CHECK(rate > hz * 0.8 && rate < hz * 1.25);
        CHECK(r.guest_late_max <= 0);
        CHECK(r.present_times.size() > r.guest_present_times.size() * 1.4);   // real repeats
    }
}

void test_scheduler_costly_present() {
    // A present that costs more than the tick leaves no room for repeats but must
    // never delay the guest.
    const Sim r = simulate(144, 9'000'000, 5'000'000'000LL);
    CHECK(r.guest_late_max <= 9'000'000);   // only the guest-frame present's own cost
    // Repeats shrink to what fits before the deadline: far fewer than 144/s.
    const double rate = static_cast<double>(r.present_times.size()) / 5.0;
    CHECK(rate < 100.0);
}

void test_scheduler_stall_resync() {
    // A 300 ms host stall: after it the schedule resyncs instead of replaying 40
    // missed ticks as a burst.
    const Sim r = simulate(144, 500'000, 3'000'000'000LL, 1'000'000'000LL, 300'000'000LL, 2'000'000);
    std::int64_t mn = INT64_MAX;
    for (std::size_t i = 1; i < r.present_times.size(); ++i)
        mn = std::min(mn, r.present_times[i] - r.present_times[i - 1]);
    CHECK(mn >= 400'000);
}

void test_scheduler_disabled() {
    RepeatPresentScheduler s;
    CHECK(!s.enabled());
    CHECK(s.next(0, 1'000'000'000).action == RepeatPresentScheduler::Action::Done);
    s.configure(0.0);
    CHECK(!s.enabled());
    s.configure(144.0);
    CHECK(s.enabled() && s.period_ns() == 6'944'444);
    s.configure(-1.0);
    CHECK(!s.enabled());
}

void test_window_mode_toggle() {
    CHECK(toggle_window_mode(WindowMode::Windowed) == WindowMode::Borderless);
    CHECK(toggle_window_mode(WindowMode::Borderless) == WindowMode::Windowed);
    CHECK(toggle_window_mode(WindowMode::Exclusive) == WindowMode::Windowed);
    WindowMode m = WindowMode::Windowed;
    for (int i = 0; i < 6; ++i) m = toggle_window_mode(m);   // even: back where it began
    CHECK(m == WindowMode::Windowed);
}

void test_nav_repeater() {
    NavRepeater r(400, 120);
    CHECK(r.update(NavDir::None, 0).dir == NavDir::None);
    auto f = r.update(NavDir::Down, 1000);           // initial press fires at once
    CHECK(f.dir == NavDir::Down && !f.repeat);
    CHECK(r.update(NavDir::Down, 1100).dir == NavDir::None);   // within the delay
    CHECK(r.update(NavDir::Down, 1399).dir == NavDir::None);
    f = r.update(NavDir::Down, 1400);                // first repeat after the delay
    CHECK(f.dir == NavDir::Down && f.repeat);
    CHECK(r.update(NavDir::Down, 1450).dir == NavDir::None);
    CHECK(r.update(NavDir::Down, 1519).dir == NavDir::None);
    CHECK(r.update(NavDir::Down, 1520).dir == NavDir::Down);   // then every 120 ms
    // A long gap fires once, not a catch-up burst.
    f = r.update(NavDir::Down, 5000);
    CHECK(f.dir == NavDir::Down);
    CHECK(r.update(NavDir::Down, 5001).dir == NavDir::None);
    // Release and press again: immediate again.
    CHECK(r.update(NavDir::None, 5002).dir == NavDir::None);
    CHECK(r.update(NavDir::Up, 5003).dir == NavDir::Up);
    // Change direction restarts the sequence.
    CHECK(r.update(NavDir::Left, 5100).dir == NavDir::Left);
    CHECK(!r.update(NavDir::Left, 5101).repeat && r.update(NavDir::Left, 5101).dir == NavDir::None);
    // 20 polls of a held stick within 400 ms = 1 step, not 20 rows.
    NavRepeater h(400, 120);
    int steps = 0;
    for (std::uint32_t t = 0; t < 390; t += 16)
        if (h.update(NavDir::Down, t).dir != NavDir::None) ++steps;
    CHECK(steps == 1);
}

void test_nav_neutral_gate() {
    NavRepeater r(400, 120);
    r.require_neutral();                              // held across menu open
    CHECK(r.update(NavDir::Down, 0).dir == NavDir::None);
    CHECK(r.update(NavDir::Down, 1000).dir == NavDir::None);   // still held: nothing
    CHECK(r.update(NavDir::None, 1001).dir == NavDir::None);
    CHECK(r.update(NavDir::Down, 1002).dir == NavDir::Down);
    r.require_neutral();
    r.reset();
    CHECK(r.update(NavDir::Up, 2000).dir == NavDir::Up);
}

void test_stick() {
    CHECK(stick_direction(0, 0, NavDir::None) == NavDir::None);
    CHECK(stick_direction(8000, 0, NavDir::None) == NavDir::None);     // under press threshold
    CHECK(stick_direction(20000, 0, NavDir::None) == NavDir::Right);
    CHECK(stick_direction(-20000, 3000, NavDir::None) == NavDir::Left);
    CHECK(stick_direction(0, -20000, NavDir::None) == NavDir::Up);
    CHECK(stick_direction(0, 32767, NavDir::None) == NavDir::Down);    // SDL Y is down-positive
    CHECK(stick_direction(20000, 25000, NavDir::None) == NavDir::Down); // dominant axis
    // Hysteresis: once Right, 12000 still holds it; from neutral 12000 does not press.
    CHECK(stick_direction(12000, 0, NavDir::Right) == NavDir::Right);
    CHECK(stick_direction(12000, 0, NavDir::None) == NavDir::None);
    CHECK(stick_direction(8000, 0, NavDir::Right) == NavDir::None);
}

void test_input_latch() {
    InputLatch l;
    const std::uint16_t all_up = 0x03FF;
    CHECK(l.apply(all_up) == all_up && !l.active());
    // B (bit 1) still held when the menu closes.
    const std::uint16_t b_held = static_cast<std::uint16_t>(all_up & ~(1u << 1));
    l.begin(b_held);
    CHECK(l.active());
    CHECK(l.apply(b_held) == all_up);                 // masked while held
    CHECK(l.apply(b_held) == all_up);
    CHECK(l.apply(all_up) == all_up && !l.active());  // released: unmasked
    CHECK(l.apply(b_held) == b_held);                 // pressing again now reaches the guest
    // A button pressed AFTER close is never masked.
    l.begin(b_held);
    const std::uint16_t a_and_b = static_cast<std::uint16_t>(b_held & ~1u);
    CHECK(l.apply(a_and_b) == static_cast<std::uint16_t>(all_up & ~1u));   // A passes, B masked
    // Multiple held buttons release independently.
    l.begin(a_and_b);
    CHECK(l.apply(b_held) == all_up);                 // A released, B still masked
    CHECK(l.apply(all_up) == all_up && !l.active());
    l.begin(b_held);
    l.clear();
    CHECK(l.apply(b_held) == b_held);
}

void test_crt_presets() {
    CHECK(crt_preset_params(CrtPreset::Lite, 99).strength == 40);   // ENHANCEMENTS-1 look
    CHECK(crt_preset_params(CrtPreset::Lite, 99).shape == 1.0);
    const CrtParams soft = crt_preset_params(CrtPreset::Soft, 99);
    const CrtParams strong = crt_preset_params(CrtPreset::Strong, 99);
    CHECK(soft.strength < 40 && strong.strength > 40);
    CHECK(soft.shape < 1.0 && strong.shape > 1.0);
    CHECK(crt_preset_params(CrtPreset::Custom, 63).strength == 63);
    // Reasonable bounds: never exceeds the slider, never approaches unreadable.
    CHECK(strong.strength <= 100 && scanline_peak_alpha(strong.strength) <= 0.5);
    // Every preset's darkest row stays readable and the profile never leaves 0..peak.
    for (CrtPreset p : {CrtPreset::Lite, CrtPreset::Soft, CrtPreset::Strong}) {
        const CrtParams c = crt_preset_params(p, 0);
        const double peak = scanline_peak_alpha(c.strength);
        double mx = 0.0, mn = 1.0;
        for (int y = 0; y < 480; ++y) {
            const double a = scanline_row_alpha(y, 480, 160, peak, c.shape);
            mx = std::max(mx, a);
            mn = std::min(mn, a);
        }
        CHECK(mx <= peak + 1e-9 && mn >= 0.0);
        CHECK(mx > 0.0);
    }
    // Strength 0 is visually off at any shape.
    for (double shape : {0.75, 1.0, 1.5})
        for (int y = 0; y < 480; ++y)
            CHECK(scanline_row_alpha(y, 480, 160, scanline_peak_alpha(0), shape) == 0.0);
    // shape 1.0 default argument is the original cosine.
    CHECK(scanline_row_alpha(7, 480, 160, 0.3) == scanline_row_alpha(7, 480, 160, 0.3, 1.0));
    // A sharper shape narrows the dark band (darker peak share, lighter mean).
    double mean1 = 0, mean2 = 0;
    for (int y = 0; y < 480; ++y) {
        mean1 += scanline_row_alpha(y, 480, 160, 0.4, 1.0);
        mean2 += scanline_row_alpha(y, 480, 160, 0.4, 1.5);
    }
    CHECK(mean2 < mean1);
}

void test_settings_persistence() {
    DisplaySettings s;
    CHECK(s.presentation == PresentationMode::Native);
    CHECK(s.crt_preset == CrtPreset::Lite && !s.crt_enabled && s.scanline_strength == 40);
    s.presentation = PresentationMode::MonitorRefresh;
    s.crt_enabled = true;
    s.crt_preset = CrtPreset::Strong;
    s.scanline_strength = 70;
    const std::string ini = display_settings_to_ini(s);
    CHECK(ini.find("presentation = monitor") != std::string::npos);
    CHECK(ini.find("crt_preset = strong") != std::string::npos);
    DisplaySettings back;
    CHECK(display_settings_parse_ini(&back, ini));
    CHECK(back == s);
    // Round trip for every preset and mode.
    for (CrtPreset p : {CrtPreset::Lite, CrtPreset::Soft, CrtPreset::Strong, CrtPreset::Custom}) {
        DisplaySettings a;
        a.crt_enabled = true;
        a.crt_preset = p;
        a.scanline_strength = crt_preset_params(p, 55).strength;
        DisplaySettings b;
        CHECK(display_settings_parse_ini(&b, display_settings_to_ini(a)) && b == a);
    }
}

void test_settings_migration() {
    // An ENHANCEMENTS-1 file: no `presentation` key, crt_preset = lite.
    const char* old_cfg =
        "[Display]\nwindow_mode = windowed\nwindow_scale = 4\nvsync = 0\n"
        "crt_enabled = 1\ncrt_preset = lite\nscanline_strength = 40\ncolor_profile = gba-like\n";
    DisplaySettings s;
    CHECK(display_settings_parse_ini(&s, old_cfg));
    CHECK(s.presentation == PresentationMode::Native);       // missing -> default
    CHECK(s.crt_enabled && s.crt_preset == CrtPreset::Lite && s.scanline_strength == 40);
    CHECK(s.window_scale == 4 && s.color_profile == ColorProfile::GbaLike);
    // An older file with a hand-tuned strength under "lite" keeps its look as Custom.
    DisplaySettings t;
    CHECK(display_settings_parse_ini(&t,
        "[Display]\ncrt_enabled = 1\ncrt_preset = lite\nscanline_strength = 65\n"));
    CHECK(t.scanline_strength == 65 && t.crt_preset == CrtPreset::Custom);
    // A preset without a strength means that preset's look.
    DisplaySettings u;
    CHECK(display_settings_parse_ini(&u, "[Display]\ncrt_enabled = 1\ncrt_preset = strong\n"));
    CHECK(u.crt_preset == CrtPreset::Strong && u.scanline_strength == 70);
    // Invalid values fall back per key; unknown keys are ignored.
    DisplaySettings v;
    int rejected = 0;
    display_settings_parse_ini(&v,
        "[Display]\npresentation = 240hz\ncrt_preset = ultra\nfuture_key = 1\nvsync = 1\n",
        &rejected);
    CHECK(v.presentation == PresentationMode::Native && v.crt_preset == CrtPreset::Lite);
    CHECK(v.vsync && rejected == 3);
    // Empty / missing section: defaults.
    DisplaySettings w;
    CHECK(!display_settings_parse_ini(&w, "[Other]\nx = 1\n") && w == DisplaySettings{});
}

void test_interval_jitter() {
    IntervalMeter m(100);
    std::uint64_t t = 1'000'000;
    // Perfectly regular 6.944 ms ticks: zero jitter.
    for (int i = 0; i < 200; ++i) { m.tick(t); t += 6944; }
    CHECK(std::fabs(m.avg_ms() - 6.944) < 0.01 && m.jitter_ms() < 0.01);
    // Alternate 5 and 9 ms: mean 7, jitter 2.
    IntervalMeter j(200);
    t = 1'000'000;
    for (int i = 0; i < 400; ++i) { j.tick(t); t += (i & 1) ? 9000 : 5000; }
    CHECK(std::fabs(j.avg_ms() - 7.0) < 0.3 && std::fabs(j.jitter_ms() - 2.0) < 0.3);
}

void test_rate_meters_separate() {
    // Repeated presents raise PRESENT without moving EMU.
    RateMeter emu(500), present(500);
    std::uint64_t frames = 0, presents = 0;
    for (std::uint32_t ms = 0; ms <= 2000; ms += 7) {      // ~143 Hz presents
        ++presents;
        frames = static_cast<std::uint64_t>(ms * 59.7275 / 1000.0);
        emu.sample(frames, ms);
        present.sample(presents, ms);
    }
    CHECK(emu.rate() > 58.0 && emu.rate() < 61.0);
    CHECK(present.rate() > 135.0 && present.rate() < 150.0);
}

}  // namespace

int main() {
    test_mode_parse();
    test_refresh_selection();
    test_scheduler_144();
    test_scheduler_rates();
    test_scheduler_costly_present();
    test_scheduler_stall_resync();
    test_scheduler_disabled();
    test_window_mode_toggle();
    test_nav_repeater();
    test_nav_neutral_gate();
    test_stick();
    test_input_latch();
    test_crt_presets();
    test_settings_persistence();
    test_settings_migration();
    test_interval_jitter();
    test_rate_meters_separate();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("presentation_pacing_tests: all passed");
    return 0;
}
