#include "edc_shell.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* ── $VAR 展开 (expand 阶段, deps 已完成后调用) ──────────────
 * 纯字符串处理, 不依赖 agent/session/scheduler —
 * 放在 shell_api 中使 //src/agent:agent 可以直接依赖, 避免
 * agent ↔ shell 的静态链接循环。 */

sds edc_shell_expand_vars(const char *input, edc_shell_var_resolver_fn resolver, void *user_data, sds *out_error)
{
    sds result = sdsempty();
    bool in_dq = false, in_sq = false;
    const char *p = input;

#define VAR_DELIM(c)                                                                                                   \
    ((c) == ' ' || (c) == '\t' || (c) == '\0' || (c) == '|' || (c) == ';' || (c) == '&' || (c) == '>' || (c) == '<')

    while (*p)
    {
        if (in_dq)
        {
            if (*p == '"')
            {
                in_dq = false;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (*p == '\\' && *(p + 1))
            {
                result = sdscatlen(result, p, 1);
                p++;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (*p == '$' && *(p + 1))
            {
                p++;
                goto expand_dq;
            }
            else
            {
                result = sdscatlen(result, p, 1);
                p++;
            }
        }
        else if (in_sq)
        {
            if (*p == '\'')
                in_sq = false;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '"')
        {
            in_dq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '\'')
        {
            in_sq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '$' && *(p + 1))
        {
            p++;
        expand_dq:
            if (*p == '$')
            {
                p++;
                sds val = resolver("$", user_data);
                if (val)
                {
                    result = sdscatsds(result, val);
                    sdsfree(val);
                }
            }
            else if (strncmp(p, "LAST", 4) == 0 && VAR_DELIM(p[4]))
            {
                p += 4;
                sds val = resolver("LAST", user_data);
                if (!val)
                {
                    if (out_error)
                        *out_error = sdscatprintf(sdsempty(), "$LAST: no completed task with output");
                    sdsfree(result);
                    return NULL;
                }
                result = sdscatsds(result, val);
                sdsfree(val);
            }
            else if (*p >= '0' && *p <= '9')
            {
                const char *ns = p;
                while (*p >= '0' && *p <= '9')
                    p++;
                char buf[32];
                size_t nl = (size_t)(p - ns);
                if (nl < sizeof(buf))
                {
                    memcpy(buf, ns, nl);
                    buf[nl] = '\0';
                    sds val = resolver(buf, user_data);
                    if (!val)
                    {
                        if (out_error)
                            *out_error = sdscatprintf(sdsempty(), "$%s: no output", buf);
                        sdsfree(result);
                        return NULL;
                    }
                    result = sdscatsds(result, val);
                    sdsfree(val);
                }
            }
            else
            {
                const char *is = p;
                while (*p && !VAR_DELIM(*p) && *p != '$')
                    p++;
                size_t il = (size_t)(p - is);
                if (il > 0)
                {
                    sds id = sdsnewlen(is, il);
                    sds val = resolver(id, user_data);
                    if (!val)
                    {
                        if (out_error)
                            *out_error = sdscatprintf(sdsempty(), "$%s: no output", id);
                        sdsfree(id);
                        sdsfree(result);
                        return NULL;
                    }
                    result = sdscatsds(result, val);
                    sdsfree(val);
                    sdsfree(id);
                }
            }
        }
        else
        {
            result = sdscatlen(result, p, 1);
            p++;
        }
    }

#undef VAR_DELIM
    return result;
}
