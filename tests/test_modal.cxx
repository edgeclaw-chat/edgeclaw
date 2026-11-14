/* tests/test_modal.cxx -- ModalOverlay: a borderless panel that still reads as a rectangle.
 *
 * Two things broke it before, and both were about what sat NEXT to the panel rather than the
 * panel itself:
 *
 *   - the ground alternated (the chat switches between the message surface and the gap between
 *     messages), so the panel's dead-straight edge met a striped neighbour;
 *   - a FULLWIDTH character in the cell just left of the panel occupies two columns, and
 *     ftxui's Screen::ToString() drops the cell after a wide character -- so the panel's first
 *     column was not covered, it was never emitted. That row came out one column short and the
 *     whole panel shifted left. Measured on a probe before it was fixed: the panel's marker
 *     character did not appear in the output at all.
 *
 * The second one needs the display column of the panel's first visible character, not its cell
 * index: the terminal lays out a CJK glyph over two columns, and that is where the shift shows.
 */

#include "edc_modal.hpp"
#include "edc_style.hpp"

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include <stdio.h>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: %s\n", msg);                                                                    \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: %s\n", msg);                                                                               \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{

std::string StripSgr(const std::string &s)
{
    std::string out;
    bool esc = false;
    for (char c : s)
    {
        if (esc)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                esc = false;
            continue;
        }
        if (c == 0x1b)
        {
            esc = true;
            continue;
        }
        out += c;
    }
    return out;
}

/* The screen's text as rows, without SGR or carriage returns. */
std::vector<std::string> Rows(const ::ftxui::Screen &screen)
{
    const std::string all = StripSgr(screen.ToString());
    std::vector<std::string> rows;
    std::string cur;
    for (size_t i = 0; i < all.size(); i++)
    {
        if (all[i] == '\r')
            continue;
        if (all[i] == '\n')
        {
            rows.push_back(cur);
            cur.clear();
            continue;
        }
        cur += all[i];
    }
    rows.push_back(cur);
    return rows;
}

/* Display column (0-based) of the first occurrence of |needle| in |row|. */
int DisplayColumnOf(const std::string &row, const std::string &needle)
{
    int col = 0;
    for (size_t i = 0; i < row.size();)
    {
        const unsigned char b = static_cast<unsigned char>(row[i]);
        int width = 1;
        size_t bytes = 1;
        if (b >= 0xF0)
        {
            width = 2;
            bytes = 4;
        }
        else if (b >= 0xE0)
        {
            width = 2; /* CJK: two columns */
            bytes = 3;
        }
        else if (b >= 0xC0)
        {
            width = 1;
            bytes = 2;
        }
        /* needle.size(), not `bytes`: `bytes` is how long the character under the cursor is, so
         * comparing that many bytes against a longer needle never matches -- the first version of
         * this helper reported the marker missing on every row. */
        if (row.compare(i, needle.size(), needle) == 0)
            return col;
        col += width;
        i += bytes;
    }
    return -1;
}

constexpr int kWidth = 100;
constexpr int kHeight = 14;
constexpr int kPanelWidth = 66;

/* Render a backdrop whose one text row starts with |prefix| ASCII characters followed by CJK,
 * with a panel over it, and hand the screen to |fn| while it is still in scope.
 *
 * Deliberately not returning the Screen: a Screen copied by value does not carry its cells, so a
 * helper that returns one reads an empty frame -- which is how the first version of this test
 * "found" the marker missing on every row. */
template <typename Fn> void RenderInto(int prefix, Fn fn)
{
    auto &pal = edc::style::palette();

    std::string line(static_cast<size_t>(prefix), 'a');
    line += "这是一个很长的中文消息用来测试宽字符边界";

    ::ftxui::Element backdrop =
        ::ftxui::vbox({::ftxui::filler(), ::ftxui::text(line), ::ftxui::filler()}) | ::ftxui::dim;

    auto panel = ::ftxui::vbox({::ftxui::text("|PANEL")}) | ::ftxui::size(::ftxui::WIDTH, ::ftxui::EQUAL, kPanelWidth) |
                 ::ftxui::bgcolor(pal.dialog_bg);

    ::ftxui::Screen screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(kWidth), ::ftxui::Dimension::Fixed(kHeight));
    ::ftxui::Render(screen, edc::ModalOverlay(std::move(backdrop), std::move(panel), kPanelWidth, pal.window_bg,
                                              pal.dialog_bg));
    fn(screen);
}

std::vector<std::string> RenderRows(int prefix)
{
    std::vector<std::string> rows;
    RenderInto(prefix, [&](::ftxui::Screen &screen) { rows = Rows(screen); });
    return rows;
}

} // namespace

int main(void)
{
    printf("\ntest_modal\n");

    printf("\n── the panel's left edge does not move with the text under it ──\n");
    {
        /* Prefix lengths 14..17 put the CJK glyph at every parity relative to the panel's edge
         * at column 17: with 16 ASCII characters the first glyph starts exactly at 16 and spans
         * 16-17, which is the case that used to swallow the edge. */
        int expected = -1;
        bool all_same = true;
        bool found_any = true;
        for (int prefix = 14; prefix <= 17; prefix++)
        {
            const auto rows = RenderRows(prefix);
            int col = -1;
            for (const std::string &row : rows)
            {
                col = DisplayColumnOf(row, "|PANEL");
                if (col >= 0)
                    break;
            }
            if (col < 0)
                found_any = false;
            if (expected < 0)
                expected = col;
            if (col != expected)
                all_same = false;
            printf("    prefix=%2d -> panel marker at display column %d\n", prefix, col);
        }
        CHECK(found_any, "the panel marker is actually rendered (not swallowed by a wide char)");
        CHECK(all_same, "its display column is the same whatever the text before it is");
        CHECK(expected == 17, "and that column is where centring puts it");
    }

    printf("\n── the ground outside the panel is one colour ──\n");
    {
        const auto &pal = edc::style::palette();
        int others = -1;
        RenderInto(16, [&](::ftxui::Screen &screen) {
            /* Row 0 is outside the panel: it holds one line of content, vertically centred. */
            others = 0;
            for (int x = 0; x < kWidth; x++)
                if (screen.CellAt(x, 0).background_color != pal.window_bg)
                    others++;
        });
        CHECK(others == 0, "every cell of a row outside the panel is the ground colour");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
