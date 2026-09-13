# 当前 Session 数据模型（现状参考）

> 状态：现状快照（as-built），2026-09-13
> 依据：`src/agent/edc_session.h` / `edc_task_group.h` / `edc_task.h` / `edc_turn.h` /
> `edc_state_serializer.c` / `edc_scheduler.c` / `edc_session_manager.c`，
> 以及线上 `~/.edgeclaw/workspace` 的真实文件。
> 目的：在讨论"存储怎么改"之前，先把**当前模型**讲清楚——实体、关系、作用、不变量、薄弱点。
> 注意：本文件只描述**现状**，不含目标设计（目标见 `session-json-model.md` / `storage-design.md`）。

---

## 1. 存储分层：一份数据可能同时存在于哪几层

```
<workspace>/
├── sessions.json              # 会话索引（列表 + active_session_id），message_count 是派生 cache
├── .task_seq                  # 全局 task id 高水位（跨会话/跨重启唯一）
└── <session_id>/              # 每个会话一个目录
    ├── session.json           # ★ 结构化真相源（v2）：session + groups[] + tasks[](含 turns[]→messages[])
    ├── tasks.idx              # sidecar：一行一个 task id（供跨会话查属主，避免解析大文件）
    ├── chat.jsonl             # 渲染副本：TUI 对话气泡（append）
    └── history.jsonl          # legacy：P4 后已停写（内容并入 turn），旧文件可残留

内存独有（不持久化的视图）：
  group->history            组内共享的 LLM 上下文（引用各 turn 的消息对象，非拥有）
  group->history_task_ids   与 history 下标严格平行的归属数组
  turn->timestamps          与 messages 平行的本地时间戳队列（当前不落盘）
```

> 提示词/记忆/技能（`SOUL.md` / `MEMORY.md` / `skills/` …）与 session 模型解耦，保留文件，本文不展开。

---

## 2. 实体关系图

```
session ──1:N──> group ──1:N──> task ──1:N──> turn ──1:N──> message
  │                 │             │
  │active_task      │leader       ├── deps[] / dependents[]   (task↔task，可跨 session)
  │foreground_group │history ─────┘ (引用 turn 的消息 + 平行 task_id 数组)
  │workdir          │memory / fold_cursor     └── forked_from   (fork 链)
  └next_completion_seq
```

- **所有权（谁 free 谁）**：`message` 归 `turn`；`turn` 归 `task`（`turns` 队列 + `active_turn`）；
  `task` 归 `group`（`members` 链表）且同时挂在 `session` 全局链表（`prev/next`）。
- **唯一例外**：`group->history` 里的消息是**引用**（`cJSON_AddItemReferenceToArray`），
  对象实体仍在 turn 里——`history` 只是"按组视角的合并视图"。

---

## 3. 各结构的作用（角色定位）

| 结构 | 角色 | 关键字段 |
|---|---|---|
| **session** | 容器 + 绑定 + 序号分配器 | `workdir`(执行目录)、`active_task`、`foreground_group`、`next_completion_seq`、`groups` 链表、`state_loaded`、`history_dir` |
| **group** | **上下文共享单位** + 折叠单位 + 前台显示单位 | `leader`、`members`(有序)、`history`(合并视图)、`memory`(折叠摘要)、`fold_cursor`、`history_task_ids`、`is_orphaned` |
| **task** | 一次用户请求的**执行单元** + 状态机 | `state`(9 态)、`turns`、`active_turn`、`deps`/`dependents`、`output`、`completion_seq`、tokens 四个计数、`tool_calls`(resume 账本)、`forked_from`/`fork_point_msg_count`、`todolist`/`todo_sync_gap`、redirect |
| **turn** | task 内一个"用户输入→助手回复(含工具链)"的**回合** | `task_id`、`messages`(拥有)、`timestamps`(平行) |
| **message** | LLM 消息体 | `role` / `content` / `reasoning_content` / `tool_calls` / `name` / `tool_call_id` |
| **task_dep** | 依赖边（语义一等公民，但存储是内嵌 JSON） | `source` / `target` / `type`(JOIN/SEQUENCE) / `completed` / `cancelled` / `injected` |
| **tool_call_record** | 未完成工具调用的 resume 账本 | `call_id` / `name` / `args` / `status`(PENDING/EXECUTING) |

task 状态机（9 态）：`IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING → TOOL_CALLING`，
外加 `SUSPENDED` / `COMPLETED` / `CANCELLED` / `ERROR`。加载时未终结态一律收敛为 `SUSPENDED`。

---

## 4. 五套"定序 / 定位"机制（模型里最需要澄清的部分）

| 关注点 | 机制 | 类型 | 说明 |
|---|---|---|---|
| task id | `.task_seq` 高水位 | 全局单调 | 跨会话唯一 |
| task 完成顺序 | `session.next_completion_seq` → `task.completion_seq` | 会话内单调 | **重载时重建 group->history 的顺序依据** |
| 组上下文顺序 | `group->history` 数组顺序 | 运行时=追加序；重载=按 `completion_seq`+turn+消息序重建 | 两条路径必须"对齐" |
| 折叠边界 | `group.fold_cursor` | **下标**（history[1:] 中已折叠条数） | 折叠后仍靠下标 |
| fork 点 | `task.fork_point_msg_count` | **消息条数** | |
| 消息身份 | **无** | — | 靠数组位置隐式表达，没有 seq |

---

## 5. 一条消息的生命周期

```
用户输入
  → 建 task + group（前台组），task->active_turn 累积消息
  → 每轮 LLM/工具往复：assistant/tool 消息 push 进 active_turn->messages + timestamps
  → STOP：active_turn 的消息按引用 add_ref 进 group->history（带 task_id）
         → commit_active_turn（active_turn → turns 队列）
         → task.completion_seq = session.next_completion_seq++
  → save_state：整棵树序列化 → session.json（turn 消息内联）+ 重写 tasks.idx
  → 重载：deser 重建 group/task/turn；按 completion_seq 重放消息 → 重建 group->history
```

关键调用点：
- 运行时写入视图：`edc_scheduler.c` STOP 分支（`edc_task_group_history_add_ref`）→ `edc_task_commit_active_turn`。
- 组 system prompt：`edc_agent.c` `send_llm` 里 `history_add_owned(history[0])`（组无历史时现拼）。
- 重载重建：`edc_state_serializer.c` `deser_rebuild_group_history`。
- 上下文切片：`edc_agent.c` `send_llm`（`hist_begin = 1 + fold_cursor`，工具边界回退）。
- 折叠：`edc_memory_actor.c` L1 fold（半量，把最老一半 task 的消息折进 `group->memory`）。

---

## 6. 不变量（必须成立的约束）

1. `history[0]` = 组 system prompt（每次加载**重建**，从不持久化）。
2. `group->history[i]` 与 `group->history_task_ids[i]` 下标严格平行。
3. `group->history` 里的消息对象**生命周期由 turn 持有**（history 只是引用）。
4. `message` 必须属于某个 turn；`turn` 必须属于某个 task；`task` 必须属于某个 group。
5. `completion_seq` 决定重载后的历史顺序；运行时追加序必须与之可复现。
6. task id 全局唯一。

---

## 7. 当前薄弱点（后续"怎么改"的输入）

| # | 薄弱点 | 说明 |
|---|---|---|
| B1 | **引用耦合** | 消息被 `turn`（拥有）与 `group->history`（引用）同时持有，释放顺序敏感——`edc_session_reload` 需要悬空护栏的根因。 |
| B2 | **顺序靠"重建"** | 重载路径（`deser_rebuild_group_history`）必须与运行时追加路径逐条对齐，漏一处即错位。 |
| B3 | **边界用下标/条数** | `fold_cursor`、`fork_point_msg_count` 在裁剪/折叠后会漂移。 |
| B4 | **消息无身份** | 无 `seq`，无法表达中途插入 / 后台乱序完成 / fork 分支的线性定位。 |
| B5 | **timestamps 实际丢失** | `turn->timestamps` 只在内存，序列化器不写；重载时每条消息拿到空时间戳。 |
| B6 | **多文件无事务** | `sessions.json` / `session.json` / `tasks.idx` / `chat.jsonl` 四次独立写；`sessions.json.message_count` 是派生 cache（会漂移）。 |
| B7 | **整块重写 + 手搓索引** | `session.json` 每次 save 全量重写；`tasks.idx` 是"缺真索引"的补丁。 |

---

## 8. 与两份设计稿的关系

- `session-json-model.md`（JSON v2 目标）：在**本模型的容器内**做字段级收敛——消息加 `seq`、
  边界改 seq 水位、删 `history.jsonl`/`tasks.idx`、`chat.jsonl` 改派生、引入 system task。
- `storage-design.md`（SQLite 目标）：把本模型的**每个实体/数组映射成表**，容器换成 SQLite。

两者都以**本文件描述的归属树**为基础；差异只在"容器"与"字段收敛程度"。
