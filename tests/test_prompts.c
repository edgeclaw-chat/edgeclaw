/* Prompt library: `<workspace>/prompts/<name>.md`.
 *
 * Unit tests for parsing (src/kernel/context/edc_prompts.c), argument splitting and
 * substitution. Two properties matter most:
 *   ① the placeholders are THIS layer's, spelled the ecosystem's way -- `$1` / `$@` /
 *      `${1:-x}` (pi / Claude Code / opencode, so their files run unedited) -- with the
 *      legacy `{{…}}` alias read by the SAME expander. There is no translation step: a body
 *      keeps exactly what its author wrote;
 *   ② `$` is inert in the *input* layer: task references are `{{LAST}}` / `{{<id>}}` /
 *      `{{SELF}}` (see tests/test_cmd_parse.c::test_expand_task_refs), so the `$` forms
 *      belong to templates and the braces to the layer below. */
#include "edc_lex.h"
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
    sds *args = edc_argv_from_tail(argline, &argc);
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
    /* ── 替换：位置占位符 + 全量形式 ───────────────────────────── */
    /* ── `{{…}}` 不是占位符（它是输入层的任务引用）──────────────────
     * 这是一次有意的对齐，2026-10-16 核过 opencode 源码：它只认 `$1..$n` 与
     * `$ARGUMENTS`，从来没有 `{{…}}` 占位符。把 `{{…}}` 也当占位符是我们自己的
     * 旧别名，它让同一个拼写在两层有两种意思。现在它在这一层是纯文本。 */
    check_expand("`{{1}}` is NOT a placeholder", "A:{{1}}", "one", "A:{{1}}\n\none");
    check_expand("`{{@}}` neither", "A:{{@}}", "one two", "A:{{@}}\n\none two");
    check_expand("`{{1:-D}}` neither", "A:{{1:-D}}", "", "A:{{1:-D}}");

    /* ── 兜底：正文里没有任何引用 → 把用户输入追加在末尾 ────────────
     * （opencode 的规则，2026-10-16 核过源码；这样 `/prompt x 一些话` 与
     * `/skill y 一些话` 里的话永远不会被静默丢掉。） */
    check_expand("no reference: the arguments are appended", "no placeholders here", "one",
                 "no placeholders here\n\none");
    check_expand("no reference, no arguments: untouched", "no placeholders here", "", "no placeholders here");
    check_expand("{{0}} is not ours -> appended", "A:{{0}}", "one", "A:{{0}}\n\none");
    check_expand("an unknown {{...}} is left verbatim, then appended", "A:{{weird}}", "one",
                 "A:{{weird}}\n\none");
    check_expand("an unterminated {{ is not a reference -> appended", "A:{{1", "one", "A:{{1\n\none");
    check_expand("a lone { is not a reference -> appended", "A:{1}", "one", "A:{1}\n\none");

    /* ── `$…` 是**本层**的占位符 ─────────────────────────────────
     * 生态写法（pi / Claude Code / opencode）就是我们的写法：与 `{{…}}` 同一套文法、同一个
     * 展开器（`edc_prompt_placeholder_at`），**没有“翻译”这一步**。 */
    check_expand("$1 substitutes like {{1}}", "A:$1", "one", "A:one");
    check_expand("$1 (last) takes the rest too", "A:$1", "one two", "A:one two");
    check_expand("$10 is the tenth argument (multi-digit, as opencode reads it)",
                 "A:$10", "1 2 3 4 5 6 7 8 9 ten", "A:ten");
    check_expand("$10 with one argument is EMPTY -- it is not $1 followed by a 0",
                 "A:$10", "one", "A:");
    check_expand("$ARGUMENTS too", "A:$ARGUMENTS", "one two", "A:one two");
    /* `$@` and the braced family (`${10}` `$@` `${1:-D}` `${@:2:1}`) are GONE: opencode has
     * only `$1..$n` and `$ARGUMENTS`, and we align with it exactly. They are not
     * placeholders any more, so they stay as text (and, with no reference found, the
     * arguments are appended). */
    check_expand("`$@` is not a form of ours", "A:$@", "one two", "A:$@\n\none two");
    check_expand("`${1}` is not a form of ours", "A:${1}", "one", "A:${1}\n\none");
    check_expand("`${1:-D}` is not a form of ours", "A:${1:-D}", "", "A:${1:-D}");
    check_expand("`$HOME` is not a form of ours -> appended", "run $HOME/bin", "one", "run $HOME/bin\n\none");
    check_expand("$$ is not a form of ours -> appended", "A:$$", "one", "A:$$\n\none");
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

int main(void)
{
    test_expand();
    test_parse();

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
