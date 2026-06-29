#include "edc_context.h"
#include "edc_history.h"
#include "edc_skills.h"
#include "log.h"
#include <glib.h>

const char *BOOTSTRAP_FILES[] = {"AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"};

static GString *edc_context_get_identity(const char *os_name, const char *time, const char *workspace)
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
    GString *identity = g_string_new(identity_fmt);
    g_string_replace(identity, "{{os_name}}", os_name, 0);
    g_string_replace(identity, "{{cwd}}", cwd, 0);
    g_string_replace(identity, "{{time}}", time, 0);
    g_string_replace(identity, "{{workspace}}", workspace, 0);
    free(cwd);

    return identity;
}

static GString *edc_context_load_bootstrap_files(const char *workspace)
{
    GString *result = g_string_new(NULL);
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
            g_string_append_printf(result, "## %s\n\n%s", filename, content);
            free(content);
        }
        else
        {
            g_error_free(error);
        }

        if (i < num_files - 1)
        {
            g_string_append(result, "\n\n");
        }

        free(filepath);
    }

    return result;
}

static GString *edc_context_load_skills(const char *workspace)
{
    GString *result = g_string_new(NULL);
    GString *skills_dir = g_string_new_take(g_build_filename(workspace, "skills", NULL));

    GArray *skills = edc_skills_load(skills_dir);
    g_string_free(skills_dir, TRUE);

    g_string_append_printf(result, "## Skills\n\n"
                                   "The following skills extend your capabilities. To use a skill, read its SKILL.md "
                                   "file using the read_file tool.\n\n");

    for (size_t i = 0; i < skills->len; i++)
    {
        struct edc_skill *skill = g_array_index(skills, struct edc_skill *, i);
        const char *desc = g_hash_table_lookup(skill->meta, "description");
        if (!desc)
        {
            edc_skill_free(skill);
            continue;
        }

        char *loc = g_build_filename(workspace, "skills", skill->name->str, "SKILL.md", NULL);
        g_string_append_printf(result, "### Skill: %s\n\n", skill->name->str);
        g_string_append_printf(result, "description: %s\nSKILL.md localtion: %s\n", desc, loc);
        free(loc);
        if (i < skills->len - 1)
        {
            g_string_append(result, "\n\n---\n\n");
        }

        edc_skill_free(skill);
    }

    g_array_free(skills, TRUE);
    return result;
}

static char *edc_context_load_memory(const char *workspace)
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

    GString *buf = g_string_new("## Memory\n\n### Long-Term Memory\n\n");
    g_string_append(buf, content);
    g_string_append(buf, "\n\n");

    free(memory_file);
    free(content);

    char *memory = buf->str;
    g_string_free(buf, FALSE);
    return memory;
}

GString *edc_context_build_system_prompt(struct edc_config *conf)
{
    const char *workspace = conf->workspace;
    char *os_name = g_get_os_info(G_OS_INFO_KEY_NAME);

    char *time = edc_get_timestamp();
    GString *identity = edc_context_get_identity(os_name ? os_name : "Unknown", time, workspace);
    GString *bootstrap = edc_context_load_bootstrap_files(workspace);
    char *memory = edc_context_load_memory(workspace);
    GString *skills = edc_context_load_skills(workspace);

    GString *system_prompt = g_string_new(NULL);
    g_string_append(system_prompt, identity->str);
    g_string_append(system_prompt, bootstrap->str);
    if (memory)
    {
        g_string_append(system_prompt, memory);
    }
    g_string_append(system_prompt, skills->str);

    if (os_name)
    {
        free(os_name);
    }

    free(time);
    g_string_free(identity, TRUE);
    g_string_free(bootstrap, TRUE);
    if (memory)
    {
        free(memory);
    }
    g_string_free(skills, TRUE);

    return system_prompt;
}
