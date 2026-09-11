#ifndef __BC_INIT_ENV_H__
#define __BC_INIT_ENV_H__

#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Canonical configuration defaults ─────────────────────────────────────
 *
 * Single source of truth for every tunable in config.json. edc_init_env()
 * materializes all of them into the file at startup — both when it is created
 * on first run (non-interactive; the provider is set later with /connect) and
 * when it already exists but is missing keys (silent
 * top-up) — so config.json always shows the full configuration panorama and
 * can be edited by hand.
 *
 * edc_config.c only falls back to these values when a key has been removed
 * from the file by hand; after init that should never happen. */
#define EDC_CONFIG_DEFAULT_API_URL "https://api.deepseek.com/chat/completions"
#define EDC_CONFIG_DEFAULT_MODEL "deepseek-v4-flash"
#define EDC_CONFIG_DEFAULT_MAX_ITER (-1) /* -1 = unlimited */
#define EDC_CONFIG_DEFAULT_MEMORY_COMPRESS_THRESHOLD 200
#define EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD 500
#define EDC_CONFIG_DEFAULT_TOOL_MAX_OUTPUT_SIZE 10000
#define EDC_CONFIG_DEFAULT_CHANNEL_TYPE "adapter"
#define EDC_CONFIG_DEFAULT_DEVICE_TRANSPORT "socket"
#define EDC_CONFIG_DEFAULT_DEVICE_IP "127.0.0.1"
#define EDC_CONFIG_DEFAULT_DEVICE_PORT 5000
#define EDC_CONFIG_DEFAULT_DEVICE_BAUD 115200
#define EDC_CONFIG_DEFAULT_DEVICE_PTY "" /* empty = no serial link */

struct edc_init_env_result
{
    sds api_key;
    sds api_url;
    sds model;
    int max_iter;
    int memory_compress_threshold;
    int group_fold_msg_threshold;
    int tool_max_output_size;
    sds sandbox_rw_paths;
    sds sandbox_ro_paths;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;
};

void edc_init_env_result_free(struct edc_init_env_result *result);
bool edc_init_env(const char *edc_home, bool setup);
#endif
