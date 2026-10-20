#include "edc_input.hpp"

#include "edc_style.hpp"
#include "edc_text_util.hpp"
#include <algorithm>
#include <cstring>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <utility>
#include <vector>

namespace edc
{
namespace
{
// Upper bound on recalled entries; older ones fall off the front. Only the
// last few dozen are ever useful, but a generous cap keeps the whole session.
constexpr size_t kMaxHistory = 500;

// Byte length of the UTF-8 sequence whose first byte sits at |at| (clamped to
// |limit|). Returns 1 on an overlong/truncated tail so we never stall.
int GlyphLenAt(const std::string &s, int at, int limit)
{
    if (at >= limit)
        return 0;
    size_t len = Utf8SeqLen(static_cast<unsigned char>(s[at]));
    if (at + static_cast<int>(len) > limit)
        len = static_cast<size_t>(limit - at);
    return len > 0 ? static_cast<int>(len) : 1;
}

// |at| clamped to [0, size], then moved left off any UTF-8 continuation byte, so the
// result is always a code point boundary. A caret between two bytes of one character
// would make the next edit corrupt it.
int SnapToBoundary(const std::string &s, int at)
{
    if (at <= 0)
        return 0;
    if (at > static_cast<int>(s.size()))
        at = static_cast<int>(s.size());
    while (at > 0 && at < static_cast<int>(s.size()) && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80)
        at--;
    return at;
}

// Byte offset of the code point preceding |pos| (or 0 when already at the
// start). '\n' counts as a single-byte code point.
int PrevGlyph(const std::string &s, int pos)
{
    if (pos <= 0)
        return 0;
    int p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80)
        --p;
    return p;
}
} // namespace

EdcInput::EdcInput(std::string *content, std::string placeholder, int max_visible_rows)
    : content_(content), placeholder_(std::move(placeholder)), max_visible_rows_(std::max(1, max_visible_rows))
{
    box_.x_min = 0;
    box_.x_max = -1; // not measured yet: no valid width until first layout
    box_.y_min = 0;
    box_.y_max = -1;
}

EdcInput::~EdcInput()
{
    edc_payload_free(blocks_);
    edc_payload_free(draft_payload_);
}

// ── Folded paste blocks ──────────────────────────────────────────────

void EdcInput::InvalidateExpansion()
{
    ++expanded_epoch_;
}

const std::string &EdcInput::ExpandedDraft() const
{
    if (expanded_at_ == expanded_epoch_)
        return expanded_cache_;
    if (edc_payload_empty(blocks_))
    {
        expanded_cache_ = *content_;
    }
    else
    {
        char *expanded = edc_payload_expand(content_->c_str(), blocks_);
        expanded_cache_.assign(expanded ? expanded : "", expanded ? strlen(expanded) : 0);
        edc_payload_string_free(expanded);
    }
    expanded_at_ = expanded_epoch_;
    return expanded_cache_;
}

void EdcInput::InsertPasteBlock(const std::string &label, const std::string &text)
{
    if (label.empty())
        return;
    StopBrowsing();
    if (!blocks_)
        blocks_ = edc_payload_new();
    edc_payload_set(blocks_, label.c_str(), text.c_str());

    // One trailing space: the caret then lands right AFTER the chip instead of inside
    // it, which is where the next character belongs. The space survives expansion
    // (only the key is replaced), so the block never welds onto the following word.
    const std::string inserted = label + " ";
    content_->insert(static_cast<size_t>(cursor_), inserted);
    cursor_ += static_cast<int>(inserted.size());
    desired_col_ = -1;
    InvalidateExpansion();
}

void EdcInput::ClearPasteBlocks()
{
    edc_payload_free(blocks_);
    blocks_ = nullptr;
    InvalidateExpansion();
}

bool EdcInput::HasBlock(const std::string &label) const
{
    return edc_payload_get(blocks_, label.c_str()) != NULL;
}

void EdcInput::RestoreBlocks(const struct edc_payload *blocks)
{
    if (edc_payload_empty(blocks))
        return;
    if (!blocks_)
        blocks_ = edc_payload_new();
    for (int i = 0; i < blocks->n; ++i)
        edc_payload_set(blocks_, blocks->items[i].key, blocks->items[i].text);
    InvalidateExpansion();
}

const struct edc_payload *EdcInput::DraftPayload() const
{
    if (draft_at_ == expanded_epoch_)
        return draft_payload_;

    edc_payload_free(draft_payload_);
    draft_payload_ = nullptr;
    if (!edc_payload_empty(blocks_))
    {
        for (int i = 0; i < blocks_->n; ++i)
        {
            /* Only the blocks the draft still points at travel with the message. */
            if (content_->find(blocks_->items[i].key) == std::string::npos)
                continue;
            if (!draft_payload_)
                draft_payload_ = edc_payload_new();
            edc_payload_set(draft_payload_, blocks_->items[i].key, blocks_->items[i].text);
        }
    }
    draft_at_ = expanded_epoch_;
    return draft_payload_;
}

bool EdcInput::HasUnresolvedBlocks() const
{
    /* The only way to notice "this placeholder has no bytes behind it": walk the draft's
     * own placeholder sigil. Nothing else in the widget looks at the text. */
    static const char kSigil[] = "[Pasted #";
    size_t at = content_->find(kSigil);
    while (at != std::string::npos)
    {
        int end = 0;
        if (!TokenStartingAt(static_cast<int>(at), &end))
            return true;
        at = content_->find(kSigil, static_cast<size_t>(end));
    }
    return false;
}

void EdcInput::RemoveHistoryEntry(const std::string &entry)
{
    for (size_t i = 0; i < history_.size(); ++i)
    {
        if (history_[i] != entry)
            continue;
        history_.erase(history_.begin() + static_cast<long>(i));
        if (browsing_)
        {
            if (history_pos_ > i)
                --history_pos_;
            else if (history_pos_ == i)
                history_pos_ = std::min(history_.size(), i);
        }
        return;
    }
}


bool EdcInput::TokenStartingAt(int at, int *end, std::string *key, bool *is_paste) const
{
    if (at < 0 || at >= static_cast<int>(content_->size()))
        return false;

    const size_t room = content_->size() - static_cast<size_t>(at);
    size_t best_len = 0;
    bool best_paste = false;
    std::string best_key;

    auto consider = [&](const char *candidate, bool paste) {
        const size_t len = candidate ? strlen(candidate) : 0;
        // Longest match wins: one key may be a prefix of another.
        if (len == 0 || len > room || len <= best_len)
            return;
        if (content_->compare(static_cast<size_t>(at), len, candidate) == 0)
        {
            best_len = len;
            best_paste = paste;
            best_key.assign(candidate, len);
        }
    };

    if (blocks_)
        for (int i = 0; i < blocks_->n; ++i)
            consider(blocks_->items[i].key, true);
    for (const std::string &atom : atoms_)
        consider(atom.c_str(), false);

    if (best_len == 0)
        return false;
    if (end)
        *end = at + static_cast<int>(best_len);
    if (key)
        *key = best_key;
    if (is_paste)
        *is_paste = best_paste;
    return true;
}

bool EdcInput::TokenEndingAt(int at, int *begin, std::string *key, bool *is_paste) const
{
    if (at <= 0 || at > static_cast<int>(content_->size()))
        return false;

    size_t best_len = 0;
    bool best_paste = false;
    std::string best_key;
    auto consider = [&](const char *candidate, bool paste) {
        const size_t len = candidate ? strlen(candidate) : 0;
        if (len == 0 || len > static_cast<size_t>(at) || len <= best_len)
            return;
        if (content_->compare(static_cast<size_t>(at) - len, len, candidate) == 0)
        {
            best_len = len;
            best_paste = paste;
            best_key.assign(candidate, len);
        }
    };

    if (blocks_)
        for (int i = 0; i < blocks_->n; ++i)
            consider(blocks_->items[i].key, true);
    for (const std::string &atom : atoms_)
        consider(atom.c_str(), false);

    if (best_len == 0)
        return false;
    if (begin)
        *begin = at - static_cast<int>(best_len);
    if (key)
        *key = best_key;
    if (is_paste)
        *is_paste = best_paste;
    return true;
}

void EdcInput::RegisterAtom(const std::string &label)
{
    if (label.empty())
        return;
    for (const std::string &known : atoms_)
        if (known == label)
            return;
    atoms_.push_back(label);
}

std::string InputModeLabel(edc_input_mode mode)
{
    switch (mode)
    {
    case EDC_INPUT_COMMAND:
        return "command";
    case EDC_INPUT_LOCALSHELL:
        return "local shell";
    case EDC_INPUT_SHELL:
        return "shell";
    case EDC_INPUT_REJECTED:
        return "syntax error";
    case EDC_INPUT_TEXT:
    default:
        return "message"; /* the common case gets a name too -- silence reads as "unclassified" */
    }
}

void EdcInput::Reset()
{
    cursor_ = 0;
    scroll_ = 0;
    desired_col_ = -1;
    hovered_ = false;
    StopBrowsing();
    // The caller cleared the draft behind our back. The library keeps its bytes: a recalled
    // history entry still needs them, and DraftPayload() only sends what the draft
    // references.
    InvalidateExpansion();
}

void EdcInput::ReplaceAll(const std::string &text)
{
    // Completion popup: swap the whole draft for the completed command and put the
    // caret after it. Browsing is left behind (the popup and history recall are
    // mutually exclusive: the popup only shows on a line starting with '/').
    *content_ = text;
    cursor_ = static_cast<int>(content_->size());
    scroll_ = 0;
    desired_col_ = -1;
    StopBrowsing();
    InvalidateExpansion();
}

void EdcInput::SetCursor(int offset)
{
    cursor_ = SnapToBoundary(*content_, offset);
    desired_col_ = -1; /* the column is derived from the caret again */
}

void EdcInput::ReplaceRange(int begin, int end, const std::string &text)
{
    if (begin > end)
    {
        const int t = begin;
        begin = end;
        end = t;
    }
    begin = SnapToBoundary(*content_, begin);
    end = SnapToBoundary(*content_, end);

    content_->replace(static_cast<size_t>(begin), static_cast<size_t>(end - begin), text);
    cursor_ = begin + static_cast<int>(text.size());
    desired_col_ = -1;
    StopBrowsing(); /* an edit ends any history browse, like typing does */
    InvalidateExpansion();
}

void EdcInput::AddHistory(const std::string &entry)
{
    // Appending is the owner's signal that the draft was consumed: leave any
    // recall so the next ArrowUp starts from the entry just added.
    StopBrowsing();
    if (entry.empty())
        return;
    // Consecutive duplicates (a re-send, a stray Enter) collapse into one.
    if (!history_.empty() && history_.back() == entry)
        return;
    history_.push_back(entry);
    if (history_.size() > kMaxHistory)
        history_.erase(history_.begin());
}

void EdcInput::ClearHistory()
{
    StopBrowsing();
    history_.clear();
}

void EdcInput::StopBrowsing()
{
    browsing_ = false;
    history_pos_ = history_.size();
    draft_.clear();
    prefix_.clear();
    draft_cursor_ = 0;
}

bool EdcInput::MatchesPrefix(size_t index) const
{
    // Byte prefix, case-sensitive (what a shell does): the draft is what the user
    // typed, so it has to match the entry from its first byte. An empty prefix
    // matches every entry, which leaves plain Up/Down browsing unchanged.
    return history_[index].compare(0, prefix_.size(), prefix_) == 0;
}

int EdcInput::WrapColumns() const
{
    if (box_.x_min <= box_.x_max)
    {
        const int w = box_.x_max - box_.x_min + 1;
        // Reserve one column for the caret cell appended at row ends so a
        // full row plus its caret never overflows the widget box.
        return std::max(1, w - 1);
    }
    // Not laid out yet (first frame, or defensive call before any render).
    return 40;
}

EdcInput::Layout EdcInput::MakeLayout(const std::string &content, int cols)
{
    Layout layout;
    const int n = static_cast<int>(content.size());
    int line_begin = 0;
    for (int i = 0; i <= n; ++i)
    {
        if (i < n && content[i] != '\n')
            continue;
        // Logical line content is [line_begin, i); '\n' (if any) sits at i.
        Logical log;
        log.begin = line_begin;
        log.end = i;
        log.first_row = static_cast<int>(layout.rows.size());
        if (log.begin == log.end)
        {
            // Empty line still occupies one visual row so the caret has a
            // place to sit.
            layout.rows.push_back({log.begin, log.begin});
            log.row_count = 1;
        }
        else
        {
            const std::string seg = content.substr(log.begin, log.end - log.begin);
            const std::vector<std::pair<int, int>> spans = VisualWrapRows(seg, cols);
            for (const auto &sp : spans)
                layout.rows.push_back({log.begin + sp.first, log.begin + sp.second});
            log.row_count = static_cast<int>(layout.rows.size()) - log.first_row;
            if (log.row_count == 0)
            {
                layout.rows.push_back({log.begin, log.begin});
                log.row_count = 1;
            }
        }
        layout.lines.push_back(log);
        line_begin = i + 1;
    }
    return layout;
}

int EdcInput::FindCaretRow(const Layout &layout, int offset, int *lo)
{
    *lo = offset;
    if (layout.rows.empty())
        return -1;

    for (size_t i = 0; i < layout.rows.size(); ++i)
    {
        const Row &r = layout.rows[i];
        if (offset >= r.begin && offset <= r.end)
            return static_cast<int>(i);
    }

    // |offset| is not covered by any row — it lies on a separator dropped at
    // a wrap boundary (or at the content edge). Snap it to the nearest row
    // end so the caret still renders on a real row.
    for (size_t i = 0; i < layout.rows.size(); ++i)
    {
        if (offset < layout.rows[i].begin)
        {
            if (i == 0)
            {
                *lo = layout.rows[0].begin;
                return 0;
            }
            *lo = layout.rows[i - 1].end;
            return static_cast<int>(i - 1);
        }
    }
    *lo = layout.rows.back().end;
    return static_cast<int>(layout.rows.size()) - 1;
}

int EdcInput::ColumnAt(const std::string &content, const Layout &layout, int row, int offset)
{
    if (row < 0 || row >= static_cast<int>(layout.rows.size()))
        return 0;
    const Row &r = layout.rows[row];
    offset = std::max(r.begin, std::min(offset, r.end));
    return StringWidth(content.substr(r.begin, offset - r.begin));
}

int EdcInput::OffsetAtColumn(const std::string &content, const Layout &layout, int row, int column)
{
    if (row < 0 || row >= static_cast<int>(layout.rows.size()))
        return row < 0 ? 0 : static_cast<int>(content.size());
    const Row &r = layout.rows[row];
    if (column <= 0)
        return r.begin;
    int width = 0;
    int off = r.begin;
    while (off < r.end)
    {
        const int len = GlyphLenAt(content, off, r.end);
        if (len == 0)
            break;
        const int w = StringWidth(content.substr(off, len));
        if (width + w > column)
            break;
        width += w;
        off += len;
    }
    return off;
}

int EdcInput::CaretColumn()
{
    const Layout layout = MakeLayout(*content_, WrapColumns());
    if (layout.rows.empty())
        return 0;
    int lo = cursor_;
    const int row = FindCaretRow(layout, cursor_, &lo);
    return ColumnAt(*content_, layout, row, lo);
}

::ftxui::Element EdcInput::RowElement(const Row &row, int caret_at, bool is_focused,
                                      const ::ftxui::Decorator &caret) const
{
    // Runs: plain text and chips, in row order. A chip is recognised by its placeholder
    // text, so a placeholder the user edited through simply renders as plain text (the
    // block is gone; its library entry is dropped by the atomic delete above).
    struct Run
    {
        bool token; /* one of the two atomic kinds */
        bool paste; /* ...and it is a folded paste (bytes behind it), not an `@mention` */
        int begin;
        int end;
    };
    std::vector<Run> runs;
    int p = row.begin;
    while (p < row.end)
    {
        int block_end = 0;
        bool is_paste = false;
        if (TokenStartingAt(p, &block_end, nullptr, &is_paste) && block_end <= row.end)
        {
            /* A paste chip owns one space of padding: the insert puts a space right after
             * the placeholder, and painting it with the chip's surface is what makes the
             * block read as a block instead of a highlighted run of text. (The extra space
             * is display only -- the placeholder that expands is still the label.) */
            if (is_paste && block_end < row.end && (*content_)[static_cast<size_t>(block_end)] == ' ')
                block_end++;
            runs.push_back({true, is_paste, p, block_end});
            p = block_end;
            continue;
        }
        const int len = GlyphLenAt(*content_, p, row.end);
        if (len <= 0)
            break;
        if (!runs.empty() && !runs.back().token)
            runs.back().end = p + len; // merge into the plain run (no hbox churn per glyph)
        else
            runs.push_back({false, false, p, p + len});
        p += len;
    }
    if (runs.empty())
        runs.push_back({false, false, row.begin, row.end}); // empty line: the caret still needs a cell

    ::ftxui::Elements parts;
    const int caret_pos = (caret_at < 0) ? -1 : std::max(row.begin, std::min(caret_at, row.end));

    auto push_text = [&](int a, int b, bool token, bool paste) {
        if (b <= a)
            return;
        ::ftxui::Element e =
            ::ftxui::text(content_->substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
        /* Two kinds, two looks (see edc_style.hpp: input_paste_text / input_paste_surface):
         *   - a pasted block is a chip -- amber on its own warm surface. It only ever appears
         *     in the input line, where no status colour is painted, so the warm hue is free
         *     there and is neither the chrome accent (cyan: references) nor the inline-code
         *     purple the replies are full of;
         *   - an `@mention` is a keyword: the keyword pink, no surface, no underline -- the
         *     colour is the whole mark (edc_style.hpp: input_mention_text).
         * Both knobs are in edc_style.hpp: these are content colours, not chrome roles. */
        if (token)
            e = paste ? (e | ::ftxui::color(style::input_paste_text()) | ::ftxui::bgcolor(style::input_paste_surface()))
                      : (e | ::ftxui::color(style::input_mention_text()));
        parts.push_back(e);
    };

    for (const Run &run : runs)
    {
        if (caret_pos < 0 || caret_pos < run.begin || caret_pos > run.end)
        {
            push_text(run.begin, run.end, run.token, run.paste);
            continue;
        }
        if (caret_pos == run.end)
        {
            // After this run's last glyph: one caret cell (between two runs, or at the row
            // end), exactly like ftxui::Input at a line end.
            push_text(run.begin, run.end, run.token, run.paste);
            parts.push_back(::ftxui::text(is_focused ? " " : "") | caret);
            continue;
        }
        // On a glyph: block/bar over it.
        const int len = GlyphLenAt(*content_, caret_pos, run.end);
        push_text(run.begin, caret_pos, run.token, run.paste);
        parts.push_back(
            ::ftxui::text(content_->substr(static_cast<size_t>(caret_pos), static_cast<size_t>(len))) | caret);
        push_text(caret_pos + len, run.end, run.token, run.paste);
    }
    return ::ftxui::hbox(std::move(parts));
}

::ftxui::Element EdcInput::OnRender()
{
    const bool is_focused = Focused();
    const ::ftxui::Decorator caret = (!is_focused && !hovered_)
                                         ? ::ftxui::Decorator(::ftxui::focus)
                                         : (insert_mode_ ? ::ftxui::Decorator(::ftxui::focusCursorBarBlinking)
                                                         : ::ftxui::Decorator(::ftxui::focusCursorBlockBlinking));

    if (content_->empty())
    {
        ::ftxui::Element e = ::ftxui::text(placeholder_) | ::ftxui::dim;
        if (is_focused || hovered_)
            e = e | caret;
        return e | ::ftxui::xflex | ::ftxui::reflect(box_);
    }

    const int cols = WrapColumns();
    const Layout layout = MakeLayout(*content_, cols);
    if (layout.rows.empty())
        return ::ftxui::text(placeholder_) | ::ftxui::dim | ::ftxui::xflex | ::ftxui::reflect(box_);

    const int total = static_cast<int>(layout.rows.size());
    int caret_lo = cursor_;
    const int caret_row = FindCaretRow(layout, cursor_, &caret_lo);

    // Keep the caret row inside the internal scrolling window.
    if (total <= max_visible_rows_)
    {
        scroll_ = 0;
    }
    else if (caret_row < scroll_)
    {
        scroll_ = caret_row;
    }
    else if (caret_row >= scroll_ + max_visible_rows_)
    {
        scroll_ = caret_row - max_visible_rows_ + 1;
    }
    scroll_ = std::max(0, std::min(scroll_, total - max_visible_rows_));

    const int visible = std::min(max_visible_rows_, total - scroll_);
    ::ftxui::Elements elements;
    elements.reserve(static_cast<size_t>(visible));
    for (int i = 0; i < visible; ++i)
    {
        const int row_index = scroll_ + i;
        const Row &r = layout.rows[row_index];
        const int caret_at = (row_index == caret_row) ? caret_lo : -1;
        elements.push_back(RowElement(r, caret_at, is_focused, caret));
    }

    return ::ftxui::vbox(std::move(elements)) | ::ftxui::xflex | ::ftxui::reflect(box_);
}

bool EdcInput::OnEvent(::ftxui::Event event)
{
    cursor_ = std::max(0, std::min(cursor_, static_cast<int>(content_->size())));

    if (event.is_character())
        return HandleCharacter(event.character());
    if (event == ::ftxui::Event::Return)
    {
        // Enter submits; TUIWindow intercepts it before it reaches us. If it
        // ever gets here, ignore rather than insert. (Note: Event equality
        // compares only the input bytes, so Event::Character("\n") — the
        // Alt+Enter / paste path — must have been handled above already.)
        return false;
    }
    if (event.is_mouse())
        return HandleMouse(event);
    if (event == ::ftxui::Event::Backspace)
        return HandleBackspace();
    if (event == ::ftxui::Event::Delete)
        return HandleDelete();
    if (event == ::ftxui::Event::ArrowLeft)
        return MoveLeft();
    if (event == ::ftxui::Event::ArrowRight)
        return MoveRight();
    if (event == ::ftxui::Event::ArrowUp)
        return HandleUp();
    if (event == ::ftxui::Event::ArrowDown)
        return HandleDown();
    if (event == ::ftxui::Event::Home)
        return HandleHome();
    if (event == ::ftxui::Event::End)
        return HandleEnd();
    if (event == ::ftxui::Event::Insert)
        return HandleInsert();
    return false;
}

bool EdcInput::HandleCharacter(const std::string &character)
{
    if (character.empty())
        return true;
    // Typing abandons a recall: the recalled entry becomes an ordinary draft,
    // and there is no longer one to restore on ArrowDown.
    StopBrowsing();
    if (!insert_mode_ && cursor_ < static_cast<int>(content_->size()) && (*content_)[cursor_] != '\n')
    {
        // Overwrite mode: replace the glyph under the caret. This is the one way to type
        // INTO a placeholder -- the key stops matching, so the block simply stops being
        // referenced (its bytes stay in the library: another history entry may want them).
        const int len = GlyphLenAt(*content_, cursor_, static_cast<int>(content_->size()));
        content_->erase(cursor_, static_cast<size_t>(len));
        content_->insert(cursor_, character);
        cursor_ += static_cast<int>(character.size());
        desired_col_ = -1;
        InvalidateExpansion();
        return true;
    }
    content_->insert(cursor_, character);
    cursor_ += static_cast<int>(character.size());
    desired_col_ = -1;
    InvalidateExpansion();
    return true;
}

bool EdcInput::HandleBackspace()
{
    if (cursor_ == 0)
        return false;
    StopBrowsing();
    int block_begin = 0;
    std::string block_key;
    bool is_paste = false;
    if (TokenEndingAt(cursor_, &block_begin, &block_key, &is_paste))
    {
        // One token on screen, one token to Backspace: the whole placeholder -- or the whole
        // `@mention` -- goes. Only a paste block also drops its bytes (deleting it is how you
        // discard that); an atom has none, and `@` keeps its meaning either way.
        content_->erase(static_cast<size_t>(block_begin), static_cast<size_t>(cursor_ - block_begin));
        cursor_ = block_begin;
        desired_col_ = -1;
        if (is_paste)
            edc_payload_remove(blocks_, block_key.c_str());
        InvalidateExpansion();
        return true;
    }
    const int p = PrevGlyph(*content_, cursor_);
    content_->erase(static_cast<size_t>(p), static_cast<size_t>(cursor_ - p));
    cursor_ = p;
    desired_col_ = -1;
    InvalidateExpansion();
    return true;
}

bool EdcInput::HandleDelete()
{
    if (cursor_ >= static_cast<int>(content_->size()))
        return false;
    StopBrowsing();
    int block_end = 0;
    std::string block_key;
    bool is_paste = false;
    if (TokenStartingAt(cursor_, &block_end, &block_key, &is_paste))
    {
        // Mirror of Backspace: Delete on the first glyph of a token removes all of it.
        content_->erase(static_cast<size_t>(cursor_), static_cast<size_t>(block_end - cursor_));
        desired_col_ = -1;
        if (is_paste)
            edc_payload_remove(blocks_, block_key.c_str());
        InvalidateExpansion();
        return true;
    }
    const int len = GlyphLenAt(*content_, cursor_, static_cast<int>(content_->size()));
    if (len == 0)
        return false;
    content_->erase(cursor_, static_cast<size_t>(len));
    desired_col_ = -1;
    InvalidateExpansion();
    return true;
}

bool EdcInput::HandleInsert()
{
    insert_mode_ = !insert_mode_;
    return true;
}

bool EdcInput::MoveLeft()
{
    if (cursor_ == 0)
        return false;
    int block_begin = 0;
    if (TokenEndingAt(cursor_, &block_begin))
    {
        cursor_ = block_begin; // one move over the whole chip
        desired_col_ = -1;
        return true;
    }
    cursor_ = PrevGlyph(*content_, cursor_);
    desired_col_ = -1;
    return true;
}

bool EdcInput::MoveRight()
{
    if (cursor_ >= static_cast<int>(content_->size()))
        return false;
    int block_end = 0;
    if (TokenStartingAt(cursor_, &block_end))
    {
        cursor_ = block_end; // right before the chip -> right after it
        desired_col_ = -1;
        return true;
    }
    const int len = GlyphLenAt(*content_, cursor_, static_cast<int>(content_->size()));
    cursor_ += len;
    desired_col_ = -1;
    return true;
}

bool EdcInput::MoveUp()
{
    if (cursor_ == 0)
        return false;
    const Layout layout = MakeLayout(*content_, WrapColumns());
    if (layout.rows.empty())
        return false;
    int lo = cursor_;
    const int row = FindCaretRow(layout, cursor_, &lo);
    const int col = ColumnAt(*content_, layout, row, lo);
    if (desired_col_ < 0)
        desired_col_ = col;
    if (row == 0)
    {
        // First visual row of the whole content: jump to the start (this is
        // what ftxui::Input does for an unwrapped first line).
        cursor_ = 0;
        desired_col_ = -1;
        return true;
    }
    cursor_ = OffsetAtColumn(*content_, layout, row - 1, desired_col_);
    return true;
}

bool EdcInput::MoveDown()
{
    if (cursor_ >= static_cast<int>(content_->size()))
        return false;
    const Layout layout = MakeLayout(*content_, WrapColumns());
    if (layout.rows.empty())
        return false;
    int lo = cursor_;
    const int row = FindCaretRow(layout, cursor_, &lo);
    const int col = ColumnAt(*content_, layout, row, lo);
    if (desired_col_ < 0)
        desired_col_ = col;
    if (row + 1 < static_cast<int>(layout.rows.size()))
    {
        cursor_ = OffsetAtColumn(*content_, layout, row + 1, desired_col_);
    }
    else
    {
        // Last visual row: jump to the end of the content, mirroring
        // ftxui::Input's "move to the end when there is no next line".
        cursor_ = static_cast<int>(content_->size());
        desired_col_ = -1;
    }
    return true;
}

bool EdcInput::HandleUp()
{
    // While browsing, ArrowUp always steps through history (the recalled
    // entry's caret row must not swallow the key).
    if (browsing_)
        return HistoryPrev();
    if (history_.empty())
        return MoveUp();
    // Recall only from the first visual row; anywhere lower ArrowUp keeps its
    // caret-navigation meaning inside a (possibly wrapped) draft.
    const Layout layout = MakeLayout(*content_, WrapColumns());
    int lo = cursor_;
    const int row = FindCaretRow(layout, cursor_, &lo);
    if (row > 0)
        return MoveUp();
    // Nothing in the history starts with what is typed: keep ArrowUp on the caret
    // (at the first row that is a no-op, exactly as with an empty history).
    return HistoryPrev() || MoveUp();
}

bool EdcInput::HandleDown()
{
    // Only history mode turns ArrowDown into "next entry"; otherwise it keeps
    // moving the caret down the draft (jumping to the end on the last row).
    if (browsing_)
        return HistoryNext();
    return MoveDown();
}

bool EdcInput::HistoryPrev()
{
    if (history_.empty())
        return false;
    if (!browsing_)
    {
        // The text already typed filters the recall: only entries that START with it
        // are candidates, so typing `task ` and pressing Up walks the `task ...` lines
        // instead of every unrelated message. A draft nothing matches is not a recall
        // at all -- return false without stashing anything, so ArrowUp stays "move the
        // caret" (and an empty draft matches everything: plain Up is unchanged).
        prefix_ = *content_;
        bool any_match = false;
        for (size_t i = 0; i < history_.size() && !any_match; ++i)
            any_match = MatchesPrefix(i);
        if (!any_match)
            return false;
        // Entering history mode: stash whatever the user was typing so it can
        // be brought back with ArrowDown past the newest entry.
        draft_ = *content_;
        draft_cursor_ = cursor_;
        browsing_ = true;
        history_pos_ = history_.size();
    }
    // Walk back to the previous entry that still fits the prefix.
    for (size_t i = history_pos_; i > 0; --i)
    {
        if (!MatchesPrefix(i - 1))
            continue;
        RecallHistory(i - 1);
        return true;
    }
    return true; // already at the oldest match: consume, no change
}

bool EdcInput::HistoryNext()
{
    if (!browsing_)
        return false;
    // Walk forward to the next entry that still fits the prefix.
    for (size_t i = history_pos_ + 1; i < history_.size(); ++i)
    {
        if (!MatchesPrefix(i))
            continue;
        RecallHistory(i);
        return true;
    }
    // Stepped past the newest match: restore the draft and leave history mode.
    *content_ = draft_;
    cursor_ = std::min(draft_cursor_, static_cast<int>(content_->size()));
    InvalidateExpansion();
    if (recall_hook_)
        recall_hook_(*content_);
    StopBrowsing();
    scroll_ = 0;
    desired_col_ = -1;
    return true;
}

void EdcInput::RecallHistory(size_t index)
{
    history_pos_ = index;
    *content_ = history_[index];
    InvalidateExpansion();
    // The owner can now pull the payload this entry's placeholders need (they are not in
    // the library after a restart).
    if (recall_hook_)
        recall_hook_(*content_);
    // Like a shell, the caret lands at the end of the recalled entry so it can
    // be edited or re-sent; OnRender() then scrolls the caret row into view.
    cursor_ = static_cast<int>(content_->size());
    desired_col_ = -1;
}

bool EdcInput::HandleHome()
{
    cursor_ = 0;
    desired_col_ = -1;
    return true;
}

bool EdcInput::HandleEnd()
{
    cursor_ = static_cast<int>(content_->size());
    desired_col_ = -1;
    return true;
}

bool EdcInput::HandleMouse(::ftxui::Event event)
{
    hovered_ = box_.Contain(event.mouse().x, event.mouse().y) && CaptureMouse(event);
    if (!hovered_)
        return false;
    if (event.mouse().button != ::ftxui::Mouse::Left)
        return false;
    if (event.mouse().motion != ::ftxui::Mouse::Pressed)
        return false;

    TakeFocus();

    if (content_->empty())
    {
        cursor_ = 0;
        desired_col_ = -1;
        return true;
    }

    const Layout layout = MakeLayout(*content_, WrapColumns());
    if (layout.rows.empty())
        return false;

    const int visible = std::min(max_visible_rows_, static_cast<int>(layout.rows.size()) - scroll_);
    int vrow = event.mouse().y - box_.y_min;
    vrow = std::max(0, std::min(vrow, std::max(0, visible - 1)));
    const int row = scroll_ + vrow;
    cursor_ = OffsetAtColumn(*content_, layout, row, std::max(0, event.mouse().x - box_.x_min));
    desired_col_ = -1;
    return true;
}

} // namespace edc
