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
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 装配根: 定义**只**在这里 (不透明) ──────────────────────
 * 外部读法: 边界经访问器, userland 收自己那几件 (见 edc_app.h)。 */
struct edc_app
{
    struct edc_config *config;
    struct edc_session_manager *session_manager;
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

/* 跨 session task 解析 / 依赖对账 / session 加载 → edc_session_store.{c,h}
 * (store 自己的读口与生命周期, 显式收 (session_manager, scheduler))。 */

/* 会话信号与落盘 (stop_all_turns / remove_all_turns) → edc_session_store.{c,h} */

/* ── 边界访问器 ────────────────────────────────────────────── */

struct edc_config *edc_app_config(const struct edc_app *rt)
{
    return rt ? rt->config : NULL;
}

struct edc_session_manager *edc_app_sessions(const struct edc_app *rt)
{
    return rt ? rt->session_manager : NULL;
}

edc_actor_t *edc_app_channel_actor(const struct edc_app *rt)
{
    return rt ? rt->channel_actor : NULL;
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

    struct edc_app *agent = g_new0(struct edc_app, 1);
    agent->config = edc_config_load(edc_home);

    /* edc_config_load() returns NULL on a missing/corrupt config.json or when
     * the stored API key cannot be decrypted (e.g. wrong passphrase / master
     * key). Everything below dereferences agent->config, so bail out cleanly
     * instead of crashing. The concrete reason is already logged by the loader. */
    if (!agent->config)
    {
        log_error("Failed to load configuration (%s); aborting startup", edc_home);
        free(agent);
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
        agent->config->enable_sandbox = (sandbox_override != 0);
    if (agent->config->enable_sandbox)
    {
        edc_sandbox_init(agent->config);
    }

    agent->channel_actor = edc_channel_actor_new();
    agent->llm_actor = edc_llm_actor_new(agent->config);
    agent->session_manager = edc_session_manager_new(agent->config);
    if (!agent->session_manager)
    {
        log_error("failed to create session manager");
        edc_app_free(agent);
        return NULL;
    }

    /* 创建 scheduler —— 不再把 active session 递给它 (P3 ③: scheduler 不持有
     * "当前 session"; 每个入口显式带 session, 标签解析交由 store 的同包读口)。 */
    agent->scheduler = edc_scheduler_new(agent->config);
    /* session 仓库句柄: 标签解析、落盘、折叠扫描都同包直调 store (与 config 同形) */
    agent->scheduler->sessions = agent->session_manager;

    /* 接线: 每个提供方把**自己的**端口装到 scheduler 上 —— 装配根只做"建对象 +
     * 叫它接线", 运行期不再出现在任何一条路径上 (见 docs/runtime-model.md §5 规则 8)。
     * 四条出站端口归通道 (消息形状是它的知识), 命令入口归命令层 (它要的 config /
     * session 仓库都在 scheduler 上)。端口签名 (edc_scheduler.h) 未动 ⇒ kernel 的
     * 调用点与测试里的假端口都不受影响。 */
    edc_channel_actor_bind_scheduler(agent->channel_actor, agent->scheduler);
    edc_commands_bind_scheduler(agent->scheduler);

    /* Phase 4: restore saved task/group state for active session */
    {
        struct edc_session *active = edc_session_manager_get_active(agent->session_manager);
        if (active)
            edc_session_store_load_session(agent->session_manager, agent->scheduler, active);
    }

    edc_channel_actor_set_observer(agent->channel_actor, edc_scheduler_get_actor(agent->scheduler));

    agent->runner = edc_actor_runner_new();
    edc_file_utils_init(&agent->runner->loop);
    edc_actor_runner_add(agent->runner, edc_scheduler_get_actor(agent->scheduler));
    edc_actor_runner_add(agent->runner, agent->channel_actor);
    edc_actor_runner_add(agent->runner, agent->llm_actor);

    agent->toolbox = edc_toolbox_new(agent->config, edc_scheduler_get_actor(agent->scheduler), agent->channel_actor);
    agent->toolbox_actor = edc_toolbox_actor_new(agent->toolbox);
    edc_actor_runner_add(agent->runner, agent->toolbox_actor);

    /* 这两条端口的实现要用到工具箱 (工具定义 / 请求链), 所以接线排在工具箱之后。 */
    edc_toolbox_actor_bind_scheduler(agent->toolbox_actor, agent->scheduler);
    edc_llm_request_bind_scheduler(agent->scheduler, agent->llm_actor, agent->toolbox ? agent->toolbox->schemas : NULL);

    edc_actor_runner_start(agent->runner);

    sdsfree(edc_home);
    return agent;
}

/* CLI 的 !n / !! 历史 → src/shell/edc_cli_history.{c,h}
 * (shell 自己的命令历史, 进程一份, 不是运行时装配; 见那里的头文件说明) */
bool edc_app_set_llm_config(struct edc_app *agent, const char *api_url, const char *model, const char *api_key)
{
    if (!agent || !agent->config)
        return false;

    /* Persist first: if the file cannot be written, leave the running agent
     * untouched so memory and disk stay in agreement. */
    if (!edc_config_update_llm(agent->config->edc_home, api_url, model, api_key))
        return false;

    struct llm_api_conf *c = &agent->config->llm_api_conf;
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
    edc_actor_send(agent->llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_SET_CONFIG, NULL, NULL, u,
                                                       (edc_actor_data_free_fn)edc_llm_api_conf_update_free));
    return true;
}


/* ── sandbox path whitelist (ro/rw) management ─────────────────────────── */

/* sandbox 配置的写口 (只碰 config) → edc_config.{c,h}
 * —— 它们与 config 的读写是同一个概念, 不该挂在 runtime 上。 */
void edc_app_shutdown(struct edc_app *rt)
{
    if (!rt || rt->shutdown_done)
        return;

    /* 1. 先停 actor (含 runner) —— 落盘必须在之后, 否则会与 actor 线程并发写。 */
    edc_actor_runner_free(rt->runner);
    rt->runner = NULL;

    /* 这三个 free **不接受 NULL** (会解引用 `actor->state`), 所以逐个判空:
     * `edc_app_new` 的失败路径上 toolbox_actor 还没建出来, 以前会在这里空指针崩。*/
    if (rt->channel_actor)
    {
        edc_channel_actor_free(rt->channel_actor);
        rt->channel_actor = NULL;
    }
    if (rt->toolbox_actor)
    {
        edc_toolbox_actor_free(rt->toolbox_actor);
        rt->toolbox_actor = NULL;
    }
    if (rt->llm_actor)
    {
        edc_llm_actor_free(rt->llm_actor);
        rt->llm_actor = NULL;
    }

    /* 2. 退出 = 关闭进程, 不是用户要 remove session: 尽量保留原状态。把所有
     * session 完整落盘 (history + state + task/group), 让中断任务以
     * LLM_PROCESSING/TOOL_CALLING + 工具账本**原样**保存, 加载时统一收敛为
     * SUSPENDED, 用户可选择是否 resume —— 因此不写 CANCELLED, 也不在取消后
     * 再落盘。 */
    if (rt->scheduler)
        edc_session_store_save_all_sessions_state(rt->session_manager, rt->scheduler);

    /* 3. 在途 turn 不再属于任何活着的 turn: 清掉 (sched 为空时这两个函数自己返回)。 */
    edc_session_store_remove_all_turns(rt->session_manager, rt->scheduler);

    rt->shutdown_done = true; /* 幂等: 上面的落盘只能做一次 */
}

void edc_app_free(struct edc_app *rt)
{
    if (!rt)
        return;

    /* 先补齐"关进程"该做的 (停 actor + 落盘 + 清在途 turn) —— 保持老的
     * "销毁即落盘"行为; 想分两步走的调用方显式先 `edc_app_shutdown()`。
     * 位置在**释放零件之前**: 落盘要用到 scheduler / session_manager。 */
    edc_app_shutdown(rt);

    edc_scheduler_free(rt->scheduler);

    if (rt->session_manager)
        edc_session_manager_free(rt->session_manager);
    if (rt->config)
        edc_config_free(rt->config);
    if (rt->toolbox)
        edc_toolbox_free(rt->toolbox);

    edc_secret_cleanup();
    free(rt);
}
