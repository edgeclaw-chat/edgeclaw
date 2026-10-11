# Pi 上下文压缩机制文档

> 基于 pi 源码仓库整理。源码依据：
> - `packages/coding-agent/src/core/compaction/compaction.ts`（1024 行，压缩算法核心）
> - `packages/coding-agent/src/core/compaction/branch-summarization.ts`（分支摘要）
> - `packages/coding-agent/src/core/compaction/utils.ts`（共享工具）
> - `packages/coding-agent/src/core/agent-session.ts`（触发逻辑，`_checkCompaction` / `_runAutoCompaction`）
> - `packages/ai/src/utils/overflow.ts`（溢出检测）
> - 官方文档：`packages/coding-agent/docs/compaction.md`

---

## 目录

1. [总览](#1-总览)
2. [触发时机](#2-触发时机)
3. [压缩算法五步](#3-压缩算法五步)
4. [切点规则：为什么永不切 toolResult](#4-切点规则为什么永不切-toolresult)
5. [Split Turn 处理](#5-split-turn-处理)
6. [上下文重建与持久化](#6-上下文重建与持久化)
7. [分支摘要（Branch Summarization）](#7-分支摘要branch-summarization)
8. [摘要格式](#8-摘要格式)
9. [与 prompt cache 的关系](#9-与-prompt-cache-的关系)
10. [扩展拦截点](#10-扩展拦截点)
11. [设置项](#11-设置项)
12. [完整流程图](#12-完整流程图)

---

## 1. 总览

pi 有两个摘要机制，共享同一套摘要生成代码：

| 机制 | 触发 | 目的 |
|------|------|------|
| **Compaction** | 阈值 / 溢出恢复 / 手动 `/compact` | 压缩上下文释放窗口 |
| **Branch Summarization** | `/tree` 切分支 | 保留被放弃路径的上下文 |

核心原则：**压缩有损，但历史无损**。完整记录永远留在 JSONL 会话文件里，压缩只改变"发给 LLM 的上下文视图"，`/tree` 可随时回看被压缩前的内容。

## 2. 触发时机

### 2.1 阈值触发（主动，最常见）

```
contextTokens > contextWindow - reserveTokens（默认 16384）
```

`shouldCompact()`（compaction.ts:250）只做这一个判断。

**token 估算方法**（`estimateContextTokens`，compaction.ts:217）：

- 优先取**最后一条有效 assistant 消息的真实 usage**（`totalTokens`），其后的尾部消息按 chars/4 估算累加
- 没有 usage 时（会话开头 / 全部无效），对全部消息做 chars/4 估算
- 有效性判断（`getAssistantUsage`）：跳过 `stopReason` 为 aborted/error 的消息和全零 usage 的消息
- 图片按 4800 字符估算（约 1200 token）

**三个检查挂载点**（agent-session.ts）：

| 挂载点 | 位置 | 说明 |
|--------|------|------|
| turn 之间 | `prepareNextTurnWithContext` → `_compactBeforeNextAssistantResponse`（:546） | 工具结果进来后、下一个 assistant 响应前。**主要路径**。工具批次终止了 run 且没有排队消息时跳过 |
| 新 prompt 前 | `prompt()` 内检查上一条 assistant 消息（:1337） | 捕获被中止的响应遗留的上下文压力 |
| run 结束后 | `_handlePostAgentRun` → `_checkCompaction`（:1212） | 低层 run 结束后的兜底 |

### 2.2 溢出恢复触发（被动）

`_checkCompaction`（agent-session.ts:2227）检测三类溢出，判定逻辑在 `ai/src/utils/overflow.ts`：

| 类型 | 判定 | 厂商场景 |
|------|------|---------|
| **错误匹配** | `stopReason: "error"` 且 errorMessage 命中约 25 种厂商正则 | Anthropic "prompt is too long"、OpenAI "exceeds the context window"、llama.cpp、Groq、Kimi 等 |
| **静默溢出** | 请求成功（stop）但 `input + cacheRead > contextWindow` | z.ai 不报错静默接受 |
| **截断溢出** | `stopReason: "length"` + output=0 + input ≥ 99% 窗口 | 小米 MiMo 截断输入填满窗口 |

另有排除规则：匹配 `rate limit`/`too many requests` 等限流正则的**不算**溢出（防止 Bedrock 的 "ThrottlingException: Too many tokens" 误报）。

**恢复流程**（agent-session.ts:2289-2296）：

1. 从 agent state **移除失败的 assistant 消息**（会话文件保留，重试上下文排除）
2. 执行压缩
3. `agent.continue()` **重试一次**（`_overflowRecoveryAttempted` 标志保证只试一次，再失败报错："Try reducing context or switching to a larger-context model"）

**防护逻辑**：

- **换模型豁免**：assistant 消息来自与当前不同的模型时不触发（从小窗口模型切到大窗口模型后，旧模型的溢出错误不该触发新模型压缩）
- **stale usage 豁免**：assistant 消息时间早于最近一次压缩 entry 时不触发（防止压缩刚完成后的旧 usage 反复触发）
- **错误/零 usage 兜底**：API 持续 529 等错误或畸形零 usage 响应时，回退到消息估算，保证会话仍能压缩

### 2.3 手动触发

`/compact [instructions]`，可选自定义聚焦指令（追加到摘要 prompt 的 "Additional focus"）。

### 2.4 分支摘要触发

`/tree` 导航到不同分支时，对被放弃的分支生成摘要注入新分支。见第 7 节。

## 3. 压缩算法五步

### 第 1 步：找切点（`findCutPoint`，compaction.ts:418）

从最新消息**往回**累加估算 token，直到 ≥ `keepRecentTokens`（默认 20000），取该位置**向后最近的合法切点**：

```typescript
// compaction.ts:448
cutIndex = cutPoints.find((candidate) => candidate >= i) ?? cutPoints[cutPoints.length - 1];
```

注意方向性 `candidate >= i`：预算恰好耗尽在 toolResult 上时，**向前多保留几条**找到它前面的 assistant——宁可少压缩，不产生孤儿 toolResult。

切点合法性见第 4 节。

### 第 2 步：确定摘要范围（`prepareCompaction`，compaction.ts:762）

关键规则：**重复压缩时，摘要范围从上一次压缩的 `firstKeptEntryId` 开始**，而非从上一个压缩 entry 开始：

- 上次压缩幸存（kept）的消息这次也被纳入摘要 → 无信息静默丢失
- 文件追踪（readFiles/modifiedFiles）持续累积

边界处理：

- 当前 leaf 本身就是 compaction entry → 无需压缩，返回 undefined
- `tokensBefore` 从重建的会话上下文重新计算（反映真实被替换的规模）
- 无任何可摘要消息 → 返回 undefined

### 第 3 步：生成摘要（`generateSummaryWithUsage`，compaction.ts:668）

一次**独立 one-shot LLM 调用**（非对话流）。请求结构：

```
系统: "You are a context summarization assistant. Your task is to read a
       conversation... Do NOT continue the conversation. Do NOT respond to
       any questions in the conversation. ONLY output the structured summary."

用户: <conversation>
      {serializeConversation(convertToLlm(messages))}
      </conversation>

      [<previous-summary>          ← 仅迭代更新模式（有旧摘要时）
      {旧摘要}
      </previous-summary>]

      {SUMMARIZATION_PROMPT}        ← 首次
      或 UPDATE_SUMMARIZATION_PROMPT  ← 迭代更新
```

要点：

- **首次 vs 迭代**：无旧摘要用 `SUMMARIZATION_PROMPT`（完整结构化格式）；有旧摘要用 `UPDATE_SUMMARIZATION_PROMPT`（规则：保留所有已有信息、增量添加、In Progress → Done 迁移、更新 Next Steps）
- `maxTokens = min(0.8 × reserveTokens, model.maxTokens)`
- **序列化保险**：消息先经 `convertToLlm` 过滤 UI-only 消息（bashExecution/custom 等转为 user 消息文本），再序列化——防模型把对话"续写"下去
- **缓存策略**：`cacheRetention: "none"` + 新鲜 session ID（`uuidv7`）——一次性请求不值得写缓存
- **重试**：`retryAssistantCall` 包裹（与主对话同一 `settings.retry` 预算），流中断不致整个压缩失败
- **失败判定**（`getSummarizationFailure`）：`stopReason: "error"` 或 `"length"`（截断的摘要不能作为 checkpoint）；响应含 toolCall 也判失败（"Summarization attempted to call a tool"）
- **自定义指令**：`/compact <instructions>` 的文本以 "Additional focus: ..." 追加

### 第 4 步：文件操作追踪

从被摘要消息的 tool call（read/write/edit/bash 参数）中提取文件操作，与上一次压缩 `details.readFiles/modifiedFiles` **累积合并**：

- 追踪是**累积的**：跨多次压缩保留完整文件历史
- 以 `<read-files>`/`<modified-files>` XML 标签追加到摘要文本尾部
- 同时存入 `CompactionEntry.details`（扩展自定义压缩时可放任意 JSON）

### 第 5 步：持久化与重建

```typescript
// agent-session.ts:2452-2456
sessionManager.appendCompaction(summary, firstKeptEntryId, tokensBefore, details, fromExtension, usage);
this.agent.state.messages = sessionManager.buildSessionContext().messages;  // 重建
```

写入 `CompactionEntry` 后，`buildSessionContext()` 按压缩边界重建上下文（见第 6 节）。溢出场景下再走 `agent.continue()` 重试。

## 4. 切点规则：为什么永不切 toolResult

**规则**：压缩切点（`firstKeptEntryId`）不能落在 toolResult 上。

**原因**：工具调用是 provider API 的严格配对协议：

```
[assistant]  content: [..., {type:"toolCall", id:"call_123", ...}]
[toolResult] toolCallId:"call_123"   ← 必须紧跟其后
```

- Anthropic：`tool_result` 必须出现在紧随 `tool_use` 的 user 消息里
- OpenAI：`role:"tool"` 消息必须引用之前 assistant 的 `tool_call_id`
- 违反即 API 拒绝请求

**如果切在 toolResult 上**：它引用的 toolCall 已被摘要吃掉 → 孤儿 toolResult → API 报错，整个请求失败：

```
错误示范（切点产生孤儿）:
  ... [assistant(toolCall 123)] [toolResult(123)] [assistant] ...
                                  ↑ 切这里
  压缩后: [system] [summary] [toolResult(123)] ...   ← 引用不存在的调用，API 拒绝

正确（切在配对前面）:
  ... [assistant(toolCall 123)] [toolResult(123)] [assistant] ...
       ↑ 切这里
  压缩后: [system] [summary] [assistant(toolCall 123)] [toolResult(123)] ...  ← 配对完整
```

**源码实现**——`isCutPointMessage`（compaction.ts:323）白名单：

```typescript
function isCutPointMessage(message: AgentMessage): boolean {
    switch (message.role) {
        case "user":
        case "assistant":          // ✅ 可切（切在带 toolCall 的 assistant 上，其 toolResult 自然跟随保留）
        case "bashExecution":
        case "custom":
        case "branchSummary":
        case "compactionSummary":
            return true;
        case "toolResult":         // ❌ 唯一被排除的上下文角色
            return false;
    }
    return false;
}
```

**双层防御**：压缩算法层保证不产生孤儿（主动）；请求转换层（`transformMessages`）给孤儿 toolCall 合成假 toolResult（`"No result provided"`, `isError: true`）兜底。孤儿 toolResult 没有兜底手段（无法凭空造调用它的 assistant 消息），所以只能源头禁止。

**turn 边界判定**（`isTurnStartMessage`）：user / bashExecution / custom / branchSummary / compactionSummary 开启新 turn；assistant 和 toolResult 属于当前 turn。切点落在 turn 中间的 assistant 上即 split turn（见第 5 节）。

## 5. Split Turn 处理

单个 turn 自身超过 `keepRecentTokens` 时，切点落在 turn 中部的 assistant 消息上：

```
entry:  0     1     2      3     4      5      6     7      8
      ┌─────┬─────┬─────┬──────┬─────┬──────┬──────┬─────┬──────┐
      │ hdr │ usr │ ass │ tool │ ass │ tool │ tool │ ass │ tool │
      └─────┴─────┴─────┴──────┴─────┴──────┴──────┴─────┴──────┘
              ↑                                     ↑
       turnStartIndex = 1                  firstKeptEntryId = 7
              │                                     │
              └──── turnPrefixMessages (1-6) ───────┘
                                                    └── kept (7-8)

isSplitTurn = true
messagesToSummarize = []   （前面没有完整 turn）
turnPrefixMessages = [usr, ass, tool, ass, tool, tool]
```

处理：生成**两段摘要再合并**（`compact()`，compaction.ts:899-938）：

1. **History summary**：之前的完整 turn（若存在，且可迭代更新旧摘要）
2. **Turn prefix summary**：被切 turn 的前半部分，用专门的 `TURN_PREFIX_SUMMARIZATION_PROMPT`（聚焦"Original Request / Early Progress / Context for Suffix"），预算是 0.5 × reserveTokens（正常摘要为 0.8 ×）

合并格式：

```
{history summary}

---

**Turn Context (split turn):**

{turn prefix summary}
```

## 6. 上下文重建与持久化

### CompactionEntry 结构

```typescript
interface CompactionEntry<T = unknown> {
    type: "compaction";
    id: string;
    parentId: string;
    timestamp: string;
    summary: string;
    firstKeptEntryId: string;   // 保留区起点
    tokensBefore: number;
    usage?: Usage;              // 摘要 LLM 调用的 usage（计入会话成本统计）
    fromHook?: boolean;         // true = 扩展提供
    details?: T;                // 默认: { readFiles: string[], modifiedFiles: string[] }
}
```

### 重建规则（`buildContextEntries`）

从 leaf 走到 root，若路径上有 CompactionEntry 取**最新一个**：

```
  entry:  0     1     2     3      4     5     6      7      8     9     10
        ┌─────┬─────┬─────┬──────┬─────┬─────┬──────┬──────┬─────┬─────┬─────┐
        │ hdr │ usr │ ass │ tool │ usr │ ass │ tool │ tool │ ass │tool │ cmp │
        └─────┴─────┴─────┴──────┴─────┴─────┴──────┴──────┴─────┴─────┴─────┘
               └──────────┬──────┘ └──────────────────────┬───────────────────┘
                 不再发给 LLM                       发给 LLM
                                    ↑ firstKeptEntryId（entry 4）

LLM 实际看到:
  ┌────────┬─────────┬─────┬─────┬──────┬──────┬─────┬──────┐
  │ system │ summary │ usr │ ass │ tool │ tool │ ass │ tool │
  └────────┴─────────┴─────┴─────┴──────┴──────┴─────┴──────┘
```

- 摘要替换 `firstKeptEntryId` **之前**的所有内容；该 entry **起到**压缩 entry 之间的内容保留
- 压缩 entry 里的 `systemMessage`（完整 prompt + tool 检查点）成为压缩后上下文的 leading system message；保留区里的旧 system 消息被丢弃（其状态已折叠进检查点）
- 重复压缩时新 entry 按树结构 append，`firstKeptEntryId` 指向上次保留区起点

## 7. 分支摘要（Branch Summarization）

### 触发

`/tree` 导航到不同分支时，pi 询问是否摘要被放弃的工作。

### 流程

```
导航前:
         ┌─ B ─ C ─ D （旧 leaf，被放弃）
    A ───┤
         └─ E ─ F （目标）

公共祖先: A
待摘要: B, C, D

导航后:
         ┌─ B ─ C ─ D
    A ───┤
         └─ E ─ F ─ [B,C,D 的摘要] （新 leaf）
```

1. **找公共祖先**：新旧位置的最深共享节点
2. **收集 entry**：从旧 leaf 回走到公共祖先
3. **预算裁剪**：按 token 预算（`branchSummary.reserveTokens`，默认 16k）从最新往前保留
4. **生成摘要**：与 compaction 相同的结构化格式 + 累积文件追踪
5. **落盘**：`BranchSummaryEntry`（含 `fromId` 指向被放弃的 leaf），以 user 可见消息注入新分支上下文

### BranchSummaryEntry 结构

```typescript
interface BranchSummaryEntry<T = unknown> {
    type: "branch_summary";
    id: string;
    parentId: string;        // 新分支的挂接点
    timestamp: string;
    summary: string;
    fromId: string;          // 被放弃的 leaf
    usage?: Usage;
    fromHook?: boolean;
    details?: T;             // { readFiles, modifiedFiles }
}
```

## 8. 摘要格式

两种机制共用同一结构（`SUMMARIZATION_PROMPT`）：

```markdown
## Goal
[用户想达成什么，可多项]

## Constraints & Preferences
- [用户提到的约束/偏好/要求]
- [无则 "(none)"]

## Progress
### Done
- [x] [已完成任务/变更]

### In Progress
- [ ] [当前工作]

### Blocked
- [阻碍进度的问题，若有]

## Key Decisions
- **[决策]**: [简要理由]

## Next Steps
1. [有序的下一步]

## Critical Context
- [继续工作所需的数据/示例/引用]
- [不适用则 "(none)"]

<read-files>
path/to/file1.ts
</read-files>

<modified-files>
path/to/changed.ts
</modified-files>
```

强制要求："Keep each section concise. **Preserve exact file paths, function names, and error messages.**"

## 9. 与 prompt cache 的关系

### 压缩 = 一次有界的缓存重置

Prompt cache 是前缀匹配。压缩后前缀从 summary 处改变，即使保留的 20k 尾部逐字节不变也全部 miss：

```
压缩前: [system, msg1, ..., msgN]        ← 已缓存
压缩后: [system', summary, msgK, ..., msgN]  ← 前缀变了，全 miss
```

### 关键设计

| 设计 | 效果 |
|------|------|
| `keepRecentTokens = 20000` | **重置成本有界**：不管之前膨胀到多大，压缩后全量重写的只有 ~22k（summary + kept） |
| 摘要调用 `cacheRetention: "none"` + 新鲜 session ID | 一次性请求不写缓存、不污染主会话缓存 |
| 主会话 `sessionId` 保持稳定（会话 UUID） | OpenAI `prompt_cache_key` 等路由稳定，重置后新基线写往确定的 shard |
| Anthropic tools 段独立 `cache_control` 断点 | 工具列表没变时 tools 段仍命中，miss 从 system 开始 |
| `cache-stats.ts` 逐 turn miss 归因 | 缓存浪费可见（missed tokens / 多付美元 / 空闲时长 / 是否换模型） |

### 同样会重置缓存的相关场景

- 换模型（`model_change`）
- 系统 prompt section 变更（diff 补丁仅在真实变化时落消息）
- `/tree` 分支切换 / branch summary 注入
- `PI_CACHE_RETENTION=long`（Anthropic 1h / OpenAI 24h）只解决过期，对前缀变化无效

## 10. 扩展拦截点

压缩流程发射的扩展事件（`agent-session.ts:2369`）：

### session_before_compact（压缩前）

```typescript
pi.on("session_before_compact", async (event) => {
    // event.preparation: { firstKeptEntryId, messagesToSummarize,
    //   turnPrefixMessages, isSplitTurn, tokensBefore, previousSummary, fileOps, settings }
    // event.reason: "manual" | "threshold" | "overflow"
    // event.willRetry: 溢出恢复是否将重试
    // event.signal: AbortSignal

    return {
        cancel: true,           // 取消压缩
        // 或完全接管:
        compaction: {
            summary: mySummary,
            firstKeptEntryId,   // 通常沿用 preparation 的
            tokensBefore,
            usage: response.usage,
            details: { /* 任意 JSON */ },
        },
    };
});
```

接管时 pi 跳过自己的摘要生成，但**不合并**默认文件追踪（扩展自己负责）。

### session_compact / session_compact_failed（压缩后）

成功时携带 `CompactionEntry` + `reason` + `willRetry`；失败时携带 `errorMessage` / `aborted` / `fromExtension`。

官方示例：`examples/extensions/custom-compaction.ts`（用 Gemini Flash 替代主模型做全量摘要）、`examples/extensions/trigger-compact.ts`。

## 11. 设置项

`/settings` 或 `settings.json`（可按模型覆盖）：

| 项 | 默认 | 含义 |
|----|------|------|
| `compaction.enabled` | `true` | 总开关 |
| `compaction.reserveTokens` | `16384` | 触发阈值的安全余量（为响应留空间）；也决定摘要 `maxTokens = 0.8 ×` |
| `compaction.keepRecentTokens` | `20000` | 保留尾部预算；也决定压缩后缓存重写的上限 |
| `branchSummary.reserveTokens` | `16384` | 分支摘要的 token 预算 |
| `branchSummary.skipPrompt` | `false` | `/tree` 导航时不询问直接摘要 |

## 12. 完整流程图

```
上下文增长
    │
    ├─ 阈值: tokens > window − 16k（turn 间 / 新 prompt 前 / run 结束后）
    ├─ 溢出: 错误正则 / 静默溢出 / length 截断
    │        └─ 失败消息出栈 → 压缩 → continue() 重试一次
    └─ 手动: /compact [instructions]
    │
    ▼
prepareCompaction
    │ 定位上次压缩边界（firstKeptEntryId 起）
    │ 从后往前累加 20k 预算找切点
    │ （候选切点永不包含 toolResult，防孤儿；
    │   预算耗尽在 toolResult 上则向前找它的 assistant）
    │
    ├── 普通情况 ──────────────┐
    │                          ▼
    │              generateSummaryWithUsage
    │              有旧摘要 → 迭代更新 prompt
    │              maxTokens = 0.8 × reserve
    │              cacheRetention: "none" + 新 session ID
    │              retryAssistantCall 包裹
    │
    └── split turn ────────────┐
        （单 turn > 20k）       ▼
          history summary（若有）
          + turn prefix summary（预算 0.5 × reserve）
          合并为一条
    │
    ▼
累积文件追踪（readFiles/modifiedFiles 追加到摘要尾部）
    │
    ▼
appendCompaction（JSONL 落盘，含 system prompt + tool 检查点）
    │
    ▼
buildSessionContext() 重建 agent 上下文
    │
    ▼
session_compact 事件 → (溢出场景) agent.continue() 重试
```

---

## 附：关键函数索引

| 函数 | 位置 | 职责 |
|------|------|------|
| `shouldCompact` | compaction.ts:250 | 阈值判断 |
| `calculateContextTokens` | compaction.ts:161 | usage → context tokens |
| `estimateContextTokens` | compaction.ts:217 | usage 优先 + 尾部估算 |
| `estimateTokens` | compaction.ts:281 | 单消息 chars/4 估算 |
| `isCutPointMessage` | compaction.ts:323 | 切点白名单（排除 toolResult） |
| `isTurnStartMessage` | compaction.ts:338 | turn 边界判定 |
| `findValidCutPoints` | compaction.ts:366 | 候选切点收集 |
| `findCutPoint` | compaction.ts:418 | 预算回溯选切点 |
| `findTurnStartIndex` | compaction.ts:384 | split turn 的 turn 起点 |
| `prepareCompaction` | compaction.ts:762 | 全量预计算（供扩展/压缩共用） |
| `generateSummaryWithUsage` | compaction.ts:668 | 摘要 LLM 调用 |
| `completeSummarization` | compaction.ts:591 | 共享 choke point（重试/缓存策略） |
| `generateTurnPrefixSummary` | compaction.ts:981 | split turn 前缀摘要 |
| `compact` | compaction.ts:870 | 主压缩（含 split 合并） |
| `extractFileOperations` | compaction.ts:55 | 累积文件追踪 |
| `isContextOverflow` | overflow.ts:135 | 三类溢出判定 |
| `isRecoverableLength` | overflow.ts:177 | length 截断可恢复判定 |
| `_checkCompaction` | agent-session.ts:2227 | 触发决策（阈值/溢出/防护） |
| `_runAutoCompaction` | agent-session.ts:2343 | 执行 + 扩展拦截 + 重建 |
