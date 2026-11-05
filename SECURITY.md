# Security policy

EdgeClaw runs shell commands and edits files on your machine, on the instructions of a
language model. That is the product, not a defect — so the useful question is not "can it
run something dangerous" but "can it be made to run something its user did not intend, or
reach something the sandbox was supposed to hold back".

## Reporting a vulnerability

Use GitHub's [private vulnerability reporting][pvr] on this repository
(Security → Report a vulnerability). It gives us a private thread and a draft advisory,
and it does not require you to publish anything.

[pvr]: https://github.com/edgeclaw-chat/edgeclaw/security/advisories/new

If that is unavailable, open a normal issue **that contains no details** and say you have
a report; we will move it somewhere private.

Please include: the version (`edgeclaw --version`), the platform, what you expected the
sandbox or the tool layer to prevent, and what actually happened. A reproducer helps more
than a description.

We are a small project and cannot promise a fixed response time. We will acknowledge a
report, tell you whether we think it is in scope, and credit you in the advisory unless
you ask us not to.

## What is in scope

* **Sandbox escapes.** A tool reaching a path outside the configured read-only /
  read-write lists, or outside the deny list, on a platform where the kernel sandbox is
  active (`kernel_sandbox` is true on Linux and macOS; see `src/common/edc_platform.c`).
* **Memory-safety defects reachable from input** — the codebase is C11 and parses
  model output, tool arguments, files and terminal input. A heap overflow, use-after-free
  or out-of-bounds read from any of those is a real finding.
* **Credential handling.** The API key is encrypted at rest with a master key; leaking it
  into a log, a transcript, the process environment of a spawned tool, or a remote device
  is in scope.
* **TLS.** The binary ships its own CA bundle (`workspace/ssl/cacert.pem`) and verifies
  with mbedtls. A path that accepts an unverified certificate is in scope.
* **The remote device agent.** Its transport (TCP or PPPoS) carries shell commands; an
  authentication or parsing flaw that lets a third party issue commands is in scope.

## What is not a vulnerability

These are documented behaviours (`docs/limitations.md`); reporting them wastes your time:

* **The agent executes commands it was asked to execute**, including `rm`, `curl` and
  `git push`. The sandbox narrows *where*, and the approval surface is yours to set.
* **Prompt injection from content you pointed it at.** The sandbox constrains what tools
  can touch; it does not constrain what the model can be told. A README, a web page or a
  dependency's source can contain instructions. Treat a run over untrusted content like
  running that content's author in your shell.
* **`--no-sandbox`.** Turning the sandbox off does exactly that.
* **Windows has path checks only.** `kernel_sandbox` is false there, `/sandbox on` is
  refused, and `--no-sandbox` does not take effect — because there is no kernel backend.
* **The device agent inherits its whole environment**, unlike `exec_shell`, which goes
  through an allowlist (`src/sandbox/edc_shell_env.c`). Do not run an agent on a host
  whose environment carries credentials you would not hand to the remote side.
* **The `<read-files>` / `<modified-files>` list is a snapshot** taken when a conversation
  was folded. It is a summary for the model, not an audit log.

## Supported versions

The latest tagged release. Older releases do not receive fixes; the fixes ship forward.
