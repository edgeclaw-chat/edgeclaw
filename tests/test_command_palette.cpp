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
                 CommandFact{"/prompt", "list the templates"},
                 CommandFact{"/prompt <name> [args…]", "run a template"},
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
        TEST("'/prompt ' without a vocabulary falls back to the table's generic row");
        CommandPalette p = Make();
        CHECK(p.Update("/prompt "), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/prompt <name> [args…]", "the generic row");
        CHECK(!p.HasMore(), "and there is nothing to complete into");
    }
    {
        TEST("with a vocabulary, bare '/prompt' ALREADY lists the names (no space needed)");
        CommandPalette p = Make();
        p.SetArgSource("/prompt", {CommandFact{"/prompt review", "review the staged changes"},
                                   CommandFact{"/prompt fix", "fix the failing test"}});
        CHECK(p.Update("/prompt"), "open");
        bool has_review = false;
        bool has_fix = false;
        bool has_placeholder = false;
        bool has_bare = false;
        for (const CommandFact &it : p.items())
        {
            has_review = has_review || it.text == "/prompt review";
            has_fix = has_fix || it.text == "/prompt fix";
            has_placeholder = has_placeholder || it.text.find('<') != std::string::npos;
            has_bare = has_bare || it.text == "/prompt";
        }
        CHECK(has_review && has_fix, "both template names are in the list before any space");
        CHECK(!has_placeholder, "the '<name>' syntax-hint row is NOT a candidate");
        CHECK(has_bare, "the bare command word stays (it is runnable: it lists the templates)");
        CHECK(p.Accept() == "/prompt ", "Tab still finishes the command word first");
        CHECK(p.HasMore(), "and there is a level left to go");
    }
    {
        TEST("with a vocabulary, '/prompt ' lists the template NAMES and Tab completes one");
        CommandPalette p = Make();
        p.SetArgSource("/prompt", {CommandFact{"/prompt review", "review the staged changes"},
                                   CommandFact{"/prompt fix", "fix the failing test"}});
        CHECK(p.Update("/prompt "), "open");
        CHECK(p.items().size() == 2, "past the word the vocabulary is the whole list");
        CHECK(p.items()[0].text == "/prompt review", "rung up as a command line");
        CHECK(p.items()[0].brief == "review the staged changes", "its description rides along");

        CHECK(p.Update("/prompt re"), "open");
        CHECK(p.items().size() == 1, "narrowed");
        CHECK(p.Accept() == "/prompt review ", "Tab completes the name (got: " + p.Accept() + ")");

        CHECK(!p.Update("/prompt review "), "closed once the name is whole");
        CHECK(!p.Update("/prompt zz"), "an unknown name closes it too (no misleading generic row)");

        CHECK(p.Update("/task n"), "the command table is back in charge for other commands");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/task new <desc>   [--bg]", "the /task row");
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

    {
        TEST("`+/cmd` completes like `/cmd` and keeps the `+` in front");
        CommandPalette p = Make();
        CHECK(p.Update("+/ta"), "a `+/` line opens the popup the way `/` does");
        CHECK(p.Accept() == "+/task ", "Tab completes the word and re-attaches the prefix (got: " + p.Accept() + ")");

        p = Make();
        p.Update("+/session memo sh");
        CHECK(p.Accept() == "+/session memo show ", "and it walks the same rows (got: " + p.Accept() + ")");

        p = Make();
        p.Update("+/");
        CHECK(p.Accept().rfind("+/", 0) == 0, "a lone '+/' lists the families, and keeps the `+`");

        p = Make();
        p.Update("+/help ");
        CHECK(p.Accept() == "+/help ", "with nothing left to complete the text is unchanged");
        CHECK(!p.HasMore(), "so the same Tab folds the list");
    }
    {
        TEST("a bare `+` (no command behind it) does not open the popup");
        CommandPalette p = Make();
        CHECK(!p.Update("+"), "just the prefix: nothing to complete");
        CHECK(!p.Update("+find bugs"), "a new-group description is not a command line");
    }

    if (failed)
    {
        printf("\n%d failed\n", failed);
        return 1;
    }
    printf("\nall passed (%d)\n", passed);
    return 0;
}
