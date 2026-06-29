#include "sds.h"
#include "edc_channel_actor.h"
#include "edc_channel_cli.h"
#include "edc_markdown_render.h"
#include "edc_config.h"
#include "linenoise.h"
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

static void cli_process_input(uv_async_t *handle)
{
    edc_channel_actor_state_t *state = container_of(handle, edc_channel_actor_state_t, cli.input_wakeup);
    edc_actor_t *actor = state->actor;

    char *line;
    uv_mutex_lock(&state->cli.input_queue_lock);
    while ((line = edc_queue_pop(&state->cli.input_queue)) != NULL)
    {
        uv_mutex_unlock(&state->cli.input_queue_lock);

        struct edc_channel *channel = edc_channel_cli_new();
        struct edc_channel_message *msg = edc_channel_message_new(channel, line);

        edc_actor_msg_t *actor_msg =
            edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, actor, NULL, msg,
                              (edc_actor_data_free_fn)edc_channel_message_free);
        edc_actor_send(state->observer, actor_msg);

        uv_mutex_lock(&state->cli.input_queue_lock);
    }
    uv_mutex_unlock(&state->cli.input_queue_lock);
}

static void cli_input_thread(void* data)
{
    edc_channel_actor_state_t *state = data;

    state->cli.input_tid = pthread_self();

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(1000);

    if (state->cli.history_path)
        linenoiseHistoryLoad(state->cli.history_path);

    while (state->cli.running)
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

            uv_mutex_lock(&state->cli.input_queue_lock);
            edc_queue_push(&state->cli.input_queue, trimmed);
            uv_mutex_unlock(&state->cli.input_queue_lock);

            uv_async_send(&state->cli.input_wakeup);
        }
        else
        {
            sdsfree(trimmed);
        }
    }

    if (state->cli.history_path)
        linenoiseHistorySave(state->cli.history_path);

    /* Wake up main loop if we exited due to EOF/Ctrl+C (not /stop) */
    if (state->cli.running)
        raise(SIGINT);
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

    if (state->cli.running)
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

        edc_terminal_save(STDIN_FILENO, &state->cli.orig_term);

        edc_queue_init(&state->cli.input_queue);
        uv_mutex_init(&state->cli.input_queue_lock);
        state->cli.pending_output = NULL;
        state->cli.running = 1;

        if (pthread_mutex_init(&state->cli.term_mutex, NULL) != 0)
        {
            printf("Failed to init CLI mutex\r\n");
            state->cli.running = 0;
            break;
        }

        uv_async_init(actor->loop, &state->cli.input_wakeup, cli_process_input);

        uv_thread_create(&state->cli.input_thread, cli_input_thread, state);

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
    sds edc_home = edc_config_resolve_bc_home();
    state->cli.history_path = sdscatprintf(state->cli.history_path, "%s/.cli_history", edc_home);
    sdsfree(edc_home);
    edc_channel_actor_add(channel_actor, edc_channel_cli, edc_channel_cli_handler);
}
