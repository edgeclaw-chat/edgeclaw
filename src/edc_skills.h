#include "edc_string.h"
#ifndef __BC_SKILLS_H__
#define __BC_SKILLS_H__

#include "uthash.h"
#include "utarray.h"

struct edc_skill_meta_entry
{
    char *key;
    char *value;
    UT_hash_handle hh;
};

struct edc_skill
{
    sds name;
    struct edc_skill_meta_entry *meta;
    sds content;
};

extern void edc_skill_free(struct edc_skill *skill);
extern UT_array *edc_skills_load(const char *dir_path);
#endif
