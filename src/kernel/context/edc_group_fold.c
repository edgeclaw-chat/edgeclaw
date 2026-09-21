#include "edc_group_fold.h"
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_init_env.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
 * L1 折叠: task turns → group memory
 * 触发 (edc_group_fold_try) 只看未折叠消息数 ≥ 阈值 (默认 200);
 * 真正折叠时按半量策略: 只把最老的一半未折叠消息 (≈ 阈值/2, 即 ~100 条)
 * 交给 LLM 做摘要, 新的一半保持未折叠细节 —— 每次总是一半记忆被压缩、
 * 一半记忆留细节, 上下文里仍有明细消息可读, 不会整批全被摘要化。
 * 完成后置 turns_folded 并推进折叠水位; max_seq 只覆盖被选中的最老
 * task, 未选中的 task 保持 turns_folded=false, 下一轮再压。
 * 注意: 折叠只处理 turns, 绝不触碰 task->output (JOIN 仍可消费)。
 *
 * 下面的 prompt 构造与阈值判据**逐字来自旧的 memory_actor** —— 折叠行为本身
 * 不该因为"它现在是个普通任务"而变。
 * ══════════════════════════════════════════════════════════════ */

/* 折叠任务的 system prompt。旧实现是在发请求时现拼一条 system 消息, 一字不差地
 * 搬到这里, 只是改成**系统组的 history[0]** —— 那是组的共用头部, 组内每个
 * system task (每次折叠) 都用它, 与旧实现每次现拼的结果相同。 */
static const char *kFoldSystemPrompt =
    "You are a memory summarizer. Merge new task history into a concise Group Memory markdown.";

bool edc_group_fold_due(struct edc_task_group *group, int msg_threshold)
{
    if (!group || msg_threshold <= 0)
        return false;
    size_t foldable = edc_task_group_count_foldable(group);
    size_t unfolded = edc_task_group_unfolded_msg_count(group);
    return foldable > 0 && unfolded >= (size_t)msg_threshold;
}

bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions, struct edc_config *conf)
{
    if (!sched || !sessions)
        return false;

    int msg_threshold = conf ? conf->group_fold_msg_threshold : EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;
    if (msg_threshold <= 0)
        msg_threshold = EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;

    /* L1 折叠触发: 组内存在可折叠 task 且未折叠消息条数 ≥ 阈值。
     * 触发时机只由对话条数驱动 —— 不再看可折叠 task 个数。
     * 扫描**所有** session: 后台 session 里的任务完成后也要能触发折叠, 否则
     * 只有切回来才折叠。一次只折叠一个组。
     *
     * 折叠本身是一次 system task: 下面只决定"该折了", 任务由 edc_group_fold_start
     * 建、派发, 它的产出经 sink=GROUP_MEMORY 落到目标组的记忆里, 跑完按
     * lifetime=TRANSIENT 被回收。 */
    struct edc_session_manager *sm = sessions;
    for (int si = 0; si < sm->session_count; si++)
    {
        struct edc_session *sess = sm->sessions[si];
        if (!sess)
            continue;
        /* 这个 session 已经有折叠在飞: 跳过 (别让两次折叠互相看见半成品记忆)。 */
        if (edc_scheduler_has_pending_system_task(sess))
            continue;

        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            size_t foldable = edc_task_group_count_foldable(g);
            size_t unfolded = edc_task_group_unfolded_msg_count(g);

            if (edc_group_fold_due(g, msg_threshold))
            {
                log_info("L1 fold trigger: group %s foldable=%zu unfolded=%zu (msg_thr=%d)", g->group_id, foldable,
                         unfolded, msg_threshold);
                edc_group_fold_start(sched, sess, g);
                return true; /* 一次只折叠一个 group */
            }
            if (foldable > 0 || unfolded > 0)
            {
                log_debug("L1 fold check: group %s foldable=%zu unfolded=%zu", g->group_id, foldable, unfolded);
            }
        }
    }
    return false;
}

sds edc_group_fold_build_prompt(struct edc_task_group *group, uint32_t *out_max_seq)
{
    if (!group)
        return NULL;

    struct edc_task **tasks = NULL;
    size_t n = 0, cap = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (!edc_task_is_foldable(t))
            continue;
        if (n == cap)
        {
            cap = cap ? cap * 2 : 8;
            tasks = realloc(tasks, cap * sizeof(*tasks));
        }
        tasks[n++] = t;
    }

    if (n == 0)
    {
        log_debug("L1 fold: group %s has no foldable tasks", group->group_id);
        free(tasks);
        return NULL;
    }

    /* 按 completion_seq 升序 (插入排序, task 数通常很小) */
    for (size_t i = 1; i < n; i++)
    {
        struct edc_task *key = tasks[i];
        size_t j = i;
        while (j > 0 && tasks[j - 1]->completion_seq > key->completion_seq)
        {
            tasks[j] = tasks[j - 1];
            j--;
        }
        tasks[j] = key;
    }

    /* 半量策略: 触发后不是把攒满的未折叠消息整批全压, 而是只压最老的
     * 一半 (≈ unfolded/2; 攒满 200 即压 ~100), 剩下一半保持未折叠细节。
     * 折叠粒度为 task (整段 turns 一起摘要), 故按 completion_seq 升序取
     * "最老、且累计消息数不超过预算"的 task 前缀; max_seq 取边界 task,
     * finish 阶段只会折叠 seq ≤ max_seq 的 task, 新的一半自然留下。 */
    size_t budget = edc_task_group_unfolded_msg_count(group) / 2;
    size_t sel = 0, folded_msgs = 0, total_msgs = 0;
    for (size_t i = 0; i < n; i++)
        total_msgs += edc_task_turn_msg_count(tasks[i]);
    while (sel < n)
    {
        size_t m = edc_task_turn_msg_count(tasks[sel]);
        if (sel > 0 && folded_msgs + m > budget)
            break; /* 再加就超过一半 → 留给下一轮当细节 */
        folded_msgs += m;
        sel++;
    }
    if (sel == 0 && n > 0)
    {
        /* 兜底: 最老单个 task 就超预算 (正常触发时 unfolded ≥ 200, 不会走到) */
        folded_msgs = edc_task_turn_msg_count(tasks[0]);
        sel = 1;
    }
    log_info("L1 fold: group %s fold oldest %zu/%zu tasks (%zu/%zu msgs), keep %zu msgs as detail", group->group_id,
             sel, n, folded_msgs, total_msgs, total_msgs - folded_msgs);

    if (out_max_seq)
        *out_max_seq = tasks[sel - 1]->completion_seq;

    sds prompt = sdsempty();
    prompt = sdscat(prompt, "## Existing Group Memory\n");
    prompt = sdscat(prompt, (group->memory && sdslen(group->memory) > 0) ? group->memory : "(empty)");
    prompt = sdscat(prompt, "\n\n## New Completed Tasks\n");

    for (size_t i = 0; i < sel; i++)
    {
        struct edc_task *tk = tasks[i];
        prompt = sdscatprintf(prompt, "\n### [task %s] %s\n", tk->task_id, tk->description ? tk->description : "");

        for (size_t qi = tk->turns.head; qi < tk->turns.tail; qi++)
        {
            struct edc_turn *turn = tk->turns.data[qi];
            for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
            {
                cJSON *m = turn->messages.data[mj];
                cJSON *role = cJSON_GetObjectItem(m, "role");
                cJSON *content = cJSON_GetObjectItem(m, "content");
                if (role && content && cJSON_IsString(role) && cJSON_IsString(content))
                    prompt = sdscatprintf(prompt, "%s: %s\n", role->valuestring, content->valuestring);
            }
        }

        if (tk->output)
            prompt = sdscatprintf(prompt, "output: %s\n", tk->output);
    }

    prompt =
        sdscat(prompt, "\nMerge the tasks above into the Group Memory. Output ONLY the merged Group Memory as concise "
                       "markdown.");

    free(tasks);
    return prompt;
}

struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                      struct edc_task_group *group)
{
    if (!sched || !session || !group)
        return NULL;

    /* 一个 session 同时只折一个组: 两次折叠并行的话, 后一次的 prompt 会读到
     * 前一次还没写完的记忆与水位。 */
    if (edc_scheduler_has_pending_system_task(session))
        return NULL;

    uint32_t max_seq = 0;
    sds prompt = edc_group_fold_build_prompt(group, &max_seq);
    if (!prompt)
        return NULL;

    struct edc_task *task = edc_scheduler_spawn_system_task(sched, session, "fold group memory");
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    task->sink_group = group;   /* 产出写进这个组的记忆 (sink = GROUP_MEMORY) */
    task->fold_max_seq = max_seq;

    /* 系统组的共用头部 (只写一次): 折叠说明。 */
    if (task->group && cJSON_GetArraySize(task->group->history) == 0)
    {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "system");
        cJSON_AddStringToObject(msg, "content", kFoldSystemPrompt);
        edc_task_group_history_add_owned(task->group, msg);
    }

    log_info("L1 fold start: group %s task=%s prompt=%zu bytes, max_seq=%u", group->group_id, task->task_id,
             sdslen(prompt), max_seq);

    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L1 fold: could not attach the fold prompt for group %s — no fold this round", group->group_id);
        /* 装不上就别留着它: 置 ERROR 让 lifetime 规则把它回收 (不留半成品)。 */
        edc_task_force_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR);
        edc_scheduler_drain_reap_pending(sched);
        sdsfree(prompt);
        return NULL;
    }

    /* 折叠 prompt 交给调度 (消息末尾的扫描) 去跑 —— 与其它任务同一条路。 */
    log_info("L1 fold: prompt attached to system task %s (it will be dispatched by the scheduler)",
             task->task_id);

    sdsfree(prompt);
    return task;
}

/* ── 折叠产出的落地 (接收半边) ─────────────────────────────────
 *
 * 原来住 scheduler (sched_deliver_group_memory), 由 executor 的
 * `deliver_group_memory` 端口转交 —— 但它只碰 group 自己的字段, 不需要宿主,
 * 所以归 fold 模块: executor 现在按 task->sink 直接调它。
 *
 * 失败 (content 空 / 没目标组) 时**什么都不改**: 折叠没成功就还是老记忆 +
 * 老水位, 下一轮再折 (与旧实现一致)。任务本身照常按 lifetime 回收。 */
void edc_group_fold_deliver_memory(struct edc_task *task, const char *content)
{
    struct edc_task_group *group = task ? task->sink_group : NULL;
    if (!group || !content || !*content)
    {
        log_error("L1 fold: task %s produced no memory (target=%s, content=%s) — group memory unchanged",
                  (task && task->task_id) ? task->task_id : "(null)", group ? "yes" : "none",
                  (content && *content) ? "non-empty" : "empty");
        return;
    }

    if (group->memory)
        sdsfree(group->memory);
    group->memory = sdsnew(content);

    size_t folded_tasks = 0, folded_msgs = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_completed(t) && !t->turns_folded && t->completion_seq <= task->fold_max_seq)
        {
            t->turns_folded = true;
            folded_tasks++;
            folded_msgs += edc_task_turn_msg_count(t);
        }
    }
    group->folded_through_completion_seq = task->fold_max_seq;
    log_info("L1 fold done: group %s folded %zu tasks (%zu msgs)", group->group_id, folded_tasks, folded_msgs);
}
