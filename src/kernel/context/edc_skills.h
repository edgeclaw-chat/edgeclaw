#ifndef __BC_SKILLS_H__
#define __BC_SKILLS_H__

#include "sds.h"
#include "utarray.h"
#include "uthash.h"
#include <stdbool.h>

struct edc_skill_meta_entry
{
    sds key;
    sds value;
    UT_hash_handle hh;
};

struct edc_skill
{
    sds name;
    struct edc_skill_meta_entry *meta;
    sds content; /* the SKILL.md body (front matter stripped) */
    sds source;  /* the resource ROOT it came from */
    /* The SKILL.md path itself. `/skill` wraps the body in pi's
     * `<skill name="…" location="…">` block, whose "References are relative to <dir>" line
     * is what tells the model where the skill's scripts/references live. */
    sds location;
    /* The copies this one SHADOWED -- the SKILL.md paths of same-named skills found in
     * later (less specific) roots, NULL-terminated. Recorded so the collision can be
     * REPORTED (a `log_warn` at load, `_(shadowed: …)_` in `/skill`'s listing) instead of
     * silently dropping them; pi does the same with its `collision` diagnostics. NULL when
     * nothing collided. */
    sds *shadowed;
    /* pi's `disable-model-invocation: true` -- the author says the MODEL must not reach
     * for this skill on its own. It then stays out of the system prompt's skill list and
     * only an explicit `/skill <name>` runs it. Default true (the model may pick it). */
    bool model_invocable;
};

extern void edc_skill_free(struct edc_skill *skill);

/** Every skill directly under |dir| (`<dir>/<name>/SKILL.md`), as `struct edc_skill *`.
 *  A missing/unreadable directory yields an empty array. Caller frees each skill, then
 *  utarray_free. */
extern UT_array *edc_skills_load(const char *dir_path);

/** The same, over the resource roots (edc_resources.h): `<root>/skills/` and
 *  `<root>/skill/`, roots IN ORDER, first name wins, `skill->source` = the root. */
extern UT_array *edc_skills_load_roots(const UT_array *roots);

#endif /* __BC_SKILLS_H__ */
