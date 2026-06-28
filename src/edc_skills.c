#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_skills.h"

#include <glib.h>
#include "log.h"
#include "uthash.h"
#include "utarray.h"
#include <stdio.h>

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
        edc_str_free(skill->name);
    }

    {
        struct edc_skill_meta_entry *e, *tmp;
        HASH_ITER(hh, skill->meta, e, tmp) {
            HASH_DEL(skill->meta, e);
            free(e->key);
            free(e->value);
            free(e);
        }
    }
    if (skill->content)
    {
        edc_str_free(skill->content);
    }

    free(skill);
}

static bool edc_extract_skill_meta_and_content(const char *content, struct edc_skill *skill)
{
    if (!edc_str_has_prefix(content, "---"))
    {
        log_error("skill content has not prefix ---");
        return FALSE;
    }

    GRegex *regex = g_regex_new("^---\n(.*?)\n---", G_REGEX_DOTALL, G_REGEX_MATCH_DEFAULT, NULL);
    bool err = TRUE;
    GMatchInfo *match_info = NULL;
    char *meta = NULL;

    if (regex)
    {
        if (!g_regex_match(regex, content, 0, &match_info))

        {
            log_error("skill content regex match failed");
            err = FALSE;
            goto match_err;
        }

        if (g_match_info_get_match_count(match_info) != 2)
        {
            log_error("skill content regex match count != 2 failed");
            err = FALSE;
            goto match_info_err;
        }

        meta = g_match_info_fetch(match_info, 1);
        if (!meta)
        {
            err = FALSE;
            goto match_info_err;
        }

        char **lines = edc_strsplit(meta, "\n", -1);
        if (!lines)
        {
            err = FALSE;
            goto meta_err;
        }

        char **p = lines;
        while (*p)
        {
            char **kv = edc_strsplit(*p, ":", 2);
            if (kv && kv[0] && kv[1])
            {
                char *k = strdup(kv[0]); edc_strstrip(k);
                char *v = strdup(kv[1]); edc_strstrip(v);
                struct edc_skill_meta_entry *e = malloc(sizeof(*e));
                e->key = k;
                e->value = v;
                HASH_ADD_KEYPTR(hh, skill->meta, e->key, strlen(e->key), e);
            }
            edc_strfreev(kv);
            p++;
        }

        edc_strfreev(lines);

        int start = 0, end = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start, &end))
        {
            skill->content = edc_str_new(edc_strstrip((char *)content + end));
        }
        else
        {
            skill->content = edc_str_new(edc_strstrip((char *)content));
        }
    }

meta_err:
    if (meta)
        free(meta);
match_info_err:
    if (match_info)
        g_match_info_unref(match_info);
match_err:
    if (regex)
        g_regex_unref(regex);
    return err;
}

static struct edc_skill *edc_skill_load(const char *name, const char *file_path)
{
    char *content = NULL;
    size_t length = 0;
    GError *error = NULL;

    if (g_file_get_contents(file_path, &content, &length, &error))
    {
        struct edc_skill *skill = edc_skill_new();
        if (!edc_extract_skill_meta_and_content(content, skill))
        {
            edc_skill_free(skill);
            return NULL;
        }

        free(content);
        skill->name = edc_str_new(name);

        return skill;
    }
    else
    {
        g_error_free(error);
    }
    return NULL;
}

UT_array *edc_skills_load(const char *dir_path)
{
    UT_array *skills;
    utarray_new(skills, &ut_skill_ptr_icd);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir)
    {
        const char *filename = NULL;
        while ((filename = g_dir_read_name(dir)))
        {
            char *full_path = g_build_filename(dir_path, filename, "SKILL.md", NULL);

            struct edc_skill *skill = edc_skill_load(filename, full_path);
            if (skill)
            {
                utarray_push_back(skills, &skill);
            }
            free(full_path);
        }
        g_dir_close(dir);
    }
    return skills;
}
