#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_fold_files.h"
#include "edc_resources.h"
#include "edc_skills.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

/* There are no "bootstrap files" any more -- the workspace is not a source of prompt text:
 *
 *   - The behaviour contract (skills / memory / guidelines / reminders / device) is generated
 *     here, in edc_context_build_contract(). It belongs to the BINARY: it describes how this
 *     program works, so it must change with the program. Shipping it as a file in the user's
 *     workspace made it a copy that could never be updated -- the workspace is released once,
 *     on first run, and an existing directory is never touched again.
 *   - Tool descriptions come from the tool registry (request `tools` field + the generated
 *     section), never from a file. TOOLS.md was the third copy of that list and it drifted
 *     (it described web_search / web_fetch / message / spawn long after they were gone).
 *   - AGENTS.md is PROJECT input, not bootstrap: ~/.edgeclaw/AGENTS.md plus the upward walk
 *     from the session's workdir, see load_project_context(). A project that has none simply
 *     gets no <project_instructions> section (same as pi).
 *   - IDENTITY.md was dropped with the array: a persona override is a different feature
 *     (custom prompt), not a file the release ships next to the model's instructions. */

/* pi's "context file" candidates (docs/pi-system-prompt-guide.md §3.6): IN a directory
 * only the highest-priority one is taken; an override suppresses only its own directory. */
static const char *CONTEXT_FILE_CANDIDATES[] = {"AGENTS.override.md", "AGENTS.md", "AGENTS.MD", "CLAUDE.md",
                                                "CLAUDE.MD"};

/* Read a context file; NULL when unreadable (not an error -- absence is normal; on pi's side the section just does not appear). */
static sds read_context_file(const char *path)
{
    EdcError *err = NULL;
    sds content = edc_file_read(path, &err);
    if (!content)
    {
        if (err)
            edc_error_free(err);
        return NULL;
    }
    return content;
}

/* Append one file (wrapped in pi's <project_instructions path="...">); empty content /
 * duplicate path are skipped. `seen` is the set already collected (so AGENTS.md is not
 * collected twice when the workdir is the workspace). */
static void append_context_file(sds *out, sds *seen, size_t *seen_n, const char *path)
{
    for (size_t i = 0; i < *seen_n; i++)
    {
        if (seen[i] && strcmp(seen[i], path) == 0)
            return;
    }
    sds content = read_context_file(path);
    if (!content || sdslen(content) == 0)
    {
        if (content)
            sdsfree(content);
        return;
    }
    *out = sdscatprintf(*out, "<project_instructions path=\"%s\">\n%s\n</project_instructions>\n\n", path, content);
    sdsfree(content);
    if (*seen_n < 64)
        seen[(*seen_n)++] = sdsnew(path);
}

/* The "Project Context" section: one user-global file + an upward walk from the
 * session's workdir to the root (broad first, then specific). No hit => empty string
 * (the section does not appear, matching pi). `skip` = disabled in config. */
static sds load_project_context(const char *workspace, const char *workdir, bool skip)
{
    if (skip)
        return sdsempty();

    sds seen[64];
    size_t seen_n = 0;
    sds body = sdsempty();

    /* (1) user-global (pi: ~/.pi/agent/AGENTS.md; us: ~/.edgeclaw/AGENTS.md) */
    sds bc_home = edc_config_resolve_bc_home();
    sds global = edc_path_join(bc_home, "AGENTS.md", NULL);
    append_context_file(&body, seen, &seen_n, global);
    sdsfree(global);
    sdsfree(bc_home);

    /* (2) walk up from workdir (collected into an array, output root -> workdir) */
    if (workdir && workdir[0])
    {
        sds dirs[64];
        size_t n = 0;
        sds cur = sdsnew(workdir);
        while (n < 64)
        {
            dirs[n++] = sdsdup(cur);
            sds parent = edc_path_dirname(cur);
            /* Reached the root (dirname stops changing) or dirname failed => stop */
            if (!parent || strcmp(parent, cur) == 0)
            {
                sdsfree(parent);
                break;
            }
            sdsfree(cur);
            cur = parent;
        }
        sdsfree(cur); /* cur is still alive on exit (the break path) -- each step above frees the old one */

        for (size_t i = n; i-- > 0;)
        {
            for (size_t c = 0; c < sizeof(CONTEXT_FILE_CANDIDATES) / sizeof(CONTEXT_FILE_CANDIDATES[0]); c++)
            {
                sds path = edc_path_join(dirs[i], CONTEXT_FILE_CANDIDATES[c], NULL);
                sds content = read_context_file(path);
                if (content && sdslen(content) > 0)
                {
                    sdsfree(content);
                    append_context_file(&body, seen, &seen_n, path);
                    sdsfree(path);
                    break; /* only the highest-priority one per directory */
                }
                if (content)
                    sdsfree(content);
                sdsfree(path);
            }
        }
        for (size_t i = 0; i < n; i++)
            sdsfree(dirs[i]);
    }

    for (size_t i = 0; i < seen_n; i++)
        sdsfree(seen[i]);

    if (sdslen(body) == 0)
    {
        sdsfree(body);
        return sdsempty();
    }
    sds section = sdsnew("## Project Context\n\nProject-specific instructions and guidelines:\n\n");
    section = sdscatsds(section, body);
    sdsfree(body);
    return section;
}

static void sds_replace_all(sds *s, const char *find, const char *replacement)
{
    size_t find_len = strlen(find);
    sds result = sdsempty();
    char *scan = *s;
    char *pos;
    while ((pos = strstr(scan, find)) != NULL)
    {
        result = sdscatlen(result, scan, pos - scan);
        result = sdscat(result, replacement);
        scan = pos + find_len;
    }
    result = sdscat(result, scan);
    sdsfree(*s);
    *s = result;
}

static sds edc_context_get_identity(const char *os_name, const char *edc_home, const char *workspace,
                                    const char *workdir)
{
    const char *identity_fmt = "# EdgeClaw\n"
                               "You are EdgeClaw, a helpful AI assistant. You have access to tools "
                               "that allow you to:\n"
                               "- Read, write, and edit files\n"
                               "- Execute shell commands\n"
                               "\n"
                               "## OS Name\n"
                               "{{os_name}}"
                               "\n"
                               "## Current Running Directory\n"
                               "{{cwd}}\n"
                               "## Workspace\n"
                               "Your workspace is at: {{workspace}}\n"
                               "- Custom skills: {{workspace}}/skills/{{skill-name}}/SKILL.md\n"
                               "\n";

    /* The manual line, added only when $EDC_HOME is known (a config built by a test may
     * carry a workspace and nothing else). Template + sds_replace_all like the paths
     * above: no printf, so a '%' in the path cannot be read as a conversion. */
    const char *manual_fmt = "## Usage Manual\n"
                             "EdgeClaw's own manual is embedded in this binary and released to:\n"
                             "`{{edc_home}}/docs/edgeclaw.md`\n"
                             "\n"
                             "Read it (read_file) whenever you need to know how EdgeClaw itself works: "
                             "commands and their options, sessions, tasks and groups, the input syntax, "
                             "tools and their limits. The other chapters live in the same directory "
                             "(`edgeclaw.1`, `input-syntax.zh.md`, `local-shell-command.zh.md`, "
                             "`tool-output-limits.zh.md`, `prompt-library.md`, ...). It documents THIS "
                             "binary -- prefer it to guessing, and to any older copy of the docs.\n"
                             "\n";

    const char *identity_tail = "IMPORTANT: When responding to direct questions or conversations, "
                                "reply directly with your text response.\n"
                                "\n"
                                "Always be helpful, accurate, and concise. When using tools, explain "
                                "what you're doing.\n"
                                "\n"
                                "This prompt carries no clock: when you need the current date or time, run "
                                "`date` (or check the OS).\n\n";

    /* The session's bound working directory is the "current directory" the
     * model is told about; unbound sessions fall back to the process cwd. */
    sds cwd = (workdir && workdir[0]) ? sdsnew(workdir) : edc_cwd();
    sds identity = sdsnew(identity_fmt);
    sds_replace_all(&identity, "{{os_name}}", os_name);
    sds_replace_all(&identity, "{{cwd}}", cwd);
    sds_replace_all(&identity, "{{workspace}}", workspace);
    sdsfree(cwd);

    if (edc_home && edc_home[0])
    {
        sds manual = sdsnew(manual_fmt);
        sds_replace_all(&manual, "{{edc_home}}", edc_home);
        identity = sdscatsds(identity, manual);
        sdsfree(manual);
    }

    identity = sdscat(identity, identity_tail);
    return identity;
}

/* The behaviour contract: how this program works, in the program's own words.
 *
 * Generated rather than read from a file because a shipped file cannot be updated: the
 * workspace is released once, so a workspace/AGENTS.md would freeze at the version the user
 * first installed (and it did -- copies still described tools that no longer existed). Only
 * facts that cannot come from a tool's JSON schema live here; anything a schema can say
 * (parameters, one-line purpose) stays in the registry.
 *
 * Interpolated: the workspace path. Everything else is a constant, so two builds of the same
 * version produce byte-identical prompts (docs/pi-system-prompt-guide.md §7). */
static sds edc_context_build_contract(const char *workspace)
{
    return sdscatprintf(sdsempty(),
        "## How this program works\n\n"
        "**Skills.** A skill is a directory with a `SKILL.md`: %s/skills/<name>/SKILL.md, and the other "
        "resource roots this program shares with pi / Claude Code / opencode. The system prompt lists them; "
        "read a SKILL.md before following it, or let the user run `/skill <name>`.\n\n"
        "**Memory.** Durable knowledge lives in this session's shared memory, visible to every task and "
        "group in the session: `memory_write` to record, `memory_search` / `memory_list` / `memory_get` to "
        "read back, `memory_update` / `memory_supersede` / `memory_delete` to revise. Cross-session memory "
        "does not exist yet. There is no memory file to edit.\n\n"
        "**Guidelines.**\n"
        "- Explain what you are about to do before doing it.\n"
        "- Multi-step work: plan with `todowrite` first, then execute step by step, updating it as you go.\n"
        "- Verify after changing something (re-read the file, run the test, check the output).\n"
        "- When a tool fails, read the error and take a different approach; do not repeat the same call.\n"
        "- Look things up with tools before asking; ask when the tools genuinely cannot answer.\n"
        "- Never fake confidence: say what you know and flag what you do not.\n\n"
        "**Reminders.** Scheduling is the `crontab` tool -- call it (list / add / remove). Writing a "
        "reminder into a notes file triggers nothing.\n\n"
        "**Remote device.** The `device_*` tools talk to a separate agent over TCP or a PPPoS serial "
        "link; they run on that device, not in this sandbox.\n",
        workspace ? workspace : "(workspace)");
}

static sds edc_context_load_skills(const struct edc_config *conf, const char *workdir)
{
    sds result = sdsempty();
    /* Every resource root, ours first -- so a skill the user brought over from pi /
     * Claude Code / opencode shows up here without being moved. The project roots come
     * from the SESSION's directory (see edc_resource_dirs). */
    UT_array *roots = edc_resource_dirs(conf, workdir);
    UT_array *skills = edc_skills_load_roots(roots, conf);

    result = sdscatprintf(result, "## Skills\n\n"
                                  "The following skills extend your capabilities. To use a skill, read its SKILL.md "
                                  "file using the read_file tool. (`/skill <name>` runs one directly.)\n\n");

    bool wrote_any = false;
    for (size_t i = 0; i < utarray_len(skills); i++)
    {
        struct edc_skill **skill_ptr = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        struct edc_skill *skill = *skill_ptr;

        /* The author said the model must not reach for it on its own: it stays off this
         * list (an explicit `/skill <name>` still runs it). */
        if (!skill->model_invocable)
        {
            edc_skill_free(skill);
            continue;
        }

        struct edc_skill_meta_entry *e;
        HASH_FIND_STR(skill->meta, "description", e);
        const char *desc = e ? e->value : NULL;
        if (!desc)
        {
            edc_skill_free(skill);
            continue;
        }

        sds loc = edc_path_join(skill->source ? skill->source : "", "skills", skill->name, "SKILL.md", NULL);
        if (wrote_any)
            result = sdscat(result, "\n\n---\n\n");
        result = sdscatprintf(result, "### Skill: %s\n\n", skill->name);
        result = sdscatprintf(result, "description: %s\nSKILL.md location: %s\n", desc, loc);
        sdsfree(loc);
        wrote_any = true;
        edc_skill_free(skill);
    }

    utarray_free(skills);
    edc_resource_dirs_free(roots);
    return result;
}


/* Compact description of the sandbox's writable / read-only path sets, so the
 * model knows up front which directories it can write and does not waste tool
 * calls probing paths that will be denied. */
static sds edc_context_build_permissions(struct edc_config *conf)
{
    sds out = sdsnew("## Filesystem Permissions\n\n"
                     "File and shell tools are sandboxed: writes are allowed only under the "
                     "writable paths below. Every other path is read-only (reads are "
                     "unrestricted, writes elsewhere are denied), so do not try to create or "
                     "modify files outside the writable set.\n\n"
                     "- Writable:\n");
    for (int i = 0; conf->sandbox_rw_paths && conf->sandbox_rw_paths[i]; i++)
        out = sdscatprintf(out, "  - %s\n", conf->sandbox_rw_paths[i]);

    out = sdscat(out, "- Read-only (writes denied):\n");
    for (int i = 0; conf->sandbox_ro_paths && conf->sandbox_ro_paths[i]; i++)
        out = sdscatprintf(out, "  - %s\n", conf->sandbox_ro_paths[i]);

    out = sdscat(out, "\nA path matched by both lists takes the more specific (longer) entry.\n\n");
    return out;
}

/* The ONLY session-memory hint: constant text, zero variable content (no count, no
 * content, no ids). Placed at the END of the system prompt -- the last thing the model
 * reads before the long prompt (project instructions / skills can be tens of thousands of tokens),
 * giving it best recency; the count/content come from the request-time tail notice.
 * Constant => the prompt cache prefix is untouched. */
static const char kSessionMemoryHint[] =
    "\n## Session Memory\n"
    "This session keeps a shared memory of durable knowledge (decisions, constraints, gotchas, external facts),\n"
    "visible to every task in it. Consult it when you need context from earlier work.\n"
    "\n"
    "When you decide to touch memory, do NOT just append -- first reconcile, then pick the right move:\n"
    "- RECONCILE: look for existing entries about the same thing (`memory_search <patterns...>` / `memory_list`)\n"
    "  and read them (`memory_get <ids...>`). If what you just learned shows an entry is now wrong, UPDATE it\n"
    "  to the correct content; if it is simply obsolete, RETIRE it (`memory_supersede <ids...>`; pass a list).\n"
    "- MERGE: if a related entry exists and the new knowledge belongs with it, UPDATE it with the merged\n"
    "  content -- do NOT create a near-duplicate.\n"
    "- CREATE: `memory_write` a new entry only when nothing related exists, or the knowledge genuinely\n"
    "  deserves its own entry.\n"
    "- Before `memory_update`, call `memory_get` and pass its `rev` as `expected_rev`. If the tool says the\n"
    "  entry changed, re-read it and redo the merge with the new rev.\n"
    "\n"
    "Tools:\n"
    "- `memory_search <patterns...>`                   -- regex (grep -E, case-insensitive) over title/summary/detail\n"
    "- `memory_list [offset] [n]`                      -- enumerate entries\n"
    "- `memory_get <ids...>`                           -- read entries (each with its rev)\n"
    "- `memory_update <entries>`                       -- update entries: [{id, expected_rev, title?, summary?, detail?}] (atomic)\n"
    "- `memory_supersede <ids...>`                     -- retire entries (obsolete; kept and still visible as retired)\n"
    "- `memory_delete <ids...>`                        -- delete entries for real (removed; cannot be read again)\n"
    "- `memory_write <title> [summary] [detail]`       -- create a new entry\n"
    "New or updated entries are announced at the tail of your context (a \"## Session Memory Updates\" block)\n"
    "when something changed. Keep progress logs and todos out of it.\n";

/* Installed by the assembly root (see edc_context.h); NULL = no tool section, which is what
 * a unit test that builds a prompt without a registry wants. */
static EdcToolSectionFn g_tool_section_fn = NULL;
static void *g_tool_section_ctx = NULL;

void edc_context_set_tool_section_provider(EdcToolSectionFn fn, void *ctx)
{
    g_tool_section_fn = fn;
    g_tool_section_ctx = ctx;
}

sds edc_context_build_system_prompt(struct edc_config *conf, const char *workdir)
{
    const char *workspace = conf->workspace;
    sds os_name = edc_os_name();

    sds identity = edc_context_get_identity(os_name, conf->edc_home, workspace, workdir);
    sds permissions = edc_context_build_permissions(conf);
    sds contract = edc_context_build_contract(workspace);
    /* Context files (pi §3.6): user-global + an upward walk from THIS session's workdir
     * -- so a session bound to a project directory really picks up that project's
     * AGENTS.md. */
    sds project_context = load_project_context(workspace, workdir, conf->skip_context_files);
    sds skills = edc_context_load_skills(conf, workdir);

    sds system_prompt = sdsempty();
    system_prompt = sdscat(system_prompt, identity);
    system_prompt = sdscat(system_prompt, permissions);
    system_prompt = sdscat(system_prompt, contract);
    system_prompt = sdscatsds(system_prompt, project_context);
    system_prompt = sdscat(system_prompt, skills);

    /* The tool section: rendered from the registry by the provider the assembly root
     * installed (see edc_context.h). It sits inside the frozen system prompt, so it is
     * generated once per group and lives in the cached prefix -- which is exactly why it
     * must be deterministic and must not restate the request's `tools` field. */
    if (g_tool_section_fn)
    {
        sds tools = g_tool_section_fn(g_tool_section_ctx);
        if (tools)
        {
            system_prompt = sdscatsds(system_prompt, tools);
            sdsfree(tools);
        }
    }
    system_prompt = sdscat(system_prompt, kSessionMemoryHint);

    sdsfree(os_name);

    sdsfree(identity);
    sdsfree(permissions);
    sdsfree(contract);
    sdsfree(project_context);
    sdsfree(skills);

    return system_prompt;
}
/* ── task todo list -> runtime reminder (request tail) ──────────
 * (2026-09-09: changed from "injected at the head system prompt's tail" to
 * "appended at the very end of the request message list, absolutely fresh" -- a head
 * injection is easily forgotten in a long context, while the tail sits right where the
 * model starts generating. Same day: not sent every request; throttled by
 * edc_app_cb_send_llm via edc_task_llm_request_poke -- only when
 * EDC_TODO_SYNC_REMIND_GAP requests have passed without a todowrite call (reset at the
 * threshold).
 * The two-part skeleton text is generated solely by
 * edc_context_render_todo_reminder, byte-identical to the todowrite tool's successful
 * result (same render source, no separate "# TODO" checklist form). This function only
 * wraps that text into one role=system message.
 * Copy semantics: builds a new system message, never touches the shared history[0] / task. */

/* Runtime todo reminder (a fresh role=system message every time). Called by
 * edc_app_cb_send_llm after the throttle passes; appended at the very end of the request
 * message list (req->tail_system; the llm actor appends after history + the current
 * turn). Three shapes:
 *  - task has a non-empty todo list -> <system-reminder> with a status section (the list
 *    rows) + a turn-instruction section;
 *  - task non-NULL but empty/cleared -> same skeleton with "no list yet" + the planning
 *    guide;
 *  - task NULL -> NULL.
 * Copy semantics: returns a fresh message not attached to any array, freed with the
 * request; no change to task/group. */
/* Renders plain "- [ ]/- [x]" list rows only (no wrapper, no guidance) -- the same line
 * format source as render_reminder: it is the todo reminder's status body, for callers
 * that need the rows alone. items is already validated at the tool entry (legal status,
 * at most one in_progress); missing fields degrade conservatively (pending / empty) so
 * app-side reads of historical data cannot crash.
 * *out_completed (may be NULL) returns the completed count. Returns sds, caller sdsfree. */
sds edc_context_render_todo_body(cJSON *items, int *out_completed)
{
    int total = items ? cJSON_GetArraySize(items) : 0;
    sds lines = sdsempty();
    int completed = 0;
    for (int i = 0; i < total; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(items, i);
        char *status = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        char *content = cJSON_GetStringValue(cJSON_GetObjectItem(item, "content"));
        char *priority = cJSON_GetStringValue(cJSON_GetObjectItem(item, "priority"));
        const char *st = status ? status : "pending";
        const char *ct = content ? content : "";
        const char *pri = priority ? priority : "";

        const char *marker;
        if (strcmp(st, "completed") == 0)
        {
            marker = "- [x]";
            completed += 1;
        }
        else if (strcmp(st, "cancelled") == 0)
        {
            marker = "- [-]";
        }
        else if (strcmp(st, "in_progress") == 0)
        {
            marker = "- [•]";
        }
        else
        {
            marker = "- [ ]";
        }

        if (pri && *pri)
            lines = sdscatprintf(lines, "%s %s [%s]\n", marker, ct, pri);
        else
            lines = sdscatprintf(lines, "%s %s\n", marker, ct);
    }
    if (out_completed)
        *out_completed = completed;
    return lines;
}

/* ── The two-part todo reminder skeleton (single render source) ─
 * reminder = "system state + this turn's instructions":
 *   1) ## CURRENT TASK SYSTEM STATE -- the whole todo list (rows in the tool's format),
 *      so the model sees the plan and how far along it is;
 *   2) ## INSTRUCTIONS FOR THIS TURN -- the per-turn cadence: check the last tool's
 *      result; after finishing the current item the NEXT action must be a todowrite
 *      to sync state; do not move on until the current item is marked [x]. A per-turn
 *      constraint fixing the "several steps before updating the list" lag.
 * edc_context.c is the sole generator of this text -- it used to live in
 * edc_tools_todo.c, but it renders the task's todos (context material) and the
 * production reader is this file's edc_context_make_todo_reminder; leaving it in the
 * toolbox would make kernel depend on the tool module. Once here, "tool echo and
 * reminder are byte-identical" is structural (one implementation): the todowrite
 * tool's successful result IS this reminder, and the request-tail reminder
 * (edc_context_make_todo_reminder's content) is generated by the same function -- there
 * is no separate "# TODO" markdown form. */

/* Status heading / instruction heading / no-list status line (shared by both branches) */
static const char *kTodoStateHeader = "## 🚨 CURRENT TASK SYSTEM STATE\n";
static const char *kTodoTurnHeader = "## 🛠️ INSTRUCTIONS FOR THIS TURN\n";
static const char *kTodoNoListState = "No todo list exists for this task yet.\n";

/* Per-turn cadence (injected when the list is non-empty): never finish several steps
 * before updating the todo -- once the current item is done, the next action must sync
 * via todowrite. */
static const char *kTodoTurnGuide =
    "- Check the execution result of the last tool call.\n"
    "- If the item you are working on is fully finished, your NEXT SINGLE ACTION must be calling "
    "todowrite to update the state (mark it \"completed\", promote the next pending item to "
    "\"in_progress\", send the FULL updated list).\n"
    "- Do NOT jump to the next item until the current one is marked as [x].\n"
    "- When the whole list is done, keep the completed list — do not clear it or invent items.\n";

/* Planning guide (injected when there is no list yet): a multi-step task first writes
 * its plan with todowrite, after which each round naturally switches to
 * kTodoTurnGuide's cadence. */
static const char *kTodoPlanningGuide =
    "- If the request is multi-step — several distinct steps or multiple tool calls (research, coding, "
    "debugging, batch edits, ...) — your FIRST action must be calling todowrite to create a "
    "step-by-step plan, then execute it.\n"
    "- Simple single-step requests or direct conversation do NOT need a todo list — just answer "
    "without creating one.\n";

/* The full todo reminder text (with the <system-reminder> wrapper), byte-identical to
 * the request-tail reminder (edc_context_make_todo_reminder's content) -- the todowrite
 * tool result and the context reminder share this single render source. Two exclusive
 * branches:
 *  - items non-empty -> status = the rows (render_body), instructions = the cadence;
 *  - items NULL/empty -> status = "no list yet", instructions = the planning guide.
 * Returns sds, caller sdsfree. */
sds edc_context_render_todo_reminder(cJSON *items)
{
    int has_list = items && cJSON_GetArraySize(items) > 0;
    sds content = sdsnew("<system-reminder>\n");
    content = sdscat(content, kTodoStateHeader);

    if (has_list)
    {
        sds body = edc_context_render_todo_body(items, NULL);
        content = sdscat(content, body); /* list rows, each ending with \n */
        sdsfree(body);
    }
    else
    {
        content = sdscat(content, kTodoNoListState);
    }

    content = sdscat(content, "\n"); /* blank line between the status and instruction sections */
    content = sdscat(content, kTodoTurnHeader);

    content = sdscat(content, has_list ? kTodoTurnGuide : kTodoPlanningGuide);

    content = sdscat(content, "\n</system-reminder>");
    return content;
}

cJSON *edc_context_make_todo_reminder(struct edc_task *task)
{
    if (!task)
        return NULL;

    sds content = edc_context_render_todo_reminder(task->todos);

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", content);
    sdsfree(content);
    return sys;
}

/* The single assembly entry for this request's "system part" (the head): system prompt +
 * group memory. No todo content -- todo guidance always comes from
 * edc_context_make_todo_reminder at the very end. Reference/copy/build semantics are in
 * edc_context.h.
 *
 * Order is CACHE-FRIENDLY: constants first, mutable last. The system prompt is fixed
 * within a session (a rebuild reuses the same history[0]), while `## Group Memory` is
 * rewritten on every fold landing -- if it came first, one change would invalidate the
 * constant that follows (a whole-prefix miss). Swapped, a fold only invalidates
 * "group memory + unfolded history". */
int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf, struct edc_task_group *group,
                                   const char *workdir, sds *out_pristine)
{
    int appended = 0;
    if (!msgs)
        return 0;
    if (out_pristine)
        *out_pristine = NULL;

    /* 1. system prompt (pristine, no todo injection)
     * - group has history: attach its first slot (the system prompt);
     * - no history / group NULL: build a pristine prompt and return it via out_pristine
     *   (the caller decides whether to write it back to group history; after that,
     *   later requests take the first branch).
     * The slot hands out an owned copy now, so this moves it into |msgs| rather than
     * referencing it. */
    if (group && edc_task_group_history_count(group) > 0)
    {
        cJSON *sys0 = edc_task_group_history_at(group, 0);
        if (sys0)
        {
            cJSON_AddItemToArray(msgs, sys0);
            appended++;
        }
    }
    else if (conf)
    {
        sds prompt = edc_context_build_system_prompt(conf, workdir);
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", prompt);
        cJSON_AddItemToArray(msgs, sys);
        appended++;
        if (out_pristine)
            *out_pristine = sdsdup(prompt);
        sdsfree(prompt);
    }

    /* 2. GROUP memory summary (a condensation of folded turns) -- the mutable part last (see the function head).
     *
     * The tracked-file blocks are RENDERED here, from `group->fold_files`, not stored in
     * `group->memory`: the text is a projection of the data. It must be deterministic --
     * same (body, lists) => same bytes -- because this message sits in the cached prefix
     * and a single different byte invalidates it from token 0. */
    if (group && group->memory && sdslen(group->memory) > 0)
    {
        cJSON *mem = cJSON_CreateObject();
        cJSON_AddStringToObject(mem, "role", "system");
        sds rendered = edc_fold_files_render(group->memory, group->fold_files);
        sds mc = sdscatprintf(sdsempty(), "## Group Memory\n%s", rendered);
        cJSON_AddStringToObject(mem, "content", mc);
        cJSON_AddItemToArray(msgs, mem);
        sdsfree(mc);
        sdsfree(rendered);
        appended++;
    }

    return appended;
}

/* ── Group context size: pi's "anchor + tail estimate" ────────
 *
 * Why an anchor: the estimator is a heuristic and estimating the whole context
 * accumulates error (worse for Chinese). The provider's usage for one request is truth,
 * and "what was appended after the anchor" is the only unknown -- pinning the estimate's
 * error to that small span.
 *
 * When the anchor is untrustworthy (both fall back to estimating from scratch):
 *   1. it predates the group's last fold: the fold replaced the prefix with a summary,
 *      and the anchor recorded the larger prefix before that;
 *   2. it has no position info (last_hist_len == 0): an anchor read from an old archive,
 *      or no response yet.
 *
 * Known conservatism (all under-estimating, never over-reporting): dep injections and
 * the tail todo reminder (constant-size additions) are omitted; a running member counts
 * only its own turn's increment. */

/* Estimate of a turn from `from`. This used to walk every message in the queue; the
 * per-message estimates are stored with the rows now, so it is one indexed SUM. */
static int64_t est_turn_from(struct edc_turn *turn, int from)
{
    if (!turn)
        return 0;
    if (from < 0)
        from = 0;
    if (from >= turn->msg_count)
        return 0;
    return edc_turn_tokens_from(turn, from);
}

extern int64_t edc_context_group_ctx_tokens(struct edc_task_group *group, bool *out_estimated)
{
    if (out_estimated)
        *out_estimated = false;
    if (!group)
        return 0;

    int64_t best = 0;
    bool best_estimated = false;

    struct edc_task *t;
    /* 1) anchor + tail */
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue; /* a fold task is not conversation content (same rule as the fold ledger) */
        if (t->last_total_tokens <= 0 || t->last_hist_len <= 0)
            continue; /* no anchor, or no position */
        if (t->last_fold_epoch != group->fold_epoch)
            continue; /* the anchor recorded the pre-fold prefix */

        int64_t tail = 0;
        {
            const int n = edc_task_group_history_count(group);
            const int from = t->last_hist_len;
            if (from < n)
                tail += edc_task_group_history_tokens(group, from, n);
        }
        tail += est_turn_from(t->active_turn, t->last_turn_len);

        int64_t candidate = t->last_total_tokens + tail;
        if (candidate > best)
        {
            best = candidate;
            best_estimated = (tail > 0);
        }
    }
    if (best > 0)
    {
        if (out_estimated)
            *out_estimated = best_estimated;
        return best;
    }

    /* 2) no usable anchor: estimate what the group's next request would carry */
    int64_t est = 0;
    if (group->memory)
        est += edc_token_estimate_text(group->memory);
    /* The blocks are rendered on top of the body at request time; leaving them out here
     * would make the estimate low by exactly the part the model is told about. */
    est += edc_fold_files_token_estimate(group->fold_files);
    if (edc_task_group_history_count(group) > 0)
    {
        /* The history only holds what is still sent (folded messages are pruned at
         * landing), so the tail starts right after the system prompt. Token accounting is
         * a SUM over the stored estimates -- never a walk that parses the messages. */
        est += edc_task_group_history_tokens(group, 0, -1);
    }
    /* A running member carries its own turn; a fresh task does not -- take the widest,
     * consistent with "group = its widest conversation". */
    int64_t widest_turn = 0;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        int turn_from = (t->active_turn && t->active_turn->folded_prefix > 0) ? t->active_turn->folded_prefix : 0;
        int64_t turn_est = est_turn_from(t->active_turn, turn_from);
        if (turn_est > widest_turn)
            widest_turn = turn_est;
    }
    est += widest_turn;

    if (out_estimated && est > 0)
        *out_estimated = true;
    return est;
}
