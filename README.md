<div align="center">
  <h1>EdgeClaw: Personal AI Assistant</h1>
  <p>
    <img src="https://img.shields.io/badge/language-C-blue" alt="Language">
    <img src="https://img.shields.io/badge/build-Bazel-green" alt="Build">
    <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  </p>
</div>

**EdgeClaw** is a personal AI assistant agent written in C. It connects to multiple chat platforms, processes user messages through an LLM with tool-calling capabilities, and executes filesystem operations, shell commands, scheduled tasks, and to-do management within a sandboxed environment.

Built with an actor-model architecture for clean concurrency, modular design for extensibility, and static linking for portable deployment.

Inspired by [nanobot](https://github.com/HKUDS/nanobot) and [OpenClaw](https://github.com/openclaw/openclaw).

## Architecture

EdgeClaw uses an **actor-based, message-passing architecture** built on GLib's event loop. Each component runs with its own mailbox (`GAsyncQueue`) and dispatches on a shared `GMainContext` thread.

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                                 EdgeClaw                                      │
│                                                                               │
│  ┌──────────────┐    ┌──────────┐    ┌───────────┐    ┌───────────┐           │
│  │ Channel Actor│    │ LLM Actor│    │Toolbox    │    │ Memory    │           │
│  │ ───────────  │    │          │    │Actor      │    │ Actor     │           │
│  │ • Telegram   │    │ OpenAI-  │    │ ────────  │    │ Context   │           │
│  │ • Feishu     │    │ compat.  │    │ • fs      │    │ compress  │           │
│  │ • QQ         │    │ API      │    │ • shell   │    │           │           │
│  │ • CLI        │    │          │    │ • cron    │    │           │           │
│  └──────┬───────┘    └────┬─────┘    │ • todo    │    └─────┬─────┘           │
│         │                 │          └─────┬─────┘          │                 │
│         └─────────────────┴───────┬────────┴────────────────┘                 │
│                                   │                                           │
│                         ┌─────────▼─────────┐                                 │
│                         │      Agent        │                                 │
│                         │  (orchestrator)   │                                 │
│                         └─────────┬─────────┘                                 │
│                                   │                                           │
│                    ┌──────────────┼──────────────┐                            │
│                    │              │              │                            │
│              ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐                      │
│              │  Sandbox  │  │  Skills   │  │  Secrets  │                      │
│              │ • BPF     │  │  Loader   │  │  (mbedtls)│                      │
│              │ • Landlock│  │           │  │           │                      │
│              │ • NS      │  │           │  │           │                      │
│              └───────────┘  └───────────┘  └───────────┘                      │
│                                                                               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                     │
│  │  Commands    │    │  Markdown    │    │  Scheduler   │                     │
│  │  Handler     │    │  Renderer    │    │  (cron fire) │                     │
│  └──────────────┘    └──────────────┘    └──────────────┘                     │
└───────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

1. A message arrives from a chat platform (Telegram polling, Feishu/QQ WebSocket, or local CLI).
2. The **Channel Actor** sends it to the **Agent** as a `CHANNEL_RECV` message.
3. The **Agent** builds the LLM request context (system prompt, skills, history, tools schema) and sends `LLM_REQUEST` to the **LLM Actor**.
4. The **LLM Actor** calls the OpenAI-compatible API via the async network layer (Mongoose HTTP/HTTPS).
5. If the LLM returns tool calls, the Agent dispatches `TOOL_REQUEST` to the **Toolbox Actor**, which executes the tool inside the **sandbox**.
6. Tool results go back to the LLM for another iteration (up to `max_iter`).
7. The final text response is sent back to the user via `CHANNEL_SEND`.
8. The **Memory Actor** monitors history length and triggers compression summaries as needed.

## Features

### Multi-Platform Chat

| Channel | Protocol | Description |
|---------|----------|-------------|
| **Telegram** | HTTP long-polling | Bot API via `getUpdates` |
| **Feishu / Lark** | WebSocket (Protobuf) | Tenant access token + WSS |
| **QQ** | WebSocket | Bot with sharding support |
| **CLI** | stdin/stdout | Interactive local terminal with Markdown rendering |

### LLM Agent Loop

- OpenAI-compatible chat completions API
- Tool-calling with recursive reasoning loop (configurable `max_iter`)
- Streaming and non-streaming responses
- Custom `finish_reason` handling (stop, tool_call, length, sensitive, network_error, context_exceeded)

### Tool System

Extensible tool framework with automatic OpenAI function-calling schema generation:

| Tool | Description |
|------|-------------|
| Filesystem | Read, write, edit, list, delete files |
| Shell | Execute shell commands |
| Cron | Schedule and manage time-based tasks (`ccronexpr`) |
| Todo | Create and manage to-do lists |

Tools are registered dynamically and can be extended by adding new `edc_tool` definitions.

### Sandboxing (Linux)

Three filesystem sandboxing backends for tool execution isolation:

| Backend | Mechanism | Kernel Required |
|---------|-----------|-----------------|
| **Landlock** | Landlock LSM rules | Linux 5.13+ |
| **BPF** | BPF syscall filtering | Linux 4.x+ |
| **Namespaces** | Mount namespace isolation | Linux 3.8+ |

(macOS builds skip sandboxing)

### Additional Features

- **Context compression**: Automatic conversation history summarization to stay within LLM context limits
- **Encrypted secrets**: Config-level AES encryption for API keys and passwords via **mbedtls**
- **Skills system**: Load markdown-based skill definitions with YAML frontmatter to augment the LLM's system prompt
- **CLI Markdown rendering**: Terminal-formatted Markdown output for the interactive CLI channel (via **md4c**)
- **Internal commands**: Slash-command handler for built-in agent commands (e.g. `/setup`)
- **Heartbeat tasks**: Periodic task execution every 30 minutes
- **Cron scheduling**: One-shot or recurring scheduled tasks via `ccronexpr`
- **Setup wizard**: Interactive configuration via `--setup` flag
- **Cross-compilation**: Supports armv7, aarch64, amd64 for Linux; aarch64 for macOS
- **Static binary**: Fully statically linked on Linux for portable deployment

## Project Structure

```
edgeclaw/
├── src/                              # Source code
│   ├── main.c                        # Entry point
│   ├── edc_actor.c/h                 # Actor infrastructure (mailbox, runner)
│   ├── edc_agent.c/h                 # Top-level orchestrator
│   ├── edc_config.c/h                # Configuration loading
│   ├── edc_secret.c/h                # Encrypted config values (mbedtls)
│   ├── edc_init_env.c/h              # Setup wizard
│   ├── edc_context.c/h               # System prompt builder
│   ├── edc_session.c/h               # Conversation session management
│   ├── edc_turn.c/h                  # Single conversation turn abstraction
│   ├── edc_history.c/h               # History file I/O (JSONL)
│   ├── edc_skills.c/h                # Skills loader
│   ├── edc_toolbox.c/h               # Tool registry & schema generation
│   ├── edc_toolbox_actor.c/h         # Tool execution actor
│   ├── edc_tools.c/h                 # Tool base infrastructure
│   ├── edc_param_types.c/h           # Typed parameter system for tool schemas
│   ├── edc_tools_fs.c/h              # Filesystem tools
│   ├── edc_tools_shell.c/h           # Shell execution tool
│   ├── edc_tools_crontab.c/h         # Cron scheduling tool
│   ├── edc_tools_todo.c/h            # To-do management tool
│   ├── edc_sandbox.c/h               # Sandbox interface
│   ├── edc_landlock_sandbox.c/h      # Landlock sandbox backend
│   ├── edc_bpf_sandbox.c/h           # BPF sandbox backend
│   ├── edc_ns_sandbox.c/h            # Namespace sandbox backend
│   ├── edc_network_async.c/h         # Async HTTP/WSS (Mongoose)
│   ├── edc_channel_actor.c/h         # Channel abstraction & dispatch
│   ├── edc_channel_telegram.c/h      # Telegram integration
│   ├── edc_channel_feishu.c/h        # Feishu/Lark integration
│   ├── edc_channel_qq.c/h            # QQ integration
│   ├── edc_channel_cli.c/h           # CLI interactive channel
│   ├── edc_llm_actor.c/h             # LLM API actor
│   ├── edc_memory_actor.c/h          # History compression actor
│   ├── edc_commands.c/h              # Internal slash-command handler
│   ├── edc_markdown_render.c/h       # Markdown-to-terminal renderer
│   ├── edc_log.c/h                   # Log initialization & rotation
│   ├── lark_api.proto                # Feishu WebSocket protobuf schema
│   └── BUILD                         # Bazel build rules
├── workspace/                        # Runtime data (copied to ~/.edgeclaw/)
│   ├── AGENTS.md                     # Agent behavior instructions
│   ├── SOUL.md                       # Personality and values
│   ├── USER.md                       # User profile
│   ├── TOOLS.md                      # Tool documentation
│   ├── HEARTBEAT.md                  # Periodic heartbeat tasks
│   ├── memory/                       # Long-term memory storage
│   ├── skills/                       # Custom skill definitions
│   └── ssl/                          # TLS certificates (cacert.pem)
├── tests/                            # Test suite
├── thirdparty/                       # Third-party dependencies
│   ├── mongoose/                     # Async HTTP/WebSocket
│   ├── ccronexpr/                    # Cron expression parser
│   ├── log.c/                        # Logging library
│   ├── linenoise/                    # Line editing
│   └── md4c/                         # Markdown-to-terminal renderer
├── toolchain/                        # Cross-compilation toolchains
├── patches/                          # Dependency patches
├── doc/                              # Documentation
├── MODULE.bazel                      # Bazel module definitions
└── BUILD                             # Top-level build & packaging
```

## Getting Started

### Download Binary (Quick)

Pre-built static binaries for Linux (amd64, armv7, aarch64) and macOS (aarch64) are available on the [Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page. Download the tarball matching your platform:

| Platform | Tarball |
|----------|---------|
| Linux x86_64 | `edgeclaw_amd64_linux.tar.gz` |
| Linux aarch64 | `edgeclaw_aarch64_linux.tar.gz` |
| Linux armv7 | `edgeclaw_arm_linux.tar.gz` |
| macOS aarch64 | `edgeclaw_aarch64_macos.tar.gz` |

```bash
# Replace {ARCH}_{OS} with your platform, e.g. edgeclaw_amd64_linux.tar.gz
curl -LO https://github.com/edgeclaw-chat/edgeclaw/releases/latest/download/edgeclaw_{ARCH}_{OS}.tar.gz

# Extract and run
tar -xzf edgeclaw_*.tar.gz
cd edgeclaw_*/
./edgeclaw --setup
```

### Build from Source

#### Prerequisites

- **Bazel** (build system)
- C compiler (GCC / Clang)

#### Build

```bash
git clone https://github.com/edgeclaw-chat/edgeclaw.git
cd edgeclaw

# Build for the host platform
bazel build //src:edgeclaw

# Cross-compile for aarch64 Linux
bazel build //src:edgeclaw --platforms=//:aarch64_linux

# Cross-compile for armv7 Linux
bazel build //src:edgeclaw --platforms=//:arm_linux

# Build release tarball
bazel build //:edgeclaw
```

### Setup

Run the binary with `--setup` to launch the interactive setup wizard:

```bash
./edgeclaw --setup
```

The wizard guides you through:
- LLM API key, URL, and model
- Chat channel type and credentials
- Workspace path, sandbox root, and other options

Configuration is saved to `~/.edgeclaw/config.json`. The bundled `workspace/` directory is also automatically copied to `~/.edgeclaw/workspace/`.

### Manual Configuration

Create `~/.edgeclaw/config.json`:

```json
{
  "llm_api_conf": {
    "api_key": "sk-your-api-key",
    "api_url": "https://api.openai.com/v1",
    "model": "gpt-4o"
  },
  "channel": {
    "type": "telegram",
    "bot_key": "123456:ABC-DEF",
    "user_id": "your-telegram-user-id"
  },
  "workspace": "~/.edgeclaw",
  "sandbox_root_real": "~/.edgeclaw/sandbox",
  "max_iter": 10,
  "load_n_histories": 10,
  "tool_max_output_size": 10000
}
```

#### Channel Configuration

**Telegram:**
```json
"channel": {
  "type": "telegram",
  "bot_key": "your-bot-token",
  "user_id": "your-telegram-id"
}
```

**Feishu / Lark:**
```json
"channel": {
  "type": "feishu",
  "app_id": "your-app-id",
  "app_secret": "your-app-secret",
  "open_id": "your-open-id"
}
```

**QQ:**
```json
"channel": {
  "type": "qq",
  "app_id": "your-app-id",
  "app_secret": "your-app-secret"
}
```

**CLI:**
```json
"channel": {
  "type": "cli"
}
```

### Run

On first run, EdgeClaw automatically copies the bundled `workspace/` directory to `~/.edgeclaw/workspace`. Use `--setup` if `config.json` does not exist.

**Development / cloned repo:**

```bash
bazel run //src:edgeclaw
```

**Production / release tarball:**

```bash
# Build the release tarball (includes binary + workspace/)
bazel build //:edgeclaw

# Extract and run
tar -xzf bazel-bin/edgeclaw_*.tar.gz
cd edgeclaw_*/
./edgeclaw
```

---

## Usage

### Command-Line Arguments

```
Usage: edgeclaw [OPTION]...

Options:
  -s, --setup         run initial setup wizard and exit
  -S, --sandbox       enable filesystem sandbox (Linux only)
  -h, --help          display this help and exit
  -V, --version       output version information and exit
```

### Running the Agent

**Basic daemon mode** — connects to configured channels and processes incoming messages:

```bash
edgeclaw
```

**With filesystem sandbox** — restricts tool execution to the configured sandbox root:

```bash
edgeclaw --sandbox
```

The agent runs as a foreground process. Press `Ctrl+C` to stop.

On startup, EdgeClaw:
1. Creates `~/.edgeclaw/` if it does not exist
2. Copies the bundled `workspace/` to `~/.edgeclaw/workspace/` on first run (the release tarball includes this directory alongside the binary; when running from a cloned repo, EdgeClaw uses the `workspace/` directory in the current working directory)

### Master Key & Encrypted Secrets

Sensitive config values (API keys, passwords) can be stored encrypted in `config.json`. Encryption is powered by **AES-256-GCM** via mbedtls.

EdgeClaw derives the encryption key from a master passphrase using HKDF-SHA256. The key is sourced in this order:

1. **Environment variable** (`EDGECLAW_MASTER_KEY`) — for scripting / Docker deployments. The variable is cleared from the environment after reading.
2. **Interactive TTY prompt** — prompted on startup if no env var is set.

On first setup (via `--setup`), you will be prompted to create a passphrase. A verification hash is stored in `~/.edgeclaw/.key_hash` to validate the passphrase on subsequent runs. The master key is locked in memory with `mlock()` to prevent swapping and is wiped after config is loaded.

**Format in `config.json`:**

```json
{
  "llm_api_conf": {
    "api_key": "EDGECLAW_ENC:<base64-encrypted-blob>"
  }
}
```

### Interacting via Chat Channels

Once running, EdgeClaw listens on the configured channel and responds to your messages:

| Channel | How It Works |
|---------|-------------|
| **Telegram** | Send messages to your bot; EdgeClaw polls `getUpdates` and replies |
| **Feishu / Lark** | Send messages in the bot conversation; EdgeClaw maintains a WebSocket connection via WSS + Protobuf frames |
| **QQ** | Send messages to the bot; EdgeClaw uses WebSocket with shard support |
| **CLI** | Type messages in the terminal; EdgeClaw renders Markdown-formatted responses |

Each LLM conversation turn may involve multiple internal tool-calling iterations (up to `max_iter`). The agent only responds once it has a final answer.

### Scheduled Tasks (Cron)

EdgeClaw can schedule one-shot or recurring tasks. These are typically created by the LLM agent through its `crontab` tool, but can also be managed directly through conversations:

**Scheduling a one-shot reminder** (via the agent):

> "Remind me to check the server at 3pm tomorrow"

The agent will call the `crontab` tool with `action: "add"` and `at: "2026-05-29T15:00:00"`. When the timer fires, EdgeClaw sends the reminder message back through the same channel.

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
| `history.jsonl` | Compressed conversation history |

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

| Library | Purpose | Type |
|---------|---------|------|
| **GLib** | Event loop, data structures, threading | Runtime |
| **cJSON** | JSON parsing | Runtime |
| **Mongoose** | Async HTTP/HTTPS/WebSocket client | Runtime |
| **ccronexpr** | Cron expression parsing | Runtime |
| **log.c** | Logging | Runtime |
| **linenoise** | Interactive input editing | Runtime |
| **md4c** | Markdown parsing (CLI rendering) | Runtime |
| **nanopb** | Protobuf (Feishu WebSocket) | Runtime |
| **mbedtls** | AES encryption for config secrets | Runtime |

All dependencies are managed via **Bazel** modules.

## Development

### Code Conventions

- C99 standard
- Naming: `edc_module_function()` for public API, `_edc_module_function()` for private
- Header guards: `__BC_MODULE_H__`
- Actors receive messages via `GAsyncQueue` and dispatch with `edc_actor_handler_t`
- Tools implement an async exec callback and register with the toolbox

### Running Tests

```bash
bazel test //tests/...
```

### Adding a New Tool

1. Create `edc_tools_xxx.c` / `edc_tools_xxx.h` with a registration function:

```c
void edc_tools_xxx_register(struct edc_toolbox *tb) {
    GSList *params = NULL;
    params = g_slist_append(params, edc_tool_param_new("param1", edc_param_str_type(NULL), TRUE));
    struct edc_tool *tool = edc_tool_new("xxx", "Tool description", params, exec_xxx);
    edc_toolbox_register(tb, tool);
}
```

2. Implement the async exec callback and call `edc_toolbox_register()`.
3. Add source files to `src/BUILD` in the `agent` library.

### Adding a New Channel

1. Create `edc_channel_xxx.c` / `edc_channel_xxx.h` with init, poll/connect, send, and cleanup functions.
2. Add the channel type to the `edc_channel_type` enum and the union struct in `edc_channel_actor.h`.
3. Add config fields to `struct channel_conf` in `edc_config.h` and a parsing case in `edc_config.c`.
4. Register initialization in `edc_channel_actor.c`.
5. Add source files to `src/BUILD`.

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
