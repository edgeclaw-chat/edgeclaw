#ifndef EDC_STATE_STORE_H
#define EDC_STATE_STORE_H

/* ── Session state, persisted in SQLite ───────────────────────────────────────
 *
 * Replaces the `session.json` snapshot writer.
 * This header is the whole surface the rest of
 * the kernel sees.
 *
 * THE ONE INVARIANT callers must respect: only the scheduler actor thread touches the
 * store (see "the only thread allowed to touch the store is this one" in
 * edc_scheduler.c). SQLite itself would tolerate more, but the in-memory session tree
 * is not thread-safe and two writers would diverge from it.
 *
 * NO MIGRATION PATH, BY DESIGN. This is a development build: the schema has exactly one
 * version (`EDC_DB_VERSION`), and a database written by any other version is REFUSED at
 * startup with an instruction to delete the file. There is no import, no dual-write, no
 * rollback, and no compatibility decoding. See design doc §3.3.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct edc_config;
struct cJSON;
struct edc_session;
struct edc_scheduler;

/* Schema version. Bump together with any DDL change. Deliberately a single number: no
 * "structure version + semantics version" pair, because there is nothing to be
 * compatible with.
 *
 * v2: turns.msg_count / turns.token_count and messages.tokens. The message rows became
 *     the runtime's only copy of a conversation (the in-memory turn queue is gone), so
 *     the counts they need to answer without reading rows became columns.
 *
 * v3: messages was split in two. Its JSON column held the whole message INCLUDING the
 *     metadata that is also stored as columns (ts appeared both as an integer column and
 *     as a "YYYY-MM-DD HH:MM:SS" string inside the JSON), so every read and write
 *     converted between the two. The columns now hold all the metadata, `body` holds only
 *     what goes on the wire, and session_id/group_id are denormalized so a group's
 *     messages can be read without joining tasks. Every timestamp in this file is an
 *     INTEGER unix second -- there is exactly one representation. */
#define EDC_DB_VERSION 3

/* Open <workspace>/edgeclaw.db for the lifetime of the process.
 *
 * Refuses (returns false, logs why) when:
 *  - the database, its -wal or -shm is a symlink -- the workspace is a model-writable
 *    directory, and this exact hole was already exploited once with jobs.json;
 *  - the resolved database does not sit inside the resolved workspace (a workspace that
 *    is itself a symlink must not become an escape hatch);
 *  - another process already holds the workspace lock;
 *  - the filesystem is not a local one (SQLite's locking is not reliable over NFS/SMB);
 *  - the file carries a different EDC_DB_VERSION.
 *
 * Idempotent: a second call with the store already open returns true.
 * Returns false for a NULL cfg or one with no workspace. */
bool edc_state_store_open(struct edc_config *cfg);

/* Checkpoint the WAL and close. Safe to call when not open. */
void edc_state_store_close(void);

bool edc_state_store_is_open(void);

/* Absolute path of the open database, or NULL when closed. Borrowed, not a copy. */
const char *edc_state_store_path(void);

/* What one flush actually cost, for the stage-0 baseline probe (edc_state_probe.h).
 *
 * These are measured around the transaction and are NOT derived from the size of the
 * document: a snapshot byte count says how much JSON there was, not how much I/O the
 * store did, and after the move to SQLite those two numbers have no reason to match.
 * Every field is -1 when it could not be measured. */
struct edc_state_flush_cost
{
    /* Bytes the pager wrote during this flush: page_size x pages written, which in WAL
     * mode is the pages appended to <db>-wal (plus, if the commit happened to trigger the
     * auto-checkpoint, the pages that checkpoint copied back). This is the full-rewrite
     * amplification in I/O units.
     *
     * Deliberately NOT the growth of the -wal file: after an auto-checkpoint the WAL is
     * reused from offset 0, so a 17 MB flush can leave the file exactly as long as it
     * was. That measured 0 bytes for the second flush of a 4000-message session. */
    long long io_bytes;
    /* Rows inserted or deleted, FK cascades included: the O(history) side in the store's
     * own units (one row per message, task, turn, dep, tool call...). */
    long long rows;
};

/* Write one session's whole tree into the database, in one transaction.
 *
 * STAGE 1 IMPLEMENTATION: a full rewrite, not a change set. Everything belonging to the
 * session is deleted and re-inserted, so the cost is O(history) -- the same shape as the
 * session.json it replaces. That is deliberate: stage 1 exists to prove the mapping and
 * the load semantics are right (see the differential test), and only stage 2 makes the
 * write incremental. Do not read this function as the performance story.
 *
 * Returns false (with the transaction rolled back) on any SQL error; the previous
 * content is left untouched, so a failed flush cannot corrupt a readable session. */
bool edc_state_store_flush(struct edc_session *session, struct edc_scheduler *sched);

/* Same call, reporting what it cost. |out| may be NULL (the common case: nobody but the
 * baseline probe wants the numbers, and measuring needs a stat() of the WAL on both sides
 * of the transaction). Identical behaviour otherwise. */
bool edc_state_store_flush_measured(struct edc_session *session, struct edc_scheduler *sched,
                                    struct edc_state_flush_cost *out);

/* Rebuild one session from the database.
 *
 * Returns false when the session has no rows (a brand-new session -- not an error) or
 * when reading failed. On success the session owns freshly built groups/tasks, exactly
 * as edc_session_load_state would have left them, including every restart normalization
 * (ALIVE -> INTERRUPTED, end inferred from life, dependency convergence, fork chains,
 * sink groups, turn watermarks, history rebuild).
 *
 * The normalization itself is NOT reimplemented here: the rows are assembled back into
 * the same JSON shape edc_state_serialize() produces and handed to
 * edc_state_deserialize(). That was a deliberate trade for stage 1 -- duplicating the
 * normalization would be the single most likely way to lose a restart semantic, and the
 * one place it is implemented is already tested. Stage 2 can move to filling structs
 * directly once the column mapping has been proven by the differential test. */
bool edc_state_store_load(struct edc_session *session, struct edc_scheduler *sched);

/* Delete every row belonging to a session (`/session remove`). Cascades handle the
 * children. Returns false only on a SQL error. */
bool edc_state_store_forget(const char *session_id);

/* ── Message rows: the store IS where messages live ──────────────────────────
 *
 * A turn's messages are NOT held in memory between calls. `struct edc_turn` keeps the
 * count, the token total and the two watermarks; the messages themselves are rows, read
 * on demand. These are the only doors to them.
 *
 * Every function here returns false / NULL when the store is closed, so a caller that
 * forgot to open one fails loudly instead of quietly losing a conversation.
 *
 * Rows are keyed (task_id, turn_ord, msg_ord); msg_ord is positional and the unique index
 * messages_natural makes append idempotent, so replaying an append cannot duplicate a
 * message.
 */

/* Make sure the rows a message needs exist: session, its group, the task, the turn.
 *
 * WHY THIS IS NECESSARY: messages has FOREIGN KEY (task_id, turn_ord) -> turns, turns ->
 * tasks, tasks -> sessions. With foreign_keys=ON (which the store sets), a message INSERT
 * fails unless its whole ancestor chain is already on disk. A turn is created and gets its
 * first message BEFORE the task is saved, so the chain has to be created here, with the
 * columns that are NOT NULL filled and everything else left at its default -- the next
 * metadata flush overwrites them with the real values.
 *
 * INSERT OR IGNORE throughout: an existing row is never modified by this call. */
bool edc_state_ensure_turn(const char *session_id, const char *group_id, const char *task_id,
                           int turn_ord);

/* Append one message at |msg_ord|. |tokens| is the estimate computed once at append
 * time (0 = unknown), so token accounting is a range SUM instead of re-estimating every
 * message per request. The turn row must already exist (edc_state_ensure_turn). */
bool edc_msg_append(const char *task_id, int turn_ord, int msg_ord, const struct cJSON *msg,
                    long long tokens);

/* One message, parsed into a fresh object the CALLER owns (free with cJSON_Delete), or
 * NULL when the row does not exist. */
struct cJSON *edc_msg_get(const char *task_id, int turn_ord, int msg_ord);

/* How many messages the turn has, straight from the rows (0 when none / store closed). */
int edc_msg_turn_len(const char *task_id, int turn_ord);

/* Sum of the token estimates of messages [from, len). One indexed range scan; this
 * replaces walking every message in C. Returns -1 when the store is closed. */
long long edc_msg_turn_tokens_from(const char *task_id, int turn_ord, int from);

/* Drop messages with msg_ord >= |new_len| (revert / rollback). */
bool edc_msg_turn_truncate(const char *task_id, int turn_ord, int new_len);

/* Drop the whole turn's messages (turn removed). */
bool edc_msg_turn_clear(const char *task_id, int turn_ord);

/* Re-key a turn: move its message rows from (old_task, old_ord) to (new_task, new_ord).
 *
 * Needed because the storage key is POSITIONAL and a turn can be built before its final
 * position is known (a fork snapshot, a test that builds turns directly). Production
 * binds the key at creation, so for it this is a no-op. The watermarks come along. */
bool edc_msg_turn_move(const char *old_task, int old_ord, const char *new_session,
                       const char *new_group, const char *new_task, int new_ord);

/* A range of messages loaded in one query: the query RESULT, not a cache. The caller
 * owns |items| and every element, and must call edc_msg_span_free. |to| < 0 means "to the
 * end of the turn". The array is dense: a missing position yields a NULL entry so indices
 * keep matching msg_ord. */
struct edc_msg_span
{
    struct cJSON **items;
    int n;
};
bool edc_msg_span_load(const char *task_id, int turn_ord, int from, int to,
                       struct edc_msg_span *out);
void edc_msg_span_free(struct edc_msg_span *span);

/* Which session owns a task: one primary-key lookup, replacing the old
 * parse-every-session.json index. Returns a malloc'd copy (caller frees) or NULL. */
char *edc_state_store_session_for_task(const char *task_id);

/* Does the database hold any row for this session?
 *
 * The loader needs to tell two "no state" cases apart, because they mean opposite things
 * for the caller: a session that was never written (a brand-new session -- saving an empty
 * tree is correct) and a session that IS there but could not be read (a failed load --
 * saving would overwrite real state with a half-built shell). */
bool edc_state_store_has_session(const char *session_id);

/* ── Introspection on the store's OWN connection ──────────────────────────────
 * Several of the settings the store relies on are PER-CONNECTION: `synchronous`,
 * `foreign_keys`, `trusted_schema`, `busy_timeout`. Reading them over a second connection
 * -- the obvious thing a test would do -- reports THAT connection's defaults and tells you
 * nothing about the store. These accessors answer for the store's own connection.
 *
 * |pragma_name| must be a bare identifier ([A-Za-z0-9_]+, e.g. "synchronous"): anything
 * else is refused, because the name is interpolated into the PRAGMA statement.
 *
 * Both return -1 / NULL when the store is closed or the pragma yields no row. The text
 * form returns a malloc'd copy the caller frees. */
int edc_state_store_pragma_int(const char *pragma_name);
char *edc_state_store_pragma_text(const char *pragma_name);

#ifdef __cplusplus
}
#endif

#endif /* EDC_STATE_STORE_H */
