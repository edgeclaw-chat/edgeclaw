#include "sds.h"
#include "edc_channel_actor.h"
#include "edc_channel_cli.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_markdown_render.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define CLI_RESET     "\e[0m"
#define CLI_DIM       "\e[2m"
#define CLI_HISTORY_MAX 1000

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
    if (!contents) {
        if (error) edc_error_free(error);
        return;
    }

    int count;
    sds *lines = sdssplitlen(contents, (ssize_t)sdslen(contents), "\n", 1, &count);
    sdsfree(contents);
    if (!lines) return;

    state->cli.history_capacity = count + 32;
    state->cli.history = (sds *)malloc((size_t)state->cli.history_capacity * sizeof(sds));

    for (int i = 0; i < count && state->cli.history_count < CLI_HISTORY_MAX; i++) {
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
    if (error) edc_error_free(error);
    sdsfree(all);
}

static void cli_history_add(edc_channel_actor_state_t *state, const char *line)
{
    if (!line || !line[0]) return;

    if (state->cli.history_count > 0 &&
        strcmp(state->cli.history[state->cli.history_count - 1], line) == 0)
        return;

    if (state->cli.history_count >= CLI_HISTORY_MAX) {
        sdsfree(state->cli.history[0]);
        memmove(state->cli.history, state->cli.history + 1,
                (size_t)(state->cli.history_count - 1) * sizeof(sds));
        state->cli.history_count--;
    }

    if (state->cli.history_count >= state->cli.history_capacity) {
        state->cli.history_capacity = state->cli.history_capacity ? state->cli.history_capacity * 2 : 32;
        state->cli.history = (sds *)realloc(state->cli.history,
                                             (size_t)state->cli.history_capacity * sizeof(sds));
    }

    state->cli.history[state->cli.history_count++] = sdsnew(line);
}

/* ---- line editing helpers ---- */

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
    return new_s;
}

/* ---- display ---- */

static void cli_redraw(edc_channel_actor_state_t *state)
{
    fprintf(stdout, "\r\e[K\e[0m\e[1;36m> \e[0m%s", state->cli.line_buffer);

    int delta = (int)sdslen(state->cli.line_buffer) - state->cli.cursor_pos;
    if (delta > 0)
        fprintf(stdout, "\e[%dD", delta);
    fflush(stdout);
}

static void cli_display_output(edc_channel_actor_state_t *state)
{
    if (!state->cli.pending_output) return;

    sds fixed = sdsempty();
    for (size_t i = 0; i < sdslen(state->cli.pending_output); i++) {
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

    cli_redraw(state);
}

/* ---- commit line to actor ---- */

static void cli_commit_line(edc_actor_t *actor, const char *line)
{
    edc_channel_actor_state_t *state = actor->state;

    if (line && line[0] != '\0')
        cli_history_add(state, line);

    fprintf(stdout, "\r\n");
    fflush(stdout);

    if (line && line[0] != '\0') {
        struct edc_channel *channel = edc_channel_cli_new();
        struct edc_channel_message *msg = edc_channel_message_new(channel, sdsnew(line));
        edc_actor_msg_t *actor_msg =
            edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, actor, NULL, msg,
                              (edc_actor_data_free_fn)edc_channel_message_free);
        edc_actor_send(state->observer, actor_msg);
    }

    sdsfree(state->cli.line_buffer);
    state->cli.line_buffer = sdsempty();
    state->cli.cursor_pos = 0;

    if (state->cli.saved_line) {
        sdsfree(state->cli.saved_line);
        state->cli.saved_line = NULL;
    }
    state->cli.history_pos = -1;

    if (state->cli.running)
        cli_redraw(state);
}

/* ---- history navigation via escape sequences ---- */

static void cli_history_up(edc_channel_actor_state_t *state)
{
    if (state->cli.history_count == 0 ||
        state->cli.history_pos >= state->cli.history_count - 1)
        return;

    if (state->cli.history_pos == -1) {
        if (state->cli.saved_line) sdsfree(state->cli.saved_line);
        state->cli.saved_line = sdsdup(state->cli.line_buffer);
    }

    state->cli.history_pos++;
    sdsfree(state->cli.line_buffer);
    state->cli.line_buffer =
        sdsdup(state->cli.history[state->cli.history_count - 1 - state->cli.history_pos]);
    state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
    cli_redraw(state);
}

static void cli_history_down(edc_channel_actor_state_t *state)
{
    if (state->cli.history_pos > 0) {
        state->cli.history_pos--;
        sdsfree(state->cli.line_buffer);
        state->cli.line_buffer =
            sdsdup(state->cli.history[state->cli.history_count - 1 - state->cli.history_pos]);
        state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
        cli_redraw(state);
    } else if (state->cli.history_pos == 0 && state->cli.saved_line) {
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

    for (ssize_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        /* escape sequences */
        if (c == '\x1b' && i + 1 < len) {
            i++;
            unsigned char c2 = (unsigned char)data[i];
            if (c2 == '[' || c2 == 'O') {
                char seq[16] = {0};
                seq[0] = (char)c2;
                int si = 1;
                while (i + 1 < len && si < 15) {
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
                else if (!strcmp(seq, "[C") || !strcmp(seq, "OC")) {
                    if (state->cli.cursor_pos < (int)sdslen(state->cli.line_buffer)) {
                        state->cli.cursor_pos++;
                        cli_redraw(state);
                    }
                } else if (!strcmp(seq, "[D") || !strcmp(seq, "OD")) {
                    if (state->cli.cursor_pos > 0) {
                        state->cli.cursor_pos--;
                        cli_redraw(state);
                    }
                } else if (!strcmp(seq, "[H") || !strcmp(seq, "OH") || !strcmp(seq, "[1~")) {
                    state->cli.cursor_pos = 0;
                    cli_redraw(state);
                } else if (!strcmp(seq, "[F") || !strcmp(seq, "OF") || !strcmp(seq, "[4~")) {
                    state->cli.cursor_pos = (int)sdslen(state->cli.line_buffer);
                    cli_redraw(state);
                } else if (!strcmp(seq, "[3~")) {
                    if (state->cli.cursor_pos < (int)sdslen(state->cli.line_buffer))
                        state->cli.line_buffer = cli_buf_delete(state, state->cli.cursor_pos, 1);
                    cli_redraw(state);
                }
            }
            continue;
        }

        /* enter */
        if (c == '\r' || c == '\n') {
            cli_commit_line(actor, state->cli.line_buffer);
            continue;
        }

        /* Ctrl+C */
        if (c == 3) {
            if (sdslen(state->cli.line_buffer) == 0) {
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
        if (c == 4) {
            if (sdslen(state->cli.line_buffer) == 0) {
                state->cli.running = 0;
                fprintf(stdout, "\r\n");
                cli_history_save(state);
                raise(SIGINT);
                return;
            }
            continue;
        }

        /* backspace — delete one full UTF-8 character before cursor */
        if (c == 127 || c == '\b') {
            if (state->cli.cursor_pos > 0) {
                int pos = state->cli.cursor_pos - 1;
                while (pos > 0 &&
                       ((unsigned char)state->cli.line_buffer[pos] & 0xC0) == 0x80)
                    pos--;
                int count = state->cli.cursor_pos - pos;
                state->cli.line_buffer = cli_buf_delete(state, pos, count);
                state->cli.cursor_pos = pos;
                cli_redraw(state);
            }
            continue;
        }

        /* Ctrl+W - delete word backwards */
        if (c == 23) {
            if (state->cli.cursor_pos > 0) {
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

        /* printable ASCII + UTF-8 continuation bytes */
        if (c >= 32) {
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
    if (nread < 0) {
        free(buf->base);
        state->cli.running = 0;
        if (!state->cli.tty_stopped) {
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

    if (!state->cli.running && !state->cli.tty_stopped) {
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

    if (strncmp(message->content, "[思考]:", strlen("[思考]:")) == 0) {
        state->cli.pending_output = sdscatprintf(sdsempty(), "%s%s%s",
                                                  CLI_DIM, message->content, CLI_RESET);
    } else {
        state->cli.pending_output = edc_markdown_render(message->content);
    }

    cli_display_output(state);
}

/* ---- actor handler ---- */

static void edc_channel_cli_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    switch (msg->type) {
    case BC_ACTOR_MSG_START: {
        edc_terminal_vt_enable();
        if (edc_terminal_raw_mode(STDIN_FILENO, &state->cli.orig_term) != 0) {
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
    state->cli.history_path = sdsempty();
    sds edc_home = edc_config_resolve_bc_home();
    state->cli.history_path = sdscatprintf(state->cli.history_path, "%s/.cli_history", edc_home);
    sdsfree(edc_home);
    state->cli.history_pos = -1;
    edc_channel_actor_add(channel_actor, edc_channel_cli, edc_channel_cli_handler);
}
