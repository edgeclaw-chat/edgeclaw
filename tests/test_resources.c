/* Resource roots + the `$`-dialect prompt translation (edc_resources.h / edc_prompts.h).
 *
 * Two things are pinned here:
 *   ① edc_resource_dirs() finds the user's roots ($HOME/.claude, .agents,
 *      .config/opencode, .pi/agent) and the project's, workspace first, and drops the
 *      project ones when `load_project_resources` is false;
 *   ② a template keeps its `$` bytes on load and remembers its source, and the workspace
 *      shadows a same-named template found in a later root. */
#include "edc_config.h"
#include "edc_prompts.h"
#include "edc_resources.h"
#include "edc_skills.h"
#include "sds.h"
#include "utarray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define PATH_BUF 1024

static void mkdirs(const char *path)
{
    char cmd[PATH_BUF + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0)
        printf("  (mkdir failed: %s)\n", path);
}

static void write_file_at(const char *dir, const char *name, const char *content)
{
    char path[PATH_BUF];
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

/* The root with this path, or NULL. */
static struct edc_resource_dir *root_at(UT_array *roots, const char *path)
{
    for (size_t i = 0; i < utarray_len(roots); i++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(roots, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->path, path) == 0)
            return *slot;
    }
    return NULL;
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
    {
        printf("  FAIL: no TEST_TMPDIR\n");
        return 1;
    }

    char home[PATH_BUF], proj[PATH_BUF], ws[PATH_BUF];
    snprintf(home, sizeof(home), "%s/home", tmp);
    snprintf(proj, sizeof(proj), "%s/proj", tmp);
    snprintf(ws, sizeof(ws), "%s/ws", tmp);

    char home_claude[PATH_BUF], home_pi[PATH_BUF], proj_claude[PATH_BUF], proj_own[PATH_BUF];
    snprintf(home_claude, sizeof(home_claude), "%s/.claude/prompts", home);
    snprintf(home_pi, sizeof(home_pi), "%s/.pi/agent/prompts", home);
    snprintf(proj_claude, sizeof(proj_claude), "%s/.claude/prompts", proj);
    snprintf(proj_own, sizeof(proj_own), "%s/.edgeclaw/prompts", proj);
    /* A root only counts when the directory EXISTS, so the workspace must be there
     * before the first edc_resource_dirs() call. */
    mkdirs(ws);
    mkdirs(home_claude);
    mkdirs(home_pi);
    mkdirs(proj_claude);
    mkdirs(proj_own);

    write_file_at(home_claude, "claude-only.md", "use $1 now\n");
    write_file_at(home_pi, "pi-only.md", "pi $2\n");
    write_file_at(proj_claude, "proj-only.md", "proj $1\n");
    write_file_at(proj_own, "own-only.md", "own {{1}}\n");

    setenv("HOME", home, 1);
    if (chdir(proj) != 0)
    {
        printf("  FAIL: chdir\n");
        return 1;
    }

    struct edc_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.workspace = sdsnew(ws);
    cfg.load_project_resources = true;

    char home_claude_root[PATH_BUF], home_pi_root[PATH_BUF], proj_claude_root[PATH_BUF], proj_own_root[PATH_BUF];
    snprintf(home_claude_root, sizeof(home_claude_root), "%s/.claude", home);
    snprintf(home_pi_root, sizeof(home_pi_root), "%s/.pi/agent", home);
    snprintf(proj_claude_root, sizeof(proj_claude_root), "%s/.claude", proj);
    snprintf(proj_own_root, sizeof(proj_own_root), "%s/.edgeclaw", proj);

    UT_array *roots = edc_resource_dirs(&cfg, proj);

    TEST("the project comes first, then the workspace, then the user's");
    struct edc_resource_dir **first = (struct edc_resource_dir **)utarray_eltptr(roots, 0);
    CHECK(first && *first && strcmp((*first)->path, proj_own_root) == 0, "root 0 is the project's ./.edgeclaw");
    CHECK(root_at(roots, ws) != NULL, "the workspace is a root too");
    CHECK(root_at(roots, home_claude_root) != NULL, "so is the user's");

    TEST("the user's foreign roots are found");
    struct edc_resource_dir *d = root_at(roots, home_claude_root);
    CHECK(d != NULL, "$HOME/.claude present");
    d = root_at(roots, home_pi_root);
    CHECK(d != NULL, "$HOME/.pi/agent present");

    TEST("project roots are read by default");
    d = root_at(roots, proj_claude_root);
    CHECK(d != NULL, "./.claude present");
    d = root_at(roots, proj_own_root);
    CHECK(d != NULL, "./.edgeclaw present");

    TEST("a foreign template loads AS WRITTEN (no translation step), and names its source");
    struct edc_prompt *p = edc_prompt_load_roots(roots, "claude-only", NULL);
    CHECK(p != NULL, "found in $HOME/.claude");
    CHECK_STR(p ? p->body : NULL, "use $1 now", "the body keeps the author's `$1`");
    CHECK(p && p->source && strcmp(p->source, home_claude_root) == 0, "source is the root");
    edc_prompt_free(p);

    TEST("a project template too");
    p = edc_prompt_load_roots(roots, "proj-only", NULL);
    CHECK_STR(p ? p->body : NULL, "proj $1", "kept as written");
    edc_prompt_free(p);

    TEST("the workspace SHADOWS a foreign template of the same name");
    char ws_prompts[PATH_BUF];
    snprintf(ws_prompts, sizeof(ws_prompts), "%s/prompts", ws);
    mkdirs(ws_prompts);
    write_file_at(ws_prompts, "shared.md", "ours {{1}}\n");
    write_file_at(home_claude, "shared.md", "theirs $1\n");

    p = edc_prompt_load_roots(roots, "shared", NULL);
    CHECK_STR(p ? p->body : NULL, "ours {{1}}", "our body, not the foreign one");
    CHECK(p && p->source && strcmp(p->source, ws) == 0, "source is the workspace");
    edc_prompt_free(p);

    TEST("in a template body `$1` is an ARGUMENT and `{{…}}` a task ref");
    write_file_at(ws_prompts, "ref.md", "arg: $1 / last: {{LAST}}\n");
    p = edc_prompt_load_roots(roots, "ref", NULL);
    CHECK_STR(p ? p->body : NULL, "arg: $1 / last: {{LAST}}", "both spellings are kept as written");
    edc_prompt_free(p);

    TEST("listing spans the roots, one row per name");
    UT_array *all = edc_prompts_list_roots(roots, NULL);
    CHECK(utarray_len(all) == 6, "claude-only, pi-only, proj-only, own-only, shared, ref");
    /* The listing is where a collision is REPORTED (the load path just takes the first
     * hit, so it has nothing to report). */
    {
        struct edc_prompt *shared = NULL;
        for (size_t i = 0; i < utarray_len(all); i++)
        {
            struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(all, (unsigned)i);
            if (slot && *slot && strcmp((*slot)->name, "shared") == 0)
                shared = *slot;
        }
        CHECK(shared && shared->shadowed && shared->shadowed[0] != NULL,
              "the shadowed template is reported, not swallowed");
    }
    for (size_t i = 0; i < utarray_len(all); i++)
    {
        struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(all, (unsigned)i);
        edc_prompt_free(*slot);
    }
    utarray_free(all);

    edc_resource_dirs_free(roots);

    TEST("`load_project_resources: false` drops the project roots");
    cfg.load_project_resources = false;
    roots = edc_resource_dirs(&cfg, proj);
    CHECK(root_at(roots, ws) != NULL, "the workspace stays");
    CHECK(root_at(roots, home_claude_root) != NULL, "the user's roots stay");
    CHECK(root_at(roots, proj_own_root) == NULL, "./.edgeclaw is gone");
    CHECK(root_at(roots, proj_claude_root) == NULL, "./.claude is gone");
    edc_resource_dirs_free(roots);
    cfg.load_project_resources = true;

    TEST("a session bound to a SUBdirectory still sees the project's resources");
    char sub[PATH_BUF];
    snprintf(sub, sizeof(sub), "%s/deep/nested", proj);
    mkdirs(sub);
    UT_array *sub_roots = edc_resource_dirs(&cfg, sub);
    CHECK(root_at(sub_roots, proj_own_root) != NULL, "the walk found ./.edgeclaw three levels up");
    CHECK(root_at(sub_roots, proj_claude_root) != NULL, "and ./.claude");
    edc_resource_dirs_free(sub_roots);
    cfg.load_project_resources = true;

    /* ── Skills over the roots ──────────────────────────────── */

    char ws_skill_pdf[PATH_BUF], home_skill_pdf[PATH_BUF], home_skill_db[PATH_BUF];
    snprintf(ws_skill_pdf, sizeof(ws_skill_pdf), "%s/skills/pdf", ws);
    snprintf(home_skill_pdf, sizeof(home_skill_pdf), "%s/skills/pdf", home_claude_root);
    snprintf(home_skill_db, sizeof(home_skill_db), "%s/skills/db", home_claude_root);
    mkdirs(ws_skill_pdf);
    mkdirs(home_skill_pdf);
    mkdirs(home_skill_db);
    write_file_at(ws_skill_pdf, "SKILL.md", "---\ndescription: ours\n---\nOUR BODY $1\n");
    write_file_at(home_skill_pdf, "SKILL.md", "---\ndescription: theirs\n---\nTHEIR BODY\n");
    write_file_at(home_skill_db, "SKILL.md",
                  "---\ndescription: dangerous\n\ndisable-model-invocation: true\n---\nDB BODY\n");

    roots = edc_resource_dirs(&cfg, proj);
    UT_array *skills = edc_skills_load_roots(roots, NULL);

    TEST("a skill is found across the roots, the workspace shadowing the user's");
    struct edc_skill *sk = NULL;
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->name, "pdf") == 0)
            sk = *slot;
    }
    CHECK(sk != NULL, "pdf found");
    CHECK(sk && sk->source && strcmp(sk->source, ws) == 0, "the workspace's copy won");
    CHECK(sk && sk->content && strstr(sk->content, "OUR BODY") != NULL, "and so did its body");
    CHECK(sk && sk->shadowed && sk->shadowed[0] != NULL, "the shadowed copy is RECORDED, not dropped silently");
    CHECK(sk && sk->shadowed && sk->shadowed[0] && strstr(sk->shadowed[0], "skills/pdf") != NULL,
          "and it names the file that lost");

    TEST("the PROJECT's copy outranks the workspace's");
    {
        char proj_skill[PATH_BUF];
        snprintf(proj_skill, sizeof(proj_skill), "%s/.claude/skills/pdf", proj);
        mkdirs(proj_skill);
        write_file_at(proj_skill, "SKILL.md", "---\ndescription: project pdf\n---\nPROJECT BODY\n");

        UT_array *r2 = edc_resource_dirs(&cfg, proj);
        UT_array *s2 = edc_skills_load_roots(r2, NULL);
        struct edc_skill *pdf = NULL;
        for (size_t i = 0; i < utarray_len(s2); i++)
        {
            struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(s2, (unsigned)i);
            if (slot && *slot && strcmp((*slot)->name, "pdf") == 0)
                pdf = *slot;
        }
        CHECK(pdf && pdf->content && strstr(pdf->content, "PROJECT BODY") != NULL,
              "the project's body is what runs");
        CHECK(pdf && pdf->shadowed && pdf->shadowed[0] && pdf->shadowed[1],
              "both losers are listed (the workspace's and the user's)");
        for (size_t i = 0; i < utarray_len(s2); i++)
        {
            struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(s2, (unsigned)i);
            if (slot)
                edc_skill_free(*slot);
        }
        utarray_free(s2);
        edc_resource_dirs_free(r2);
    }

    TEST("`disable-model-invocation: true` is read");
    struct edc_skill *db = NULL;
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->name, "db") == 0)
            db = *slot;
    }
    CHECK(db != NULL, "db found");
    CHECK(db && !db->model_invocable, "the model must not reach for it");
    CHECK(sk && sk->model_invocable, "an ordinary skill stays model-invocable");

    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot)
            edc_skill_free(*slot);
    }
    utarray_free(skills);
    edc_resource_dirs_free(roots);

    sdsfree(cfg.workspace);

    char cleanup[PATH_BUF + 32];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", tmp);
    if (system(cleanup) != 0)
        printf("  (cleanup failed)\n");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
