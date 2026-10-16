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
    CHECK(has(full, "/session memo"), "/session memo");
    CHECK(has(full, "/session status"), "/session status");
    CHECK(has(full, "/task history"), "/task history");
    CHECK(has(full, "--with"), "/task new --with");
    CHECK(has(full, "/group list"), "/group list (groups live here)");
    CHECK(has(full, "/group fold"), "/group fold");

    TEST("full help documents the `+` new-group forms");
    CHECK(has(full, "+desc"), "the new-group task form");
    CHECK(has(full, "+/cmd"), "the composition with / commands");
    CHECK(has(full, "+!cmd"), "the composition with the local shell");
    CHECK(has(full, "--new-group"), "the long form is still documented, not replaced");

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

    TEST("/help <sigil> reaches the `+` rows' detail");
    t = edc_commands_help_text("+/cmd");
    CHECK(has(t, "composes with the other input sigils"), "`/help +/cmd` prints the composition detail");
    sdsfree(t);

    TEST("/help <unknown> says so");
    t = edc_commands_help_text("frobnicate");
    CHECK(has(t, "No command matches"), "no-match message");
    sdsfree(t);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
