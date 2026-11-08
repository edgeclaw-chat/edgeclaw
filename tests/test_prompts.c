/* Prompt library: `<workspace>/prompts/<name>.md`.
 *
 * Unit tests for parsing (src/kernel/context/edc_prompts.c), argument splitting and
 * substitution. Two properties matter most:
 *   ① the placeholders are THIS layer's -- `$1` … `$N` and `$ARGUMENTS`, the two forms a
 *      template written for another agent tool already uses. There is no translation step:
 *      a body keeps exactly what its author wrote, and `{{…}}` is NOT one of them;
 *   ② `$` is inert in the *input* layer: task references are `{{LAST}}` / `{{<id>}}` /
 *      `{{SELF}}` (see tests/test_cmd_parse.c::test_expand_task_refs), so the `$` forms
 *      belong to templates and the braces to the layer below. */
#include "edc_lex.h"
#include "edc_config.h"
#include "edc_prompts.h"
#include "sds.h"
#include "utarray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* symlink/unlink */
#include "test_fs_util.h"

/* Split a tail the way a shell does -- a TEST helper only.
 *
 * This used to be `edc_argv_from_tail` in edc_lex.h. Production does not need it: the
 * dispatcher sends a command line through edc_lex + edc_parse (the grammar builds
 * `struct edc_ast_cmd`), and the lexer is the only place that decides word boundaries.
 * Keeping a public "split this raw text into argv" call would invite the next handler to
 * bypass the grammar -- which is the very dialect this refactor removed -- so it lives
 * here, on top of the real lexer, and nothing can call it from src/.
 *
 * Quotes are syntax (adjacent fragments join, quotes are gone); operators do not
 * participate (SHELL_OFF). An unterminated quote is an ERROR: NULL plus a readable
 * message in |out_error|, never a silently dropped argument. Free with
 * sdsfreesplitres(). */
static sds *edc_argv_from_tail(const char *tail, int *out_count, sds *out_error)
{
    if (out_count)
        *out_count = 0;
    if (out_error)
        *out_error = NULL;

    edc_lex_out o;
    if (!edc_lex(tail ? tail : "", EDC_LEX_SHELL_OFF, &o))
        return NULL;

    sds *args = NULL;
    int n = 0;
    for (int i = 0; i < o.count; i++)
    {
        if (o.toks[i].kind == TK_BAD_QUOTED)
        {
            if (out_error)
                *out_error = edc_lex_unterminated_quote(o.toks[i].value, o.toks[i].begin + 1);
            edc_lex_out_free(&o);
            sdsfreesplitres(args, n);
            return NULL;
        }
        if (o.toks[i].kind != TK_TEXT && o.toks[i].kind != TK_STRING)
            continue;
        args = realloc(args, sizeof(sds) * (size_t)(n + 1));
        args[n++] = sdsdup(o.toks[i].value ? o.toks[i].value : sdsempty());
    }
    edc_lex_out_free(&o);
    if (out_count)
        *out_count = n;
    return args;
}

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
    sds qerr = NULL;
    sds *args = edc_argv_from_tail(argline, &argc, &qerr);
    /* A malformed argument line is an ERROR, never a silently dropped argument (see
     * edc_argv_from_tail). Without this the split would quietly yield "no arguments" and
     * the assertion below would blame the substitution for the split's mistake. */
    if (qerr)
    {
        printf("  (cannot split `%s`: %s)\n", argline, qerr);
        sdsfree(qerr);
    }
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
     * 本层的词汇只有 `$1..$n` 与 `$ARGUMENTS`。把 `{{…}}` 也当占位符是旧别名，
     * 它让同一个拼写在两层有两种意思。现在它在这一层是纯文本。 */
    check_expand("`{{1}}` is NOT a placeholder", "A:{{1}}", "one", "A:{{1}}\n\none");
    check_expand("`{{@}}` neither", "A:{{@}}", "one two", "A:{{@}}\n\none two");
    check_expand("`{{1:-D}}` neither", "A:{{1:-D}}", "", "A:{{1:-D}}");

    /* ── 兜底：正文里没有任何引用 → 把用户输入追加在末尾 ────────────
     * （这样 `/prompt x 一些话` 与 `/skill y 一些话` 里的话永远不会被静默丢掉。） */
    check_expand("no reference: the arguments are appended", "no placeholders here", "one",
                 "no placeholders here\n\none");
    check_expand("no reference, no arguments: untouched", "no placeholders here", "", "no placeholders here");
    check_expand("{{0}} is not ours -> appended", "A:{{0}}", "one", "A:{{0}}\n\none");
    check_expand("an unknown {{...}} is left verbatim, then appended", "A:{{weird}}", "one",
                 "A:{{weird}}\n\none");
    check_expand("an unterminated {{ is not a reference -> appended", "A:{{1", "one", "A:{{1\n\none");
    check_expand("a lone { is not a reference -> appended", "A:{1}", "one", "A:{1}\n\none");

    /* ── `$…` 是**本层**的占位符 ─────────────────────────────────
     * 与 `{{…}}` 走同一个展开器（`edc_prompt_placeholder_at`），**没有“翻译”这一步**。 */
    check_expand("$1 substitutes like {{1}}", "A:$1", "one", "A:one");
    check_expand("$1 (last) takes the rest too", "A:$1", "one two", "A:one two");
    check_expand("$10 is the tenth argument (multi-digit)",
                 "A:$10", "1 2 3 4 5 6 7 8 9 ten", "A:ten");
    check_expand("$10 with one argument is EMPTY -- it is not $1 followed by a 0",
                 "A:$10", "one", "A:");
    check_expand("$ARGUMENTS too", "A:$ARGUMENTS", "one two", "A:one two");
    /* `$@` and the braced family (`${10}` `${1:-D}` `${@:2:1}`) are GONE: the vocabulary is
     * `$1..$n` and `$ARGUMENTS` only. They are not placeholders any more, so they stay as
     * text (and, with no reference found, the arguments are appended). */
    check_expand("`$@` is not a form of ours", "A:$@", "one two", "A:$@\n\none two");
    check_expand("`${1}` is not a form of ours", "A:${1}", "one", "A:${1}\n\none");
    check_expand("`${1:-D}` is not a form of ours", "A:${1:-D}", "", "A:${1:-D}");
    check_expand("`$HOME` is not a form of ours -> appended", "run $HOME/bin", "one", "run $HOME/bin\n\none");
    check_expand("$$ is not a form of ours -> appended", "A:$$", "one", "A:$$\n\none");
}


/* ── Parsing files ─────────────────────────────────────────── */

static void test_parse(void)
{
    /* Under TEST_TMPDIR: "/tmp" is POSIX-only, and a FIXED name would also make two runs
     * of this binary share one tree. */
    char dir_buf[1024];
    test_tmp_template(dir_buf, sizeof(dir_buf), "edc_prompt_unit");
    if (!mkdtemp(dir_buf))
        return;
    const char *dir = dir_buf;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (test_sh(cmd) != 0)
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
    struct edc_prompt *p = edc_prompt_load(dir, "full", NULL);
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
    p = edc_prompt_load(dir, "bare", NULL);
    CHECK(p != NULL, "loaded");
    if (p)
    {
        CHECK_STR(p->description, "Just a body with no front matter.", "first line became the description");
        CHECK(p->argument_hint == NULL, "no argument-hint");
        CHECK(p->body && strstr(p->body, "Second line.") != NULL, "and the whole file is the body");
    }
    edc_prompt_free(p);

    TEST("a value may contain a colon (split on the FIRST one only)");
    p = edc_prompt_load(dir, "colon", NULL);
    CHECK(p != NULL, "loaded");
    if (p)
        CHECK_STR(p->description, "Review: focus on X", "the colon inside the value survived");
    edc_prompt_free(p);

    TEST("front matter opened but never closed is refused, not half-parsed");
    p = edc_prompt_load(dir, "unclosed", NULL);
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("an empty body is refused");
    p = edc_prompt_load(dir, "empty", NULL);
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("a missing file is refused");
    p = edc_prompt_load(dir, "nope", NULL);
    CHECK(p == NULL, "NULL");
    edc_prompt_free(p);

    TEST("a name that could leave the directory is refused");
    CHECK(edc_prompt_load(dir, "../secret", NULL) == NULL, "../ refused");
    CHECK(edc_prompt_load(dir, "a/b", NULL) == NULL, "/ refused");
    CHECK(edc_prompt_load(dir, ".hidden", NULL) == NULL, "dot-prefixed refused");
    CHECK(edc_prompt_load(dir, "", NULL) == NULL, "empty refused");
    CHECK(edc_prompt_load(dir, "ok`;rm -rf /", NULL) == NULL, "non-[A-Za-z0-9._-] refused");

    TEST("a symlinked template is refused, not read through");
    {
        /* The model can write into workspace/prompts: without this it could plant
         * `evil.md -> ~/.ssh/id_rsa` and have the contents land in its own context. */
        char outside[1024], link[1024];
        snprintf(outside, sizeof(outside), "%s_outside_secret.txt", dir);
        FILE *f = fopen(outside, "w");
        if (f)
        {
            fputs("TOP-SECRET\n", f);
            fclose(f);
        }
        snprintf(link, sizeof(link), "%s/evil.md", dir);
        unlink(link);
        CHECK(symlink(outside, link) == 0, "symlink planted");
        struct edc_prompt *evil = edc_prompt_load(dir, "evil", NULL);
        CHECK(evil == NULL, "the link is refused");
        edc_prompt_free(evil);
        unlink(link);
        unlink(outside);
    }

    TEST("a HARDLINKED template is refused too (lstat cannot see it)");
    {
        char outside[1024], href[1024];
        snprintf(outside, sizeof(outside), "%s_hardlink_secret.txt", dir);
        FILE *f = fopen(outside, "w");
        if (f)
        {
            fputs("TOP-SECRET\n", f);
            fclose(f);
        }
        snprintf(href, sizeof(href), "%s/linked.md", dir);
        unlink(href);
        CHECK(link(outside, href) == 0, "hardlink planted");
        struct edc_prompt *linked = edc_prompt_load(dir, "linked", NULL);
        CHECK(linked == NULL, "refused (st_nlink > 1)");
        edc_prompt_free(linked);
        unlink(href);
        unlink(outside);
    }

    TEST("the sandbox DENY list applies to templates (its roots are outside ro/rw)");
    {
        /* Being inside the resource root is not permission: a denied directory still
         * refuses, even though it is neither in ro_paths nor in rw_paths. */
        struct edc_prompt *allowed = edc_prompt_load(dir, "bare", NULL);
        CHECK(allowed != NULL, "loads with no policy (control)");
        edc_prompt_free(allowed);

        struct edc_config conf;
        memset(&conf, 0, sizeof(conf));
        conf.enable_sandbox = true;
        sds deny[2] = {sdsnew(dir), NULL};
        conf.sandbox_deny_paths = deny;

        struct edc_prompt *denied_p = edc_prompt_load(dir, "bare", &conf);
        CHECK(denied_p == NULL, "the same template is refused when its directory is denied");
        edc_prompt_free(denied_p);

        sdsfree(deny[0]);
        conf.sandbox_deny_paths = NULL;
    }

    TEST("listing is sorted, only .md, and skips refused files");
    UT_array *all = edc_prompts_list(dir, NULL);
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
    all = edc_prompts_list("/tmp/edc_prompt_unit_does_not_exist", NULL);
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
