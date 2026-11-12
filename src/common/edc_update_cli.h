#ifndef __EDC_UPDATE_CLI_H__
#define __EDC_UPDATE_CLI_H__

#include <stdbool.h>

/* The frontend is C++ and calls this, so the symbol needs C linkage -- otherwise the call is
 * looked up under its mangled name and the link fails with
 * "undefined reference to edc_update_check_cli(bool)". */
#ifdef __cplusplus
extern "C"
{
#endif

/* `--update-check`: run the release check once, synchronously, print what came back, return a
 * process exit status.
 *
 * Why this is a C function rather than code in main.cpp: the frontend is C++ and must not
 * include anything that pulls in sds (sds.h's SDS_HDR_VAR does not compile as C++ -- see the
 * note in edc_app.h). The check itself works in sds, so the boundary is here, with a signature
 * of plain types.
 *
 * Why the command exists at all: the once-per-launch check is otherwise unobservable in the
 * two cases that matter for verifying it -- a build whose version is not a release never
 * checks, and a check that finds nothing is deliberately silent. Without a way to ask
 * directly, "did it work?" could only be answered by publishing a release.
 *
 * Does NOT load config.json: that needs the master key (the api_key is encrypted) and so a
 * passphrase prompt, for a question unrelated to the user's key. The channel is |allow_pre|
 * and nothing else.
 */
int edc_update_check_cli(bool allow_pre);

#ifdef __cplusplus
}
#endif

#endif /* __EDC_UPDATE_CLI_H__ */
