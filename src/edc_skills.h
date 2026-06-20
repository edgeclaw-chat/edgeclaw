#ifndef __BC_SKILLS_H__
#define __BC_SKILLS_H__

#include <glib.h>
struct edc_skill
{
    GString *name;
    GHashTable *meta;
    GString *content;
};

extern void edc_skill_free(struct edc_skill *skill);
extern GArray *edc_skills_load(const GString *dir_path);
#endif
