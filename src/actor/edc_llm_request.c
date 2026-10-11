#include "edc_llm_request.h"
#include "edc_actor.h"
#include "edc_context.h"
#include "edc_group_fold.h"
#include "edc_llm_actor.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include <stdbool.h>
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include <edc_shell_vars.h>
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <string.h>

/* 把依赖材料**拼进 task 自己的输入**（B 方案, 2026-10-10）: 就地改写 active turn 的
 * 第一条 user 消息 —— 于是它跟着 turn 一起留存（不再是"只在一次请求里出现"的派生块）。
 * 返回 false = 没有可拼的 user 消息（异常形状；调用方退回"追加一条 system 消息"，不丢材料）。 */
static bool append_to_task_input(struct edc_task *task, const char *block)
{
    if (!task || !task->active_turn || !block || !*block)
        return false;
    struct edc_turn *turn = task->active_turn;
    for (size_t i = turn->messages.head; i < turn->messages.tail; i++)
    {
        cJSON *msg = turn->messages.data[i];
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
        if (!role || strcmp(role, "user") != 0)
            continue;
        const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
        sds joined = sdscatprintf(sdsempty(), "%s%s%s", old ? old : "", (old && *old) ? "\n\n" : "", block);
        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(joined));
        sdsfree(joined);
        return true;
    }
    return false;
}

/* ── LLM 请求装配 (原 src/app/edc_app.c 的搬家) ──────────
 * 本模块只有一条对外入口 edc_llm_request_send(); 它不含任何装配根私有状态:
 *   - kernel 侧输入 (group 历史 / 折叠水位 / deps / active_turn / todo 节流 /
 *     config / session 仓库) 全部经 sched 与 task 取得;
 *   - kernel 之外只有两样, 由装配根当参数交进来 (见 edc_llm_request.h)。
 * 三条不变量: 不认识 struct edc_app / 不认识 toolbox / 不反向依赖 app。
 *
 * $VAR 的"值语义" ($$ / $LAST / $N / $NAME) 是本文件私有的 static resolver:
 * 数据源 = sched->sessions (store 句柄)。shell 侧只做两件事 —— 语法识别与
 * 打 has_deferred_vars 标记 (edc_shell_parse.c:88 / edc_shell_ast.c:216),
 * 真正的替换推迟到这里, 因为值取自别的 task 的 output。 */

static sds edc_llm_var_resolver(const char *name, void *user_data)
{
    struct edc_scheduler *sched = user_data;
    if (!sched)
        return NULL;

    /* $$ → 当前活跃 task ID */
    if (strcmp(name, "$") == 0)
    {
        struct edc_task *active = edc_session_get_active_task(edc_session_manager_get_active(sched->sessions));
        return active ? sdsnew(active->task_id) : NULL;
    }

    /* $LAST → 最后一个完成 task 的 output */
    if (strcmp(name, "LAST") == 0)
    {
        struct edc_task *last = NULL;
        uint32_t highest_seq = 0;
        struct edc_task_group *g;
        struct edc_session *active = edc_session_manager_get_active(sched->sessions);
        DL_FOREACH(active ? active->groups : NULL, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (edc_task_is_completed(t) && t->completion_seq > highest_seq)
                {
                    highest_seq = t->completion_seq;
                    last = t;
                }
            }
        }
        return (last && last->output) ? sdsdup(last->output) : NULL;
    }

    /* $NUMBER 或 $NAME → 按 task ID / job number 查找 (全局; 必要时按需激活属主 session) */
    struct edc_task *t = edc_session_store_resolve_task(sched->sessions, sched, name, NULL);
    return (t && t->output) ? sdsdup(t->output) : NULL;
}

/* ── 回调: 发送 LLM 请求 ──────────────────────────────────── */

/* 确保 group history 切片起点不落在 tool 消息上。
 * OpenAI 要求每个 role=tool 消息前面必须有对应的 assistant(tool_calls) 消息；
 * 若窗口起点正好切在 assistant(tool_calls)+tool 序列中间，会导致 tool 消息孤儿化，
 * 从而报 "Messages with role 'tool' must be a response to a preceding message with 'tool_calls'"。
 * 这里把起点向前回退到第一个非 tool 消息（即其 assistant），保证序列完整。 */
static size_t edc_llm_history_safe_start(cJSON *history, size_t start, size_t lower_bound)
{
    while (start > lower_bound)
    {
        cJSON *m = cJSON_GetArrayItem(history, start);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0)
        {
            start--;
        }
        else
        {
            break;
        }
    }
    return start;
}

void edc_llm_request_send(struct edc_scheduler *sched, struct edc_task *task, cJSON *tool_schemas,
                          edc_actor_t *llm_actor, void *request_id)
{
    if (!sched || !task || !llm_actor)
        return;

    /* Group 级上下文: system 部分 (group memory + system prompt, 纯净、不含
     * todo) 由 edc_context_append_system_part 统一组装; 余下为全部未折叠历史
     * + task 专属 (dep outputs / $VAR 展开)。todo 状态提醒不放在
     * 头部——也不每轮请求都塞: 由 edc_task_llm_request_poke 节流, 只有"隔了
     * EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过 todowrite"
     * 时, 才经 edc_context_make_todo_reminder 重新计算并追加到消息列表绝对
     * 队尾 (req->tail_system, llm actor 在 history + 当前 turn 之后发出)。 */
    cJSON *msgs = cJSON_CreateArray();

    /* sink = GROUP_MEMORY 的折叠任务要的是**一次自足的摘要调用**: 折叠说明 (system)
     * + 它自己的 turn (折叠 prompt), 不含别的东西。所以它不走组上下文 —— 尤其不能
     * 走组 history[0]: 那个槽位不落盘, 重启后加载期会把它填成主提示词
     * (edc_task_group_rebuild_history), 摘要器于是背上 EdgeClaw 的人设 / MEMORY.md /
     * 工具指令, 而它根本没有工具。折叠 prompt 自带全部语料, 不需要任何组历史。 */
    const bool is_memory_sink = edc_task_is_memory_sink(task);

    sds pristine = NULL;
    /* 属主 session 的 workdir: 后台 session 里的任务续跑时系统提示里的
     * {{cwd}} 必须是它自己的目录 (P3 ③: 没有属主就没有目录, 不猜"当前的")。 */
    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    const char *workdir = own ? edc_session_get_workdir(own) : NULL;
    if (is_memory_sink)
    {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", task->sink == EDC_SINK_SESSION_MEMORY
                                                    ? edc_session_memory_system_prompt()
                                                    : edc_group_fold_system_prompt());
        cJSON_AddItemToArray(msgs, sys);
    }
    else
    {
        edc_context_append_system_part(msgs, sched->config, task->group, workdir, &pristine);
    }

    /* group 无历史时 context 现拼的纯净 prompt: 写回 group history[0] (组内
     * 所有 task 共享、保持纯净) */
    if (pristine)
    {
        if (task->group)
        {
            cJSON *sys_hist = cJSON_CreateObject();
            cJSON_AddStringToObject(sys_hist, "role", "system");
            cJSON_AddStringToObject(sys_hist, "content", pristine);
            /* 组 system prompt 不属于任何 task */
            edc_task_group_history_add_owned(task->group, sys_hist);
        }
        sdsfree(pristine);
    }

    if (!is_memory_sink && task->group && cJSON_GetArraySize(task->group->history) > 0)
    {
        size_t total = cJSON_GetArraySize(task->group->history);
        size_t end = total;

        /* 未折叠历史起点: 跳过 system prompt (history[0])。已折叠消息不在
         * history 里 —— 折叠落地时就被剪掉了 (见 edc_task_group_rebuild_history),
         * 所以这里不再需要任何水位算术。 */
        size_t hist_begin = 1;
        if (hist_begin > end)
            hist_begin = end;
        /* 水位边界也可能切在 tool 消息上, 回退到安全边界 */
        hist_begin = edc_llm_history_safe_start(task->group->history, hist_begin, 1);

        /* 注入全部未折叠历史 [hist_begin, end): fold 采用"半量"策略 (见
         * edc_group_fold 的 L1 fold), 触发时只压最老的一半, 剩下的未折叠
         * 消息正是模型该看到的近期细节, 因此整段注入、不再截断。请求体量的
         * 上界由 config 的 context_window 决定 (fold 触发线 = 窗口 - reserve,
         * 触发即把它压回去)。 */
        for (size_t i = hist_begin; i < end; i++)
        {
            cJSON_AddItemReferenceToArray(msgs, cJSON_GetArrayItem(task->group->history, i));
        }
    }

    /* 注入已完成依赖 task 的输出 (user inputs + final output) */
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        /* 打出 completed **与** cancelled: 被作废 (源已终态) 的边两个字段一假一真,
         * 只看 completed 会把它误读成"还在等"。 */
        log_info("send_llm task=%s dep src=%s completed=%d cancelled=%d output=%s", task->task_id,
                 dep->source ? dep->source->task_id : "(null)", dep->completed, dep->cancelled,
                 (dep->source && dep->source->output) ? "yes" : "none");
        if (dep->completed && dep->source && !dep->injected && dep->type == TASK_DEP_JOIN)
        {
            dep->injected = true; /* "已经拼过了" —— 材料已进输入, 不该再拼第二遍 */

            /* **B 方案** (2026-10-10 裁决): 材料**拼进目标自己的输入**, 不再往请求里插一对消息。
             * 就地改写 active turn 的第一条 user 消息 ⇒ 它跟着 turn 留存, 语义也更直白:
             * "我的输入里带着子任务的产出"。(就近原则: 只有出生时就挂的 JOIN 才走到这里 ——
             * 边没满足时目标根本不会被驱动。)
             * 记忆型源 (折叠 / 策展) 的"输入"是整段语料 (折叠 prompt 可能几百 KB) 且与 output
             * 讲同一件事 ⇒ 只拼它的 output (对 L2 策展来说那就是刚落地的 group memory)。 */
            if (dep->source->output)
            {
                const bool source_is_memory = edc_task_is_memory_sink(dep->source);
                sds block = source_is_memory
                                ? sdsnew(dep->source->output)
                                : sdscatprintf(sdsempty(), "[subtask %s result]:\n%s",
                                               dep->source->description ? dep->source->description : "(task)",
                                               dep->source->output);
                if (!append_to_task_input(task, block))
                {
                    log_warn("  -> task %s has no input message to splice into; appending a system message instead",
                             task->task_id ? task->task_id : "(null)");
                    cJSON *sys = cJSON_CreateObject();
                    cJSON_AddStringToObject(sys, "role", "system");
                    cJSON_AddStringToObject(sys, "content", block);
                    cJSON_AddItemToArray(msgs, sys);
                }
                sdsfree(block);
            }
            log_info("  -> spliced the output of task %s into the task input", dep->source->task_id);
        }
    }

    /* expand deferred $VAR references in the current turn's user messages */
    if (task->has_deferred_vars && task->active_turn)
    {
        for (size_t mj = task->active_turn->messages.head; mj < task->active_turn->messages.tail; mj++)
        {
            cJSON *msg = task->active_turn->messages.data[mj];
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
            {
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (content && cJSON_IsString(content))
                {
                    sds expanded = edc_shell_expand_vars(content->valuestring, edc_llm_var_resolver, sched, NULL);
                    if (expanded)
                    {
                        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(expanded));
                        sdsfree(expanded);
                    }
                }
            }
        }
        task->has_deferred_vars = 0;
    }

    /* sink = GROUP_MEMORY 的 system task (fold) 要的是**一次干净的摘要调用**:
     * 它的"镜像"就是 系统组头部 (折叠说明) + 它自己的 turn (折叠 prompt)。
     * 所以不给它工具定义 —— 摘要器没有工具可用, 也不该被工具循环带走 (旧实现
     * 发给 LLM 的请求里 tools 字段是空的, 这条差别的唯一理由就是保持那份请求
     * 逐字节可比)。 */
    cJSON *tools = (!is_memory_sink) ? tool_schemas : NULL;
    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msgs, tools,
                                                          task->active_turn ? &task->active_turn->messages : NULL);
    /* 摘要的输出预算 (a): 语料上限 EDC_FOLD_MAX_CORPUS_TOKENS 与它配套, 保证单次
     * 折叠的压缩比可达成 (≤ 4:1)。"请求从不发 max_tokens" 只有这一处例外。 */
    if (is_memory_sink)
        req->max_tokens = (task->sink == EDC_SINK_SESSION_MEMORY) ? EDC_SESSION_MEMORY_MAX_TOKENS
                                                                  : EDC_FOLD_SUMMARY_MAX_TOKENS;
    /* 本 turn 的前缀若已进记忆 (折叠切在 turn 内部), 从这里之后开始发。 */
    if (task->active_turn && task->active_turn->folded_prefix > 0)
        req->turn_begin = (size_t)task->active_turn->folded_prefix;
    req->history_owned = true;
    /* 绝对队尾: todo reminder —— 不再每轮请求都塞 (避免唠叨), 只在
     * "隔了 EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过
     * todowrite" 时生成 (edc_task_llm_request_poke 达阈值返回 true,
     * 内部已清零重计)。两段式: CURRENT TASK SYSTEM STATE 状态段 +
     * INSTRUCTIONS FOR THIS TURN 指令段; 有清单→清单行+逐轮同步节拍, 无清
     * 单→"尚无清单"+建计划引导。llm actor 在 history + turn 之后追加, 每
     * 轮请求都是最新状态。req 持有并随请求释放 (副本, 不改 task/group)。 */
    req->tail_parts = NULL;
    if (!is_memory_sink)
    {
        /* [0] L2 session 记忆通知: 自这个 task 上次"真的读到"以来新建/更新的条目,
         * 一行一条 (cap 10)。**请求期现算的临时物** —— 不写进任何消息数组、不落盘;
         * 水位在响应回来后由 `edc_task_memo_commit` 推进 (取消/失败不推进 ⇒ 重发)。
         * 常驻里没有 memory 内容 (系统提示只留一行常量提示, 见 edc_context.c)。 */
        struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
        if (own && own->memo)
        {
            uint64_t max_injected = 0;
            sds notice = edc_session_memory_notice(own->memo, task->memo_seen_seq, 10, &max_injected);
            if (notice)
            {
                cJSON *sys = cJSON_CreateObject();
                cJSON_AddStringToObject(sys, "role", "system");
                cJSON_AddStringToObject(sys, "content", notice);
                if (!req->tail_parts)
                    req->tail_parts = cJSON_CreateArray();
                cJSON_AddItemToArray(req->tail_parts, sys);
                task->memo_notice_seq = max_injected; /* 响应回来才提升成 memo_seen_seq */
                sdsfree(notice);
            }
        }
        /* [1] todo 提醒 (节流): 给"正在干活的用户任务"的节拍。摘要调用不该被它插尾
         * (同样是为了让折叠请求与旧实现逐字节一致)。 */
        if (edc_task_llm_request_poke(task))
        {
            cJSON *todo = edc_context_make_todo_reminder(task);
            if (todo)
            {
                if (!req->tail_parts)
                    req->tail_parts = cJSON_CreateArray();
                cJSON_AddItemToArray(req->tail_parts, todo);
            }
        }
    }

    edc_actor_send(llm_actor,
                   edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, edc_scheduler_get_actor(sched), request_id, req,
                                     (edc_actor_data_free_fn)edc_llm_actor_req_free));
}

/* ── 端口: 请求装配器是 `send_llm` 的提供方 ──────────────────
 *
 * 请求体怎么装、投给谁, 都是本文件的知识 (见 edc_llm_request.h), 所以"把端口
 * 装到 scheduler 上"也归这里 —— **谁提供谁接线**。装配期交一次两样 kernel 拿
 * 不到的值 (工具定义 + LLM 端点邮箱), 此后每次请求都从这里出去, 装配根不再出现
 * 在运行期路径上。端口签名一个字没动。
 *
 * 两个值的生命周期 = 进程 (装配根创建、装配根释放, 且只在运行时被读)。 */
static edc_actor_t *g_llm_actor = NULL;
static cJSON *g_tool_schemas = NULL;

static void edc_llm_request_cb_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    edc_llm_request_send(sched, task, g_tool_schemas, g_llm_actor, request_id);
}

void edc_llm_request_bind_scheduler(struct edc_scheduler *sched, edc_actor_t *llm_actor, cJSON *tool_schemas)
{
    if (!sched)
        return;

    g_llm_actor = llm_actor;
    g_tool_schemas = tool_schemas;
    sched->send_llm = edc_llm_request_cb_send_llm;
}
