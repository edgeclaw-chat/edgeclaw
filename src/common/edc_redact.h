#ifndef __EDC_REDACT_H__
#define __EDC_REDACT_H__

#include "sds.h"

/* Replace credential-looking substrings in tool output with "<redacted>".
 *
 * A backstop, not a boundary. The deny list is the boundary: it masks a path out of the
 * sandbox entirely. This catches what deny cannot -- a command that reaches a secret
 * through a name no deny entry covers, which is the ordinary case for `cat ~/.zshrc`,
 * `env`, `git config --list`, or an `ssh -v` that logs a key. Tool output goes into the
 * model's context and from there to the API provider, so a leak here is one-way.
 *
 * Recognised, in order of precedence:
 *   - a PEM private-key block (BEGIN ... PRIVATE KEY ... END), entire block;
 *   - "EDGECLAW_ENC:..." ciphertext, the whole value (tag included: the tag only says what
 *     it was, and a reader who needs it can see the redaction marker just as well);
 *   - well-known token prefixes (sk-, ghp_, github_pat_, xoxb-, AKIA, glpat-, npm_, ...);
 *   - a NAME=value / NAME: value assignment whose NAME looks like a credential
 *     (TOKEN, SECRET, PASSWORD, API_KEY, ...) and whose value is long enough to be one.
 *
 * Over-redaction is the intended failure direction: a redacted value costs the model a
 * retry, an unredacted one cannot be taken back. Returns a new sds; the input is not
 * modified. NULL in, NULL out.
 */
sds edc_redact_secrets(sds text);

#endif /* __EDC_REDACT_H__ */
