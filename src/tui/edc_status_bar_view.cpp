#include "edc_status_bar_view.hpp"

#include "edc_style.hpp"

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace edc::ui
{

namespace
{

// The only hint the footer shows: instead of spelling out every key binding in
// the status line, point users at the slash command that lists them. Placed at
// the right end of the footer, after the model name.
constexpr char kFooterHelp[] = "/help";

// Replace a leading $HOME with "~" so a deep directory stays readable in the
// one-line footer. Falls back to the raw path when HOME is unset or the path is
// not under it.
std::string ShortenHome(const char *path)
{
    if (!path || !*path)
        return "";
    const char *home = std::getenv("HOME");
    if (home && *home)
    {
        const size_t n = std::strlen(home);
        if (std::strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
            return std::string("~") + (path + n);
    }
    return path;
}

} // namespace

StatusBarView::StatusBarView(std::shared_ptr<const model::SessionModel> session,
                             std::shared_ptr<const port::ITerminalInfo> terminal,
                             std::function<bool()> sandbox_enabled, std::function<int()> sidebar_width)
    : session_(std::move(session)), terminal_(std::move(terminal)), sandbox_enabled_(std::move(sandbox_enabled)),
      sidebar_width_(std::move(sidebar_width))
{
}

void StatusBarView::SetModel(std::string model)
{
    model_ = std::move(model);
}

std::string StatusBarView::RightText(int pane_width, const std::string &left, const std::string &model, bool sandbox_on)
{
    // Right end: the model in use, the sandbox state and the "/help" pointer,
    // joined by middle dots so they read as one compact unit -- model first, then
    // the sandbox state, then "/help". Flush to the pane's right edge with the
    // filler; when the pane is narrow drop fields from the middle outward: first
    // the model, then the sandbox state, leaving "/help" alone.
    const std::string sandbox = std::string("sandbox:") + (sandbox_on ? "on" : "off");
    const int budget = pane_width - ::ftxui::string_width(left) - 1;

    std::string right = kFooterHelp;
    const std::string sandbox_help = sandbox + " · " + kFooterHelp;
    if (::ftxui::string_width(sandbox_help) <= budget)
        right = sandbox_help;
    if (!model.empty())
    {
        const std::string full = model + " · " + sandbox + " · " + kFooterHelp;
        if (::ftxui::string_width(full) <= budget)
            right = full;
    }
    return right;
}

std::string StatusBarView::CurrentDir() const
{
    /* The active session's bound working directory (what the model is told via
     * {{cwd}} and where tools run), cached from the last applied snapshot so the
     * footer never reads the store; fall back to the process launch directory
     * when the session is unbound, or before the first snapshot lands. */
    const std::string &bound = session_->workdir();
    if (!bound.empty())
        return ShortenHome(bound.c_str());
    char cwd_buf[4096];
    return ::getcwd(cwd_buf, sizeof(cwd_buf)) ? ShortenHome(cwd_buf) : std::string();
}

::ftxui::Element StatusBarView::Draw(const Frame &)
{
    const auto &pal = style::palette();

    const std::string left = " " + CurrentDir();

    // Columns available to the footer: the chat pane, i.e. the terminal minus the
    // sidebar and the two-cell split separator. The width comes from the injected
    // terminal port (the real tty by default, a fixed one in tests).
    const int pane_w = terminal_->width() - sidebar_width_() - 2;
    const bool sandbox_on = sandbox_enabled_();

    const std::string right = RightText(pane_w, left, model_, sandbox_on);

    return ::ftxui::hbox({
        ::ftxui::text(left) | ::ftxui::color(pal.text_dim),
        ::ftxui::filler(),
        ::ftxui::text(right + " ") | ::ftxui::color(pal.text_dim),
    });
}

} // namespace edc::ui
