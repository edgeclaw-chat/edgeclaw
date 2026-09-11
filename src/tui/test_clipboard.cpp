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
    std::cout << "the chip is flat, right-aligned and keeps a margin\n";

    const int width = 40;
    const char *kLabel = "Copied to clipboard";
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(3));
    auto base = ::ftxui::vbox({::ftxui::text("BASE-MARK"), ::ftxui::filler()});
    ::ftxui::Render(screen, ::ftxui::dbox({std::move(base), edc::ClipboardToastElement()}));

    // Cells nobody drew on hold an empty string, not a space, so fold them to
    // spaces first: otherwise the columns before the label vanish and its real
    // position cannot be read off the row.
    auto row_text = [&](int y)
    {
        std::string s;
        for (int x = 0; x < width; x++)
        {
            const std::string &c = screen.CellAt(x, y).character;
            s += c.empty() ? " " : c;
        }
        return s;
    };
    const std::string row0 = row_text(0);
    const std::string row1 = row_text(1);
    const size_t label = row1.find(kLabel);

    Check(label != std::string::npos, "the label is drawn on the row below the edge");
    Check(row0.find("Copied") == std::string::npos, "one blank row is kept from the top edge");
    Check(row0.find("BASE-MARK") != std::string::npos, "the overlay leaves the pane behind it alone");
    // Right-aligned, ending one chip pad plus one margin column short of the
    // edge: it must hug neither the left nor the right edge.
    Check(label != std::string::npos && label + std::string(kLabel).size() == static_cast<size_t>(width) - 2,
          "the label ends a pad and a margin short of the right edge");

    // Flat like the Tasks/Todos headers: coloured text on the pane background,
    // no border and no second surface behind it.
    const auto &pal = edc::style::palette();
    const auto cell = screen.CellAt(static_cast<int>(label), 1);
    Check(cell.background_color == ::ftxui::Color::Default, "no background band");
    Check(cell.foreground_color == pal.accent && cell.bold, "bold accent, like the section headers");
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
