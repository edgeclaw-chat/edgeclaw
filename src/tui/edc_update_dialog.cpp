#include "edc_update_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <string>

namespace edc::ui
{

namespace
{

/* Narrower than the setup wizard: this dialog is prose plus two buttons, and the setup
 * wizard's width makes short lines read as gaps. */
constexpr int kDialogWidth = 62;
/* The failure page shows the installer's output, which can be any shape; keep it to what a
 * user can actually read in one screen. */
constexpr int kMaxDetailLines = 10;

::ftxui::Element Line(const std::string &s)
{
    return ::ftxui::text("  " + s);
}

} // namespace

UpdateDialog::UpdateDialog(StartFn on_start, HintFn on_hint)
    : on_start_(std::move(on_start)), on_hint_(std::move(on_hint))
{
}

void UpdateDialog::Open(const std::string &tag, bool can_upgrade)
{
    open_ = true;
    tag_ = tag;
    detail_.clear();
    button_ = 0;
    /* The one state that is decided before the user is asked anything: a copy the installer
     * did not put there cannot be replaced by it, and saying so up front is better than
     * offering a button that fails. */
    state_ = can_upgrade ? State::CONFIRM : State::UNAVAILABLE;
}

void UpdateDialog::Close()
{
    open_ = false;
    state_ = State::CONFIRM;
    detail_.clear();
}

void UpdateDialog::OnResult(bool ok, const std::string &detail)
{
    if (state_ != State::RUNNING)
        return;
    state_ = ok ? State::DONE : State::FAILED;
    detail_ = detail;
    button_ = 0;
}

void UpdateDialog::Begin()
{
    if (state_ != State::CONFIRM)
        return;
    state_ = State::RUNNING;
    detail_.clear();
    if (on_start_)
        on_start_(tag_);
}

bool UpdateDialog::OnEvent(::ftxui::Event event)
{
    if (!open_)
        return false;

    using ::ftxui::Event;

    if (event == Event::Escape)
    {
        /* Every state closes on Esc, including RUNNING: abandoning an upgrade has to be
         * possible, and the confirmation page already said what that costs. */
        Close();
        return true;
    }

    switch (state_)
    {
    case State::CONFIRM:
        if (event == Event::ArrowLeft || event == Event::Character('h'))
        {
            button_ = 0;
            return true;
        }
        if (event == Event::ArrowRight || event == Event::Character('l'))
        {
            button_ = 1;
            return true;
        }
        if (event == Event::Tab)
        {
            button_ = 1 - button_;
            return true;
        }
        if (event == Event::Return)
        {
            if (button_ == 0)
                Begin();
            else
                Close();
            return true;
        }
        return false;

    case State::DONE:
    case State::FAILED:
    case State::UNAVAILABLE:
        /* One button, so Enter and Space both dismiss. Consuming them keeps the key from
         * reaching the input box behind the dialog. */
        if (event == Event::Return || event == Event::Character(' '))
        {
            Close();
            return true;
        }
        return false;

    case State::RUNNING:
    default:
        /* Nothing to press; Esc above is the only exit. */
        return false;
    }
}

::ftxui::Element UpdateDialog::ButtonsElement() const
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    auto chip = [&](const char *label, bool active) {
        Element e = text("  " + std::string(label) + "  ");
        return active ? (e | bold | bgcolor(pal.accent) | color(pal.dialog_bg)) : (e | color(pal.text));
    };

    switch (state_)
    {
    case State::CONFIRM:
        return hbox({filler(), chip("Update", button_ == 0), text("   "), chip("Cancel", button_ == 1), filler()});
    case State::DONE:
        return hbox({filler(), chip("Close", true), filler()});
    case State::FAILED:
    case State::UNAVAILABLE:
        return hbox({filler(), chip("Close", true), filler()});
    case State::RUNNING:
    default:
        return hbox({filler(), text("  working…  ") | color(pal.text_dim), filler()});
    }
}

::ftxui::Element UpdateDialog::Draw(const Frame &)
{
    using namespace ::ftxui;
    const auto &pal = style::palette();

    Elements rows;

    switch (state_)
    {
    case State::CONFIRM:
    {
        rows.push_back(hbox({Line("EdgeClaw "), text(tag_) | bold | color(pal.accent), filler()}));
        rows.push_back(separator() | color(pal.line_dim));
        rows.push_back(Line("A newer release is available. Updating replaces this"));
        rows.push_back(Line("binary with the published one for your platform."));
        rows.push_back(text(""));
        /* The part a user cannot undo, stated before they press anything. */
        rows.push_back(Line("Your conversations are kept, but the database is"));
        rows.push_back(Line("upgraded on first start -- and an older version will") | color(pal.text_dim));
        rows.push_back(Line("refuse it afterwards. This update cannot be undone.") | color(pal.text_dim));
        rows.push_back(text(""));
        rows.push_back(hbox({filler(), text("Left/Right choose  |  Enter confirm  |  Esc cancel ") |
                                         color(pal.text_dim)}));
        break;
    }

    case State::RUNNING:
        rows.push_back(hbox({Line("Updating to "), text(tag_) | bold | color(pal.accent)}));
        rows.push_back(separator() | color(pal.line_dim));
        rows.push_back(Line("Downloading and installing. This can take a moment."));
        rows.push_back(text(""));
        rows.push_back(hbox({filler(), text("Esc abandons the update ") | color(pal.text_dim)}));
        break;

    case State::DONE:
        rows.push_back(hbox({Line("Updated to "), text(tag_) | bold | color(pal.accent)}));
        rows.push_back(separator() | color(pal.line_dim));
        rows.push_back(Line("The new binary is in place."));
        rows.push_back(text(""));
        /* Deliberately not "restarting now": an automatic restart would interrupt whatever
         * task is running, and the choice of when belongs to the user. */
        rows.push_back(Line("Quit and start EdgeClaw again to use it.") | bold);
        rows.push_back(text(""));
        rows.push_back(hbox({filler(), text("Enter closes ") | color(pal.text_dim)}));
        break;

    case State::FAILED:
    {
        rows.push_back(Line("Update failed") | bold | color(pal.accent));
        rows.push_back(separator() | color(pal.line_dim));
        rows.push_back(Line("The update did not complete; this version is unchanged."));
        if (!detail_.empty())
        {
            rows.push_back(text(""));
            /* The installer's own output, clipped: it is the only diagnostic a user can
             * paste into a report. */
            int shown = 0;
            size_t pos = 0;
            while (pos <= detail_.size() && shown < kMaxDetailLines)
            {
                const size_t nl = detail_.find('\n', pos);
                const std::string line =
                    detail_.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (!line.empty())
                {
                    rows.push_back(Line(line) | color(pal.text_dim));
                    shown++;
                }
                if (nl == std::string::npos)
                    break;
                pos = nl + 1;
            }
        }
        if (on_hint_)
        {
            rows.push_back(text(""));
            rows.push_back(Line("To update by hand:") | color(pal.text_dim));
            rows.push_back(Line(on_hint_()) | color(pal.text_dim));
        }
        rows.push_back(text(""));
        rows.push_back(hbox({filler(), text("Enter closes ") | color(pal.text_dim)}));
        break;
    }

    case State::UNAVAILABLE:
    default:
    {
        rows.push_back(hbox({Line("Update "), text(tag_) | bold | color(pal.accent), filler()}));
        rows.push_back(separator() | color(pal.line_dim));
        /* Why this copy cannot do it itself: the installer is what knows how to fetch and
         * check a release, and it only manages binaries it put there. */
        rows.push_back(Line("This copy was not installed by the EdgeClaw installer,"));
        rows.push_back(Line("so it cannot update itself."));
        if (on_hint_)
        {
            rows.push_back(text(""));
            rows.push_back(Line("To update by hand:") | color(pal.text_dim));
            rows.push_back(Line(on_hint_()) | color(pal.text_dim));
        }
        rows.push_back(text(""));
        rows.push_back(hbox({filler(), text("Enter closes ") | color(pal.text_dim)}));
        break;
    }
    }

    rows.push_back(text(""));
    rows.push_back(separator() | color(pal.line_dim));
    rows.push_back(ButtonsElement());

    return vbox(std::move(rows)) | size(WIDTH, EQUAL, kDialogWidth) | borderStyled(BorderStyle::ROUNDED) |
           bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
