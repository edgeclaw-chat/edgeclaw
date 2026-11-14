/* test_update_dialog: the update modal, driven through its public surface.
 *
 * Why this file exists: the dialog is the only place EdgeClaw tells a user that an update
 * costs them the ability to go back, and it had no tests at all. It owns no process -- it is
 * handed a "start this tag" callback and told the result -- so the state machine can be
 * driven completely, with no server, no installer and no child process.
 *
 * Cases are chosen to pin DECISIONS rather than to reach lines:
 *
 *   - the version the user is being offered appears on the confirmation page, and so does the
 *     part they cannot undo ("cannot be undone"): a dialog that omitted it would be a trap;
 *   - Enter starts the upgrade ONLY on the primary button. The default cursor sits on Update,
 *     so Enter-on-open is the dangerous path and is what the button order has to make safe;
 *   - Esc on the confirmation page closes without starting anything;
 *   - Esc while RUNNING tells the host to CANCEL. This is the bug the limitations file found:
 *     the cancel callback existed and was wired to nothing, so Esc hid the dialog while the
 *     installer kept replacing the binary;
 *   - a result that arrives when we are not RUNNING is ignored, so a late callback from an
 *     abandoned upgrade cannot overwrite a newer dialog;
 *   - RUNNING has no buttons, so no key other than Esc does anything;
 *   - a copy the installer did not place shows the manual command instead of a button that
 *     would fail.
 */

#include "edc_update_dialog.hpp"
#include "edc_style.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: %s\n", msg);                                                                    \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: %s\n", msg);                                                                               \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{

/* Strips the SGR escapes Screen::ToString interleaves with cells, so assertions are about
 * what a reader sees rather than about colour codes. */
std::string Plain(const std::string &frame)
{
    std::string out;
    bool in_escape = false;
    for (char c : frame)
    {
        if (in_escape)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                in_escape = false;
            continue;
        }
        if (c == '\x1b')
        {
            in_escape = true;
            continue;
        }
        out += c;
    }
    return out;
}

std::string Frame(edc::ui::UpdateDialog &dialog, int w = 78, int h = 22)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w), ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, dialog.Render());
    return Plain(screen.ToString());
}

/* A rendered frame kept as cells, for the two things a stripped string cannot answer: what
 * colour a row actually is, and which column a row actually ends at. */
struct Cells
{
    int width = 0;
    int height = 0;
    std::vector<std::string> chars; /* one per cell */
    std::vector<::ftxui::Color> fg;

    std::string RowText(int y) const
    {
        std::string s;
        for (int x = 0; x < width; x++)
            s += chars[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        return s;
    }

    /* Last column holding a non-space character, or -1 for a blank row. */
    int LastInkColumn(int y) const
    {
        for (int x = width - 1; x >= 0; x--)
            if (chars[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] != " ")
                return x;
        return -1;
    }

    /* Row index whose text contains |needle|, or -1. */
    int FindRow(const std::string &needle) const
    {
        for (int y = 0; y < height; y++)
            if (RowText(y).find(needle) != std::string::npos)
                return y;
        return -1;
    }

    ::ftxui::Color FgAt(int x, int y) const
    {
        return fg[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }

    /* Foreground colour of the first cell of the first row containing |needle|. Assumes the
     * needle is unique in the frame, which every caller below arranges. */
    ::ftxui::Color InkOf(const std::string &needle) const
    {
        const int y = FindRow(needle);
        if (y < 0)
            return ::ftxui::Color::Default;
        const std::string row = RowText(y);
        const size_t at = row.find(needle);
        return FgAt(static_cast<int>(at), y);
    }
};

Cells Capture(edc::ui::UpdateDialog &dialog, int w = 78, int h = 24)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w), ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, dialog.Render());

    Cells c;
    c.width = w;
    c.height = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const auto &cell = screen.CellAt(x, y);
            c.chars.push_back(cell.character);
            c.fg.push_back(cell.foreground_color);
        }
    return c;
}

bool Has(const std::string &frame, const std::string &needle)
{
    return frame.find(needle) != std::string::npos;
}

/* The frame with every space removed: used to prove that a wrapped line lost nothing. A
 * truncated line and a wrapped one look equally short in a per-row search. */
std::string Squashed(const std::string &frame)
{
    std::string out;
    for (char c : frame)
        if (c != ' ' && c != '\n' && c != '\r')
            out += c;
    return out;
}

/* The first row that has any non-space character.
 *
 * Used to pin "there is no border on top": a framed dialog's first non-blank row IS the
 * frame's top edge, so reading it is the direct way to assert the absence of one, without
 * depending on which glyph a particular BorderStyle happens to draw. */
std::string FirstNonBlankLine(const std::string &frame)
{
    size_t pos = 0;
    while (pos <= frame.size())
    {
        const size_t nl = frame.find('\n', pos);
        const std::string line = frame.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find_first_not_of(" \t\r") != std::string::npos)
            return line;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return std::string();
}

/* The hooks the dialog is built with, and what they were asked to do. */
struct Hooks
{
    int started = 0;
    int cancelled = 0;
    std::string started_tag;
    std::string hint = "curl -fsSL https://example/install.sh | sh -s -- --tag v9.9.9";
};

Hooks *g_hooks = nullptr;

edc::ui::UpdateDialog MakeDialog(Hooks &hooks)
{
    g_hooks = &hooks;
    return edc::ui::UpdateDialog(
        [&hooks](const std::string &tag) {
            hooks.started++;
            hooks.started_tag = tag;
        },
        [&hooks]() { return hooks.hint; },
        [&hooks]() { hooks.cancelled++; });
}

} // namespace

int main()
{
    printf("\n── opening ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        CHECK(!dialog.IsOpen(), "a fresh dialog is closed");

        dialog.Open("v1.0.1", /*can_upgrade=*/true);
        CHECK(dialog.IsOpen(), "Open opens it");
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::CONFIRM, "at the confirmation page");

        const std::string frame = Frame(dialog);
        CHECK(Has(frame, "v1.0.1"), "the offered version is named");
        CHECK(Has(frame, "Update"), "the primary button is drawn");
        CHECK(Has(frame, "Cancel"), "the cancel button is drawn");
        /* The part a user cannot undo, stated before they can press anything. */
        CHECK(Has(frame, "cannot be undone") || Has(frame, "Cannot be undone"),
              "the confirmation says the update cannot be undone");
    }

    printf("\n── the primary button is what Enter presses ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);

        /* Enter on open must START: the default cursor is on Update. If it were on Cancel,
         * the safe-looking default would be the one that does nothing. */
        CHECK(dialog.OnEvent(::ftxui::Event::Return), "Enter is consumed");
        CHECK(hooks.started == 1, "Enter starts the upgrade");
        CHECK(hooks.started_tag == "v1.0.1", "and hands over the offered tag");
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::RUNNING, "the dialog is now running");
        CHECK(Has(Frame(dialog), "working"), "the running page says it is working");
    }

    printf("\n── moving to Cancel ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);

        CHECK(dialog.OnEvent(::ftxui::Event::ArrowRight), "Right moves the cursor");
        CHECK(dialog.OnEvent(::ftxui::Event::Return), "Enter is consumed");
        CHECK(hooks.started == 0, "Enter on Cancel starts nothing");
        CHECK(!dialog.IsOpen(), "and the dialog closes");
    }

    printf("\n── Tab toggles between the two buttons ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Tab); /* -> Cancel */
        dialog.OnEvent(::ftxui::Event::Return);
        CHECK(hooks.started == 0, "Tab then Enter cancels");
        CHECK(!dialog.IsOpen(), "and closes");
    }

    printf("\n── Esc on the confirmation page ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        CHECK(dialog.OnEvent(::ftxui::Event::Escape), "Esc is consumed");
        CHECK(hooks.started == 0, "Esc starts nothing");
        CHECK(hooks.cancelled == 0, "and there is nothing to cancel yet");
        CHECK(!dialog.IsOpen(), "the dialog closes");
    }

    printf("\n── Esc while running CANCELS ──\n");
    {
        /* The regression this pins: a cancel hook that exists but is called from nowhere
         * means Esc hides the dialog while the installer keeps replacing the binary. */
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Return); /* -> RUNNING */

        CHECK(dialog.OnEvent(::ftxui::Event::Escape), "Esc is consumed");
        CHECK(hooks.cancelled == 1, "Esc while running tells the host to cancel");
        CHECK(!dialog.IsOpen(), "and closes the dialog");
    }

    printf("\n── nothing but Esc does anything while running ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Return); /* -> RUNNING */

        CHECK(!dialog.OnEvent(::ftxui::Event::Return), "Enter is not consumed while running");
        CHECK(!dialog.OnEvent(::ftxui::Event::Character(' ')), "Space does nothing while running");
        CHECK(hooks.started == 1, "no second upgrade was started");
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::RUNNING, "still running");
        CHECK(Has(Frame(dialog), "Downloading"), "the running page mentions downloading");
    }

    printf("\n── the result ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Return);

        dialog.OnResult(true, "");
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::DONE, "a success moves to DONE");
        const std::string frame = Frame(dialog);
        CHECK(Has(frame, "Updated to"), "the done page says it is updated");
        CHECK(Has(frame, "restart") || Has(frame, "Quit and start"),
              "and tells the user to restart rather than restarting itself");
        CHECK(!Has(frame, "Update "), "no button offers to run it again");

        CHECK(dialog.OnEvent(::ftxui::Event::Return), "Enter closes the done page");
        CHECK(!dialog.IsOpen(), "and it is closed");
    }
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Return);

        dialog.OnResult(false, "the installer exited with status 3\npermission denied");
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::FAILED, "a failure moves to FAILED");
        const std::string frame = Frame(dialog);
        CHECK(Has(frame, "did not complete"), "the failure page says the update did not happen");
        CHECK(Has(frame, "status 3"), "the installer's own output is shown");
        CHECK(Has(frame, "permission denied"), "every line of it, not just the first");
        CHECK(Has(frame, "install.sh"), "the manual command is offered");
    }

    printf("\n── a late result is ignored ──\n");
    {
        /* An abandoned upgrade can still call back (the process is killed, but the callback
         * may already be queued). It must not overwrite a dialog the user has since reopened. */
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", true);
        dialog.OnEvent(::ftxui::Event::Escape); /* closed from CONFIRM, still CONFIRM state */
        CHECK(!dialog.IsOpen(), "closed");

        dialog.OnResult(true, "");
        CHECK(dialog.state() != edc::ui::UpdateDialog::State::DONE, "a result outside RUNNING does not move to DONE");

        dialog.Open("v1.0.2", true);
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::CONFIRM, "reopening starts at the confirmation page");
        CHECK(Has(Frame(dialog), "v1.0.2"), "and shows the new version");
    }

    printf("\n── a copy that cannot upgrade itself ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", /*can_upgrade=*/false);
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::UNAVAILABLE,
              "no usable installer means the unavailable page, decided before asking anything");

        const std::string frame = Frame(dialog);
        CHECK(Has(frame, "cannot update itself"), "it says this copy cannot update itself");
        CHECK(Has(frame, "install.sh"), "and gives the command to run by hand");
        CHECK(!Has(frame, "Cancel"), "there is no button that would fail");

        /* Enter must not start an upgrade from a page that has no Update button. */
        dialog.OnEvent(::ftxui::Event::Return);
        CHECK(hooks.started == 0, "Enter on the unavailable page starts nothing");
        CHECK(!dialog.IsOpen(), "Enter dismisses it");
    }

    printf("\n── events when closed ──\n");
    {
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        CHECK(!dialog.OnEvent(::ftxui::Event::Return), "a closed dialog consumes nothing");
        CHECK(!dialog.OnEvent(::ftxui::Event::Escape), "not even Esc");
    }

    printf("\n── body prose is one colour ──\n");
    {
        /* The bug: some rows had no colour (terminal default) and others were pal.text_dim, and
         * the two met INSIDE a paragraph -- "Your conversations are kept…" started bright and
         * turned grey mid-sentence. A row that says nothing about its colour is a row that will
         * disagree with its neighbours, so this asserts the whole paragraph against pal.text. */
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", /*can_upgrade=*/true);
        const Cells c = Capture(dialog);

        const auto &pal = edc::style::palette();
        const char *body[] = {"A newer release is available", "binary with the published one",
                              "Your conversations are kept", "upgraded on first start",
                              "refuse it afterwards"};
        for (const char *needle : body)
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "\"%s\" is pal.text, not the terminal default", needle);
            CHECK(c.InkOf(needle) == pal.text, msg);
        }
        /* All five agree with each other, which is the property the user actually sees. */
        CHECK(c.InkOf(body[0]) == c.InkOf(body[3]), "the paragraph does not change colour halfway through");
    }

    printf("\n── the key hint is flush with the right edge ──\n");
    {
        /* It used to end in a space, which after right-alignment left the last character one
         * column short of the separator above it -- a hairline that reads as "this line does
         * not belong to the dialog".
         *
         * Measured in columns of TEXT, not cells: the separator is a wide (3-byte) character and
         * the cells past the dialog are blank padding, so counting cells answers a different
         * question than "do these two rows end together". */
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", /*can_upgrade=*/true);

        /* Exactly as wide as the dialog, so there is no padding to confuse the arithmetic. */
        const std::string frame = Frame(dialog, 66, 20);
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char ch : frame)
            {
                if (ch == '\n')
                {
                    lines.push_back(cur);
                    cur.clear();
                }
                else if (ch != '\r')
                {
                    cur += ch;
                }
            }
            lines.push_back(cur);
        }

        auto columns = [](const std::string &s) {
            /* Count UTF-8 code points: every glyph ftxui draws here occupies one column. */
            size_t n = 0;
            for (size_t i = 0; i < s.size();)
            {
                const unsigned char b = static_cast<unsigned char>(s[i]);
                i += (b < 0x80) ? 1 : ((b >> 5) == 0x6) ? 2 : ((b >> 4) == 0xE) ? 3 : 4;
                n++;
            }
            return n;
        };

        int hint_end = -1;
        int rule_end = -1;
        for (const std::string &line : lines)
        {
            const size_t at = line.find("Esc cancel");
            if (at != std::string::npos)
                hint_end = static_cast<int>(columns(line.substr(0, at)) + strlen("Esc cancel"));

            if (line.find("\xE2\x94\x80") != std::string::npos) /* first rule row: the header's */
            {
                /* Trim the row's own right padding first: the ink ends where the last
                 * non-space character is, and the line is padded to the dialog's width. */
                size_t end = line.find_last_not_of(' ');
                end = (end == std::string::npos) ? 0 : end + 1;
                rule_end = hint_end >= 0 ? rule_end : static_cast<int>(columns(line.substr(0, end)));
            }
        }
        CHECK(hint_end > 0, "the key hint renders");
        CHECK(rule_end > 0, "the separator renders");

        printf("      hint ends at column %d, separator at %d\n", hint_end, rule_end);
        CHECK(hint_end == rule_end, "the hint ends on the same column as the separator above it");
    }

    printf("\n── a long command wraps instead of being cut off ──\n");
    {
        /* `edc_upgrade_manual_hint` returns lines ~120 columns wide against a ~64-column
         * dialog. Clipping them is the worst option: a truncated command still looks complete,
         * so a user copies half of it and cannot tell. Squash the frame (drop every space) and
         * the whole command must still be in there. */
        Hooks hooks;
        /* The real shape: an intro line, a release URL (61 columns), a lead-in and the
         * command. */
        hooks.hint =
            "download v9.9.9 from\n"
            "  https://github.com/edgeclaw-chat/edgeclaw/releases/tag/v9.9.9\n"
            "or run:\n"
            "  curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/v9.9.9/tools/install.sh "
            "| sh -s -- --tag v9.9.9";

        auto dialog = MakeDialog(hooks);
        dialog.Open("v9.9.9", /*can_upgrade=*/false);
        CHECK(dialog.state() == edc::ui::UpdateDialog::State::UNAVAILABLE, "the unavailable page");

        const std::string frame = Frame(dialog);
        const std::string squashed = Squashed(frame);

        CHECK(squashed.find(Squashed("https://github.com/edgeclaw-chat/edgeclaw/releases/tag/v9.9.9")) !=
                  std::string::npos,
              "the release URL survives whole");
        CHECK(squashed.find(Squashed("https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/v9.9.9/tools/"
                                     "install.sh")) != std::string::npos,
              "and so does the long URL inside the command");
        CHECK(squashed.find(Squashed("| sh -s -- --tag v9.9.9")) != std::string::npos,
              "including its tail -- the part a clipped line loses first");

        /* The hint's own leading indent is dropped rather than added to the row's: rows already
         * indent, and doubling it pushed the URL past the width (it spilled a lone "9"). The
         * measurable form of that is "the URL starts in the same column as every other row".
         * Compared against another row, because an absolute column count depends on the screen
         * the frame was rendered into. */
        const Cells c = Capture(dialog);
        const int body_row = c.FindRow("This copy was not installed");
        const int url_row = c.FindRow("https://github.com/edgeclaw-chat/edgeclaw/releases");
        CHECK(body_row >= 0 && url_row >= 0, "both rows render");
        if (body_row >= 0 && url_row >= 0)
        {
            const int body_col = static_cast<int>(c.RowText(body_row).find_first_not_of(" "));
            const int url_col = static_cast<int>(c.RowText(url_row).find_first_not_of(" "));
            printf("      body starts at %d, url at %d\n", body_col, url_col);
            CHECK(url_col == body_col, "the URL sits at the same indent as the prose around it");
        }
    }

    printf("\n── borderless, like every other surface ──\n");
    {
        /* The house style: this interface draws no boxes, and a framed modal reads as a
         * different program laid over it. /connect (edc_setup_dialog.cpp) is the reference;
         * this dialog used BorderStyle::ROUNDED and was the odd one out.
         *
         * Asserted two ways, because either alone can be satisfied by accident:
         *   - no frame glyphs anywhere (the corners are unique to a drawn border; the
         *     horizontal '-' separators inside are legitimate and are NOT asserted against);
         *   - the first non-blank row is the title, not a top edge.
         * A future change back to any bordered style fails the first; a style that draws
         * only some edges fails the second. */
        Hooks hooks;
        auto dialog = MakeDialog(hooks);
        dialog.Open("v1.0.1", /*can_upgrade=*/true);
        const std::string frame = Frame(dialog);

        CHECK(!Has(frame, "╭") && !Has(frame, "╮") && !Has(frame, "╰") && !Has(frame, "╯"),
              "no rounded corners are drawn");
        CHECK(!Has(frame, "┌") && !Has(frame, "┐") && !Has(frame, "└") && !Has(frame, "┘"),
              "no square corners either");
        CHECK(!Has(frame, "│"), "no vertical edges");

        const std::string first = FirstNonBlankLine(frame);
        CHECK(Has(first, "EdgeClaw v1.0.1"), "the first visible row is the title, not a border");
        CHECK(!Has(first, "─"), "and that row carries no rule glyph");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
