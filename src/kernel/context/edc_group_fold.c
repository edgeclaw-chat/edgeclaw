#include "edc_group_fold.h"
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_init_env.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
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

/* Fold task system prompt (pi §3): the summary is a **separate one-shot call**,
 * not a continuation of the conversation, so the prompt has to forbid continuing
 * or answering it. The request builder puts it in front of the fold prompt
 * (edc_llm_request); the fold task shares no group context. */
static const char *kFoldSystemPrompt =
    "You are a context summarization assistant. Read the transcript and produce (or update) a structured "
    "summary of it. Do NOT continue the conversation, do NOT answer anything in it, and do NOT call tools. "
    "Output ONLY the summary.";

const char *edc_group_fold_system_prompt(void)
{
    return kFoldSystemPrompt;
}

/* ── 摘要 prompt (P2b, pi §3/§8) ────────────────────────────────────────
 * 一次独立请求, 两段式拼法:
 *   <group-transcript> 语料 </group-transcript>
 *   [<previous-summary> 旧记忆 </previous-summary>]     ← 有旧记忆 = 迭代更新
 *   {首次 / 迭代} 指令
 *   [Additional focus: <focus>]                        ← 手动 /group fold 的聚焦词
 * 首次用完整六段格式; 迭代要求"保留仍成立的信息 + 增量合并 + 重排 Next Steps",
 * 输出**完整**的新摘要 —— 落地是替换 (`group->memory = 输出`), 不是拼接。 */
static const char *kInitialSummaryInstruction =
    "Summarize the transcript above as markdown with exactly these sections:\n"
    "\n"
    "## Goal\n"
    "[what the user wants to achieve; multiple items allowed]\n"
    "\n"
    "## Constraints & Preferences\n"
    "- [constraints / preferences / requirements the user stated]\n"
    "- [(none) if none]\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [completed work]\n"
    "\n"
    "### In Progress\n"
    "- [ ] [current work]\n"
    "\n"
    "### Blocked\n"
    "- [what blocks progress, if anything]\n"
    "\n"
    "## Key Decisions\n"
    "- **[decision]**: [brief reason]\n"
    "\n"
    "## Next Steps\n"
    "1. [ordered next steps]\n"
    "\n"
    "## Critical Context\n"
    "- [data / examples / references needed to continue]\n"
    "- [(none) if none]\n"
    "\n"
    "Keep every section concise. Preserve exact file paths, function names and error messages.";

/* pi §5 的 split turn: 语料以"没写完的 turn"结尾时, 记忆里要额外带上"那个 turn 走到
 * 哪了", 否则续跑的那半边只看到一份对话级摘要, 不知道自己接手的是什么。
 *
 * 我们与 pi 的差别 (写明): pi 为 turn 前缀单独发**第二次**调用 (专用 prompt, 预算
 * 0.5 x reserve), 我们只有一次调用 —— 摘要与 turn 上下文共用同一个 max_tokens
 * (EDC_FOLD_SUMMARY_MAX_TOKENS = 0.8 x reserve)。合并格式照抄 pi:
 *   {history summary}
 *   ---
 *   **Turn Context (split turn):**
 *   {turn prefix}
 */
static const char *kSplitTurnInstruction =
    "\n\nThe transcript also ends in the middle of a turn that continues afterwards (a split turn). "
    "After the sections above, add a `---` separator and then:\n"
    "\n"
    "**Turn Context (split turn):**\n"
    "- Original Request: [what that turn was asked to do]\n"
    "- Early Progress: [what it did before the summary cut in]\n"
    "- Context for Suffix: [what the continuation needs to know]";

static const char *kUpdateSummaryInstruction =
    "Update the previous summary with the transcript above. Keep every piece of information that is still "
    "true, merge in what is new, move finished items from \"In Progress\" to \"Done\", refresh \"Next Steps\" "
    "and \"Critical Context\", and keep the same sections. Preserve exact file paths, function names and error "
    "messages. Output the complete updated summary, not a diff.";

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

void edc_group_fold_note_failure(struct edc_task_group *group)
{
    if (!group)
        return;
    group->fold_failures++;
    if (group->fold_failures >= EDC_FOLD_MAX_FAILURES)
        log_error("L1 fold: group %s failed %d times in a row — automatic folding is paused for this group "
                  "(do new work in it, or run /group fold %s, to try again)",
                  group->group_id, group->fold_failures, group->group_id);
    else
        log_info("L1 fold: group %s fold failure %d/%d", group->group_id, group->fold_failures,
                 EDC_FOLD_MAX_FAILURES);
}

void edc_group_fold_note_progress(struct edc_task_group *group)
{
    if (!group || group->fold_failures == 0)
        return;
    log_info("L1 fold: group %s failure streak cleared (was %d)", group->group_id, group->fold_failures);
    group->fold_failures = 0;
}

bool edc_group_fold_due(struct edc_task_group *group, const struct edc_config *conf)
{
    if (!group || edc_task_group_count_foldable(group) == 0)
        return false;

    /* (c) 连续失败到上限 ⇒ 自动折叠停手 (手动 /group fold 走 start, 不过这道门)。
     * 没有这道闸, "失败 → 组仍然该折 → 原样重试"就是永动机: 2026-10-09 实测每 ~40s
     * 一发真调用, 组里的成员被反复 park。 */
    if (group->fold_failures >= EDC_FOLD_MAX_FAILURES)
        return false;

    /* 唯一的判据 = token 轴 (2026-10-10: 消息条数轴整条退休)。config 的默认窗口是
     * 1M 且 "<= 0" 在 init 期就被修成默认值 ⇒ 条数分支必然不可达, 留着只是死路。
     * 窗口仍然读不到 (有人手改脏了 config) 时**不折**: 不知道窗口不是"随便折"的理由,
     * 而手动 /group fold 永远能用。 */
    int window = conf ? conf->context_window : 0;
    if (window <= 0)
        return false;
    long long ctx = edc_context_group_ctx_tokens(group, NULL);
    return ctx > (long long)window - EDC_FOLD_RESERVE_TOKENS;
}

bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions, struct edc_config *conf,
                        struct edc_session **out_session)
{
    if (out_session)
        *out_session = NULL;
    if (!sched || !sessions)
        return false;

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

            if (edc_group_fold_due(g, conf))
            {
                log_info("L1 fold trigger: group %s foldable=%zu unfolded=%zu (window=%d)", g->group_id,
                         foldable, unfolded, conf ? conf->context_window : 0);
                /* Not startable right now (a member is still running) → keep looking
                 * at this session's other groups instead of declaring one started. */
                if (edc_group_fold_start(sched, sess, g, NULL))
                {
                    if (out_session)
                        *out_session = sess;
                    return true; /* 一次只折叠一个 group */
                }
            }
            if (foldable > 0 || unfolded > 0)
            {
                log_debug("L1 fold check: group %s foldable=%zu unfolded=%zu", g->group_id, foldable, unfolded);
            }
        }
    }
    return false;
}

/* ── 选料 (P2a-4): 从队尾保留 keep_recent 的细节, 更老的进语料 ──────────────
 *
 * 内容序 = 已完成的成员按 completion_seq, **在飞的排在最后**(它们的内容最新), 成员内
 * 按 turn 顺序, turn 内按消息顺序, 且只算未折叠的那一段 (turn->folded_prefix 之后)。
 * 切点永远落在消息边界上: 一条消息要么整条进语料, 要么整条留作细节。
 *
 * P2a-4a 只折"已提交"的 turn (`task->turns`); 在飞任务的 active turn 不在里面 ——
 * 它还没提交, 请求侧自己带着它。切到它内部 (点亮 req->turn_begin) 是 P2a-4b。 */

struct fold_piece
{
    struct edc_task *task;
    struct edc_turn *turn;
    size_t turn_index; /* 在该成员 turns 队列里的序号 (落地时按它找回同一个 turn) */
    size_t msg_index;  /* 在该 turn 里的绝对序号 */
    long long tokens;
};

/* 把一个 turn 的**未折叠段**摊进那一列。turn_index = 该 turn 在成员 turns 队列里的
 * 序号; 在飞的 active turn 用 EDC_FOLD_CUT_ACTIVE_TURN 这个哨兵。 */
static void push_pieces(struct fold_piece **ps, size_t *cnt, size_t *cap, struct edc_task *t, struct edc_turn *turn,
                        size_t turn_index)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t from = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if (from > len)
        from = len;
    for (size_t mi = from; mi < len; mi++)
    {
        if (*cnt == *cap)
        {
            *cap = *cap ? *cap * 2 : 64;
            *ps = realloc(*ps, *cap * sizeof(**ps));
        }
        struct fold_piece *fp = &(*ps)[(*cnt)++];
        fp->task = t;
        fp->turn = turn;
        fp->turn_index = turn_index;
        fp->msg_index = mi;
        fp->tokens = edc_token_estimate_msg(turn->messages.data[turn->messages.head + mi]);
    }
}

/* piece 的 role 是不是 "tool" (工具结果)。pi §4: 切点永不落在工具结果上 —— 被折掉的
 * assistant(tool_calls) 会让它变成孤儿, 整个请求直接失败。 */
static bool piece_is_tool_result(const struct fold_piece *fp)
{
    cJSON *m = fp->turn->messages.data[fp->turn->messages.head + fp->msg_index];
    cJSON *role = cJSON_GetObjectItem(m, "role");
    return role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0;
}

/* 把成员们的未折叠消息摊平成一列 (内容序)。 */
static size_t collect_pieces(struct edc_task *const *members, size_t n, struct fold_piece **out)
{
    struct fold_piece *ps = NULL;
    size_t cnt = 0, cap = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
            push_pieces(&ps, &cnt, &cap, t, t->turns.data[qi], qi);

        /* 在飞成员 (P2a-4b): active turn 还没提交, 但它是**最新**的内容。折它的前缀
         * 就是 pi 的 split turn —— 落地后请求从 turn->folded_prefix 之后开始发
         * (req->turn_begin 因此点亮)。只有 ALIVE 的成员算"在飞": SUSPENDED /
         * CANCELLED 手里那半个 turn 不属于当前对话, 等它续跑时再走常规路径。 */
        if (t->life == EDC_LIFE_ALIVE && t->active_turn)
            push_pieces(&ps, &cnt, &cap, t, t->active_turn, EDC_FOLD_CUT_ACTIVE_TURN);
    }
    *out = ps;
    return cnt;
}

sds edc_group_fold_build_prompt(struct edc_task_group *group, const char *focus, struct edc_fold_cut *out_cut)
{
    if (out_cut)
    {
        out_cut->task_id = NULL;
        out_cut->turn = 0;
        out_cut->prefix = 0;
    }
    if (!group)
        return NULL;

    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct fold_piece *ps = NULL;
    size_t cnt = collect_pieces(members, n, &ps);
    if (cnt == 0)
    {
        log_debug("L1 fold: group %s has nothing unfolded to fold", group->group_id);
        free(members);
        free(ps);
        return NULL;
    }

    long long total = 0;
    for (size_t i = 0; i < cnt; i++)
        total += ps[i].tokens;

    /* 保留多少条作细节。pi: keep_recent = 20k token 的尾部。整组还不够这个预算时
     * (小结组 —— 语料比它的摘要还小, 或手动 /group fold 点了个小小组) 没有"窗口"可比
     * —— 压最老一半 (老策略), 否则小组永远折不动。 */
    size_t keep = 0;
    if (total <= EDC_FOLD_KEEP_RECENT_TOKENS)
    {
        keep = cnt / 2;
        log_info("L1 fold: group %s is under keep_recent (%lld <= %d tokens) — folding the oldest half (%zu of %zu msgs)",
                 group->group_id, total, EDC_FOLD_KEEP_RECENT_TOKENS, cnt - keep, cnt);
    }
    else
    {
        long long acc = 0;
        size_t k = 0;
        for (size_t i = cnt; i > 0; i--)
        {
            acc += ps[i - 1].tokens;
            k++;
            if (acc >= EDC_FOLD_KEEP_RECENT_TOKENS)
                break;
        }
        keep = (k < cnt) ? k : cnt - 1; /* 至少折一条, 否则这次折叠没有意义 */
    }
    size_t cut_at = cnt - keep; /* 语料 = ps[0, cut_at) */

    /* (b) 语料上限: keep_recent 只说"留多少细节", 不说"这一发要压多大"。尾部预算
     * 只保证留下 20k, 剩下的可以是一整段历史 —— 那就把 >10:1 的压缩比压给摘要器,
     * 必然截断 (而截断被丢弃 ⇒ 折叠不落地 ⇒ 原样重试, 2026-10-09 现场见头文件)。
     * 这里把切点往回收, 直到语料装进 EDC_FOLD_MAX_CORPUS_TOKENS; 收不动 (第一条
     * 消息自己就超上限) 时留一条, 让上面的失败上限兜底 —— 绝不把切点收到 0, 那是
     * "该折却折不出东西"的空转。 */
    long long corpus = 0;
    for (size_t i = 0; i < cut_at; i++)
        corpus += ps[i].tokens;
    while (cut_at > 1 && corpus > EDC_FOLD_MAX_CORPUS_TOKENS)
    {
        cut_at--;
        corpus -= ps[cut_at].tokens;
    }

    /* 切点合法性 (pi §4): 第一条留下的消息不能是工具结果 —— 它的 assistant(tool_calls)
     * 已经被折进记忆, 留下孤立的 tool 消息会让整个请求失败。往回退到那条 assistant
     * (它的工具结果自然跟随保留), 也就是"宁可少折一条"。 */
    while (cut_at > 0 && cut_at < cnt && piece_is_tool_result(&ps[cut_at]))
        cut_at--;

    if (cut_at == 0)
    {
        free(members);
        free(ps);
        return NULL;
    }

    /* 切点 = 第一条留下的消息: 它所在成员 + 它那个 turn + 它在 turn 内的序号。一条都
     * 不留 (keep == 0, 只可能发生在整组只有一条未折叠消息时) 的切点 = 最后那条**之后**,
     * 也就是"全折"(落地时会把最后那个 turn 的前缀推到它的长度)。 */
    const struct fold_piece *cut_piece = &ps[cut_at < cnt ? cut_at : cnt - 1];
    uint32_t cut_prefix =
        (cut_at < cnt) ? (uint32_t)cut_piece->msg_index : (uint32_t)cut_piece->msg_index + 1;
    if (out_cut)
    {
        out_cut->task_id = cut_piece->task->task_id;
        out_cut->turn = (uint32_t)cut_piece->turn_index;
        out_cut->prefix = cut_prefix;
    }
    log_info("L1 fold: group %s fold %zu/%zu msgs (%lld corpus tokens, cap %d; keep_recent ~%d), "
             "cut at task %s turn %zu msg %u",
             group->group_id, cut_at, cnt, corpus, EDC_FOLD_MAX_CORPUS_TOKENS, EDC_FOLD_KEEP_RECENT_TOKENS,
             cut_piece->task->task_id, cut_piece->turn_index, cut_prefix);

    bool has_memory = (group->memory && sdslen(group->memory) > 0);
    sds prompt = sdsempty();
    prompt = sdscat(prompt, "<group-transcript>\n");

    struct edc_task *cur = NULL;
    for (size_t i = 0; i < cut_at; i++)
    {
        struct edc_task *tk = ps[i].task;
        if (tk != cur)
        {
            cur = tk;
            prompt = sdscatprintf(prompt, "\n### [task %s] %s\n", tk->task_id, tk->description ? tk->description : "");
        }
        cJSON *m = ps[i].turn->messages.data[ps[i].turn->messages.head + ps[i].msg_index];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *content = cJSON_GetObjectItem(m, "content");
        if (role && content && cJSON_IsString(role) && cJSON_IsString(content))
            prompt = sdscatprintf(prompt, "%s: %s\n", role->valuestring, content->valuestring);
    }

    /* 只有**整条进了语料**的成员才带 output (半折的成员, 它的尾巴还在历史里当细节)。 */
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *tk = members[i];
        if (tk == cut_piece->task)
            continue; /* 切点所在成员: 半折, 不带 output */
        bool in_corpus = false;
        for (size_t j = 0; j < cut_at && !in_corpus; j++)
            if (ps[j].task == tk)
                in_corpus = true;
        if (in_corpus && tk->output)
            prompt = sdscatprintf(prompt, "output: %s\n", tk->output);
    }

    prompt = sdscat(prompt, "</group-transcript>\n\n");
    if (has_memory)
    {
        prompt = sdscat(prompt, "<previous-summary>\n");
        prompt = sdscat(prompt, group->memory);
        prompt = sdscat(prompt, "\n</previous-summary>\n\n");
    }
    /* 语料以"没写完的 turn"结尾 (pi §5): 切点落在 turn 内部 (它后面的消息留着当细节),
     * 或者语料最后那段本来就是还在跑的 active turn (内容还没生成完)。 */
    bool split_turn = false;
    if (cut_at < cnt && cut_prefix > 0)
        split_turn = true; /* 切在 turn 中间 */
    if (cut_at > 0 && ps[cut_at - 1].turn_index == EDC_FOLD_CUT_ACTIVE_TURN)
        split_turn = true; /* 语料尾部是一个还在飞的 turn */
    if (split_turn)
        log_info("L1 fold: group %s split turn at the cut (pi §5 turn context section)", group->group_id);

    prompt = sdscat(prompt, has_memory ? kUpdateSummaryInstruction : kInitialSummaryInstruction);
    if (split_turn)
        prompt = sdscat(prompt, kSplitTurnInstruction);
    if (focus && *focus)
    {
        prompt = sdscat(prompt, "\n\nAdditional focus: ");
        prompt = sdscat(prompt, focus);
    }
    prompt = sdscat(prompt, "\n");

    free(members);
    free(ps);
    return prompt;
}

struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                      struct edc_task_group *group, const char *focus)
{
    if (!sched || !session || !group)
        return NULL;

    /* One fold per session at a time, or the second prompt reads unwritten state. */
    if (edc_scheduler_has_pending_system_task(session))
        return NULL;

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = edc_group_fold_build_prompt(group, focus, &cut);
    if (!prompt)
        return NULL;

    struct edc_task *task = edc_scheduler_spawn_system_task(sched, session, "fold group memory");
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    task->sink_group = group; /* 产出写进这个组的记忆 (sink = GROUP_MEMORY) */
    /* 语料与切点是同一遍算出来的: 落地照这个切点打前缀。 */
    task->fold_cut_task_id = cut.task_id ? strdup(cut.task_id) : NULL;
    task->fold_cut_turn = cut.turn;
    task->fold_cut_prefix = cut.prefix;

    /* No shared header any more: the fold request carries the instruction itself
     * (see edc_group_fold_system_prompt). The old version wrote it into the system
     * group's history[0] — which is not persisted, so after a restart the loader
     * refilled that slot with the agent's main system prompt and the "written once"
     * guard never corrected it: every fold call then ran with EdgeClaw's persona
     * and MEMORY.md in front of the transcript. */

    log_info("L1 fold start: group %s task=%s prompt=%zu bytes, cut=task %s turn %u msg %u", group->group_id,
             task->task_id, sdslen(prompt), task->fold_cut_task_id ? task->fold_cut_task_id : "(none)",
             task->fold_cut_turn, task->fold_cut_prefix);

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
static void fold_turn_whole(struct edc_turn *turn, size_t *turns, size_t *msgs)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t cur = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if (cur >= len)
        return;
    *msgs += len - cur;
    (*turns)++;
    turn->folded_prefix = (int)len;
}

/* 只折前 |prefix| 条 (前缀只许变长, 不许回退)。 */
static void fold_turn_prefix(struct edc_turn *turn, uint32_t prefix, size_t *turns, size_t *msgs)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t cur = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if ((size_t)prefix <= cur || (size_t)prefix > len)
        return;
    *msgs += (size_t)prefix - cur;
    (*turns)++;
    turn->folded_prefix = (int)prefix;
}

bool edc_group_fold_deliver_memory(struct edc_task *task, const char *content)
{
    struct edc_task_group *group = task ? task->sink_group : NULL;
    if (!group || !content || !*content)
    {
        log_error("L1 fold: task %s produced no memory (target=%s, content=%s) — group memory unchanged",
                  (task && task->task_id) ? task->task_id : "(null)", group ? "yes" : "none",
                  (content && *content) ? "non-empty" : "empty");
        /* (c) 没写出记忆 = 没落地, 一样算一次失败。 */
        edc_group_fold_note_failure(group);
        return false;
    }

    if (group->memory)
        sdsfree(group->memory);
    group->memory = sdsnew(content);

    /* 应用切点: 内容序上切点**之前**的成员整折 (含整条进语料的在飞成员 —— 它的 active
     * turn 也整折, 请求侧按前缀切片); 切点成员折到 cut_turn 的 fold_cut_prefix 条为止;
     * 那之后一律不动。切点为空 = 老二进制写下的在飞折叠任务 (没有切点信息) —— 记忆已经
     * 写了, 前缀没法打, 记一次错 (无回退/无重试)。 */
    if (!task->fold_cut_task_id)
    {
        log_error("L1 fold: task %s carries no cut (older format) — group %s memory updated but no prefix applied",
                  task->task_id, group->group_id);
        edc_task_group_rebuild_history(group);
        group->fold_epoch++;
        edc_group_fold_note_failure(group); /* (c) 前缀没打上 = 没真正折掉东西 */
        return true; /* 记忆写了 —— 对 L2 来说这就是新料 */
    }

    /* 先把切点**精确**解析出来 (成员 + 那个 turn), 解析不出来就只写记忆、什么都不折。
     * 宁可这轮白折 (内容留在历史里, 顶多与记忆重复), 也不能折进记忆没覆盖的消息 ——
     * 那是静默丢内容。 */
    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct edc_task *cut_member = NULL;
    for (size_t i = 0; i < n && !cut_member; i++)
    {
        if (strcmp(members[i]->task_id, task->fold_cut_task_id) == 0)
            cut_member = members[i];
    }
    struct edc_turn *cut_turn = NULL;
    if (cut_member)
    {
        if ((uint32_t)task->fold_cut_turn == EDC_FOLD_CUT_ACTIVE_TURN)
            cut_turn = cut_member->active_turn
                           ? cut_member->active_turn
                           : (cut_member->turns.tail > cut_member->turns.head
                                  ? cut_member->turns.data[cut_member->turns.tail - 1]
                                  : NULL);
        else if ((size_t)task->fold_cut_turn >= cut_member->turns.head &&
                 (size_t)task->fold_cut_turn < cut_member->turns.tail)
            cut_turn = cut_member->turns.data[(size_t)task->fold_cut_turn];
    }
    if (!cut_member || !cut_turn)
    {
        log_error("L1 fold: cut (task=%s turn=%u) does not resolve in group %s — memory updated, nothing folded",
                  task->fold_cut_task_id, task->fold_cut_turn, group->group_id);
        free(members);
        edc_task_group_rebuild_history(group);
        group->fold_epoch++;
        edc_group_fold_note_failure(group); /* (c) 白折一轮: 没折掉任何东西 */
        return true; /* 记忆写了 —— L2 仍该消化它 (否则这段新料永远进不了 session) */
    }

    /* 应用切点: 切点**之前**的内容整折 —— 已提交的 turn 一律安全 (提交后不再变),
     * 在飞 turn 只在"它当时进了语料"的同一条件下整折 (语料只收 ALIVE 成员的 active
     * turn, 这里必须照同一条件, 否则会把没进过摘要的内容折掉); 切点那个 turn 只折前缀;
     * 之后一律不动。 */
    size_t folded_turns = 0, folded_msgs = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        bool stop_after = false;
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            if (turn == cut_turn)
            {
                fold_turn_prefix(turn, task->fold_cut_prefix, &folded_turns, &folded_msgs);
                stop_after = true;
                break;
            }
            fold_turn_whole(turn, &folded_turns, &folded_msgs);
        }
        if (!stop_after && t->active_turn && t->active_turn != cut_turn && t->life == EDC_LIFE_ALIVE)
            fold_turn_whole(t->active_turn, &folded_turns, &folded_msgs);
        if (!stop_after && cut_turn && cut_turn == t->active_turn)
        {
            /* 切点就在 active turn 上 (它没提交, 不在 turns 里): 上面的循环已把它之前
             * 的 turn 整折完。 */
            fold_turn_prefix(cut_turn, task->fold_cut_prefix, &folded_turns, &folded_msgs);
        }
        if (t == cut_member)
            break;
    }
    free(members);
    /* The history says what will be sent: drop what the memory now covers. */
    edc_task_group_rebuild_history(group);
    /* The prefix these members' requests counted is gone: every anchor older than
     * this is stale from here on (see the fold_epoch field). */
    group->fold_epoch++;
    /* (c) 落地成功 ⇒ 连续失败账清零。 */
    edc_group_fold_note_progress(group);
    log_info("L1 fold done: group %s folded %zu turns (%zu msgs), cut at task %s turn %u msg %u", group->group_id,
             folded_turns, folded_msgs, task->fold_cut_task_id, task->fold_cut_turn, task->fold_cut_prefix);
    return true;
}
