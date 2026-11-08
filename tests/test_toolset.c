/* tests/test_toolset.c -- the declaration set (src/toolbox/edc_toolset.c).
 *
 * The declaration set decides what the model can SEE, and it lands in the request's `tools`
 * field -- i.e. inside the provider's cached prefix. So the two properties worth pinning are
 * (a) the output is deterministic regardless of registration order, and (b) only a removal or
 * a same-name redefinition flips has_non_additive, which is what makes the request recompute
 * the field (and cost one cache miss). */

#include "edc_toolset.h"
#include "test_ws.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "edc_task_group.h"
#include "edc_tools_fs.h"
#include "edc_file_utils.h"
#include "edc_tools_search.h"

#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_fs_util.h"

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

/* A toolbox with an empty registry: enough for the declaration set, which only reads
 * `tb->tools` (schemas are built here because edc_toolbox_register appends to them). */
static struct edc_toolbox *bare_toolbox(void)
{
    struct edc_toolbox *tb = (struct edc_toolbox *)calloc(1, sizeof(*tb));
    tb->schemas = cJSON_CreateArray();
    tb->memory_schemas = cJSON_CreateArray();
    return tb;
}

static void add_tool(struct edc_toolbox *tb, const char *name, uint32_t scope, uint8_t exposure)
{
    struct edc_tool *tool = edc_tool_new(name, "test description", NULL, NULL);
    edc_tool_set_scope(tool, scope);
    edc_tool_set_exposure(tool, exposure);
    edc_toolbox_register(tb, tool);
}

/* "alpha,beta,gamma" -- the names in the order the array carries them. */
static sds names_of(cJSON *arr)
{
    sds out = sdsempty();
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, arr)
    {
        cJSON *fn = cJSON_GetObjectItem(entry, "function");
        cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : cJSON_GetObjectItem(entry, "name");
        if (sdslen(out) > 0)
            out = sdscat(out, ",");
        out = sdscat(out, name && cJSON_IsString(name) ? name->valuestring : "?");
    }
    return out;
}

static cJSON *delta_message(const char *removed_name, cJSON *added_entry)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "system");
    cJSON_AddStringToObject(msg, "content", "Tool set changed (test).");
    if (removed_name)
    {
        cJSON *removed = cJSON_AddArrayToObject(msg, "tools_removed");
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", removed_name);
        cJSON_AddItemToArray(removed, entry);
    }
    if (added_entry)
    {
        cJSON *added = cJSON_AddArrayToObject(msg, "tools_added");
        cJSON_AddItemToArray(added, cJSON_Duplicate(added_entry, 1));
    }
    return msg;
}

static cJSON *schema_entry(const char *name, const char *desc)
{
    cJSON *function = cJSON_CreateObject();
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", desc);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type", "function");
    cJSON_AddItemToObject(entry, "function", function);
    return entry;
}

static void test_initial_is_sorted_and_scoped(void)
{
    TEST("initial: sorted by name, MEMORY and DEFERRED left out");
    struct edc_toolbox *tb = bare_toolbox();
    /* Registered out of order on purpose. */
    add_tool(tb, "gamma", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "memory_list", EDC_TOOL_SCOPE_MEMORY, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "mcp__x__y", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DEFERRED);
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *decl = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    sds names = names_of(decl);
    CHECK(strcmp(names, "alpha,beta,gamma") == 0, "normal scope = %s (want alpha,beta,gamma)", names);
    sdsfree(names);
    cJSON_Delete(decl);

    cJSON *mem = edc_toolset_initial(tb, EDC_TOOL_SCOPE_MEMORY);
    sds mem_names = names_of(mem);
    CHECK(strcmp(mem_names, "memory_list") == 0, "memory scope = %s (want memory_list)", mem_names);
    sdsfree(mem_names);
    cJSON_Delete(mem);

    edc_toolbox_free(tb);
}

static void test_initial_is_registration_order_independent(void)
{
    TEST("initial: two registries with the same tools print byte-identically");
    struct edc_toolbox *a = bare_toolbox();
    add_tool(a, "gamma", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(a, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(a, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    struct edc_toolbox *b = bare_toolbox();
    add_tool(b, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(b, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(b, "gamma", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *da = edc_toolset_initial(a, EDC_TOOL_SCOPE_NORMAL);
    cJSON *db = edc_toolset_initial(b, EDC_TOOL_SCOPE_NORMAL);
    char *sa = cJSON_PrintUnformatted(da);
    char *sb = cJSON_PrintUnformatted(db);
    CHECK(strcmp(sa, sb) == 0, "the same registry in another order must produce the same bytes");
    cJSON_free(sa);
    cJSON_free(sb);
    cJSON_Delete(da);
    cJSON_Delete(db);
    edc_toolbox_free(a);
    edc_toolbox_free(b);
}

static void test_replay_without_deltas_is_identity(void)
{
    TEST("replay: no deltas -> the initial set, unchanged and additive");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *initial = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    cJSON *history = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "hi");
    cJSON_AddItemToArray(history, msg);

    bool non_additive = true; /* must be cleared by the call */
    cJSON *cur = edc_toolset_replay(initial, history, &non_additive);
    char *a = cJSON_PrintUnformatted(initial);
    char *b = cJSON_PrintUnformatted(cur);
    CHECK(strcmp(a, b) == 0, "no delta -> identical bytes");
    CHECK(!non_additive, "no removal/redefinition -> additive");

    cJSON_free(a);
    cJSON_free(b);
    cJSON_Delete(cur);
    cJSON_Delete(initial);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_removal_forces_recompute(void)
{
    TEST("replay: a removal drops the tool and is non-additive");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *initial = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    cJSON *history = cJSON_CreateArray();
    cJSON_AddItemToArray(history, delta_message("beta", NULL));

    bool non_additive = false;
    cJSON *cur = edc_toolset_replay(initial, history, &non_additive);
    sds names = names_of(cur);
    CHECK(strcmp(names, "alpha") == 0, "beta removed, got %s", names);
    CHECK(non_additive, "a removal must force the request to recompute the field");
    sdsfree(names);

    cJSON_Delete(cur);
    cJSON_Delete(initial);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_pure_addition_keeps_the_field_frozen(void)
{
    TEST("replay: a new tool appends (sorted) and stays additive");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *initial = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    cJSON *history = cJSON_CreateArray();
    cJSON *added = schema_entry("delta", "a tool that appeared mid-session");
    cJSON_AddItemToArray(history, delta_message(NULL, added));
    cJSON_Delete(added);

    bool non_additive = true;
    cJSON *cur = edc_toolset_replay(initial, history, &non_additive);
    sds names = names_of(cur);
    CHECK(strcmp(names, "alpha,delta") == 0, "delta appended in name order, got %s", names);
    /* Additive: the request may keep sending the frozen initial set and let the delta
     * message tell the model about the new tool. */
    CHECK(!non_additive, "a brand-new tool must NOT invalidate the cached prefix");
    sdsfree(names);

    cJSON_Delete(cur);
    cJSON_Delete(initial);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_redefinition_is_non_additive(void)
{
    TEST("replay: re-declaring the same name replaces it and is non-additive");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *initial = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    cJSON *history = cJSON_CreateArray();
    cJSON *redef = schema_entry("alpha", "the new signature");
    cJSON_AddItemToArray(history, delta_message(NULL, redef));
    cJSON_Delete(redef);

    bool non_additive = false;
    cJSON *cur = edc_toolset_replay(initial, history, &non_additive);
    sds names = names_of(cur);
    CHECK(strcmp(names, "alpha") == 0, "still one alpha, got %s", names);
    CHECK(non_additive, "a same-name redefinition changes an entry already in the prefix");
    char *printed = cJSON_PrintUnformatted(cur);
    CHECK(strstr(printed, "the new signature") != NULL, "the newer declaration wins");
    cJSON_free(printed);
    sdsfree(names);

    cJSON_Delete(cur);
    cJSON_Delete(initial);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_replay_is_stable_when_reapplied(void)
{
    TEST("replay: replaying the same history over its own result changes nothing");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    cJSON *initial = edc_toolset_initial(tb, EDC_TOOL_SCOPE_NORMAL);
    cJSON *history = cJSON_CreateArray();
    cJSON *added = schema_entry("delta", "appeared");
    cJSON_AddItemToArray(history, delta_message(NULL, added));
    cJSON_Delete(added);

    cJSON *once = edc_toolset_replay(initial, history, NULL);
    cJSON *twice = edc_toolset_replay(once, history, NULL);
    char *a = cJSON_PrintUnformatted(once);
    char *b = cJSON_PrintUnformatted(twice);
    CHECK(strcmp(a, b) == 0, "replay must be idempotent");
    cJSON_free(a);
    cJSON_free(b);

    cJSON_Delete(once);
    cJSON_Delete(twice);
    cJSON_Delete(initial);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_resolve_always_reflects_the_current_set(void)
{
    TEST("resolve: the field is the contract -- additions, removals and deferred all follow");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "mcp__x", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DEFERRED);

    /* A new DIRECT tool enters the field: it is the only place the model gets its schema,
     * so "known by name only" would not be the same thing. */
    cJSON *history = cJSON_CreateArray();
    cJSON *added = schema_entry("beta", "appeared mid-session");
    cJSON_AddItemToArray(history, delta_message(NULL, added));
    cJSON_Delete(added);

    cJSON *decl = edc_toolset_resolve(tb, history, EDC_TOOL_SCOPE_NORMAL);
    sds names = names_of(decl);
    CHECK(strcmp(names, "alpha,beta") == 0, "an added tool IS declared, got %s", names);
    sdsfree(names);
    cJSON_Delete(decl);

    /* A removal leaves the field. */
    cJSON *history2 = cJSON_CreateArray();
    cJSON_AddItemToArray(history2, delta_message("alpha", NULL));
    cJSON *decl2 = edc_toolset_resolve(tb, history2, EDC_TOOL_SCOPE_NORMAL);
    CHECK(cJSON_GetArraySize(decl2) == 0, "the removed tool is gone from the field");
    cJSON_Delete(decl2);

    /* A DEFERRED tool never enters the field, even when a delta carries its schema --
     * that is the whole point of deferred: named in prose, called by name, not declared. */
    cJSON *history3 = cJSON_CreateArray();
    cJSON *deferred_entry = schema_entry("mcp__x", "a deferred tool");
    cJSON_AddItemToArray(history3, delta_message(NULL, deferred_entry));
    cJSON_Delete(deferred_entry);
    cJSON *decl3 = edc_toolset_resolve(tb, history3, EDC_TOOL_SCOPE_NORMAL);
    sds names3 = names_of(decl3);
    CHECK(strcmp(names3, "alpha") == 0, "a deferred tool is filtered out of the field, got %s", names3);
    sdsfree(names3);
    cJSON_Delete(decl3);

    cJSON_Delete(history3);
    cJSON_Delete(history2);
    cJSON_Delete(history);
    edc_toolbox_free(tb);
}

static void test_render_prompt_is_terse_and_ruleful(void)
{
    TEST("render_prompt: snippet lines + the rules a schema cannot carry");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "exec_shell", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "memory_write", EDC_TOOL_SCOPE_NORMAL | EDC_TOOL_SCOPE_MEMORY, EDC_TOOL_EXPOSURE_DIRECT);
    /* Give the shell tool the data a real registration carries. */
    struct edc_tool *shell = NULL;
    HASH_FIND_STR(tb->tools, "exec_shell", shell);
    edc_tool_set_snippet(shell, "Run a shell command locally");
    edc_tool_set_guidelines(shell, "Output is capped.");
    struct edc_tool *mem = NULL;
    HASH_FIND_STR(tb->tools, "memory_write", mem);
    edc_tool_set_snippet(mem, "Record a durable note");

    /* 一个 deferred 工具：它不进 tools 字段，所以散文段是模型唯一能知道它存在的途径。 */
    add_tool(tb, "mcp__fs", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DEFERRED);
    struct edc_tool *deferred_tool = NULL;
    HASH_FIND_STR(tb->tools, "mcp__fs", deferred_tool);
    edc_tool_set_snippet(deferred_tool, "Read a file through the filesystem server");

    sds prose = edc_toolset_render_prompt(tb, EDC_TOOL_SCOPE_NORMAL, NULL);
    CHECK(strstr(prose, "<tools>") != NULL && strstr(prose, "</tools>") != NULL,
          "section is tagged (a later copy supersedes it)");
    CHECK(strstr(prose, "Also callable by name") != NULL, "deferred tools are listed by name");
    CHECK(strstr(prose, "- mcp__fs: Read a file through the filesystem server") != NULL,
          "deferred tool carries its snippet");
    CHECK(strstr(prose, "## Tools") != NULL, "has a tool section");
    CHECK(strstr(prose, "- exec_shell: Run a shell command locally") != NULL, "snippet line for exec_shell");
    CHECK(strstr(prose, "- memory_write: Record a durable note") != NULL, "memory tools are declared too");
    CHECK(strstr(prose, "Rules:") != NULL, "has a rules block");
    /* With a shell but no grep/find, the prose has to say so. */
    CHECK(strstr(prose, "Use exec_shell for searching") != NULL, "the no-grep/find rule is present");
    CHECK(strstr(prose, "exec_shell: Output is capped.") != NULL, "a tool's guideline reaches the prompt");
    /* The schemas are NOT repeated here -- that is the `tools` field's job. */
    CHECK(strstr(prose, "parameters") == NULL && strstr(prose, "type\":") == NULL,
          "no schema restatement in the prose");

    sds again = edc_toolset_render_prompt(tb, EDC_TOOL_SCOPE_NORMAL, NULL);
    CHECK(strcmp(prose, again) == 0, "two renderings are byte-identical (it lives in the cached prefix)");
    sdsfree(again);

    /* A scope that declares nothing renders nothing: no empty section in the prompt. */
    sds empty = edc_toolset_render_prompt(tb, 0, NULL);
    CHECK(sdslen(empty) == 0, "no declared tools -> empty string, not an empty heading");
    sdsfree(empty);

    sdsfree(prose);
    edc_toolbox_free(tb);
}

static void test_note_change_adding_a_direct_tool_rewrites_the_field(void)
{
    TEST("note_change: adding a direct tool rewrites the field (one cache reset)");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(*group));
    group->group_id = edc_test_id("g1");
    /* The group is built by hand, so it has no session: the history starts empty and the
     * tool set is resolved against nothing (same as a fresh group). */
    group->tools_decl = edc_toolset_resolve(tb, NULL, EDC_TOOL_SCOPE_NORMAL);
    char *before = cJSON_PrintUnformatted(group->tools_decl);
    const uint32_t epoch_before = group->toolset_epoch;

    cJSON *added = cJSON_CreateArray();
    cJSON_AddItemToArray(added, schema_entry("beta", "appeared"));
    CHECK(edc_toolset_note_change(tb, group, added, NULL, 0, "test:added"), "records the change");
    cJSON_Delete(added);

    char *after = cJSON_PrintUnformatted(group->tools_decl);
    CHECK(strcmp(before, after) != 0, "the field now carries the new tool");
    CHECK(group->toolset_epoch == epoch_before + 1, "a declared-set change is one cache reset");
    CHECK(group->tool_deltas && cJSON_GetArraySize(group->tool_deltas) == 1, "notice kept for rebuilds");
    CHECK(edc_task_group_history_count(group) == 1, "the next request already sees the notice");
    {
        /* The notice lives in the history as an in-memory slot; read it back the way a
         * request would. */
        cJSON *notice = edc_task_group_history_at(group, 0);
        char *printed = notice ? cJSON_PrintUnformatted(notice) : NULL;
        CHECK(printed != NULL, "the notice is readable from the history");
        CHECK(printed && strstr(printed, "Tool set changed") != NULL, "the model is told, in prose");
        CHECK(printed && strstr(printed, "test:added") != NULL, "the reason is in the notice");
        if (printed)
            cJSON_free(printed);
        if (notice)
            cJSON_Delete(notice);
    }

    cJSON_free(before);
    cJSON_free(after);
    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

static void test_note_change_adding_a_deferred_tool_keeps_the_bytes(void)
{
    TEST("note_change: adding a deferred tool leaves the field byte-identical");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "mcp__fs", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DEFERRED);

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(*group));
    group->group_id = edc_test_id("g1b");
    /* Hand-built group with no session: the history starts empty. */
    group->tools_decl = edc_toolset_resolve(tb, NULL, EDC_TOOL_SCOPE_NORMAL);
    char *before = cJSON_PrintUnformatted(group->tools_decl);
    const uint32_t epoch_before = group->toolset_epoch;

    cJSON *added = cJSON_CreateArray();
    cJSON_AddItemToArray(added, schema_entry("mcp__fs", "connected"));
    CHECK(edc_toolset_note_change(tb, group, added, NULL, 0, "mcp:connected:fs"), "records the change");
    cJSON_Delete(added);

    char *after = cJSON_PrintUnformatted(group->tools_decl);
    CHECK(strcmp(before, after) == 0, "deferred stays out of the field -> the prefix is kept");
    CHECK(group->toolset_epoch == epoch_before, "no cache reset for a deferred tool");
    CHECK(group->tool_deltas && cJSON_GetArraySize(group->tool_deltas) == 1, "the model still hears about it");

    cJSON_free(before);
    cJSON_free(after);
    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

static void test_note_change_removal_rewrites_the_field(void)
{
    TEST("note_change: a removal rewrites the field and bumps the epoch");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(*group));
    group->group_id = edc_test_id("g2");
    /* Hand-built group with no session: the history starts empty. */
    group->tools_decl = edc_toolset_resolve(tb, NULL, EDC_TOOL_SCOPE_NORMAL);
    const uint32_t epoch_before = group->toolset_epoch;

    const char *removed[] = {"beta"};
    CHECK(edc_toolset_note_change(tb, group, NULL, removed, 1, "test:removed"), "records the removal");

    sds names = names_of(group->tools_decl);
    CHECK(strcmp(names, "alpha") == 0, "the field no longer declares beta, got %s", names);
    sdsfree(names);
    CHECK(group->toolset_epoch == epoch_before + 1, "a removal is a real change -> one cache reset");
    {
        cJSON *notice = edc_task_group_history_at(group, 0);
        char *printed = notice ? cJSON_PrintUnformatted(notice) : NULL;
        CHECK(printed && strstr(printed, "no longer callable") != NULL, "the notice says what is gone");
        if (printed)
            cJSON_free(printed);
        if (notice)
            cJSON_Delete(notice);
    }

    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

static void test_notices_survive_a_history_rebuild(void)
{
    TEST("note_change: the notice survives rebuild_history (fold landing / restart path)");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(struct edc_task_group));
    group->group_id = edc_test_id("g3");
    const char *removed[] = {"ghost"};
    edc_toolset_note_change(tb, group, NULL, removed, 1, "test:rebuild");

    /* history is derived: a rebuild (fold landing / restart) recreates it from the turns. */
    edc_task_group_rebuild_history(group);
    {
        cJSON *notice = edc_task_group_history_at(group, edc_task_group_history_count(group) - 1);
        char *printed = notice ? cJSON_PrintUnformatted(notice) : NULL;
        CHECK(printed && strstr(printed, "Tool set changed") != NULL, "the notice is re-appended, not lost");
        if (printed)
            cJSON_free(printed);
        if (notice)
            cJSON_Delete(notice);
    }

    /* A second rebuild must not accumulate copies. */
    edc_task_group_rebuild_history(group);
    int notices = 0;
    for (int i = 0; i < edc_task_group_history_count(group); i++)
    {
        cJSON *msg = edc_task_group_history_at(group, i);
        if (msg && cJSON_GetObjectItem(msg, "toolset_reason"))
            notices++;
        if (msg)
            cJSON_Delete(msg);
    }
    CHECK(notices == 1, "exactly one notice after two rebuilds, got %d", notices);

    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

static void test_note_change_appends_the_current_section(void)
{
    TEST("note_change: the notice carries the current <tools> section, and history[0] is untouched");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    struct edc_tool *alpha = NULL, *beta = NULL;
    HASH_FIND_STR(tb->tools, "alpha", alpha);
    HASH_FIND_STR(tb->tools, "beta", beta);
    edc_tool_set_snippet(alpha, "Alpha one");
    edc_tool_set_snippet(beta, "Beta two");

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(*group));
    group->group_id = edc_test_id("g-head");
    /* 模拟已经冻结的 system prompt：note_change 不许碰它。 */
    cJSON *head = cJSON_CreateObject();
    cJSON_AddStringToObject(head, "role", "system");
    cJSON_AddStringToObject(head, "content", "HEAD-SENTINEL");
    edc_task_group_history_add_owned(group, head);
    {
        cJSON *hist = edc_task_group_history_msgs(group, 0, -1);
        group->tools_decl = edc_toolset_resolve(tb, hist, EDC_TOOL_SCOPE_NORMAL);
        if (hist)
            cJSON_Delete(hist);
    }

    const char *removed[] = {"beta"};
    CHECK(edc_toolset_note_change(tb, group, NULL, removed, 1, "tools:off:beta"), "records the change");

    /* head 一个字节都没动。 */
    cJSON *head_now = edc_task_group_history_at(group, 0);
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(head_now, "content")), "HEAD-SENTINEL") == 0,
          "history[0] is never rewritten");

    /* 追加的那条带着新的段：alpha 在，beta 不在。 */
    int n = edc_task_group_history_count(group);
    cJSON *notice = edc_task_group_history_at(group, n - 1);
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(notice, "content"));
    CHECK(content && strstr(content, "out of date") != NULL, "it says the earlier section is stale");
    CHECK(content && strstr(content, "<tools>") != NULL, "and carries a fresh tagged section");
    CHECK(content && strstr(content, "- alpha: Alpha one") != NULL, "the current list is in it");
    CHECK(content && strstr(content, "Beta two") == NULL, "the removed tool is not");

    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

static void test_rebuild_keeps_only_the_latest_notice(void)
{
    TEST("rebuild: only the newest tool-set notice survives (it subsumes the older ones)");
    struct edc_toolbox *tb = bare_toolbox();
    add_tool(tb, "alpha", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(tb, "beta", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    struct edc_tool *t = NULL;
    HASH_FIND_STR(tb->tools, "alpha", t);
    edc_tool_set_snippet(t, "Alpha one");
    HASH_FIND_STR(tb->tools, "beta", t);
    edc_tool_set_snippet(t, "Beta two");

    struct edc_task_group *group = (struct edc_task_group *)calloc(1, sizeof(*group));
    group->group_id = edc_test_id("g-rebuild");
    /* Hand-built group with no session: the history starts empty. */
    group->tools_decl = edc_toolset_resolve(tb, NULL, EDC_TOOL_SCOPE_NORMAL);

    /* 两次变更：先移除 beta，再加入 gamma。 */
    const char *removed[] = {"beta"};
    CHECK(edc_toolset_note_change(tb, group, NULL, removed, 1, "r1-remove-beta"), "first change recorded");
    cJSON *added = cJSON_CreateArray();
    cJSON_AddItemToArray(added, schema_entry("gamma", "Gamma three"));
    CHECK(edc_toolset_note_change(tb, group, added, NULL, 0, "r2-add-gamma"), "second change recorded");
    cJSON_Delete(added);

    int notices = 0;
    for (int i = 0; i < edc_task_group_history_count(group); i++)
    {
        cJSON *m = edc_task_group_history_at(group, i);
        if (m && cJSON_HasObjectItem(m, "toolset_reason"))
            notices++;
        if (m)
            cJSON_Delete(m);
    }
    CHECK(notices == 2, "both notices are in history before a rebuild, got %d", notices);

    /* rebuild = 折叠落地 / 重启那条路：history 从 turns 重建，通知只补最后一条。 */
    edc_task_group_rebuild_history(group);

    notices = 0;
    sds content = NULL;
    for (int i = 0; i < edc_task_group_history_count(group); i++)
    {
        cJSON *m = edc_task_group_history_at(group, i);
        if (m && cJSON_HasObjectItem(m, "toolset_reason"))
        {
            notices++;
            if (content)
                sdsfree(content);
            content = sdsnew(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")));
        }
        if (m)
            cJSON_Delete(m);
    }
    CHECK(notices == 1, "exactly one notice after the rebuild, got %d", notices);
    CHECK(content && strstr(content, "r2-add-gamma") != NULL, "the survivor is the newest one");
    CHECK(content && strstr(content, "r1-remove-beta") == NULL, "the older notice is gone");
    CHECK(content && strstr(content, "- alpha: Alpha one") != NULL, "its section still lists what exists");
    /* gamma 只在 delta 里、没注册进 registry ⇒ 散文段只给名字（真实路径里 MCP 工具会同时注册，那时才有 snippet）。 */
    CHECK(content && strstr(content, "- gamma") != NULL, "including the newest addition");
    CHECK(content && strstr(content, "Beta two") == NULL, "and not what was removed");

    edc_task_group_free(group);
    edc_toolbox_free(tb);
}

/* ── R4: 检索工具的散文规则与命令拼装 ──────────────────────────── */

static void test_search_tools_flip_the_search_rule(void)
{
    TEST("render_prompt: with grep+find the rule switches from exec_shell to 'prefer grep/find'");

    struct edc_toolbox *before = bare_toolbox();
    add_tool(before, "exec_shell", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    sds no_search = edc_toolset_render_prompt(before, EDC_TOOL_SCOPE_NORMAL, NULL);
    CHECK(strstr(no_search, "Use exec_shell for searching") != NULL,
          "without grep/find the model is told to use exec_shell");
    sdsfree(no_search);
    edc_toolbox_free(before);

    struct edc_toolbox *after = bare_toolbox();
    add_tool(after, "exec_shell", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(after, "grep", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    add_tool(after, "find", EDC_TOOL_SCOPE_NORMAL, EDC_TOOL_EXPOSURE_DIRECT);
    sds with_search = edc_toolset_render_prompt(after, EDC_TOOL_SCOPE_NORMAL, NULL);
    CHECK(strstr(with_search, "Use exec_shell for searching") == NULL,
          "the fallback rule is gone once the tools exist");
    CHECK(strstr(with_search, "Prefer grep/find over exec_shell") != NULL, "and it is replaced by the preference");
    sdsfree(with_search);
    edc_toolbox_free(after);
}

/* 模型的 pattern 是任意字符串。它进了 `sh -c` 的脚本，所以引号必须被真正关住 ——
 * 这条测试钉的就是"注入不成立"。 */
/* find/grep 用同一个 glob 匹配器：`*` 与 `?`，且 `*` 不跨越 '/'
 * （这样 "*.c" 只匹配当前层的 .c 文件，而不是整条路径里任意位置）。 */
static void test_search_glob_matching(void)
{
    TEST("grep/find: the glob matcher (star does not cross a separator)");

    CHECK(edc_search_glob_match("*.c", "main.c"), "*.c matches main.c");
    CHECK(!edc_search_glob_match("*.c", "main.h"), "*.c does not match main.h");
    CHECK(edc_search_glob_match("*", "anything"), "a bare star matches any name");
    CHECK(edc_search_glob_match("a?c", "abc"), "? matches exactly one character");
    CHECK(!edc_search_glob_match("a?c", "ac"), "? requires that one character");
    CHECK(edc_search_glob_match("src/*.c", "src/a.c"), "a glob with a separator matches the relative path");
    CHECK(!edc_search_glob_match("src/*.c", "src/sub/a.c"), "star must not cross '/'");
    CHECK(edc_search_glob_match("*main.c", "the_main.c"), "a star prefix is allowed");
    CHECK(edc_search_glob_match("", "whatever"), "an empty glob accepts everything");

#ifdef _WIN32
    /* Windows filesystems fold case, so the matcher does too there. */
    CHECK(edc_search_glob_match("*.C", "main.c"), "on Windows the glob folds case");
#else
    CHECK(!edc_search_glob_match("*.C", "main.c"), "on POSIX the glob is case-sensitive");
#endif
}

/* ── R4 端到端：真的遍历一个目录树 ──────────────────────────────── */

static void write_test_file(const char *dir, const char *name, const char *body, size_t len)
{
    sds path = sdscatprintf(sdsempty(), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite(body, 1, len, f);
        fclose(f);
    }
    sdsfree(path);
}

static void test_search_walks_a_real_tree(void)
{
    TEST("grep/find: a real tree -- relative paths, globs, binary and CRLF handling");

    char root[1024];


    test_tmp_template(root, sizeof(root), "edc_search");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    sds sub = sdscatprintf(sdsempty(), "%s/sub", root);
    mkdir(sub, 0755);

    write_test_file(root, "a.c", "int a;\nTODO: alpha\n", 18);
    write_test_file(root, "b.h", "TODO: header\n", 13);
    write_test_file(sub, "c.c", "/* TODO: beta */\n", 16);
    /* 二进制：前 8KB 有 NUL，必须整份跳过（否则会喷乱码） */
    write_test_file(root, "blob.bin", "TODO\0\0\0", 7);
    /* CRLF：'\r' 属于行尾，不该出现在结果里 */
    write_test_file(root, "crlf.c", "TODO: windows\r\n", 15);

    /* grep：所有 TODO，路径相对 root（不带前导 '/'） */
    sds r = edc_search_run_blocking(root, true, "TODO", NULL, false, false, 100);
    CHECK(strstr(r, "a.c:2:TODO: alpha") != NULL, "hit with its line number, relative path");
    CHECK(strstr(r, "sub/c.c:1:"), "a nested file is searched");
    CHECK(strstr(r, "crlf.c:1:TODO: windows\n") != NULL, "CRLF keeps no stray carriage return");
    CHECK(strstr(r, "blob.bin") == NULL, "a binary file is skipped");
    CHECK(strstr(r, "/tmp/") == NULL, "paths are relative to the search root");
    sdsfree(r);

    /* glob 过滤 */
    r = edc_search_run_blocking(root, true, "TODO", "*.c", false, false, 100);
    CHECK(strstr(r, "a.c:2:") != NULL, "glob kept the .c file");
    CHECK(strstr(r, "b.h") == NULL, "glob dropped the .h file");
    sdsfree(r);

    /* literal：'(/*' 当正则会报错，当字面量就该命中 */
    r = edc_search_run_blocking(root, true, "/* TODO", NULL, false, true, 100);
    CHECK(strstr(r, "sub/c.c:1:") != NULL, "literal mode matches punctuation as-is");
    sdsfree(r);

    /* limit：只要 1 条，并说明被截断 */
    r = edc_search_run_blocking(root, true, "TODO", NULL, false, false, 1);
    int hits = 0;
    for (const char *q = r; (q = strstr(q, "TODO")) != NULL; q += 4)
        hits++;
    CHECK(hits == 1, "limit returns exactly one match");
    /* 诚实措辞：我们停在计数达到上限的那一刻，并不知道后面还有没有，所以只说"到了上限"，
     * 不断言"被截断"（那会让模型去缩小一个其实已经完整的检索）。 */
    CHECK(strstr(r, "reached the limit of 1 matches") != NULL, "and says the limit was reached");
    CHECK(strstr(r, "there may be more") != NULL, "without claiming that something was dropped");
    sdsfree(r);

    /* find：只列文件，不读内容 */
    r = edc_search_run_blocking(root, false, NULL, "*.c", false, false, 100);
    CHECK(strstr(r, "a.c") != NULL && strstr(r, "sub/c.c") != NULL, "find lists matching files");
    CHECK(strstr(r, "TODO") == NULL, "find prints paths, not content");
    sdsfree(r);

    /* 清理 */
    sds paths[6];
    const char *names[] = {"a.c", "b.h", "blob.bin", "crlf.c", NULL};
    for (int i = 0; names[i]; i++)
    {
        paths[i] = sdscatprintf(sdsempty(), "%s/%s", root, names[i]);
        remove(paths[i]);
        sdsfree(paths[i]);
    }
    sds cc = sdscatprintf(sdsempty(), "%s/c.c", sub);
    remove(cc);
    sdsfree(cc);
    rmdir(sub);
    sdsfree(sub);
    rmdir(root);
}

/* ── read_file 的行窗口 ────────────────────────────────────────── */

static sds window_of(const char *text, int64_t offset, int64_t limit)
{
    return edc_fs_read_file_window(text, strlen(text), offset, limit);
}

static void test_read_file_window(void)
{
    TEST("read_file: the whole file, a window, and the boundaries between them");

    /* A short file with no window: returned verbatim, no marker. */
    sds w = window_of("one\ntwo\nthree\n", 1, 0);
    CHECK(strcmp(w, "one\ntwo\nthree\n") == 0, "a short file comes back whole and unmarked");
    sdsfree(w);

    /* The last line without a trailing newline still counts as a line. */
    w = window_of("one\ntwo\nthree", 1, 0);
    CHECK(strcmp(w, "one\ntwo\nthree") == 0, "a file without a trailing newline is not mangled");
    sdsfree(w);

    /* A window: the head says which lines, the tail says how to continue. */
    w = window_of("l1\nl2\nl3\nl4\nl5\n", 2, 2);
    CHECK(strstr(w, "[lines 2-3 of 5]") != NULL, "the head names the window");
    CHECK(strstr(w, "l2\nl3\n") != NULL, "and carries exactly those lines");
    CHECK(strstr(w, "l1") == NULL && strstr(w, "l4") == NULL, "nothing outside the window");
    CHECK(strstr(w, "offset=4") != NULL, "and says which offset continues");
    sdsfree(w);

    /* Reaching the end of the file: no "more lines" line, because there are none. */
    w = window_of("a\nb\nc\n", 3, 10);
    CHECK(strstr(w, "[lines 3-3 of 3]") != NULL, "a window that ends the file still says where it is");
    CHECK(strstr(w, "more lines follow") == NULL, "but does not promise more");
    sdsfree(w);

    /* Past the end: an empty read must say so instead of looking like an empty file. */
    w = window_of("a\nb\n", 99, 10);
    CHECK(strstr(w, "no line 99") != NULL, "an offset past the end is reported");
    CHECK(strstr(w, "2 line(s)") != NULL, "with the real line count");
    sdsfree(w);

    /* An empty file has no line 1. */
    w = window_of("", 1, 10);
    CHECK(strstr(w, "no line 1") != NULL, "an empty file has no first line");
    sdsfree(w);

    /* CRLF: the '\r' belongs to the line's bytes and is returned as-is (the tool reads bytes,
     * it does not transcode), but counting must not be confused by it. */
    w = window_of("a\r\nb\r\n", 2, 1);
    CHECK(strstr(w, "[lines 2-2 of 2]") != NULL, "CRLF counts as two lines");
    CHECK(strstr(w, "b\r\n") != NULL, "and the bytes come back untouched");
    sdsfree(w);

    /* The byte budget ends a window on a line boundary. */
    {
        sds big = sdsempty();
        for (int i = 0; i < 100; i++)
        {
            big = sdscatprintf(big, "%03d ", i);
            for (int j = 0; j < 1000; j++)
                big = sdscatlen(big, "x", 1);
            big = sdscatlen(big, "\n", 1);
        }
        w = edc_fs_read_file_window(big, sdslen(big), 1, 1000);
        /* 64 KiB / ~1005 bytes per line -> about 65 lines, and every one of them complete. */
        CHECK(sdslen(w) < 70 * 1024, "a window stops near the byte budget (got %zu bytes)", sdslen(w));
        CHECK(strstr(w, "more lines follow") != NULL, "and says the rest is fetchable");
        CHECK(strstr(w, "[lines 1-") != NULL, "naming the range it did return");
        sdsfree(w);
        sdsfree(big);
    }

    /* One line longer than the whole budget: cut, and marked as a cut LINE (not "more
     * lines follow", which would be a lie about where the rest is). */
    {
        sds oneline = sdsempty();
        for (int i = 0; i < 100000; i++)
            oneline = sdscatlen(oneline, "y", 1);
        w = edc_fs_read_file_window(oneline, sdslen(oneline), 1, 0);
        CHECK(sdslen(w) < 70 * 1024, "a single huge line is bounded (got %zu)", sdslen(w));
        CHECK(strstr(w, "cut at 64 KiB") != NULL, "the marker says the line was cut");
        CHECK(strstr(w, "longer than 64 KiB and was cut") != NULL, "and says so again at the tail");
        CHECK(strstr(w, "more lines follow") == NULL, "it must not claim later lines are fetchable");
        sdsfree(w);
        sdsfree(oneline);
    }

    /* A big file CAN be read to the end by paging -- the point of offset/limit. */
    {
        sds big = sdsempty();
        for (int i = 1; i <= 5000; i++)
            big = sdscatprintf(big, "line %d\n", i);
        int64_t offset = 1;
        bool saw_last = false;
        int pages = 0;
        for (;;)
        {
            sds page = edc_fs_read_file_window(big, sdslen(big), offset, 0);
            if (strstr(page, "line 5000") != NULL)
                saw_last = true;
            const char *m = strstr(page, "pass offset=");
            if (!m)
            {
                sdsfree(page);
                break;
            }
            offset = atoll(m + strlen("pass offset="));
            sdsfree(page);
            CHECK(++pages < 10, "paging terminates within the expected number of pages");
            if (pages >= 10)
                break;
        }
        CHECK(saw_last, "paging with offset/limit reaches the end of a 5000-line file");
        CHECK(pages == 2, "5000 lines is 3 pages of 2000 (got %d)", pages);
        sdsfree(big);
    }
}

/* ── 带上限的读取：在分配之前就拒绝 ────────────────────────────── */

static void test_read_capped(void)
{
    TEST("edc_file_read_capped: the ceiling is checked before anything is allocated");

    char root[1024];


    test_tmp_template(root, sizeof(root), "edc_capped");
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    sds path = sdscatprintf(sdsempty(), "%s/f.txt", root);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fputs("0123456789", f); /* exactly 10 bytes */
        fclose(f);
    }

    size_t size = 0;
    EdcError *err = NULL;

    /* Exactly at the ceiling: allowed ("> max" is the rule, not ">= max"). */
    sds ok = edc_file_read_capped(path, 10, &size, &err);
    CHECK(ok && strcmp(ok, "0123456789") == 0, "a file exactly at the limit is read");
    CHECK(size == 10, "and its size is reported (got %zu)", size);
    if (ok)
        sdsfree(ok);
    if (err)
        edc_error_free(err);

    /* One byte over: refused, and the message carries the REAL size so a caller can say
     * "this is 10 bytes, the limit was 9, here is what to do". */
    err = NULL;
    size = 0;
    sds refused = edc_file_read_capped(path, 9, &size, &err);
    CHECK(refused == NULL, "one byte over the limit is refused");
    CHECK(size == 10, "the real size is still reported on refusal (got %zu)", size);
    CHECK(err && err->message && strstr(err->message, "10 bytes") != NULL, "the message names the size");
    CHECK(err && err->message && strstr(err->message, "9-byte") != NULL, "and the limit it broke");
    if (err)
        edc_error_free(err);

    /* Zero means "no ceiling" -- the callers that read their own small state files use it. */
    err = NULL;
    sds unlimited = edc_file_read_capped(path, 0, NULL, &err);
    CHECK(unlimited != NULL, "0 means no ceiling");
    if (unlimited)
        sdsfree(unlimited);
    if (err)
        edc_error_free(err);

    /* A directory is refused with a reason, not read as bytes. */
    err = NULL;
    sds dir = edc_file_read_capped(root, 0, NULL, &err);
    CHECK(dir == NULL, "a directory is not readable as a file");
    CHECK(err && err->message && strstr(err->message, "directory") != NULL, "and it says so");
    if (err)
        edc_error_free(err);

    /* Empty file: readable, size 0, content empty. */
    err = NULL;
    sds empty_path = sdscatprintf(sdsempty(), "%s/empty.txt", root);
    FILE *ef = fopen(empty_path, "wb");
    if (ef)
        fclose(ef);
    sds empty = edc_file_read_capped(empty_path, 10, &size, &err);
    CHECK(empty && sdslen(empty) == 0, "an empty file reads as empty");
    CHECK(size == 0, "with size 0");
    if (empty)
        sdsfree(empty);
    if (err)
        edc_error_free(err);
    remove(empty_path);
    sdsfree(empty_path);

    remove(path);
    sdsfree(path);
    rmdir(root);
}

int main(void)
{
    log_info("test_toolset: the declaration set (sorted, scope-filtered, replayable)");
    test_initial_is_sorted_and_scoped();
    test_initial_is_registration_order_independent();
    test_replay_without_deltas_is_identity();
    test_removal_forces_recompute();
    test_pure_addition_keeps_the_field_frozen();
    test_redefinition_is_non_additive();
    test_replay_is_stable_when_reapplied();
    test_resolve_always_reflects_the_current_set();
    test_render_prompt_is_terse_and_ruleful();
    test_search_tools_flip_the_search_rule();
    test_read_file_window();
    test_read_capped();
    test_search_glob_matching();
    test_search_walks_a_real_tree();
    test_note_change_adding_a_direct_tool_rewrites_the_field();
    test_note_change_adding_a_deferred_tool_keeps_the_bytes();
    test_note_change_appends_the_current_section();
    test_rebuild_keeps_only_the_latest_notice();
    test_note_change_removal_rewrites_the_field();
    test_notices_survive_a_history_rebuild();

    log_info("test_toolset: %d passed, %d failed", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
