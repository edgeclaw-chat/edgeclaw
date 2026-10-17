#include "edc_passes.h"

#include <stddef.h>

/* 一个 pass：就地改 |ast|。false = 拒绝这条输入（|*out_error| 给用户看）。 */
typedef bool (*edc_pass_fn)(const struct edc_pass_ctx *ctx, edc_ast *ast, sds *out_error);

/* 注册表 —— 新增 pass 只加一行。顺序＝执行顺序。 */
static const edc_pass_fn k_passes[] = {
    edc_pass_mentions,
};

bool edc_passes_run(const struct edc_pass_ctx *ctx, edc_ast *ast, sds *out_error)
{
    if (out_error)
        *out_error = NULL;
    if (!ast)
        return true;

    const size_t n = sizeof(k_passes) / sizeof(k_passes[0]);
    for (size_t i = 0; i < n; i++)
    {
        if (!k_passes[i])
            continue;
        if (!k_passes[i](ctx, ast, out_error))
            return false;
    }
    return true;
}
