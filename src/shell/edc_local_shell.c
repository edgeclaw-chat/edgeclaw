#include "edc_local_shell.h"

#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_shell.h"
#include "edc_task.h"
#include "sds.h"
#include <cJSON.h>
#include <string.h>

/* The tool the command is run through. Named here because "which tool executes a
 * `!cmd`" is this module's decision, not the kernel's. */
#define EDC_LOCAL_SHELL_TOOL "exec_shell"

static const char *kUsage = "## `!` — run a shell command\n"
                            "\n"
                            "Give it something to run: `!git status`.\n"
                            "\n"
                            "The command becomes a task of its own — its own line in the sidebar, its own entry in "
                            "the group's history, and its output is that task's result. **No model is called**, and "
                            "nothing is sent to the LLM: it costs no tokens.\n"
                            "\n"
                            "- `!cmd` — run `cmd`\n"
                            "- `!!` — run the previous `!` command of this session again\n"
                            "- `\\!cmd` — send `!cmd` as ordinary input (a task description) instead\n";

/* Create the tool-only task for |cmd|. |raw| is the text the user typed (the turn's
 * user message); `session` owns the task. Returns NULL when the task could not be
 * created. */
static struct edc_task *spawn_command_task(struct edc_scheduler *sched, struct edc_session *session, sds raw,
                                           const char *cmd, edc_channel_type channel_type)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", cmd);

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = cmd;
    /* `raw` is borrowed by the turn (edc_turn_add_user_message copies it), so the caller
     * may free it as soon as this returns. */
    spec.input = raw;
    spec.channel_type = channel_type;
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    /* The whole mechanism: one seeded call instead of an LLM round. The drive then walks
     * the ordinary tool path (dispatch -> result into the turn -> settle). */
    spec.initial_tool_name = EDC_LOCAL_SHELL_TOOL;
    spec.initial_tool_args = args;

    struct edc_task *task = edc_scheduler_task_create(sched, session, &spec);
    cJSON_Delete(args);
    return task;
}

bool edc_local_shell_handle(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                            struct edc_channel_message *message)
{
    if (!message || !message->content || message->content[0] != '!')
        return false;

    sds raw = sdstrim(sdsnew(message->content), " \t\r\n");

    /* `\!`: the escape. Drop the backslash and send the rest down the ordinary input
     * path (it becomes a task description, as if `!` were never special). The message is
     * not ours to rewrite, so the swapped content lives on a shallow copy for the length
     * of the call -- edc_shell_execute only reads it. */
    if (sdslen(raw) >= 2 && raw[0] == '\\' && raw[1] == '!')
    {
        struct edc_channel_message plain = *message;
        plain.content = sdsnew(raw + 1);
        bool handled = edc_shell_execute(sm, sched, to, &plain);
        sdsfree(plain.content);
        sdsfree(raw);
        return handled;
    }

    struct edc_session *session = edc_session_store_resolve_session(sm, message->session_id);
    if (!session)
    {
        edc_commands_reply(to, "No session for this input — nothing to run the command in.");
        sdsfree(raw);
        return true;
    }

    /* `!` with nothing after it: say how to use it, create nothing. */
    const char *cmd = raw + 1;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (*cmd == '\0')
    {
        edc_commands_reply(to, kUsage);
        sdsfree(raw);
        return true;
    }

    /* `!!`: run this session's previous `!` command again. Kept in memory only (it is a
     * convenience, not state the session owns across a restart). */
    if (strcmp(cmd, "!") == 0)
    {
        if (!session->last_local_cmd || sdslen(session->last_local_cmd) == 0)
        {
            edc_commands_reply(to, "No previous `!` command in this session.");
            sdsfree(raw);
            return true;
        }
        sdsfree(raw);
        raw = sdscatprintf(sdsempty(), "!%s", session->last_local_cmd);
        cmd = raw + 1;
    }
    else
    {
        /* Remember it for `!!` (the command text, without the `!`). */
        sdsfree(session->last_local_cmd);
        session->last_local_cmd = sdsnew(cmd);
    }

    struct edc_task *task = spawn_command_task(sched, session, raw, cmd, message->type);
    if (!task)
        edc_commands_reply(to, "Could not create a task for this command.");
    else
        edc_commands_announce_task(to, task);
    sdsfree(raw);
    return true;
}
