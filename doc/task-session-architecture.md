# Task / Session / Turn — 三层会话架构设计

## 概述

从当前扁平的 `edc_session`（仅含 messages 数组）重构为多层抽象，以 Linux 进程模型类比：

| EdgeClaw 概念 | Linux 概念 | 标识 | 职责 |
|---------------|-----------|------|------|
| **Turn** | Thread | `task_id` | 单次交互：用户输入 → LLM → tool call → 回复 |
| **Task** | Process | `task_id` (PID) | 一个对话任务：状态机、prompt、fork 链、IPC 依赖 |
| **Task Group** | Process Group | `group_id` = leader 的 task_id (PGID) | 信号广播单元：kill PGID → 全组成员收到 |
| **Session** | Session | `session_id` (SID) | 持久化容器：多个 group，只有一个前台组可输出 |

---

## 一、数据结构

### 1.1 三层标识体系

```
session_id "sess-001"
│
├── group gA  (group_id = "task-A1")
│   ├── task-A1  (task_id = "task-A1")  [leader] [foreground]
│   ├── task-A2  (task_id = "task-A2")  [forked_from = A1]
│   └── task-A3  (task_id = "task-A3")  [forked_from = A1]
│
├── group gB  (group_id = "task-B1")     [background]
│   └── task-B1  (task_id = "task-B1")  [leader]
│
└── group gC  (group_id = "task-C1")     [orphaned, leader exited]
    ├── task-C2  (task_id = "task-C2")  [forked_from = C1]
    └── task-C3  (task_id = "task-C3")  [forked_from = C2]
```

### 1.2 edc_task_state (Task 状态机)

```
                        /task stop ──→ CANCELLED
                        │
IDLE ──(有未满足dep)──→ WAITING_DEPS ──(所有dep完成)──→ WAITING_INPUT ──(用户输入)──→ LLM_PROCESSING
                            │                            ↑                              │
                            │                            └── 继续 LLM ──────────────────┘
                            │                                                                │
                            │                                                      tool_call ↓ finish/stop
                            │                                                         TOOL_CALLING → COMPLETED
                            │                                                                │
                            └── /task stop ──→ CANCELLED ←── /task stop ───────────────────┘
```

```c
typedef enum {
    TASK_IDLE,            // 刚创建，未开始
    TASK_WAITING_DEPS,    // 等待依赖 task 完成
    TASK_WAITING_INPUT,   // 等待用户输入
    TASK_LLM_PROCESSING,  // LLM 生成中
    TASK_TOOL_CALLING,    // 执行工具调用链
    TASK_COMPLETED,       // 任务完成
    TASK_CANCELLED,       // 被取消
    TASK_ERROR,           // 出错
} edc_task_state;
```

### 1.3 edc_task_signal (信号系统)

```c
typedef enum {
    TASK_SIGTERM,   // 优雅取消：取消 in-flight 请求，通知依赖者
    TASK_SIGKILL,   // 强制取消：立即取消，不通知依赖者，依赖者 deps_total 不减
} edc_task_signal;
```

### 1.4 edc_task_dep (Task 间依赖)

```c
typedef enum {
    TASK_DEP_JOIN,   // 等待源 task 完成，将源 output 注入自己上下文
    TASK_DEP_PIPE,   // 流式传输（后续扩展）
} edc_task_dep_type;

struct edc_task_dep {
    struct edc_task *source;       // 依赖的源 task
    struct edc_task *target;       // 被依赖的目标 task (this)
    edc_task_dep_type type;
    struct edc_task_dep *next;     // 单链表 (在 target 的 deps 链上)
};
```

### 1.5 edc_task

```c
struct edc_task {
    // 三层标识
    char *task_id;                  // PID — 任务唯一标识 (UUID)
    char *group_id;                 // PGID — 所属 group (leader 的 task_id)
    char *session_id;               // SID — 所属 session

    char *description;              // 用户可见描述
    edc_task_state state;           // 当前状态

    // 进程组关系
    int is_group_leader;            // 是否是 group leader

    // Turn 管理
    EdcQueue turns;                 // FIFO: struct edc_turn*
    struct edc_turn *active_turn;   // 当前活跃 turn

    // Actor 通信
    void *current_request_id;       // 发往 actor 的 request_id
    int cancelled_request;          // 标记请求已取消

    // 绑定
    struct edc_channel *channel;

    // Todo
    cJSON *todolist;                // JSON Array

    // ── Fork 上下文继承 ──
    struct edc_task *forked_from;   // 父 task (NULL=非 fork)
    size_t fork_point_msg_count;    // fork 时刻 session 消息总数 (切片点)

    // ── Subagent / Prompt 绑定 ──
    sds prompt;                     // 自定义 system prompt (NULL=继承默认)
    sds prompt_name;                // prompt 名称 (如 "code-reviewer")

    // ── Task IPC ──
    struct edc_task_dep *deps;          // 我依赖谁
    struct edc_task_dep *dependents;    // 谁依赖我
    size_t deps_total;                  // 总依赖数
    size_t deps_completed;              // 已完成依赖数
    struct edc_task_dep *cancelled_deps; // 被取消的依赖列表 (SIGKILL 场景)
    sds output;                         // 本 task 最终输出 (供 JOIN 注入)

    // Group 内链表 & Session tasks 链表
    struct edc_task *group_next;    // group 内下一成员
    struct edc_task *prev, *next;   // session 全局链表
};
```

### 1.6 edc_task_group

```c
struct edc_task_group {
    char *group_id;                 // PGID (= leader 的 task_id)
    struct edc_task *leader;        // group leader task
    struct edc_task *members;       // 链表头: group 内所有 task (含 leader)
    int member_count;

    struct edc_channel *channel;    // 绑定的输出通道
    int is_orphaned;                // leader 退出后置 1

    struct edc_task_group *prev, *next;  // session 内 groups 链表
};
```

### 1.7 edc_turn (微调)

```c
struct edc_turn {
    char *task_id;             // 新增：序列化时标记归属
    EdcQueue messages;         // cJSON* 队列
    EdcQueue timestamps;       // sds 队列
};
```

### 1.8 edc_session (重构)

```c
struct edc_session {
    char *session_id;               // SID
    struct edc_task_group *groups;  // 双向链表
    struct edc_task_group *foreground_group;  // 只有此组可以输出到 channel
    struct edc_config *config;
    int save_skip_count;
    struct edc_timestamp_node *timestamps;
};
```

### 1.9 edc_scheduler (新建独立模块)

```c
struct edc_scheduler {
    struct edc_session *session;    // 所属 session
    edc_actor_t *self;              // 自己的 actor handle
    edc_actor_t *llm_actor;
    edc_actor_t *channel_actor;
    edc_actor_t *memory_actor;
    struct edc_toolbox *toolbox;

    struct edc_task *task_hash;     // uthash: current_request_id → edc_task*
    struct edc_task_group *group_hash; // uthash: group_id → edc_task_group*
};
```

### 1.10 edc_agent (瘦身后)

```c
struct edc_agent {
    struct edc_config *config;
    struct edc_session *session;
    struct edc_toolbox *toolbox;
    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *memory_actor;
    edc_actor_t *toolbox_actor;
    edc_actor_runner_t *runner;
    struct edc_scheduler *scheduler;  // 调度器模块 (替代旧 scheduler actor)
};
```

---

## 二、信号系统

### 2.1 信号传播规则 (Linux 规则映射)

```
SIGTERM to task_id (PID):
  Step 1 — 处理自己:
    task->cancelled_request = 1
    向 scheduler 发 TASK_CANCEL 消息
    task->state = TASK_CANCELLED

  Step 2 — 传播给 group 成员 (若 is_group_leader):
    for member in group->members:
      if member != leader:
        edc_task_send_signal(member, SIGTERM)
    → group 变为 orphaned (leader 退出)

  Step 3 — 传播给 fork 子进程:
    for child in session: if child->forked_from == this:
      edc_task_send_signal(child, SIGTERM)

  Step 4 — 通知依赖者:
    for dep in dependents:
      edc_task_on_dep_cancelled(dep->target, this)
        → 从 deps 链中移除，deps_total--
        → cancelled_deps 记录（可选）
        → 若 target.deps_total == 0: WAITING_DEPS → WAITING_INPUT
        → 注入通知: "[系统] 依赖任务 {id} 已被取消, 可继续"

SIGTERM to group_id (PGID):
  → SIGTERM to all members of the group (每个按 PID 规则递归)

SIGTERM to session_id (SID):
  → SIGTERM to all group leaders → 每个按 PGID 规则递归

SIGKILL vs SIGTERM 区别:
  SIGKILL: 跳过 Step 4 (不通知依赖者)
    依赖者的 deps_total 不减 (永久阻塞，故意设计)
    → 适用场景: --force, 不关心下游后果
```

### 2.2 信号 API

```c
void edc_task_send_signal(edc_task *task, edc_task_signal sig);
    // 对 task 发信号，自动传播给 group (若 leader) 和 children

void edc_task_group_send_signal(edc_task_group *group, edc_task_signal sig);
    // 对 group 内所有成员发信号

void edc_session_send_signal(edc_session *session, edc_task_signal sig);
    // 对所有 group leaders 发信号 → 全部 task 收到

void edc_task_on_dep_cancelled(edc_task *target, edc_task *source);
    // SIGTERM 时调用，通知依赖者 source 已取消
```

### 2.3 Orphaned Process Group 处理

```
当 group leader 退出 (COMPLETED/CANCELLED/ERROR) 时:
  if group->member_count > 1:
    group->is_orphaned = 1
    不指定新 leader (避免竞争)
    orphaned group 自动变为 background
    成员继续执行，但:
      - 不可作为 /task group kill 的目标 (需逐个 kill)
      - /task list 显示 [ORPHAN] 标记
```

---

## 三、前台/后台与终端绑定

### 3.1 规则

| 属性 | 含义 |
|------|------|
| `foreground_group` | session 中唯一可以输出 reply 到 channel 的 group |
| `background group` | 继续执行 LLM/tool，但输出静默 |
| `session->active_task` | foreground group 中当前聚焦的 task (接收用户输入) |

### 3.2 输出行为

| 场景 | 行为 |
|------|------|
| 前台 task LLM 返回 STOP | reply 正常发送到 channel |
| 后台 task LLM 返回 STOP | reply 静默丢弃，仅设置 `task->output` |
| 后台 task 完成 | 向 channel 发一条通知: "[系统] Task {id} ({description}) completed" |
| `/task fg <id>` | 将 task 的 group 设为 foreground_group |
| `/task bg <id>` | 将 task 的 group 标记为 background |
| 用户输入 | 路由到 foreground_group 的 active_task |

### 3.3 前台切换命令

```
/task fg @               → 恢复到上一个前台 group (类似 fg %-)
/task fg <task_id>       → 将该 task 所在 group 切换到前台
/task groups             → 列出所有 groups, 标记 foreground(*) 和 orphaned
```

---

## 四、核心 API

### 4.1 edc_task API

| 函数 | 说明 |
|------|------|
| `edc_task_new(session_id, group_id, desc, channel)` | 创建 task，初始状态 IDLE |
| `edc_task_fork(parent, desc, session, channel)` | fork 子 task，继承 group 和上下文切片点 |
| `edc_task_free(task)` | 销毁 task (含 turns, deps, todolist, prompt) |
| `edc_task_set_state(task, state)` | 状态转换，返回是否合法 |
| `edc_task_add_turn(task, turn)` | 追加 turn (FIFO) |
| `edc_task_commit_active_turn(task)` | 将 active_turn 推入 turns 队列 |
| `edc_task_add_dependency(target, source, type)` | 注册依赖 (target 等 source) |
| `edc_task_on_dep_completed(target, source)` | 依赖完成通知，自动推进 |
| `edc_task_on_dep_cancelled(target, source)` | 依赖取消通知 |
| `edc_task_set_output(task, sds)` | 设置 task 最终输出 |
| `edc_task_set_prompt(task, prompt_name, prompt_text)` | 绑定 subagent prompt |
| `edc_task_get_context_messages(task, session)` | 构建 LLM 上下文 (含 fork/dep) |
| `edc_task_send_signal(task, sig)` | 发信号，自动传播 |

### 4.2 edc_task_group API

| 函数 | 说明 |
|------|------|
| `edc_task_group_new(leader)` | 创建 group (group_id = leader->task_id) |
| `edc_task_group_free(group)` | 销毁 group (释放所有 task) |
| `edc_task_group_add_task(group, task)` | 将 task 加入 group |
| `edc_task_group_remove_task(group, task)` | 将 task 移出 group |
| `edc_task_group_send_signal(group, sig)` | 对全组发信号 |
| `edc_task_group_set_leader(group, new_leader)` | 换 leader (group_id 不变) |

### 4.3 edc_session API

| 函数 | 说明 | 变化 |
|------|------|------|
| `edc_session_new(config)` | 创建 session | groups 链表初始化 |
| `edc_session_add_group(session, group)` | 添加 group | **新增** |
| `edc_session_find_task(session, task_id)` | 全局查找 task | **新增** |
| `edc_session_find_group(session, group_id)` | 查找 group | **新增** |
| `edc_session_set_foreground(session, group)` | 切换前台 | **新增** |
| `edc_session_get_messages(session)` | 获取所有消息 (cJSON*) | 按 group→task→turn 拼接 |
| `edc_session_get_messages_slice(session, from, count)` | 消息切片 | **新增** |
| `edc_session_get_message_count(session)` | 消息总数 | 不变 |
| `edc_session_save_history(session)` | 持久化 | 每条消息追加 `task_id`, `group_id` |
| `edc_session_reload(session, load_history)` | 重新加载 | 按 `group_id`→`task_id` 重建结构 |
| `edc_session_send_signal(session, sig)` | 对全 session 发信号 | **新增** |

### 4.4 edc_scheduler API

| 函数 | 说明 |
|------|------|
| `edc_scheduler_new(session, llm, channel, memory, toolbox)` | 创建调度器 |
| `edc_scheduler_free(scheduler)` | 销毁调度器 |
| `edc_scheduler_handle_message(scheduler, msg)` | 消息路由入口 |
| `edc_scheduler_get_actor(scheduler)` | 获取调度器 actor handle |

---

## 五、调度器流程

### 5.1 handle_user_input (CHANNEL_RECV)

```
check_commands(msg.content) → 匹配命令则处理并 return

group = session->foreground_group
if !group or !group->leader or group->leader.state is terminal:
    // 无前台组 → 创建新组
    leader = edc_task_new(session_id, own_id, desc, channel)
    group = edc_task_group_new(leader)
    edc_session_add_group(session, group)
    edc_session_set_foreground(session, group)

task = group leader or group active_task (若有 switch)
if !task or task.state ∈ {COMPLETED, CANCELLED, ERROR}:
    // 创建续接 task (在同一 group 内)
    task = edc_task_new(session_id, group->group_id, desc, channel)
    edc_task_group_add_task(group, task)

if task.state == WAITING_DEPS and deps_completed < deps_total:
    reply "任务依赖未满足: {列出未完成的 deps}"
    return

turn = edc_turn_new()
turn->task_id = task->task_id
edc_turn_add_user_message(turn, msg.content)
task->active_turn = turn
task->current_request_id = msg.request_id
HASH_ADD_PTR(scheduler->task_hash, current_request_id, task)
edc_task_set_state(task, TASK_LLM_PROCESSING)

messages = edc_task_get_context_messages(task, session)
req = edc_llm_actor_req_new(messages, toolbox->schemas, &turn->messages)
edc_actor_send(scheduler->llm_actor, llm_msg)
```

### 5.2 handle_llm_resp (LLM_RESPONSE)

```
task = HASH_FIND_PTR(scheduler->task_hash, &msg.request_id)
if !task or task->cancelled_request: return

if resp->error:
    edc_task_set_state(task, TASK_ERROR)
    reply_if_foreground(session, task, resp->error)
    return

if resp->reason == TOOL_CALL:
    edc_task_set_state(task, TASK_TOOL_CALLING)
    edc_turn_add_assistant_message(task->active_turn, resp->msg)
    edc_agent_handle_tool_calls(toolbox, task->channel, resp, msg->request_id)

elif resp->reason == STOP:
    edc_turn_add_assistant_message(task->active_turn, resp->msg)
    edc_task_commit_active_turn(task)
    edc_task_set_output(task, extract_content(resp))
    edc_task_set_state(task, TASK_COMPLETED)
    edc_task_notify_dependents(task)
    reply_if_foreground(session, task, resp->content)
    edc_session_save_history(session)

    // 若 leader 退出且 group 有其他成员 → 变为 orphaned
    if task->is_group_leader:
        group = edc_session_find_group(session, task->group_id)
        if group->member_count > 1:
            group->is_orphaned = 1
            notify_channel(session, "Group {group_id} is now orphaned")
```

### 5.3 handle_tool_resp (TOOL_RESPONSE)

```
task = HASH_FIND_PTR(scheduler->task_hash, &msg.request_id)
if !task or task->cancelled_request: return

edc_turn_add_tool_message(task->active_turn, ...)

if tool_chain_done:
    edc_task_set_state(task, TASK_LLM_PROCESSING)
    messages = edc_task_get_context_messages(task, session)
    edc_actor_send(scheduler->llm_actor, llm_msg)
```

---

## 六、命令清单

### 6.1 Task 控制

| 命令 | 信号 | 功能 |
|------|------|------|
| `/task new <描述>` | — | 创建新 group + leader task |
| `/task fork <父id> <描述> [--prompt <名>\|--text "..."]` | — | fork 子 task，继承 group 和上下文 |
| `/task stop <id>` | SIGTERM | 取消 task + 传播 + 通知依赖者 |
| `/task stop --force <id>` | SIGKILL | 强制取消，不通知依赖者 |
| `/task stop --group <id>` | SIGTERM to PGID | 取消整个 group |
| `/task resume <id>` | — | CANCELLED/ERROR → WAITING_INPUT |

### 6.2 终端控制

| 命令 | 功能 |
|------|------|
| `/task fg <id>` | 将 task 所在 group 设为前台 |
| `/task fg @` | 切换到上一个前台 group |
| `/task bg` | 将当前前台 group 移入后台 |
| `/task groups` | 列出所有 group (标记 * foreground, ! orphaned) |
| `/task switch <id>` | 切换 group 内活跃 task |

### 6.3 信息与依赖

| 命令 | 功能 |
|------|------|
| `/task list` | 按 group 分组列出所有 task |
| `/task status` | 显示 active_task 详情 |
| `/task chain <源id> <目标id>` | 建立 JOIN 依赖 (目标等源) |
| `/task chains [id]` | 显示 task 依赖图 |
| `/task prompt <id> [<名>\|--text "..."]` | 修改 task prompt |

### 6.4 保留命令

| 命令 | 功能 |
|------|------|
| `/new` | SIGTERM to SID — 归档 session 后全部重启 |
| `/stop` | SIGTERM to foreground PGID |
| `/help` | 显示帮助 |
| `/exit` | 退出程序 |

### 6.5 `list` / `groups` 输出格式

```
/task groups
  [*] gA  3 tasks  leader=task-1  "重构项目"
  [ ] gB  1 tasks  leader=task-2  "分析日志"
  [!] gC  2 tasks  ORPHANED       "数据迁移"     (leader exited)

/task list
  [group gA *FG]
  [F] task-1  WAITING_DEPS    3 turns  "重构项目"        (group leader)
  [B] task-2  LLM_PROCESSING  2 turns  "重构前端子任务"
  [B] task-3  COMPLETED       5 turns  "重构后端子任务"
  [group gB]
  [ ] task-2  WAITING_INPUT   0 turns  "分析日志"        (group leader)
  [group gC !ORPHAN]
  [B] task-C2 COMPLETED       2 turns  "导出数据"
  [B] task-C3 LLM_PROCESSING  1 turns  "清洗数据"

  * = foreground group, ! = orphaned
  F = foreground task, B = background task
```

---

## 七、被删除的结构

| 旧结构 | 替代 |
|--------|------|
| `struct edc_channel_turn` (edc_agent.c) | `edc_task` (含 channel 绑定和 request_id 追踪) |
| `edc_agent.turns` uthash | `edc_scheduler.task_hash` (uthash: request_id→task) |
| `edc_agent_scheduler_handler` | `edc_scheduler.handle_message` |
| `edc_session.messages` (cJSON*) | `edc_session.groups` (链表) |

---

## 八、历史持久化格式

```json
[
  {"role":"system", "content":"..."},
  {"role":"user", "content":"帮我写代码", "task_id":"task-1", "group_id":"task-1"},
  {"role":"assistant", "content":"好的", "task_id":"task-1", "group_id":"task-1"},
  {"role":"tool", "content":"...", "task_id":"task-1", "group_id":"task-1"},
  {"role":"user", "content":"再帮我分析", "task_id":"task-2", "group_id":"task-1"}
]
```

**恢复逻辑**：
- 按 `group_id` 分组 → 每个 unique `group_id` 一个 `edc_task_group`
- 按 `task_id` 分组 → 每个 unique `task_id` 一个 `edc_task`（多消息合并）
- 无 `task_id`/`group_id` → 归入 legacy group, leader = "legacy-leader"
- completed/cancelled task 保留在 session 中供 JOIN 引用

---

## 九、工作流示例

### 9.1 Fork + Subagent + 前台切换

```
用户: "review 整个项目的代码质量"

→ foreground group G1, leader=Task-Main
  Task-Main: LLM 分析 → 决定 fork 子 task:
    /task fork Main "review-frontend" --prompt code-reviewer
      → Task-FE: group=G1, prompt=skills/code-reviewer/SKILL.md
    /task fork Main "review-backend" --prompt code-reviewer
      → Task-BE: group=G1
  Task-Main: /task chain FE Main; /task chain BE Main
    → Main: WAITING_DEPS
    → Task-FE, Task-BE: WAITING_INPUT

用户: /task fg FE → foreground_group 仍是 G1 (都在同组)
用户: /task switch FE → G1.active_task = FE
用户: "review src/frontend/"
  → FE: LLM_PROCESSING → TOOL_CALLING → COMPLETED
  → Main.deps_completed++
  → channel: "[系统] Task review-frontend completed"

用户: /task switch BE → BE similarly completes
  → Main: WAITING_DEPS → WAITING_INPUT
  → Main LLM context 注入 FE/BE 的 output

用户: /task switch Main → "汇总 review 结果"
  → Main: COMPLETED, reply 输出到 channel
```

### 9.2 独立 Group + 后台执行 + SIGTERM

```
用户: /task new "分析日志" → group G2
用户: /task chain G2 G1 (不存在的语法，改为 /task switch first)

  G1: foreground group (重构项目)
  G2: background group (分析日志)

用户: /task fg G2 → G2 变为前台，G1 变为后台
  → G1.task-A [LLM_PROCESSING] 继续执行但输出静默

用户: /task fg G1
  → G2 变为后台

G2 在后台完成:
  → channel: "[系统] Task 分析日志 completed, output: {summary}"

用户: /task stop --group G2
  → SIGTERM to PGID=G2
  → 所有 G2 成员收到 SIGTERM
  → 如果 G2 中有 JOIN 依赖者，收到取消通知 deps_completed = true
```

### 9.3 SIGKILL vs SIGTERM 区别

```
Task-A 依赖 Task-B (JOIN)
Task-A 依赖 Task-C (JOIN)

用户: /task stop B     (SIGTERM)
  → B → CANCELLED
  → A.deps_total: 2 → 1, A.deps_completed unchanged
  → A 收到通知: "[系统] 依赖任务 B 已被取消"

用户: /task stop --force C  (SIGKILL)
  → C → CANCELLED
  → A.deps_total 仍是 2 (不减!)
  → A 永久阻塞于 WAITING_DEPS
  → A 的 reply 显示: "等待中: C [已 SIGKILL-ed, 请 /task chain --remove C A 或 resume C]"
```

---

## 十、实现阶段

| Phase | 内容 | 文件 | 独立编译 |
|-------|------|------|----------|
| 1 | `edc_task.h/c` — 数据结构 + 状态机 + 信号 + fork + 依赖 + context builder | 建 2 | 是 |
| 2 | `edc_task_group.h/c` — group 管理 + orphaned 处理 + 信号广播 | 建 2 | 是 |
| 3 | `edc_scheduler.h/c` — 调度器模块 (从 agent 抽取) + foreground/background 控制 | 建 2 | 是 |
| 4 | `edc_session.h/c` — groups 链表 + 序列化兼容 + foreground 切换 | 改 2 | 是 |
| 5 | `edc_agent.h/c` + `edc_commands.c` — 瘦身 + 命令族 | 改 3 | 是 |
| 6 | `edc_turn.h/c` + `src/BUILD` — task_id 字段 + 构建 | 改 3 | 是 |

---

## 十一、兼容性

- 旧 session 历史无 `task_id`/`group_id` → 归入 `legacy` group, leader = `"legacy-leader"`
- 旧命令 `/new` `/stop` `/help` `/exit` 语义映射为新信号系统
- 旧 `edc_turn` 新增 `task_id` 字段，旧代码写 NULL 不崩溃
