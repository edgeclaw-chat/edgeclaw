#ifndef __BC_CMD_PARSE_H__
#define __BC_CMD_PARSE_H__

#include "edc_msg_part.h" /* struct edc_msg_part (pass 追加的附加消息) */
#include "sds.h"
#include <stdbool.h>

/* Max UTF-8 characters in a task description (desc); longer is truncated with "...". */
#define EDC_TASK_DESC_MAX 30

/* ── /task command parse result ────────────────────────────── */

struct edc_cmd_task_opts
{
    sds desc;     /* task description (the remaining text) */
    sds input;    /* quoted input -- '+' syntax only */
    bool bg;        /* --bg background */
    bool new_group; /* --new-group: open a group of its own instead of joining the one in front */
    bool literal; /* input came from a '''/''' block or single quotes: do not expand {{…}} refs */
    sds *dep_ids; /* -d/--with <id> dependency list (JOIN: waits and injects) */
    int dep_count;
    sds group_id;    /* --group <id> / -g <id> */
    /* The first token in the OPTION AREA that is not an option and not the `2` of a
     * stderr redirect. The option area takes options; anything else is an error rather
     * than text silently dropped (`"hello" and more` must not lose ` and more`, and a
     * message that happens to open with a quote lands exactly there). NULL = clean. */
    sds unknown_option;
    /* pass 追加的附加消息（@文件读出来的正文等），**借用**自 AST 的载荷：opts 不拥有
     * 它们，调用方在解释期间保持 AST 存活。模型看到的额外内容就靠这张表 —— 新增一个
     * pass 只往载荷里 add_part，不需要再动 opts / spec / task_create。 */
    const struct edc_msg_part *parts;
    int nparts;
};

/* ── API ───────────────────────────────────────────────────── */

/* The option words a shell line accepts AFTER its quoted description, and a one-line
 * brief each. This is the single table the completion palette's shell-line vocabulary
 * comes from (it mirrors edc_lex.l, which is what actually tokenizes them): the rows ARE
 * these entries, so the popup cannot promise an option the lexer does not know. A row's
 * text stops at the argument shape ("-d <task>"), so completing it leaves a space for it. */
size_t edc_cmd_shell_option_count(void);
const char *edc_cmd_shell_option_text(size_t index);
const char *edc_cmd_shell_option_brief(size_t index);

/** Unquote ONE leading quoted region -- `"…"`, `'…'`, `"""…"""`, `'''…'''`, escapes as in
 *  Python -- and store the position just past the closing delimiter in |*end_out|.
 *  Returns the inner text (caller frees), or NULL when |s| does not open a quoted region
 *  (or never closes one). Used wherever a string operand is taken, e.g. a redirect target. */
extern sds edc_cmd_unquote(const char *s, const char **end_out);

/** Parse task operation arguments. arg may be NULL; the caller frees with edc_cmd_task_opts_free. */
extern void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts);

/** True when |arg| looks like it was given options (a token starting with `-`) while
 *  its description is NOT quoted. The two callers that accept options (`@` and
 *  `/task new`) reject that as a syntax error: options are only recognised after a
 *  quoted description, so letting them through would silently swallow the option into
 *  the description text -- the description must be quoted instead. */
extern bool edc_cmd_desc_needs_quotes(const char *arg);

/** Free the resources parse allocated. */
extern void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts);

#endif
