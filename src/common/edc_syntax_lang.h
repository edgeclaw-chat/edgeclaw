#ifndef EDC_SYNTAX_LANG_H
#define EDC_SYNTAX_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Language-name mapping shared by the syntax highlighter and the fs tool.
 *
 * This is deliberately tree-sitter-free: it lives in its own small library so
 * a C caller (read_file tagging tool output) can classify a path without
 * pulling the highlighter, its grammars or PCRE2 into its link closure.
 * The alias table here is the single source of truth: edc_syntax.c's language
 * registry and the fs tool's path tagging both use it, so they cannot drift. */

/* Return the canonical language name for a filename extension, or NULL when
 * unknown. |path| may be a bare name or a full path; the part after the last
 * '.' is the extension. E.g. "foo.py" -> "python", "a.cpp" -> "cpp". */
const char *edc_syntax_lang_for_path(const char *path);

/* Map a language name or extension alias to its canonical name (the primary
 * tag used by the highlight registry), or NULL when not registered. E.g.
 * "py" -> "python", "hpp" -> "c", "typescript" -> "typescript". */
const char *edc_syntax_normalize_lang(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* EDC_SYNTAX_LANG_H */
