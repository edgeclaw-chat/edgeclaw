#include "edc_text_passes.h"

#include "sds.h"

#include <stddef.h>

/* 一个 text pass：|raw| -> 定稿文本（新 sds）。契约见头文件；顺序＝执行顺序。 */
typedef sds (*edc_text_pass_fn)(const char *raw, const struct edc_payload *payload);

/* 注册表 —— 新增 text pass 只加一行。 */
static const edc_text_pass_fn k_text_passes[] = {
    edc_pass_paste,
};

char *edc_pass_paste(const char *text, const struct edc_payload *payload)
{
    /* The whole pass: a pure, single-scan replacement. No I/O, no grammar. */
    return edc_payload_expand(text, payload);
}

char *edc_text_passes_run(const char *raw, const struct edc_payload *payload)
{
    /* Nothing to expand: hand back a copy without touching the registry. This is the
     * path every ordinary line takes, so it must not allocate more than the copy. */
    if (edc_payload_empty(payload))
        return sdsnew(raw ? raw : "");

    sds current = sdsnew(raw ? raw : "");
    const size_t n = sizeof(k_text_passes) / sizeof(k_text_passes[0]);
    for (size_t i = 0; i < n; i++)
    {
        if (!k_text_passes[i])
            continue;
        sds next = k_text_passes[i](current, payload);
        sdsfree(current);
        current = next ? next : sdsempty();
    }
    return current;
}
