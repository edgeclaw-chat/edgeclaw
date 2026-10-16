/* The input's completion popup (model::CommandPalette): pure logic, no terminal and no
 * window. It is fed the input content and answers with the rows + the completed text, so
 * every rule (the '/' gate, family vs subcommand, Tab completion, Esc dismissal) is
 * pinned here rather than through a rendered frame. */
#include "edc_command_palette.hpp"
#include "edc_style.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <cstdio>
#include <string>
#include <vector>

using edc::model::CommandFact;
using edc::model::CommandPalette;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", std::string(m).c_str());                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static CommandPalette Make()
{
    CommandPalette p;
    p.SetSource({CommandFact{"/help", "help"}, CommandFact{"/task", "tasks"},
                 CommandFact{"/session", "sessions"}, CommandFact{"/group", "groups"}},
                {CommandFact{"/help [<command>]", "show help"},
                 CommandFact{"/task new <desc>   [--bg]", "create a task"},
                 CommandFact{"/task list", "list tasks"}, CommandFact{"/task bg [id]", "background"},
                 CommandFact{"/session memo [show [id] | search <q>]", "memory"},
                 CommandFact{"/group fold [id] [focus]", "fold"}});
    return p;
}

int main(void)
{
    printf("=== command palette tests ===\n\n");

    {
        TEST("bare '/' lists the families, not every row");
        CommandPalette p = Make();
        CHECK(p.Update("/"), "open");
        CHECK(p.items().size() == 4, "4 families");
        CHECK(p.items()[0].text == "/help" && p.items()[1].text == "/task", "family names, in order");
    }
    {
        TEST("'/ta' converges to the /task family");
        CommandPalette p = Make();
        CHECK(p.Update("/ta"), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/task", "one family row");
    }
    {
        TEST("'/task ' steps into the subcommands");
        CommandPalette p = Make();
        CHECK(p.Update("/task "), "open");
        CHECK(p.items().size() == 3, "3 subcommands");
        CHECK(p.items()[0].text == "/task new <desc>   [--bg]", "first is new");
    }
    {
        TEST("'/task n' narrows to a single subcommand");
        CommandPalette p = Make();
        CHECK(p.Update("/task n"), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/task new <desc>   [--bg]", "only new");
    }
    {
        TEST("no match closes the popup");
        CommandPalette p = Make();
        CHECK(!p.Update("/nope"), "closed");
        CHECK(!p.Update("/task new x"), "closed once the args start");
    }
    {
        TEST("only a single line starting with '/' opens it");
        CommandPalette p = Make();
        CHECK(!p.Update("hello"), "plain text closed");
        CHECK(!p.Update(""), "empty closed");
        CHECK(!p.Update("/task\nnew"), "multi-line closed");
    }
    {
        TEST("Tab completes the highlighted row");
        CommandPalette p = Make();
        p.Update("/ta");
        CHECK(p.Accept() == "/task ", "family row -> name + space");
        p.Update("/task n");
        CHECK(p.Accept() == "/task new ", "drops <desc>");
        p.Update("/task b");
        CHECK(p.Accept() == "/task bg ", "drops [id] (got: " + p.Accept() + ")");
        p.Update("/hel");
        CHECK(p.Accept() == "/help ", "drops [<command>] (got: " + p.Accept() + ")");
    }
    {
        TEST("Up/Down move the selection and clamp");
        CommandPalette p = Make();
        p.Update("/");
        CHECK(p.selected() == 0, "starts on the first row");
        p.MoveUp();
        CHECK(p.selected() == 0, "clamps at the top");
        p.MoveDown();
        p.MoveDown();
        CHECK(p.selected() == 2, "two steps down");
        for (int i = 0; i < 10; i++)
            p.MoveDown();
        CHECK(p.selected() == 3, "clamps at the bottom");
    }
    {
        TEST("Esc closes until the text changes again");
        CommandPalette p = Make();
        p.Update("/ta");
        p.Close();
        CHECK(!p.open(), "closed");
        CHECK(!p.Update("/ta"), "same text stays closed");
        CHECK(p.Update("/tas"), "a new keystroke re-opens");
    }
    {
        TEST("Tab with nothing left to complete yields the same text (=> confirm)");
        CommandPalette p = Make();
        p.Update("/task n");
        CHECK(p.Accept() == "/task new ", "advances into the args");
        p.Update("/task new ");
        CHECK(p.open(), "the row still matches as a prefix");
        CHECK(p.Accept() == "/task new ", "but Tab would change nothing");
    }
    {
        TEST("Tab on a closed popup is a no-op (keeps the text)");
        CommandPalette p = Make();
        p.Update("hello");
        CHECK(p.Accept() == "hello", "text unchanged");
    }
    {
        TEST("selected row = FOREGROUND highlight (accent fg + bold, no background bar)");
        // Colour assertions need real colour support declared (the test has no tty).
        ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::TrueColor);
        CommandPalette p = Make();
        p.Update("/task n"); // exactly one row, and it is the selected one
        auto screen = ::ftxui::Screen::Create(::ftxui::Dimension::Fixed(40), ::ftxui::Dimension::Fixed(3));
        ::ftxui::Render(screen, edc::ui::CommandPaletteElement(p, 3));

        const edc::style::Palette &pal = edc::style::palette();
        const ::ftxui::Cell sel = screen.CellAt(4, 0);  // the selected command row
        const ::ftxui::Cell hint = screen.CellAt(4, 1); // the hint row
        CHECK(sel.foreground_color == pal.accent && sel.bold, "selected: accent foreground + bold");
        CHECK(sel.background_color != pal.accent, "selected: NO accent background bar");
        CHECK(hint.foreground_color == pal.text_dim, "the hint row stays dim");
    }

    if (failed)
    {
        printf("\n%d failed\n", failed);
        return 1;
    }
    printf("\nall passed (%d)\n", passed);
    return 0;
}
