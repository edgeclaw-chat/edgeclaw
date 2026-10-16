#pragma once

// The input box's completion popup: while the user types a line starting with '/', a
// list of matching commands floats ABOVE the input (above its top rule, so it grows into
// the chat area and never looks like part of the input box). Up/Down move the selection,
// Tab completes -- or, when there is nothing left to complete, CONFIRMS and the caller
// folds the list; Esc dismisses. Enter is NOT touched -- it still submits.
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
};

class CommandPalette
{
  public:
    // |families| = the bare-`/` list (one row per first token, e.g. "/task").
    // |commands| = every table row. Both come from IAppFacade.
    void SetSource(std::vector<CommandFact> families, std::vector<CommandFact> commands);

    // Recompute from the WHOLE input content. Returns true when the list is visible.
    bool Update(const std::string &content);

    bool open() const
    {
        return open_;
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

    // True while Tab would change the text -- i.e. the last completion has NOT been reached
    // yet. False means the same Tab is also the "that's it", so the CALLER folds the list
    // instead of demanding one more press.
    bool HasMore() const;

  private:
    // Accept()'s first half: grow the last token to the candidates' common prefix for it.
    std::string WordCompletion() const;

    std::vector<CommandFact> families_;
    std::vector<CommandFact> commands_;
    std::vector<CommandFact> items_;
    int selected_ = 0;
    bool open_ = false;
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
