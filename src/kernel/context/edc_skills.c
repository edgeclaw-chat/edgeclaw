#include "edc_skills.h"
#include "edc_error.h"
#include "sds.h"

#include "edc_compat.h"
#include "edc_file_utils.h"
#include "edc_resources.h"
#include "log.h"
#include "utarray.h"
#include "uthash.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static UT_icd ut_skill_ptr_icd = {sizeof(struct edc_skill *), NULL, NULL, NULL};

static struct edc_skill *edc_skill_new()
{
    struct edc_skill *skill = g_new0(struct edc_skill, 1);
    skill->content = NULL;
    skill->model_invocable = true; /* the default; `disable-model-invocation: true` opts out */
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
        HASH_ITER(hh, skill->meta, e, tmp)
        {
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
    if (skill->source)
    {
        sdsfree(skill->source);
    }
    if (skill->location)
    {
        sdsfree(skill->location);
    }

    free(skill);
}

static bool edc_extract_skill_meta_and_content(const char *content, struct edc_skill *skill)
{
    /* YAML front matter: starts with ---\n, ends with \n--- */
    if (strncmp(content, "---\n", 4) != 0)
    {
        log_error("skill content has no YAML front matter prefix");
        return false;
    }

    const char *meta_start = content + 4;
    const char *meta_end = strstr(meta_start, "\n---\n");
    if (!meta_end)
    {
        meta_end = strstr(meta_start, "\n---");
        if (!meta_end)
        {
            log_error("skill content: closing --- not found");
            return false;
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
                sds k = sdsdup(kv[0]);
                k = sdstrim(k, " \t\r\n");
                sds v = sdsdup(kv[1]);
                v = sdstrim(v, " \t\r\n");
                struct edc_skill_meta_entry *e = malloc(sizeof(*e));
                e->key = k;
                e->value = v;
                HASH_ADD_KEYPTR(hh, skill->meta, e->key, sdslen(e->key), e);
            }
            sdsfreesplitres(kv, kv_count);
        }

        sdsfreesplitres(lines, line_count);
    }

    /* pi's `disable-model-invocation: true`: the author says the MODEL must not reach for
     * this skill on its own -- it stays out of the system prompt's skill list and only an
     * explicit `/skill <name>` runs it. */
    struct edc_skill_meta_entry *dm = NULL;
    HASH_FIND_STR(skill->meta, "disable-model-invocation", dm);
    if (dm && dm->value)
    {
        sds v = sdstrim(sdsdup(dm->value), " \t\r\n");
        for (size_t i = 0; i < sdslen(v); i++)
            v[i] = (char)tolower((unsigned char)v[i]);
        skill->model_invocable = !(strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0);
        sdsfree(v);
    }

    const char *body_start = meta_end;
    if (body_start[0] == '\n')
        body_start++; /* skip the leading \n of the delimiter */
    if (strncmp(body_start, "---\n", 4) == 0)
        body_start += 4; /* skip "---\n" */
    else if (strncmp(body_start, "---", 3) == 0)
        body_start += 3; /* skip "---" at end of file */

    sds tmp = sdsnew(body_start);
    skill->content = sdstrim(tmp, " \t\r\n");

    sdsfree(meta);
    return true;
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
    {
        sdsfree(skill->source);
        skill->source = sdsdup(ctx->dir_path);
        sdsfree(skill->location);
        skill->location = sdsdup(full_path);
        utarray_push_back(ctx->skills, &skill);
    }
    sdsfree(full_path);
}

UT_array *edc_skills_load(const char *dir_path)
{
    UT_array *skills;
    utarray_new(skills, &ut_skill_ptr_icd);

    struct edc_skills_foreach_ctx ctx = {.skills = skills, .dir_path = sdsnew(dir_path)};
    edc_dir_foreach(dir_path, edc_skills_foreach_cb, &ctx, NULL);
    sdsfree(ctx.dir_path);

    return skills;
}

/* ── Over the resource roots (edc_resources.h) ─────────────── */

static struct edc_skill *edc_skills_find(UT_array *skills, const sds name)
{
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->name, name) == 0)
            return *slot;
    }
    return NULL;
}

/* Roots in order, first name wins -- so the workspace shadows a migrated skill of the same
 * name, and `skill->source` names the root it came from. A root may keep its skills in
 * `skills/` (Claude / pi / opencode) or `skill/` (opencode's other spelling). */
UT_array *edc_skills_load_roots(const UT_array *roots)
{
    UT_array *out;
    utarray_new(out, &ut_skill_ptr_icd);
    if (!roots)
        return out;

    static const char *const k_skill_subdirs[] = {"skills", "skill"};

    for (size_t r = 0; r < utarray_len(roots); r++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(roots, (unsigned)r);
        if (!slot || !*slot)
            continue;
        struct edc_resource_dir *root = *slot;

        for (size_t s = 0; s < sizeof(k_skill_subdirs) / sizeof(k_skill_subdirs[0]); s++)
        {
            sds dir = edc_path_join(root->path, k_skill_subdirs[s], NULL);
            UT_array *one = edc_skills_load(dir);
            sdsfree(dir);

            for (size_t i = 0; i < utarray_len(one); i++)
            {
                struct edc_skill **ps = (struct edc_skill **)utarray_eltptr(one, (unsigned)i);
                struct edc_skill *sk = ps ? *ps : NULL;
                if (!sk)
                    continue;
                if (edc_skills_find(out, sk->name))
                {
                    edc_skill_free(sk); /* shadowed by a higher-precedence root */
                    continue;
                }
                sdsfree(sk->source);
                sk->source = sdsdup(root->path);
                utarray_push_back(out, &sk);
            }
            utarray_free(one); /* the array only: its elements moved into |out| */
        }
    }

    return out;
}
