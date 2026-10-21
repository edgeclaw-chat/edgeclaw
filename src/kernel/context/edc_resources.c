#include "edc_resources.h"

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"

#include <stdlib.h>
#include <sys/stat.h>

static UT_icd ut_resource_dir_icd = {sizeof(struct edc_resource_dir *), NULL, NULL, NULL};

static bool is_dir(const sds path)
{
    struct stat st;
    return path && sdslen(path) > 0 && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_root(const UT_array *out, const sds path)
{
    for (size_t i = 0; i < utarray_len(out); i++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(out, (unsigned)i);
        if (slot && *slot && strcmp((*slot)->path, path) == 0)
            return true;
    }
    return false;
}

/* Take ownership of |path|; only an existing directory is recorded, and only once (a
 * missing directory is normal: nobody has every vendor's). */
static void add_root(UT_array *out, sds path)
{
    if (has_root(out, path) || !is_dir(path))
    {
        sdsfree(path);
        return;
    }
    struct edc_resource_dir *d = EDC_NEW0(struct edc_resource_dir, 1);
    d->path = path;
    utarray_push_back(out, &d);
}

UT_array *edc_resource_dirs(const struct edc_config *conf, const char *workdir)
{
    UT_array *out;
    utarray_new(out, &ut_resource_dir_icd);

    /* The PROJECT first: the most specific one wins, so a repository may ship its own
     * skills and govern them. (Position decided 2026-10-16: opencode puts the project
     * after the global dir and lets the LAST source win, which comes to the same thing;
     * pi puts the agent dir first instead. We take opencode's outcome, and -- like pi --
     * report what a collision shadowed rather than dropping it silently.) */
    if (!conf || conf->load_project_resources)
    {
        static const char *const project[] = {
            ".edgeclaw",
            ".claude",
            ".agents",
            ".opencode",
            ".pi",
        };
        sds cur = (workdir && workdir[0]) ? sdsnew(workdir) : edc_cwd();
        int depth = 0;
        while (cur && depth++ < 8)
        {
            for (size_t i = 0; i < sizeof(project) / sizeof(project[0]); i++)
                add_root(out, edc_path_join(cur, project[i], NULL));

            sds parent = edc_path_dirname(cur);
            if (!parent || strcmp(parent, cur) == 0)
            {
                sdsfree(parent); /* reached the root: dirname stopped changing */
                break;
            }
            sdsfree(cur);
            cur = parent;
        }
        sdsfree(cur);
    }

    /* Ours next: the workspace is our own store and the only WRITABLE root. */
    if (conf && conf->workspace)
        add_root(out, sdsdup(conf->workspace));

    /* The user's foreign roots last, under $HOME. */
    const char *home = getenv("HOME");
    if (home && *home)
    {
        sds h = sdsnew(home);
        static const char *const user[] = {
            ".claude",
            ".agents",
            ".config/opencode",
            ".pi/agent",
        };
        for (size_t i = 0; i < sizeof(user) / sizeof(user[0]); i++)
            add_root(out, edc_path_join(h, user[i], NULL));
        sdsfree(h);
    }

    return out;
}

void edc_resource_dirs_free(UT_array *dirs)
{
    if (!dirs)
        return;
    for (size_t i = 0; i < utarray_len(dirs); i++)
    {
        struct edc_resource_dir **slot = (struct edc_resource_dir **)utarray_eltptr(dirs, (unsigned)i);
        if (slot && *slot)
        {
            sdsfree((*slot)->path);
            free(*slot);
        }
    }
    utarray_free(dirs);
}
