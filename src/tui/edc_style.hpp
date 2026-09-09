#pragma once

#include <ftxui/screen/color.hpp>

namespace edc
{
// Central style configuration for the whole TUI module.
//
// Widgets used to build their look from local color constants scattered
// across each file (chat messages, the sidebar task tree, the todo list,
// the window chrome and the Markdown renderer each inlined their own RGB
// values). They now draw every color from style::palette() below, so:
//   - one file is the single source of truth for the look;
//   - panes that must match (sidebar background, chat message background,
//     accent hues shared between the spinner, headers and status labels)
//     can never drift apart again;
//   - retheming the UI is a one-place change instead of a grep across
//     widgets.
//
// Initialization note: palette() is a function-local static, deliberately
// NOT a namespace-scope object. ftxui::Color::RGB(...) resolves the color
// against the current terminal (it calls Terminal::ColorSupport(), see
// ftxui/src/ftxui/screen/color.cpp), and that state is not ready during C++
// static initialization — a file-scope const Color would run before main()
// and can crash. The first palette() call therefore happens at render time
// (always after main()), and every later call just returns the same cached
// object.
namespace style
{

// One look for the application: semantic color roles, not raw values.
// Roles are named after what they color, so widgets read intent ("the
// in_progress accent") instead of repeating magic RGB triples.
struct Palette
{
    // ---- window chrome -------------------------------------------------
    ::ftxui::Color window_bg;   // outermost backdrop behind everything.
    ::ftxui::Color pane_bg;     // sidebar pane + chat message surface.
    ::ftxui::Color text_dim;    // quiet secondary text / neutral rows.
    ::ftxui::Color line_dim;    // subtle separator lines (todo list divider).

    // ---- accents & status ----------------------------------------------
    ::ftxui::Color accent;      // main blue accent: headers, in_progress,
                                // spinner, active/tool-calling emphasis.
    ::ftxui::Color ok;          // success: completed todo summary, COMPLETED.
    ::ftxui::Color warn;        // warm yellow: foreground group, IDLE/WAITING.
    ::ftxui::Color error;       // ERROR states.
    ::ftxui::Color suspended;   // SUSPENDED states.
    ::ftxui::Color thought_open;   // expanded "Thought" toggle.
    ::ftxui::Color thought_closed; // collapsed "Thought" toggle.

    // ---- input & chat rules ---------------------------------------------
    ::ftxui::Color prompt;    // the "❯ " input prompt glyph.
    ::ftxui::Color user_rule; // heavy rule marking user messages.

    // ---- Markdown syntax colors -----------------------------------------
    // Aligned with glow/glamour's "dark" look (xterm indices from
    // glamour/styles/dark.json): headings 39, h1 banner 228 on 63,
    // code 244/251, quote bar & body 252.
    ::ftxui::Color md_h1;      // ATX heading level 1 text (banner).
    ::ftxui::Color md_h1_bg;   // ATX heading level 1 banner background.
    ::ftxui::Color md_h2;      // ATX heading level 2.
    ::ftxui::Color md_h3;      // ATX heading level 3.
    ::ftxui::Color md_h4;      // ATX headings level 4+.
    ::ftxui::Color md_code;        // fenced code block text (glow: plain gray).
    ::ftxui::Color md_code_inline; // inline code text (glow-style coral).
    ::ftxui::Color md_code_bg;     // inline code chip background (glow-style faint).
    ::ftxui::Color md_rule;        // hr / table / code frame / heading underline.
    ::ftxui::Color md_quote;   // blockquote bar.
    ::ftxui::Color md_list;    // list bullet glyphs.
    ::ftxui::Color md_link;    // hyperlink text.
};

inline const Palette &palette()
{
    static const Palette k = {
        // window chrome
        ::ftxui::Color::RGB(10, 10, 10),     // window_bg
        ::ftxui::Color::RGB(30, 30, 30),     // pane_bg
        ::ftxui::Color::RGB(147, 147, 147),  // text_dim
        ::ftxui::Color::RGB(60, 60, 60),     // line_dim
        // accents & status
        ::ftxui::Color::RGB(94, 151, 247),   // accent
        ::ftxui::Color::RGB(64, 168, 96),    // ok
        ::ftxui::Color::RGB(229, 192, 123),  // warn
        ::ftxui::Color::RGB(224, 108, 117),  // error
        ::ftxui::Color::RGB(198, 120, 221),  // suspended
        ::ftxui::Color::RGB(245, 167, 66),   // thought_open
        ::ftxui::Color::RGB(151, 104, 44),   // thought_closed
        // input & chat rules
        ::ftxui::Color::Blue,                // prompt
        ::ftxui::Color::Blue,                // user_rule
        // Markdown syntax colors (glow/glamour "dark" style)
        ::ftxui::Color::RGB(255, 255, 135),  // md_h1 (228)
        ::ftxui::Color::RGB(95, 95, 255),    // md_h1_bg (63)
        ::ftxui::Color::RGB(0, 175, 255),    // md_h2 (39)
        ::ftxui::Color::RGB(0, 175, 255),    // md_h3 (39)
        ::ftxui::Color::RGB(0, 175, 255),    // md_h4 (39)
        ::ftxui::Color::RGB(196, 196, 196),  // md_code (251-ish)
        // Inline code follows the glow/glamour "dark" look: coral text on a
        // near-black chip (glamour 256-color 203 over 236). The chip sits
        // only one step above the pane background, so code reads as tinted
        // text rather than a loud highlighted block.
        ::ftxui::Color::RGB(255, 95, 135),   // md_code_inline
        ::ftxui::Color::RGB(48, 48, 48),     // md_code_bg
        ::ftxui::Color::RGB(128, 128, 128),  // md_rule
        ::ftxui::Color::RGB(208, 208, 208),  // md_quote (252)
        ::ftxui::Color::Blue,                // md_list
        ::ftxui::Color::Blue,                // md_link
    };
    return k;
}

} // namespace style
} // namespace edc
