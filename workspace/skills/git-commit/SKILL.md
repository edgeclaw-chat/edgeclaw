---
name: git-commit
description: Generate and execute git commits following Conventional Commits format (without emoji). Use this skill when the user asks to commit changes, create a commit, or mentions anything related to git commit workflow. Automatically detects change scope and generates English commit messages.
---

# Git Commit Skill

This skill handles the complete git commit workflow: analyzing changes, generating a Conventional Commits formatted message, and executing the commit.

## Workflow

When asked to create a commit, follow these steps:

### 1. Gather Information

Run these commands in parallel:
- `git status` - see staged and unstaged changes
- `git diff --staged` - see staged changes
- `git diff` - see unstaged changes
- `git log --oneline -10` - understand recent commit style

### 2. Analyze Changes

Determine:
- **Type**: What kind of change is this?
  - `feat`: New feature
  - `fix`: Bug fix
  - `docs`: Documentation only
  - `style`: Formatting, no code change
  - `refactor`: Code change that neither fixes nor adds feature
  - `perf`: Performance improvement
  - `test`: Adding/updating tests
  - `build`: Build system or dependency changes
  - `ci`: CI configuration changes
  - `chore`: Other changes that don't modify src or test files
  - `revert`: Revert a previous commit

- **Scope** (optional): Which module/component is affected?
  - Look at file paths: `src/bc_agent.c` → scope could be `agent`
  - Look at directory structure
  - Common scopes in this project: `agent`, `channels`, `tools`, `llm`, `context`, `skills`
  - If changes span multiple scopes, either pick the primary one or omit scope

- **Description**: A concise summary in imperative mood, lowercase, no period at end
  - Good: `add email reply support`
  - Bad: `Added email reply support.`

### 3. Stage Changes

If there are unstaged changes that should be committed:
```bash
git add <files>
```

Stage all relevant files. Be careful NOT to stage:
- Files in `.cache/` directories
- Files that look like secrets or credentials
- Files the user didn't explicitly want to commit

### 4. Generate and Execute Commit

Create the commit message following this format:
```
<type>(<scope>): <description>

[optional body]
```

**Rules:**
- No emoji
- English only
- Lowercase description
- Imperative mood ("add" not "added")
- No period at end of description
- Scope is optional but encouraged when clear
- Body is optional, use for complex changes

**Examples:**
- `feat(channels): add email auto-reply support`
- `fix(agent): resolve memory leak in session handling`
- `docs: update README with build instructions`
- `refactor(tools): simplify shell command execution`
- `perf(llm): cache tool schemas to reduce overhead`

Execute:
```bash
git commit -m "<message>"
```

If the commit has a body:
```bash
git commit -m "<subject>" -m "<body>"
```

### 5. Verify

Run `git status` after commit to confirm success.

## Important Notes

- Never force push unless explicitly requested
- Never use `--no-verify` unless explicitly requested
- If pre-commit hooks fail, report the error and let the user decide
- Don't commit files that look like secrets (.env, credentials, API keys)
- Always show the generated commit message to the user before executing (or immediately after for transparency)
