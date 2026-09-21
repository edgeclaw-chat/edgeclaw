#include "edc_shell.h"
#include "edc_cli_history.h"
#include "edc_session_store.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_shell_ast.h"
#include "edc_shell_token.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <errno.h>
#include <log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Append the sandbox allow/deny path lists as one markdown sub-list per section. */
static sds edc_shell_sandbox_paths_md(sds out, struct edc_config *conf)
{
    struct
    {
        const char *label;
        sds *paths;
    } sections[] = {
        {"read-write", conf->sandbox_rw_paths},
        {"read-only", conf->sandbox_ro_paths},
        {"denied (masked)", conf->sandbox_deny_paths},
    };

    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
    {
        out = sdscatprintf(out, "- **%s**\n", sections[i].label);
        bool any = false;
        for (int j = 0; sections[i].paths && sections[i].paths[j]; j++)
        {
            out = sdscatprintf(out, "  - `%s`\n", sections[i].paths[j]);
            any = true;
        }
        if (!any)
            out = sdscat(out, "  - _(none)_\n");
    }
    return out;
}

bool edc_shell_handle_builtin(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_reply_to to,
                              struct edc_channel_message *message)
{
    sds content = sdstrim(sdsnew(message->content), " \t\r\n");

    if (strcmp(content, "/whoami") == 0)
    {
        struct edc_session *sess = edc_session_manager_get_active(sm);
        struct edc_task_group *fg = edc_scheduler_get_foreground(sess);
        struct edc_task *active = sess ? sess->active_task : NULL;
        sds out = sdscatprintf(sdsempty(),
                               "## Whoami\n\n"
                               "- **session**: `%s`\n"
                               "- **model**: `%s`\n",
                               sess ? edc_session_get_name(sess) : "none",
                               cfg->llm_api_conf.model ? cfg->llm_api_conf.model : "default");
        if (fg)
        {
            struct edc_task *leader = fg->leader;
            out = sdscatprintf(out, "- **foreground group**: `%s` · %d tasks\n", fg->group_id, fg->member_count);
            out = sdscatprintf(out, "  - **leader**: `%s`%s%s\n", leader ? leader->task_id : "?",
                               (leader && *leader->description) ? " — " : "",
                               (leader && *leader->description) ? leader->description : "");
            out = sdscatprintf(out, "  - **active**: `%s`%s%s\n", active ? active->task_id : "none",
                               (active && *active->description) ? " — " : "",
                               (active && *active->description) ? active->description : "");
        }
        else
        {
            out = sdscat(out, "- **foreground group**: _(none)_\n");
        }
        edc_commands_reply(to, out);
        sdsfree(out);
        sdsfree(content);
        return true;
    }
    if (strncmp(content, "/model ", 7) == 0)
    {
        sds arg = sdsnew(content + 7);
        sdstrim(arg, " \t");
        if (sdslen(arg) > 0)
        {
            if (cfg->llm_api_conf.model)
                sdsfree(cfg->llm_api_conf.model);
            cfg->llm_api_conf.model = sdsdup(arg);
            sds reply = sdscatprintf(sdsempty(), "**Model** — set to `%s`.", cfg->llm_api_conf.model);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        sdsfree(arg);
        sdsfree(content);
        return true;
    }
    if (strcmp(content, "/model") == 0)
    {
        sds reply = sdscatprintf(sdsempty(), "**Model** — `%s`",
                                 cfg->llm_api_conf.model ? cfg->llm_api_conf.model : "default");
        edc_commands_reply(to, reply);
        sdsfree(reply);
        sdsfree(content);
        return true;
    }
    if (strcmp(content, "/sandbox") == 0 || strncmp(content, "/sandbox ", 9) == 0)
    {
        sds arg = sdstrim(sdsnew(content + 8), " \t");

        /* Split "<verb> [path]". */
        char *sp = strchr(arg, ' ');
        sds verb = sp ? sdsnewlen(arg, (size_t)(sp - arg)) : sdsdup(arg);
        sds rest = sp ? sdstrim(sdsnew(sp + 1), " \t") : sdsempty();

        struct edc_config *conf = cfg;

        if (sdslen(verb) == 0)
        {
            sds reply = sdscatprintf(sdsempty(), "## Sandbox `%s`\n\n", conf->enable_sandbox ? "on" : "off");
            reply = edc_shell_sandbox_paths_md(reply, conf);
            reply = sdscat(reply, "\nManage: `/sandbox add-rw|add-ro|add-deny <dir>` · "
                                  "`/sandbox rm-rw|rm-ro|rm-deny <dir>`\n");
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        else if (strcmp(verb, "on") == 0 || strcmp(verb, "off") == 0)
        {
            bool enable = strcmp(verb, "on") == 0;
            bool saved = edc_config_set_sandbox_enabled(cfg, enable);
            sds reply = sdscatprintf(sdsempty(), "**Sandbox** — %s%s", enable ? "on" : "off",
                                     saved ? "" : " (not persisted: config.json write failed)");
            edc_commands_reply(to, reply);
            sdsfree(reply);
            log_info("/sandbox: filesystem sandbox %s (persisted=%d)", enable ? "enabled" : "disabled", saved);
        }
        else if (strcmp(verb, "list") == 0)
        {
            sds reply = edc_shell_sandbox_paths_md(sdsempty(), conf);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
        else if (strcmp(verb, "add-rw") == 0 || strcmp(verb, "rm-rw") == 0 || strcmp(verb, "add-ro") == 0 ||
                 strcmp(verb, "rm-ro") == 0 || strcmp(verb, "add-deny") == 0 || strcmp(verb, "rm-deny") == 0)
        {
            edc_sandbox_path_kind kind = strstr(verb, "-rw")   ? EDC_SANDBOX_PATH_RW
                                         : strstr(verb, "-ro") ? EDC_SANDBOX_PATH_RO
                                                               : EDC_SANDBOX_PATH_DENY;
            const char *klabel = (kind == EDC_SANDBOX_PATH_RW) ? "rw" : (kind == EDC_SANDBOX_PATH_DENY) ? "deny" : "ro";
            bool add = (strncmp(verb, "add-", 4) == 0);
            if (sdslen(rest) == 0)
            {
                edc_commands_reply(to,
                                   "Usage: /sandbox add-rw|add-ro|add-deny <dir> | rm-rw|rm-ro|rm-deny <dir>");
            }
            else
            {
                sds msg = NULL;
                bool ok = edc_config_set_sandbox_path(cfg, rest, kind, add, &msg);
                sds reply;
                if (ok)
                    reply = sdscatprintf(sdsempty(), "**Sandbox** — %s %s path: `%s`", add ? "added" : "removed",
                                         klabel, rest);
                else
                    reply = sdscatprintf(sdsempty(), "**Sandbox** — cannot %s `%s`: %s", add ? "add" : "remove", rest,
                                         msg ? msg : "unknown error");
                edc_commands_reply(to, reply);
                sdsfree(reply);
                if (msg)
                    sdsfree(msg);
            }
        }
        else
        {
            edc_commands_reply(to,
                               "Usage: /sandbox [on|off|list] | add-rw|add-ro|add-deny|rm-rw|rm-ro|rm-deny <dir>");
        }

        sdsfree(verb);
        sdsfree(rest);
        sdsfree(arg);
        sdsfree(content);
        return true;
    }
    sdsfree(content);
    return false;
}

struct edc_task *edc_shell_spawn_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                      edc_channel_type channel_type, struct edc_session *session,
                                      struct edc_cmd_task_opts *opts,
                                      struct edc_task *prev_task, struct edc_task_group *target_group,
                                      const char *reply_prefix, struct edc_task_group **out_group,
                                      sds redirect_stdout, bool redirect_append, sds redirect_stderr,
                                      edc_task_dep_type prev_dep_type, sds *out_error)
{
    if (out_error)
        *out_error = NULL;

    /* 任务必须带着它的 input —— 与 /task new (desc 就是 input) 同一条规则。
     *
     * 一个只有 flag、没有正文的命令 (如 `-d 5`) 会走到这里而没有内容可跑。旧模型
     * 下这种任务会停在 WAITING_INPUT, 等下一条普通输入被 attach 进来; 那条路已退休,
     * 所以这里直接拒绝, 不再建一个没有 input 的空任务。
     * 见 docs/retire-waiting-input.md §4。 */
    if (!opts->input || sdslen(opts->input) == 0)
    {
        if (out_error)
            *out_error =
                sdsnew("A task needs an input — write `@<desc>` or `<desc>` (flags alone do not create a task).");
        return NULL;
    }

    /* Resolve `-d` dependencies up front. Task ids are globally unique (see
     * edc_session_manager_find_task_any), so a dependency may live in another
     * session. An id that cannot be resolved fails the whole command — it is
     * never silently dropped. */
    struct edc_task **deps = NULL;
    if (opts->dep_count > 0)
    {
        deps = calloc((size_t)opts->dep_count, sizeof(*deps));
        for (int i = 0; i < opts->dep_count; i++)
        {
            deps[i] = edc_session_store_resolve_task(sm, sched, opts->dep_ids[i], NULL);
            if (!deps[i])
            {
                if (out_error)
                    *out_error = sdscatprintf(sdsempty(), "Task not found: `%s`", opts->dep_ids[i]);
                free(deps);
                return NULL;
            }
        }
    }

    struct edc_task *task = edc_scheduler_create_task(
        sched, session, (opts->desc && sdslen(opts->desc) > 0) ? opts->desc : "New task", channel_type);
    if (!task)
        return NULL;
    if (opts->prompt_name)
        edc_task_set_prompt(task, opts->prompt_name, NULL);

    if (redirect_stdout)
    {
        task->redirect_stdout = redirect_stdout;
        task->redirect_append = redirect_append;
    }
    if (redirect_stderr)
        task->redirect_stderr = redirect_stderr;

    struct edc_task_group *group;
    if (target_group)
    {
        edc_task_group_add_task(target_group, task);
        group = target_group;
    }
    else
    {
        group = edc_scheduler_create_group(sched, session, task);
    }

    if (!opts->bg)
    {
        edc_scheduler_set_foreground(session, group);
        edc_session_set_active_task(edc_session_manager_get_active(sm), task);
    }

    for (int i = 0; i < opts->dep_count; i++)
        edc_task_add_dependency(task, deps[i], TASK_DEP_JOIN);
    free(deps);

    if (prev_task)
        edc_task_add_dependency(task, prev_task, prev_dep_type);

    /* task creation is intentionally silent: no "[task] created" echo back
     * to the channel. State transitions are still observable via
     * /task list, /task status, /whoami and the TUI task snapshot. */
    log_info("%s created task=%s group=%s%s%s%s%s", reply_prefix, task->task_id, group->group_id,
             opts->bg ? " [bg]" : "", opts->dep_count > 0 ? " [chained]" : "",
             prev_task ? (prev_dep_type == TASK_DEP_SEQUENCE ? " [seq]" : " [pipe]") : "",
             redirect_stdout ? (redirect_append ? " (>> redirect)" : " (> redirect)") : "");

    if (opts->input)
    {
        /* **装输入**到此为止: 跑不跑由消息末尾的调度按状态决定 (唯一驱动入口)。
         * 从前这里还自己 register → AWAIT_EXEC → task_moved → send_llm, 是那份被抄
         * 了 6 遍的实现之一; 排队等依赖的分支还因此与 /task new 顺序不同。 */
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(task->task_id);
        edc_turn_add_user_message(turn, opts->input);
        task->active_turn = turn;

        log_info("%s spawn: task=%s state=%s deps_total=%zu input='%s'", reply_prefix, task->task_id,
                 edc_task_state_str(task), task->deps_total, opts->input);
    }

    if (out_group)
        *out_group = group;

    return task;
}

sds edc_shell_read_file(const char *path, sds *err_out)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        if (err_out)
            *err_out = sdscatprintf(sdsempty(), "cannot read '%s': %s", path, strerror(errno));
        return NULL;
    }
    if (S_ISDIR(st.st_mode))
    {
        if (err_out)
            *err_out = sdscatprintf(sdsempty(), "cannot read '%s': Is a directory", path);
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (!f)
    {
        if (err_out)
            *err_out = sdscatprintf(sdsempty(), "cannot read '%s': %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0)
    {
        fclose(f);
        if (err_out)
            *err_out = sdscatprintf(sdsempty(), "cannot read '%s': %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    sds content = sdsnewlen(NULL, (size_t)sz);
    if (sz > 0)
        fread(content, 1, (size_t)sz, f);
    fclose(f);
    return content;
}

sds edc_shell_expand_history(const char *input)
{
    if (edc_cli_history_count() == 0)
        return sdsnew(input);

    sds result = sdsempty();
    bool in_dq = false, in_sq = false;
    const char *p = input;

    while (*p)
    {
        if (in_dq)
        {
            if (*p == '"')
                in_dq = false;
            else if (*p == '\\' && *(p + 1))
            {
                result = sdscatlen(result, p, 1);
                p++;
            }
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (in_sq)
        {
            if (*p == '\'')
                in_sq = false;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '"')
        {
            in_dq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '\'')
        {
            in_sq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '!' && *(p + 1))
        {
            p++; /* skip ! */
            if (*p == '!')
            {
                p++;
                result = sdscat(result, edc_cli_history_at(edc_cli_history_count() - 1));
            }
            else if (*p >= '0' && *p <= '9')
            {
                int idx = 0;
                while (*p >= '0' && *p <= '9')
                {
                    idx = idx * 10 + (*p - '0');
                    p++;
                }
                if (idx > 0 && idx <= edc_cli_history_count())
                    result = sdscat(result, edc_cli_history_at(idx - 1));
            }
            else
            {
                result = sdscatlen(result, "!", 1);
                result = sdscatlen(result, p, 1);
                p++;
            }
        }
        else
        {
            result = sdscatlen(result, p, 1);
            p++;
        }
    }

    return result;
}

sds edc_shell_collect_var_refs(struct edc_session_manager *sm, struct edc_scheduler *sched, const char *input,
                               sds **out_deps, int *out_dep_count,
                               sds *out_error)
{
    sds result = sdsempty();
    *out_deps = NULL;
    *out_dep_count = 0;
    *out_error = NULL;

    bool in_dq = false, in_sq = false;
    const char *p = input;

#define VAR_DELIM(c)                                                                                                   \
    ((c) == ' ' || (c) == '\t' || (c) == '\0' || (c) == '|' || (c) == ';' || (c) == '&' || (c) == '>' || (c) == '<')

    while (*p)
    {
        if (in_dq)
        {
            if (*p == '"')
            {
                in_dq = false;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (*p == '\\' && *(p + 1))
            {
                result = sdscatlen(result, p, 1);
                p++;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (*p == '$' && *(p + 1))
            {
                p++;
                goto expand_var;
            }
            else
            {
                result = sdscatlen(result, p, 1);
                p++;
            }
        }
        else if (in_sq)
        {
            if (*p == '\'')
                in_sq = false;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '"')
        {
            in_dq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '\'')
        {
            in_sq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '$' && *(p + 1))
        {
            p++; /* skip $ */
        expand_var:
            if (*p == '$')
            {
                p++;
                struct edc_task *active =
                    edc_session_get_active_task(edc_session_manager_get_active(sm));
                if (active)
                    result = sdscat(result, active->task_id);
            }
            else if (strncmp(p, "LAST", 4) == 0 && VAR_DELIM(p[4]))
            {
                result = sdscatlen(result, "$LAST", 5);
                p += 4;
            }
            else if (*p >= '0' && *p <= '9')
            {
                const char *ns = p;
                while (*p >= '0' && *p <= '9')
                    p++;
                char buf[32];
                size_t nl = (size_t)(p - ns);
                if (nl < sizeof(buf))
                {
                    memcpy(buf, ns, nl);
                    buf[nl] = '\0';
                    struct edc_task *t = edc_session_store_resolve_task(sm, sched, buf, NULL);
                    if (!t)
                    {
                        *out_error = sdscatprintf(sdsempty(), "$%s: task not found", buf);
                        sdsfree(result);
                        return NULL;
                    }
                    (*out_dep_count)++;
                    *out_deps = realloc(*out_deps, (size_t)*out_dep_count * sizeof(sds));
                    (*out_deps)[*out_dep_count - 1] = sdsnew(buf);
                    result = sdscatprintf(result, "$%s", buf);
                }
            }
            else
            {
                const char *is = p;
                while (*p && !VAR_DELIM(*p) && *p != '$')
                    p++;
                size_t il = (size_t)(p - is);
                if (il > 0)
                {
                    sds id = sdsnewlen(is, il);
                    struct edc_task *t = edc_session_store_resolve_task(sm, sched, id, NULL);
                    if (!t)
                    {
                        *out_error = sdscatprintf(sdsempty(), "$%s: task not found", id);
                        sdsfree(id);
                        sdsfree(result);
                        return NULL;
                    }
                    (*out_dep_count)++;
                    *out_deps = realloc(*out_deps, (size_t)*out_dep_count * sizeof(sds));
                    (*out_deps)[*out_dep_count - 1] = id;
                    result = sdscatprintf(result, "$%s", id);
                }
            }
        }
        else
        {
            result = sdscatlen(result, p, 1);
            p++;
        }
    }

#undef VAR_DELIM
    return result;
}

bool edc_shell_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                       struct edc_channel_message *message)
{
    if (!message || !message->content || sdslen(message->content) == 0)
        return false;

    bool is_at = (message->content[0] == '@');
    const char *body = is_at ? message->content + 1 : message->content;

    sds trimmed = sdstrim(sdsnew(body), " \t");
    if (sdslen(trimmed) == 0)
    {
        sdsfree(trimmed);
        return true;
    }
    sdsfree(trimmed);

    /* expand !n / !! history references */
    sds hist_expanded = edc_shell_expand_history(body);

    /* tokenize → parse → execute via AST */
    int tok_count = 0;
    edc_shell_token_t *tokens = edc_shell_tokenize(hist_expanded, &tok_count);
    sdsfree(hist_expanded);

    ast_node_t *root = ast_parse(tokens, tok_count, is_at);
    ast_execute(sm, sched, to, root,
                edc_session_store_resolve_session(sm, message->session_id));
    ast_free(root);

    for (int i = 0; i < tok_count; i++)
        edc_shell_token_free(&tokens[i]);
    free(tokens);
    return true;
}
