#include "edc_shell.h"
#include "edc_agent.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

volatile sig_atomic_t _run_loop;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { printf("  TEST: %-55s ", name); } while (0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); test_failed++; } \
    else { printf("PASS\n"); test_passed++; } \
} while (0)

/* ── history expansion tests (!n, !!) ──────────────────────── */

static void test_history_expand_double_bang(void)
{
    TEST("history !! expansion");
    struct edc_agent *agent = calloc(1, sizeof(struct edc_agent));
    agent->history_capacity = 4;
    agent->history = malloc(4 * sizeof(sds));
    edc_agent_add_history(agent, "first cmd");
    edc_agent_add_history(agent, "second cmd");

    sds result = edc_shell_expand_history(agent, "!! after");
    CHECK(strcmp(result, "second cmd after") == 0, "!! expands to last command");
    sdsfree(result);

    for (int i = 0; i < agent->history_count; i++) sdsfree(agent->history[i]);
    free(agent->history);
    free(agent);
}

static void test_history_expand_nth(void)
{
    TEST("history !n expansion");
    struct edc_agent *agent = calloc(1, sizeof(struct edc_agent));
    agent->history_capacity = 4;
    agent->history = malloc(4 * sizeof(sds));
    edc_agent_add_history(agent, "cmd one");
    edc_agent_add_history(agent, "cmd two");
    edc_agent_add_history(agent, "cmd three");

    sds result = edc_shell_expand_history(agent, "!2 !3 !1");
    CHECK(strcmp(result, "cmd two cmd three cmd one") == 0, "!n positional expansion");
    sdsfree(result);

    for (int i = 0; i < agent->history_count; i++) sdsfree(agent->history[i]);
    free(agent->history);
    free(agent);
}

static void test_history_no_expand(void)
{
    TEST("no history expansion when none");
    struct edc_agent *agent = calloc(1, sizeof(struct edc_agent));

    sds result = edc_shell_expand_history(agent, "no history here");
    CHECK(strcmp(result, "no history here") == 0, "unchanged without history");
    sdsfree(result);

    free(agent);
}

static void test_history_quoted_bang(void)
{
    TEST("quoted ! is not expanded");
    struct edc_agent *agent = calloc(1, sizeof(struct edc_agent));
    agent->history_capacity = 4;
    agent->history = malloc(4 * sizeof(sds));
    edc_agent_add_history(agent, "should not appear");

    sds result = edc_shell_expand_history(agent, "\"!1\" or '!1'");
    CHECK(strcmp(result, "\"!1\" or '!1'") == 0, "quoted ! preserved");
    sdsfree(result);

    for (int i = 0; i < agent->history_count; i++) sdsfree(agent->history[i]);
    free(agent->history);
    free(agent);
}

static void test_history_out_of_range(void)
{
    TEST("history !n out of range");
    struct edc_agent *agent = calloc(1, sizeof(struct edc_agent));
    agent->history_capacity = 4;
    agent->history = malloc(4 * sizeof(sds));
    edc_agent_add_history(agent, "only one");

    sds result = edc_shell_expand_history(agent, "!5 not found");
    CHECK(strcmp(result, " not found") == 0, "!5 silently removed");
    sdsfree(result);

    for (int i = 0; i < agent->history_count; i++) sdsfree(agent->history[i]);
    free(agent->history);
    free(agent);
}

int main(void)
{
    printf("=== edc_shell history expansion tests ===\n\n");

    test_history_expand_double_bang();
    test_history_expand_nth();
    test_history_no_expand();
    test_history_quoted_bang();
    test_history_out_of_range();

    printf("\n=== Results: %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
