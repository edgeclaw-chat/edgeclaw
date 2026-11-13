/* tests/test_update_tool.c -- `update_install`, the internal tool.
 *
 * The property under test is not "the tool works" but "the model cannot see it, and the
 * kernel still can". Those pull in opposite directions, and each one has its own way of
 * failing silently:
 *
 *   - if it leaks into the declared set, the model can offer to replace the user's binary;
 *   - if it leaks into the prompt's prose or an "Available:" error, the model learns the name
 *     and can call it by name -- the DEFERRED mechanism, which is exactly what this must not
 *     become;
 *   - if it is not in the name table at all, the seeded call (edc_task_spec.initial_tool_name)
 *     cannot resolve, and an update silently does nothing.
 *
 * The last one is why the lookup and exec assertions are here alongside the absence ones: a
 * test that only checks "not declared" would pass for a tool that does not exist.
 */

#include "edc_tools.h"
#include "edc_tools_update.h"
#include "edc_toolbox.h"
#include "edc_toolset.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: %s\n", msg);                                                                    \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: %s\n", msg);                                                                               \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

static struct edc_toolbox *make_toolbox(void)
{
    struct edc_toolbox *tb = (struct edc_toolbox *)calloc(1, sizeof(*tb));
    tb->schemas = cJSON_CreateArray();
    tb->memory_schemas = cJSON_CreateArray();
    return tb;
}

/* Does |arr| (an array of {"type":"function","function":{"name":...}} entries) name |want|? */
static bool array_names(cJSON *arr, const char *want)
{
    if (!arr)
        return false;
    for (int i = 0; i < cJSON_GetArraySize(arr); i++)
    {
        cJSON *entry = cJSON_GetArrayItem(arr, i);
        cJSON *fn = entry ? cJSON_GetObjectItem(entry, "function") : NULL;
        cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
        if (name && cJSON_IsString(name) && strcmp(name->valuestring, want) == 0)
            return true;
    }
    return false;
}

/* The injected upgrade, faked: records the tag and optionally refuses. */
struct fake_upgrade
{
    int started;
    char tag[64];
    bool refuse;
    /* When set, completes immediately (through the observer) instead of waiting. */
    bool complete_now;
    bool complete_ok;
};

static struct fake_upgrade *g_upgrade = NULL;

static bool fake_start(void *start_ud, const char *tag, void (*on_done)(bool, const char *, void *), void *done_ud)
{
    struct fake_upgrade *f = (struct fake_upgrade *)start_ud;
    if (!f || f->refuse)
        return false;
    f->started++;
    snprintf(f->tag, sizeof(f->tag), "%s", tag ? tag : "");
    if (f->complete_now && on_done)
        on_done(f->complete_ok, f->complete_ok ? "" : "installer said: checksum mismatch", done_ud);
    return true;
}

/* The exec callback records what the tool reported. */
struct exec_outcome
{
    int calls;
    char result[512];
    bool is_error;
};

/* Declared before its user: on_exec_done writes through it, and the pointer is only ever
 * set for the duration of one case, so a stale value cannot outlive the assertion. */
static struct exec_outcome *g_exec_outcome = NULL;

static void on_exec_done(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    (void)ctx;
    struct exec_outcome *o = g_exec_outcome;
    if (!o)
    {
        if (result)
            sdsfree(result);
        if (error)
            edc_error_free(error);
        return;
    }
    o->calls++;
    snprintf(o->result, sizeof(o->result), "%s", result ? result : "");
    o->is_error = error != NULL;
    if (error)
        edc_error_free(error);
}

int main(void)
{
    printf("\ntest_update_tool\n");

    printf("\n── registered, but not declared ──\n");
    {
        struct edc_toolbox *tb = make_toolbox();
        struct fake_upgrade up = {0};
        edc_tools_update_register(tb, fake_start, &up);

        /* Present in the name table: the seeded call resolves through this. */
        struct edc_tool *found = NULL;
        HASH_FIND_STR(tb->tools, "update_install", found);
        CHECK(found != NULL, "the tool is in the name table (a seeded call can resolve it)");
        CHECK(found && found->exposure == EDC_TOOL_EXPOSURE_INTERNAL, "its exposure is INTERNAL");
        CHECK(found && found->snippet == NULL, "and it carries no prompt snippet");

        /* Absent from the process-wide declaration set: this one IS the request's `tools`
         * field whenever a session has no group to freeze against. */
        CHECK(!array_names(tb->schemas, "update_install"), "not in tb->schemas (the tools field fallback)");

        /* Absent from a declared set resolved for an ordinary task. */
        cJSON *decl = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
        CHECK(!array_names(decl, "update_install"), "not in an ordinary task's declared set");
        cJSON_Delete(decl);

        /* Absent from the prompt prose -- the DEFERRED path names tools there, and a model
         * that reads the name can call it. */
        sds prompt = edc_toolset_render_prompt(tb, EDC_TOOL_SCOPE_NORMAL, NULL);
        CHECK(prompt == NULL || strstr(prompt, "update_install") == NULL, "not named in the system prompt");
        if (prompt)
            sdsfree(prompt);

        /* Other tools are unaffected: the guard is specific, not a blanket filter. */
        struct edc_tool *other = edc_tool_new("read_file", "stub", NULL, NULL);
        edc_toolbox_register(tb, other);
        CHECK(array_names(tb->schemas, "read_file"), "a normally registered tool IS still declared");

        edc_toolbox_free(tb);
    }

    printf("\n── the refusal message must not name it either ──\n");
    {
        /* edc_toolbox_exec answers a scope rejection with "Available: <names>", which is a
         * tool RESULT -- prompt content. Listing an internal tool there would hand the model
         * the name it is not supposed to know. */
        struct edc_toolbox *tb = make_toolbox();
        struct fake_upgrade up = {0};
        edc_tools_update_register(tb, fake_start, &up);

        struct exec_outcome out = {0};
        g_exec_outcome = &out;

        struct edc_toolbox_call_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.tb = tb;
        /* No task: scope resolves to NORMAL, and this tool declares NORMAL, so the call
         * proceeds -- what is being checked is the OTHER branch, where a tool of another
         * scope is refused and the available names are listed. */
        edc_toolbox_exec(&ctx, "nonexistent_tool", NULL, on_exec_done);
        CHECK(out.calls == 1, "an unknown tool reports once");
        CHECK(out.is_error, "and it is an error");
        CHECK(strstr(out.result, "update_install") == NULL, "whose list of available tools omits update_install");

        g_exec_outcome = NULL;
        edc_toolbox_free(tb);
    }

    printf("\n── the kernel can still run it ──\n");
    {
        struct edc_toolbox *tb = make_toolbox();
        struct fake_upgrade up = {0};
        up.complete_ok = true;
        up.complete_now = true;
        edc_tools_update_register(tb, fake_start, &up);

        struct exec_outcome out = {0};
        g_exec_outcome = &out;

        struct edc_toolbox_call_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.tb = tb;

        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "tag", "v9.9.9");
        edc_toolbox_exec(&ctx, "update_install", args, on_exec_done);
        cJSON_Delete(args);

        CHECK(out.calls == 1, "the call completes exactly once");
        CHECK(up.started == 1, "the injected upgrade was started");
        CHECK(strcmp(up.tag, "v9.9.9") == 0, "with the requested tag");
        CHECK(!out.is_error, "a successful install is not an error");
        CHECK(strstr(out.result, "start EdgeClaw again") != NULL,
              "and the result tells the user the change takes effect on restart");

        g_exec_outcome = NULL;
        edc_toolbox_free(tb);
    }

    printf("\n── failures are reported, not swallowed ──\n");
    {
        struct edc_toolbox *tb = make_toolbox();
        struct fake_upgrade up = {0};
        up.refuse = true;
        edc_tools_update_register(tb, fake_start, &up);

        struct exec_outcome out = {0};
        g_exec_outcome = &out;

        struct edc_toolbox_call_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.tb = tb;
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "tag", "v9.9.9");
        edc_toolbox_exec(&ctx, "update_install", args, on_exec_done);
        cJSON_Delete(args);

        CHECK(out.calls == 1, "a refusal reports once");
        CHECK(out.is_error, "and is an error, so the task fails rather than claiming success");

        g_exec_outcome = NULL;
        edc_toolbox_free(tb);
    }

    printf("\n── the installer's own output reaches the result ──\n");
    {
        struct edc_toolbox *tb = make_toolbox();
        struct fake_upgrade up = {0};
        up.complete_now = true;
        up.complete_ok = false; /* fails, with the installer's detail */
        edc_tools_update_register(tb, fake_start, &up);

        struct exec_outcome out = {0};
        g_exec_outcome = &out;

        struct edc_toolbox_call_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.tb = tb;
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "tag", "v9.9.9");
        edc_toolbox_exec(&ctx, "update_install", args, on_exec_done);
        cJSON_Delete(args);

        CHECK(out.calls == 1, "the failure reports once");
        CHECK(out.is_error, "as an error");
        CHECK(strstr(out.result, "checksum mismatch") != NULL,
              "carrying the installer's own message -- the only real diagnosis there is");

        g_exec_outcome = NULL;
        edc_toolbox_free(tb);
    }

    printf("\n── no injected upgrade: an error, not a silent success ──\n");
    {
        struct edc_toolbox *tb = make_toolbox();
        edc_tools_update_register(tb, NULL, NULL);

        struct exec_outcome out = {0};
        g_exec_outcome = &out;

        struct edc_toolbox_call_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.tb = tb;
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "tag", "v9.9.9");
        edc_toolbox_exec(&ctx, "update_install", args, on_exec_done);
        cJSON_Delete(args);

        CHECK(out.calls == 1, "it still reports");
        CHECK(out.is_error, "and refuses instead of pretending to have installed something");

        g_exec_outcome = NULL;
        edc_toolbox_free(tb);
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
