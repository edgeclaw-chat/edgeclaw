#include "edc_session.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_state_serializer.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct edc_session *edc_session_new(struct edc_config *config, const char *session_id, const char *name)
{
    return edc_session_new_with_workdir(config, session_id, name, NULL);
}

const char *edc_session_get_workdir(struct edc_session *session)
{
    if (!session)
        return NULL;
    return session->workdir;
}

bool edc_session_set_workdir(struct edc_session *session, const char *workdir, EdcError **error)
{
    if (!session)
        return false;

    /* NULL/empty unbinds: back to the launch directory. */
    if (!workdir || !workdir[0])
    {
        if (session->workdir)
        {
            sdsfree(session->workdir);
            session->workdir = NULL;
        }
        return true;
    }

    /* A relative |workdir| resolves against the process cwd here (callers that
     * want a different anchor pre-resolve with edc_session_resolve_workdir). */
    sds resolved = edc_session_resolve_workdir(NULL, workdir, error);
    if (!resolved)
        return false;

    /* The bound directory must be writable inside the sandbox, same as the
     * launch directory: grant it before storing it, so tool execution there
     * cannot be denied by path validation or the kernel sandbox. */
    edc_config_add_sandbox_rw(session->config, resolved);

    if (session->workdir)
        sdsfree(session->workdir);
    session->workdir = sdsnew(resolved);
    sdsfree(resolved);

    log_info("session %s bound to working directory %s", session->session_id, session->workdir);
    return true;
}

sds edc_session_resolve_workdir(const char *base, const char *workdir, EdcError **error)
{
    if (!workdir || !workdir[0])
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "empty working directory");
        return NULL;
    }

    /* Absolute paths are used as-is; a leading "~" expands to the home
     * directory; any other relative path is anchored at |base| (a session's
     * current working directory), or the process cwd when none. */
    sds candidate;
    if (workdir[0] == '~' && (workdir[1] == '/' || workdir[1] == '\0'))
    {
        sds home = edc_homedir();
        candidate = workdir[1] ? edc_path_join(home, workdir + 2, NULL) : sdsnew(home);
        sdsfree(home);
    }
    else if (workdir[0] == '/')
    {
        candidate = sdsnew(workdir);
    }
    else
    {
        sds anchor;
        if (base && base[0])
        {
            anchor = sdsnew(base);
        }
        else
        {
            anchor = edc_cwd();
        }
        candidate = edc_path_join(anchor, workdir, NULL);
        sdsfree(anchor);
    }

    char *resolved = realpath(candidate, NULL);
    if (!resolved)
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "working directory does not exist: %s", candidate);
        sdsfree(candidate);
        return NULL;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "not a directory: %s", candidate);
        free(resolved);
        sdsfree(candidate);
        return NULL;
    }

    sds out = sdsnew(resolved);
    free(resolved);
    sdsfree(candidate);
    return out;
}

struct edc_session *edc_session_new_with_workdir(struct edc_config *config, const char *session_id, const char *name,
                                                 const char *workdir)
{
    struct edc_session *session = g_new0(struct edc_session, 1);
    session->session_id = strdup(session_id ? session_id : "sess-1");
    session->name = sdsnew(name && strlen(name) > 0 ? name : session->session_id);
    session->created_at = edc_datetime_timestamp();
    session->last_active = edc_datetime_timestamp();
    session->config = config;
    session->history_dir = edc_path_join(config->workspace, session->session_id, NULL);
    session->next_completion_seq = 1;

    if (workdir && workdir[0])
    {
        /* An explicitly requested working directory is a precondition, not a
         * hint: if it cannot be resolved to an existing directory, fail the
         * creation rather than silently falling back to the launch directory. */
        EdcError *error = NULL;
        if (!edc_session_set_workdir(session, workdir, &error))
        {
            log_warn("session %s: cannot bind working directory '%s': %s", session->session_id, workdir,
                     error ? error->message : "unknown");
            edc_error_free(error);
            edc_session_free(session);
            return NULL;
        }
    }

    return session;
}

void edc_session_free(struct edc_session *session)
{
    if (!session)
        return;

    /* Groups (and the tasks they own) belong to the session now — free them
     * here so every session's groups are released, not just the one the
     * scheduler happened to be bound to. */
    struct edc_task_group *group, *gtmp;
    DL_FOREACH_SAFE(session->groups, group, gtmp)
    {
        DL_DELETE(session->groups, group);
        edc_task_group_free(group);
    }
    session->groups = NULL;

    free(session->session_id);
    sdsfree(session->name);
    sdsfree(session->created_at);
    sdsfree(session->last_active);
    sdsfree(session->history_dir);
    sdsfree(session->workdir);
    free(session);
}

size_t edc_session_get_message_count(struct edc_session *session)
{
    /* P4: 消息归属 turn, 计数按所有 turn 的消息数求和 (含在途 active_turn)。 */
    if (!session)
        return 0;
    size_t n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            for (size_t i = t->turns.head; i < t->turns.tail; i++)
            {
                struct edc_turn *turn = (struct edc_turn *)t->turns.data[i];
                if (turn)
                    n += edc_queue_length(&turn->messages);
            }
            if (t->active_turn)
                n += edc_queue_length(&t->active_turn->messages);
        }
    }
    return n;
}

/* ══════════════════════════════════════════════════════════════
   Phase 4: Task/Group 管理
   ══════════════════════════════════════════════════════════════ */

void edc_session_add_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group)
        return;
    /* 属主反向指针: 任务在后台 session 里跑完时, 调度器靠它把 turn/通知/落盘
     * 写回**它自己**。P3 ③ 起 core 里没有"当前 session"这份副本可回退了 ——
     * 这个指针是唯一的归属来源。 */
    group->session = session;
    DL_APPEND(session->groups, group);
}

void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group)
        return;
    DL_DELETE(session->groups, group);
    group->session = NULL;
}

struct edc_task_group *edc_session_find_group(struct edc_session *session, const char *group_id)
{
    if (!session || !group_id)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        if (strcmp(group->group_id, group_id) == 0)
            return group;
    }
    return NULL;
}

/* 系统任务的容器, 见 edc_session.h 的声明 (kind 在核心侧唯一的读取点)。 */
struct edc_task_group *edc_session_find_system_group(struct edc_session *session)
{
    if (!session)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        if (edc_task_group_is_system(group))
            return group;
    }
    return NULL;
}

struct edc_task *edc_session_find_task(struct edc_session *session, const char *task_id)
{
    if (!session || !task_id)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        struct edc_task *t = edc_task_group_find_task(group, task_id);
        if (t)
            return t;
    }
    return NULL;
}

/* ── system task: 承载无主/会话级消息 ──────────────────────── */

void edc_session_set_active_task(struct edc_session *session, struct edc_task *task)
{
    if (!session)
        return;
    session->active_task = task;
}

struct edc_task *edc_session_get_active_task(struct edc_session *session)
{
    return session ? session->active_task : NULL;
}

sds edc_session_status_text(struct edc_session *session, bool is_current, const char *foreground_group_id)
{
    if (!session)
        return sdsnew("[session] no session");

    /* Tally groups and per-state tasks. */
    int group_count = 0, orphan_count = 0, task_count = 0;
    /* 直方图按**显示名**分桶: `/task status` 的文案是既有外部行为, 词表冻在这张表里
     * (顺序 = 旧枚举顺序, 输出逐字不变)。串是闭的 (`edc_task_state_str` 只可能返回
     * 表里的词), 找不到只可能是漏改了状态机的行 —— 那时报警, 不要静默丢。
     * `WAITING_INPUT` 已退休 (旧名 = READY), 永远是 0; 留着只为顺序稳定。 */
    static const char *const k_state_order[] = {"IDLE",        "WAITING_DEPS", "WAITING_INPUT", "LLM_PROCESSING",
                                                "TOOL_CALLING", "SUSPENDED",    "COMPLETED",     "CANCELLED",
                                                "ERROR"};
    enum
    {
        k_state_order_n = sizeof(k_state_order) / sizeof(k_state_order[0])
    };
    int state_counts[k_state_order_n] = {0};
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        group_count++;
        if (g->is_orphaned)
            orphan_count++;
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            task_count++;
            const char *state = edc_task_state_str(t);
            size_t bucket = k_state_order_n;
            for (size_t s = 0; s < k_state_order_n; s++)
            {
                if (strcmp(state, k_state_order[s]) == 0)
                {
                    bucket = s;
                    break;
                }
            }
            if (bucket == k_state_order_n)
            {
                log_warn("status: task %s reports an unknown state '%s'", t->task_id, state);
                continue;
            }
            state_counts[bucket]++;
        }
    }

    sds out = sdscatprintf(sdsempty(), "## Session Status `[%s] %s`%s\n\n", session->session_id,
                           edc_session_get_name(session), is_current ? " · current" : " · background");
    out = sdscatprintf(out, "- **State**: %s\n", is_current ? "current (live)" : "background");
    out = sdscatprintf(out, "- **Working Directory**: %s\n",
                       edc_session_get_workdir(session) ? edc_session_get_workdir(session) : "(default)");
    out = sdscatprintf(out, "- **Groups**: %d", group_count);
    if (orphan_count > 0)
        out = sdscatprintf(out, " (%d orphaned)", orphan_count);
    out = sdscat(out, "\n");
    out = sdscatprintf(out, "- **Tasks**: %d", task_count);

    /* Per-state breakdown, in the frozen word-list order, only for non-zero buckets. */
    bool first_state = true;
    for (size_t s = 0; s < k_state_order_n; s++)
    {
        if (state_counts[s] == 0)
            continue;
        out = sdscatprintf(out, "%s %s %d", first_state ? " —" : " ·", k_state_order[s], state_counts[s]);
        first_state = false;
    }
    out = sdscat(out, "\n");

    if (is_current)
    {
        out = sdscatprintf(out, "- **Foreground group**: %s\n", foreground_group_id ? foreground_group_id : "(none)");
        struct edc_task *active = session->active_task;
        if (active)
            out = sdscatprintf(out, "- **Active task**: %s%s%s\n", active->task_id,
                               (active->description && *active->description) ? " — " : "",
                               active->description ? active->description : "");
        else
            out = sdscat(out, "- **Active task**: (none)\n");
    }
    else if (group_count == 0)
    {
        out = sdscat(out, "- _live task state not loaded (switch to this session to load it)_\n");
    }

    out = sdscatprintf(out, "- **Messages**: %zu\n", edc_session_get_message_count(session));
    out = sdscatprintf(out, "- **Last Active**: %s\n", session->last_active ? session->last_active : "unknown");
    return out;
}

/* ── Phase 2: 完整状态持久化 ───────────────────────────────── */

bool edc_session_save_state(struct edc_session *session, struct edc_scheduler *sched)
{
    if (!session || !session->history_dir)
        return false;

    cJSON *root = edc_state_serialize(session, sched);
    if (!root)
    {
        log_error("save_state: edc_state_serialize returned NULL");
        return false;
    }

    /* 紧凑序列化: cJSON_Print 会生成带缩进换行的美化 JSON,体积大数倍;
       session.json 只用于程序内部加载,无需人类可读,故用 Unformatted。 */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str)
        return false;

    sds path = edc_path_join(session->history_dir, "session.json", NULL);
    sds tmp = edc_path_join(session->history_dir, "session.json.tmp", NULL);
    EdcError *err = NULL;

    /* ensure directory exists (with parents) */
    {
        EdcError *mk_err = NULL;
        edc_dir_create_with_parents(session->history_dir, 0755, &mk_err);
        if (mk_err)
        {
            log_warn("save_state: mkdir %s: %s", session->history_dir, mk_err->message);
            edc_error_free(mk_err);
        }
    }

    bool ok = edc_file_write(tmp, json_str, (ssize_t)strlen(json_str), &err);
    cJSON_free(json_str);

    if (!ok)
    {
        log_error("save_state: write to %s failed: %s", tmp, err ? err->message : "unknown");
        if (err)
            edc_error_free(err);
        sdsfree(path);
        sdsfree(tmp);
        return false;
    }

    /* atomic rename */
    if (rename(tmp, path) != 0)
    {
        log_error("save_state: rename %s -> %s failed: %s", tmp, path, strerror(errno));
        sdsfree(path);
        sdsfree(tmp);
        return false;
    }

    sdsfree(path);
    sdsfree(tmp);

    return true;
}

/* ── 加载护栏 (v1→v2 事故的教训) ─────────────────────────────
 * 无法解析/迁移的 session.json 先改名隔离, 免得进程随后用空 session 把它
 * 覆盖掉 —— 那次事故正是"加载失败后仍照常落盘"抹掉了 s7 的任务结构。 */
static void session_quarantine_state(struct edc_session *session, const char *path, const char *why)
{
    sds broken = edc_path_join(session->history_dir, "session.json.broken", NULL);
    if (rename(path, broken) == 0)
        log_warn("load_state: quarantined %s -> %s (%s)", path, broken, why);
    else
        log_error("load_state: cannot quarantine %s (%s): %s", path, why, strerror(errno));
    sdsfree(broken);
}

bool edc_session_load_state(struct edc_session *session, struct edc_scheduler *sched)
{
    if (!session || !session->history_dir)
        return false;

    sds path = edc_path_join(session->history_dir, "session.json", NULL);

    if (!edc_file_exists(path))
    {
        /* new session — no state file yet */
        sdsfree(path);
        return false;
    }

    EdcError *err = NULL;
    sds content = edc_file_read(path, &err);
    if (!content)
    {
        log_warn("load_state: read %s failed: %s", path, err ? err->message : "unknown");
        if (err)
            edc_error_free(err);
        sdsfree(path);
        return false;
    }

    cJSON *json = cJSON_Parse(content);
    sdsfree(content);

    if (!json)
    {
        log_error("load_state: failed to parse %s (corrupt JSON)", path);
        session_quarantine_state(session, path, "corrupt JSON");
        sdsfree(path);
        return false;
    }

    int src_version = edc_state_json_version(json);
    bool ok = edc_state_deserialize(json, session, sched);
    cJSON_Delete(json);

    if (!ok)
    {
        log_error("load_state: deserialize failed for %s (version %d)", path, src_version);
        session_quarantine_state(session, path, "deserialize failed");
        sdsfree(path);
        return false;
    }

    sdsfree(path);
    return true;
}

/* ── Multi-session support ─────────────────────────────────── */

void edc_session_reset(struct edc_session *session)
{
    if (!session)
        return;

    /* Clear active task */
    session->active_task = NULL;

    /* Groups (and their tasks) belong to the session — drop them for real, not
     * just unlink the head pointer. */
    struct edc_task_group *group, *gtmp;
    DL_FOREACH_SAFE(session->groups, group, gtmp)
    {
        DL_DELETE(session->groups, group);
        edc_task_group_free(group);
    }
    session->groups = NULL;

    /* 前台组随 groups 一起没了 */
    session->foreground_group = NULL;
}

const char *edc_session_get_name(struct edc_session *session)
{
    if (!session)
        return NULL;
    return session->name ? session->name : session->session_id;
}
