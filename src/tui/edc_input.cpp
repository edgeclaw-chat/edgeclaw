#include "edc_input.hpp"

#include "edc_text_util.hpp"
#include <algorithm>
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
    : content_(content), placeholder_(std::move(placeholder)),
      max_visible_rows_(std::max(1, max_visible_rows))
{
    box_.x_min = 0;
    box_.x_max = -1; // not measured yet: no valid width until first layout
    box_.y_min = 0;
    box_.y_max = -1;
}

void EdcInput::Reset()
{
    cursor_ = 0;
    scroll_ = 0;
    desired_col_ = -1;
    hovered_ = false;
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

::ftxui::Element EdcInput::OnRender()
{
    const bool is_focused = Focused();
    const ::ftxui::Decorator caret =
        (!is_focused && !hovered_)
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
        return ::ftxui::text(placeholder_) | ::ftxui::dim | ::ftxui::xflex |
               ::ftxui::reflect(box_);

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
        if (row_index != caret_row)
        {
            elements.push_back(
                ::ftxui::text(content_->substr(r.begin, r.end - r.begin)));
            continue;
        }

        const std::string before = content_->substr(r.begin, caret_lo - r.begin);
        if (caret_lo >= r.end)
        {
            // Caret sits after the last glyph of this visual row: append a
            // one-cell caret, like ftxui::Input does at a line end.
            const std::string caret_cell = is_focused ? " " : "";
            elements.push_back(::ftxui::hbox({
                ::ftxui::text(before),
                ::ftxui::text(caret_cell) | caret,
            }));
        }
        else
        {
            // Caret sits on a glyph of this row: block/bar over that glyph.
            const int len = GlyphLenAt(*content_, caret_lo, r.end);
            const std::string at = content_->substr(caret_lo, len);
            const std::string after = content_->substr(
                caret_lo + len, r.end - (caret_lo + len));
            elements.push_back(::ftxui::hbox({
                ::ftxui::text(before),
                ::ftxui::text(at) | caret,
                ::ftxui::text(after),
            }));
        }
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
        return MoveUp();
    if (event == ::ftxui::Event::ArrowDown)
        return MoveDown();
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
    if (!insert_mode_ && cursor_ < static_cast<int>(content_->size()) &&
        (*content_)[cursor_] != '\n')
    {
        // Overwrite mode: replace the glyph under the caret.
        const int len = GlyphLenAt(*content_, cursor_, static_cast<int>(content_->size()));
        content_->erase(cursor_, static_cast<size_t>(len));
    }
    content_->insert(cursor_, character);
    cursor_ += static_cast<int>(character.size());
    desired_col_ = -1;
    return true;
}

bool EdcInput::HandleBackspace()
{
    if (cursor_ == 0)
        return false;
    const int p = PrevGlyph(*content_, cursor_);
    content_->erase(p, static_cast<size_t>(cursor_ - p));
    cursor_ = p;
    desired_col_ = -1;
    return true;
}

bool EdcInput::HandleDelete()
{
    if (cursor_ >= static_cast<int>(content_->size()))
        return false;
    const int len = GlyphLenAt(*content_, cursor_, static_cast<int>(content_->size()));
    if (len == 0)
        return false;
    content_->erase(cursor_, static_cast<size_t>(len));
    desired_col_ = -1;
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
    cursor_ = PrevGlyph(*content_, cursor_);
    desired_col_ = -1;
    return true;
}

bool EdcInput::MoveRight()
{
    if (cursor_ >= static_cast<int>(content_->size()))
        return false;
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

    const int visible = std::min(max_visible_rows_,
                                 static_cast<int>(layout.rows.size()) - scroll_);
    int vrow = event.mouse().y - box_.y_min;
    vrow = std::max(0, std::min(vrow, std::max(0, visible - 1)));
    const int row = scroll_ + vrow;
    cursor_ = OffsetAtColumn(*content_, layout, row, std::max(0, event.mouse().x - box_.x_min));
    desired_col_ = -1;
    return true;
}

} // namespace edc
