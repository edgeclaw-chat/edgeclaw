#include "edc_interp.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_store.h"
#include "edc_session_manager.h"
#include "edc_shell.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"

#include <cJSON.h>
#include <log.h>
#include <stdlib.h>
#include <string.h>

/* ── 分类：**纯 token 函数**（§2 的提交点）─────────────────────────────
 *
 * 只读 token 流：命中即定、不回退。**不读源字节** —— 如果某个判断在 token 里表达不出来，
 * 那是 token 模型缺东西（`fully_quoted` 曾把"以引号开头"和"是一个闭合引号区"揉在一起，
 * 于是"引号没闭合"两不沾，分类器只能回头读字节；现在它有自己的 token：`TK_BAD_QUOTED`）。
 *
 * 开关关闭时不需要在这里再判一次：那时的词法器不产出 sigil/算符，也不给任何 word 打
 * `fully_quoted`（词汇表不存在 ⇒ 引号不是信号）。 */
edc_in_kind edc_classify(const edc_lex_out *toks, sds *err)
{
    if (err)
        *err = NULL;
    if (!toks || toks->count == 0 || toks->toks[0].kind == TK_EOF)
        return EDC_IN_MESSAGE; /* Ⓒ 空输入：调用方什么也不做 */

    const edc_tok *f = &toks->toks[0];
    if (f->kind == TK_SLASH_CMD)
        return EDC_IN_COMMAND; /* 命令层永不受开关影响 */
    if (f->kind == TK_SIGIL_LOCALSHELL)
        return EDC_IN_LOCALSHELL;

    /* 行首开了引号却没闭合：已提交到 shell 行，是语法错误，不是消息。 */
    if (f->kind == TK_BAD_QUOTED)
    {
        if (err)
            *err = sdsnew("Unterminated quoted string — a line that opens with a quote must close it.");
        return EDC_IN_MESSAGE;
    }
    if (f->kind == TK_WORD && f->fully_quoted)
        return EDC_IN_SHELL;

    if (f->kind == TK_SIGIL_NEWGROUP)
    {
        const edc_tok *n = (toks->count > 1) ? &toks->toks[1] : NULL;
        if (!n || n->kind == TK_EOF)
        {
            if (err)
                *err = sdsnew("`+` needs a quoted description, e.g. `+\"<text>\"`.");
            return EDC_IN_MESSAGE;
        }
        if (n->kind == TK_BAD_QUOTED)
        {
            if (err)
                *err = sdsnew("Unterminated quoted string — the `\"…\"` after `+` has no closing quote.");
            return EDC_IN_MESSAGE;
        }
        /* Ⓐ `+!cmd` / `+/task …`：两层语法不组合（token 的 value 里看得见）。 */
        if (n->kind == TK_WORD && n->value && (n->value[0] == '!' || n->value[0] == '/'))
        {
            if (err)
                *err = sdsnew("`+` is shell syntax and `/` / `!` are not -- a line is one or the other, "
                              "they do not combine.\n\nFor a task in a group of its own: "
                              "`/task new \"<description>\" --new-group`.");
            return EDC_IN_MESSAGE;
        }
        if (n->kind == TK_WORD && n->fully_quoted)
            return EDC_IN_SHELL;
        return EDC_IN_MESSAGE; /* ① `+foo`（有内容、不是引号）→ 消息 */
    }
    return EDC_IN_MESSAGE;
}

/* `< file`：把文件内容追加到这条 turn 的用户消息（与老 AST_REDIR_IN 一致）。 */
static void apply_input_file(struct edc_task *task, const char *path)
{
    sds err = NULL;
    sds content = edc_shell_read_file(path, &err);
    if (err)
        sdsfree(err);
    if (!content || !task->active_turn)
    {
        if (content)
            sdsfree(content);
        return;
    }
    for (size_t mj = task->active_turn->messages.head; mj < task->active_turn->messages.tail; mj++)
    {
        cJSON *msg = task->active_turn->messages.data[mj];
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0)
            continue;
        cJSON *c = cJSON_GetObjectItem(msg, "content");
        if (!c || !cJSON_IsString(c))
            continue;
        sds merged = sdscatprintf(sdsempty(), "%s\n\n%s", c->valuestring, content);
        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(merged));
        sdsfree(merged);
    }
    sdsfree(content);
}

bool edc_interp_shell(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                      struct edc_channel_message *message, const edc_ast *ast)
{
    struct edc_session *session = edc_session_store_resolve_session(sm, message->session_id);
    if (!session)
        return false;

    struct edc_task_group *group = session->foreground_group;
    if (group && (group->is_orphaned || group->member_count == 0 || edc_task_group_is_system(group)))
        group = NULL;
    if (ast->new_group)
        group = NULL; /* `+`：新组（首个任务会成为 leader） */

    struct edc_task *prev = NULL;
    edc_task_dep_type dep = TASK_DEP_SEQUENCE;

    for (struct edc_ast_pipeline *p = ast->pipelines; p; p = p->next)
    {
        dep = TASK_DEP_SEQUENCE; /* 跨 pipeline（`;`）：只等、不注入 */
        for (struct edc_ast_stage *s = p->stages; s; s = s->next)
        {
            /* 重定向：按 fd 收，同 fd 后写覆盖（shell 的 last-wins）。 */
            sds r_out = NULL, r_err = NULL, r_in = NULL;
            bool r_out_app = false, r_err_app = false;
            for (struct edc_ast_redir *r = s->redirs; r; r = r->next)
            {
                if (r->fd == 1)
                {
                    if (r_out)
                        sdsfree(r_out);
                    r_out = sdsdup(r->target);
                    r_out_app = r->append;
                }
                else if (r->fd == 2)
                {
                    if (r_err)
                        sdsfree(r_err);
                    r_err = sdsdup(r->target);
                    r_err_app = r->append;
                }
                else
                {
                    if (r_in)
                        sdsfree(r_in);
                    r_in = sdsdup(r->target);
                }
            }

            /* `{{…}}` 任务引用：**按 stage 顺序**收集（前一个 stage 的任务已存在）。
             * 词法器只在双引号区里标记，所以这里用 has_var_refs 就够了。收集器会同时
             * 校验引用、把 {{SELF}} 就地解析掉。 */
            sds *vdeps = NULL;
            int vn = 0;
            sds stage_text = s->text;
            sds processed = NULL;
            if (s->has_var_refs)
            {
                sds verr = NULL;
                processed = edc_shell_collect_var_refs(sm, sched, s->text, &vdeps, &vn, &verr);
                if (!processed)
                {
                    edc_commands_reply(to, verr ? verr : "bad task reference");
                    if (verr)
                        sdsfree(verr);
                    for (struct edc_ast_redir *r = s->redirs; r; r = r->next)
                        ;
                    if (r_in)
                        sdsfree(r_in);
                    continue; /* 这一条 stage 不建任务，其余照旧 */
                }
                stage_text = processed;
            }

            struct edc_cmd_task_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.input = processed ? processed : sdsdup(s->text); /* processed 的所有权给 opts */
            opts.desc = edc_str_utf8_truncate(stage_text, EDC_TASK_DESC_MAX, true);
            opts.bg = ast->background; /* 链尾 `&`：作用整条链的每个 stage（⑨） */
            if (s->ndep > 0)
            {
                opts.dep_ids = malloc(sizeof(sds) * (size_t)s->ndep);
                for (int i = 0; i < s->ndep; i++)
                    opts.dep_ids[i] = sdsdup(s->deps[i]);
                opts.dep_count = s->ndep;
            }
            if (vn > 0)
            {
                const int base = opts.dep_count;
                opts.dep_ids = realloc(opts.dep_ids, sizeof(sds) * (size_t)(base + vn));
                for (int i = 0; i < vn; i++)
                    opts.dep_ids[base + i] = vdeps[i]; /* 所有权转移 */
                opts.dep_count = base + vn;
                free(vdeps);
                vdeps = NULL;
            }
            if (s->group)
                opts.group_id = sdsdup(s->group);

            struct edc_task_group *out_group = NULL;
            sds spawn_err = NULL;
            struct edc_task *task = edc_shell_spawn_task(sm, sched, to.type, session, &opts, prev, group,
                                                         ast->new_group ? "+" : "task", &out_group, r_out,
                                                         r_out_app, r_err, dep, &spawn_err);
            if (spawn_err)
            {
                edc_commands_reply(to, spawn_err);
                sdsfree(spawn_err);
            }
            if (task)
            {
                if (r_err_app)
                    task->redirect_stderr_append = true;
                if (r_in)
                    apply_input_file(task, r_in);
                if (s->has_var_refs)
                    task->has_deferred_vars = 1;
                edc_commands_announce_task(to, task);
                if (out_group)
                    group = out_group; /* 后续 stage 进同一个组 */
            }
            if (r_in)
                sdsfree(r_in);
            edc_cmd_task_opts_free(&opts);

            prev = task ? task : prev;
            dep = TASK_DEP_JOIN; /* 同一 pipeline 内：JOIN（注入源输出） */
        }
    }
    return true;
}
