# Contributing to EdgeClaw

Thanks for looking. This file covers what you need to get a change built, tested and
merged. For how the program *works* (commands, tasks, sessions, tools) read
[`docs/edgeclaw.md`](docs/edgeclaw.md) — it is the user manual, and it is the same text
the binary embeds.

## Before you write code

Open an issue first for anything larger than a bug fix. The architecture is opinionated
in ways that are cheap to follow and expensive to work around (see *Conventions* below),
and it is easier to agree on the shape before the patch exists.

For a bug, a failing test that demonstrates it is worth more than a description. Most of
the suite is per-behaviour, so a new `TEST("...")` case next to the existing ones usually
fits without new build wiring.

## Getting a build

Bazel 8.0.0 (`.bazelversion`) plus a C11 / C++17 toolchain. On Windows, run
`tools\windows\setup-build-env.ps1` once first — the README explains why.

```bash
bazel test  //tests:all       # the suite
bazel build //tests:all       # compiles the `manual` targets too
bazel build //...             # everything, including the release tarball
bazel run   //src/tui:edgeclaw
```

**Run both `bazel test` and `bazel build //tests:all`.** `bazel test` silently skips
targets tagged `manual` — the ones that need network, a serial device or a long run — so
they only *compile* under the build command. CI runs both; skipping the second lets those
targets rot, which has already happened twice.

Other entry points worth knowing:

```bash
bazel test //tests:all --config=asan   # AddressSanitizer + UBSan; part of the gate
bazel coverage --config=cov //tests/... # lcov report
python3 tools/gen_notices.py --check    # third-party notices are current
```

## Conventions

Read these before a patch; they are the difference between a review and a rewrite.

* **C11.** State is owned by an explicit context (actor, session, task). The input
  parser's scanner is the one file-level exception, and it is single-threaded by design.
* **One owner per decision.** "How an input becomes a task" lives in the shell layer,
  "what a turn contains" in the kernel. New syntax goes in as a **pass**, not as a branch
  in the interpreter.
* **Tests are per-behaviour, not per-file** (`test_mention_pass`, `test_shell_job_control`,
  `test_llm_request`, …), and they drive the public entry points rather than internals.
* **Comments explain why**, especially where the code looks odd. A comment that restates
  the code is noise; a comment recording the measurement or the bug that forced the shape
  is what makes the next change safe.

## Commits

[Conventional Commits][cc], English, lowercase imperative subject, no trailing period:
`fix(sandbox): reject a symlinked parent on the write path`.

The body is where the work is explained: what was wrong, what the evidence was, why this
shape and not the other one. If you measured something, put the number in. If you did not
verify something, say so — "tested on Linux only" is more useful than silence.

[cc]: https://www.conventionalcommits.org/

## Branches and pull requests

`dev` is the trunk. Work lands there; `master` is only ever a merge of `dev`, and a
release is a tag on `master`. Nothing is committed to `master` directly.

1. Branch from `dev`.
2. Make the change, with tests.
3. Open the PR **against `master`** — that is the only base `PR Check` runs for, and it
   builds ten platforms plus the full suite. A PR against another branch gets no CI.
4. Keep the PR to one concern where you can. A bug fix that also reformats a file is two
   changes, and the reformatting hides the fix.

A merge needs the `PR Check` jobs green. If a job fails for a reason that predates your
change, say so in the PR rather than working around it.

## Security

Do not open a public issue for a vulnerability. [`SECURITY.md`](SECURITY.md) has the
private channel and the scope.

## Licence

MIT. By contributing you agree your contribution is licensed under the same terms
(see [`LICENSE`](LICENSE)).
