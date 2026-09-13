#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "log.h"
#include "sds.h"
#include "uthash.h"
#include <errno.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────── */

static sds edc_sm_gen_session_id(struct edc_session_manager *sm)
{
    int max_id = 0;
    for (int i = 0; i < sm->session_count; i++)
    {
        const char *sid = sm->sessions[i]->session_id;
        const char *num = NULL;
        if (strncmp(sid, "sess-", 5) == 0)
            num = sid + 5;
        else if (sid[0] == 's')
            num = sid + 1;
        if (num && num[0] >= '0' && num[0] <= '9')
        {
            int n = atoi(num);
            if (n > max_id)
                max_id = n;
        }
    }
    return sdscatprintf(sdsempty(), "s%d", max_id + 1);
}

static sds edc_sm_gen_session_name(struct edc_session_manager *sm)
{
    /* Generate a default name like "Session 2" */
    int max_n = 0;
    for (int i = 0; i < sm->session_count; i++)
    {
        const char *name = edc_session_get_name(sm->sessions[i]);
        if (name && strncmp(name, "Session ", 8) == 0)
        {
            int n = atoi(name + 8);
            if (n > max_n)
                max_n = n;
        }
    }
    return sdscatprintf(sdsempty(), "Session %d", max_n + 1);
}

/* ── Index persistence ─────────────────────────────────────── */

static cJSON *edc_sm_build_index(struct edc_session_manager *sm)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", s->session_id);
        cJSON_AddStringToObject(obj, "name", s->name ? s->name : s->session_id);
        cJSON_AddStringToObject(obj, "created_at", s->created_at ? s->created_at : "");
        cJSON_AddItemToArray(arr, obj);
    }

    cJSON_AddItemToObject(root, "sessions", arr);
    if (sm->active_index >= 0 && sm->active_index < sm->session_count)
        cJSON_AddStringToObject(root, "active_session_id", sm->sessions[sm->active_index]->session_id);

    return root;
}

static void edc_sm_save_index(struct edc_session_manager *sm)
{
    cJSON *root = edc_sm_build_index(sm);
    char *json_str = cJSON_Print(root);
    EdcError *err = NULL;

    /* ensure directory exists */
    sds dir = edc_path_dirname(sm->index_path);
    mkdir(dir, 0755);
    sdsfree(dir);

    if (!edc_file_write(sm->index_path, json_str, (ssize_t)strlen(json_str), &err))
    {
        log_error("failed to save sessions.json: %s", err ? err->message : "unknown");
        if (err)
            edc_error_free(err);
    }

    cJSON_free(json_str);
    cJSON_Delete(root);
}

/* ── Load ──────────────────────────────────────────────────── */

bool edc_session_manager_load(struct edc_session_manager *sm)
{
    /* 1. Try to load index */
    EdcError *err = NULL;
    sds content = edc_file_read(sm->index_path, &err);

    if (!content)
    {
        edc_error_free(err);
        /* No existing sessions — create default */
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
            return false;
        edc_sm_save_index(sm);
        return true;
    }

    cJSON *root = cJSON_Parse(content);
    if (!root)
    {
        sdsfree(content);
        /* fallback: create default */
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
            return false;
        return true;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "sessions");
    if (!arr || !cJSON_IsArray(arr))
    {
        cJSON_Delete(root);
        sdsfree(content);
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
            return false;
        return true;
    }

    int count = cJSON_GetArraySize(arr);
    if (count == 0)
    {
        cJSON_Delete(root);
        sdsfree(content);
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
            return false;
        return true;
    }

    sm->sessions = g_new0(struct edc_session *, count);
    sm->session_count = count;

    cJSON *active_id_json = cJSON_GetObjectItem(root, "active_session_id");
    const char *active_id =
        active_id_json && cJSON_IsString(active_id_json) ? cJSON_GetStringValue(active_id_json) : NULL;

    for (int i = 0; i < count; i++)
    {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        if (!obj)
            continue;

        cJSON *id_j = cJSON_GetObjectItem(obj, "id");
        cJSON *name_j = cJSON_GetObjectItem(obj, "name");
        cJSON *created_j = cJSON_GetObjectItem(obj, "created_at");

        const char *id = id_j && cJSON_IsString(id_j) ? cJSON_GetStringValue(id_j) : NULL;
        const char *name = name_j && cJSON_IsString(name_j) ? cJSON_GetStringValue(name_j) : NULL;

        if (!id)
            continue;

        struct edc_session *s = edc_session_new(sm->config, id, name ? name : id);
        if (!s)
            continue;

        /* restore timestamps */
        if (created_j && cJSON_IsString(created_j) && strlen(cJSON_GetStringValue(created_j)) > 0)
        {
            sdsfree(s->created_at);
            s->created_at = sdsnew(cJSON_GetStringValue(created_j));
        }

        sm->sessions[i] = s;

        if (active_id && strcmp(id, active_id) == 0)
            sm->active_index = i;
    }

    /* Compact out NULL entries */
    int write = 0;
    for (int i = 0; i < count; i++)
    {
        if (sm->sessions[i])
        {
            if (write != i)
                sm->sessions[write] = sm->sessions[i];
            write++;
        }
    }
    sm->session_count = write;

    /* Clamp active_index */
    if (sm->active_index < 0 || sm->active_index >= sm->session_count)
        sm->active_index = 0;

    /* If nothing loaded, create default */
    if (sm->session_count == 0)
    {
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
        {
            cJSON_Delete(root);
            sdsfree(content);
            return false;
        }
    }

    cJSON_Delete(root);
    sdsfree(content);
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────── */

struct edc_session_manager *edc_session_manager_new(struct edc_config *config)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->config = config;
    sm->active_index = -1;
    sm->sessions_dir = sdsnew(config->workspace);
    sm->index_path = edc_path_join(config->workspace, "sessions.json", NULL);

    if (!edc_session_manager_load(sm))
    {
        edc_session_manager_free(sm);
        return NULL;
    }

    /* Update last_active for the active session */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
    {
        sdsfree(active->last_active);
        active->last_active = edc_datetime_timestamp();
    }

    return sm;
}

static void sm_task_index_free(struct edc_session_manager *sm);

void edc_session_manager_free(struct edc_session_manager *sm)
{
    if (!sm)
        return;

    /* Save before freeing */
    edc_session_manager_save_all(sm);

    for (int i = 0; i < sm->session_count; i++)
        edc_session_free(sm->sessions[i]);

    sm_task_index_free(sm);
    free(sm->sessions);
    sdsfree(sm->sessions_dir);
    sdsfree(sm->index_path);
    free(sm);
}

/* ── Access ────────────────────────────────────────────────── */

struct edc_session *edc_session_manager_get_active(struct edc_session_manager *sm)
{
    if (!sm || sm->active_index < 0 || sm->active_index >= sm->session_count)
        return NULL;
    return sm->sessions[sm->active_index];
}

struct edc_session *edc_session_manager_find(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return NULL;
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (strcmp(s->session_id, name_or_id) == 0)
            return s;
        if (s->name && strcmp(s->name, name_or_id) == 0)
            return s;
    }
    return NULL;
}

int edc_session_manager_index_of(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return -1;
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (strcmp(s->session_id, name_or_id) == 0)
            return i;
        if (s->name && strcmp(s->name, name_or_id) == 0)
            return i;
    }
    return -1;
}

struct edc_task *edc_session_manager_find_task_any(struct edc_session_manager *sm, const char *task_id,
                                                   struct edc_session **owner_out)
{
    if (owner_out)
        *owner_out = NULL;
    if (!sm || !task_id)
        return NULL;

    /* Task ids are globally unique, so the first hit is the only hit. Search
     * every in-memory session; a session that has not been activated yet has no
     * groups loaded and therefore nothing to match. */
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (!s)
            continue;
        struct edc_task *t = edc_session_find_task(s, task_id);
        if (t)
        {
            if (owner_out)
                *owner_out = s;
            return t;
        }
    }
    return NULL;
}

/* ── task id 索引 (未加载 session) ───────────────────────────
 * 跨 session 查找 task id 时, 若 owner 未加载, 我们只需要"它在哪个 session"。
 * v3 起不再有 tasks.idx 侧车 (单一真相源 = session.json): 首次查找时解析一次
 * 每个未加载 session 的 session.json, 收进一个 uthash, 之后 O(1) 定位。
 *
 * 不变式: 一个未加载的 session 整个 run 都不会被修改 (要改它就得先 load), 所以
 * 这张表恒等于"未加载 session 的磁盘冻结态"; 已加载 session 一律由内存查找
 * 覆盖, 新建 task 也必然在已加载 session → 本表无需任何写侧同步。 */

struct edc_task_index_entry
{
    sds task_id;
    sds session_id;
    UT_hash_handle hh;
};

static void sm_task_index_add(struct edc_session_manager *sm, const char *task_id, const char *session_id)
{
    if (!sm || !task_id || !task_id[0] || !session_id)
        return;
    struct edc_task_index_entry *e = NULL;
    HASH_FIND(hh, sm->task_index, task_id, strlen(task_id), e);
    if (e)
        return; /* task ids are globally unique → first wins */
    e = g_new0(struct edc_task_index_entry, 1);
    e->task_id = sdsnew(task_id);
    e->session_id = sdsnew(session_id);
    HASH_ADD_KEYPTR(hh, sm->task_index, e->task_id, sdslen(e->task_id), e);
}

/* 解析一次 session.json 收集 task id, 供未加载 session 的 owner 定位。 */
static void sm_task_index_ingest_session_json(struct edc_session_manager *sm, struct edc_session *s)
{
    sds path = edc_path_join(s->history_dir, "session.json", NULL);
    if (!edc_file_exists(path))
    {
        sdsfree(path);
        return;
    }
    EdcError *err = NULL;
    sds content = edc_file_read(path, &err);
    sdsfree(path);
    if (!content)
    {
        if (err)
            edc_error_free(err);
        return;
    }
    cJSON *json = cJSON_Parse(content);
    sdsfree(content);
    if (!json)
        return;

    cJSON *tasks = cJSON_GetObjectItem(json, "tasks");
    if (cJSON_IsArray(tasks))
    {
        int n = cJSON_GetArraySize(tasks);
        for (int i = 0; i < n; i++)
        {
            const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(tasks, i), "task_id"));
            if (tid && tid[0])
                sm_task_index_add(sm, tid, s->session_id);
        }
    }
    cJSON_Delete(json);
}

static void sm_task_index_build(struct edc_session_manager *sm)
{
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (!s || s->state_loaded)
            continue; /* 已加载 → 内存优先, 不入表 */
        sm_task_index_ingest_session_json(sm, s);
    }
    sm->task_index_built = true;
}

static void sm_task_index_free(struct edc_session_manager *sm)
{
    struct edc_task_index_entry *e, *tmp;
    HASH_ITER(hh, sm->task_index, e, tmp)
    {
        HASH_DEL(sm->task_index, e);
        sdsfree(e->task_id);
        sdsfree(e->session_id);
        free(e);
    }
    sm->task_index = NULL;
    sm->task_index_built = false;
}

void edc_session_manager_reset_task_index(struct edc_session_manager *sm)
{
    if (sm)
        sm_task_index_free(sm);
}

struct edc_session *edc_session_manager_session_for_task(struct edc_session_manager *sm, const char *task_id)
{
    if (!sm || !task_id)
        return NULL;
    if (!sm->task_index_built)
        sm_task_index_build(sm);

    struct edc_task_index_entry *e = NULL;
    HASH_FIND(hh, sm->task_index, task_id, strlen(task_id), e);
    if (!e)
        return NULL;
    return edc_session_manager_find(sm, e->session_id);
}

/* ── Operations ────────────────────────────────────────────── */

struct edc_session *edc_session_manager_create(struct edc_session_manager *sm, const char *name)
{
    return edc_session_manager_create_ex(sm, name, NULL);
}

struct edc_session *edc_session_manager_create_ex(struct edc_session_manager *sm, const char *name, const char *workdir)
{
    if (!sm)
        return NULL;

    sds id = edc_sm_gen_session_id(sm);
    sds sname = (name && strlen(name) > 0) ? sdsnew(name) : edc_sm_gen_session_name(sm);

    struct edc_session *s = edc_session_new_with_workdir(sm->config, id, sname, workdir);
    sdsfree(id);
    sdsfree(sname);

    if (!s)
        return NULL;

    /* Add to array */
    int new_count = sm->session_count + 1;
    struct edc_session **new_arr = g_new0(struct edc_session *, new_count);
    if (sm->sessions && sm->session_count > 0)
        memcpy(new_arr, sm->sessions, (size_t)sm->session_count * sizeof(struct edc_session *));
    new_arr[sm->session_count] = s;
    free(sm->sessions);
    sm->sessions = new_arr;
    sm->session_count = new_count;
    sm->active_index = sm->session_count - 1;

    sdsfree(s->last_active);
    s->last_active = edc_datetime_timestamp();

    edc_sm_save_index(sm);
    log_info("created session: id=%s name=%s", s->session_id, s->name);
    return s;
}

bool edc_session_manager_switch(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return false;

    int idx = edc_session_manager_index_of(sm, name_or_id);
    if (idx < 0)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    if (idx == sm->active_index)
    {
        log_info("already on session: %s", name_or_id);
        return true;
    }

    /* Save the outgoing session's history. Its *state* is persisted by the
     * caller before switching (see edc_commands_handle_session_switch) — the
     * manager has no scheduler handle, so saving state here would drop the
     * foreground group id and a new task would later spawn its own group. */
    struct edc_session *old = edc_session_manager_get_active(sm);
    if (old)
    {
        sdsfree(old->last_active);
        old->last_active = edc_datetime_timestamp();
    }

    /* Switch */
    sm->active_index = idx;
    struct edc_session *new_sess = sm->sessions[idx];

    sdsfree(new_sess->last_active);
    new_sess->last_active = edc_datetime_timestamp();

    edc_sm_save_index(sm);
    log_info("switched to session: id=%s name=%s", new_sess->session_id, new_sess->name);
    return true;
}

bool edc_session_manager_stop(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return false;
    if (sm->session_count <= 1)
    {
        log_error("cannot stop the last session");
        return false;
    }

    int idx = edc_session_manager_index_of(sm, name_or_id);
    if (idx < 0)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    struct edc_session *to_stop = sm->sessions[idx];

    /* Save history one final time */
    edc_session_save_state(to_stop, NULL);

    /* Remove from array */
    int new_count = sm->session_count - 1;
    struct edc_session **new_arr = g_new0(struct edc_session *, new_count);
    int write = 0;
    for (int i = 0; i < sm->session_count; i++)
    {
        if (i != idx)
            new_arr[write++] = sm->sessions[i];
    }
    free(sm->sessions);
    sm->sessions = new_arr;
    sm->session_count = new_count;

    /* Adjust active_index */
    if (sm->active_index >= idx)
        sm->active_index--;
    if (sm->active_index < 0)
        sm->active_index = 0;
    if (sm->active_index >= sm->session_count)
        sm->active_index = sm->session_count - 1;

    log_info("stopped session: id=%s name=%s", to_stop->session_id, to_stop->name);
    edc_session_free(to_stop);

    /* Update active session's last_active */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
    {
        sdsfree(active->last_active);
        active->last_active = edc_datetime_timestamp();
    }

    edc_sm_save_index(sm);
    return true;
}

bool edc_session_manager_rename(struct edc_session_manager *sm, const char *name_or_id, const char *new_name)
{
    if (!sm || !name_or_id || !new_name || strlen(new_name) == 0)
        return false;

    struct edc_session *s = edc_session_manager_find(sm, name_or_id);
    if (!s)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    sdsfree(s->name);
    s->name = sdsnew(new_name);

    edc_sm_save_index(sm);
    log_info("renamed session %s to %s", s->session_id, new_name);
    return true;
}

sds edc_session_manager_list(struct edc_session_manager *sm)
{
    if (!sm)
        return sdsnew("**Sessions** — none.");

    sds result = sdscatprintf(sdsempty(), "## Sessions (%d)\n\n", sm->session_count);

    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        size_t msg_count = edc_session_get_message_count(s);
        bool active = (i == sm->active_index);

        result = sdscatprintf(result, "- `[%s]` **%s**%s\n", s->session_id, s->name ? s->name : "(unnamed)",
                              active ? " · **current**" : "");
        result = sdscatprintf(result, "  - messages: %zu · created: %s\n", msg_count,
                              s->created_at ? s->created_at : "unknown");
    }
    if (sm->session_count == 0)
        result = sdscat(result, "_No sessions._\n");

    return result;
}

/* ── Persistence ───────────────────────────────────────────── */

void edc_session_manager_save_all(struct edc_session_manager *sm)
{
    if (!sm)
        return;

    /* Save active session's history */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)

    /* Save index */
    edc_sm_save_index(sm);
}
