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
                              "`/task new \"<description>\" --new-group`, `/prompt --new-group <name>` "
                              "or `/skill --new-group <name>`.");
            return EDC_IN_MESSAGE;
        }
        if (n->kind == TK_WORD && n->fully_quoted)
            return EDC_IN_SHELL;
        return EDC_IN_MESSAGE; /* ① `+foo`（有内容、不是引号）→ 消息 */
    }
    return EDC_IN_MESSAGE;
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
    edc_task_dep_type dep = TASK_DEP_BARRIER;

    for (struct edc_ast_pipeline *p = ast->pipelines; p; p = p->next)
    {
        /* pipeline 之间，算符决定边的"种类"（都只等、不注入）：
         *   `;`  -> BARRIER  源怎么结束都放行（shell 的 `;`）
         *   `&&` -> SEQUENCE 只有成功才继续，源失败/被停就级联取消后件、边保留
         *   `||` -> UNLESS   反过来：源成功就跳过（取消）后件，源失败/被停就放行 */
        switch (p->chain)
        {
        case EDC_CHAIN_AND: dep = TASK_DEP_SEQUENCE; break;
        case EDC_CHAIN_OR: dep = TASK_DEP_UNLESS; break;
        case EDC_CHAIN_SEMI:
        default: dep = TASK_DEP_BARRIER; break;
        }
        for (struct edc_ast_stage *s = p->stages; s; s = s->next)
        {
            /* 重定向：按 fd 收，同 fd 后写覆盖（shell 的 last-wins）。 */
            sds r_out = NULL, r_err = NULL;
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
            }

            /* `{{…}}` 任务引用：**按 stage 顺序**收集（前一个 stage 的任务已存在）。
             * 词法器只在双引号区里标记，所以这里用 has_var_refs 就够了。收集器会同时
             * 校验引用、把 {{SELF}} 就地解析掉。 */
            sds *vdeps = NULL;
            int vn = 0;
            sds stage_text = s->msg.text;
            sds processed = NULL;
            if (s->has_var_refs)
            {
                sds verr = NULL;
                processed = edc_shell_collect_var_refs(sm, sched, s->msg.text, &vdeps, &vn, &verr);
                if (!processed)
                {
                    edc_commands_reply(to, verr ? verr : "bad task reference");
                    if (verr)
                        sdsfree(verr);
                    continue; /* 这一条 stage 不建任务，其余照旧 */
                }
                stage_text = processed;
            }

            struct edc_cmd_task_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.input = processed ? processed : sdsdup(s->msg.text); /* processed 的所有权给 opts */
            opts.desc = edc_str_utf8_truncate(stage_text, EDC_TASK_DESC_MAX, true);
            /* pass 追加的消息（借用；AST 活到解释结束）。解释器不知道它们是什么。 */
            opts.parts = s->msg.parts;
            opts.nparts = s->msg.nparts;
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
            /* 前一个任务和"怎么等它"是一个整体：没有前一个就没有这一类边（见 edc_prev_dep）。 */
            const struct edc_prev_dep link = {prev, dep};
            struct edc_task *task = edc_shell_spawn_task(sm, sched, to.type, session, &opts, prev ? &link : NULL,
                                                         group, ast->new_group ? "+" : "task", &out_group, r_out,
                                                         r_out_app, r_err, &spawn_err);
            if (spawn_err)
            {
                edc_commands_reply(to, spawn_err);
                sdsfree(spawn_err);
            }
            if (task)
            {
                if (r_err_app)
                    task->redirect_stderr_append = true;
                if (s->has_var_refs)
                    task->has_deferred_vars = 1;
                edc_commands_announce_task(to, task);
                if (out_group)
                    group = out_group; /* 后续 stage 进同一个组 */
            }
            edc_cmd_task_opts_free(&opts);

            prev = task ? task : prev;
            dep = TASK_DEP_JOIN; /* 同一 pipeline 内：JOIN（注入源输出） */
        }
    }
    return true;
}

/* 消息：整条载荷 -> 一个 task。载荷是 passes 定稿后的样子（synthetic 已挂好）。 */
static bool interp_message(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                           struct edc_channel_message *message, const edc_ast *ast)
{
    return edc_shell_spawn_plain_task(sm, sched, to, message, &ast->message);
}

bool edc_interp(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                struct edc_channel_message *message, const edc_ast *ast)
{
    if (!ast)
        return false;
    if (ast->kind == EDC_AST_MESSAGE)
        return interp_message(sm, sched, to, message, ast);
    return edc_interp_shell(sm, sched, to, message, ast);
}
