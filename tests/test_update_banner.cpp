/* tests/test_update_banner.cpp -- the update banner's four states, and what /update means in
 * each.
 *
 * Why a separate file: the install runs in the background and the dialog closes the moment it
 * is confirmed, so this one line above the chat is the entire report of progress and outcome.
 * Reaching those states for real needs an installed copy and a real release, so the state is
 * an argument here and the wording is asserted directly.
 *
 * Each state is asserted on BOTH sides: what the banner says, and what /update does. An
 * inconsistency between the two is the failure mode that matters -- a banner offering
 * "/update to install" while /update answers "no update available" is exactly the bug this
 * file exists to prevent, and it is invisible if only one side is checked.
 */

#include "edc_update_banner.hpp"

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include <stdio.h>
#include <string.h>

#include <string>

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

/* Render one banner into a fixed screen and return the plain text. Escapes are stripped (the
 * same way test_update_dialog does it) so assertions are about what a reader sees. */
std::string Frame(edc::ui::UpdateBanner state, const std::string &tag, const std::string &detail)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(100), ::ftxui::Dimension::Fixed(3));
    ::ftxui::Render(screen, edc::ui::UpdateBannerElement(state, tag, detail));
    const std::string raw = screen.ToString();

    std::string out;
    bool in_escape = false;
    for (char c : raw)
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

bool Has(const std::string &frame, const char *needle) { return frame.find(needle) != std::string::npos; }

} // namespace

int main(void)
{
    printf("\ntest_update_banner\n");

    printf("\n── available: points at the command that installs it ──\n");
    {
        const std::string f = Frame(edc::ui::UpdateBanner::Available, "v9.9.9", "");
        CHECK(Has(f, "v9.9.9"), "the version is named");
        CHECK(Has(f, "is available"), "it says a release is available");
        CHECK(Has(f, "/update to install"), "and names the command that installs it");
        CHECK(!Has(f, "restart"), "it does not mention a restart yet -- nothing is installed");

        CHECK(edc::ui::UpdateCommandFor(edc::ui::UpdateBanner::Available) == edc::ui::UpdateCommand::OpenDialog,
              "/update opens the consent dialog");
        CHECK(edc::ui::UpdateCommandHint(edc::ui::UpdateCommand::OpenDialog)[0] == '\0',
              "and has no hint: the dialog is the answer");
    }

    printf("\n── running: says it is under way, and offers nothing ──\n");
    {
        const std::string f = Frame(edc::ui::UpdateBanner::Running, "v9.9.9", "");
        CHECK(Has(f, "v9.9.9"), "the version is named");
        CHECK(Has(f, "is being installed"), "it says the install is under way");
        CHECK(Has(f, "background"), "and that it does not block the interface");
        CHECK(!Has(f, "/update to install"), "it stops inviting a second /update");

        CHECK(edc::ui::UpdateCommandFor(edc::ui::UpdateBanner::Running) == edc::ui::UpdateCommand::AlreadyRunning,
              "/update reports that one is already running");
        CHECK(strstr(edc::ui::UpdateCommandHint(edc::ui::UpdateCommand::AlreadyRunning), "already running") != NULL,
              "and says so");
    }

    printf("\n── done: the only remaining step is the user's ──\n");
    {
        const std::string f = Frame(edc::ui::UpdateBanner::Done, "v9.9.9", "");
        CHECK(Has(f, "v9.9.9"), "the version is named");
        CHECK(Has(f, "is installed"), "it says the binary was replaced");
        CHECK(Has(f, "restart EdgeClaw to use it"), "and that a restart is what remains");
        CHECK(!Has(f, "/update to install"), "it no longer offers to install");
        CHECK(!Has(f, "restarting"), "it does not promise to restart by itself: that would "
                                     "interrupt whatever task is running");

        CHECK(edc::ui::UpdateCommandFor(edc::ui::UpdateBanner::Done) == edc::ui::UpdateCommand::RestartNeeded,
              "/update reports that a restart is needed");
    }

    printf("\n── failed: retryable, with the installer's own words ──\n");
    {
        const std::string f = Frame(edc::ui::UpdateBanner::Failed, "v9.9.9", "checksum mismatch");
        CHECK(Has(f, "v9.9.9"), "the version is named");
        CHECK(Has(f, "did not install"), "it says the install did not happen");
        CHECK(Has(f, "/update to retry"), "and that /update retries");
        CHECK(Has(f, "checksum mismatch"), "it carries the installer's own message -- the only diagnosis there is");

        /* No detail: still a coherent line, not a stray separator. */
        const std::string bare = Frame(edc::ui::UpdateBanner::Failed, "v9.9.9", "");
        CHECK(Has(bare, "did not install"), "without a detail it still says what happened");
        CHECK(!Has(bare, "·  ·"), "and does not leave an empty separator behind");

        CHECK(edc::ui::UpdateCommandFor(edc::ui::UpdateBanner::Failed) == edc::ui::UpdateCommand::OpenDialog,
              "/update reopens the dialog to retry");
    }

    printf("\n── none: no banner, and /update says there is nothing ──\n");
    {
        CHECK(edc::ui::UpdateCommandFor(edc::ui::UpdateBanner::None) == edc::ui::UpdateCommand::NoUpdate,
              "/update says there is no update");
        CHECK(strstr(edc::ui::UpdateCommandHint(edc::ui::UpdateCommand::NoUpdate), "No update available") != NULL,
              "with the wording the toast has always used");
    }

    printf("\n── the pair never disagrees ──\n");
    {
        /* The property worth stating once: a state whose banner offers an action must not be
         * a state where /update refuses, and vice versa. Each case above checks one state; this
         * checks the rule. */
        const edc::ui::UpdateBanner all[] = {edc::ui::UpdateBanner::None, edc::ui::UpdateBanner::Available,
                                             edc::ui::UpdateBanner::Running, edc::ui::UpdateBanner::Done,
                                             edc::ui::UpdateBanner::Failed};
        for (edc::ui::UpdateBanner s : all)
        {
            const edc::ui::UpdateCommand cmd = edc::ui::UpdateCommandFor(s);
            const std::string f = Frame(s, "v9.9.9", "detail");
            const bool banner_offers_install = Has(f, "/update to install") || Has(f, "/update to retry");
            const bool command_opens = (cmd == edc::ui::UpdateCommand::OpenDialog);

            char msg[160];
            snprintf(msg, sizeof(msg), "state %d: banner offers install <=> /update opens (%d)", (int)s,
                     (int)command_opens);
            CHECK(banner_offers_install == command_opens, msg);
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
