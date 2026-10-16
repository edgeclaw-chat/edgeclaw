#ifndef __BC_CONFIG_H__
#define __BC_CONFIG_H__
#include "sds.h"
#include <stdbool.h>
#include <cJSON.h>
#include <stdint.h>

/* Canonical read-only sandbox whitelist: only these system directories are
 * readable inside the sandbox. '/' is intentionally absent -- the whole
 * filesystem is NOT readable any more. The user's home directory is appended
 * as read-only at load time, and the writable set is sandbox_rw_paths.
 *
 * Kept as a comma-separated string so edc_init_env (which persists the list as
 * text) and edc_config (which stores it as an array) share one source of
 * truth. /run is needed because /etc/resolv.conf is usually a symlink into it
 * (DNS), and /sys for the many tools that inspect kernel state read-only. */
#define EDC_SANDBOX_DEFAULT_RO_PATHS "/usr,/lib,/lib32,/lib64,/libx32,/bin,/sbin,/etc,/proc,/sys,/run"

struct llm_api_conf
{
    sds model;
    sds api_key;
    sds api_url;
};

typedef enum
{
    edc_channel_adapter, /* The only channel implementation: TUI/adapter message callback.
                          * The multi-channel (telegram/feishu/qq/cli/email) implementations and
                          * registry are gone, but this enum is KEPT ON PURPOSE -- it is the
                          * "where a message came from" tag; to add a channel later, add a value
                          * here and install an adapter on the channel actor.
                          * It is still written to disk (the task/crontab "channel" key). */
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
    EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE,      /* A pull REPLY: the task tree the TUI named */
    EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE,  /* An EVENT: "some session's tree changed" (id + reason
                                                  * only, no tree) -- the TUI decides whether to pull */
    EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE,       /* REPLY payload for `/task history <id>` /
                                                  * `/group history <id>`: that task's/group's message
                                                  * history (JSON). The answer returns with the command's
                                                  * reply; the frontend only splits it into bubbles. */
} edc_channel_message_type;

/* Render preference for message content: declared explicitly by the sender (tool/app);
 * the renderer (CLI/TUI) no longer guesses from the message type / header presence. The
 * zero value PLAIN is the default -- an undeclared message always shows as plain text;
 * Markdown requires an explicit EDC_RENDER_MARKDOWN. */
typedef enum
{
    EDC_RENDER_PLAIN = 0,
    EDC_RENDER_MARKDOWN,
} edc_render_pref_t;

/* Which sandbox path list a /sandbox add-* / rm-* command targets. */
typedef enum
{
    EDC_SANDBOX_PATH_RO = 0,  /* readable only */
    EDC_SANDBOX_PATH_RW = 1,  /* readable and writable */
    EDC_SANDBOX_PATH_DENY = 2 /* masked out of the namespace; wins over ro/rw */
} edc_sandbox_path_kind;

struct edc_config
{
    sds edc_home;

    struct llm_api_conf llm_api_conf;

    sds workspace;
    bool enable_sandbox;
    /* Background services master switch (the shared-timer "silent checks", see
     * edc_service.h). Defaults to on; config.json's top-level key `services_enabled`
     * may turn it off (then the timer never arms; manual pumps still work). */
    bool services_enabled;
    /* First-run wizard state: false until the user finishes or skips the setup page.
     * Persisted (config.json top-level "onboarded"); the TUI opens the setup page once
     * when it is false. A stored flag, not "is anything configured": clearing the key
     * later must not make the wizard reappear. */
    bool onboarded;
    /* Turn off "context file" discovery (--no-context-files / -nc in pi): do not read the
     * user-global and workdir-tree AGENTS.md / CLAUDE.md, leaving only the workspace's
     * bootstrap files in the prompt. config.json's top-level key `skip_context_files`
     * (default -> false). */
    bool skip_context_files;
    /* Whether the SHELL-LIKE input syntax is on: the sigils `+<desc>` (a new group) and
     * `!<cmd>` (the local shell), and the operators `|` `;` `&` `>` `>>` `2>` `<`. OFF by
     * default -- those operators spell out a Unix shell vocabulary, which a user coming
     * from another system has no reason to know, and getting them wrong is silent (the
     * line just becomes one odd task). config.json's top-level `shell_syntax` (true)
     * turns it on. Flipped at runtime ONLY through /shell on|off -- deliberately no CLI
     * flag, so there is one way to say it and it is persisted with it. */
    bool shell_syntax;
    /* Read the project-level resource dirs (./.edgeclaw, ./.claude, ./.agents, ./.opencode,
     * ./.pi) for prompt templates and skills. ON by default; config.json's top-level
     * `load_project_resources` (false) turns it off. The user-level roots under $HOME are
     * never gated by this. */
    bool load_project_resources;
    sds sandbox_root_real;
    sds *sandbox_ro_paths;
    sds *sandbox_rw_paths;
    sds *sandbox_deny_paths;
    int max_iter; /* LLM/tool iteration budget: -1 = unlimited, N > 0 = cap, <= 0 (incl. 0) = no limit */
    /* Declared model context window in tokens. Always positive: edc_init_env()
     * repairs a missing or <= 0 value to EDC_CONFIG_DEFAULT_CONTEXT_WINDOW, and
     * the fold compares the group's context against `window - reserve`. */
    int context_window;
    int tool_max_output_size;
    /* Shell output limits (see docs/tool-output-limits.zh.md). 0 = use the compile-time
     * default (1 MiB kept per stream / 256 MiB before the command is killed). Deliberately
     * NOT read from config.json: they exist so the policy is a value a test can pin -- a
     * runaway test would otherwise have to emit 256 MiB to reach the kill. Promoting them
     * to config keys later is a two-line change. */
    int shell_capture_max;
    int shell_output_kill_max;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;
};

extern sds edc_config_resolve_bc_home(void);
extern struct edc_config *edc_config_load(const char *edc_home);
extern void edc_config_free(struct edc_config *config);

/* Grant read+write inside the filesystem sandbox to |path| (dedup by realpath,
 * prepended to sandbox_rw_paths). Used by a session binding a working
 * directory so the bound directory is admissible under both the kernel sandbox
 * and in-process path validation. Returns true when the path is in the set. */
extern bool edc_config_add_sandbox_rw(struct edc_config *conf, const char *path);

/* Rewrite api_url / model / api_key in <edc_home>/config.json. The key is
 * encrypted before writing. A NULL argument leaves that field unchanged.
 * Returns true on success. Used by the app's /connect command. */
extern bool edc_config_update_llm(const char *edc_home, const char *api_url, const char *model, const char *api_key);

/* Persist the first-run wizard flag (config.json top-level "onboarded"), preserving
 * every other setting. Set it once the user finishes OR skips the setup page, so the
 * wizard does not reappear. Returns true on success. */
extern bool edc_config_update_onboarded(const char *edc_home, bool onboarded);
/* Persist first, then update the running config (see /sandbox's pattern). */
extern bool edc_config_set_onboarded(struct edc_config *cfg, bool onboarded);

/* Persist the context window (config.json top-level "context_window"). The wizard writes
 * the chosen model's catalog limit here. |context_window| <= 0 is refused: the window has
 * no "unconfigured" state (see edc_init_env). Returns true on success. */
extern bool edc_config_update_context_window(const char *edc_home, int context_window);

/* Persist the sandbox master switch ("sandbox.enable") in config.json,
 * preserving all other settings. Used by the in-TUI /sandbox command; the CLI
 * --sandbox flag is a per-run override and is intentionally not persisted.
 * Returns true on success. */
extern bool edc_config_update_sandbox_enabled(const char *edc_home, bool enable);

/* Persist the shell-like syntax switch (config.json top-level "shell_syntax"),
 * preserving all other settings. Used by the in-TUI /shell command -- which is the ONLY
 * way to flip it (there is deliberately no CLI flag). Returns true on success. */
extern bool edc_config_update_shell_syntax(const char *edc_home, bool enable);
/* Persist first, then update the running config (see /sandbox's pattern). */
extern bool edc_config_set_shell_syntax(struct edc_config *cfg, bool enable);

/* Add (add=true) or remove (add=false) a directory from a sandbox path list
 * and persist it to <edc_home>/config.json, preserving all other settings.
 * |kind| selects ro_paths / rw_paths / deny_paths. A path is kept in exactly
 * one list: adding it to one drops it from the other two. |path| must be
 * absolute. Returns true on success. Used by /sandbox add-ro|add-rw|add-deny
 * and the matching rm-* commands. */
extern bool edc_config_update_sandbox_path(const char *edc_home, const char *path, edc_sandbox_path_kind kind,
                                           bool add);
/* Sandbox config writers: persist to config.json first, only then change memory -- memory
 * and disk stay consistent on failure (see the implementation). They touch config only,
 * so they are the config module's API. */
extern bool edc_config_set_sandbox_enabled(struct edc_config *cfg, bool enable);
extern bool edc_config_set_sandbox_path(struct edc_config *cfg, const char *path, edc_sandbox_path_kind kind, bool add,
                                        sds *msg);

/* Channel serialization (the only channel = adapter): writes the task/message "channel"
 * field; an on-disk format compatibility concern. It used to live in
 * edc_channel_actor.{c,h} -- a boundary module -- but the kernel's state_serializer needs
 * it too, which would draw a "low-level -> boundary" reverse edge; moved next to
 * edc_channel_type's definition (this file), the direction is right. */
extern cJSON *edc_channel_to_json(void);

#endif
