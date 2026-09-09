# Available Tools

Tools available via function-calling.

---

## File Operations

### read_file
Read the contents of a file.
```
read_file(path: str) -> str
```

### write_file
Write content to a file (creates parent directories if needed).
```
write_file(path: str, content: str) -> str
```

### edit_file
Perform a small, exact replacement in a file. Use for narrow text substitutions.
```
edit_file(path: str, old_text: str, new_text: str) -> str
```
When `old_text` matches multiple times, these optional params help disambiguate:
- `replace_all` (bool): Replace all occurrences
- `occurrence` (int): 1-based specific occurrence
- `line_hint` (int): 1-based line number hint
- `expected_replacements` (int): Guard for expected match count

If file doesn't exist and `old_text` is empty, creates the file with `new_text` as content.

### list_dir
List contents of a directory.
```
list_dir(path: str) -> str
```

---

## Shell Execution

### exec_shell
Execute `sh -c '<command>'` and return output.
```
exec_shell(command: str, timeout: int = 30) -> str
```
- Default timeout 30s, max 3600s
- Output truncated at 10,000 characters
- Non-zero exit codes returned as errors with stdout+stderr content

---

## Device Communication

Connect to a remote device agent via TCP or PPPoS.

### device_shell
Execute a shell command on a remote device.
```
device_shell(command: str, timeout: int = 30) -> str
```

### device_upload
Upload a file from host to device (chunked transfer for large files).
```
device_upload(local_path: str, remote_path: str, mode: int = 0o644, timeout: int = 60) -> str
```

### device_download
Download a file from device to host.
```
device_download(remote_path: str, timeout: int = 60) -> str
```

---

## Task Management

### todowrite
Validate and update the todo list. Send a complete new list each time.
```
todowrite(items: list[Todo]) -> str
```

Each Todo item:
```
content: str   — Task description
status: str    — "pending" | "in_progress" | "completed" | "cancelled"
priority: str  — "high" | "medium" | "low"
```

Rules: max 20 items, only one `in_progress` at a time.

---

## Scheduling

### crontab
Schedule reminders and recurring tasks.
```
crontab(action: "add"|"list"|"remove", ...) -> str
```

```
# Recurring (cron expression)
crontab(action="add", name="morning", message="Good morning!", cron_expr="0 9 * * *")

# Recurring (interval in seconds)
crontab(action="add", name="water", message="Drink water!", every_seconds=7200)

# One-time
crontab(action="add", name="meeting", message="Meeting starts!", at="2026-07-01T15:00:00")

# Manage
crontab(action="list")
crontab(action="remove", job_id="<id>")
```

---

## Web Access

### web_search
Search the web using Brave Search API.
```
web_search(query: str, count: int = 5) -> str
```

### web_fetch
Fetch and extract main content from a URL.
```
web_fetch(url: str, extractMode: str = "markdown", maxChars: int = 50000) -> str
```
- `extractMode`: "markdown" or "text"
- Output truncated at `maxChars` (default 50,000)

---

## Communication

### message
Send a message to a specific chat channel.
```
message(content: str, channel: str = None, chat_id: str = None) -> str
```

### spawn
Spawn a subagent to handle a task in the background.
```
spawn(task: str, label: str = None) -> str
```

---

## Heartbeat Tasks

`HEARTBEAT.md` is checked every 30 minutes. Use `edit_file` to manage periodic tasks:

```
# Add
edit_file(path="HEARTBEAT.md", old_text="## Active Tasks", new_text="- [ ] New task\n\n## Active Tasks")

# Remove
edit_file(path="HEARTBEAT.md", old_text="- [ ] Task to remove\n", new_text="")
```

Keep this file short.
