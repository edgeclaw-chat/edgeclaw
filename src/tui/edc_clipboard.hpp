#pragma once

#include "edc_style.hpp"
#include <chrono>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

// Clipboard support for the chat: copy a selection and acknowledge it on screen.
//
// FTXUI cannot reach a system clipboard, so everything goes through OSC 52, which
// the terminal performs. It is also the only route that works over SSH, and mouse
// reporting is on, so a plain drag reaches the app instead of the terminal's own
// selection.
namespace edc
{

// The OSC 52 sequence putting |text| on the terminal clipboard (base64 in
// ESC ] 52 ; c ; ... BEL). Exposed so tests can pin the exact bytes.
std::string Osc52ClipboardSequence(const std::string &text);

void CopyToClipboard(const std::string &text);

// Whether a left-button release ends a drag rather than a plain click. A click
// also leaves FTXUI with a one-cell selection nobody meant to copy, possibly on
// a collapsible entry's footer.
bool IsSelectionDrag(int press_x, int press_y, int release_x, int release_y);

// The transient "Copied to clipboard" chip: an overlay pinned to the top-right of
// the pane it is drawn in, one row and column clear of the edges. |fade| runs from
// 0 (fully painted) to 1 (gone) -- FTXUI has no alpha.
::ftxui::Element ClipboardToastElement(float fade);

// When the chip is on screen and how far gone it is.
class ClipboardToast
{
  public:
    // How long the chip holds at full strength, then how long it fades.
    static constexpr auto kHold = std::chrono::milliseconds(1400);
    static constexpr auto kFade = std::chrono::milliseconds(600);

    // A copy just happened: (re)start the chip's life at |now|.
    void Raise(std::chrono::steady_clock::time_point now);

    bool Visible(std::chrono::steady_clock::time_point now) const;

    // 0 while it holds, ramping to 1 as it expires.
    float Fade(std::chrono::steady_clock::time_point now) const;

  private:
    // When the chip goes away; the default (epoch) means "never raised".
    std::chrono::steady_clock::time_point until_;
};

} // namespace edc
