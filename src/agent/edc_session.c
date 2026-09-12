#include "edc_session.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_history.h"
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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void edc_session_trim_orphan_tool_messages(cJSON *history)
{
    while (cJSON_GetArraySize(history) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(history, 0);
        cJSON *role = cJSON_GetObjectItem(first, "role");
        if (role && cJSON_IsString(role) && strcmp(cJSON_GetStringValue(role), "tool") == 0)
        {
            cJSON_DeleteItemFromArray(history, 0);
        }
        else
        {
            break;
        }
    }
}

static void edc_session_merge_history(struct edc_session *session, cJSON *history)
{
    while (cJSON_GetArraySize(history) > 0)
    {
        cJSON *msg = cJSON_DetachItemFromArray(history, 0);
        if (cJSON_HasObjectItem(msg, "timestamp"))
        {
            cJSON_DeleteItemFromObject(msg, "timestamp");
        }
        cJSON_AddItemToArray(session->messages, msg);
    }
}

static void edc_session_build_messages(struct edc_session *session, const char *user_input, bool load_history)
{
    sds system_prompt = edc_context_build_system_prompt(session->config, session->workdir);

    cJSON *system_msg = cJSON_CreateObject();
    sds valid_utf8 = edc_str_utf8_make_valid(system_prompt, -1);
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", valid_utf8);
    sdsfree(valid_utf8);
    sdsfree(system_prompt);

    cJSON_AddItemToArray(session->messages, system_msg);

    if (load_history)
    {
        size_t compress_index = 0;
        edc_history_get_info(session->history_dir, &compress_index, NULL);
        cJSON *history = edc_history_load(session->history_dir, compress_index);
        if (history != NULL)
        {
            edc_session_trim_orphan_tool_messages(history);
            edc_session_merge_history(session, history);
            cJSON_Delete(history);
        }
    }

    if (user_input)
    {
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_input);
        cJSON_AddItemToArray(session->messages, user_msg);
    }
}

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
    session->messages = cJSON_CreateArray();
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

    edc_session_build_messages(session, NULL, true);
    session->save_skip_count = edc_session_get_message_count(session);
    return session;
}

void edc_session_reload(struct edc_session *session, bool load_history)
{
    edc_session_save_history(session);

    while (cJSON_GetArraySize(session->messages) > 0)
    {
        cJSON_DeleteItemFromArray(session->messages, 0);
    }

    edc_session_build_messages(session, NULL, load_history);

    session->save_skip_count = edc_session_get_message_count(session);
}

void edc_session_free(struct edc_session *session)
{
    if (!session)
        return;

    if (session->messages)
    {
        cJSON_Delete(session->messages);
    }

    struct edc_timestamp_node *n, *tmp;
    DL_FOREACH_SAFE(session->timestamps, n, tmp)
    {
        DL_DELETE(session->timestamps, n);
        sdsfree(n->ts);
        free(n);
    }

    /* groups freed by edc_scheduler_free (shared list) */

    free(session->session_id);
    sdsfree(session->name);
    sdsfree(session->created_at);
    sdsfree(session->last_active);
    sdsfree(session->history_dir);
    sdsfree(session->workdir);
    free(session);
}

void edc_session_add_turn(struct edc_session *sess, struct edc_turn *turn)
{
    /* Phase 3: record message range before moving messages out */
    size_t msg_count = edc_queue_length(&turn->messages);
    if (msg_count > 0)
    {
        turn->msg_range_start = cJSON_GetArraySize(sess->messages);
        turn->msg_range_count = msg_count;
        turn->range_valid = true;
    }

    EdcQueue *m = &turn->messages;
    EdcQueue *t = &turn->timestamps;
    while (edc_queue_length(m) > 0 && edc_queue_length(t) > 0)
    {
        cJSON *message = edc_queue_pop(m);
        sds ts = edc_queue_pop(t);
        cJSON_AddItemToArray(sess->messages, message);
        struct edc_timestamp_node *n = g_new0(struct edc_timestamp_node, 1);
        n->ts = ts;
        DL_APPEND(sess->timestamps, n);
    }
}

cJSON *edc_session_get_messages(struct edc_session *session)
{
    return session->messages;
}

size_t edc_session_get_message_count(struct edc_session *session)
{
    return cJSON_GetArraySize(session->messages);
}

bool edc_session_save_history(struct edc_session *session)
{
    bool ok = edc_history_save(session->history_dir, session->messages, session->timestamps, session->save_skip_count);
    if (ok)
    {
        session->save_skip_count = edc_session_get_message_count(session);
        struct edc_timestamp_node *n, *tmp;
        DL_FOREACH_SAFE(session->timestamps, n, tmp)
        {
            DL_DELETE(session->timestamps, n);
            sdsfree(n->ts);
            free(n);
        }
        session->timestamps = NULL;
    }

    return ok;
}

/* ══════════════════════════════════════════════════════════════
   Phase 4: Task/Group 管理
   ══════════════════════════════════════════════════════════════ */

void edc_session_add_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group)
        return;
    /* 属主反向指针: 任务在后台 session 里跑完时, 调度器靠它把 turn/快照/落盘
     * 写回正确的 session, 而不是当前绑定的 sched->session。 */
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
    int state_counts[TASK_ERROR + 1] = {0};
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
            if (t->state >= TASK_IDLE && t->state <= TASK_ERROR)
                state_counts[t->state]++;
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

    /* Per-state breakdown, in enum order, only for non-zero buckets. */
    bool first_state = true;
    for (int s = TASK_IDLE; s <= TASK_ERROR; s++)
    {
        if (state_counts[s] == 0)
            continue;
        out = sdscatprintf(out, "%s %s %d", first_state ? " —" : " ·", edc_task_state_name((edc_task_state)s),
                           state_counts[s]);
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

/* ── 消息切片 ──────────────────────────────────────────────── */

cJSON *edc_session_get_messages_slice(struct edc_session *session, size_t from, size_t count)
{
    cJSON *slice = cJSON_CreateArray();
    if (!session || !session->messages)
        return slice;

    size_t total = cJSON_GetArraySize(session->messages);
    if (from >= total)
        return slice;

    size_t end = from + count;
    if (count == 0 || end > total)
        end = total;

    for (size_t i = from; i < end; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(session->messages, i);
        cJSON_AddItemReferenceToArray(slice, msg);
    }
    return slice;
}

/* ── Task 结构持久化 ──────────────────────────────────────── */

bool edc_session_save_tasks(struct edc_session *session)
{
    if (!session)
        return false;

    /* 给 messages 中每条 non-system non-tool 消息追加 task_id */
    /* system 消息和 tool 消息不标记 (属于所有 task 共享) */

    cJSON *messages = session->messages;
    size_t count = cJSON_GetArraySize(messages);

    for (size_t i = 0; i < count; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        if (!role || !cJSON_IsString(role))
            continue;

        const char *role_s = cJSON_GetStringValue(role);
        if (strcmp(role_s, "system") == 0 || strcmp(role_s, "tool") == 0)
            continue;

        if (!cJSON_HasObjectItem(msg, "task_id"))
        {
            /* 使用默认 "legacy" task_id */
            cJSON_AddStringToObject(msg, "task_id", "legacy");
            cJSON_AddStringToObject(msg, "group_id", "legacy");
        }
    }

    return edc_session_save_history(session);
}

void edc_session_reload_tasks(struct edc_session *session)
{
    if (!session || !session->messages)
        return;

    /* 从 messages 中提取所有 unique task_id，为每个创建一个 task */
    /* 简化为: 扫描 messages, 收集唯一 task_id */
    /* 完整实现留 Phase 5 在 edc_agent 中处理 */
    (void)session;
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
        sdsfree(path);
        return false;
    }

    bool ok = edc_state_deserialize(json, session, sched);
    cJSON_Delete(json);

    if (!ok)
    {
        log_error("load_state: deserialize failed for %s", path);
    }

    sdsfree(path);
    return ok;
}

/* ── Multi-session support ─────────────────────────────────── */

void edc_session_reset(struct edc_session *session)
{
    if (!session)
        return;

    /* Clear messages */
    while (cJSON_GetArraySize(session->messages) > 0)
        cJSON_DeleteItemFromArray(session->messages, 0);

    /* Clear timestamps */
    struct edc_timestamp_node *n, *tmp;
    DL_FOREACH_SAFE(session->timestamps, n, tmp)
    {
        DL_DELETE(session->timestamps, n);
        sdsfree(n->ts);
        free(n);
    }
    session->timestamps = NULL;

    /* Reset save skip count */
    session->save_skip_count = 0;

    /* Clear active task */
    session->active_task = NULL;

    /* groups are owned by scheduler — just clear the list */
    session->groups = NULL;
}

const char *edc_session_get_name(struct edc_session *session)
{
    if (!session)
        return NULL;
    return session->name ? session->name : session->session_id;
}
