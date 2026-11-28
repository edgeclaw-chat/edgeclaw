/* test_highlight_fold: the "..." of a collapsed, syntax-highlighted bubble sits
 * at the end of the preview (the line just above "+Click to expand"), not as a
 * stray right-hand column.
 *
 * A long tool output is collapsed to kPreviewLines lines plus a "..." marker and
 * a "+Click to expand" footer. For an UNHIGHLIGHTED body the marker is appended to
 * the text ("WrappedText(preview + "...")"), so it lands on the last preview line
 * right above the footer. A HIGHLIGHTED body used to put "..." in an hbox beside
 * `HighlightedCode(preview)|flex`; the flexed code stretched to full width and
 * shoved the dots to the far right of that row, reading like a new column. The
 * fix appends "..." to the highlighted text instead, matching the plain path.
 *
 * This test renders a real collapsed, language-tagged ToolMessage and checks the
 * marker's row and column against the footer. It is RED against the hbox version
 * (dots pushed to the right edge of the code block) and GREEN with the fix.
 */

#include "edc_message.hpp"
#include "edc_style.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include <cstdio>
#include <string>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            printf("  ok   %s\n", msg);                                                                                \
            g_passed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("  FAIL %s\n", msg);                                                                                \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

static const int kW = 80;
static const int kH = 40;

/* Strip SGR escapes so row/column maths sees only printed text. */
static std::string Plain(const std::string &frame)
{
    std::string out;
    bool esc = false;
    for (char c : frame)
    {
        if (esc)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                esc = false;
            continue;
        }
        if (c == '\x1b')
        {
            esc = true;
            continue;
        }
        out += c;
    }
    return out;
}

struct Hit
{
    int row;
    int col;
    bool found;
};

static Hit Find(const std::string &frame, const std::string &needle)
{
    const std::string plain = Plain(frame);
    int row = 0;
    size_t pos = 0;
    while (pos <= plain.size())
    {
        const size_t end = plain.find('\n', pos);
        const std::string line = end == std::string::npos ? plain.substr(pos) : plain.substr(pos, end - pos);
        const size_t at = line.find(needle);
        if (at != std::string::npos)
            return {row, (int)at, true};
        if (end == std::string::npos)
            break;
        row++;
        pos = end + 1;
    }
    return {-1, -1, false};
}

int main(void)
{
    /* 20 short code lines -> over kPreviewLines (14), so the bubble collapses. Each line
     * is short enough to fit well inside kW, so wrapping cannot move the marker around. */
    std::string code;
    for (int i = 0; i < 20; i++)
        code += "int x" + std::to_string(i) + " = " + std::to_string(i) + ";\n";

    auto bubble = edc::MakeChatBubble("tool", "read_file", code, "", /*render_pref=*/0, /*ts=*/0, /*lang=*/"cpp");
    bubble->SetRenderWidth(kW);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kW), ftxui::Dimension::Fixed(kH));
    ftxui::Render(screen, bubble->Render());
    const std::string frame = screen.ToString();

    const Hit footer = Find(frame, "Click to expand");
    CHECK(footer.found, "collapsed highlighted bubble has a +Click to expand footer");

    const Hit dots = Find(frame, "...");
    CHECK(dots.found, "collapsed highlighted bubble shows a ... marker");

    if (footer.found && dots.found)
    {
        CHECK(dots.row == footer.row - 1,
              "... is on the line just above the expand footer (not the same row / a right column)");
        const std::string plain = Plain(frame);
        const size_t nl = plain.find('\n', plain.find("..."));
        // A trailing right-hand column would sit at the far edge; the appended marker does not.
        CHECK(dots.col < kW - 10, "... is not pinned to the far right column");
        (void)nl;
    }

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
