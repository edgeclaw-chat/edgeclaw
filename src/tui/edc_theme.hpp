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

// Parse "dark" / "light" (surrounding whitespace and case are ignored).
// Returns false for anything else -- including "auto" -- so the caller falls
// through to asking the terminal.
bool ParseMode(std::string_view name, Mode &out);

// The mode in effect. Dark until Init() (or a test) says otherwise.
void SetMode(Mode mode);
bool IsLight();

// Decide the mode for this run and install it:
//   1. $EDGECLAW_THEME=dark|light wins outright;
//   2. otherwise ask the terminal for its background colour (OSC 11) and pick
//      by brightness;
//   3. a terminal that does not answer leaves the dark default in place.
// Called once from main(), before ftxui takes over the tty -- the query needs
// the tty still in cooked mode and unclaimed, or the reply would be read as
// keystrokes by the event loop.
void Init();

// --- pieces, exposed for tests -------------------------------------------

// Parse the first OSC 11 reply in |buffer|, in any of the forms terminals use:
//   "\x1b]11;rgb:1e1e/1e1e/1e1e\x07"   xterm, 16-bit components (BEL-terminated)
//   "\x1b]11;rgb:1e/1e/1e\x1b\\"       8-bit components
//   "\x1b]11;#1e1e1e\x1b\\"            CSS-style
// Returns false when |buffer| holds no *complete* reply yet, so the reader can
// keep accumulating.
bool ParseOsc11(std::string_view buffer, unsigned char &r, unsigned char &g, unsigned char &b);

// Whether a background colour reads as light: ITU-R BT.601 luma against the
// midpoint. That puts every common dark theme (down to roughly #2a2a2a) on the
// dark side and every off-white on the light side.
bool IsLightBackground(unsigned char r, unsigned char g, unsigned char b);

// Ask |fd| for its background colour, waiting up to |timeout_ms| for the reply.
// Returns false when |fd| is not a tty, or the terminal stayed silent -- the
// caller then keeps its default.
bool QueryTerminalBackground(int fd, int timeout_ms, unsigned char &r, unsigned char &g, unsigned char &b);

} // namespace theme
} // namespace edc
