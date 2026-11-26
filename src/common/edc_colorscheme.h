#ifndef EDC_COLORSCHEME_H
#define EDC_COLORSCHEME_H

/*
 * Vim colorscheme import for EdgeClaw's TUI.
 *
 * This is plan B: a self-contained reader for the classic Vim `highlight` /
 * `hi` commands inside a `colors/<name>.vim` file. It executes NOTHING -- no
 * Vimscript, no subprocess, no shell -- it only reads assignments. Themes that
 * compute their colours with `execute()` / Lua `nvim_set_hl` will therefore
 * parse partially (or not at all); that is the deliberate trade for a parser
 * that cannot run arbitrary code.
 *
 * The output is a table of semantic roles as 0xRRGGBB. Roles vim has no name
 * for are left "unset" and the C++ side fills them from the built-in palette,
 * so a partial theme still produces a usable, legible result.
 *
 * C++-safe on purpose: this header is included by the TUI. No sds, no cJSON,
 * no opaque structs from other modules in any signature.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One entry per slot in edc::style::Palette. Keep the ORDER in sync with
 * src/tui/edc_colorscheme.cpp's role table -- that file maps index to slot. */
enum edc_cs_role
{
    EDC_CS_WINDOW_BG = 0,
    EDC_CS_PANE_BG,
    EDC_CS_TEXT_DIM,
    EDC_CS_TEXT,
    EDC_CS_LINE_DIM,
    EDC_CS_DIALOG_BG,
    EDC_CS_ACCENT,
    EDC_CS_OK,
    EDC_CS_WARN,
    EDC_CS_ERROR,
    EDC_CS_INTERRUPTED,
    EDC_CS_THOUGHT_OPEN,
    EDC_CS_THOUGHT_CLOSED,
    EDC_CS_PROMPT,
    EDC_CS_USER_RULE,
    EDC_CS_MD_H1,
    EDC_CS_MD_H1_BG,
    EDC_CS_MD_H2,
    EDC_CS_MD_H3,
    EDC_CS_MD_H4,
    EDC_CS_MD_H5,
    EDC_CS_MD_H6,
    EDC_CS_MD_CODE,
    EDC_CS_MD_CODE_INLINE,
    EDC_CS_MD_CODE_BG,
    EDC_CS_MD_RULE,
    EDC_CS_MD_QUOTE,
    EDC_CS_MD_LIST,
    EDC_CS_MD_LINK,
    EDC_CS_MD_TABLE_HEAD,
    EDC_CS_PASTE_TEXT,
    EDC_CS_PASTE_SURFACE,
    EDC_CS_MENTION_TEXT,

    EDC_CS_ROLE_COUNT
};

/* A parsed theme. `set[i]` says rgb[i] was explicitly assigned; an unset role
 * must be filled from the built-in palette by the caller. */
struct edc_colorscheme
{
    uint32_t rgb[EDC_CS_ROLE_COUNT]; /* 0x00RRGGBB */
    bool set[EDC_CS_ROLE_COUNT];
};

/* Wipe to "nothing set". Call before parse/merge. */
void edc_colorscheme_reset(struct edc_colorscheme *cs);

/* Parse a whole file. Returns true when at least one known group was learned.
 * |err|, when non-NULL, receives a short reason on failure. */
bool edc_colorscheme_parse_file(const char *path, struct edc_colorscheme *out, char *err, size_t err_cap);

/* Parse a buffer (NUL-terminated, or |len| bytes when len > 0). */
bool edc_colorscheme_parse(const char *text, size_t len, struct edc_colorscheme *out);

/* --- the <edc_home>/colors directory ------------------------------------- */

/* Absolute path of the colors directory. Returns false if |buf| is too small. */
bool edc_colorscheme_dir(const char *edc_home, char *buf, size_t cap);

/* Create the colors directory (0700 where modes mean anything). */
bool edc_colorscheme_ensure_dir(const char *edc_home);

/* Call cb(name, ud) for every installed theme name, sorted, without the .vim
 * suffix. Returns the number of themes. Unknown/hidden names are skipped. */
typedef void (*edc_cs_name_cb)(const char *name, void *ud);
size_t edc_colorscheme_list(const char *edc_home, edc_cs_name_cb cb, void *ud);

/* Resolve a theme name to its file. Refuses names that are not plain
 * identifiers, so a config value cannot escape the colors directory. */
bool edc_colorscheme_path(const char *edc_home, const char *name, char *buf, size_t cap);

/* Load a theme by name into |out|. A NULL/empty name, or the literals "off"
 * and "builtin", return false without touching |out| (the caller then uses
 * the built-in palette). */
bool edc_colorscheme_load(const char *edc_home, const char *name, struct edc_colorscheme *out);

/* Copy a Vim colorscheme file into <edc_home>/colors/<name>.vim and return the
 * installed name in |out_name|. The name comes from the source file's base
 * name with any trailing `.vim` removed, sanitised to [A-Za-z0-9_-]. Rejects
 * non-regular files and oversized files. |err| receives a short reason. */
bool edc_colorscheme_install(const char *edc_home, const char *src_path, char *out_name, size_t name_cap, char *err,
                             size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* EDC_COLORSCHEME_H */
