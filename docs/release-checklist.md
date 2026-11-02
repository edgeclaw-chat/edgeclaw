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

**Pushing to `dev` runs no CI.** `PR Check` triggers on `pull_request → master` and `Release`
on push to `master` or a `v*` tag, so a commit that only lands on `dev` has never been built
by anything but the machine that wrote it. Open the `dev → master` PR and let it run — that
PR is the first time CI sees the work, not a formality after it.

- [ ] The `dev → master` PR is green: the `build` matrix, `test`, `asan`, the licence check
      and the archive check. `release` will not start until `build`/`test`/`asan` pass.
- [ ] `bazel build //tests:all` passes. It is a separate check from `bazel test` because
      `manual`-tagged targets compile only under `build`; two of them once sat broken for
      months.
- [ ] Working tree clean, and `dev` pushed. A release built from a dirty tree cannot be
      reproduced from its tag.

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

## 6. Tag and publish

```bash
# merge dev -> master through a PR (never commit to master directly)
git tag -a vX.Y.Z -m "vX.Y.Z: <one line on what this release promises>"
git push origin vX.Y.Z
```

- [ ] The `release` job ran and produced `edgeclaw_{amd64,aarch64,arm}_{linux,macos}.tar.gz`
      plus the device-agent archives.
- [ ] Download one archive from the release page — not from `bazel-bin` — and confirm it
      starts and prints the version you tagged.
- [ ] Release notes say what changed for a *user*; the checklist above is for you, not them.

## 7. After

- [ ] If the release fixed something listed in `docs/limitations.md`, move that entry.
- [ ] If it added a limitation, add it there too.
- [ ] Close the issues the release actually closed. "Fixed in vX.Y.Z" beats silence.
