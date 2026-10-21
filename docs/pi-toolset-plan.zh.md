# 工具集设计（对齐 pi）：声明 / 散文 / 权限三层

> 基线：pi `4dd2b14`（`/tmp/pi-verify`，逐文件核实）；本文的实现落点按本仓 HEAD `44a5fb0` 的文件与结构体写。
> 上游口径：`docs/product-alignment.zh.md` 的 **R2**（工具注册面改造）。本文是 R2 的实作方案。
> 三条已拍板的口径：**散文段用方案 ②**（一行摘要 + 条件规则，不重复签名）；**单一真源**（注册表）；
> **声明 ≠ 权限**（dispatch 用 allowed 表，不用 declared 表）。

---

## 0. 决策记录

| # | 事项 | 定论 | 备注 |
|---|---|---|---|
| D0 | prose 段保留程度 | **② 一行 snippet + 条件规则** | 用户已拍板（2026-10-21）；不重复参数说明 |
| D1 | 冻结粒度 | **组级**（`group`） | 已定：与 history 同生命周期 |
| D2 | 缓存相关请求字段 | **`prompt_cache_key`**：只发给 `api.openai.com`（其他 OpenAI 兼容家可能拒未知参数）；**`cache_control` 暂不适用**——我们只会说 OpenAI chat-completions 格式，没有 Anthropic 原生 `/v1/messages` 路径 | 已定（`cache_control` 待 Anthropic 原生协议支持再补） |
| D3 | 越权调用的回应 | **结构化拒绝 + 可用列表**（`isError: true`）；"终止整轮"只留标记位 | 已定 |
| D4 | 移除类变更 | **重算声明集并接受一次缓存 miss**，同时追加一条带 reason 的 system message | 已定（对齐 pi 的 `hasNonAdditiveToolChanges`） |

**D5 总方针（2026-10-21 用户拍板）**：**照 pi 的实现走**，不发明第四档。
我们与 pi 的唯一必然差异是：**我们没有 Anthropic 的 mid-conversation tool changes beta**，
所以主路径固定是 pi 的**最后一档：永远发当前集**（`getCurrentTools`）。中间档（纯新增时冻结、靠消息锚定）的前提是"传输能在消息里新增工具声明"，OpenAI chat-completions 做不到——工具 schema 只能来自 `tools` 字段。

---

## 1. 不变式

- **I1 单一真源**：**只有两处**描述工具——请求的 `tools` 字段（签名）与 system prompt 的散文段（选择引导），都由注册表生成。任何手写清单、任何独立文档都视为缺陷。
- **I2 组内稳定**：同一 group 的正常任务在两次变更之间，请求体 `tools` 字段**字节级相同**。
- **I3 字段是契约，头部不可变**：`tools` 字段**始终等于当前声明集**（DIRECT 工具的增/删/改都跟着变）；system prompt 的 `history[0]` 在组的生命周期内**永不改写**，散文的更新以**追加一个 `<tools>` 段**表达（pi 的形状）。
- **I4 声明 ≠ 权限**：dispatch 按 `allowed`（per-task scope）判定；`declared` 只影响模型知道什么。
- **I5 可见**：缓存命中率、工具集变更（含原因）必须可见；变更引起的 miss 不静默。
- **I6 确定性构建**：散文段按固定顺序（注册名排序）生成，同一组状态第二次生成字符串**逐字节相等**（不读时钟、不依赖 hash 迭代顺序）。

---

## 2. 数据模型

### 2.1 注册表：`struct edc_tool` 增字段（`src/toolbox/edc_tools.h`）

沿用现有"新字段用 setter，不动 `edc_tool_new` 的 17 处调用"的风格（`edc_tool_set_abort` / `edc_tool_set_keep_full_output` 是先例）：

```c
sds  snippet;       /* 一行选择摘要，<= 60 字符；NULL = 不进散文段 */
sds  guidelines;    /* 行为约定，一行一条，'\n' 分隔；NULL = 无 */
uint32_t scope;     /* EDC_TOOL_SCOPE_NORMAL | _MEMORY | _FOLD ... 位掩码 */
uint8_t exposure;   /* DIRECT（进 tools 字段）/ DEFERRED（只进散文，B4 起用） */

void edc_tool_set_snippet(struct edc_tool *t, const char *snippet);
void edc_tool_set_guidelines(struct edc_tool *t, const char *lines);
void edc_tool_set_scope(struct edc_tool *t, uint32_t scope);
```

各工具要写的 snippet（示意，落库时统一口径）：

| 工具 | snippet | guidelines |
|---|---|---|
| `read_file` | Read a file (offset/limit supported) | 2000 行 / 64KB 上限 |
| `list_dir` | List one directory level | 不递归 |
| `edit_file` | Replace an exact snippet in a file | 匹配多处要消歧参数 |
| `exec_shell` | Run a shell command | 输出上限；最小 env 白名单 |
| `grep`（R4 后） | Search file contents (respects .gitignore) | 单行截断 500 字符 |
| `find`（R4 后） | Find files by glob | — |
| `device_*` | Run on the remote device | 走 PPPoS/TCP，不是本机 |
| `memory_*` | — | scope = `_MEMORY`，普通任务散文段里不出现 |

### 2.2 组的工具状态：`struct edc_task_group`

```c
cJSON *tools_decl;     /* 请求用的声明集：启动时 resolve 出来，变更时重算；不落盘 */
uint32_t toolset_epoch;/* 每次真正改变声明集 +1；用于日志与 cache miss 归因 */
```

**不落盘的理由**：初始集可从 config + 注册表重建，变更可从 history 的 delta 重放——两份都不用存。
（pi 的等价物是"transcript 第一条 system message 里存初始 tools"；我们 `history[0]` 不持久化
——见 `edc_group_fold.c:565` 的注释——所以走"重建 + 重放"。）

### 2.3 history 里的 delta 消息

一条 system message，三部分：

```json
{ "role": "system",
  "content": "Tool set changed (mcp disconnected: filesystem).\nRemoved: mcp__fs__read_file, mcp__fs__list_dir. Use read_file / list_dir instead.",
  "tools_added":   [ /* 完整 schema，供 B4 的 deferred 与 replay 用 */ ],
  "tools_removed": [ {"name": "mcp__fs__read_file"}, ... ],
  "toolset_reason": "mcp:disconnected:filesystem" }
```

**⚠ 2026-10-21 实作时推翻了一处设计**：原先打算"delta 追加进 `group->history` 并标 `pinned`，让折叠不要吃掉它"。
实现时核实到 **`history` 是从 turns 重建的派生状态**（`edc_task_group_rebuild_history`：system prompt + 各成员的 turns，
折叠落地和重启都会重建），所以追加进 history 的任何东西在下次 rebuild 时都会消失——`pinned` 自然也就无意义。

改法：delta 存进 **`group->tool_deltas`**（组的字段，**不落盘**），由 `rebuild_history` 在**尾部**重新追加。
- 不落盘是有意的：工具集本身由 registry + config 重建，那才是持久真源；delta 只是"这次变化"的说明。
- 尾部追加不会破坏缓存（rebuild 本来就已经改变了前缀）。
- `note_change` 同时把消息追加进 `history`，所以下一次请求不需要等 rebuild 就能看到。

**reason 词表**（自由字符串会退化成日志垃圾，先定死）：
`reload` · `mcp:connected:<name>` · `mcp:disconnected:<name>` · `tools:off:<name>` · `tools:on:<name>` · `model:switch`

---

## 3. 派生（纯函数，`src/kernel/context/edc_toolset.{c,h}`）

```c
/* 初始声明集：注册表里 exposure==DIRECT 且 scope 含 NORMAL 的工具，按名字排序。 */
cJSON *edc_toolset_initial(struct edc_toolbox *tb);

/* 重放 history 里的 delta，得到当前集；不分配状态，不读时钟。 */
bool edc_toolset_replay(cJSON *history, cJSON **decl /*out*/, bool *has_non_additive);

/* 组的请求用声明集（幂等，可反复调用） */
cJSON *edc_toolset_resolve(struct edc_toolbox *tb, struct edc_task_group *g);
```

**规则**（2026-10-21 修正，对齐 pi 的最后一档）：**`tools` 字段永远等于当前声明集**。

```
declared = registry ∩ scope ∩ exposure == DIRECT     ← 每次重算，永远是"现在"
```

| 变更类型 | `tools` 字段 | 缓存 | 为什么 |
|---|---|---|---|
| direct 工具新增 | **变**（多一项） | **一次 miss** | 字段是模型拿到 schema 的唯一途径，"只有名字"不等于可用 |
| direct 工具移除/重定义 | **变** | **一次 miss** | 同上 |
| **deferred** 工具增删 | **不变** | **保住** | 它本来就不在字段里——省缓存靠**配置**，不靠代码偷偷降级 |

> 早期实现曾在"纯新增"时冻结字段以省一次 miss。那确实是省了，但代价是把一个 direct 工具
> 降级成了 deferred（名字知道、参数不知道）——用缓存优化改掉了语义。现在改为：想省缓存就把它
> 声明成 deferred。

---

## 4. 请求组装（`src/actor/edc_llm_request.c:300`）

```c
if (task->sink == EDC_SINK_SESSION_MEMORY)   tools = memory_scope_schemas;  /* 现在就从 scope 派生 */
else if (is_memory_sink)                     tools = NULL;                  /* fold：现状不变 */
else                                          tools = edc_toolset_resolve(tb, task->group);
```

请求级字段（**核实后修正**：我们只会说 OpenAI chat-completions 格式，见
`src/common/edc_string_utils.c:359` 拼 `/chat/completions`；没有 Anthropic 原生 `/v1/messages` 路径）：

| 字段 | 条件 | 值 | 状态 |
|---|---|---|---|
| `prompt_cache_key` | base url 含 `api.openai.com` | `group_id`（组级稳定 key → 缓存分片亲和） | ✅ 已实现（2026-10-21） |
| `cache_control` | — | — | ⏸ **暂不适用**：需要 Anthropic 原生协议路径才挂得上 |

`prompt_cache_key` 只发给 `api.openai.com` 的理由：OpenAI 对未知顶层参数会整单 400，而其他家
（DeepSeek / 各类兼容网关）的前缀缓存是按内容自动命中的、不需要这个字段。pi 同样只对
`api.openai.com` 发（`openai-completions.ts` 的 `prompt_cache_key` 条件）。

> 这两条独立于工具集形态，**先做**（B0），因为它们比改工具集便宜得多、收益更大。

---

## 5. 散文段生成（方案 ② 的完整规则）

`edc_toolset_render_prompt(cJSON *decl)` → 替换 `edc_context.c` 里现在手写的那段。输出形状：

```
## Tools

- read_file: Read a file (offset/limit supported)
- edit_file: Replace an exact snippet in a file
- list_dir: List one directory level
- exec_shell: Run a shell command (ls, rg, find)
- ...

Rules:
- Use exec_shell for searching and listing until grep/find are enabled.
- Use the device_* tools for anything on the remote device; exec_shell runs locally.
- Outputs are truncated; if a truncation note appears, narrow the query instead of re-reading.

Other tools may become available during this session; a "Tool set changed" message says so.
```

生成规则（**每一条都必须来自代码，不许出现手写段落**）：

1. **摘要行**：`- <name>: <snippet>`，只列 `snippet != NULL` 的声明工具，按名字排序（I6）。
2. **条件规则**：由"当前声明集"推导，规则表在 `edc_toolset_rules.c` 里以代码形式存在：

   | 条件 | 规则 |
   |---|---|
   | 有 `exec_shell` 且无 `grep`/`find` | Use exec_shell for searching and listing |
   | 有 `grep` 且有 `find` | Prefer grep/find over exec_shell for searching |
   | 有 `exec_shell` 且有 `device_*` | device_* 走远端，exec_shell 只在本机 |
   | 有 `edit_file` 且有 `read_file` | Read before editing（现有 pre-edit 校验的另一半，散文层） |

3. **工具自带 guidelines**：追加在规则段末尾（去重，稳定顺序）。
4. **兜底句**：仅当 `exposure==DEFERRED` 存在时才出现——没有 deferred 就不写，避免空话。
5. **禁止**：不重复参数说明（array 里有）；不写工具数量；不写"你可以使用以下工具"这种客套。

`TOOLS.md` 的处理（I1）：**直接删除，不留任何生成器**。

理由（2026-10-21，用户提出"这不是直接删了就好"）：
- 模型**不读它**——`tools` 字段已带完整参数 schema，散文段带选择引导，两份都在请求里；
- 它躺在用户自己的可写工作区（`~/.edgeclaw/workspace/`），**用户一改，下次启动就被静默覆盖**——比没有更糟；
- 每次安装要么带一份会过期的快照，要么依赖"先跑一次"才正确。

同一个病还有第三份：`workspace/AGENTS.md` 里的 `## Available Tools` 清单（它是注入 prompt 的）。
该清单同样列着 `web_search` / `web_fetch` / `message` / `spawn` 四个不存在的工具，已改为一句话
"工具以每次请求的 `tools` 字段为准，没有单独的文档"。**删掉副本才是修根**：只要有人手写清单，它迟早说谎。

---

## 6. 变更协议

```c
/* 唯一入口。三个触发点之外不许调用。 */
void edc_toolset_note_change(struct edc_task_group *g,
                            cJSON *added_schemas, const char **removed_names, size_t n_removed,
                            const char *reason);
```

行为：
1. 追加 §2.3 的 delta 消息：存进 `g->tool_deltas`（rebuild 时重放）并同时追加到 `g->history`（下一次请求立刻可见）；
2. `has_non_additive` → 重算 `g->tools_decl` 并 `toolset_epoch++`；
3. 日志：`toolset change group=<id> reason=<reason> +<n> -<n> epoch=<n> cache=reset|kept`。

触发点：`/reload`、`/mcp` 启停（B4）、`/tools on|off`；模型切换若改变了可用工具集则也算。

**可见性**：`cache=reset` 的变更在下一个回复后打一行提示（不是弹窗），措辞固定：
`Tool set changed (reason). Prompt cache restarted for this conversation.`

---

## 7. 执行侧：`declared` vs `allowed`

`edc_toolbox_exec`（`edc_toolbox.c:255` 那条"not found"之前）插入 scope 判定：

```c
uint32_t want = edc_tool_scope_for_task(task);   /* NORMAL / MEMORY / FOLD */
if (!(tool->scope & want)) → 结构化拒绝
```

| 情况 | 回应 |
|---|---|
| 已声明 + 允许 | 执行 |
| **未声明 + 允许**（deferred、被冻结集裁掉的历史工具） | **执行 + 日志**（`undeclared call: <name>`）——deferred 机制依赖这条 |
| 任意 + 不允许 | `isError: true`，`"<name> is not available to this task (<scope>). Available: a, b, c"` |

**明确**：工具集不是安全边界。越权的真实防线是沙箱（`edc_path_guard` + userns），这里只是
"别让模型跑偏 + 出错要说得清楚"。

---

## 8. 可观测

| 观测点 | 形式 |
|---|---|
| 缓存命中 | **不上屏**（2026-10-21 用户裁定：删掉 footer 的 `cache%`，常驻栏位只放会变且要看的东西）。数字仍在记账 —— `cached_tokens / prompt_tokens` 已在 task / group / snapshot 三层累积，日志可查；需要时再开 `/stats` 类命令 |
| miss 归因 | 日志：`cache miss group=<id> idle=<ms> model_changed=<0|1> toolset_epoch=<n>`；≤1024 token 的 miss 不计（pi 的噪声地板） |
| 工具集变更 | §6 的日志行 + §6 的用户可见提示 |
| 测试锚点 | 抓请求断言 `tools` 字段**字节级相等**（同组连跑 10 轮）；delta 消息在折叠后仍然存在 |

---

## 9. 改动清单

| 文件 | 改动 |
|---|---|
| `src/toolbox/edc_tools.h/.c` | `snippet` / `guidelines` / `scope` / `exposure` + setter |
| `src/toolbox/edc_toolbox.c/.h` | 按 scope 构建 schema 数组（替换 `schemas` / `memory_schemas` 两套硬编码），`exec` 里加 scope 判定 |
| `src/toolbox/edc_tools_*.c` | 各工具补 snippet / guidelines / scope（纯数据） |
| **新增** `src/toolbox/edc_toolset.{c,h}` | initial / replay / resolve（**位置修正**：文档原写 `src/kernel/context/`，但 `//src/toolbox` 依赖 `//src/kernel`，反向会成环——声明集是从注册表派生的，本来就属于 toolbox） |
| **新增** `src/kernel/context/edc_toolset_rules.c` | 条件规则表（代码，非文本） |
| `src/kernel/context/edc_context.c` | 散文段改为渲染；`BOOTSTRAP_FILES` **整套取消**（工作区不再提供提示词文本），契约由 `edc_context_build_contract()` 生成 |
| `src/actor/edc_llm_request.c` | tools 三档选择 + `prompt_cache_key` + `cache_control` |
| `src/kernel/task/edc_task_group.c` | `tool_deltas` 字段 + `rebuild_history` 尾部重放（折叠切点无需改动） |
| `src/tui/edc_status_bar_view.*` | footer 曾加过 cache 字段，已按同一天的裁定**移除** |
| `workspace/TOOLS.md` / `workspace/AGENTS.md` | 两个都**删除**，不再打包（内容搬进运行时生成；`AGENTS.md` 只保留项目维度的加载路径） |

---

## 10. 批次与判据

| 批次 | 内容 | 判据 |
|---|---|---|
| **B0** | ✅ `prompt_cache_key`（仅 `api.openai.com`）· ⛔ footer `cache%`（做过又删：诊断数字不进常驻栏位） · ✅ **B0-4b：`TOOLS.md` 直接删除**（不生成不打包）· ✅ **2026-10-21：`BOOTSTRAP_FILES` 整套取消**，`workspace/AGENTS.md` 一并删除，契约改为 `edc_context_build_contract()` 运行时生成；⏸ `cache_control` 暂缓（需 Anthropic 原生协议） | **代码与注入 prompt 里都不再有工具清单副本** |
| **B1** | ✅ `edc_tool` 加 `snippet`/`guidelines`/`scope`/`exposure`（setter + free）· ✅ 10 个 DIRECT 工具补 snippet、7 个 memory 工具标 `SCOPE_MEMORY` · ✅ `edc_toolset.{c,h}`：`initial` / `replay` / `resolve` · ✅ `tests/test_toolset.c`（13 checks） | replay 幂等；移除用例返回 `has_non_additive`；**同一注册表换注册顺序 → 输出字节相同**（已由测试钉住） |
| **B2** | ✅ `edc_llm_request.c` 改走 `edc_toolset_resolve`（组冻结集；fold 仍 NULL、curator 仍 memory 集）· ✅ system prompt 的 `## Tools` 散文段由 `edc_toolset_render_prompt` 生成（kernel 侧装 provider 钩子，避免成环）· ✅ **执行侧 scope 判定**（curator 只能记忆工具、fold 全拒、拒绝时列出可用工具） | 同组连跑 `tools` 字节级相同；`resolve` **永远返回当前集**（新增即声明、deferred 被过滤，单测已钉）；curator 硬调 `exec_shell` 被结构化拒绝 |
| **B3** | ✅ `edc_toolset_note_change`（delta + 重算 + epoch/日志）· ✅ `group->tool_deltas` + `rebuild_history` 尾部重放 · ⏳ 触发器留给 B4（今天没有能真正改变工具集的动作） | 新增 **direct** → 字段变、epoch+1；新增 **deferred** → 字节不变、epoch 不涨；移除 → 字段变、epoch+1；**两次 rebuild 后 delta 仍在且只有一份**（单测已钉） |
| **B4** | **⏸ 暂缓（2026-10-21 用户裁定，理由与重开条件见 `product-alignment.zh.md` R1）** —— 原计划：MCP 接入 + `exposure` | `deferred` 工具不在 `tools` 里但能被调用且日志有 `undeclared call` |

**B0 可当天做完，且与工具集形态无关**——先把它摘掉，缓存才开始有数可看。

---

## 11. 风险

| 风险 | 缓解 |
|---|---|
| deferred 工具只有 `name + snippet`，模型参数猜错 | snippet 里带关键参数名（`exec_shell: Run a shell command (command, timeout)`）；执行侧的参数错误必须指出缺哪个字段 |
| 移除即 miss，频率取决于 MCP 断连 | 变更原因写进日志与用户提示；高频反复启停时提示"本次会话缓存已重置 N 次" |
| delta 与派生 history 的关系 | 已纠正：delta 存 `tool_deltas`、rebuild 尾部重放；单测钉住"两次 rebuild 后只有一份" |
| prose 与 array 再次漂移 | I1：两处都从注册表生成，且**不再有第三处**；测试断言"每个声明工具都有 snippet" |

---

## 12. 不做

- **per-task 裁剪工具集**：会让每个任务边界都打穿缓存，与 I2 冲突。
- **Anthropic 的 mid-conversation tool changes beta**：私有 beta + 单 provider，不押。
- **把 allowed 当沙箱**：安全边界在 `edc_path_guard`，不在这里。
- **完整描述在散文里再抄一遍**：纯浪费且必然漂移（这正是方案 ③）。

---

## 13. 待确认（未反对即按默认执行）

| # | 事项 | 默认值 |
|---|---|---|
| Q1 | 冻结粒度 | 组级（D1） |
| Q2 | `cache_control` 范围 | 只给 Anthropic 系（D2） |
| Q3 | 越权回应是否允许"终止整轮" | 默认不允许；留 `terminate` 标记位给明确危险场景 |
