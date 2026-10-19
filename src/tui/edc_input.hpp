#pragma once

#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/box.hpp>
#include <string>
#include <vector>

namespace edc
{

// A multi-line editor that soft-wraps long logical lines to the widget's width
// (CJK/wide-char aware) instead of clipping them. Editing only ever changes the
// real content (\n comes from Alt+Enter or a paste); wrap boundaries are a
// display concern. Navigates ArrowUp/Down across *visual* rows while keeping the
// column, scrolls internally past |max_visible_rows|, keeps a recallable
// submitted-message history, and puts the terminal caret on the wrapped row.
class EdcInput : public ::ftxui::ComponentBase
{
  public:
    // Edits |content| in place; |placeholder| shows while empty.
    EdcInput(std::string *content, std::string placeholder, int max_visible_rows = 8);

    // Reset caret/scroll state after the caller clears the content. Also leaves
    // history mode.
    void Reset();

    // Append to the recall history (consecutive duplicates collapse, the list is
    // capped); also leaves any in-progress recall.
    void AddHistory(const std::string &entry);

    // Replace the whole draft (the completion popup completes by swapping the line)
    // and put the caret at its end; leaves history recall.
    void ReplaceAll(const std::string &text);

    // Put the caret at |offset|: clamped to the content and snapped back to a code point
    // boundary (a caret inside a UTF-8 sequence would corrupt the next edit).
    void SetCursor(int offset);

    // Replace the bytes [begin, end) with |text| and leave the caret right after it --
    // what `@` completion does ("@src" -> "@src/main.c ") without touching the rest of
    // the draft. Offsets are clamped/snapped like SetCursor; |begin| > |end| is accepted.
    void ReplaceRange(int begin, int end, const std::string &text);

    // Drop every entry, so the owner can re-seed from disk idempotently.
    void ClearHistory();

    // Entries oldest-first.
    const std::vector<std::string> &History() const
    {
        return history_;
    }

    // True between the first ArrowUp that recalls an entry and the first edit (or
    // ArrowDown past the newest entry) that leaves history mode. The owner uses it to
    // keep the completion popup out of the way: a recalled line is not being typed, and
    // Up/Down belong to the history while a browse lasts.
    bool IsBrowsingHistory() const
    {
        return browsing_;
    }

    bool Focusable() const override
    {
        return true;
    }

    // Caret byte offset into the edited content (tests/debugging).
    int CursorPosition() const
    {
        return cursor_;
    }

    ::ftxui::Element OnRender() override;
    bool OnEvent(::ftxui::Event event) override;

  private:
    // One visual (displayed) row: a byte range into the content, never splitting
    // a UTF-8 sequence.
    struct Row
    {
        int begin = 0;
        int end = 0;
    };

    // One logical line (between '\n' separators; may be empty).
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
        std::vector<Row> rows;
        std::vector<Logical> lines;
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

    // Recall the previous entry only when the caret sits on the first visual row;
    // otherwise move up a row. HandleDown() mirrors it.
    bool HandleUp();
    bool HandleDown();

    // History recall: step to the next-older/newer entry that starts with the text
    // that was typed when the browse began (an empty draft matches everything, so
    // plain Up/Down still walks the whole list); entering history mode stashes the
    // in-progress draft, and stepping past the newest match restores it. Both return
    // false when there is nothing to recall -- HistoryPrev also returns false when no
    // entry starts with the draft, which keeps ArrowUp on its caret meaning.
    bool HistoryPrev();
    bool HistoryNext();
    void RecallHistory(size_t index);
    // True when history entry |index| starts with the browse prefix.
    bool MatchesPrefix(size_t index) const;
    // Forget the browsing position, the prefix and the stashed draft.
    void StopBrowsing();

    // Terminal cells available for one wrapped row (one column reserved for the
    // caret cell appended at a row end).
    int WrapColumns() const;

    // Display layout for |content| wrapped to |cols| cells.
    static Layout MakeLayout(const std::string &content, int cols);

    // Locate |offset|: returns the visual-row index and stores the "effective"
    // offset (a boundary byte dropped by wrapping clamps to the end of the
    // previous row) into |*lo|.
    static int FindCaretRow(const Layout &layout, int offset, int *lo);

    // Terminal-cell column of |offset| inside visual |row|.
    static int ColumnAt(const std::string &content, const Layout &layout, int row, int offset);

    // Byte offset in |content| of visual |row| at cell |column| (clamped to the
    // row, wide-char aware).
    static int OffsetAtColumn(const std::string &content, const Layout &layout, int row, int column);

    // Column of the caret (cached for vertical movement) or -1 if unset.
    int CaretColumn();

    std::string *content_;
    std::string placeholder_;
    int max_visible_rows_ = 8;

    // Byte offset of the caret, always on a UTF-8 boundary.
    int cursor_ = 0;
    // Index into Layout::rows of the first visible row.
    int scroll_ = 0;
    // Column kept while moving vertically; -1 = derive from the caret.
    int desired_col_ = -1;
    // Insert mode (bar caret) vs. overwrite (block caret).
    bool insert_mode_ = true;
    bool hovered_ = false;

    // Submitted-message history, oldest first. |history_pos_| is the entry shown
    // (== size() while editing the draft); |draft_| is the text stashed when
    // browsing starts, so HistoryNext() can restore it. |prefix_| is the draft as it
    // was at that moment: only entries starting with it are recall candidates, and
    // editing the recalled entry ends the browse (so the prefix never has to follow
    // an edit).
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    std::string draft_;
    std::string prefix_;
    int draft_cursor_ = 0;
    bool browsing_ = false;

    // Box measured during the previous frame's layout; the wrap width comes from
    // it, so the widget adapts to a resize one frame later.
    ::ftxui::Box box_;
};

} // namespace edc
