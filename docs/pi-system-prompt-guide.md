# Pi System Prompt 结构文档

> 基于 pi 源码整理。核心源码：`packages/coding-agent/src/core/system-prompt.ts`（216 行）；
> 各工具的 prompt 贡献定义在 `core/tools/*.ts`；技能格式化在 `core/skills.ts`；
> 渲染与重放在 `packages/ai/src/utils/text.ts` / `transcript.ts`。

---

## 目录

1. [设计总览](#1-设计总览)
2. [完整结构](#2-完整结构)
3. [各 Section 详解](#3-各-section-详解)
4. [渲染与传输](#4-渲染与传输)
5. [变更机制：diff 补丁](#5-变更机制diff-补丁)
6. [配置入口](#6-配置入口)
7. [缓存稳定性分析](#7-缓存稳定性分析)

---

## 1. 设计总览

pi 的 system prompt 不是一段文本，而是**命名 section 的有序集合**（`SystemPromptSections = Record<string, string>`）：

- `preamble` 是无标签的裸文本，其余每个 section 渲染为 `<name>\n...\n</name>`
- 每个 section **独立可替换**，变更时按名字打 diff 补丁持久化到 transcript
- 完整 prompt 是纯函数 `buildSystemPromptSections(options)` 的产物——**确定性**，无任何时间/环境注入
- 工具/技能/项目文件按需贡献内容，缺省 section 直接不出现

## 2. 完整结构

默认（无自定义 prompt、默认工具集）的完整渲染结果：

```
You are an expert coding assistant operating inside pi, a coding agent
harness. You help users by reading files, executing commands, editing
code, and writing new files.

<tools>
- read: Read file contents
- bash: Execute bash commands (ls, grep, find, etc.)
- edit: Make precise file edits with exact text replacement, including
  multiple disjoint edits in one call
- write: Create or overwrite files

In addition to the tools above, you may have access to other custom
tools depending on the project.
</tools>

<rules>
- Use read to examine files instead of cat or sed.
- You can inspect PI_* environment variables for current model and session details.
- Use edit for precise changes (edits[].oldText must match exactly)
- When changing multiple separate locations in one file, use one edit call with multiple entries in edits[] instead of multiple edit calls
- Each edits[].oldText is matched against the original file, not after earlier edits are applied. Do not emit overlapping or nested edits. Merge nearby changes into one edit.
- Keep edits[].oldText as small as possible while still being unique in the file. Do not pad with large unchanged regions.
- Use write only for new files or complete rewrites.
- Be concise in your responses
- Show file paths clearly when working with files
</rules>

<docs>
Pi documentation (read only when the user asks about pi itself, its SDK, extensions, themes, skills, or TUI):
- Main documentation: <安装路径>/README.md
- Additional docs: <安装路径>/docs
- Examples: <安装路径>/examples (extensions, custom tools, SDK)
- When reading pi docs or examples, resolve docs/... under Additional docs and examples/... under Examples, not the current working directory
- When asked about: extensions (docs/extensions.md, examples/extensions/), themes (docs/themes.md), skills (docs/skills.md), prompt templates (docs/prompt-templates.md), TUI components (docs/tui.md), keybindings (docs/keybindings.md), SDK integrations (docs/sdk.md), custom providers (docs/custom-provider.md), adding models (docs/models.md), pi packages (docs/packages.md), environment variables (docs/environment-variables.md)
- When working on pi topics, read the docs and examples, and follow .md cross-references before implementing
- Always read pi .md files completely and follow links to related docs (e.g., tui.md for TUI API details)
</docs>

<addendum>                      ← 可选：用户配置了 appendSystemPrompt 时
（--append-system-prompt / APPEND_SYSTEM.md 的内容）
</addendum>

<project_context>               ← 可选：发现上下文文件时
Project-specific instructions and guidelines:

<project_instructions path="/home/user/.pi/agent/AGENTS.md">
（全局 AGENTS.md 全文）
</project_instructions>

<project_instructions path="/path/to/repo/AGENTS.md">
（父目录向上遍历 + 当前目录的 AGENTS.md 全文，逐个包裹）
</project_instructions>
</project_context>

<skills>                        ← 可选：有技能且 read/bash 激活时

The following skills provide specialized instructions for specific tasks.
Use the read tool to load a skill's file when the task matches its description.
When a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / dirname of the path) and use that absolute path in tool commands.

<available_skills>
  <skill>
    <name>add-llm-provider</name>
    <description>...</description>
    <location>/path/to/.pi/skills/add-llm-provider.md</location>
  </skill>
  ...
</available_skills>
</skills>

<cwd>
/Users/username/projects/my-repo
</cwd>
```

## 3. 各 Section 详解

### 3.1 `preamble` —— 身份声明

固定一句话（system-prompt.ts:147）：

> You are an expert coding assistant operating inside pi, a coding agent harness. You help users by reading files, executing commands, editing code, and writing new files.

**唯一受 `customPrompt` 影响的总开关**：设置了自定义 prompt（`--system-prompt` / `SYSTEM.md`）时，preamble 使用自定义文本，且 `tools`/`rules`/`docs` 三个 section **全部跳过**（system-prompt.ts:143-161 的 if/else 分支）——只保留 preamble + addendum + project_context + skills + cwd。

### 3.2 `tools` —— 激活工具速览

每个**激活的**工具贡献一行 `- name: snippet`。内置 8 工具的贡献定义（各工具源文件中的 `*ToolSystemPromptContribution` 常量）：

| 工具 | snippet | guidelines（进 rules） |
|------|---------|----------------------|
| `read` | Read file contents | Use read to examine files instead of cat or sed. |
| `bash` | Execute bash commands (ls, grep, find, etc.) | You can inspect PI_* environment variables for current model and session details. |
| `edit` | Make precise file edits with exact text replacement, including multiple disjoint edits in one call | 4 条（见 3.3） |
| `write` | Create or overwrite files | Use write only for new files or complete rewrites. |
| `grep` | Search file contents for patterns (respects .gitignore) | （空） |
| `find` | Find files by glob pattern (respects .gitignore) | （空） |
| `ls` | List directory contents | （空） |
| `powershell` | Execute PowerShell commands | 同 bash 的 PI_* 提示 |

要点：

- 这是**速览不是定义**：完整参数 schema 通过 provider API 的 tools 字段单独传输，不进 prompt
- 扩展/自定义工具声明 `promptSnippet` 时同样出现在此 section
- 尾部固定一句 "In addition to the tools above, you may have access to other custom tools depending on the project."
- 无任何可见工具时渲染 `(none)`

### 3.3 `rules` —— 行为规则（动态拼装）

`buildRules()`（system-prompt.ts:81）按序拼装，**去重**（规范化后 Set 过滤）后逐条 `- `：

1. **工具适配规则**（按激活组合）：
   - 只有 bash/powershell 而无 grep/find/ls 时 → "Use bash for file operations like ls, rg, find"
   - 同时有 bash 和 powershell → "Use bash or PowerShell for file operations..."
   - 只有 powershell → "Use PowerShell for file operations..."
2. **各激活工具的 guidelines**（按 selectedTools 顺序）：edit 的 4 条精确编辑规则——
   - Use edit for precise changes (edits[].oldText must match exactly)
   - When changing multiple separate locations in one file, use one edit call with multiple entries in edits[] instead of multiple edit calls
   - Each edits[].oldText is matched against the original file, not after earlier edits are applied. Do not emit overlapping or nested edits. Merge nearby changes into one edit.
   - Keep edits[].oldText as small as possible while still being unique in the file. Do not pad with large unchanged regions.
3. **promptGuidelines**（扩展经 `before_agent_start` 的 `systemPromptOptions.promptGuidelines` 注入）
4. **固定收尾**：
   - Be concise in your responses
   - Show file paths clearly when working with files

### 3.4 `docs` —— pi 自身文档索引

指向**安装路径**（`getPackageDir()` 解析：`~/.pi/agent/npm/...` 或开发路径），内容：

- 三个文档入口：README.md（主文档）、docs/（附加文档）、examples/（扩展示例/自定义工具/SDK）
- **路径解析规则**：读 pi 文档时 `docs/...` 和 `examples/...` 要解析到安装路径下，不是当前工作目录
- **主题路由表**：问到某主题时读哪个文件（十来项映射）——extensions→`docs/extensions.md`、themes、skills、prompt templates、TUI、keybindings、SDK、custom providers、adding models、pi packages、environment variables
- **阅读纪律**：处理 pi 相关任务先读文档和示例、跟随 .md 交叉引用、完整读完 pi 的 .md 文件

这是 pi "自解释" 能力的来源——README 里说 "you can also ask the agent to explain itself"，模型被问到 pi 时知道去哪里读。

### 3.5 `addendum`（可选）—— 用户追加文本

来源（多来源拼接）：

- CLI `--append-system-prompt <text>`
- `~/.pi/agent/APPEND_SYSTEM.md`（全局）
- `.pi/APPEND_SYSTEM.md`（项目，需信任）

### 3.6 `project_context`（可选）—— 上下文文件

**发现顺序与优先级**（resource-loader.ts）：

1. `~/.pi/agent/AGENTS.md`（全局）
2. 父目录向上遍历（cwd 到根）
3. 当前目录

每目录的候选优先级：`AGENTS.override.md` > `AGENTS.md` > `AGENTS.MD` > `CLAUDE.md` > `CLAUDE.MD`（override 只压制同目录的其他候选，不压制其他目录）。所有命中文件**全部拼接**，每个用 `<project_instructions path="...">` 包裹。`--no-context-files` / `-nc` 禁用。

### 3.7 `skills`（可选）—— 技能目录（惰性）

出现条件：有技能 **且** read 或 bash 至少一个激活（system-prompt.ts:165-169）。

内容是**目录而非正文**：每个可见技能的 name / description / location 三元组（`formatSkillsForPrompt`，skills.ts:355）。技能正文由模型按需用 read/bash 工具现读——`/skill:name` 命令也是把文件内容包装成 `<skill>` 块注入 user 消息，不走 system prompt。

`disableModelInvocation: true` 的技能不进目录（仅供用户 `/skill:` 调用）。

### 3.8 `cwd` —— 工作目录

一行，反斜杠归一化为正斜杠（Windows 兼容）。会话生命周期内不变。

### 3.9 自定义 section

扩展/调用方通过 `sections` 选项注入任意 section，名字必须匹配 `^[a-z][a-z0-9_-]*$` 且不能是 `preamble`（system-prompt.ts:136-139 校验）。空内容不渲染。

## 4. 渲染与传输

### 三种形态

| 形态 | 载体 | 场景 |
|------|------|------|
| 结构化 sections | `SystemMessage.sections` | transcript 持久化、diff、重放 |
| 完整文本 | `getSystemMessageText()`（text.ts:15） | 转成 provider 请求 |
| 补丁消息 | `renderSystemMessageUpdate()`（text.ts:28） | mid-conversation 更新（provider 支持时） |

### Provider 侧两条路径（`resolveTranscript`）

- **collapse（默认）**：重放所有 system 补丁 → 合成**一个** leading system message，后续 system 消息全部丢弃。Anthropic 上这个头部带独立 `cache_control` 断点（tools 段之后）
- **mid-conversation（已建未启用）**：`supportsMidConvoSystemMessages` 开启时，变更渲染为对话中间的 "Updated system prompt section X" 消息，头部前缀不动

### 强制 prompt（`forceSystemPrompt`）

`before_agent_start` 返回 `systemPrompt` 时，transcript 仍记录结构化 sections，但请求投影为单一 head 文本（`_installAgentForcedPromptProjection`，agent-session.ts:1141）。

## 5. 变更机制：diff 补丁

每次 `prompt()` 前都重新推导期望 sections，然后 diff（`diffSystemPromptSections`，system-prompt.ts:204）：

```typescript
const sections = diffSystemPromptSections(
    getCurrentSystemMessage(messages)?.sections ?? {},  // 模型当前有的（transcript 重放）
    buildSystemPromptSections(options),                 // 现在期望的
);
return sections ? { role: "system", content: "", sections, timestamp: Date.now() } : undefined;
```

- **无变化 → undefined → 不落消息 → 请求前缀逐字节不变**
- 有变化 → 只落变化的 section（null 表示删除）
- 重复推导幂等：重新计算本身零成本

## 6. 配置入口

| 入口 | 作用 |
|------|------|
| `--system-prompt <text>` / `SYSTEM.md` | 替换 preamble 并跳过 tools/rules/docs |
| `--append-system-prompt <text>` / `APPEND_SYSTEM.md` | addendum section |
| `--tools` / `--exclude-tools` / `-nbt` / settings `defaultTools` | 影响 tools + rules |
| `--no-context-files` / `-nc` | 禁用 project_context |
| `--no-skills` / `--skill <path>` / settings `skills` | 影响 skills |
| cwd | 直接决定 cwd section |
| `pi.skills` / 扩展 `resources_discover` / `setActiveTools` | 间接影响 skills / tools |
| 扩展 `before_agent_start` 返回 `systemPromptOptions` 或 `forceSystemPrompt` | 每轮可变覆盖 |

## 7. 缓存稳定性分析

### 确定性保证

所有 section 均为静态输入的纯函数产物，**无任何**：

- 日期/时间戳注入（需要时间感知时模型用 bash `date` 现查）
- 文件监听（AGENTS.md/skills/SYSTEM.md 不 watch，`/reload` 才重读）
- 环境漂移（版本号、遥测、网络状态）

跨天、重启、resume——只要下述通道未触发，prompt 字节完全不变。

### 变化通道（全部显式）

| 通道 | 触发方式 | 变化范围 |
|------|---------|---------|
| 用户操作 | `/reload`、工具开关、`pi install/remove` | 对应 section，一次补丁 |
| 扩展 API | `setActiveTools`、`resources_discover`、`before_agent_start` | 显式代码调用 |
| 会话事件 | `/new` / `/resume` / `/fork` 重建 runtime | 新前缀新缓存（结构性） |

### 缓存代价

- **无变化**：零成本（diff 幂等）
- **collapse 路径下任何 section 变化**：全量 miss（system 在位置 0）——例外：Anthropic 的 tools 段有独立断点，工具不变仍命中
- **`forceSystemPrompt` 每轮重写**：每轮全量 miss（应避免用于动态数据）

### 设计意图

system prompt 是**会话内的不可变基线**：变化必须是"某段代码明确要求改变"的事件，而非环境漂移的副作用。缓存稳定是这一设计的直接受益。
