#ifndef __EDC_COMPLETION_H__
#define __EDC_COMPLETION_H__

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

/* ---- completion candidate ---- */
typedef struct
{
    sds text;     /* completion text */
    sds display;  /* display text (may contain colour/description) */
    double score; /* sort score, higher is better */
    int source;   /* source: 0=cmd, 1=file, 2=history */
} edc_completion_candidate_t;

/* ---- completion result set ---- */
#define EDC_COMPLETION_MAX 256
typedef struct
{
    edc_completion_candidate_t items[EDC_COMPLETION_MAX];
    int count;
    int selected;      /* currently selected index, -1 = none */
    sds common_prefix; /* longest common prefix of all candidates */
    sds token;         /* the prefix token the user typed */
} edc_completion_result_t;

/* ---- completion source: callback that fills result ---- */
typedef void (*edc_completion_source_fn)(const char *token, edc_completion_result_t *result, void *user_data);

/* ---- core API ---- */

/* init / free */
void edc_completion_init(void);
void edc_completion_free(void);

/* register a completion source (commands, files, history, ...) */
void edc_completion_register_source(edc_completion_source_fn source, void *user_data);

/* add a command to the internal Trie (for /command completion) */
void edc_completion_add_command(const char *command);

/* add a history entry (for history-based completion) */
void edc_completion_add_history(const char *line);

/* core: run completion, filling the result set */
void edc_completion_collect(const char *line_buffer, int cursor_pos, edc_completion_result_t *result);

/* free the result set */
void edc_completion_result_free(edc_completion_result_t *result);

/* extract the longest common prefix among candidates */
sds edc_completion_lcp(edc_completion_result_t *result);

/* fuzzy-match score (0.0 ~ 1.0), for fallback scenarios */
double edc_completion_fuzzy_score(const char *pattern, const char *target);

#endif /* __EDC_COMPLETION_H__ */
