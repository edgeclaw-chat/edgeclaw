# chat_bubbles：把 TUI 视图状态迁进 edgeclaw.db（B1 + 分页加载 A）

状态：**第一阶段已实现并验证（DB 层 + v9 迁移 + 一次性导入器），跨线程通道与 TUI 改造留待下一轮。**

第一阶段（本提交）：
- `chat_bubbles` 表、`EDC_DB_VERSION` 8 → 9、`step_v8_to_v9`、`EDC_EXPECTED_TABLES`；
- `edc_chat_bubble_append / page / rows_free / count / import_file`；
- `tests/test_chat_bubbles.c`（33/33，含 RED 变异验证）、`test_state_migrate` 更新到 v9（64/64）；
- `bazel test //tests:all` 155/155。

**行为尚未改变**：`chat.jsonl` 仍是 TUI 的权威、仍由 UI 线程写；新表只是先建好、
新 API 只是先备好，没有任何调用方。因此这次提交对用户是完全无感的：老库下次
启动会安全地补建表（`v8 → v9` 纯 DDL，零数据重写），数据不动。

下一轮：新增 `BC_ACTOR_MSG_CHAT_BUBBLE_PAGE` / `EDC_CHANNEL_MESSAGE_CHAT_BUBBLE_PAGE_TYPE`
/ `IChannelSink::OnChatBubblePage` 跨线程通道，把 6 处 `AppendChatTranscript*` 从
UI 线程移到 scheduler actor，改 `ChatBubbleRef` 的 `offset/length` 为 `seq/line`，
`ChatModel` 分页加载 + 滚到顶 `LoadOlderPage`，启动时调 `import_file`，补 TUI 测试。

## 已确认的架构决策（用户拍板）

- **所有消息进同一个 `edgeclaw.db`**，删掉 `chat.jsonl` 作为权威。
- **TUI 线程只负责展示**；所有 DB 读写都在 **scheduler actor 线程**执行，
  遵守 `edc_state_store.h` 的不变量「only the scheduler actor thread touches
  the store」。
- 因为新增表属于 DDL 变更，按头文件纪律 **bump `EDC_DB_VERSION` 8 → 9**，
  走 `edc_state_migrate.c` 的正规迁移（备份 → 校验 → 单事务）。
- **分页加载 A**：启动只取最后一页（200 行），滚到顶按游标取上一页；
  已加载页的**描述符保留在内存**（~48 B/条），渲染树仍由 `kMaxLive=200`
  驱逐 + `OnFrameBegin` 清理，保持 O(viewport) 常驻。
- LLM 上下文组装**零改动**：它读 `messages` 表，`chat_bubbles` 独立。

## 跨线程通道（复用现有范式，不新造机制）

请求（TUI → actor），仿 `edc_app_request_task_snapshot`：

```
IAppFacade::RequestChatBubblePage(session_id, before_seq, limit)
  → edc_app_request_chat_bubble_page()
  → edc_actor_msg_new(BC_ACTOR_MSG_CHAT_BUBBLE_PAGE, channel, ..., sds 请求)
  → scheduler actor handler 分支
  → edc_chat_bubble_page(...)：在 actor 线程读 DB
```

追加（事件产生时就在 actor 线程）：`PresentChat` 现在在 UI 线程写文件；
改为 scheduler 侧产生 `ChatMessageEvent` 时顺带 `edc_chat_bubble_append()`，
回传给 TUI 的 event 只用于展示。**UI 线程不再写任何存储。**

回传（actor → TUI），仿 `EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE`：

```
新增 EDC_CHANNEL_MESSAGE_CHAT_BUBBLE_PAGE_TYPE
  → IChannelSink::OnChatBubblePage(const std::string &payload_json)
  → ChannelDispatcher::Post 到 UI 线程
  → ChatModel::ApplyPage(payload)
```

## 表结构（v9 新增，不改任何现有表）

```sql
CREATE TABLE IF NOT EXISTS chat_bubbles (
  session_id  TEXT NOT NULL,             -- 前端 session_id 是字符串，不转 int
  seq         INTEGER NOT NULL,          -- 每会话单调递增，替代文件 offset
  line        TEXT NOT NULL,             -- EncodeTranscriptLine 的原始行（含转义）
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX IF NOT EXISTS chat_bubbles_by_session
  ON chat_bubbles(session_id, seq DESC);
```

**存编码后的原始行**，不拆列：导入是逐行字节拷贝，`DecodeTranscriptLine` /
`SplitUnescaped` / `EncodeTranscriptLine` 全部复用，零转义漂移；`message_type`
就是 `line` 的第一个 tab 字段（`user|tool|thought|reply|sys`），LLM 侧的
「按类型过滤」由**物理隔离**实现 —— 它根本不读这张表。

## DB 层 API（仅 actor 线程调用，加进 edc_state_store.h）

```c
bool edc_chat_bubble_append(const char *session_id, const char *line, int64_t *out_seq);
/* before_seq = INT64_MAX 取最后一页；rows 按 seq 升序 */
bool edc_chat_bubble_page(const char *session_id, int64_t before_seq, int limit,
                          struct edc_chat_bubble_row **out, int *n);
void edc_chat_bubble_rows_free(struct edc_chat_bubble_row *rows, int n);
int64_t edc_chat_bubble_count(const char *session_id);
```

`seq` 用 `SELECT coalesce(max(seq),0)+1 WHERE session_id=?`；state_store 已有
`claim_exclusive` 保证单进程 + 单写者，足够。

## v9 迁移

- `EDC_DB_VERSION` 8 → 9（`edc_state_store.h:102`）
- `EDC_EXPECTED_TABLES` 加 `"chat_bubbles"`
- `EDC_SCHEMA_SQL` 追加两段 DDL（新库直接建）
- `edc_state_migrate.c` 新增 `step_v8_to_v9`：只执行那两段 DDL，
  注册 `{8, step_v8_to_v9}`
- 按 m72 / `docs/release-checklist.md "## 3. Migrations"` 验证：
  拿**真实旧库副本**跑新二进制，确认 (a) 写了备份且日志点名、(b) 备份
  能打开且仍是 v8、(c) 会话可读、(d) 行数不变；`SQLITE_TMPDIR` 指同一文件系统。

## 一次性导入 chat.jsonl（actor 线程）

触发：`edc_state_store_open()` 成功后、scheduler 启动时。对每个
`<workspace>/<session_id>/chat.jsonl`：

1. 逐行读原始字节（含结尾 `\n` 则去掉），`DecodeTranscriptLine` 校验可解码；
2. 事务内 `INSERT OR IGNORE INTO chat_bubbles(session_id, seq, line)`，
   `seq` = 行号 1..N；
3. COMMIT 成功后改名 `chat.jsonl.imported`（**不删**，留回滚）；
4. 任何失败：ROLLBACK、保留原文件、记日志、下次再试。
5. 幂等：`INSERT OR IGNORE` + 固定 seq，中断重跑不重复。

## TUI 层

- `ChatBubbleRef`：`offset/length/task_id_off/...` → `int64_t seq` +
  `std::string line`（或按需解码）；`source` 只留 `kMemory`。
- `BuildChatBubbleRefs(path)` → 由 actor 回传的 page payload 构建描述符；
  `DecodeChatBubbleAt` 改成 `DecodeTranscriptLine(ref.line)`，不再 `seekg`。
- `ChatModel::LoadTranscript(session_id)`：请求最后一页 → `ApplyPage`。
- `ChatModel::LoadOlderPage()`：`visible_first_ == 0` 时由 `MessageList`
  发信号 → facade 请求上一页 → `ApplyOlderPage` **前插**到
  `refs_/all_/visible_`（复用 `PrependMessages` 的锚定修正），
  `TakeOffsetAdjustment` 保证屏幕行不动。
- `AppendChatTranscript*` 删除；`SessionModel::chat_log_path()` 保留给
  导入器/兼容，但不再写。
- `/task history` 不动（继续读 `messages` 表）。

## 保留的现状

`kMaxLive=200`、`kPrimeWindow=40`、`kFirstFrameKeep=200`、
`OnFrameBegin` 清树、`EnsureItemMeasured` 重建 —— 全部保留。
`EncodeTranscriptLine`/`DecodeTranscriptLine`/`test_chat_transcript` 继续覆盖转义。
`messages` 表 / LLM 请求组装 / fold / memo —— 一行不改。

## 测试（先写能红的）

DB 层（新 `tests/test_chat_bubbles.c`）：
- append → page 往返，含 `\n`/`\t`/`\\` 转义行不丢、顺序正确；
- 分页边界：游标不重不漏、limit 生效、空页；
- 导入幂等：同一文件两次行数不变；
- v9 迁移：真实 v8 库副本 → 备份存在且仍 v8、新表存在、旧表行数不变。

TUI 层（扩 `tests/test_chat_window.cpp` / 新 `test_chat_bubble_page.cpp`）：
- 只加载最后一页时首帧显示尾部（防空洞）；
- 滚到顶触发 `LoadOlderPage` 后能看到更早 bubble；
- 已加载页来回滚动不重复请求（计数探针）；
- 不变量：UI 线程不触碰 DB（用 FakeFacade 断言请求走 facade）。

## 验收

- `bazel test //tests:all` 全绿（数量会增加）；
- 真实 `.diag_dhat` 8.2 MB / 7943 行：导入后重启，首帧在尾部、
  滚到顶能看到第 0 条、RSS 峰值仍 ~90 MB 量级；
- 提交 + 推送，不开 PR。

## 风险

- v9 迁移会拒绝用老二进制打开新库（安全但硬），文档化降级路径。
- 导入是唯一接触旧文件处；失败保留原文件、成功才改名且不删。
- actor 线程做 7943 行导入会短暂占用该线程：分块提交并让 TUI 先显示
  最后一页，或放在启动早期（首个 snapshot 之前）。
