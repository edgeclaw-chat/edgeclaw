#pragma once

#include "edc_theme.hpp"

#include <ftxui/screen/color.hpp>

namespace edc
{
// Single source of truth for the TUI look: every widget draws its colors from
// style::palette(), so panes that must match cannot drift apart and a retheme
// is a one-place change.
//
// There are two palettes, dark and light, and palette() picks between them from
// the terminal background edc_theme resolved at startup (OSC 11 probe, or
// $EDGECLAW_THEME). Widgets never see this: they ask for a role and get the
// value that suits the terminal they are running in.
namespace style
{

// Semantic color roles, not raw values, so widgets read intent ("the
// in_progress accent") instead of repeating magic RGB triples.
struct Palette
{
    // ---- window chrome -------------------------------------------------
    ::ftxui::Color window_bg; // outermost backdrop.
    ::ftxui::Color pane_bg;   // sidebar pane + chat message surface.
    ::ftxui::Color text_dim;  // quiet secondary text / neutral rows.
    ::ftxui::Color text;      // body/label text. Always an explicit value, on
                              // both palettes: every surface this palette paints
                              // (window_bg, pane_bg, dialog_bg) is one it chose,
                              // so the body colour must be too -- inheriting the
                              // terminal's default foreground would pair a
                              // light-terminal black with the dark palette's
                              // near-black pane. Matches md_code.
    ::ftxui::Color line_dim;  // subtle separator lines.
    ::ftxui::Color dialog_bg; // raised surface for borderless modals.

    // ---- accents & status ----------------------------------------------
    ::ftxui::Color accent;         // cyan: headers, in_progress, spinner.
    ::ftxui::Color ok;             // green: success/COMPLETED, and nothing else.
    ::ftxui::Color warn;           // yellow: foreground group, IDLE/WAITING.
    ::ftxui::Color error;          // ERROR states.
    ::ftxui::Color suspended;      // SUSPENDED states.
    ::ftxui::Color thought_open;   // expanded "Thought" toggle.
    ::ftxui::Color thought_closed; // collapsed "Thought" toggle.

    // ---- input & chat rules ---------------------------------------------
    ::ftxui::Color prompt;    // the "❯ " input prompt glyph.
    ::ftxui::Color user_rule; // heavy rule marking user messages.

    // ---- Markdown -------------------------------------------------------
    // Monokai's syntax mapping: h1 is a banner with its text punched out in the
    // surface colour, the heading ladder walks yellow -> orange -> cyan, and
    // inline code is a violet chip. No role uses green -- that stays reserved
    // for Palette::ok.
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
// crash. The first call happens at render time.

// The original Monokai look, for terminals with a dark background.
inline const Palette &dark_palette()
{
    static const Palette k = {
        ::ftxui::Color::RGB(30, 31, 28),    // window_bg      #1E1F1C
        ::ftxui::Color::RGB(39, 40, 34),    // pane_bg        #272822
        ::ftxui::Color::RGB(117, 113, 94),  // text_dim       #75715E comment
        ::ftxui::Color::RGB(248, 248, 242), // text           #F8F8F2 foreground
        ::ftxui::Color::RGB(73, 72, 62),    // line_dim       #49483E selection
        ::ftxui::Color::RGB(62, 61, 50),    // dialog_bg      #3E3D32 lite
        ::ftxui::Color::RGB(102, 217, 239), // accent         #66D9EF cyan
        ::ftxui::Color::RGB(166, 226, 46),  // ok             #A6E22E green
        ::ftxui::Color::RGB(230, 219, 116), // warn           #E6DB74 yellow
        ::ftxui::Color::RGB(249, 38, 114),  // error          #F92672 pink
        ::ftxui::Color::RGB(174, 129, 255), // suspended      #AE81FF purple
        ::ftxui::Color::RGB(230, 219, 116), // thought_open   #E6DB74 yellow
        ::ftxui::Color::RGB(138, 131, 69),  // thought_closed #8A8345 dim yellow
        ::ftxui::Color::RGB(102, 217, 239), // prompt         #66D9EF accent
        ::ftxui::Color::RGB(102, 217, 239), // user_rule      #66D9EF accent
        ::ftxui::Color::RGB(39, 40, 34),    // md_h1          #272822 punched out
        ::ftxui::Color::RGB(249, 38, 114),  // md_h1_bg       #F92672 pink
        ::ftxui::Color::RGB(230, 219, 116), // md_h2          #E6DB74 yellow
        ::ftxui::Color::RGB(253, 151, 31),  // md_h3          #FD971F orange
        ::ftxui::Color::RGB(102, 217, 239), // md_h4          #66D9EF cyan
        ::ftxui::Color::RGB(248, 248, 242), // md_code        #F8F8F2 foreground
        ::ftxui::Color::RGB(174, 129, 255), // md_code_inline #AE81FF purple
        ::ftxui::Color::RGB(62, 61, 50),    // md_code_bg     #3E3D32 lite
        ::ftxui::Color::RGB(73, 72, 62),    // md_rule        #49483E selection
        ::ftxui::Color::RGB(117, 113, 94),  // md_quote       #75715E comment
        ::ftxui::Color::RGB(249, 38, 114),  // md_list        #F92672 pink
        ::ftxui::Color::RGB(102, 217, 239), // md_link        #66D9EF cyan
    };
    return k;
}

// The light counterpart: rjfranco's Monokai Light, used verbatim.
//   https://github.com/rjfranco/monokai-light
// Surfaces keep the dark palette's relationship (pane_bg is the lighter one,
// window_bg sits behind it). Every hue is taken from that theme -- pane_bg is
// its `background`, text its `foreground`, text_dim its `comment`, line_dim its
// `invisibles`, window_bg its `inactiveSelection`, and md_h1/md_h1_bg are
// exactly its `invalid` pair (a pink banner with the text punched out).
//
// Note on contrast: Monokai Light puts its syntax colours on pure white at
// 2.0-3.8:1, which is below WCAG AA for small text -- that is the look, not an
// oversight, and it is in the same range the dark palette already ships for
// `error` (3.9), `md_list` (3.9) and `text_dim` (3.0). The ratios below are
// measured against #FFFFFF; only `text` is high-contrast (21:1).
//
// The Thought toggle is the one role that does not come from Monokai Light: it
// stays yellow, as it is on the dark palette, but the dark yellow (#E6DB74) is
// only 1.4:1 on white -- invisible -- so the hue is kept at the intensity that
// survives a white surface.
inline const Palette &light_palette()
{
    static const Palette k = {
        ::ftxui::Color::RGB(237, 237, 237), // window_bg      #EDEDED inactiveSelection
        ::ftxui::Color::RGB(255, 255, 255), // pane_bg        #FFFFFF background
        ::ftxui::Color::RGB(159, 159, 143), // text_dim       #9F9F8F comment 2.7:1
        ::ftxui::Color::RGB(0, 0, 0),       // text           #000000 foreground 21:1
        ::ftxui::Color::RGB(224, 224, 224), // line_dim       #E0E0E0 invisibles
        ::ftxui::Color::RGB(255, 255, 255), // dialog_bg      #FFFFFF raised off the #EDEDED backdrop
        ::ftxui::Color::RGB(40, 198, 228),  // accent         #28C6E4 support.type 2.0:1
        ::ftxui::Color::RGB(106, 175, 25),  // ok             #6AAF19 function/class 2.7:1
        ::ftxui::Color::RGB(253, 151, 31),  // warn           #FD971F function argument 2.2:1
        ::ftxui::Color::RGB(249, 38, 114),  // error          #F92672 keyword 3.8:1
        ::ftxui::Color::RGB(174, 129, 255), // suspended      #AE81FF constant 2.8:1
        ::ftxui::Color::RGB(138, 131, 69),  // thought_open   #8A8345 yellow 3.9:1
        ::ftxui::Color::RGB(168, 154, 94),  // thought_closed #A89A5E quieter yellow 2.8:1
        ::ftxui::Color::RGB(40, 198, 228),  // prompt         #28C6E4 accent
        ::ftxui::Color::RGB(40, 198, 228),  // user_rule      #28C6E4 accent
        ::ftxui::Color::RGB(248, 248, 240), // md_h1          #F8F8F0 invalid foreground
        ::ftxui::Color::RGB(249, 38, 114),  // md_h1_bg       #F92672 invalid background
        ::ftxui::Color::RGB(242, 90, 0),    // md_h2          #F25A00 string
        ::ftxui::Color::RGB(253, 151, 31),  // md_h3          #FD971F function argument
        ::ftxui::Color::RGB(40, 198, 228),  // md_h4          #28C6E4 support.type
        ::ftxui::Color::RGB(0, 0, 0),       // md_code        #000000 foreground
        ::ftxui::Color::RGB(174, 129, 255), // md_code_inline #AE81FF constant
        ::ftxui::Color::RGB(237, 237, 237), // md_code_bg     #EDEDED chip under md_code_inline
        ::ftxui::Color::RGB(224, 224, 224), // md_rule        #E0E0E0 invisibles
        ::ftxui::Color::RGB(159, 159, 143), // md_quote       #9F9F8F comment
        ::ftxui::Color::RGB(249, 38, 114),  // md_list        #F92672 keyword
        ::ftxui::Color::RGB(40, 198, 228),  // md_link        #28C6E4 support.type
    };
    return k;
}

// The palette the widgets draw from, chosen from the terminal background
// edc_theme resolved at startup.
inline const Palette &palette()
{
    return theme::IsLight() ? light_palette() : dark_palette();
}

} // namespace style
} // namespace edc
