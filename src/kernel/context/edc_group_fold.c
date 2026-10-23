#include "edc_group_fold.h"
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_datetime_utils.h"
#include "edc_fold_files.h"
#include "edc_init_env.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* L1 fold: finished tasks' turns -> group memory.
 *
 * Triggered when unfolded messages reach the threshold (default 200); the fold
 * itself summarizes only the oldest half, so half the memory stays detailed.
 * Only turns are folded, never task->output. The prompt building and threshold
 * logic are unchanged from the old memory_actor. */

/* Fold task system prompt (pi §3): the summary is a **separate one-shot call**,
 * not a continuation of the conversation, so the prompt has to forbid continuing
 * or answering it. The request builder puts it in front of the fold prompt
 * (edc_llm_request); the fold task shares no group context. */
static const char *kFoldSystemPrompt =
    "You are a context summarization assistant. Read the transcript and produce (or update) a structured "
    "summary of it. Do NOT continue the conversation, do NOT answer anything in it, and do NOT call tools. "
    "Output ONLY the summary.";

const char *edc_group_fold_system_prompt(void)
{
    return kFoldSystemPrompt;
}

/* ── Summary prompt (P2b, pi §3/§8) ─────────────────────────────────────
 * One independent request, assembled in two parts:
 *   <group-transcript> corpus </group-transcript>
 *   [<previous-summary> old memory </previous-summary>]  <- old memory = iterative update
 *   {first-time / iterative} instruction
 *   [Additional focus: <focus>]                          <- /group fold's focus term
 * The first time uses the full six-section format; iteration asks to "keep what still
 * holds + merge incrementally + reorder Next Steps" and outputs the COMPLETE new
 * summary -- landing replaces (`group->memory = output`), never concatenates. */
static const char *kInitialSummaryInstruction =
    "Summarize the transcript above as markdown with exactly these sections:\n"
    "\n"
    "## Goal\n"
    "[what the user wants to achieve; multiple items allowed]\n"
    "\n"
    "## Constraints & Preferences\n"
    "- [constraints / preferences / requirements the user stated]\n"
    "- [(none) if none]\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [completed work]\n"
    "\n"
    "### In Progress\n"
    "- [ ] [current work]\n"
    "\n"
    "### Blocked\n"
    "- [what blocks progress, if anything]\n"
    "\n"
    "## Key Decisions\n"
    "- **[decision]**: [brief reason]\n"
    "\n"
    "## Next Steps\n"
    "1. [ordered next steps]\n"
    "\n"
    "## Critical Context\n"
    "- [data / examples / references needed to continue]\n"
    "- [(none) if none]\n"
    "\n"
    "Keep every section concise. Preserve exact file paths, function names and error messages.";

/* pi §5's split turn: when the corpus ends on an UNFINISHED turn, the memory must
 * additionally carry "where that turn got to", or the resuming half sees only a
 * conversation-level summary and does not know what it is taking over.
 *
 * Our difference from pi: pi sends a SECOND call for the turn prefix (a dedicated
 * prompt, budget 0.5 x reserve); we use one call -- summary and turn context share the
 * same max_tokens (EDC_FOLD_SUMMARY_MAX_TOKENS = 0.8 x reserve). The merge format is
 * copied from pi:
 *   {history summary}
 *   ---
 *   **Turn Context (split turn):**
 *   {turn prefix}
 */
static const char *kSplitTurnInstruction =
    "\n\nThe transcript also ends in the middle of a turn that continues afterwards (a split turn). "
    "After the sections above, add a `---` separator and then:\n"
    "\n"
    "**Turn Context (split turn):**\n"
    "- Original Request: [what that turn was asked to do]\n"
    "- Early Progress: [what it did before the summary cut in]\n"
    "- Context for Suffix: [what the continuation needs to know]";

static const char *kUpdateSummaryInstruction =
    "Update the previous summary with the transcript above. Keep every piece of information that is still "
    "true, merge in what is new, move finished items from \"In Progress\" to \"Done\", refresh \"Next Steps\" "
    "and \"Critical Context\", and keep the same sections. Preserve exact file paths, function names and error "
    "messages. Output the complete updated summary, not a diff.";

/* ── Folding is a barrier ────────────────────────────────────
 *
 * A fold reads the whole group's turns and writes group->memory. If the group's tasks
 * are still advancing (committing turns, appending group history), the fold sees no
 * stable snapshot -- and the next request's context would burst the window. So the
 * fold YIELDS NOTHING: at fold start every non-terminal member is hung on the fold
 * task (SEQUENCE) and parks in AWAIT_DEPS until the fold lands:
 *   - READY / AWAIT_DEPS members: edc_task_add_dependency attaches the edge directly
 *     (a READY task goes to AWAIT_DEPS);
 *   - members already in flight (AWAIT_EXEC / AWAIT_TOOLS): NOT interrupted (this round
 *     was sent / the tool is running). The edge still attaches and takes effect at
 *     their next request boundary -- in tool_done the executor sees
 *     edc_task_has_unresolved_deps true, sends no next LLM request and parks in
 *     AWAIT_DEPS (B1: tool results are still received, only the next request is
 *     blocked). After the fold lands it is redispatched with compressed context.
 * Inputs arriving while a fold is in flight / tasks released back, are caught by the
 * edc_group_fold_holds_group gate (they are READY, not in a dep).
 *
 * The fold task is now an ORDINARY PERSISTED task (sink = GROUP_MEMORY, lifetime =
 * PERSISTENT), so its dep edges persist with the members and read back after a restart;
 * a restart-interrupted fold task is set back to ready at load and redispatched by the
 * scan (see edc_state_serializer.c). */

bool edc_group_fold_in_flight(struct edc_task_group *group)
{
    if (!group || !group->session)
        return false;
    struct edc_task_group *sys = edc_session_find_system_group(group->session);
    if (!sys)
        return false;

    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        /* This group is folding (GROUP_MEMORY) or CURATING (SESSION_MEMORY) = a
         * non-terminal memory task is working on it => all group work stops. Unified
         * into one rule: L2 and the fold are built together, so "members resume only
         * after curation" holds for ALL members (including those that joined the group
         * mid-flight -- they have no explicit dep edge and are held by this predicate). */
        if (edc_task_is_memory_sink(t) && t->sink_group == group && !edc_task_is_completed(t) &&
            !edc_task_is_cancelled(t) && !edc_task_has_failed(t))
            return true;
    }
    return false;
}

int edc_group_fold_hold_task(struct edc_task *task)
{
    /* The maintenance rounds themselves are never held (their own barrier must not grab them).
     * NOTE: a FINISHED task is deliberately NOT skipped: the resume door hands us exactly those
     * (INTERRUPTED / ERROR / CANCELLED all count as finished), and holding them is the point --
     * resume then parks them at READY until the round ends. */
    if (!task || !task->group || !task->group->session)
        return 0;
    if (edc_task_is_memory_sink(task))
        return 0;

    struct edc_task_group *sys = edc_session_find_system_group(task->group->session);
    if (!sys)
        return 0;

    int added = 0;
    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        if (t == task || edc_task_is_finished(t))
            continue;
        /* L1 (the fold) and L2 (its curation) both carry sink_group == the folded group, and
         * the members wait for BOTH: L1 produces the memory, L2 reconciles it into the index. */
        if (!edc_task_is_memory_sink(t) || t->sink_group != task->group)
            continue;
        if (edc_task_add_dependency(task, t, TASK_DEP_BARRIER))
            added++;
    }
    return added;
}

void edc_group_fold_note_failure(struct edc_task_group *group)
{
    if (!group)
        return;
    group->fold_failures++;
    if (group->fold_failures >= EDC_FOLD_MAX_FAILURES)
        log_error("L1 fold: group %s failed %d times in a row — automatic folding is paused for this group "
                  "(do new work in it, or run /group fold %s, to try again)",
                  group->group_id, group->fold_failures, group->group_id);
    else
        log_info("L1 fold: group %s fold failure %d/%d", group->group_id, group->fold_failures,
                 EDC_FOLD_MAX_FAILURES);
}

void edc_group_fold_note_progress(struct edc_task_group *group)
{
    if (!group || group->fold_failures == 0)
        return;
    log_info("L1 fold: group %s failure streak cleared (was %d)", group->group_id, group->fold_failures);
    group->fold_failures = 0;
}

bool edc_group_fold_due(struct edc_task_group *group, const struct edc_config *conf)
{
    if (!group || edc_task_group_count_foldable(group) == 0)
        return false;

    /* (c) Consecutive failures at the cap => automatic folding stops (manual
     * /group fold goes through start and skips this gate). Without it, "fail -> group
     * still due -> retry unchanged" is a perpetual motion machine: measured ~one real
     * call every ~40s, members parked over and over. */
    if (group->fold_failures >= EDC_FOLD_MAX_FAILURES)
        return false;

    /* The only predicate = the token axis (the message-count axis is fully retired).
     * config's default window is 1M and "<= 0" is repaired to the default at init, so a
     * count branch would be unreachable dead code. When the window still cannot be read
     * (a hand-edited config), do NOT fold: not knowing the window is no reason to fold
     * blindly, and manual /group fold is always available. */
    int window = conf ? conf->context_window : 0;
    if (window <= 0)
        return false;
    long long ctx = edc_context_group_ctx_tokens(group, NULL);
    return ctx > (long long)window - EDC_FOLD_RESERVE_TOKENS;
}

bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions, struct edc_config *conf,
                        struct edc_session **out_session)
{
    if (out_session)
        *out_session = NULL;
    if (!sched || !sessions)
        return false;

    /* Scan every session (a background session folds too) and fold at most one
     * group at a time. */
    struct edc_session_manager *sm = sessions;
    for (int si = 0; si < sm->session_count; si++)
    {
        struct edc_session *sess = sm->sessions[si];
        if (!sess)
            continue;
        if (edc_scheduler_has_pending_system_task(sess))
            continue;

        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            size_t foldable = edc_task_group_count_foldable(g);
            size_t unfolded = edc_task_group_unfolded_msg_count(g);

            if (edc_group_fold_due(g, conf))
            {
                log_info("L1 fold trigger: group %s foldable=%zu unfolded=%zu (window=%d)", g->group_id,
                         foldable, unfolded, conf ? conf->context_window : 0);
                /* Not startable right now (a member is still running) → keep looking
                 * at this session's other groups instead of declaring one started. */
                if (edc_group_fold_start(sched, sess, g, NULL))
                {
                    if (out_session)
                        *out_session = sess;
                    return true; /* fold one group at a time */
                }
            }
            if (foldable > 0 || unfolded > 0)
            {
                log_debug("L1 fold check: group %s foldable=%zu unfolded=%zu", g->group_id, foldable, unfolded);
            }
        }
    }
    return false;
}

/* ── Selection (P2a-4): keep keep_recent of detail from the tail, older goes to the
 * corpus ──────────────────────────────────────────────────────────────────────
 *
 * Content order = completed members by completion_seq, with IN-FLIGHT ones last (their
 * content is newest); inside a member by turn order, inside a turn by message order, and
 * only the unfolded span (after turn->folded_prefix) counts. The cut always lands on a
 * message boundary: a message goes wholly to the corpus or stays wholly as detail.
 *
 * P2a-4a folds only COMMITTED turns (task->turns); an in-flight task's active turn is
 * not among them -- it is uncommitted and the request side carries it. Cutting inside it
 * is P2a-4b. */

struct fold_piece
{
    struct edc_task *task;
    struct edc_turn *turn;
    size_t turn_index; /* index in the member's turns queue (used to find the same turn at landing) */
    size_t msg_index;  /* absolute index within that turn */
    long long tokens;
};

/* Flatten a turn's UNFOLDED span into that column. turn_index = the turn's index in the
 * member's turns queue; an in-flight active turn uses the EDC_FOLD_CUT_ACTIVE_TURN
 * sentinel. */
static void push_pieces(struct fold_piece **ps, size_t *cnt, size_t *cap, struct edc_task *t, struct edc_turn *turn,
                        size_t turn_index)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t from = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if (from > len)
        from = len;
    for (size_t mi = from; mi < len; mi++)
    {
        if (*cnt == *cap)
        {
            *cap = *cap ? *cap * 2 : 64;
            *ps = realloc(*ps, *cap * sizeof(**ps));
        }
        struct fold_piece *fp = &(*ps)[(*cnt)++];
        fp->task = t;
        fp->turn = turn;
        fp->turn_index = turn_index;
        fp->msg_index = mi;
        fp->tokens = edc_token_estimate_msg(turn->messages.data[turn->messages.head + mi]);
    }
}

/* Whether a piece's role is "tool" (a tool result). pi §4: a cut never lands on a tool
 * result -- with its assistant(tool_calls) folded away it becomes an orphan and the
 * whole request fails. */
static bool piece_is_tool_result(const struct fold_piece *fp)
{
    cJSON *m = fp->turn->messages.data[fp->turn->messages.head + fp->msg_index];
    cJSON *role = cJSON_GetObjectItem(m, "role");
    return role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0;
}

/* Flatten members' unfolded messages into one column (content order). */
static size_t collect_pieces(struct edc_task *const *members, size_t n, struct fold_piece **out)
{
    struct fold_piece *ps = NULL;
    size_t cnt = 0, cap = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
            push_pieces(&ps, &cnt, &cap, t, t->turns.data[qi], qi);

        /* In-flight member (P2a-4b): the active turn is uncommitted but is the NEWEST
         * content. Folding its prefix is pi's split turn -- after landing the request
         * starts after turn->folded_prefix (req->turn_begin lights up). Only ALIVE
         * members count as in-flight: an INTERRUPTED / CANCELLED half-turn is
         * not part of the current conversation and takes the normal path when it
         * resumes. */
        if (t->life == EDC_LIFE_ALIVE && t->active_turn)
            push_pieces(&ps, &cnt, &cap, t, t->active_turn, EDC_FOLD_CUT_ACTIVE_TURN);
    }
    *out = ps;
    return cnt;
}

/* ── File tracking (R5b) ──────────────────────────────────────────────────────
 *
 * A fold drops the turns it summarizes, and with them the tool calls that named the files
 * the task read and changed. What survives is a narrative: the model knows what happened but
 * not where it happened, so a long task re-reads files it already read and cannot answer
 * "what did you change".
 *
 * The summary therefore ends with two blocks, pi's shape:
 *
 *   <read-files>
 *   src/a.c
 *   </read-files>
 *   <modified-files>
 *   src/b.c
 *   </modified-files>
 *
 * The content is EXTRACTED FROM THE TOOL CALLS in code, never asked of the model: extraction
 * is deterministic, cumulative across folds, and cannot be forgotten halfway through a
 * summary.
 *
 * The lists themselves are DATA (`struct edc_fold_files`, edc_fold_files.h) and live on the
 * GROUP, not in the memory text: the two blocks above are that data's projection, rendered
 * fresh on every request. Keeping the data out of the text is what stops a summary that
 * rewrites a block from corrupting the accumulated list. */

/* The file a tool call names, classified: reader (read_file) or writer (write_file /
 * edit_file). Everything else is ignored -- `list_dir` names a DIRECTORY (a directory in a
 * file list is noise and pi's `ls` is not tracked either), device_* paths are on the device,
 * crontab/todo have no paths, and guessing would pollute a list that travels in every
 * request from now on. */
static void collect_tool_call_file(cJSON *call, struct edc_fold_files *files)
{
    cJSON *fn = cJSON_GetObjectItem(call, "function");
    cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
    cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
    if (!files || !name || !cJSON_IsString(name) || !args)
        return;

    const bool is_read = (strcmp(name->valuestring, "read_file") == 0);
    const bool is_write =
        (strcmp(name->valuestring, "write_file") == 0) || (strcmp(name->valuestring, "edit_file") == 0);
    if (!is_read && !is_write)
        return;

    /* On the wire `arguments` is a JSON *string*; tolerate an object for hand-built history. */
    cJSON *parsed = cJSON_IsString(args) ? cJSON_Parse(args->valuestring) : args;
    cJSON *path = parsed ? cJSON_GetObjectItem(parsed, "path") : NULL;
    if (path && cJSON_IsString(path))
    {
        if (is_read)
            edc_fold_files_add_read(files, path->valuestring);
        else
            edc_fold_files_add_modified(files, path->valuestring);
    }
    if (parsed != args)
        cJSON_Delete(parsed);
}

static void collect_message_files(cJSON *msg, struct edc_fold_files *files)
{
    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (!cJSON_IsArray(calls))
        return;
    cJSON *call = NULL;
    cJSON_ArrayForEach(call, calls) collect_tool_call_file(call, files);
}

/* A newline-separated view of one list (NULL when empty), for the callers that want text
 * rather than the structure (`/group files`, `/group info`). This is a projection, not a
 * parse: the bytes come from OUR arrays, never from a model-written string. */
static sds list_to_text(char *const *list, size_t n)
{
    if (!list || n == 0)
        return NULL;
    sds out = sdsempty();
    for (size_t i = 0; i < n; i++)
        out = sdscatprintf(out, "%s\n", list[i]);
    return out;
}

int edc_group_fold_tracked_files(const struct edc_task_group *group, sds *out_reads, sds *out_writes)
{
    const struct edc_fold_files *files = group ? group->fold_files : NULL;
    if (out_reads)
        *out_reads = files ? list_to_text(files->read, files->read_n) : NULL;
    if (out_writes)
        *out_writes = files ? list_to_text(files->modified, files->modified_n) : NULL;
    return (int)edc_fold_files_count(files);
}

/* The implementation; the public wrapper below keeps its three-argument shape for the tests
 * and callers that only want the prompt. */
static sds fold_build_prompt(struct edc_task_group *group, const char *focus, struct edc_fold_cut *out_cut,
                             struct edc_fold_files **out_files)
{
    if (out_cut)
    {
        out_cut->task_id = NULL;
        out_cut->turn = 0;
        out_cut->prefix = 0;
    }
    if (!group)
        return NULL;

    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct fold_piece *ps = NULL;
    size_t cnt = collect_pieces(members, n, &ps);
    if (cnt == 0)
    {
        log_debug("L1 fold: group %s has nothing unfolded to fold", group->group_id);
        free(members);
        free(ps);
        return NULL;
    }

    long long total = 0;
    for (size_t i = 0; i < cnt; i++)
        total += ps[i].tokens;

    /* How many messages to keep as detail. pi: keep_recent = a 20k-token tail. When the
     * whole group is under that budget (a small group whose corpus is smaller than its
     * summary, or a manual /group fold on a tiny group) there is no "window" to compare
     * -- compress the oldest half, or a small group would never fold. */
    size_t keep = 0;
    if (total <= EDC_FOLD_KEEP_RECENT_TOKENS)
    {
        keep = cnt / 2;
        log_info("L1 fold: group %s is under keep_recent (%lld <= %d tokens) — folding the oldest half (%zu of %zu msgs)",
                 group->group_id, total, EDC_FOLD_KEEP_RECENT_TOKENS, cnt - keep, cnt);
    }
    else
    {
        long long acc = 0;
        size_t k = 0;
        for (size_t i = cnt; i > 0; i--)
        {
            acc += ps[i - 1].tokens;
            k++;
            if (acc >= EDC_FOLD_KEEP_RECENT_TOKENS)
                break;
        }
        keep = (k < cnt) ? k : cnt - 1; /* fold at least one, or the fold is pointless */
    }
    size_t cut_at = cnt - keep; /* corpus = ps[0, cut_at) */

    /* (b) Corpus cap: keep_recent says "how much detail to keep", not "how much this
     * call compresses". The tail budget only guarantees 20k kept; the rest can be a
     * whole history -- pushing >10:1 onto the summarizer, which must truncate (and a
     * truncated result is discarded => the fold never lands => retry unchanged; see the
     * header). Here the cut moves back until the corpus fits
     * EDC_FOLD_MAX_CORPUS_TOKENS; if it cannot (the first message alone exceeds the
     * cap) keep one and let the failure cap above catch it -- never move the cut to 0,
     * which is a "due but yields nothing" spin. */
    long long corpus = 0;
    for (size_t i = 0; i < cut_at; i++)
        corpus += ps[i].tokens;
    while (cut_at > 1 && corpus > EDC_FOLD_MAX_CORPUS_TOKENS)
    {
        cut_at--;
        corpus -= ps[cut_at].tokens;
    }

    /* Cut validity (pi §4): the first kept message must not be a tool result -- its
     * assistant(tool_calls) was folded into memory, and an orphan tool message fails the
     * whole request. Move back to that assistant (its tool result then stays too), i.e.
     * "rather fold one fewer". */
    while (cut_at > 0 && cut_at < cnt && piece_is_tool_result(&ps[cut_at]))
        cut_at--;

    if (cut_at == 0)
    {
        free(members);
        free(ps);
        return NULL;
    }

    /* The cut = the first kept message: its member + its turn + its index in the turn.
     * Keeping none (keep == 0, possible only when the whole group has one unfolded
     * message) cuts AFTER the last one, i.e. "fold all" (landing pushes the last turn's
     * prefix to its length). */
    const struct fold_piece *cut_piece = &ps[cut_at < cnt ? cut_at : cnt - 1];
    uint32_t cut_prefix =
        (cut_at < cnt) ? (uint32_t)cut_piece->msg_index : (uint32_t)cut_piece->msg_index + 1;
    if (out_cut)
    {
        out_cut->task_id = cut_piece->task->task_id;
        out_cut->turn = (uint32_t)cut_piece->turn_index;
        out_cut->prefix = cut_prefix;
    }
    log_info("L1 fold: group %s fold %zu/%zu msgs (%lld corpus tokens, cap %d; keep_recent ~%d), "
             "cut at task %s turn %zu msg %u",
             group->group_id, cut_at, cnt, corpus, EDC_FOLD_MAX_CORPUS_TOKENS, EDC_FOLD_KEEP_RECENT_TOKENS,
             cut_piece->task->task_id, cut_piece->turn_index, cut_prefix);

    bool has_memory = (group->memory && sdslen(group->memory) > 0);
    sds prompt = sdsempty();
    prompt = sdscat(prompt, "<group-transcript>\n");

    struct edc_task *cur = NULL;
    for (size_t i = 0; i < cut_at; i++)
    {
        struct edc_task *tk = ps[i].task;
        if (tk != cur)
        {
            cur = tk;
            prompt = sdscatprintf(prompt, "\n### [task %s] %s\n", tk->task_id, tk->description ? tk->description : "");
        }
        cJSON *m = ps[i].turn->messages.data[ps[i].turn->messages.head + ps[i].msg_index];
        if (out_files)
            collect_message_files(m, *out_files);
        if (cJSON_GetStringValue(cJSON_GetObjectItem(m, "role")))
        {
            /* One line, with the message's wall-clock time and (for a tool call) its
             * name + arguments -- see edc_turn_render_message. */
            sds line = edc_turn_render_message(m);
            prompt = sdscatprintf(prompt, "%s\n", line);
            sdsfree(line);
        }
    }

    /* Only members ENTIRELY in the corpus carry output (a half-folded member keeps its tail as detail). */
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *tk = members[i];
        if (tk == cut_piece->task)
            continue; /* the cut's member: half-folded, no output */
        bool in_corpus = false;
        for (size_t j = 0; j < cut_at && !in_corpus; j++)
            if (ps[j].task == tk)
                in_corpus = true;
        if (in_corpus && tk->output)
            prompt = sdscatprintf(prompt, "output: %s\n", tk->output);
    }

    prompt = sdscat(prompt, "</group-transcript>\n\n");
    if (has_memory)
    {
        prompt = sdscat(prompt, "<previous-summary>\n");
        prompt = sdscat(prompt, group->memory);
        prompt = sdscat(prompt, "\n</previous-summary>\n\n");
    }
    /* The corpus ends on an unfinished turn (pi §5): the cut falls inside a turn (later
     * messages stay as detail), or the last corpus piece was already a running active
     * turn (content not finished generating). */
    bool split_turn = false;
    if (cut_at < cnt && cut_prefix > 0)
        split_turn = true; /* cut mid-turn */
    if (cut_at > 0 && ps[cut_at - 1].turn_index == EDC_FOLD_CUT_ACTIVE_TURN)
        split_turn = true; /* the corpus tail is an in-flight turn */
    if (split_turn)
        log_info("L1 fold: group %s split turn at the cut (pi §5 turn context section)", group->group_id);

    prompt = sdscat(prompt, has_memory ? kUpdateSummaryInstruction : kInitialSummaryInstruction);
    if (split_turn)
        prompt = sdscat(prompt, kSplitTurnInstruction);
    if (focus && *focus)
    {
        prompt = sdscat(prompt, "\n\nAdditional focus: ");
        prompt = sdscat(prompt, focus);
    }
    prompt = sdscat(prompt, "\n");

    free(members);
    free(ps);
    return prompt;
}

sds edc_group_fold_build_prompt(struct edc_task_group *group, const char *focus, struct edc_fold_cut *out_cut)
{
    return fold_build_prompt(group, focus, out_cut, NULL);
}

struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                      struct edc_task_group *group, const char *focus)
{
    if (!sched || !session || !group)
        return NULL;

    /* One fold per session at a time, or the second prompt reads unwritten state. */
    if (edc_scheduler_has_pending_system_task(session))
        return NULL;

    struct edc_fold_cut cut;
    memset(&cut, 0, sizeof(cut));
    struct edc_fold_files *files = edc_fold_files_new();
    sds prompt = fold_build_prompt(group, focus, &cut, &files);
    if (!prompt)
    {
        edc_fold_files_free(files);
        return NULL;
    }

    struct edc_task *task = edc_scheduler_spawn_system_task(sched, session, "fold group memory");
    if (!task)
    {
        sdsfree(prompt);
        edc_fold_files_free(files);
        return NULL;
    }
    /* The files travel with the task: landing is where the memory is written, and the turns
     * that named them are gone by then (see edc_group_fold_deliver_memory). */
    task->fold_files = files;
    task->sink_group = group; /* output goes into this group's memory (sink = GROUP_MEMORY) */
    /* The corpus and the cut are computed in one pass, so landing applies this cut. */
    task->fold_cut_task_id = cut.task_id ? strdup(cut.task_id) : NULL;
    task->fold_cut_turn = cut.turn;
    task->fold_cut_prefix = cut.prefix;

    /* No shared header any more: the fold request carries the instruction itself
     * (see edc_group_fold_system_prompt). The old version wrote it into the system
     * group's history[0] — which is not persisted, so after a restart the loader
     * refilled that slot with the agent's main system prompt and the "written once"
     * guard never corrected it: every fold call then ran with EdgeClaw's persona
     * and MEMORY.md in front of the transcript. */

    log_info("L1 fold start: group %s task=%s prompt=%zu bytes, cut=task %s turn %u msg %u", group->group_id,
             task->task_id, sdslen(prompt), task->fold_cut_task_id ? task->fold_cut_task_id : "(none)",
             task->fold_cut_turn, task->fold_cut_prefix);

    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L1 fold: could not attach the fold prompt for group %s — no fold this round", group->group_id);
        /* Do not leave a half-built task; marking it failed also releases any
         * dependents (SEQUENCE). */
        edc_task_mark_failed(task);
        sdsfree(prompt);
        return NULL;
    }

    /* Barrier: every non-terminal member now waits for this fold (see the note at
     * the top of the file). A READY member takes the SEQUENCE edge and lands in
     * AWAIT_DEPS at once; a member already in flight keeps running, and the
     * executor parks it at its next request boundary (tool_done refuses the next
     * LLM send while a dep is outstanding). Either way the release is the same:
     * the fold completing notifies its dependents. */
    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
    {
        if (edc_task_is_completed(m) || edc_task_is_cancelled(m) || edc_task_has_failed(m))
            continue;
        edc_task_add_dependency(m, task, TASK_DEP_BARRIER);
    }

    /* L2 and the fold are built TOGETHER: the whole chain is visible at once
     *     members --SEQUENCE--> L2 --JOIN--> L1
     * L1 runs first; only after it lands is L2 driven (the JOIN edge plus injecting its
     * output as material); members wait for both. L1 failing/cancelling => L2 fails/
     * cancels automatically (JOIN cascade) => members are released -- no extra logic. */
    edc_session_memory_start(sched, session, group, task);

    /* The prompt is dispatched by the message-end scan, like any task. */
    log_info("L1 fold: prompt attached to system task %s (it will be dispatched by the scheduler)",
             task->task_id);

    sdsfree(prompt);
    return task;
}

/* Landing half of the fold: write the memory into task->sink_group, mark the
 * selected members folded, advance the watermark. On failure nothing changes and
 * the next round retries. */
static void fold_turn_whole(struct edc_turn *turn, size_t *turns, size_t *msgs)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t cur = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if (cur >= len)
        return;
    *msgs += len - cur;
    (*turns)++;
    turn->folded_prefix = (int)len;
    /* Folded messages are in the group memory, so the chained curation puts them in
     * L2: the extraction watermark must not lag behind the fold watermark. */
    if (turn->memo_prefix < turn->folded_prefix)
        turn->memo_prefix = turn->folded_prefix;
}

/* Fold only the first |prefix| messages (the prefix only grows, never regresses). */
static void fold_turn_prefix(struct edc_turn *turn, uint32_t prefix, size_t *turns, size_t *msgs)
{
    size_t len = edc_queue_length(&turn->messages);
    size_t cur = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if ((size_t)prefix <= cur || (size_t)prefix > len)
        return;
    *msgs += (size_t)prefix - cur;
    (*turns)++;
    turn->folded_prefix = (int)prefix;
    if (turn->memo_prefix < turn->folded_prefix)
        turn->memo_prefix = turn->folded_prefix;
}

/* The newest wall-clock among the messages the group memory now covers: for every
 * member's committed turns (and a live member's active turn), the leading
 * [0, folded_prefix) messages. This is the "up to when does the memory know" bound a
 * curator uses to judge whether new material is newer or older than the memory. Keeps
 * the previous value when nothing parseable is found (e.g. messages added without ts). */
static uint64_t group_memory_covers_to(struct edc_task_group *group)
{
    uint64_t best = group->memory_covers_to;
    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            size_t len = edc_queue_length(&turn->messages);
            size_t upto = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
            if (upto > len)
                upto = len;
            for (size_t mi = 0; mi < upto; mi++)
            {
                cJSON *m = turn->messages.data[turn->messages.head + mi];
                const char *ts = cJSON_GetStringValue(cJSON_GetObjectItem(m, "ts"));
                time_t v;
                if (ts && edc_datetime_parse(ts, &v) && (uint64_t)v > best)
                    best = (uint64_t)v;
            }
        }
    }
    free(members);
    return best;
}

bool edc_group_fold_deliver_memory(struct edc_task *task, const char *content)
{
    struct edc_task_group *group = task ? task->sink_group : NULL;
    if (!group || !content || !*content)
    {
        log_error("L1 fold: task %s produced no memory (target=%s, content=%s) — group memory unchanged",
                  (task && task->task_id) ? task->task_id : "(null)", group ? "yes" : "none",
                  (content && *content) ? "non-empty" : "empty");
        /* (c) No memory written = not landed; still counts as one failure. */
        edc_group_fold_note_failure(group);
        return false;
    }

    /* The lists are data on the group; `group->memory` is the summary body ONLY. The two
     * blocks are rendered from the data at request time (edc_context.c), so a summary that
     * echoes the old blocks cannot pollute the accumulated list -- `body` strips them. */
    if (task->fold_files)
    {
        if (!group->fold_files)
            group->fold_files = edc_fold_files_new();
        edc_fold_files_merge(group->fold_files, task->fold_files);
        edc_fold_files_free(task->fold_files);
        task->fold_files = NULL;
    }
    sds body = edc_fold_files_body(content);
    if (group->memory)
        sdsfree(group->memory);
    group->memory = body;
    /* Wall-clock about the memory: first write + last write. The covered-to bound is
     * refreshed only where the cut really advanced (below), so a landing that wrote the
     * memory without folding anything does not claim new coverage. */
    uint64_t now = (uint64_t)edc_datetime_now();
    if (group->memory_created_at == 0)
        group->memory_created_at = now;
    group->memory_updated_at = now;

    /* Apply the cut: members BEFORE it in content order fold whole (including an
     * in-flight member entirely in the corpus -- its active turn folds whole too and the
     * request side slices by prefix); the cut's member folds up to its
     * fold_cut_prefix; nothing after. An empty cut = an in-flight fold task written by an
     * older binary (no cut info) -- the memory was written but the prefix cannot be
     * applied; count one failure (no fallback/retry). */
    if (!task->fold_cut_task_id)
    {
        log_error("L1 fold: task %s carries no cut (older format) — group %s memory updated but no prefix applied",
                  task->task_id, group->group_id);
        edc_task_group_rebuild_history(group);
        group->fold_epoch++;
        edc_group_fold_note_failure(group); /* (c) no prefix applied = nothing really folded */
        return true; /* memory written -- new material to L2 */
    }

    /* Resolve the cut EXACTLY first (member + that turn); if it cannot be resolved,
     * write the memory and fold nothing. Rather a wasted round (content stays in history,
     * at worst duplicating the memory) than folding messages the memory does not cover --
     * that is silent data loss. */
    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct edc_task *cut_member = NULL;
    for (size_t i = 0; i < n && !cut_member; i++)
    {
        if (strcmp(members[i]->task_id, task->fold_cut_task_id) == 0)
            cut_member = members[i];
    }
    struct edc_turn *cut_turn = NULL;
    if (cut_member)
    {
        if ((uint32_t)task->fold_cut_turn == EDC_FOLD_CUT_ACTIVE_TURN)
            cut_turn = cut_member->active_turn
                           ? cut_member->active_turn
                           : (cut_member->turns.tail > cut_member->turns.head
                                  ? cut_member->turns.data[cut_member->turns.tail - 1]
                                  : NULL);
        else if ((size_t)task->fold_cut_turn >= cut_member->turns.head &&
                 (size_t)task->fold_cut_turn < cut_member->turns.tail)
            cut_turn = cut_member->turns.data[(size_t)task->fold_cut_turn];
    }
    if (!cut_member || !cut_turn)
    {
        log_error("L1 fold: cut (task=%s turn=%u) does not resolve in group %s — memory updated, nothing folded",
                  task->fold_cut_task_id, task->fold_cut_turn, group->group_id);
        free(members);
        edc_task_group_rebuild_history(group);
        group->fold_epoch++;
        edc_group_fold_note_failure(group); /* (c) wasted round: nothing folded */
        return true; /* memory written -- L2 should still digest it (else this material never reaches the session) */
    }

    /* Apply the cut: content BEFORE it folds whole -- committed turns are always safe
     * (they do not change after commit); an in-flight turn folds whole only under the
     * same condition it entered the corpus (the corpus takes only ALIVE members' active
     * turns; the same condition here, else content never summarized would be folded);
     * the cut's turn folds its prefix only; nothing after. */
    size_t folded_turns = 0, folded_msgs = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        bool stop_after = false;
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            if (turn == cut_turn)
            {
                fold_turn_prefix(turn, task->fold_cut_prefix, &folded_turns, &folded_msgs);
                stop_after = true;
                break;
            }
            fold_turn_whole(turn, &folded_turns, &folded_msgs);
        }
        if (!stop_after && t->active_turn && t->active_turn != cut_turn && t->life == EDC_LIFE_ALIVE)
            fold_turn_whole(t->active_turn, &folded_turns, &folded_msgs);
        if (!stop_after && cut_turn && cut_turn == t->active_turn)
        {
            /* The cut is on the active turn (uncommitted, not in turns): the loop above
             * already folded all turns before it. */
            fold_turn_prefix(cut_turn, task->fold_cut_prefix, &folded_turns, &folded_msgs);
        }
        if (t == cut_member)
            break;
    }
    free(members);
    /* The history says what will be sent: drop what the memory now covers. */
    edc_task_group_rebuild_history(group);
    /* The prefix these members' requests counted is gone: every anchor older than
     * this is stale from here on (see the fold_epoch field). */
    group->fold_epoch++;
    /* The memory now covers up to the newest message it folded. */
    group->memory_covers_to = group_memory_covers_to(group);
    /* (c) Successful landing => clear the consecutive-failure count. */
    edc_group_fold_note_progress(group);
    log_info("L1 fold done: group %s folded %zu turns (%zu msgs), cut at task %s turn %u msg %u", group->group_id,
             folded_turns, folded_msgs, task->fold_cut_task_id, task->fold_cut_turn, task->fold_cut_prefix);
    return true;
}
