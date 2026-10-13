#pragma once

#include "edc_theme.hpp"

#include <cmath>
#include <ftxui/screen/color.hpp>
#include <memory>
#include <string>

namespace edc
{
// Single source of truth for the TUI look: every widget draws from
// style::palette(). Two palettes (dark/light); palette() picks one from the
// terminal background edc_theme resolved at startup.
namespace style
{

// Semantic colour roles, not raw values.
struct Palette
{
    ::ftxui::Color window_bg;
    ::ftxui::Color pane_bg;
    ::ftxui::Color text_dim;
    // Always explicit on both palettes: inheriting the terminal's default
    // foreground would pair a light terminal's black with the dark palette's
    // near-black pane.
    ::ftxui::Color text;
    ::ftxui::Color line_dim;
    ::ftxui::Color dialog_bg;

    ::ftxui::Color accent;         // cyan: headers, in_progress, spinner
    ::ftxui::Color ok;             // green: success/COMPLETED, nothing else
    ::ftxui::Color warn;           // yellow: foreground group, IDLE/WAITING
    ::ftxui::Color error;          // ERROR
    ::ftxui::Color suspended;      // INTERRUPTED / SUSPENDED (both are parked)
    ::ftxui::Color thought_open;   // expanded "Thought" toggle
    ::ftxui::Color thought_closed; // collapsed "Thought" toggle

    ::ftxui::Color prompt;    // the "❯ " input prompt glyph
    ::ftxui::Color user_rule; // heavy rule marking user messages

    // Markdown syntax mapping (Monokai). No role uses green -- that is Palette::ok.
    ::ftxui::Color md_h1;
    ::ftxui::Color md_h1_bg;
    ::ftxui::Color md_h2;
    ::ftxui::Color md_h3;
    ::ftxui::Color md_h4;
    ::ftxui::Color md_code;
    ::ftxui::Color md_code_inline;
    ::ftxui::Color md_code_bg;
    ::ftxui::Color md_rule;
    ::ftxui::Color md_quote;
    ::ftxui::Color md_list;
    ::ftxui::Color md_link;
};

// Function-local statics on purpose: ftxui::Color::RGB() resolves against the
// current terminal (Terminal::ColorSupport()), which is not ready during C++
// static initialization -- a file-scope object would run before main() and can
// crash.

// The original Monokai, for dark terminals.
inline const Palette &dark_palette()
{
    static const Palette k = {
        ::ftxui::Color::RGB(30, 31, 28),    // window_bg
        ::ftxui::Color::RGB(39, 40, 34),    // pane_bg
        ::ftxui::Color::RGB(117, 113, 94),  // text_dim
        ::ftxui::Color::RGB(248, 248, 242), // text
        ::ftxui::Color::RGB(73, 72, 62),    // line_dim
        ::ftxui::Color::RGB(62, 61, 50),    // dialog_bg
        ::ftxui::Color::RGB(102, 217, 239), // accent
        ::ftxui::Color::RGB(166, 226, 46),  // ok
        ::ftxui::Color::RGB(230, 219, 116), // warn
        ::ftxui::Color::RGB(249, 38, 114),  // error
        ::ftxui::Color::RGB(174, 129, 255), // suspended
        ::ftxui::Color::RGB(230, 219, 116), // thought_open
        ::ftxui::Color::RGB(138, 131, 69),  // thought_closed
        ::ftxui::Color::RGB(102, 217, 239), // prompt
        ::ftxui::Color::RGB(102, 217, 239), // user_rule
        ::ftxui::Color::RGB(39, 40, 34),    // md_h1
        ::ftxui::Color::RGB(249, 38, 114),  // md_h1_bg
        ::ftxui::Color::RGB(230, 219, 116), // md_h2
        ::ftxui::Color::RGB(253, 151, 31),  // md_h3
        ::ftxui::Color::RGB(102, 217, 239), // md_h4
        ::ftxui::Color::RGB(248, 248, 242), // md_code
        ::ftxui::Color::RGB(174, 129, 255), // md_code_inline
        ::ftxui::Color::RGB(62, 61, 50),    // md_code_bg
        ::ftxui::Color::RGB(73, 72, 62),    // md_rule
        ::ftxui::Color::RGB(117, 113, 94),  // md_quote
        ::ftxui::Color::RGB(249, 38, 114),  // md_list
        ::ftxui::Color::RGB(102, 217, 239), // md_link
    };
    return k;
}

// Monokai Light, used verbatim: https://github.com/rjfranco/monokai-light
// Its syntax colours sit at 2.0-3.8:1 on white (below WCAG AA) -- that is the
// look, not an oversight. The Thought toggle keeps the dark palette's yellow
// hue at an intensity that survives a white surface.
inline const Palette &light_palette()
{
    static const Palette k = {
        ::ftxui::Color::RGB(237, 237, 237), // window_bg
        ::ftxui::Color::RGB(255, 255, 255), // pane_bg
        ::ftxui::Color::RGB(159, 159, 143), // text_dim
        ::ftxui::Color::RGB(0, 0, 0),       // text
        ::ftxui::Color::RGB(224, 224, 224), // line_dim
        ::ftxui::Color::RGB(255, 255, 255), // dialog_bg
        ::ftxui::Color::RGB(40, 198, 228),  // accent
        ::ftxui::Color::RGB(106, 175, 25),  // ok
        ::ftxui::Color::RGB(253, 151, 31),  // warn
        ::ftxui::Color::RGB(249, 38, 114),  // error
        ::ftxui::Color::RGB(174, 129, 255), // suspended
        ::ftxui::Color::RGB(138, 131, 69),  // thought_open
        ::ftxui::Color::RGB(168, 154, 94),  // thought_closed
        ::ftxui::Color::RGB(40, 198, 228),  // prompt
        ::ftxui::Color::RGB(40, 198, 228),  // user_rule
        ::ftxui::Color::RGB(248, 248, 240), // md_h1
        ::ftxui::Color::RGB(249, 38, 114),  // md_h1_bg
        ::ftxui::Color::RGB(242, 90, 0),    // md_h2
        ::ftxui::Color::RGB(253, 151, 31),  // md_h3
        ::ftxui::Color::RGB(40, 198, 228),  // md_h4
        ::ftxui::Color::RGB(0, 0, 0),       // md_code
        ::ftxui::Color::RGB(174, 129, 255), // md_code_inline
        ::ftxui::Color::RGB(237, 237, 237), // md_code_bg
        ::ftxui::Color::RGB(224, 224, 224), // md_rule
        ::ftxui::Color::RGB(159, 159, 143), // md_quote
        ::ftxui::Color::RGB(249, 38, 114),  // md_list
        ::ftxui::Color::RGB(40, 198, 228),  // md_link
    };
    return k;
}

// Palette port: the render layer takes colours from palette(). Tests install a
// fixed provider to render (or replay) with a known palette without depending
// on the terminal; a provider, once installed, wins over the theme.
class IPalette
{
  public:
    virtual ~IPalette() = default;
    virtual const Palette &colors() const = 0;
};

// Function-local static + inline: one slot per process (same object as palette()).
inline std::shared_ptr<const IPalette> &palette_provider_slot()
{
    static std::shared_ptr<const IPalette> provider;
    return provider;
}

// Install a provider; nullptr restores the built-in theme selection.
inline void SetPaletteProvider(std::shared_ptr<const IPalette> provider)
{
    palette_provider_slot() = std::move(provider);
}

inline const Palette &palette()
{
    if (const std::shared_ptr<const IPalette> &provider = palette_provider_slot())
        return provider->colors();
    return theme::IsLight() ? light_palette() : dark_palette();
}

// One vocabulary for the scheduler's task-state names. The task tree paints its
// markers with this table and the todo list takes the failure colour of its
// current step from it, so the two panes cannot drift apart.

// Processing = working right now (the states edc_task_is_running marks).
inline bool TaskStateProcessing(const std::string &state)
{
    return state == "LLM_PROCESSING" || state == "TOOL_CALLING";
}

inline bool TaskStateFailed(const std::string &state)
{
    return state == "ERROR";
}

// Map a scheduler task-state name to the colour of its marker.
inline ::ftxui::Color TaskStateColor(const Palette &pal, const std::string &state)
{
    if (state == "COMPLETED")
        return pal.ok;
    if (TaskStateFailed(state))
        return pal.error;
    if (TaskStateProcessing(state))
        return pal.accent;
    if (state == "INTERRUPTED" || state == "SUSPENDED")
        return pal.suspended;
    if (state == "CANCELLED")
        return pal.text_dim;
    // IDLE / WAITING_DEPS / WAITING_INPUT
    return pal.warn;
}

// The working bar's lit cell and a processing task's sidebar marker are the
// same block, so "working now" reads as one signal.
constexpr const char *kBlockGlyph = "■";

// Period matches the working bar's full sweep in TUIWindow
// (2 * (kRunPositions - 1) * kStepMs = 1800 ms) so the sidebar and footer
// animate at the same rate. The clock is the window's animation time in ms.
constexpr long long kBreathPeriodMs = 1800;

// Breathing curve at |elapsed_ms| in [0, 1]: 1-cos, dim at both ends, brightest
// mid-cycle.
inline float breath_phase(long long elapsed_ms)
{
    long long phase = elapsed_ms % kBreathPeriodMs;
    if (phase < 0)
        phase += kBreathPeriodMs; // a momentarily negative clock must not rewind
    constexpr double kPi = 3.14159265358979323846;
    const double radians = 2.0 * kPi * static_cast<double>(phase) / static_cast<double>(kBreathPeriodMs);
    return 0.5f * (1.0f - static_cast<float>(std::cos(radians)));
}

// Fraction the accent fades towards the pane surface at the trough; the bright
// end is exactly the theme accent.
constexpr float kBreathDimFade = 0.85f;

inline ::ftxui::Color breath_dim(const Palette &pal)
{
    return ::ftxui::Color::Interpolate(kBreathDimFade, pal.accent, pal.pane_bg);
}

// The accent mid-breath: the dim end and the plain accent, nothing brighter.
// The extremes are returned exactly rather than interpolated into, because
// ftxui's per-channel gamma round-trip lands a unit low at t = 0 and t = 1.
inline ::ftxui::Color breath_accent(long long elapsed_ms)
{
    const Palette &pal = palette();
    const float phase = breath_phase(elapsed_ms);
    if (phase <= 0.0f)
        return breath_dim(pal);
    if (phase >= 1.0f)
        return pal.accent;
    return ::ftxui::Color::Interpolate(phase, breath_dim(pal), pal.accent);
}

} // namespace style
} // namespace edc
