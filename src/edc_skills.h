#include "edc_string.h"
#ifndef __BC_SKILLS_H__
#define __BC_SKILLS_H__

#include <glib.h>
struct edc_skill
{
    sds name;
    GHashTable *meta;
    sds content;
};

extern void edc_skill_free(struct edc_skill *skill);
extern GArray *edc_skills_load(const char *dir_path);
#endif
