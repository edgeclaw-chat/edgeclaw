#include "edc_shell.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_file_utils.h"
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

sds *edc_shell_split_pipeline(const char *input, int *count)
{
    int tok_count = 0;
    edc_shell_token_t *tokens = edc_shell_tokenize(input, &tok_count);

    int n = 0;
    int *ranges = edc_shell_tokens_split_pipe(tokens, tok_count, &n);

    sds *segs = malloc((size_t)n * sizeof(sds));
    for (int i = 0; i < n; i++)
        segs[i] = edc_shell_tokens_join(tokens, ranges[i * 2], ranges[i * 2 + 1]);

    for (int i = 0; i < tok_count; i++)
        edc_shell_token_free(&tokens[i]);
    free(tokens);
    free(ranges);
    *count = n;
    return segs;
}

static struct edc_task *shell_find_task_by_id_or_job(struct edc_agent *agent, const char *id_or_job)
{
    return edc_session_find_task(edc_session_manager_get_active(agent->session_manager), id_or_job);
}

static void shell_cmd_jobs(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel)
{
    sds out = sdsempty();
    struct edc_task_group *fg = agent->scheduler->foreground_group;

    if (!fg || fg->member_count == 0)
    {
        out = sdscat(out, "no jobs");
    }
    else
    {
        struct edc_task *t;
        LL_FOREACH2(fg->members, t, group_next)
        {
            out = sdscatprintf(out, "  [%s] %-12s", t->task_id, edc_task_state_name(t->state));
            if (*t->description)
                out = sdscatprintf(out, "  \"%s\"", t->description);
            if (edc_session_manager_get_active(agent->session_manager) &&
                edc_session_manager_get_active(agent->session_manager)->active_task == t)
                out = sdscat(out, "  [fg]");
            out = sdscat(out, "\n");
        }
    }
    edc_commands_reply(agent, actor, channel, out);
    sdsfree(out);
}

static void shell_cmd_cancel(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel, sds arg)
{
    if (!arg || sdslen(arg) == 0)
    {
        edc_commands_reply(agent, actor, channel, "/cancel <id>");
        return;
    }
    struct edc_task *t = shell_find_task_by_id_or_job(agent, arg);
    if (!t)
    {
        sds reply = sdscatprintf(sdsempty(), "task not found: %s", arg);
        edc_commands_reply(agent, actor, channel, reply);
        sdsfree(reply);
        return;
    }
    edc_scheduler_send_signal_to_task(agent->scheduler, t->task_id, TASK_SIGTERM);
    sds reply = sdscatprintf(sdsempty(), "cancelled %s", t->task_id);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);
}

bool edc_shell_handle_builtin(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message)
{
    sds content = sdstrim(sdsnew(message->content), " \t\r\n");

    if (strcmp(content, "/jobs") == 0)
    {
        shell_cmd_jobs(agent, actor, message->channel);
        sdsfree(content);
        return true;
    }
    if (strncmp(content, "/cancel ", 8) == 0)
    {
        sds arg = sdsnew(content + 8);
        sdstrim(arg, " \t");
        shell_cmd_cancel(agent, actor, message->channel, arg);
        sdsfree(arg);
        sdsfree(content);
        return true;
    }
    if (strcmp(content, "/whoami") == 0)
    {
        struct edc_session *sess = edc_session_manager_get_active(agent->session_manager);
        struct edc_task_group *fg = agent->scheduler->foreground_group;
        struct edc_task *active = sess ? sess->active_task : NULL;
        sds out = sdscatprintf(sdsempty(),
                               "session: %s\n"
                               "model: %s\n",
                               sess ? edc_session_get_name(sess) : "none",
                               agent->config->llm_api_conf.model ? agent->config->llm_api_conf.model : "default");
        if (fg)
        {
            struct edc_task *leader = fg->leader;
            out = sdscatprintf(out,
                               "[fg] group: %s (%d tasks)\n"
                               "      leader: %s  %s\n"
                               "      active: %s",
                               fg->group_id, fg->member_count, leader ? leader->task_id : "?",
                               (leader && *leader->description) ? leader->description : "",
                               active ? active->task_id : "none");
            if (active && *active->description)
                out = sdscatprintf(out, "  %s", active->description);
        }
        else
        {
            out = sdscat(out, "foreground group: none");
        }
        edc_commands_reply(agent, actor, message->channel, out);
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
            if (agent->config->llm_api_conf.model)
                sdsfree(agent->config->llm_api_conf.model);
            agent->config->llm_api_conf.model = sdsdup(arg);
            sds reply = sdscatprintf(sdsempty(), "model set to: %s", agent->config->llm_api_conf.model);
            edc_commands_reply(agent, actor, message->channel, reply);
            sdsfree(reply);
        }
        sdsfree(arg);
        sdsfree(content);
        return true;
    }
    if (strcmp(content, "/model") == 0)
    {
        sds reply = sdscatprintf(sdsempty(), "model: %s",
                                 agent->config->llm_api_conf.model ? agent->config->llm_api_conf.model : "default");
        edc_commands_reply(agent, actor, message->channel, reply);
        sdsfree(reply);
        sdsfree(content);
        return true;
    }
    if (strcmp(content, "/history") == 0)
    {
        sds edc_home = edc_config_resolve_bc_home();
        sds history_path = edc_path_join(edc_home, ".cli_history", NULL);
        EdcError *err = NULL;
        sds contents = edc_file_read(history_path, &err);
        if (contents && sdslen(contents) > 0)
        {
            /* show last 20 entries */
            int nlines = 0;
            sds *lines = sdssplitlen(contents, sdslen(contents), "\n", 1, &nlines);
            sds out = sdsempty();
            int start = nlines > 20 ? nlines - 20 : 0;
            for (int i = start; i < nlines; i++)
                out = sdscatprintf(out, "  %d  %s\n", i + 1, lines[i]);
            edc_commands_reply(agent, actor, message->channel, sdslen(out) > 0 ? out : "(empty)");
            sdsfree(out);
            sdsfreesplitres(lines, nlines);
        }
        else
        {
            edc_commands_reply(agent, actor, message->channel, "(no history)");
        }
        if (contents)
            sdsfree(contents);
        if (err)
            edc_error_free(err);
        sdsfree(history_path);
        sdsfree(edc_home);
        sdsfree(content);
        return true;
    }

    sdsfree(content);
    return false;
}

struct edc_task *edc_shell_spawn_task(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel *channel,
                                      struct edc_cmd_task_opts *opts, struct edc_task *prev_task,
                                      struct edc_task_group *target_group, const char *reply_prefix,
                                      struct edc_task_group **out_group, sds redirect_stdout, bool redirect_append,
                                      sds redirect_stderr, edc_task_dep_type prev_dep_type)
{
    struct edc_task *task = edc_scheduler_create_task(
        agent->scheduler, (opts->desc && sdslen(opts->desc) > 0) ? opts->desc : "New task", edc_channel_copy(channel));
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
        group = edc_scheduler_create_group(agent->scheduler, task);
    }

    if (!opts->bg)
    {
        edc_scheduler_set_foreground(agent->scheduler, group);
        edc_session_set_active_task(edc_session_manager_get_active(agent->session_manager), task);
    }

    for (int i = 0; i < opts->dep_count; i++)
    {
        struct edc_task *dt =
            edc_session_find_task(edc_session_manager_get_active(agent->session_manager), opts->dep_ids[i]);
        if (dt)
            edc_task_add_dependency(task, dt, TASK_DEP_JOIN);
    }

    if (prev_task)
        edc_task_add_dependency(task, prev_task, prev_dep_type);

    sds reply = sdscatprintf(sdsempty(), "[%s] created %s (group %s)%s%s%s%s%s%s", reply_prefix, task->task_id,
                             group->group_id, opts->bg ? " [bg]" : "", opts->dep_count > 0 ? " [chained]" : "",
                             prev_task ? (prev_dep_type == TASK_DEP_SEQUENCE ? " [seq]" : " [pipe]") : "",
                             redirect_stdout ? (redirect_append ? " >> " : " > ") : "",
                             redirect_stdout ? redirect_stdout : "", redirect_stderr ? " 2>" : "");
    if (redirect_stderr)
        reply = sdscatprintf(reply, "%s", redirect_stderr);
    edc_commands_reply(agent, actor, channel, reply);
    sdsfree(reply);

    if (opts->input)
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(task->task_id);
        edc_turn_add_user_message(turn, opts->input);
        task->active_turn = turn;

        static int64_t counter = 0;
        void *req_id = (void *)(intptr_t)(++counter);
        edc_scheduler_register_task(agent->scheduler, task, req_id);
        task->current_request_id = req_id;

        log_info("%s spawn: task=%s state=%s deps_total=%zu input='%s'", reply_prefix, task->task_id,
                 edc_task_state_name(task->state), task->deps_total, opts->input);

        if (task->state != TASK_WAITING_DEPS)
        {
            bool ok = edc_task_set_state(task, TASK_LLM_PROCESSING);
            log_info("%s spawn: set_state LLM_PROCESSING -> %s, send_llm=%s", reply_prefix, ok ? "ok" : "FAILED",
                     (agent->scheduler && agent->scheduler->send_llm) ? "ready" : "NULL");
            if (ok && agent->scheduler && agent->scheduler->send_llm)
                agent->scheduler->send_llm(agent->scheduler, task, req_id);
        }
        else
        {
            log_info("%s spawn: task %s blocked in WAITING_DEPS, turn queued", reply_prefix, task->task_id);
        }
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

void edc_shell_strip_redirects(sds *segment, sds *out_stdout, bool *out_append, sds *out_stderr, sds *out_input_file)
{
    *out_stdout = NULL;
    *out_append = false;
    *out_stderr = NULL;
    *out_input_file = NULL;

    int tok_count = 0;
    edc_shell_token_t *tokens = edc_shell_tokenize(*segment, &tok_count);

    /* scan for redirect tokens, collect surviving text token indices */
    int *keep = malloc((size_t)tok_count * sizeof(int));
    int keep_n = 0;
    int i = 0;

    while (i < tok_count)
    {
        if (tokens[i].type == TOK_REDIR_OUT || tokens[i].type == TOK_REDIR_APPEND)
        {
            bool is_stderr = false;
            if (i > 0 && tokens[i - 1].type == TOK_TEXT && tokens[i - 1].value)
            {
                sds tv = tokens[i - 1].value;
                size_t slen = sdslen(tv);
                if (slen > 0 && tv[slen - 1] == '2')
                {
                    if (slen == 1 || tv[slen - 2] == ' ' || tv[slen - 2] == '\t')
                    {
                        is_stderr = true;
                        sdssetlen(tv, slen - 1);
                        sdstrim(tv, " \t");
                        if (sdslen(tv) == 0 && keep_n > 0)
                            keep_n--;
                    }
                }
            }

            bool is_append = (tokens[i].type == TOK_REDIR_APPEND);
            i++; /* skip redirect token */

            if (i < tok_count && tokens[i].type == TOK_TEXT && tokens[i].value)
            {
                /* split first word as filename, keep rest */
                sds val = tokens[i].value;
                char *sp = strchr(val, ' ');
                sds fname;
                if (sp)
                {
                    fname = sdsnewlen(val, (size_t)(sp - val));
                    sds rest = sdsnew(sp + 1);
                    sdstrim(rest, " \t");
                    sdsfree(tokens[i].value);
                    tokens[i].value = rest; /* modified token holds remaining text */
                }
                else
                {
                    fname = sdsdup(val);
                    i++; /* consume token */
                }
                if (is_stderr)
                {
                    if (*out_stderr)
                        sdsfree(*out_stderr);
                    *out_stderr = fname;
                }
                else
                {
                    if (*out_stdout)
                        sdsfree(*out_stdout);
                    *out_stdout = fname;
                    *out_append = is_append;
                }
            }
            else
            {
                i++;
            }
            continue;
        }

        if (tokens[i].type == TOK_REDIR_IN)
        {
            i++;
            if (i < tok_count && tokens[i].type == TOK_TEXT && tokens[i].value)
            {
                sds val = tokens[i].value;
                char *sp = strchr(val, ' ');
                if (sp)
                {
                    if (*out_input_file)
                        sdsfree(*out_input_file);
                    *out_input_file = sdsnewlen(val, (size_t)(sp - val));
                    sds rest = sdsnew(sp + 1);
                    sdstrim(rest, " \t");
                    sdsfree(tokens[i].value);
                    tokens[i].value = rest;
                }
                else
                {
                    if (*out_input_file)
                        sdsfree(*out_input_file);
                    *out_input_file = sdsdup(val);
                    i++;
                }
            }
            else
            {
                i++;
            }
            continue;
        }

        if (tokens[i].type == TOK_PIPE || tokens[i].type == TOK_SEQ || tokens[i].type == TOK_BG ||
            tokens[i].type == TOK_TEXT)
        {
            keep[keep_n++] = i;
        }
        i++;
    }

    /* rebuild segment from kept tokens */
    sds clean;
    if (keep_n == 0)
    {
        clean = sdsempty();
    }
    else
    {
        clean = sdsempty();
        for (int i = 0; i < keep_n; i++)
        {
            edc_shell_token_t *t = &tokens[keep[i]];
            if (t->type == TOK_TEXT && t->value)
            {
                if (sdslen(clean) > 0)
                    clean = sdscat(clean, " ");
                clean = sdscatsds(clean, t->value);
            }
            else if (t->type == TOK_BG)
                clean = sdscat(clean, " &");
            else if (t->type == TOK_PIPE)
                clean = sdscat(clean, " |");
            else if (t->type == TOK_SEQ)
                clean = sdscat(clean, " ;");
        }
        sdstrim(clean, " \t");
    }

    for (int i = 0; i < tok_count; i++)
        edc_shell_token_free(&tokens[i]);
    free(tokens);
    free(keep);
    sdsfree(*segment);
    *segment = clean;
}

static sds shell_expand_vars(struct edc_agent *agent, const char *input, sds *out_error);
static sds shell_collect_var_refs(struct edc_agent *agent, const char *input, sds **out_deps, int *out_dep_count,
                                  sds *out_error);

sds edc_shell_expand_vars(struct edc_agent *agent, const char *input, sds *out_error)
{
    return shell_expand_vars(agent, input, out_error);
}

static struct edc_task *shell_exec_pipeline(struct edc_agent *agent, struct edc_actor *actor,
                                            struct edc_channel *channel, sds *segments, int seg_count, bool is_at,
                                            struct edc_task *initial_prev)
{
    struct edc_task *prev_task = initial_prev;
    struct edc_task_group *target_group = NULL;
    bool is_first = true;

    if (!is_at)
    {
        target_group = agent->scheduler->foreground_group;
        if (!target_group || target_group->is_orphaned || target_group->member_count == 0)
            target_group = NULL;
    }

    for (int i = 0; i < seg_count; i++)
    {
        if (sdslen(segments[i]) == 0)
            continue;

        sds redirect_stdout = NULL, redirect_stderr = NULL, input_file = NULL;
        bool redirect_append = false;
        edc_shell_strip_redirects(&segments[i], &redirect_stdout, &redirect_append, &redirect_stderr, &input_file);

        /* collect $VAR references: validate + collect deps (keep $ text) */
        sds *var_deps = NULL;
        int var_dep_count = 0;
        sds var_err = NULL;
        sds processed = shell_collect_var_refs(agent, segments[i], &var_deps, &var_dep_count, &var_err);
        if (!processed)
        {
            edc_commands_reply(agent, actor, channel, var_err);
            sdsfree(var_err);
            sdsfree(segments[i]);
            if (redirect_stdout)
                sdsfree(redirect_stdout);
            if (redirect_stderr)
                sdsfree(redirect_stderr);
            if (input_file)
                sdsfree(input_file);
            continue;
        }
        sdsfree(segments[i]);
        segments[i] = processed;

        struct edc_cmd_task_opts opts;
        edc_cmd_parse_task_opts(segments[i], &opts);

        /* merge $VAR deps into opts */
        for (int j = 0; j < var_dep_count; j++)
        {
            opts.dep_count++;
            opts.dep_ids = realloc(opts.dep_ids, (size_t)opts.dep_count * sizeof(sds));
            opts.dep_ids[opts.dep_count - 1] = var_deps[j];
        }
        free(var_deps);
        int has_deferred = (var_dep_count > 0) || (strstr(segments[i], "$LAST") != NULL);

        if (input_file)
        {
            sds err = NULL;
            sds file_content = edc_shell_read_file(input_file, &err);
            if (file_content)
            {
                if (opts.input && sdslen(opts.input) > 0)
                {
                    opts.input = sdscat(opts.input, "\n\n");
                    opts.input = sdscatsds(opts.input, file_content);
                    sdsfree(file_content);
                }
                else
                {
                    if (opts.input)
                        sdsfree(opts.input);
                    opts.input = file_content;
                }
            }
            else if (err)
            {
                edc_commands_reply(agent, actor, channel, err);
                sdsfree(err);
                sdsfree(input_file);
                if (redirect_stdout)
                    sdsfree(redirect_stdout);
                if (redirect_stderr)
                    sdsfree(redirect_stderr);
                edc_cmd_task_opts_free(&opts);
                continue;
            }
            sdsfree(input_file);
        }

        struct edc_task_group *group = NULL;
        if (!is_at && opts.group_id)
        {
            group = edc_session_find_group(edc_session_manager_get_active(agent->session_manager), opts.group_id);
            if (!group)
            {
                sds reply = sdscatprintf(sdsempty(), "group %s not found", opts.group_id);
                edc_commands_reply(agent, actor, channel, reply);
                sdsfree(reply);
            }
        }
        if (!group && !is_at)
            group = target_group;

        edc_task_dep_type dep = (is_first && initial_prev) ? TASK_DEP_SEQUENCE : TASK_DEP_JOIN;
        struct edc_task_group *out_group = NULL;
        prev_task = edc_shell_spawn_task(agent, actor, channel, &opts, prev_task, group, is_at ? "@" : "task",
                                         &out_group, redirect_stdout, redirect_append, redirect_stderr, dep);
        if (has_deferred && prev_task)
            prev_task->has_deferred_vars = 1;
        is_first = false;
        if (!target_group && out_group)
            target_group = out_group;

        edc_cmd_task_opts_free(&opts);
    }
    return prev_task;
}

sds *edc_shell_split_sequential(const char *input, int *count)
{
    int tok_count = 0;
    edc_shell_token_t *tokens = edc_shell_tokenize(input, &tok_count);

    int n = 0;
    int *ranges = edc_shell_tokens_split_seq(tokens, tok_count, &n);

    sds *segs = malloc((size_t)n * sizeof(sds));
    for (int i = 0; i < n; i++)
        segs[i] = edc_shell_tokens_join(tokens, ranges[i * 2], ranges[i * 2 + 1]);

    for (int i = 0; i < tok_count; i++)
        edc_shell_token_free(&tokens[i]);
    free(tokens);
    free(ranges);
    *count = n;
    return segs;
}

static struct edc_task *shell_find_last_completed(struct edc_agent *agent)
{
    struct edc_task *last = NULL;
    uint32_t highest_seq = 0;
    struct edc_task_group *g;
    DL_FOREACH(agent->scheduler->session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t->state == TASK_COMPLETED && t->completion_seq > highest_seq)
            {
                highest_seq = t->completion_seq;
                last = t;
            }
        }
    }
    return last;
}

sds edc_shell_expand_history(struct edc_agent *agent, const char *input)
{
    if (!agent || agent->history_count == 0)
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
                result = sdscat(result, agent->history[agent->history_count - 1]);
            }
            else if (*p >= '0' && *p <= '9')
            {
                int idx = 0;
                while (*p >= '0' && *p <= '9')
                {
                    idx = idx * 10 + (*p - '0');
                    p++;
                }
                if (idx > 0 && idx <= agent->history_count)
                    result = sdscat(result, agent->history[idx - 1]);
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

static sds shell_expand_vars(struct edc_agent *agent, const char *input, sds *out_error);

static sds shell_collect_var_refs(struct edc_agent *agent, const char *input, sds **out_deps, int *out_dep_count,
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
                    edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
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
                    struct edc_task *t =
                        edc_session_find_task(edc_session_manager_get_active(agent->session_manager), buf);
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
                    struct edc_task *t =
                        edc_session_find_task(edc_session_manager_get_active(agent->session_manager), id);
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

static sds shell_expand_vars(struct edc_agent *agent, const char *input, sds *out_error)
{
    sds result = sdsempty();
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
                goto expand_dq;
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
            p++;
        expand_dq:
            if (*p == '$')
            {
                p++;
                struct edc_task *active =
                    edc_session_get_active_task(edc_session_manager_get_active(agent->session_manager));
                if (active)
                    result = sdscat(result, active->task_id);
            }
            else if (strncmp(p, "LAST", 4) == 0 && VAR_DELIM(p[4]))
            {
                p += 4;
                struct edc_task *last = shell_find_last_completed(agent);
                if (!last || !last->output)
                {
                    if (out_error)
                        *out_error = sdscatprintf(sdsempty(), "$LAST: no completed task with output");
                    sdsfree(result);
                    return NULL;
                }
                result = sdscatsds(result, last->output);
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
                    struct edc_task *t =
                        edc_session_find_task(edc_session_manager_get_active(agent->session_manager), buf);
                    if (!t || !t->output)
                    {
                        if (out_error)
                            *out_error = sdscatprintf(sdsempty(), "$%s: no output", buf);
                        sdsfree(result);
                        return NULL;
                    }
                    result = sdscatsds(result, t->output);
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
                    struct edc_task *t =
                        edc_session_find_task(edc_session_manager_get_active(agent->session_manager), id);
                    if (!t || !t->output)
                    {
                        if (out_error)
                            *out_error = sdscatprintf(sdsempty(), "$%s: no output", id);
                        sdsfree(id);
                        sdsfree(result);
                        return NULL;
                    }
                    result = sdscatsds(result, t->output);
                    sdsfree(id);
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

bool edc_shell_execute(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message)
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
    sds hist_expanded = edc_shell_expand_history(agent, body);

    /* tokenize → parse → execute via AST */
    int tok_count = 0;
    edc_shell_token_t *tokens = edc_shell_tokenize(hist_expanded, &tok_count);
    sdsfree(hist_expanded);

    ast_node_t *root = ast_parse(tokens, tok_count, is_at);
    ast_execute(agent, actor, message->channel, root);
    ast_free(root);

    for (int i = 0; i < tok_count; i++)
        edc_shell_token_free(&tokens[i]);
    free(tokens);
    return true;
}
