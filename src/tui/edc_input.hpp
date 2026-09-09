#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/box.hpp>
#include <string>
#include <vector>

namespace edc
{

// A multi-line text editor that soft-wraps (visual line wrap) long logical
// lines to the widget's width instead of clipping them, so text that exceeds
// the terminal width is still fully visible. Editing happens on the real
// content (\n only where the user pressed Alt+Enter / pasted newlines); wrap
// boundaries are purely a display concern.
//
// Beyond ftxui::Input's behavior this component:
//  - wraps every logical line to the measured widget width (CJK-aware,
//    wide-char safe), so an over-long draft flows onto following rows;
//  - navigates ArrowUp/ArrowDown across *visual* rows while keeping the
//    horizontal column, with the caret row kept visible when the draft is
//    taller than the maximum rendered height (internal scrolling window);
//  - keeps a submitted-message history (AddHistory) recallable with
//    ArrowUp/ArrowDown: ArrowUp on the first visual row recalls older
//    entries, ArrowDown walks back to the newest and finally restores the
//    in-progress draft. Entering history stashes the current text; typing
//    leaves history mode, keeping the edited text as the new draft so it is
//    not lost when walking the list again;
//  - places the terminal caret exactly at the caret glyph of the wrapped
//    row, like ftxui::Input does for its (unwrapped) lines.
class EdcInput : public ::ftxui::ComponentBase
{
  public:
    // Edits |content| in place. |placeholder| is shown while empty.
    // |max_visible_rows| caps the rendered height; longer drafts scroll
    // internally so the caret line stays in view.
    EdcInput(std::string *content, std::string placeholder, int max_visible_rows = 8);

    // Reset caret/scroll state — used after the caller clears the content
    // (e.g. after a message has been submitted). Also leaves history mode.
    void Reset();

    // Append |entry| to the recall history (consecutive duplicates collapse,
    // the list is capped). Called by the owner when a message is submitted;
    // this also leaves any in-progress recall so the next ArrowUp starts
    // from the newest entry.
    void AddHistory(const std::string &entry);

    // Drop every stored entry. The owner uses it before re-seeding history
    // from disk (so the load is idempotent).
    void ClearHistory();

    // Entries oldest-first, exactly as handed to AddHistory.
    const std::vector<std::string> &History() const { return history_; }

    bool Focusable() const override { return true; }

    // Current caret position as a byte offset into the edited content
    // (exposed for tests/debugging).
    int CursorPosition() const { return cursor_; }

    // Overrides of ComponentBase's public interface; kept public so callers
    // holding this component directly can feed it events and render it.
    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;

  private:
    // One visual (displayed) row. [begin, end) is a byte range into the whole
    // content; a wrap point never splits a UTF-8 sequence.
    struct Row
    {
        int begin = 0;
        int end = 0;
    };

    // One logical line (text between '\n' separators; empty line allowed).
    struct Logical
    {
        int begin = 0;     // first content byte
        int end = 0;       // one past the last content byte (excludes '\n')
        int first_row = 0; // index into Rows of this line's first visual row
        int row_count = 0; // visual rows this line wraps to (>= 1)
    };

    // Flattened display layout of the current content.
    struct Layout
    {
        std::vector<Row> rows;      // visual rows, in reading order
        std::vector<Logical> lines; // logical lines, in order
    };

    bool HandleCharacter(const std::string &character);
    bool HandleBackspace();
    bool HandleDelete();
    bool HandleInsert();
    bool MoveLeft();
    bool MoveRight();
    bool MoveUp();
    bool MoveDown();
    bool HandleHome();
    bool HandleEnd();
    bool HandleMouse(::ftxui::Event event);

    // Arrow dispatchers that fold history recall into visual-row navigation:
    // HandleUp() recalls the previous entry only when the caret already sits
    // on the first visual row (otherwise it moves up a row), HandleDown()
    // walks history forward while browsing and moves down a row otherwise.
    bool HandleUp();
    bool HandleDown();

    // History recall. HistoryPrev() steps to the next-older entry (entering
    // history mode on the first call, which stashes the in-progress text);
    // HistoryNext() steps to the next-newer entry and, past the newest, puts
    // the stashed draft back and leaves history mode. Both return false when
    // there is nothing to recall (empty list / not browsing).
    bool HistoryPrev();
    bool HistoryNext();
    void RecallHistory(size_t index);
    // Leave history mode: forget the browsing position and the stashed draft
    // (callers that restore the draft already copied it into |content_|).
    void StopBrowsing();

    // Width (in terminal cells) available for one wrapped row. One column is
    // reserved for the caret cell, which is appended when the caret sits at a
    // row end, so rows never overflow the widget box.
    int WrapColumns() const;

    // Build the display layout for |content| wrapped to |cols| cells.
    static Layout MakeLayout(const std::string &content, int cols);

    // Locate |offset| in the visual layout. Returns the visual-row index and
    // stores the "effective" offset (a boundary byte dropped during wrapping
    // is clamped to the end of the previous row) into |*lo|.
    static int FindCaretRow(const Layout &layout, int offset, int *lo);

    // Terminal-cell column of |offset| inside visual |row|.
    static int ColumnAt(const std::string &content, const Layout &layout, int row, int offset);

    // Byte offset in |content| of visual |row| at the given cell |column|
    // (clamped to the row, wide-char aware).
    static int OffsetAtColumn(const std::string &content, const Layout &layout, int row, int column);

    // Column of the caret (cached for vertical movement) or -1 if unset.
    int CaretColumn();

    std::string *content_;
    std::string placeholder_;
    int max_visible_rows_ = 8;

    // Byte offset of the caret into |content_| (always at a UTF-8 boundary).
    int cursor_ = 0;
    // Index (into Layout::rows) of the first visible row of the internal
    // scrolling window.
    int scroll_ = 0;
    // Horizontal column kept while moving vertically; -1 = derive from the
    // caret's current column. Cleared by any horizontal edit/movement.
    int desired_col_ = -1;
    // Insert mode (caret bar) vs. overwrite (caret block).
    bool insert_mode_ = true;
    // Mouse hovers this widget (from the last mouse event).
    bool hovered_ = false;

    // Submitted-message history, oldest first. |history_pos_| is the index of
    // the entry currently shown; it equals history_.size() while the user is
    // editing the draft rather than browsing (see browsing_). |draft_| holds
    // the in-progress text saved when browsing starts so HistoryNext() past
    // the newest entry can restore it.
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    std::string draft_;
    int draft_cursor_ = 0;
    bool browsing_ = false;

    // Box of this component measured during the previous frame's layout; the
    // wrap width comes from it, so the widget adapts to terminal/panel
    // resizes one frame after they happen.
    ::ftxui::Box box_;
};

} // namespace edc
