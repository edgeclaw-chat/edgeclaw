#include "edc_status_bar_view.hpp"

#include "edc_style.hpp"

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

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

// Compact token count, matching the sidebar's own formatter (edc_task_widget.cpp):
// 942 -> "942", 12345 -> "12.3k", 1250000 -> "1.2M", 2000 -> "2k" (the ".0" is trimmed).
std::string CompactTokens(long long n)
{
    if (n <= 0)
        return "";
    if (n < 1000)
        return std::to_string(n);

    const double v = n < 1000000 ? static_cast<double>(n) / 1000.0 : static_cast<double>(n) / 1000000.0;
    const char *suffix = n < 1000000 ? "k" : "M";

    char buf[32];
    if (v < 100.0)
        std::snprintf(buf, sizeof(buf), "%.1f%s", v, suffix);
    else
        std::snprintf(buf, sizeof(buf), "%.0f%s", v, suffix);

    std::string s(buf);
    const std::string trailing = std::string(".0") + suffix;
    if (s.size() > trailing.size() && s.compare(s.size() - trailing.size(), trailing.size(), trailing) == 0)
        s.erase(s.size() - trailing.size(), 2); // drop the ".0", keep the suffix
    return s;
}

} // namespace

StatusBarView::StatusBarView(std::shared_ptr<const model::SessionModel> session,
                             std::shared_ptr<const port::ITerminalInfo> terminal,
                             std::function<bool()> sandbox_enabled, std::function<bool()> shell_syntax,
                             std::function<int()> sidebar_width, std::function<int()> context_window)
    : session_(std::move(session)), terminal_(std::move(terminal)), sandbox_enabled_(std::move(sandbox_enabled)),
      shell_syntax_(std::move(shell_syntax)), sidebar_width_(std::move(sidebar_width)),
      context_window_(std::move(context_window))
{
}

void StatusBarView::SetModel(std::string model)
{
    model_ = std::move(model);
}

void StatusBarView::SetContextUsage(long long tokens, bool estimated)
{
    ctx_tokens_ = tokens;
    ctx_estimated_ = estimated;
}

void StatusBarView::SetHint(std::string hint)
{
    hint_ = std::move(hint);
}

std::string StatusBarView::LeftText(const std::string &dir, const std::string &hint)
{
    // While a hint is up it REPLACES the working directory (same left corner, no extra
    // run), so the footer keeps exactly the same shape.
    if (!hint.empty())
        return " " + hint;
    return " " + dir;
}

std::string StatusBarView::ContextText(long long used, bool estimated, int window)
{
    if (used <= 0)
        return "";
    std::string s = std::string("ctx ") + (estimated ? "~" : "") + CompactTokens(used);
    if (window > 0)
        s += "/" + CompactTokens(window);
    return s;
}

std::string StatusBarView::RightText(int pane_width, const std::string &left, const std::string &model,
                                     const std::string &ctx, bool sandbox_on, bool shell_on)
{
    // Right end: the model in use, the current context size, and the two mode switches,
    // then the "/help" pointer, joined by middle dots so they read as one compact unit.
    // Flush to the pane's right edge with the filler; as the pane narrows, drop fields
    // from the front outward -- the model first, then ctx, then sandbox, then shell --
    // and never drop "/help".
    const std::string sandbox = std::string("sandbox:") + (sandbox_on ? "on" : "off");
    const std::string shell = std::string("shell:") + (shell_on ? "on" : "off");

    std::vector<std::string> fields;
    if (!model.empty())
        fields.push_back(model);
    if (!ctx.empty())
        fields.push_back(ctx);
    fields.push_back(sandbox);
    fields.push_back(shell);
    fields.push_back(kFooterHelp);

    const int budget = pane_width - ::ftxui::string_width(left) - 1;
    for (size_t first = 0; first < fields.size(); first++)
    {
        std::string cand;
        for (size_t i = first; i < fields.size(); i++)
        {
            if (!cand.empty())
                cand += " · ";
            cand += fields[i];
        }
        if (::ftxui::string_width(cand) <= budget || first + 1 == fields.size())
            return cand;
    }
    return kFooterHelp; // unreachable: the loop returns on its last iteration
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

    const std::string dir = CurrentDir();
    const std::string left = LeftText(dir, hint_);

    // Columns available to the footer: the chat pane, i.e. the terminal minus the
    // sidebar and the two-cell split separator. The width comes from the injected
    // terminal port (the real tty by default, a fixed one in tests).
    const int pane_w = terminal_->width() - sidebar_width_() - 2;
    const bool sandbox_on = sandbox_enabled_();
    const std::string ctx = ContextText(ctx_tokens_, ctx_estimated_, context_window_ ? context_window_() : 0);

    const std::string right = RightText(pane_w, left, model_, ctx, sandbox_on, shell_syntax_());

    // The left end is one dim run: the working directory -- or, while a hint is up, the
    // hint in its place (same style: no highlight, same corner).
    return ::ftxui::hbox({
        ::ftxui::text(left) | ::ftxui::color(pal.text_dim),
        ::ftxui::filler(),
        ::ftxui::text(right + " ") | ::ftxui::color(pal.text_dim),
    });
}

} // namespace edc::ui
