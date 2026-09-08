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
 * (2026-09-09: 从"头部 system prompt 尾部注入"改为"每次请求消息列表最末尾
 * 追加绝对新鲜的 system reminder"——头部注入在长上下文里易被模型遗忘; 队尾
 * 紧贴模型开始生成的位置, 每轮 tool 循环都重新计算、保持最新。)
 * 目标不只是让模型"看见"清单: (1) 说明清单是权威、每请求刷新的进度记录;
 * (2) 提醒何时调用 update_todolist 同步进度; (3) 给一眼可读的当前执行状态
 * 摘要; (4) 附上与工具回显同格式的清单行 (单一渲染源 render_body)。
 * 副本语义: 生成新 system 消息, 绝不改动组内共享的 history[0] / task。
 *
 * 两条引导互补、互斥出现 (都在队尾, 见 edc_context_make_todo_reminder):
 *  - task 尚无清单 → kTodoPlanningGuide (短): 鼓励多步任务先建清单规划;
 *  - task 已有清单 → kTodoSysGuide + 状态摘要 + 清单行。 */

/* 规划引导: 任务还没建 todo 清单时追加 (位于请求队尾的 reminder), 让 LLM
 * 面对多步任务主动先用 update_todolist 建计划, 而不是等清单存在后才被告知
 * 维护规则。短小为宜——一旦建了清单, 后续每轮自然切换到 kTodoSysGuide +
 * state + 清单行。 */
static const char *kTodoPlanningGuide =
    "## Task Planning — plan multi-step work with a todo list\n"
    "\n"
    "When the user's request needs several distinct steps or will likely involve "
    "multiple tool calls (research, coding, debugging, batch edits, ...), FIRST "
    "call the update_todolist tool to create a step-by-step plan, then execute it:\n"
    "- Split the work into concrete, verifiable, ordered items — one action each, "
    "max 20 items.\n"
    "- Mark the item you are currently working on \"in_progress\" and set its "
    "activeForm to the concrete action you are doing right now; mark finished "
    "items \"completed\"; keep exactly ONE item in_progress.\n"
    "- Keep the list as your live progress record: call update_todolist with the "
    "FULL updated list whenever the state changes, and keep it when everything "
    "is done.\n"
    "\n"
    "Simple single-step requests or direct conversation do NOT need a todo list — "
    "just answer without creating one.\n";

/* 维护引导: 任务已持有 todo 清单时注入 (与状态摘要 + 清单行组成完整
 * reminder)。说明清单是权威记录、提醒何时调用 update_todolist 同步进度。
 * 注意: 本引导随 reminder 出现在请求【最末尾】, 措辞不再说"注入到 system
 * prompt 头部"。 */
static const char *kTodoSysGuide =
    "## Task Todo List — live progress record\n"
    "\n"
    "The todo list below is the authoritative, up-to-date record of the task you "
    "are executing. This reminder is recomputed and appended as a fresh system "
    "message at the end of every request, so trust it over update_todolist echoes "
    "in the conversation history — those tool messages may be trimmed out of the "
    "context window.\n"
    "\n"
    "Keep this list accurate: call the update_todolist tool with the FULL updated "
    "list whenever the state changes.\n"
    "- When you start an item, mark it \"in_progress\" and set its activeForm to the "
    "concrete action you are doing right now.\n"
    "- When you finish an item, mark it \"completed\" and promote the next pending "
    "item to \"in_progress\" (only ONE item may be in_progress at a time).\n"
    "- While a step runs long, keep refreshing its activeForm as your actual work "
    "changes.\n"
    "- When the whole list is done, keep the completed list as the record — do not "
    "clear it and do not invent items.\n"
    "- If the user changes the plan, update the list to mirror the new plan.\n"
    "\n"
    "Every call replaces the entire list (max 20 items). The list belongs to this "
    "task and persists with it.\n";

/* 从 items 推导当前执行状态摘要 (计数 / 正在做 / 下一个待办)。
 * items 非空。返回新 sds, 调用方负责 sdsfree。 */
static sds edc_context_todo_state_block(cJSON *items)
{
    int total = cJSON_GetArraySize(items);
    int completed = 0;
    const char *ip_content = NULL, *ip_form = NULL;
    const char *next_pending = NULL;
    for (int i = 0; i < total; i++)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        const char *ct = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        const char *af = cJSON_GetStringValue(cJSON_GetObjectItem(item, "activeForm"));
        if (!st) st = "pending";
        if (!ct) ct = "";
        if (!af) af = "";
        if (strcmp(st, "completed") == 0)
        {
            completed++;
        }
        else if (strcmp(st, "in_progress") == 0 && !ip_content)
        {
            ip_content = ct;
            ip_form = af;
        }
        else if (strcmp(st, "pending") == 0 && !next_pending)
        {
            next_pending = ct;
        }
    }

    sds b = sdsnew("Current state:\n");
    b = sdscatprintf(b, "- %d/%d completed\n", completed, total);
    if (ip_content)
    {
        b = sdscatprintf(b, "- in progress: \"%s\"", ip_content);
        if (ip_form && *ip_form)
            b = sdscatprintf(b, " (%s)", ip_form);
        b = sdscat(b, "\n");
    }
    if (next_pending)
    {
        b = sdscatprintf(b, "- next pending: \"%s\"\n", next_pending);
    }
    else if (!ip_content && completed == total)
    {
        b = sdscat(b, "- all items completed — nothing left to do\n");
    }
    return b;
}

/* 运行时 todo reminder (role=system 的新消息, 每次请求重新计算、绝对新鲜),
 * 由 edc_agent_cb_send_llm 塞到本次 LLM 请求消息列表的最末尾 (req->tail_system,
 * 真正队尾: llm actor 在 history + 当前 turn 之后追加)。返回三种形态:
 *  - task 持有非空 todo 清单 → 维护引导 + 状态摘要 + 清单行 (内容不带 base,
 *    独立成一条 system 消息, 不再拼进头部 system prompt);
 *  - task 非 NULL 但尚无清单 → 短规划引导 (## Task Planning);
 *  - task 为 NULL → NULL。
 * 副本语义: 返回未挂到任何数组的新消息对象, 随请求释放; 不改 task/group。 */
cJSON *edc_context_make_todo_reminder(struct edc_task *task)
{
    if (!task)
        return NULL;

    if (task->todolist && cJSON_GetArraySize(task->todolist) > 0)
    {
        sds content = sdsnew(kTodoSysGuide); /* 尾部以 "\n" 结尾 */
        content = sdscat(content, "\n");     /* 引导与状态块之间留空行 */
        sds state = edc_context_todo_state_block(task->todolist);
        content = sdscatprintf(content, "%s\n", state); /* 状态块后留空行 */
        sdsfree(state);
        sds body = edc_tools_todo_render_body(task->todolist, NULL);
        content = sdscat(content, body);
        sdsfree(body);

        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", content);
        sdsfree(content);
        return sys;
    }

    /* task 尚无清单 → 规划引导 */
    cJSON *guide = cJSON_CreateObject();
    cJSON_AddStringToObject(guide, "role", "system");
    cJSON_AddStringToObject(guide, "content", kTodoPlanningGuide);
    return guide;
}

/* 本次 LLM 请求 "system 部分" (头部) 的唯一组装入口: group memory + system
 * prompt。不含任何 todo 内容——todo 引导一律由 edc_context_make_todo_reminder
 * 生成并塞到请求最末尾。引用/副本/现拼语义见 edc_context.h 注释——与迁移前
 * edc_agent_cb_send_llm 的行为字节级一致。 */
int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf,
                                   struct edc_task_group *group, sds *out_pristine)
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
