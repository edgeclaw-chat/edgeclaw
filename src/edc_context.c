#include "edc_context.h"
#include "edc_history.h"
#include "edc_skills.h"
#include "log.h"
#include <glib.h>
#include "sds.h"

const char *BOOTSTRAP_FILES[] = {"AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"};

static void sds_replace_all(sds *s, const char *find, const char *replacement)
{
    size_t find_len = strlen(find);
    sds result = sdsempty();
    char *scan = *s;
    char *pos;
    while ((pos = strstr(scan, find)) != NULL)
    {
        result = sdscatlen(result, scan, pos - scan);
        result = sdscat(result, replacement);
        scan = pos + find_len;
    }
    result = sdscat(result, scan);
    sdsfree(*s);
    *s = result;
}

static sds edc_context_get_identity(const char *os_name, const char *time, const char *workspace)
{
    const char *identity_fmt = "# EdgeClaw\n"
                               "You are EdgeClaw, a helpful AI assistant. You have access to tools "
                               "that allow you to:\n"
                               "- Read, write, and edit files\n"
                               "- Execute shell commands\n"
                               "- Search the web and fetch web pages\n"
                               "- Send messages to users on chat channels\n"
                               "\n- Spawn subagents for complex background tasks\n"
                               "\n"
                               "## Current Time\n"
                               "{{time}}\n"
                               "## OS Name\n"
                               "{{os_name}}"
                               "\n"
                               "## Current Running Directory\n"
                               "{{cwd}}\n"
                               "## Workspace\n"
                               "Your workspace is at: {{workspace}}\n"
                               "- Memory files: {{workspace}}/memory/MEMORY.md\n"
                               "- Timeline memory: {{workspace}}/memory/HISTORY.md\n"
                               "- Custom skills: {{workspace}}/skills/{{skill-name}}/SKILL.md\n"
                               "\n"
                               "IMPORTANT: When responding to direct questions or conversations, "
                               "reply directly with your text response.\n"
                               "Only use the 'message' tool when you need to send a message to a "
                               "specific chat channel (like WhatsApp).\n"
                               "For normal conversation, just respond with text - do not call the "
                               "message tool.\n"
                               "\n"
                               "Always be helpful, accurate, and concise. When using tools, explain "
                               "what you're doing.\n"
                               "When remembering something, write to "
                               "{{workspace}}/memory/MEMORY.md\n\n";

    char *cwd = g_get_current_dir();
    sds identity = sdsnew(identity_fmt);
    sds_replace_all(&identity, "{{os_name}}", os_name);
    sds_replace_all(&identity, "{{cwd}}", cwd);
    sds_replace_all(&identity, "{{time}}", time);
    sds_replace_all(&identity, "{{workspace}}", workspace);
    free(cwd);

    return identity;
}

static sds edc_context_load_bootstrap_files(const char *workspace)
{
    sds result = sdsempty();
    size_t num_files = sizeof(BOOTSTRAP_FILES) / sizeof(BOOTSTRAP_FILES[0]);

    for (size_t i = 0; i < num_files; i++)
    {
        const char *filename = BOOTSTRAP_FILES[i];
        char *filepath = g_build_filename(workspace, filename, NULL);

        char *content = NULL;
        size_t length = 0;
        GError *error = NULL;

        if (g_file_get_contents(filepath, &content, &length, &error))
        {
            result = sdscatprintf(result, "## %s\n\n%s", filename, content);
            free(content);
        }
        else
        {
            g_error_free(error);
        }

        if (i < num_files - 1)
        {
            result = sdscat(result, "\n\n");
        }

        free(filepath);
    }

    return result;
}

static sds edc_context_load_skills(const char *workspace)
{
    sds result = sdsempty();
    char *tmp = g_build_filename(workspace, "skills", NULL);
    sds skills_dir = sdsnewlen(tmp, strlen(tmp));
    free(tmp);

    UT_array *skills = edc_skills_load(skills_dir);
    sdsfree(skills_dir);

    result = sdscatprintf(result, "## Skills\n\n"
                                   "The following skills extend your capabilities. To use a skill, read its SKILL.md "
                                   "file using the read_file tool.\n\n");

    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **skill_ptr = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        struct edc_skill *skill = *skill_ptr;
        struct edc_skill_meta_entry *e;
        HASH_FIND_STR(skill->meta, "description", e);
        const char *desc = e ? e->value : NULL;
        if (!desc)
        {
            edc_skill_free(skill);
            continue;
        }

        char *loc = g_build_filename(workspace, "skills", skill->name, "SKILL.md", NULL);
        result = sdscatprintf(result, "### Skill: %s\n\n", skill->name);
        result = sdscatprintf(result, "description: %s\nSKILL.md localtion: %s\n", desc, loc);
        free(loc);
        if (i < utarray_len(skills) - 1)
        {
            result = sdscat(result, "\n\n---\n\n");
        }

        edc_skill_free(skill);
    }

    utarray_free(skills);
    return result;
}

static sds edc_context_load_memory(const char *workspace)
{
    char *memory_file = g_build_filename(workspace, "memory", "MEMORY.md", NULL);
    char *content = NULL;
    GError *error = NULL;
    size_t len = 0;

    if (!g_file_get_contents(memory_file, &content, &len, &error))
    {
        log_error("load memory failed: %s", error->message);
        g_error_free(error);
        free(memory_file);
        return NULL;
    }

    sds buf = sdsnew("## Memory\n\n### Long-Term Memory\n\n");
    buf = sdscat(buf, content);
    buf = sdscat(buf, "\n\n");

    free(memory_file);
    free(content);

    return buf;
}

sds edc_context_build_system_prompt(struct edc_config *conf)
{
    const char *workspace = conf->workspace;
    char *os_name = g_get_os_info(G_OS_INFO_KEY_NAME);

    sds time = edc_get_timestamp();
    sds identity = edc_context_get_identity(os_name ? os_name : "Unknown", time, workspace);
    sds bootstrap = edc_context_load_bootstrap_files(workspace);
    sds memory = edc_context_load_memory(workspace);
    sds skills = edc_context_load_skills(workspace);

    sds system_prompt = sdsempty();
    system_prompt = sdscat(system_prompt, identity);
    system_prompt = sdscat(system_prompt, bootstrap);
    if (memory)
    {
        system_prompt = sdscat(system_prompt, memory);
    }
    system_prompt = sdscat(system_prompt, skills);

    if (os_name)
    {
        free(os_name);
    }

    sdsfree(time);
    sdsfree(identity);
    sdsfree(bootstrap);
    if (memory)
    {
        sdsfree(memory);
    }
    sdsfree(skills);

    return system_prompt;
}
