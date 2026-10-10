#include "edc_session_memory.h"

#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"

#include "edc_compat.h"
#include "log.h"
#include "utlist_fix.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ── 词表 ──────────────────────────────────────────────────── */

const char *edc_memo_event_kind_name(enum edc_memo_event_kind kind)
{
    switch (kind)
    {
    case EDC_MEMO_CREATED:
        return "created";
    case EDC_MEMO_UPDATED:
        return "updated";
    case EDC_MEMO_SUPERSEDED:
        return "superseded";
    }
    return "created";
}

enum edc_memo_event_kind edc_memo_event_classify_kind(const char *name)
{
    if (!name)
        return EDC_MEMO_CREATED;
    if (strcmp(name, "updated") == 0)
        return EDC_MEMO_UPDATED;
    if (strcmp(name, "superseded") == 0)
        return EDC_MEMO_SUPERSEDED;
    return EDC_MEMO_CREATED;
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_session_memory *edc_session_memory_new(void)
{
    return g_new0(struct edc_session_memory, 1);
}

static void memo_event_free(struct edc_memo_event *e)
{
    free(e->entry_id);
    free(e->title);
    free(e->summary);
}

static void memo_entry_free(struct edc_memo_entry *e)
{
    free(e->id);
    free(e->title);
    free(e->summary);
    free(e->sources);
    free(e->proposed_by);
    if (e->detail)
        sdsfree(e->detail);
}

void edc_session_memory_free(struct edc_session_memory *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < m->event_count; i++)
        memo_event_free(&m->events[i]);
    free(m->events);
    for (size_t i = 0; i < m->entry_count; i++)
        memo_entry_free(&m->entries[i]);
    free(m->entries);
    free(m);
}

/* ── 小工具 ────────────────────────────────────────────────── */

static void *grow_array(void *data, size_t *cap, size_t elem_size, size_t need)
{
    if (need <= *cap)
        return data;
    size_t next = (*cap == 0) ? 8 : (*cap * 2);
    while (next < need)
        next *= 2;
    void *grown = realloc(data, next * elem_size);
    if (!grown)
        return NULL;
    *cap = next;
    return grown;
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* ── 事件表 ────────────────────────────────────────────────── */

/* 追加一条**已定 seq** 的事件 (装配用的内部件: 写者给 seq, 读回时用盘上的 seq)。 */
static struct edc_memo_event *append_event(struct edc_session_memory *m, uint64_t seq, const char *entry_id,
                                           enum edc_memo_event_kind kind, const char *title, const char *summary)
{
    if (!m || !entry_id || seq == 0)
        return NULL;
    void *grown = grow_array(m->events, &m->event_cap, sizeof(*m->events), m->event_count + 1);
    if (!grown)
        return NULL;
    m->events = grown;

    struct edc_memo_event *e = &m->events[m->event_count];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->entry_id = dup_or_null(entry_id);
    e->kind = kind;
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    m->event_count++;
    return e;
}

uint64_t edc_session_memory_add_event(struct edc_session_memory *m, const char *entry_id,
                                      enum edc_memo_event_kind kind, const char *title, const char *summary)
{
    if (!m)
        return 0;
    uint64_t seq = m->next_seq + 1;
    if (!append_event(m, seq, entry_id, kind, title, summary))
        return 0;
    m->next_seq = seq;
    return seq;
}

/* ── 条目表 ────────────────────────────────────────────────── */

struct edc_memo_entry *edc_session_memory_entry(struct edc_session_memory *m, const char *id)
{
    if (!m || !id)
        return NULL;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        if (m->entries[i].id && strcmp(m->entries[i].id, id) == 0)
            return &m->entries[i];
    }
    return NULL;
}

struct edc_memo_entry *edc_session_memory_upsert_entry(struct edc_session_memory *m, const char *id,
                                                       const char *title, const char *summary, const char *detail,
                                                       const char *sources)
{
    if (!m || !id)
        return NULL;

    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    if (!e)
    {
        void *grown = grow_array(m->entries, &m->entry_cap, sizeof(*m->entries), m->entry_count + 1);
        if (!grown)
            return NULL;
        m->entries = grown;
        e = &m->entries[m->entry_count];
        memset(e, 0, sizeof(*e));
        m->entry_count++;
        e->id = dup_or_null(id);
    }

    free(e->title);
    free(e->summary);
    free(e->sources);
    if (e->detail)
        sdsfree(e->detail);
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    e->sources = dup_or_null(sources);
    e->detail = detail ? sdsnew(detail) : NULL;
    return e;
}

/* ── 给工具与通知用的读/写口 ───────────────────────────────── */

/* 条目按 id 升序 ("m12" 的数字部分) —— 保证 memory_list 翻页稳定。 */
static int entry_id_num(const char *id)
{
    if (!id || id[0] != 'm')
        return 0;
    return atoi(id + 1);
}

static sds index_line_for(const struct edc_memo_entry *e)
{
    sds line = sdscatprintf(sdsempty(), "- [%s]%s %s", e->id ? e->id : "?",
                            (e->superseded ? " (retired)" : ""), e->title ? e->title : "(untitled)");
    if (e->summary && e->summary[0])
        line = sdscatprintf(line, " — %s", e->summary);
    return line;
}

sds edc_session_memory_index_lines(const struct edc_session_memory *m, size_t offset, size_t n, size_t *out_total)
{
    if (out_total)
        *out_total = m ? m->entry_count : 0;
    if (!m || n == 0)
        return sdsempty();

    /* 收集 + 按 id 号升序排 (条目数不大, 直接插入排序) */
    struct edc_memo_entry **sorted = malloc(m->entry_count * sizeof(*sorted));
    if (!sorted)
        return sdsempty();
    size_t k = 0;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        size_t j = k++;
        while (j > 0 && entry_id_num(sorted[j - 1]->id) > entry_id_num(m->entries[i].id))
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = &m->entries[i];
    }

    sds out = sdsempty();
    for (size_t i = offset; i < k && i < offset + n; i++)
    {
        if (sdslen(out))
            out = sdscat(out, "\n");
        sds line = index_line_for(sorted[i]);
        out = sdscatsds(out, line);
        sdsfree(line);
    }
    free(sorted);
    return out;
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
    {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

sds edc_session_memory_search_lines(const struct edc_session_memory *m, const char *query, size_t cap, size_t *out_hits)
{
    if (out_hits)
        *out_hits = 0;
    if (!m || !query || !query[0] || cap == 0 || m->entry_count == 0)
        return sdsempty();

    /* 命中下标, 按 updated_seq (退化成 created_seq) 倒序输出 —— 搜索是"看一眼",
     * 稳定排序不作要求。 */
    size_t *hits = malloc(m->entry_count * sizeof(*hits));
    if (!hits)
        return sdsempty();
    size_t nhits = 0;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        if (contains_ci(e->title, query) || contains_ci(e->summary, query) ||
            (e->detail && contains_ci(e->detail, query)))
        {
            size_t j = nhits++;
            while (j > 0)
            {
                const struct edc_memo_entry *prev = &m->entries[hits[j - 1]];
                uint64_t a = prev->updated_seq ? prev->updated_seq : prev->created_seq;
                uint64_t b = e->updated_seq ? e->updated_seq : e->created_seq;
                if (a >= b)
                    break;
                hits[j] = hits[j - 1];
                j--;
            }
            hits[j] = i;
        }
    }

    sds out = sdsempty();
    for (size_t i = 0; i < nhits && i < cap; i++)
    {
        if (sdslen(out))
            out = sdscat(out, "\n");
        sds line = index_line_for(&m->entries[hits[i]]);
        out = sdscatsds(out, line);
        sdsfree(line);
    }
    free(hits);
    if (out_hits)
        *out_hits = nhits;
    return out;
}

sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id)
{
    if (!m || !id)
        return NULL;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        if (e->id && strcmp(e->id, id) == 0)
            return (e->detail && sdslen(e->detail) > 0) ? sdsdup(e->detail) : index_line_for(e);
    }
    return NULL;
}

sds edc_session_memory_create_entry(struct edc_session_memory *m, const char *title, const char *summary,
                                    const char *detail, const char *proposed_by)
{
    if (!m || !title)
        return NULL;
    sds id = sdscatprintf(sdsempty(), "m%llu", (unsigned long long)(m->next_entry_id + 1));
    struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, id, title, summary, detail, NULL);
    if (!e)
    {
        sdsfree(id);
        return NULL;
    }
    m->next_entry_id++;
    if (proposed_by)
        edc_session_memory_note_proposer(e, proposed_by);
    /* created_seq/updated_seq 由调用方按它追加的事件 seq 填 (见 memory_write / L2 落地)。 */
    return id;
}

/* ── L2 策展任务 ───────────────────────────────────────────── */

/* 策展说明: 与折叠说明同族 —— 自足请求的 system 段, 常量文本 (不随记忆变化,
 * 所以 L2 调用的前缀是稳定的)。输出契约见 docs/l2-session-memory.md §4.7。 */
static const char kCurateSystemPrompt[] =
    "You are the session-memory curator for the EdgeClaw agent.\n"
    "\n"
    "You receive (a) the memory of ONE group that was just folded, and (b) the index of the\n"
    "session's existing memory entries. Your job: turn the material into durable entries that\n"
    "other tasks in this session will need later, and keep the existing entries consistent.\n"
    "\n"
    "Rules:\n"
    "- Record knowledge, not activity: decisions, constraints, gotchas, external facts, exact\n"
    "  paths / commands / error text. Never progress logs, todos, or transient conclusions.\n"
    "- Prefer `update` on an existing entry (by its id) over creating a near-duplicate.\n"
    "- `supersede` an entry the material shows to be obsolete or wrong.\n"
    "- Keep entries small: title <= 80 chars, summary <= 240 chars, detail holds the rest.\n"
    "- Judgment, not transcribing: merge what belongs together, drop what does not matter.\n"
    "\n"
    "Output ONLY one JSON object, no prose and no code fences:\n"
    "{\"ops\":[{\"op\":\"create\",\"title\":\"...\",\"summary\":\"...\",\"detail\":\"...\"},\n"
    "         {\"op\":\"update\",\"id\":\"m7\",\"title\":\"...\",\"summary\":\"...\",\"detail\":\"...\"},\n"
    "         {\"op\":\"supersede\",\"id\":\"m9\"}]}\n"
    "Use {\"ops\":[]} when the material holds nothing worth recording.\n";

const char *edc_session_memory_system_prompt(void)
{
    return kCurateSystemPrompt;
}

/* 材料 (L2 的 user 消息): 组记忆 + 现有条目索引 (+ 手动/自动的 focus)。
 * `group == NULL` = 手动策展: 收**所有**有记忆的组。 */
static sds build_curate_prompt(struct edc_session *session, struct edc_task_group *group, const char *focus)
{
    sds out = sdsempty();
    if (!group)
        out = sdscat(out, "## Group memories (all groups of this session)\n");
    else
        out = sdscatprintf(out, "## Group memory (group %s, just folded)\n",
                           group->group_id ? group->group_id : "?");

    if (group)
    {
        out = sdscat(out, (group->memory && sdslen(group->memory) > 0) ? group->memory : "(empty)");
    }
    else
    {
        bool any = false;
        struct edc_task_group *g = NULL;
        if (session)
        {
            DL_FOREACH(session->groups, g)
            {
                if (!g->memory || sdslen(g->memory) == 0)
                    continue;
                any = true;
                out = sdscatprintf(out, "\n### group %s\n%s", g->group_id ? g->group_id : "?", g->memory);
            }
        }
        if (!any)
            out = sdscat(out, "(no group has memory yet)");
    }

    if (focus && *focus)
        out = sdscatprintf(out, "\n\nAdditional focus: %s", focus);
    out = sdscat(out, "\n\n## Existing session entries\n");

    const struct edc_session_memory *m = session ? session->memo : NULL;
    size_t total = 0;
    sds index = edc_session_memory_index_lines(m, 0, 50, &total);
    if (sdslen(index))
        out = sdscatsds(out, index);
    else
        out = sdscat(out, "(none yet)");
    if (total > 50)
        out = sdscatprintf(out, "\n(… %zu more; this is the first 50)", total - 50);
    sdsfree(index);
    out = sdscat(out, "\n\nNow return the ops JSON.\n");
    return out;
}

struct edc_task *edc_session_memory_start(struct edc_scheduler *sched, struct edc_session *session,
                                          struct edc_task_group *folded_group, struct edc_task *fold_task)
{
    if (!sched || !session || !session->memo)
        return NULL;

    sds prompt = build_curate_prompt(session, folded_group, NULL);
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "curate session memory", EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    /* 记住来源组 (sources / 日志用; 它不参与"某组在折"的判定 —— 那条只认 GROUP_MEMORY)。 */
    task->sink_group = folded_group;

    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo: could not attach the curation prompt — no curation this round");
        edc_task_mark_failed(task); /* 失败也会释放依赖者 */
        sdsfree(prompt);
        return NULL;
    }
    sdsfree(prompt);

    /* 链: L2 → L1 (声明式: L1 已完成, 这条边一建就是 completed), 成员 → L2 (真正让它们等)。
     * 加边发生在同一个消息处理里, 扫描还没跑 ⇒ 被释放的成员会立刻重新 park 到 AWAIT_DEPS。 */
    if (fold_task)
        edc_task_add_dependency(task, fold_task, TASK_DEP_SEQUENCE);
    if (folded_group)
    {
        struct edc_task *m;
        LL_FOREACH2(folded_group->members, m, group_next)
        {
            if (edc_task_is_completed(m) || edc_task_is_cancelled(m) || edc_task_has_failed(m))
                continue;
            edc_task_add_dependency(m, task, TASK_DEP_SEQUENCE);
        }
    }

    log_info("L2 memo start: session=%s from group=%s task=%s entries=%zu", session->session_id,
             (folded_group && folded_group->group_id) ? folded_group->group_id : "?", task->task_id,
             session->memo->entry_count);
    return task;
}

struct edc_task *edc_session_memory_start_manual(struct edc_scheduler *sched, struct edc_session *session,
                                                  const char *focus)
{
    if (!sched || !session || !session->memo)
        return NULL;
    /* 与自动路径同一条闸门: 一个 session 同时只有一个 system task。 */
    if (edc_scheduler_has_pending_system_task(session))
    {
        log_info("L2 memo: manual compaction refused — a system task is already in flight");
        return NULL;
    }

    sds prompt = build_curate_prompt(session, NULL, focus);
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "curate session memory", EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo: could not attach the manual curation prompt — nothing started");
        edc_task_mark_failed(task);
        sdsfree(prompt);
        return NULL;
    }
    sdsfree(prompt);
    log_info("L2 memo start (manual): session=%s task=%s entries=%zu", session->session_id, task->task_id,
             session->memo->entry_count);
    return task;
}

/* ── `/session memo` 的文本 ────────────────────────────────── */

sds edc_session_memory_show_text(const struct edc_session_memory *m, const char *id)
{
    if (!m)
        return sdsnew("**Session memory** — no session loaded.");

    if (id && *id)
    {
        sds detail = edc_session_memory_get_detail(m, id);
        if (!detail)
            return sdscatprintf(sdsempty(), "**Session memory** — no entry `%s`.", id);
        struct edc_memo_entry *e = NULL;
        for (size_t i = 0; i < m->entry_count; i++)
        {
            if (m->entries[i].id && strcmp(m->entries[i].id, id) == 0)
                e = &m->entries[i];
        }
        sds out = sdscatprintf(sdsempty(), "**Session memory · %s**%s\n\n", id,
                               (e && e->superseded) ? " (retired)" : "");
        if (e && e->title)
            out = sdscatprintf(out, "**%s**\n\n", e->title);
        out = sdscatsds(out, detail);
        if (e && e->sources)
            out = sdscatprintf(out, "\n\n_sources: %s_", e->sources);
        if (e && e->proposed_by)
            out = sdscatprintf(out, "  _proposed by task %s_", e->proposed_by);
        sdsfree(detail);
        return out;
    }

    size_t total = 0;
    sds lines = edc_session_memory_index_lines(m, 0, 200, &total);
    if (total == 0)
        return sdsnew("**Session memory** — nothing yet. It fills up when a group is folded "
                      "(`/group fold`) or a task records something (`memory_write`).");
    sds out = sdscatprintf(sdsempty(), "**Session memory** — %zu entr%s\n\n", total, total == 1 ? "y" : "ies");
    out = sdscatsds(out, lines);
    out = sdscat(out, "\n\n`/session memo show <id>` for one entry · `/session memo search <q>` · "
                      "`/session memo compact` to curate now");
    sdsfree(lines);
    return out;
}

sds edc_session_memory_search_text(const struct edc_session_memory *m, const char *query)
{
    if (!m || !query || !*query)
        return sdsnew("Usage: `/session memo search <keyword>`");
    size_t hits = 0;
    sds lines = edc_session_memory_search_lines(m, query, 50, &hits);
    if (hits == 0)
    {
        sdsfree(lines);
        return sdscatprintf(sdsempty(), "**Session memory** — nothing matches `%s`.", query);
    }
    sds out = sdscatprintf(sdsempty(), "**Session memory** — %zu hit%s for `%s`\n\n", hits, hits == 1 ? "" : "s",
                           query);
    out = sdscatsds(out, lines);
    sdsfree(lines);
    return out;
}

/* ── ops 契约: 先整批校验, 再应用 ─────────────────────────── */

static const char *op_str(const cJSON *op, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(op, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

bool edc_session_memory_apply_ops(struct edc_session_memory *m, const char *text, const char *sources)
{
    if (!m || !text || !*text)
        return false;

    cJSON *root = cJSON_Parse(text);
    if (!root)
        return false;
    cJSON *ops = cJSON_GetObjectItemCaseSensitive(root, "ops");
    bool ok = cJSON_IsArray(ops);
    size_t creates = 0;

    /* 第一遍: 整批校验 (任一条不合法 ⇒ 什么都不落) */
    if (ok)
    {
        const cJSON *op = NULL;
        cJSON_ArrayForEach(op, ops)
        {
            const char *kind = op_str(op, "op");
            if (!kind)
                ok = false;
            else if (strcmp(kind, "create") == 0)
            {
                const char *title = op_str(op, "title");
                if (!title || !*title)
                    ok = false;
                creates++;
            }
            else if (strcmp(kind, "update") == 0 || strcmp(kind, "supersede") == 0)
            {
                const char *id = op_str(op, "id");
                if (!id || !*id || !edc_session_memory_entry(m, id))
                    ok = false;
            }
            else
            {
                ok = false;
            }
            if (!ok)
                break;
        }
    }
    if (ok && m->entry_count + creates > EDC_SESSION_MEMORY_MAX_ENTRIES)
    {
        log_error("L2 memo: batch would grow past %d entries — refused", EDC_SESSION_MEMORY_MAX_ENTRIES);
        ok = false;
    }
    if (!ok)
    {
        log_error("L2 memo: invalid ops — nothing landed (entries and events untouched)");
        cJSON_Delete(root);
        return false;
    }

    /* 第二遍: 应用 */
    const cJSON *op = NULL;
    cJSON_ArrayForEach(op, ops)
    {
        const char *kind = op_str(op, "op");
        if (strcmp(kind, "create") == 0)
        {
            const char *title = op_str(op, "title");
            const char *summary = op_str(op, "summary");
            const char *detail = op_str(op, "detail");
            sds id = edc_session_memory_create_entry(m, title, summary, detail, NULL);
            if (!id)
                continue;
            struct edc_memo_entry *e = edc_session_memory_entry(m, id);
            uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_CREATED, title, summary);
            if (e)
            {
                e->created_seq = seq;
                e->updated_seq = seq;
                if (sources)
                    e->sources = strdup(sources);
            }
            sdsfree(id);
        }
        else if (strcmp(kind, "update") == 0)
        {
            const char *id = op_str(op, "id");
            struct edc_memo_entry *e = edc_session_memory_entry(m, id);
            const char *title = op_str(op, "title");
            const char *summary = op_str(op, "summary");
            const char *detail = op_str(op, "detail");
            /* 只覆盖**给了的**字段 (缺的保持原样)。 */
            if (title)
            {
                free(e->title);
                e->title = strdup(title);
            }
            if (summary)
            {
                free(e->summary);
                e->summary = strdup(summary);
            }
            if (detail)
            {
                if (e->detail)
                    sdsfree(e->detail);
                e->detail = sdsnew(detail);
            }
            uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_UPDATED, e->title, e->summary);
            e->updated_seq = seq;
        }
        else /* supersede */
        {
            const char *id = op_str(op, "id");
            struct edc_memo_entry *e = edc_session_memory_entry(m, id);
            e->superseded = true;
            uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_SUPERSEDED, e->title, e->summary);
            e->updated_seq = seq;
        }
    }
    cJSON_Delete(root);
    return true;
}

bool edc_session_memory_deliver(struct edc_task *task, const char *content)
{
    struct edc_session *session = (task && task->group) ? task->group->session : NULL;
    if (!session || !session->memo)
    {
        log_error("L2 memo: no session memory to land into — nothing written");
        return false;
    }
    if (!content || !*content)
    {
        log_error("L2 memo: task %s returned nothing — nothing landed", task->task_id ? task->task_id : "(null)");
        return false;
    }
    return edc_session_memory_apply_ops(session->memo, content,
                                        (task->sink_group && task->sink_group->group_id)
                                            ? task->sink_group->group_id
                                            : NULL);
}

/* ── 队尾通知 ─────────────────────────────────────────────── */

sds edc_session_memory_notice(const struct edc_session_memory *m, uint64_t seen, size_t cap, uint64_t *out_max_injected)
{
    if (out_max_injected)
        *out_max_injected = 0;
    if (!m || m->next_seq <= seen || m->event_count == 0)
        return NULL;

    /* 同一 id 只留最新那条事件 (模型要的是"现在的样子", 不是流水) */
    const struct edc_memo_event **picked = malloc(m->event_count * sizeof(*picked));
    if (!picked)
        return NULL;
    size_t npick = 0;
    for (size_t i = 0; i < m->event_count; i++)
    {
        const struct edc_memo_event *e = &m->events[i];
        if (e->seq <= seen || !e->entry_id)
            continue;
        size_t j = 0;
        for (; j < npick; j++)
        {
            if (picked[j]->entry_id && strcmp(picked[j]->entry_id, e->entry_id) == 0)
            {
                if (e->seq > picked[j]->seq) /* 只换新, 不换旧 */
                    picked[j] = e;
                break;
            }
        }
        if (j == npick)
            picked[npick++] = e;
    }
    if (npick == 0)
    {
        free(picked);
        return NULL;
    }

    /* 先按 seq 升序 (pick 是"首次出现"序, 不保证单调), 再取**最新的** cap 条 */
    for (size_t i = 1; i < npick; i++)
    {
        const struct edc_memo_event *cur = picked[i];
        size_t j = i;
        while (j > 0 && picked[j - 1]->seq > cur->seq)
        {
            picked[j] = picked[j - 1];
            j--;
        }
        picked[j] = cur;
    }

    /* 最新的 cap 条 (超出只报数) */
    size_t first = (npick > cap) ? (npick - cap) : 0;
    size_t shown = npick - first;

    sds body = sdsempty();
    for (size_t i = first; i < npick; i++)
    {
        const struct edc_memo_event *ev = picked[i];
        const struct edc_memo_entry *en = NULL;
        for (size_t k = 0; k < m->entry_count; k++)
        {
            if (m->entries[k].id && strcmp(m->entries[k].id, ev->entry_id) == 0)
            {
                en = &m->entries[k];
                break;
            }
        }
        if (sdslen(body))
            body = sdscat(body, "\n");
        body = sdscatprintf(body, "- [%s] %s: %s", ev->entry_id, edc_memo_event_kind_name(ev->kind),
                            en && en->title ? en->title : (ev->title ? ev->title : "(untitled)"));
        const char *sum = (en && en->summary && en->summary[0]) ? en->summary : ev->summary;
        if (sum && sum[0])
            body = sdscatprintf(body, " — %s", sum);
    }

    sds out = sdscatprintf(sdsempty(), "## Session Memory Updates  (共 %zu 条)\n%s", m->entry_count, body);
    if (npick > shown)
        out = sdscatprintf(out, "\n… %zu more（memory_list 0 20 / memory_search <关键词> / memory_get <id>）",
                           npick - shown);
    else
        out = sdscat(out, "\n（memory_list / memory_search / memory_get 可查完整内容）");

    free(picked);
    /* 水位推进到**表头**: "最新 cap 条"窗口保证最新那条一定在里面, 被 cap 截掉的
     * 老事件由通知里那句 "N more" + memory_list/memory_search 兜 (见头文件说明)。 */
    if (out_max_injected)
        *out_max_injected = m->next_seq;
    return out;
}

/* ── 出生点采纳 / 提议者 ────────────────────────────────────── */

void edc_session_memory_adopt_task(struct edc_session *session, struct edc_task *task)
{
    if (!session || !task || !session->memo)
        return;
    /* 0 = "什么都没看过" ⇒ 第一次请求自带一次回填通知 (最新 cap 条)。 */
    task->memo_seen_seq = 0;
}

void edc_session_memory_note_proposer(struct edc_memo_entry *entry, const char *task_id)
{
    if (!entry)
        return;
    free(entry->proposed_by);
    entry->proposed_by = dup_or_null(task_id);
}

/* ── 落盘 / 读回 ───────────────────────────────────────────── */

static void json_add_str(cJSON *o, const char *key, const char *v)
{
    if (v)
        cJSON_AddStringToObject(o, key, v);
}

cJSON *edc_session_memory_to_json(const struct edc_session_memory *m)
{
    cJSON *root = cJSON_CreateObject();
    if (!m)
        return root; /* 空结构也写: 加载端不必处理"这个键不在" */

    cJSON_AddNumberToObject(root, "next_seq", (double)m->next_seq);
    cJSON_AddNumberToObject(root, "next_entry_id", (double)m->next_entry_id);
    cJSON_AddNumberToObject(root, "pruned_before", (double)m->pruned_before);

    cJSON *events = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < m->event_count; i++)
    {
        const struct edc_memo_event *e = &m->events[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", (double)e->seq);
        json_add_str(o, "entry_id", e->entry_id);
        cJSON_AddStringToObject(o, "kind", edc_memo_event_kind_name(e->kind));
        json_add_str(o, "title", e->title);
        json_add_str(o, "summary", e->summary);
        cJSON_AddItemToArray(events, o);
    }

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        cJSON *o = cJSON_CreateObject();
        json_add_str(o, "id", e->id);
        json_add_str(o, "title", e->title);
        json_add_str(o, "summary", e->summary);
        if (e->detail)
            cJSON_AddStringToObject(o, "detail", e->detail);
        json_add_str(o, "sources", e->sources);
        json_add_str(o, "proposed_by", e->proposed_by);
        cJSON_AddNumberToObject(o, "created_seq", (double)e->created_seq);
        cJSON_AddNumberToObject(o, "updated_seq", (double)e->updated_seq);
        cJSON_AddBoolToObject(o, "superseded", e->superseded);
        cJSON_AddItemToArray(entries, o);
    }

    return root;
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

struct edc_session_memory *edc_session_memory_from_json(const cJSON *json)
{
    struct edc_session_memory *m = edc_session_memory_new();
    if (!m)
        return NULL;
    if (!json || !cJSON_IsObject(json))
        return m;

    m->next_seq = (uint64_t)json_num(json, "next_seq");
    m->next_entry_id = (uint64_t)json_num(json, "next_entry_id");
    m->pruned_before = (uint64_t)json_num(json, "pruned_before");

    /* 事件: 用盘上的 seq 回填 (单调由写者保证; next_seq 跟着抬到最大值)。 */
    const cJSON *ev;
    cJSON_ArrayForEach(ev, cJSON_GetObjectItemCaseSensitive(json, "events"))
    {
        const char *eid = json_str(ev, "entry_id");
        if (!eid)
            continue;
        uint64_t seq = (uint64_t)json_num(ev, "seq");
        if (!append_event(m, seq, eid, edc_memo_event_classify_kind(json_str(ev, "kind")), json_str(ev, "title"),
                          json_str(ev, "summary")))
            continue;
        if (seq > m->next_seq)
            m->next_seq = seq;
    }

    const cJSON *en;
    cJSON_ArrayForEach(en, cJSON_GetObjectItemCaseSensitive(json, "entries"))
    {
        const char *id = json_str(en, "id");
        if (!id)
            continue;
        struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, id, json_str(en, "title"),
                                                                   json_str(en, "summary"), json_str(en, "detail"),
                                                                   json_str(en, "sources"));
        if (!e)
            continue;
        edc_session_memory_note_proposer(e, json_str(en, "proposed_by"));
        /* 缺键的老档 (或手写的) 也能继续发号: 抬到"已见过的最大 id"之上 */
        if ((uint64_t)entry_id_num(id) > m->next_entry_id)
            m->next_entry_id = (uint64_t)entry_id_num(id);
        e->created_seq = (uint64_t)json_num(en, "created_seq");
        e->updated_seq = (uint64_t)json_num(en, "updated_seq");
        const cJSON *sup = cJSON_GetObjectItemCaseSensitive(en, "superseded");
        e->superseded = cJSON_IsTrue(sup) != 0;
    }

    return m;
}
