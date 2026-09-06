#include "edc_channel_cli.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_completion.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_markdown_render.h"
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#include <uv.h>

#define CLI_RESET "\e[0m"
#define CLI_DIM "\e[2m"
#define CLI_HISTORY_MAX 1000

/* ---- completion state (kept static to avoid header changes) ---- */
static edc_completion_result_t g_comp;
static bool g_comp_active = false;

static void cli_redraw(edc_channel_actor_state_t *state);
static void cli_display_output(edc_channel_actor_state_t *state);

/* ---- history ---- */

static void cli_history_load(edc_channel_actor_state_t *state)
{
    if (!state->cli.history_path || !sdslen(state->cli.history_path))
        return;
    if (!edc_file_exists(state->cli.history_path))
        return;

    EdcError *error = NULL;
    sds contents = edc_file_read(state->cli.history_path, &error);
    if (!contents)
    {
        if (error)
            edc_error_free(error);
        return;
    }

    int count;
    sds *lines = sdssplitlen(contents, (ssize_t)sdslen(contents), "\n", 1, &count);
    sdsfree(contents);
    if (!lines)
        return;

    state->cli.history_capacity = count + 32;
    state->cli.history = (sds *)malloc((size_t)state->cli.history_capacity * sizeof(sds));

    for (int i = 0; i < count && state->cli.history_count < CLI_HISTORY_MAX; i++)
    {
        if (sdslen(lines[i]) > 0)
            state->cli.history[state->cli.history_count++] = lines[i];
        else
            sdsfree(lines[i]);
    }
    free(lines);
}

static void cli_history_save(edc_channel_actor_state_t *state)
{
    if (!state->cli.history_path || !sdslen(state->cli.history_path) || state->cli.history_count == 0)
        return;

    sds all = sdsempty();
    for (int i = 0; i < state->cli.history_count; i++)
        all = sdscatprintf(all, "%s\n", state->cli.history[i]);

    EdcError *error = NULL;
    edc_file_write(state->cli.history_path, all, -1, &error);
    if (error)
        edc_error_free(error);
    sdsfree(all);
}

static void cli_history_add(edc_channel_actor_state_t *state, const char *line)
{
    if (!line || !line[0])
        return;

    if (state->cli.history_count > 0 && strcmp(state->cli.history[state->cli.history_count - 1], line) == 0)
        return;

    if (state->cli.history_count >= CLI_HISTORY_MAX)
    {
        sdsfree(state->cli.history[0]);
        memmove(state->cli.history, state->cli.history + 1, (size_t)(state->cli.history_count - 1) * sizeof(sds));
        state->cli.history_count--;
    }

    if (state->cli.history_count >= state->cli.history_capacity)
    {
        state->cli.history_capacity = state->cli.history_capacity ? state->cli.history_capacity * 2 : 32;
        state->cli.history = (sds *)realloc(state->cli.history, (size_t)state->cli.history_capacity * sizeof(sds));
    }

    state->cli.history[state->cli.history_count++] = sdsnew(line);
}

/* ---- line editing helpers ---- */

static int cli_display_width(const char *s, int byte_pos)
{
    int w = 0;
    int i = 0;
    while (i < byte_pos)
    {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x7F)
        {
            w++;
            i++;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            w += 2;
            i += 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            w += 2;
            i += 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            w += 2;
            i += 4;
        }
        else
        {
            i++;
        }
    }
    return w;
}

static int cli_prev_char_pos(const char *s, int byte_pos)
{
    if (byte_pos <= 0)
        return 0;
    int pos = byte_pos - 1;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static int cli_next_char_pos(const char *s, int byte_pos, int total_len)
{
    if (byte_pos >= total_len)
        return total_len;
    unsigned char c = (unsigned char)s[byte_pos];
    if ((c & 0x80) == 0)
        return byte_pos + 1;
    if ((c & 0xE0) == 0xC0)
        return byte_pos + 2;
    if ((c & 0xF0) == 0xE0)
        return byte_pos + 3;
    if ((c & 0xF8) == 0xF0)
        return byte_pos + 4;
    return byte_pos + 1;
}

static sds cli_buf_insert(edc_channel_actor_state_t *state, const char *s, size_t len)
{
    sds new_s = sdsnewlen(state->cli.line_buffer, (size_t)state->cli.cursor_pos);
    new_s = sdscatlen(new_s, s, len);
    new_s = sdscat(new_s, state->cli.line_buffer + state->cli.cursor_pos);
    sdsfree(state->cli.line_buffer);
    state->cli.cursor_pos += (int)len;
    return new_s;
}

static sds cli_buf_delete(edc_channel_actor_state_t *state, int pos, int count)
{
    sds new_s = sdsnewlen(state->cli.line_buffer, (size_t)pos);
    new_s = sdscat(new_s, state->cli.line_buffer + pos + count);
    sdsfree(state->cli.line_buffer);
    state->cli.cursor_pos = pos;
    return new_s;
}

/* ---- display ---- */

static int cli_get_term_width(edc_channel_actor_state_t *state)
{
    (void)state;

#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
#endif

    const char *cols = getenv("COLUMNS");
    if (cols)
    {
        int w = atoi(cols);
        if (w > 0)
            return w;
    }

    return 80;
}

static int cli_calc_lines(edc_channel_actor_state_t *state, sds line)
{
    int prompt_w = 2;
    int total_w = prompt_w + cli_display_width(line, (int)sdslen(line));
    int tw = cli_get_term_width(state);
    int lines = (total_w + tw - 1) / tw;
    return lines > 0 ? lines : 1;
}

static void cli_redraw(edc_channel_actor_state_t *state)
{
    int new_lines = cli_calc_lines(state, state->cli.line_buffer);

    /* move cursor up to cover all previous lines of the input */
    if (state->cli.prev_lines > 1)
    {
        for (int l = 1; l < state->cli.prev_lines; l++)
            fprintf(stdout, "\e[A");
    }
    fprintf(stdout, "\r\e[J");

    /* draw prompt and input */
    fprintf(stdout, "\e[0m\e[1;36m> \e[0m");

    sds prefix = sdsnewlen(state->cli.line_buffer, (size_t)state->cli.cursor_pos);
    fprintf(stdout, "%s\e[s%s\e[u", prefix, state->cli.line_buffer + state->cli.cursor_pos);
    sdsfree(prefix);

    state->cli.prev_lines = new_lines;
    fflush(stdout);
}

static void cli_display_output(edc_channel_actor_state_t *state)
{
    if (!state->cli.pending_output)
        return;

    sds fixed = sdsempty();
    for (size_t i = 0; i < sdslen(state->cli.pending_output); i++)
    {
        char c = state->cli.pending_output[i];
        if (c == '\n' && (i == 0 || state->cli.pending_output[i - 1] != '\r'))
            fixed = sdscat(fixed, "\r\n");
        else
            fixed = sdscatlen(fixed, &c, 1);
    }

    fprintf(stdout, "\r\e[K%s\r\n", fixed);
    fflush(stdout);

    sdsfree(fixed);
    sdsfree(state->cli.pending_output);
    state->cli.pending_output = NULL;

    state->cli.prev_lines = 0;
    cli_redraw(state);
}

/* ---- commit line to actor ---- */

static void cli_commit_line(edc_actor_t *actor, const char *line)
{
    edc_channel_actor_state_t *state = actor->state;

    if (line && line[0] != '\0')
    {
        cli_history_add(state, line);
        edc_completion_add_history(line);
        if (g_active_agent)
            edc_agent_add_history(g_active_agent, line);
    }

    fprintf(stdout, "\r\n");
    fflush(stdout);

    if (line && line[0] != '\0')
    {
        struct edc_channel_message *msg = edc_channel_message_new(edc_channel_cli, sdsnew(line));
        edc_actor_msg_t *actor_msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, actor, NULL, msg,
                                                       (edc_actor_data_free_fn)edc_channel_message_free);
        edc_actor_send(state->observer, actor_msg);
    }

    sdsfree(state->cli.line_buffer);
    state->cli.line_buffer = sdsempty();
    state->cli.cursor_pos = 0;

    if (state->cli.saved_line)
    {
        sdsfree(state->cli.saved_line);
        state->cli.saved_line = NULL;
    }
    state->cli.history_pos = -1;

    if (state->cli.running)
    {
        state->cli.prev_lines = 0;
        cli_redraw(state);
    }
}

/* ---- history navigation via escape sequences ---- */

static void cli_history_up(edc_channel_actor_state_t *state)
{
    if (state->cli.history_count == 0 || state->cli.history_pos >= state->cli.history_count - 1)
        return;

    if (state->cli.history_pos == -1)
    {
        if (state->cli.saved_line)
            sdsfree(state->cli.saved_line);
        state->cli.saved_line = sdsdup(state->cli.line_buffer);
    }

    state->cli.history_pos++;
    sdsfree(state->cli.line_buffer);
    state->cli.line_buffer = sdsdup(state->cli.history[state->cli.history_count - 1 - state->cli.history_pos]);
    state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
    cli_redraw(state);
}

static void cli_history_down(edc_channel_actor_state_t *state)
{
    if (state->cli.history_pos > 0)
    {
        state->cli.history_pos--;
        sdsfree(state->cli.line_buffer);
        state->cli.line_buffer = sdsdup(state->cli.history[state->cli.history_count - 1 - state->cli.history_pos]);
        state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
        cli_redraw(state);
    }
    else if (state->cli.history_pos == 0 && state->cli.saved_line)
    {
        state->cli.history_pos = -1;
        sdsfree(state->cli.line_buffer);
        state->cli.line_buffer = sdsdup(state->cli.saved_line);
        state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
        sdsfree(state->cli.saved_line);
        state->cli.saved_line = NULL;
        cli_redraw(state);
    }
}

/* ---- input processing ---- */

static void cli_process_input(edc_channel_actor_state_t *state, const char *data, ssize_t len)
{
    edc_actor_t *actor = state->actor;

    for (ssize_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)data[i];

        /* 非 TAB 按键：重置补全状态 */
        if (c != 9 && g_comp_active)
        {
            g_comp_active = false;
            edc_completion_result_free(&g_comp);
        }

        /* escape sequences */
        if (c == '\x1b' && i + 1 < len)
        {
            i++;
            unsigned char c2 = (unsigned char)data[i];
            if (c2 == '[' || c2 == 'O')
            {
                char seq[16] = {0};
                seq[0] = (char)c2;
                int si = 1;
                while (i + 1 < len && si < 15)
                {
                    i++;
                    unsigned char nc = (unsigned char)data[i];
                    seq[si++] = (char)nc;
                    if ((nc >= 0x40 && nc <= 0x7E) || nc == '|')
                        break;
                }

                if (!strcmp(seq, "[A") || !strcmp(seq, "OA"))
                    cli_history_up(state);
                else if (!strcmp(seq, "[B") || !strcmp(seq, "OB"))
                    cli_history_down(state);
                else if (!strcmp(seq, "[C") || !strcmp(seq, "OC"))
                {
                    int total = (int)sdslen(state->cli.line_buffer);
                    if (state->cli.cursor_pos < total)
                    {
                        state->cli.cursor_pos = cli_next_char_pos(state->cli.line_buffer, state->cli.cursor_pos, total);
                        cli_redraw(state);
                    }
                }
                else if (!strcmp(seq, "[D") || !strcmp(seq, "OD"))
                {
                    if (state->cli.cursor_pos > 0)
                    {
                        state->cli.cursor_pos = cli_prev_char_pos(state->cli.line_buffer, state->cli.cursor_pos);
                        cli_redraw(state);
                    }
                }
                else if (!strcmp(seq, "[H") || !strcmp(seq, "OH") || !strcmp(seq, "[1~"))
                {
                    state->cli.cursor_pos = 0;
                    cli_redraw(state);
                }
                else if (!strcmp(seq, "[F") || !strcmp(seq, "OF") || !strcmp(seq, "[4~"))
                {
                    state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
                    cli_redraw(state);
                }
                else if (!strcmp(seq, "[3~"))
                {
                    int total = (int)sdslen(state->cli.line_buffer);
                    if (state->cli.cursor_pos < total)
                    {
                        int next = cli_next_char_pos(state->cli.line_buffer, state->cli.cursor_pos, total);
                        state->cli.line_buffer =
                            cli_buf_delete(state, state->cli.cursor_pos, next - state->cli.cursor_pos);
                        cli_redraw(state);
                    }
                    cli_redraw(state);
                }
            }
            continue;
        }

        /* enter */
        if (c == '\r' || c == '\n')
        {
            cli_commit_line(actor, state->cli.line_buffer);
            continue;
        }

        /* Ctrl+C */
        if (c == 3)
        {
            if (sdslen(state->cli.line_buffer) == 0)
            {
                state->cli.running = 0;
                fprintf(stdout, "\r\n");
                cli_history_save(state);
                raise(SIGINT);
                return;
            }
            sdsfree(state->cli.line_buffer);
            state->cli.line_buffer = sdsempty();
            state->cli.cursor_pos = 0;
            cli_redraw(state);
            continue;
        }

        /* Ctrl+D */
        if (c == 4)
        {
            if (sdslen(state->cli.line_buffer) == 0)
            {
                state->cli.running = 0;
                fprintf(stdout, "\r\n");
                cli_history_save(state);
                raise(SIGINT);
                return;
            }
            continue;
        }

        /* backspace — delete one full UTF-8 character before cursor */
        if (c == 127 || c == '\b')
        {
            if (state->cli.cursor_pos > 0)
            {
                int pos = state->cli.cursor_pos - 1;
                while (pos > 0 && ((unsigned char)state->cli.line_buffer[pos] & 0xC0) == 0x80)
                    pos--;
                int count = state->cli.cursor_pos - pos;
                state->cli.line_buffer = cli_buf_delete(state, pos, count);
                state->cli.cursor_pos = pos;
                cli_redraw(state);
            }
            continue;
        }

        /* Ctrl+W - delete word backwards */
        if (c == 23)
        {
            if (state->cli.cursor_pos > 0)
            {
                int pos = state->cli.cursor_pos;
                while (pos > 0 && state->cli.line_buffer[pos - 1] == ' ')
                    pos--;
                while (pos > 0 && state->cli.line_buffer[pos - 1] != ' ')
                    pos--;
                int count = state->cli.cursor_pos - pos;
                state->cli.line_buffer = cli_buf_delete(state, pos, count);
                state->cli.cursor_pos = pos;
                cli_redraw(state);
            }
            continue;
        }

        /* Tab — completion (new engine) */
        if (c == 9)
        {
            int len = (int)sdslen(state->cli.line_buffer);
            if (len == 0)
                continue;

            int wstart = state->cli.cursor_pos;
            while (wstart > 0 && state->cli.line_buffer[wstart - 1] != ' ')
                wstart--;
            int wlen = state->cli.cursor_pos - wstart;
            if (wlen == 0)
                continue;

            /* 首次 TAB：收集候选项 */
            if (!g_comp_active)
            {
                edc_completion_result_free(&g_comp);
                edc_completion_collect(state->cli.line_buffer, state->cli.cursor_pos, &g_comp);
                g_comp_active = true;

                if (g_comp.count == 0)
                {
                    /* 无匹配，重置 */
                    g_comp_active = false;
                    continue;
                }

                if (g_comp.count == 1)
                {
                    /* 唯一匹配：直接补全 + 空格 */
                    const char *match = g_comp.items[0].text;
                    for (size_t ci = (size_t)wlen; ci < strlen(match); ci++)
                    {
                        char tmp[2] = {match[ci], '\0'};
                        state->cli.line_buffer = cli_buf_insert(state, tmp, 1);
                    }
                    state->cli.line_buffer = sdscatlen(state->cli.line_buffer, " ", 1);
                    state->cli.cursor_pos++;
                    edc_completion_result_free(&g_comp);
                    g_comp_active = false;
                    cli_redraw(state);
                    continue;
                }

                /* 多个候选项：补全公共前缀 */
                if (g_comp.common_prefix && sdslen(g_comp.common_prefix) > (size_t)wlen)
                {
                    size_t cp_len = sdslen(g_comp.common_prefix);
                    for (size_t ci = (size_t)wlen; ci < cp_len; ci++)
                    {
                        char tmp[2] = {g_comp.common_prefix[ci], '\0'};
                        state->cli.line_buffer = cli_buf_insert(state, tmp, 1);
                    }
                    cli_redraw(state);
                    /* 不选第一个，等用户再按 TAB 看列表 */
                    continue;
                }

                /* 公共前缀等于 token（各候选差异在后部）：展示候选列表 */
                fprintf(stdout, "\r\n");
                for (int ci = 0; ci < g_comp.count; ci++)
                {
                    fprintf(stdout, "  %s", g_comp.items[ci].text);
                    if ((ci + 1) % 4 == 0 || ci == g_comp.count - 1)
                        fprintf(stdout, "\r\n");
                    else
                        fprintf(stdout, "    ");
                }
                state->cli.prev_lines = 0;
                cli_redraw(state);
                continue;
            }

            /* 再次 TAB：循环选择候选项 */
            if (g_comp_active && g_comp.count > 0)
            {
                /* 回退当前 token 到 wstart */
                state->cli.line_buffer = cli_buf_delete(state, wstart, state->cli.cursor_pos - wstart);
                state->cli.cursor_pos = wstart;

                /* 选下一个 */
                g_comp.selected = (g_comp.selected + 1) % g_comp.count;
                const char *sel = g_comp.items[g_comp.selected].text;
                state->cli.line_buffer = cli_buf_insert(state, sel, strlen(sel));
                cli_redraw(state);
                continue;
            }
        }

        /* printable ASCII + UTF-8 continuation bytes */
        if (c >= 32)
        {
            char ins[2] = {(char)c, '\0'};
            state->cli.line_buffer = cli_buf_insert(state, ins, 1);
            cli_redraw(state);
            continue;
        }
    }
}

/* ---- libuv callbacks ---- */

static void cli_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
    (void)handle;
    buf->base = (char *)malloc(suggested);
    buf->len = suggested;
}

static void cli_on_stdin_data(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    edc_channel_actor_state_t *state = stream->data;
    if (nread < 0)
    {
        free(buf->base);
        state->cli.running = 0;
        if (!state->cli.tty_stopped)
        {
            state->cli.tty_stopped = true;
            uv_read_stop(stream);
            uv_close((uv_handle_t *)stream, NULL);
        }
        raise(SIGINT);
        return;
    }
    if (nread > 0)
        cli_process_input(state, buf->base, nread);
    free(buf->base);

    if (!state->cli.running && !state->cli.tty_stopped)
    {
        state->cli.tty_stopped = true;
        uv_read_stop(stream);
        uv_close((uv_handle_t *)stream, NULL);
    }
}

/* ---- send handler ---- */

static void edc_channel_cli_handle_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;
    struct edc_channel_message *message = msg->data;

    if (state->cli.pending_output)
        sdsfree(state->cli.pending_output);

    if (strncmp(message->content, "[思考]:", strlen("[思考]:")) == 0)
    {
        state->cli.pending_output = sdscatprintf(sdsempty(), "%s%s%s", CLI_DIM, message->content, CLI_RESET);
    }
    else
    {
        state->cli.pending_output = edc_markdown_render(message->content);
    }

    cli_display_output(state);
}

/* ---- actor handler ---- */

static void edc_channel_cli_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_START: {
        edc_terminal_vt_enable();
        if (edc_terminal_raw_mode(STDIN_FILENO, &state->cli.orig_term) != 0)
        {
            fprintf(stderr, "Failed to set raw terminal mode\r\n");
            state->cli.running = 0;
            break;
        }

        uv_tty_init(actor->loop, &state->cli.stdin_tty, STDIN_FILENO, 1);
        state->cli.stdin_tty.data = state;
        uv_read_start((uv_stream_t *)&state->cli.stdin_tty, cli_alloc_cb, cli_on_stdin_data);

        state->cli.line_buffer = sdsempty();
        state->cli.cursor_pos = 0;
        state->cli.pending_output = NULL;
        state->cli.running = 1;
        state->cli.history_pos = -1;
        state->cli.saved_line = NULL;

        cli_history_load(state);

        /* 将历史记录也喂给补全引擎 */
        for (int hi = 0; hi < state->cli.history_count; hi++)
            edc_completion_add_history(state->cli.history[hi]);

        fprintf(stdout, "\e[1;32mEdgeClaw CLI ready. Type /help for commands.\e[0m\r\n");
        cli_redraw(state);
        break;
    }
    case BC_ACTOR_MSG_CHANNEL_SEND:
        edc_channel_cli_handle_send(actor, msg);
        break;
    default:
        break;
    }
}

void edc_channel_cli_init(edc_actor_t *channel_actor)
{
    edc_channel_actor_state_t *state = channel_actor->state;
    memset(&state->cli, 0, sizeof(state->cli));
    sds edc_home = edc_config_resolve_bc_home();
    state->cli.history_path = edc_path_join(edc_home, ".cli_history", NULL);
    sdsfree(edc_home);
    state->cli.history_pos = -1;

    /* 初始化补全引擎 */
    edc_completion_init();

    /* 注册所有 slash 命令 */
    static const char *cmds[] = {"/new",
                                 "/stop",
                                 "/help",
                                 "/exit",
                                 "/jobs",
                                 "/tasks",
                                 "/cancel",
                                 "/bg",
                                 "/fg",
                                 "/whoami",
                                 "/model",
                                 "/history",
                                 "/task new",
                                 "/task fork",
                                 "/task list",
                                 "/task status",
                                 "/task output",
                                 "/task fg",
                                 "/task bg",
                                 "/task switch",
                                 "/task stop",
                                 "/task groups",
                                 "/task chain",
                                 "/task join",
                                 "/task prompt",
                                 "/task suspend",
                                 "/task resume",
                                 "/session new",
                                 "/session switch",
                                 "/session list",
                                 "/session stop",
                                 "/session rename",
                                 "/session info",
                                 NULL};
    for (const char **c = cmds; *c; c++)
        edc_completion_add_command(*c);

    edc_channel_actor_add(channel_actor, edc_channel_cli, edc_channel_cli_handler);
}
