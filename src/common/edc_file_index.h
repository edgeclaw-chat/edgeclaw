#ifndef EDC_FILE_INDEX_H
#define EDC_FILE_INDEX_H

/* "What files are under this directory?" -- the list behind the `@` completion.
 *
 * Deliberately knows nothing about the sandbox or the config: the caller decides which
 * directory may be listed (edc_app_file_candidates does the sandbox check), and this
 * only walks, ranks and answers.
 *
 * The whole tree is walked ONCE per root and kept in memory: a completion popup filters
 * on every keystroke, and walking a repository per keystroke would stall the input box.
 * edc_file_index_reset() is how a caller says "the tree may have changed" (the TUI calls
 * it when a message is submitted -- the model may have just written a file). */
#include <stddef.h>

/* Up to |cap| bytes of "<relative path>\t<f|d|s>\n" rows, best match first, at most 20
 * rows. A directory's path ends with '/' (kind 'd'); kind 's' is the directory the query
 * ITSELF names ("sub/" -> "sub\ts"), offered so a mention of the whole directory can be
 * completed -- accepting that row finishes the mention instead of descending into it.
 * |query| NULL/"" lists the default order.
 * Returns the number of rows written (0 when nothing matches or the root is unusable). */
size_t edc_file_index_query(const char *root, const char *query, char *buf, size_t cap);

/* Forget the cached tree (next query walks again). */
void edc_file_index_reset(void);

#endif
