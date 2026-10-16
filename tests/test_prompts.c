/* Prompt library: `<workspace>/prompts/<name>.md`.
 *
 * Unit tests for parsing (src/kernel/context/edc_prompts.c), argument splitting and
 * substitution. Two properties matter most:
 *   ① `edc_prompt_expand`'s placeholder set is exactly the one the header promises --
 *      `{{…}}` -- and it reads nothing else. In particular it never reads `$`: the `$`
 *      forms of the ecosystem (`$1`, `$@`, `${1:-x}`) are turned into `{{…}}` by the
 *      LOADER (edc_prompt_parse -> edc_prompt_translate_dollar), so by the time the
 *      expander runs there is only one syntax left;
 *   ② `$` is inert in the input layer too: task references are `{{LAST}}` / `{{<id>}}` /
 *      `{{SELF}}` now (see tests/test_cmd_parse.c::test_expand_task_refs), so a `$HOME` or
 *      a `$5.00` in a body passes through everything untouched. */
#include "edc_prompts.h"
#include "sds.h"
#include "utarray.h"

#include <stdio.h>
#include <stdlib.h>
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

#define CHECK_STR(got, want, m)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        CHECK((got) && strcmp((got), (want)) == 0, m);                                                                 \
    } while (0)

static void write_file_at(const char *dir, const char *name, const char *content)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f)
    {
        printf("  (cannot write %s)\n", path);
        return;
    }
    fputs(content, f);
    fclose(f);
}

/* Substitute |body| with the argument line |argline| (split the same way the command
 * does). Caller frees. */
static sds expand(const char *body, const char *argline)
{
    struct edc_prompt prompt;
    memset(&prompt, 0, sizeof(prompt));
    prompt.body = sdsnew(body);
    int argc = 0;
    sds *args = edc_prompt_split_args(argline, &argc);
    sds out = edc_prompt_expand(&prompt, args, argc);
    edc_prompt_args_free(args, argc);
    sdsfree(prompt.body);
    return out;
}

static void check_expand(const char *name, const char *body, const char *argline, const char *want)
{
    TEST(name);
    sds got = expand(body, argline);
    CHECK_STR(got, want, name);
    sdsfree(got);
}

/* ── Substitution ──────────────────────────────────────────── */

static void test_expand(void)
{
    check_expand("{{1}} is the first argument", "A:{{1}}", "one two", "A:one");
    check_expand("{{2}} is the second argument", "A:{{2}}", "one two", "A:two");
    check_expand("{{10}} is multi-digit (the tenth argument)", "A:{{10}}", "1 2 3 4 5 6 7 8 9 ten", "A:ten");
    check_expand("a missing positional is EMPTY, not literal", "A[{{3}}]", "one two", "A[]");
    check_expand("{{@}} joins every argument with a space", "A:{{@}}", "one two three", "A:one two three");
    check_expand("{{@}} with no arguments is empty", "A:{{@}}", "", "A:");
    check_expand("{{1:-default}} uses the argument", "A:{{1:-D}}", "one", "A:one");
    check_expand("{{1:-default}} falls back when missing", "A:{{1:-D}}", "", "A:D");
    check_expand("{{1:-default}} falls back when empty", "A:{{1:-D}}", "\"\"", "A:D");
    check_expand("{{@:-default}} uses the arguments", "A:{{@:-D}}", "one two", "A:one two");
    check_expand("{{@:-default}} falls back when there are none", "A:{{@:-D}}", "", "A:D");
    check_expand("{{@:2}} slices from the second argument", "A:{{@:2}}", "one two three", "A:two three");
    check_expand("{{@:2:1}} takes one from the second", "A:{{@:2:1}}", "one two three", "A:two");
    check_expand("{{@:9}} past the end is empty", "A:{{@:9}}", "one", "A:");
    check_expand("{{0}} is not ours", "A:{{0}}", "one", "A:{{0}}");
    check_expand("an unknown {{...}} is left verbatim", "A:{{weird}}", "one", "A:{{weird}}");
    check_expand("an unterminated {{ is left verbatim", "A:{{1", "one", "A:{{1");
    check_expand("a lone { is left verbatim", "A:{1}", "one", "A:{1}");
    check_expand("plain text is untouched", "no placeholders here", "one", "no placeholders here");

    /* ── 分层：展开器只认 `{{…}}` ─────────────────────────────────
     * 生态的 `$` 语法由**加载器**翻译成 `{{…}}`（见 test_translate_dollar），所以展开器
     * 永远只看得到一种写法；这里钉住"展开器读到 `$` 时一个字也不碰"。 */
    check_expand("$1 is not the expander's business", "A:$1", "one", "A:$1");
    check_expand("$@ neither", "A:$@", "one", "A:$@");
    check_expand("$$ neither", "A:$$", "one", "A:$$");
    check_expand("$HOME passes through untouched", "run $HOME/bin", "one", "run $HOME/bin");
    check_expand("$ARGUMENTS passes through untouched", "A:$ARGUMENTS", "one", "A:$ARGUMENTS");
}

/* ── Argument splitting ────────────────────────────────────── */

static void test_split(void)
{
    TEST("splits on whitespace");
    int argc = 0;
    sds *args = edc_prompt_split_args("one  two\tthree", &argc);
    CHECK(argc == 3, "three arguments");
    CHECK(argc == 3 && strcmp(args[0], "one") == 0 && strcmp(args[1], "two") == 0 && strcmp(args[2], "three") == 0,
          "and they are the words");
    edc_prompt_args_free(args, argc);

    TEST("double quotes group a spaced argument");
    args = edc_prompt_split_args("\"API compatibility\" now", &argc);
    CHECK(argc == 2, "two arguments");
    CHECK(argc == 2 && strcmp(args[0], "API compatibility") == 0, "the quoted one keeps its space");
    CHECK(argc == 2 && strcmp(args[1], "now") == 0, "and the rest follows");
    edc_prompt_args_free(args, argc);

    TEST("single quotes group too");
    args = edc_prompt_split_args("'a b' c", &argc);
    CHECK(argc == 2 && strcmp(args[0], "a b") == 0, "single-quoted keeps its space");
    edc_prompt_args_free(args, argc);

    TEST("backslash escapes the next byte");
    args = edc_prompt_split_args("a\\ b c", &argc);
    CHECK(argc == 2 && strcmp(args[0], "a b") == 0, "the escaped space does not split");
    edc_prompt_args_free(args, argc);

    TEST("operators are ordinary bytes here, not shell syntax");
    args = edc_prompt_split_args("a | b", &argc);
    CHECK(argc == 3, "three arguments -- the pipe is just text");
    CHECK(argc == 3 && strcmp(args[1], "|") == 0, "and it comes through unchanged");
    edc_prompt_args_free(args, argc);

    TEST("an empty argument line yields no arguments");
    args = edc_prompt_split_args("   ", &argc);
    CHECK(argc == 0, "zero arguments");
    edc_prompt_args_free(args, argc);
}

/* ── Parsing files ─────────────────────────────────────────── */

static void test_parse(void)
{
    const char *dir = "/tmp/edc_prompt_unit";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0)
        return;

    write_file_at(dir, "full.md",
                  "---\n"
                  "description: Review staged changes\n"
                  "argument-hint: \"[focus]\"\n"
                  "---\n"
                  "Review. Focus on {{1:-everything}}.\n");

    write_file_at(dir, "bare.md", "Just a body with no front matter.\nSecond line.\n");

    write_file_at(dir, "colon.md",
                  "---\n"
                  "description: Review: focus on X\n"
                  "---\n"
                  "body\n");

    write_file_at(dir, "unclosed.md", "---\ndescription: never closed\nbody\n");
    write_file_at(dir, "empty.md", "---\ndescription: nothing below\n---\n");
    write_file_at(dir, "other.txt", "not a prompt\n");

    TEST("front matter: description + argument-hint");
    struct edc_prompt *p = edc_prompt_load(dir, "full");
    CHECK(p != NULL, "loaded");
    if (p)
    {
        CHECK_STR(p->description, "Review staged changes", "description parsed");
        CHECK_STR(p->argument_hint, "[focus]", "argument-hint parsed (surrounding quotes dropped)");
        CHECK_STR(p->body, "Review. Focus on {{1:-everything}}.", "body is what follows the closing ---");
        CHECK_STR(p->name, "full", "name is the file's base name");
    }
    edc_prompt_free(p);

    TEST("no front matter: description falls back to the first non-empty line");
    p = edc_prompt_load(dir, "bare");
    CHECK(p != NULL, "loaded");
    if (p)
    {
        CHECK_STR(p->description, "Just a body with no front matter.", "first line became the description");
        CHECK(p->argument_hint == NULL, "no argument-hint");
        CHECK(p->body && strstr(p->body, "Second line.") != NULL, "and the whole file is the body");
    }
    edc_prompt_free(p);

    TEST("a value may contain a colon (split on the FIRST one only)");
    p = edc_prompt_load(dir, "colon");
    CHECK(p != NULL, "loaded");
    if (p)
        CHECK_STR(p->description, "Review: focus on X", "the colon inside the value survived");
    edc_prompt_free(p);

    TEST("front matter opened but never closed is refused, not half-parsed");
    p = edc_prompt_load(dir, "unclosed");
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("an empty body is refused");
    p = edc_prompt_load(dir, "empty");
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("a missing file is refused");
    p = edc_prompt_load(dir, "nope");
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("a name that could leave the directory is refused");
    CHECK(edc_prompt_load(dir, "../secret") == NULL, "../ refused");
    CHECK(edc_prompt_load(dir, "a/b") == NULL, "/ refused");
    CHECK(edc_prompt_load(dir, ".hidden") == NULL, "dot-prefixed refused");
    CHECK(edc_prompt_load(dir, "") == NULL, "empty refused");
    CHECK(edc_prompt_load(dir, "ok`;rm -rf /") == NULL, "non-[A-Za-z0-9._-] refused");

    TEST("listing is sorted, only .md, and skips refused files");
    UT_array *all = edc_prompts_list(dir);
    CHECK(utarray_len(all) == 3, "bare / colon / full (no other.txt, no unclosed, no empty)");
    if (utarray_len(all) == 3)
    {
        struct edc_prompt **s0 = (struct edc_prompt **)utarray_eltptr(all, 0);
        struct edc_prompt **s1 = (struct edc_prompt **)utarray_eltptr(all, 1);
        struct edc_prompt **s2 = (struct edc_prompt **)utarray_eltptr(all, 2);
        CHECK_STR((*s0)->name, "bare", "first is bare");
        CHECK_STR((*s1)->name, "colon", "second is colon");
        CHECK_STR((*s2)->name, "full", "third is full");
    }
    for (size_t i = 0; i < utarray_len(all); i++)
    {
        struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(all, (unsigned)i);
        edc_prompt_free(*slot);
    }
    utarray_free(all);

    TEST("a missing directory lists as empty, not an error");
    all = edc_prompts_list("/tmp/edc_prompt_unit_does_not_exist");
    CHECK(utarray_len(all) == 0, "empty");
    utarray_free(all);

    char cleanup[256];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
    if (system(cleanup) != 0)
        printf("  (cleanup failed)\n");
}

/* The `$` dialect (pi / Claude Code / opencode): the translation itself is mechanical.
 * WHEN to apply it is the caller's decision -- a template of ours must keep its `$`, which
 * is why this is a standalone step and not baked into edc_prompt_load (see the header). */
static void test_translate_dollar(void)
{
    sds (*T)(const char *) = edc_prompt_translate_dollar;

    TEST("positional: $1 / $2");
    sds t = T("$1 then $2");
    CHECK_STR(t, "{{1}} then {{2}}", "single digits");
    sdsfree(t);

    TEST("$10 is $1 followed by a literal 0 (shell positional semantics)");
    t = T("$10");
    CHECK_STR(t, "{{1}}0", "NOT the tenth argument (that spelling is {{10}})");
    sdsfree(t);

    TEST("$@ and $ARGUMENTS -> {{@}}");
    t = T("$@ / $ARGUMENTS");
    CHECK_STR(t, "{{@}} / {{@}}", "both spellings");
    sdsfree(t);

    TEST("braced forms map 1:1");
    t = T("${1}|${1:-d}|${@}|${@:-d}|${@:2}|${@:2:3}");
    CHECK_STR(t, "{{1}}|{{1:-d}}|{{@}}|{{@:-d}}|{{@:2}}|{{@:2:3}}", "all six forms");
    sdsfree(t);

    TEST("an escaped dollar is a literal dollar");
    t = T("\\$1 costs \\$2");
    CHECK_STR(t, "$1 costs $2", "backslash escape");
    sdsfree(t);

    TEST("a non-parameter $ is left to the input layer");
    t = T("$HOME and ${HOME} and $ and $$");
    CHECK_STR(t, "$HOME and ${HOME} and $ and $$", "verbatim");
    sdsfree(t);

    TEST("a body with no dollar is unchanged");
    t = T("plain {{1}} text");
    CHECK_STR(t, "plain {{1}} text", "untouched");
    sdsfree(t);
}

int main(void)
{
    test_expand();
    test_split();
    test_parse();
    test_translate_dollar();

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
