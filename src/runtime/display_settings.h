// display_settings.h — host PRESENTATION settings model (no SDL, no guest state).
//
// Everything in this header describes how the already-rendered 240x160 frame is
// shown on the host: window mode, destination rectangle, filtering, sync and
// post-processing. None of it is read by the emulation. The guest still runs at
// the GBA's 59.7275 Hz regardless of what is selected here; "vsync" or a 144 Hz
// monitor changes how often the host swaps buffers, never how often the guest
// advances.
//
// Header-only and dependency-free so the defaults, parsing, serialisation and
// layout maths are unit-testable without a window.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "presentation_layout.h"

namespace gbarecomp {

enum class WindowMode : std::uint8_t { Windowed = 0, Borderless = 1, Exclusive = 2 };
// Fit: largest destination at the logical aspect. IntegerFit: largest whole
// multiple of the logical size (never cropped; falls back to Fit when the
// drawable is smaller than 1x).
enum class ScaleMode : std::uint8_t { Fit = 0, IntegerFit = 1 };
// Native: preserve the logical aspect (letter/pillarbox). Stretch: fill the
// drawable, distorting the image. Native is the default and Stretch is opt-in.
enum class AspectMode : std::uint8_t { Native = 0, Stretch = 1 };
enum class FilterMode : std::uint8_t { Nearest = 0, Linear = 1 };
// Only Lite exists. The enum is the extension point for future presets.
enum class CrtPreset : std::uint8_t { Lite = 0 };
// Present-time colour transform. GbaLike applies the community-canonical
// gamma-4.0 GBA colour model (color_lut.h, ScreenKind::Classic). It is an
// approximation of the handheld's look, NOT an exact emulation of any specific
// LCD revision.
enum class ColorProfile : std::uint8_t { Off = 0, GbaLike = 1 };

constexpr int kMinWindowScale = 1;
constexpr int kMaxWindowScale = 6;
constexpr int kDefaultWindowScale = 3;   // matches the runtime's historical --scale default
constexpr int kMinScanlineStrength = 0;
constexpr int kMaxScanlineStrength = 100;
constexpr int kDefaultScanlineStrength = 40;
// A 100% slider is capped at this peak darkening so no setting can make the
// image unreadable (the darkest scanline row is never darker than 50%).
constexpr double kScanlinePeakAlpha = 0.5;

struct DisplaySettings {
    WindowMode window_mode = WindowMode::Windowed;
    int window_scale = kDefaultWindowScale;
    ScaleMode scale_mode = ScaleMode::Fit;
    AspectMode aspect = AspectMode::Native;
    FilterMode filtering = FilterMode::Nearest;
    // Default OFF: FramePacer is the sole emulation clock. A blocking monitor
    // vsync in series with it measurably cost frame budget on high-refresh
    // displays (see host_window.cpp), so synchronized swap is opt-in.
    bool vsync = false;
    bool show_fps = false;

    bool crt_enabled = false;
    CrtPreset crt_preset = CrtPreset::Lite;
    int scanline_strength = kDefaultScanlineStrength;   // 0..100 (%)
    ColorProfile color_profile = ColorProfile::Off;

    bool operator==(const DisplaySettings& o) const {
        return window_mode == o.window_mode && window_scale == o.window_scale &&
               scale_mode == o.scale_mode && aspect == o.aspect &&
               filtering == o.filtering && vsync == o.vsync &&
               show_fps == o.show_fps && crt_enabled == o.crt_enabled &&
               crt_preset == o.crt_preset &&
               scanline_strength == o.scanline_strength &&
               color_profile == o.color_profile;
    }
    bool operator!=(const DisplaySettings& o) const { return !(*this == o); }

    // True when the destination rectangle needs the explicit-layout path
    // instead of SDL's built-in logical-size letterbox. The default settings
    // stay on the historical path byte-for-byte.
    bool needs_explicit_layout() const {
        return aspect == AspectMode::Stretch ||
               scale_mode == ScaleMode::IntegerFit || crt_enabled;
    }
};

// Clamp every field into its valid range. Used after any parse so an invalid
// value can never reach the renderer.
inline void sanitize_display_settings(DisplaySettings* s) {
    if (!s) return;
    s->window_scale = std::clamp(s->window_scale, kMinWindowScale, kMaxWindowScale);
    s->scanline_strength = std::clamp(s->scanline_strength, kMinScanlineStrength,
                                      kMaxScanlineStrength);
}

// Peak scanline darkening (0..kScanlinePeakAlpha) for a 0..100 slider value.
inline double scanline_peak_alpha(int strength_percent) {
    const int s = std::clamp(strength_percent, kMinScanlineStrength,
                             kMaxScanlineStrength);
    return kScanlinePeakAlpha * static_cast<double>(s) / 100.0;
}

// Darkening (0..peak) of destination row `dest_row` when `logical_rows` guest
// lines are spread across `dest_height` host rows. Each guest line carries one
// cosine bump whose peak sits at 75% of the line and whose trough at 25%, so
// even a 2x scale alternates a dark and a light host row per guest line; the
// pattern follows the guest raster at any scale, including non-integer fits.
// Below 2 host rows per guest line there is no room for a pattern: returns 0.
inline double scanline_row_alpha(int dest_row, int dest_height, int logical_rows,
                                 double peak_alpha) {
    if (dest_height <= 0 || logical_rows <= 0 || peak_alpha <= 0.0) return 0.0;
    if (dest_height < 2 * logical_rows) return 0.0;
    const double t = std::fmod((dest_row + 0.5) * logical_rows / dest_height, 1.0);
    constexpr double kTwoPi = 6.283185307179586;
    return peak_alpha * 0.5 * (1.0 + std::cos(kTwoPi * (t - 0.75)));
}

// Destination rectangle for the logical frame inside the drawable, honouring
// aspect/scale mode. Native+Fit reproduces SDL_RenderSetLogicalSize's letterbox
// exactly (same float maths), so using this function never moves the default
// image by a pixel.
inline PresentationLayout compute_display_layout(int drawable_w, int drawable_h,
                                                 int logical_w, int logical_h,
                                                 const DisplaySettings& s) {
    if (drawable_w <= 0 || drawable_h <= 0 || logical_w <= 0 || logical_h <= 0)
        return {};
    if (s.aspect == AspectMode::Stretch) return {0, 0, drawable_w, drawable_h, 0};
    if (s.scale_mode == ScaleMode::IntegerFit) {
        const int k = std::min(drawable_w / logical_w, drawable_h / logical_h);
        if (k >= 1) {
            const int w = logical_w * k;
            const int h = logical_h * k;
            return {(drawable_w - w) / 2, (drawable_h - h) / 2, w, h, k};
        }
        // Window smaller than 1x: no whole multiple fits. Never crop; fit.
    }
    const float want = static_cast<float>(logical_w) / static_cast<float>(logical_h);
    const float real = static_cast<float>(drawable_w) / static_cast<float>(drawable_h);
    if (std::fabs(want - real) < 0.0001f) return {0, 0, drawable_w, drawable_h, 0};
    if (want > real) {
        const float scale = static_cast<float>(drawable_w) / static_cast<float>(logical_w);
        const int h = static_cast<int>(std::floor(static_cast<float>(logical_h) * scale));
        return {0, (drawable_h - h) / 2, drawable_w, h, 0};
    }
    const float scale = static_cast<float>(drawable_h) / static_cast<float>(logical_h);
    const int w = static_cast<int>(std::floor(static_cast<float>(logical_w) * scale));
    return {(drawable_w - w) / 2, 0, w, drawable_h, 0};
}

// ---- INI representation ([Display] section of config.ini) -------------------

namespace display_ini_detail {
inline bool parse_bool(std::string_view v, bool* out) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") { *out = true; return true; }
    if (v == "0" || v == "false" || v == "off" || v == "no") { *out = false; return true; }
    return false;
}
inline bool parse_int(std::string_view v, int* out) {
    if (v.empty() || v.size() > 9) return false;
    std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
    if (i == v.size()) return false;
    for (std::size_t j = i; j < v.size(); ++j)
        if (v[j] < '0' || v[j] > '9') return false;
    *out = std::atoi(std::string(v).c_str());
    return true;
}
inline std::string lower(std::string_view v) {
    std::string s(v);
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
}  // namespace display_ini_detail

// Apply one `key = value` pair. Returns true when the key is recognised AND the
// value valid; an invalid value leaves the field untouched (safe fallback), an
// unknown key is ignored so older/newer files stay compatible.
inline bool display_settings_set(DisplaySettings* s, std::string_view key,
                                 std::string_view raw_value) {
    using namespace display_ini_detail;
    if (!s) return false;
    const std::string v = lower(raw_value);
    bool b = false;
    int n = 0;
    if (key == "window_mode") {
        if (v == "windowed") s->window_mode = WindowMode::Windowed;
        else if (v == "borderless") s->window_mode = WindowMode::Borderless;
        else if (v == "exclusive") s->window_mode = WindowMode::Exclusive;
        else return false;
    } else if (key == "window_scale") {
        if (!parse_int(v, &n) || n < kMinWindowScale || n > kMaxWindowScale) return false;
        s->window_scale = n;
    } else if (key == "scale_mode") {
        if (v == "fit") s->scale_mode = ScaleMode::Fit;
        else if (v == "integer") s->scale_mode = ScaleMode::IntegerFit;
        else return false;
    } else if (key == "aspect") {
        if (v == "native") s->aspect = AspectMode::Native;
        else if (v == "stretch") s->aspect = AspectMode::Stretch;
        else return false;
    } else if (key == "filtering") {
        if (v == "nearest") s->filtering = FilterMode::Nearest;
        else if (v == "linear") s->filtering = FilterMode::Linear;
        else return false;
    } else if (key == "vsync") {
        if (!parse_bool(v, &b)) return false;
        s->vsync = b;
    } else if (key == "show_fps") {
        if (!parse_bool(v, &b)) return false;
        s->show_fps = b;
    } else if (key == "crt_enabled") {
        if (!parse_bool(v, &b)) return false;
        s->crt_enabled = b;
    } else if (key == "crt_preset") {
        if (v == "lite") s->crt_preset = CrtPreset::Lite;
        else return false;
    } else if (key == "scanline_strength") {
        if (!parse_int(v, &n) || n < kMinScanlineStrength || n > kMaxScanlineStrength)
            return false;
        s->scanline_strength = n;
    } else if (key == "color_profile") {
        if (v == "off") s->color_profile = ColorProfile::Off;
        else if (v == "gba-like") s->color_profile = ColorProfile::GbaLike;
        else return false;
    } else {
        return false;
    }
    return true;
}

inline const char* window_mode_name(WindowMode m) {
    return m == WindowMode::Borderless ? "borderless"
         : m == WindowMode::Exclusive  ? "exclusive" : "windowed";
}

// Serialise the whole [Display] section (header line included).
inline std::string display_settings_to_ini(const DisplaySettings& s) {
    std::string o = "[Display]\n";
    o += std::string("window_mode = ") + window_mode_name(s.window_mode) + "\n";
    o += "window_scale = " + std::to_string(s.window_scale) + "\n";
    o += std::string("scale_mode = ") +
         (s.scale_mode == ScaleMode::IntegerFit ? "integer" : "fit") + "\n";
    o += std::string("aspect = ") +
         (s.aspect == AspectMode::Stretch ? "stretch" : "native") + "\n";
    o += std::string("filtering = ") +
         (s.filtering == FilterMode::Linear ? "linear" : "nearest") + "\n";
    o += std::string("vsync = ") + (s.vsync ? "1" : "0") + "\n";
    o += std::string("show_fps = ") + (s.show_fps ? "1" : "0") + "\n";
    o += std::string("crt_enabled = ") + (s.crt_enabled ? "1" : "0") + "\n";
    o += "crt_preset = lite\n";
    o += "scanline_strength = " + std::to_string(s.scanline_strength) + "\n";
    o += std::string("color_profile = ") +
         (s.color_profile == ColorProfile::GbaLike ? "gba-like" : "off") + "\n";
    return o;
}

// Parse the [Display] section of a whole config.ini text into `s`. Keys that are
// missing keep their current value; invalid values are ignored one by one.
// Returns true when at least one valid key was applied. `*rejected` (optional)
// receives the number of recognised-but-invalid or unknown lines.
inline bool display_settings_parse_ini(DisplaySettings* s, std::string_view text,
                                       int* rejected = nullptr) {
    if (!s) return false;
    bool any = false, in_section = false;
    int bad = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        auto trim = [](std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
            return v;
        };
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            in_section = close != std::string_view::npos &&
                         display_ini_detail::lower(line.substr(1, close - 1)) == "display";
            continue;
        }
        if (!in_section) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) { ++bad; continue; }
        std::string_view value = line.substr(eq + 1);
        const std::size_t hash = value.find('#');
        if (hash != std::string_view::npos) value = value.substr(0, hash);
        if (display_settings_set(s, trim(line.substr(0, eq)), trim(value))) any = true;
        else ++bad;
    }
    if (rejected) *rejected = bad;
    return any;
}

// Replace (or append) one INI section in `existing`, preserving every other
// section byte-for-byte. `body` must include the "[Section]" header line.
inline std::string ini_replace_section(std::string_view existing,
                                       std::string_view section,
                                       std::string_view body) {
    const std::string header = "[" + display_ini_detail::lower(section) + "]";
    std::string kept;
    bool in_section = false;
    std::size_t pos = 0;
    while (pos < existing.size()) {
        std::size_t end = existing.find('\n', pos);
        const bool has_nl = end != std::string_view::npos;
        if (!has_nl) end = existing.size();
        std::string_view line = existing.substr(pos, end - pos);
        pos = end + (has_nl ? 1 : 0);
        std::string_view t = line;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
        if (!t.empty() && t.front() == '[') in_section = display_ini_detail::lower(t) == header;
        if (in_section) continue;
        kept.append(line);
        kept.push_back('\n');
    }
    while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r')) kept.pop_back();
    std::string out = kept;
    if (!out.empty()) out += "\n\n";
    out.append(body);
    return out;
}

}  // namespace gbarecomp
