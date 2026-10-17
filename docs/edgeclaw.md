# EdgeClaw — User Manual

## Overview

EdgeClaw is an interactive terminal AI assistant offering shell-like task management.
It supports multi-turn conversations, tool calling, and Unix-inspired process
control for orchestrating multiple parallel agent tasks.

## Quick Start

```bash
$ edgeclaw              # start the interactive TUI
```

On first run EdgeClaw creates `config.json` with defaults and opens the guided
setup wizard once (until it is completed). Reopen it any time with `/connect`: it
walks through three steps — provider, API key, model — and applies the choice
immediately (it writes the base URL, key, model and the model's context window).

While the input starts with `/`, a completion list hangs just below it: `↑`/`↓` move
the selection, `Tab` completes it, `Esc` dismisses it. `Enter` is untouched — it still
sends what you typed.

Two keys interrupt things. **Esc** stops the foreground group's running task (what
`/task stop` does); while the completion list is open, `Esc` dismisses the list instead.
**Ctrl+C** clears the input box and shows "Ctrl+C again to quit" in the footer in place of
the working directory — press it a second time (within ~1.5s) to quit EdgeClaw.

The footer's right end reads `model · ctx used/window · sandbox:on|off · /help` (the
context gauge is the foreground group's current context; a `~` marks an estimate), and
while a task runs the bottom row becomes a progress bar carrying its elapsed time and an
"Esc to stop" hint.

Type a message in the input box and press Enter. Conversations are saved per session under `workspace/<session_id>/`.

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

Quoting and escapes follow Python's string rules: `"..."` and `'...'` both process
escapes (`\n`, `\t`, `\\`, `\"`, `\'`, `\xHH`; an unknown escape keeps its backslash),
and `"""` / `'''` are the multi-line forms. The two quote kinds differ in exactly one
thing: a `{{…}}` reference is expanded inside double quotes and never inside single ones
(single quotes are the way to write a literal `{{`). A quoted region must open the
argument for anything after the closing quote to be read as options.

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

There is none. `!` is only special at the **start of a line** (the local shell — see Local
Shell below); a `!n` or `!!` inside a task description is literal text.

---

## Shell-like Syntax (off by default)

Everything below borrows a shell's vocabulary: the sigils `+` (open a new group) and `!`
(run a local shell command), and the operators `|` `;` `&` `>` `>>` `2>` `<`. **That
vocabulary is off until you ask for it**, because it presumes you know a Unix shell — and
it fails *silently* when you do not: a line with a stray `|` becomes one odd task instead
of two, and nothing tells you why.

```
/shell on
```

With it **off** (the default), every line that is not a `/` command is **one task
description, verbatim** — no operators to get wrong, no sigil to escape.

**A shell line is recognised by its first character.** Every string in shell syntax is
quoted, so a shell line always begins with `"` or `'` — or with its own sigil, `+` / `!`. A
line beginning with none of those is **a normal message**: one task, verbatim, nothing
parsed out of it. That is what ordinary conversation looks like, and it is why it needs no
quoting:

```
analyze src/ for bugs                 a MESSAGE -- one task, nothing parsed
现在新建任务A|B                        a MESSAGE -- one task whose text contains the |
"analyze src/ for bugs"               shell -- a string literal (also one task, but parsed)
"scan src/" | "create a fix plan"     shell -- two tasks, the second fed the first's output
"alpha|beta"                          shell -- one task whose text contains the |
```

The quoting rule is Python's: `"…"` interpolates `{{…}}`, `'…'` is raw (no interpolation),
`"""…"""` / `'''…'''` span lines (the shape pasted text wants), escapes as in Python. Inside
a shell line it is mandatory: an unquoted operand is refused, never guessed (`"a" | b` comes
back with the fix). There is no escape character for the sigils — quoting is what makes a
`+` or `@` literal: `"+1 issue"` is a `+` and a task in the *current* group.

The redirect target is a string too — `"say hi" > "out.md"` — and its quotes are
delimiters, not part of the path, which is what makes a filename with a space in it
expressible.

What follows the closing quote is the **option area** (`-d <id>`, `-g <group>`,
`2>`, …). Anything else there is refused rather than dropped, so `"hello" and more` is an
error — not a task quietly called `hello`.

`/` commands are never gated: the command layer is a different thing from the shell layer,
and `/shell` itself has to stay reachable to turn the switch back on. So `/task new`,
`/skill` and `/prompt` keep working either way.

The state is persisted (`config.json` → `shell_syntax`) and shown in the footer as
`shell:on|off`. `/shell on|off` is the only way to flip it — deliberately no CLI flag, so
there is one way to say it and it is the one that persists.

**What the switch does *not* govern.** It is about the line you type. Two things sit
outside it:

- **A `/prompt` or `/skill` body is a document, not input to this layer.** It becomes one
  task's input, verbatim; the operators do nothing inside it and the input layer's `{{…}}`
  references are not collected. That is deliberate: a template is authored content, and an
  author must not have the meaning of their template depend on the *reader's* setting —
  they could never reproduce a bug report. (It is the same decision that made skill bodies
  spawn directly, and the same shape as a `'''` block: one literal atom.)
- **`@` is not part of it.** `@file` is a file reference — the opencode / Claude Code
  convention, not a Unix shell vocabulary — so it is expected to work with the switch off
  too. The migration hint an old `@desc` gets is the one piece that only makes sense with
  the switch on, since that is the only world where `@desc` ever meant a group.

---

## Quick Task Syntax (`+`)

Needs the shell-like syntax: **off by default**, `/shell on` turns it on.

Use `+` to create a task in a **new** group with one line:

```
+"find performance bugs in src/"
```

The description is a quoted string like any other (see Shell-like Syntax above); quoting is
also how you write one that itself starts with `+` or `@`.

To pass options, they ride after the closing quote — a `+` command whose description is
unquoted while carrying an option (`-`, `-d <id>`, ...) is a syntax error, not a
silently-ignored flag:

```
+"find bugs" &                   # background
+"please merge" -d task-1        # wait for task-1 first
+"check code" -d t1 -d t2        # wait for multiple tasks
```

`+` only says "put it in a new group" (a plain string joins the current group). See
Shell-like Job Control below.

### Shell syntax and command syntax are separate

`+` is **shell** syntax; `/…` is **command** syntax. A line is one or the other, and they do
not compose: `+/task new …`, `+/skill …`, `+/prompt …` and `+!cmd` are not a thing, and are
refused -- rather than quietly becoming a task whose description is the command.

To put a task in a group of its own, say it in the syntax you are already in:

```
+"security audit"                         # shell: a new-group task
/task new "security audit" --new-group    # command: the same statement
```

A prompt or a skill has no `--new-group` of its own -- its arguments belong to the template,
so there is nowhere unambiguous to put the option. Open the group first, then run the
command in it.

To write a description that starts with a sigil, quote it — there is no escape character:

```
"+1 issue is still open"         # a `+` that is just text, in the CURRENT group
"@alice please take a look"      # a `@` that is just text
```

> `@` **used to be the new-group sigil.** It no longer creates a task: a leading `@` is
> answered with a migration hint, because silently rereading an old `@desc` as "a task in
> the current group" would move the task to another group without saying so.

---

## Local Shell (`!`)

Needs the shell-like syntax: **off by default**, `/shell on` turns it on.

A line starting with `!` runs a **local shell command** on this machine:

```
!git status
```

What happens is worth stating exactly, because it is also how the rest of the system
sees it: the command becomes **a task of its own** (its own line in the sidebar, its own
entry in the group's history) whose only job is to issue one `exec_shell` call. So

- the command runs through `sh -c`, in the session's working directory, subject to the
  sandbox setting (`/sandbox`), with the same output cap as the `exec_shell` tool the model
  uses and **no timeout at all** — a command runs until it exits. Bound it yourself when
  you want to: `!timeout 60 make -j8`;
- its output is that task's result, and it is **shown on screen and stored in the group's
  history** — the next prompt sees the command and its output;
- **no model is called** — no LLM request is made for it, and it costs no tokens.

| Input | Behavior |
|---|---|
| `!cmd` | run `cmd` (a task of its own) |
| `!!` | run this session's previous `!` command again |
| `"!cmd"` | the same text as a normal task description instead (a quoted string) |
| `!` alone | usage |

The text after `!` is a command line for `sh`, so it is **not** quoted here — quoting is
for the strings of our own syntax, and this payload belongs to the shell.

Commands that need a terminal (`sudo`, `vim`, `ssh`, an editor) fail immediately: the
child gets no tty. A non-zero exit is not a task failure — the output (including
stderr) is still the result.

A running command is stopped from the task side: `/task stop <id>` (or `/session stop`)
cancels the task and **kills the command's whole process group** — that is what makes
"no timeout" safe.

---

## Task Model

Three-layer Unix-like process model:

| EdgeClaw   | Unix        | Behavior                              |
|------------|-------------|---------------------------------------|
| **Task**   | Thread      | Conversation sharing history with siblings. |
| **Group**  | Process     | Isolated container. Cross-group needs a pipe (`\|`). |
| **Session**| Session     | Per-session history storage, groups container. |

### Task States

```
IDLE → WAITING_DEPS → LLM_PROCESSING ⇄ TOOL_CALLING → COMPLETED / CANCELLED / ERROR
                          INTERRUPTED (the loop stopped mid-flight)
```

The LLM_PROCESSING and TOOL_CALLING states alternate as the LLM invokes tools
and receives results. Tasks reach COMPLETED when the LLM produces a final
response; CANCELLED when stopped; ERROR on failure.

**Restart behavior**: on exit, every unfinished task is rewritten as **INTERRUPTED**
(the loop stopped underneath it -- that is what happened, so that is what is recorded),
queued ones included, and the interrupt propagates to dependents like a failure so nobody
is left waiting for a task that will not run on its own. Load reads that back as stated:
an INTERRUPTED task stays INTERRUPTED until `/task resume [tid]` re-derives the state to
return to. Only fold / curation tasks resume by themselves (a half-done background round
is re-dispatched).

- unsatisfied dependencies → `WAITING_DEPS`
- an unfinished tool call → `TOOL_CALLING` (not-yet-started tools are re-run; a
  tool that was interrupted mid-flight gets a synthesized "interrupted" result)
- a pending turn (input not yet answered) → `LLM_PROCESSING`
- otherwise → `IDLE` (ready; there is no separate "waiting for input" phase)

**Failed tasks**: a task that failed mid-run is `ERROR` but keeps its half-finished
turn (and any interrupted tool ledger). `/task resume [tid]` continues that turn
instead of starting over — it re-derives the same way (deps / tool chain / turn)
and re-drives it.

**Cancelled tasks**: a task stopped with `/task stop` is `CANCELLED`, but it is not
destroyed — its turn, messages and the work done so far are all kept (the tool
ledger is dropped, since a cancelled tool no longer has a result to wait for).
`/task resume [tid]` revives it: the cancel flattened the step to `DONE`, so resume
re-derives the position. A tool call that was in flight when the task was stopped
gets a synthesized "interrupted" result first (the assistant message is the only
record left of it), so the next LLM request stays well-formed. Only `COMPLETED`
cannot be resumed.

- **COMPLETED**: terminal; its output stays readable with `/task output <id>`.
- **WAITING_DEPS**: accepts input and queues it until dependencies resolve.
- **INTERRUPTED**: the process went down while it was running; `/task resume` revives it.
  Like ERROR it propagates to its dependents (a JOIN dependent is cancelled, an order edge
  is voided), and it never holds a group's front.
- **SUSPENDED**: the user parked it; `/task resume` revives it. Nothing produces it today.
- **ERROR**: a failed task; `/task resume` continues its unfinished turn.
- **CANCELLED**: stopped with `/task stop`; `/task resume` continues it too.

### Dependency System

Tasks form a directed acyclic graph (DAG) via dependency edges. Each task
maintains two linked lists: `deps` (tasks it waits for) and `dependents`
(tasks waiting for it). **DFS cycle detection** runs on every new dependency
to prevent deadlocks.

Two dependency types:

| Type | Syntax | Behavior | On source CANCELLED |
|------|--------|----------|---------------------|
| **JOIN** | `\|` (pipe) | Source output is injected into destination's LLM context | Destination is CANCELLED too — the dependency can never be satisfied |
| **SEQUENCE** | `;` | Destination waits for source, but no output injection | Destination is cancelled too, and the edge stays: resuming it returns it to the wait |

### Fork

`/task fork <parent> <desc>` creates a child task that **leads a group of its own**
and takes the foreground. It inherits the parent group's memory summary plus a snapshot
of the parent's conversation context up to the fork point (`desc` is its input).

---

## Shell-like Job Control

Everything in this section needs the shell-like syntax (see above): it is **off by
default**, and `/shell on` turns it on.

**Every string in a shell line is quoted** — `"alpha" | "beta"`, never `alpha | beta`.
Unquoted, nothing says whether that `|` is a pipe the user meant or a `|` inside their text,
so the line is refused and the fix is spelled out. Quote the operator instead and it is
text: `"alpha|beta"`.

(A line with no operator at all is *not* a shell line — see above: it is a message, and
stays as written. Only a line that begins with a quote or a sigil is parsed.)

| Shell             | EdgeClaw                      |
|-------------------|-------------------------------|
| `cmd &`           | `"input" &`                   |
| `jobs`            | `/task list`                  |
| `fg %1`           | `/task fg <task_id>`          |
| `bg %1`           | `/task bg`                    |
| `kill %1`         | `/task stop <task_id>`        |
| `cmd1 \| cmd2`    | `"a" \| "b"`                  |
| `fork()`          | `/task fork <parent> <desc>`  |

`&` is the background modifier: the task joins the **current** group but does not take
the foreground. `+"input"` is a different thing -- it puts the task in a **new** group
(`+"input" &` = new group, still background) -- so `+` alone is not `&`.

---

## Command Reference

### General

| Command | Description |
|---------|-------------|
| `/help` | Show the command list |
| `/whoami` | Show the current session, model, foreground group and active task |
| `/model` | List the current provider's chat models (the current one marked) |
| `/model <id>` | Switch model (persisted; the catalog's context window is applied too) |
| `/model search <q>` | Search chat models across all providers |
| `/prompt` | List the templates in `workspace/prompts/` |
| `/prompt <name> [args…]` | Load a template, substitute its `$1` … `$N` / `$ARGUMENTS` placeholders (exactly opencode's; `{{…}}` and `${…}` are NOT ones), and make the body ONE task — as written. A body is a **document**, not a command line: `\| ; & >` in it do nothing, and `{{…}}` in it is plain text too (the last placeholder takes the remaining args; a body with no placeholder gets the args appended) |
| `/skill` | List the skills — ours, plus any found in a `.claude` / `.agents` / `.pi` / `.opencode` skills directory |
| `/skill <name> [args…]` | Run a skill directly: its `SKILL.md` body goes into this task wrapped in a `<skill name=… location=…>` block (pi's shape, so the model knows where the skill's relative paths hang off); the args are substituted, or appended after the closing tag when the body has none. Same as a prompt body, the text is a **document**: one task, verbatim |
| `/reload` | Re-read the resource files and rebuild the cached system prompt (a skill or AGENTS.md edited on disk takes effect; `config.json` still needs a restart) |
| `/connect` | Guided setup: choose a provider, API key, and model (three-step wizard) |
| `/sandbox [on\|off]` | Enable/disable the filesystem sandbox |
| `/sandbox list` | Show the sandbox allow/deny path lists |
| `/sandbox add-ro\|rm-ro <dir>` | Add/remove a read-only path |
| `/sandbox add-rw\|rm-rw <dir>` | Add/remove a read-write path |
| `/sandbox add-deny\|rm-deny <dir>` | Add/remove a masked dir (wins over ro/rw; e.g. `~/.ssh`) |
| `/shell [on\|off]` | Turn the shell-like input syntax on or off (off by default; persisted; shown in the footer as `shell:on\|off`) |

### Session

| Command | Description |
|---------|-------------|
| `/session new [name] [dir]` | Start a new session (optionally named, optionally with a working directory) |
| `/session switch <name_or_id>` | Switch to another session |
| `/session list` | List all sessions |
| `/session info` | Show current session details |
| `/session status [id]` | Show a session's state (no id = the active session) |
| `/session save` | Persist the current session's history and state |
| `/session stop` | Cancel all active tasks in the current session |
| `/session remove <name_or_id>` | Remove a session (the last one cannot be removed) |
| `/session rename <old> <new>` | Rename a session |
| `/session memo` · `/session memo show [id]` | Show the session memory (all entries, or one entry's detail) |
| `/session memo search <q>` | Search the session memory |
| `/session memo curate [focus]` | Curate durable entries out of the group memories now |
| `/session memo compact` | Compact the session memory now (merge / condense / rebuild) |

### Task Creation

| Command | Description |
|---------|-------------|
| `/task new <desc>` | Create a task in the group in front (queues behind whatever runs there) |
| `/task new <desc> --bg` | Same group, but background: runs alongside, takes no foreground |
| `/task new <desc> --new-group` | Open a group of its own (a fresh conversation) and take the foreground |
| `/task new <desc> -g <gid>` | Create in that existing group (`--group <gid>` is the long form) |
| `/task new <desc> -d <tid>` | Create + wait for dependency, injecting its output (repeatable; `--with <tid>` is the explicit form) |
| `/task fork <pid> <desc>` | Fork: a child task that leads a group of its own and takes the foreground, inheriting the parent group's memory and a snapshot of its context |

### Task Inspection

| Command | Description |
|---------|-------------|
| `/task list` | List all groups and tasks. Active task marked with `*` (e.g. `[L*]` or `[M*]`). |
| `/task status [tid]` | Show task details (state, output, deps, turns) |
| `/task output <tid>` | Show only the task's output text |
| `/task history <tid>` | Show the task's messages (`--json` = raw payload) |

### Foreground / Active

| Command | Description |
|---------|-------------|
| `/task fg <tid>` | Foreground + set as active. **Only the active task outputs to terminal.** |
| `/task bg [tid]` | Move the foreground group (or that task) to the background |

### Task Control

| Command | Description |
|---------|-------------|
| `/task stop [tid]` | Cancel a task (SIGTERM — notifies dependents). Without an id, stops the active task of the foreground group; errors if the id is not found |
| `/task resume [tid]` | Resume an INTERRUPTED, SUSPENDED, ERROR or CANCELLED task: re-derives state (deps / tool chain / turn) and drives it; an ERROR or CANCELLED task continues its unfinished turn. Without an id, resumes the active task of the foreground group |
| `/task resume --all` | Re-decide every stopped task of the session (after a restart); ones whose dependency is stopped too wait for it |

### Group

| Command | Description |
|---------|-------------|
| `/group list` | List all process groups |
| `/group info [id]` | A group's context / fold numbers (window, the fold line, how far it is, epoch, failures, fold in flight); no id = the foreground group |
| `/group history <id>` | The whole group's history, one block per member |
| `/group fold [id] [focus]` | Fold the group now, ignoring the trigger; no id = the foreground group |

---

## Output

Only the session's **active task** outputs to the terminal. `/task fg` changes
the active task. Other tasks complete silently; use
`/task output <id>` to read their output.

---

## Variable Expansion

Task references (`{{…}}`) undergo a **two-phase expansion** — and they belong to the
**shell** syntax: only a line the shell parses recognizes one.

A task created by a **command** takes its input as text, never as a shell line. So a
`{{LAST}}` written in a `/task new` description, or in a `/prompt` or `/skill` body, is
plain text: no reference is recognized, no dependency is made, nothing is substituted. The
same goes for the operators there — a command's input is not split into a `|` chain, and
`>` does not redirect.

The sigil is `{{…}}`, **not** `$`: `$` belongs to the prompt template layer (`$1` … `$N`,
`$ARGUMENTS` — opencode's two forms), so the two layers never collide.

### Phase 1: Collect (at task creation)

When a task is created with `{{<id>}}`, `{{LAST}}`, or `{{SELF}}`:
1. The reference is **validated** — the target task must exist
2. An **implicit dependency** is added from this task to the referenced task
3. The `{{…}}` text is **preserved** as-is in the task input

### Phase 2: Expand (when dependencies resolve)

Once the referenced task completes:
1. The reference is replaced with the referenced task's **actual output text**
2. The expanded input is then sent to the LLM

This means references are resolved **just in time** — the task waits for
its dependency, then substitutes the output before LLM processing begins.

| Reference | Meaning |
|----------|---------|
| `{{LAST}}` | Output of the most recently completed task (by completion order) |
| `{{<id>}}` | Output of task with id N (e.g. `{{1}}`, `{{2}}`) |
| `{{SELF}}` | Task ID of the current foreground active task |

**Only these forms count.** Any other `{{…}}` — a pasted `{{ var }}`, a stray brace — is
copied verbatim, and so is every `$`: nothing becomes a reference unless it is spelled
exactly, which is what makes pasted text (Jinja, shell snippets, `$HOME`, `$5.00`) safe.

Expansion happens **after** pipe splitting and redirect stripping, so any `|`
or `>` in expanded output is treated as literal text.

---

## Context Sharing

**Within a group**: all tasks share the full conversation history automatically.

**Cross-task** (a `-d` dependency, or a `|` pipe): the source task's user inputs
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
> +code "write a Python HTTP server"
```

### Pipe Chain (output injected at each step)

```
> "scan src/ for vulnerabilities" | "create a fix plan" | "apply the patches"
```

Creates three tasks in sequence. Task 2 receives task 1's output injected into
its LLM context. Task 3 receives task 2's output. All share the same group.
(Each operand is quoted because the line uses an operator — see Shell-like
Syntax above.)

### Sequential (no injection between steps)

```
> "review src/" ; "run the test suite" ; "commit and push"
```

Creates three tasks that run in order. Unlike `|`, task outputs are NOT injected
between segments — each task starts fresh.

### Background + Dependencies

```
> +"analyze error logs in /var/log" &
> +"audit all API endpoints" &
> +"merge both findings" -d task-1 -d task-2
```

### Combined Syntax: Pipes + Redirects

```
> "analyze codebase" | "find critical bugs" | "generate report" > bugs.md
> "build project" 2> build-errors.log
> "fix all issues" < context.txt
```

Note: `<` reads the file and appends its content to the task input. If there is
text before `<`, they are joined with a newline.

### Variable Expansion

```
> find performance bottlenecks in src/
... task 1 completes with output ...
> based on {{LAST}}, create an optimization plan
... task 2 waits for task 1, then sees its output ...
```

### Local Shell

```
> !git status          # runs on this machine, as a task of its own
... task 2 completes with the command's output ...

> !make -j8            # a command that needs a terminal fails fast (no tty)
> !!                   # runs `make -j8` again
```

### Cross-Group Chain

Three separate conversations, wired together by hand. `--new-group` is what makes them
separate -- without it they would all land in the group in front. `-d <id>` makes a task
wait for another, and the source's inputs + output are injected into it.

```
> /task new "security audit" --new-group
> /task new "performance review" --new-group
> /task new "final summary" --new-group -d task-1 -d task-2

... task-1, task-2 complete → task-3 runs with both reports injected ...

> /task fg task-3
→ LLM sees task-1 and task-2 Q&A injected as context
```

### Fork

```
> /task fork task-0 "review the database schema"
```

Creates a child task that leads a group of its own (taking the foreground),
inheriting the parent's memory and conversation context up to the fork point.

---

## Files

| Path | Description |
|------|-------------|
| `~/.edgeclaw/` | EdgeClaw home directory |
| `~/.edgeclaw/config.json` | Configuration (API key, model) |
| `~/.edgeclaw/workspace/` | Workspace root |
| `~/.edgeclaw/workspace/<sid>/` | Per-session directory |
| `~/.edgeclaw/workspace/<sid>/history.jsonl` | Session history |
| `~/.edgeclaw/workspace/AGENTS.md` | Agent behavior instructions (the agent's contract) |
| `~/.edgeclaw/workspace/TOOLS.md` | Tool quick reference |
| `~/.edgeclaw/workspace/skills/` | Custom skill SKILL.md files |

### Where prompts and skills come from

`/prompt` and `/skill` read more than `~/.edgeclaw/workspace`: the directories the other
tools already use are searched as well, so a template or skill you wrote for them works
here unedited. Precedence — the first hit wins:

| # | Root |
|---|------|
| 1 | the **project**, walking up from the session's directory: `.edgeclaw` `.claude` `.agents` `.opencode` `.pi` (nearest level first) |
| 2 | `~/.edgeclaw/workspace` (ours, the only writable one) |
| 3 | the user's: `~/.claude` · `~/.agents` · `~/.config/opencode` · `~/.pi/agent` |

A root may keep templates in `prompts/` (pi) or `command/` `commands/` (opencode / Claude
Code), and skills in `skills/` or `skill/`. The project level can be switched off with
`load_project_resources: false` in `config.json`.

**A collision is reported, not swallowed.** When the same name exists in two roots the
higher one wins and the shadowed copy is named: a warning in the log, and a
`_(shadowed: …)_` line under it in `/prompt` and `/skill`.

## See Also

- Architecture: `docs/task-session-architecture.md`
- Source: <https://github.com/edgeclaw-chat/edgeclaw>
