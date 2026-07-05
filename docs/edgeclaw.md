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

## Task Model (Unix-Inspired)

EdgeClaw uses a three-layer process model:

| EdgeClaw   | Unix        | Behavior                              |
|------------|-------------|---------------------------------------|
| **Task**   | Thread      | Conversation with state, output. Shares full history with sibling tasks in the same group. |
| **Group**  | Process     | Isolated container. All tasks in a group share conversation history automatically. Cross-group needs explicit `/task chain`. |
| **Session**| Session     | Login session containing multiple groups. History stored per-session on disk. |

### Task States

```
IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING
  → TOOL_CALLING → SUSPENDED → COMPLETED / CANCELLED / ERROR
```

- **COMPLETED**: persists until reclaimed with `/task join`.
- **SUSPENDED**: can be resumed.
- **CANCELLED / ERROR**: can be reclaimed with `/task join`.

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
| `fork()`          | `/task fork <parent> <desc>`  |
| `setsid`          | `/task new <desc>`            |

---

## Command Reference

### Session

| Command | Description |
|---------|-------------|
| `/new`  | Start a new session (clear turns, trigger compress) |
| `/stop` | Cancel all active tasks in foreground group |
| `/help` | Show help message |
| `/exit` | Exit edgeclaw |

### Task Creation

| Command | Description |
|---------|-------------|
| `/task new <desc>` | Create new group + task (foreground) |
| `/task new <desc> --bg` | Create new group + task (background) |
| `/task new <desc> --group <gid>` | Create task inside an existing group |
| `/task fork <pid> <desc>` | Fork a child task in parent's group, inheriting fork-point context |
| `/task fork <pid> <desc> --prompt <name>` | Fork with a subagent prompt |

### Task Inspection

| Command | Description |
|---------|-------------|
| `/task list` | List all groups and tasks (markdown tree) |
| `/task status [tid]` | Show task details (state, output, deps, turns) |
| `/task output <tid>` | Show only the task's output text |
| `/task groups` | List all process groups |

### Foreground / Active Task

| Command | Description |
|---------|-------------|
| `/task fg <tid>` | Bring task's group to foreground |
| `/task bg` | Move current foreground group to background |
| `/task switch <tid>` | Set active task within foreground group. **Only the active task outputs to terminal.** |

### Task Control

| Command | Description |
|---------|-------------|
| `/task stop <tid>` | Cancel a task (SIGTERM — notifies dependents) |
| `/task stop --force <id>` | Force-kill (SIGKILL — no dep notification) |
| `/task stop --group <id>` | Cancel all tasks in a process group |
| `/task suspend <tid>` | Pause a task (like Ctrl+Z) |
| `/task resume <tid>` | Resume a suspended task |
| `/task join <tid>` | If COMPLETED: show output and reclaim. If RUNNING: shows how to chain/wait |
| `/task prompt <tid> <name>` | Set a subagent prompt for the task |

### Pipelines

| Command | Description |
|---------|-------------|
| `/task chain <src> <dst>` | dst waits for src. When src completes, its output (final LLM reply) is injected into dst's context. Like `src \| dst`. |

---

## Context Sharing

**Within a group**: all tasks automatically share the full conversation history
(every turn with all messages, tool calls, and thinking content).

**Cross-group**: use `/task chain` to inject a source task's output into a
destination task's LLM context when the source completes.

**Forked tasks**: a child task inherits the group's history up to the fork
point (not the parent's subsequent messages). Use `/task chain` to pass the
parent's final result to the child.

## Output

Only the session's **active task** outputs to the terminal. Other tasks in the
same group complete silently; use `/task join` to retrieve their output.
Use `/task switch` to change the active task.

---

## Workflow Examples

### Simple Conversation

```
$ edgeclaw
> hello
Welcome! How can I help?
> write a Python HTTP server
Here's a simple HTTP server: ...
```

### Background Analysis

```
> /task new "analyze logs" --bg
[task] created task-1 (group task-1) in background

> /task new "analyze frontend" --bg

> /task list
- [fg] task-1 (2)
    - [L] task-1  WAITING_INPUT  analyze logs
    - [M] task-2  WAITING_INPUT  analyze frontend
```

### Pipeline (Cross-Group)

```
> /task new "summary"
> /task chain task-1 task-0     ← task-0 waits for task-1
> /task chain task-2 task-0     ← task-0 waits for both

... task-1, task-2 complete ...

> /task switch task-0
> please summarize both results
→ LLM sees task-1 and task-2 outputs injected as system messages
```

### Fork + Subagent

```
> /task fork task-1 "review" --prompt code-reviewer
[fork] created fork-xx from task-1 (group task-1)

> /task switch fork-xx
> review src/agent.c
→ child sees parent's group history up to fork point
```

### Join and Reclaim

```
> /task join task-1
[task-1 output]: "Analysis complete: 3 issues found..."
→ task-1 reclaimed and removed
```

### Suspend and Resume

```
> /task suspend task-3
task task-3 suspended
> /task resume task-3
task task-3 resumed
```

---

## Files

| Path | Description |
|------|-------------|
| `~/.edgeclaw/` | EdgeClaw home directory |
| `~/.edgeclaw/config.json` | Configuration (API key, model) |
| `~/.edgeclaw/workspace/` | Workspace root for tools and files |
| `~/.edgeclaw/workspace/<sid>/` | Per-session directory |
| `~/.edgeclaw/workspace/<sid>/history.jsonl` | Session conversation history |
| `~/.edgeclaw/workspace/<sid>/history_info.json` | Session metadata |
| `~/.edgeclaw/workspace/memory/MEMORY.md` | Agent memory file |
| `~/.edgeclaw/workspace/skills/` | Custom skill SKILL.md files |

## See Also

- Architecture: `docs/task-session-architecture.md`
- Source: <https://github.com/edgeclaw-chat/edgeclaw>
