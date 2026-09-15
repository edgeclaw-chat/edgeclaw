#ifndef EDC_SANDBOX_HELPER_BLOB_H
#define EDC_SANDBOX_HELPER_BLOB_H

#include <stddef.h>

/*
 * The sandbox helper executable (see //src/agent:sandbox_helper), embedded
 * into the binary at build time so the release stays a single self-contained
 * file. edc_sandbox_helper_path() extracts it to disk on first use.
 */
extern const unsigned char edc_sandbox_helper_blob[];
extern const size_t edc_sandbox_helper_blob_size;

#endif /* EDC_SANDBOX_HELPER_BLOB_H */
