#ifndef __BC_PARAM_TYPES_H__
#define __BC_PARAM_TYPES_H__
#include <cJSON.h>
#include <glib.h>

typedef enum
{
    BC_TOOL_ARG_INVALID = 0,
    BC_TOOL_NOT_FOUND,
    BC_TOOL_EXEC_ERROR,
} edc_tool_err_t;

struct edc_param_type;

struct edc_param_object_property
{
    gchar *name;
    struct edc_param_type *type;
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

    gchar *desc;

    union {
        struct
        {
            gint64 min;
            gint64 max;
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
            gchar *enums;
            int n;
        } str;
    };
};

extern struct edc_param_type *edc_param_int_type(gchar *desc, gint64 min, gint64 max);
extern struct edc_param_type *edc_param_str_type(gchar *desc, const gchar *first, ...);
extern struct edc_param_type *edc_param_bool_type(gchar *desc);
extern struct edc_param_type *edc_param_array_type(gchar *desc, struct edc_param_type *item_type);
extern struct edc_param_type *edc_param_object_type(gchar *desc, struct edc_param_object_property *properties, int n);
extern void edc_param_type_free(struct edc_param_type *type);
extern gboolean edc_param_type_validate(cJSON *args, struct edc_param_type *type);
extern cJSON *edc_param_type_to_json(struct edc_param_type *type);
extern gchar *edc_param_type_to_s(struct edc_param_type *type);
#endif
