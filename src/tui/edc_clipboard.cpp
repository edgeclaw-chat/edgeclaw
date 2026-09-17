#include "edc_clipboard.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace edc
{

// Put |text| on the terminal's clipboard with OSC 52: ESC ] 52 ; c ; <base64> BEL.
// It is the only clipboard a TUI can reach without a helper program, and it works
// over SSH: the sequence travels with the drawing and the terminal performs the
// copy. We have to do it ourselves anyway -- mouse reporting is on, so a drag reaches
// the app instead of starting the terminal's own selection. Defined here (declared in
// the header) so tests can pin the exact bytes.
std::string Osc52ClipboardSequence(const std::string &text)
{
    static constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve((text.size() + 2) / 3 * 4);
    for (size_t i = 0; i < text.size(); i += 3)
    {
        const size_t remaining = text.size() - i;
        const unsigned b0 = static_cast<unsigned char>(text[i]);
        const unsigned b1 = remaining > 1 ? static_cast<unsigned char>(text[i + 1]) : 0;
        const unsigned b2 = remaining > 2 ? static_cast<unsigned char>(text[i + 2]) : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;

        encoded += kBase64[(triple >> 18) & 0x3f];
        encoded += kBase64[(triple >> 12) & 0x3f];
        encoded += remaining > 1 ? kBase64[(triple >> 6) & 0x3f] : '=';
        encoded += remaining > 2 ? kBase64[triple & 0x3f] : '=';
    }

    return std::string("\x1b]52;c;") + encoded + '\a';
}

void CopyToClipboard(const std::string &text)
{
    const std::string sequence = Osc52ClipboardSequence(text);
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
}

namespace
{
// Fades its child back into whatever the screen already held where the child
// paints. The chip is an overlay, so at this point those cells still carry the
// pane behind it: fading toward them lands exactly on the real backdrop instead
// of on a guessed colour. FTXUI cells have no alpha, so blending their colours
// is as close to a fade as a terminal cell gets.
class ToastFadeNode : public ::ftxui::Node
{
  public:
    ToastFadeNode(::ftxui::Element child, float fade) : Node({std::move(child)}), fade_(fade)
    {
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(::ftxui::Screen &screen) override
    {
        // A finished fade paints nothing at all: blending onto the backdrop
        // would still overwrite the characters sitting under the chip, and they
        // are not ours to erase.
        if (fade_ >= 1.0f)
            return;

        if (fade_ <= 0.0f)
        {
            Node::Render(screen);
            return;
        }

        const int columns = box_.x_max - box_.x_min + 1;
        const int rows = box_.y_max - box_.y_min + 1;
        if (columns <= 0 || rows <= 0)
            return;

        std::vector<::ftxui::Color> backdrop;
        backdrop.reserve(static_cast<size_t>(columns) * static_cast<size_t>(rows));
        for (int y = box_.y_min; y <= box_.y_max; y++)
            for (int x = box_.x_min; x <= box_.x_max; x++)
                backdrop.push_back(screen.CellAt(x, y).background_color);

        Node::Render(screen);

        size_t i = 0;
        for (int y = box_.y_min; y <= box_.y_max; y++)
        {
            for (int x = box_.x_min; x <= box_.x_max; x++)
            {
                ::ftxui::Cell &cell = screen.CellAt(x, y);
                const ::ftxui::Color &behind = backdrop[i++];
                cell.background_color = ::ftxui::Color::Interpolate(fade_, cell.background_color, behind);
                cell.foreground_color = ::ftxui::Color::Interpolate(fade_, cell.foreground_color, behind);
            }
        }
    }

  private:
    float fade_;
};
} // namespace

// The "Copied to clipboard" chip: flat and borderless like the section headers, but
// on the palette's raised surface (dialog_bg) so it reads as a surface of its own
// instead of vanishing into the message background. A blank row above and below the
// label lives inside it, and one cell of margin keeps it off the corner. The heavy
// left rule is the one UserMessage carries, so the chip reads as something the user
// just did. |fade| runs from 0 (fully painted) to 1 (gone).
::ftxui::Element ClipboardToastElement(float fade)
{
    const auto &pal = style::palette();
    auto bar = ::ftxui::separatorHeavy() | ::ftxui::color(pal.user_rule);
    auto label = ::ftxui::text(" Copied to clipboard ") | ::ftxui::bold | ::ftxui::color(pal.accent);
    ::ftxui::Element chip = ::ftxui::hbox({
                                std::move(bar),
                                ::ftxui::vbox({
                                    ::ftxui::text(""), // breathing room above the label
                                    std::move(label),
                                    ::ftxui::text(""), // and below it
                                }),
                            }) |
                            ::ftxui::bgcolor(pal.dialog_bg);
    chip = std::make_shared<ToastFadeNode>(std::move(chip), fade);
    // The right margin is an empty element pinned to one column rather than a
    // space: a literal space would write a character over the pane even when the
    // chip itself paints nothing.
    auto margin = ::ftxui::text("") | ::ftxui::size(::ftxui::WIDTH, ::ftxui::EQUAL, 1);
    return ::ftxui::vbox({
        ::ftxui::text(""), // one blank row below the top edge
        ::ftxui::hbox({::ftxui::filler(), std::move(chip), std::move(margin)}),
        ::ftxui::filler(),
    });
}

// Whether a left-button release ends a drag rather than a plain click. A click
// also leaves FTXUI with a one-cell selection, which is not something anyone
// meant to copy, and it may have been aimed at a collapsible entry's footer.
bool IsSelectionDrag(int press_x, int press_y, int release_x, int release_y)
{
    return press_x != release_x || press_y != release_y;
}

void ClipboardToast::Raise(std::chrono::steady_clock::time_point now)
{
    until_ = now + kHold + kFade;
}

bool ClipboardToast::Visible(std::chrono::steady_clock::time_point now) const
{
    return now < until_;
}

float ClipboardToast::Fade(std::chrono::steady_clock::time_point now) const
{
    if (now >= until_)
        return 1.0f;

    const auto left = until_ - now;
    if (left >= kFade)
        return 0.0f;

    // What is left of the chip's life is exactly the fade's remaining fraction,
    // so the fade lands on gone just as the chip expires.
    const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    return 1.0f - static_cast<float>(left_ms) / static_cast<float>(kFade.count());
}

} // namespace edc
