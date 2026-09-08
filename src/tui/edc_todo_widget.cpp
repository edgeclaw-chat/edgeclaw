#include "edc_todo_widget.hpp"
#include "edc_text_util.hpp"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <string>

namespace edc
{

TodoWidget::TodoWidget()
{
}

void TodoWidget::SetItems(const std::vector<Item> &items)
{
    items_ = items;
    completed_ = 0;
    for (const Item &item : items_)
    {
        if (item.status == "completed")
            completed_++;
    }
}

::ftxui::Element TodoWidget::OnRender()
{
    using namespace ::ftxui;

    // Palette is constructed inline at render time (same style as
    // TaskWidget): ftxui::Color's constructor queries terminal state, which
    // is not ready during C++ static initialization — file-scope const Color
    // objects would run before main() and segfault.
    const Color kBlue = Color::RGB(94, 151, 247);
    const Color kGreen = Color::RGB(64, 168, 96);
    const Color kGray = Color::RGB(147, 147, 147);
    const Color kDimLine = Color::RGB(60, 60, 60);
    const Color kTodoBg = Color::RGB(27, 27, 27);

    const size_t total = items_.size();
    const size_t shown = std::min(total, kMaxVisibleRows);

    Elements lines;
    {
        // Header carries the progress summary ("2/5"); it turns green when
        // every listed item is done.
        const std::string summary = std::to_string(completed_) + "/" + std::to_string(total);
        Element header = hbox({
            text(" Todo ") | bold | color(kBlue),
            filler(),
            text(summary) | color(completed_ == static_cast<int>(total) ? kGreen : kGray) | bold,
        });
        lines.push_back(std::move(header));
        lines.push_back(separator() | color(kDimLine));
    }

    // Width available for an item row. reflect() below sits on the vbox
    // *inside* the (empty-style) border; before the first measurement fall
    // back to the default sidebar width (36) minus the 2 border columns.
    int row_width = 34;
    if (content_box_.x_min != 0 || content_box_.x_max != 0 || content_box_.y_min != 0 || content_box_.y_max != 0)
        row_width = content_box_.x_max - content_box_.x_min + 1;

    // Fixed cells per row: 2 indent + 3 marker + 1 gap after the marker.
    constexpr int kFixedCells = 6;

    for (size_t i = 0; i < shown; i++)
    {
        const Item &item = items_[i];
        const bool done = item.status == "completed";
        const bool doing = item.status == "in_progress";

        const std::string mark = done ? "[x]" : (doing ? "[•]" : "[ ]");
        Element marker = text("  " + mark + " ");
        if (doing)
            marker |= bold;
        marker |= color(done ? kGreen : (doing ? kBlue : kGray));

        // The in_progress item may carry a dim " <- <activeForm>" tail.
        std::string active_trunc;
        if (doing && !item.active_form.empty())
            active_trunc = TruncateToWidth(item.active_form, 24);

        const int active_cells = active_trunc.empty() ? 0 : 4 + StringWidth(active_trunc);
        const int budget = row_width - kFixedCells - active_cells;
        const std::string content = (budget > 4) ? TruncateToWidth(item.content, budget) : std::string();

        Elements row;
        row.push_back(std::move(marker));

        if (!content.empty())
        {
            Element content_el = text(content);
            if (done)
            {
                content_el |= dim;
                content_el |= strikethrough;
                content_el |= color(kGreen);
            }
            else if (doing)
            {
                content_el |= color(Color::RGB(229, 229, 229));
            }
            else
            {
                content_el |= dim;
            }
            row.push_back(std::move(content_el));
        }
        if (!active_trunc.empty())
            row.push_back(text(" <- " + active_trunc) | dim | color(kBlue));

        lines.push_back(hbox(std::move(row)));
    }

    if (total > shown)
    {
        lines.push_back(text("  … +" + std::to_string(total - shown) + " more") | dim | color(kGray));
    }

    return vbox(std::move(lines)) | reflect(content_box_) | borderStyled(BorderStyle::EMPTY) | bgcolor(kTodoBg);
}

} // namespace edc
