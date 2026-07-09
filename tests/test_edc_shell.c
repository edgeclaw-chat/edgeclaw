#include "src/edc_shell.h"
#include "src/edc_agent.h"
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

static void free_segments(sds *segs, int count)
{
    for (int i = 0; i < count; i++) sdsfree(segs[i]);
    free(segs);
}

static void check_segment(sds *segs, int idx, const char *expected, const char *label)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected '%s'", label, expected);
    CHECK(idx >= 0 && strcmp(segs[idx], expected) == 0, buf);
}

/* ── pipeline splitter tests ──────────────────────────────── */

static void test_split_single(void)
{
    TEST("single unquoted word");
    int n;
    sds *s = edc_shell_split_pipeline("hello", &n);
    CHECK(n == 1, "count = 1");
    check_segment(s, 0, "hello", "segment[0]");
    free_segments(s, n);
}

static void test_split_simple_pipe(void)
{
    TEST("simple pipe a | b");
    int n;
    sds *s = edc_shell_split_pipeline("a | b", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    free_segments(s, n);
}

static void test_split_multi_pipe(void)
{
    TEST("multi pipe a | b | c");
    int n;
    sds *s = edc_shell_split_pipeline("a | b | c", &n);
    CHECK(n == 3, "count = 3");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    check_segment(s, 2, "c", "segment[2]");
    free_segments(s, n);
}

static void test_split_quoted_pipe_dq(void)
{
    TEST("quoted pipe in double quotes \"a | b\"");
    int n;
    sds *s = edc_shell_split_pipeline("\"a | b\" | c", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a | b", "\"a | b\" → a | b");
    check_segment(s, 1, "c", "segment[1]");
    free_segments(s, n);
}

static void test_split_quoted_pipe_sq(void)
{
    TEST("quoted pipe in single quotes 'a | b'");
    int n;
    sds *s = edc_shell_split_pipeline("'a | b' | c", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a | b", "'a | b' → a | b");
    check_segment(s, 1, "c", "segment[1]");
    free_segments(s, n);
}

static void test_split_dq_escape(void)
{
    TEST("double-quote escape \\\"");
    int n;
    sds *s = edc_shell_split_pipeline("\"he said \\\"hi\\\"\" | bye", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "he said \"hi\"", "escaped quotes");
    check_segment(s, 1, "bye", "segment[1]");
    free_segments(s, n);
}

static void test_split_dq_backslash(void)
{
    TEST("double-quote backslash \\\\");
    int n;
    sds *s = edc_shell_split_pipeline("\"path\\\\to\\\\file\" | next", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "path\\to\\file", "backslashes preserved");
    check_segment(s, 1, "next", "segment[1]");
    free_segments(s, n);
}

static void test_split_mixed_quotes(void)
{
    TEST("mixed double and single quotes");
    int n;
    sds *s = edc_shell_split_pipeline("\"hello\" | 'world'", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "hello", "\"hello\"");
    check_segment(s, 1, "world", "'world'");
    free_segments(s, n);
}

static void test_split_pipe_at_start(void)
{
    TEST("pipe at start | a");
    int n;
    sds *s = edc_shell_split_pipeline("| a", &n);
    CHECK(n == 2, "count = 2");
    CHECK(sdslen(s[0]) == 0, "segment[0] empty");
    check_segment(s, 1, "a", "segment[1]");
    free_segments(s, n);
}

static void test_split_pipe_at_end(void)
{
    TEST("pipe at end a |");
    int n;
    sds *s = edc_shell_split_pipeline("a |", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0]");
    CHECK(sdslen(s[1]) == 0, "segment[1] empty");
    free_segments(s, n);
}

static void test_split_consecutive_pipes(void)
{
    TEST("consecutive pipes a || b");
    int n;
    sds *s = edc_shell_split_pipeline("a || b", &n);
    CHECK(n == 3, "count = 3");
    check_segment(s, 0, "a", "segment[0]");
    CHECK(sdslen(s[1]) == 0, "segment[1] empty");
    check_segment(s, 2, "b", "segment[2]");
    free_segments(s, n);
}

static void test_split_whitespace(void)
{
    TEST("whitespace trimming");
    int n;
    sds *s = edc_shell_split_pipeline("  a  |  b  ", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0] trimmed");
    check_segment(s, 1, "b", "segment[1] trimmed");
    free_segments(s, n);
}

static void test_split_no_spaces_pipe(void)
{
    TEST("pipe without spaces a|b");
    int n;
    sds *s = edc_shell_split_pipeline("a|b", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    free_segments(s, n);
}

static void test_split_empty_input(void)
{
    TEST("empty input");
    int n;
    sds *s = edc_shell_split_pipeline("", &n);
    CHECK(n == 1, "count = 1");
    CHECK(sdslen(s[0]) == 0, "segment[0] empty");
    free_segments(s, n);
}

static void test_split_only_whitespace(void)
{
    TEST("only whitespace");
    int n;
    sds *s = edc_shell_split_pipeline("   \t  ", &n);
    CHECK(n == 1, "count = 1");
    CHECK(sdslen(s[0]) == 0, "segment[0] empty after trim");
    free_segments(s, n);
}

static void test_split_flags_in_segments(void)
{
    TEST("flags preserved in segments");
    int n;
    sds *s = edc_shell_split_pipeline("hello --bg | --prompt foo world", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "hello --bg", "segment[0]");
    check_segment(s, 1, "--prompt foo world", "segment[1]");
    free_segments(s, n);
}

static void test_split_ampersand_at_end(void)
{
    TEST("ampersand at end preserved");
    int n;
    sds *s = edc_shell_split_pipeline("hello world &", &n);
    CHECK(n == 1, "count = 1");
    check_segment(s, 0, "hello world &", "& preserved for option parser");
    free_segments(s, n);
}

static void test_split_sq_backslash_literal(void)
{
    TEST("single quotes: backslash is literal");
    int n;
    /* Input: 'a\\b' | c
     * single-quoted part: a\b (backslash is literal, NOT an escape)
     * then unquoted | c */
    sds *s = edc_shell_split_pipeline("'a\\b' | c", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a\\b", "literal backslash preserved");
    check_segment(s, 1, "c", "segment[1]");
    free_segments(s, n);
}

static void test_split_unterminated_dquote(void)
{
    TEST("unterminated double quote");
    int n;
    sds *s = edc_shell_split_pipeline("\"hello world", &n);
    CHECK(n == 1, "count = 1");
    /* unterminated quote: everything after " goes into cur until EOF */
    check_segment(s, 0, "hello world", "content after \"");
    free_segments(s, n);
}

/* ── redirect stripping tests ─────────────────────────────── */

static void test_redirect_stdout(void)
{
    TEST("redirect > file");
    sds seg = sdsnew("hello > out.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(fout != NULL && strcmp(fout, "out.txt") == 0, "redirect_stdout = out.txt");
    CHECK(!append, "not append");
    CHECK(ferr == NULL, "no stderr");
    CHECK(fin == NULL, "no input file");
    sdsfree(seg); if (fout) sdsfree(fout);
}

static void test_redirect_append(void)
{
    TEST("redirect >> file");
    sds seg = sdsnew("hello >> out.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(fout != NULL && strcmp(fout, "out.txt") == 0, "redirect_stdout = out.txt");
    CHECK(append, "append = true");
    sdsfree(seg); if (fout) sdsfree(fout);
}

static void test_redirect_stderr(void)
{
    TEST("redirect 2> file");
    sds seg = sdsnew("hello 2> err.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(fout == NULL, "no stdout");
    CHECK(ferr != NULL && strcmp(ferr, "err.txt") == 0, "redirect_stderr = err.txt");
    sdsfree(seg); if (ferr) sdsfree(ferr);
}

static void test_redirect_input(void)
{
    TEST("redirect < file");
    sds seg = sdsnew("hello < in.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(fin != NULL && strcmp(fin, "in.txt") == 0, "input_file = in.txt");
    sdsfree(seg); if (fin) sdsfree(fin);
}

static void test_redirect_at_start(void)
{
    TEST("redirect at start > file");
    sds seg = sdsnew("> out.txt hello world");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello world") == 0, "cleaned segment");
    CHECK(fout != NULL && strcmp(fout, "out.txt") == 0, "redirect_stdout = out.txt");
    sdsfree(seg); if (fout) sdsfree(fout);
}

static void test_redirect_no_input(void)
{
    TEST("redirect only > file");
    sds seg = sdsnew("> out.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(sdslen(seg) == 0, "empty segment remaining");
    CHECK(fout != NULL && strcmp(fout, "out.txt") == 0, "redirect_stdout = out.txt");
    sdsfree(seg); if (fout) sdsfree(fout);
}

static void test_redirect_quoted_not_redirect(void)
{
    TEST("quoted > is NOT a redirect");
    sds seg = sdsnew("\"hello > world\" fix");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "\"hello > world\" fix") == 0, "segment unchanged");
    CHECK(fout == NULL, "no redirect");
    sdsfree(seg);
}

static void test_redirect_multiple(void)
{
    TEST("multiple redirects: last wins");
    sds seg = sdsnew("hello > a.txt > b.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(fout != NULL && strcmp(fout, "b.txt") == 0, "last redirect wins");
    sdsfree(seg); if (fout) sdsfree(fout);
}

static void test_redirect_no_filename(void)
{
    TEST("redirect > without filename");
    sds seg = sdsnew("hello >");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "segment cleaned");
    CHECK(fout == NULL, "no filename → no redirect");
    sdsfree(seg);
}

static void test_redirect_stderr_no_space(void)
{
    TEST("stderr redirect 2>file (no space)");
    sds seg = sdsnew("hello 2>err.txt");
    sds fout = NULL, ferr = NULL, fin = NULL;
    bool append = false;
    edc_shell_strip_redirects(&seg, &fout, &append, &ferr, &fin);
    CHECK(strcmp(seg, "hello") == 0, "cleaned segment");
    CHECK(ferr != NULL && strcmp(ferr, "err.txt") == 0, "stderr = err.txt");
    sdsfree(seg); if (ferr) sdsfree(ferr);
}

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

/* ── sequential splitter tests (;) ────────────────────────── */

static void test_seq_simple(void)
{
    TEST("simple ; split");
    int n;
    sds *s = edc_shell_split_sequential("a ; b", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    free_segments(s, n);
}

static void test_seq_multiple(void)
{
    TEST("multiple ; split a ; b ; c");
    int n;
    sds *s = edc_shell_split_sequential("a ; b ; c", &n);
    CHECK(n == 3, "count = 3");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    check_segment(s, 2, "c", "segment[2]");
    free_segments(s, n);
}

static void test_seq_quoted(void)
{
    TEST("quoted ; is not split");
    int n;
    sds *s = edc_shell_split_sequential("\"a ; b\" ; c", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "\"a ; b\"", "segment[0]");
    check_segment(s, 1, "c", "segment[1]");
    free_segments(s, n);
}

static void test_seq_whitespace(void)
{
    TEST("whitespace around ;");
    int n;
    sds *s = edc_shell_split_sequential("  a  ;  b  ", &n);
    CHECK(n == 2, "count = 2");
    check_segment(s, 0, "a", "segment[0]");
    check_segment(s, 1, "b", "segment[1]");
    free_segments(s, n);
}

static void test_seq_single(void)
{
    TEST("single segment no semicolon");
    int n;
    sds *s = edc_shell_split_sequential("hello world", &n);
    CHECK(n == 1, "count = 1");
    check_segment(s, 0, "hello world", "segment[0]");
    free_segments(s, n);
}

int main(void)
{
    printf("=== edc_shell pipeline splitter tests ===\n\n");

    test_split_single();
    test_split_simple_pipe();
    test_split_multi_pipe();
    test_split_quoted_pipe_dq();
    test_split_quoted_pipe_sq();
    test_split_dq_escape();
    test_split_dq_backslash();
    test_split_mixed_quotes();
    test_split_pipe_at_start();
    test_split_pipe_at_end();
    test_split_consecutive_pipes();
    test_split_whitespace();
    test_split_no_spaces_pipe();
    test_split_empty_input();
    test_split_only_whitespace();
    test_split_flags_in_segments();
    test_split_ampersand_at_end();
    test_split_sq_backslash_literal();
    test_split_unterminated_dquote();

    printf("\n=== redirect stripping tests ===\n\n");
    test_redirect_stdout();
    test_redirect_append();
    test_redirect_stderr();
    test_redirect_input();
    test_redirect_at_start();
    test_redirect_no_input();
    test_redirect_quoted_not_redirect();
    test_redirect_multiple();
    test_redirect_no_filename();
    test_redirect_stderr_no_space();

    printf("\n=== history expansion tests (!n, !!) ===\n\n");
    test_history_expand_double_bang();
    test_history_expand_nth();
    test_history_no_expand();
    test_history_quoted_bang();
    test_history_out_of_range();

    printf("\n=== sequential splitter tests (;) ===\n\n");
    test_seq_simple();
    test_seq_multiple();
    test_seq_quoted();
    test_seq_whitespace();
    test_seq_single();

    printf("\n=== Results: %d passed, %d failed ===\n", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
