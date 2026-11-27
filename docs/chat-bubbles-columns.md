# chat_bubbles: from single encoded line to typed columns (v19)

## Why

The highlighter (P1–P5) paints fenced code blocks from a language tag. To
highlight **tool output** (`read_file` etc.) consistently across restarts, the
language must be persisted alongside the bubble. V9–v18 stored each bubble as
one tab-separated escaped `line`; this change splits that line into real SQL
columns and adds `highlight_lang`.

## New schema (v19)

```sql
CREATE TABLE chat_bubbles (
  session_id     TEXT NOT NULL,
  seq            INTEGER NOT NULL,
  kind           TEXT NOT NULL,     -- user|tool|thought|reply|sys
  render_pref    INTEGER NOT NULL,  -- EDC_APP_RENDER_PLAIN / _MARKDOWN
  task_id        TEXT NOT NULL,     -- '' when absent
  header         TEXT NOT NULL,     -- '' when absent
  content        TEXT NOT NULL,
  highlight_lang TEXT NOT NULL DEFAULT '',  -- language tag for code/tool output
  PRIMARY KEY (session_id, seq)
);
CREATE INDEX chat_bubbles_by_session ON chat_bubbles(session_id, seq DESC);
```

All text columns store **raw text** (embedded `\n`/`\t`/`\` are literal, not
escaped): SQLite handles them. The old `\\n` escaper exists only in the v18
`line` format and in the v19 migration that decodes it.

## Migration v18 -> v19

`step_v18_to_v19`:
1. Rename `chat_bubbles` -> `chat_bubbles_v18`.
2. Create the new `chat_bubbles` with the schema above.
3. For each old row `(session_id, seq, line)`: decode the 5 escaped fields
   (kind, render_pref, task_id, header, content) with the C decoder shared
   with the page path; insert into the new table with `highlight_lang=''`.
   `step_v18_to_v19` is written in C because SQLite cannot unescape the old
   fields in SQL.
4. Drop `chat_bubbles_v18`.

`verify_v19`: new table has all 8 columns, no `line` column, and the index
exists. `EDC_DB_VERSION` 18 -> 19; `EDC_SCHEMA_SQL` updated to match (fresh DB
and migrated DB must have the same shape — `test_state_migrate` asserts it).

## Store API (edc_state_store.h)

```c
struct edc_chat_bubble_fields {
  const char *kind; const char *task_id; const char *header; const char *content;
  const char *highlight_lang;
  int render_pref;
};
bool edc_chat_bubble_append(const char *session_id,
                            const struct edc_chat_bubble_fields *f, int64_t *out_seq);
struct edc_chat_bubble_row {
  int64_t seq;
  char *kind, *task_id, *header, *content, *highlight_lang;
  int render_pref;
};
bool edc_chat_bubble_page(... same cursor ...);
```

Single-writer / per-session seq / page ordering semantics are unchanged.

## Codec (edc_chat_bubble_line)

The `line` encoder/decoder is no longer the persistence format. It is kept only
as the v19 migration's decoder (reading legacy `line`). New code passes typed
fields end to end; no escape/unescape on the hot path.

## Write path

- `channel_actor` persists with the fields already on the channel message;
  `highlight_lang` travels on `edc_channel_message` (empty for non-tool kinds).
- `scheduler` sys-hint bubbles pass `highlight_lang=''`.

## Wire + replay

`EncodeChatBubblePageReply` emits each row as
`{seq, kind, render_pref, task_id, header, content, highlight_lang}`.
`ChatBubbleRow` mirrors those fields. `ChatModel` builds `ToolMessage` with
`highlight_lang`; `ToolMessage::OnRender` highlights plain tool output using
`edc_syntax_highlight(content, lang)` when the language is registered,
otherwise the existing `WrappedText` path. Folding/preview behaviour and the
model's own copy of tool output are untouched: content itself is unchanged.

## highlight_lang source

fs tools construct header `"<tool> <path>"`; the toolbox derives the language
from the path extension via a shared helper and sets it on the emitted tool
message. Unknown extension => empty => no highlighting (safe fallback).
