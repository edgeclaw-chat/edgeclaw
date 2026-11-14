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

// The transient chip look shared by the "Copied to clipboard" toast and the
// "Loading earlier history" hint: a heavy left rule + label on the raised dialog
// surface, pinned to the top-right of the pane it is drawn in. |label| is used
// verbatim (keep the surrounding spaces for the chip's breathing room). |fade| runs
// from 0 (fully painted) to 1 (gone) -- FTXUI has no alpha.
//
// |rows_above| is how many rows of the pane are already spoken for, so the chip can be
// stacked under them instead of painted over them: when the update notice is up, the toasts
// start below it. It defaults to 0, which keeps the lone-chip behaviour (top margin only).
::ftxui::Element ToastChipElement(const std::string &label, float fade, int rows_above = 0);

// The clipboard chip (ToastChipElement with the "Copied to clipboard" label).
::ftxui::Element ClipboardToastElement(float fade, int rows_above = 0);

// The same chip, but a standing notice rather than a toast: no fade, and a TRANSLUCENT
// surface so what is behind it still reads through. |alpha| is how much of the backdrop
// shows: 0 = the opaque surface a toast uses, 1 = nothing painted at all.
//
// Only the background is blended. ToastFadeNode interpolates the foreground too, because a
// toast is on its way out and washing out its text is the point; a notice that stays on
// screen would spend its whole life hard to read. This is the readable half of the two.
::ftxui::Element TranslucentChipElement(const std::string &label, float alpha);


// How many rows one chip occupies: the label plus the blank row above and below it that give
// the surface its breathing room. Exported because the notices STACK -- a toast drawn while
// the update notice is up has to start below it -- and that offset has to be the chip's real
// height rather than a number someone typed twice.
constexpr int kChipRows = 3;

// One row of margin from the top edge, before the first chip. Every caller starts here so a
// stack and a lone chip put their first row in the same place.
constexpr int kChipTopMargin = 1;

// When a chip is on screen and how far gone it is: a hold at full strength, then a
// fade. Generic over which chip, so the clipboard toast and the loading hint both
// drive it (Raise while the condition holds, leave it alone to let it fade out).
class Toast
{
  public:
    // How long the chip holds at full strength, then how long it fades.
    static constexpr auto kHold = std::chrono::milliseconds(1400);
    static constexpr auto kFade = std::chrono::milliseconds(600);

    // A chip just (re)started at |now|.
    void Raise(std::chrono::steady_clock::time_point now);

    bool Visible(std::chrono::steady_clock::time_point now) const;

    // 0 while it holds, ramping to 1 as it expires.
    float Fade(std::chrono::steady_clock::time_point now) const;

  private:
    // When the chip goes away; the default (epoch) means "never raised".
    std::chrono::steady_clock::time_point until_;
};

// The clipboard chip's state machine (kept named for the tests / call sites).
using ClipboardToast = Toast;

} // namespace edc
