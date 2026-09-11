#pragma once

#include <ftxui/screen/color.hpp>

namespace edc
{
// Single source of truth for the TUI look (Monokai): every widget draws its
// colors from style::palette(), so panes that must match cannot drift apart and
// a retheme is a one-place change.
namespace style
{

// Semantic color roles, not raw values, so widgets read intent ("the
// in_progress accent") instead of repeating magic RGB triples.
struct Palette
{
    // ---- window chrome -------------------------------------------------
    ::ftxui::Color window_bg;   // outermost backdrop.
    ::ftxui::Color pane_bg;     // sidebar pane + chat message surface.
    ::ftxui::Color text_dim;    // quiet secondary text / neutral rows.
    ::ftxui::Color text;        // body/label text: the terminal's default
                                // foreground. An explicit near-white RGB gets
                                // downsampled to gray on low-color terminals.
    ::ftxui::Color line_dim;    // subtle separator lines.
    ::ftxui::Color dialog_bg;   // raised surface for borderless modals.

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
    // Monokai's syntax mapping: h1 is a pink banner with its text punched out
    // in the editor background, the heading ladder walks yellow -> orange ->
    // cyan, and inline code is a violet chip. No role uses green -- that stays
    // reserved for Palette::ok.
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

// A function-local static on purpose: ftxui::Color::RGB() resolves against the
// current terminal (Terminal::ColorSupport()), which is not ready during C++
// static initialization -- a file-scope object would run before main() and can
// crash. The first call happens at render time.
inline const Palette &palette()
{
    static const Palette k = {
        ::ftxui::Color::RGB(30, 31, 28),     // window_bg      #1E1F1C
        ::ftxui::Color::RGB(39, 40, 34),     // pane_bg        #272822
        ::ftxui::Color::RGB(117, 113, 94),   // text_dim       #75715E comment
        ::ftxui::Color::Default,             // text           terminal foreground
        ::ftxui::Color::RGB(73, 72, 62),     // line_dim       #49483E selection
        ::ftxui::Color::RGB(62, 61, 50),     // dialog_bg      #3E3D32 lite
        ::ftxui::Color::RGB(102, 217, 239),  // accent         #66D9EF cyan
        ::ftxui::Color::RGB(166, 226, 46),   // ok             #A6E22E green
        ::ftxui::Color::RGB(230, 219, 116),  // warn           #E6DB74 yellow
        ::ftxui::Color::RGB(249, 38, 114),   // error          #F92672 pink
        ::ftxui::Color::RGB(174, 129, 255),  // suspended      #AE81FF purple
        ::ftxui::Color::RGB(230, 219, 116),  // thought_open   #E6DB74 yellow
        ::ftxui::Color::RGB(138, 131, 69),   // thought_closed #8A8345 dim yellow
        ::ftxui::Color::RGB(102, 217, 239),  // prompt         #66D9EF accent
        ::ftxui::Color::RGB(102, 217, 239),  // user_rule      #66D9EF accent
        ::ftxui::Color::RGB(39, 40, 34),     // md_h1          #272822
        ::ftxui::Color::RGB(249, 38, 114),   // md_h1_bg       #F92672 pink
        ::ftxui::Color::RGB(230, 219, 116),  // md_h2          #E6DB74 yellow
        ::ftxui::Color::RGB(253, 151, 31),   // md_h3          #FD971F orange
        ::ftxui::Color::RGB(102, 217, 239),  // md_h4          #66D9EF cyan
        ::ftxui::Color::RGB(248, 248, 242),  // md_code        #F8F8F2 foreground
        ::ftxui::Color::RGB(174, 129, 255),  // md_code_inline #AE81FF purple
        ::ftxui::Color::RGB(62, 61, 50),     // md_code_bg     #3E3D32 lite
        ::ftxui::Color::RGB(73, 72, 62),     // md_rule        #49483E selection
        ::ftxui::Color::RGB(117, 113, 94),   // md_quote       #75715E comment
        ::ftxui::Color::RGB(249, 38, 114),   // md_list        #F92672 pink
        ::ftxui::Color::RGB(102, 217, 239),  // md_link        #66D9EF cyan
    };
    return k;
}

} // namespace style
} // namespace edc
