#include "edc_command_palette.hpp"

#include "edc_style.hpp"

#include <utility>

namespace edc::model
{

namespace
{

// Rows whose command starts with |prefix|.
std::vector<CommandFact> Prefixed(const std::vector<CommandFact> &rows, const std::string &prefix)
{
    std::vector<CommandFact> out;
    for (const CommandFact &r : rows)
    {
        if (r.text.size() >= prefix.size() && r.text.compare(0, prefix.size(), prefix) == 0)
            out.push_back(r);
    }
    return out;
}

// Whitespace-separated token count ("/task n" = 2; "/task " = 1).
int CountTokens(const std::string &s)
{
    int n = 0;
    bool in = false;
    for (char c : s)
    {
        if (c == ' ')
            in = false;
        else if (!in)
        {
            in = true;
            n++;
        }
    }
    return n;
}

} // namespace

void CommandPalette::SetSource(std::vector<CommandFact> families, std::vector<CommandFact> commands)
{
    families_ = std::move(families);
    commands_ = std::move(commands);
}

bool CommandPalette::Update(const std::string &content)
{
    content_ = content;

    // Esc dismissed this exact text; any change re-opens.
    if (closed_by_user_ && content == closed_content_)
    {
        open_ = false;
        return false;
    }
    closed_by_user_ = false;

    items_.clear();
    selected_ = 0;

    // Gate: a single line that starts with '/'. (Multi-line input -- Alt+Enter or a
    // paste -- never shows the popup.)
    if (content.empty() || content[0] != '/' || content.find('\n') != std::string::npos)
    {
        open_ = false;
        return false;
    }

    if (content.back() == ' ')
    {
        // "... " -> the next level: that family's subcommands.
        items_ = Prefixed(commands_, content);
    }
    else
    {
        std::vector<CommandFact> hits = Prefixed(commands_, content);
        if (CountTokens(content) <= 1)
        {
            // Still typing the first token ("/", "/ta"): list families, so a bare '/'
            // shows ~9 rows instead of all of them.
            std::vector<CommandFact> fams = Prefixed(families_, content);
            if (!fams.empty())
                hits.swap(fams);
        }
        items_ = std::move(hits);
    }

    open_ = !items_.empty();
    return open_;
}

void CommandPalette::MoveUp()
{
    if (open_ && selected_ > 0)
        selected_--;
}

void CommandPalette::MoveDown()
{
    if (open_ && selected_ + 1 < static_cast<int>(items_.size()))
        selected_++;
}

void CommandPalette::Close()
{
    open_ = false;
    closed_by_user_ = true;
    closed_content_ = content_;
}

std::string CommandPalette::Accept() const
{
    if (!open_ || items_.empty())
        return content_;

    // Complete to the command itself: drop the argument shape ("<desc>", "[--bg]", the
    // "|" of an alternative), trim, and leave a space for the next token.
    const std::string &cmd = items_[selected_].text;
    const size_t cut = cmd.find_first_of("<|[");
    std::string head = (cut == std::string::npos) ? cmd : cmd.substr(0, cut);
    while (!head.empty() && head.back() == ' ')
        head.pop_back();
    return head + " ";
}

} // namespace edc::model

namespace edc::ui
{

::ftxui::Element CommandPaletteElement(const ::edc::model::CommandPalette &palette, int max_rows)
{
    using namespace ::ftxui;

    const style::Palette &pal = style::palette();
    const std::vector<::edc::model::CommandFact> &items = palette.items();
    const int count = static_cast<int>(items.size());
    if (count == 0)
        return text("");
    if (max_rows < 1)
        max_rows = 1;

    // Scroll the window so the selection stays visible.
    int first = 0;
    if (count > max_rows)
    {
        first = palette.selected() - max_rows + 1;
        if (first < 0)
            first = 0;
        if (first > count - max_rows)
            first = count - max_rows;
    }

    Elements rows;
    for (int i = first; i < count && i < first + max_rows; i++)
    {
        std::string line = "  " + items[i].text;
        if (!items[i].brief.empty())
            line += "  " + items[i].brief;

        // One FIXED-height row that expands horizontally. xflex, not filler(): filler
        // also grows on Y, which would stretch this block and squeeze the chat (the
        // block must stay glued to the input's top edge).
        Element row = text(line) | xflex;
        if (i == palette.selected())
            row = row | color(pal.accent) | bold; // foreground highlight, no background bar
        else
            row = row | color(pal.text);
        rows.push_back(row);
    }

    rows.push_back(text("  ↑↓ move · Tab complete · Esc close") | xflex | color(pal.text_dim));
    return vbox(std::move(rows)) | bgcolor(pal.pane_bg);
}

} // namespace edc::ui
