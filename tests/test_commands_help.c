/* `/help` is generated from the command table in edc_commands.c (edc_commands_help_text),
 * so this pins (a) the table's coverage of the command surface and (b) the lookup
 * behaviour of `/help <command>` (prefix / subcommand match, and the no-match message).
 *
 * It exists because the help text used to be a hand-written blob that drifted from the
 * dispatcher -- the table is now the single source of truth, and this is what keeps it so. */
#include "edc_commands.h"
#include "sds.h"

#include <stdio.h>
#include <string.h>

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
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

int main(void)
{
    printf("=== /help tests ===\n\n");

    sds full = edc_commands_help_text(NULL);

    TEST("full help lists every section");
    CHECK(has(full, "### Input") && has(full, "### General") && has(full, "### Session") && has(full, "### Task") &&
              has(full, "### Group"),
          "Input / General / Session / Task / Group");

    TEST("full help covers the command surface");
    CHECK(has(full, "/help [<command>]"), "/help itself");
    CHECK(has(full, "/whoami"), "/whoami");
    CHECK(has(full, "/model "), "/model");
    CHECK(has(full, "/connect"), "/connect");
    CHECK(has(full, "/prompt <name>"), "/prompt");
    CHECK(has(full, "/sandbox add-deny"), "/sandbox");
    /* One row per `/sandbox` verb: the completion palette inserts the row's literal
     * words, so a packed "add-ro|rm-ro" row both showed the two verbs as one and made
     * `rm-*` unreachable (the row text starts with "add-"). */
    CHECK(has(full, "/sandbox rm-ro <dir>") && has(full, "/sandbox rm-rw <dir>") && has(full, "/sandbox rm-deny <dir>"),
          "each rm-* verb has its own row (so it is completable)");
    CHECK(!has(full, "/sandbox add-ro|rm-ro") && !has(full, "/sandbox add-rw|rm-rw") &&
              !has(full, "/sandbox add-deny|rm-deny"),
          "and no row packs two verbs behind a `|`");
    CHECK(has(full, "/session memo"), "/session memo");
    CHECK(has(full, "/session status"), "/session status");
    CHECK(has(full, "/task history"), "/task history");
    CHECK(has(full, "--with"), "/task new --with");
    CHECK(has(full, "/group list"), "/group list (groups live here)");
    CHECK(has(full, "/group fold"), "/group fold");

    TEST("full help documents the `+` new-group form");
    CHECK(has(full, "+\"desc\""), "the new-group task form (its description is a quoted string)");
    CHECK(has(full, "a message: a task in the current group"),
          "and a line with no sigil is documented as a message");
    CHECK(has(full, "SHELL syntax: a string literal"),
          "...while a quote-led line is documented as shell syntax");
    CHECK(has(full, "--new-group"), "the long form is still documented, not replaced");
    CHECK(!has(full, "+/cmd") && !has(full, "+!cmd"),
          "and the retired `+`-composes-with-commands syntax is gone from the list");

    TEST("full help no longer lists /task groups");
    CHECK(!has(full, "/task groups"), "old spelling gone");

    TEST("full help does not mention history expansion");
    CHECK(!has(full, "!n") && !has(full, "history expansion"), "no !n / !! expansion");
    sdsfree(full);

    TEST("/help <prefix> returns the matching entries' detail");
    sds t = edc_commands_help_text("/task");
    CHECK(has(t, "/task new") && has(t, "/task fork") && has(t, "/task resume"), "/task entries");
    CHECK(!has(t, "/group list"), "unrelated entry excluded");
    sdsfree(t);

    TEST("/help <subcommand name> matches anywhere in the command");
    t = edc_commands_help_text("memo");
    CHECK(has(t, "/session memo") && has(t, "curate"), "memo detail");
    sdsfree(t);

    TEST("/help <sigil> reaches the `+` row's detail");
    t = edc_commands_help_text("+");
    CHECK(has(t, "does not combine with the command syntax"),
          "`/help +` explains that the two syntaxes are separate");
    CHECK(has(t, "its description is a quoted string"), "and that the description is a quoted string");
    sdsfree(t);

    TEST("/help <unknown> says so");
    t = edc_commands_help_text("frobnicate");
    CHECK(has(t, "No command matches"), "no-match message");
    sdsfree(t);

    TEST("every dispatchable command is documented (the dispatch table and /help cannot drift)");
    {
        const char *names[32];
        const int dn = edc_commands_table_names(names, 32);
        CHECK(dn >= 6, "the table carries the commands");
        for (int i = 0; i < dn; i++)
        {
            sds d = edc_commands_help_text(names[i]);
            const bool documented = d && d[0] && strstr(d, names[i]) != NULL;
            if (!documented)
                printf("[undocumented: %s] ", names[i]);
            CHECK(documented, names[i]);
            sdsfree(d);
        }
    }

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
