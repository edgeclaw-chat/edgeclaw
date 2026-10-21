# 产品层面对齐 pi：缺口、取舍与需求清单

> **对齐基线 = pi（earendil-works/pi）**，本机副本 `/tmp/pi-verify` @ `4dd2b14`，逐文件核实。
> 我们的现状 = 本仓 HEAD `44a5fb0`（2026-10-20），逐条 grep 核实。
> **opencode（sst）仅作参考**：它的东西只在 pi 没有、而我们确实缺时才进本文，并明确标注"非 pi 项"。
> 本文是**产品决策文档**：定义"该不该做、为什么、先做哪个"。单条能力的实作方案另开文档
> （上下文估算与折叠见 `docs/pi-alignment.md`）。
> 目标口径沿用 2026-10-19 裁决：**不做架构对齐，抢用户**——优先级 = ① 迁移成本（愿不愿试）
> × ② 独有杀手锏（试完留不留）。

---

## 0. TL;DR

1. **pi 的定位是"最小可扩展 harness"**：内建工具只有 8 个、明确**不做** sub-agents 和 plan mode，
   能力靠 extensions / skills / prompt templates / themes，打成 **Pi Packages** 用 npm 或 git 分发。
2. 我们的差距集中在**接入面**（愿不愿试）：无 MCP、无 headless、无检索工具（grep/find）。
3. 我们已**超出** pi 的部分不要退化：task/group/依赖图、可见沙箱（pi 靠 docker）、cron、device、
   CJK token 估算、shell 语法、`@file`、粘贴折叠。
4. 最近两周（10-18 → 10-20）提交全在输入框体验（留人环节），**"愿不愿试"的门槛一条没动**。
   这是当前最需要纠的资源配置。

---

## 1. 基线：pi 是什么（核实结论）

| 维度 | pi 的做法 | 证据 |
|---|---|---|
| 内建工具 | 8 个：`bash` `read` `write` `edit` `ls` `find` `grep` `powershell`。**没有** web 工具、没有 sub-agent 工具 | `src/core/tools/`（`grep -rniE "web_fetch\|webfetch\|web_search"` 零命中） |
| 输出上限 | 行 / 字节**先到先得**：2000 行 或 50KB；grep 单行 500 字符；拆行不返回半行 | `src/core/tools/truncate.ts`、`output-guard.ts` |
| 交互模式 | 四模式：interactive / `--print` / `--mode json` / `--mode rpc`，外加 TS SDK。stdin 或 stdout 被重定向时自动进 print 模式 | `src/modes/{print-mode,json-event,rpc}`、`docs/cli.md` |
| MCP | 一等公民：`pi mcp add\|list\|login\|logout`；stdio + streamable HTTP；`~/.pi/agent/mcp.json` + 项目 `.pi/mcp.json`（**需 trust**）；`exposure` / `toolExposure` **按需暴露**；`/mcp` 面板 | `docs/mcp.md`、`src/core/mcp-servers.ts`、`packages/mcp` |
| 扩展与分发 | **Pi Packages** = extensions + skills + prompts + themes；`pi install npm:\|git:\|./path`、`-e` 试跑、user/project 分层写入 settings.json。**包可以执行扩展代码** | `docs/packages.md`、`src/core/package-manager.ts`、`pi-manifest.ts` |
| 项目信任 | `/trust`：gate 项目级 `mcp.json`、包声明、以及资源目录 | `src/core/{project-trust,trust-manager}.ts`、`docs/security.md` |
| 会话 | `/tree`（选中历史消息回填编辑器分叉）、`/fork`、`/clone`、`/copy`、`/export`（HTML/JSONL）、`/share`（gist/Radius）、`/new`、`/resume` | `docs/sessions.md`、`src/core/session-export.ts` |
| 主题 | `system` 默认：查终端 fg/bg + 16 ANSI 取色相，正文 ≥ 4.5:1 WCAG，亮暗切换重新取色；100ms 超时回退 ANSI；另有 dark/light/自定义 | `docs/themes.md` |
| 设置 | user（`~/.pi/agent/`）vs project（`.pi/`）分层 + `/settings` 面板 + `keybindings.json` + `/hotkeys` | `docs/settings.md`、`src/core/settings-*.ts` |
| 运行中输入 | `streamingBehavior: "steer" \| "followUp"`（steer 在安全边界生效，followUp 排队）；运行中还有 context-only 的 "asides" | `src/core/agent-session.ts:320` |
| 压缩 | 6 段结构化摘要 + 文件追踪 + 有旧摘要时**迭代更新** + 失败判定 | `docs/compaction.md`、`docs/pi-alignment.md` |
| 模型 | models.dev 远程目录、`scoped-models`、`--models` 轮换、**virtual-models**（扩展注册，按任务/成本路由）、`/login` 订阅或 API key | `docs/{models,virtual-models}.md`、`src/core/virtual-models.ts` |
| 隔离 | **不给自己的沙箱**：跑在 docker / Docker Sandboxes / OpenShell 里 | `docs/containerization.md` |
| 明确不做 | sub-agents、plan mode（README 原话："skips features like sub-agents and plan mode"） | `README.md` |

**对我们的直接含义**：我们的沙箱（userns + `/sandbox list`）和 task 图都是 pi 没有的形态，
所以"对齐 pi"不是"变成 pi"——是补齐它**默认预期**的那几项，其余保持我们的形状。

---

## 2. 现状对照（pi 为主轴）

| 能力 | 我们 | pi | 证据（我们 / pi） |
|---|---|---|---|
| 检索工具 `grep` / `find` | ❌ 只有 `list_dir`+`read_file` | ✅ | `edc_tools_fs.c` / `tools/{grep,find}.ts` |
| web 工具 | ❌ | **❌ 也没有** | `edc_toolbox.c` / 零命中 |
| MCP | ❌ 零命中 | ✅ | `grep -i mcp src` / `docs/mcp.md` |
| headless | ❌ 只解析 `--sandbox` | ✅ 四模式 + SDK | `src/tui/main.cpp` / `docs/cli.md` |
| 工具注册面 | ❌ 静态数组；system prompt 工具清单写死 | ✅ 每轮按激活集重推（静态可见性） | `edc_toolbox.c`、`edc_context.c` / `system-prompt.ts` |
| 工具输出外溢 | ❌ 超限直接截断 | ✅ 双上限 + 有界预览 | `edc_output_buf.c` / `truncate.ts` |
| 工具并行 | ❌ 串行 `req->next` | ✅ 嵌套调用 + 写串行队列 | `/` `nested-tool-calls.ts`、`file-mutation-queue.ts` |
| 先落盘再副作用 | ❌ 只在 settle 落盘 | ✅ | — |
| 运行中输入 steer/followUp | ❌ 输入即建任务，无受理段 | ✅ | `edc_scheduler.c` / `agent-session.ts:320` |
| 压缩摘要格式 | ✅ 6 段 + 迭代更新 + split turn + `<read-files>`/`<modified-files>` | ✅ 同形 | `edc_group_fold.c:52/84/96` / `docs/compaction.md` |
| 上下文估算 + 尾部 + token 轴折叠 | ✅ 刚落地（对齐 pi 的 P1/P2） | ✅ | `edc_token_estimate.c` |
| Pi Packages（资源打包） | ❌ | ✅ | — |
| 项目信任 | ❌ **已拍板不做** | ✅ `/trust` | — / `trust-manager.ts` |
| `/export` `/copy` `/share` | ❌ | ✅ | — / `session-export.ts` |
| 会话树 `/tree` `/fork` `/clone` | ⚠️ 有 `/task fork`（语义不同：分任务图，不是分会话文件） | ✅ | `edc_task*` |
| `system` 主题 + 自定义主题 | ⚠️ 只做亮/暗检测，无 `/theme` | ✅ | `edc_style.hpp` / `docs/themes.md` |
| settings 分层 / `/settings` / 键位 | ❌ | ✅ | — / `docs/settings.md` |
| 图片输入 | ❌ | ✅（`@path` 图、粘贴） | — / `docs/images.md` |
| virtual-models / scoped-models | ❌ | ✅ | — / `virtual-models.ts` |
| 资源多 root（`.edgeclaw` `.claude` `.agents` `.opencode` `.pi`）+ `$1..$N` | ✅ | ✅ | `edc_prompts.c`、`edc_skills.c` |
| AGENTS.md / `<project_instructions>` | ✅ | ✅ | `edc_context.c` |
| `@file` 引用 / 粘贴折叠 | ✅ | ✅ | `edc_input.cpp` |
| sub-agents / plan mode | 不适用（我们有 task 图，是超集） | ❌ 明说不做 | — |

---

## 3. P0 论证 —— 不补就没人来

### 3.1 R1 MCP

pi 用户（以及 Claude Code / opencode 用户）的工具生态在 mcp server 上。我们没有 = 他们现有能力归零。
形态直接照 pi：`mcp.json` 两层、stdio + streamable HTTP、`/mcp` 面板、**`exposure`/`toolExposure` 按需暴露**
（不是"全量塞进工具表"）。成本与风险点：长驻子进程的生命周期、server 崩溃不拖死 turn、
工具名冲突（内建优先还是拒绝？——要拍板）。

### 3.2 R2 工具注册面（R1 的前置）

> **实作方案见 `docs/pi-toolset-plan.zh.md`**（声明 / 散文 / 权限三层 + B0–B4 批次）。
> 下面这段是"为什么要改"的论证；"改成什么样"以那份文档为准。

现在 `edc_toolbox` 是静态数组，system prompt 的工具清单是写死文本（`edc_context.c`）。
不改这两处就上 MCP，等于把 MCP 的工具硬塞进一张静态表——返工。
pi 的静态可见性值得直接抄：**只列激活的工具**，被排除的工具模型根本不知道它存在（省 token、少误用），
`rules` 段按激活组合动态拼（pi 的例：只有 bash 没有 grep/find 时自动加一条"用 bash 干 ls/rg/find"）。

### 3.3 R3 headless 双模式

我们完全不可脚本化：CI 跑不了、cron 只能唤起交互式 TUI、device agent 没法把 EdgeClaw 当库。
**只做 `--print` + `--mode json`**；`--mode rpc` 与 TS SDK 单列为决策 C（那是"把内核做成可嵌入服务"，
与"C 单二进制"没有冲突，但工作量和稳定性要求是另一个量级）。

### 3.4 R4 grep / find

pi 8 个内建工具里我们缺的正是这两个。缺了它们，模型在大仓里的行为是"逐层 list、逐个 read"，
token 消耗肉眼可见。成本最低的一条（spawn `rg`/`grep` 走沙箱；glob 复用 `edc_file_index.c`）。

### 3.5 R5 折叠摘要格式

触发轴已对齐 pi（token 轴 + reserve），**摘要格式也已对齐**（2026-10-21 核实：早前文档把它写成"仍是一句 concise markdown"是错的）。
pi 的摘要格式是 6 段：

```
## Goal
## Constraints & Preferences
## Progress (Done · In Progress · Blocked)
## Key Decisions
## Next Steps
## Critical Context
```

外加 `<read-files>` / `<modified-files>`（从被摘要消息的 tool call 里抽，跨多次折叠累积）、
有旧摘要时走**迭代更新**而非重写、明令"保留精确路径、函数名、错误原文"。
这是"长任务不失忆"的真正来源，纯 prompt 工程 + 一点解析。方案在 `docs/pi-alignment.md`。

---

## 4. P1 论证 —— 决定留不留

### 4.1 R6 steer / followUp

pi：流式输出中发来的消息可选择 `steer`（下个安全边界生效）或 `followUp`（排队），
运行中还有 context-only 的 asides（工具结果回来后 flush）。我们是**输入即建任务**，没有受理/提升分离，
用户"边说边改"的唯一手段是打断。这会动 scheduler 的受理段，成本中，但这是交互感的真差异。

### 4.2 R7 输出双上限 + 外溢

我们现在有上限但**直接截断**（信息丢了）。pi：行/字节先到先得、命中即截断、不返回半行、
grep 单行 500 字符；配合 managed 文件后"读了大文件还想再查一句"才成立。

### 4.3 R8 先落盘再副作用

tool-call 记录先写 transcript 再执行。`kill -9` 后重启能看到崩溃前跑到哪、哪些工具结果未知。
纯可靠性，没有产品争议。

### 4.4 R9 同轮工具并行

读类并发、写类串行（pi 的 `withFileMutationQueue` 防同文件撕裂）。收益是首字延迟与轮次时长。

### 4.5 R10 EdgeClaw 包

pi 的包是"可安装的定制单元"。我们的约束：**只做本地目录 + git URL，永不做 npm 自动执行脚本**。
这条与"不做 TS 扩展运行时"不冲突——包先只承载 **skills / prompts / themes**（纯资源），
扩展执行另按决策 A 处理。

### 4.6 R11 项目信任（决策 B）

pi 用它 gate `mcp.json`、包声明、资源目录。我们已拍板"项目级资源默认开"，那是 prompt 注入面的取舍；
但**"能执行代码"的口子**（MCP server 命令、包里的扩展）不能沿用这个默认。若 R10 只装纯资源，R11 可后置。

---

## 5. P2 论证 —— 便宜且被期待

| # | 需求 | 要点 |
|---|---|---|
| R12 | `/export`（HTML/JSONL）+ `/copy` | 用户的逃生舱，也是收 bug 的手段（现在只能让人截图） |
| R13 | `system` 主题 + `/theme` | 我们只有亮/暗检测；pi 是"取终端 16 ANSI 的色相 + 保对比度 + 亮暗切换重建" |
| R14 | settings 分层 + `/settings` | 项目级设置只影响该项目，面板里能看出值来自哪层 |
| R15 | 图片输入 | `@path` 图片 / 粘贴截图；我们完全没有视觉输入 |
| R16 | 会话树 `/tree` `/fork` `/clone` | 与 `/task fork` 的关系要先定（会话文件 vs 任务图），否则语义打架 |
| R17 | 键位可配 | `keybindings.json`；小活，但用户会找 |
| R18 | 模型面 | `--models` 轮换、scoped-models、virtual-models（后者依赖扩展机制） |

---

## 6. 功能需求清单（按重要性）

§3–§5 是**论证**；本节是**排期用的清单**。排序依据三条，按权重：① 降迁移成本（影响所有 pi 用户）；
② 长会话/输入质量（用得下去）；③ 成本的反向。**编号即优先级**（R1 最重要）。
"依赖"列可能让实际批次与重要性不一致——例如 R1 必须等 R2。

### P0 —— 不补就没人来

| # | 需求 | 判据 | 成本 | 依赖 |
|---|---|---|---|---|
| **R1** | **MCP 接入** —— **⏸ 暂缓（2026-10-21 用户裁定）**，见下方"为什么暂缓 / 重开条件" | 挂一个 server → 工具出现在工具段且可调用；崩溃/超时不拖死 turn；未启用时工具段完全不出现 | L | R2 |
| **R2** | ✅ **已完成（2026-10-21）**：工具集由注册表派生（`edc_toolset`），组内冻结、散文段生成、执行侧按 scope 判定；实作见 `pi-toolset-plan.zh.md` B0–B3 | 同组连跑 `tools` 字节级相同；禁用后散文段里也不出现（`test_toolset.c` 37 checks） | — | — |
| **R3** | **headless 双模式**：`--print`（管道 stdin、`@path`）+ `--mode json` | 无 tty 下退出码正确、stdout 只有结果；json 可逐行消费；与 TUI **共用同一 kernel 路径**（不复制 agent 循环） | M | — |
| **R4** | **检索工具 `grep` + `find`** | "找出所有调用 X 的地方"一轮完成；grep 单行 500 字符；走同一输出上限 | S | — |
| **R5** | ✅ **已完成（2026-10-21）**：6 段格式 + 迭代更新 + split turn + 精确路径要求 + `max_tokens`（早就有）；`<read-files>`/`<modified-files>` 从 tool call 抽取、跨折叠累积、上限 50、确定性顺序（本次补） | 折叠后能说出改过哪些文件、卡在哪；折叠两次取并集不重复（`test_group_fold.c` 三个用例钉住） | — | — |

> **R1 为什么暂缓（2026-10-21）**
>
> 1. **pi 自己也是最近才加的，而且是 extension 不是 core**：`packages/coding-agent/src/extensions/mcp`
>    4.3k 行 + `packages/mcp` 3.3k 行，core 里只有 `mcp-servers.ts` 346 行的配置与注册表。一个以"最小
>    harness"为哲学的对手都把它当外挂——说明它不是 harness 的必要组成。
> 2. **它把"单二进制零依赖"的叙事打穿**：我们不自带运行时，但 `npx` / `uvx` / `docker run` 形态的 server
>    自带**用户机器上的** Node / Python / 容器。对已有这些 server 的人是零成本，对从零开始的人是"先装 Node"。
> 3. **真正的成本不在协议，在协议之外**：JSON-RPC 很简单，难的是进程生命周期（`npx` 包裹层的孤儿进程、
>    崩溃重连、超时）、信任（项目级 `mcp.json` 是提示注入入口）、token（一个 server 几十个工具就是几万 token
>    的 schema）。这三样都要单独设计，不能顺手塞进一个批次。
> 4. 生态价值随用户量放大，在用户量还小的时候，做深现有工具（grep/find/web）的收益高于铺协议。
>
> **重开条件**（满足其一再谈）：① 有明确的迁移请求（几个人明确说"我因为没 MCP 不走"）；② 我们有了扩展机制
> （决策 A），MCP 可以作为外挂而不是内核；③ 决定做 HTTP-only 形态（零本地运行时）作为独立小批次。
>
> **已经沉淀下来、不会浪费的东西**：`exposure`（direct/deferred）、`edc_toolset_note_change`、
> `group->tool_deltas`、执行侧 `scope` 判定——这些是"任何动态工具来源"都需要的机制，MCP 只是它第一个消费者。

### P1 —— 决定留不留

| # | 需求 | 判据 | 成本 | 依赖 |
|---|---|---|---|---|
| **R6** | **输入受理段：steer / followUp** | 流式中发一句：steer 在安全边界生效、followUp 排队；都不打断已开始的工具 | M | scheduler 受理段 |
| **R7** | **输出双上限 + 外溢**（行 2000 / 字节 50KB 先到先得；落 managed 文件 + 有界预览 + 路径） | 超大输出后仍能"再查一句"；截断提示带路径与命中哪个上限 | S | — |
| **R8** | **先落盘再副作用** | `kill -9` 后重启能看到崩溃前记录与未完成标记 | M | — |
| **R9** | **同轮工具并行**（读并发、写串行队列） | 3 个 `read_file` wall time ≈ 1 个；同文件并发写不撕裂 | M | R2 |
| **R10** | **EdgeClaw 包**（本地目录 + git URL；只承载 skills/prompts/themes） | 装一个包后 `/skill`、`/prompt` 立刻可见；卸载干净 | M | 决策 A |
| **R11** | **项目信任**（只 gate "能执行代码"的口子） | 未授信项目的 `mcp.json`/包被忽略且**明确告知原因**；不静默兜底 | M | 决策 B、R10 |

### P2 —— 便宜且被期待

| # | 需求 | 判据 | 成本 | 依赖 |
|---|---|---|---|---|
| **R12** | `/export`（HTML / JSONL）+ `/copy` | 一条命令导出整段会话；HTML 能直接发人看 | S | — |
| **R13** | `system` 主题 + `/theme` + 自定义主题 | 换终端配色无需重启即跟随；正文对比度 ≥ 4.5:1 可测 | M | 已有亮/暗两板 |
| **R14** | settings 分层 + `/settings` 面板 | 面板里能看出某值来自哪一层 | M | R11 |
| **R15** | 图片输入（`@path` 图 / 粘贴截图） | 带图 prompt 到多模态模型；超限明确报错 | M | — |
| **R16** | 会话树 `/tree` `/fork` `/clone` | 能从任意一条用户消息分叉；与 `/task fork` 的差异写进文档 | L | 语义先定 |
| **R17** | 键位可配 | 改一个键不重编译 | S | — |
| **R18** | `--models` 轮换 / scoped-models / virtual-models | 一次调用按序轮换；虚拟模型在 `/model` 可选 | M | R3/R14 |

### 非 pi 项，但独立值得（不算对齐）

| # | 需求 | 说明 | 成本 |
|---|---|---|---|
| **X1** | `web_fetch` | **pi 没有**（opencode / Claude Code 有）。https client + 沙箱已就绪；"用工具先查"这条准则目前没落地 | S |
| **X2** | 上下文缓存前缀治理 | pi 走 `cache-stats`/`cache-warmer`，opencode 走 Context Epoch。我们每轮把 Group Memory 当新 system message 追加，每轮破前缀 | M |
| **X3** | 沙箱越界 ask | opencode 的 runtime 权限粒度。我们是"越界那一刻问一次"，不是加 trust 门 | M |

### 保持项 —— 超出 pi 的部分，别退化

`task` / `group` / 依赖图（pi 没有 sub-agent；我们是超集）、沙箱可见（pi 靠 docker，我们 userns +
`/sandbox list`）、cron、device 远程执行、CJK 感知 token 估算、shell 语法（`\| ; & >`）、`@file`、粘贴折叠。

---

## 7. 明确不做

| 不做 | 理由 |
|---|---|
| sub-agents、plan mode | pi 自己就不做；我们的 task 图已经覆盖 sub-agent 的用途 |
| TS 扩展运行时（内嵌 JS） | 与单二进制冲突；替代方案见决策 A |
| `/share` 上传（gist / Radius） | 隐私面 + 托管依赖，不做 |
| `--mode rpc` + TS SDK | 见决策 C（可能做，但不是现在） |
| opencode 的架构件：事件溯源、LayerNode DI、`packages/llm` 三层、SolidJS TUI、完整 session-tree 存储格式 | 与 C 内核 + 单二进制形态冲突，用户感知低 |

---

## 8. 待拍板的三个冲突点

这三条是"对齐 pi"与我们既有裁决**真正的分歧**，不是审美问题。

| # | 冲突 | 选项 | 成本 | 建议 |
|---|---|---|---|---|
| **A** | pi 的可扩展性押在 TS extensions 上；我们已拍板"不做 TS 运行时" | ① 不抄，skills/prompts 覆盖 ② **子进程式扩展**（外部进程 + JSON-RPC，不内嵌 JS） ③ 抄 TS 运行时 | ①0 ②中 ③高 | **②**：拿到 pi 包的形态，不背 Node |
| **B** | pi 用 `/trust` gate `mcp.json` 与包；我们已拍板"项目级资源默认开" | ① 维持默认开 + 可关开关 ② 抄 `/trust`，**只 gate "能执行代码"的口子** | ①0 ②中 | **先①**；做 A② 或 R10 时**必须**上② |
| **C** | pi 有 rpc 模式 + SDK | ① 只 `--print` + `--mode json` ② 再加 `--mode rpc` ③ 再加 SDK | ①小 ②中 ③大 | **①先做**；rpc 看是否要喂 device agent |

---

## 9. 维护规则

- 每条能力只有**两种状态**：`已对齐`（给文件/函数证据）或 `缺口`（给判据）。**不写"部分支持"**——
  含糊的状态等于没有状态。
- 升级到 `已对齐` 必须落在**测试或可复现命令**上，不接受"代码看起来有"。
- 对标结论以**核过的源码**为准（pin commit + 本机副本路径）；凭知识写的必须标注"未核实"。
- 需求编号（R1…）**只增不改**：完成一项标 `✅ done <commit>`，废弃标 `✗ dropped <原因>`，不要重新编号——
  编号是外部引用锚点。
- 新增对标对象（例：Claude Code）另开一节，不与 pi 的结论混写。
