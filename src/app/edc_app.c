#include "edc_app.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_context.h"
#include "edc_init_env.h"
#include "edc_llm_actor.h"
#include "edc_llm_request.h"
#include "edc_sandbox.h"
#include "edc_secret.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── 装配根: 定义**只**在这里 (不透明) ──────────────────────
 * 外部读法: 边界经访问器, userland 收自己那几件 (见 edc_app.h)。 */
struct edc_app
{
    struct edc_config *config;
    /* 会话仓库不是本对象的字段: 它由 edc_scheduler_open 建立、归 scheduler 所有。
     * 装配根既不持有、也不代问 —— 前端要的 session_id / workdir 都在快照里。 */
    struct edc_toolbox *toolbox;
    struct edc_scheduler *scheduler;

    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *toolbox_actor;

    edc_actor_runner_t *runner;

    /* shutdown() 幂等闸门 (见 edc_app_shutdown): "落盘"只能做一次 ——
     * 重复跑一遍等于把刚恢复的状态又写一次。 */
    bool shutdown_done;
};

/* 会话仓库 / 跨 session 解析 / 落盘 → edc_scheduler_open/close: 仓库是 scheduler
 * 的私产, 装配根只建对象、叫它开、叫它关 —— 既不持有也不代问 (前端从快照里拿
 * session_id / workdir)。 */

/* ── 边界访问器 ────────────────────────────────────────────── */

struct edc_config *edc_app_config(const struct edc_app *app)
{
    return app ? app->config : NULL;
}

edc_actor_t *edc_app_channel_actor(const struct edc_app *app)
{
    return app ? app->channel_actor : NULL;
}

/* ── 生命周期 ──────────────────────────────────────────────── */

struct edc_app *edc_app_new(bool setup, int sandbox_override)
{
    sds edc_home = edc_config_resolve_bc_home();

    if (!edc_init_env(edc_home, setup))
    {
        log_error("Failed to initialize environment");
        sdsfree(edc_home);
        return NULL;
    }

    if (!edc_secret_init(false, edc_home))
    {
        log_error("Failed to initialize secret module");
        sdsfree(edc_home);
        return NULL;
    }

    struct edc_app *app = g_new0(struct edc_app, 1);
    app->config = edc_config_load(edc_home);

    /* edc_config_load() returns NULL on a missing/corrupt config.json or when
     * the stored API key cannot be decrypted (e.g. wrong passphrase / master
     * key). Everything below dereferences app->config, so bail out cleanly
     * instead of crashing. The concrete reason is already logged by the loader. */
    if (!app->config)
    {
        log_error("Failed to load configuration (%s); aborting startup", edc_home);
        free(app);
        sdsfree(edc_home);
        return NULL;
    }

    /* The master key is intentionally NOT wiped here: the /connect command
     * re-encrypts the API key when the user updates the provider at runtime,
     * so the key must stay available (it remains mlock()ed). */

    /* The master switch was loaded from config.json (default on). The CLI flag,
     * when given, overrides it for this run only: sandbox_override < 0 keeps the
     * persisted value, 0 forces off, 1 forces on. It is deliberately not
     * written back -- only the in-TUI /sandbox command persists the state. */
    if (sandbox_override >= 0)
        app->config->enable_sandbox = (sandbox_override != 0);
    if (app->config->enable_sandbox)
    {
        edc_sandbox_init(app->config);
    }

    app->channel_actor = edc_channel_actor_new();
    app->llm_actor = edc_llm_actor_new(app->config);

    /* 创建 scheduler —— 不再把 active session 递给它 (P3 ③: scheduler 不持有
     * "当前 session"; 每个入口显式带 session, 标签解析交由 store 的同包读口)。 */
    app->scheduler = edc_scheduler_new(app->config);

    /* 建 session 仓库并恢复活跃 session 的已存状态: 仓库是 scheduler 的私产,
     * 由它自己的入口建立 (本文件不再持有那个句柄)。 */
    if (!edc_scheduler_open(app->scheduler))
    {
        log_error("failed to open the scheduler session store");
        edc_app_free(app);
        return NULL;
    }

    /* 接线: 每个提供方把**自己的**端口装到 scheduler 上 —— 装配根只做"建对象 +
     * 叫它接线", 运行期不再出现在任何一条路径上 (见 docs/runtime-model.md §5 规则 8)。
     * 四条出站端口归通道 (消息形状是它的知识), 命令入口归命令层 (它要的 config /
     * session 仓库都在 scheduler 上)。端口签名 (edc_scheduler.h) 未动 ⇒ kernel 的
     * 调用点与测试里的假端口都不受影响。 */
    edc_channel_actor_bind_scheduler(app->channel_actor, app->scheduler);
    edc_commands_bind_scheduler(app->scheduler);

    edc_channel_actor_set_observer(app->channel_actor, edc_scheduler_get_actor(app->scheduler));

    app->runner = edc_actor_runner_new();
    edc_file_utils_init(&app->runner->loop);
    edc_actor_runner_add(app->runner, edc_scheduler_get_actor(app->scheduler));
    edc_actor_runner_add(app->runner, app->channel_actor);
    edc_actor_runner_add(app->runner, app->llm_actor);

    app->toolbox = edc_toolbox_new(app->config, edc_scheduler_get_actor(app->scheduler), app->channel_actor);
    app->toolbox_actor = edc_toolbox_actor_new(app->toolbox);
    edc_actor_runner_add(app->runner, app->toolbox_actor);

    /* 这两条端口的实现要用到工具箱 (工具定义 / 请求链), 所以接线排在工具箱之后。 */
    edc_toolbox_actor_bind_scheduler(app->toolbox_actor, app->scheduler);
    edc_llm_request_bind_scheduler(app->scheduler, app->llm_actor, app->toolbox ? app->toolbox->schemas : NULL);

    edc_actor_runner_start(app->runner);

    sdsfree(edc_home);
    return app;
}

/* CLI 的 !n / !! 历史 → src/shell/edc_cli_history.{c,h}
 * (shell 自己的命令历史, 进程一份, 不是运行时装配; 见那里的头文件说明) */
bool edc_app_set_llm_config(struct edc_app *app, const char *api_url, const char *model, const char *api_key)
{
    if (!app || !app->config)
        return false;

    /* Persist first: if the file cannot be written, leave the running app
     * untouched so memory and disk stay in agreement. */
    if (!edc_config_update_llm(app->config->edc_home, api_url, model, api_key))
        return false;

    struct llm_api_conf *c = &app->config->llm_api_conf;
    if (api_url && api_url[0])
    {
        sdsfree(c->api_url);
        c->api_url = sdsnew(api_url);
    }
    if (model && model[0])
    {
        sdsfree(c->model);
        c->model = sdsnew(model);
    }
    if (api_key && api_key[0])
    {
        sdsfree(c->api_key);
        c->api_key = sdsnew(api_key);
    }

    /* The llm_actor keeps its own copy of the provider settings and runs on the
     * actor-runner thread, so hand it the new values through its mailbox. */
    struct edc_llm_api_conf_update *u = calloc(1, sizeof(*u));
    if (!u)
        return true; /* config is already updated; actor refresh is best-effort */
    u->api_url = api_url ? sdsnew(api_url) : NULL;
    u->model = model ? sdsnew(model) : NULL;
    u->api_key = api_key ? sdsnew(api_key) : NULL;
    edc_actor_send(app->llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_SET_CONFIG, NULL, NULL, u,
                                                       (edc_actor_data_free_fn)edc_llm_api_conf_update_free));
    return true;
}


/* ── sandbox path whitelist (ro/rw) management ─────────────────────────── */

/* sandbox 配置的写口 (只碰 config) → edc_config.{c,h}
 * —— 它们与 config 的读写是同一个概念, 不该挂在 app 上。 */
void edc_app_shutdown(struct edc_app *app)
{
    if (!app || app->shutdown_done)
        return;

    /* 1. 先停 actor (含 runner) —— 落盘必须在之后, 否则会与 actor 线程并发写。 */
    edc_actor_runner_free(app->runner);
    app->runner = NULL;

    /* 这三个 free **不接受 NULL** (会解引用 `actor->state`), 所以逐个判空:
     * `edc_app_new` 的失败路径上 toolbox_actor 还没建出来, 以前会在这里空指针崩。*/
    if (app->channel_actor)
    {
        edc_channel_actor_free(app->channel_actor);
        app->channel_actor = NULL;
    }
    if (app->toolbox_actor)
    {
        edc_toolbox_actor_free(app->toolbox_actor);
        app->toolbox_actor = NULL;
    }
    if (app->llm_actor)
    {
        edc_llm_actor_free(app->llm_actor);
        app->llm_actor = NULL;
    }

    /* 2. 关进程: 把每个 session 原样落盘 + 清在途 turn —— 全在 scheduler 的那一个
     * 入口里 (它按同一份顺序做完, 这里不再各自列一遍)。契约: 必须在 ① 之后,
     * 否则会与 actor 线程并发写。 */
    if (app->scheduler)
        edc_scheduler_close(app->scheduler);

    app->shutdown_done = true; /* 幂等: 上面的落盘只能做一次 */
}

void edc_app_free(struct edc_app *app)
{
    if (!app)
        return;

    /* 先补齐"关进程"该做的 (停 actor + 落盘 + 清在途 turn) —— 保持老的
     * "销毁即落盘"行为; 想分两步走的调用方显式先 `edc_app_shutdown()`。
     * 位置在**释放零件之前**: 落盘要用到 scheduler。 */
    edc_app_shutdown(app);

    /* scheduler 释放时一并释放它自己建的 session 仓库 (sessions_owned)。 */
    edc_scheduler_free(app->scheduler);

    if (app->config)
        edc_config_free(app->config);
    if (app->toolbox)
        edc_toolbox_free(app->toolbox);

    edc_secret_cleanup();
    free(app);
}
