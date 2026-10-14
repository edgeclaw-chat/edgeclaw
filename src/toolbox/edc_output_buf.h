#ifndef __EDC_OUTPUT_BUF_H__
#define __EDC_OUTPUT_BUF_H__

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

/* ── A capped accumulator for tool output ─────────────────────
 *
 * Tools that stream data in from the outside (a child process's pipes, a socket) must not
 * let that data decide how much memory the process uses. This is the one place that rule
 * lives: bytes offered are always COUNTED, but only the first |cap| are KEPT, so memory is
 * flat no matter how much a command emits.
 *
 * Deliberately pure data -- no I/O, no libuv, no config: the limit itself becomes unit
 * testable, which it never was while it lived inside an event-loop callback.
 *
 * Truncation is reported, never hidden: |truncated| says it happened and
 * edc_output_buf_note_truncation() appends the "kept the first X of Y" line, so the tool
 * result tells the reader what it is missing. */
struct edc_output_buf
{
    sds data;        /* at most |cap| bytes: exactly what the caller will get */
    size_t total;    /* every byte offered, kept or dropped */
    size_t cap;      /* 0 = keep nothing (NOT "unlimited": the fail-safe direction) */
    bool truncated;  /* something was dropped */
};

/** |cap| bytes are kept, everything after that is only counted. */
extern void edc_output_buf_init(struct edc_output_buf *b, size_t cap);

/** Offer |n| bytes at |p|. Keeps what fits, counts the rest. */
extern void edc_output_buf_append(struct edc_output_buf *b, const char *p, size_t n);

/** True once more than |hard_max| bytes have been offered in total (the runaway test).
 *  Separate from |cap| on purpose: a legitimately verbose command is truncated, not
 *  killed; only one that never stops gets killed. */
extern bool edc_output_buf_over(const struct edc_output_buf *b, size_t hard_max);

/** Format the truncation note ("kept the first X of Y") into |out|. No-op text when nothing
 *  was dropped (writes an empty string). Callers put it in FRONT of the payload: that is
 *  where it survives -- the collapsed bubble draws only the first lines, and the model's
 *  copy is cut at tool_max_output_size, both from the head. */
extern void edc_output_buf_truncation_note(const struct edc_output_buf *b, char *out, size_t out_cap);

/** Release |data|; the struct stays usable after another init. */
extern void edc_output_buf_free(struct edc_output_buf *b);

/** Compact byte count for a human: "812 B" / "1.0 MiB" / "3.2 GiB". */
extern void edc_output_buf_size_str(size_t n, char *buf, size_t buf_cap);

#endif
