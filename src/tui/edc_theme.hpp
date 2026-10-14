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
