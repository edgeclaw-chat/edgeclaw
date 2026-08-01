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

## Shell Architecture

EdgeClaw's input parser is modeled after Unix shells, with a full pipeline:

```
User Input → History Expansion → Tokenizer → Parser → AST → Executor
```

### Tokenizer

A quote-aware tokenizer splits input into tokens, respecting `"` double-quotes
and `'` single-quotes. Recognized token types:

| Token | Type | Description |
|-------|------|-------------|
| `text` | TOK_TEXT | Regular text (may contain whitespace inside quotes) |
| `\|`   | TOK_PIPE | Pipe — chains tasks with output injection |
| `;`   | TOK_SEQ | Sequential — chains tasks without output injection |
| `&`   | TOK_BG | Background — runs task asynchronously |
| `>`   | TOK_REDIR_OUT | Redirect stdout to file (overwrite) |
| `>>`  | TOK_REDIR_APPEND | Redirect stdout to file (append) |
| `2>`  | TOK_REDIR_ERR | Redirect stderr to file |
| `<`   | TOK_REDIR_IN | Read input from file |

### Parser & AST

A recursive-descent parser builds an AST (Abstract Syntax Tree). Node hierarchy:

```
AST_ROOT   — top-level wrapper
  AST_SEQ  — ; chain (sequential dependency)
    AST_PIPE — | chain (pipe dependency with injection)
      AST_BG     — & background modifier
      AST_REDIR_* — > / >> / 2> / < redirect
      AST_CMD   — leaf node: text + options
```

### Executor

The AST executor (`exec_ctx_t`) walks the tree recursively, carrying context:
`target_group` (preserved across pipe segments), `bg` (background flag), and
`dep_type` (dependency type). Each `AST_CMD` leaf spawns a task via the scheduler.

### History Expansion

Before tokenization, `!n` and `!!` patterns are expanded:
- `!!` — repeat the last command
- `!n` — repeat the nth command from history (see `/history`)

---

## Quick Task Syntax (`@`)

Use `@` to create a task in a new group with one line:

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
| **Group**  | Process     | Isolated container. Cross-group needs pipe or `/task chain`. |
| **Session**| Session     | Per-session history storage, groups container. |

### Task States

```
IDLE → WAITING_DEPS → WAITING_INPUT → LLM_PROCESSING
  ⇄ TOOL_CALLING → SUSPENDED → COMPLETED / CANCELLED / ERROR
```

The LLM_PROCESSING and TOOL_CALLING states alternate as the LLM invokes tools
and receives results. Tasks reach COMPLETED when the LLM produces a final
response; CANCELLED when stopped; ERROR on failure.

- **COMPLETED**: persists until reclaimed with `/task join`.
- **WAITING_DEPS**: accepts input and queues it until dependencies resolve.
- **SUSPENDED**: can be resumed.

### Dependency System

Tasks form a directed acyclic graph (DAG) via dependency edges. Each task
maintains two linked lists: `deps` (tasks it waits for) and `dependents`
(tasks waiting for it). **DFS cycle detection** runs on every new dependency
to prevent deadlocks.

Three dependency types:

| Type | Syntax | Behavior |
|------|--------|----------|
| **JOIN** | `\|` (pipe) | Source output is injected into destination's LLM context |
| **SEQUENCE** | `;` | Destination waits for source, but no output injection |
| **PIPE** | (reserved) | Streaming mode — not yet implemented |

### Fork

`/task fork <parent> <desc>` creates a child task in the parent's group.
The child inherits a slice of the parent's conversation context. Use
`--prompt <name>` to assign a subagent prompt to the forked task.

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

## Variable Expansion

Variable references (`$VAR`) undergo a **two-phase expansion**:

### Phase 1: Collect (at task creation)

When a task is created with `$N`, `$LAST`, or `$$`:
1. The reference is **validated** — the target task must exist
2. An **implicit dependency** is added from this task to the referenced task
3. The `$VAR` text is **preserved** as-is in the task input

### Phase 2: Expand (when dependencies resolve)

Once the referenced task completes:
1. `$VAR` is replaced with the referenced task's **actual output text**
2. The expanded input is then sent to the LLM

This means `$VAR` references are resolved **just in time** — the task waits for
its dependency, then substitutes the output before LLM processing begins.

| Variable | Meaning |
|----------|---------|
| `$LAST` | Output of the most recently completed task (by completion order) |
| `$N` | Output of task with id N (e.g. `$1`, `$2`) |
| `$$` | Task ID of the current foreground active task |

Expansion happens **after** pipe splitting and redirect stripping, so any `|`
or `>` in expanded output is treated as literal text.

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

### Quick Conversation

```
$ edgeclaw
> hello
Welcome! How can I help?
> @code "write a Python HTTP server"
```

### Pipe Chain (output injected at each step)

```
> scan src/ for vulnerabilities | create a fix plan | apply the patches
```

Creates three tasks in sequence. Task 2 receives task 1's output injected into
its LLM context. Task 3 receives task 2's output. All share the same group.

### Sequential (no injection between steps)

```
> review src/ ; run the test suite ; commit and push
```

Creates three tasks that run in order. Unlike `|`, task outputs are NOT injected
between segments — each task starts fresh.

### Background + Dependencies

```
> @logs "analyze error logs in /var/log" --bg
> @api "audit all API endpoints" --bg
> @summary "merge both findings" -d task-1 -d task-2
```

### Combined Syntax: Pipes + Redirects

```
> analyze codebase | find critical bugs | generate report > bugs.md
> build project 2> build-errors.log
> fix all issues < context.txt
```

Note: `<` reads the file and appends its content to the task input. If there is
text before `<`, they are joined with a newline.

### Variable Expansion

```
> find performance bottlenecks in src/
... task 1 completes with output ...
> based on $LAST, create an optimization plan
... task 2 waits for task 1, then sees its output ...
```

### History Expansion

```
> review the latest PR
> !!                    # repeats "review the latest PR"
> !3                    # repeats command #3 from history
```

### Cross-Group Chain

```
> /task new "security audit"
> /task new "performance review"
> /task new "final summary"
> /task chain task-1 task-3     ← task-3 waits for task-1
> /task chain task-2 task-3     ← task-3 waits for both

... task-1, task-2 complete ...

> /task switch task-3
> please summarize both reports
→ LLM sees task-1 and task-2 Q&A injected as context
```

### Join and Reclaim

```
> /task join task-1
[task-1 output]: ... → injected into active task → task-1 reaped
```

### Suspend and Resume

```
> /task suspend task-3
> /task resume task-3
```

### Fork with Subagent

```
> /task fork task-0 "review the database schema" --prompt dba
```

Creates a child task in task-0's group, using the "dba" subagent prompt,
inheriting the parent's conversation context.

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
