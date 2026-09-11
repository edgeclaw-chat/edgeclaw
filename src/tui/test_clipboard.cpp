// Tests for the OSC 52 clipboard sequence (edc::Osc52ClipboardSequence).
//
// Selecting text in the TUI copies it to the clipboard, and the only clipboard
// a TUI can reach without a helper program is the terminal's, written with an
// OSC 52 escape. That sequence is assembled by hand here (base64 included), so
// its exact bytes are pinned below: a subtle encoding bug would put silently
// garbled text on the user's clipboard, which is hard to notice and hard to
// debug.
#include "edc_clipboard.hpp"
#include "edc_style.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <string>
#include <vector>

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
    std::cout << "the chip is a right-aligned surface taller than its text\n";

    const int width = 40;
    const char *kLabel = "Copied to clipboard";
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(6));
    auto base = ::ftxui::vbox({::ftxui::text("BASE-MARK"), ::ftxui::filler()});
    ::ftxui::Render(screen, ::ftxui::dbox({std::move(base), edc::ClipboardToastElement(0.0f)}));

    // One character per cell. Undrawn cells hold an empty string (not a space,
    // which would shift the columns along), and the bar is multi-byte UTF-8
    // (which would turn find() into a byte offset rather than a column), so the
    // row is built from the first byte of every cell.
    auto columns = [&](int y)
    {
        std::string s;
        for (int x = 0; x < width; x++)
        {
            const std::string &c = screen.CellAt(x, y).character;
            s += c.empty() ? ' ' : c[0];
        }
        return s;
    };

    // Row 0 is the margin below the pane's top edge, rows 1..3 are the surface
    // (blank, label, blank), row 4 is the pane again.
    const std::string row0 = columns(0);
    const std::string row2 = columns(2);
    const size_t label = row2.find(kLabel);

    Check(label != std::string::npos, "the label is drawn below the top edge");
    Check(row0.find("Copied") == std::string::npos, "one blank row is kept from the top edge");
    Check(row0.find("BASE-MARK") != std::string::npos, "the overlay leaves the pane behind it alone");
    // Right-aligned, ending one chip pad plus one margin column short of the
    // edge: it must hug neither the left nor the right edge.
    Check(label != std::string::npos && label + std::string(kLabel).size() == static_cast<size_t>(width) - 2,
          "the label ends a pad and a margin short of the right edge");

    const auto &pal = edc::style::palette();
    const auto cell = screen.CellAt(static_cast<int>(label), 2);
    Check(cell.background_color == pal.dialog_bg, "the chip has a visible surface");
    Check(cell.foreground_color == pal.accent && cell.bold, "bold accent text on it");

    // The surface is taller than the text: the blank rows above and below the
    // label are painted with it too, and the block stops before the edges.
    // The heavy rule the user's own messages carry, on the surface's left edge
    // and spanning its full height.
    const int bar_x = static_cast<int>(label) - 2; // the bar, then the label's leading pad
    Check(screen.CellAt(bar_x, 2).character == "┃" && screen.CellAt(bar_x, 2).foreground_color == pal.user_rule,
          "a user-rule bar flanks the label");
    Check(screen.CellAt(bar_x, 1).character == "┃" && screen.CellAt(bar_x, 3).character == "┃",
          "the bar spans the full surface height");
    Check(screen.CellAt(bar_x, 0).character != "┃", "the bar does not reach the top edge");

    const int surface_x = static_cast<int>(label) - 1; // on the surface, over its leading pad
    Check(screen.CellAt(surface_x, 1).background_color == pal.dialog_bg, "a blank row of surface above the label");
    Check(screen.CellAt(surface_x, 3).background_color == pal.dialog_bg, "a blank row of surface below the label");
    Check(screen.CellAt(surface_x, 0).background_color == ::ftxui::Color::Default,
          "the surface does not reach the top edge");
    Check(screen.CellAt(surface_x, 4).background_color == ::ftxui::Color::Default,
          "the surface ends just below the label");
    Check(screen.CellAt(width - 1, 2).background_color == ::ftxui::Color::Default,
          "the surface stops short of the right edge");
}

void TestDragDetection()
{
    std::cout << "only a drag copies; a click does not\n";
    Check(!edc::IsSelectionDrag(10, 4, 10, 4), "a click is not a drag");
    Check(edc::IsSelectionDrag(10, 4, 12, 4), "a horizontal drag is");
    Check(edc::IsSelectionDrag(10, 4, 10, 7), "a vertical drag is");
    Check(edc::IsSelectionDrag(10, 4, 11, 5), "a one-cell diagonal drag is");
}

// The fade has to land the chip exactly on the pane behind it, or the last
// frames would pop instead of dissolving.
void TestToastFade()
{
    std::cout << "the chip fades into whatever the pane has behind it\n";

    const int width = 40;
    const int height = 6;

    struct Frame
    {
        std::vector<std::string> chars;
        std::vector<::ftxui::Color> fg;
        std::vector<::ftxui::Color> bg;
    };

    auto draw = [&](bool with_toast, float fade, const ::ftxui::Color &pane)
    {
        auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(width), ::ftxui::Dimension::Fixed(height));
        auto base = ::ftxui::vbox({::ftxui::text("BASE-MARK"), ::ftxui::filler()}) | ::ftxui::bgcolor(pane);
        if (with_toast)
            ::ftxui::Render(screen, ::ftxui::dbox({std::move(base), edc::ClipboardToastElement(fade)}));
        else
            ::ftxui::Render(screen, std::move(base));

        Frame frame;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                const auto &cell = screen.CellAt(x, y);
                frame.chars.push_back(cell.character);
                frame.fg.push_back(cell.foreground_color);
                frame.bg.push_back(cell.background_color);
            }
        return frame;
    };

    // A pane colour of its own, so the blend has something to land on.
    const ::ftxui::Color pane = ::ftxui::Color::RGB(12, 34, 56);

    const Frame plain = draw(false, 0.0f, pane);
    const Frame solid = draw(true, 0.0f, pane);
    const Frame half = draw(true, 0.5f, pane);
    const Frame gone = draw(true, 1.0f, pane);

    Check(solid.chars != plain.chars, "fade 0 paints the chip over the pane");

    // A finished fade leaves no trace: not even the characters under it, which
    // blending alone would still have overwritten.
    Check(gone.chars == plain.chars && gone.fg == plain.fg && gone.bg == plain.bg,
          "fade 1 leaves the pane untouched");

    size_t label = half.chars.size();
    for (size_t i = 0; i < half.chars.size(); i++)
    {
        if (half.chars[i] == "C")
        {
            label = i;
            break;
        }
    }
    Check(label < half.chars.size(), "the label is drawn half way through the fade");
    Check(half.bg[label] != solid.bg[label] && half.bg[label] != pane, "half way it is neither surface nor pane");
    Check(half.bg[label] == ::ftxui::Color::Interpolate(0.5f, solid.bg[label], pane),
          "and it is a plain interpolation toward what is behind it");
}

// The chip's own clock: it holds at full strength, then fades to nothing over
// its last stretch, and it is not on screen at all before a copy raised it.
void TestToastTiming()
{
    std::cout << "the chip holds, then fades away\n";

    using clock = std::chrono::steady_clock;
    using ms = std::chrono::milliseconds;
    const clock::time_point t0{};
    const auto fade_mid = edc::ClipboardToast::kHold + edc::ClipboardToast::kFade / 2;

    edc::ClipboardToast toast;
    Check(!toast.Visible(t0), "nothing is shown before a copy");

    toast.Raise(t0);
    Check(toast.Visible(t0), "a copy puts it on screen");
    Check(toast.Fade(t0) == 0.0f, "at full strength to begin with");
    Check(toast.Fade(t0 + edc::ClipboardToast::kHold) == 0.0f, "still at full strength when the fade starts");
    Check(toast.Fade(t0 + fade_mid) > 0.4f && toast.Fade(t0 + fade_mid) < 0.6f, "half gone half way through the fade");
    Check(toast.Fade(t0 + edc::ClipboardToast::kHold + edc::ClipboardToast::kFade) == 1.0f,
          "fully faded when it expires");
    Check(!toast.Visible(t0 + edc::ClipboardToast::kHold + edc::ClipboardToast::kFade), "and then it is gone");

    toast.Raise(t0 + ms(100));
    Check(toast.Visible(t0 + ms(100)), "a second copy raises it again");
}

} // namespace

int main()
{
    TestBase64Payload();
    TestRawBytes();
    TestToast();
    TestDragDetection();
    TestToastFade();
    TestToastTiming();

    if (g_failures == 0)
    {
        std::cout << "\nall clipboard tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " clipboard check(s) failed\n";
    return 1;
}
