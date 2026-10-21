#ifndef EDC_BUNDLED_DOCS_H
#define EDC_BUNDLED_DOCS_H

#include "edc_bundled_files.h"

/* The user manual -- a deliberately small subset of the source tree's docs/: the six
 * chapters that answer "how do I use EdgeClaw" (the `/help` reference, the man page, the
 * input-syntax and local-shell notes, the tool-output limits, the prompt library; the list
 * itself is DOCS_FILES in //BUILD). The rest of docs/ is design notes for people working
 * ON EdgeClaw, and is NOT embedded: it answers no user question and would be paid for by
 * every binary that carried it.
 *
 * The manual is embedded at build time by //:docs_embedded_c and released into
 * $EDC_HOME/docs on startup (see edc_init_env_docs).
 *
 * The system prompt names that path (see edc_context_get_identity), so the model can
 * read the manual whenever it needs to know how EdgeClaw ITSELF is used -- without the
 * source tree on disk, and without the user having to point it somewhere. */

extern const struct edc_bundled_file edc_bundled_docs_entries[];
extern const size_t edc_bundled_docs_count;

#endif /* EDC_BUNDLED_DOCS_H */
