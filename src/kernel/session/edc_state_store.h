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
 * MIGRATIONS RUN FORWARD, IN THE BINARY. The schema has one version (`EDC_DB_VERSION`) and
 * a registry of steps that upgrade a database written by an older build in place
 * (edc_state_migrate.{c,h}). Starting EdgeClaw is the whole procedure: it checkpoints the
 * WAL, copies the database aside, proves the copy opens (`quick_check`), then commits each
 * step together with its version stamp, so a crash can only leave the file at a complete
 * version. The backup is kept and named in the log.
 *
 * A database written by a NEWER build is still refused: an older binary cannot understand
 * a newer schema, and writing it back would drop whatever that version added. That refusal
 * says "update EdgeClaw", never "delete the file" -- the delete-and-recreate instruction
 * this header used to give was how a schema bump cost a user every conversation they had.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
 *     INTEGER unix second -- there is exactly one representation.
 *
 * v4: sessions.is_active. Which session the UI is on used to live only in
 *     <workspace>/sessions.json, and the session LIST lived there too because the index was
 *     the only place the answer could be written; the two copies could disagree, which is
 *     how an imported session stayed invisible. It is a column now, with a partial unique
 *     index so "at most one active" is enforced rather than assumed, and the list is a
 *     query. */
/* v5: task_id and group_id are INTEGER PRIMARY KEYs assigned by the database.
 *     The old shape was TEXT ids produced by a process-wide counter whose high-water mark
 *     had to be persisted in <ws>/.task_seq (one open/write/rename per allocation). The
 *     INSERT allocates now, in one transaction with the group_id fill-in, so the counter
 *     and its file are gone. group_id follows task_id because a group's id IS its leader's
 *     task id. */
/* v6: session_id is an INTEGER PRIMARY KEY AUTOINCREMENT, assigned by the INSERT -- the
 *     same change v5 made for task_id, for the same reason. The counter and
 *     <ws>/.session_seq are gone; AUTOINCREMENT guarantees an id is never reused, which is
 *     what stops a new session from landing on a removed one's directory and replaying its
 *     chat.jsonl. Every table that references a session (groups, group_fold_files, tasks,
 *     messages, memo_entries, memo_events) follows.
 *
 *     The on-disk DIRECTORY still says "s<id>" (edc_session_dir_name): those directories
 *     hold live chat.jsonl transcripts, and renaming them would be a data migration for a
 *     formatting decision. */
/* v7: group_fold_files gained a `kind` column.
 *
 *     A group's tracked-file state is TWO lists (read, modified) and the table only had room
 *     for one flat sequence of paths. Combined with a flush that tested cJSON_IsArray on a
 *     {"modified":[...],"read":[...]} object, that meant the table was never written at all
 *     since the store landed -- and the load, which built a flat array, could not have read it
 *     back either. Both sides now go through edc_fold_files_*, so the shape lives in one
 *     place.
 *
 * v8 (2026-10-30): two session-memory fields became explicit state instead of being
 *     encoded in something else.
 *     - tasks.memo_baseline_done: whether this task has received the memory CATALOGUE.
 *       It used to be inferred from memo_seen_seq == 0, which overloaded one watermark
 *       with two meanings ("never saw the index" vs "cursor at the beginning of the
 *       feed") and could not express "the catalogue was delivered but the feed is
 *       empty".
 *     - memo_events.changed: which fields a change touched (EDC_MEMO_F_*). It was
 *       in-memory only, so a restart silently turned every past change into "unknown"
 *       and the notices fell back to re-printing summaries.
 *     Migration: built in since v1.0 (edc_state_migrate.c step_v7_to_v8); the python
 *     script tools/migrate_edgeclaw_db_v7_to_v8.py stays for databases that stopped at
 *     v7 before the binary could upgrade them. It backfills
 *     memo_baseline_done = (memo_seen_seq > 0) -- a task that had already consumed
 *     events has necessarily been through the index -- and leaves changed = 0, which is
 *     the documented "unknown, render the content" value, not a guess. */
#define EDC_DB_VERSION 8

/* Open <workspace>/edgeclaw.db for the lifetime of the process.
 *
 * Refuses (returns false, logs why) when:
 *  - the database, its -wal or -shm is a symlink -- the workspace is a model-writable
 *    directory, and this exact hole was already exploited once with jobs.json;
 *  - the resolved database does not sit inside the resolved workspace (a workspace that
 *    is itself a symlink must not become an escape hatch);
 *  - another process already holds the workspace lock;
 *  - the filesystem is not a local one (SQLite's locking is not reliable over NFS/SMB);
 *  - the file carries a NEWER EDC_DB_VERSION (an older binary must not write a newer
 *    schema). An older version is migrated forward in place, not refused.
 *
 * Idempotent: a second call with the store already open returns true.
 * Returns false for a NULL cfg or one with no workspace. */
bool edc_state_store_open(struct edc_config *cfg);

/* Checkpoint the WAL and close. Safe to call when not open. */
void edc_state_store_close(void);

bool edc_state_store_is_open(void);

/* The store's own connection. NULL when the store is closed.
 *
 * WHY THIS EXISTS: the store holds the database exclusively (see the Locking note in
 * edc_state_store.c), so while it is open there is no second connection to inspect it
 * with -- not from another process, and not from this one. Anything that wants to see the
 * rows as they are on disk RIGHT NOW has to come through here. Diagnostics that only need
 * the file itself (backup, export, integrity check) run against a stopped agent.
 *
 * READ-ONLY BY CONTRACT. The store owns the schema, the PRAGMAs and the transaction
 * boundaries: do not sqlite3_close() it, do not write through it, do not change its
 * PRAGMAs. It exists for inspection (tests, and a future `edgeclaw db` subcommand). */
struct sqlite3 *edc_state_store_handle(void);

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
    int64_t io_bytes;
    /* Rows inserted or deleted, FK cascades included: the O(history) side in the store's
     * own units (one row per message, task, turn, dep, tool call...). */
    int64_t rows;
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
bool edc_state_store_forget(int64_t session_id);

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
/* A session id reserved for a turn built without a real session (tests, and any caller
 * that only carries a task id): the FK chain still needs an ancestor row. Negative because
 * AUTOINCREMENT only ever produces positive ids, so it can never collide with a real
 * session. */
#define EDC_SESSION_ID_PLACEHOLDER (-1)

bool edc_state_ensure_turn(int64_t session_id, int64_t group_id, int64_t task_id,
                           int turn_ord);

/* Append one message at |msg_ord|. |tokens| is the estimate computed once at append
 * time (0 = unknown), so token accounting is a range SUM instead of re-estimating every
 * message per request. The turn row must already exist (edc_state_ensure_turn). */
bool edc_msg_append(int64_t task_id, int turn_ord, int msg_ord, const struct cJSON *msg,
                    int64_t tokens);

/* One message, parsed into a fresh object the CALLER owns (free with cJSON_Delete), or
 * NULL when the row does not exist. */
struct cJSON *edc_msg_get(int64_t task_id, int turn_ord, int msg_ord);

/* Sum of the token estimates of messages [from, len). One indexed range scan; this
 * replaces walking every message in C. Returns -1 when the store is closed. */
int64_t edc_msg_turn_tokens_from(int64_t task_id, int turn_ord, int from);

/* Re-key a turn: move its message rows from (old_task, old_ord) to (new_task, new_ord).
 *
 * Needed because the storage key is POSITIONAL and a turn can be built before its final
 * position is known (a fork snapshot, a test that builds turns directly). Production
 * binds the key at creation, so for it this is a no-op. The watermarks come along. */
bool edc_msg_turn_move(int64_t old_task, int old_ord, int64_t new_session,
                       int64_t new_group, int64_t new_task, int new_ord);

/* A range of messages loaded in one query: the query RESULT, not a cache. The caller
 * owns |items| and every element. `edc_turn_msgs` moves the non-NULL entries into its own
 * array and frees |items| itself, so a caller that hands the entries on still owns (and
 * must free) whatever it took. |to| < 0 means "to the end of the turn". The array is
 * dense: a missing position yields a NULL entry so indices keep matching msg_ord. */
struct edc_msg_span
{
    struct cJSON **items;
    int n;
};
bool edc_msg_span_load(int64_t task_id, int turn_ord, int from, int to,
                       struct edc_msg_span *out);

/* ── Addressing messages by id ──────────────────────────────────────────────
 *
 * The group history holds message IDS, not copies (a copy of every unfolded message was
 * 164 MiB for a 40k-message session). So the runtime has to be able to read messages by
 * id, and to do it in bulk: one query per message would be a round trip per message.
 */

/* The ids of one turn's messages [from, to), in msg_ord order. |to| < 0 means "to the
 * end". Used to append a turn to the group history without touching the rows themselves. */
struct edc_msg_ids
{
    char **ids;
    int n;
};
bool edc_msg_ids_for_turn(int64_t task_id, int turn_ord, int from, int to,
                          struct edc_msg_ids *out);
void edc_msg_ids_free(struct edc_msg_ids *s);

/* One message by id: a fresh object the CALLER owns, or NULL. */
struct cJSON *edc_msg_get_by_id(const char *id);

/* Load |n| messages by id, IN THE ORDER GIVEN, as one JSON array the caller owns.
 *
 * Uses a temp table and a join rather than `WHERE id IN (...)`: IN returns rows in id
 * order, which is not the caller's order, and re-sorting 40k messages in C is work SQLite
 * can do while it streams. Missing ids are SKIPPED (not NULL-padded) -- this feeds a
 * request or a transcript, where a gap has no meaning.
 */
struct cJSON *edc_msg_load_by_ids(const char *const *ids, int n);

/* Sum of the token estimates of the messages' stored estimates, by id. -1 when the store
 * is closed. */
int64_t edc_msg_tokens_by_ids(const char *const *ids, int n);

/* The role column of one message, copied into |buf| ("user" / "assistant" / "tool" /
 * "system"; "" when the column is empty). Returns false when the row does not exist.
 *
 * Separate from edc_msg_get_by_id because the request builder asks this on every request
 * to decide where a window starts, and paying for a full message parse to answer
 * "is it a tool result?" would defeat the point of keeping the history as ids. */
bool edc_msg_role_by_id(const char *id, char *buf, size_t n);

/* Which session owns a task: one primary-key lookup, replacing the old
 * index that used to be built by parsing every unloaded session's state file.
 * Returns a malloc'd copy (caller frees) or NULL. */
char *edc_state_store_session_for_task(int64_t task_id);

/* Does the database hold any row for this session?
 *
 * The loader needs to tell two "no state" cases apart, because they mean opposite things
 * for the caller: a session that was never written (a brand-new session -- saving an empty
 * tree is correct) and a session that IS there but could not be read (a failed load --
 * saving would overwrite real state with a half-built shell). */
/* ── The session list, and which one is active ───────────────────────────────
 *
 * The list used to live in <workspace>/sessions.json and this table was just "where the
 * rows go". Now the table IS the list: the index file restated it, and keeping two copies
 * in step is what made an imported session invisible until the file was rewritten by hand.
 *
 * `is_active` is the one thing the file held that the table did not; it is a column now,
 * with a partial unique index so "at most one" is enforced rather than assumed.
 */
struct edc_session_row
{
    int64_t session_id;
    char *name;
    uint64_t created_at;
    uint64_t last_active;
    bool is_active;
};

/* Every session, oldest first (the order an id list wants). Caller frees with
 * edc_session_rows_free(). Returns false on a SQL error; *out stays NULL with *n == 0. */
bool edc_state_store_list_sessions(struct edc_session_row **out, int *n);
void edc_session_rows_free(struct edc_session_row *rows, int n);

/* Make |session_id| the active one and clear every other. One statement pair inside one
 * transaction: the partial unique index would reject a half-applied switch. */
bool edc_state_store_set_active_session(int64_t session_id);

/* Insert a session row if it does not exist yet (creation path, before any state is
 * flushed for it). */
bool edc_state_store_ensure_session(int64_t session_id, const char *name, uint64_t created_at);

/* Delete a session row. Cascades take its groups/tasks/turns/messages. */
bool edc_state_store_delete_session(int64_t session_id);

/* Create a task row and return the id the database assigned.
 *
 * This is how a task gets its identity now: `tasks.task_id` is an INTEGER PRIMARY KEY, so
 * the INSERT allocates it -- no counter, no .task_seq file, no fsync per allocation.
 *
 * ONE TRANSACTION, deliberately: the row is inserted with group_id = 0 (a task that starts
 * its own group does not know the id yet -- the group id IS the leader's task id), the id
 * is read back with last_insert_rowid(), and group_id is filled in. Splitting those two
 * statements across transactions would leave a task pointing at group 0 after a crash, and
 * nothing would ever repair it. The caller then builds the struct around the id.
 *
 * |group_id| 0 = "this task leads its own group": the group id becomes the task id. */
bool edc_state_store_insert_task_new(int64_t session_id, int64_t group_id, int64_t *out_task_id);

/* Create a session row and return the id the database assigned.
 *
 * Same shape as insert_task_new: sessions.session_id is an INTEGER PRIMARY KEY
 * AUTOINCREMENT, so the INSERT allocates it and the database guarantees the id is never
 * reused -- the invariant <ws>/.session_seq used to be responsible for. */
bool edc_state_store_insert_session_new(const char *name, int64_t *out_session_id);

bool edc_state_store_has_session(int64_t session_id);

/* ── Introspection on the store's OWN connection ──────────────────────────────
 * Several of the settings the store relies on are PER-CONNECTION: `synchronous`,
 * `foreign_keys`, `trusted_schema`, `busy_timeout`. Reading them over a second connection
 * -- the obvious thing a test would do -- reports THAT connection's defaults and tells you
 * nothing about the store. These accessors answer for the store's own connection.
 *
 * |pragma_name| must be a bare identifier ([A-Za-z0-9_]+, e.g. "synchronous"): anything
 * else is refused, because the name is interpolated into the PRAGMA statement.
 *
 * Returns -1 when the store is closed or the pragma yields no row. */
int edc_state_store_pragma_int(const char *pragma_name);

#ifdef __cplusplus
}
#endif

#endif /* EDC_STATE_STORE_H */
