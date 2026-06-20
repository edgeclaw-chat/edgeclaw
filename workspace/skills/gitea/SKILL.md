---
name: gitea
description: Comprehensive Gitea operations using the tea CLI tool. Use this skill when the user asks to manage Gitea issues, releases, repositories, branches, organizations, milestones, labels, or notifications. Handles creating, listing, editing, and deleting various Gitea entities. Use for any Gitea-related task except creating pull requests (use gitea-pr skill for that).
---

# Gitea Operations Skill

Comprehensive Gitea management using `tea` CLI.

## Prerequisites

Verify tea is configured:
```bash
tea whoami
tea login list
```

If not configured:
```bash
tea login add --name <name> --url <gitea-url> --token <token>
```

## Common Options

Most commands support:
- `-r, --repo`: Override repository (e.g., `owner/repo`)
- `-R, --remote`: Use specific remote
- `-l, --login`: Use specific login
- `-o, --output`: Output format (simple, table, csv, json, yaml)
- `--limit`: Limit results
- `-p, --page`: Pagination

---

## Issues

### List Issues
```bash
tea issues list                    # Open issues in current repo
tea issues --state all             # All issues
tea issues -k "search term"        # Search by keyword
tea issues -L "bug,urgent"         # Filter by labels
tea issues -m "v1.0"               # Filter by milestone
tea issues -a username             # Filter by assignee
tea issues -r owner/repo           # Specific repo
```

### Create Issue
```bash
tea issues create \
  --title "Issue title" \
  --description "Description here" \
  --labels "bug,help wanted" \
  --assignees "user1,user2" \
  --milestone "v1.0"
```

### Edit Issue
```bash
tea issues edit <issue-index> --title "New title"
tea issues edit <issue-index> --labels "bug"
```

### Close/Reopen Issue
```bash
tea issues close <issue-index>
tea issues reopen <issue-index>
```

### View Issue Details
```bash
tea issues <issue-index>           # Show details
tea issues <issue-index> --comments  # With comments
```

---

## Releases

### List Releases
```bash
tea releases list
tea releases list -r owner/repo
```

### Create Release
```bash
tea releases create \
  --tag v1.0.0 \
  --target main \
  --title "Version 1.0.0" \
  --note "Release notes here" \
  --asset ./dist/app.tar.gz \
  --asset ./dist/checksums.txt
```

For multi-line notes:
```bash
tea releases create \
  --tag v1.0.0 \
  --title "v1.0.0" \
  --note "$(cat <<'EOF'
## Changes
- Feature A
- Bug fix B

## Installation
Instructions here
EOF
)"
```

Or from file:
```bash
tea releases create --tag v1.0.0 --note-file CHANGELOG.md
```

### Draft/Pre-release
```bash
tea releases create --tag v1.0.0-beta --draft        # Draft release
tea releases create --tag v1.0.0-rc1 --prerelease    # Pre-release
```

### Delete Release
```bash
tea releases delete <tag-or-id>
```

### Manage Assets
```bash
tea releases assets list <release-tag>
tea releases assets delete <release-tag> <asset-name>
```

---

## Repositories

### List Repositories
```bash
tea repos list                      # Your repos
tea repos list --starred            # Starred repos
tea repos list --watched            # Watched repos
tea repos search "search term"      # Search repos
```

### Create Repository
```bash
tea repos create \
  --name my-repo \
  --description "Repo description" \
  --private \
  --init                            # Initialize with README
```

### Fork Repository
```bash
tea repos fork owner/repo
```

### Delete Repository
```bash
tea repos delete owner/repo
```

### View Repository Details
```bash
tea repos owner/repo
```

---

## Branches

### List Branches
```bash
tea branches list
tea branches                        # Shortcut
```

### View Branch Details
```bash
tea branches <branch-name>
```

### Protect/Unprotect Branch
```bash
tea branches protect main
tea branches unprotect main
```

---

## Organizations

### List Organizations
```bash
tea organizations list
tea orgs list                       # Shortcut
```

### Create Organization
```bash
tea organizations create --name my-org
```

### Delete Organization
```bash
tea organizations delete my-org
```

### View Organization Details
```bash
tea organizations my-org
```

---

## Milestones

### List Milestones
```bash
tea milestones list
tea milestones list --state all
```

### Create Milestone
```bash
tea milestones create \
  --title "v1.0" \
  --description "First release" \
  --deadline "2024-12-31"
```

### Close/Reopen Milestone
```bash
tea milestones close "v1.0"
tea milestones reopen "v1.0"
```

### Delete Milestone
```bash
tea milestones delete "v1.0"
```

### View Issues in Milestone
```bash
tea milestones issues "v1.0"
```

---

## Labels

### List Labels
```bash
tea labels list
```

### Create Label
```bash
tea labels create --name "bug" --color "#ff0000"
```

### Update Label
```bash
tea labels update <label-id> --name "enhancement"
```

### Delete Label
```bash
tea labels delete <label-id>
```

---

## Notifications

### List Notifications
```bash
tea notifications list              # Current repo
tea notifications list --mine       # All repos
tea notifications list --states unread,pinned
```

### Mark Notifications
```bash
tea notifications read              # Mark all as read
tea notifications read <notification-id>
tea notifications unread <notification-id>
tea notifications pin <notification-id>
tea notifications unpin <notification-id>
```

---

## Comments

### Add Comment
```bash
tea comment <issue-or-pr-index> --comment "Your comment"
```

For multi-line:
```bash
tea comment <issue-index> --comment "$(cat <<'EOF'
## Review Comment
- Point 1
- Point 2
EOF
)"
```

---

## API Direct Access

For operations not covered by tea commands:
```bash
tea api /repos/{owner}/{repo}/issues
tea api -X POST /repos/{owner}/{repo}/issues -d '{"title":"test"}'
```

---

## Workflow Examples

### Create Issue with Label and Milestone
```bash
tea issues create \
  -t "Bug in login flow" \
  -d "Users cannot login after password reset" \
  -L "bug,urgent" \
  -m "v1.0" \
  -a "maintainer"
```

### Release with Assets
```bash
git tag v1.0.0 && git push origin v1.0.0

tea releases create \
  --tag v1.0.0 \
  --title "v1.0.0 - First Stable Release" \
  --note "$(cat CHANGELOG.md)" \
  -a ./build/app-linux.tar.gz \
  -a ./build/app-macos.tar.gz \
  -a ./build/app-windows.zip
```

### Setup Project Milestones
```bash
tea milestones create -t "v0.1 - MVP" -d "Minimum viable product"
tea milestones create -t "v0.2 - Beta" -d "Beta release with feedback"
tea milestones create -t "v1.0 - Stable" -d "First stable release"
```

---

## Tips

1. **Repo Context**: Run tea commands from within a git repository for automatic repo detection
2. **Output Formats**: Use `-o json` for scripting, `-o yaml` for config files
3. **Pagination**: Use `--limit` to control output size
4. **Multiple Logins**: Use `-l <login-name>` to switch between Gitea instances
5. **Dry Run**: Add `--debug` to see API calls without executing
