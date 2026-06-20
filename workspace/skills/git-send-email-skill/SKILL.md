---
name: git-send-email
description: Send git patches via email using git send-email. Use this skill whenever the user mentions sending patches, emailing commits, submitting patches via email, or references git send-email, even if they don't explicitly ask for "git send-email." Also trigger when users talk about contributing to projects that use email-based patch submission (like the Linux kernel, Git itself, or other mailing list-driven projects).
---

# Git Send-Email Skill

This skill helps you send git patches via email using `git send-email`.

## Prerequisites

The user must have `git send-email` configured with SMTP settings. This skill assumes the configuration is already complete.

## Usage Patterns

### Pattern 1: Send existing patch files

When the user has `.patch` files ready:

```bash
git send-email --confirm=never --to=<recipient> <patch-file>
```

### Pattern 2: Generate and send from commits

When the user wants to send commits directly:

```bash
git send-email --confirm=never --to=<recipient> <revision-range>
```

Common revision ranges:
- `HEAD~1` - last commit
- `HEAD~3..HEAD` - last 3 commits
- `origin/main..HEAD` - commits not yet upstream
- `<commit-hash>` - single commit

## Workflow

1. **Gather required information:**
   - Recipient email address (required)
   - Patch source: file paths OR commit range
   - Optional: CC list, cover letter, etc.

2. **Validate the patches:**
   - Check patch files exist, or
   - Verify commits exist in the repository
   - Show a summary of what will be sent

3. **Confirm with user:**
   - Display: number of patches, recipient, subject lines
   - Ask for confirmation before sending

4. **Execute send-email:**
   - Run `git send-email` with the --confirm=never option
   - Report success or any errors

## Common Options

| Option | Purpose |
|--------|---------|
| `--to=<email>` | Primary recipient |
| `--cc=<email>` | CC recipient (can use multiple times) |
| `--compose` | Open editor for cover letter |
| `--cover-letter` | Generate cover letter |
| `--subject-prefix=<prefix>` | Add prefix like "PATCH v2" |
| `--in-reply-to=<message-id>` | Thread to existing email |
| `--annotate` | Review/edit each patch before sending |
| `--dry-run` | Preview without sending |

## Examples

**Send a single patch file:**
```bash
git send-email --confirm=never --to=maintainer@example.com 0001-fix-bug.patch
```

**Send last commit:**
```bash
git send-email --confirm=never --to=maintainer@example.com HEAD~1
```

**Send with cover letter and CC:**
```bash
git send-email --confirm=never --to=maintainer@example.com --cc=reviewer@example.com --cover-letter origin/main..HEAD
```

**Send with version prefix (v2):**
```bash
git send-email --confirm=never --to=maintainer@example.com --subject-prefix="PATCH v2" HEAD~1
```

## Safety Notes

- Always confirm with user before sending
- Use `--dry-run` first for new users or complex sends
- Warn if sending many patches at once (>10)
- Check for unintended files in patch directories

## Error Handling

If `git send-email` fails:
1. Check if SMTP is configured: `git config --get sendemail.smtpserver`
2. Verify recipient email format
3. Ensure patches/commits are valid
4. Suggest running with `--dry-run` to debug
