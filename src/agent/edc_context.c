#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_history.h"
#include "edc_skills.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_tools_todo.h"
#include "log.h"
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

    sds cwd = edc_cwd();
    sds identity = sdsnew(identity_fmt);
    sds_replace_all(&identity, "{{os_name}}", os_name);
    sds_replace_all(&identity, "{{cwd}}", cwd);
    sds_replace_all(&identity, "{{time}}", time);
    sds_replace_all(&identity, "{{workspace}}", workspace);
    sdsfree(cwd);

    return identity;
}

static sds edc_context_load_bootstrap_files(const char *workspace)
{
    sds result = sdsempty();
    size_t num_files = sizeof(BOOTSTRAP_FILES) / sizeof(BOOTSTRAP_FILES[0]);

    for (size_t i = 0; i < num_files; i++)
    {
        const char *filename = BOOTSTRAP_FILES[i];
        sds filepath = edc_path_join(workspace, filename, NULL);

        EdcError *error = NULL;
        sds content = edc_file_read(filepath, &error);

        if (content)
        {
            result = sdscatprintf(result, "## %s\n\n%s", filename, content);
            sdsfree(content);
        }
        else
        {
            edc_error_free(error);
        }

        if (i < num_files - 1)
        {
            result = sdscat(result, "\n\n");
        }

        sdsfree(filepath);
    }

    return result;
}

static sds edc_context_load_skills(const char *workspace)
{
    sds result = sdsempty();
    sds tmp = edc_path_join(workspace, "skills", NULL);
    UT_array *skills = edc_skills_load(tmp);
    sdsfree(tmp);

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

        sds loc = edc_path_join(workspace, "skills", skill->name, "SKILL.md", NULL);
        result = sdscatprintf(result, "### Skill: %s\n\n", skill->name);
        result = sdscatprintf(result, "description: %s\nSKILL.md localtion: %s\n", desc, loc);
        sdsfree(loc);
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
    sds memory_file = edc_path_join(workspace, "memory", "MEMORY.md", NULL);
    EdcError *error = NULL;
    sds content = edc_file_read(memory_file, &error);

    if (!content)
    {
        log_error("load memory failed: %s", error->message);
        edc_error_free(error);
        sdsfree(memory_file);
        return NULL;
    }

    sds buf = sdsnew("## Memory\n\n### Long-Term Memory\n\n");
    buf = sdscat(buf, content);
    buf = sdscat(buf, "\n\n");

    sdsfree(memory_file);
    sdsfree(content);

    return buf;
}

sds edc_context_build_system_prompt(struct edc_config *conf)
{
    const char *workspace = conf->workspace;
    sds os_name = edc_os_name();

    sds time = edc_get_timestamp();
    sds identity = edc_context_get_identity(os_name, time, workspace);
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

    sdsfree(os_name);

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
/* ── task todo list → 运行时 reminder (请求队尾) ────────────────
 * (2026-09-09: 从"头部 system prompt 尾部注入"改为"请求消息列表最末尾追加
 * 绝对新鲜的 system reminder"——头部注入在长上下文里易被模型遗忘; 队尾紧贴
 * 模型开始生成的位置。同年同日再改: 不每轮请求都发, 由 edc_agent_cb_send_llm
 * 按 edc_task_llm_request_poke 节流 —— 隔了 EDC_TODO_SYNC_REMIND_GAP 条对话
 * (LLM 请求) 都没调用过 update_todolist 才生成一次 (达阈值清零重计)。
 * 两段式骨架文本 (含 <system-reminder> 包裹、状态段内容选择、逐轮同步节拍/
 * 建计划引导两个分支) 由 edc_tools_todo_render_reminder 统一生成 —— 与
 * update_todolist 工具每次成功返回的 result 逐字节一致 (同一渲染源, 不再有
 * 独立的 "# TODO" checklist md 形态)。本函数只负责把这份文本包成一条
 * role=system 消息。
 * 副本语义: 生成新 system 消息, 绝不改动组内共享的 history[0] / task。 */

/* 运行时 todo reminder (role=system 的新消息, 每次生成都绝对新鲜)。由
 * edc_agent_cb_send_llm 在节流判定通过后调用, 塞到本次 LLM 请求消息列表的
 * 最末尾 (req->tail_system, 真正队尾: llm actor 在 history + 当前 turn 之后
 * 追加)。返回三种形态:
 *  - task 持有非空 todo 清单 → <system-reminder> 内: 状态段 (清单行) + 指令段
 *    (逐轮同步节拍)。内容不带 base, 独立成一条 system 消息;
 *  - task 非 NULL 但尚无/已清空清单 → 同骨架: 状态段 "尚无清单" + 指令段
 *    (建计划引导);
 *  - task 为 NULL → NULL。
 * 副本语义: 返回未挂到任何数组的新消息对象, 随请求释放; 不改 task/group。 */
cJSON *edc_context_make_todo_reminder(struct edc_task *task)
{
    if (!task)
        return NULL;

    sds content = edc_tools_todo_render_reminder(task->todolist);

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", content);
    sdsfree(content);
    return sys;
}

/* 本次 LLM 请求 "system 部分" (头部) 的唯一组装入口: group memory + system
 * prompt。不含任何 todo 内容——todo 引导一律由 edc_context_make_todo_reminder
 * 生成并塞到请求最末尾。引用/副本/现拼语义见 edc_context.h 注释——与迁移前
 * edc_agent_cb_send_llm 的行为字节级一致。 */
int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf, struct edc_task_group *group,
                                   sds *out_pristine)
{
    int appended = 0;
    if (!msgs)
        return 0;
    if (out_pristine)
        *out_pristine = NULL;

    /* 1. GROUP 记忆摘要 (已折叠 turns 的浓缩), 注入所有新 task */
    if (group && group->memory && sdslen(group->memory) > 0)
    {
        cJSON *mem = cJSON_CreateObject();
        cJSON_AddStringToObject(mem, "role", "system");
        sds mc = sdscatprintf(sdsempty(), "## Group Memory\n%s", group->memory);
        cJSON_AddStringToObject(mem, "content", mc);
        cJSON_AddItemToArray(msgs, mem);
        sdsfree(mc);
        appended++;
    }

    /* 2. system prompt (纯净, 无 todo 注入)
     * - group 有历史: 以引用挂 history[0] (不复制)
     * - group 无历史 / group 为空: 现拼纯净 prompt, 经 out_pristine 交还调用
     *   方 (由其决定写回 group history; 写回后后续请求即走引用分支) */
    if (group && cJSON_GetArraySize(group->history) > 0)
    {
        cJSON *sys0 = cJSON_GetArrayItem(group->history, 0);
        cJSON_AddItemReferenceToArray(msgs, sys0);
        appended++;
    }
    else if (conf)
    {
        sds prompt = edc_context_build_system_prompt(conf);
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", prompt);
        cJSON_AddItemToArray(msgs, sys);
        appended++;
        if (out_pristine)
            *out_pristine = sdsdup(prompt);
        sdsfree(prompt);
    }

    return appended;
}
