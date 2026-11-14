#include "edc_update_dialog.hpp"

#include "edc_style.hpp"

#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>
#include <string>

namespace edc::ui
{

namespace
{

/* Narrower than the setup wizard: this dialog is prose plus two buttons, and the setup
 * wizard's width makes short lines read as gaps.
 *
 * 64, not 62, because of the "by hand" block: the release URL is 61 columns, and the text
 * area is kDialogWidth - 2, so at 62 the URL broke at its last slash and left "v9.9.9" on a
 * line of its own. 64 puts it back on one line without widening the prose enough to read as
 * gappy. Measured on a rendered frame, not estimated. */
constexpr int kDialogWidth = 64;
/* What a line of text can actually occupy: the width minus the two-space indent every row
 * carries. Wrapping is measured against this, not kDialogWidth, or every wrapped line would
 * overflow by exactly the indent. */
constexpr int kTextWidth = kDialogWidth - 2;
/* The failure page shows the installer's output, which can be any shape; keep it to what a
 * user can actually read in one screen. Counted in SOURCE lines, before wrapping. */
constexpr int kMaxDetailLines = 10;

enum class Tone
{
    Body,   /* prose: everything the dialog is saying to the reader */
    Accent, /* the one emphasised line: the page's own heading */
};

/* One row of dialog text.
 *
 * EVERY row goes through here so its colour is explicit, and the tone is chosen HERE rather
 * than by decorating the result. That distinction is not style: `Row(...) | color(pal.accent)`
 * does not work, because Row has already set a colour on the cell and ftxui keeps the inner
 * one. The failed page's heading was written that way and rendered in the body colour -- so the
 * page had a heading that looked like prose, followed by body text, followed by dim quoted
 * output: three shades down the page for readers who could not tell which was meant.
 *
 * So the page is one colour for everything it says, accent for its heading, and nothing else.
 */
::ftxui::Element Row(const std::string &s, Tone tone = Tone::Body)
{
    const auto &pal = style::palette();
    const ::ftxui::Color c = (tone == Tone::Accent) ? pal.accent : pal.text;
    return ::ftxui::text("  " + s) | ::ftxui::color(c);
}

/* Drop ANSI escapes from |s|. The run layer already strips them at the source, but |detail_|
 * can also arrive from a caller that did not, and the symptom is bad enough to be worth two
 * layers: ftxui DRAWS an escape rather than interpreting it, so one sequence both prints as
 * "[1m" and breaks the colour of every cell after it on the row. */
std::string StripEscapes(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (static_cast<unsigned char>(s[i]) != 0x1B)
        {
            out += s[i];
            continue;
        }
        /* Skip the sequence. CSI ends at a byte in 0x40..0x7E; OSC at BEL or ESC \; every
         * other escape is the ESC plus one byte. */
        size_t j = i + 1;
        if (j >= s.size())
            break;
        if (s[j] == '[')
        {
            j++;
            while (j < s.size() && !(static_cast<unsigned char>(s[j]) >= 0x40 && static_cast<unsigned char>(s[j]) <= 0x7E))
                j++;
            i = (j < s.size()) ? j : s.size();
            continue;
        }
        if (s[j] == ']')
        {
            j++;
            while (j < s.size() && s[j] != '\x07')
            {
                if (s[j] == 0x1B && j + 1 < s.size() && s[j + 1] == '\\')
                {
                    j++;
                    break;
                }
                j++;
            }
            i = (j < s.size()) ? j : s.size();
            continue;
        }
        i = j;
    }
    return out;
}

/* A row that may be longer than the dialog, or span several lines.
 *
 * The "by hand" command is ~120 columns and the installer's output can be anything; both used
 * to be clipped at 62, which is worse than useless -- a truncated command looks complete. So
 * long text is wrapped instead.
 *
 * Two passes, and both matter: '\n' is a hard break (the hint is written as lines on purpose),
 * and within a logical line the text wraps at spaces when one is in reach. Only a token that
 * cannot fit on a line by itself is hard-broken -- a URL has no spaces to break at -- and that
 * break backs off to a UTF-8 boundary, because installer output is not guaranteed to be ASCII
 * and splitting a multi-byte character emits bytes no terminal can render.
 *
 * Leading whitespace is dropped: every row already carries Row()'s two-space indent, so the
 * hint's own indentation would double it and push the release URL one column past the width
 * (measured: it spilled a single "9" onto its own line).
 */
void PushRow(::ftxui::Elements &rows, const std::string &raw, Tone tone = Tone::Body)
{
    const std::string s = StripEscapes(raw);
    if (s.empty())
    {
        rows.push_back(::ftxui::text(""));
        return;
    }

    size_t line_start = 0;
    while (line_start <= s.size())
    {
        const size_t nl = s.find('\n', line_start);
        const size_t line_end = (nl == std::string::npos) ? s.size() : nl;
        std::string line = s.substr(line_start, line_end - line_start);

        /* Drop the caller's own indentation; Row() supplies one, uniformly. */
        const size_t first = line.find_first_not_of(" \t");
        line = (first == std::string::npos) ? std::string() : line.substr(first);

        if (line.empty())
        {
            rows.push_back(::ftxui::text(""));
        }
        else
        {
            size_t pos = 0;
            while (pos < line.size())
            {
                size_t take = std::min(static_cast<size_t>(kTextWidth), line.size() - pos);
                if (pos + take < line.size())
                {
                    const size_t sp = line.rfind(' ', pos + take);
                    if (sp != std::string::npos && sp > pos)
                        take = sp - pos;
                    else
                    {
                        /* No space to break at: this is a URL or a long flag. Break after a
                         * separator rather than in the middle of a word, so the reader gets
                         * ".../tools/" + "install.sh" instead of ".../to" + "ols/install.sh". */
                        const size_t slash = line.rfind('/', pos + take);
                        if (slash != std::string::npos && slash > pos + take / 2)
                            take = slash + 1 - pos;
                    }
                }
                while (take > 1 && pos + take < line.size() &&
                       (static_cast<unsigned char>(line[pos + take]) & 0xC0) == 0x80)
                    take--;

                rows.push_back(Row(line.substr(pos, take), tone));
                pos += take;
                while (pos < line.size() && line[pos] == ' ')
                    pos++;
            }
        }

        if (nl == std::string::npos)
            break;
        line_start = nl + 1;
    }
}

/* The key hints at the bottom used to live here. They are gone: the buttons are labelled, the
 * only other key is Esc, and a legend under a two-button dialog explained something the dialog
 * already said. Their removal also removes a class of bug -- the legend was the one row that
 * could not wrap, so it was forever one column away from matching the separator above it. */

} // namespace

UpdateDialog::UpdateDialog(StartFn on_start, HintFn on_hint, CancelFn on_cancel)
    : on_start_(std::move(on_start)), on_hint_(std::move(on_hint)), on_cancel_(std::move(on_cancel))
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
        /* Every state closes on Esc. While RUNNING, closing also has to CANCEL: hiding the
         * dialog and letting the installer run on would leave the user believing they
         * abandoned an upgrade that is still replacing their binary. */
        if (state_ == State::RUNNING && on_cancel_)
            on_cancel_();
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
        rows.push_back(hbox({Row("EdgeClaw ", Tone::Accent), text(tag_) | bold | color(pal.accent), filler()}));
        rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
        rows.push_back(Row("A newer release is available. Updating replaces this"));
        rows.push_back(Row("binary with the published one for your platform."));
        rows.push_back(text(""));
        /* The part a user cannot undo, stated before they press anything. One colour for the
         * whole paragraph: it used to go dim after the first line, mid-sentence. */
        rows.push_back(Row("Your conversations are kept, but the database is"));
        rows.push_back(Row("upgraded on first start -- and an older version will"));
        rows.push_back(Row("refuse it afterwards. This update cannot be undone."));
        break;
    }

    case State::RUNNING:
        rows.push_back(hbox({Row("Updating to ", Tone::Accent), text(tag_) | bold | color(pal.accent)}));
        rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
        rows.push_back(Row("Downloading and installing. This can take a moment."));
        break;

    case State::DONE:
        rows.push_back(hbox({Row("Updated to ", Tone::Accent), text(tag_) | bold | color(pal.accent)}));
        rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
        rows.push_back(Row("The new binary is in place."));
        rows.push_back(text(""));
        /* Deliberately not "restarting now": an automatic restart would interrupt whatever
         * task is running, and the choice of when belongs to the user. */
        rows.push_back(Row("Quit and start EdgeClaw again to use it.") | bold);
        break;

    case State::FAILED:
    {
        rows.push_back(Row("Update failed", Tone::Accent));
        rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
        rows.push_back(Row("The update did not complete; this version is unchanged."));
        if (!detail_.empty())
        {
            rows.push_back(text(""));
            /* The installer's own output, wrapped rather than clipped: it is the only
             * diagnostic a user can paste into a report, and half a line is not a diagnostic.
             * Same colour as the rest: a quoted paragraph in a different shade read as a
             * different kind of statement, and on a page that is entirely about one failure
             * there is nothing for the distinction to say. */
            int shown = 0;
            size_t pos = 0;
            while (pos <= detail_.size() && shown < kMaxDetailLines)
            {
                const size_t nl = detail_.find('\n', pos);
                const std::string line =
                    detail_.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (!line.empty())
                {
                    PushRow(rows, line);
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
            rows.push_back(Row("To update by hand:"));
            /* Wrapped, not clipped: this is the line the user copies, and the real one is
             * twice the dialog's width. */
            PushRow(rows, on_hint_());
        }
        break;
    }

    case State::UNAVAILABLE:
    default:
    {
        rows.push_back(hbox({Row("Update ", Tone::Accent), text(tag_) | bold | color(pal.accent), filler()}));
        rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
        /* Why this copy cannot do it itself: the installer is what knows how to fetch and
         * check a release, and it only manages binaries it put there. */
        rows.push_back(Row("This copy was not installed by the EdgeClaw installer,"));
        rows.push_back(Row("so it cannot update itself."));
        if (on_hint_)
        {
            rows.push_back(text(""));
            rows.push_back(Row("To update by hand:"));
            PushRow(rows, on_hint_());
        }
        break;
    }
    }

    rows.push_back(text(""));
    rows.push_back(separator() | color(pal.line_dim) | bgcolor(pal.dialog_bg));
    rows.push_back(ButtonsElement());

    /* Borderless, matching the /connect dialog (edc_setup_dialog.cpp).
     *
     * This is the house style, not an oversight in either file: the interface has no boxes
     * anywhere else, so a framed modal reads as a different program laid over it. The
     * structure that a border would normally provide is carried by the header row, the
     * separators and the right-aligned hint instead -- which is exactly what the setup
     * dialog does, and why the two now look like one surface.
     *
     * BorderStyle::EMPTY still reserves the one column a border would have used, so the
     * title needs the same two-space lead-in as the setup dialog's ("  " + title) to sit
     * where the eye expects it. */
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, kDialogWidth) | borderStyled(BorderStyle::EMPTY) |
           bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
