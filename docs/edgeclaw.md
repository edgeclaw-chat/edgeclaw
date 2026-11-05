# EdgeClaw — User Manual

## Overview

EdgeClaw is an interactive terminal AI assistant offering shell-like task management.
It supports multi-turn conversations, tool calling, and Unix-inspired process
control for orchestrating multiple parallel agent tasks.

This manual ships inside the binary: on startup EdgeClaw releases the manual chapters it
was built from into `$EDC_HOME/docs` (`~/.edgeclaw/docs` by default), and its system prompt
names `$EDC_HOME/docs/edgeclaw.md`, so the agent can read up on *how EdgeClaw itself is
used* without the source tree -- and without being pointed at a file by hand. What ships is
the manual, not this project's design notes: this chapter, the man page, the input-syntax
and local-shell notes, the tool-output limits and the prompt library. A released file whose
bytes differ from the bundled copy is rewritten on the next start (an upgraded binary never
leaves an old manual behind); files you add to that directory are left alone.

## Quick Start

```bash
$ edgeclaw              # start the interactive TUI
```

On first run EdgeClaw creates `config.json` with defaults and opens the guided
setup wizard once (until it is completed). Reopen it any time with `/connect`: it
walks through three steps — provider, API key, model — and applies the choice
immediately (it writes the base URL, key, model and the model's context window).

While the input starts with `/`, a completion list hangs just below it: `↑`/`↓` move
the selection, `Tab` completes it, `Esc` dismisses it. `Enter` sends what you typed —
except once you have walked the list with `↑`/`↓`: then `Enter` picks the highlighted row
(what `Tab` does) instead of sending, and the next `Enter` sends.

With no completion list open, `↑`/`↓` recall earlier messages — but only the ones that
start with what you have already typed (an empty line recalls everything). Type `task `
and `↑` walks the `task …` lines; when nothing matches, `↑` keeps moving the caret
instead. `↓` past the newest match restores the draft you were typing.

Two keys interrupt things. **Esc** stops the foreground group's running task (what
`/task stop` does); while the completion list is open, `Esc` dismisses the list instead.
**Ctrl+C** clears the input box and shows "Ctrl+C again to quit" in the footer in place of
the working directory — press it a second time (within ~1.5s) to quit EdgeClaw.

The footer's right end reads `model · ctx used/window · sandbox:on|off` (the context
gauge is the foreground group's current context; a `~` marks an estimate), and while a
task runs the bottom row becomes a progress bar carrying its elapsed time and an "Esc to
stop" hint. That is all it carries: a switch that changes once in a blue moon does not
earn a permanent cell, so the shell-like syntax is reported by `/shell` (and, where it
matters, by the input line itself). The empty input box's placeholder points at `/help`.

Type a message in the input box and press Enter. Conversations are saved per session under `workspace/<session_id>/`.

### Without a terminal (`edgeclaw_cli`)

`edgeclaw_cli` is the same input box without the screen. **The argument is the line you
would have typed** — there is no second command line to learn:

```bash
edgeclaw_cli 'summarize this repository'
edgeclaw_cli '/task new "write the docs"'
edgeclaw_cli '+"explore the parser"'
edgeclaw_cli '"a" | "b"'
edgeclaw_cli '!make -j8'
```

Each of those goes through exactly the path a keystroke does: the same passes, the same
classification (command / local shell / shell line / plain message), the same command table,
the same shell grammar, the same sessions and groups. If a line works in the TUI it works
here; if it does not, it fails with the same message. Unquoted arguments are joined with
single spaces, so `edgeclaw_cli /task new "abc"` is the same line as the quoted form; `--`
ends the flags, for a line that starts with a dash.

What a caller chooses is only **what to do with the output** — the messages exist before the
process decides to print them:

| mode | behaviour |
|---|---|
| (default) | the answer alone on stdout |
| `--json` | every message this run produced, one JSON object per line, ending with `done` |
| `--silent` | nothing — the run is still recorded, so `/task output <id>` has it later |

"Where the answer is" depends on what the line was, and the CLI looks in that order: a
**reply** (a task), a **system message** (a command), a **tool output** (`!cmd` has no reply
at all). That is why it collects messages first instead of assuming one shape.

Exit codes: `0` the line ran to the end, `1` a task it created failed, `2` usage or
configuration (no input, the app could not start), `3` timed out (`--timeout`, default 300s).
stdout carries the answer and nothing else, in every mode, so `> answer.md` is clean;
progress, tool output and failures go to stderr.

```
$ edgeclaw_cli --silent 'Reply with exactly: SILENT-OK'   # nothing printed
$ edgeclaw_cli '/task output 24'
SILENT-OK
```

Two things to know unattended. The **master key** is needed to read the stored API key:
interactively it is asked for on a terminal, so in cron/CI set `EDGECLAW_MASTER_KEY` (running
`edgeclaw` once interactively is also how a fresh install gets configured). And each task
joins its session's foreground group exactly as typed input would — a group's queue is
serial, so a run started from inside a busy session queues like anything else (`+` opens a
group of its own, as it does in the TUI).

`edgeclaw_cli` is a separate binary on purpose: it links the app, **not ftxui**, so a
container or a cron host ships a frontend that cannot take over a terminal by accident (and
about half the size). The interactive `edgeclaw` binary contains no headless code.

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
  AST_SEQ  — ; / && / || chain (order only: ; never gates, && needs success, || needs failure)
    AST_PIPE — | chain (pipe dependency with injection)
      AST_BG     — & background modifier
      AST_REDIR_* — > / >> / 2> redirect
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

## Shell-like Syntax (on by default)

Everything below borrows a shell's vocabulary: the sigil `+` (open a new group) and the
operators `|` `;` `&` `>` `>>` `2>`. **That vocabulary is ON by default** — the operators
are EdgeClaw's own idiom — and it costs ordinary text nothing: a line is only parsed when
it *begins* with a quote or with `+`, so plain conversation needs no escaping and a `|`
inside a sentence is just a character.

```
/shell off
```

With it **off**, every line that is not a `/` command is **one task description,
verbatim** — no operators to get wrong, no sigil to escape. That is the escape hatch for a
workflow where even the quoted forms are unwanted.

The one thing to know while it is on: an **unquoted operand** in a shell line is refused,
never guessed (`"a" | b` comes back with the fix), because nothing says whether that `|`
was a pipe you meant or text you typed.

A line that starts with `/` is **always a command**, never a message. If no command answers
to that word the line is refused with `Unknown command`; it is not sent to the model, and
there is no spelling that turns it into one. The `/` only matters as the *first* character,
so text about a path can simply start with a word: `check /usr/bin for the old binary`.

**A shell line is recognised by its first character.** Every string in shell syntax is
quoted, so a shell line always begins with `"` or `'` — or with its own sigil, `+`. A
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

What follows the closing quote of a **shell line** is the **option area** (`-d <id>`,
`-g <group>`, `2>`, …). Anything else there is refused rather than dropped, so `"hello" and
more` is an error — not a task quietly called `hello`.

A `/` command is different: its words are handed to the command, which reads its own
options out of them (the way `ls` reads `-l`). So there is no "option area" on a command
line and no rule that the description be quoted — `-d`, `--bg` are options only when they
are their own word, and quoting is what makes them text:

```
/task new 写脚本 -d 1      # a task called 写脚本 that waits for task 1
/task new '写脚本 -d 1'    # a task called 写脚本 -d 1; nothing to wait for
```

That is the same distinction a shell makes, and it is the whole reason quoting is worth
having: it says where a word begins and ends, nothing more.

`/` commands are never gated: the command layer is a different thing from the shell layer,
and `/shell` itself has to stay reachable to turn the switch back on. So `/task new`,
`/skill` and `/prompt` keep working either way.

**The rule above the input box tells you how the line you are typing will be read.** While you
are typing, the draft's name is written into the line, near the left, as a filled badge --
`─── message ───` for ordinary text (the empty box says nothing, there is nothing to read yet),
then `─── command ───` (`/…`),
`─── local shell ───` (`!…`), `─── shell ───` (a shell line), `─── syntax error ───` (a line
that already cannot be parsed — the same badge, not an alarm: half-typed quotes are *supposed*
to be unterminated while you type). The label comes
from the same predicate the dispatch uses, so it cannot disagree with what Enter does, and the
prompt below it never moves.

The state is persisted (`config.json` → `shell_syntax`). `/shell on|off` is the only way
to flip it — deliberately no CLI flag, so there is one way to say it and it is the one that
persists — and `/shell` with no argument reports the current state. It is deliberately not
a footer cell: the switch changes once in a blue moon, and its effect shows up where it
matters (a line that will be parsed says so above the input).

**What the switch does *not* govern.** It is about the line you type. Three things sit
outside it:

- **A `/prompt` or `/skill` body is a document, not input to this layer.** It becomes one
  task's input, verbatim; the operators do nothing inside it and the input layer's `{{…}}`
  references are not collected. That is deliberate: a template is authored content, and an
  author must not have the meaning of their template depend on the *reader's* setting —
  they could never reproduce a bug report. (It is the same decision that made skill bodies
  spawn directly, and the same shape as a `'''` block: one literal atom.)
- **`@` is not part of it.** `@file` is a file reference — not a Unix shell vocabulary —
  so it is expected to work with the switch off
  too. The migration hint an old `@desc` gets is the one piece that only makes sense with
  the switch on, since that is the only world where `@desc` ever meant a group.
- **`!` is not part of it.** `!cmd` never enters the parser: it is a one-token dispatch to
  a task that issues one `exec_shell` call, so it means the same thing on or off. The
  switch governs the *vocabulary* of the parser, and `!` is not in that vocabulary — with
  the switch off it would be a plain word that happens to start with a bang, and turning a
  command into a task description is never what a user wants from a safety switch.

---

## Quick Task Syntax (`+`)

Needs the shell-like syntax (**on by default**); `/shell off` turns it off.

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

Not governed by the switch (see above): `!` works with the shell-like syntax **on or off**.

A line starting with `!` runs a **local shell command** on this machine:

```
!git status
```

What happens is worth stating exactly, because it is also how the rest of the system
sees it: the command becomes **a task of its own** (its own line in the sidebar, its own
entry in the group's history) whose only job is to issue one `exec_shell` call. So

- the command runs through `sh -c` (on Windows: `powershell.exe -Command`, with an encoding
  prologue in front of it), in the session's working directory, subject to the
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

## @file References

`@path` attaches a file's content to the task. It is **not** shell syntax: it works with
the switch off, in a message and in a shell line's text alike, because it is a file
reference, not a shell operator.

```
> explain @src/main.c
> 看下 @src/tui/edc_input.hpp#40-80 这个滚动逻辑
> @src/ 目录里有什么
```

Rules:

- The `@` must open a token: line start, or right after whitespace. `foo@bar.com` and
  `@Override` inside pasted code are not references.
- The path runs to the next whitespace. Trailing ASCII punctuation is ignored when
  resolving (`看下 @a.c.` reads `a.c`), and the text is left exactly as typed.
- **Quote the path when it contains a space**: `@"my file.txt"` — same quoting rules as
  everywhere else (`@'…'` and `@$'…'` work too). Quotes are delimiters, and inside them
  nothing is taken back: `@"a.c."` reads the file literally named `a.c.`, where `@a.c.`
  reads `a.c`. An unterminated quote is not a reference at all, so the text is left alone
  rather than read up to the next space.
- `\@` is the escape: it becomes a literal `@` and attaches nothing.
- `@path#12` / `@path#12-20` attaches only those lines (1-based, inclusive; `#12-` runs to
  the end). The range may follow a closing quote: `@"my file.txt"#2-3`.
- A relative path is resolved against the **session's working directory**, and the read
  goes through the sandbox and symlink checks — the same rules the `read_file` tool obeys.
- A directory attaches its listing; a binary file is skipped with a note.
- Caps: 2000 lines / 64 KB per file, 128 KB per message. Truncation says so.
- A missing or unreadable file does not drop the message: the task is created and the
  failure appears in the attached content.

**Completion follows the same rule.** Typing `@` opens the picker; it stays open across a
space when a quote is open (`@"my fi` keeps listing) and closes once the quote is. Taking a
row writes back the form the parser can read: a path with a space (or a `#`) is quoted for
you, so `@my` → `my file.txt` inserts `@"my file.txt"`, not `@my file.txt` (which would name
the file `my`). A directory keeps the quote open for the next level (`@"my dir/`), and a
`#12` you already typed survives the completion.

What the model sees: your text keeps `@path` verbatim, and the file's content arrives as
its own message, shaped exactly like a `read_file` result
(`Called the Read tool with the following input: {"path":"src/main.c"}` + the body). Two
consequences: `raw_input`, the transcript, `/task history` and the sidebar keep showing
what you typed, and `{{...}}` references are never expanded inside a file's body.

> There is no input redirect. `<` used to read a file into a task's input; it is refused
> now with a pointer at `@file`, because `@file` does the same job better — anchored at the
> session's directory, sandboxed, capped, and visible in the text.

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
an INTERRUPTED task stays INTERRUPTED until `/task resume <id>` re-derives the state to
return to. Only fold / curation tasks resume by themselves (a half-done background round
is re-dispatched).

- unsatisfied dependencies → `WAITING_DEPS`
- an unfinished tool call → `TOOL_CALLING` (not-yet-started tools are re-run; a
  tool that was interrupted mid-flight gets a synthesized "interrupted" result)
- a pending turn (input not yet answered) → `LLM_PROCESSING`
- otherwise → `IDLE` (ready; there is no separate "waiting for input" phase)

**Failed tasks**: a task that failed mid-run is `ERROR` but keeps its half-finished
turn (and any interrupted tool ledger). `/task resume <id>` continues that turn
instead of starting over — it re-derives the same way (deps / tool chain / turn)
and re-drives it.

**Cancelled tasks**: a task stopped with `/task stop` is `CANCELLED`, but it is not
destroyed — its turn, messages and the work done so far are all kept (the tool
ledger is dropped, since a cancelled tool no longer has a result to wait for).
`/task resume <id>` revives it: the cancel flattened the step to `DONE`, so resume
re-derives the position. A tool call that was in flight when the task was stopped
gets a synthesized "interrupted" result first (the assistant message is the only
record left of it), so the next LLM request stays well-formed. Only `COMPLETED`
cannot be resumed.

- **COMPLETED**: terminal; its output stays readable with `/task output <id>`.
- **WAITING_DEPS**: accepts input and queues it until dependencies resolve.
- **INTERRUPTED**: the process went down while it was running; `/task resume` revives it.
  Like ERROR it propagates to its dependents (a JOIN dependent is cancelled, an order edge
  is voided), and it never holds a group's front.
- **ERROR**: a failed task; `/task resume` continues its unfinished turn.
- **CANCELLED**: stopped with `/task stop`; `/task resume` continues it too.

Those five are the *bucket*. `/task status <id>` also prints an **ended** line, because a
bucket is not a reason: `CANCELLED` covers "the user stopped it", "a `||` branch was
skipped because its source succeeded" and "a dependency was cancelled, so this could not
produce" (`STOPPED` / `SKIPPED` / `BLOCKED`), and `ERROR` covers "it failed by itself" and
"a dependency ended without producing" (`FAILED` / `BLOCKED`). `INTERRUPTED` stands alone.
A `BLOCKED` task also names which dependency blocked it and what that one's end was.
Resuming a task clears the reason (it can move again, so it has no end). A `FAILED` one
also records *why* the model call failed (`AUTH`, `CONTEXT_OVERFLOW`, `RATE_LIMIT`,
`TRANSPORT`, `OTHER`), and `/task resume` repeats it: a bad key or an overflowing context
does not heal by retrying.

State and reason still do not say what a busy task is *doing*: `LLM_PROCESSING` while a
tool runs looks the same as `LLM_PROCESSING` while the reply streams. So `/task status`
also prints a **running** block — but only the calls still in flight, never the ones that
already returned. The tool name and its JSON arguments are both shown, because the name is
not the command (`{"command":"ls -la"}` is). "In flight" is exactly the resume ledger: a
call enters it when the model asks for it (`queued`, not dispatched yet), becomes
`running` when dispatched, and leaves it the moment its result arrives — so a finished
call is not printed, and the block disappears by itself once the task settles. Earlier
turns are never listed; those are `/task history <id>`.

The sidebar's marker says the same thing at a glance — the colour is the state, the shape
is the reason: `■` the active task, `●` anything else, `*` finished while you were not
looking, `□` interrupted, `×` stopped by you, `–` skipped by `||`, `⊘` blocked by a
dependency. The `*` drops as soon as the task's content is in front of you — being the
active task, its output arriving live in the chat pane, or asking for it: `/task output`,
`/task status`, `/task fg`, and equally `/task history` / `/group history` (a transcript
on screen *is* the output; making the user re-read it through a second command to clear a
marker would be busywork). When a session opens with interrupted tasks, one line in the
chat says so (`/task resume <id>` takes one back at a time); it is not written to the
transcript, so it does not come back on the next load.

Two behaviours follow from the reason:

- **`BLOCKED` is not a decision, so nobody has to make it.** When the dependency named in
  `blocked_by` finally produces, the blocked task is put back in dependency-wait, runs on
  its own (`AWAIT_DEPS → READY`, then the normal scan drives it), and a one-line notice
  says so — the same task was just reported as having failed. Its `output` is cleared with
  it (that text described a state that is over). Two things do *not* trigger the release:
  another dependency of the same task finishing (that says nothing about the blocker), and
  the blocker merely being resumed — it has to actually produce. A routine queue advance
  (the next task in a group) is *not* announced.
- **`SKIPPED` cannot be resumed.** `||` decided the branch is not taken, and running it now
  would execute exactly that with the input it was created with. `/task resume` refuses and
  says so; send the command again if you want it to run.

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

Everything in this section needs the shell-like syntax (see above): it is **on by
default**, and `/shell off` turns it off.

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

**Arguments follow bash quoting.** Every `/command` tail is split the way a shell
splits words, and nothing is expanded (no `$VAR`, globs or backticks):

- unquoted blanks separate arguments;
- `'...'` is literal, `"..."` is literal except `\"` and `\\`;
- escapes have a home of their own, as in bash: `$'...'` (ANSI-C) is the only form
  that decodes `\n`, `\t`, `\xHH`, `\uHHHH` -- that is how a newline or a tab is
  written now (a real newline inside quotes works too); in-tree alias: `'''...'''`
  (its ANSI-C semantics took over the old "raw block" spelling);
- a value containing spaces has to be quoted: `/session new dev 'D:\my repo'`,
  `/sandbox add-rw 'C:\Program Files\tool'`;
- an unterminated quote is a syntax error, not a silently truncated argument;
- one deliberate Windows concession: outside quotes a backslash is a plain byte (bash
  would eat it), so `D:\github\edgeclaw` needs no quoting; a space therefore has to
  be quoted, not escaped (`'C:\Program Files\x'`, not `C:\Program\ Files\x`).
- free-text tails (`/task fork <id> <desc>`, `/group fold [id] [focus]`) take the
  remaining words joined by single spaces.

### General

| Command | Description |
|---------|-------------|
| `/help` | Show the command list |
| `/whoami` | Show the current session, model, foreground group and active task |
| `/model` | List the current provider's chat models (the current one marked) |
| `/model <id>` | Switch model (persisted; the catalog's context window is applied too) |
| `/model search <q>` | Search chat models across all providers |
| `/prompt` | List the templates in `workspace/prompts/` |
| `/prompt <name> [args…]` | Load a template, substitute its `$1` … `$N` / `$ARGUMENTS` placeholders (those two are the whole vocabulary; `{{…}}` and `${…}` are NOT placeholders), and make the body ONE task — as written. A body is a **document**, not a command line: `\| ; & >` in it do nothing, and `{{…}}` in it is plain text too (the last placeholder takes the remaining args; a body with no placeholder gets the args appended). `--new-group` runs it in a group of its own, `--group <id>` (or `--group=<id>`) in that existing group |
| `/skill` | List the skills — ours, plus any found in a `.claude` / `.agents` / `.pi` / `.opencode` skills directory |
| `/skill <name> [args…]` | Run a skill directly: its `SKILL.md` body goes into this task wrapped in a `<skill name=… location=…>` block (so the model knows where the skill's relative paths hang off); the args are substituted, or appended after the closing tag when the body has none. Same as a prompt body, the text is a **document**: one task, verbatim. `--new-group` / `--group <id>` place it, exactly as for `/prompt` |
| `/reload` | Re-read the resource files and rebuild the cached system prompt (a skill or AGENTS.md edited on disk takes effect; `config.json` still needs a restart) |
| `/connect` | Guided setup: choose a provider, API key, and model (three-step wizard) |
| `/sandbox [on\|off]` | Enable/disable the filesystem sandbox |
| `/sandbox list` | Show the sandbox allow/deny path lists |
| `/sandbox add-ro <dir>` | Make a path read-only (it leaves the rw/deny lists) |
| `/sandbox rm-ro <dir>` | Drop a read-only path |
| `/sandbox add-rw <dir>` | Make a path read-write (it leaves the ro/deny lists) |
| `/sandbox rm-rw <dir>` | Drop a read-write path |
| `/sandbox add-deny <dir>` | Mask a dir (wins over ro/rw; e.g. `~/.ssh`) |
| `/sandbox rm-deny <dir>` | Unmask a dir |
| `/shell [on\|off]` | Turn the shell-like input syntax on or off (on by default; persisted; `/shell` alone reports the state) |

### Session

| Command | Description |
|---------|-------------|
| `/session new [name] <dir>` | Start a new session bound to `<dir>` (the name is optional, the directory is not: an unusable directory fails the command and creates no session) |
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

`<desc>` does not need quotes. Quote it when the description itself contains something
that looks like an option: `/task new 'fix -d 1'` is a description, `/task new fix -d 1`
is a task called `fix` waiting for task `1`. The last word wins as the value of `-d`/`-g`,
and a missing value is a usage error.
| `/task fork <pid> <desc>` | Fork: a child task that leads a group of its own and takes the foreground, inheriting the parent group's memory and a snapshot of its context |

### Task Inspection

| Command | Description |
|---------|-------------|
| `/task list` | List all groups and tasks. Active task marked with `*` (e.g. `[L*]` or `[M*]`). |
| `/task status [tid]` | Show task details (state, deps, output, and the tool call it is running right now) |
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
| `/task resume <id>` | Resume an INTERRUPTED, ERROR or CANCELLED task: re-derives state (deps / tool chain / turn) and drives it; an ERROR or CANCELLED task continues its unfinished turn. The id is **required** — there is no batch form, and no "the obvious one" derivation |
| `/task dep <id> [--drop [<sid>]]` | Show a task's dependency edges (kind, state, and the source's own state). `--drop` voids one edge — or every outstanding one when no source is named — so a task waiting on a source that can never produce again (its session is gone) can run; the drop persists, and the task is re-decided immediately |

### Group

| Command | Description |
|---------|-------------|
| `/group list` | List all process groups |
| `/group info [id]` | A group's context / fold numbers (window, the fold line, how far it is, epoch, failures, fold in flight); no id = the foreground group |
| `/group files [id]` | The files the group's **folded** work read / modified. A snapshot written when a fold lands, not a live view: later edits are not reflected and `exec_shell` calls (`sed -i`, `cat`, `git checkout`) are never tracked |
| `/group history <id>` | The whole group's history, one block per member |
| `/group fold [id] [focus]` | Fold the group now, ignoring the trigger; no id = the foreground group |

### Scheduling

Scheduled jobs live in `workspace/jobs.json` and are created by the assistant (the
`crontab` tool). These commands are how a person sees and manages them:

| Command | Description |
|---------|-------------|
| `/jobs` (or `/jobs list`) | Every job: its id, name, schedule (`cron 0 3 * * *`, `every 2h`, `one-shot`) and when it runs next — local time, how far away, and the zone it was scheduled in |
| `/jobs run <id>` | Fire a job **now**. Its instruction goes through the input pipeline exactly as a timer firing does, so the task is the one the schedule would have created; the schedule itself is left alone |
| `/jobs rm <id>` | Remove a job |

`<id>` is any unique prefix of a job's id (they are uuids, so the first eight characters
are enough). An unknown or ambiguous prefix changes nothing and says which it was.

---

### Where a template's task goes

A `/prompt` or `/skill` task joins the group in front, unless you say otherwise:

```
> /prompt --new-group review src/main.c        # a fresh conversation for this one
> /prompt review --new-group src/main.c        # same thing; position does not matter
> /prompt --group grp-3 review src/main.c      # into an existing group
> /prompt --group=grp-3 review src/main.c      # the same, equals form
> /prompt --bg review src/main.c               # alongside: no queueing, no front
```

`--new-group` and `--group <id>` (equals form `--group=<id>` too) may stand before or after
the template name.

A group runs one task at a time, and that applies whether or not it is the group in front:
a task sent to a group that is still busy waits for the task already running there.

To run something alongside instead, say so explicitly: `&` on a shell line, `--bg` on
`/task new`, `/prompt` or `/skill`. A `--bg` task does not queue and does not take the
front — with `--new-group` it opens a group and leaves the group in front alone. A `--…` token
anywhere on the line is read as an option, so **a template argument may not start with `--`**
— quote it (`/prompt review "--strict"`) when a template really needs that word. Single
dashes are ordinary arguments (`/prompt review -strict`).

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
`$ARGUMENTS`), so the two layers never collide.

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
```

Note: there is no input redirect. To give a task a file's content, reference it with
`@path` (see `@file` below): `<` used to do this, and it was removed because `@file`
does it better — relative to the session's directory, through the sandbox, with a size
cap, and visible in the text the model reads.

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
| `~/.edgeclaw/workspace/skills/` | Custom skill SKILL.md files |

### Where prompts and skills come from

`/prompt` and `/skill` read more than `~/.edgeclaw/workspace`: the directories other agent
tools use are searched as well, so an existing template or skill works here unedited.
Precedence — the first hit wins:

| # | Root |
|---|------|
| 1 | the **project**, walking up from the session's directory: `.edgeclaw` `.claude` `.agents` `.opencode` `.pi` (nearest level first) |
| 2 | `~/.edgeclaw/workspace` (ours, the only writable one) |
| 3 | the user's: `~/.claude` · `~/.agents` · `~/.config/opencode` · `~/.pi/agent` |

A root may keep templates in `prompts/` or `command/` `commands/`, and skills in `skills/`
or `skill/`. The project level can be switched off with
`load_project_resources: false` in `config.json`.

**A collision is reported, not swallowed.** When the same name exists in two roots the
higher one wins and the shadowed copy is named: a warning in the log, and a
`_(shadowed: …)_` line under it in `/prompt` and `/skill`.

## See Also

- Architecture: `docs/task-session-architecture.md`
- Source: <https://github.com/edgeclaw-chat/edgeclaw>
