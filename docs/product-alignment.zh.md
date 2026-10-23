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
2. 我们的差距集中在**接入面**（愿不愿试）：无 MCP、无 headless（grep/find 已于 2026-10-21 补上）。
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
| 检索工具 `grep` / `find` | ✅ | ✅ | `edc_tools_search.c` / `tools/{grep,find}.ts` |
| web 工具 | ❌ | **❌ 也没有** | `edc_toolbox.c` / 零命中 |
| MCP | ❌ 零命中 | ✅ | `grep -i mcp src` / `docs/mcp.md` |
| headless | ❌ 只解析 `--sandbox` | ✅ 四模式 + SDK | `src/tui/main.cpp` / `docs/cli.md` |
| 工具注册面 | ❌ 静态数组；system prompt 工具清单写死 | ✅ 每轮按激活集重推（静态可见性） | `edc_toolbox.c`、`edc_context.c` / `system-prompt.ts` |
| 工具输出外溢 | ❌ 超限直接截断 | ✅ 双上限 + 有界预览 | `edc_output_buf.c` / `truncate.ts` |
| 工具并行 | ❌ 串行 `req->next` | ✅ 嵌套调用 + 写串行队列 | `/` `nested-tool-calls.ts`、`file-mutation-queue.ts` |
| 先落盘再副作用 | ✅ 任务创建 / 工具派发前 / EXECUTING / 工具结果 / ERROR 五处落盘（2026-10-23） | ✅ | `edc_scheduler.c` `sched_save_progress` |
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

**2026-10-22 补：清单有了查看出口。** 在那之前这份清单只存在于 `group->memory` 里——
折叠任务的 output 是模型返回的纯净摘要，**块不含在内**，`/group info` 只说 `memory present`，
于是"做了但谁都不知道它存在"。现在 `/group files [id]` 渲染它，`/group info` 给条数行；
两者共用 `edc_group_fold_tracked_files()` 这一个读取口。

**它是什么、不是什么**（输出里无条件带这句免责）：**折叠落地那一刻的快照，不是实时视图**。
之后的改动不反映；`exec_shell`（`sed -i` / `cat` / `git checkout`）从来不记。
这不是理论缺口：真实会话里某组 **917 次 `exec_shell` 对 1 次 `read_file`**，
读清单为空不是因为没读，而是因为读都走了 shell。把它当 `git status` 用就会错，
而错误从清单本身看不出来——所以免责声明和信息一起发。

与 pi 的差异（2026-10-23 已收敛）：pi 把清单存成结构化 `details.readFiles/modifiedFiles` 数组，
我们**现在也是结构化的** —— `struct edc_fold_files`（`src/kernel/context/edc_fold_files.h`）挂在
`group->fold_files`，落盘为 `"fold_files": {"modified":[…],"read":[…]}`；`group->memory` 只存摘要正文，
两个块在**每次请求时渲染**（`edc_fold_files_render`）。累积不再解析任何文本，模型改写块也污染不了数据。
开工顺序是"先钉字节再接线"：`render` 与旧实现（把清单拼进 memory 那段）逐字节相同、且确定性
（同状态同字节），因为这段文本在缓存前缀里，差一个字节就是从 token 0 起全废。

两条顺便对齐的语义：`read-files` 减去 `modified`（读过又被改的不算"只读"）；`list_dir` 移出读桶
（目录不是文件，pi 的 `ls` 也不记）。

**有意偏离 pi 的一处**：pi 排序、我们不排序（首见序 + 每份清单 cap 50）。排序会让"最近碰过的文件"
被字典序永久挤出 cap 之外，而 cap 又不能去掉（这段文本每轮都进缓存前缀）。

**不兼容旧档案**（用户 2026-10-23 裁定）：不写迁移、schema 不升。缺 `fold_files` 键读回空清单；
`group->memory` 是整体替换，旧档案里残留的块会在下一次折叠落地时自然消失（在那之前模型仍能看到
它们，行为和改动前一致）。

---

## 4. P1 论证 —— 决定留不留

### 4.1 R6 steer / followUp

pi：流式输出中发来的消息可选择 `steer`（下个安全边界生效）或 `followUp`（排队），
运行中还有 context-only 的 asides（工具结果回来后 flush）。我们是**输入即建任务**，没有受理/提升分离，
用户"边说边改"的唯一手段是打断。这会动 scheduler 的受理段，成本中，但这是交互感的真差异。

### 4.2 R7 输出双上限 + 外溢

**✅ 已落地（2026-10-22）。** 原先有上限但**直接截断**（信息丢了，只得到一句"你丢了 8893 字节"）。
现在：两个上限先到先得（行 `EDC_TOOL_OUTPUT_MAX_LINES` = 2000 / 字节 `tool_max_output_size`，默认
**10000**——文档此前写的"50KB"是错的，配置项在 `edc_config.h:177`、默认值在
`edc_init_env.h:40`），命中即截断且不返回半行；
**整份输出先落盘**再算预览，所以"截断"等于"搬家"而不是"丢失"；消息形状是
`[truncated: showing X of Y lines (A of B bytes)]` + `[full output: <path>]` + 有界预览。
闭环靠 `read_file` 的 offset/limit（`cad5018`）：预览说剩下在哪，读它的工具也存在。

### 4.3 R8 先落盘再副作用

**✅ 已落地（2026-10-23）。** 原先状态只在任务 settle（或干净退出 `edc_scheduler_close`）时落盘，
所以 `kill -9` 丢的**不是"哪个工具在跑"，而是整个在飞任务**——重启后它不存在，连 `/task resume`
都没有对象。

调查结论：`kill -9` 的损失点不在"工具状态没单独落盘"，而在**根本没有任何中途保存**。
恢复机制一直是齐的（ledger 的 PENDING / EXECUTING 两态、`active_turn` 随会话序列化、
PENDING 重派、EXECUTING 给"结果未知"提示），缺的只是磁盘上那份状态。

因此不需要新机制，只在五个边界各写一次：任务创建、工具派发前（ledger 已 PENDING）、
TOOL_STARTED（已 EXECUTING）、TOOL_RESULT 到达、ERROR（这个分支原先只标脏不落盘，
失败状态要等别的任务结束才上盘，重启可能显示成还在跑）。

**刻意不做"每个工具状态变化都落盘"**：序列化的单位是整个会话，要保证的是"重启能恢复什么"，
不是镜像每个字段。粗粒度已经覆盖全部可恢复事实。

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
| **R3** | ✅ **已完成（2026-10-22），但交付的形态与 pi 不同**：`edgeclaw_cli '<你在 TUI 里会输入的那一行>'` —— **输入行直通**，没有第二套命令行词汇；只有输出可选（默认答案 / `--json` / `--silent` + `/task output <id>` 事后取）。独立二进制，只链 app 不链 ftxui（6.5 MB vs 12 MB） | 同一行输入在 TUI 与 CLI 行为一致（实测：消息、`/help`、`/task new`、`+`、`!cmd`、`"a" \| "b"`）；退出码 0/1/2/3；print 模式 stdout 只有答案；`//tests:all` 113（`test_cli.sh` 13 checks） | — | — |
| **R4** | ✅ **已完成（2026-10-22，纯 C 版）**：`grep` + `find` 在进程内实现（`edc_tools_search.c`），无子进程、无 shell；遍历在 libuv 线程池，路径先过沙箱；POSIX regex（Windows 用 pcre2-posix）；`literal` / `limit` / `glob` / `ignore_case` / `timeout` | 一轮定位；单行 500 字符；默认 100 条命中并说明截断；二进制/CRLF/.git 等跳过；散文规则自动从"用 exec_shell 搜索"切成"优先 grep/find"（单测钉住） | — | — |
| **R5** | ✅ **已完成（2026-10-21 / 2026-10-22 / 2026-10-23）**：6 段格式 + 迭代更新 + split turn + 精确路径要求 + `max_tokens`（早就有）；`<read-files>`/`<modified-files>` 从 tool call 抽取、跨折叠累积、上限 50、确定性顺序（2026-10-21）；**查看出口 `/group files [id]`** + `/group info` 条数行 + 修掉 `without_block` 的 use-after-free（2026-10-22，`20d563a`）；**清单升级为结构化数据** `struct edc_fold_files`（2026-10-23）：`group->memory` 只存正文，两个块每请求渲染，累积不再解析文本，`read-files` 减去 `modified`，`list_dir` 移出读桶 | 折叠后能说出改过哪些文件、卡在哪；两次折叠取并集不重复（`test_group_fold.c`）；`render` 与旧实现**逐字节相同** + 确定性 + 变异自检（`test_fold_files.c` 37 checks）；`/group files` 三种状态 + 恒带"非实时快照"免责（`test_commands_group_fold.c`） | — | — |

> **R3 为什么不是 pi 的 `--print` / `--mode json`（2026-10-22，用户裁定）**
>
> pi 的形态是"给 CLI 自己一套旗标"（`-p`、`--mode json`、`--mode rpc`）。我第一版照做了，被否掉，理由成立：
> 用户得学第二套说法才能表达他已经会说的话，而且这套说法要与 TUI 永远保持同步。
>
> **现在的模型是"同一行输入，只是没有屏幕"**：参数就是输入框里那一行，走
> `edc_app_send_user_message`（TUI 按 Enter 调的那个），passes / 分类 / 命令表 / shell 文法 /
> session / group 全是同一份代码。留下的可选项只有输出——因为消息在进程决定打印之前就已经存在：
> 默认给答案、`--json` 给逐行事件、`--silent` 什么都不给（仍记录，事后 `/task output <id>` 取）。
>
> 代价与收获：**没有** `--mode rpc` / TS SDK（决策 C 仍然悬着），**也没有** pi 那种"stdin/stdout 被重定向就自动进 print 模式"的隐式切换——我们是显式的另一个二进制。换来的是一条永远不需要同步的规则，以及一个连"误画终端"都做不到的小二进制。

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
| **R7** | ✅ **已完成（2026-10-22）**：两个上限先到先得（行 `EDC_TOOL_OUTPUT_MAX_LINES` 2000 + 字节 `tool_max_output_size` 默认 10000）、超限先落盘再截断、有界预览 + `[full output: <path>]`、`read_file` 的 offset/limit 闭环（`ddf40b0` 外溢 / `cd8b62f` / `57867c7` / `cad5018`） | 截断提示同时给出命中哪个上限、显示多少行/字节，并带可读路径；`test_toolbox_response.c:403 test_result_truncated` | — | — |
| **R8** | ✅ **已完成（2026-10-23）**：状态不再只在任务 settle（或干净退出）时落盘。新增 `sched_save_progress()`，在**任务创建**、**工具派发前**（ledger 已是 PENDING）、**TOOL_STARTED**（已是 EXECUTING）、**TOOL_RESULT 到达**、以及**ERROR** 五个边界各写一次。刻意是粗粒度（每任务 / 每工具边界一次），不是"每个工具状态变化都写"——序列化单位是整个会话，要保证的是"重启时能恢复什么"，不是镜像每个字段。恢复机制本来就齐（PENDING 重派、EXECUTING 不重派而是给"结果未知"提示、`active_turn` 随会话落盘），缺的只是磁盘上根本没有那份状态 | `kill -9` 后重启，崩溃前在跑的任务**存在**、带 `INTERRUPTED`、`/task resume` 可用、未完成的 turn 也在（`test_scheduler_open_close.c` 的 `test_running_task_survives_a_kill`，用「free 而不 close」真实模拟被杀；去掉创建时的落盘 ⇒ 8 条断言全红） | M | — |
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
| **C** | pi 有 rpc 模式 + SDK | ① 只做输入行直通的 CLI（**已落地**） ② 再加 `--mode rpc` ③ 再加 SDK | ①done ②中 ③大 | ①已完成；②③ 看是否要喂 device agent / 当库用 |

---

## 9. 技术债（不在 R 清单里：能力已对齐，但工程状态有欠账）

这三条**不占 R 编号**——R 编号的语义是"按迁移成本排的优先级"，技术债不是需求，
混进去会破坏"编号 = 优先级"这个约定。记录在此，修的时候单独立项。

| # | 债 | 后果 | 成本 |
|---|---|---|---|
| **T1** | **内置资源安装后冻结** | 提示词类已不再是风险（不再从磁盘读取），但 `skills/*/SKILL.md`、`prompts/review.md` 改好了**老用户拿不到**——安装时复制、之后不跟随更新 | S/M |
| **T2** | **技能索引为读 frontmatter 而读全文** | 每个 `SKILL.md` 最多读 512KB 只为取 name/description ⇒ 装几十个包时**每次构建提示词**都有固定开销 | S |
| **T3** | **`/reload` 不重建工具集**（有意为之） | 用户改配置后 `/reload` 不会让工具变化生效，但 `/help` 里没写清楚 ⇒ 会当作 bug 报 | S（文档） |

---

## 10. 维护规则

- 每条能力只有**两种状态**：`已对齐`（给文件/函数证据）或 `缺口`（给判据）。**不写"部分支持"**——
  含糊的状态等于没有状态。
- 升级到 `已对齐` 必须落在**测试或可复现命令**上，不接受"代码看起来有"。
- 对标结论以**核过的源码**为准（pin commit + 本机副本路径）；凭知识写的必须标注"未核实"。
- 需求编号（R1…）**只增不改**：完成一项标 `✅ done <commit>`，废弃标 `✗ dropped <原因>`，不要重新编号——
  编号是外部引用锚点。
- 新增对标对象（例：Claude Code）另开一节，不与 pi 的结论混写。
