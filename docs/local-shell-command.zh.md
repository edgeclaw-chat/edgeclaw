# 用户本地 Shell 命令 `!cmd`：一个「只发起一次工具调用」的 task

> 状态：**已实现**（2026-10-14；见文末「实现记录」一节，其中记录了落地时与设计稿的三处偏差）
> 目标读者：改 `src/shell`、`src/kernel/{task,scheduler,executor}`、`src/kernel/session` 的人
> 修订：
> - v1 「纯本地逃生舱、不进 group」—— 已废弃
> - v2 「新增本地 step producer（`run_local` + `LOCAL_DONE` + 抽 `edc_proc`）」—— **已废弃**
> - **v3（本稿）**：`!cmd` = 建一个 task，**它唯一的动作就是发起一次 `exec_shell` 工具调用**。
>   执行/沙箱/超时/输出/回显/resume 全部复用现有工具链路，新增代码极少。

## 1. 背景与目标

今天用户输入只有三条路：

1. `/...` → 命令层（`edc_commands_handler`）；
2. `@...` / 纯文本 → shell DSL（`edc_shell_execute`）→ **建任务** → 走 LLM；
3. `@`/`$VAR`/`|`/`;`/`>` 等是**任务图语义**，不是真的起进程。

**用户没有任何一条「在我这台机器上跑一条命令、直接看输出、不惊动模型」的路。**

**目标**：敲 `!git status`，得到**一个 task**，它的输入是这条命令、输出是命令结果，落进当前前台组、
**自动进入 group 历史**（下一轮模型看得到、参与 fold、参与 ctx 估算），且**不消耗 token**。

**核心思路**：这个 task 不引入任何新的"执行原语"。它**只做一件事——发起一次 `exec_shell` 工具调用**，
然后结束。于是整条流程无缝接入现有的 task/工具机制。

已拍板：沙箱跟随 `config->enable_sandbox`；输出进 group 历史；**用户自己跑的命令不设超时**（要限时
自己写 `timeout 60 cmd`），因此必须能让用户把它停掉——取消时杀整个进程组（见「实现记录 · 超时与取消」）。

## 2. 交互语义

| 输入 | 行为 |
|---|---|
| `!cmd` | 建本地任务（= 发起一次 `exec_shell(cmd)`），进当前前台组 |
| `! cmd`（`!` 后有空格） | 同上（前导空白忽略） |
| `!`（后面什么都没有） | 用法提示，**不建 task** |
| `!!` | 用上一条 `!` 命令再建一个 |
| `\!cmd` | 转义：当普通文本送进正常链路（当成任务描述） |
| 非行首 `!` | 不触发 |

`!cmd` 内部不再做 shell DSL 解析（不认 `|` `;` `>` `$VAR`）——整行 remainder 就是一条命令，
交给 `exec_shell`（它本来就是 `sh -c`）。管道由 `sh` 自己处理。

> 行首 `!` 目前是**死功能**：`edc_shell_expand_history()`（`edc_shell.c:336`）的 `!!` / `!n` 读
> `edc_cli_history`，而生产路径**从不写**它（写入端只在测试里）
> → 行首 `!` 可安全挪用，`!!` 顺便复活成"重跑上条 `!` 命令"。

## 3. 为什么这样就能"无缝接进现有机制"

`exec_shell` 作为一个**工具**，今天已经拥有我们要的一切：

| 需求 | 现有机制 |
|---|---|
| 起进程、拿 stdout/stderr、超时杀整个进程组 | `edc_tools_shell.c::edc_shell_spawn()`（已有） |
| 沙箱（`enable_sandbox`） | 同一函数里的 `edc_sandbox_helper_prepare` 分支 |
| env 白名单、无控制终端（`UV_PROCESS_DETACHED`） | `edc_shell_env_build` / `UV_PROCESS_DETACHED` |
| cwd | `edc_scheduler_task_workdir(task)` → 会话工作目录 |
| 输出变成 LLM 上下文里的一条 `tool` 消息 | `executor::tool_done()` → `edc_turn_add_tool_message()` |
| 输出在 TUI 里回显 | `edc_toolbox_emit_tool_message_plain(ctx, cmd, result)`（工具自己的 TOOL_EMIT） |
| 中断后恢复（重跑未完成的调用） | 工具台账 `edc_tool_call_record`（PENDING/EXECUTING）+ `edc_toolbox_actor_redispatch()` |
| 任务生命周期、`completion_seq`、group 历史、fold、持久化 | 现有 settle 路径（`sched_apply_advance`） |

**唯一缺的两个接点**，而且都极小：

1. **起点**：任务如何"不经过 LLM"就发出这次工具调用 —— 现有的 **`exec_start` 分支 2**（"台账里有
   PENDING 调用 → 重新派发"）已经做到了。**我们只要在创建任务时预置一条 PENDING 台账记录 + 对应的
   assistant 消息**，START 就会自动走这条路，executor 一行都不用改。
2. **终点**：工具结果回来后，`tool_done()` 会接着发 LLM 请求。对这个任务我们要它**结束**而不是继续。

## 4. 数据模型

### 4.1 任务多一个布尔

```c
/* src/kernel/task/edc_task.h */
struct edc_task {
    ...
    /* 一个"只发起一次工具调用"的任务（`!cmd` 的第一个用户）：它不经过 LLM，
     * 被预置了一次工具调用；该工具链跑完就结束，绝不发 LLM 请求。持久化。 */
    bool tool_only;
};
```

刻意用**显式布尔**而不是"推断"（例如"台账空了且 `llm_rounds==0` 就结束"）：`llm_rounds` 不落盘，
resume 一个普通任务时它会是 0，推断会**误把一个正常任务的工具轮当成收尾**。

### 4.2 创建入口多一个可选参数

```c
/* src/kernel/scheduler/edc_scheduler.h */
struct edc_task_spec {
    ...
    /* 预置的起始工具调用：给了就建一个 tool_only 任务，并预置这一条调用。
     * `args` 是 JSON 参数对象（会被拷贝）。NULL = 普通任务。 */
    const char *initial_tool_name;   /* e.g. "exec_shell" */
    cJSON *initial_tool_args;        /* e.g. {"command":"git status"} */
};
```

### 4.3 turn 形态（"任务输入/输出"）

```
user      : !git status
assistant : {"role":"assistant","content":null,
             "tool_calls":[{"id":"call_local_<task_id>","type":"function",
                            "function":{"name":"exec_shell",
                                        "arguments":"{\"command\":\"git status\"}"}}]}
tool      : {"role":"tool","tool_call_id":"call_local_<task_id>","name":"exec_shell",
             "content":"On branch main\nnothing to commit"}
```

- `user` 消息 = `spec->input`（原始输入，走现有 `edc_turn_add_user_message`）= **任务的输入**；
- `assistant` 消息 = 我们预置的工具调用（**必须**有：`edc_llm_request.c` 的注释明确说
  "every role=tool message must be preceded by a matching assistant(tool_calls)"，
  少了它这条 `tool` 就是孤儿，会被 fold/窗口裁剪搞坏）；
- `tool` 消息 = 工具结果（`tool_done` 自动加）= **任务的输出**；
- 这三条一起 commit 进 turn，再由 settle 路径进 group 历史。

> 备选（不推荐）：不加 `user` 消息，turn 只有 `assistant(tool_calls)+tool`——更"纯"，但任务的
> "输入"就没了。既然要"task 的输入输出"，保留 user 消息更好读。

## 5. 改动

### 5.1 命令层（`src/shell`）

`edc_commands_handler()` 最前面拦 `!`（早于 `content[0]=='/'` 分支）：

```c
/* src/shell/edc_commands.c */
if (message->content[0] == '!')
    return edc_commands_handle_local_shell(cfg, sm, sched, to, message);
```

新增 `src/shell/edc_local_shell.{c,h}`：解析 `!` / `!!` / `\!` / 空命令 → 解析会话 → 建任务：

```c
cJSON *args = cJSON_CreateObject();
cJSON_AddStringToObject(args, "command", cmd);

struct edc_task_spec spec;
memset(&spec, 0, sizeof(spec));
spec.desc               = cmd;
spec.input              = message->content;   /* "!git status" → turn 的 user 消息 */
spec.channel_type       = message->type;
spec.place              = EDC_TASK_PLACE_FOREGROUND;
spec.take_foreground    = true;
spec.background         = false;
spec.initial_tool_name  = "exec_shell";
spec.initial_tool_args  = args;

edc_scheduler_task_create(sched, session, &spec);
cJSON_Delete(args);
```

- `raw_input` **白拿**：`edc_scheduler_task_new()` 会自动取 `sched->input_raw`
  （`edc_scheduler.c:422`，由 `sched_handle_input` 设好）→ 所以选中该任务时 TUI 画的"用户气泡"、
  `/task history` 里的 `input:` 都正常；
- `!!`：`session` 上加一个**内存**字段 `last_local_cmd`（不落盘），每次 `!cmd` 后写入。

**为什么拦截点是命令层**：`sched_handle_input()` 对非 `/`、非 `@` 输入一律当 plain 建任务，改那里要动调度器；
命令层是"所有入站输入都经过、且可以自己决定建什么样的任务"的缝。**也不在 TUI 里本地执行**：那样要重写沙箱/env/cwd。

### 5.2 `edc_scheduler_task_adopt()`：预置调用

在现有"挂 turn"之后（`edc_scheduler.c:540-549`）补一段：

```c
if (spec->input) { ...现有：建 turn、add user message... }

/* 预置的起始工具调用（`!cmd`）：assistant(tool_calls) 进 turn + 台账记一条 PENDING。
 * 之后 START 的"重新派发 PENDING"分支会自动把它发出去，executor 无需改动。 */
if (spec->initial_tool_name)
{
    task->tool_only = true;

    sds call_id = sdscatprintf(sdsempty(), "call_local_%s", task->task_id);
    if (task->active_turn)
        edc_turn_add_assistant_message(task->active_turn,
                                       edc_tool_call_assistant_msg(call_id, spec->initial_tool_name,
                                                                   spec->initial_tool_args));  /* 新建的小 helper */
    edc_task_tool_call_add(task, call_id, spec->initial_tool_name, spec->initial_tool_args);
    sdsfree(call_id);
}
```

`assistant` 消息的 JSON 形态必须和 provider 返回的一致（OpenAI 形态，见 §4.3）；
`arguments` 是**字符串**（把 `initial_tool_args` 序列化）。

### 5.3 executor：起点与终点两个守卫

**起点**（`exec_start`，在现有分支 2 之后、分支 3 `send_llm` 之前）：

```c
/* 2. 台账里的 PENDING 调用 -> 重新派发（现有代码，本地任务就走这条把调用发出去）。 */
if (edc_task_has_pending_tool_calls(task)) { ...现有... }

/* 2'. tool_only 任务已经没有待派发的调用了：说明它那一次调用**已经被答复**
 *     （resume 时 repair 把 EXECUTING 记录补成了"interrupted"工具结果）。
 *     它到此为止，绝不落到分支 3 去发 LLM 请求。 */
if (task->tool_only)
    return exec_tool_only_finish(task, task->output ? task->output : kInterruptedToolNote);
```

**终点**（`tool_done`，在 `if (!t->has_next)` 块内、发 LLM 请求之前）：

```c
if (!t->has_next)
{
    /* tool_only：这个任务存在的唯一目的就是发起这一次调用。结果已经落到 turn 里，
     * 收尾——不去问模型。 */
    if (task->tool_only)
        return exec_tool_only_finish(task, t->result);
    ...现有：max_iter / AWAIT_DEPS / send_llm...
}
```

**共同的收尾**（align `EDC_REASON_STOP` 分支的终点动作，顺序不能变）：

```c
static struct edc_advance_result exec_tool_only_finish(struct edc_task *task, const char *output)
{
    edc_task_set_output(task, sdsnew(output ? output : ""));
    edc_task_flush_redirects(task);
    if (task->active_turn)                                   /* 先入 group 历史 */
        edc_task_group_history_add_turn(task->group, task->active_turn);
    edc_task_commit_active_turn(task);                       /* 再 commit（会清台账、清 active_turn） */
    edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);

    /* notice = NULL：输出已经由工具自己的 TOOL_EMIT 显示了；再发一条 RESP 会重复，
     * 而且 RESP 被硬编码成 markdown（edc_channel_actor.c:191）会把命令输出吃掉。 */
    return (struct edc_advance_result){.outcome = EDC_STEP_DONE, .settled = true, .notice = NULL};
}
```

于是 `sched_apply_advance` 的现有 settle 路径自动完成：`completion_seq`（sink==CHANNEL）、
通知依赖、持久化、fold 检查。**"自动进入 group 历史"就靠 `history_add_turn` 那一句。**

### 5.4 fold 前的一个小守卫

`sched_fold_before_request()`（`edc_scheduler.c:703`）会在"任务即将发请求"前触发 fold。
tool_only 任务**不发任何上下文**，为它 fold 是白烧一次 LLM 调用，加一句：

```c
if (task->tool_only) return;   /* 它不发请求，没有上下文要压缩 */
```

### 5.5 显示（全部白拿）

- **用户气泡**：`TUIWindow::Submit()` 照旧（`!git status` 画成用户气泡 + 写 chat.jsonl +
  进 ArrowUp 历史）；选中该任务时由 `raw_input` 重画；
- **输出气泡**：`exec_shell` 自己的 `edc_toolbox_emit_tool_message_plain(ctx, ctx->command, result)`
  → `sched_handle_tool_emit` → `send_tool_message(header=命令, content=输出, PLAIN)`
  → TUI 画一个 **tool 气泡**（等宽、header 是命令）。**不需要任何新代码**；
  - 该 emit 只受"请求是否还活着"约束，不受"是否 active task"约束（那个门控在前端，
    见 `from_active_task`）。任务建时取了前台，所以会显示。
- **不重复**：`notice = NULL` → `edc_channel_send_task_reply` 对空 content 直接 return，
  不会有第二条 RESP 气泡。

### 5.6 持久化与 resume

- `tool_only` 落盘（`edc_state_serializer.c`），台账本来就落盘（`edc_tool_call_record`）；
- 崩溃/退出时在跑的本地任务 → 加载成 `INTERRUPTED`（和其它任务一致）；
- `/task resume` 的三种路径：
  - 记录还是 **PENDING**（没派发出去）→ `exec_start` 分支 2 重新派发 → **重跑命令**；
  - 记录是 **EXECUTING**（派发了、结果丢了）→ `repair_dangling_tool_calls` 补一条
    `[interrupted]` 工具结果并删记录 → 落到守卫 2' → 任务**结束**（输出是那条 interrupted 说明）；
  - 已经 COMMITTED → 本来就不是可 resume 的状态。
- ⚠️ **重跑不幂等**（`rm -rf` 之类）。这与现状一致：`exec_shell` 的 PENDING tool call 在 resume 时
  本来就会重新派发。**写进文档**，并建议 `/task resume` 对 `tool_only` 任务多提示一句。

## 6. 数据流

```
TUI Submit()
  ├─ 画用户气泡(!git status) + 写 chat.jsonl + 进 ArrowUp 历史
  └─ edc_app_send_user_message(sid, "!git status")
        └─ (actor) sched_handle_input          ← 设好 sched->input_raw
              └─ sched->handle_command == edc_commands_cb_handle_command
                    └─ edc_commands_handler
                          └─ content[0]=='!' → edc_commands_handle_local_shell()
                                └─ edc_scheduler_task_create(spec{
                                       input="!git status", tool_only via initial_tool_*})
                                      ├─ 进当前前台组、取前台
                                      ├─ turn: user="!git status"
                                      │        assistant(tool_calls=[exec_shell])
                                      ├─ 台账: PENDING call_local_<id>
                                      └─ (message-end scan) sched_drive_task
                                            └─ regime register + executor START
                                                  └─ 分支 2: 有 PENDING → redispatch
                                                        └─ toolbox actor → exec_shell 工具
                                                              └─ edc_shell_spawn(sh -c "git status")  …异步…
                                                                    ├─ TOOL_EMIT(header=命令, 输出) → tool 气泡
                                                                    └─ TOOL_RESPONSE
                                                                          └─ tool_done()
                                                                                ├─ turn += tool(结果)   ← 输出
                                                                                ├─ 台账删该记录
                                                                                └─ tool_only → finish
                                                                                      ├─ group 历史 += 这个 turn
                                                                                      └─ DONE/COMPLETED
                                                                          └─ sched_apply_advance
                                                                                ├─ completion_seq（进 group 内容序）
                                                                                ├─ 通知依赖、持久化
                                                                                └─ notice=NULL → 无 RESP 气泡
```

## 7. 改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/kernel/task/edc_task.{h,c}` | 加 `bool tool_only`（new/free 无需特判） |
| 2 | `src/kernel/session/edc_state_serializer.c` | `tool_only` 落盘/读回 |
| 3 | `src/kernel/scheduler/edc_scheduler.h` | `edc_task_spec` 加 `initial_tool_name` / `initial_tool_args` |
| 4 | `src/kernel/scheduler/edc_scheduler.c` | `task_adopt` 预置 turn 的 assistant(tool_calls) + 台账 PENDING；`task_new` 无需改（`raw_input` 白拿）；`sched_fold_before_request` 加 tool_only 守卫 |
| 5 | `src/kernel/executor/edc_executor.c` | `exec_start` 加守卫 2'；`tool_done` 的 `!has_next` 加 tool_only 分支；新增 `exec_tool_only_finish()` |
| 6 | `src/kernel/task/edc_turn.{h,c}`（或 executor 内） | 小 helper：造 `assistant(tool_calls)` 消息 JSON |
| 7 | `src/shell/edc_local_shell.{c,h}`（新）、`edc_commands.c/.h` | `!` 拦截 + 解析 + 建任务；`src/shell/BUILD` 加源文件 |
| 8 | `src/kernel/session/edc_session.h` | （可选）`last_local_cmd`（`!!` 用，不落盘） |
| 9 | `/help`、`docs/edgeclaw.md`、`docs/edgeclaw.1` | 文档 |

**对比 v2 省掉的重活**：不需要 `edc_proc` 抽取、不需要新事件 / 新 op、不需要
`render_pref` 改动、不需要进程生命周期登记与 `abort_all`、不需要自己拼输出文本。
（v2 的 §4.4/§4.5 整节作废。）

## 8. 测试计划

- `tests/test_edc_local_shell.c`（新）：`!` 解析（前导空格、空命令、`\!`、`!!`）、非行首 `!` 不触发。
- `tests/test_edc_scheduler.c`（改/加）：`!cmd` →
  - 建出 **1 个** task、`tool_only == true`、`sink == CHANNEL`；
  - turn 里依次是 `user` / `assistant(tool_calls)` / `tool`；
  - 台账从 PENDING → 空；
  - 完成后 `completion_seq` 有值、`group->history` 里能查到那三条消息；
  - **`send_llm` 一次都没被调用**（这是本方案的验收断言）。
- 边界：`exec_shell` 未注册 / 超时（**仅 LLM 命令**）/ 非 0 退出 → 任务仍 COMPLETED，输出是工具返回文本；
- resume：EXECUTING 丢失 → 补 interrupted 结果后任务**结束**，**不发 LLM 请求**。
- 现有 `test_edc_shell.c`：钉住"行首 `!` 不再当任务描述、不再走历史展开"。

## 9. 边界与风险

| 风险 | 处理 |
|---|---|
| `role=tool` 缺配对 `assistant(tool_calls)` → 请求非法 / fold 剪坏 | 创建时就把 assistant 消息写进 turn（§4.3）；`edc_llm_request.c` / `edc_group_fold.c` 的注释都点名了这个约束 |
| resume 后误发 LLM 请求 | 守卫 2'（§5.3） |
| `llm_rounds` 不落盘 → 不能用它推断 | 用显式 `tool_only` 布尔 |
| 非 0 退出 / 超时 | 任务仍 COMPLETED，输出是工具文本（**注意**：`exec_shell` 目前不把 exit code 写进结果，需要的话是 Phase 2 的小改）。「超时」只适用于 **LLM 发的**命令：用户命令不设超时 |
| 交互命令（sudo/vim/ssh） | 工具已是 detached 无 tty → 立即失败（现状，不是本方案引入） |
| resume 重跑不幂等 | 写进文档；`/task resume` 对 `tool_only` 加提示 |
| 输出被 markdown 吃掉 | 不再发生：显示走工具的 TOOL_EMIT（PLAIN），RESP 被 `notice=NULL` 抑制 |
| fold 前白烧一次 | `sched_fold_before_request` 跳过 tool_only（§5.4） |
| 前台排队 | `!cmd` 默认像普通输入一样**排在当前前台未完成成员后面**，保持 group 顺序确定。嫌慢可改成 `background=true`，但那样"命令后立刻发的普通消息"看到的历史顺序会反直觉 |

## 10. 落地顺序

1. `tool_only` 字段 + `spec.initial_tool_*` + `task_adopt` 预置（先写单测：能建出带 PENDING 台账和
   assistant 消息的任务）；
2. executor 两个守卫 + `exec_tool_only_finish`（单测：`send_llm` 不被调用）；
3. 命令层 `!` 拦截 + `edc_local_shell`；
4. fold 守卫、持久化、`/help` 与文档。

## 11. 顺带的一个泛化

`spec.initial_tool_* + tool_only` 是**通用**能力，不是 `!` 专用：任何"不需要 LLM 决策就要跑一次工具"的场景
（例如将来某个 `/tool <name> <args>`、或 crontab 触发的一次工具调用）都能复用它。`!cmd` 是第一个用户。

---

## 实现记录（2026-10-14）

### 落地时与设计稿的三处偏差

1. **`READY → AWAIT_TOOLS` 被状态机禁止**（`tests/test_edc_task.c:122` 明确钉住
   "must go via AWAIT_EXEC"）。所以 §5.3 里"分支 2 会自动派发"对**新建**任务不成立：
   tool-only 任务在 `exec_start` 里走自己的分支，显式 `READY → AWAIT_EXEC → AWAIT_TOOLS`
   再 `redispatch_tools`。语义上这也更准：任务先产出这次调用，再等它的结果。
2. **`(DONE, COMPLETED)` 只能从"在跑"的 step 到达**（AWAIT_EXEC / AWAIT_TOOLS；从 READY
   或非 ALIVE 的 life 出发，迁移表只允许 cancel）。因此 `exec_tool_only_finish()` 在收尾前
   会先确认自己在 AWAIT_EXEC/AWAIT_TOOLS/ALIVE，否则先补一跳 AWAIT_EXEC——否则 resume 的
   repair 路径（任务此时可能是 READY 或 AWAIT_TOOLS+INTERRUPTED）会**停在原地不结算**。
   这条是跑测试时抓出来的（`test_lost_result_settles_instead_of_calling_llm` 第一版就是 FAIL）。
3. `!!` 的记忆放在 **session 上的内存字段** `last_local_cmd`（不落盘），而不是模块静态变量
   ——`!!` 是"这个会话的上一条"，不是"这个进程的上一条"。

### 实际改动文件

- 新增：`src/shell/edc_local_shell.{c,h}`、`tests/test_local_shell.c`、本文档
- 内核：`edc_task.{h,c}`（`tool_only` + `edc_task_interrupted_tool_note()`）、
  `edc_turn.{h,c}`（`edc_turn_add_assistant_tool_call()`）、
  `edc_scheduler.{h,c}`（`spec.initial_tool_*` + `task_adopt` 预置 + fold 守卫）、
  `edc_executor.c`（起点/终点守卫 + `exec_tool_only_finish`）、
  `edc_state_serializer.c`、`edc_session.{h,c}`（`last_local_cmd`）
- 命令层：`edc_commands.c`（`!` 拦截 + `/help`）、`src/shell/BUILD`
- 文档：`docs/edgeclaw.md`、`docs/edgeclaw.1`

### 验证

- **单元**：`tests/test_local_shell.c`（4 组，48 断言）——创建预置、派发+结算且 `send_llm`
  零调用、result 丢失的 resume、`!` 入口解析；`test_state_serializer.c` 加了 `tool_only` 往返；
  `test_llm_request.c` 加了 `test_tool_only_turn_reaches_the_next_request`——钉住
  "命令 + 输出"这一轮（`user` → `assistant(tool_calls)` → `tool`，id 配对、顺序正确）**确实进了
  下一轮请求的消息数组**，也就是"自动进入 group 历史、模型下一轮看得到"这条承诺在装配层成立。
  全套 `bazel test //tests/... --nocache_test_results` → **74/74 通过**。
- **端到端**（pty 驱动真实二进制，`HOME` 指向临时目录、`EDGECLAW_MASTER_KEY` 跳过口令提示）；
  两条真实路径都跑，**都不加 `--no-sandbox` 覆盖**，让 app 自己读 config：

  | 检查 | 沙箱开（新 config 默认） | 沙箱关（用户配置） |
  |---|---|---|
  | `!echo bangA$((21*2))` → `bangA42` | ✅ | ✅ |
  | 非 0 退出 + stderr（`exit 3`）仍拿到 stdout/stderr | ✅ | ✅ |
  | `!!` 真的重跑（两个不同 `$$`） | ✅ | ✅ |
  | 读一个**不在 rw/ro 列表**里的文件 | ❌ `Permission denied` | ✅ 读到内容 |

  最后一行同时证明了两件事：**沙箱确实在 gate 子进程**，而 `!cmd` 用的就是 `exec_shell`
  那条路径（"沙箱跟随 `enable_sandbox`"这条设计声明成立）。落盘复核：每个 task 都是
  `tool_only=true / DONE / COMPLETED`，turn 恰好三条（`user` = 原始输入、`assistant` =
  `exec_shell({"command":…})`、`tool` = 输出），全部持久化在 `session.json` 里。
- 未覆盖：真实 `exec_shell` + uv loop 的**自动化**测试（既有测试都 mock `redispatch_tools`）；
  上面是脚本化 pty 手测，不是 `bazel test` 目标。
- 顺带确认的现状：`exec_shell` 不把 exit code 写进结果，所以非 0 退出时输出里看不到 `[exit 3]`；
  任务本身照常 COMPLETED。

### 上线后观察（用户自测）

用户在真实 session 里敲 `!pwd` / `!ls`，落盘为 `tool_only=true / DONE / COMPLETED` 两个任务
（task 1173 / 1174）——真实环境确认可用。

一处值得注意的观察：这两轮的 turn 进模型上下文后，**模型侧看到的可能只是裸的 `!pwd`**
（本次会话里 agent 收到 `!pwd` 后自己去跑了一次 `pwd`，属于重复劳动）。已核实这**不是**数据问题
（磁盘 turn 三条齐全、装配层也三条齐全，见上面的 `test_llm_request` 用例），更可能是展示侧的渲染
差异。若要让模型侧读起来更无歧义，可选的后续（未做）：把 user 消息写成自解释的形式
（例如 `!cmd` → "Run this command locally and show me the output:\n```\npwd\n```"），
代价是 `raw_input` / `/task history` 看到的不再是用户原样输入的那一行。

### 实现记录 · 超时与取消（2026-10-14，用户拍板后追加）

**决定**：用户自己跑的命令**不设超时**——他拥有这台机器，要限时自己写 `timeout 60 make -j8`。
AI 发的 `exec_shell` 保持原有 30s 上限（模型挑了个会挂的命令，不该把任务永久挂住）。

**实现**：判断依据不是参数，而是"这是谁的命令"——`!cmd` 是 `tool_only` 任务，工具直接读
`tb_ctx->task->tool_only`（它本来就为 cwd 读 `tb_ctx->task`）：

- `edc_tools_shell.c::edc_shell_spawn(..., timeout_sec, ...)`：**0 = 无超时**（定时器根本不启动；
  它仍会在 context 完成时被 `uv_close`，libuv 接受未启动的 handle）。
- `edc_tool_exec_shell_async()`：`user_command ? 0 : timeout_sec`。
- 没有加新参数、没动 schema（`timeout` 的 1..3600 校验对 LLM 那条路保持不变）。

**带来的必然后果（已一并修掉）**：既然不设超时，就必须能杀掉它。原本**没有任何机制**能在取消时
结束一个在跑的工具子进程——`edc_tool_shell_abort_all()` 只在 app 退出（`edc_toolbox_free`）时被调用，
工具也没有 cancel 类消息。于是加了一个端口 + 一个 **per-tool 的 abort 接口**（最初只给了 shell 用，
后来统一成通用形状，见下）：

- 端口 `edc_scheduler.abort_tools`（`edc_scheduler_abort_tools_fn`），由
  `edc_toolbox_actor_bind_scheduler` 安装（`edc_toolbox_cb_abort_tools`）。
- 调用点：`sched_abort_cancelled_tools()`，接在**两条**取消路径上——
  `edc_scheduler_send_signal_to_task_ptr()`（`/task stop <id>`）和 `sched_signal_one()`
  （`/session stop`、`/task stop --all`、退出时的扇出）。判据是"信号应用后任务确实 CANCELLED"。
- **分发**：actor 走它自己的在飞登记表 `state->inflight[]`（每次 TOOL_REQUEST 登记、回调后注销），
  按 `request_id` 匹配后把**该调用自己的 `call_ctx`** 交回给它的工具——`struct edc_tool` 上新增的
  可选钩子 `void (*abort)(struct edc_toolbox_call_ctx *ctx)`（`edc_tool_set_abort()` 安装）。
  actor 还为此在 `edc_toolbox_call_ctx` 上记了 `struct edc_tool *tool`（在 `edc_toolbox_exec` 里填，
  那是唯一把名字解析成工具的地方）。
  - 遍历**反序**：工具的 abort 可能同步完成这次调用（回调 → `inflight_remove` 把末尾元素搬进空位），
    从后往前走时"被搬走的那个"已经访问过，前面的下标不受影响。
  - **不做 timeout、不做 delay**：abort 是立刻的（shell 直接 SIGKILL 整组）。
- `edc_tools_shell.c` 的实现变成 `static void edc_tool_shell_abort(struct edc_toolbox_call_ctx *call)`：
  用**调用上下文本身**找到自己的那条在跑记录（`ctx->tb_ctx == call`）再杀——不再需要外部按 request_id
  扫它的私有表（旧版 `edc_tool_shell_kill_request(request_id)` 已删除）。语义**按工具各自定义**：
  能抢占的就中断，不能抢占的（例如已经交给线程池的文件写）只能是"放弃并丢弃结果"。

**覆盖现状**：`exec_shell` 已接。`device_shell` / `device_upload` / `device_download` **未接**
（worker 线程 + 各自私有 loop/netconn，ctx 上没有取消位、socket 也不在 ctx 上；要做真中断得加取消位 +
把 `uv_async_t` 挂到 ctx 上用 `uv_async_send` 唤醒 worker 自己的 loop，PPPoS 还要 shutdown 阻塞的
`netconn_recv`——跨线程生命周期风险 + 需要假设备才能自动化测）。fs 工具（线程池）**结构上无法抢占**，
`/task stop` 不会阻止一次已经在跑的 `edit_file` 落盘。crontab/todo/memory 无可中断窗口。

**验证**（pty 驱动真二进制，config 与用户一致＝沙箱关）：

| 检查 | 结果 |
|---|---|
| `!sleep 33; echo SURVIVED-PAST-30` | ✅ 输出 `SURVIVED-PAST-30` —— 旧的 30s 上限确实没了 |
| `!sleep 300` 跑起来后从**进程表**看 | ✅ 子进程与孙进程都在（`sh -c sleep 300` / `sleep 300`） |
| `/session stop` 之后再看进程表 | ✅ 两个都没了（整组被杀） |

任务落盘：33s 那条 `DONE/COMPLETED`，被取消那条 `DONE/CANCELLED`；日志里**没有** error/warn
（子进程死后工具结果回调落在已结算的任务上，被 request_id 查不到安全丢弃）。
