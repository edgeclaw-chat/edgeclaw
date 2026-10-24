#include "edc_toolset.h"
#include "edc_toolbox.h"
#include "edc_task_group.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* A delta entry is either a request-shaped schema ({"type":"function","function":{"name":…}})
 * or a removal ({"name":…}); both name a tool, and that name is the only identity we use. */
static const char *entry_name(cJSON *entry)
{
    if (!entry)
        return NULL;
    cJSON *fn = cJSON_GetObjectItem(entry, "function");
    if (fn)
    {
        cJSON *name = cJSON_GetObjectItem(fn, "name");
        if (name && cJSON_IsString(name))
            return name->valuestring;
    }
    cJSON *name = cJSON_GetObjectItem(entry, "name");
    if (name && cJSON_IsString(name))
        return name->valuestring;
    return NULL;
}

static int cmp_entries(const void *a, const void *b)
{
    const char *na = entry_name(*(cJSON *const *)a);
    const char *nb = entry_name(*(cJSON *const *)b);
    if (!na)
        return nb ? -1 : 0;
    if (!nb)
        return 1;
    return strcmp(na, nb);
}

/* A duplicate of |arr| with the entries in name order. Sorting in place would mean moving
 * cJSON children around their owner; ordering is a property this module owns, so it just
 * rebuilds the array instead. Determinism is not cosmetic here -- the result goes into the
 * request's cached prefix (see the header). */
static cJSON *sorted_copy(cJSON *arr)
{
    cJSON *out = cJSON_CreateArray();
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    if (n == 0)
        return out;

    cJSON **items = (cJSON **)malloc(sizeof(*items) * (size_t)n);
    if (!items)
        return out;
    for (int i = 0; i < n; i++)
        items[i] = cJSON_GetArrayItem(arr, i);
    qsort(items, (size_t)n, sizeof(*items), cmp_entries);
    for (int i = 0; i < n; i++)
    {
        cJSON *dup = cJSON_Duplicate(items[i], 1);
        if (dup)
            cJSON_AddItemToArray(out, dup);
    }
    free(items);
    return out;
}

static int find_index(cJSON *arr, const char *name)
{
    if (!arr || !name)
        return -1;
    for (int i = 0; i < cJSON_GetArraySize(arr); i++)
    {
        const char *have = entry_name(cJSON_GetArrayItem(arr, i));
        if (have && strcmp(have, name) == 0)
            return i;
    }
    return -1;
}

cJSON *edc_toolset_initial(struct edc_toolbox *tb, uint32_t scope)
{
    cJSON *unsorted = cJSON_CreateArray();
    if (tb)
    {
        struct edc_tool *tool, *tmp;
        HASH_ITER(hh, tb->tools, tool, tmp)
        {
            /* DEFERRED tools are named in prose rather than declared in the field (B4). */
            if (tool->exposure != EDC_TOOL_EXPOSURE_DIRECT)
                continue;
            if ((tool->scope & scope) == 0)
                continue;
            cJSON *entry = edc_tool_to_schema(tool);
            if (entry)
                cJSON_AddItemToArray(unsorted, entry);
        }
    }
    cJSON *sorted = sorted_copy(unsorted);
    cJSON_Delete(unsorted);
    return sorted;
}

cJSON *edc_toolset_replay(cJSON *initial, cJSON *history, bool *has_non_additive)
{
    cJSON *cur = initial ? cJSON_Duplicate(initial, 1) : cJSON_CreateArray();
    bool non_additive = false;

    int n = history ? cJSON_GetArraySize(history) : 0;
    for (int i = 0; i < n; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(history, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        if (!role || !cJSON_IsString(role) || strcmp(role->valuestring, "system") != 0)
            continue;

        /* Any removal at all counts, even one that names a tool which never entered the
         * field: pi's hasNonAdditiveToolChanges returns true on the same condition, and it is
         * the conservative side -- recomputing when nothing changed yields the identical
         * array, so the cache survives anyway. */
        cJSON *removed = cJSON_GetObjectItem(msg, "tools_removed");
        if (cJSON_IsArray(removed))
        {
            if (cJSON_GetArraySize(removed) > 0)
                non_additive = true;
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, removed)
            {
                int at = find_index(cur, entry_name(entry));
                if (at >= 0)
                    cJSON_DeleteItemFromArray(cur, at);
            }
        }

        /* An addition either appends (a tool the model had never seen) or replaces a
         * same-name declaration -- and a replacement is not "additive": it changes an entry
         * the cached prefix already carries. */
        cJSON *added = cJSON_GetObjectItem(msg, "tools_added");
        if (cJSON_IsArray(added))
        {
            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, added)
            {
                const char *name = entry_name(entry);
                if (!name)
                    continue;
                int at = find_index(cur, name);
                if (at >= 0)
                {
                    non_additive = true;
                    cJSON *dup = cJSON_Duplicate(entry, 1);
                    if (dup)
                        cJSON_ReplaceItemInArray(cur, at, dup);
                }
                else
                {
                    cJSON *dup = cJSON_Duplicate(entry, 1);
                    if (dup)
                        cJSON_AddItemToArray(cur, dup);
                }
            }
        }
    }

    cJSON *sorted = sorted_copy(cur);
    cJSON_Delete(cur);
    if (has_non_additive)
        *has_non_additive = non_additive;
    return sorted;
}

/* A delta may carry the schema of a tool that is not DIRECT (that is how the model learns a
 * deferred tool by name). It must not enter the `tools` field: deferred means "not declared
 * here, reachable by name". Filtering by the registry's CURRENT exposure also means flipping a
 * live tool to deferred removes it from the field on the next request. */
static void drop_non_direct(struct edc_toolbox *tb, cJSON *set)
{
    if (!tb || !set)
        return;
    for (int i = cJSON_GetArraySize(set) - 1; i >= 0; i--)
    {
        const char *name = entry_name(cJSON_GetArrayItem(set, i));
        if (!name)
            continue;
        struct edc_tool *tool = NULL;
        HASH_FIND_STR(tb->tools, name, tool);
        if (tool && tool->exposure != EDC_TOOL_EXPOSURE_DIRECT)
            cJSON_DeleteItemFromArray(set, i);
    }
}

cJSON *edc_toolset_resolve(struct edc_toolbox *tb, cJSON *history, uint32_t scope)
{
    cJSON *initial = edc_toolset_initial(tb, scope);
    bool non_additive = false;
    cJSON *current = edc_toolset_replay(initial, history, &non_additive);
    cJSON_Delete(initial);

    /* ALWAYS the current set. The `tools` field is the contract: it is the only place the
     * model gets a tool's parameter schema, so it has to describe what exists right now --
     * adding, removing or redefining a DIRECT tool all change it. An earlier version froze
     * the field on pure additions to save a cache miss, which silently downgraded a direct
     * tool to a deferred one (name known, schema unknown): the saving was real, the semantics
     * were not. What preserves the prefix now is the CONFIG: a tool that should not disturb
     * it is declared deferred, and then never enters the field in the first place.
     *
     * `non_additive` stays useful, but only for attribution: it says whether this change
     * rewrote entries the cached prefix already carried (one miss) or just appended. */
    drop_non_direct(tb, current);
    if (non_additive)
        log_debug("toolset: the transcript's deltas are non-additive (removal/redefinition)");
    return current;
}

/* ── recording a change ───────────────────────────────────────────────────────── */

bool edc_toolset_note_change(struct edc_toolbox *tb, struct edc_task_group *group, cJSON *added_schemas,
                             const char **removed_names, size_t n_removed, const char *reason)
{
    if (!tb || !group)
        return false;
    const int n_added = added_schemas ? cJSON_GetArraySize(added_schemas) : 0;
    if (n_added == 0 && n_removed == 0)
        return false;

    const char *why = reason && *reason ? reason : "unspecified";

    /* The notice itself: one line of prose plus the machine-readable delta. */
    sds text = sdscatprintf(sdsempty(), "Tool set changed (%s).", why);
    if (n_added > 0)
    {
        text = sdscat(text, "\nAdded:");
        for (int i = 0; i < n_added; i++)
        {
            const char *name = entry_name(cJSON_GetArrayItem(added_schemas, i));
            text = sdscatprintf(text, "%s %s", i ? "," : "", name ? name : "?");
        }
        text = sdscat(text, ". Their definitions may not be in this request; call them by name.");
    }
    if (n_removed > 0)
    {
        text = sdscat(text, "\nRemoved:");
        for (size_t i = 0; i < n_removed; i++)
            text = sdscatprintf(text, "%s %s", i ? "," : "", removed_names[i] ? removed_names[i] : "?");
        text = sdscat(text, ". They are no longer callable.");
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "system");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddStringToObject(msg, "toolset_reason", why);
    if (n_added > 0)
        cJSON_AddItemToObject(msg, "tools_added", cJSON_Duplicate(added_schemas, 1));
    if (n_removed > 0)
    {
        cJSON *removed = cJSON_AddArrayToObject(msg, "tools_removed");
        for (size_t i = 0; i < n_removed; i++)
        {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "name", removed_names[i] ? removed_names[i] : "");
            cJSON_AddItemToArray(removed, entry);
        }
    }
    sdsfree(text);

    /* The delta must be IN history before the set is recomputed (replay reads the transcript),
     * and the recomputation must happen before the notice is duplicated into tool_deltas --
     * the appended section is part of that notice. So: add to history first, finish the
     * message, then take the master copy. */
    char *before = group->tools_decl ? cJSON_PrintUnformatted(group->tools_decl) : NULL;
    edc_task_group_history_add_owned(group, msg);
    cJSON_Delete(group->tools_decl);
    {
        /* The replay has to see the transcript, and the history is ids now, so this loads
         * it. Acceptable because it is NOT the per-request path: a group resolves its
         * toolset once, and recomputes only when a change is non-additive (a removal or a
         * redefinition). The request path never comes through here. */
        cJSON *hist = edc_task_group_history_msgs(group, 0, -1);
        group->tools_decl = edc_toolset_resolve(tb, hist, EDC_TOOL_SCOPE_NORMAL);
        if (hist)
            cJSON_Delete(hist);
    }
    char *after = group->tools_decl ? cJSON_PrintUnformatted(group->tools_decl) : NULL;

    /* The system prompt's tool section was frozen into history[0] when the group started and
     * is NEVER rewritten (that would invalidate everything behind it for no reason beyond the
     * one miss the field already costs). Instead the notice carries a fresh copy: the section
     * is tagged `<tools>`, so a later copy supersedes the earlier one -- pi's shape, and the
     * reason it can append instead of rewriting. */
    sds section = edc_toolset_render_prompt(tb, EDC_TOOL_SCOPE_NORMAL, group->tools_decl);
    if (section && sdslen(section) > 0)
    {
        sds content = sdsnew(cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content")));
        content = sdscat(content,
                         "\nThe tool section in the system prompt above is out of date. This is the current "
                         "one and it supersedes it:");
        content = sdscatsds(content, section);
        cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(content));
        sdsfree(content);
    }
    if (section)
        sdsfree(section);

    /* Master copy last: it now carries the appended section (a history rebuild replays it). */
    if (!group->tool_deltas)
        group->tool_deltas = cJSON_CreateArray();
    cJSON_AddItemToArray(group->tool_deltas, cJSON_Duplicate(msg, 1));

    const bool changed = (before == NULL) != (after == NULL) ||
                         (before && after && strcmp(before, after) != 0);
    if (changed)
        group->toolset_epoch++;
    log_info("toolset: group %s reason=%s +%d -%zu epoch=%u cache=%s", edc_id_str(group->group_id),
             why, n_added, n_removed, group->toolset_epoch, changed ? "reset" : "kept");
    if (before)
        cJSON_free(before);
    if (after)
        cJSON_free(after);
    return true;
}

static int cmp_tools_by_name(const void *a, const void *b)
{
    const struct edc_tool *ta = *(const struct edc_tool *const *)a;
    const struct edc_tool *tb_ = *(const struct edc_tool *const *)b;
    return strcmp(ta->name, tb_->name);
}

/* The declared tools of |scope|, name-ordered. Caller frees the array (not the tools). */
static struct edc_tool **declared_tools(struct edc_toolbox *tb, uint32_t scope, size_t *out_n)
{
    struct edc_tool **list = NULL;
    size_t n = 0;
    *out_n = 0;
    if (!tb)
        return NULL;

    size_t total = (size_t)HASH_COUNT(tb->tools);
    if (total == 0)
        return NULL;
    list = (struct edc_tool **)malloc(sizeof(*list) * total);
    if (!list)
        return NULL;

    struct edc_tool *tool, *tmp;
    HASH_ITER(hh, tb->tools, tool, tmp)
    {
        if (tool->exposure != EDC_TOOL_EXPOSURE_DIRECT)
            continue;
        if ((tool->scope & scope) == 0)
            continue;
        list[n++] = tool;
    }
    if (n > 1)
        qsort(list, n, sizeof(*list), cmp_tools_by_name);
    *out_n = n;
    return list;
}

/* Is |name| in a declaration set? */
static bool decl_has(cJSON *decl, const char *name)
{
    if (!decl || !name)
        return false;
    for (int i = 0; i < cJSON_GetArraySize(decl); i++)
    {
        const char *have = entry_name(cJSON_GetArrayItem(decl, i));
        if (have && strcmp(have, name) == 0)
            return true;
    }
    return false;
}

static struct edc_tool *tool_by_name(struct edc_toolbox *tb, const char *name)
{
    struct edc_tool *tool = NULL;
    if (tb && name)
        HASH_FIND_STR(tb->tools, name, tool);
    return tool;
}

/* The rules no JSON schema can carry: they are about the SET, not about one tool. Kept in
 * code so they follow the declared set instead of drifting from it. */
static void append_rules(sds *out, struct edc_toolbox *tb, cJSON *decl)
{
    const bool exec = decl_has(decl, "exec_shell");
    const bool grep = decl_has(decl, "grep");
    const bool find = decl_has(decl, "find");
    const bool device = decl_has(decl, "device_shell");

    sds rules = sdsempty();
    if (exec && !grep && !find)
        rules = sdscat(rules,
                       "- Use exec_shell for searching and listing: this session has no grep/find tool.\n");
    if (grep && find)
        rules = sdscat(rules, "- Prefer grep/find over exec_shell when searching file contents.\n");
    if (exec && device)
        rules = sdscat(rules,
                       "- The device_* tools run on the remote device; exec_shell runs on this machine.\n");
    if (decl_has(decl, "read_file") && decl_has(decl, "edit_file"))
        rules = sdscat(rules,
                       "- Read a file before editing it: edit_file's old_text must match the file byte for byte.\n");

    /* Behavioural notes that belong to a tool but not to its schema. Stable order: the
     * declaration set is already name-sorted. */
    for (int i = 0; decl && i < cJSON_GetArraySize(decl); i++)
    {
        const char *name = entry_name(cJSON_GetArrayItem(decl, i));
        struct edc_tool *tool = tool_by_name(tb, name);
        if (!tool || !tool->guidelines || sdslen(tool->guidelines) == 0)
            continue;
        int lines = 0;
        char **parts = sdssplitlen(tool->guidelines, (ssize_t)sdslen(tool->guidelines), "\n", 1, &lines);
        for (int li = 0; parts && li < lines; li++)
        {
            sds line = sdstrim(sdsnew(parts[li]), " \t");
            if (sdslen(line) > 0)
                rules = sdscatprintf(rules, "- %s: %s\n", name, line);
            sdsfree(line);
        }
        if (parts)
            sdsfreesplitres(parts, lines);
    }

    if (sdslen(rules) > 0)
        *out = sdscatprintf(*out, "\nRules:\n%s", rules);
    sdsfree(rules);
}

sds edc_toolset_render_prompt(struct edc_toolbox *tb, uint32_t scope, cJSON *decl)
{
    /* |decl| = the set to describe. NULL means "the initial set" (the system prompt's
     * snapshot at group start); a change notice passes the group's CURRENT set, so the
     * appended section and the `tools` field cannot disagree. */
    cJSON *own = NULL;
    if (!decl)
    {
        own = edc_toolset_initial(tb, scope);
        decl = own;
    }

    /* Deferred tools are not declared in the field, so this section is the only place the
     * model can learn they exist -- "deferred" has to mean "named here, called by name", or
     * it just means "invisible". */
    struct edc_tool **by_name = NULL;
    size_t n_by_name = 0;
    if (tb)
    {
        size_t total = (size_t)HASH_COUNT(tb->tools);
        if (total > 0)
            by_name = (struct edc_tool **)malloc(sizeof(*by_name) * total);
        if (by_name)
        {
            struct edc_tool *tool, *tmp;
            HASH_ITER(hh, tb->tools, tool, tmp)
            {
                if (tool->exposure != EDC_TOOL_EXPOSURE_DEFERRED)
                    continue;
                if ((tool->scope & scope) == 0)
                    continue;
                if (decl_has(decl, tool->name))
                    continue;
                by_name[n_by_name++] = tool;
            }
            if (n_by_name > 1)
                qsort(by_name, n_by_name, sizeof(*by_name), cmp_tools_by_name);
        }
    }

    const int n = cJSON_GetArraySize(decl);
    if (n == 0 && n_by_name == 0)
    {
        free(by_name);
        cJSON_Delete(own);
        return sdsempty();
    }

    /* The tag is what makes a LATER copy authoritative: pi wraps every prompt section in a
     * tag of its own name, so an update appended mid-conversation supersedes the earlier one
     * instead of contradicting it. `history[0]` therefore never has to be rewritten. */
    sds out = sdsnew("\n<tools>\n## Tools\n\n");
    for (int i = 0; i < n; i++)
    {
        const char *name = entry_name(cJSON_GetArrayItem(decl, i));
        struct edc_tool *tool = tool_by_name(tb, name);
        if (tool && tool->snippet && sdslen(tool->snippet) > 0)
            out = sdscatprintf(out, "- %s: %s\n", name, tool->snippet);
        else
            out = sdscatprintf(out, "- %s\n", name ? name : "?");
    }
    append_rules(&out, tb, decl);

    if (n_by_name > 0)
    {
        out = sdscat(out, "\nAlso callable by name (definitions are not in this request; call them and the "
                          "arguments are checked when the call lands):\n");
        for (size_t i = 0; i < n_by_name; i++)
        {
            if (by_name[i]->snippet && sdslen(by_name[i]->snippet) > 0)
                out = sdscatprintf(out, "- %s: %s\n", by_name[i]->name, by_name[i]->snippet);
            else
                out = sdscatprintf(out, "- %s\n", by_name[i]->name);
        }
    }

    out = sdscat(out, "</tools>\n");
    free(by_name);
    cJSON_Delete(own);
    return out;
}
