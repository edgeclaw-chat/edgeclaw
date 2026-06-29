#include "sds.h"
#include "edc_error.h"
#include "edc_skills.h"

#include "edc_compat.h"
#include "edc_file_utils.h"
#include "log.h"
#include "uthash.h"
#include "utarray.h"
#include <stdio.h>
#include <string.h>

static UT_icd ut_skill_ptr_icd = {sizeof(struct edc_skill *), NULL, NULL, NULL};

static struct edc_skill *edc_skill_new()
{
    struct edc_skill *skill = g_new0(struct edc_skill, 1);
    skill->content = NULL;
    return skill;
}

void edc_skill_free(struct edc_skill *skill)
{
    if (!skill)
    {
        return;
    }

    if (skill->name)
    {
        sdsfree(skill->name);
    }

    {
        struct edc_skill_meta_entry *e, *tmp;
        HASH_ITER(hh, skill->meta, e, tmp) {
            HASH_DEL(skill->meta, e);
            sdsfree(e->key);
            sdsfree(e->value);
            free(e);
        }
    }
    if (skill->content)
    {
        sdsfree(skill->content);
    }

    free(skill);
}

static bool edc_extract_skill_meta_and_content(const char *content, struct edc_skill *skill)
{
    /* YAML front matter: starts with ---\n, ends with \n--- */
    if (strncmp(content, "---\n", 4) != 0)
    {
        log_error("skill content has no YAML front matter prefix");
        return FALSE;
    }

    const char *meta_start = content + 4;
    const char *meta_end = strstr(meta_start, "\n---\n");
    if (!meta_end)
    {
        meta_end = strstr(meta_start, "\n---");
        if (!meta_end)
        {
            log_error("skill content: closing --- not found");
            return FALSE;
        }
    }

    sds meta = sdsnewlen(meta_start, (size_t)(meta_end - meta_start));

    int line_count;
    sds *lines = sdssplitlen(meta, sdslen(meta), "\n", 1, &line_count);
    if (lines)
    {
        for (int i = 0; i < line_count; i++)
        {
            int kv_count;
            sds *kv = sdssplitlen(lines[i], sdslen(lines[i]), ":", 1, &kv_count);
            if (kv && kv_count >= 2 && kv[0] && kv[1])
            {
                sds k = sdsdup(kv[0]); k = sdstrim(k, " \t\r\n");
                sds v = sdsdup(kv[1]); v = sdstrim(v, " \t\r\n");
                struct edc_skill_meta_entry *e = malloc(sizeof(*e));
                e->key = k;
                e->value = v;
                HASH_ADD_KEYPTR(hh, skill->meta, e->key, sdslen(e->key), e);
            }
            sdsfreesplitres(kv, kv_count);
        }

        sdsfreesplitres(lines, line_count);
    }

    const char *body_start = meta_end;
    if (body_start[0] == '\n')
        body_start++;  /* skip the leading \n of the delimiter */
    if (strncmp(body_start, "---\n", 4) == 0)
        body_start += 4;  /* skip "---\n" */
    else if (strncmp(body_start, "---", 3) == 0)
        body_start += 3;  /* skip "---" at end of file */

    sds tmp = sdsnew(body_start);
    skill->content = sdstrim(tmp, " \t\r\n");

    sdsfree(meta);
    return TRUE;
}

static struct edc_skill *edc_skill_load(const char *name, const char *file_path)
{
    EdcError *gerror = NULL;
    sds content = edc_file_read(file_path, &gerror);

    if (content)
    {
        struct edc_skill *skill = edc_skill_new();
        if (!edc_extract_skill_meta_and_content(content, skill))
        {
            edc_skill_free(skill);
            sdsfree(content);
            return NULL;
        }

        sdsfree(content);
        skill->name = sdsnew(name);

        return skill;
    }
    else
    {
        edc_error_free(gerror);
    }
    return NULL;
}

struct edc_skills_foreach_ctx
{
    UT_array *skills;
    sds dir_path;
};

static void edc_skills_foreach_cb(const char *name, int type, void *userdata)
{
    struct edc_skills_foreach_ctx *ctx = (struct edc_skills_foreach_ctx *)userdata;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return;

    sds full_path = edc_path_join(ctx->dir_path, name, "SKILL.md", NULL);
    struct edc_skill *skill = edc_skill_load(name, full_path);
    if (skill)
        utarray_push_back(ctx->skills, &skill);
    sdsfree(full_path);
}

UT_array *edc_skills_load(const char *dir_path)
{
    UT_array *skills;
    utarray_new(skills, &ut_skill_ptr_icd);

    struct edc_skills_foreach_ctx ctx = { .skills = skills, .dir_path = sdsnew(dir_path) };
    edc_dir_foreach(dir_path, edc_skills_foreach_cb, &ctx, NULL);
    sdsfree(ctx.dir_path);

    return skills;
}
