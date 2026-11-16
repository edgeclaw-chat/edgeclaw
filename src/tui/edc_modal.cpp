#include "edc_modal.hpp"

#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/string.hpp"

#include <algorithm>

namespace edc
{

namespace
{

/* Draws the backdrop, flattens its background, centres the panel by explicit padding, and
 * clears the one cell that could swallow the panel's left column. */
class ModalOverlayNode : public ::ftxui::Node
{
  public:
    ModalOverlayNode(::ftxui::Element backdrop, ::ftxui::Element panel, int panel_width,
                     ::ftxui::Color ground, ::ftxui::Color panel_bg)
        : Node({std::move(backdrop), std::move(panel)}),
          panel_width_(panel_width),
          ground_(ground),
          panel_bg_(panel_bg)
    {
    }

    void SetBox(::ftxui::Box box) override
    {
        Node::SetBox(box);
        /* The backdrop fills the box; the panel is positioned by Render(). */
        children_[0]->SetBox(box);
        const int left_pad = std::max(0, (box.x_max - box.x_min + 1 - panel_width_) / 2);
        ::ftxui::Box panel_box = box;
        panel_box.x_min = box.x_min + left_pad;
        panel_box.x_max = std::min(box.x_max, panel_box.x_min + panel_width_ - 1);
        children_[1]->SetBox(panel_box);
    }

    void ComputeRequirement() override
    {
        ::ftxui::Node::ComputeRequirement();

        /* The panel owns the terminal caret while it is up.
         *
         * ftxui puts the caret where |requirement().focused| says, and its propagation between
         * siblings (Node::ComputeRequirement / Requirement::Focused::Prefer) settles a tie between
         * two focused components by keeping whichever came FIRST. The backdrop is the whole
         * window, whose chat input never stops reporting focus -- it is still the window's active
         * child while a modal is open -- so the tie was always won by the input BEHIND the dialog,
         * and every field in the dialog looked cursor-less: the caret was blinking at the bottom
         * of the screen the whole time.
         *
         * A modal is the exception to "first wins": while one is up, the only thing the user can
         * type into is the panel. Taking its focus requirement unconditionally is what says so. */
        const auto &panel = children_[1]->requirement().focused;
        if (panel.enabled)
            requirement_.focused = panel;
    }

    void Render(::ftxui::Screen &screen) override
    {
        /* 1. The transcript, with its own backgrounds replaced by one colour. Only the
         *    background: the text must stay readable, which is the point of not covering it. */
        children_[0]->Render(screen);
        for (int y = box_.y_min; y <= box_.y_max; y++)
            for (int x = box_.x_min; x <= box_.x_max; x++)
                screen.CellAt(x, y).background_color = ground_;

        /* 2. The panel, on that clean ground. */
        children_[1]->Render(screen);

        /* 3. A fullwidth character starting at the column just left of the panel would occupy
         *    that column AND the panel's first one, and Screen::ToString() drops the cell that
         *    follows a wide character -- so the panel's left edge would vanish on those rows,
         *    shortening them by a column. Clearing the offending cell keeps the edge intact.
         *
         *    Done AFTER the panel is drawn, because the panel's background is what identifies the
         *    rows that have one; before it, every row looks like it has no panel. */
        const int edge = PanelLeft();
        if (edge > box_.x_min)
        {
            for (int y = box_.y_min; y <= box_.y_max; y++)
            {
                if (screen.CellAt(edge, y).background_color != panel_bg_)
                    continue; /* this row has no panel */
                ::ftxui::Cell &before = screen.CellAt(edge - 1, y);
                if (!before.character.empty() && ::ftxui::string_width(before.character) == 2)
                {
                    before.character = " ";
                    before.background_color = ground_;
                }
            }
        }
    }

  private:
    int PanelLeft() const
    {
        const int width = box_.x_max - box_.x_min + 1;
        return box_.x_min + std::max(0, (width - panel_width_) / 2);
    }

    int panel_width_;
    ::ftxui::Color ground_;
    ::ftxui::Color panel_bg_;
};

} // namespace

::ftxui::Element ModalOverlay(::ftxui::Element backdrop, ::ftxui::Element panel, int panel_width,
                              ::ftxui::Color ground, ::ftxui::Color panel_bg)
{
    /* |clear_under| is load-bearing, not decoration: |bgcolor| sets only the BACKGROUND of the
     * cells in the panel's box, so any cell the panel does not write a character to -- the filler
     * gap on the title row, a right-aligned hint's left side, an empty line -- keeps the glyph
     * the backdrop put there, and the chat text shows straight through the dialog. clear_under
     * blanks those cells before the panel draws. (The pre-ModalOverlay code had it as
     * `dialog | clear_under | center`; moving to this node dropped it.)
     *
     * Centred vertically by ftxui's fillers rather than by the node: the panel's height depends on
     * its content and on how many rows the failure page's output took, so it is not a number the
     * host can supply. Horizontally the node narrows the box, which is what puts the left edge at
     * a column it can compute. */
    auto centred = ::ftxui::vbox({::ftxui::filler(), std::move(panel) | ::ftxui::clear_under,
                                  ::ftxui::filler()});
    return std::make_shared<ModalOverlayNode>(std::move(backdrop), std::move(centred), panel_width, ground, panel_bg);
}

} // namespace edc
