#ifndef __BC_FOLD_FILES_H__
#define __BC_FOLD_FILES_H__

#include "sds.h"
#include <cJSON.h>
#include <stddef.h>

/* The files a group's folded work touched -- as DATA, not as text.
 *
 * Why this type exists: the lists used to live only as the `<read-files>` /
 * `<modified-files>` blocks inside `group->memory`, so every consumer (the model,
 * `/group files`, `/group info`) had to parse the string back out, and a summary that
 * rewrote a block in its own words silently corrupted the accumulated list. Here the
 * data is the source of truth and those blocks are its projection for the model.
 *
 * Every list is in FIRST-SEEN order (oldest first) and capped per list. The order is
 * deliberate and is NOT pi's: pi sorts alphabetically because it has no cap, while a
 * cap plus sorting would permanently evict the most recently touched files -- exactly
 * the ones worth keeping. Order is also part of the cached prompt prefix, so `merge`
 * must produce the same bytes for the same inputs. */

#define EDC_FOLD_MAX_TRACKED_FILES 50

struct edc_fold_files
{
    char **modified; /* write_file / edit_file */
    size_t modified_n;
    char **read; /* read_file, minus anything in modified (a file you edited was not "only read") */
    size_t read_n;
};

extern struct edc_fold_files *edc_fold_files_new(void);
extern struct edc_fold_files *edc_fold_files_dup(const struct edc_fold_files *f);
extern void edc_fold_files_free(struct edc_fold_files *f);
/* Total entries across both lists (0 for NULL). */
extern size_t edc_fold_files_count(const struct edc_fold_files *f);

/* Insert if absent (whole-string compare). A NULL or empty path is ignored; a NULL `f`
 * allocates on first use via the *f form below, so callers never branch. */
extern void edc_fold_files_add_modified(struct edc_fold_files *f, const char *path);
extern void edc_fold_files_add_read(struct edc_fold_files *f, const char *path);

/* dst absorbs src: append what dst lacks (keeping first-seen order), then drop from
 * `read` every path that is in `modified`, then cap each list at
 * EDC_FOLD_MAX_TRACKED_FILES by discarding the OLDEST entries. Either side may be NULL. */
extern void edc_fold_files_merge(struct edc_fold_files *dst, const struct edc_fold_files *src);

/* The memory TEXT to store: |summary| with any echoed `<read-files>` /
 * `<modified-files>` blocks removed and trailing spaces/newlines trimmed. The blocks are
 * NOT part of the stored text -- that is what makes the model unable to corrupt them.
 * Caller frees. */
extern sds edc_fold_files_body(const char *summary);

/* The text the MODEL sees: |body| plus the two blocks plus the final newline. This is
 * the projection, computed per request, so it must be deterministic: same (body, lists)
 * => same bytes, or every request would miss the prompt cache. Caller frees. */
extern sds edc_fold_files_render(const char *body, const struct edc_fold_files *f);

/* Token estimate for the rendered blocks (what `render` adds on top of the body). */
extern long long edc_fold_files_token_estimate(const struct edc_fold_files *f);

/* Persistence: {"modified":[...],"read":[...]}. Array order is the list order. */
extern cJSON *edc_fold_files_to_json(const struct edc_fold_files *f);
/* NULL / missing / malformed => an empty list (never NULL), so callers need no branch.
 * An archive written before this field existed simply has no key: nothing to migrate,
 * because the fold landing replaces `group->memory` wholesale on the next fold. */
extern struct edc_fold_files *edc_fold_files_from_json(const cJSON *j);

#endif
