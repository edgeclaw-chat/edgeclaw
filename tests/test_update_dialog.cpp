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

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdio>
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

std::string Frame(edc::ui::UpdateDialog &dialog, int w = 70, int h = 22)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(w), ::ftxui::Dimension::Fixed(h));
    ::ftxui::Render(screen, dialog.Render());
    return Plain(screen.ToString());
}

bool Has(const std::string &frame, const std::string &needle)
{
    return frame.find(needle) != std::string::npos;
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

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
