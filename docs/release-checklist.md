# Release checklist

The steps that turn a green `dev` into a published version. Written down because the
expensive mistakes here are the ones nobody remembers to check: a release built from an
untested commit, or an archive that redistributes a library without its licence.

**Branch model.** `dev` is the trunk — all work lands there. `master` is the release branch:
it is only ever a merge of `dev` (the history is literally `Merge pull request #NN from
edgeclaw-chat/dev`), never committed to directly. A version is a tag on `master`.
`.github/workflows/release.yml` builds on every push to `master` (an early warning that the
release builds still work) and publishes only for `v*` tags.

---

## 1. Before you start

**Every push to `dev` now runs `Build` and `Tests`** (`build.yml`, `tests.yml`). Before
2026-11-03 it ran nothing — `PR Check` is pull-request-only and `Release` is master/tag-only,
so trunk was the one branch CI never touched. If a badge on the README reads "no status",
this is the first thing to check: the workflow may simply not have run yet.

- [ ] The latest `dev` push is green: `Build`, and both `Tests` jobs that gate a release
      (`Test`, `AddressSanitizer + UBSan`). `release` will not start until `build`/`test`/
      `asan` pass on the `dev → master` PR.
- [ ] The `dev → master` PR is green: the ten-platform `build` matrix, `test`, `asan`, the
      licence check and the archive check.
- [ ] `bazel build //tests:all` passes. It is a separate check from `bazel test` because
      `manual`-tagged targets compile only under `build`; two of them once sat broken for
      months.
- [ ] Working tree clean, and `dev` pushed. A release built from a dirty tree cannot be
      reproduced from its tag.

### CI status: badges, and why they may look broken

`README.md` shows four badges. What each one needs, and what it does when that is missing:

| Badge | Source | Requires |
|---|---|---|
| `build`, `tests` | shields.io, reading `build.yml` / `tests.yml` on `dev` | **A public repository.** |
| `coverage` | codecov.io, reading `master` | The repo added on codecov.io; no secret (OIDC). |
| `license` | static shields badge | nothing |

**On a private repository the GitHub Actions badges cannot render, and no configuration
changes that.** This repository is public now, so the two workflow badges work; the note is
kept because it is the first thing to suspect if they ever go blank again. Measured
2026-11-03, while the repository was private:

- GitHub's own endpoint (`.../actions/workflows/<file>/badge.svg`) returns **404** — with and
  without a token, and with `?token=<PAT>`.
- shields.io returns HTTP 200 but the SVG text is **"repo or workflow not found"**; the same
  URL against a public repository renders "passing", so the URL is right and the visibility
  is what differs.

So the two workflow badges only start showing a status once the repository is public. That is
a decision, not a bug to chase.

**Coverage** is different: codecov serves that badge itself, so it works either way. Its
GitHub App is installed on the organisation, which is all the integration needs -- there is
no upload token to create or rotate. `tests.yml`'s `coverage` job authenticates through
OIDC (`use_oidc: true`, which requires the job's `id-token: write` permission) and uploads
`bazel-out/_coverage/_coverage_report.dat` on every run.

`fail_ci_if_error: false` is deliberate: Codecov is a signal, not a gate, and an outage
there must not turn a green test run red. The consequence is that a broken upload is
**quiet** — the job stays green -- so when changing anything about it, open the run's
`Coverage` job log and look for `Sending upload (...) to storage` followed by
`Upload queued for processing complete`. A run that merely stays green proves nothing.

The `Coverage summary` step after it prints lcov's three totals into the job log on every
run (`if: always()`), which is the record of what was measured independent of whether the
upload landed. Note that it will not agree with the badge: lcov's `LH/LF` counts a line
whose branches were not all taken as hit, while Codecov does not.

One trap when editing that step: the report path is written literally
(`bazel-out/_coverage/_coverage_report.dat`) on purpose. `bazel-out` is a symlink and `find`
does not follow it, so discovering the file with `find bazel-out` yields an empty string and
the step dies with `FileNotFoundError: ''`. That happened on the workflow's first run.

## 2. Decide the version

**This release is `v2.0.0`.** Decided 2026-11-02: `v1.0.0`–`v1.6.2` are development
snapshots (the first is literally `init(project)`), so a "1.0" promise made now would be
indistinguishable from them. A major bump is the one number that cannot be confused with
what came before, and this release does change things users depend on — the database is
migrated in place instead of refused, and the shell tool now reports exit status.

- [ ] Confirm the number is still right (has anything shipped since this was written?).
- [ ] Say why in the annotated tag message, not just the number.
- [ ] `./edgeclaw --version` reports the *right* thing. The version string is derived at
      build time from `git describe`, so a build from a stale cache shows the previous
      describe. Confirm on the CI artifact, not locally.

## 3. Migrations

The one thing that can silently destroy a user's history.

- [ ] `bazel test //tests:test_state_migrate` is green.
- [ ] If `EDC_DB_VERSION` changed since the last release: run the new binary against a
      **copy** of a real older database and confirm (a) a backup was written and named in the
      log, (b) the backup opens and is still the old version, (c) the sessions are readable
      afterwards, (d) row counts are unchanged.
- [ ] `SQLITE_TMPDIR` points at the same filesystem as the database when doing that — a small
      `tmpfs` there produces ENOSPC disguised as "database disk image is malformed".

## 4. Licences

- [ ] `python3 tools/gen_notices.py --check` passes (CI runs it, after the build, so the
      external repositories exist).
- [ ] If a dependency was added or bumped: run `tools/gen_notices.py`, confirm the SPDX
      identifier against the licence text that was actually copied, and commit the result.

## 5. Before tagging

- [ ] `bazel build //:edgeclaw //:device_agent` and inspect one archive:

      tar tzf bazel-bin/*/edgeclaw_*.tar.gz | grep -E 'LICENSE|NOTICE|licenses/'

  Expect `LICENSE`, `THIRD_PARTY_NOTICES.md`, and one `licenses/<component>/LICENSE` per
  redistributed component.
- [ ] `docs/limitations.md` is current. Anything fixed in this release that appears there
      should move to its "fixed at v2.0.0" section in the same commit.
- [ ] The sanitizer gate is what this checklist says it is (hard or
      report-only right now?).
- [ ] The coverage number quoted in `docs/limitations.md` matches what Codecov shows for
      `master`. It is re-measured, not remembered: the document once quoted a baseline two
      months and 585 lines out of date, which made it wrong in a way no test would catch.
      Do not compare it against the log's `Coverage summary` -- that prints lcov's `LH/LF`,
      which counts partially-covered lines as hit and reads ~18 points higher.

## 6. Tag and publish

```bash
# merge dev -> master through a PR (never commit to master directly)
git tag -a vX.Y.Z -m "vX.Y.Z: <one line on what this release promises>"
git push origin vX.Y.Z
```

- [ ] The `release` job ran and produced one archive per matrix entry — the five
      `edgeclaw_*` targets (`amd64_linux`, `arm_linux`, `aarch64_linux`,
      `aarch64_macos`, `amd64_windows`), the three `*_linux_musl` variants, and the
      `device_agent_*` set — each attached under **two** names: `<target>_vX.Y.Z.tar.gz`
      and the version-less `<target>.tar.gz` that the README's
      `releases/latest/download/` link depends on. A tarball with only one of the two
      means "Collect artifact" lost a line.
- [ ] Download one archive from the release page — not from `bazel-bin` — and confirm it
      starts and prints the version you tagged.
- [ ] Release notes say what changed for a *user*; the checklist above is for you, not them.

## 7. After

- [ ] If the release fixed something listed in `docs/limitations.md`, move that entry.
- [ ] If it added a limitation, add it there too.
- [ ] Close the issues the release actually closed. "Fixed in vX.Y.Z" beats silence.
