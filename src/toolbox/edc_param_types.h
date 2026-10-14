#ifndef __BC_PARAM_TYPES_H__
#define __BC_PARAM_TYPES_H__
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BC_TOOL_ARG_INVALID = 0,
    BC_TOOL_NOT_FOUND,
    BC_TOOL_EXEC_ERROR,
} edc_tool_err_t;

struct edc_param_type;

struct edc_param_object_property
{
    sds name;
    struct edc_param_type *type;
    bool required; /* absent + required => validation fails; absent + optional is allowed */
};

struct edc_param_type
{
    enum
    {
        BC_TOOL_TYPE_INT = 0,
        BC_TOOL_TYPE_STR,
        BC_TOOL_TYPE_BOOL,
        BC_TOOL_TYPE_ARRAY,
        BC_TOOL_TYPE_OBJECT
    } type;

    sds desc;

    union {
        struct
        {
            int64_t min;
            int64_t max;
        } num;

        struct
        {
            struct edc_param_type *item;
        } array;

        struct
        {
            struct edc_param_object_property *properties;
            int n;
        } object;

        struct
        {
            sds *enums;
            int n;
        } str;
    };
};

extern struct edc_param_type *edc_param_int_type(char *desc, int64_t min, int64_t max);
extern struct edc_param_type *edc_param_str_type(char *desc, const char *first, ...);
extern struct edc_param_type *edc_param_bool_type(char *desc);
extern struct edc_param_type *edc_param_array_type(char *desc, struct edc_param_type *item_type);
extern struct edc_param_type *edc_param_object_type(char *desc, struct edc_param_object_property *properties, int n);
extern void edc_param_type_free(struct edc_param_type *type);
extern bool edc_param_type_validate(cJSON *args, struct edc_param_type *type);
extern cJSON *edc_param_type_to_json(struct edc_param_type *type);
extern sds edc_param_type_to_s(struct edc_param_type *type);
#endif
