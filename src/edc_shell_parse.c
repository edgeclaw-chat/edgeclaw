#include "edc_shell_ast.h"
#include "edc_shell_token.h"
#include <stdlib.h>
#include <string.h>

static bool is_flag(const char *s)
{
    return (strcmp(s, "--bg") == 0 || strcmp(s, "-d") == 0 ||
            strcmp(s, "--group") == 0 || strcmp(s, "-g") == 0 ||
            strcmp(s, "--prompt") == 0);
}

static ast_node_t *parse_cmd(edc_shell_token_t *tokens, int count, int *pos);

/* ── parse one command: text + options (no redirects/bg) ─── */

static ast_node_t *parse_cmd(edc_shell_token_t *tokens, int count, int *pos)
{
    ast_node_t *cmd = ast_new(AST_CMD);
    sds text = sdsempty();

    while (*pos < count)
    {
        edc_shell_token_t *t = &tokens[*pos];

        if (t->type == TOK_PIPE || t->type == TOK_SEQ ||
            t->type == TOK_BG ||
            t->type == TOK_REDIR_OUT || t->type == TOK_REDIR_APPEND ||
            t->type == TOK_REDIR_IN)
            break;

        if (t->type == TOK_TEXT && t->value)
        {
            if (is_flag(t->value))
            {
                const char *flag = t->value;
                (*pos)++; /* consume flag */
                if (*pos < count && tokens[*pos].type == TOK_TEXT && tokens[*pos].value)
                {
                    sds arg = tokens[*pos].value;
                    if (strcmp(flag, "-d") == 0)
                    {
                        cmd->dep_count++;
                        cmd->dep_ids = realloc(cmd->dep_ids,
                            (size_t)cmd->dep_count * sizeof(sds));
                        cmd->dep_ids[cmd->dep_count - 1] = sdsnew(arg);
                    }
                    else if (strcmp(flag, "-g") == 0 || strcmp(flag, "--group") == 0)
                    {
                        if (cmd->group_id) sdsfree(cmd->group_id);
                        cmd->group_id = sdsnew(arg);
                    }
                    else if (strcmp(flag, "--prompt") == 0)
                    {
                        if (cmd->prompt_name) sdsfree(cmd->prompt_name);
                        cmd->prompt_name = sdsnew(arg);
                    }
                    (*pos)++; /* consume arg */
                }
            }
            else
            {
                if (sdslen(text) > 0) text = sdscat(text, " ");
                text = sdscatsds(text, t->value);
                (*pos)++;
            }
        }
        else
        {
            (*pos)++;
        }
    }

    cmd->text = text;
    return cmd;
}

/* ── parse command with redirect modifiers ────────────────── */

static ast_node_t *parse_redirects(edc_shell_token_t *tokens, int count,
                                    int *pos, ast_node_t *inner)
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
                    if (slen == 1 || inner->text[slen - 2] == ' ' ||
                        inner->text[slen - 2] == '\t')
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

static ast_node_t *parse_bg_cmd(edc_shell_token_t *tokens, int count,
                                  int *pos, bool at_group)
{
    ast_node_t *cmd = parse_cmd(tokens, count, pos);
    cmd->at_group = at_group;

    ast_node_t *inner = parse_redirects(tokens, count, pos, cmd);

    if (*pos < count && tokens[*pos].type == TOK_BG)
    {
        (*pos)++;
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
            if (!pipe_chain) pipe_chain = cmd;
            else
            {
                /* append to pipe chain */
                ast_node_t *c = pipe_chain;
                while (c->next) c = c->next;
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
                while (c->next) c = c->next;
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
