#ifndef EDC_VERSION_H
#define EDC_VERSION_H

/* What this binary is, for the one question every bug report needs answered.
 *
 * Before this existed, `--version` was not a thing and "unknown arguments are ignored"
 * meant a user could not even say which build they were running: two bug reports against
 * the same symptom could be a year apart. Everything here is derived from git at build
 * time (see tools/gen_version.py) rather than written down, so it cannot drift.
 *
 * The values live in a GENERATED header (edc_version_gen.h, produced by the
 * //src/common:version_header genrule). This header is the stable API over them; the
 * generated one may change shape without touching callers.
 */

#include <stddef.h>
#include <stdio.h>

#include "edc_version_gen.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The tag or describe string: "v1.6.2", "v1.6.2-3-gabc1234", "v1.6.2-3-gabc1234-dirty",
 * or "unknown" when built from a tree with no git metadata. Never NULL. */
const char *edc_version(void);

/* Abbreviated commit, or "unknown". Never NULL. */
const char *edc_version_commit(void);

/* Commit timestamp (ISO 8601), not build wall-clock: rebuilding the same commit keeps the
 * same string, which is what lets the build cache work. Never NULL. */
const char *edc_version_build_time(void);

/* Write "edgeclaw <version> (<commit>) built <time>" into buf.
 *
 * Returns the number of bytes that WOULD have been written (snprintf semantics), so a
 * caller can detect truncation. A buffer of 160 bytes is always enough.
 */
int edc_version_banner(char *buf, size_t cap);

/* "linux/aarch64", "macos/arm64", "windows/amd64": the platform triple in the same words
 * the release tarballs use. Never NULL. */
const char *edc_platform(void);

/* The `--version` block both frontends print. `schema_version` comes from the caller
 * because it lives in the kernel (EDC_DB_VERSION) and this unit must not depend upward on
 * it -- common is below kernel, and a back-edge would be a cycle. */
void edc_version_print(int schema_version, FILE *out);

/* The `--help` block, for the frontend named by `program`. Lives here, next to the
 * version banner, because the two answer the same question from the same place: "what is
 * this binary and what may I do with it" must be answerable by a binary that cannot
 * start -- no config, no passphrase, no terminal. `program` is argv[0]'s basename; NULL
 * or empty falls back to "edgeclaw".
 *
 * It lists EVERY option the binary answers, however rarely used. An earlier version said
 * "kept short on purpose: a screenful of flags is worse than a pointer to where the rest
 * lives" and listed three -- and `--update-check`, added afterwards, was then absent from
 * the one place a user looks. A flag missing from --help is indistinguishable from a flag
 * that does not exist; `tests/test_version.c` asserts the list against the parser for
 * that reason. */
void edc_usage_print(const char *program, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* EDC_VERSION_H */
