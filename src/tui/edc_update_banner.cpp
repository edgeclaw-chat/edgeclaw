#include "edc_update_banner.hpp"

#include "edc_style.hpp"

#include <string>

namespace edc::ui
{

namespace
{

::ftxui::Element Dim(const char *s)
{
    return ::ftxui::text(s) | ::ftxui::color(style::palette().text_dim);
}

::ftxui::Element Tag(const std::string &tag)
{
    return ::ftxui::text(tag) | ::ftxui::bold | ::ftxui::color(style::palette().accent);
}

} // namespace

UpdateCommand UpdateCommandFor(UpdateBanner state)
{
    switch (state)
    {
    case UpdateBanner::Available:
        return UpdateCommand::OpenDialog;
    case UpdateBanner::Failed:
        /* A failure is retryable, and the dialog is where a retry starts -- it is also how the
         * user sees the installer's output again. */
        return UpdateCommand::OpenDialog;
    case UpdateBanner::Running:
        /* Nothing to retry while it runs, and nothing to cancel from here: the install
         * finishes on its own or hits its own deadline. */
        return UpdateCommand::AlreadyRunning;
    case UpdateBanner::Done:
        /* Terminal for this process: the new binary cannot take over a running one. */
        return UpdateCommand::RestartNeeded;
    case UpdateBanner::None:
    default:
        return UpdateCommand::NoUpdate;
    }
}

const char *UpdateCommandHint(UpdateCommand command)
{
    switch (command)
    {
    case UpdateCommand::AlreadyRunning:
        return " An update is already running ";
    case UpdateCommand::RestartNeeded:
        return " Restart EdgeClaw to use the new version ";
    case UpdateCommand::NoUpdate:
        return " No update available ";
    case UpdateCommand::OpenDialog:
    default:
        return "";
    }
}

::ftxui::Element UpdateBannerElement(UpdateBanner state, const std::string &tag, const std::string &detail)
{
    using ::ftxui::text;

    switch (state)
    {
    case UpdateBanner::Running:
        return ::ftxui::hbox({
            text("  EdgeClaw ") | ::ftxui::color(style::palette().accent),
            Tag(tag),
            Dim(" is being installed  ·  this runs in the background "),
            ::ftxui::filler(),
        });

    case UpdateBanner::Done:
        /* Deliberately not "restarting": an automatic restart would interrupt whatever task is
         * running, and the moment belongs to the user. */
        return ::ftxui::hbox({
            text("  EdgeClaw ") | ::ftxui::color(style::palette().accent),
            Tag(tag),
            Dim(" is installed  ·  restart EdgeClaw to use it "),
            ::ftxui::filler(),
        });

    case UpdateBanner::Failed:
        /* The installer's own last words: it is the only diagnosis there is, and "/update to
         * retry" is the only action. */
        return ::ftxui::hbox({
            text("  EdgeClaw ") | ::ftxui::color(style::palette().accent),
            Tag(tag),
            Dim(" did not install  ·  /update to retry "),
            detail.empty() ? text("") : (text(" · " + detail) | ::ftxui::color(style::palette().text_dim)),
            ::ftxui::filler(),
        });

    case UpdateBanner::None:
        /* No banner at all. Not the "available" line with an empty tag: that would render
         * "EdgeClaw  is available · /update to install" while /update answers "no update
         * available" -- the two sides disagreeing, which is the failure mode this file exists
         * to catch (it caught exactly that). */
        return ::ftxui::text("");

    case UpdateBanner::Available:
    default:
        return ::ftxui::hbox({
            text("  EdgeClaw ") | ::ftxui::color(style::palette().accent),
            Tag(tag),
            Dim(" is available  ·  /update to install "),
            ::ftxui::filler(),
        });
    }
}

} // namespace edc::ui
