#pragma once

#include <string_view>

namespace edc
{
namespace theme
{

// Which of the two palettes in edc_style.hpp the TUI draws with.
enum class Mode
{
    kDark,
    kLight,
};

// Parse "dark" / "light" (whitespace and case ignored). False for anything else --
// including "auto" -- so the caller falls through to asking the terminal.
bool ParseMode(std::string_view name, Mode &out);

// The mode in effect. Dark until Init() (or a test) says otherwise.
void SetMode(Mode mode);
bool IsLight();

// Decide the mode for this run and install it:
//   1. $EDGECLAW_THEME=dark|light wins outright;
//   2. otherwise ask the terminal for its background colour (OSC 11) and pick by
//      brightness;
//   3. a terminal that does not answer leaves the dark default.
// Called once from main(), before ftxui takes over the tty: the query needs the
// tty still in cooked mode and unclaimed.
void Init();

// Colour depth the renderer is allowed to emit.
//
// This is not cosmetic plumbing: ftxui::Color::RGB() bakes the depth in force at
// construction time into the colour object, snapping it to the nearest entry of
// the palette it has to live with. Under 256 colours the surfaces of
// edc_style.hpp -- designed to sit 1.25-1.35:1 apart, i.e. a handful of sRGB
// levels -- collapse onto the xterm grey ramp, whose rungs are 10 levels apart:
// window_bg #0E0F0D lands on 233 (#121212), pane_bg #272822 on 235 (#262626),
// dialog_bg #3E3D32 on 237 (#3A3A3A), line_dim #49483E on 238 (#444444). Every
// boundary the borderless layout draws survives as a step no one can see, and
// the palette looks exactly as flat as the 1.11:1 one it replaced. So the depth
// has to be settled before the first palette is built.
enum class ColorDepth
{
    kUnset, // no opinion: leave ftxui's own detection alone
    kPalette16,
    kPalette256,
    kTrueColor,
};

// Parse "truecolor" / "24bit" / "16m", "256", "16". False for anything else.
bool ParseColorDepth(std::string_view name, ColorDepth &out);

// What the environment says the terminal can do.
//
// ftxui's own detection reads only $COLORTERM, $TERM and $TERM_PROGRAM, which is
// why a Windows session comes up 256-colour: Windows Terminal announces itself
// through $WT_SESSION and sets neither COLORTERM nor TERM, and conhost announces
// nothing at all. Both render 24-bit SGR once the console is in VT mode (which
// ftxui enables before its first frame), so |wt_session|, |conemu_ansi| and
// |windows_console| take part in the decision. |windows_console| is consulted
// last: an explicit "this terminal only does 256 colours" still wins over the
// platform default.
ColorDepth DetectColorDepth(std::string_view colorterm,
                            std::string_view term,
                            std::string_view term_program,
                            std::string_view wt_session,
                            std::string_view conemu_ansi,
                            bool windows_console);

// Decide the depth for this run and hand it to ftxui: $EDGECLAW_COLOR
// (truecolor|256|16) wins outright, otherwise DetectColorDepth reads the
// environment. A kUnset outcome leaves ftxui's detection standing.
//
// Must run in main() before anything constructs a colour -- that is, before the
// first style::palette() call, because the palettes are built lazily on first
// use and each colour is quantised at that moment.
void InitColorSupport();

// --- pieces, exposed for tests -------------------------------------------

// Parse the first OSC 11 reply in |buffer|, in any of the forms terminals use:
//   "\x1b]11;rgb:1e1e/1e1e/1e1e\x07"   xterm, 16-bit components (BEL-terminated)
//   "\x1b]11;rgb:1e/1e/1e\x1b\\"       8-bit components
//   "\x1b]11;#1e1e1e\x1b\\"            CSS-style
// False while |buffer| holds no complete reply yet, so the reader can keep
// accumulating.
bool ParseOsc11(std::string_view buffer, unsigned char &r, unsigned char &g, unsigned char &b);

// Whether a background colour reads as light: ITU-R BT.601 luma against the
// midpoint.
bool IsLightBackground(unsigned char r, unsigned char g, unsigned char b);

// Ask |fd| for its background colour, waiting up to |timeout_ms|. False when |fd|
// is not a tty or the terminal stayed silent.
bool QueryTerminalBackground(int fd, int timeout_ms, unsigned char &r, unsigned char &g, unsigned char &b);

} // namespace theme
} // namespace edc
