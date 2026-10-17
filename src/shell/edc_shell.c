#include "edc_shell.h"
#include "edc_session_store.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_models.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_interp.h"
#include "edc_lex.h"
#include "edc_parse.h"
#include "edc_string_utils.h"
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

/* The provider whose model list contains |model_id|, or NULL (the catalog may not know
 * the configured model). */
static const char *edc_shell_provider_of_model(const char *model_id)
{
    if (!model_id)
        return NULL;
    size_t np = edc_models_provider_count();
    for (size_t i = 0; i < np; i++)
    {
        const char *pid = edc_models_provider_id(i);
        size_t nm = edc_models_model_count(pid);
        for (size_t j = 0; j < nm; j++)
        {
            const char *mid = edc_models_model_id_at(pid, j);
            if (mid && strcmp(mid, model_id) == 0)
                return pid;
        }
    }
    return NULL;
}

/* Append one provider's chat models as list rows, marking |current|. */
static sds edc_shell_append_provider_models(sds out, const char *pid, const char *current)
{
    size_t nm = edc_models_model_count(pid);
    for (size_t j = 0; j < nm; j++)
    {
        const char *mid = edc_models_model_id_at(pid, j);
        if (!mid || !edc_models_is_chat_model(mid))
            continue;
        struct edc_model_info f;
        edc_models_model_info(mid, &f);
        out = sdscatprintf(out, "- `%s`%s — %lld ctx\n", mid, (current && strcmp(mid, current) == 0) ? " **(current)**" : "",
                           f.context);
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
    if (strcmp(content, "/model") == 0 || strncmp(content, "/model ", 7) == 0)
    {
        const char *arg = strncmp(content, "/model ", 7) == 0 ? content + 7 : "";
        while (*arg == ' ' || *arg == '\t')
            arg++;
        const char *cur = cfg->llm_api_conf.model;

        if (*arg == '\0')
        {
            /* List the current provider's chat models (switch with `/model <id>`). */
            const char *pid = edc_shell_provider_of_model(cur);
            sds out = sdscatprintf(sdsempty(), "**Model** — `%s`\n", cur ? cur : "default");
            if (pid)
            {
                out = sdscatprintf(out, "\n`%s` models:\n", pid);
                out = edc_shell_append_provider_models(out, pid, cur);
            }
            else
            {
                out = sdscat(out, "\n_(the current model is not in the catalog; `/model <id>` sets one)_\n");
            }
            out = sdscat(out, "\n`/model <id>` switches · `/model search <q>` searches all providers.");
            edc_commands_reply(to, out);
            sdsfree(out);
        }
        else if (strncmp(arg, "search ", 7) == 0)
        {
            const char *q = arg + 7;
            while (*q == ' ' || *q == '\t')
                q++;
            sds out = sdscatprintf(sdsempty(), "**Model search** — `%s`\n", q);
            size_t np = edc_models_provider_count();
            size_t hits = 0;
            for (size_t i = 0; i < np; i++)
            {
                const char *pid = edc_models_provider_id(i);
                size_t nm = edc_models_model_count(pid);
                for (size_t j = 0; j < nm && hits < 40; j++)
                {
                    const char *mid = edc_models_model_id_at(pid, j);
                    if (!mid || !edc_models_is_chat_model(mid) || !edc_models_model_matches(mid, q))
                        continue;
                    struct edc_model_info f;
                    edc_models_model_info(mid, &f);
                    out = sdscatprintf(out, "- `%s` (%s) — %lld ctx\n", mid, pid, f.context);
                    hits++;
                }
            }
            if (hits == 0)
                out = sdscat(out, "_(no chat model matches)_");
            else if (hits >= 40)
                out = sdscat(out, "… (more; narrow the query)");
            edc_commands_reply(to, out);
            sdsfree(out);
        }
        else
        {
            if (cfg->llm_api_conf.model)
                sdsfree(cfg->llm_api_conf.model);
            cfg->llm_api_conf.model = sdsdup(arg);
            /* If the catalog knows it, carry its context window too (as the wizard does),
             * and persist both so the switch survives a restart (best-effort writes). */
            struct edc_model_info f;
            if (edc_models_model_info(arg, &f) && f.context > 0)
            {
                cfg->context_window = (int)f.context;
                edc_config_update_context_window(cfg->edc_home, (int)f.context);
            }
            edc_config_update_llm(cfg->edc_home, NULL, arg, NULL);
            sds reply = sdscatprintf(sdsempty(), "**Model** — set to `%s`.", cfg->llm_api_conf.model);
            edc_commands_reply(to, reply);
            sdsfree(reply);
        }
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
    if (strcmp(content, "/shell") == 0 || strncmp(content, "/shell ", 7) == 0)
    {
        /* The ONLY way to flip the shell-like syntax switch (there is deliberately no CLI
         * flag): one statement, and it persists. Off by default -- the operators below
         * spell out a Unix shell vocabulary, so a user who does not expect it should not
         * get a half-parsed task out of it. */
        sds arg = sdstrim(sdsnew(content + 6), " \t");
        sds reply;
        if (sdslen(arg) == 0)
        {
            reply = sdscatprintf(sdsempty(),
                                 "## Shell-like syntax `%s`\n\n"
                                 "The sigils `+<desc>` (open a new group) and `!<cmd>` (run a local shell "
                                 "command), and the operators `|` `;` `&` `>` `>>` `2>` `<`.\n\n"
                                 "Off, every line that is not a `/` command is one task description, "
                                 "verbatim.\n\n"
                                 "Toggle: `/shell on|off` (persisted to config.json).\n",
                                 cfg->shell_syntax ? "on" : "off");
        }
        else if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0)
        {
            bool enable = strcmp(arg, "on") == 0;
            bool saved = edc_config_set_shell_syntax(cfg, enable);
            reply = sdscatprintf(sdsempty(), "**Shell-like syntax** — `%s`%s", enable ? "on" : "off",
                                 saved ? "" : " (not persisted: config.json write failed)");
            log_info("/shell: shell-like syntax %s (persisted=%d)", enable ? "enabled" : "disabled", saved);
        }
        else
        {
            reply = sdsnew("Usage: `/shell [on|off]`");
        }
        edc_commands_reply(to, reply);
        sdsfree(reply);
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

    /* A task must carry its input (same rule as /task new: desc is the input).
     * A flags-only command has nothing to run, so it is rejected rather than
     * creating an empty task. */
    if (!opts->input || sdslen(opts->input) == 0)
    {
        if (out_error)
            *out_error =
                sdsnew("A task needs an input — write `@<desc>` or `<desc>` (flags alone do not create a task).");
        return NULL;
    }

    /* Resolve -d dependencies up front (ids are globally unique, so a dep may live
     * in another session); an unresolvable id fails the whole command. */
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

    /* The shell's syntax decides the placement: `&` is the only thing that does not
     * take the foreground, and the AST's walk-local target group (a pipeline shares
     * one) names the group. Everything else -- object, edges, group, turn -- is the
     * scheduler's edc_scheduler_task_create. */
    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = (opts->desc && sdslen(opts->desc) > 0) ? opts->desc : "New task";
    spec.channel_type = channel_type;
    spec.input = opts->input;
    spec.deps = deps;
    spec.dep_count = (size_t)opts->dep_count;
    spec.prev = prev_task;
    spec.prev_type = prev_dep_type;
    spec.take_foreground = !opts->bg;
    spec.background = opts->bg;
    if (target_group)
    {
        spec.place = EDC_TASK_PLACE_GROUP;
        spec.group_id = target_group->group_id;
    }
    else
    {
        spec.place = EDC_TASK_PLACE_NEW_GROUP;
    }

    struct edc_task *task = edc_scheduler_task_create(sched, session, &spec);
    free(deps);
    deps = NULL;
    if (!task)
        return NULL;

    if (redirect_stdout)
    {
        task->redirect_stdout = redirect_stdout;
        task->redirect_append = redirect_append;
    }
    if (redirect_stderr)
        task->redirect_stderr = redirect_stderr;

    struct edc_task_group *group = task->group;

    /* The receipt is emitted by the caller (edc_shell_ast.c announces the task it just
     * spawned). It cannot live here: `!cmd` and `/task new` create tasks too and must say
     * the same line. What is logged here stays for the trace. */
    log_info("%s created task=%s group=%s%s%s%s%s", reply_prefix, task->task_id,
             group ? group->group_id : "-", opts->bg ? " [bg]" : "", opts->dep_count > 0 ? " [chained]" : "",
             prev_task ? (prev_dep_type == TASK_DEP_SEQUENCE ? " [seq]" : " [pipe]") : "",
             redirect_stdout ? (redirect_append ? " (>> redirect)" : " (> redirect)") : "");

    /* The turn is already attached (input is mandatory in this path); running it is
     * the message-end scan's job. */
    log_info("%s spawn: task=%s state=%s deps_total=%zu input='%s'", reply_prefix, task->task_id,
             edc_task_state_str(task), task->deps_total, opts->input);

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

/* Same recognition as the expand phase (edc_shell_vars.c): only our exact forms count. */
static bool edc_shell_all_digits(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
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
            else if (p[0] == '{' && p[1] == '{')
            {
                p += 2;
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
        else if (p[0] == '{' && p[1] == '{')
        {
            p += 2;
        expand_var:
            {
                const char *close = strstr(p, "}}");
                if (!close)
                {
                    result = sdscatlen(result, "{{", 2); /* unterminated: literal bytes */
                    continue;
                }
                const char *after = close + 2;
                sds body = sdsnewlen(p, (size_t)(close - p));

                if (strcmp(body, "SELF") == 0)
                {
                    /* `{{SELF}}` is the task in front (not the screen focus: that one may
                     * sit on a task that already finished). Resolved NOW, as `$$` was. */
                    struct edc_task *front = edc_scheduler_current_task(edc_session_manager_get_active(sm));
                    if (front)
                        result = sdscat(result, front->task_id);
                }
                else if (strcmp(body, "LAST") == 0)
                {
                    /* No dependency, and no value yet: the marker stays for the expand
                     * phase (edc_shell_vars.c), which runs once this task's deps are done. */
                    result = sdscat(result, "{{LAST}}");
                }
                else if (edc_shell_all_digits(body, sdslen(body)))
                {
                    struct edc_task *t = edc_session_store_resolve_task(sm, sched, body, NULL);
                    if (!t)
                    {
                        *out_error = sdscatprintf(sdsempty(), "{{%s}}: task not found", body);
                        sdsfree(body);
                        sdsfree(result);
                        return NULL;
                    }
                    (*out_dep_count)++;
                    *out_deps = realloc(*out_deps, (size_t)*out_dep_count * sizeof(sds));
                    (*out_deps)[*out_dep_count - 1] = sdsdup(body);
                    result = sdscatprintf(result, "{{%s}}", body);
                }
                else
                {
                    /* Not one of our forms: verbatim, no dep, no error (see the header of
                     * edc_shell_vars.c -- nothing is a reference unless spelled exactly). */
                    result = sdscatprintf(result, "{{%s}}", body);
                }
                sdsfree(body);
                p = after;
                continue;
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

/* Shell-like syntax OFF: the whole line becomes ONE task description, verbatim -- no
 * tokenizing, so no `| ; & > >> 2> <` operators and no `+ ! @` sigil. This is the
 * DEFAULT path, and it is why the switch exists: those operators spell out a Unix shell
 * vocabulary, and a user who does not expect them would otherwise get a silently
 * half-parsed task instead of their text.
 *
 * It goes through edc_shell_spawn_task (the single creation entry point) rather than
 * building a spec here, so "how an input line becomes a task" keeps one owner. The
 * placement is the scheduler's plain-input one: the group in front, when it is still a
 * place to put new work. */
/**
 * Shell-like syntax OFF: the whole line becomes ONE task description, verbatim -- no
 * tokenizing, so no `| ; & > >> 2> <` operators and no `+ !` sigil. This is the DEFAULT
 * path, and it is why the switch exists: those operators spell out a Unix shell
 * vocabulary, and a user who does not expect them would otherwise get a silently
 * half-parsed task instead of their text.
 *
 * It goes through edc_shell_spawn_task (the single creation entry point) rather than
 * building a spec here, so "how an input line becomes a task" keeps one owner. The
 * placement is the scheduler's plain-input one: the group in front, when it is still a
 * place to put new work.
 *
 * |text| is the line as it should become the task's input -- the caller passes the
 * de-escaped form (see edc_shell_execute), so `\@`/`\+` mean a literal character here too.
 */
bool edc_shell_spawn_plain_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                struct edc_reply_to to, struct edc_channel_message *message, const char *text)
{
    struct edc_session *session = edc_session_store_resolve_session(sm, message->session_id);

    struct edc_task_group *group = session ? session->foreground_group : NULL;
    if (group && (group->is_orphaned || group->member_count == 0 || edc_task_group_is_system(group)))
        group = NULL;

    struct edc_cmd_task_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.input = sdsnew(text);
    /* The sender may have declared a better label (a `/prompt` expansion names the line
     * the user typed, not the body it became); otherwise the text is the label. */
    opts.desc = edc_str_utf8_truncate(message->task_desc ? message->task_desc : text, EDC_TASK_DESC_MAX, true);

    struct edc_task_group *out_group = NULL;
    sds spawn_err = NULL;
    struct edc_task *task = edc_shell_spawn_task(sm, sched, to.type, session, &opts, NULL, group, "task",
                                                 &out_group, NULL, false, NULL, TASK_DEP_SEQUENCE, &spawn_err);
    if (spawn_err)
    {
        edc_commands_reply(to, spawn_err);
        sdsfree(spawn_err);
    }
    if (task)
        edc_commands_announce_task(to, task);

    edc_cmd_task_opts_free(&opts); /* frees input + desc */
    return true;
}

bool edc_shell_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                       struct edc_channel_message *message)
{
    if (!message || !message->content || sdslen(message->content) == 0)
        return false;

    /* 开关是**词法器模式**（docs/input-pipeline.zh.md §1.5）：关闭时 sigil/算符/选项根本不
     * 成为 token，整行退化成 word，并记一笔 saw_disabled 供下面的 note 用。 */
    const bool shell_on = sched && sched->config && sched->config->shell_syntax;

    edc_lex_out toks;
    if (!edc_lex(message->content, shell_on ? EDC_LEX_SHELL_ON : EDC_LEX_SHELL_OFF, &toks))
        return false;

    sds cerr = NULL;
    const edc_in_kind kind = edc_classify(&toks, &cerr);
    if (cerr)
    {
        edc_commands_reply(to, cerr);
        sdsfree(cerr);
        edc_lex_out_free(&toks);
        return true;
    }

    /* 开关关闭却用了 shell 语法：说一声，别静默改道（Ⓔ）。 */
    if (toks.saw_disabled)
        edc_commands_reply(to, "Shell-like syntax is off (`/shell on`) — this line went to the model "
                               "as a message, not to the shell layer.");

    if (kind == EDC_IN_SHELL)
    {
        edc_ast ast;
        sds perr = NULL;
        const bool ok = edc_parse(&toks, &ast, &perr);
        edc_lex_out_free(&toks);
        if (!ok)
        {
            edc_commands_reply(to, perr ? perr : "syntax error");
            if (perr)
                sdsfree(perr);
            return true;
        }
        edc_interp_shell(sm, sched, to, message, &ast);
        edc_ast_free(&ast);
        return true;
    }

    edc_lex_out_free(&toks);
    if (kind == EDC_IN_MESSAGE)
        return edc_shell_spawn_plain_task(sm, sched, to, message, message->content);

    /* COMMAND / LOCALSHELL 由派发层（edc_commands_handler）先一步处理掉。 */
    return false;
}
