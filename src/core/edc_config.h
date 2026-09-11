#ifndef __BC_CONFIG_H__
#define __BC_CONFIG_H__
#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

struct llm_api_conf
{
    sds model;
    sds api_key;
    sds api_url;
};

typedef enum
{
    edc_channel_adapter, /* 唯一通道实现: TUI/adapter 消息回调 */
} edc_channel_type;

/* The kind of content a channel message carries. Defined here (rather than in
 * edc_channel_actor.h) so headers that only need the type, like edc_scheduler,
 * don't have to pull in sds/cJSON/uthash via edc_channel_actor.h. */
typedef enum
{
    EDC_CHANNEL_MESSAGE_USER_TYPE = 0,
    EDC_CHANNEL_MESSAGE_TOOL_TYPE,
    EDC_CHANNEL_MESSAGE_THOUGHT_TYPE,
    EDC_CHANNEL_MESSAGE_RESP_TYPE,
    EDC_CHANNEL_MESSAGE_SYS_TYPE,
    EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE, /* serialized task-tree snapshot for the TUI */
} edc_channel_message_type;

/* 消息内容的渲染偏好：由发送方(tool/agent)显式声明，渲染端(CLI/TUI)
 * 不再按消息类型/header 有无去猜测。零值 PLAIN 即默认——发送方未声明时
 * 消息一律按纯文本显示，想按 Markdown 渲染必须显式声明 EDC_RENDER_MARKDOWN。 */
typedef enum
{
    EDC_RENDER_PLAIN = 0,
    EDC_RENDER_MARKDOWN,
} edc_render_pref_t;

struct channel_conf
{
    edc_channel_type type;
};

struct edc_config
{
    sds edc_home;

    struct llm_api_conf llm_api_conf;
    struct channel_conf channel;

    sds workspace;
    bool enable_sandbox;
    sds sandbox_root_real;
    sds *sandbox_ro_paths;
    sds *sandbox_rw_paths;
    int max_iter;            /* LLM/tool 迭代预算: -1 = 无限, N > 0 = 上限, <= 0 (含 0) = 不限制 */
    int memory_compress_threshold;
    int group_fold_msg_threshold;
    int tool_max_output_size;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;
};

extern sds edc_config_resolve_bc_home(void);
extern struct edc_config *edc_config_load(const char *edc_home);
extern void edc_config_free(struct edc_config *config);

/* Rewrite api_url / model / api_key in <edc_home>/config.json. The key is
 * encrypted before writing. A NULL argument leaves that field unchanged.
 * Returns true on success. Used by the runtime /connect command. */
extern bool edc_config_update_llm(const char *edc_home, const char *api_url, const char *model, const char *api_key);

/* Persist the sandbox master switch ("sandbox.enable") in config.json,
 * preserving all other settings. Used by the in-TUI /sandbox command; the CLI
 * --sandbox flag is a per-run override and is intentionally not persisted.
 * Returns true on success. */
extern bool edc_config_update_sandbox_enabled(const char *edc_home, bool enable);
#endif
