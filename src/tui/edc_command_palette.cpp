#include "edc_command_palette.hpp"

#include "edc_style.hpp"

#include <algorithm>
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

// The whitespace-separated words of a row's LITERAL part (its text up to the argument
// shape "<…", "[…", "|"): "/task new <desc>   [--bg]" -> {"/task", "new"}.
std::vector<std::string> WordsOf(const std::string &cmd)
{
    const size_t cut = cmd.find_first_of("<|[");
    const std::string head = (cut == std::string::npos) ? cmd : cmd.substr(0, cut);

    std::vector<std::string> words;
    std::string word;
    for (char c : head)
    {
        if (c == ' ')
        {
            if (!word.empty())
            {
                words.push_back(word);
                word.clear();
            }
        }
        else
        {
            word += c;
        }
    }
    if (!word.empty())
        words.push_back(word);
    return words;
}

// A syntax hint rather than a runnable command: its text carries an argument shape
// ("<…", "[…", the "|" of an alternative), so it names no concrete thing to pick.
bool IsPlaceholderRow(const CommandFact &r)
{
    return r.text.find_first_of("<|[") != std::string::npos;
}

std::string CommonPrefix(const std::string &a, const std::string &b)
{
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n])
        n++;
    return a.substr(0, n);
}

// A row's literal part: the text up to its argument shape ("<desc>", "[--bg]", the "|" of
// an alternative), trailing spaces trimmed. "-d <task>" -> "-d".
std::string LiteralOf(const std::string &cmd)
{
    const size_t cut = cmd.find_first_of("<|[");
    std::string head = (cut == std::string::npos) ? cmd : cmd.substr(0, cut);
    while (!head.empty() && head.back() == ' ')
        head.pop_back();
    return head;
}

// The text Tab inserts for one row: the command itself, dropping the argument shape, with
// one space left for the next token.
std::string CompletionOf(const std::string &cmd)
{
    return LiteralOf(cmd) + " ";
}

} // namespace

void CommandPalette::SetSource(std::vector<CommandFact> families, std::vector<CommandFact> commands)
{
    families_ = std::move(families);
    commands_ = std::move(commands);
}

void CommandPalette::SetArgSource(std::string head, std::vector<CommandFact> rows)
{
    arg_head_ = std::move(head);
    arg_rows_ = std::move(rows);
}

void CommandPalette::SetFileSource(std::vector<CommandFact> rows)
{
    file_rows_ = std::move(rows);
}

void CommandPalette::SetShellOptionSource(std::vector<CommandFact> rows)
{
    shell_option_rows_ = std::move(rows);
}

CommandPalette::MentionHit CommandPalette::ParseMention(const std::string &content, int cursor)
{
    MentionHit hit;
    if (cursor < 0 || cursor > static_cast<int>(content.size()))
        cursor = static_cast<int>(content.size());
    if (cursor == 0)
        return hit;

    // The LAST '@' before the caret, with no whitespace between it and the caret, and at
    // the line start or right after whitespace (so "foo@bar" and "@Override in a word"
    // never trigger).
    const size_t at = content.rfind('@', static_cast<size_t>(cursor) - 1);
    if (at == std::string::npos || static_cast<int>(at) >= cursor)
        return hit;
    if (at > 0)
    {
        const char prev = content[at - 1];
        if (prev != ' ' && prev != '\t' && prev != '\n')
            return hit;
    }
    for (size_t i = at + 1; i < static_cast<size_t>(cursor); i++)
    {
        if (content[i] == ' ' || content[i] == '\t' || content[i] == '\n')
            return hit;
    }

    hit.active = true;
    hit.begin = static_cast<int>(at);
    hit.query = content.substr(at + 1, static_cast<size_t>(cursor) - at - 1);
    return hit;
}

std::string CommandPalette::MentionInsert() const
{
    if (items_.empty() || selected_ < 0 || selected_ >= static_cast<int>(items_.size()))
        return "";
    const model::CommandFact &item = items_[static_cast<size_t>(selected_)];
    /* The directory the query already points into: accepting it means "this directory",
     * so the mention is finished (trailing space) rather than opened for the next level. */
    if (item.mention_completes)
        return "@" + item.text + " ";
    const std::string &row = item.text;
    const bool dir = !row.empty() && row.back() == '/';
    return "@" + row + (dir ? "" : " ");
}

bool CommandPalette::Update(const std::string &content, int cursor)
{
    // A new text is a new question: the walk over the previous rows is over, so Enter
    // means "send" again until the user moves the selection once more. (A caret move
    // with the same text is not a new question -- the highlight stays meaning what it
    // meant.)
    if (content != content_)
        navigated_ = false;
    content_ = content;
    cursor_ = cursor;

    // Esc dismissed this exact text; any change re-opens.
    if (closed_by_user_ && content == closed_content_)
    {
        open_ = false;
        return false;
    }
    closed_by_user_ = false;

    items_.clear();
    selected_ = 0;

    // `@`: the caret is inside a file reference. The rows are whatever the kernel found
    // for this query -- filtering happens there (it owns the tree), so they go straight in.
    const MentionHit hit = ParseMention(content, cursor);
    if (hit.active)
    {
        mode_ = Mode::Mention;
        trigger_ = hit.begin;
        items_ = file_rows_;
        open_ = !items_.empty();
        return open_;
    }
    mode_ = Mode::Command;

    // Gate: a single line. (Multi-line input -- Alt+Enter or a paste -- never shows the
    // popup.)
    if (content_.empty() || content_.find('\n') != std::string::npos)
    {
        open_ = false;
        return false;
    }

    // A SHELL line ("desc" --bg / +"desc" --bg / 'desc' --bg): while an option word is
    // being typed, the option vocabulary is the list. Only the word under the caret is a
    // filter here -- the quoted description before it is context, not a prefix.
    const bool shell_line = content_[0] == '"' || content_[0] == '\'' || content_[0] == '+';
    if (shell_line && !shell_option_rows_.empty())
    {
        const size_t sp = content_.rfind(' ');
        const std::string word = (sp == std::string::npos) ? content_ : content_.substr(sp + 1);
        if (!word.empty() && word[0] == '-')
        {
            mode_ = Mode::ShellOption;
            items_.clear();
            for (const CommandFact &r : shell_option_rows_)
                if (r.text.compare(0, word.size(), word) == 0)
                    items_.push_back(r);
            open_ = !items_.empty();
            return open_;
        }
        open_ = false;
        return false;
    }

    if (content_[0] != '/')
    {
        open_ = false;
        return false;
    }

    // A command whose first argument is an open set (the prompt library). Once the command
    // word is WHOLE its vocabulary is part of the list: past the word ("/prompt re") those
    // rows ARE the list, and at the bare word ("/prompt", no space yet) they join the
    // command's own rows -- so the names are visible before the space is even typed.
    const bool head_word = !arg_rows_.empty() && content_.compare(0, arg_head_.size(), arg_head_) == 0;
    const bool past_head =
        head_word && content_.size() > arg_head_.size() && content_[arg_head_.size()] == ' ';
    if (past_head)
    {
        items_ = Prefixed(arg_rows_, content_);
        open_ = !items_.empty();
        return open_;
    }

    if (content_.back() == ' ')
    {
        // "... " -> the next level: that family's subcommands.
        items_ = Prefixed(commands_, content_);
    }
    else
    {
        std::vector<CommandFact> hits = Prefixed(commands_, content_);
        if (CountTokens(content_) <= 1)
        {
            // Still on the first token ("/", "/ta"): list the FAMILIES, so a bare '/' shows
            // ~9 modules instead of every row. Families are for telling modules apart,
            // though -- once the prefix identifies exactly ONE of them ("/ta" -> /task),
            // step into it: the family row would only echo the name plus its blurb, while
            // the subcommands are what the user is after. (Falling back when no command row
            // matches keeps the list from going empty.)
            std::vector<CommandFact> fams = Prefixed(families_, content_);
            if (!fams.empty() && (fams.size() > 1 || hits.empty()))
                hits.swap(fams);
        }
        items_ = std::move(hits);
    }

    // Bare command word ("/prompt"): the argument vocabulary rides along with the command's
    // own rows. The syntax-hint row ("/prompt <name> [args…]") steps aside -- a placeholder
    // is not a candidate once the real names are known; the bare word itself stays (it is
    // runnable: it lists the templates). They share the first token, so Tab still completes
    // the word itself.
    if (head_word && content_ == arg_head_)
    {
        items_.erase(std::remove_if(items_.begin(), items_.end(), IsPlaceholderRow), items_.end());
        for (const CommandFact &r : arg_rows_)
            items_.push_back(r);
    }

    open_ = !items_.empty();
    return open_;
}

void CommandPalette::MoveUp()
{
    if (!open_)
        return;
    /* Walking the list: the highlighted row is now what the user means, so Enter picks
     * it instead of sending (see navigated()). Set even at the top -- pressing Up is a
     * "I am choosing here" signal, not a request to move pixels. */
    navigated_ = true;
    if (selected_ > 0)
        selected_--;
}

void CommandPalette::MoveDown()
{
    if (!open_)
        return;
    navigated_ = true;
    if (selected_ + 1 < static_cast<int>(items_.size()))
        selected_++;
}

void CommandPalette::Close()
{
    open_ = false;
    navigated_ = false;
    closed_by_user_ = true;
    closed_content_ = content_;
}

std::string CommandPalette::WordCompletion() const
{
    const size_t sp = content_.rfind(' ');
    const std::string typed = (sp == std::string::npos) ? std::string() : content_.substr(0, sp + 1);

    // How many whole words precede the one being typed (a trailing space means "the next
    // word").
    int index = 0;
    bool in = false;
    for (char c : typed)
    {
        if (c == ' ')
            in = false;
        else if (!in)
        {
            in = true;
            index++;
        }
    }

    std::string common;
    bool first = true;
    bool whole = true;
    for (const CommandFact &item : items_)
    {
        const std::vector<std::string> words = WordsOf(item.text);
        if (static_cast<int>(words.size()) <= index)
            return content_; /* this candidate has no token there: nothing to grow into */
        if (first)
        {
            common = words[index];
            first = false;
        }
        else
        {
            common = CommonPrefix(common, words[index]);
        }
        if (common != words[index])
            whole = false;
    }
    if (common.empty())
        return content_;

    // The space goes on only when every candidate agrees on the whole token: the word is
    // finished ("/sess" -> "/session "), not merely narrowed ("/task st" stays "/task st").
    const std::string grown = typed + common + (whole ? " " : "");
    return grown == content_ ? content_ : grown;
}

std::string CommandPalette::Accept() const
{
    if (!open_ || items_.empty())
        return content_;

    // A shell line's option word is spliced into the line: everything before it stays
    // (that is the quoted description), the chosen option replaces the word. WordCompletion
    // cannot do this -- it grows a token, and for "--bg" the token is the whole candidate.
    if (mode_ == Mode::ShellOption)
    {
        const size_t sp = content_.rfind(' ');
        const std::string prefix = (sp == std::string::npos) ? std::string() : content_.substr(0, sp + 1);
        const std::string typed = (sp == std::string::npos) ? content_ : content_.substr(sp + 1);

        std::string common;
        bool whole = true;
        bool first = true;
        for (const CommandFact &r : items_)
        {
            const std::string lit = LiteralOf(r.text);
            if (first)
            {
                common = lit;
                first = false;
            }
            else
            {
                common = CommonPrefix(common, lit);
                if (common != lit)
                    whole = false;
            }
        }
        // Grow the word when the candidates still share something ("--" stays "--");
        // otherwise take the highlighted row, which is what Up/Down is for.
        if (common.size() > typed.size() && common.compare(0, typed.size(), typed) == 0)
            return prefix + common + (whole ? " " : "");
        return prefix + CompletionOf(items_[selected_].text);
    }

    // 1. Complete the ONE word being typed ("/sess" -> "/session "): Tab grows a token, it
    //    does not swallow a whole candidate row.
    const std::string word = WordCompletion();
    if (word != content_)
        return word;

    // 2. That word has nothing left to give (the candidates disagree on it, or they agree
    //    on it and already disagree on the next one): take the highlighted row, which
    //    breaks the tie by choosing one -- that is what Up/Down is for.
    return CompletionOf(items_[selected_].text);
}

std::string CommandPalette::AcceptSelected() const
{
    if (!open_ || items_.empty())
        return content_;

    // A shell line's option word: keep the quoted description in front, replace the word
    // with the chosen option (the same splice Accept() does, without the common-prefix step).
    if (mode_ == Mode::ShellOption)
    {
        const size_t sp = content_.rfind(' ');
        const std::string prefix = (sp == std::string::npos) ? std::string() : content_.substr(0, sp + 1);
        return prefix + CompletionOf(items_[selected_].text);
    }

    if (mode_ == Mode::Mention)
        return MentionInsert(); // already a whole row: "@src/main.c " / "@dir/"

    return CompletionOf(items_[selected_].text);
}

bool CommandPalette::HasMore() const
{
    return open_ && !items_.empty() && Accept() != content_;
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

    /* The hint names what the list IS, so a `@` popup and a `/` popup are told apart at a
     * glance (and a stale build is visible: the mention hint is newer). */
    const char *hint = (palette.mode() == ::edc::model::CommandPalette::Mode::Mention)
                           ? "  @ files · Tab: enter directory / complete · Esc close"
                           : "  ↑↓ move · Tab complete · Esc close";
    rows.push_back(text(hint) | xflex | color(pal.text_dim));
    /* A raised surface, not the pane's: the popup is drawn OVER the chat, which paints
     * pane_bg from edge to edge -- a pane_bg popup would sink into it without a trace.
     * dialog_bg is the palette's "this floats" tone (the same one the toast chip and
     * the setup modal wear); the popup's bottom edge is the input's accent rule, so the
     * block is closed on the side the eye needs. */
    return vbox(std::move(rows)) | bgcolor(pal.dialog_bg);
}

} // namespace edc::ui
