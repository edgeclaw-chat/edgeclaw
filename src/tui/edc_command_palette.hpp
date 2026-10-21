#pragma once

// The input box's completion popup: while the user types a line starting with '/', a
// list of matching commands floats ABOVE the input (above its top rule, so it grows into
// the chat area and never looks like part of the input box). Up/Down move the selection,
// Tab completes -- or, when there is nothing left to complete, CONFIRMS and the caller
// folds the list; Esc dismisses. Enter submits, EXCEPT once Up/Down has been used to walk
// the list: then Enter picks the highlighted row (navigated()), the same thing Tab does,
// because the row under the bar is what the user is pointing at.
//
// CommandPalette below is the MODEL (pure logic, no terminal): the window feeds it the
// input content and renders items(). Its source is the shell's command table, projected
// to the frontend through IAppFacade, so the list can never drift from `/help`.

#include <ftxui/dom/elements.hpp> /* the renderer below returns ftxui::Element */

#include <string>
#include <vector>

namespace edc::model
{

// One palette row.
struct CommandFact
{
    std::string text;  // "/task new <desc>   [--bg] …", or a family name "/task"
    std::string brief; // one line
    // `@` completion only: this row IS the directory the query already points into
    // ("@src/" -> "src"). Accepting it finishes the mention instead of descending.
    bool mention_completes = false;
};

class CommandPalette
{
  public:
    // What the popup is listing. The two are mutually exclusive: a line either starts a
    // `/` command or contains an `@` reference under the caret.
    enum class Mode
    {
        Command,
        Mention,
        ShellOption, // a shell line's option word ("desc" --bg) -- see SetShellOptionSource
    };

    // Where an `@` reference starts and what has been typed after it. Pure: no state, so
    // the rule can be tested on its own (it is the same rule opencode uses).
    struct MentionHit
    {
        bool active = false;
        int begin = 0;        // byte offset of the '@'
        std::string query;    // what follows it, up to the caret
    };
    static MentionHit ParseMention(const std::string &content, int cursor);

    // |families| = the bare-`/` list (one row per first token, e.g. "/task").
    // |commands| = every table row. Both come from IAppFacade.
    void SetSource(std::vector<CommandFact> families, std::vector<CommandFact> commands);

    // The candidate rows for the tokens AFTER |head| (e.g. "/prompt" -> one row per prompt
    // template, spelled "/prompt <name>"). While the input is "|head| " plus an argument,
    // these replace the command table's rows; an empty |rows| leaves the table in charge.
    // This is how a command whose first argument is an open set -- the prompt library --
    // gets its names into the list.
    void SetArgSource(std::string head, std::vector<CommandFact> rows);

    // The option vocabulary of a shell line (--bg, -g <group>, …), empty when the shell
    // switch is off. Only consulted when the line IS a shell line and the word under the
    // caret starts with '-': the popup then lists options instead of commands.
    void SetShellOptionSource(std::vector<CommandFact> rows);

    // The `@` candidates, already filtered by the query (the kernel walks the tree; this
    // only holds the rows). A directory's text ends with '/'.
    void SetFileSource(std::vector<CommandFact> rows);

    // Recompute from the WHOLE input content and the caret. Returns true when the list is
    // visible. The caret decides which `@` (if any) is being typed.
    bool Update(const std::string &content, int cursor);

    Mode mode() const
    {
        return mode_;
    }
    // The `@` of the mention being typed (Mention mode only).
    int trigger_begin() const
    {
        return trigger_;
    }
    // Text to put in place of [trigger_begin(), cursor): "@path " for a file, "@dir/" for
    // a directory (which keeps the list open for the next level).
    std::string MentionInsert() const;

    bool open() const
    {
        return open_;
    }
    // True when the selection was moved since the text last changed -- the user is walking
    // the list. The window reads this to let Enter PICK the highlighted row (what the eye
    // is already on) instead of sending; a text change resets it, so plain typing plus
    // Enter still submits.
    bool navigated() const
    {
        return navigated_;
    }
    const std::vector<CommandFact> &items() const
    {
        return items_;
    }
    int selected() const
    {
        return selected_;
    }

    void MoveUp();
    void MoveDown();
    // Esc: stay closed until the content changes again (typing re-opens).
    void Close();

    // Tab: the text the input should be replaced with. It completes the ONE word being
    // typed -- the last token grows to the longest common prefix of the candidates' token
    // there ("/sess" -> "/session ", with a space once the word is whole) -- and only when
    // that word has nothing left to give does it fall back to the highlighted row (which
    // breaks the tie by choosing one).
    std::string Accept() const;

    // Enter, after Up/Down: the HIGHLIGHTED row taken WHOLE ("/tas" + Up/Down onto
    // "/task new" -> "/task new "). Accept() above is Tab's job -- grow the word being
    // typed -- and growing is exactly what must NOT happen once the user has said which
    // row they mean.
    std::string AcceptSelected() const;

    // True while Tab would change the text -- i.e. the last completion has NOT been reached
    // yet. False means the same Tab is also the "that's it", so the CALLER folds the list
    // instead of demanding one more press.
    bool HasMore() const;

  private:
    // Accept()'s first half: grow the last token to the candidates' common prefix for it.
    std::string WordCompletion() const;

    std::vector<CommandFact> families_;
    std::vector<CommandFact> commands_;
    // The open-set vocabulary for one command's first argument (see SetArgSource).
    std::string arg_head_;
    std::vector<CommandFact> arg_rows_;
    std::vector<CommandFact> items_;
    std::vector<CommandFact> file_rows_;
    std::vector<CommandFact> shell_option_rows_;
    Mode mode_ = Mode::Command;
    int trigger_ = 0;
    int cursor_ = 0;
    int selected_ = 0;
    bool open_ = false;
    // The user moved the selection since the last text change (see navigated()).
    bool navigated_ = false;
    // Esc dismissed the list for this exact content.
    bool closed_by_user_ = false;
    std::string closed_content_;
    std::string content_;
};

} // namespace edc::model

namespace edc::ui
{

// Draw the popup: at most |max_rows| rows around the selection, plus a hint line. No
// border (the window's look has none); the selected row is a full-width accent bar.
::ftxui::Element CommandPaletteElement(const ::edc::model::CommandPalette &palette, int max_rows = 8);

} // namespace edc::ui
