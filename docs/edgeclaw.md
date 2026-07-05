# EdgeClaw — User Manual

## Overview

EdgeClaw is a command-line AI assistant that integrates LLMs, tools, and a
shell-like task management system. It supports multi-turn conversations,
tool calling, and Unix-inspired process control for managing multiple
parallel agent tasks.

## Quick Start

```bash
$ edgeclaw              # start interactive CLI
$ edgeclaw --setup       # first-time setup wizard
$ edgeclaw --sandbox     # enable filesystem sandbox
```

Type your message and press Enter. EdgeClaw responds using the configured
LLM. Conversations are saved automatically.

---

## Task Management (Unix Process Model)

EdgeClaw implements a Unix-like process model:

| EdgeClaw   | Unix        | Description                              |
|------------|-------------|------------------------------------------|
| **Turn**   | Thread      | Single request/response exchange         |
| **Task**   | Process     | Conversation task with state and output  |
| **Group**  | PGID        | Related tasks sharing context            |
| **Session**| Session     | Container for multiple groups            |

### Task States

```
IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING
  → TOOL_CALLING → SUSPENDED → COMPLETED / CANCELLED / ERROR
```

- **COMPLETED** tasks auto-reap after replying.
- **SUSPENDED** tasks can be resumed.
- **CANCELLED / ERROR** tasks can be reaped with `/task join`.

---

## Shell-like Job Control

| Shell             | EdgeClaw                      |
|-------------------|-------------------------------|
| `cmd &`           | `/task new "desc" --bg`       |
| `jobs`            | `/task list`                  |
| `fg %1`           | `/task fg <task_id>`          |
| `bg %1`           | `/task bg`                    |
| `kill %1`         | `/task stop <task_id>`        |
| `Ctrl+Z`          | `/task suspend <task_id>`     |
| `fg / bg (resume)`| `/task resume <task_id>`      |
| `cmd1 \| cmd2`    | `/task chain <src> <dst>`     |
| `pthread_join()`  | `/task join <task_id>`        |

---

## Command Reference

### Session Commands

| Command     | Description                                         |
|-------------|-----------------------------------------------------|
| `/new`      | Start a new session (clear turns, trigger compress) |
| `/stop`     | Cancel all active tasks in foreground group         |
| `/help`     | Show help message                                   |
| `/exit`     | Exit edgeclaw                                       |

### Task Creation

| Command                                      | Description                              |
|----------------------------------------------|------------------------------------------|
| `/task new <desc>`                           | Create a new task and set as foreground  |
| `/task new <desc> --bg`                      | Create a new background task             |
| `/task fork <parent_id> <desc> [--prompt <name>]` | Fork a child task inheriting parent context |

### Task Inspection

| Command                     | Description                                    |
|-----------------------------|------------------------------------------------|
| `/task list`                | List all tasks grouped by process group         |
| `/task status [task_id]`    | Show task details (state, output, deps)        |
| `/task output <task_id>`    | Show only the task's output text               |
| `/task groups`              | List all process groups (PGID)                 |

### Foreground / Background

| Command                    | Description                                      |
|----------------------------|--------------------------------------------------|
| `/task fg <task_id>`       | Bring task's group to foreground (receive input)  |
| `/task bg`                 | Move current foreground group to background       |
| `/task switch <task_id>`   | Switch active task within foreground group        |

### Task Control

| Command                    | Description                              |
|----------------------------|------------------------------------------|
| `/task stop <task_id>`     | Cancel a task (SIGTERM — notifies deps)  |
| `/task stop --force <id>`  | Force-kill (SIGKILL — no dep notification)|
| `/task stop --group <id>`  | Cancel all tasks in a process group      |
| `/task suspend <task_id>`  | Pause a task (like Ctrl+Z)               |
| `/task resume <task_id>`   | Resume a suspended task                  |

### Dependencies and Pipelines

| Command                    | Description                              |
|----------------------------|------------------------------------------|
| `/task chain <src> <dst>`  | dst waits for src. When src finishes, its output is injected into dst's LLM context as a system message. Like `src \| dst`. |
| `/task join <task_id>`     | If COMPLETED: show output and reap. If RUNNING: show suggestion to chain/wait. |

### Subagent / Prompt

| Command                              | Description                       |
|--------------------------------------|-----------------------------------|
| `/task prompt <id> <name>`           | Set a subagent prompt for the task|

---

## Workflow Examples

### Simple Conversation

```
$ edgeclaw
> 你好
Welcome! How can I help?
> 帮我写一个 Python HTTP server
Here's a simple HTTP server: ...
```

### Background Analysis

```
> /task new "分析日志" --bg
[task] created task-1 (group task-1) in background

> /task new "分析前端" --bg
> /task list
  [*] group task-1 (1 members)    foreground
  [+] group task-2 (1 members)    background
```

### Pipeline (src → dst, like `cmd1 | cmd2`)

```
> /task new "汇总"
> /task chain task-1 task-0    ← task-0 waits for task-1
> /task chain task-2 task-0    ← task-0 waits for both
> /task list
  [task-0 WAITING_DEPS]: waiting for task-1, task-2

... task-1, task-2 finish (auto-reap) ...

> /task switch task-0
> 请汇总以上两个分析结果
→ LLM sees both task-1 and task-2 outputs injected as context
```

### Suspend and Resume (Ctrl+Z)

```
> /task suspend task-3
task task-3 suspended

> /task resume task-3
task task-3 resumed
```

---

## Context Sharing

**Within a group**: Tasks automatically share conversation history. Each new
task sees all prior turns in the group.

**Cross-group**: Use `/task chain <src> <dst>` to inject src's output into
dst's LLM context when src completes.

---

## Files

| Path                                          | Description                  |
|-----------------------------------------------|------------------------------|
| `~/.edgeclaw/`                                | EdgeClaw home directory      |
| `~/.edgeclaw/config.json`                     | Configuration (API key, model) |
| `~/.edgeclaw/workspace/`                      | Workspace root for tools     |
| `~/.edgeclaw/workspace/memory/MEMORY.md`      | Agent memory file            |
| `~/.edgeclaw/workspace/history.jsonl`         | Conversation history         |
| `~/.edgeclaw/workspace/skills/`               | Custom skill SKILL.md files  |

---

## See Also

- Source: <https://github.com/edgeclaw-chat/edgeclaw>
- Architecture: `docs/task-session-architecture.md`
