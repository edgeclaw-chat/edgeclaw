#include "edc_output_buf.h"

#include <stdio.h>

void edc_output_buf_size_str(size_t n, char *buf, size_t buf_cap)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    if (!buf || buf_cap == 0)
        return;

    double v = (double)n;
    int unit = 0;
    /* Promote as soon as the value would ROUND to 1024.0 at this scale: otherwise a size
     * just under a unit boundary prints as "1024.0 KiB" instead of "1.0 MiB" (measured with
     * a real capture of 1 048 570 bytes). */
    while (v >= 1023.95 && unit < 4)
    {
        v /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, buf_cap, "%zu B", n);
    else
        snprintf(buf, buf_cap, "%.1f %s", v, units[unit]);
}

void edc_output_buf_init(struct edc_output_buf *b, size_t cap)
{
    if (!b)
        return;
    b->data = NULL;
    b->total = 0;
    b->cap = cap;
    b->truncated = false;
}

void edc_output_buf_append(struct edc_output_buf *b, const char *p, size_t n)
{
    if (!b || !p || n == 0)
        return;

    /* Counted before any early return: |total| must be the truth about the volume even
     * when nothing is kept. */
    b->total += n;

    if (b->cap == 0)
    {
        b->truncated = true;
        return;
    }

    size_t have = b->data ? sdslen(b->data) : 0;
    if (have >= b->cap)
    {
        b->truncated = true;
        return;
    }

    size_t take = b->cap - have;
    if (take > n)
        take = n;
    if (!b->data)
        b->data = sdsempty();
    b->data = sdscatlen(b->data, p, take);
    if (take < n)
        b->truncated = true;
}

bool edc_output_buf_over(const struct edc_output_buf *b, size_t hard_max)
{
    return b && hard_max > 0 && b->total > hard_max;
}

void edc_output_buf_truncation_note(const struct edc_output_buf *b, char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        return;
    out[0] = '\0';
    if (!b || !b->truncated)
        return;

    char kept[32];
    char total[32];
    edc_output_buf_size_str(b->data ? sdslen(b->data) : 0, kept, sizeof(kept));
    edc_output_buf_size_str(b->total, total, sizeof(total));
    snprintf(out, out_cap, "[output truncated: kept the first %s of %s]", kept, total);
}

void edc_output_buf_free(struct edc_output_buf *b)
{
    if (!b)
        return;
    if (b->data)
        sdsfree(b->data);
    b->data = NULL;
    b->total = 0;
    b->truncated = false;
}
