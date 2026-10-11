#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_skills.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

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

static sds edc_context_get_identity(const char *os_name, const char *workspace, const char *workdir)
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
                               "Your long-term memory lives in those files, **not** in this prompt: read "
                               "MEMORY.md (and HISTORY.md when you need the timeline) before answering "
                               "anything that depends on earlier context, and write durable knowledge back "
                               "into MEMORY.md.\n"
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
                               "{{workspace}}/memory/MEMORY.md\n"
                               "\n"
                               "This prompt carries no clock: when you need the current date or time, run "
                               "`date` (or check the OS).\n\n";

    /* The session's bound working directory is the "current directory" the
     * model is told about; unbound sessions fall back to the process cwd. */
    sds cwd = (workdir && workdir[0]) ? sdsnew(workdir) : edc_cwd();
    sds identity = sdsnew(identity_fmt);
    sds_replace_all(&identity, "{{os_name}}", os_name);
    sds_replace_all(&identity, "{{cwd}}", cwd);
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


/* Compact description of the sandbox's writable / read-only path sets, so the
 * model knows up front which directories it can write and does not waste tool
 * calls probing paths that will be denied. */
static sds edc_context_build_permissions(struct edc_config *conf)
{
    sds out = sdsnew("## Filesystem Permissions\n\n"
                     "File and shell tools are sandboxed: writes are allowed only under the "
                     "writable paths below. Every other path is read-only (reads are "
                     "unrestricted, writes elsewhere are denied), so do not try to create or "
                     "modify files outside the writable set.\n\n"
                     "- Writable:\n");
    for (int i = 0; conf->sandbox_rw_paths && conf->sandbox_rw_paths[i]; i++)
        out = sdscatprintf(out, "  - %s\n", conf->sandbox_rw_paths[i]);

    out = sdscat(out, "- Read-only (writes denied):\n");
    for (int i = 0; conf->sandbox_ro_paths && conf->sandbox_ro_paths[i]; i++)
        out = sdscatprintf(out, "  - %s\n", conf->sandbox_ro_paths[i]);

    out = sdscat(out, "\nA path matched by both lists takes the more specific (longer) entry.\n\n");
    return out;
}

/* session 记忆的**唯一**提示：常量文本，零可变内容（没有条数、没有内容、没有 id）。
 * 放在 system prompt 的**最后** —— 它是模型要看那段长提示（MEMORY.md 可能有几万 token）
 * 时最后读到的东西，recency 最好；而"条数/内容"由请求期队尾通知带（见 docs/l2-session-memory.md
 * §1.2.1）。放在这里而不是每次现拼一个块：常量 ⇒ 不碰 prompt cache 前缀。 */
static const char kSessionMemoryHint[] =
    "\n## Session Memory\n"
    "This session keeps a shared memory of durable knowledge (decisions, constraints, gotchas, external facts),\n"
    "visible to every task in it. Consult it when you need context from earlier work; record what will matter to\n"
    "other tasks later. Uses:\n"
    "- `memory_search <keyword>` — find entries (returns one line per hit)\n"
    "- `memory_list [offset] [n]` — enumerate entries\n"
    "- `memory_get <id>` — read one entry's full detail\n"
    "- `memory_write <title> [summary] [detail]` — record durable knowledge\n"
    "New or updated entries are announced at the end of the conversation. Keep progress logs and todos out of it.\n";

sds edc_context_build_system_prompt(struct edc_config *conf, const char *workdir)
{
    const char *workspace = conf->workspace;
    sds os_name = edc_os_name();

    sds identity = edc_context_get_identity(os_name, workspace, workdir);
    sds permissions = edc_context_build_permissions(conf);
    sds bootstrap = edc_context_load_bootstrap_files(workspace);
    sds skills = edc_context_load_skills(workspace);

    sds system_prompt = sdsempty();
    system_prompt = sdscat(system_prompt, identity);
    system_prompt = sdscat(system_prompt, permissions);
    system_prompt = sdscat(system_prompt, bootstrap);
    system_prompt = sdscat(system_prompt, skills);
    system_prompt = sdscat(system_prompt, kSessionMemoryHint);

    sdsfree(os_name);

    sdsfree(identity);
    sdsfree(permissions);
    sdsfree(bootstrap);
    sdsfree(skills);

    return system_prompt;
}
/* ── task todo list → 运行时 reminder (请求队尾) ────────────────
 * (2026-09-09: 从"头部 system prompt 尾部注入"改为"请求消息列表最末尾追加
 * 绝对新鲜的 system reminder"——头部注入在长上下文里易被模型遗忘; 队尾紧贴
 * 模型开始生成的位置。同年同日再改: 不每轮请求都发, 由 edc_app_cb_send_llm
 * 按 edc_task_llm_request_poke 节流 —— 隔了 EDC_TODO_SYNC_REMIND_GAP 条对话
 * (LLM 请求) 都没调用过 todowrite 才生成一次 (达阈值清零重计)。
 * 两段式骨架文本 (含 <system-reminder> 包裹、状态段内容选择、逐轮同步节拍/
 * 建计划引导两个分支) 由 edc_context_render_todo_reminder 统一生成 —— 与
 * todowrite 工具每次成功返回的 result 逐字节一致 (同一渲染源, 不再有
 * 独立的 "# TODO" checklist md 形态)。本函数只负责把这份文本包成一条
 * role=system 消息。
 * 副本语义: 生成新 system 消息, 绝不改动组内共享的 history[0] / task。 */

/* 运行时 todo reminder (role=system 的新消息, 每次生成都绝对新鲜)。由
 * edc_app_cb_send_llm 在节流判定通过后调用, 塞到本次 LLM 请求消息列表的
 * 最末尾 (req->tail_system, 真正队尾: llm actor 在 history + 当前 turn 之后
 * 追加)。返回三种形态:
 *  - task 持有非空 todo 清单 → <system-reminder> 内: 状态段 (清单行) + 指令段
 *    (逐轮同步节拍)。内容不带 base, 独立成一条 system 消息;
 *  - task 非 NULL 但尚无/已清空清单 → 同骨架: 状态段 "尚无清单" + 指令段
 *    (建计划引导);
 *  - task 为 NULL → NULL。
 * 副本语义: 返回未挂到任何数组的新消息对象, 随请求释放; 不改 task/group。 */
/* 纯渲染"- [ ]/- [x]" 清单行 (无 "<system-reminder>" 包裹、无引导段) ——
 * 与 render_reminder 共用同一行格式源: 它是 todo reminder 状态段的正文, 供
 * 需要把清单行单独嵌入的调用方使用。items 已在工具入口校验过 (status 合法、
 * 至多一个 in_progress)。对缺失字段做保守兜底 (缺省视为 pending / 空文本)，
 * 避免 app 侧读取历史数据时因脏数据崩溃。
 * *out_completed (可为 NULL) 返回已完成条数。返回 sds，调用方负责 sdsfree。 */
sds edc_context_render_todo_body(cJSON *items, int *out_completed)
{
    int total = items ? cJSON_GetArraySize(items) : 0;
    sds lines = sdsempty();
    int completed = 0;
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        char *content = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        char *priority = cJSON_GetStringValue(cJSON_GetObjectItem(item, "priority"));
        const char *st = status ? status : "pending";
        const char *ct = content ? content : "";
        const char *pri = priority ? priority : "";

        const char *marker;
        if (strcmp(st, "completed") == 0)
        {
            marker = "- [x]";
            completed += 1;
        }
        else if (strcmp(st, "cancelled") == 0)
        {
            marker = "- [-]";
        }
        else if (strcmp(st, "in_progress") == 0)
        {
            marker = "- [•]";
        }
        else
        {
            marker = "- [ ]";
        }

        if (pri && *pri)
            lines = sdscatprintf(lines, "%s %s [%s]\n", marker, ct, pri);
        else
            lines = sdscatprintf(lines, "%s %s\n", marker, ct);
    }
    if (out_completed)
        *out_completed = completed;
    return lines;
}

/* ── todo reminder 两段式骨架 (单一渲染源) ──────────────────────
 * 提醒 = "系统状态 + 本轮指令" (2026-09-09, 参照用户模板重构):
 *   ① ## 🚨 CURRENT TASK SYSTEM STATE —— 当前 todo 清单全貌 (与工具回显
 *      同格式的清单行), 让模型看到整个计划与执行到哪一步;
 *   ② ## 🛠️ INSTRUCTIONS FOR THIS TURN —— 每轮必须遵守的节拍: 检查上一个
 *     工具的执行结果; 当前条目干完后的【下一个动作必须是调用 todowrite】
 *     同步状态; 当前条目未标 [x] 不得跳到下一项。逐轮强约束, 解决"执行完
 *     好几步才想起来更新一次清单"的滞后问题。
 * 本文件 (edc_context.c) 是这套文本的唯一生成地 —— 原先它在 edc_tools_todo.c
 * 里, 但渲染的是 **task 的 todos**（上下文材料），生产读取方是本文件的
 * edc_context_make_todo_reminder; 留在工具箱侧会让 kernel 反过来引 tool 模块。
 * 搬到这里后, "工具回显与 reminder 逐字节一致"由结构保证（同一个实现）:
 *  todowrite 工具每次
 * 成功返回的 result/回显 就是这条 todo reminder 本身, LLM 请求队尾的 reminder
 * (edc_context_make_todo_reminder 的 content) 也由同一函数生成 —— 不再存在
 * 独立的 "# TODO" checklist markdown 形态, 模型在任何位置看到的清单格式
 * 逐字节一致。 */

/* 状态段标题 / 指令段标题 / 无清单状态行 (两分支共用) */
static const char *kTodoStateHeader = "## 🚨 CURRENT TASK SYSTEM STATE\n";
static const char *kTodoTurnHeader = "## 🛠️ INSTRUCTIONS FOR THIS TURN\n";
static const char *kTodoNoListState = "No todo list exists for this task yet.\n";

/* 逐轮同步节拍 (清单非空时注入)。核心诉求: 不许干完好几步才更新一次 todo——
 * 当前条目一完成, 下一步就强制 todowrite 同步。 */
static const char *kTodoTurnGuide =
    "- Check the execution result of the last tool call.\n"
    "- If the item you are working on is fully finished, your NEXT SINGLE ACTION must be calling "
    "todowrite to update the state (mark it \"completed\", promote the next pending item to "
    "\"in_progress\", send the FULL updated list).\n"
    "- Do NOT jump to the next item until the current one is marked as [x].\n"
    "- When the whole list is done, keep the completed list — do not clear it or invent items.\n";

/* 建清单引导 (尚无清单时注入)。多步任务先把计划落到 todowrite, 之后每轮
 * 自然切换到 kTodoTurnGuide 的逐轮同步节拍。 */
static const char *kTodoPlanningGuide =
    "- If the request is multi-step — several distinct steps or multiple tool calls (research, coding, "
    "debugging, batch edits, ...) — your FIRST action must be calling todowrite to create a "
    "step-by-step plan, then execute it.\n"
    "- Simple single-step requests or direct conversation do NOT need a todo list — just answer "
    "without creating one.\n";

/* 完整 todo reminder 文本 (含 <system-reminder> 包裹), 与 LLM 请求队尾的
 * reminder (edc_context_make_todo_reminder 的 content) 逐字节一致 ——
 * todowrite 工具 result/回显与上下文提醒共用此单一渲染源。两分支互斥:
 *  - items 非空 → 状态段 = 清单行 (render_body), 指令段 = 逐轮同步节拍;
 *  - items 为 NULL/空 → 状态段 = "尚无清单", 指令段 = 建计划引导。
 * 返回 sds, 调用方负责 sdsfree。 */
sds edc_context_render_todo_reminder(cJSON *items)
{
    int has_list = items && cJSON_GetArraySize(items) > 0;
    sds content = sdsnew("<system-reminder>\n");
    content = sdscat(content, kTodoStateHeader);

    if (has_list)
    {
        sds body = edc_context_render_todo_body(items, NULL);
        content = sdscat(content, body); /* 清单行, 每行以 \n 结尾 */
        sdsfree(body);
    }
    else
    {
        content = sdscat(content, kTodoNoListState);
    }

    content = sdscat(content, "\n"); /* 状态段与指令段之间留空行 */
    content = sdscat(content, kTodoTurnHeader);

    content = sdscat(content, has_list ? kTodoTurnGuide : kTodoPlanningGuide);

    content = sdscat(content, "\n</system-reminder>");
    return content;
}

cJSON *edc_context_make_todo_reminder(struct edc_task *task)
{
    if (!task)
        return NULL;

    sds content = edc_context_render_todo_reminder(task->todos);

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", content);
    sdsfree(content);
    return sys;
}

/* 本次 LLM 请求 "system 部分" (头部) 的唯一组装入口: group memory + system
 * prompt。不含任何 todo 内容——todo 引导一律由 edc_context_make_todo_reminder
 * 生成并塞到请求最末尾。引用/副本/现拼语义见 edc_context.h 注释——与迁移前
 * edc_app_cb_send_llm 的行为字节级一致。 */
int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf, struct edc_task_group *group,
                                   const char *workdir, sds *out_pristine)
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
        sds prompt = edc_context_build_system_prompt(conf, workdir);
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

/* ── 组上下文规模: pi 的 "anchor + 尾部估算" ──────────────────
 *
 * 为什么要锚: 预估器是启发式, 用它估整段上下文会累积误差 (中文尤甚)。provider
 * 对某一次请求报的 usage 是真值, 而"锚之后追加的东西"才是未知量 —— 把估算的
 * 误差钉在那一小段上。
 *
 * 锚什么时候不可信 (两条都退回"从零估一遍"):
 *   1. 它早于该组最近一次折叠: 折叠把前缀换成了摘要, 锚记的是换之前那个大前缀;
 *   2. 它没有位置信息 (last_hist_len == 0): 老存档读回来的锚, 或还没有过应答。
 *
 * 已知保守处 (都是"少算", 不虚报): 略去 dep 注入与队尾 todo reminder 这类常量级
 * 追加; 在跑的成员只算它自己 turn 的增量。 */

/* 一个 turn 从 from 起的估算 (from <= head 时从头算)。 */
static long long est_turn_from(struct edc_turn *turn, int from)
{
    if (!turn)
        return 0;
    size_t begin = (from > 0) ? (size_t)from : turn->messages.head;
    if (begin < turn->messages.head)
        begin = turn->messages.head;
    long long total = 0;
    for (size_t i = begin; i < turn->messages.tail; i++)
        total += edc_token_estimate_msg(turn->messages.data[i]);
    return total;
}

extern long long edc_context_group_ctx_tokens(struct edc_task_group *group, bool *out_estimated)
{
    if (out_estimated)
        *out_estimated = false;
    if (!group)
        return 0;

    long long best = 0;
    bool best_estimated = false;

    struct edc_task *t;
    /* 1) 锚 + 尾部 */
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue; /* 折叠任务不是会话内容 (与折叠账同一口径) */
        if (t->last_total_tokens <= 0 || t->last_hist_len <= 0)
            continue; /* 没有锚, 或没有位置 */
        if (t->last_fold_epoch != group->fold_epoch)
            continue; /* 锚记的是折叠前那个前缀 */

        long long tail = 0;
        if (group->history)
        {
            size_t n = (size_t)cJSON_GetArraySize(group->history);
            size_t from = (size_t)t->last_hist_len;
            if (from < n)
                tail += edc_token_estimate_msgs(group->history, from, n);
        }
        tail += est_turn_from(t->active_turn, t->last_turn_len);

        long long candidate = t->last_total_tokens + tail;
        if (candidate > best)
        {
            best = candidate;
            best_estimated = (tail > 0);
        }
    }
    if (best > 0)
    {
        if (out_estimated)
            *out_estimated = best_estimated;
        return best;
    }

    /* 2) 没有可用锚: 估一遍该组下次请求会带的东西 */
    long long est = 0;
    if (group->memory)
        est += edc_token_estimate_text(group->memory);
    if (group->history)
    {
        size_t n = (size_t)cJSON_GetArraySize(group->history);
        if (n > 0)
            est += edc_token_estimate_msg(cJSON_GetArrayItem(group->history, 0)); /* system prompt */
        /* The history only holds what is still sent (folded messages are pruned
         * at landing), so the tail starts right after the system prompt. */
        size_t from = 1;
        if (from < n)
            est += edc_token_estimate_msgs(group->history, from, n);
    }
    /* 在跑的成员带着自己的 turn; 新起一个任务则不带 —— 取最宽的那个, 与
     * "组 = 该组最宽的那个对话" 的口径一致。 */
    long long widest_turn = 0;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        int turn_from = (t->active_turn && t->active_turn->folded_prefix > 0) ? t->active_turn->folded_prefix : 0;
        long long turn_est = est_turn_from(t->active_turn, turn_from);
        if (turn_est > widest_turn)
            widest_turn = turn_est;
    }
    est += widest_turn;

    if (out_estimated && est > 0)
        *out_estimated = true;
    return est;
}
