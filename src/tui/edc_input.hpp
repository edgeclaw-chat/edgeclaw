#pragma once

#include "edc_api.h" /* edc_input_mode: what the mode tag renders */
#include "edc_payload.h" /* folded paste blocks: placeholder key -> real bytes */

#include <ftxui/component/component_base.hpp>
#include <functional>
#include <ftxui/screen/box.hpp>
#include <string>
#include <vector>

namespace edc
{

// The label a mode gets when it is written INTO the input box's top rule
// ("──── shell ────"): how the command layer will read the line being typed.
//
// Every draft gets a name, the plain message included: silence would read as "not classified
// yet", and the whole point of the label is that the reader never has to remember the rule
// table. The one state with no label is the EMPTY input box -- nothing is being read yet --
// which is also what keeps that row from looking decorated before you type.
std::string InputModeLabel(edc_input_mode mode);

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
    ~EdcInput() override;

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

    // ── Folded paste blocks ──────────────────────────────────────────
    //
    // A large paste becomes ONE line of placeholder text (|label|, e.g.
    // "[Pasted #1 · 42 lines]") with the real bytes parked in the payload. Folding is
    // display only: everything that decides meaning (the input-kind badge, the message
    // that gets sent, the transcript) reads ExpandedDraft(), never the buffer. See
    // edc_text_passes.h for why that distinction is load-bearing.
    //
    // The placeholder is a byte string that has to survive editing, so it is matched as
    // text (no offsets to maintain). Editing treats a whole block as one atom:
    // Backspace/Delete at either edge removes the block AND its payload entry, and
    // Left/Right step over it in one move. Editing *inside* a placeholder (only possible
    // by overwrite mode) simply breaks the match -- see ExpandedDraft().
    void InsertPasteBlock(const std::string &label, const std::string &text);

    // Every block this widget knows about (key = the placeholder text), including the ones
    // a recalled history entry brought back. Borrowed; empty when nothing is known.
    const struct edc_payload *Blocks() const
    {
        return blocks_;
    }

    // The subset |content| actually references, i.e. what has to travel with the message.
    // A block whose placeholder is no longer in the draft contributes nothing: folding is a
    // display device, so an unreferenced block is not part of what the line says. Rebuilt
    // when the draft or the library changes; NULL when nothing is referenced.
    const struct edc_payload *DraftPayload() const;

    // True while |label| is already known (the caller numbers pastes, and two blocks must
    // never share a key).
    bool HasBlock(const std::string &label) const;

    // Merge blocks back into the library (a history entry's payload arrived from the
    // store). Does NOT touch the draft: the placeholder text is already there, and
    // ExpandedDraft() starts working as soon as its bytes are known.
    void RestoreBlocks(const struct edc_payload *blocks);

    // True when the draft contains a placeholder whose bytes are NOT known (a recalled
    // history entry still waiting for its payload). Submitting such a line would send the
    // literal `[Pasted #1 · 42 lines]`, so the owner must ask first.
    bool HasUnresolvedBlocks() const;

    // A recalled history entry (called from the recall path, so the owner can pull its
    // payload if this widget does not know it yet).
    void SetRecallHook(std::function<void(const std::string &)> hook)
    {
        recall_hook_ = std::move(hook);
    }

    // Drop one recall entry (its payload is gone for good: never offer it again).
    void RemoveHistoryEntry(const std::string &entry);

    // ── Tokens ───────────────────────────────────────────────────────
    //
    // A token's identity is a function of the TEXT, not of what this run happens to
    // remember: the same `ScanTokens()` the chat bubble uses decides, so a line typed now
    // and the same line recalled from history behave identically -- display, atomic
    // delete, everything. Nothing has to be registered, and nothing is lost on a restart.
    //
    // What the library adds is only the BYTES behind a paste placeholder (and those arrive
    // from the store when a recalled line needs them); `@path` needs none, because the AST
    // pass reads the file at submit time.
    struct Token
    {
        int begin = 0;
        int end = 0;
        bool paste = false;
        // A mention wears its mark only when it is FINISHED and the path exists: half-typed
        // text is not a reference yet, and a reference to nothing is not one either.
        bool marked = false;
    };
    // The draft's tokens, in order, cached on the same epoch as the expansion.
    const std::vector<Token> &Tokens() const;

    // The draft with every placeholder replaced by its parked text -- what this line
    // actually says. Single pass, non-recursive, cached until the draft or the blocks
    // change (the input badge asks for it once per frame).
    const std::string &ExpandedDraft() const;

    // Forget every block (the caller cleared the draft).
    void ClearPasteBlocks();

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

    // The token that starts at / ends at |at| (nullptr when there is none).
    const Token *TokenStartingAt(int at) const;
    const Token *TokenEndingAt(int at) const;
    void InvalidateExpansion();
    // One visual row, split into plain text and chips (a chip is drawn as a block).
    ::ftxui::Element RowElement(const Row &row, int caret_at, bool is_focused,
                                const ::ftxui::Decorator &caret) const;

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

    // Folded paste blocks, by key (placeholder text). This is a LIBRARY, not "the current
    // draft's blocks": history recall brings blocks back into it, and deleting a chip only
    // removes the text that referenced one. NULL until the first paste, so the ordinary
    // typing path never allocates for it.
    struct edc_payload *blocks_ = nullptr;
    // Cache for DraftPayload() (same epoch scheme as the expansion cache).
    mutable struct edc_payload *draft_payload_ = nullptr;
    mutable long long draft_at_ = -1;
    std::function<void(const std::string &)> recall_hook_;
    // Tokens of the current draft (see Token above), rebuilt when the text changes.
    mutable std::vector<Token> tokens_cache_;
    mutable long long tokens_at_ = -1;
    // ExpandedDraft() cache. |expanded_epoch_| moves on every edit or payload change;
    // |expanded_at_| is the epoch the cached string was built at.
    mutable std::string expanded_cache_;
    long long expanded_epoch_ = 0;
    mutable long long expanded_at_ = -1;
};

} // namespace edc
