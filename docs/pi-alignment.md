# 向 pi 对齐：上下文 token 统计 + 记忆折叠（方案）

> 依据一：pi 源码（`packages/coding-agent/src/core/compaction/compaction.ts` / `agent-session.ts`、`packages/ai/src/utils/overflow.ts`）。
> 依据二：本仓 HEAD `824dc07`（分支 `memory`）的**实际代码**，逐条核实过，未核实的不写。
> 本文只是方案，**不改代码**。术语：本仓的 **折叠**（L1 fold，`src/kernel/context/edc_group_fold.c`）↔ pi 的 **压缩**（compaction）。

---

## 0. TL;DR

| | pi | 我们现在 | 结论 |
|---|---|---|---|
| **上下文规模怎么算** | 最后一条**有效** assistant 的 `usage.totalTokens` + 其后消息按 `chars/4` 估算；无有效 usage → 全量估算 | 最近一次应答的 `usage.total_tokens`（`task->last_total_tokens`），组取成员**最大**；**尾部不计、折叠后不失效** | **P1** 补估算器 + anchor 失效规则 |
| **什么时候折** | `tokens > contextWindow − reserveTokens`（默认 16k） | **只剩 token 轴**：`ctx > 窗口 − reserve`；窗口默认 **1M**，`<= 0` 在启动时被修成默认值（2026-10-10，见 §2.9） | **P2 ✅**（消息条数轴已整条退休） |
| **折到哪里** | 从尾部往前累加到 `keepRecentTokens`(20k) 的**合法切点** | **折最老的一半**（按 task / 消息数） | **P2** 改尾部 token 预算 |
| **切点安全** | 永不切在 `toolResult` 上 | `edc_llm_history_safe_start` 把起点回退到非 tool 消息 | ✅ 已具备 |
| **摘要怎么生成** | 结构化 6 段 + 文件追踪 + **迭代更新**指令 + `maxTokens` 上限 + 失败判定 | "concise markdown" 一句，只有 `## Existing Group Memory` 前缀 | **P2** 采纳格式与迭代规则 + 卫生检查 |
| **窗口爆了怎么办** | 三类溢出识别 → 压缩 → **重试一次** | 三类溢出识别 → **一条带数字的提示**（不自动恢复，裁决 #10） | **P3 ✅** |
| **摘要调用的成本** | 计入会话 usage | 折叠任务在**系统组**，用量记在系统组 | 已知，不改 |

一句话：**P1 让 ctx 从"上一条应答的规模"变成"下一次请求的规模"；P2 让折叠从"条数减半"变成"向窗口要空间"；P3 让超窗**有人告诉你该怎么办**（不自动恢复：裁决 #10）。** 三期的估算是**同一个函数**（同一个 `estimate`，三个消费者：footer / 触发 / 溢出恢复）。

---

# 第一部分：上下文 token 统计

## 1.1 pi 怎么算

`estimateContextTokens`（compaction.ts:217）：

```
ctx = 最后一条**有效** assistant 消息的真实 usage.totalTokens
    + 该消息之后所有消息的 chars/4 估算           ← "尾部"
若无有效 usage（会话开头 / 无有效消息）:
    ctx = 全部消息的 chars/4 估算
```

- **有效性**（`getAssistantUsage`）：跳过 `stopReason` 为 `aborted`/`error` 的消息；跳过 usage 全零的消息。
- **图片**：按 4800 字符（≈1200 token）计。
- **两个 usage 口径**：阈值估算取 **`totalTokens`**（prompt + completion，即"这一轮之后对话的规模"）；**静默溢出**判定取 `input + cacheRead`（prompt 的两半）配 `contextWindow`。
- **stale 豁免**（关键）：assistant 消息**早于最近一次压缩**时**不**作为 anchor（它记的是压缩前的旧规模）→ 退回全量估算。
- 阈值判定只有一条：`shouldCompact = ctx > contextWindow − reserveTokens`（16384）。

**为什么是"anchor + 尾部估算"而不是全量估算**：anchor 是 provider 数的真值，误差只落在尾部（很小）；全量 `chars/4` 对 CJK 会严重失准（见 1.4）。

## 1.2 我们现在怎么算（已核实）

- 写点唯一：`edc_task_add_usage`（`edc_task.c:580`），三出口（stop / tool_calls / error）都算。
- `task->last_total_tokens = total_tokens`（最近一次应答的 `prompt + completion`），**覆盖不累加**；usage 全零的应答不动它。
- 组：`edc_scheduler.c:1213` 取成员 `last_total_tokens` 的**最大值** → 快照 `ctx_tokens` → footer。
- **没有**：任何尾部估算、任何窗口概念（全仓无 `context_window` / `max_tokens`；请求体也**从不发** `max_tokens`）。

## 1.3 差异与后果（三个真问题）

1. **系统性偏低**：anchor 是"那一次请求发出去的"，之后追加的工具结果 / 新输入 / 新 task 的 turns 全不计。工具循环里"下一个请求"永远比显示的宽。
2. **折叠之后偏高**：折叠只压**已完成** task 的消息，**不重置**幸存成员的 anchor。而 anchor 记的是**折叠前**那个大前缀 → 折叠刚落地的那段时间，footer 显示的是旧的大数字，直到下一次应答把它刷新。（pi 用 stale 豁免挡住的就是这个。）
3. **没有窗口**：所以我们只知道"多大"，不知道"还剩多少"。折叠只能靠"消息条数"这种与窗口无关的代理指标（见 2.3 的隐患）。

## 1.4 方案（P1）

**A. 估算器**（新，`src/kernel/context/`）

```c
/* 单条消息的 token 估算（CJK 感知） */
long long edc_context_estimate_msg(const cJSON *msg);
/* 一段消息 / 一段文本的估算 */
long long edc_context_estimate_msgs(const cJSON *arr, size_t from, size_t to);
long long edc_context_estimate_text(const char *s);
```

估算法：`ascii_chars / 4 + non_ascii_chars`，每条消息再加一个小的结构开销常数；数组型 `content`（tool_calls 等）递归进字符串字段。

- **为什么不是纯 `chars/4`**：我们的输入大量是中文。中文约 1 token/字 → `chars/4` 会**低估 3~4 倍**。仍只是启发式（要标注），但方向对了。
- **校准办法**（可复现，写进测试）：拿一次真请求的 `usage.prompt_tokens`（provider 真值）与估算器对同一段文本的输出对比；在测试里用固定语料（纯 ASCII / 纯中文 / 混合 / 空 / 带 tool_calls）钉住相对误差区间，而不是钉死绝对值。

**B. anchor 有效性（对齐 pi 的 stale 豁免）**

- `group->fold_epoch`（每落地一次折叠 +1，落盘）；
- task 每次记 anchor 时同时记 `last_fold_epoch = group->fold_epoch`；
- **anchor 有效 ⟺ `m->last_fold_epoch == group->fold_epoch`**。

**C. 组口径（三选一，取第一个可用）**

```
1) 有有效 anchor 的成员 m：
     ctx = max over m [ m->last_total_tokens + estimate(该 anchor 之后追加的消息) ]
     尾部 = completion_seq > m->completion_seq 的成员的消息 + 它们/自己的 active_turn + todo reminder
2) 无有效 anchor（刚折过 / 组里还没有任何应答）：
     ctx = estimate(该组"下一次请求"的全部消息)   ← memory 文本 + system + 未折叠历史 + …
3) 组为空：0
```

**D. 与 P2 共用一个函数**：`edc_context_estimate_group_tokens(sched, group)` 就是"这个组下一次请求会有多大"。
- footer 用它（口径 C）；
- 折叠触发用它（`> window − reserve`）；
- 溢出恢复用它（判断要不要加大力度）。
**一份估算，三个消费者**——这也是本期最值的部分。

**E. 显示（可选）**：估算值 / 失效期显示成 `ctx ~72k`，锚定真值显示成 `ctx 65.5k`。诚实，但会让那行在折叠前后"跳"一下。

**判据（P1）**：估算器单测（5 类语料 + 相对误差区间）；anchor 失效规则单测（折叠前后各一例）；组聚合单测（有效/失效/混合）；`test_session_snapshot` 的真生产者用例（不得再只用喂手写 JSON 的解析测试——上次 M4 就是这么空转的）；变异：去掉尾部估算 → 红；忘掉 `fold_epoch` 失效 → 红；CJK 分支写反 → 红。

---

# 第二部分：记忆折叠

## 2.1 pi 的机制（清单）

| # | 机制 | 要点 |
|---|---|---|
| 1 | 触发（主动） | `ctx > window − reserve(16k)`；三个挂载点：turn 之间（主）/ 新 prompt 前 / run 结束 |
| 2 | 触发（被动） | 三类溢出：错误正则（~25 种厂商）/ 静默溢出（成功但 `input+cacheRead > window`）/ `length` 截断（output=0 且 input ≥ 99%）。→ 移出失败消息 → 压缩 → `continue()` 重试**一次** |
| 3 | 豁免 | 换模型（消息来自别的模型不触发）、stale usage（早于上次压缩）、限流错误不算溢出 |
| 4 | 切点 | 从尾部往前累加到 `keepRecentTokens`(20k)；只落在**合法切点**上；预算耗尽在 toolResult 上就**向前多留** |
| 5 | split turn | 单个 turn > 预算 → 切在 turn 中部的 assistant 上 → 前缀摘要+历史摘要**两段合并**（前缀预算 0.5×reserve） |
| 6 | 摘要调用 | 独立 one-shot；system 明令 "Do NOT continue the conversation. ONLY output the structured summary."；有旧摘要 → **迭代更新** prompt（保留所有旧信息、In Progress→Done、更新 Next Steps）；`maxTokens = min(0.8×reserve, model max)`；不写缓存 |
| 7 | 摘要失败判定 | `stopReason` 为 error/length → 不算数；响应里带 toolCall → 失败 |
| 8 | 摘要格式 | `## Goal / ## Constraints & Preferences / ## Progress(Done·In Progress·Blocked) / ## Key Decisions / ## Next Steps / ## Critical Context` + `<read-files>` / `<modified-files>`；要求保留**精确的路径、函数名、错误原文** |
| 9 | 文件追踪 | 从被摘要消息的 tool call 里抽文件操作，**跨多次压缩累积** |
| 10 | 持久化/重建 | `CompactionEntry{summary, firstKeptEntryId, tokensBefore, usage}`；重建 = `[system][summary][firstKeptEntryId…]` |
| 11 | 重复压缩的摘要范围 | 从**上一次的 `firstKeptEntryId`** 起（上次"幸存"的这次也被摘要）→ 不静默丢信息 |
| 12 | 缓存 | 压缩 = 一次有界的缓存重置；`keepRecentTokens` 把"全量重写"上限钉在 ~22k |

## 2.2 我们现在（已核实）

- **触发**：`edc_group_fold_due` = `foldable > 0 && unfolded_msgs >= group_fold_msg_threshold`（默认 500，你配 200）。**纯消息条数**。
- **切点**：`edc_group_fold_build_prompt` = 按 `completion_seq` 排序后，从最老开始按"消息数 ≤ 未折叠总数的一半"选一批 task；**折最老一半**，`max_seq` = 最后被选中的 task 的 `completion_seq`。
- **屏障**：起折给组内每个非终态成员挂 `SEQUENCE` dep（B1）；在飞成员到下一个请求边界才 park。
- **摘要**：`## Existing Group Memory` + `## New Completed Tasks`（每条 `role: content` + `output:`）+ 一句 "Merge … Output ONLY the merged Group Memory as concise markdown."。**没有**结构化格式、没有迭代规则、没有 `max_tokens`。
- **落地**：`edc_group_fold_deliver_memory` 写 `group->memory`，把 `completion_seq <= fold_max_seq` 的已完成 task 标 `turns_folded`，推 `folded_through_completion_seq`。**只要内容非空就写**（无卫生检查）。
- **失败**：折叠任务走一般错误路径 → task ERROR；**记忆与水位不动**；下一次有 task 完成、又满足条数阈值 → 再折一次。
- **折叠范围**：只折**已完成** task 的 turns（`edc_task_is_foldable` = `COMPLETED && !turns_folded && sink != GROUP_MEMORY`）。**正在跑的长任务永远不会被压**。

## 2.3 存在的一个隐患（不是风格问题）

折叠 prompt = 被折 task 的 turns **明文全文**。触发只看**条数**（200），与窗口无关 ⇒ **折叠请求自己可能就超窗** → LLM 报溢出 → 折叠任务 ERROR（记忆/水位不动）→ 下一个 task 完成 → 又满足条数 → **再折、再失败**（无进展的重试环），而且每次失败都要花一次请求。

pi 的结构天然没有这个问题：**触发发生在接近窗口时，所以"要摘要的部分" ≤ 窗口 − reserve − keep**，摘要请求必然装得下。⇒ **token 触发 + 尾部预算**不只是"更准"，它把这条隐患一起消掉。

## 2.4 方案（P2）

**① 触发改用 token（消息数降级为兜底）**

```
折 ⟺ foldable > 0 && ( estimate_group_next_request(group) > context_window − fold_reserve_tokens
                     || unfolded_msgs >= group_fold_msg_threshold )     ← 后者保留：窗口未知时的兜底
```
新增配置：`context_window`（**0 = 未知，禁用 token 触发**，绝不猜）、`fold_reserve_tokens`（默认 16384，对齐 pi）、`fold_keep_recent_tokens`（默认 20000）。

**② 切点改成尾部预算 × task 边界**

从**最新**的 foldable task 往回累加**估算 token**，累到 ≥ `fold_keep_recent_tokens` 为止；切点落在该 task **之前**（更老的都折），至少折 1 个 task（否则不折，等更多）。

- 保守（先不引入 split turn）：粒度仍是 **task 边界**（`completion_seq` 水位不用动）；代价是"保留区 ≈ keep + 最后一个 task"。
- 收益：① 被折部分 ≤ 窗口 − reserve（摘要请求装得下）；② 保留区有预算，不再靠"一半"这种相对量；③ 与 pi 的 `keepRecentTokens` 同一个心智模型。

**③ 摘要格式与迭代（直接采纳 pi 的）**

- 结构化 6 段 + `Additional focus`（留给手动触发）+ "保留精确路径/函数名/错误原文"；
- 有旧记忆 → **迭代更新**语义：保留所有已有信息、In Progress → Done 迁移、更新 Next Steps（不再是一句"merge"）；
- system 用 pi 的强约束："Do NOT continue the conversation. ONLY output the summary."
- 序列化沿用今天的 `role: content`（等价于 pi 的 `serializeConversation`），但把工具调用也带上：今天的实现只在 `role` 与 `content` **都是字符串**时才输出一行（`edc_group_fold.c:203`），所以**带 `tool_calls` 的 assistant 消息整条被跳过** —— 摘要器看得到工具**结果**，看不到"调用了什么"（见 2.6）。

**④ 摘要卫生（便宜且值）** —— ✅ 已落地（2026-10-09）

落地前检查（`edc_group_fold_deliver_memory` 之前）：非空；`stop` 正常结束；**没有**疑似工具调用；没有被 `length` 截断。任一不满足 → **不写记忆、不推水位**。

四条各有归属：**非空** = `deliver_memory` 开头的判空；**正常结束 / 非截断** = executor 只对 `EDC_REASON_STOP`
走完成路径，`LENGTH` 单独映射成 ERROR（P2b-2），折叠任务的提示直说 "discarded"；**无工具调用** = 新增
守卫（`exec_done` 的 `EDC_REASON_TOOL_CALL` 分支：`sink == GROUP_MEMORY` 直接 `mark_failed`，**不派发、
不入账本、不写记忆** —— pi §3 "Summarization attempted to call a tool"）。用量仍在公共段累加，废掉那次
调用照样记账。

**⑤ 给折叠请求加 `max_tokens`**

我们请求体从不发 `max_tokens`（已核实）→ 摘要长度没有上界（记忆会一直长）。加一个可选 `max_tokens`（默认 = `min(0.8 × fold_reserve_tokens, 模型上限)`，对齐 pi），只有折叠请求用得上；这需要 `edc_llm_actor` 支持一个可选字段。

**⑥ 手动触发（可选）**：`/group fold <id> [focus]` ← pi 的 `/compact [instructions]`。

**⑦ 不做**：扩展钩子（我们无插件体系）、分支摘要（`/tree` 我们也没有）。

**判据（P2）**：触发（token 到/没到、窗口=0 时退回条数）；切点（尾部预算的边界用例：预算落在最后一个 task 内 / 只有一个巨大 task / 全都很小）；摘要卫生（截断/空/工具调用三类各一例，必须"记忆与水位都不动"）；**"折叠请求装得下"**用一条断言把它钉住（estimate(被折部分) < window − reserve）；变异：切点忽略预算 → 红；卫生检查去掉 → 红；触发忽略窗口 0 → 红。

**落地情况（2026-10-09）**：以上判据全部为真。`test_fold_request_fits_the_window` 在**触发线最坏位置**
（`window − reserve = ctx − 1`）断言 `estimate(prompt) < window − reserve` 且最新那条仍在细节里；
`test_fold_tool_call_is_failure`（`tests/test_system_task.c`）钉工具调用守卫，并用一个普通任务作对照
（同一条路照常 `AWAIT_TOOLS` + 派发），变异 M19（去守卫）/ M20（生效范围过宽）红；变异 M21（预算→0）/
M22（丢掉尾部预算）红在既有切点用例 + 新用例的"最新那条留在细节里"。

## 2.5 溢出检测 + 提示（P3 —— **已落地 2026-10-09**，分支 `memory`）

> **裁决（2026-10-09）：不自动恢复。** 命中溢出只给一条明确的提示，任务该失败还是失败
> （静默溢出时任务本来就成功，提示就附在结果旁边）。理由：自动收紧折叠 + 自动 resume 会引入
> "收紧多少 / 只试一次 / 递归（折叠请求自己超窗）/ 换模型豁免 / attempted 位要不要落盘"一整串
> 状态，收益却不比一句"该怎么办"的提示大。**没有自动动作 ⇒ 那些豁免规则全部不需要。**（保持这条
> 可逆：检测这一层是恢复的前置，将来想要恢复，加动作即可。）

**① 分类器（纯函数，新文件）** `src/kernel/executor/edc_llm_error_kind.{c,h}`：

```c
edc_llm_error_kind edc_llm_error_classify(int http_status, const char *message);
/* -> CONTEXT_OVERFLOW / RATE_LIMIT / AUTH / TRANSPORT / OTHER */
```

判定顺序（**顺序即语义**）：`http_status == 0` ⇒ TRANSPORT（连状态行都没有）；`429` 或 message 含
`rate limit` / `too many requests` ⇒ RATE_LIMIT（**先排除** —— pi 的显式反例）；message 含
`maximum context length` / `reduce the length of the messages` / `context_length_exceeded` / `prompt is too long`
⇒ CONTEXT_OVERFLOW；`401`/`403` 或 `Authentication` / `invalid api key` ⇒ AUTH；其余 OTHER。
**以 message 为主键**：实测那条 400 的 `type`/`code` 都是泛化的 `invalid_request_error`（见 2.5.1），
状态码只用来分 400 与 429、以及认"根本没有响应"。

**② 三个检测点**（都在 `edc_executor.c` 的 `exec_done` 里，挂在已有分支上，不新增层）：

| # | 落点 | 条件 | 结果 |
|---|---|---|---|
| ① | 失败路径（`e->failed`） | classify ⇒ CONTEXT_OVERFLOW | 任务照旧 ERROR；notice = **provider 原话（自带真数字）+ 一句带数字的下一步** |
| ② | `EDC_REASON_STOP` 分支 | `context_window > 0 && prompt_tokens > window` | 任务**照常 COMPLETED**；提示走 `result.warning`（**不叫停、不改回复**） |
| ③ | ERROR 分支 | `length && completion_tokens == 0 && prompt >= 99% x window` | 认成溢出（普通 `length` 仍是通用文案） |

- **状态码随事件走**：`edc_event_exec` 新增 `int http_status`（`advance_llm_resp` 从 `resp` 搬）。
  丢掉它，①就退化成 TRANSPORT（这条有测试钉住）。
- **找回一个一直白丢的信号**：`finish_reason == "context_length_exceeded"` **早就解析了**
  （`edc_llm_actor.c` ⇒ `BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED`），但 executor 的词表只认
  STOP/TOOL_CALL/LENGTH ⇒ 被压成 `EDC_REASON_OTHER` → "unknown finish reason"。现在有
  `EDC_REASON_CONTEXT_EXCEEDED` 这一格，也有穿过适配器的测试。
- **静默类需要第二个出口**：`struct edc_advance_result` 新增 `char warning[256]`（**值不是指针** ——
  它在 executor 的栈帧里生成，数组让生命周期零问题；空串 = 没话说）。scheduler 在 COMPLETED 分支里
  把回复发完之后，把它当 **SYS 消息**再发一条（不走 `sched_error_copy`：它是建议，不是错误）。

**③ 提示文案**（复用现有 notice 管道 ⇒ **零 UI 改动**；带数字 + 带下一步）：

```
context overflow (the provider refused the request): this request held about 2.0M tokens and the model
window is 1.0M — run /group fold to tighten the context, then retry (or switch to a model with a larger window)
```

- 数字来源：`prompt_tokens`（真 usage；0 → 显示 unknown）+ `config->context_window`（0 → unknown）；
  ① 还把 provider 原话整段保留在前面（它自带"请求了多大 / 窗口多大"）。
- **窗口未知（默认 0）时 ②③ 不触发** —— 没有比较对象就不下结论。
- **折叠任务自己超窗**：照旧失败、沿用折叠话术（"discarded, the group memory is unchanged"），
  **不给"去 `/group fold`"的建议**（它*就是*折叠），**一律不递归**。

**判据（P3，已达成）**：新测 `tests/test_llm_error_kind.c`（**23 断言**：三类溢出的各家说法 + 限流反例 +
传输/鉴权/其余 + 大小写不敏感 + NULL）与 `tests/test_executor_overflow.c`（**53 断言**：三个落点各走真
executor + 限流反例 + 窗口未知不触发 + 投递顺序 + 折叠护栏 + **穿过 `advance_llm_resp` 的 reason 映射与
状态码**）。**九条变异全红**：限流→溢出 14 / `length` 不看 `completion==0` 2 / 去 99% 门槛 2 / 丢状态码 4 /
窗口未知也判 2 / 映射丢回 OTHER 4 / 折叠任务也给建议 2 / ①去 sink 护栏 2 / 不投递 warning 2。

### 2.5.1 实测（2026-10-09，真打 provider）

探针：先校准 tokenizer 比例（`deepseek-v4-flash` 实测 **4.11 字符/token**），再按目标 token 数投纯文本、`max_tokens=1`。

| 目标 | 结果 |
|---|---|
| 250k | HTTP 200，`prompt_tokens=251330`（**未截断**） |
| 500k | HTTP 200，`prompt_tokens=502620`（未截断） |
| 1M | HTTP 200，`prompt_tokens=1005501`（未截断） |
| **2M** | **HTTP 400**：`This model's maximum context length is 1048576 tokens. However, you requested 2011367 tokens (2011366 in the messages, 1 in the completion). Please reduce the length of the messages or completion.` |

**结论（三条，都改变了设计）**：

1. **窗口真值 = 1,048,576**（1 MiB）—— `GET /models` 直接给了元数据：
   `{"id":"deepseek-flash","name":"DeepSeek-V4.1-Flash","context_window":1048576,"max_output_tokens":393216,...}`。
   ⇒ 用户这份 config 的 `context_window = 0`（未知）可以改成 1048576 或刻意更小的值；**注意**：填 1M 时触发线在
   1,032,192（1M − reserve），而实测会话最大 ~253k ⇒ **token 轴实际上永不触发**（= pi 意义上的"显式选择不按 token 折"）。
2. **≤ 窗口以内不会静默截断**（1M 那发报的 `prompt_tokens` 与实投一致）⇒ 这个 provider 的**静默溢出类不存在**，
   别为它写复杂逻辑；保留判定即可（其它 provider 仍然可能）。
3. **判别靠 message 文本，不靠 `code`**：溢出的 `type`/`code` 都是泛化的 `invalid_request_error`，只有 message 里的
   `maximum context length is <N>` / `Please reduce the length` 是判别特征。⇒ 正则表必须以 message 为主键，
   并且**顺带能从中抽到"请求了多大 / 窗口多大"**（提示里可以直接写真实数字）。
   **限流**（`rate limit` / `too many requests`）在 message 上显著不同 ⇒ 反例可测。

## 2.6 不采纳 / 已知限制（写下来免得以后当 bug 报）

| 项 | 处置 | 理由 |
|---|---|---|
| **单 task 巨型 turn 的 split** | ✅ **已落地**（P2a/P2b） | 水位已从 `completion_seq` 改成**消息级** `struct edc_turn.folded_prefix`（P2a-2），P2b-4 又补了 split-turn 指令段 —— 这条原记录已作废 |
| **正在跑的长任务不被压** | ✅ **已落地**（裁决 7a） | 起折时给组内每个非终态成员挂 `SEQUENCE` dep，成员到自己的**请求边界**才 park（工具结果照收）—— 不再是"只折已完成" |
| **文件操作累积追踪** | **不做**（用户 2026-10-09 裁决，P4 候选保留） | 我们是 coding-capable agent，可能有价值；但要抽 tool call 参数 + 跨轮累积 + 落盘，独立一摊 |
| **记忆本身无下限保护** | **接受现状**（用户 2026-10-09 裁决） | 窗口 < ~1.8 × reserve 时触发会偏密（只是吵/贵，不报错）；P2b-2 的 `max_tokens` 封顶已让记忆单体装得进 reserve，pi 同样没有下限保护 |
| **扩展钩子 / 分支摘要** | 不做 | 无对应体系 |
| **缓存断点 / 缓存统计** | 不做（可记录） | 我们已有命中率；只需在文档里写明"**折叠 = 一次缓存重置**"，免得命中率突然掉被当 bug |
| **`memory_compress_threshold`** | ✅ 已清（2026-10-09） | 已核实**全仓无读者**（`src/` 零引用），本次删掉 `tools/ab/ab.py` 夹具里那条；`config.json` 是用户侧文件，键留着也无害（`edc_init_env` 只补缺不删键，不会自动消失） |

## 2.7 折叠任务的成本口径（已知，不改）

摘要调用是**系统组**里的一个 task，它的 usage 记在**系统组**的计数器上，不记进被折的组。⇒ 侧栏里"被折组"的 `↑↓` 不含这次摘要；系统组的用量会涨。pi 是把摘要 usage 计进会话成本的（`CompactionEntry.usage`）。**只是口径差异，不是 bug**；要不要改（例如把摘要用量记回被折组）需拍板。

## 2.8 折叠请求的形状：自足（2026-10-09 订正）

折叠任务住在 **system group** 里。要担心的两件事都**不成立**，第三件成立且已修：

| 问题 | 结论 | 依据（护栏） |
|---|---|---|
| 折叠任务会不会共享目标组的 `group->memory`？ | **不会** | 请求装配只认 `task->group`（= 系统组），不认 `task->sink_group`（= 目标组）。目标组的记忆**只**以折叠 prompt 里 `[<previous-summary>]` 的形式进来（pi 的迭代语义，故意的） |
| 会不会累积上一轮折叠的 prompt / 摘要？ | **不会** | 三条同向护栏：① 落盘侧 `edc_executor.c` 只有 `sink != GROUP_MEMORY` 才把 turn 并进组历史；② 组内容序 `edc_task_group_content_order` 跳过 `sink == GROUP_MEMORY` 成员；③ `rebuild_history` 同口径 |
| 重启后呢？ | **曾会** ✗→✅ | `group->history` **不落盘** ⇒ 加载期 `rebuild_history` 给空 history 现拼**主提示词**；而折叠说明当时住 `history[0]`、且"只在空时写"的守卫不会纠正 ⇒ **每条折叠请求都背着主提示词**。同一路径也让 **resume 的折叠**（不起折、只重发请求）拿到主提示词 |

**修法（2026-10-09）**：折叠请求**自足** —— `edc_llm_request.c` 对 `sink == GROUP_MEMORY` 直接
`[edc_group_fold_system_prompt()]` + 它自己的 turn，**完全不走组上下文**（不调
`edc_context_append_system_part`、不注入组历史）；系统组不再有共享头部；加载期也不再给系统组
现拼主提示词。判据：`tests/test_llm_request.c` 的 **"折叠请求是自足的"**（夹具显式把主提示词摆进
系统组 `history[0]`，断言请求第一条消息**逐字节**等于折叠说明且不含 `# EdgeClaw`；变异：改回走组
上下文 → 2 红，既自足又走一遍组上下文 → 1 红）。

## 2.9 窗口是唯一判据：默认 1M，条数轴退休（2026-10-10）

**决定（用户）**：删掉"靠消息条数判断要不要折"的逻辑；`context_window` 不会为 0，因为有默认值 1M。

**为什么**：条数轴本来只是"窗口未知"时的兜底（写法乙的分支）。一旦窗口有了默认值，那条分支
就**必然不可达** —— 留着是一条死路，而"兜底只在键缺席时可达"正是本仓库吃过亏的写法
（§7.2 的那句警告就是这么来的）。窗口是这台机器上唯一能回答"下一次请求装得下吗"的量。

**落地**：

| 面 | 改动 |
|---|---|
| 默认值 | `EDC_CONFIG_DEFAULT_CONTEXT_WINDOW = 1024 * 1024`（= 默认模型 `deepseek-flash` 的实测窗口；该 id 现由 `DeepSeek-V4.1-Flash-1017` 提供，与旧名 `deepseek-v4-flash` 同一 1M 家族，§2.5.1） |
| 修复 | `edc_init_env` 的 top-up 新增 `ensure_positive_int_key`：`context_window` **缺失 / 非数字 / `<= 0`** 一律写成默认值（并重写文件，所以盘上不会留一个撒谎的 0）；读侧 `edc_config.c` 同样只接受正数 —— 双保险，因为启动顺序里 top-up 先跑 |
| 退休 | `group_fold_msg_threshold`：字段、默认值、ensure、读侧全删；键进 `drop_obsolete_keys`（老 config.json 里的那条会在下次启动被清掉） |
| 判据 | `edc_group_fold_due` = `foldable > 0 && failures < 3 && ctx > 窗口 - reserve`。窗口仍读不到（脏 config）时**返回 false**：不知道窗口不是"随便折"的理由；手动 `/group fold` 不受影响 |
| 夹具 | A/B 的 `fold` 改成手动 `/group fold` 触发（`AB_CONTEXT_WINDOW` 可让将来的夹具按窗口触发）：夹具语料只有几十 token，1M 窗口下自动路径永远不会过线 |

**代价（已知，写下来）**：窗口必须**跟真实模型一致**。填太大 ⇒ 折叠来晚了（provider 先报溢出，
P3 会给一条带数字的提示）；填太小（比如小于"系统提示 + 记忆"这个基线）⇒ 组每轮都"该折"，
变成一直在折。所以这个值不是"随便填一个数"，它现在是一等配置。

**验证**：`bazel clean` → build **903 actions**、全量 **64/64**；**6 条变异全红**（窗口 <= 0 也照折 /
条数轴复活为 OR / 默认值退回 0 / top-up 不修 <= 0 / 退役键不删 / 读侧不修 0），逐字节还原。
A/B 三夹具对**同 HEAD 基线二进制**：`session.json` / `chat.jsonl` / `llm_requests.jsonl` /
`.task_seq` 全部逐字节相同；日志剥时钟 + 抹行号后唯一差异是旧二进制多一条"config 重写"日志
（它在给已退休的键补默认值）。`fold` 夹具的折叠产物（切点 `task 826 turn 0 msg 1`、prompt
1092 字节、折叠任务 827、落地 "folded 1 turns"）**新旧完全一致** —— 手动入口与自动入口共用同一条
start → 屏障 → prompt → 落地路径。

**变异电池的收获**：M5/M6 第一版**没红** —— `cJSON_ReplaceItemInObject` 对缺席的键是 no-op，
测试里"手工写回一个 0 / 写回那条退役键"其实**什么都没写**，断言空转。改成 delete+add 并补
"前置：文件里真的写着 0"之后两条都红。**（老教训的新案例：先证明夹具真的摆好了，再断言结果。）**

---

---

# 第三部分：分期

| 期 | 内容 | 规模（估） | 触点 |
|---|---|---|---|
| **P1** | ✅ **已完成（2026-10-09，commit `d83f4ba`）** 估算器（CJK 感知）+ anchor 失效（`fold_epoch`）+ 组口径三选一 + 快照 `ctx_tokens`/`ctx_estimated` + footer `~` | 中（context/ + scheduler + serializer + tui 注释 + 测试） | `edc_context.*`、`edc_task.*`、`edc_scheduler.c`、`edc_state_serializer.c` |
| **P2a** | ✅ **已完成（2026-10-09，commit `d83f4ba`）** 窗口配置 + 水位重构（消息级 `folded_prefix`，schema v6 + 迁移脚本）+ 触发（写法乙，两个挂载点）+ 切点（尾部 token 预算 / 在飞 turn 进语料 / 切点合法性） | 中大 | `edc_group_fold.*`、`edc_config.*`、`edc_state_serializer.c`、`edc_context.*` |
| **P2b** | ✅ **已完成（2026-10-09，commit `3b9baf6`）** 摘要 prompt 结构化（6 段 + `<previous-summary>` 迭代 + focus）、折叠请求 `max_tokens` 且截断不当 checkpoint、手动 `/group fold [id] [focus]`、split-turn 段 | 中 | `edc_group_fold.*`、`edc_llm_actor.*`、`edc_llm_request.c`、`edc_executor.*`、`edc_scheduler.*`、`edc_commands.c` |
| **P2b 卫生** | ✅ **已完成（2026-10-09，commit `824ebc8`）** 四条落地前检查齐（非空 / 正常结束 / 非截断 / **无工具调用**）；"折叠请求装得下"钉成用例 | 小 | `edc_executor.c`、`tests/test_system_task.c`、`tests/test_group_fold.c` |
| **P3** | ✅ **已完成（2026-10-09，分支 `memory`）** 分类器 + 三个检测点 + 一条带数字的提示（**不自动恢复**，裁决 #10）；顺带找回 `context_length_exceeded` 这个一直白丢的信号 | 小 | `executor/edc_llm_error_kind.{c,h}`、`edc_executor.{c,h}`、`edc_scheduler.c`、两个新测试 |
| **P4（可选）** | ⏸ 文件操作累积追踪（pi 的 `<read-files>`/`<modified-files>`）—— 用户 2026-10-09 裁决**先不做** | 小 | `edc_executor.c`、`edc_llm_request.c` |

**流程**（按仓里规矩）：一期一 commit；每期先给判据再动代码；`bazel clean` 后 `bazel build //...` + 全量 `bazel test //src/tui:all //tests:all --nocache_test_results`；变异自检**必须打到生产者**（`test_session_snapshot` 那种真生产者用例，不能只喂手写 JSON）；A/B 五夹具（`smoke`/`switch`/`bg`/`cancel`/`fold`）比对，其中 **`fold` 夹具是这套改动的主战场**。

---

# 第四部分：已拍板（1–15，2026-10-09）

**用户裁决：全部按推荐（1–15 已闭）。2026-10-09 二次澄清两处**：① 触发为**写法乙**（窗口优先，
缺失才用条数）；② 因此 `context_window` **默认 = 0（未配置/未知）**，原"默认 1M"作废。

| # | 裁决 | 落地 |
|---|---|---|
| 1 | ~~**A** 显式配置 `context_window`；默认 0 = 未配置/未知 ⇒ 走条数轴~~ **2026-10-10 取代**：窗口有**默认值 1M**，缺失/`<= 0` 一律被修成它；窗口是**唯一**判据（见 §2.9） | P2 → 已改 |
| 2 | **B** footer 区分精确 / 估算：锚定真值 `ctx 65.5k`、含估算 `ctx ~72k` | ✅ P1 |
| 3 | **A** 估算器 CJK 感知（`ascii/4 + non_ascii×1`） | ✅ P1 |
| 4 | **A** 顺序 P1 → P2 → P3，一期一 commit | — |
| 5 | **B** 触发时机：task 完成时 **+ 每次 LLM 请求前** | P2 |
| 6 | ~~**A** 条数是**次选**（写法乙：窗口缺失/为 0 时才用条数）~~ **2026-10-10 取代**：条数轴删掉，`group_fold_msg_threshold` 从 config 退休（见 §2.9） | P2 → 已改 |
| 7a | **B** 范围：**运行中的 task 也能被折**（折叠任务已是每个在飞成员的 dep ⇒ 到请求边界 park、结果照收，等折叠落地） | P2（最大改动面，见 §6） |
| 7b | **B** 粒度：**消息级水位**（不再是 `completion_seq` 标量；支持切在 turn 内部 = pi 的 split turn） | P2 |
| 8 | **要** 手动 `/group fold [id] [focus]`（focus 进摘要 prompt） | P2 |
| 9 | **A** 摘要调用的用量记在**系统组**（不改） | — |
| 13 | **跟 pi**：`reserve` **16k**、`keep_recent` **20k**（固定值）、摘要上限 **`min(0.8×reserve, 模型上限)`** | P2 |
| 14 | **A** 触发判据 = **P1 的组 ctx**（`edc_context_group_ctx_tokens`）—— 单一来源 | P2 |
| 15 | **A** `context_window` 跟 P2 一起落（**不留死键**）：**默认 0 = 未配置/未知**（见第 1 行二次澄清） | P2 |

> **2026-10-09 二次澄清后的语义**：不填 `context_window` ⇒ 行为与今天一致（按 200 条折）；
> 填了窗口 ⇒ **token 轴独占**（按窗口压力折，pi 式）。实测你最大的组到过 **253,735** token
> ⇒ 填 256k 时它已在触发线上；填 1M 则等于**你显式选择**不按 token 折。
>
> **实测数据（2026-10-09 扫 `s13/session.json`，字段 `task.last_total_tokens` = 最近一发的
> `usage.total_tokens`）**：本次对话所在的 group 768 = **190,732 / 192,007 / 199,079 / 253,735**
> （task 810 / 814 / 815 / 807，全部是这一轮对话）；相邻 group 789 里两个 `fold group memory`
> = **219,604 / 214,594** ⇒ **折叠请求自己就 ~215k**（§2.3 隐患的实测数字）。
>
> **同一份存档的分解（这才是关键）**：group 768 共 42 成员 / 1244 条消息；**已折 31 个成员 /
> 1152 条**，**未折 11 个成员 / 只剩 92 条** —— 而请求带的就是那 92 条，**= 253,735 token**。
> ⇒ **平均 ~2.8k token/条**（这条对话里全是长文档读取与 shell 转储），
> ⇒ **条数轴此刻把上下文压在 ~25 万 token，不是 2 万**；而且 **折掉的 1152 条 ≈ 215k token、
> 留下的 92 条 ≈ 250k token** —— 条数与 token 在这台机器上差 **~15 倍**，这正是 P2 改用 token 的实证。

---

# 第五部分：仍待拍板（10–12）

> 只剩这三条，且都不挡 P2。回复点号即可（"10A 11A 12A"）。

**10.（P3 动工前必须定）⚠️ 窗口爆了要不要自动恢复？** —— ✅ **已定 B（2026-10-09，用户裁决）**：
**不自动恢复，但给一条明确的系统提示**。scope 因此从"分类 + 收紧折叠 + 重试一次"缩到"**分类 + 提示**"，
连带这些设计点全部作废：`overflow_recovery_attempted` 存哪、收紧力度、换模型豁免、递归上界。
见 §2.5。

**11. 死键 `memory_compress_threshold`** —— ✅ **已定 A（清掉）并落地（2026-10-09）**：删 `tools/ab/ab.py` 夹具里那条；`src/` 本来就没读者。你 `config.json` 里那条我改不到（在可写路径外），留着无害 —— 想清就 `python3 -c "import json,pathlib;p=pathlib.Path.home()/'.edgeclaw/config.json';d=json.loads(p.read_text());d.pop('memory_compress_threshold',None);p.write_text(json.dumps(d,indent='\t',ensure_ascii=False))"`。

**12. 文件操作追踪**（pi 的 `<read-files>` / `<modified-files>`，跨多次压缩累积）：**用户 2026-10-09 裁决 A —— 先不做**（P4 候选保留）。

---

# 第六部分：7a=B 的机械后果（设计约束）

## 7a = B 的机械后果（P2 的改动面，已核实）

"运行中的 task 也能被折" + "消息级水位" 两条连起来，说的是 **pi 的 split turn**：切点可以落在
**一个还在跑的 turn 内部**。今天做不到，卡在一个事实上：

- 折叠语料 = `group->history`（**只有 commit 过的 turn**，`executor.c:146`）；
- 在跑 task 的 `active_turn` **不在** history 里 ⇒ 今天它根本进不了折叠语料；
- 而且请求装配 = `memory + system + history[水位..] + **整个** active_turn` ⇒ 就算把 turn 前缀折了，
  它**还会被原样再发一遍**（摘要 + 原文双份）。

所以要落 7a=B，这三处必须一起改：

| 处 | 今天 | 要变成 |
|---|---|---|
| 折叠语料 | `group->history` 里"已完成 task 的 turns" | 还要含**在飞成员 active_turn 的前缀** |
| 水位 | `group->folded_through_completion_seq`（标量，task 级） | **消息级位置**（近似 pi 的 `firstKeptEntryId`）：history 侧一个位置 + **每个 task 自己 turn 内的一个位置** |
| 请求装配 | `history[1+folded_msg_count..] + 整个 turn` | `history[hist_watermark..] + turn[turn_watermark..]` |

连带的记账要重做（这块就是 P2 的主要工作量）：`edc_task_is_foldable` 的"只折已完成"、
`turns_folded` 布尔、`edc_task_group_folded_msg_count` / `_unfolded_msg_count`（按 task 数消息）、
落盘的水位键，以及 **turn 提交时的交接**（在飞 turn 后来 commit 进 history，它被折掉的那段前缀
必须被算进 history 侧水位，否则会再发一遍）。


---

# 第七部分：P2 设计（判据先行）

> 依据 §4 的 15 条裁决 + §6 的改动面。这一步**先出判据，再动代码**。

## 7.1 核心：水位不能是"group 的历史前缀切片"

今天的模型是 `请求 = memory + sys + history[1+folded_msg_count..] + 整个 active_turn`，
其中 `folded_msg_count` 由 `Σ(completion_seq ≤ 水位的 task 的消息数)` 推出 ⇒ **只有"折掉最老的若干个
已完成 task"时它才等于历史数组的前缀**。7a=B（运行中的 task 也能折）+ 7b=B（切在 turn 内部）一上来
就打破它：一个还在跑的 turn 的前缀被折了，但它**不在** history 里（`executor.c:146` 只在 commit
时写进去），而且就算折了，请求里那个 turn 是**整段**注入的 ⇒ **摘要 + 原文双份**。更糟的是那个 turn
后来 commit 进 history，它会落在**折叠前缀之后**（追加在尾部）⇒ 别的 task 从此每次请求都再发一遍。

所以水位的权威换成**"哪一段内容被折过"**，落在 turn 上：

```
struct edc_turn  +=  int folded_prefix;   /* 本 turn 前 N 条消息已进记忆（落盘） */
```

三处配合（这是本期的骨架）：

| 处 | 规则 |
|---|---|
| **请求装配** | `memory + sys + group->history[1..]`（history 里**只留未折叠**）+ `turn[folded_prefix..]` + deps + tail |
| **折叠落地** | ① 把被折消息从 `group->history` 里**剪掉**（数组只留"还会发出去的东西"）；② 给被折的 turn 记 `folded_prefix`；③ `fold_epoch++`（P1 已有） |
| **turn 提交** | `executor.c` 往 history 追加时只追加 `turn[folded_prefix..]`；`deser_rebuild_group_history` 用**同一条规则** ⇒ 重启后数组与运行期同形 |

**退休清单**（一并删，别留半套）：`task->turns_folded`、`group->folded_through_completion_seq`、
`edc_task_group_folded_msg_count()` / `_unfolded_msg_count()`（改为从 turns 现算）、
`edc_task_is_foldable()` 的"只折已完成"（新判据 = **身上还有未折叠消息**）、
`edc_llm_request.c` 里的 `1 + folded_msg_count` 切片与 `edc_commands.c` 的 fork 切片（同一个）。
`/task fork` 的语义不变（它要的是"这个组还会发出去的历史" = 现在的 `history[1..]`）。

## 7.2 触发（裁决 5B / 6A / 14）

> **2026-10-10 更新（见 §2.9）**：下面的**条数兜底已删除**，窗口是唯一判据；`context_window`
> 也不再可能 <= 0（启动期修成默认 1M）。本节保留为当期的决策记录。

**判据（单一来源 = P1 的组 ctx）**：

```
该折 ⟺  (context_window > 0 && edc_context_group_ctx_tokens(g) > context_window - reserve)
      ||  (unfolded_msgs(g) >= group_fold_msg_threshold)     ← 2026-10-10 已删
```

**已定案（2026-10-09 二次澄清）= 写法乙**：`context_window > 0` 时按窗口触发（token 轴独占）；
窗口**缺失/为 0** 时才用消息条数。

```c
/* 乙（= 定案）：窗口优先，缺失才退回条数 */
due = (window > 0) ? (ctx > window - reserve) : (unfolded_msgs >= msg_threshold);
```

> ⚠️ 唯一的硬约束：**默认值必须是 0**。只要默认是 1M，"键缺失"这条路径就拿不到"缺失"这个信息
> （缺省的键也会填成 1M）⇒ 条数轴变死代码，而 token 轴的触发线 983,616 又远高于实际上下文
> ⇒ 两根轴同时落空、**永不折叠**。这也解释了一般教训：**只在"键缺席"时才可达的兜底，会被任何
> 默认值悄悄杀死**；要让"缺失"可观测，默认值只能表示"未知"（0）。
>
> 因此行为随配置分三档：**不填**（默认 0）⇒ 消息数轴（与今天一致）；**填真实窗口**（如 256k）
> ⇒ 按窗口压力折（pi 式，实测你最大的组 253,735 已在线）；**填 1M** ⇒ 等于显式选择不按 token 折。

**挂载点两处（5B）**：
- **(a) task 完成时** —— 现成（`sched_apply_advance` 尾部 984/990），只换判据；
- **(b) 每次 LLM 请求前** —— 在调度扫描的 **dispatch 之前**再跑一次同一个检查：正要派发的那个
  READY task 会被挂上折叠 dep ⇒ 这一轮不发请求、park 到 `AWAIT_DEPS`（**复用 B1 的机制，零新机制**）。

`context_window <= 0`（未知）⇒ 只走消息数。

## 7.3 切点（裁决 7b / 13）

从**最新往最老**累加，累到 `keep_recent = 20000` token 为止，**切点必须是合法消息边界**（不能切在
`tool` 消息上 —— 复用 `edc_llm_history_safe_start` 的同款回退；粒度到消息，**可以切在一个 turn 内部**
= pi 的 split turn）。比 `keep_recent` 更老的全部进折叠语料。

> ⚠️ **行为变更**：今天"折最老一半"（保守，留一半细节）退休 —— 新规则下**一次折到只剩近期 ~20k**。
> 消息数触发时这比以前激进得多（200 条消息可能一把折掉大半）。
>
> ⚠️ **折叠请求自己也必须放得下**：折叠的 prompt ≈ "整个语料 + 指令 + 旧记忆"，比普通请求还大
> （实测已到 215k）。`reserve = 16k` 正好覆盖"指令开销 + 摘要输出上限 13k"，所以切点按
> **真值窗口**算就能保证 `语料 + 开销 + 输出 ≤ 窗口`；但**消息数轴**触发时不受窗口约束 ⇒
> 语料可能贴顶（今天就是这个状态）—— 这是 P2 消掉的隐患，也是 P3 兜不住时唯一的保险。

## 7.4 摘要（裁决 8 / 13）

- **格式**：pi 的结构化 6 段（`Goal / Constraints & Preferences / Progress(Done·In Progress·Blocked) /
  Key Decisions / Next Steps / Critical Context`），要求保留**精确路径、函数名、错误原文**；
- **迭代**：带旧摘要 = "更新它"（保留旧信息、In Progress→Done、Next Steps 重写），不是重写；
- **上限**：请求体第一次带上 `max_tokens = min(0.8 × reserve, 模型上限)`（今天**从不发**）；
- **手动**：`/group fold [id] [focus]`，`focus` 作为额外指令进 prompt；
- **卫生**（失败 = **什么都不改**，记忆与水位原样，等下一轮）：内容空 / 只有空白 /
  `finish_reason ∈ {length, context_length_exceeded, network_error, unknown}` / 响应里带 tool_calls
  ⇒ 不写记忆、不推水位。为此把 finish reason 从 executor 透到 `edc_advance_result`（新字段），
  再传给 `edc_group_fold_deliver_memory(task, content, reason)`。

> 顺带：`BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED` 词表**已经存在**（`edc_exec_io.h:36`）——
> P3 的溢出识别不用新造词。

## 7.5 schema v6 + 迁移

水位换了表示 ⇒ 存档要升 **v6**（`EDC_STATE_SCHEMA_VERSION`），并给 `tools/migrate_session_v5_to_v6.py`：
v5 的 `group.folded_through_completion_seq` + `task.turns_folded` → 每个 turn 的 `folded_prefix = 消息数`
（这些内容**已经在记忆里**）。不迁移的后果写清楚：老存档会被当成"全都没折过" ⇒ 已折内容再发一遍
（并再次进摘要），token 白烧。

## 7.6 判据

**单测（新/改）**
1. 触发谓词：token 超线 / 消息数超线 / OR / `context_window=0` 只看消息数 / reserve 边界（等于不加不减）；
2. 切点：保留 ≈ `keep_recent`、**不切在 `tool` 上**、**切在 turn 内部**（split turn）、语料 = 更老的全部；
3. 水位：`folded_prefix` 落盘/回读；请求跳过被折前缀；**turn 提交只追加未折叠段**（这条是双份发送的回归）；
4. 卫生：空 / `length` / 带 tool_calls ⇒ 记忆与水位**逐字节不变**；
5. `max_tokens` 真的进了请求体（llm actor 侧断言）；
6. `/group fold <id> <focus>` 解析 + focus 进 prompt；
7. 加载重建 + 迁移：v5 存档迁到 v6 后**同一个 task 收到的请求内容与迁移前一致**（请求逐条对拍）。

**真生产者用例**：`test_group_fold.c` 的 sched+session 夹具跑到端到端（超线 → 起折 → 落地 →
记忆变、水位推进、**下一个请求确实变小**）；`test_session_snapshot` 侧验 ctx 与触发一致。

**变异（各自必须精确变红）**：切点忽略 `tool` 边界 / 切点忽略 keep_recent / 触发漏掉 token 支 /
触发漏掉消息数支 / 落地不剪 history / 提交时不跳前缀（**双份发送**）/ 卫生放行空摘要 /
卫生放行 `length` / `max_tokens` 不发 / `fold_epoch` 不 bump（P1 已有）/ fork 切片不看前缀。

## 7.7 改动面（预计）

| 包 | 文件 | 改什么 |
|---|---|---|
| common | `edc_config.{c,h}`、`edc_init_env.{c,h}` | `context_window`（默认 1M，0=未知）；`EDC_FOLD_RESERVE_TOKENS`/`KEEP_RECENT_TOKENS` 常量 |
| kernel/task | `edc_turn.{c,h}`、`edc_task.{c,h}`、`edc_task_group.{c,h}` | `folded_prefix` + 落盘；删 `turns_folded`/水位/两个计数；`is_foldable` 重写 |
| kernel/context | `edc_group_fold.{c,h}` | 触发谓词、尾部预算切点、6 段 + 迭代 prompt、卫生、focus |
| kernel/scheduler | `edc_scheduler.c` | 触发挂载点 (b)（dispatch 前）；把 finish reason 透给 `deliver_memory` |
| kernel/executor | `edc_executor.{c,h}` | turn 提交只追加未折叠段；`edc_advance_result` 带 finish reason |
| kernel/session | `edc_state_serializer.c` | v6；重建按 `folded_prefix` 剪枝；迁移脚本 |
| actor | `edc_llm_request.{c,h}`、`edc_llm_actor.{c,h}` | history 切片去掉水位、turn 从前缀起、`max_tokens` |
| shell | `edc_commands.c` | fork 切片；`/group fold [id] [focus]` |
| tests | `test_group_fold.c`(大改)、`test_state_serializer.c`、`test_context_estimate.c`、`test_group_ctx.c`、`test_llm_actor.c`、`test_cmd_parse.c` | 见 7.6 |

## 7.8 分步（两步各自可验，最后合成 commit）

- **P2a = 骨架**：配置键 + 水位表示（`folded_prefix` / 剪枝 / 提交交接 / v6 迁移）+ 触发（两挂载点、OR）+ 切点（尾部预算）。
- **P2b = 摘要**：6 段 + 迭代 + 卫生 + `max_tokens` + `/group fold`。
  - **已落地**：`kFoldSystemPrompt`（pi 口径：context summarization assistant + 禁止续写/答问/工具）、
    首次 `kInitialSummaryInstruction`（pi §8 六段 + "保留路径/函数名/错误原文"）与迭代
    `kUpdateSummaryInstruction`（增量合并、In Progress→Done、"输出完整摘要不是 diff"）；
    拼法 `<group-transcript>` + `[<previous-summary>]` + 指令 + `[Additional focus: …]`；
    落地仍是**替换** ⇒ 迭代语义天然成立。
  - `req.max_tokens`（0 = 不发）**只给 `sink == GROUP_MEMORY` 装**（`EDC_FOLD_SUMMARY_MAX_TOKENS`
    = min(0.8 x reserve, …) = 13107）⇒ 全仓唯一发 `max_tokens` 的请求；响应侧新增
    `EDC_REASON_LENGTH`，折叠任务的截断提示直说"discarded, the group memory is unchanged"。
  - 手动 `/group fold [id] [focus]`（`edc_scheduler_fold_group_now`）**越过 due 判据、屏障不松**；
    无参 = 前台组，首个 token = 组 id（跨 session），其余 = focus。
  - split-turn 段（pi §5）：`---` + `**Turn Context (split turn):**` + Original Request / Early
    Progress / Context for Suffix；判定 = 切点落在 turn 内部**或**语料尾部是还在飞的 turn。
    **与 pi 的差别**：pi 为 turn 前缀发第二次调用（专用 prompt、0.5 x reserve），我们单次调用共用
    0.8 x reserve 上限。
  - **验证**：`bazel clean` → 全量 **62/62**；12 条变异全红（迭代/六段/focus/previous-summary/
    装上限/写字段/length 映射/忽略 id/手动也判 due/丢 focus/split 段两个方向）；A/B `smoke`/`cancel`
    `compare: 一致`，`fold` 的差异**逐条取证**（请求体只多 `body.max_tokens=13107` + 两条消息文本；
    `session.json` 5 处时钟 + 1 处折叠 prompt 变长；日志新增一条 `split turn at the cut`）。

## 7.9 待点头（4 条）

1. ~~6A 的读法~~ → **已定案：写法乙**（窗口优先；缺失/0 才用条数）+ **默认值必须为 0**（见 §7.2）；
2. **水位落在 turn 上 + group history 剪枝**（7.1 的骨架）—— 这是 7a/7b 成立的前提；
3. **三个数字走常量**（16k / 20k / `min(0.8×16k, …)`）而不是配置键；
4. **P2 拆两步（P2a / P2b）**，各自验证；commit 按你的规矩来（合成一个也行）。

## §2.10 系统提示的确定性纪律（2026-10-10）

参考 `docs/pi-system-prompt-guide.md` §7 的三条"无"：**无日期/时间戳注入**（要时间自己跑 `date`）、
**无文件监听**（`/reload` 才重读）、**无环境漂移**（版本号/遥测/网络状态）。判据是
"跨天、重启、resume，prompt 字节完全不变"。

拿它量 EdgeClaw 的 `edc_context_build_system_prompt`（六段：身份头 / 权限表 / bootstrap /
全局记忆 / 技能 / session 记忆常量提示）：

| 段落 | 判定 |
|---|---|
| **`## Current Time`（时间戳）** | ❌ 违反第一条 ⇒ **已删**（连 `{{time}}` 占位符与那个参数一起清掉；身份头补一句常量行："这个提示词没有时钟，要时间就跑 `date`"） |
| `## Current Running Directory` / OS / workspace | ✅ 会话内常量（pi 自己也有 `<cwd>`） |
| 权限表（sandbox 路径） | ✅ 会话配置；只在 config / 绑新 workdir 时变（那是会话事件，pi §7 的"结构性变化 ⇒ 新前缀"） |
| bootstrap（`AGENTS.md` / `TOOLS.md` / 可选 `IDENTITY.md`） | ✅ 与 pi 的 `<project_context>` 同性质：会话事件时读一次。**`SOUL.md` / `USER.md` 已删**（见 §2.12） |
| 全局记忆 `MEMORY.md` | ✅ **2026-10-10 整套删除**（见 §2.13）：正文先移出提示词，随后连机制一起退休 —— 它是 agent 自己会写、且没有渐进式披露/去重/通知的文件 |
| 技能清单 | ✅ pi `<skills>` 同款（只放索引，正文惰性读） |
| session 记忆常量提示 | ✅ 常量（无条数、无内容） |

守卫：`tests/test_session_workdir.c::test_system_prompt_has_no_clock` —— 连续构造两次逐字节相同、
没有 `Current Time` 段、没有未替换的 `{{time}}`、全文不含 `20xx-` 形式的日期。
（`{{skill-name}}` 是**故意**留给模型看的路径模板，不算漏替换。）
以及 `tests/test_session_workdir.c::test_memory_file_mechanism_is_gone` —— 造一份带标记的 MEMORY.md，
断言提示词里**既没有正文、也不提**这两个文件名，而 `memory_write` 的路子还在。
（**后续**：这一步之后的同一天，整个 `MEMORY.md` 机制被删除了 —— 见 §2.13。）

把 MEMORY.md 移出提示词还顺带修掉一个语义毛病：原来它常驻（缓存在 `history[0]`）⇒ agent 本次会话
写进去的记忆，**它自己反而读不到最新版**（要等重建）。

## §2.11 上下文文件的发现机制（2026-10-10）

**缺口**：在那之前，提示词只从 **workspace** 里读 5 个固定名字的文件（`AGENTS.md` / `SOUL.md` /
`USER.md` / `TOOLS.md` / `IDENTITY.md`）。而 session 是可以绑定工作目录的
（`/session new x /path/to/repo`）—— **那个 repo 自己的 `AGENTS.md` 一个字都进不来**。

照 pi §3.6 补齐（`edc_context.c` 的 `load_project_context`）：

| pi 的做法 | 现在 |
|---|---|
| 用户全局 `~/.pi/agent/AGENTS.md` | ✅ `~/.edgeclaw/AGENTS.md` |
| 从 cwd **向上遍历到根** | ✅ 从 session 的**工作目录**向上遍历到根 |
| 同目录候选优先级 `AGENTS.override.md` > `AGENTS.md` > `AGENTS.MD` > `CLAUDE.md` > `CLAUDE.MD`（override 只压制同目录） | ✅ 同一条优先级，同一目录只取一个 |
| 每个文件包 `<project_instructions path="…">` | ✅ 同款包裹（外加 `## Project Context` 标题与 pi 那句引导语） |
| 一个都没命中 ⇒ 整段不出现 | ✅ 返回空串，段不出现 |
| `--no-context-files` / `-nc` 禁用 | ✅ config.json 顶层键 **`skip_context_files`**（缺省 false） |
| 附着顺序：全局 → 外层 → 内层 | ✅ 先全局、再 根 → 工作目录（先广后专） |

**有意保留的差别**：workspace 的那 5 个文件仍照旧躺在最前面（它们是"agent 自己的家目录规则"，
pi 没有这一层）；工作目录就是 workspace 时那 5 个文件会被**去重**，不会收两遍。

**守卫**：`tests/test_session_workdir.c::test_project_context_discovery` —— 造一棵
`home/.edgeclaw/AGENTS.md` + `proj/AGENTS.md` + `proj/sub/{AGENTS.override.md, AGENTS.md, CLAUDE.md}`
的树，断言：全局/项目/子目录 override 都进来、被 override 压制的与 `CLAUDE.md` 都不进来、
顺序是 全局 → 项目 → 子目录、工作目录为空时只收全局、`skip_context_files` 一开一份都不读。

## §2.12 砍掉 SOUL.md / USER.md / HEARTBEAT.md（2026-10-10）

被问："pi 是不是都没有这部分内容？" —— **是，pi 一个都没有**。逐条对照后删掉：

| 文件 | 原内容 | 为什么删 | pi 里对应什么 |
|---|---|---|---|
| `SOUL.md` | Core Principles / Execution Rules（单步多步、错误恢复、二进制调试、文件管理）/ Task Management / Tool Catalog / Channels | **绝大部分是重复**：Task Management 与 `todowrite` 的工具描述重复；Tool Catalog 与 `TOOLS.md` 重复；二进制调试与 `dtbo-debug` 技能重复；原则与 `AGENTS.md` 的 Guidelines 重复 | 无（pi 的身份是**代码里的 preamble 常量**） |
| `USER.md` | 一份**全空模板**（`(your name)`、未勾选的复选框、空列表项） | 没有任何真实字段 ⇒ 纯噪声，占 token 还误导 | 无（要个性化就写进 AGENTS.md / `--append-system-prompt`） |
| `HEARTBEAT.md` | 周期性任务清单 | **本仓库没有任何代码实现"每 30 分钟检查"** —— 那条声称只写在 `AGENTS.md` / `TOOLS.md` / `README.md` 里，机制不在这个仓库 | 无（pi 没有 heartbeat；要周期任务用 cron/外部调度） |

**改动面**（一次做干净，不留悬空引用）：
- `src/kernel/context/edc_context.c`：`BOOTSTRAP_FILES` = `AGENTS.md` / `TOOLS.md` / `IDENTITY.md`；
- 种子工作区 `workspace/`：删这三个文件；`AGENTS.md` / `TOOLS.md` 里那两段 heartbeat 说明一并摘掉；
- 引用同步：`tests/test_bundled_workspace.c` 的必需文件名单、`tools/ab/ab.py` 的 `BOOTSTRAP` 名单、`README.md`（目录树 / Heartbeat 段落 / Workspace 文件表）；
- 已有安装的 `~/.edgeclaw/workspace/`：那两份与种子**逐字节相同** ⇒ 同样摘掉 heartbeat 段后与种子再一致（不丢用户内容）。

**用户自己那份也删了**（2026-10-10 裁决）：`~/.edgeclaw/workspace/HEARTBEAT.md` 里那条"时钟哨兵"
任务一并丢掉 —— 反正本仓库没有任何东西会去执行它（要周期任务，用真实现的 `crontab` 工具）。

**仍然保留的**：`AGENTS.md`（行为契约）、`TOOLS.md`（工具速查）—— 它们与 pi 的 `<project_context>` 同类（人写、会话事件时读一次）。`TOOLS.md` 与 provider 的 tools 字段确有部分重复，但它是"给人看的清单 + 额外说明（设备、通道）"，暂留。

## §2.13 删掉 MEMORY.md 机制（2026-10-10）

**裁决**：文件式的长期记忆（`workspace/memory/MEMORY.md` + `HISTORY.md`）**整套删除**；跨 session 记忆
"后面再设计一个更好的机制"（要求：像 session 条目那样有 id / 检索 / 更新纪律 / 通知）。

**为什么它该走**（与 session 条目逐条对比）：

| | `MEMORY.md`（旧） | session 条目（新） |
|---|---|---|
| 存放 | 一个平面文件（实测 840 行 / 116 KB ≈ 3 万 token） | 条目 + 事件表（id / title / summary / detail） |
| 取用 | `read_file` **只能整读**（没有 offset/limit）⇒ 无渐进式披露 | `memory_search` / `memory_list` / `memory_get` |
| 写入 | agent 直接改写整个文件（无合并/去重/校验） | `memory_write` + 策展 ops（整批校验、fail-closed） |
| 通知 | 无 | 队尾通知（按水位补差集） |
| 唯一独占能力 | **跨 session**（这一条以后要用新机制补回来） | session 内 |

**改动面**：
- `edc_context.c` 身份头：删掉 `MEMORY.md` / `HISTORY.md` 两条路径 + "长期记忆在那些文件里…写回 MEMORY.md"
  两段常量说明（提示词里现在**完全不提**这两个文件）；
- 种子工作区：删 `workspace/memory/MEMORY.md`；删 `workspace/skills/memory/`（那个 skill 实现的就是这套
  文件记忆的 grep 检索流程）；
- `workspace/AGENTS.md` 的 "## Memory System" 改成 "## Memory"（只讲 session 记忆的四个工具 + 明说
  跨 session 还没做）；
- 引用同步：`tests/test_bundled_workspace.c` 名单、`README.md`、`docs/edgeclaw.md`、`docs/edgeclaw.1`、
  `docs/runtime-model.md`、`docs/l2-session-memory.md`（含台账 ⑯）。

**守卫**：`tests/test_session_workdir.c::test_memory_file_mechanism_is_gone` —— 造一份带标记的
`MEMORY.md`，断言提示词里**既没有正文、也不提 `MEMORY.md`/`HISTORY.md`**，而 `memory_write` 的路子还在。

**用户数据**：已有的 `~/.edgeclaw/workspace/memory/MEMORY.md`（840 行）与 `HISTORY.md` **内容保留在盘上**
（机制没了 = 没人再读写它，但那些是攒出来的知识，删不删由用户决定）。

