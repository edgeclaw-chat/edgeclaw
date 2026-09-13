# session.json v2 存储模型（JSON 版）

> 状态：设计稿 / 待评审
> 目标：把 JSON 版的存储体系梳理清楚——**turn 拥有消息、全局 seq 定序、单一真相源**；
> 结构对齐未来的 SQLite 表（每个对象→一行，每个数组→子表），迁移只做字段搬运。
> 前提：**SQLite vs JSON 只是容器**，本文件只定义模型。

---

## 0. 一句话

`session.json` 成为**每个 session 唯一的、自洽的结构化真相源**：一条
`session → groups[] → tasks[] → turns[] → messages[]` 的归属树，消息体内联在所属 turn 下，
顺序由**全局 `seq`** 决定。删除 `history.jsonl`（消息内容）与 `tasks.idx`（不再需要 sidecar）。

---

## 1. 设计原则

1. **归属清晰**：每个 message 一定属于某个 turn，每个 turn 属于某个 task，每个 task 属于某个 group，每个 group 属于本 session。没有"无主的消息"。
2. **顺序显式**：每条 message 带 session 内单调的 `seq`。**顺序不再靠数组下标隐式表达**，因此不存在"区间漂移"。
3. **单一副本**：消息体只在所属 turn 里存一份，不再有 `session->messages` 与 turn 队列双份。
4. **无位置耦合**：不再有 `message_start/message_count`；不再有 `range_valid` / EMBED 双模式。
5. **可迁移**：字段 = 未来 DB 的列；嵌套数组 = 未来 DB 的子表（靠 id 关联）。见 §9。

---

## 2. 实体与归属树

```
session (session.json 根)
└── groups[]       组（前台/后台、orphan、memory 摘要、折叠水位）
    └── tasks[]    任务（状态、依赖、tokens、fork 链、tool_calls 账本）
        └── turns[] 回合（committed / active）
            └── messages[]  消息体（线性顺序由 seq 决定）
```

- **一个 session 一个文件**：`<workspace>/<session_id>/session.json`。
- **保留一个 system 组/任务**（§6）承载非用户任务的消息（迁移的旧消息、系统注入、压缩上下文）。

---

## 3. `session.json` v2 结构（示例）

```json
{
  "version": 2,
  "session": {
    "id": "s7",
    "name": "default",
    "created_at": "2026-09-11 16:43:24",
    "updated_at": "2026-09-13 12:25:00",
    "workdir": "/home/conley/workspace/github/edgeclaw-agent",
    "active_task_id": "204",
    "foreground_group_id": "197",
    "next_completion_seq": 12,
    "next_message_seq": 1119,
    "message_count": 1118,
    "tokens": {"input":0,"output":0,"reasoning":0,"cache_read":0,"cache_write":0},
    "metadata": {}
  },
  "groups": [
    {
      "id": "197",
      "leader_task_id": "197",
      "is_orphaned": false,
      "folded_through_seq": 0,
      "memory": null
    }
  ],
  "tasks": [
    {
      "id": "204",
      "group_id": "197",
      "description": "有一个问题，现在session.json里存了...",
      "state": "COMPLETED",
      "is_group_leader": false,
      "is_system": false,
      "auto_reap": false,
      "channel": {"type": "adapter"},
      "prompt_name": null,
      "prompt": null,
      "forked_from_task_id": null,
      "fork_point_seq": null,
      "output": null,
      "completion_seq": 11,
      "turns_folded": false,
      "tokens": {"prompt":1,"completion":1,"total":2,"cached":0},
      "redirect": {"stdout": null, "stderr": null, "append": false},
      "created_at": "2026-09-13 11:40:00",
      "updated_at": "2026-09-13 12:25:00",
      "metadata": {},
      "deps": [
        {"source_task_id": "203", "type": "seq", "completed": true, "cancelled": false, "injected": false}
      ],
      "tool_calls": [
        {"call_id": "call_00_x", "name": "exec_shell", "args": {"command": "ls"}, "status": "pending"}
      ],
      "turns": [
        {
          "index": 0,
          "is_active": false,
          "messages": [
            {"seq": 1084, "role": "user", "content": "...", "timestamp": "2026-09-13 11:40:01"},
            {"seq": 1085, "role": "assistant", "content": "...", "reasoning_content": "...",
             "tool_calls": [{"id":"call_00_y","type":"function","function":{"name":"exec_shell","arguments":"{...}"}}],
             "timestamp": "2026-09-13 11:40:03"},
            {"seq": 1086, "role": "tool", "tool_call_id": "call_00_y", "name": "exec_shell",
             "content": "BUILD\n...", "timestamp": "2026-09-13 11:40:04"}
          ]
        }
      ]
    }
  ]
}
```

> 说明：示例只展示一个 group/task。真实文件是三类数组 + 内联消息体，**不含任何下标区间**。

---

## 4. 字段规范

### 4.1 `session`

| 字段 | 类型 | 说明 | 对应旧字段 |
|---|---|---|---|
| `id` / `name` | string | 标识、显示名 | 同名 |
| `created_at` / `updated_at` | string | ISO 风格时间戳 | `created_at` / `last_active` |
| `workdir` | string? | 绑定工作目录（绝对 realpath），可空 | `workdir` |
| `active_task_id` | string? | 当前 active task | 同名 |
| `foreground_group_id` | string? | 前台组 | 同名 |
| `next_completion_seq` | int | 完成序号分配器 | 同名 |
| `next_message_seq` | int | **消息 seq 分配器（新增）** | 替代"数组长度/下标" |
| `message_count` | int | 派生缓存 = 所有 turn 消息数 | 同名 |
| `tokens` | object | session 级 token 汇总（新增，可空） | 无（原在 task） |
| `metadata` | object | 兜底扩展位 | 无 |

### 4.2 `groups[]`

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | 组 id（或保留值 `"system"`） |
| `leader_task_id` | string | leader 任务 id（orphan 组回退为组 id） |
| `is_orphaned` | bool | 组是否孤立 |
| `memory` | string? | 组内折叠摘要（L1 压缩产物） |
| `folded_through_seq` | int | **折叠水位：seq ≤ 它的前缀消息已折进 `memory`** |
| `member_task_ids[]` | array | **有序**成员列表（leader 在前，其余按加入序） |

> **变更**：`fold_cursor`（下标）→ `folded_through_seq`（seq 水位），避免压缩/裁剪后错位。
> **保留** `member_task_ids[]`：成员集合虽可由 `tasks[].group_id` 反查，但**顺序是被使用的**
> （侧边栏按 `group->members` 顺序渲染，leader 在前），且它直接对应未来的
> `task_group_member(group_id, task_id, position)`——保留更清晰、更好迁移。

### 4.3 `tasks[]`

| 字段 | 类型 | 说明 | 对应旧字段 |
|---|---|---|---|
| `id` / `group_id` | string | task id、所属组 | 同名（去掉冗余 `session_id`，见 §5 注） |
| `description` / `state` | string | 描述、状态枚举 | 同名 |
| `is_group_leader` / `auto_reap` / `is_system` | bool | 标志位（`is_system` 新增） | 前二者同名 |
| `channel` | object | 来源通道 | `channel` |
| `prompt_name` / `prompt` | string? | 提示词 | 同名 |
| `forked_from_task_id` | string? | fork 父任务 | 同名 |
| `fork_point_seq` | int? | **fork 点（父的 seq，替代 `fork_point_msg_count`）** | `fork_point_msg_count` |
| `output` / `completion_seq` / `turns_folded` | string/int/bool | 结果、完成序号、是否折叠 | 同名 |
| `tokens` | object | `{prompt,completion,total,cached}` | `tokens_*` |
| `redirect` | object | `{stdout,stderr,append}` | `redirect_*` |
| `created_at` / `updated_at` | string | 时间 | `created_at`（新增 updated） |
| `todos[]` | array | 待办清单（**任务级**，替代 `todolist`） | `todolist` |
| `todo_sync_gap` | int | 提醒节流计数 | `todo_sync_gap` |
| `metadata` | object | 兜底扩展位 | 无 |
| `deps[]` | array | 依赖边 | `deps[]` |
| `tool_calls[]` | array | 未完成工具调用 resume 账本 | `tool_calls[]` |
| `turns[]` | array | 回合 | `turns[]` + `active_turn` |

> **定稿**：todo **保持任务级**（与现状一致）——`tasks[].todos[]` 替代 `todolist`，`todo_sync_gap`
> 保留。理由：当前 todo 就是 task 作用域，TodoWidget 展示的是前台 active task 的清单；改成会话级
> 会改变语义。
> 旧 `session_id` 字段去掉：一个文件就是一个 session，冗余会带来不一致风险。

### 4.4 `turns[]`

| 字段 | 类型 | 说明 |
|---|---|---|
| `index` | int | 回合在该 task 内的序号（0 起） |
| `is_active` | bool | 是否在途（原 `active_turn`） |
| `messages[]` | array | 本回合的消息（内联） |

> **变更**：删除 `message_start` / `message_count`（区间）；`active_turn` 并入 `turns[]`（`is_active=true`），
> 不再用 `turn_index=-1` 的旁路字段。

### 4.5 `messages[]`

| 字段 | 类型 | 说明 |
|---|---|---|
| `seq` | int | **session 内唯一、单调递增；消息的身份与顺序键** |
| `role` | string | `user` / `assistant` / `tool` / `system` |
| `content` | string | 正文 |
| `reasoning_content` | string? | 思维链 |
| `name` | string? | tool 名 |
| `tool_call_id` | string? | tool 消息对应的调用 id |
| `tool_calls` | array? | assistant 发起的工具调用（多态 JSON 负载） |
| `render` | string | `plain` / `markdown`（渲染偏好） |
| `timestamp` | string | 产生时间 |

> **变更**：消息不再以"位置"存在，`seq` 即身份。消息体**只存一份**，就在所属 turn 下。

---

## 5. 顺序与上下文重建（seq 是唯一权威）

- **线性会话顺序** = 所有 message 按 `seq` 升序。
- **组的 LLM 上下文**（原 `group->history`）= 该组所有 task 的 message，按 `seq` 升序；若 `folded_through_seq > 0`，则前缀已折进 `group.memory`，只取 `seq > folded_through_seq` 的部分。
- **task 的回合视图** = 该 task 的 `turns[].messages[]`（内部已有序）。
- **fork 点** 用 `fork_point_seq` 表达：子任务快照 = 父组中 `seq ≤ fork_point_seq` 的未折叠消息。
  - **重要**：fork 快照是**物化的副本**。为保证 `seq` 唯一，子任务的快照消息**分配新的 seq**（它们是新分支的独立上下文）。父任务不受影响。

> 这套"全局 seq + 归属标签"能同时表达：任务顺序、用户中途插入、fork 分支、后台任务乱序完成——
> 而这些正是"靠数组下标"表达不了、现在要靠 `completion_seq` 二次重建的地方。

---

## 6. system 组 / system task

保留一个特殊承载位：

- `groups[].id = "system"`，其中 `tasks[].id = "system"`、`is_system = true`。
- 用途：
  1. **迁移旧消息**：任务模型之前产生的、无 task 归属的消息（s1/s2/s4/s5）；
  2. **会话级系统消息**：心跳/cron 注入、压缩上下文替换消息等。
- **不放 system prompt**：system prompt 每次加载由 `edc_context_build_system_prompt(config, workdir)` **重建**（改了 SOUL/AGENTS/skills/MEMORY 才会生效）。一旦落盘就成陈旧副本，**禁止持久化**。
- system task 的 message 同样带 `seq`，因此能与真实任务消息一起参与线性排序。

---

## 7. 文件布局与读写语义

### 7.1 保留 / 删除 / 派生

| 文件 | 处置 |
|---|---|
| `<sid>/session.json` | **唯一真相源**（v2 结构） |
| `<sid>/history.jsonl` | **删除**（内容并入 turn 内联消息） |
| `<sid>/tasks.idx` | **删除**（不再有跨会话查 id 的 sidecar 需求） |
| `<sid>/chat.jsonl` | **改为派生**：由 message 重新渲染（见 §8 开放项）；确认后删除 |
| `<workspace>/sessions.json` | **保留为索引缓存**（列表用），真相以 session.json 为准 |
| `<workspace>/.task_seq` | 保留（**task id 全局**唯一分配器）；消息 seq 用 `session.next_message_seq` |
| `<workspace>/jobs.json` | 保留（定时任务，独立话题） |
| `memory/*.md`、`SOUL/AGENTS/TOOLS/USER/HEARTBEAT`、`skills/`、`ssl/` | 保留文件 |
| `config.json` | 保留文件（密钥仍加密） |

### 7.2 读写语义

- **写**：从内存 session 直接序列化整棵树（**无重复、无区间**）→ 写 `.tmp` → `rename`（原子）。
- **读**：解析 → 重建 group/task/turn/message → **按 `seq` 重建各 group 的上下文**。
- **不再有** `range_valid` / EMBED 双模式、不再有指针查找、不再有 `session->messages` 副本。

> `save_skip_count` / `edc_history_save` 的 append 机制随 history.jsonl 一起移除。

---

## 8. 已定决策（2026-09-13）

1. **todo 保持任务级** → `tasks[].todos[]` + `tasks[].todo_sync_gap`（见 §4.3）。现状即任务级，不改语义。
2. **chat.jsonl 改为纯派生**（删除该文件）。做法：TUI 在加载/切会话时，遍历本 session 的 `message`
   （按 `seq`）用运行时同一套 kind 映射重建气泡：
   - `user` 消息 → `user` 气泡；
   - `assistant` 消息 → `content` 出 `reply`、`reasoning_content` 出 `thought`、`tool_calls` 供 tool 气泡 header；
   - `tool` 消息 → `tool` 气泡（header 由对应 `tool_calls` 的 `name`+`args` 重建）；
   - system / system task 消息 → `sys` 气泡。

   **前置条件**（缺一不可）：① message 必须持久化 `render`（现状只挂在 channel message 上、未落盘）；
   ② message 的 task 归属能从结构取到（v2 走 turn→task）；③ tool 气泡 `header` 由 `tool_calls` 重建
   （**不保证与旧 chat.jsonl 逐字节一致**）。
   **派生不出的**：不落 message 的纯 UI 气泡（复制/状态提示、临时错误等）。处置：要么把它们也作为
   `sys` 消息存进 system task，要么接受其不回放。
3. **保留 `member_task_ids[]`**（有序）。理由见 §4.2——顺序被侧边栏使用，且对应未来的
   `task_group_member(position)`。
4. **接受全量重写代价**（已确认）。每次保存重写整个 `session.json`。缓解（定期归档 / 按 group 分片）
   列为后续版本，不阻塞本设计。

---

## 9. 为什么这个 JSON 好迁移到 SQLite

每个 JSON 概念与未来表一一对应，且**无位置耦合**：

| JSON | SQLite |
|---|---|
| `session` 对象 | `session` 一行 |
| `groups[]` | `task_group` 行 |
| `tasks[]` | `task` 行 |
| `tasks[].deps[]` | `task_dep` 行 |
| `tasks[].tool_calls[]` | `task_tool_call` 行 |
| `tasks[].turns[]` | `turn` 行（`task_id` 外键） |
| `turns[].messages[]` | `message` 行（`turn_id`、`task_id`、`seq`） |
| `messages[].seq` | `message` 唯一键/主键 |

**关键**：因为消息归属于 turn（`message.turn_id`）、顺序靠 `seq`，迁库时**只需把嵌套数组摊平成行**，
不涉及任何"下标换算"——这正是现在 `message_start/count` 方案最难迁移的地方。

---

## 10. 与现状的差异清单

| 现状 | v2 |
|---|---|
| turn 存 `message_start/count`（区间）或内嵌 `messages`（兜底），两模式并存 | turn 只内嵌 `messages[]`，单一模式 |
| 消息体同时存在于 `history.jsonl` 和 `session.json`（重载后冗余 92%） | 消息体只在 session.json，一份 |
| 线性顺序靠 `session->messages` 下标 | 靠 `message.seq` |
| `fold_cursor` 是下标 | `folded_through_seq` 是 seq 水位 |
| `fork_point_msg_count` 是消息条数 | `fork_point_seq` 是 seq |
| `active_turn` 是旁路字段（index=-1） | `turns[].is_active` |
| `group.member_task_ids[]` + `task.group_id` | 均保留（有序成员列表 + 反查） |
| todo 塞在 task JSON（`todolist`）+ `todo_sync_gap` | `tasks[].todos[]`（仍任务级）+ 节流计数 |
| `tasks[].session_id` 冗余 | 去掉（一文件一 session） |
| `edc_session_reload`/`session->messages` 副本 | 移除 |
| 无 task 的消息没有归属（s1/s2/s4 只在 history.jsonl） | 归入 system task |

---

## 11. v1 → v2 迁移

**触发**：`session.json` 的 `version < 2`，或同目录存在 `history.jsonl`。

**步骤（幂等）**：
1. 按现有逻辑加载 v1：`session->messages`（= `[system_prompt] + history.jsonl`）、groups、tasks、turns。
2. **分配 seq**：按「task 的 `completion_seq` 升序 → task 内 turn 顺序 → turn 内消息顺序」遍历，
   依次赋 `seq = 1, 2, 3, …`。这与现有 `deser_rebuild_group_history` 的线性顺序一致。
   - v1 的 RANGE turn：从 `session->messages` 按 `message_start/count` 取出对应消息。
   - v1 的 EMBED turn：直接用内嵌消息（其内容与 history.jsonl 冗余，取其一即可）。
3. **无归属消息 → system task**：v1 中不属于任何 turn 的 `session->messages`（s1/s2/s4/s5 的全部消息）
   放进 `groups["system"].tasks["system"].turns[0].messages`，保序赋 seq。
4. 写 `session.json`（`version: 2`）。
5. 删除 `history.jsonl`、`tasks.idx`（迁移成功且校验通过后）。
6. **不做**：不持久化 system prompt（§6）。

**幂等标记**：`session.json.version == 2` 即跳过；重复运行安全。

**校验**：迁移前后 message 总数一致（v1 的 `session->messages` 中除 system prompt 外，应等于
v2 所有 turn 消息之和）。不一致则告警且**不删旧文件**。

---

## 12. 待办 / 影响面（实现时）

- `edc_state_serializer.c`：重写 serialize/deserialize → 单一内联模式 + seq。
- `edc_session.{c,h}`：移除 `session->messages`、`save_history`、`get_messages_slice` 的下标语义；
  改按 turn/seq 遍历。`edc_session_reload` 删除或改写。
- `edc_history.{c,h}`：删除（或仅保留一次性迁移读取）。
- `edc_task_group.c`：`group->history` 改为**按 seq 计算的视图**，不再持有 `history_task_ids` 平行数组。
- `edc_scheduler.c`：commit 路径改为"把消息挂到 turn 并分配 seq"，去掉 `edc_session_add_turn` 的下标记录。
- `edc_memory_actor.c`：压缩输入改为按 group 的 seq 前缀（不再读 history.jsonl）。
- `edc_session_manager.c`：`task_index` / `tasks.idx` 逻辑删除；跨会话查 task 直接遍历（将来迁库用索引）。
- `src/tui/edc_c_bridge.c` / `edc_tui_window.cpp`：chat transcript 改为从 message 渲染（§8 开放项 2）。
