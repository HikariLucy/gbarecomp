// menu_nav.h — controller navigation helpers for the host menu (no SDL).
//
// Semantic actions only: the host maps whatever the platform reports (SDL's
// standard controller vocabulary: a south-face button is "accept" on an Xbox pad
// and on a PlayStation pad alike) onto Navigate / Accept / Back / Resume. The
// guest never sees input that the menu owns:
//   * while the menu is open the guest's keyinput is forced to "all released";
//   * InputLatch keeps any button that was still held when the menu closed
//     masked until it is released, so closing with B never presses B in-game.

#pragma once

#include <cstdint>
#include <cstdlib>

namespace gbarecomp {

enum class NavDir : std::uint8_t { None = 0, Up, Down, Left, Right };

// Stick -> direction with hysteresis. `prev` is the direction reported last
// time: once a direction is active it only drops below `release`, so a stick
// hovering at the threshold does not chatter. Dominant axis wins; SDL Y is
// positive-down.
constexpr int kStickPress = 16000;     // ~49% of full deflection
constexpr int kStickRelease = 9000;    // ~27%

inline NavDir stick_direction(int x, int y, NavDir prev) {
    const int ax = std::abs(x), ay = std::abs(y);
    const int thr = prev == NavDir::None ? kStickPress : kStickRelease;
    if (ax < thr && ay < thr) return NavDir::None;
    if (ax >= ay) return x < 0 ? NavDir::Left : NavDir::Right;
    return y < 0 ? NavDir::Up : NavDir::Down;
}

// Edge + auto-repeat: the first press fires at once, then after `delay_ms`,
// then every `rate_ms`. Changing direction restarts the sequence. After
// `require_neutral()` nothing fires until the input has been released once, so a
// direction already held when the menu opens does not scroll it.
class NavRepeater {
public:
    NavRepeater(std::uint32_t delay_ms = 400, std::uint32_t rate_ms = 120)
        : delay_ms_(delay_ms), rate_ms_(rate_ms) {}

    struct Fire {
        NavDir dir = NavDir::None;
        bool repeat = false;   // true for auto-repeats, false for the initial press
    };

    Fire update(NavDir held, std::uint32_t now_ms) {
        Fire f;
        if (held == NavDir::None) {
            neutral_needed_ = false;
            cur_ = NavDir::None;
            return f;
        }
        if (neutral_needed_) return f;
        if (held != cur_) {
            cur_ = held;
            next_ms_ = now_ms + delay_ms_;
            f.dir = held;
            return f;
        }
        if (static_cast<std::int32_t>(now_ms - next_ms_) >= 0) {
            // One step per call even after a long gap: no catch-up burst.
            next_ms_ = now_ms + rate_ms_;
            f.dir = held;
            f.repeat = true;
        }
        return f;
    }

    void require_neutral() { neutral_needed_ = true; cur_ = NavDir::None; }
    void reset() { neutral_needed_ = false; cur_ = NavDir::None; }

private:
    std::uint32_t delay_ms_, rate_ms_;
    NavDir cur_ = NavDir::None;
    std::uint32_t next_ms_ = 0;
    bool neutral_needed_ = false;
};

// Masks buttons that were held across a menu close. Operates on GBA KEYINPUT
// (active-low: bit set = released).
class InputLatch {
public:
    // Call when the menu closes with the keyinput the player is holding right
    // then (active-low).
    void begin(std::uint16_t keyinput_active_low) {
        masked_ = static_cast<std::uint16_t>(~keyinput_active_low & 0x03FFu);
    }
    // Filter the live keyinput: a masked button reads released until it has been
    // seen released once.
    std::uint16_t apply(std::uint16_t keyinput_active_low) {
        const std::uint16_t held = static_cast<std::uint16_t>(~keyinput_active_low & 0x03FFu);
        masked_ &= held;                                   // released -> unmask
        return static_cast<std::uint16_t>((keyinput_active_low | masked_) & 0x03FFu);
    }
    bool active() const { return masked_ != 0; }
    void clear() { masked_ = 0; }

private:
    std::uint16_t masked_ = 0;
};

}  // namespace gbarecomp
