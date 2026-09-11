#pragma once

#include "edc_style.hpp"
#include <chrono>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

// Clipboard support for the chat: put a selection on the terminal's clipboard
// and acknowledge it on screen.
//
// FTXUI cannot reach a system clipboard on its own, so everything here goes
// through OSC 52: the escape sequence travels in the same stream as the drawing
// and the terminal in front of the user performs the copy. That is also the only
// route that works over SSH, and the only one available to us anyway -- mouse
// reporting is on (the log scrolls on the wheel), so a plain drag reaches the app
// instead of starting the terminal's own selection.
namespace edc
{

// The OSC 52 sequence that puts |text| on the terminal's clipboard: the base64
// of the text wrapped in ESC ] 52 ; c ; ... BEL. Exposed so tests can pin the
// exact bytes without needing a terminal.
std::string Osc52ClipboardSequence(const std::string &text);

// Send that sequence for |text| to the terminal.
void CopyToClipboard(const std::string &text);

// Whether a left-button release ends a drag rather than a plain click. A click
// also leaves FTXUI with a one-cell selection, which is not something anyone
// meant to copy, and the click may have been aimed at a collapsible entry's
// footer.
bool IsSelectionDrag(int press_x, int press_y, int release_x, int release_y);

// The transient "Copied to clipboard" chip: an overlay pinned to the top-right
// corner of the pane it is drawn in, one row and one column clear of the edges.
// |fade| runs from 0 (fully painted) to 1 (gone), so the chip dissolves into
// whatever is behind it instead of blinking out.
::ftxui::Element ClipboardToastElement(float fade);

// When the chip is on screen and how far gone it is.
class ClipboardToast
{
  public:
    // How long the chip holds at full strength, and how long it then takes to
    // fade out.
    static constexpr auto kHold = std::chrono::milliseconds(1400);
    static constexpr auto kFade = std::chrono::milliseconds(600);

    // A copy just happened: (re)start the chip's life at |now|.
    void Raise(std::chrono::steady_clock::time_point now);

    // Is the chip on screen at |now|?
    bool Visible(std::chrono::steady_clock::time_point now) const;

    // How far gone it is at |now|: 0 while it holds, ramping to 1 as it expires.
    float Fade(std::chrono::steady_clock::time_point now) const;

  private:
    // When the chip goes away. The default (epoch) means "never raised", which
    // reads as not on screen.
    std::chrono::steady_clock::time_point until_;
};

} // namespace edc
