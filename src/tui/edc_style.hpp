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
// The palette is a single, coherent theme: Monokai. The chrome (pane
// backgrounds, sidebar, prompt, status colors) and the Markdown syntax
// colors are drawn from the same token set, so the chat pane and the
// sidebar can never look like two different applications.
//
// Monokai tokens
// --------------
//   background      #272822  (39, 40, 34)    editor background
//   background dark #1E1F1C  (30, 31, 28)    outermost backdrop
//   background lite #3E3D32  (62, 61, 50)    current line / chip surface
//   selection       #49483E  (73, 72, 62)    rules, inactive surfaces
//   comment         #75715E  (117, 113, 94)  muted / secondary text
//   foreground      #F8F8F2  (248, 248, 242) body text, code
//   pink            #F92672  (249, 38, 114)  keywords, emphasis, errors
//   orange          #FD971F  (253, 151, 31)  numbers, constants
//   yellow          #E6DB74  (230, 219, 116) classes, types
//   green           #A6E22E  (166, 226, 46)  strings, success
//   cyan            #66D9EF  (102, 217, 239) functions, primary accent
//   purple          #AE81FF  (174, 129, 255) statements, suspended
//
// The Thought toggle is the one role pair derived from a token instead of
// used as-is: the collapsed label is a dimmed Monokai yellow (60% of
// #E6DB74 => #8A8345) so a folded thought reads as a quiet yellow marker,
// and it expands to the full-brightness yellow so the open thought reads
// as active.
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
    ::ftxui::Color text;        // primary body/label text: the terminal's
                                // default foreground (the same bright white the
                                // task tree uses), so a flat dialog stays fully
                                // legible. An explicit near-white RGB can be
                                // downsampled to gray on low-color terminals,
                                // which is what made the labels look dark.
    ::ftxui::Color line_dim;    // subtle separator lines (todo list divider).
    ::ftxui::Color dialog_bg;   // raised dialog surface for borderless modals
                                // (one step above pane_bg so a flat /connect
                                // panel still reads as a floating surface).

    // ---- accents & status ----------------------------------------------
    ::ftxui::Color accent;      // main cyan accent: headers, in_progress,
                                // spinner, active/tool-calling emphasis.
    ::ftxui::Color ok;          // success: completed todo summary, COMPLETED.
    ::ftxui::Color warn;        // warm yellow: foreground group, IDLE/WAITING.
    ::ftxui::Color error;       // ERROR states.
    ::ftxui::Color suspended;   // SUSPENDED states.
    ::ftxui::Color thought_open;   // expanded "Thought" toggle: bright yellow.
    ::ftxui::Color thought_closed; // collapsed "Thought" toggle: dim yellow.

    // ---- input & chat rules ---------------------------------------------
    // The user turn gets its own hue (violet) so it is distinct from both the
    // pink that means error and the accent blue the task tags and headers use.
    ::ftxui::Color prompt;    // the "❯ " input prompt glyph.
    ::ftxui::Color user_rule; // heavy rule marking user messages.

    // ---- Markdown syntax colors -----------------------------------------
    // Markdown uses the same Monokai tokens as the chrome, mapped the way
    // Monokai themes map syntax: h1 is an emphasis (pink) banner whose
    // text is punched out in the editor background color, then the heading
    // ladder walks yellow -> orange -> cyan, inline code is a purple chip
    // (the violet token sits between the cyan accent and the pink emphasis
    // and list tokens, so a highlight reads as "one of ours"), links follow the
    // function token, and quotes/rules use the comment and selection grays.
    //
    // Note: no Markdown role uses the green token. Green is reserved for
    // the success/COMPLETED status (Palette::ok) so that "done" is the only
    // thing in the UI painted green.
    ::ftxui::Color md_h1;      // ATX heading level 1 text (banner knockout).
    ::ftxui::Color md_h1_bg;   // ATX heading level 1 banner background.
    ::ftxui::Color md_h2;      // ATX heading level 2.
    ::ftxui::Color md_h3;      // ATX heading level 3 (orange).
    ::ftxui::Color md_h4;      // ATX headings level 4+.
    ::ftxui::Color md_code;        // fenced code block text.
    ::ftxui::Color md_code_inline; // inline code text (violet highlight).
    ::ftxui::Color md_code_bg;     // inline code chip background.
    ::ftxui::Color md_rule;        // hr / table / heading rule.
    ::ftxui::Color md_quote;   // blockquote bar.
    ::ftxui::Color md_list;    // list bullet glyphs.
    ::ftxui::Color md_link;    // hyperlink text.
};

inline const Palette &palette()
{
    static const Palette k = {
        // window chrome — Monokai backgrounds
        ::ftxui::Color::RGB(30, 31, 28),     // window_bg   (#1E1F1C)
        ::ftxui::Color::RGB(39, 40, 34),     // pane_bg     (#272822)
        ::ftxui::Color::RGB(117, 113, 94),   // text_dim    (#75715E comment)
        ::ftxui::Color::Default,             // text        (terminal foreground)
        ::ftxui::Color::RGB(73, 72, 62),     // line_dim    (#49483E selection)
        ::ftxui::Color::RGB(62, 61, 50),     // dialog_bg   (#3E3D32 lite)
        // accents & status — Monokai syntax tokens
        ::ftxui::Color::RGB(102, 217, 239),  // accent      (#66D9EF cyan)
        ::ftxui::Color::RGB(166, 226, 46),   // ok          (#A6E22E green)
        ::ftxui::Color::RGB(230, 219, 116),  // warn        (#E6DB74 yellow)
        ::ftxui::Color::RGB(249, 38, 114),   // error       (#F92672 pink)
        ::ftxui::Color::RGB(174, 129, 255),  // suspended   (#AE81FF purple)
        ::ftxui::Color::RGB(230, 219, 116),  // thought_open   (#E6DB74 yellow)
        ::ftxui::Color::RGB(138, 131, 69),   // thought_closed (#8A8345 dim yellow)
        // input & chat rules
        ::ftxui::Color::RGB(174, 129, 255),  // prompt      (#AE81FF violet)
        ::ftxui::Color::RGB(174, 129, 255),  // user_rule   (#AE81FF violet)
        // Markdown syntax colors (Monokai tokens)
        // h1 banner: pink band with the text punched out in the editor
        // background color, i.e. dark-on-pink — maximum contrast, and it
        // matches the surface the Markdown is rendered on.
        ::ftxui::Color::RGB(39, 40, 34),     // md_h1       (#272822)
        ::ftxui::Color::RGB(249, 38, 114),   // md_h1_bg    (#F92672 pink)
        ::ftxui::Color::RGB(230, 219, 116),  // md_h2       (#E6DB74 yellow)
        ::ftxui::Color::RGB(253, 151, 31),   // md_h3       (#FD971F orange)
        ::ftxui::Color::RGB(102, 217, 239),  // md_h4       (#66D9EF cyan)
        ::ftxui::Color::RGB(248, 248, 242),  // md_code     (#F8F8F2 foreground)
        // Inline code is a violet chip: the Monokai purple token on the
        // current-line surface, one step above the pane background. Violet
        // sits between the cyan accent (headers/spinner) and the pink
        // emphasis/list tokens, so a code highlight reads as part of the same
        // family instead of borrowing the green that is reserved for success.
        ::ftxui::Color::RGB(174, 129, 255),  // md_code_inline (#AE81FF purple)
        ::ftxui::Color::RGB(62, 61, 50),     // md_code_bg     (#3E3D32 lite)
        ::ftxui::Color::RGB(73, 72, 62),     // md_rule        (#49483E selection)
        ::ftxui::Color::RGB(117, 113, 94),   // md_quote       (#75715E comment)
        ::ftxui::Color::RGB(249, 38, 114),   // md_list        (#F92672 pink)
        ::ftxui::Color::RGB(102, 217, 239),  // md_link        (#66D9EF cyan)
    };
    return k;
}

} // namespace style
} // namespace edc
