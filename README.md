<div align="center">
  <h1>EdgeClaw: Personal AI Assistant</h1>
  <p>
    <img src="https://img.shields.io/badge/language-C-blue" alt="Language">
    <img src="https://img.shields.io/badge/build-Bazel-green" alt="Build">
    <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  </p>
</div>

**EdgeClaw** is a personal AI assistant agent written in C. It runs an LLM with
tool-calling capabilities and executes filesystem operations, shell commands,
scheduled tasks, remote-device commands, and to-do management. Input arrives
from the terminal frontend (FTXUI TUI) or from scheduled cron jobs.

Built with an actor-model architecture for clean concurrency, modular design
for extensibility, and static linking for portable deployment.

Inspired by [nanobot](https://github.com/HKUDS/nanobot) and [OpenClaw](https://github.com/openclaw/openclaw).

## Architecture

EdgeClaw uses an **actor-based, message-passing architecture built on libuv**.
Each component is an actor with its own mailbox (`EdcQueue` + mutex) and a
`uv_async_t` wakeup handle; all actors are attached to a **single shared
`uv_loop`** running in one thread (`edc_actor_runner_t`). `edc_actor_send()`
enqueues a message and wakes the target actor, so messages are safe to send
from any thread (the LLM HTTPS worker thread, the device worker thread, the
TUI thread) while each actor's state is only ever touched serially on the
runner thread.

```
                         ┌─────────────────────────────────────────────┐
                         │        edc_actor_runner (1 thread)          │
                         │         shared uv_loop + uv_async           │
                         │                                             │
                         │   ┌───────────┐   ┌───────────┐             │
   user input (TUI /     │   │ Scheduler │◀──│ Channel   │◀── adapter  │
   -c / script / cron) ──┼──▶│ (核心编排) │   │ Actor     │    callback │
                         │   └───┬───┬───┘   └───────────┘             │
                         │       │   │                                 │
                         │   ┌───▼───▼────────────┐   ┌────────────┐   │
                         │   │      Agent         │   │ Toolbox    │   │
                         │   │ (装配+回调注入层)   │   │ Actor      │   │
                         │   └───┬───────────┬────┘   │ (执行工具)  │   │
                         │   ┌───▼──────┐ ┌──▼────────┴───┐         │   │
                         │   │ LLM Actor│ │ Memory Actor  │         │   │
                         │   └───┬──────┘ └───────────────┘         │   │
                         └───────┼─────────────────────────────────────┘
                                 │  HTTPS (独立 worker loop/线程)
                         ┌───────▼────────┐    OpenAI-compatible API
                         │ edc_https_client│◀─────────────────────────▶
                         │ (libuv+mbedtls │    (TLS + HTTP/1.1)
                         │  +llhttp+yuarel)│
                         └────────────────┘

   Remote device agent ──▶ device_shell / device_file tools (TCP 或 PPPoS)
   TUI (FTXUI, C++)     ──▶ edc_c_bridge.h 纯 C 接口 + channel adapter
```

Actor mailbox delivery is FIFO: `edc_actor_msg_t` carries a type, an optional
`sub_type`, the sender (`from`), a `request_id` used by the scheduler to map
async responses back to tasks, the payload (`data`) and a `data_free_fn`
ownership callback.

### Actors

| Actor | File | Responsibility |
|-------|------|----------------|
| **Scheduler** | `edc_actor.c`, `edc_scheduler.c` | Core orchestrator. Routes `CHANNEL_RECV` input, runs the task state machine, tracks `request_id → task` (uthash), serializes task-tree snapshots for the TUI |
| **Agent** | `edc_agent.c` | Composition root. Creates all actors, injects 8 scheduler callbacks (`send_llm` / `send_reply` / `send_tool_message` / `dispatch_tools` / `handle_command` / `trigger_memory` / `save_session` / `snapshot_ready`), restores the active session |
| **Channel Actor** | `edc_channel_actor.c`, `edc_channel_adapter.c` | Routes outbound `CHANNEL_SEND` messages to the connected frontend via a registered adapter callback; delivers inbound input to the scheduler |
| **LLM Actor** | `edc_llm_actor.c` | Sends chat-completions requests through the async HTTPS client; reports finish reasons and tool calls |
| **Toolbox Actor** | `edc_toolbox_actor.c` | Executes tool-call chains one by one (`edc_toolbox_actor_req` linked list), echoing each result back as `TOOL_RESPONSE` |
| **Memory Actor** | `edc_memory_actor.c` | Two compression paths: global memory compress (`/session new`, writes MEMORY.md/HISTORY.md) and per-group L1 fold (summarizes completed task turns into `group->memory`) |

### Message Types

Defined in `src/agent/edc_actor.h` (`edc_actor_msg_type_t`):

| Message | Direction | Meaning |
|---------|-----------|---------|
| `BC_ACTOR_MSG_START` | runner → all | Actor started on the loop |
| `BC_ACTOR_MSG_USER_INPUT` | frontend | Raw user input (legacy path) |
| `BC_ACTOR_MSG_LLM_REQUEST` | agent → llm | Build + send an LLM chat request |
| `BC_ACTOR_MSG_LLM_RESPONSE` | llm → scheduler | Finish reason + assistant message / tool calls |
| `BC_ACTOR_MSG_TOOL_REQUEST` | scheduler → toolbox | Execute one tool (or a chain via `req->next`) |
| `BC_ACTOR_MSG_TOOL_RESPONSE` | toolbox → scheduler | Tool result, keyed by the originating `request_id` |
| `BC_ACTOR_MSG_CHANNEL_SEND` | scheduler/agent → channel | Outbound message for the frontend |
| `BC_ACTOR_MSG_CHANNEL_RECV` | channel → scheduler | Inbound user message |
| `BC_ACTOR_MSG_MEMORY_COMPRESS` | scheduler/commands → memory | Trigger global memory compression |
| `BC_ACTOR_MSG_MEMORY_DONE` | memory → scheduler | Compression finished; reload history |
| `BC_ACTOR_MSG_TASK_SNAPSHOT` | TUI → scheduler / scheduler → TUI | Request/push serialized task-tree snapshot |
| `BC_ACTOR_MSG_TOOL_EMIT` | toolbox → scheduler | Tool echo, gated by the active-task check before forwarding |
| `BC_ACTOR_MSG_GROUP_FOLD` | scheduler → memory | Fold a group's completed task turns |
| `BC_ACTOR_MSG_GROUP_FOLD_DONE` | memory → scheduler | Fold finished; persist state |

### Data Flow

1. A user message arrives at the **Channel Actor** — from the TUI adapter
   callback or a scheduled cron job.
2. The Channel Actor delivers it to the **Scheduler** as `CHANNEL_RECV`.
3. The Scheduler attaches the input to a waiting task (or creates one): plain
   text creates/attaches an LLM task; `/`-prefixed input and `@group`
   addressing go to the **command handler** (shell module: tokenizer → parser
   → AST → executor).
4. For an LLM round, the agent's `send_llm` callback builds the context (group
   memory summary, system prompt, safe-window history with tool-boundary
   protection, `|`-JOIN dependency injection, task todo injection) and sends
   `LLM_REQUEST` to the **LLM Actor**.
5. The LLM Actor calls the OpenAI-compatible API through the async HTTPS
   client and replies with `LLM_RESPONSE` (finish reason + message + optional
   tool calls).
6. If the model requested tools, the Scheduler dispatches `TOOL_REQUEST` to
   the **Toolbox Actor**, which executes each tool (results are truncated to
   `tool_max_output_size`). Tool echoes go through `TOOL_EMIT` → Scheduler →
   active-task gate → `CHANNEL_SEND`. Only after the *last* tool in a chain
   completes does the Scheduler resume the LLM loop.
7. On `finish_reason == stop`, the turn is committed (synced to group history
   and the session), the task moves to `COMPLETED`, the reply is sent to the
   user, dependents are notified, L1 fold / save-session triggers run.
8. The **Memory Actor** handles `MEMORY_COMPRESS` (global) and `GROUP_FOLD`
   (per-group) summarization as history grows.

### Task & Session Model

Three-layer Unix-inspired model — see `docs/edgeclaw.md` for the full
description:

```
Session (会话) → Group (进程组, 共享 group history) → Task (LLM 任务)
```

Task states: `IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING ⇄
TOOL_CALLING → COMPLETED / CANCELLED / ERROR / SUSPENDED`. Tasks form a DAG
through `deps`/`dependents` with cycle detection; `|`/`;`/`&` and redirections
map onto dependency types (`JOIN`/`SEQUENCE`) and output files, mirroring a
Unix shell. Unfinished tasks are persisted on exit and parked as `SUSPENDED` on
load; `/task resume [tid]` re-derives their state (deps / tool chain / turn).
A task that failed mid-run is `ERROR` but keeps its half-finished turn;
`/task resume` continues it.

Task ids are **globally unique across sessions** (persisted high-water mark in
`<workspace>/.task_seq`), so a dependency may point at a task living in a
**different session**. Every dependency-creating path — `/task new -d <id>`,
`/task chain`, `/task join`, `$N` / `$VAR`, and `|` / `&&` — resolves the id
globally, and so do `/task status`, `/task output`, `/task stop` and
`/task resume` (the latter report the owning session). A referenced session that
exists on disk but has not been loaded in this run is **activated on demand**
(its `session.json` is probed and lazy-loaded). An id that cannot be resolved
fails the command — dependencies are never silently dropped. Commands that
change the *active session's* foreground view (`/task fg`, `/task bg`,
`/task fork`) instead require the task to belong to the active session and
refuse otherwise. The **foreground group** is a property of the session (its own
tab), never mirrored on the scheduler.

> Note: `/task new` parses flags only after a quoted input, so a cross-session
> dependency is written `/task new """<description>""" -d <task_id>`.

## Features

### Frontends & Interfaces

| Interface | Binary | Description |
|-----------|--------|-------------|
| **TUI** | `//src/tui:edgeclaw_tui` | Interactive terminal UI (FTXUI): chat pane + task/todo sidebar, driven by scheduler task-tree snapshots. Primary daily driver — **this is the released binary** (packaged as `edgeclaw`) |
| **Device agent** | `//src:device_agent` | Remote shell/file execution over TCP socket or PPPoS/lwIP |

There is currently **one** channel implementation (`edc_channel_adapter`); the
enum `edc_channel_type` and config `channel.type` reflect that. A new frontend
registers an adapter callback (`edc_channel_adapter_init`) on the Channel
Actor. Telegram/Feishu/QQ integrations do not exist in this codebase.

### LLM Agent Loop

- OpenAI-compatible chat completions API (non-streaming)
- Tool-calling loop: tools execute serially, results feed the next iteration
- Custom `finish_reason` handling: `stop`, `tool_calls`, `length`,
  `sensitive`, `network_error`, `context_exceeded`, `unknown`
- Context windowing with tool-boundary protection and per-group L1 folding

### Tool System

Extensible tool framework with automatic OpenAI function-calling schema
generation (typed params via `edc_param_types`):

| Tool | Description |
|------|-------------|
| Filesystem | `read_file`, `write_file`, `edit_file`, `list_dir` |
| Shell | `exec_shell` (async `uv_process` with timeout) |
| Cron | `crontab` schedule/manage one-shot & recurring jobs (`jobs.json`) |
| Todo | `todowrite` (persisted on the calling task, live in snapshots) |
| Device | `device_shell`, `device_upload`, `device_download` |

Tools are registered dynamically; see "Adding a New Tool" below.

### Sandboxing

Tool execution enforces **sandbox path verification** (`edc_sandbox_verify_path`)
against the configured sandbox policy in the fs/shell/device tools on all
platforms (gated by the agent's `enable_sandbox` flag). The policy is three path
lists — **read-only**, **read-write** and **deny** — stored under the `sandbox`
key in `config.json` and editable at runtime with `/sandbox` (see below). A path
is decided by the **most specific matching entry** (longest prefix wins), and a
**deny** entry beats every allow entry, for reads and writes alike.

On Linux, when the sandbox is enabled, the shell tool additionally runs each
command through a tiny embedded **sandbox helper**
(`src/agent/sandbox_helper.c`): the agent execs the helper, which applies the
kernel sandbox in its own fresh, single-threaded process and then execs
`/bin/sh`. This is the one place kernel isolation can be applied safely:
`edc_sandbox_apply()` must run in the confined process (it is irreversible and
inherited, so it must never run in the agent), and doing it between `fork()` and
`exec()` in the multithreaded agent is unsafe (only async-signal-safe calls are
allowed there) — applying it after an `exec` sidesteps both. `exec` keeps the
pid, so the existing process-group kill / exit handling is unchanged. The helper
is embedded in the binary at build time (`tools/embed_blob.py`) and extracted to
`$EDC_HOME/.cache/` on first use, so the release stays a single file.

The helper applies, in order:

1. **Mount namespace** (`edc_ns_sandbox.c`) — unshares a user + mount namespace,
   remounts `/` read-only and bind-mounts every read-write path back. This makes
   the read-only set immutable at the VFS level (unlink/chmod/chown/xattr on it
   fail with `EROFS`) and **masks deny paths** (an empty mode-000 tmpfs over each
   denied directory, an empty file over each denied file) — the only layer that
   can deny a path inside an allowed parent. It then verifies the outcome (`/`
   really read-only, every rw path really writable).
2. **Landlock** (`edc_landlock_sandbox.c`) — an additive allow-list over the
   same read-only / read-write sets.
3. **seccomp-BPF** (`edc_bpf_sandbox.c`) — denies the capability/escape class
   (ptrace, bpf, module loading, mount/setns, io_uring, keyring, `pidfd_getfd`,
   cross-process memory access, ...) plus the metadata mutators Landlock cannot
   mediate (chmod/chown/utimes/xattr, `ioctl(FS_IOC_SETFLAGS)`). Those metadata
   rules are path-blind, so they are installed only as a **fallback** when the
   mount namespace is not enforced; with it active the read-only paths already
   return `EROFS`.

`edc_sandbox_apply()` runs the same three layers but is **reserved and currently
unused** — the active path is the helper. macOS/Windows skip kernel isolation
(path verification only).

The **default writable** set is the agent's own workspace
(`$EDC_HOME/workspace`), the directory EdgeClaw was launched from, `/tmp`,
`/dev`, and the user's `~/.cache` (so build/package-manager caches such as
bazel's keep working under the sandbox). The **default read-only** set is a
system whitelist (`EDC_SANDBOX_DEFAULT_RO_PATHS`: `/usr`, `/lib*`, `/bin`,
`/sbin`, `/etc`, `/proc`, `/sys`, `/run`) plus the user's home directory; `/run`
is included because `/etc/resolv.conf` is usually a symlink into it. The whole
filesystem is **not** readable by default — `/` is intentionally absent.

The launch directory is the sandbox root and the default working directory for
tools; there is no separate sandbox directory. The read-only home stays
read-only while the workspace (which lives under it) stays writable, because the
longest-prefix rule lets the more specific writable entry win. Landlock only
unions access rights, so it cannot carve a read-only subtree out of a writable
parent — the deny list plus the namespace masking cover that case instead.

**Working directory is session-scoped.** Every session binds a working
directory when it is created — `/session new [name] [dir]`. With no `dir` the
session binds the launch directory (today's behavior); with a `dir` it binds
that directory instead. A relative `dir` is resolved to an absolute path
against the **process launch directory** (today's cwd), not the active
session's workdir, so the anchor is stable no matter which session is current;
`~` expands to the home directory. If it does not resolve to an existing
directory the command **fails** and no session is created — it never silently
falls back to the default. The bound directory is granted read+write in the
sandbox (`edc_config_add_sandbox_rw()`, deduplicated like the launch
directory), persisted in the session's `session.json`, and restored (re-granted)
when the session is switched back to. It then becomes the `exec_shell` default
`cwd`, the base that the fs tools resolve **relative** paths against, and the
value of `{{cwd}}` (the identity prompt's "Current Running Directory").
Different sessions therefore operate in different directories, giving the
session abstraction a concrete meaning.

Writes the agent itself performs into model-writable directories (chat
transcript, input history, `history.json`, crontab `jobs.json`, `MEMORY.md` /
`HISTORY.md`) go through symlink-safe helpers (`edc_file_fopen_safe()` /
`edc_file_write_safe()`: `O_NOFOLLOW`, confined to the workspace) so a planted
symlink cannot redirect them into a protected path. Commands spawned by the
shell tool receive a **minimal environment whitelist** (`edc_shell_env.c`:
`PATH`, `HOME`, `USER`, `LOGNAME`, `SHELL`, locale, `TERM`, `TZ`, `TMPDIR`)
instead of the agent's full environment, so secrets exported in the user's shell
(e.g. an API token) do not leak into every child.

Manage the policy at runtime (persisted to `config.json`, effective for
subsequently spawned commands):

- `/sandbox` — show status plus the read-write / read-only / denied lists
- `/sandbox list` — list the three lists
- `/sandbox on|off` — enable/disable the sandbox
- `/sandbox add-ro|rm-ro <dir>` — add/remove a read-only directory
- `/sandbox add-rw|rm-rw <dir>` — add/remove a read-write directory
- `/sandbox add-deny|rm-deny <dir>` — mask a directory (wins over ro/rw, e.g. `~/.ssh`)

### Additional Features

- **Context compression**: automatic history summarization (global MEMORY.md +
  per-group L1 fold) to stay within LLM context limits
- **Encrypted secrets**: AES-256-GCM for config secrets via mbedtls
- **Skills system**: markdown skill definitions with YAML frontmatter augment
  the system prompt
- **Markdown rendering**: md4c-based rendering in the TUI frontend
- **Internal commands**: slash commands (`/session ...`,
  `/task ...`, `/connect`, `/sandbox ...`) and shell-like syntax (`|`, `;`, `&`, redirection, `$VAR`,
  `!n`/`!!`)
- **Cross-session task dependencies**: task ids are globally unique, so
  `-d`/`|`/`&&`/`$N` plus `/task chain` and `/task join` can reference a task in
  another session; its session is activated on demand. `fg`/`bg`/`fork` remain
  scoped to the active session, while `status`/`output`/`stop`/`resume` work
  across sessions.
- **Cron scheduling**: one-shot (`at`) or recurring (`cron_expr`/
  `every_seconds`) agent tasks via `ccronexpr`
- **First-run setup**: `config.json` is created with sensible defaults (no prompts); configure the provider — API URL, model, key — later with `/connect`
- **Cross-compilation**: armv7/aarch64/amd64 for Linux; aarch64 for macOS;
  amd64 for Windows
- **Static binary**: statically linked on Linux for portable deployment

## Project Structure

```
edgeclaw/
├── src/
│   ├── core/                         # 基础层 (无 agent 依赖)
│   │   ├── edc_config.c/h            # config.json 解析
│   │   ├── edc_secret.c/h            # 加密配置值 (AES-256-GCM, mbedtls)
│   │   ├── edc_https_client.c/h      # 异步 HTTPS 客户端 (libuv+mbedtls+llhttp+yuarel)
│   │   ├── edc_init_env.c/h          # 首启向导 + 释放内嵌 workspace
│   │   ├── edc_containers.c/h        # EdcQueue 等容器
│   │   ├── edc_markdown_render.c/h   # Markdown 渲染 (md4c)
│   │   ├── edc_param_types.c/h       # 工具参数类型 / schema 生成
│   │   └── edc_log/error/string/file_utils/datetime/terminal ...
│   ├── agent/                        # Actor 与编排
│   │   ├── edc_actor.c/h             # Actor 基础设施 (mailbox, uv_async, runner)
│   │   ├── edc_agent.c/h             # 装配层: 建 actor、注入 scheduler 回调
│   │   ├── edc_scheduler.c/h         # 调度核心: 状态机、request_id 映射、快照
│   │   ├── edc_channel_actor.c/h     # 通道消息路由
│   │   ├── edc_channel_adapter.c/h   # TUI/frontend adapter
│   │   ├── edc_llm_actor.c/h         # LLM 请求 actor
│   │   ├── edc_toolbox*.c/h          # 工具注册 / 链式执行 actor
│   │   ├── edc_tools_fs.c/h          # 文件工具
│   │   ├── edc_tools_shell.c/h       # shell 工具
│   │   ├── edc_tools_crontab.c/h     # cron 工具
│   │   ├── edc_tools_todo.c/h        # todowrite 工具
│   │   ├── edc_tools_device_*.c/h    # 远程设备工具
│   │   ├── edc_task.c/h              # Task (Unix 进程式任务)
│   │   ├── edc_task_group.c/h        # Group (进程组)
│   │   ├── edc_turn.c/h              # Turn (单轮消息)
│   │   ├── edc_session*.c/h          # Session 生命周期 + 多会话索引
│   │   ├── edc_state_serializer.c/h  # session.json 序列化/反序列化
│   │   ├── edc_memory_actor.c/h      # 记忆压缩 (全局 + L1 fold)
│   │   ├── edc_context.c/h           # system prompt 组装
│   │   ├── edc_history.c/h           # JSONL 历史 I/O
│   │   ├── edc_skills.c/h            # Skills 加载
│   │   └── edc_*sandbox.c/h          # 沙箱后端 (Landlock / NS / seccomp-BPF)
│   ├── shell/                        # 命令解析执行 (token→parse→AST→exec)
│   │   ├── edc_shell_token.c/h       # 分词器
│   │   ├── edc_shell_parse.c/h       # 解析器
│   │   ├── edc_shell_ast.c/h         # AST 执行
│   │   ├── edc_cmd_parse.c/h         # task 选项解析 (-d/-g/--bg/...)
│   │   └── edc_commands.c/h          # / 命令处理器
│   ├── device/                       # 远程 device agent
│   │   ├── device_agent.c            # 设备端 agent (fork/exec、base64 传输)
│   │   ├── edc_socket_agent.c/h      # TCP socket 传输
│   │   ├── edc_pppos_agent.c/h       # PPPoS/lwIP 传输
│   │   └── edc_lwip.c/h
│   ├── tui/                          # FTXUI 前端 (C++, 仅经 edc_c_bridge.h 纯 C 接口)
│   │   ├── main.cpp                  # TUI 入口
│   │   ├── edc_c_bridge.c/h          # C ⇄ C++ 桥接
│   │   ├── edc_tui_window.cpp/hpp    # 主窗口 (chat + 侧栏)
│   │   ├── edc_chat_widget.cpp/hpp   # 对话区
│   │   ├── edc_task_widget.cpp/hpp   # 任务树侧栏
│   │   └── edc_todo_widget.cpp/hpp   # todo 侧栏
│   └── BUILD
├── tests/                            # C/C++ 测试 (bazel test //tests/...)
├── thirdparty/                       # 第三方源码 (vendored)
│   ├── libuv/                        # 事件循环
│   ├── llhttp/  libyuarel/           # HTTP/1.1 解析 + URL 解析
│   ├── sds/                          # 动态字符串
│   ├── uthash/                       # 哈希/链表宏
│   ├── ccronexpr/                    # cron 表达式解析
│   ├── md4c/                         # Markdown 解析
│   ├── utf8proc/                     # Unicode 宽度
│   ├── lwip/                         # PPPoS 设备链路
│   ├── brutezone/                    # timezone 数据库 (crontab tz)
│   └── log.c/                        # 日志
#   注: cJSON / mbedtls / ftxui 经 MODULE.bazel (bzlmod) 拉取, 非 vendored
├── patches/                          # 依赖补丁
├── toolchain/                        # 交叉编译工具链
├── docs/                             # 用户手册 (edgeclaw.md) + man page + TUI 设计文档
├── workspace/                        # 默认工作区 (编译期打包进二进制, 首次运行释放到 ~/.edgeclaw/)
│   ├── AGENTS.md  SOUL.md  USER.md  TOOLS.md  HEARTBEAT.md
│   ├── memory/   skills/   ssl/
├── MODULE.bazel                      # Bazel 模块 (bzlmod)
└── BUILD                             # 顶层构建与打包
```

## Getting Started

### Download Binary (Quick)

Pre-built static binaries for Linux (amd64, armv7, aarch64) and macOS (aarch64) are available on the [Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page. Download the tarball matching your platform:

Each tarball holds a single self-contained binary — the default workspace is embedded in it and released to `~/.edgeclaw/` on first run, so nothing else needs to be unpacked.

| Platform | Tarball |
|----------|---------|
| Linux x86_64 | `edgeclaw_amd64_linux.tar.gz` |
| Linux aarch64 | `edgeclaw_aarch64_linux.tar.gz` |
| Linux armv7 | `edgeclaw_arm_linux.tar.gz` |
| macOS aarch64 | `edgeclaw_aarch64_macos.tar.gz` |

```bash
# Replace {ARCH}_{OS} with your platform, e.g. edgeclaw_amd64_linux.tar.gz
curl -LO https://github.com/edgeclaw-chat/edgeclaw/releases/latest/download/edgeclaw_{ARCH}_{OS}.tar.gz

# Extract and run (first run creates config with defaults)
tar -xzf edgeclaw_*.tar.gz
cd edgeclaw_*/
./edgeclaw
```

### Build from Source

#### Prerequisites

- **Bazel** (build system)
- C compiler (GCC / Clang); C++17 compiler for the TUI

#### Build

```bash
git clone https://github.com/edgeclaw-chat/edgeclaw.git
cd edgeclaw

# Build the interactive TUI (the released binary) for the host platform
bazel build //src/tui:edgeclaw_tui

# Cross-compile the TUI for aarch64 Linux
bazel build //src/tui:edgeclaw_tui --platforms=//:aarch64_linux

# Build release tarball (a single self-contained TUI binary; workspace is embedded)
bazel build //:edgeclaw
```

### Setup

The released binary is the TUI. On first run (no `config.json`) it creates the config with defaults automatically:

```bash
./edgeclaw
```

The first run does not ask for configuration: `config.json` is written with the canonical defaults (the built-in default provider for the API URL and model, an empty API key). The only prompt is for the master passphrase, unless `EDGECLAW_MASTER_KEY` is set. Configure the provider with the **`/connect`** command: it opens a dialog (prefilled with the current values) for the API URL, model and key, and both persists them to `config.json` and applies them to the running agent.

Configuration is saved to `~/.edgeclaw/config.json`. The default `workspace/` tree is embedded into the binary at build time and released to `~/.edgeclaw/workspace/` on first run, so no source `workspace/` directory is required next to the executable.

### Manual Configuration

Create `~/.edgeclaw/config.json`:

```json
{
  "api_key": "",
  "api_url": "https://api.openai.com/v1/chat/completions",
  "model": "gpt-4o",
  "max_iter": -1,
  "group_fold_msg_threshold": 500,
  "tool_max_output_size": 10000,
  "sandbox": {
    "enable": true,
    "rw_paths": ["/home/<user>/.edgeclaw/workspace", "/tmp", "/dev", "/home/<user>/.cache"],
    "ro_paths": ["/usr", "/lib", "/lib32", "/lib64", "/libx32", "/bin", "/sbin", "/etc", "/proc", "/sys", "/run"],
    "deny_paths": []
  },
  "channel": {
    "type": "adapter"
  }
}
```

`max_iter` controls the LLM/tool iteration budget per user input: `-1`
(default) means **unlimited** (the agent loop keeps running until the model
produces a final answer), while a positive integer caps the number of
LLM/tool rounds before the scheduler stops and asks for a follow-up.

#### Channel Configuration

Only `channel.type = "adapter"` is supported (used by the TUI / frontend
adapter). `cli`, `telegram`, `feishu`, and `qq` are **not** supported.

Optional remote-device settings for the `device_shell` / `device_file` tools:

```json
"device_transport": "socket",   // or "pppos"
"device_ip": "192.168.1.50",
"device_port": 5000,
"device_pty": "/dev/ttyUSB0",   // PPPoS transport only
"device_baud": 115200
```

### Run

On first run, EdgeClaw releases the default workspace (embedded in the binary at build time) to `~/.edgeclaw/workspace`; if `config.json` is missing it is created with defaults.

**Interactive TUI:**

```bash
bazel run //src/tui:edgeclaw_tui
```

**Production / release tarball (the TUI):**

```bash
# Build the release tarball (a single self-contained TUI binary; workspace is embedded)
bazel build //:edgeclaw

# Extract and run
tar -xzf bazel-bin/edgeclaw_*.tar.gz
cd edgeclaw_*/
./edgeclaw
```

---

## Usage

### Running the Agent

**Interactive TUI** (recommended): type messages in the chat pane; the agent
responds and updates the task/todo sidebar. See the quick-start section of
`docs/edgeclaw.md`. The TUI binary takes no command-line arguments.

### Master Key & Encrypted Secrets

Sensitive config values (API keys, passwords) can be stored encrypted in `config.json`. Encryption is powered by **AES-256-GCM** via mbedtls.

EdgeClaw derives the encryption key from a master passphrase using HKDF-SHA256. The key is sourced in this order:

1. **Environment variable** (`EDGECLAW_MASTER_KEY`) — for scripting / Docker deployments. The variable is cleared from the environment after reading.
2. **Interactive TTY prompt** — prompted on startup if no env var is set.

On first setup, you will be prompted to create a passphrase. A verification hash is stored in `~/.edgeclaw/.key_hash` to validate the passphrase on subsequent runs. The master key is locked in memory with `mlock()` and kept for the lifetime of the process, so the `/connect` command can re-encrypt the API key when you change it at runtime.

**Format in `config.json`:**

```json
{
  "llm_api_conf": {
    "api_key": "EDGECLAW_ENC:<base64-encrypted-blob>"
  }
}
```

### Interacting

Start the TUI and type a message, or use `-c` for a one-shot question. Each
conversation turn may involve multiple internal tool-calling iterations. The
agent only responds once it has a final answer. Tool echo messages (TOOL
type), LLM replies (RESP), and internal system/thought messages are displayed
in the chat pane; a task tree and the active task's todo list live in the
right sidebar.

### Scheduled Tasks (Cron)

EdgeClaw can schedule one-shot or recurring tasks. These are typically created by the LLM agent through its `crontab` tool, but can also be managed directly through conversations:

**Scheduling a one-shot reminder** (via the agent):

> "Remind me to check the server at 3pm tomorrow"

The agent will call the `crontab` tool with `action: "add"` and `at: "2026-05-29T15:00:00"`. When the timer fires, EdgeClaw sends the reminder message back through the connected frontend.

**Periodic / recurring tasks** (via the agent):

> "Check the weather every morning at 8am"

The agent calls `crontab` with `action: "add"` and `cron_expr: "0 8 * * *"` or `every_seconds: 86400`.

**Listing scheduled jobs:**

> "Show me my scheduled tasks"

The agent calls `crontab` with `action: "list"`.

**Removing a job:**

> "Cancel my weather reminder"

The agent calls `crontab` with `action: "remove"` and the corresponding `job_id`.

All scheduled jobs persist in `~/.edgeclaw/workspace/jobs.json` and survive restarts. The crontab tool supports:
- **`at`**: ISO 8601 datetime for one-shot execution
- **`cron_expr`**: Standard cron expression (e.g. `0 2 * * *`)
- **`every_seconds`**: Fixed interval in seconds
- **`tz`**: IANA timezone (e.g. `Asia/Shanghai`)

### Heartbeat Tasks

Defined in `~/.edgeclaw/workspace/HEARTBEAT.md`, these are checked every 30 minutes. The LLM reads and acts on tasks in a to-do format:

```
- [ ] Check calendar and remind of upcoming events
- [ ] Scan inbox for urgent emails
```

The agent processes each task and marks completed items. Keep the file small to minimize token usage.

Use heartbeat tasks for recurring maintenance — no cron expression needed; the agent just runs them periodically.

### Environment Variables

| Variable | Description |
|----------|-------------|
| `EDGECLAW_MASTER_KEY` | Master passphrase for secret decryption (cleared after reading) |

## Workspace

The `~/.edgeclaw/` workspace directory contains:

| File | Purpose |
|------|---------|
| `AGENTS.md` | Agent behavior instructions and tool usage guidelines |
| `SOUL.md` | Personality definition and values |
| `USER.md` | User profile and preferences |
| `TOOLS.md` | Tool usage documentation |
| `HEARTBEAT.md` | Periodic tasks (checked every 30 minutes) |
| `memory/` | Long-term memory storage |
| `skills/` | Custom skill definitions (markdown + YAML frontmatter) |
| `ssl/cacert.pem` | TLS CA certificate bundle for HTTPS connections |
| `sessions.json` | Multi-session index (id / name / created_at + active) |
| `<session_id>/session.json` | Per-session state — the single source of truth (schema v3: groups/tasks/turns/messages) |
| `<session_id>/chat.jsonl` | Per-session TUI chat transcript (to be derived from `session.json`; see `docs/session-model-v3.md`) |
| `jobs.json` | Scheduled cron jobs (created by the `crontab` tool) |

### Skills

Skills are markdown files in `~/.edgeclaw/workspace/skills/` with YAML frontmatter:

```markdown
---
name: skill_name
description: Brief description
key: value
---
# Skill Content

Instructions that augment the agent's system prompt...
```

## Dependencies

Vendored under `thirdparty/` or fetched as Bazel modules (`MODULE.bazel`):

| Library | Purpose |
|---------|---------|
| **libuv** | Event loop, async handles, threads |
| **cJSON** | JSON parsing / serialization |
| **mbedtls** | TLS for HTTPS client; AES-256-GCM for config secrets |
| **llhttp** | HTTP/1.1 parser (HTTPS client) |
| **libyuarel** | URL parsing (HTTPS client) |
| **sds** | Dynamic strings |
| **uthash / utlist** | Hash table & linked-list macros |
| **ccronexpr** | Cron expression parsing |
| **log.c** | Logging |
| **md4c** | Markdown parsing (TUI rendering) |
| **utf8proc** | Unicode width handling (TUI text layout) |
| **lwip** | PPPoS transport for the device agent |
| **brutezone** | Timezone database (`tz` support in crontab) |
| **ftxui** | C++ terminal UI (TUI frontend only) |

## Development

### Code Conventions

- C99 standard (`-std=gnu99`); C++17 for `src/tui/`
- Naming: `edc_module_function()` for public API; `static` for private
- Header guards: `__BC_MODULE_H__`
- Actors receive messages via `edc_actor_send()` and dispatch in their
  `edc_actor_handler_t` on the shared runner loop
- Tools implement an async exec callback and register with the toolbox

### Running Tests

```bash
bazel test //tests/...
```

(Some tests require network/API keys or are platform-specific; they are
skipped/known-failing in restricted environments.)

### Adding a New Tool

1. Create `edc_tools_xxx.c` / `edc_tools_xxx.h` with a registration function:

```c
void edc_tools_xxx_register(struct edc_toolbox *tb)
{
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("param1", edc_param_str_type("...", NULL), true));
    struct edc_tool *tool = edc_tool_new("xxx", "Tool description", params, exec_xxx);
    edc_toolbox_register(tb, tool);
}
```

2. Implement the async exec callback (call the provided callback when done)
   and call `edc_toolbox_register()`.
3. Add source files to the `src/agent/BUILD` library.

### Adding a New Frontend / Channel

1. Implement a channel actor handler for inbound messages and an adapter
   callback for outbound messages (see `edc_channel_adapter.c`).
2. Register the adapter on the channel actor with `edc_channel_adapter_init`.
3. If a new `channel.type` value is desired, extend `edc_channel_type` in
   `edc_config.h` and the parsing in `edc_config.c`.

### Code Formatting

```bash
clang-format -i src/*.c src/*.h
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- Inspired by [nanobot](https://github.com/HKUDS/nanobot)
- Inspired by [OpenClaw](https://github.com/openclaw/openclaw)

---

<p align="center">
  <em>Built with C, powered by actors.</em>
</p>
