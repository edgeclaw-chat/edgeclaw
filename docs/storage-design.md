# EdgeClaw 数据存储方案（借鉴 opencode）

> 状态：设计稿 / 待评审
> 目标读者：edgeclaw C 代码维护者
> 结论先行：把"会话/任务/依赖/todo/消息/定时任务"这些**结构化、高频读改写**的数据
> 收进一个 SQLite 库（WAL、事务、索引、schema 迁移），把**用户可编辑的提示词 / 记忆 /
> 技能**继续留在文件里，把**大产物**放文件并入库只存引用。**不照抄 opencode 的全量事件溯源**。

---

## 1. 背景与目标

opencode 已从"一堆 JSON 文件"迁移到 **单一 SQLite 主库**（`~/.local/share/opencode/opencode.db`，
WAL）作为系统真相源，辅以 git 快照目录和少量遗留 JSON。本机实测（2026-09）：

| 表 | 行数 | 说明 |
|---|---|---|
| `event` | 119,672 | 事件流（事件溯源）|
| `event_sequence` | 115 | 每个聚合的 seq |
| `message` | 12,922 | 消息投影（`data` 为 JSON）|
| `part` | 55,552 | 消息分段投影 |
| `session` | 269 | 会话投影（token/cost/model/revert 独立列）|
| `todo` | 243 | 待办（`session_id,position` 主键）|

edgeclaw 当前的"一物一文件 + 整块 JSON 重写 + 依赖 sidecar 做索引"模式已出现明显天花板。
本方案给出**分层存储**目标与**可灰度的迁移路径**。

**设计原则**

1. **真相源唯一**：会话运行态只用一处可事务提交的存储，不再让 `history.jsonl` / `session.json` /
   `tasks.idx` 三份文件互相靠约定保持一致。
2. **可查询**：按 `task_id`、`session_id`、`state`、依赖边直接索引命中，替代"扫盘 / 建 sidecar / 懒加载"。
3. **用户资产保持文件**：提示词、技能、记忆是用户用编辑器/工具直接改的 markdown，进 DB 只会伤可编辑性与可 diff 性。
4. **大对象走文件、库内只存引用**：工具输出、文件快照不进 blob 列。
5. **渐进式**：可先引入抽象层 + 双写，跑通再摘掉旧路径，不做一次性大爆炸重写。

---

## 2. 现状盘点（edgeclaw 数据地图）

### 2.1 根目录 `<edc_home>/.edgeclaw/`

| 路径 | 内容 | 格式 | 写入频率 |
|---|---|---|---|
| `config.json` | LLM(api_url/model/api_key 加密)、sandbox 路径、device、阈值 | JSON | 低（改配置时）|
| `.key_hash` | 主密钥校验 | 文本 | 极低 |
| `.tui_history` | TUI 输入历史 | 文本行 | 低 |
| `log/` | 运行日志 | 文本 | 高（append）|
| `sandbox/` | sandbox 根 | — | — |

### 2.2 workspace 根 `<edc_home>/.edgeclaw/workspace/`

| 路径 | 内容 | 格式 | 写入频率 |
|---|---|---|---|
| `sessions.json` | 会话索引：id/name/created_at/last_active/message_count + active_session_id | JSON | 每次会话切换/保存 |
| `.task_seq` | 全局 task id 高水位 | 文本 | 每次发号 |
| `jobs.json` | crontab 定时任务 | JSON 数组 | 增删任务时 |
| `SOUL.md` `AGENTS.md` `TOOLS.md` `USER.md` `HEARTBEAT.md` | 提示词/配置 | Markdown | 低（用户编辑）|
| `memory/MEMORY.md` | 长期记忆（注入 prompt）| Markdown | 中 |
| `memory/HISTORY.md` | 时间线记忆（append-only，不注入）| Markdown | 高（压缩时 append）|
| `skills/<name>/SKILL.md` | 技能 | Markdown+YAML | 低 |
| `ssl/cacert.pem` | CA 证书 | PEM | 极低 |

### 2.3 每会话 `<workspace>/<session_id>/`

| 路径 | 内容 | 格式 | 写入频率 |
|---|---|---|---|
| `session.json` | 状态：`version` + `session`(meta/active_task_id/foreground_group_id/next_completion_seq/message_count) + `groups[]` + `tasks[]`（含 deps[]、turns[消息下标区间]、tokens、todolist、tool_calls）| 单块紧凑 JSON | **每次保存整块重写** |
| `history.jsonl` | LLM 上下文消息（role/content/reasoning_content/timestamp，带 task_id/group_id）| JSONL append | 高 |
| `chat.jsonl` | 渲染用对话记录（类型/计数字段/header/body）| JSONL append | 高 |
| `tasks.idx` | 仅 task id 一行一个，供跨会话查属主 | 文本 | 每次保存状态 |

**关键耦合**：`session.json` 里的 turn **不存消息体，只存 `message_start/message_count` 下标**，指向
`history.jsonl` 的行号。状态与历史分属两个文件、两次独立写，靠约定对齐。

---

## 3. 现状问题

1. **整块重写**：每次保存把全部 tasks/groups/turns 序列化成一个大 JSON 再 `rename`。任务越多，保存越慢、写放大越严重，且中途崩溃只能靠 `.tmp` 兜底，无事务。
2. **跨文件不一致**：`history.jsonl` append 与 `session.json` 重写是两次独立操作；崩在中间 → 下标错位 / `message_count` 失真（`sessions.json` 里的计数已与真实行数对不上）。
3. **索引靠 sidecar 打补丁**：为了不解析巨大的 `session.json`，额外维护 `tasks.idx`、迁移回退时还得"解析一次再补写 sidecar"。这正是"缺一个真正索引"的症状。
4. **依赖图是一等公民却没有一等存储**：`deps[]` 内嵌在 task JSON 里，跨会话/跨重启解析要靠内存里的 fixpoint reconciliation（S1/S2/S3 那套 `edc_pending_dep`）。
5. **同一份对话存三处**：LLM 视角（history）、渲染视角（chat）、任务映射（session.json 下标），语义重叠、难以关联查询。
6. **无 schema 迁移**：`version:1` 是自描述，升级靠手写 `edc_sm_migrate_legacy` 之类的一次性代码。
7. **todo / jobs 无事务**：`task->todolist` JSON + `todo_sync_gap` 补偿、`jobs.json` 全量重写。

---

## 4. opencode 的做法与借鉴取舍

### 4.1 它的模型

- **单一 SQLite 主库**，Drizzle ORM，`__drizzle_migrations` 版本化迁移。
- **事件溯源**：`event(id, aggregate_id, seq, type, data)` + `event_sequence(aggregate_id, seq)`。
  聚合 = session；每个事件携带该实体的**完整快照**（`session.updated.1` 带整个 session info）。
  实测 11.9 万条 event 里绝大多数是 `message.part.updated.1`(7.8万) / `message.updated.1`(3.3万)——
  即**流式增量也逐条落事件**。
- **投影表**：`session`/`message`/`part`/`session_message`/`todo` 是从事件物化的读模型，JSON 负载放 `data` 列。
- **可查询字段独立成列**：session 的 `tokens_*`/`cost`/`model`/`agent`/`revert`/`permission` 都是列。
- **git 快照**：`snapshot/<project_hash>/<hash>/` 是真正的 git 仓库，支撑文件级 revert/diff。
- **凭证**：`auth.json`/`account.json`（明文）；DB 里 `account`/`credential` 表预留。
- **遗留 `storage/`**：迁移后基本清空，仅剩 `migration` 标记与 `session_diff/*.json`。

### 4.2 借鉴 / 不借鉴

| 借鉴 | 理由 |
|---|---|
| 单一事务型存储作真相源 | 直接消灭多文件一致性、整块重写、sidecar 索引 |
| 可查询字段成列 + 多态负载放 JSON 列 | 兼顾查询性能与 schema 灵活 |
| append-only 消息日志 | 便宜、保序、可检索 |
| schema 版本 + 迁移表 | 干掉一次性迁移代码 |
| 大对象走文件、库内只存引用 | 控制库体积 |
| （可选）git 快照做文件 revert | 后续能力 |

| 不借鉴 | 理由 |
|---|---|
| **全量事件溯源 + 逐 delta 落事件** | 本机 589MB、事件占 11.9 万条；edgeclaw 只需"可提交的当前态 + append 消息"，不需要 replay/undo 事件流。盲抄 = 体积与复杂度双爆。 |
| 凭证明文入库 | edgeclaw 已用主密钥 AES-GCM 加密，保持更好。 |
| 一个 DB 塞所有 project | edgeclaw 是单 workspace 多 session，无需 project 分层。 |

---

## 5. 目标架构（分层）

```
<edc_home>/.edgeclaw/
├── config.json                  # L0 配置 + 加密密钥（文件，保留，用户可编辑）
├── .key_hash / log/ / sandbox/  # 保留
└── workspace/
    ├── edgeclaw.db (+ -wal/-shm)# L1 系统真相源：会话/任务/组/依赖/turn/todo/job
    ├── SOUL.md AGENTS.md ...    # L0 提示词（文件，保留）
    ├── memory/{MEMORY,HISTORY}.md  # L4 记忆（文件，保留；可选 FTS 索引）
    ├── skills/                  # L0 技能（文件，保留）
    ├── ssl/cacert.pem
    ├── tool-output/<hash>       # L3 大产物（文件，库内存引用）
    ├── snapshot/<workdir_hash>/ # L3（可选，git 仓库，文件 revert/diff）
    └── export/<session_id>/{history,chat}.jsonl  # 兼容导出（可选，非真相源）
```

- **L0 文件**：提示词/技能/配置/密钥——保持文件，用户可编辑、可 diff、可注入 prompt。
- **L1 SQLite**：一切结构化运行态。
- **L2 消息**：作为 L1 中的 append-only 表（见下）。
- **L3 文件**：大产物与快照，库内只存 `hash`/`path`。
- **L4 记忆**：Markdown 保留，可选 `memory_fts` 做检索。

---

## 6. SQLite schema 设计（DDL 草案）

```sql
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
PRAGMA synchronous=NORMAL;

-- schema 版本 / 迁移
CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE migration(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);

-- ── 会话 ───────────────────────────────────────────────
CREATE TABLE session(
  id                    TEXT PRIMARY KEY,
  name                  TEXT NOT NULL,
  created_at            INTEGER NOT NULL,
  updated_at            INTEGER NOT NULL,
  workdir               TEXT,                 -- 绝对 realpath，可空=回退启动目录
  active_task_id        TEXT,                 -- 弱引用(避免循环 FK)
  foreground_group_id   TEXT,
  next_completion_seq   INTEGER NOT NULL DEFAULT 1,
  message_count         INTEGER NOT NULL DEFAULT 0,
  tokens_input          INTEGER NOT NULL DEFAULT 0,
  tokens_output         INTEGER NOT NULL DEFAULT 0,
  tokens_reasoning      INTEGER NOT NULL DEFAULT 0,
  tokens_cache_read     INTEGER NOT NULL DEFAULT 0,
  tokens_cache_write    INTEGER NOT NULL DEFAULT 0,
  metadata              TEXT                  -- JSON 兜底
);
CREATE INDEX idx_session_updated ON session(updated_at);

-- ── 组 ────────────────────────────────────────────────
CREATE TABLE task_group(
  group_id        TEXT PRIMARY KEY,
  session_id      TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  leader_task_id  TEXT,
  is_orphaned     INTEGER NOT NULL DEFAULT 0,
  fold_cursor     INTEGER NOT NULL DEFAULT 0,
  memory          TEXT
);
CREATE INDEX idx_group_session ON task_group(session_id);

CREATE TABLE task_group_member(
  group_id  TEXT NOT NULL REFERENCES task_group(group_id) ON DELETE CASCADE,
  task_id   TEXT NOT NULL,
  position  INTEGER NOT NULL,
  PRIMARY KEY(group_id, position)
);
CREATE INDEX idx_member_task ON task_group_member(task_id);

-- ── 任务 ───────────────────────────────────────────────
CREATE TABLE task(
  id                    TEXT PRIMARY KEY,       -- 全局唯一(替代 .task_seq，见 §8)
  session_id            TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  group_id              TEXT NOT NULL REFERENCES task_group(group_id) ON DELETE CASCADE,
  description           TEXT,
  state                 TEXT NOT NULL,          -- PENDING/RUNNING/COMPLETED/... 
  is_group_leader       INTEGER NOT NULL DEFAULT 0,
  auto_reap             INTEGER NOT NULL DEFAULT 0,
  channel               TEXT,                   -- JSON
  prompt_name           TEXT,
  prompt                TEXT,
  forked_from_task_id   TEXT REFERENCES task(id),
  fork_point_msg_count  INTEGER NOT NULL DEFAULT 0,
  output                TEXT,
  completion_seq        INTEGER,
  turns_folded          INTEGER NOT NULL DEFAULT 0,
  tokens_prompt         INTEGER NOT NULL DEFAULT 0,
  tokens_completion     INTEGER NOT NULL DEFAULT 0,
  tokens_total          INTEGER NOT NULL DEFAULT 0,
  tokens_cached         INTEGER NOT NULL DEFAULT 0,
  redirect_stdout       TEXT,
  redirect_stderr       TEXT,
  redirect_append       INTEGER NOT NULL DEFAULT 0,
  todo_sync_gap         INTEGER NOT NULL DEFAULT 0,  -- 迁移期保留，收尾后删
  created_at            INTEGER NOT NULL,
  updated_at            INTEGER NOT NULL,
  metadata              TEXT                    -- JSON 兜底
);
CREATE INDEX idx_task_session ON task(session_id);
CREATE INDEX idx_task_state   ON task(session_id, state);
CREATE INDEX idx_task_group   ON task(group_id);

-- 依赖边：一等公民（替代内嵌 deps[] 与跨会话懒加载 reconciliation）
CREATE TABLE task_dep(
  task_id         TEXT NOT NULL REFERENCES task(id) ON DELETE CASCADE,
  source_task_id  TEXT NOT NULL,               -- 可能属于别的 session(索引可解析)
  type            TEXT NOT NULL,               -- seq / join / ...
  completed       INTEGER NOT NULL DEFAULT 0,
  cancelled       INTEGER NOT NULL DEFAULT 0,
  injected        INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(task_id, source_task_id, type)
);
CREATE INDEX idx_dep_source      ON task_dep(source_task_id);
CREATE INDEX idx_dep_task        ON task_dep(task_id);

-- turn：task → 消息区间
CREATE TABLE turn(
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id        TEXT NOT NULL REFERENCES task(id) ON DELETE CASCADE,
  turn_index     INTEGER NOT NULL,
  message_start  INTEGER NOT NULL,
  message_count  INTEGER NOT NULL,
  is_active      INTEGER NOT NULL DEFAULT 0,
  UNIQUE(task_id, turn_index)
);

-- todo：真表（替代 task.todolist JSON + todo_sync_gap 补偿）
CREATE TABLE todo(
  session_id   TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  task_id      TEXT,                            -- 归属任务(可空=会话级)
  position     INTEGER NOT NULL,
  content      TEXT NOT NULL,
  status       TEXT NOT NULL,                  -- pending/in_progress/completed/cancelled
  priority     TEXT NOT NULL DEFAULT 'medium',
  created_at   INTEGER NOT NULL,
  updated_at   INTEGER NOT NULL,
  PRIMARY KEY(session_id, position)
);
CREATE INDEX idx_todo_task ON todo(task_id);

-- ── 消息 / 对话（append-only，替代 history.jsonl + chat.jsonl）────
-- LLM 上下文消息：seq 全局单调，按 (session_id, seq) 有序
CREATE TABLE message(
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id     TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  task_id        TEXT,
  group_id       TEXT,
  seq            INTEGER NOT NULL,
  role           TEXT NOT NULL,               -- user/assistant/tool/system
  content        TEXT,
  reasoning_content TEXT,
  name           TEXT,
  tool_calls     TEXT,                        -- JSON（多态负载）
  render         TEXT NOT NULL DEFAULT 'plain', -- plain/markdown
  created_at     INTEGER NOT NULL,
  UNIQUE(session_id, seq)
);
CREATE INDEX idx_msg_task ON message(task_id);
CREATE INDEX idx_msg_seq  ON message(session_id, seq);

-- 渲染用 transcript（TUI 直接读；也可由 message 派生，见 §7 说明）
CREATE TABLE chat_event(
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id  TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
  seq         INTEGER NOT NULL,
  type        TEXT NOT NULL,                  -- user/reply/thought/tool/...
  a           INTEGER, b INTEGER, c INTEGER,  -- 原 chat.jsonl 的计数字段
  header      TEXT,
  body        TEXT,
  created_at  INTEGER NOT NULL,
  UNIQUE(session_id, seq)
);

-- ── 定时任务（替代 jobs.json）────────────────────────────
CREATE TABLE job(
  id            TEXT PRIMARY KEY,
  name          TEXT,
  message       TEXT NOT NULL,
  cron_expr     TEXT,                         -- 二选一
  every_seconds INTEGER,
  at            INTEGER,                      -- 一次性(epoch ms)
  channel       TEXT,                         -- JSON
  next_run_at   INTEGER,
  enabled       INTEGER NOT NULL DEFAULT 1,
  created_at    INTEGER NOT NULL
);
CREATE INDEX idx_job_next ON job(enabled, next_run_at);

-- ── 可选：记忆检索 ────────────────────────────────────
CREATE VIRTUAL TABLE memory_fts USING fts5(session_id, kind, body, ts UNINDEXED);
```

**替代的遗留文件**：`sessions.json`、`<sid>/session.json`、`<sid>/tasks.idx`、`jobs.json`
（以及 `history.jsonl` / `chat.jsonl` 在 Phase B 后降级为可选导出）。

---

## 7. 数据映射表（现状 → 目标）

| 现状 | 目标 | 备注 |
|---|---|---|
| `sessions.json` 每行 | `session` 表一行 | 差异写：切会话时 UPSERT，不再整表重写 |
| `.task_seq` | `session`/`task` 上的 `seq` 分配器（见 §8） | 用 `sqlite_sequence` 或 `setting` 表高水位 + 事务发号 |
| `<sid>/session.json` session 段 | `session` 表列 | `active_task_id`/`foreground_group_id` 直接成列 |
| `groups[]` | `task_group` + `task_group_member` | |
| `tasks[]` 元数据/计数 | `task` 表列 | token 类独立成列，其余 JSON 入 `metadata` |
| `tasks[].deps[]` | `task_dep` 表 | **跨会话依赖查表即可** |
| `tasks[].turns[]`（消息下标）| `turn` 表 | message_start/count 保留语义 |
| `tasks[].todolist` + `todo_sync_gap` | `todo` 表 | widget 直接读，补偿字段收尾后删 |
| `history.jsonl` 每行 | `message` 表一行 | 保序 append；`task_id/group_id` 成列 |
| `chat.jsonl` 每行 | `chat_event` 表一行 | TUI 改读表；如需可留导出 |
| `jobs.json` | `job` 表 | 增删改 = 行操作，事务安全 |
| `MEMORY.md` / `HISTORY.md` | 保留文件（可选 `memory_fts` 索引）| 可编辑性优先 |
| `SOUL/AGENTS/TOOLS/USER/HEARTBEAT/skills` | 保留文件 | 不迁 |
| `config.json` | 保留文件（密钥继续加密）| 不迁 |
| 工具大输出 | `tool-output/<hash>` + `message.tool_calls` 引用 | 超 `tool_max_output_size` 落文件 |
| 文件 revert/diff | （可选）`snapshot/<hash>/` git 仓库 | Phase D |

**关于 chat_event 与 message 的关系**：两者语义不同——`message` 是 LLM 上下文（含 reasoning、
按 group 折叠），`chat_event` 是渲染产物（含 header、工具调用展示）。第一阶段**都建表**避免重设计；
若后续确认 `chat_event` 可由 `message` 纯派生，可退化为视图。

---

## 8. ID 分配（替代 `.task_seq`）

现状用进程全局单调计数 + `.task_seq` 文件保证 task id 跨会话/跨重启唯一。SQLite 下有两种做法：

- **推荐**：`CREATE TABLE id_seq(kind TEXT PRIMARY KEY, next INTEGER NOT NULL)`，
  `BEGIN IMMEDIATE; UPDATE id_seq SET next=next+1 WHERE kind='task'; SELECT next ...; COMMIT;`
  ——事务发号天然单调、跨进程安全。
- 或直接 `id INTEGER PRIMARY KEY AUTOINCREMENT` 作内部主键，对外 id 用 `task-<n>` 呈现。

`.task_seq` 在迁移时读入高水位灌进 `id_seq`，之后删除该文件。

---

## 9. 迁移与灰度

**原则**：一次性、幂等、可回退；旧文件在确认稳定前**只读保留**。

1. **打开/建库时检测**：`workspace/edgeclaw.db` 不存在且存在 `sessions.json` → 进入迁移。
2. **导入顺序**：`session` ← `sessions.json`；逐会话解析 `session.json` → `task_group`/`task_group_member`/
   `task`/`task_dep`/`turn`/`todo`；逐行导入 `history.jsonl`→`message`、`chat.jsonl`→`chat_event`；
   `jobs.json`→`job`；`.task_seq`→`id_seq`。整个导入包在**单个事务**里。
3. **幂等**：用 `schema_meta('migrated_from','files@v1')` 打标；重复运行直接跳过。
4. **回退**：迁移期间**不改动**旧文件；DB 出错可删库重来。
5. **保留期**：至少一个版本内，写路径 **DB 为准 + 旧文件只读**（可选短暂双写用于对拍），确认后删旧文件。
6. **校验**：迁移后比对（session 数、task 数、每会话 message 行数 vs `history.jsonl` 行数），不一致则告警不切换。

---

## 10. 落地步骤（分阶段，每阶段可独立交付）

**Phase A — 存储抽象 + 结构化态入库**
- 新增 `edc_store.{h,c}`（接口）与 `edc_store_sqlite.c`（后端），vendor `thirdparty/sqlite3/`（amalgamation，沿用现有 vendored 风格，Bazel 加 `cc_library`）。
- `edc_store_sqlite.c` 实现 `schema_meta`/`migration` 自动建表与迁移。
- 迁移 `session`/`task_group`/`task`/`task_dep`/`turn`/`todo`/`job`；`sessions.json`/`session.json`/`tasks.idx`/`jobs.json` 停止写（保留只读）。
- `edc_state_serializer` 改为 `edc_store` 的上层封装（保留函数签名，内部换实现，降低改动面）。
- 测试：`test_state_serializer` / `test_session_state_file` / `test_task_index` / `test_cross_dep_restart` 改造为跑 SQLite 后端。

**Phase B — 消息入库**
- `history.jsonl`→`message`、`chat.jsonl`→`chat_event`；TUI 桥接 (`edc_c_bridge.c` 的 chat-log 路径) 改走 DB。
- 保留导出命令（`/session export <sid>` 生成两份 jsonl），旧文件由导出替代。

**Phase C — 清理 sidecar 与懒加载补丁**
- 删除 `.task_seq`/`tasks.idx` 及 `edc_session_manager` 里的 `task_id → session_id` 缓存建造逻辑：
  `SELECT session_id FROM task WHERE id=?` 直接命中。
- 跨会话依赖：`edc_agent_reconcile_pending_deps` 的 fixpoint 循环简化为一次 `task_dep` 查询 + 状态回填。

**Phase D（可选）— 快照与记忆检索**
- `snapshot/` git 仓库做文件 revert/diff；`memory_fts` 做 HISTORY 检索。

---

## 11. 取舍与风险

| 项 | 取舍 |
|---|---|
| 新增 SQLite 依赖 | 收益（事务/索引/迁移）远大于成本；amalgamation 单文件 vendor，无网络、跨平台（含 Android/arm）可控。需在 Bazel 建 `cc_library` 并进各 target 的 deps。 |
| 不抄事件溯源 | 保留"结构化表 + append 消息"，需要审计/undo 时再加轻量 `audit_event` 表，不预置 11 万条级事件流。 |
| Markdown 提示词/记忆不迁库 | 保可编辑、可 diff、可被用户直接 grep；DB 只做可选 FTS。 |
| 双写/迁移窗口 | 旧文件保留只读，允许对拍；迁移失败可回退删库。 |
| 并发 | WAL + 单写进程；发号/状态更新用 `BEGIN IMMEDIATE` 事务；跨进程（如外部工具）通过 SQLite 锁自然串行。 |
| 大库体积 | 消息行多但可控；大产物走 `tool-output/` 文件；定期 `PRAGMA wal_checkpoint` / `VACUUM`。 |
| 测试面 | 现有大量文件路径断言需改；通过"先接口后实现 + 迁移对拍"降低一次性风险。 |

---

## 12. 对现有代码的主要影响点

- 新增：`src/agent/edc_store.{h,c}`、`edc_store_sqlite.c`、`thirdparty/sqlite3/`、`tests/test_store_*.c`。
- 改写：`edc_state_serializer.c`（换后端）、`edc_session.c`（save/load 走 store）、
  `edc_session_manager.c`（索引/迁移/sidecar 删除）、`edc_tools_todo.c`、`edc_tools_crontab.c`、
  `edc_history.c`、`edc_memory_actor.c`（消息读写）。
- 桥接：`src/tui/edc_c_bridge.c`（chat transcript 路径 → store 查询）。
- 迁移代码：`edc_sm_migrate_legacy` 与 `deser_deps` 的 pending-dep reconciliation 可大幅简化/删除。

---

## 13. Undo / Redo / Replay（借鉴 opencode）

### 13.1 opencode 的真实语义（实测证据）

本机 opencode 1.18.30 的 bundle 与 `opencode.db` 里可查证：

- **replay 是"视图回放"，不是"执行重放"**。CLI 帮助原文：`--no-replay` =
  *"disable mini session history replay on resume and after resize"*，`--replay-limit N` =
  *"cap visible mini replay to the newest N messages"*。即：恢复会话 / 终端 resize 后，把已持久化的
  session 历史重新渲染进 TUI，并可用 N 限制回放条数；默认开启。
- **undo 是"可逆边界 + 文件快照"**。`SessionRevert` 服务暴露：
  - `POST /session/{id}/revert`，注解 *"Revert a specific message in a session, undoing its effects
    and restoring the previous state."*
  - `POST /session/{id}/unrevert`，注解 *"Restore all previously reverted messages in a session."* —— 即 **redo**。
  - 分阶段 API：`revert/stage {messageID, files:boolean}`（*"Stage or move a reversible session
    boundary and optionally apply its file changes"*）、`revert/clear`、`revert/commit`。
  - `session.revert` 列存 `{snapshot, diff}`；`snapshot/` 是每 worktree 一个 git 仓库；
    `session_diff` / `summary_diffs` 存 unified patch。

**拆开看是两件正交的事**：① 会话消息的**边界回退**（便宜，纯 DB）；② 工作区文件的**快照/还原**
（需要快照机制）。redo 只是把边界指针移回——消息并不物理删除。

### 13.2 edgeclaw 已有什么

| opencode 能力 | edgeclaw 现状 |
|---|---|
| replay（视图回放）| **已有**：`TUIWindow::LoadChatTranscript()` 读 `chat.jsonl` 重建气泡，启动时 + `/session switch`（`SyncChatSession`）触发 |
| 从某点分支 | **已有**：`/task fork` + `edc_task_snapshot_turns()`，把父 group 未折叠区间 `[1+fold_cursor, fork_point)` 切片成子 group 的初始 turn |
| 消息/turn 可定位 | **已有**：`turn.msg_range_start/count`、`group.fold_cursor`、`task.turns_folded` |
| 会话回退（rewind）| **无**：消息只增不减，压缩只是向前摘要进 MEMORY |
| 文件快照/还原 | **无**：无任何 snapshot/revert |

### 13.3 可借鉴性与建议

**a) replay —— 已经具备，只需两点小改**
- 借鉴 `--replay-limit`：超大 transcript 只回放最近 N 条，避免开屏卡顿（现在是一次性全读）。
- 配合 §6 存储方案 → 从 `chat_event` 表回放（替代读 `chat.jsonl`），可过滤 / 限额。
- **不要**引入事件溯源版 replay：崩溃恢复 edgeclaw 已由 state 文件覆盖，且与已定方案冲突。

**b) 会话 rewind/redo —— 可借鉴，改动中等，优先做**
采用 opencode 的"**边界指针**"模型，而非物理删消息：

```sql
CREATE TABLE session_revert(               -- 对齐 opencode session.revert
  session_id       TEXT PRIMARY KEY REFERENCES session(id) ON DELETE CASCADE,
  boundary_msg_seq INTEGER NOT NULL,       -- 可见消息上界；> 它的消息视为已回退
  snapshot_id      TEXT,                   -- 可选：该边界的文件快照
  staged           INTEGER NOT NULL DEFAULT 0,
  updated_at       INTEGER NOT NULL
);
-- 消息加可见性列（redo 只改指针/标志，不删行）
ALTER TABLE message ADD COLUMN hidden INTEGER NOT NULL DEFAULT 0;
```

- **undo** = 移动 `boundary_msg_seq` + 回算受影响 task 的 turns 与 group `fold_cursor`
  （`edc_task_snapshot_turns` 的切片逻辑可复用）+ 可选恢复文件。
- **redo** = `unrevert`：移回指针。
- 命令：`/task undo [n]` / `/task redo`；TUI 回放时跳过 `hidden=1`。
- 与现有 `fork` 的关系：**fork 向前分支，undo 向后收边界**，二者共享消息切片代码，但不互相替代。

**c) 文件 undo —— 部分可借鉴，优先级次之，有硬约束**
opencode 用 **git 仓库**做 worktree 快照。edgeclaw 不建议照搬（目标含 Android/嵌入式，不保证有
`git`；workdir 是任意沙箱目录）。建议改为**内容寻址 blob store**：

```sql
CREATE TABLE snapshot(id TEXT PRIMARY KEY, session_id TEXT, task_id TEXT, turn_id INTEGER,
                      label TEXT, created_at INTEGER NOT NULL);
CREATE TABLE file_blob(hash TEXT PRIMARY KEY, size INTEGER NOT NULL, created_at INTEGER);
CREATE TABLE file_change(snapshot_id TEXT, path TEXT, op TEXT,   -- add/mod/del
                         blob_hash TEXT, prev_hash TEXT, PRIMARY KEY(snapshot_id, path));
```

- blob 落 `workspace/blobs/<aa>/<hash>`；`file_change` 即 opencode 的 `session_diff`。
- 触发点：一个 turn 开始前 capture；`edc_tools_fs.c` 的 write/edit 路径逐文件快照。
- `/task diff` 比较两快照输出 patch；`/task undo` 应用还原。

**硬约束（必须说清）**：
1. **`exec_shell` 是最大难点**。任何 shell 命令都能写文件，逐文件 pre-write 快照**只覆盖 fs 工具**。
   要还原 shell 的效果，必须做 **workdir 整树快照**（opencode 正是因此用 git）——代价随目录大小增长。
   取舍：要么接受"文件 undo 只覆盖 fs 工具"，要么对 shell 前后做整树 diff（mtime/size 预筛 + 内容寻址去重）。
2. **范围**：只快照 session workdir（sandbox rw 内），需明确 tracked paths 与 ignore 规则
   （大文件 / 二进制 / `.git`），否则会试图吞掉 `/tmp`、`/home`。
3. **体积**：内容寻址能去重，但仍需上限与 `VACUUM` / GC。

### 13.4 结论

- **replay**：已等价具备；借 `--replay-limit` + 改从 `chat_event` 回放即可。
- **undo/redo（会话）**：**值得借鉴**，是 opencode 最干净的部分，改动中等、纯 store 内可完成。
- **undo（文件）**：**可借鉴但降级**——用内容寻址 blob store 替代 git，且要接受 shell 覆盖不全
  或付出整树快照成本。
- **不建议**：事件溯源 replay、git-per-worktree 直搬。

---

## 14. 一句话总结

**用"一个 SQLite（WAL）+ 可查询列 + JSON 兜底 + append 消息表"作真相源，替换掉
`sessions.json`/`session.json`/`tasks.idx`/`jobs.json`/两份 jsonl，把跨会话依赖与 todo 变成一等表；
提示词/记忆/技能继续留在文件。** opencode 值得借的是"单一事务型存储 + 投影化读模型 + 迁移机制"，
不值得借的是它那套逐 delta 的全量事件溯源。
