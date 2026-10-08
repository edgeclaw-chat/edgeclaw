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

/* L1 fold: finished tasks' turns -> group memory.
 *
 * Triggered when unfolded messages reach the threshold (default 200); the fold
 * itself summarizes only the oldest half, so half the memory stays detailed.
 * Only turns are folded, never task->output. The prompt building and threshold
 * logic are unchanged from the old memory_actor. */

/* Fold task system prompt: the system group's shared history[0]. */
static const char *kFoldSystemPrompt =
    "You are a memory summarizer. Merge new task history into a concise Group Memory markdown.";

/* ── 折叠是一次屏障 (barrier) ────────────────────────────────
 *
 * 折叠读整组的 turns 写 group->memory。若此时组内的任务还在往前跑 (提交 turn、
 * 追加组历史), 折叠看到的就不是一个稳定快照 —— 而且下一个请求的上下文会顶破窗口。
 * 所以折叠**不让路**: 起折时把组内每个非终态成员挂到折叠任务上 (SEQUENCE),
 * 它们停在 AWAIT_DEPS, 折叠落地才被释放:
 *   - READY / AWAIT_DEPS 成员: `edc_task_add_dependency` 直接挂边 (READY 的自动进
 *     AWAIT_DEPS);
 *   - 已经在飞的成员 (AWAIT_EXEC / AWAIT_TOOLS): **不打断** (这一轮已发出/工具已跑)。
 *     依赖边照挂, 到它的下一个请求边界才生效 —— `tool_done` 里 executor 见
 *     `edc_task_has_unresolved_deps` 为真就不发下一个 LLM 请求、park 到 AWAIT_DEPS
 *     (B1: 工具结果照收, 只拦下一次请求)。折叠落地后它带着压缩过的上下文重新派发。
 * 折叠在飞期间新来的输入 / 被释放回来的任务, 由 `edc_group_fold_holds_group` 闸门
 * 兜住 (它们是 READY, 不在 dep 里)。
 *
 * 折叠任务现在是一个**普通落盘任务** (sink = GROUP_MEMORY, lifetime = PERSISTENT),
 * 所以指向它的 dep 边会随成员一起落盘、重启后读得回来; 被重启打断的折叠任务会在加载
 * 时置回就绪、由扫描重新派发 (见 edc_state_serializer.c)。 */

bool edc_group_fold_holds_group(struct edc_task_group *group)
{
    if (!group || !group->session)
        return false;
    struct edc_task_group *sys = edc_session_find_system_group(group->session);
    if (!sys)
        return false;

    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        /* A fold in flight = a non-terminal SYSTEM_MEMORY task writing this group. */
        if (t->sink == EDC_SINK_GROUP_MEMORY && t->sink_group == group && !edc_task_is_completed(t) &&
            !edc_task_is_cancelled(t) && !edc_task_has_failed(t))
            return true;
    }
    return false;
}

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

    /* Scan every session (a background session folds too) and fold at most one
     * group at a time. */
    struct edc_session_manager *sm = sessions;
    for (int si = 0; si < sm->session_count; si++)
    {
        struct edc_session *sess = sm->sessions[si];
        if (!sess)
            continue;
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
                /* Not startable right now (a member is still running) → keep looking
                 * at this session's other groups instead of declaring one started. */
                if (edc_group_fold_start(sched, sess, g))
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

    /* Sort by completion_seq (insertion sort; few tasks). */
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

    /* Half strategy: fold the oldest prefix of tasks (by completion_seq) whose
     * messages fit in half of the unfolded budget; max_seq is the boundary. */
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
        /* Fallback: the oldest task alone exceeds the budget. */
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

    /* One fold per session at a time, or the second prompt reads unwritten state. */
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

    /* The system group's shared header, written once. */
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
        /* Do not leave a half-built task; marking it failed also releases any
         * dependents (SEQUENCE). */
        edc_task_mark_failed(task);
        sdsfree(prompt);
        return NULL;
    }

    /* Barrier: every non-terminal member now waits for this fold (see the note at
     * the top of the file). A READY member takes the SEQUENCE edge and lands in
     * AWAIT_DEPS at once; a member already in flight keeps running, and the
     * executor parks it at its next request boundary (tool_done refuses the next
     * LLM send while a dep is outstanding). Either way the release is the same:
     * the fold completing notifies its dependents. */
    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
    {
        if (edc_task_is_completed(m) || edc_task_is_cancelled(m) || edc_task_has_failed(m))
            continue;
        edc_task_add_dependency(m, task, TASK_DEP_SEQUENCE);
    }

    /* The prompt is dispatched by the message-end scan, like any task. */
    log_info("L1 fold: prompt attached to system task %s (it will be dispatched by the scheduler)",
             task->task_id);

    sdsfree(prompt);
    return task;
}

/* Landing half of the fold: write the memory into task->sink_group, mark the
 * selected members folded, advance the watermark. On failure nothing changes and
 * the next round retries. */
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
