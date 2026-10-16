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
                 CommandFact{"/session memo show [id]", "show the memory"},
                 CommandFact{"/session memo search <q>", "search the memory"},
                 CommandFact{"/session memo curate [focus]", "curate"},
                 CommandFact{"/session memo compact", "compact"},
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
        TEST("'/ta' identifies ONE family -> steps into its subcommands");
        CommandPalette p = Make();
        CHECK(p.Update("/ta"), "open");
        CHECK(p.items().size() == 3, "the /task subcommands, not the family row");
        CHECK(p.items()[0].text == "/task new <desc>   [--bg]", "first is new");
        CHECK(p.Update("/t"), "a shorter unique prefix too");
        CHECK(p.items().size() == 3, "same subcommands");
    }
    {
        TEST("'/h' identifies /help -> its single row, not the family");
        CommandPalette p = Make();
        CHECK(p.Update("/h"), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/help [<command>]", "the /help row");
    }
    {
        TEST("'/task ' steps into the subcommands");
        CommandPalette p = Make();
        CHECK(p.Update("/task "), "open");
        CHECK(p.items().size() == 3, "3 subcommands");
        CHECK(p.items()[0].text == "/task new <desc>   [--bg]", "first is new");
    }
    {
        TEST("'/session memo ' lists its subcommands (they are rows of their own)");
        CommandPalette p = Make();
        CHECK(p.Update("/session memo "), "open");
        CHECK(p.items().size() == 4, "4 subcommands");
        CHECK(p.items()[0].text == "/session memo show [id]", "first is show");
        CHECK(p.Accept() == "/session memo show ", "Tab lands inside 'show'");
        p.Update("/session memo show ");
        CHECK(!p.HasMore(), "nothing more to complete after that");
    }
    {
        TEST("Tab grows ONE word: '/session memo sh' -> '/session memo show '");
        CommandPalette p = Make();
        CHECK(p.Update("/session memo sh"), "open");
        CHECK(p.items().size() == 1, "one candidate");
        CHECK(p.Accept() == "/session memo show ", "exactly that word (got: " + p.Accept() + ")");
        p.Update("/session memo se");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/session memo search <q>", "still one");
        CHECK(p.Accept() == "/session memo search ", "and its word");
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
        TEST("Tab completes ONE word, not a whole row");
        CommandPalette p = Make();
        p.Update("/");
        CHECK(p.Accept() == "/help ", "no shared word (families differ) -> the highlighted row");
        p.Update("/ta");
        CHECK(p.Accept() == "/task ", "the family word + a space (got: " + p.Accept() + ")");
        p.Update("/task n");
        CHECK(p.Accept() == "/task new ", "one word further in (got: " + p.Accept() + ")");
        p.Update("/task b");
        CHECK(p.Accept() == "/task bg ", "drops [id] (got: " + p.Accept() + ")");
        p.Update("/hel");
        CHECK(p.Accept() == "/help ", "drops [<command>] (got: " + p.Accept() + ")");
        p.Update("/sess");
        CHECK(p.Accept() == "/session ", "the memo rows share their first word (got: " + p.Accept() + ")");
        p.Update("/task ");
        CHECK(p.Accept() == "/task new ", "next word is ambiguous -> the highlighted row");
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
        CHECK(p.HasMore(), "at '/task n' there is still something to complete");
        p.Update("/task new ");
        CHECK(p.open(), "the row still matches as a prefix");
        CHECK(p.Accept() == "/task new ", "but Tab would change nothing");
        CHECK(!p.HasMore(), "nothing left -> the same Tab also folds the list");
    }
    {
        TEST("HasMore: a family with pickable subcommands still has more");
        CommandPalette p = Make();
        p.Update("/task ");
        CHECK(p.HasMore(), "the subcommands would still change the text");
        p.Update("/session memo ");
        CHECK(p.HasMore(), "likewise for the memo subcommands");
        p.Update("/session memo show ");
        CHECK(!p.HasMore(), "landed inside 'show' -> nothing more to add");
        p.Update("/session memo compact");
        CHECK(p.HasMore(), "no trailing space yet: Tab would add it");
        p.Update("/session memo compact ");
        CHECK(!p.HasMore(), "then nothing more to add");
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
