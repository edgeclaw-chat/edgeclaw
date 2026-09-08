#include "edc_shell_ast.h"
#include "edc_shell_token.h"
#include "edc_cmd_parse.h"
#include <stdlib.h>
#include <string.h>

static ast_node_t *parse_cmd(edc_shell_token_t *tokens, int count, int *pos);

/* ── parse one command: text + options (no redirects/bg) ─── */

static ast_node_t *parse_cmd(edc_shell_token_t *tokens, int count, int *pos)
{
    ast_node_t *cmd = ast_new(AST_CMD);
    sds text = sdsempty();
    bool literal = false;

    /* Collect all TOK_TEXT into one string. The tokenizer doesn't split on
     * whitespace, so flags like -d / -g / --bg / --prompt end up embedded
     * in text tokens. We defer all flag parsing to edc_cmd_parse_task_opts below. */
    while (*pos < count)
    {
        edc_shell_token_t *t = &tokens[*pos];

        if (t->type == TOK_PIPE || t->type == TOK_SEQ || t->type == TOK_BG || t->type == TOK_REDIR_OUT ||
            t->type == TOK_REDIR_APPEND || t->type == TOK_REDIR_IN)
            break;

        if (t->type == TOK_TEXT && t->value)
        {
            if (sdslen(text) > 0)
                text = sdscat(text, " ");
            text = sdscatsds(text, t->value);
        }
        (*pos)++;
    }

    /* Extract flags and input from the collected text string */
    if (sdslen(text) > 0)
    {
        struct edc_cmd_task_opts t_opts;
        edc_cmd_parse_task_opts(text, &t_opts);

        /* Replace text with input portion (flags stripped) */
        if (t_opts.input)
        {
            sdsfree(text);
            text = t_opts.input;
            t_opts.input = NULL;
        }

        cmd->bg = t_opts.bg;

        /* Literal input (''' / ' blocks) keeps $VAR text verbatim: no
         * deferred variable expansion is registered for it. */
        literal = t_opts.literal;

        /* Merge dependencies from -d flags */
        if (t_opts.dep_count > 0)
        {
            cmd->dep_ids = t_opts.dep_ids;
            cmd->dep_count = t_opts.dep_count;
            t_opts.dep_ids = NULL;  /* transfer ownership */
            t_opts.dep_count = 0;
        }

        /* Merge group_id */
        if (t_opts.group_id)
        {
            cmd->group_id = t_opts.group_id;
            t_opts.group_id = NULL;
        }

        /* Merge prompt_name */
        if (t_opts.prompt_name)
        {
            cmd->prompt_name = t_opts.prompt_name;
            t_opts.prompt_name = NULL;
        }

        edc_cmd_task_opts_free(&t_opts);
    }

    cmd->text = text;

    /* detect $VAR references: mark for AST_VAR_ASSIGN wrapping.
     * Literal inputs ('''/'-quoted) never expand, so they skip this. */
    if (text && strchr(text, '$') && !literal)
        cmd->has_deferred_vars = true;

    return cmd;
}

/* ── parse command with redirect modifiers ────────────────── */

static ast_node_t *parse_redirects(edc_shell_token_t *tokens, int count, int *pos, ast_node_t *inner)
{
    while (*pos < count)
    {
        edc_shell_token_t *t = &tokens[*pos];

        if (t->type == TOK_PIPE || t->type == TOK_SEQ || t->type == TOK_BG)
            break;

        if (t->type == TOK_REDIR_OUT || t->type == TOK_REDIR_APPEND)
        {
            bool is_append = (t->type == TOK_REDIR_APPEND);
            ast_type_t rtype = is_append ? AST_REDIR_APPEND : AST_REDIR_OUT;

            /* check for 2> */
            if (!is_append)
            {
                size_t slen = inner->text ? sdslen(inner->text) : 0;
                if (slen > 0 && inner->text[slen - 1] == '2')
                {
                    if (slen == 1 || inner->text[slen - 2] == ' ' || inner->text[slen - 2] == '\t')
                    {
                        rtype = AST_REDIR_ERR;
                        sdssetlen(inner->text, slen - 1);
                        sdstrim(inner->text, " \t");
                    }
                }
            }

            (*pos)++;
            if (*pos < count && tokens[*pos].type == TOK_TEXT && tokens[*pos].value)
            {
                sds val = tokens[*pos].value;
                char *sp = strchr(val, ' ');
                sds fname;
                if (sp)
                {
                    fname = sdsnewlen(val, (size_t)(sp - val));
                    sds rest = sdsnew(sp + 1);
                    sdstrim(rest, " \t");
                    sdsfree(tokens[*pos].value);
                    tokens[*pos].value = rest;
                }
                else
                {
                    fname = sdsdup(val);
                    (*pos)++;
                }

                ast_node_t *wrap = ast_new(rtype);
                wrap->filename = fname;
                wrap->child = inner;
                inner = wrap;
            }
            else
            {
                (*pos)++;
            }
            continue;
        }

        if (t->type == TOK_REDIR_IN)
        {
            (*pos)++;
            if (*pos < count && tokens[*pos].type == TOK_TEXT && tokens[*pos].value)
            {
                sds val = tokens[*pos].value;
                char *sp = strchr(val, ' ');
                sds fname;
                if (sp)
                {
                    fname = sdsnewlen(val, (size_t)(sp - val));
                    sds rest = sdsnew(sp + 1);
                    sdstrim(rest, " \t");
                    sdsfree(tokens[*pos].value);
                    tokens[*pos].value = rest;
                }
                else
                {
                    fname = sdsdup(val);
                    (*pos)++;
                }

                ast_node_t *wrap = ast_new(AST_REDIR_IN);
                wrap->filename = fname;
                wrap->child = inner;
                inner = wrap;
            }
            else
            {
                (*pos)++;
            }
            continue;
        }

        break;
    }

    return inner;
}

/* ── parse bg_cmd := redirects cmd '&'? ────────────────────── */

static ast_node_t *parse_bg_cmd(edc_shell_token_t *tokens, int count, int *pos, bool at_group)
{
    ast_node_t *cmd = parse_cmd(tokens, count, pos);
    cmd->at_group = at_group;

    /* save $VAR info before redirect wrapping may modify cmd->text */
    bool has_vars = cmd->has_deferred_vars;
    sds saved_text = has_vars ? sdsdup(cmd->text) : NULL;

    ast_node_t *inner = parse_redirects(tokens, count, pos, cmd);

    /* wrap with AST_VAR_ASSIGN if this command has $VAR references */
    if (has_vars)
    {
        ast_node_t *va = ast_new(AST_VAR_ASSIGN);
        va->child = inner;
        va->text = saved_text; /* original text with $VAR refs for executor */
        inner = va;
    }

    bool is_bg = cmd->bg;

    if (*pos < count && tokens[*pos].type == TOK_BG)
    {
        (*pos)++;
        is_bg = true;
    }

    if (is_bg)
    {
        ast_node_t *bg = ast_new(AST_BG);
        bg->child = inner;
        return bg;
    }

    return inner;
}

/* ── public entry: parse full input ────────────────────────── */

ast_node_t *ast_parse(edc_shell_token_t *tokens, int count, bool is_at)
{
    int pos = 0;

    ast_node_t *root = ast_new(AST_ROOT);
    ast_node_t **tail = &root->child;

    ast_node_t *pipe_chain = NULL; /* current pipe chain being built */

    while (pos < count)
    {
        ast_node_t *cmd = parse_bg_cmd(tokens, count, &pos, is_at);
        /* only first cmd in chain gets is_at */
        is_at = false;

        if (pos < count && tokens[pos].type == TOK_PIPE)
        {
            pos++;
            if (!pipe_chain)
                pipe_chain = cmd;
            else
            {
                /* append to pipe chain */
                ast_node_t *c = pipe_chain;
                while (c->next)
                    c = c->next;
                c->next = cmd;
            }
        }
        else if (pos < count && tokens[pos].type == TOK_SEQ)
        {
            pos++;
            if (pipe_chain)
            {
                /* close pipe chain, add to seq */
                ast_node_t *wrapper = ast_new(AST_PIPE);
                wrapper->child = pipe_chain;
                *tail = wrapper;
                tail = &wrapper->next;
                pipe_chain = NULL;
            }
            else
            {
                *tail = cmd;
                tail = &cmd->next;
            }
        }
        else
        {
            /* end of chain */
            if (pipe_chain)
            {
                ast_node_t *c = pipe_chain;
                while (c->next)
                    c = c->next;
                c->next = cmd;

                ast_node_t *wrapper = ast_new(AST_PIPE);
                wrapper->child = pipe_chain;
                *tail = wrapper;
                pipe_chain = NULL;
            }
            else
            {
                *tail = cmd;
            }
        }
    }

    /* any remaining pipe chain */
    if (pipe_chain)
    {
        ast_node_t *wrapper = ast_new(AST_PIPE);
        wrapper->child = pipe_chain;
        *tail = wrapper;
    }

    /* flatten: if only one child, no SEQ wrapper needed */
    if (root->child && !root->child->next)
    {
        ast_node_t *single = root->child;
        root->child = NULL;
        ast_free(root);
        return single;
    }

    /* wrap children in SEQ */
    ast_node_t *seq = ast_new(AST_SEQ);
    seq->child = root->child;
    root->child = NULL;
    ast_free(root);

    if (!seq->child->next)
    {
        /* single child, no SEQ needed */
        ast_node_t *single = seq->child;
        seq->child = NULL;
        ast_free(seq);
        return single;
    }

    return seq;
}
