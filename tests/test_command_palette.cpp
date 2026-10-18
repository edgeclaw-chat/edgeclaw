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

// The palette takes the caret now; every case here types at the end of the line.
static bool Upd(CommandPalette &p, const std::string &text)
{
    return p.Update(text, static_cast<int>(text.size()));
}

static void test_mention_trigger(void)
{
    using Hit = CommandPalette::MentionHit;
    auto at_end = [](const std::string &text) {
        return CommandPalette::ParseMention(text, static_cast<int>(text.size()));
    };

    TEST("@ alone at the line start opens with an empty query");
    Hit h = at_end("@");
    CHECK(h.active && h.begin == 0 && h.query.empty(), "active at 0, empty query");

    TEST("@ after a space: the query is what follows");
    h = at_end("look @sr");
    CHECK(h.active && h.begin == 5 && h.query == "sr", "begin 5, query 'sr'");
    h = at_end("\xe4\xb8\xad\xe6\x96\x87 @sr"); /* 中文 @sr */
    CHECK(h.active && h.query == "sr", "a multi-byte character before the space is fine");

    TEST("@ inside a word does not trigger (emails, decorators, pasted code)");
    h = at_end("foo@bar");
    CHECK(!h.active, "foo@bar is not a reference");
    h = at_end("\xe4\xb8\xad\xe6\x96\x87@sr"); /* 中文@sr */
    CHECK(!h.active, "and neither is it right after a character");

    TEST("the caret decides: the query stops there");
    h = CommandPalette::ParseMention("@src/main", 4);
    CHECK(h.active && h.begin == 0 && h.query == "src", "caret after 'src' -> query 'src'");
    h = CommandPalette::ParseMention("@src/main", 0);
    CHECK(!h.active, "caret at 0 has nothing before it");

    TEST("a space between @ and the caret closes it");
    h = at_end("@src file");
    CHECK(!h.active, "the caret is past a space");

    TEST("a newline counts as whitespace before the @");
    h = at_end("first line\n@x");
    CHECK(h.active && h.query == "x", "line start after a newline");

    TEST("the LAST @ wins");
    h = at_end("@a @b");
    CHECK(h.active && h.begin == 3 && h.query == "b", "begin 3, query 'b'");
}

int main(void)
{
    printf("=== command palette tests ===\n\n");
    test_mention_trigger();

    {
        TEST("bare '/' lists the families, not every row");
        CommandPalette p = Make();
        CHECK(Upd(p, "/"), "open");
        CHECK(p.items().size() == 4, "4 families");
        CHECK(p.items()[0].text == "/help" && p.items()[1].text == "/task", "family names, in order");
    }
    {
        TEST("'/ta' identifies ONE family -> steps into its subcommands");
        CommandPalette p = Make();
        CHECK(Upd(p, "/ta"), "open");
        CHECK(p.items().size() == 3, "the /task subcommands, not the family row");
        CHECK(p.items()[0].text == "/task new <desc>   [--bg]", "first is new");
        CHECK(Upd(p, "/t"), "a shorter unique prefix too");
        CHECK(p.items().size() == 3, "same subcommands");
    }
    {
        TEST("'/h' identifies /help -> its single row, not the family");
        CommandPalette p = Make();
        CHECK(Upd(p, "/h"), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/help [<command>]", "the /help row");
    }
    {
        TEST("'/task ' steps into the subcommands");
        CommandPalette p = Make();
        CHECK(Upd(p, "/task "), "open");
        CHECK(p.items().size() == 3, "3 subcommands");
        CHECK(p.items()[0].text == "/task new <desc>   [--bg]", "first is new");
    }
    {
        TEST("'/session memo ' lists its subcommands (they are rows of their own)");
        CommandPalette p = Make();
        CHECK(Upd(p, "/session memo "), "open");
        CHECK(p.items().size() == 4, "4 subcommands");
        CHECK(p.items()[0].text == "/session memo show [id]", "first is show");
        CHECK(p.Accept() == "/session memo show ", "Tab lands inside 'show'");
        Upd(p, "/session memo show ");
        CHECK(!p.HasMore(), "nothing more to complete after that");
    }
    {
        TEST("Tab grows ONE word: '/session memo sh' -> '/session memo show '");
        CommandPalette p = Make();
        CHECK(Upd(p, "/session memo sh"), "open");
        CHECK(p.items().size() == 1, "one candidate");
        CHECK(p.Accept() == "/session memo show ", "exactly that word (got: " + p.Accept() + ")");
        Upd(p, "/session memo se");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/session memo search <q>", "still one");
        CHECK(p.Accept() == "/session memo search ", "and its word");
    }
    {
        TEST("'/task n' narrows to a single subcommand");
        CommandPalette p = Make();
        CHECK(Upd(p, "/task n"), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/task new <desc>   [--bg]", "only new");
    }
    {
        TEST("no match closes the popup");
        CommandPalette p = Make();
        CHECK(!Upd(p, "/nope"), "closed");
        CHECK(!Upd(p, "/task new x"), "closed once the args start");
    }
    {
        TEST("only a single line starting with '/' opens it");
        CommandPalette p = Make();
        CHECK(!Upd(p, "hello"), "plain text closed");
        CHECK(!Upd(p, ""), "empty closed");
        CHECK(!Upd(p, "/task\nnew"), "multi-line closed");
    }
    {
        TEST("Tab completes ONE word, not a whole row");
        CommandPalette p = Make();
        Upd(p, "/");
        CHECK(p.Accept() == "/help ", "no shared word (families differ) -> the highlighted row");
        Upd(p, "/ta");
        CHECK(p.Accept() == "/task ", "the family word + a space (got: " + p.Accept() + ")");
        Upd(p, "/task n");
        CHECK(p.Accept() == "/task new ", "one word further in (got: " + p.Accept() + ")");
        Upd(p, "/task b");
        CHECK(p.Accept() == "/task bg ", "drops [id] (got: " + p.Accept() + ")");
        Upd(p, "/hel");
        CHECK(p.Accept() == "/help ", "drops [<command>] (got: " + p.Accept() + ")");
        Upd(p, "/sess");
        CHECK(p.Accept() == "/session ", "the memo rows share their first word (got: " + p.Accept() + ")");
        Upd(p, "/task ");
        CHECK(p.Accept() == "/task new ", "next word is ambiguous -> the highlighted row");
    }
    {
        TEST("Up/Down move the selection and clamp");
        CommandPalette p = Make();
        Upd(p, "/");
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
        Upd(p, "/ta");
        p.Close();
        CHECK(!p.open(), "closed");
        CHECK(!Upd(p, "/ta"), "same text stays closed");
        CHECK(Upd(p, "/tas"), "a new keystroke re-opens");
    }
    {
        TEST("Tab with nothing left to complete yields the same text (=> confirm)");
        CommandPalette p = Make();
        Upd(p, "/task n");
        CHECK(p.Accept() == "/task new ", "advances into the args");
        CHECK(p.HasMore(), "at '/task n' there is still something to complete");
        Upd(p, "/task new ");
        CHECK(p.open(), "the row still matches as a prefix");
        CHECK(p.Accept() == "/task new ", "but Tab would change nothing");
        CHECK(!p.HasMore(), "nothing left -> the same Tab also folds the list");
    }
    {
        TEST("HasMore: a family with pickable subcommands still has more");
        CommandPalette p = Make();
        Upd(p, "/task ");
        CHECK(p.HasMore(), "the subcommands would still change the text");
        Upd(p, "/session memo ");
        CHECK(p.HasMore(), "likewise for the memo subcommands");
        Upd(p, "/session memo show ");
        CHECK(!p.HasMore(), "landed inside 'show' -> nothing more to add");
        Upd(p, "/session memo compact");
        CHECK(p.HasMore(), "no trailing space yet: Tab would add it");
        Upd(p, "/session memo compact ");
        CHECK(!p.HasMore(), "then nothing more to add");
    }
    {
        TEST("'/prompt ' without a vocabulary falls back to the table's generic row");
        CommandPalette p = Make();
        CHECK(Upd(p, "/prompt "), "open");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/prompt <name> [args…]", "the generic row");
        CHECK(!p.HasMore(), "and there is nothing to complete into");
    }
    {
        TEST("with a vocabulary, bare '/prompt' ALREADY lists the names (no space needed)");
        CommandPalette p = Make();
        p.SetArgSource("/prompt", {CommandFact{"/prompt review", "review the staged changes"},
                                   CommandFact{"/prompt fix", "fix the failing test"}});
        CHECK(Upd(p, "/prompt"), "open");
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
        CHECK(Upd(p, "/prompt "), "open");
        CHECK(p.items().size() == 2, "past the word the vocabulary is the whole list");
        CHECK(p.items()[0].text == "/prompt review", "rung up as a command line");
        CHECK(p.items()[0].brief == "review the staged changes", "its description rides along");

        CHECK(Upd(p, "/prompt re"), "open");
        CHECK(p.items().size() == 1, "narrowed");
        CHECK(p.Accept() == "/prompt review ", "Tab completes the name (got: " + p.Accept() + ")");

        CHECK(!Upd(p, "/prompt review "), "closed once the name is whole");
        CHECK(!Upd(p, "/prompt zz"), "an unknown name closes it too (no misleading generic row)");

        CHECK(Upd(p, "/task n"), "the command table is back in charge for other commands");
        CHECK(p.items().size() == 1 && p.items()[0].text == "/task new <desc>   [--bg]", "the /task row");
    }
    {
        TEST("Tab on a closed popup is a no-op (keeps the text)");
        CommandPalette p = Make();
        Upd(p, "hello");
        CHECK(p.Accept() == "hello", "text unchanged");
    }
    {
        TEST("selected row = FOREGROUND highlight (accent fg + bold, no background bar)");
        // Colour assertions need real colour support declared (the test has no tty).
        ::ftxui::Terminal::SetColorSupport(::ftxui::Terminal::TrueColor);
        CommandPalette p = Make();
        Upd(p, "/task n"); // exactly one row, and it is the selected one
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
        TEST("a bare `+` (no command behind it) does not open the popup");
        CommandPalette p = Make();
        CHECK(!Upd(p, "+"), "just the prefix: nothing to complete");
        CHECK(!Upd(p, "+find bugs"), "a new-group description is not a command line");
        CHECK(!Upd(p, "+/task"), "`+` does not combine with the command syntax, so `+/...` is not a command line");
    }

    if (failed)
    {
        printf("\n%d failed\n", failed);
        return 1;
    }
    printf("\nall passed (%d)\n", passed);
    return 0;
}
