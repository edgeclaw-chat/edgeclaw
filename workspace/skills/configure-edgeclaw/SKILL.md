---
name: configure-edgeclaw
description: Use when the user is changing EdgeClaw's own configuration or resources — config.json, a skill, a prompt template, a resource root, the sandbox path lists, or a provider — and also when something they configured is not taking effect. Do not use it for the user's own project code.
disable-model-invocation: false
---

# Configuring EdgeClaw

Everything a user can change about EdgeClaw is a file. There are no hidden settings and no
precedence rules beyond "the first root that has the name wins", so the work is mostly
finding the right file and knowing whether it needs a restart.

## Where the files are

```
<home>/config.json              provider, model, window, sandbox, switches
<home>/workspace/               the workspace root: the only writable resource root
<home>/workspace/skills/<n>/SKILL.md
<home>/workspace/prompts/<n>.md
<home>/AGENTS.md                user-global project instructions
<home>/log/edgeclaw-<date>.log  the log; one file per day
```

`<home>` is `~/.edgeclaw` unless overridden. `edgeclaw --version` prints the platform if you
need to confirm which binary is running.

## Restart, `/reload`, or neither

This is the part people get wrong, and it is the first thing to check when an edit "did
nothing":

| What changed | What it takes |
|---|---|
| `config.json` | **restart** — `/reload` does not re-read it |
| a `SKILL.md` or a prompt template | `/reload` rebuilds the cached system prompt |
| an `AGENTS.md` | `/reload` |
| adding a skill directory | `/reload` — the skill list is part of the system prompt |
| a sandbox path list | restart (`config.json`) |

`/reload` exists because the system prompt is held constant across a conversation to keep
the provider's cache prefix intact. Editing a skill on disk does not change the prompt the
model already has; `/reload` is what swaps it.

## config.json

Top-level keys, and the ones that matter most:

| Key | Type | Default | Notes |
|---|---|---|---|
| `api_url`, `api_key`, `model` | string | — | set by `/connect`; the key is stored encrypted |
| `context_window` | number | `1048576` | **must match the model**. Too large postpones folding past the provider's real limit; too small folds every round. `<= 0` is repaired to the default on startup |
| `tool_max_output_size` | number | — | cap on tool output handed to the model; the tool layer has its own byte caps on top |
| `sandbox` | object | — | `enable` (bool), `rw_paths`, `ro_paths`, `deny_paths` (arrays of strings) |
| `shell_syntax` | bool | **true** | the `\|` `;` `&` `>` operators and the `+` / `!` sigils. Flip at runtime only with `/shell on\|off` |
| `load_project_resources` | bool | true | read `.edgeclaw` `.claude` `.agents` `.opencode` `.pi` from the project tree. The `$HOME` roots are never gated by this |
| `skip_context_files` | bool | false | do not read `AGENTS.md` / `CLAUDE.md` |
| `onboarded` | bool | false | the first-run wizard flag; clearing it re-opens the wizard |
| `workspace` | string | `<home>/workspace` | |
| `device_*` | — | — | `device_transport`, `device_ip`, `device_port`, `device_pty`, `device_baud` for the remote agent |
| `services_enabled` | bool | **true** | background services (crontab scans, memory curation) |
| `max_iter` | number | `-1` | tool-call round cap per task; `-1` = unlimited |

Two rules that save a debugging round:

- **`api_key` is encrypted.** Writing a plaintext key into `config.json` by hand does not
  work; use `/connect`, or set `EDGECLAW_MASTER_KEY` and let the binary encrypt it.
- **A malformed `config.json` is a startup failure, not a silent fallback.** If the binary
  will not start, read the log before editing anything else.

## Writing a skill

A skill is a directory with a `SKILL.md`. It is instructions plus whatever files sit beside
it; the model sees only the name and description until it decides to read the body.

```markdown
---
name: pdf-tools
description: Extract text and tables from PDFs. Use when the user asks about a PDF's contents.
---
# PDF tools

...instructions, with paths relative to this file...
```

- `description` is the **only** thing the model uses to decide whether to read the skill.
  Write it as a trigger ("Use when …"), not as a summary, and say when *not* to use it.
- `disable-model-invocation: true` keeps the skill out of the system prompt entirely; it then
  runs only via an explicit `/skill <name>`. Use it for anything destructive or expensive.
- The body is handed to the model verbatim, wrapped in a `<skill name=… location=…>` block,
  so **relative paths in the body resolve against the skill's directory**.
- Nothing is loaded at startup beyond the front matter; do not worry about a large body.

## Writing a prompt template

A template is one `.md` file whose file name is the command:

```markdown
---
description: Review staged git changes
argument-hint: "[focus]"
---
Review the staged changes. Focus on $1.
```

- Placeholders are `$1` … `$N` and `$ARGUMENTS`. Nothing else — `$@`, `${1}`, `${1:-x}` and
  `{{…}}` are all plain text.
- The **last** positional placeholder takes every remaining argument.
- A body with **no** placeholder gets the user's arguments appended at the end.
- The body is a document, not a command line: `|` `;` `&` `>` inside it do nothing, and the
  `/shell` switch does not change that.
- `argument-hint` is display only; it does not validate anything.

## Resource roots, and why an edit may be invisible

Templates and skills are read from several roots; **the first root that has a given name
wins**, and the shadowed copy is reported (`_(shadowed: …)_` in `/skill`).

```
project (walking up from the session's directory): .edgeclaw .claude .agents .opencode .pi
then  <home>/workspace        (ours, the only writable one)
then  $HOME: .claude .agents .config/opencode .pi/agent
```

So if a skill "does not exist" but the file does, check for a same-named skill in a root
closer to the session — the nearer one is silently the winner. `/skill` lists the source of
each entry for exactly this reason.

## Sandbox

`/sandbox list` shows what is enforced right now; it is the only trustworthy answer, because
enforcement differs by platform. `/sandbox add-ro` / `add-rw` / `rm-ro` / `rm-rw` edit the
lists in place (a path added to one list leaves the others), so the editing does not need a
hand-written `config.json`.

- `kernel_sandbox` is real on **Linux and macOS** only. On Windows there are path checks and
  nothing more, `/sandbox on` is refused, and `--no-sandbox` does not take effect.
- The lists constrain what **tools** may touch. They do not constrain what the model can be
  told — a file the model already read is in the context regardless.
- `deny_paths` applies even to resource reads (templates, `SKILL.md`), which otherwise bypass
  the rw/ro lists by design so that resources outside the workspace stay usable.

## Provider and model

`/connect` is the guided path and is the only one that stores the key correctly. For a
provider the wizard does not list, set `api_url` to an OpenAI-compatible base URL and
`model` to the model id, then restart. The model catalog (what the picker offers) is
generated at build time, so a model newer than the binary needs the id typed in, not looked
up.

## Before reporting a configuration problem

Check these in order — most "it does not work" reports are one of them:

1. Did it need a restart instead of `/reload`? (table above)
2. Is a nearer resource root shadowing the file? (`/skill`, or the log)
3. Is the value actually being read, or is it the default? (`/sandbox list`, `/model`)
4. Does the log say why? `<home>/log/edgeclaw-<date>.log`
