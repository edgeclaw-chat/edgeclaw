#include "edc_working_indicator_view.hpp"

#include "edc_style.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace edc::ui
{

namespace
{

// The bar's fixed geometry (see the header): 10 cells, a run of 5, one cell of
// travel per kStepMs.
constexpr int kBarCells = 10;
constexpr int kBarLit = 5;
constexpr long long kStepMs = 60;
// The run slides from completely past the left edge to completely past the right
// edge before the sweep reverses, so the ends of the bar are genuine turning
// points (a few frames of empty bar) rather than bumpers.
constexpr int kRunFirst = -kBarLit;
constexpr int kRunLast = kBarCells;
constexpr int kRunPositions = kRunLast - kRunFirst + 1;
// First frame: the run fully on screen at the left, so the row looks busy
// straight away instead of opening on an empty bar.
constexpr int kRunStart = 0;

// Where the lit run sits at |elapsed_ms|, and which way it is travelling. The
// position is fractional so the run can slide *between* cells; at whole-cell
// times it lands exactly on the integer a discrete sweep would use.
struct BarState
{
    float pos;    // leftmost lit cell; may be negative or past the right edge
    bool forward; // true while the run sweeps right
};

BarState BarAt(long long elapsed_ms)
{
    const float step = static_cast<float>(elapsed_ms < 0 ? 0 : elapsed_ms) / static_cast<float>(kStepMs);
    const float cycle = static_cast<float>(2 * (kRunPositions - 1));
    // Negative elapsed is clamped above, so the phase is never negative.
    const float phase = std::fmod(static_cast<float>(kRunStart - kRunFirst) + step, cycle);
    if (phase < static_cast<float>(kRunPositions))
        return {static_cast<float>(kRunFirst) + phase, true};
    return {static_cast<float>(kRunFirst) + (cycle - phase), false};
}

// The shade an unlit cell uses; the run's edge cells fade towards it.
::ftxui::Color UnlitColor(const style::Palette &pal)
{
    return pal.text_dim;
}

// A lit block, shaded by depth. |tier| 0 is the brightest (bold accent) and 4
// the darkest lit shade.
::ftxui::Element LitBlock(int tier, const style::Palette &pal)
{
    if (tier <= 2)
    {
        auto e = ::ftxui::text(style::kBlockGlyph) | ::ftxui::color(pal.accent);
        if (tier == 0)
            e = e | ::ftxui::bold;
        else if (tier == 2)
            e = e | ::ftxui::dim;
        return e;
    }
    auto e = ::ftxui::text(style::kBlockGlyph) | ::ftxui::color(pal.text_dim);
    if (tier >= 4)
        e = e | ::ftxui::dim;
    return e;
}

// A cell the run's edge is only part-way across: the shade is faded towards the
// unlit colour by how much of the cell is covered, so the head and tail dissolve
// in and out. |cover| is in (0, 1); whole cells never take this path.
::ftxui::Element PartialLitBlock(int tier, float cover, const style::Palette &pal)
{
    const ::ftxui::Color lit = (tier <= 2) ? pal.accent : pal.text_dim;
    auto e = ::ftxui::text(style::kBlockGlyph) |
             ::ftxui::color(::ftxui::Color::Interpolate(cover, UnlitColor(pal), lit));
    if (tier == 0)
        e = e | ::ftxui::bold;
    else if (tier == 2 || tier == 4)
        e = e | ::ftxui::dim;
    return e;
}

// An unlit cell: the small square that keeps the bar's full length visible, so
// the eye can see the run move rather than a floating blob.
::ftxui::Element OffBlock(const style::Palette &pal)
{
    return ::ftxui::text("⬝") | ::ftxui::color(pal.text_dim) | ::ftxui::dim;
}

} // namespace

::ftxui::Element WorkingIndicatorView::ElementAt(long long elapsed_ms)
{
    const auto &pal = style::palette();

    // BarAt clamps a negative elapsed (a clock adjustment between the start
    // stamp and the frame must not rewind the bar).
    const BarState bar = BarAt(elapsed_ms);
    const float run_begin = bar.pos;
    const float run_end = bar.pos + static_cast<float>(kBarLit);

    // The bar is the whole row: the animation alone says "working", so the row
    // carries no label and its width is just the bar's.
    ::ftxui::Elements row;
    row.reserve(static_cast<size_t>(kBarCells));
    for (int i = 0; i < kBarCells; i++)
    {
        // How much of cell |i| (the span [i, i+1]) the run currently covers.
        const float cover = std::min(static_cast<float>(i + 1), run_end) - std::max(static_cast<float>(i), run_begin);
        if (cover <= 0.0f)
        {
            row.push_back(OffBlock(pal));
            continue;
        }

        // Depth from the leading edge: the edge the run travels towards is the
        // brightest and the shade falls off behind it.
        const float into_run = static_cast<float>(i) - bar.pos;
        float depth = bar.forward ? (static_cast<float>(kBarLit - 1) - into_run) : into_run;
        depth = std::min(std::max(depth, 0.0f), static_cast<float>(kBarLit - 1));
        const int tier = static_cast<int>(std::lround(depth));

        // A whole cell draws its exact depth shade; a part-covered edge cell
        // fades that shade towards the unlit colour by its coverage.
        if (cover >= 1.0f)
            row.push_back(LitBlock(tier, pal));
        else
            row.push_back(PartialLitBlock(tier, cover, pal));
    }
    return ::ftxui::hbox(std::move(row));
}

WorkingIndicatorView::WorkingIndicatorView(std::shared_ptr<const port::IClock> clock)
    : clock_(clock ? std::move(clock) : port::SystemClock::Shared())
{
}

void WorkingIndicatorView::Restart()
{
    started_ = clock_->now();
    running_ = true;
}

void WorkingIndicatorView::Stop()
{
    running_ = false;
}

long long WorkingIndicatorView::ElapsedMs() const
{
    if (!running_)
        return 0;
    const auto delta = clock_->now() - started_;
    return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
}

::ftxui::Element WorkingIndicatorView::Draw(const Frame &)
{
    return ElementAt(ElapsedMs());
}

} // namespace edc::ui

namespace edc
{
// 旧入口: 薄委托, 与从前逐帧等价。
::ftxui::Element WorkingIndicatorElement(long long elapsed_ms)
{
    return ui::WorkingIndicatorView::ElementAt(elapsed_ms);
}
} // namespace edc
