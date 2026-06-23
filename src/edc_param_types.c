#include "edc_param_types.h"

#include <stdarg.h>

struct edc_param_type *edc_param_int_type(gchar *desc, gint64 min, gint64 max)
{
    struct edc_param_type *type = g_new0(struct edc_param_type, 1);
    type->type = BC_TOOL_TYPE_INT;
    type->desc = g_strdup(desc);
    type->num.min = min;
    type->num.max = max;
    return type;
}

struct edc_param_type *edc_param_str_type(gchar *desc, const gchar *first, ...)
{
    struct edc_param_type *type = g_new0(struct edc_param_type, 1);
    type->type = BC_TOOL_TYPE_STR;
    type->desc = g_strdup(desc);

    if (!first)
        return type;

    va_list args, args_copy;
    va_start(args, first);
    va_copy(args_copy, args);

    int n = 1;
    while (va_arg(args_copy, const gchar *))
        n++;
    va_end(args_copy);

    gchar **dst = g_new(gchar *, n);
    dst[0] = g_strdup(first);
    for (int i = 1; i < n; i++)
    {
        dst[i] = g_strdup(va_arg(args, const gchar *));
    }
    va_end(args);

    type->str.enums = (gchar *)dst;
    type->str.n = n;
    return type;
}

struct edc_param_type *edc_param_bool_type(gchar *desc)
{
    struct edc_param_type *type = g_new0(struct edc_param_type, 1);
    type->type = BC_TOOL_TYPE_BOOL;
    type->desc = g_strdup(desc);
    return type;
}

struct edc_param_type *edc_param_array_type(gchar *desc, struct edc_param_type *item_type)
{
    struct edc_param_type *type = g_new0(struct edc_param_type, 1);
    type->type = BC_TOOL_TYPE_ARRAY;
    type->desc = g_strdup(desc);
    type->array.item = item_type;
    return type;
}

struct edc_param_type *edc_param_object_type(gchar *desc, struct edc_param_object_property *properties, int n)
{
    struct edc_param_type *type = g_new0(struct edc_param_type, 1);
    type->type = BC_TOOL_TYPE_OBJECT;
    type->desc = g_strdup(desc);
    type->object.properties = g_new(struct edc_param_object_property, n);
    type->object.n = n;
    for (int i = 0; i < n; i++)
    {
        type->object.properties[i].name = g_strdup(properties[i].name);
        type->object.properties[i].type = properties[i].type;
    }
    return type;
}

void edc_param_type_free(struct edc_param_type *type)
{
    if (!type)
        return;

    switch (type->type)
    {
    case BC_TOOL_TYPE_STR:
        if (type->str.n > 0 && type->str.enums)
        {
            gchar **enums = (gchar **)type->str.enums;
            for (int i = 0; i < type->str.n; i++)
            {
                g_free(enums[i]);
            }
            g_free(enums);
        }
        break;
    case BC_TOOL_TYPE_ARRAY:
        edc_param_type_free(type->array.item);
        break;
    case BC_TOOL_TYPE_OBJECT:
        for (int i = 0; i < type->object.n; i++)
        {
            g_free(type->object.properties[i].name);
            edc_param_type_free(type->object.properties[i].type);
        }
        g_free(type->object.properties);
        break;
    default:
        break;
    }

    g_free(type->desc);
    g_free(type);
}

gboolean edc_param_type_validate(cJSON *args, struct edc_param_type *type)
{
    if (!args || !type)
        return FALSE;

    switch (type->type)
    {
    case BC_TOOL_TYPE_INT:
        if (!cJSON_IsNumber(args) || (args->valueint != args->valuedouble))
            return FALSE;
        if (type->num.min != G_MININT64 && args->valueint < type->num.min)
            return FALSE;
        if (type->num.max != G_MAXINT64 && args->valueint > type->num.max)
            return FALSE;
        return TRUE;
    case BC_TOOL_TYPE_BOOL:
        return cJSON_IsBool(args);
    case BC_TOOL_TYPE_STR:
        if (!cJSON_IsString(args))
            return FALSE;
        if (type->str.n > 0 && type->str.enums)
        {
            gchar **enums = (gchar **)type->str.enums;
            for (int i = 0; i < type->str.n; i++)
            {
                if (g_str_equal(cJSON_GetStringValue(args), enums[i]))
                    return TRUE;
            }
            return FALSE;
        }
        return TRUE;
    case BC_TOOL_TYPE_ARRAY:
        if (!cJSON_IsArray(args))
            return FALSE;
        {
            int size = cJSON_GetArraySize(args);
            for (int i = 0; i < size; i++)
            {
                if (!edc_param_type_validate(cJSON_GetArrayItem(args, i), type->array.item))
                    return FALSE;
            }
        }
        return TRUE;
    case BC_TOOL_TYPE_OBJECT:
        if (!cJSON_IsObject(args))
            return FALSE;
        for (int i = 0; i < type->object.n; i++)
        {
            cJSON *item = cJSON_GetObjectItem(args, type->object.properties[i].name);
            if (!edc_param_type_validate(item, type->object.properties[i].type))
                return FALSE;
        }
        return TRUE;
    default:
        return FALSE;
    }
}

cJSON *edc_param_type_to_json(struct edc_param_type *type)
{
    if (!type)
        return NULL;

    switch (type->type)
    {
    case BC_TOOL_TYPE_INT: {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "integer");
        if (type->desc)
            cJSON_AddStringToObject(obj, "description", type->desc);
        if (type->num.min != G_MININT64)
            cJSON_AddNumberToObject(obj, "minimum", (double)type->num.min);
        if (type->num.max != G_MAXINT64)
            cJSON_AddNumberToObject(obj, "maximum", (double)type->num.max);
        return obj;
    }
    case BC_TOOL_TYPE_BOOL: {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "boolean");
        if (type->desc)
            cJSON_AddStringToObject(obj, "description", type->desc);
        return obj;
    }
    case BC_TOOL_TYPE_STR: {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "string");
        if (type->desc)
            cJSON_AddStringToObject(obj, "description", type->desc);
        if (type->str.n > 0 && type->str.enums)
        {
            cJSON *arr = cJSON_CreateArray();
            gchar **enums = (gchar **)type->str.enums;
            for (int i = 0; i < type->str.n; i++)
            {
                cJSON_AddItemToArray(arr, cJSON_CreateString(enums[i]));
            }
            cJSON_AddItemToObject(obj, "enum", arr);
        }
        return obj;
    }
    case BC_TOOL_TYPE_ARRAY: {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "array");
        if (type->desc)
            cJSON_AddStringToObject(obj, "description", type->desc);
        cJSON_AddItemToObject(obj, "items", edc_param_type_to_json(type->array.item));
        return obj;
    }
    case BC_TOOL_TYPE_OBJECT: {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "object");
        if (type->desc)
            cJSON_AddStringToObject(obj, "description", type->desc);
        cJSON *props = cJSON_CreateObject();
        for (int i = 0; i < type->object.n; i++)
        {
            cJSON_AddItemToObject(props, type->object.properties[i].name,
                                  edc_param_type_to_json(type->object.properties[i].type));
        }
        cJSON_AddItemToObject(obj, "properties", props);
        return obj;
    }
    default:
        return NULL;
    }
}

gchar *edc_param_type_to_s(struct edc_param_type *type)
{
    cJSON *json = edc_param_type_to_json(type);
    if (!json)
        return g_strdup("<string format error>");
    gchar *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return str;
}
