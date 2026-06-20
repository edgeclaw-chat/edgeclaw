#include "edc_skills.h"

#include "log.h"
#include <stdio.h>

static struct edc_skill *edc_skill_new()
{
    struct edc_skill *skill = g_new(struct edc_skill, 1);
    skill->name = NULL;
    skill->meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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
        g_string_free(skill->name, TRUE);
    }

    g_hash_table_destroy(skill->meta);
    if (skill->content)
    {
        g_string_free(skill->content, TRUE);
    }

    g_free(skill);
}

static gboolean edc_extract_skill_meta_and_content(const gchar *content, struct edc_skill *skill)
{
    if (!g_str_has_prefix(content, "---"))
    {
        log_error("skill content has not prefix ---");
        return FALSE;
    }

    GRegex *regex = g_regex_new("^---\n(.*?)\n---", G_REGEX_DOTALL, G_REGEX_MATCH_DEFAULT, NULL);
    gboolean err = TRUE;
    GMatchInfo *match_info = NULL;
    gchar *meta = NULL;

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

        gchar **lines = g_strsplit(meta, "\n", -1);
        if (!lines)
        {
            err = FALSE;
            goto meta_err;
        }

        gchar **p = lines;
        while (*p)
        {
            gchar **kv = g_strsplit(*p, ":", 2);
            if (kv && kv[0] && kv[1])
            {
                g_hash_table_insert(skill->meta, g_strstrip(g_strdup(kv[0])), g_strstrip(g_strdup(kv[1])));
            }
            g_strfreev(kv);
            p++;
        }

        g_strfreev(lines);

        gint start = 0, end = 0;
        if (g_match_info_fetch_pos(match_info, 0, &start, &end))
        {
            skill->content = g_string_new(g_strstrip((char *)content + end));
        }
        else
        {
            skill->content = g_string_new(g_strstrip((char *)content));
        }
    }

meta_err:
    if (meta)
        g_free(meta);
match_info_err:
    if (match_info)
        g_match_info_unref(match_info);
match_err:
    if (regex)
        g_regex_unref(regex);
    return err;
}

static struct edc_skill *edc_skill_load(const char *name, const gchar *file_path)
{
    gchar *content = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (g_file_get_contents(file_path, &content, &length, &error))
    {
        struct edc_skill *skill = edc_skill_new();
        if (!edc_extract_skill_meta_and_content(content, skill))
        {
            edc_skill_free(skill);
            return NULL;
        }

        g_free(content);
        skill->name = g_string_new(name);

        return skill;
    }
    else
    {
        g_error_free(error);
    }
    return NULL;
}

GArray *edc_skills_load(const GString *dir_path)
{
    GArray *skills = g_array_new(FALSE, FALSE, sizeof(struct edc_skill *));
    GDir *dir = g_dir_open(dir_path->str, 0, NULL);
    if (dir)
    {
        const gchar *filename = NULL;
        while ((filename = g_dir_read_name(dir)))
        {
            gchar *full_path = g_build_filename(dir_path->str, filename, "SKILL.md", NULL);

            struct edc_skill *skill = edc_skill_load(filename, full_path);
            if (skill)
            {
                g_array_append_val(skills, skill);
            }
            g_free(full_path);
        }
        g_dir_close(dir);
    }
    return skills;
}
