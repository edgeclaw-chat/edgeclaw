# EdgeClaw — User Manual

## Overview

EdgeClaw is a command-line AI assistant offering shell-like task management.
It supports multi-turn conversations, tool calling, and Unix-inspired process
control for orchestrating multiple parallel agent tasks.

## Quick Start

```bash
$ edgeclaw              # start interactive CLI
$ edgeclaw --setup       # first-time setup wizard
$ edgeclaw --sandbox     # enable filesystem sandbox
```

Type and press Enter. Conversations are saved per session under `workspace/<session_id>/`.

---

## Quick Task Syntax (`@`)

Use `@` to create a task with input in one line:

```
@analyze "find performance bugs in src/"
@analyze "find bugs" --bg                # background
@summary "please merge" -d task-1        # wait for task-1 first
@review "check code" -d t1 -d t2        # wait for multiple tasks
```

---

## Task Model

Three-layer Unix-like process model:

| EdgeClaw   | Unix        | Behavior                              |
|------------|-------------|---------------------------------------|
| **Task**   | Thread      | Conversation sharing history with siblings. |
| **Group**  | Process     | Isolated container. Cross-group needs `/task chain`. |
| **Session**| Session     | Per-session history storage. |

### Task States

```
IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING
  → TOOL_CALLING → SUSPENDED → COMPLETED / CANCELLED / ERROR
```

- **COMPLETED**: persists until reclaimed with `/task join`.
- **WAITING_DEPS**: accepts input and queues it until dependencies resolve.
- **SUSPENDED**: can be resumed.

---

## Shell-like Job Control

| Shell             | EdgeClaw                      |
|-------------------|-------------------------------|
| `cmd &`           | `@task "input" --bg`          |
| `jobs`            | `/task list`                  |
| `fg %1`           | `/task fg <task_id>`          |
| `bg %1`           | `/task bg`                    |
| `kill %1`         | `/task stop <task_id>`        |
| `Ctrl+Z`          | `/task suspend <task_id>`     |
| `fg / bg (resume)`| `/task resume <task_id>`      |
| `cmd1 \| cmd2`    | `/task chain <src> <dst>`     |
| `pthread_join()`  | `/task join <task_id>`        |
| `fork()`          | `/task fork <parent> <desc>`  |

---

## Command Reference

### Session

| Command | Description |
|---------|-------------|
| `/new`  | Start a new session |
| `/stop` | Cancel all active tasks in foreground group |
| `/help` | Show help message |
| `/exit` | Exit edgeclaw |

### Task Creation

| Command | Description |
|---------|-------------|
| `/task new <desc>` | Create new group + task (foreground) |
| `/task new <desc> --bg` | Create new group + task (background) |
| `/task new <desc> -g <gid>` | Create in existing group |
| `/task new <desc> -d <tid>` | Create + wait for dependency |
| `/task fork <pid> <desc>` | Fork child in parent's group |
| `/task fork <pid> <desc> --prompt <name>` | Fork with subagent prompt |

### Task Inspection

| Command | Description |
|---------|-------------|
| `/task list` | List all groups and tasks. Active task marked with `*` (e.g. `[L*]` or `[M*]`). |
| `/task status [tid]` | Show task details (state, output, deps, turns) |
| `/task output <tid>` | Show only the task's output text |
| `/task groups` | List all process groups |

### Foreground / Active

| Command | Description |
|---------|-------------|
| `/task fg <tid>` | Foreground + set as active. **Only the active task outputs to terminal.** |
| `/task bg` | Move current foreground group to background |
| `/task switch <tid>` | Set active task within foreground group |

### Task Control

| Command | Description |
|---------|-------------|
| `/task stop <tid>` | Cancel a task (SIGTERM — notifies dependents) |
| `/task stop --force <id>` | Force-kill (SIGKILL — no dep notification) |
| `/task stop --group <id>` | Cancel all tasks in a process group |
| `/task suspend <tid>` | Pause a task (like Ctrl+Z) |
| `/task resume <tid>` | Resume a suspended task |
| `/task join <tid>` | Show output, auto-inject into active task (or create one), then reclaim |
| `/task prompt <tid> <name>` | Set a subagent prompt for the task |

### Pipelines

| Command | Description |
|---------|-------------|
| `/task chain <src> <dst>` | dst waits for src. When src completes, its user inputs and final output are injected into dst's LLM context as Q&A. Like `src \| dst`. |

---

## Output

Only the session's **active task** outputs to the terminal. `/task fg` and
`/task switch` change the active task. Other tasks complete silently; use
`/task join` to retrieve their output.

---

## Context Sharing

**Within a group**: all tasks share the full conversation history automatically.

**Cross-group** (`/task chain` or `/task join`): the source task's user inputs
and final output are injected into the destination's LLM context as a
Q&A-style system message:

```
[子任务 analyze (id=task-1)]:
Q: help me analyze src/ for bugs
A: Found 3 performance issues ...
```

---

## Workflow Examples

### Quick Conversation with @

```
$ edgeclaw
> hello
Welcome! How can I help?
> @code "write a Python HTTP server"
```

### Background + Pipeline

```
> @logs "analyze error logs" --bg
> @api "audit API endpoints" --bg
> @summary "merge results" -d task-1 -d task-2
```

### Cross-Group Chain

```
> /task new "summary"
> /task chain task-1 task-0     ← task-0 waits for task-1
> /task chain task-2 task-0     ← task-0 waits for both

... task-1, task-2 complete ...

> /task switch task-0
> please summarize both results
→ LLM sees task-1 and task-2 Q&A injected as context
```

### Join and Reclaim

```
> /task join task-1
[task-1 output]: ... → injected into task-3 → task-1 reaped
```

### Suspend and Resume

```
> /task suspend task-3
> /task resume task-3
```

---

## Files

| Path | Description |
|------|-------------|
| `~/.edgeclaw/` | EdgeClaw home directory |
| `~/.edgeclaw/config.json` | Configuration (API key, model) |
| `~/.edgeclaw/workspace/` | Workspace root |
| `~/.edgeclaw/workspace/<sid>/` | Per-session directory |
| `~/.edgeclaw/workspace/<sid>/history.jsonl` | Session history |
| `~/.edgeclaw/workspace/memory/MEMORY.md` | Agent memory |
| `~/.edgeclaw/workspace/skills/` | Custom skill SKILL.md files |

## See Also

- Architecture: `docs/task-session-architecture.md`
- Source: <https://github.com/edgeclaw-chat/edgeclaw>
