// Tests for the OSC 52 clipboard sequence (edc::Osc52ClipboardSequence).
//
// Selecting text in the TUI copies it to the clipboard, and the only clipboard
// a TUI can reach without a helper program is the terminal's, written with an
// OSC 52 escape. That sequence is assembled by hand here (base64 included), so
// its exact bytes are pinned below: a subtle encoding bug would put silently
// garbled text on the user's clipboard, which is hard to notice and hard to
// debug.
#include "edc_style.hpp"
#include "edc_tui_window.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <string>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

// ESC ] 52 ; c ; <base64> BEL -- the 'c' selects the system clipboard.
std::string Expected(const std::string &base64)
{
    return std::string("\x1b]52;c;") + base64 + '\a';
}

void TestBase64Payload()
{
    std::cout << "the payload is the base64 of the selection, wrapped in OSC 52\n";
    Check(edc::Osc52ClipboardSequence("hello") == Expected("aGVsbG8="), "hello");
    Check(edc::Osc52ClipboardSequence("hello world") == Expected("aGVsbG8gd29ybGQ="), "hello world");
    Check(edc::Osc52ClipboardSequence("a") == Expected("YQ=="), "one byte: two pad chars");
    Check(edc::Osc52ClipboardSequence("ab") == Expected("YWI="), "two bytes: one pad char");
    Check(edc::Osc52ClipboardSequence("abc") == Expected("YWJj"), "three bytes: no pad");
    Check(edc::Osc52ClipboardSequence("") == Expected(""), "an empty selection is a bare sequence");
}

void TestRawBytes()
{
    std::cout << "bytes are encoded as raw bytes, so multibyte text survives\n";
    Check(edc::Osc52ClipboardSequence("\xe4\xb8\xad") == Expected("5Lit"), "U+4E2D (3-byte UTF-8)");
    // Bytes >= 0x80 must stay unsigned; sign-extending them would set the high
    // bits and corrupt every non-ASCII selection.
    Check(edc::Osc52ClipboardSequence("\xff\xff\xff") == Expected("////"), "0xFF bytes stay unsigned");
}

void TestToast()
{
    std::cout << "the chip is right-aligned and lets the pane behind it show through\n";

    const int width = 40;
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(3));
    auto base = ::ftxui::vbox({::ftxui::text("BASE-MARK"), ::ftxui::filler()});
    ::ftxui::Render(screen, ::ftxui::dbox({std::move(base), edc::ClipboardToastElement()}));

    std::string row0;
    for (int x = 0; x < width; x++)
        row0 += screen.CellAt(x, 0).character;

    const size_t chip = row0.find("Copied to clipboard");
    Check(chip != std::string::npos, "the label is drawn on the first row");
    Check(row0.find("BASE-MARK") != std::string::npos, "the overlay does not paint over what is behind it");
    Check(chip != std::string::npos && row0.find("BASE-MARK") < chip, "the label sits on the right");
    Check(screen.CellAt(width - 1, 0).background_color == edc::style::palette().dialog_bg,
          "the chip carries the raised surface colour");
}

} // namespace

int main()
{
    TestBase64Payload();
    TestRawBytes();
    TestToast();

    if (g_failures == 0)
    {
        std::cout << "\nall clipboard tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " clipboard check(s) failed\n";
    return 1;
}
