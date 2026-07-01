#include "sds.h"
#include "edc_channel_actor.h"
#include "edc_channel_cli.h"
#include "edc_markdown_render.h"
#include "edc_config.h"
#include "linenoise.h"
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CLI_RESET "\e[0m"
#define CLI_DIM "\e[2m"

static void cli_sigusr1_handler(int sig)
{
    (void)sig;
}

static bool cli_process_input(void* data)
{
    edc_actor_t *actor = data;
    edc_channel_actor_state_t *state = actor->state;
    char *line = g_async_queue_try_pop(state->cli.input_queue);

    if (!line)
        return G_SOURCE_CONTINUE;

    struct edc_channel *channel = edc_channel_cli_new();
    struct edc_channel_message *msg = edc_channel_message_new(channel, line);

    edc_actor_msg_t *actor_msg =
        edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, actor, NULL, msg, (edc_actor_data_free_fn)edc_channel_message_free);
    edc_actor_send(state->observer, actor_msg);

    return G_SOURCE_CONTINUE;
}

static void* cli_input_thread(void* data)
{
    edc_actor_t *actor = data;
    edc_channel_actor_state_t *state = actor->state;

    state->cli.input_tid = pthread_self();

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(1000);

    if (state->cli.history_path)
        linenoiseHistoryLoad(state->cli.history_path);

    while (g_atomic_int_get(&state->cli.running))
    {
        char *line = linenoise("\e[1;36m> \e[0m");

        pthread_mutex_lock(&state->cli.term_mutex);
        if (state->cli.pending_output)
        {
            printf("\r\e[K%s\n", state->cli.pending_output);
            fflush(stdout);
            sdsfree(state->cli.pending_output);
            state->cli.pending_output = NULL;
        }
        pthread_mutex_unlock(&state->cli.term_mutex);

        if (!line)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        sds trimmed = sdsnew(line);
        trimmed = sdstrim(trimmed, " \t\r\n");
        free(line);

        if (trimmed[0] != '\0')
        {
            linenoiseHistoryAdd(trimmed);

            g_async_queue_push(state->cli.input_queue, trimmed);

            g_main_context_invoke(state->cli.queue_source ? g_source_get_context(state->cli.queue_source) : NULL,
                                  (GSourceFunc)cli_process_input, actor);
        }
        else
        {
            sdsfree(trimmed);
        }
    }

    if (state->cli.history_path)
        linenoiseHistorySave(state->cli.history_path);

    /* Wake up main loop if we exited due to EOF/Ctrl+C (not /stop) */
    if (g_atomic_int_get(&state->cli.running))
        raise(SIGINT);

    return NULL;
}

static void edc_channel_cli_handle_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;
    struct edc_channel_message *message = msg->data;

    pthread_mutex_lock(&state->cli.term_mutex);
    if (state->cli.pending_output)
    {
        sdsfree(state->cli.pending_output);
    }
    if (strncmp(message->content, "[思考]:", strlen("[思考]:")) == 0)
    {
        state->cli.pending_output = sdscatprintf(sdsempty(), "%s%s%s", CLI_DIM, message->content, CLI_RESET);
    }
    else
    {
        state->cli.pending_output = edc_markdown_render(message->content);
    }
    pthread_mutex_unlock(&state->cli.term_mutex);

    if (state->cli.input_thread)
        pthread_kill(state->cli.input_tid, SIGUSR1);
}

static void edc_channel_cli_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_START: {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = cli_sigusr1_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);

        tcgetattr(STDIN_FILENO, &state->cli.orig_term);

        state->cli.input_queue = g_async_queue_new();
        state->cli.pending_output = NULL;
        state->cli.running = TRUE;

        if (pthread_mutex_init(&state->cli.term_mutex, NULL) != 0)
        {
            printf("Failed to init CLI mutex\r\n");
            state->cli.running = FALSE;
            break;
        }

        state->cli.queue_source = g_timeout_source_new(50);
        g_source_set_callback(state->cli.queue_source, (GSourceFunc)cli_process_input, actor, NULL);
        g_source_attach(state->cli.queue_source, actor->context);

        state->cli.input_thread = g_thread_new("cli_input", (GThreadFunc)cli_input_thread, actor);

        printf("\e[1;32mEdgeClaw CLI ready. Type /help for commands.\e[0m\n");
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
    char *edc_home = edc_config_resolve_bc_home();
    state->cli.history_path = sdscatprintf(state->cli.history_path, "%s/.cli_history", edc_home);
    sdsfree(edc_home);
    edc_channel_actor_add(channel_actor, edc_channel_cli, edc_channel_cli_handler);
}
