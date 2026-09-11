// Tests for the animated "working" row (WorkingIndicatorElement).
//
// The row is a bar of blocks (■) with a run of lit cells that sweeps back and
// forth: it slides right until it has left the bar completely — tail and all,
// leaving a blank bar for a frame — and only then comes back, entering from
// the edge it left by. The depth of the lit cells flows along the run, the
// brightest block always leading. These tests pin down that behaviour, and
// just as importantly the things it must NOT do: turn around while it is still
// visible, jump, change the row's width, use the full-height █▓▒░ blocks, or
// paint a background band.
//
// Rendering note: Screen::ToString() drops all styling, so the assertions
// inspect Screen::CellAt() directly (character + foreground color + the
// bold/dim attributes) and compare colors against the palette roles.

#include "edc_style.hpp"
#include "edc_tui_window.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
    if (ok)
    {
        std::cout << "  ok   " << what << "\n";
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    g_failures++;
}

constexpr int kScreenWidth = 40;
constexpr int kBarCells = 10; // must match the row
constexpr int kBarLit = 5;
constexpr long long kStepMs = 120;     // must match the row
// The run travels from completely past the left edge to completely past the
// right edge, so the ends of the bar are real turning points.
constexpr int kRunFirst = -kBarLit; // -5: run fully off the left edge
constexpr int kRunLast = kBarCells; // 10: run fully off the right edge
constexpr int kRunPositions = kRunLast - kRunFirst + 1;
constexpr int kCycleSteps = 2 * (kRunPositions - 1); // 30 steps = 3.6s

std::vector<::ftxui::Cell> RenderRow(long long elapsed_ms)
{
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kScreenWidth),
                                          ::ftxui::Dimension::Fixed(1));
    ::ftxui::Render(screen, edc::WorkingIndicatorElement(elapsed_ms));
    std::vector<::ftxui::Cell> cells;
    cells.reserve(kScreenWidth);
    for (int x = 0; x < kScreenWidth; x++)
        cells.push_back(screen.CellAt(x, 0));
    return cells;
}

// Frame at a given step of the animation.
std::vector<::ftxui::Cell> FrameAtStep(int step)
{
    return RenderRow(static_cast<long long>(step) * kStepMs);
}

bool SameFrame(const std::vector<::ftxui::Cell> &a, const std::vector<::ftxui::Cell> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].character != b[i].character || a[i].foreground_color != b[i].foreground_color ||
            a[i].bold != b[i].bold || a[i].dim != b[i].dim)
            return false;
    }
    return true;
}

bool IsBarGlyph(const std::string &c)
{
    return c == "■" || c == "⬝";
}

// x positions of the bar cells (lit and unlit alike), in order.
std::vector<int> BarColumns(const std::vector<::ftxui::Cell> &cells)
{
    std::vector<int> out;
    for (int x = 0; x < kScreenWidth; x++)
        if (IsBarGlyph(cells[x].character))
            out.push_back(x);
    return out;
}

// Cell indices of the lit blocks, in order.
std::vector<int> LitCells(const std::vector<::ftxui::Cell> &cells)
{
    std::vector<int> out;
    for (int x = 0; x < kScreenWidth; x++)
        if (cells[x].character == "■")
            out.push_back(x);
    return out;
}

int LitCount(const std::vector<::ftxui::Cell> &cells)
{
    return static_cast<int>(LitCells(cells).size());
}

// A single number tracking where the light is: the sum of the ends of the lit
// run. It rises while the run travels right and falls while it travels left,
// and it is unchanged by the run shrinking at an edge.
int LightSum(const std::vector<::ftxui::Cell> &cells)
{
    const auto lit = LitCells(cells);
    if (lit.empty())
        return -1;
    return lit.front() + lit.back();
}

// Brightness depth of a lit block, 0 being the brightest (bold accent) and 4
// the darkest lit shade. Anything that is not a lit block scores -1.
int TierOf(const ::ftxui::Cell &cell, const edc::style::Palette &pal)
{
    if (cell.character != "■")
        return -1;
    const bool accent = cell.foreground_color == pal.accent;
    const bool gray = cell.foreground_color == pal.text_dim;
    if (accent && cell.bold && !cell.dim)
        return 0;
    if (accent && !cell.bold && !cell.dim)
        return 1;
    if (accent && !cell.bold && cell.dim)
        return 2;
    if (gray && !cell.bold && !cell.dim)
        return 3;
    if (gray && !cell.bold && cell.dim)
        return 4;
    return -1;
}

int BarX(const std::vector<::ftxui::Cell> &cells)
{
    const auto bar = BarColumns(cells);
    return bar.empty() ? -1 : bar.front();
}

// ---------------------------------------------------------------------------

void TestBarFrameIsFixed()
{
    std::cout << "the bar is always 10 cells and is the whole row\n";
    for (int step : {0, 6, 9, 10, 12, 20, 26})
    {
        const auto cells = FrameAtStep(step);
        Check(static_cast<int>(BarColumns(cells).size()) == kBarCells,
              "10 bar cells at step " + std::to_string(step));
        const auto lit = LitCells(cells);
        bool contiguous = true;
        for (size_t i = 1; i < lit.size(); i++)
            if (lit[i] != lit[i - 1] + 1)
                contiguous = false;
        Check(contiguous, "the visible lit blocks are contiguous at step " + std::to_string(step));
        std::string odd;
        for (int x = 0; x < kScreenWidth; x++)
        {
            const std::string &c = cells[x].character;
            if (!IsBarGlyph(c) && !c.empty() && c != " ")
                odd += "[x" + std::to_string(x) + "='" + c + "']";
        }
        Check(odd.empty(), "only bar glyphs, no label text, at step " + std::to_string(step) +
                               (odd.empty() ? "" : " (odd: " + odd + ")"));
    }
}

void TestRunSlidesOutAndComesBack()
{
    std::cout << "the run slides out of the bar and only then returns\n";
    // How many blocks are lit at each step of one full back-and-forth: the run
    // is fully visible for a while, then walks off the right edge, is gone for
    // one frame, comes back in from the right, and does the same on the left.
    const int expected[kCycleSteps] = {5, 5, 5, 5, 5, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4,
                                       5, 5, 5, 5, 5, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4};
    for (int step = 0; step < kCycleSteps; step++)
    {
        const int lit = LitCount(FrameAtStep(step));
        Check(lit == expected[step], "step " + std::to_string(step) + " has " +
                                         std::to_string(expected[step]) + " lit blocks (got " +
                                         std::to_string(lit) + ")");
    }
    Check(SameFrame(FrameAtStep(0), FrameAtStep(kCycleSteps)),
          "the cycle closes: frame at 3600ms equals frame at 0ms");
    Check(SameFrame(FrameAtStep(1), FrameAtStep(kCycleSteps + 1)),
          "and the frame after that matches too");
}

void TestTurnsAroundOnlyWhenOffScreen()
{
    std::cout << "the run is never on screen when it reverses\n";
    const int bar_x = BarX(FrameAtStep(0));
    // The two turning points: the bar is empty, because the run walked all the
    // way out — tail included — instead of bouncing off the edge.
    for (int step : {10, 25})
    {
        Check(LitCount(FrameAtStep(step)) == 0, "bar is empty at the turn (step " + std::to_string(step) +
                                                    ")");
    }
    // Exactly two empty frames per cycle, both of them the turning points.
    int empties = 0;
    for (int step = 0; step < kCycleSteps; step++)
        if (LitCount(FrameAtStep(step)) == 0)
            empties++;
    Check(empties == 2, "exactly two empty frames per cycle (got " + std::to_string(empties) + ")");
    // On the frame before each turn only the tail is left, hard against the
    // edge it is leaving by.
    const auto leaving_right = FrameAtStep(9);
    Check(LitCount(leaving_right) == 1 && LitCells(leaving_right)[0] == bar_x + kBarCells - 1,
          "just before the right turn only the tail is left, at the right edge");
    const auto leaving_left = FrameAtStep(24);
    Check(LitCount(leaving_left) == 1 && LitCells(leaving_left)[0] == bar_x,
          "just before the left turn only the tail is left, at the left edge");
    // And no reversal happens while any block is on screen. A blank frame is
    // the only place a change of direction is allowed, so it resets the
    // tracked direction.
    int direction = 0;
    for (int step = 0; step < 2 * kCycleSteps; step++)
    {
        const int a = LightSum(FrameAtStep(step));
        const int b = LightSum(FrameAtStep(step + 1));
        if (a < 0 || b < 0)
        {
            direction = 0; // the bar is empty: this is where the turn belongs
            continue;
        }
        if (b > a)
        {
            Check(direction >= 0, "no leftward reversal at step " + std::to_string(step));
            direction = 1;
        }
        else if (b < a)
        {
            Check(direction <= 0, "no rightward reversal at step " + std::to_string(step));
            direction = -1;
        }
    }
}

void TestNeverJumps()
{
    std::cout << "the run never jumps between frames\n";
    for (int step = 0; step < 2 * kCycleSteps; step++)
    {
        const int a = LitCount(FrameAtStep(step));
        const int b = LitCount(FrameAtStep(step + 1));
        Check(b >= a - 1 && b <= a + 1, "lit count moves by at most one, step " + std::to_string(step));
        const auto lit_a = LitCells(FrameAtStep(step));
        const auto lit_b = LitCells(FrameAtStep(step + 1));
        if (lit_a.empty() || lit_b.empty())
            continue;
        Check(lit_b.front() - lit_a.front() >= -1 && lit_b.front() - lit_a.front() <= 1,
              "the run's near edge moves by at most one, step " + std::to_string(step));
    }
}

void TestDepthLadderMatchesDirection()
{
    std::cout << "depth is a ladder, and it points the way the run travels\n";
    const edc::style::Palette &pal = edc::style::palette();
    for (int step = 0; step < kCycleSteps; step++)
    {
        const auto cells = FrameAtStep(step);
        const auto lit = LitCells(cells);
        if (lit.size() < 2)
            continue; // a lone block has no ladder to check
        bool ladder = true;
        for (size_t i = 1; i < lit.size(); i++)
            if (std::abs(TierOf(cells[lit[i]], pal) - TierOf(cells[lit[i - 1]], pal)) != 1)
                ladder = false;
        Check(ladder, "adjacent blocks differ by one depth step, step " + std::to_string(step));

        // Brightness falls off behind the leading edge, so within the visible
        // run the depth decreases towards the direction of travel.
        const int left = TierOf(cells[lit.front()], pal);
        const int right = TierOf(cells[lit.back()], pal);
        const int ramp = (right > left) ? -1 : 1; // -1: moving left, +1: moving right
        const int now = LightSum(cells);
        const int next = LightSum(FrameAtStep(step + 1));
        const int motion = (next > now) ? 1 : (next < now ? -1 : 0);
        if (motion != 0)
            Check(ramp == motion, "the gradient points the way the run moves at step " +
                                      std::to_string(step));
    }
}

void TestDepthChangesAsTheRunPassesOverACell()
{
    std::cout << "a fixed cell changes depth as the run passes over it\n";
    // This is the "flowing water" requirement: the colours must not just slide
    // with the run, they must deepen and lighten over time.
    const edc::style::Palette &pal = edc::style::palette();
    const int bar_x = BarX(FrameAtStep(0));
    for (int offset : {2, 4, 5, 7})
    {
        std::set<int> seen;
        for (int step = 0; step < kCycleSteps; step++)
        {
            const int tier = TierOf(FrameAtStep(step)[bar_x + offset], pal);
            if (tier >= 0)
                seen.insert(tier);
        }
        Check(seen.size() >= 3, "bar cell " + std::to_string(offset) + " shows " +
                                    std::to_string(seen.size()) + " different depths over a cycle");
    }
}

void TestRowWidthIsConstant()
{
    std::cout << "the row keeps its width in every frame\n";
    const auto reference = FrameAtStep(0);
    for (int step = 0; step < kCycleSteps; step++)
    {
        const auto cells = FrameAtStep(step);
        Check(BarColumns(cells) == BarColumns(reference),
              "bar columns unchanged at step " + std::to_string(step));
        Check(BarX(cells) == BarX(reference),
              "the bar starts at the same column at step " + std::to_string(step));
    }
}

void TestNeverUsesTheHeavyBlockGlyphs()
{
    std::cout << "never falls back to the heavy █▓▒░ blocks\n";
    // Those are full text-height and read as a solid wall when they animate.
    for (int step : {0, 1, 6, 10, 15, 20, 25})
    {
        const auto cells = FrameAtStep(step);
        bool heavy = false;
        for (int x = 0; x < kScreenWidth; x++)
        {
            const std::string &c = cells[x].character;
            if (c == "█" || c == "▓" || c == "▒" || c == "░")
                heavy = true;
        }
        Check(!heavy, "no █▓▒░ at step " + std::to_string(step));
    }
}

void TestOnlyPaletteRolesAndNoBackground()
{
    std::cout << "colours come from the palette, with no background band\n";
    const edc::style::Palette &pal = edc::style::palette();
    for (int step : {0, 5, 10, 12, 20, 24})
    {
        const auto cells = FrameAtStep(step);
        bool roles = true;
        bool no_bg = true;
        for (int x = 0; x < kScreenWidth; x++)
        {
            if (cells[x].background_color != ::ftxui::Color::Default)
                no_bg = false;
            if (IsBarGlyph(cells[x].character) && cells[x].foreground_color != pal.accent &&
                cells[x].foreground_color != pal.text_dim)
                roles = false;
        }
        Check(roles, "bar colours are accent/text_dim at step " + std::to_string(step));
        Check(no_bg, "no background band at step " + std::to_string(step));
    }
    // The unlit cells stay quiet so the moving run is what catches the eye.
    const auto cells = FrameAtStep(0);
    int off_dim = 0;
    for (int x : BarColumns(cells))
        if (cells[x].character == "⬝" && cells[x].dim && !cells[x].bold)
            off_dim++;
    Check(off_dim == kBarCells - kBarLit, "the 5 unlit cells are dim and not bold");
    // Even when the run is completely gone the bar's full length is drawn, so
    // the frame does not collapse or shift.
    const auto empty = FrameAtStep(10);
    Check(static_cast<int>(BarColumns(empty).size()) == kBarCells && LitCount(empty) == 0,
          "the empty frame still draws all 10 cells");
}

void TestRowHasNoLabel()
{
    std::cout << "the row is the bare bar: no trailing label\n";
    for (long long ms : {0LL, 120LL, 240LL, 1200LL})
    {
        const auto cells = RenderRow(ms);
        for (int x = kBarCells; x < kScreenWidth; x++)
            Check(cells[x].character.empty() || cells[x].character == " ",
                  "nothing after the bar at x=" + std::to_string(x) + " (t=" + std::to_string(ms) +
                      "ms)");
    }
    const auto a = FrameAtStep(0);
    Check(BarX(a) == 0, "the bar starts at the left edge of the row");
}

void TestNegativeElapsedIsClamped()
{
    std::cout << "a negative elapsed cannot rewind the bar\n";
    Check(SameFrame(RenderRow(-1), FrameAtStep(0)), "elapsed -1ms renders the first frame");
    Check(SameFrame(RenderRow(-1000), FrameAtStep(0)), "elapsed -1000ms renders the first frame");
}

void TestRowIsOneLineTall()
{
    std::cout << "the row is exactly one line tall\n";
    auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kScreenWidth),
                                          ::ftxui::Dimension::Fixed(3));
    ::ftxui::Render(screen, edc::WorkingIndicatorElement(240));
    bool overflow = false;
    for (int y = 1; y < 3; y++)
        for (int x = 0; x < kScreenWidth; x++)
            if (!screen.CellAt(x, y).character.empty() && screen.CellAt(x, y).character != " ")
                overflow = true;
    Check(!overflow, "nothing is drawn below the first line");
}

void TestSlidesBetweenCells()
{
    std::cout << "the run slides between cells instead of snapping\n";
    const edc::style::Palette &pal = edc::style::palette();
    // Half a step in, the run's leading edge sits half-way across a cell. That
    // edge cell carries a colour blended between the lit and unlit shades, so
    // the motion reads as a slide rather than a one-cell jump...
    const auto mid = RenderRow(kStepMs / 2);
    int blended = 0;
    for (int x : BarColumns(mid))
        if (mid[x].character == "■" && mid[x].foreground_color != pal.accent &&
            mid[x].foreground_color != pal.text_dim)
            blended++;
    Check(blended >= 1,
          "a mid-step edge cell carries a blended colour (got " + std::to_string(blended) + ")");
    // ...and that mid-step frame is really distinct from both whole-cell frames
    // it sits between.
    Check(!SameFrame(mid, FrameAtStep(0)) && !SameFrame(mid, FrameAtStep(1)),
          "the mid-step frame differs from both neighbouring whole-cell frames");
}

} // namespace

int main()
{
    TestBarFrameIsFixed();
    TestRunSlidesOutAndComesBack();
    TestTurnsAroundOnlyWhenOffScreen();
    TestNeverJumps();
    TestSlidesBetweenCells();
    TestDepthLadderMatchesDirection();
    TestDepthChangesAsTheRunPassesOverACell();
    TestRowWidthIsConstant();
    TestNeverUsesTheHeavyBlockGlyphs();
    TestOnlyPaletteRolesAndNoBackground();
    TestRowHasNoLabel();
    TestNegativeElapsedIsClamped();
    TestRowIsOneLineTall();

    if (g_failures == 0)
    {
        std::cout << "\nall working-status tests passed\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " working-status check(s) failed\n";
    return 1;
}
