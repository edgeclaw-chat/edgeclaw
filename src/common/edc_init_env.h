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
/* The model's context window, in tokens. The fold trigger compares the group's
 * context against `window - EDC_FOLD_RESERVE_TOKENS` (see edc_group_fold_due) --
 * there is no other trigger, so this key is always meaningful and is never 0.
 *
 * 1M is the window of the default model (`deepseek-v4-flash` reports 1048576 via
 * GET /models; measured in docs/pi-alignment.md 2.5.1). A different model needs
 * this edited to match, or a window bigger than the real one postpones folding
 * past the provider's own limit -- and a window much smaller than the group's
 * baseline context (system prompt + memory) makes it fold every round.
 *
 * `<= 0` (unknown) is not a legal setting: edc_init_env repairs it to this default
 * on startup, and edc_group_fold_due() refuses to fold automatically when it finds
 * one anyway (not knowing the window is not a reason to guess). */
#define EDC_CONFIG_DEFAULT_CONTEXT_WINDOW (1024 * 1024)
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
    int context_window;
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
