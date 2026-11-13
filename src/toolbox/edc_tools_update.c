#include "edc_tools_update.h"

#include "edc_list.h"
#include "edc_param_types.h"
#include "edc_toolbox.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

/* `update_install` -- see the header for why this exists and why it is INTERNAL.
 *
 * The shape is the ordinary async tool one: exec starts the work and returns, and the
 * callback ends the call exactly once with a result or an error. What is unusual is the
 * latency: an upgrade fetches the installer, spawns it and waits for it to finish, with a
 * 120s ceiling of its own (edc_upgrade_run). None of that blocks the actor loop -- the
 * upgrade reports back through a uv_async -- which is the reason this is a tool and not
 * something the caller polls.
 */

struct update_call
{
    struct edc_toolbox_call_ctx *ctx;
    void (*callback)(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error);
    edc_update_install_fn start;
    void *start_ud;
};

static void on_upgrade_done(bool ok, const char *detail, void *ud)
{
    struct update_call *call = ud;
    if (!call)
        return;

    if (ok)
    {
        /* The task's own result. Deliberately says what the user has to DO next: an install
         * that finished changes nothing until the program is started again, and a result
         * saying only "done" would suggest otherwise. */
        sds msg = sdsnew("Installed. Quit and start EdgeClaw again to use the new version.");
        call->callback(call->ctx, msg, NULL);
    }
    else
    {
        /* The installer's own final output is the only diagnosis there is (a failed download,
         * a checksum mismatch, a refused platform), so it is passed through rather than
         * replaced with our own summary. */
        sds msg = sdscatprintf(sdsempty(), "Error: the update did not complete. %s",
                               (detail && detail[0]) ? detail : "The installer gave no output.");
        EdcError *err = edc_error_new(EDC_ERR_EXEC, "%s", msg);
        call->callback(call->ctx, msg, err);
    }
    free(call);
}

static void edc_tools_update_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                        void (*callback)(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error))
{
    (void)tool;

    struct update_call *call = tool ? tool->priv : NULL;
    if (!call || !call->start)
    {
        /* No upgrade path was injected: the toolbox was built without an app (tests), or this
         * build has no installer. An error, not a silent success. */
        sds msg = sdsnew("Error: updating is not available in this build.");
        callback(ctx, msg, edc_error_new(EDC_ERR_INVAL, "%s", msg));
        return;
    }

    cJSON *tag = args ? cJSON_GetObjectItem(args, "tag") : NULL;
    if (!tag || !cJSON_IsString(tag) || !tag->valuestring[0])
    {
        sds msg = sdsnew("Error: update_install needs a \"tag\" naming the release to install.");
        callback(ctx, msg, edc_error_new(EDC_ERR_INVAL, "%s", msg));
        return;
    }

    /* One record per call: the upgrade reports asynchronously, possibly long after this
     * returns. |ctx| stays valid until the callback runs -- the toolbox actor owns it and is
     * waiting for exactly that. */
    struct update_call *live = calloc(1, sizeof(*live));
    if (!live)
    {
        sds msg = sdsnew("Error: out of memory starting the update.");
        callback(ctx, msg, edc_error_new(EDC_ERR_FAILED, "%s", msg));
        return;
    }
    live->ctx = ctx;
    live->callback = callback;
    live->start = call->start;
    live->start_ud = call->start_ud;

    if (!live->start(live->start_ud, tag->valuestring, on_upgrade_done, live))
    {
        /* Refused before anything began: an upgrade is already running, or this copy cannot
         * replace itself (hand-installed). Neither is a crash, and both are worth saying. */
        free(live);
        sds msg = sdsnew("Error: could not start the update (one may already be running, or this copy "
                         "cannot replace itself).");
        callback(ctx, msg, edc_error_new(EDC_ERR_INVAL, "%s", msg));
    }
}

/* Frees the registration record. edc_tool_free() calls this, so the start pointer cannot
 * outlive the toolbox -- the same lifetime the header requires of the injected callback. */
static void update_registration_free(void *priv)
{
    free(priv);
}

void edc_tools_update_register(struct edc_toolbox *tb, edc_update_install_fn start, void *ud)
{
    if (!tb)
        return;

    struct update_call *call = calloc(1, sizeof(*call));
    if (!call)
        return;
    call->start = start;
    call->start_ud = ud;

    /* The description is written for a human reading a transcript, not for a model choosing
     * a tool: the model never sees it. It still matters, because the seeded call is recorded
     * in the task's turn and rendered in the history view. */
    const char *desc = "Replace this EdgeClaw binary with a published release.\n"
                       "\n"
                       "Internal. The user asks for this from the update dialog; no model chooses it, "
                       "which is why it is not in the tool list. The download and install run in the "
                       "background and this task reports when they finish.";

    struct edc_tool_param *params = NULL;
    EDC_LL_APPEND(struct edc_tool_param, params,
                  edc_tool_param_new("tag", edc_param_str_type("The release to install, e.g. v0.1.6", NULL), true));

    struct edc_tool *tool = edc_tool_new("update_install", desc, params, edc_tools_update_exec_async);
    tool->priv = call;
    tool->priv_free_fn = update_registration_free;
    tool->keep_full_output = true;

    /* INTERNAL scope, and NOT NORMAL: this is the gate rather than the invisibility. An
     * ordinary reply task resolves to EDC_TOOL_SCOPE_NORMAL, so edc_toolbox_exec refuses this
     * name even when the model somehow learns it -- and nothing a conversation does can
     * produce an EDC_SINK_INTERNAL task. */
    edc_tool_set_scope(tool, EDC_TOOL_SCOPE_INTERNAL);

    /* INTERNAL exposure as well: in the name table (a seeded call resolves through it) but in
     * no declared set, no prompt section and no "Available:" list. The two mechanisms cover
     * different failures: exposure keeps the name out of the model's context, scope keeps the
     * call from running if the name gets there anyway. */
    edc_toolbox_register_internal(tb, tool);
}
