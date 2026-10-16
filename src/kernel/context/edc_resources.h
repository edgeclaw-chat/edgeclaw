#ifndef __EDC_RESOURCES_H__
#define __EDC_RESOURCES_H__

/* ── Where prompt templates and skills may come from ──────────
 *
 * EdgeClaw reads its own resources from the workspace, and -- so that a user can move over
 * from pi / opencode / Claude Code without moving any file -- the SAME kinds of resource
 * from their directories.
 *
 * Order = precedence: the first root that has a given name wins.
 * ours (workspace)  ->  user (~/.claude, ~/.agents, ~/.config/opencode, ~/.pi/agent)
 *                   ->  project (walking up from the SESSION's bound directory:
 *                               .edgeclaw, .claude, .agents, .opencode, .pi)
 *
 * Project-level roots are ON by default; config.json's top-level `load_project_resources`
 * (false) turns them off. The layouts a root may have -- only the ones that exist are used:
 *
 *     <root>/prompts/           pi prompt templates (one .md per template)
 *     <root>/command(s)/        opencode / Claude Code commands (nested .md files)
 *     <root>/skill(s)/          skills, one SKILL.md per skill (Claude: skills/<name>/)
 *
 * There is no per-root "dialect": a template may spell a parameter the ecosystem's way
 * (`$1` / `$@` / `${1:-x}`) or with the legacy `{{…}}` alias, and the same expander reads
 * both -- task references are `{{…}}` too, but they belong to the input layer below.
 */

#include "sds.h"
#include "utarray.h"
#include <stdbool.h>

struct edc_config;

struct edc_resource_dir
{
    sds path;
};

/* The ordered, existing resource dirs. UT_array of `struct edc_resource_dir *`; free with
 * edc_resource_dirs_free. Never NULL (an empty array when nothing exists).
 *
 * |workdir| is the SESSION's bound directory: the project roots are looked for by walking
 * up from it (nearest first), because a session is about that directory, not about
 * wherever the process happens to run. NULL/"" falls back to the process cwd. */
UT_array *edc_resource_dirs(const struct edc_config *conf, const char *workdir);
void edc_resource_dirs_free(UT_array *dirs);

#endif /* __EDC_RESOURCES_H__ */
